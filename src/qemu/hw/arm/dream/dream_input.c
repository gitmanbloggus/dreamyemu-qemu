/*
 * HTC Dream buttons: the trout keypad matrix, the power key and the CPLD
 * key bank, fed from QEMU's keyboard.
 *
 * Everything below was read from the trout 2.6.25 vmlinux (runtime data,
 * so the board's keypad-v3 patching is applied):
 *
 *   gpio_event_matrix_info @0xC02A7DE0: flags 0x10016 (DRIVE_INACTIVE,
 *   two GPIO lists, 42,41,40,39,38,37,36 (the rows, read) and
 *   35,34,33,32,31,23,30,109 (the columns, driven), keymap @0xC0215F28
 *   indexed [column * 7 + row] (Back = index 0 = column 35 x row 42,
 *   Home = index 1 = column 35 x row 41).
 *
 *   The SPL scans the same matrix the other way (board op +0x30 of the
 *   table at 0x8806FB90, scan at 0x880305C4): it drives one column
 *   (35,34,33,32,31,23,30, then 108 on this board revision or 78) low
 *   and reads the rows (42..36), a 0 meaning pressed.  So a key is
 *   modeled as a switch between its row and column pin: an undriven pin
 *   follows a driven pin it is connected to, else rows idle high and
 *   columns low.  The kernel scans the same way as the SPL despite the
 *   ACTIVE_HIGH-looking flags: measured while recovery waits, it drives
 *   columns 35,34,33,32,31,30,23 low (OE set, OUT 0) and arms rows 36-42
 *   as level-low interrupts (POS 0).
 *
 *   gpio_event_input_info @0xC02A7E60, flags 0 (active low):
 *   KEY_END (the Dream's red key, also power) on GPIO 20, KEY_FOCUS on
 *   165, KEY_CAMERA on 166; BTN_LEFT (trackball press) on 160.  GPIOs
 *   160-167 are the trout CPLD's input bank at CPLD +0x08, which the SPL
 *   also reads.  SW_LID, the keyboard slide, is GPIO 162 (CPLD bit 2,
 *   gpio_event_input_info @0xC02A8040, type EV_SW, active low): low =
 *   switch active = keyboard closed.  Left high it read "open" and
 *   Android 1.0 laid the setup wizard out in landscape (measured).
 *
 * Trackball: two gpio_event_axis inputs of the "trout-nav" device
 * (platform data @0xC02A7BDC, infos @0xC02A66F8/0xC02A6720): EV_REL X on
 * GPIO 18/19 and REL_Y on GPIO 94/90, each a 2-bit Gray code decoded by
 * the board's map at 0xC002D984, edge interrupts on all four.  Each step
 * is one transition 00 -> 01 -> 11 -> 10 (or back), emitted one per 2 ms
 * of virtual time so every edge gets its interrupt.  Host: arrow keys,
 * mouse wheel (vertical); press = F10 or the middle button.
 *
 * CPLD (0x98000000): +0x00..+0x06 output shadows (read back), +0x08 the
 * input bank (pressed = 0), interrupt mask +0x0C, status +0x0E (W1C) for
 * that bank; its interrupt output is MSM GPIO 17, active high.  The mask
 * and status offsets are inferred from the kernel's accesses (status
 * reads at +0x0E/+0x10 in trout_gpio_irq_handler), not from a datasheet.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "system/memory.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"
#include "../htcleo/htcleo.h"
#include "dream_input.h"
#include "dream_synaptics.h"

#define NOUT 7
#define NIN  8

static const int kp_out[NOUT] = { 42, 41, 40, 39, 38, 37, 36 };
static const int kp_in[NIN] = { 35, 34, 33, 32, 31, 23, 30, 109 };
/* the eighth column under the SPL's other board revisions */
static const int kp_in7_alias[] = { 108, 78 };

/* trout-keypad-v3 keymap, [column * 7 + row] (vmlinux 0xC0215F28) */
static const uint16_t kp_map[NOUT * NIN] = {
    158, 102, 0, 14, 28, 54, 25, 139,
    231, 0, 42, 30, 59, 16, 22, 8,
    37, 36, 50, 52, 9, 6, 7, 48,
    35, 49, 57, 21, 5, 19, 47, 34,
    46, 215, 20, 3, 17, 56, 115, 31,
    44, 2, 23, 11, 24, 38, 51, 100,
    10, 4, 18, 127, 114, 45, 33, 32,
};

#define GPIO_POWER          20
/*
 * PS_HOLD.  The trout kernel restarts through msm_pm_restart (vmlinux
 * 0xC003C334): with the default reason 0x776655AA it calls the board hook
 * at 0xC002DAB8, which is gpio_set_value(25, 0) and nothing else.  On the
 * phone that drops the PMIC's PS_HOLD and the board restarts; here the
 * latch falling 1 -> 0 requests a system reset.  (Other reasons go
 * through proc_comm PCOM_RESET_CHIP, which msm-modem already answers.)
 */
#define GPIO_PS_HOLD        25
#define GPIO_CPLD_INT       17
#define CPLD_BIT_TRACKBALL  0       /* GPIO 160 */
#define CPLD_BIT_LID        2       /* GPIO 162, keyboard slide */
#define CPLD_BIT_TS_ATT     7       /* GPIO 167, touchscreen attention */
#define CPLD_BIT_FOCUS      5       /* GPIO 165 */
#define CPLD_BIT_CAMERA     6       /* GPIO 166 */

#define CPLD_PHYS           0x98000000
#define CPLD_IN             0x08
#define CPLD_MASK           0x0C
#define CPLD_STATUS         0x0E

static const int tb_gpio[2][2] = { { 18, 19 }, { 94, 90 } };
static const uint8_t gray[4] = { 0, 1, 3, 2 };
#define TB_STEP_NS          (2 * SCALE_MS)

/*
 * How long -M dream,keys=... holds the keys after power-on: about as long
 * as a person holds the combo.  Power+Back held 3 s still gives boot
 * mode 2 (HBOOT), measured.
 */
#define BOOT_HOLD_NS        (3 * NANOSECONDS_PER_SECOND)

typedef struct DreamInput {
    MsmGpioState *gpio;
    MemoryRegion cpld;
    uint8_t shadow[8];
    uint8_t cpld_in;        /* pressed = 0 */
    uint8_t cpld_mask;      /* 1 = masked */
    uint8_t cpld_status;
    bool pressed[NOUT * NIN];
    bool power;
    bool kbd_open;          /* keyboard slid out */
    int ps_hold;            /* last PS_HOLD latch */
    int tb_phase[2];        /* Gray code position per axis */
    int tb_pending[2];      /* steps still to emit (signed) */
    QEMUTimer *tb_timer;
    DeviceState *ts;
    int touch_x, touch_y;   /* screen pixels */
    bool touch_down;
    uint8_t boot_cpld;      /* CPLD bits held from power-on */
    bool boot_power;
    int boot_matrix[4];
    int nboot_matrix;
    QEMUTimer *release;
} DreamInput;

static void cpld_update_irq(DreamInput *d)
{
    msm_gpio_set_input(d->gpio, GPIO_CPLD_INT,
                       !!(d->cpld_status & ~d->cpld_mask));
}

static void cpld_set_bit(DreamInput *d, int bit, bool down)
{
    uint8_t old = d->cpld_in;

    if (down) {
        d->cpld_in &= ~(1u << bit);
    } else {
        d->cpld_in |= 1u << bit;
    }
    if (old != d->cpld_in) {
        d->cpld_status |= 1u << bit;
        cpld_update_irq(d);
    }
}

static uint64_t cpld_read(void *opaque, hwaddr off, unsigned size)
{
    DreamInput *d = opaque;
    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        hwaddr o = off + i;
        uint8_t b;

        switch (o) {
        case CPLD_IN:
            b = d->cpld_in;
            break;
        case CPLD_MASK:
            b = d->cpld_mask;
            break;
        case CPLD_STATUS:
            b = d->cpld_status;
            break;
        default:
            b = o < 8 ? d->shadow[o] : (o == 0x09 ? 0xFF : 0);
            break;
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void cpld_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    DreamInput *d = opaque;

    for (unsigned i = 0; i < size; i++) {
        hwaddr o = off + i;
        uint8_t b = val >> (8 * i);

        if (o < 8) {
            d->shadow[o] = b;
        } else if (o == CPLD_MASK) {
            d->cpld_mask = b;
        } else if (o == CPLD_STATUS) {
            d->cpld_status &= ~b;
        }
    }
    cpld_update_irq(d);
}

static const MemoryRegionOps cpld_ops = {
    .read = cpld_read,
    .write = cpld_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* The level a column's pins drive, or -1 when none drives */
static int col_driven(DreamInput *d, int i)
{
    int v = msm_gpio_get_output(d->gpio, kp_in[i]);

    for (int a = 0; v < 0 && i == NIN - 1 && a < ARRAY_SIZE(kp_in7_alias); a++) {
        v = msm_gpio_get_output(d->gpio, kp_in7_alias[a]);
    }
    return v;
}

static void matrix_update(DreamInput *d)
{
    /* rows: undriven ones follow a driven column through a pressed key */
    for (int o = 0; o < NOUT; o++) {
        int level = 1;

        for (int i = 0; i < NIN; i++) {
            int c;

            if (d->pressed[o * NIN + i] && (c = col_driven(d, i)) >= 0) {
                level &= c;
            }
        }
        msm_gpio_set_input(d->gpio, kp_out[o], level);
    }
    /* columns: likewise from a driven row */
    for (int i = 0; i < NIN; i++) {
        int level = 0;

        for (int o = 0; o < NOUT; o++) {
            if (d->pressed[o * NIN + i] &&
                msm_gpio_get_output(d->gpio, kp_out[o]) == 1) {
                level = 1;
            }
        }
        msm_gpio_set_input(d->gpio, kp_in[i], level);
        if (i == NIN - 1) {
            for (int a = 0; a < ARRAY_SIZE(kp_in7_alias); a++) {
                msm_gpio_set_input(d->gpio, kp_in7_alias[a], level);
            }
        }
    }
    msm_gpio_set_input(d->gpio, GPIO_POWER, !d->power);
}

static void gpio_out_changed(void *opaque, int pin, int level)
{
    DreamInput *d = opaque;
    int ps = msm_gpio_get_output_latch(d->gpio, GPIO_PS_HOLD);

    if (d->ps_hold == 1 && ps == 0) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
    d->ps_hold = ps;

    if (pin == -1) {
        matrix_update(d);
        return;
    }
    for (int o = 0; o < NOUT; o++) {
        if (pin == kp_out[o]) {
            matrix_update(d);
            return;
        }
    }
    for (int i = 0; i < NIN; i++) {
        if (pin == kp_in[i]) {
            matrix_update(d);
            return;
        }
    }
    for (int a = 0; a < ARRAY_SIZE(kp_in7_alias); a++) {
        if (pin == kp_in7_alias[a]) {
            matrix_update(d);
            return;
        }
    }
}

static void tb_out(DreamInput *d, int axis)
{
    uint8_t g = gray[d->tb_phase[axis] & 3];

    msm_gpio_set_input(d->gpio, tb_gpio[axis][0], g & 1);
    msm_gpio_set_input(d->gpio, tb_gpio[axis][1], (g >> 1) & 1);
}

static void tb_tick(void *opaque)
{
    DreamInput *d = opaque;
    bool more = false;

    for (int a = 0; a < 2; a++) {
        if (d->tb_pending[a]) {
            int dir = d->tb_pending[a] > 0 ? 1 : -1;

            d->tb_phase[a] += dir;
            d->tb_pending[a] -= dir;
            tb_out(d, a);
        }
        more |= d->tb_pending[a] != 0;
    }
    if (more) {
        timer_mod(d->tb_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  TB_STEP_NS);
    }
}

/* One detent is a full Gray cycle (4 edges) on that axis */
static void tb_move(DreamInput *d, int axis, int detents)
{
    d->tb_pending[axis] += 4 * detents;
    if (!timer_pending(d->tb_timer)) {
        timer_mod(d->tb_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  TB_STEP_NS);
    }
}

/* the pressed[] slot (row * NIN + column) of a keycode, or -1 */
static int matrix_index(int linux_code)
{
    for (int k = 0; k < NOUT * NIN; k++) {
        if (kp_map[k] && kp_map[k] == linux_code) {
            return (k % NOUT) * NIN + k / NOUT;
        }
    }
    return -1;
}

static void dream_key(DreamInput *d, int code, bool down)
{
    int k;

    switch (code) {
    case KEY_F12:
    case KEY_END:
        d->power = down;
        matrix_update(d);
        return;
    case KEY_F9:
        cpld_set_bit(d, CPLD_BIT_CAMERA, down);
        return;
    case KEY_F8:
        cpld_set_bit(d, CPLD_BIT_FOCUS, down);
        return;
    case KEY_F10:
        cpld_set_bit(d, CPLD_BIT_TRACKBALL, down);
        return;
    case KEY_UP:
    case KEY_DOWN:
        if (down) {
            tb_move(d, 1, code == KEY_DOWN ? 1 : -1);
        }
        return;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (down) {
            tb_move(d, 0, code == KEY_RIGHT ? 1 : -1);
        }
        return;
    case KEY_F11:
        /* slide the keyboard out or back in */
        if (down) {
            d->kbd_open = !d->kbd_open;
            cpld_set_bit(d, CPLD_BIT_LID, !d->kbd_open);
        }
        return;
    case KEY_ESC:
        code = KEY_BACK;
        break;
    case KEY_MAIL:
        /* the @ key (KEY_EMAIL in trout-keypad-v3.kl) has no PC key: qcode "mail" */
        code = KEY_EMAIL;
        break;
    case KEY_F1:
        code = KEY_MENU;
        break;
    case KEY_F2:
        code = KEY_SEND;
        break;
    case KEY_PAGEUP:
        code = KEY_VOLUMEUP;
        break;
    case KEY_PAGEDOWN:
        code = KEY_VOLUMEDOWN;
        break;
    }
    k = matrix_index(code);
    if (k >= 0) {
        d->pressed[k] = down;
        matrix_update(d);
    }
}

static DreamInput *dream_input;

/* the keyboard slide, for the machine's keyboard-open property (web UI) */
bool dream_input_keyboard_open(void)
{
    return dream_input && dream_input->kbd_open;
}

void dream_input_set_keyboard_open(bool open)
{
    if (dream_input && dream_input->kbd_open != open) {
        dream_input->kbd_open = open;
        cpld_set_bit(dream_input, CPLD_BIT_LID, !open);
    }
}

static void dream_key_event(DeviceState *dev, QemuConsole *src,
                            QemuInputEvent *evt)
{
    if (!dream_input) {
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_KEY) {
        dream_key(dream_input, evt->key.key, evt->key.down);
    } else if (evt->type == INPUT_EVENT_KIND_BTN) {
        switch (evt->btn.button) {
        case INPUT_BUTTON_WHEEL_UP:
        case INPUT_BUTTON_WHEEL_DOWN:
            if (evt->btn.down) {
                tb_move(dream_input, 1,
                        evt->btn.button == INPUT_BUTTON_WHEEL_DOWN ? 1 : -1);
            }
            break;
        case INPUT_BUTTON_LEFT:
            dream_input->touch_down = evt->btn.down;
            if (dream_input->ts) {
                dream_synaptics_touch(dream_input->ts, evt->btn.down,
                                      dream_input->touch_x,
                                      dream_input->touch_y);
            }
            break;
        case INPUT_BUTTON_MIDDLE:
            cpld_set_bit(dream_input, CPLD_BIT_TRACKBALL, evt->btn.down);
            break;
        default:
            break;
        }
    } else if (evt->type == INPUT_EVENT_KIND_ABS) {
        /* the pointer over the 320x480 screen is the finger */
        int64_t v = evt->abs.value;

        if (evt->abs.axis == INPUT_AXIS_X) {
            dream_input->touch_x = v * 320 / (INPUT_EVENT_ABS_MAX + 1);
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            dream_input->touch_y = v * 480 / (INPUT_EVENT_ABS_MAX + 1);
        }
        if (dream_input->touch_down && dream_input->ts) {
            dream_synaptics_touch(dream_input->ts, true, dream_input->touch_x,
                                  dream_input->touch_y);
        }
    }
}

static const QemuInputHandler dream_keyboard = {
    .name  = "HTC Dream buttons, trackball and touchscreen",
    .mask  = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = dream_key_event,
};

static void boot_keys_apply(DreamInput *d, bool down)
{
    for (int b = 0; b < 8; b++) {
        if (d->boot_cpld & (1u << b)) {
            cpld_set_bit(d, b, down);
        }
    }
    if (d->boot_power) {
        d->power = down;
    }
    for (int i = 0; i < d->nboot_matrix; i++) {
        d->pressed[d->boot_matrix[i]] = down;
    }
    matrix_update(d);
}

static void boot_keys_release(void *opaque)
{
    boot_keys_apply(opaque, false);
}

static void dream_input_reset(void *opaque)
{
    DreamInput *d = opaque;

    memset(d->shadow, 0, sizeof(d->shadow));
    memset(d->pressed, 0, sizeof(d->pressed));
    d->cpld_in = 0xFF;
    d->kbd_open = false;
    d->cpld_in &= ~(1u << CPLD_BIT_LID);       /* keyboard closed */
    d->cpld_mask = 0xFF;
    d->cpld_status = 0;
    d->power = false;
    d->ps_hold = 1;
    for (int a = 0; a < 2; a++) {
        d->tb_phase[a] = 0;
        d->tb_pending[a] = 0;
        tb_out(d, a);
    }
    boot_keys_apply(d, true);
    cpld_update_irq(d);
    if (d->boot_cpld || d->boot_power || d->nboot_matrix) {
        timer_mod(d->release,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BOOT_HOLD_NS);
    }
}

/* "power+camera" etc (commas would split -M): the combo held at power-on */
static void parse_boot_keys(DreamInput *d, const char *spec)
{
    g_auto(GStrv) names = spec ? g_strsplit(spec, "+", 0) : NULL;

    for (int i = 0; names && names[i]; i++) {
        const char *n = names[i];
        int code = 0;

        if (!strcmp(n, "power")) {
            d->boot_power = true;
        } else if (!strcmp(n, "camera")) {
            d->boot_cpld |= 1u << CPLD_BIT_CAMERA;
        } else if (!strcmp(n, "focus")) {
            d->boot_cpld |= 1u << CPLD_BIT_FOCUS;
        } else if (!strcmp(n, "trackball")) {
            d->boot_cpld |= 1u << CPLD_BIT_TRACKBALL;
        } else if (!strcmp(n, "home")) {
            code = KEY_HOME;
        } else if (!strcmp(n, "back")) {
            code = KEY_BACK;
        } else if (!strcmp(n, "menu")) {
            code = KEY_MENU;
        } else if (!strcmp(n, "volup")) {
            code = KEY_VOLUMEUP;
        } else if (!strcmp(n, "voldown")) {
            code = KEY_VOLUMEDOWN;
        } else {
            warn_report("dream: unknown key '%s' in keys=", n);
        }
        if (code && d->nboot_matrix < ARRAY_SIZE(d->boot_matrix)) {
            d->boot_matrix[d->nboot_matrix++] = matrix_index(code);
        }
    }
}

/* the touchscreen's attention output: active low on CPLD bit 7 */
static void ts_att(void *opaque, int n, int level)
{
    cpld_set_bit(opaque, CPLD_BIT_TS_ATT, level);
}

void dream_input_init(MsmGpioState *gpio, const char *boot_keys,
                      DeviceState *touchscreen)
{
    DreamInput *d = g_new0(DreamInput, 1);

    d->ts = touchscreen;
    if (touchscreen) {
        qdev_connect_gpio_out_named(touchscreen, "att", 0,
                                    qemu_allocate_irq(ts_att, d, 0));
    }

    d->gpio = gpio;
    parse_boot_keys(d, boot_keys);
    memory_region_init_io(&d->cpld, NULL, &cpld_ops, d, "dream.cpld", 0x100);
    memory_region_add_subregion_overlap(get_system_memory(), CPLD_PHYS,
                                        &d->cpld, 1);
    d->release = timer_new_ns(QEMU_CLOCK_VIRTUAL, boot_keys_release, d);
    d->tb_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tb_tick, d);
    msm_gpio_set_output_notify(gpio, gpio_out_changed, d);
    qemu_input_handler_register(NULL, &dream_keyboard);
    qemu_register_reset(dream_input_reset, d);
    dream_input = d;
}

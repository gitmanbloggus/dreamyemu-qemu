/*
 * HTC Dream touchscreen: Synaptics RMI controller on I2C 0x20, as the
 * trout 2.6.25 synaptics_i2c_rmi driver drives it (read from the vmlinux:
 * probe 0xC01604A0.., synaptics_init_panel 0xC015FFDC, work 0xC0160F00):
 *
 *  - 0xE0..0xFF answer in every page: 0xE3 product property, 0xE4/0xE5
 *    major/minor version, 0xF0 device control, 0xF1 interrupt enable,
 *    0xF4 device command (1 = reset), 0xFF page select.
 *  - page 0x10: 0x04/0x05 and 0x06/0x07 are max X and max Y, read as
 *    words and decoded (lo >> 8 & 0xff) | (hi & 0x1f) << 8, i.e. the
 *    register at the lower address holds the top 5 bits.  0x41 is
 *    written 4 ("No Clip Z").
 *  - page 4: the report, read 15 bytes from 0x00: [0] fingers (bits 2:0)
 *    and width (7:4), [1] Z, [2..5] X and Y (13-bit, high byte first),
 *    [8..11] a second finger, [12..13] gestures, [14] status, whose top
 *    two bits must read 01 (else "bad read" and the panel is re-inited)
 *    and whose bit 0 (absolute data) must be set for the report to reach
 *    the input layer (work func 0xC0160FEC; without it taps were read
 *    and dropped, measured).
 *
 * The attention line is IRQ 193 = trout CPLD GPIO 167 (bit 7), active
 * low: asserted while a report waits, released when the report has been
 * read.  While a finger is down a new report comes every 12.5 ms (the
 * driver's "80 reports per second" setting).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "dream_synaptics.h"

OBJECT_DECLARE_SIMPLE_TYPE(DreamSynaptics, DREAM_SYNAPTICS)

/* sensor range; the driver reads it back, so any 13-bit values do */
#define SYN_MAX_X       4095
#define SYN_MAX_Y       6143
#define SYN_VERSION     0x0100      /* < 0x010C: the platform data's entry 1 */
#define SYN_REPORT_NS   12500000    /* 80 reports per second */

struct DreamSynaptics {
    I2CSlave parent_obj;
    qemu_irq att;           /* level: 1 = report waiting (line low) */
    QEMUTimer *timer;
    uint8_t page;
    uint8_t ptr;
    bool first;             /* next byte sent is the register pointer */
    uint8_t ctl[0x20];      /* 0xE0..0xFF */
    uint8_t no_clip_z;
    bool down;
    uint16_t raw_x, raw_y;
    bool pending;
    int screen_w, screen_h;
    /* the transform the driver applies, from its probe printout */
    int32_t inact_l, inact_r, inact_t, inact_b;
    bool flip_y;
};

static void syn_set_att(DreamSynaptics *s, bool on)
{
    s->pending = on;
    qemu_set_irq(s->att, on);
}

static uint8_t syn_report_byte(DreamSynaptics *s, unsigned i)
{
    switch (i) {
    case 0:
        return s->down ? (0x50 | 1) : 0;      /* width 5, one finger */
    case 1:
        return s->down ? 0x40 : 0;             /* Z */
    case 2:
        return (s->raw_x >> 8) & 0x1F;
    case 3:
        return s->raw_x & 0xFF;
    case 4:
        return (s->raw_y >> 8) & 0x1F;
    case 5:
        return s->raw_y & 0xFF;
    case 14:
        return 0x41;
    default:
        return 0;
    }
}

static uint8_t syn_read_reg(DreamSynaptics *s, uint8_t r)
{
    if (r >= 0xE0) {
        switch (r) {
        case 0xE4:
            return SYN_VERSION >> 8;
        case 0xE5:
            return SYN_VERSION & 0xFF;
        case 0xFF:
            return s->page;
        default:
            return s->ctl[r - 0xE0];
        }
    }
    if (s->page == 0x10) {
        switch (r) {
        case 0x04:
            return (SYN_MAX_X >> 8) & 0x1F;
        case 0x05:
            return SYN_MAX_X & 0xFF;
        case 0x06:
            return (SYN_MAX_Y >> 8) & 0x1F;
        case 0x07:
            return SYN_MAX_Y & 0xFF;
        case 0x41:
            return s->no_clip_z;
        }
        return 0;
    }
    if (r < 15) {
        uint8_t v = syn_report_byte(s, r);

        if (r == 14) {
            syn_set_att(s, false);      /* the report has been read */
        }
        return v;
    }
    return 0;
}

static void syn_write_reg(DreamSynaptics *s, uint8_t r, uint8_t v)
{
    if (r == 0xFF) {
        s->page = v;
    } else if (r == 0xF4 && (v & 1)) {
        s->page = 0;                    /* reset */
    } else if (r >= 0xE0) {
        s->ctl[r - 0xE0] = v;
    } else if (s->page == 0x10 && r == 0x41) {
        s->no_clip_z = v;
    }
}

static int syn_event(I2CSlave *i2c, enum i2c_event ev)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(i2c);

    if (ev == I2C_START_SEND) {
        s->first = true;
    }
    return 0;
}

static int syn_send(I2CSlave *i2c, uint8_t data)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(i2c);

    if (s->first) {
        s->ptr = data;
        s->first = false;
    } else {
        syn_write_reg(s, s->ptr++, data);
    }
    return 0;
}

static uint8_t syn_recv(I2CSlave *i2c)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(i2c);

    return syn_read_reg(s, s->ptr++);
}

static void syn_tick(void *opaque)
{
    DreamSynaptics *s = opaque;

    syn_set_att(s, true);
    if (s->down) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SYN_REPORT_NS);
    }
}

/*
 * A touch at screen pixel (px, py).  The driver reports
 * [-inact_l, max + inact_r] (inactive edges as fractions of 0x10000 of
 * max) and Android maps that range onto the screen; with FLIP_Y the
 * reported Y is max_y - raw.  Invert that to find the raw position.
 */
void dream_synaptics_touch(DeviceState *dev, bool down, int px, int py)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(dev);
    int64_t il = (int64_t)SYN_MAX_X * s->inact_l / 0x10000;
    int64_t ir = (int64_t)SYN_MAX_X * s->inact_r / 0x10000;
    int64_t it = (int64_t)SYN_MAX_Y * s->inact_t / 0x10000;
    int64_t ib = (int64_t)SYN_MAX_Y * s->inact_b / 0x10000;
    int64_t rx = -il + (int64_t)px * (SYN_MAX_X + il + ir) / s->screen_w;
    int64_t ry = -it + (int64_t)py * (SYN_MAX_Y + it + ib) / s->screen_h;

    if (s->flip_y) {
        ry = SYN_MAX_Y - ry;
    }
    s->raw_x = MIN(MAX(rx, 0), SYN_MAX_X);
    s->raw_y = MIN(MAX(ry, 0), SYN_MAX_Y);
    if (down != s->down || down) {
        s->down = down;
        if (!timer_pending(s->timer)) {
            syn_tick(s);
        }
    }
}

static void syn_reset(DeviceState *dev)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(dev);

    timer_del(s->timer);
    memset(s->ctl, 0, sizeof(s->ctl));
    s->page = 0;
    s->down = false;
    syn_set_att(s, false);
}

static void syn_init(Object *obj)
{
    DreamSynaptics *s = DREAM_SYNAPTICS(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->att, "att", 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, syn_tick, s);
}

static const Property syn_props[] = {
    DEFINE_PROP_INT32("screen-width", DreamSynaptics, screen_w, 320),
    DEFINE_PROP_INT32("screen-height", DreamSynaptics, screen_h, 480),
    /* trout platform data entry 1 (version < 0x010C), vmlinux 0xC02A7AC8 */
    DEFINE_PROP_INT32("inactive-left", DreamSynaptics, inact_l, 0x15DA),
    DEFINE_PROP_INT32("inactive-right", DreamSynaptics, inact_r, 0x15DA),
    DEFINE_PROP_INT32("inactive-top", DreamSynaptics, inact_t, 0x4C7),
    DEFINE_PROP_INT32("inactive-bottom", DreamSynaptics, inact_b, 0x4C7),
    DEFINE_PROP_BOOL("flip-y", DreamSynaptics, flip_y, true),
};

static void syn_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = syn_event;
    k->send = syn_send;
    k->recv = syn_recv;
    device_class_set_legacy_reset(dc, syn_reset);
    device_class_set_props(dc, syn_props);
}

static const TypeInfo syn_info = {
    .name = TYPE_DREAM_SYNAPTICS,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(DreamSynaptics),
    .instance_init = syn_init,
    .class_init = syn_class_init,
};

static void syn_register(void)
{
    type_register_static(&syn_info);
}

type_init(syn_register)

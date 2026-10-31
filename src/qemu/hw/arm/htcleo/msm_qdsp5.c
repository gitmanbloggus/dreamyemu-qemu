/*
 * MSM7201A (Dream) application DSP: QDSP5 audio, high level
 *
 * The 2.6.25 trout kernel's qdsp5 driver (arch/arm/mach-msm/qdsp5) plays
 * PCM like this, all read from the RC29 kernel (vmlinux symbols in
 * parentheses):
 *
 *  1. audmgr (audmgr_enable) calls the modem's audmgr program
 *     0x30000013:e94e8f0c, ENABLE_CLIENT (2), and waits for two callbacks on
 *     the same endpoint (audmgr_rpc_thread): proc 1 with args
 *     { cb_id, 1, status, value }, status 0 READY (value = handle) and
 *     1 CODEC_CONFIG (value = volume), which marks it enabled.
 *  2. msm_adsp_enable("AUDPPTASK", module 4) calls "adsp rtos"
 *     0x3000000a:71d1094b proc 2 with { 1, cmd, proc_id, module }, cmd 1
 *     enable / 2 disable.  AMSS answers by calling the kernel's
 *     0x3000000b:ee3a9966 proc 2 with { 1, event, proc_id, module, image },
 *     event 0 READY (adsp_rpc_thread), which also picks the queue table.
 *  3. Commands (msm_adsp_write): the ADSP window is PA 0xAC000000 (VA
 *     0xE0300000, 0xD00000).  write_ctrl at +0x400034 reads 0x7xxxxxxx when
 *     ready; the host writes 0x8 << 28 | queue offset and rings the doorbell
 *     (+0xC00200 = 1).  The DSP puts a buffer address (DSP memory, < 4 MiB)
 *     in the low 24 bits with bit 31 clear; the host copies the command
 *     there as halfwords, writes 0x9 << 28 | queue offset and rings again.
 *  4. Events (adsp_irq_handler, INT_ADSP_A11): read_ctrl at +0x400038 holds
 *     0xA << 28 | address of a message: halfword 0 = msg id | task << 8,
 *     halfword 1 = length in halfwords, then the payload.  The host clears
 *     the top three bits and rings the doorbell; 0x80000000 is "nothing".
 *  5. audpp (module 4, task 9; image 0 queues 1-3 at 0x6F0/0x6F4/0x6F8):
 *     queue 1 { 4, 0xFFFF } enables it, answered by msg 7 (CFG) { 0xFFFF }.
 *     The PCM driver (audio_out) then sends on queue 2 { 2, 5, 0, 8, ... }
 *     with its two buffers (words 12-17: address lo/hi and length each),
 *     sample rate (22) and channel mode (23), and per filled buffer
 *     { 2, 5, 0xFFFF, 8, 0, idx, len }.  A played buffer goes back as msg 3
 *     (HOST_PCM_INTF) { .., .., 3, idx } (audio_dsp_event).
 *
 * The DSP's own firmware is not run: this plays the PCM through QEMU's
 * audio backend at the stream's own pace.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/irq.h"
#include "qemu/audio.h"
#include "htcleo.h"
#include "msm_modem.h"

#define DSP_MEM_SIZE        0x400000
#define WRITE_CTRL          0x400034
#define READ_CTRL           0x400038
#define SEND_IRQ            0xC00200

#define CMD_BUF             0x3F0000    /* where the host copies commands */
#define MSG_BUF             0x3F8000    /* where events are posted */

#define WR_READY            0x70000000
#define RD_IDLE             0x80000000
#define RD_MSG16            0xA0000000

#define MOD_AUDPP           4
#define TASK_AUDPP          9
#define Q_AUDPP1            0x6F0
#define Q_AUDPP2            0x6F4
#define Q_AUDPP3            0x6F8

#define AUDPP_MSG_HOST_PCM  3
#define AUDPP_MSG_CFG       7
#define AUDPP_CMD_CFG       4
#define AUDPP_CMD_PCM_INTF  2
#define HOSTPCM_ARM_RX      3

#define ADSP_MTOA_PROG      0x3000000b
#define ADSP_MTOA_VERS      0xee3a9966
#define ADSP_EV_READY       0
#define ADSP_EV_DISABLED    1
#define AUDMGR_CB_PROG      0x31000013

typedef struct Qdsp5Msg {
    uint16_t id, task, len;
    uint16_t data[8];
} Qdsp5Msg;

typedef struct MsmQdsp5 {
    MsmModemState *modem;
    MemoryRegion mem, ctrl, irqreg;
    uint8_t *ram;
    qemu_irq irq;
    uint32_t write_ctrl, read_ctrl;
    GQueue *events;             /* Qdsp5Msg, waiting for read_ctrl */

    /* audmgr callbacks go out after the RPC reply */
    QEMUTimer *rpc_timer;
    uint32_t am_cb_id;
    int am_pending;             /* 1 enable, 2 disable */
    GQueue *adsp_pending;       /* (event << 16 | module), as pointers */

    /* host PCM */
    uint32_t buf_addr[2], buf_size[2];
    uint32_t rate, channels;
    int queued[2];              /* length in bytes, 0 = not queued */
    int playing;                /* 1-based index being played, 0 none */
    QEMUTimer *pcm_timer;
    SWVoiceOut *voice;
} MsmQdsp5;

static void post(MsmQdsp5 *q, uint16_t task, uint16_t id,
                 const uint16_t *data, int n);

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void event_next(MsmQdsp5 *q)
{
    Qdsp5Msg *m;

    if (q->read_ctrl & 0xE0000000) {
        return;                 /* the host has not taken the last one */
    }
    m = g_queue_pop_head(q->events);
    if (!m) {
        q->read_ctrl = RD_IDLE;
        return;
    }
    stw_le_p(q->ram + MSG_BUF, m->id | m->task << 8);
    stw_le_p(q->ram + MSG_BUF + 2, m->len);
    for (int i = 0; i < m->len; i++) {
        stw_le_p(q->ram + MSG_BUF + 4 + 2 * i, m->data[i]);
    }
    q->read_ctrl = RD_MSG16 | MSG_BUF;
    g_free(m);
    qemu_irq_pulse(q->irq);
}

static void post(MsmQdsp5 *q, uint16_t task, uint16_t id,
                 const uint16_t *data, int n)
{
    Qdsp5Msg *m = g_new0(Qdsp5Msg, 1);

    m->id = id;
    m->task = task;
    m->len = MIN(n, ARRAY_SIZE(m->data));
    memcpy(m->data, data, 2 * m->len);
    g_queue_push_tail(q->events, m);
    if (q->read_ctrl == RD_IDLE) {
        q->read_ctrl = 0;
        event_next(q);
    }
}

/* ------------------------------------------------------------------ */
/* Host PCM                                                            */
/* ------------------------------------------------------------------ */

static void pcm_request(MsmQdsp5 *q, int idx)
{
    uint16_t m[4] = { 5, 0, HOSTPCM_ARM_RX, idx };

    post(q, TASK_AUDPP, AUDPP_MSG_HOST_PCM, m, 4);
}

static void voice_cb(void *opaque, int avail)
{
    /* paced by pcm_timer */
}

static void pcm_start(MsmQdsp5 *q)
{
    MsmModemState *s = q->modem;
    int idx, len;
    uint64_t bps;

    if (q->playing || timer_pending(q->pcm_timer)) {
        return;
    }
    idx = q->queued[0] ? 0 : q->queued[1] ? 1 : -1;
    if (idx < 0) {
        return;
    }
    len = q->queued[idx];
    q->playing = idx + 1;
    if (s->audio_be && q->voice) {
        g_autofree uint8_t *pcm = g_malloc(len);

        address_space_read(&address_space_memory, q->buf_addr[idx],
                           MEMTXATTRS_UNSPECIFIED, pcm, len);
        audio_be_write(s->audio_be, q->voice, pcm, len);
    }
    bps = (uint64_t)MAX(q->rate, 8000) * MAX(q->channels, 1) * 2;
    timer_mod(q->pcm_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              muldiv64(MAX(len, 64), NANOSECONDS_PER_SECOND, bps));
}

static void pcm_done(void *opaque)
{
    MsmQdsp5 *q = opaque;
    int idx = q->playing;

    if (!idx) {
        return;
    }
    q->playing = 0;
    q->queued[idx - 1] = 0;
    pcm_request(q, idx);
    pcm_start(q);
}

static uint32_t pcm_rate(uint16_t v)
{
    /* the field is Hz on the PCM path; small values are an index */
    static const uint32_t idx[] = { 96000, 88200, 64000, 48000, 44100,
                                    32000, 24000, 22050, 16000, 12000,
                                    11025, 8000 };
    if (v >= 8000) {
        return v;
    }
    return v < ARRAY_SIZE(idx) ? idx[v] : 48000;
}

static void pcm_config(MsmQdsp5 *q, const uint16_t *c)
{
    MsmModemState *s = q->modem;

    for (int i = 0; i < 2; i++) {
        q->buf_addr[i] = c[12 + 3 * i] | (uint32_t)c[13 + 3 * i] << 16;
        q->buf_size[i] = c[14 + 3 * i];
        q->queued[i] = 0;
    }
    q->rate = pcm_rate(c[22]);
    q->channels = c[23] == 1 ? 1 : 2;
    q->playing = 0;
    timer_del(q->pcm_timer);
    if (s->trace) {
        qemu_log("qdsp5: pcm config bufs %08x/%u %08x/%u rate %u (%u) "
                 "chan %u (%u)\n", q->buf_addr[0], q->buf_size[0],
                 q->buf_addr[1], q->buf_size[1], q->rate, c[22],
                 q->channels, c[23]);
    }
    if (s->audio_be) {
        struct audsettings as = {
            .freq = q->rate,
            .nchannels = q->channels,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };
        q->voice = audio_be_open_out(s->audio_be, q->voice, "dream.qdsp5",
                                     q, voice_cb, &as);
        if (q->voice) {
            audio_be_set_active_out(s->audio_be, q->voice, true);
        }
    }
    /* ask for both buffers */
    pcm_request(q, 1);
    pcm_request(q, 2);
}

static void pcm_buffer(MsmQdsp5 *q, const uint16_t *c)
{
    int idx = c[5];
    uint32_t len = c[6];

    if (idx < 1 || idx > 2) {
        return;
    }
    /* bytes; a length above the buffer would be halfwords */
    if (len > q->buf_size[idx - 1]) {
        len *= 2;
    }
    q->queued[idx - 1] = MIN(len, MAX(q->buf_size[idx - 1], 2u * len));
    if (q->modem->trace) {
        qemu_log("qdsp5: pcm buffer %d len %u\n", idx, c[6]);
    }
    pcm_start(q);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void command(MsmQdsp5 *q, uint32_t queue)
{
    uint16_t c[24];
    uint16_t ena;

    for (int i = 0; i < ARRAY_SIZE(c); i++) {
        c[i] = lduw_le_p(q->ram + CMD_BUF + 2 * i);
    }
    if (q->modem->trace) {
        qemu_log("qdsp5: queue %03x cmd %04x %04x %04x %04x\n", queue,
                 c[0], c[1], c[2], c[3]);
    }
    switch (queue) {
    case Q_AUDPP1:
        if (c[0] == AUDPP_CMD_CFG) {
            ena = c[1] ? 0xFFFF : 0;
            post(q, TASK_AUDPP, AUDPP_MSG_CFG, &ena, 1);
            if (!ena) {
                timer_del(q->pcm_timer);
                q->playing = 0;
                q->queued[0] = q->queued[1] = 0;
                if (q->voice && q->modem->audio_be) {
                    audio_be_set_active_out(q->modem->audio_be, q->voice,
                                            false);
                }
            }
        }
        break;
    case Q_AUDPP2:
        if (c[0] == AUDPP_CMD_PCM_INTF && c[2] == 0xFFFF) {
            pcm_buffer(q, c);
        } else if (c[0] == AUDPP_CMD_PCM_INTF && c[2] == 0) {
            pcm_config(q, c);
        }
        break;
    default:            /* volume, EQ, decoders: accepted, not modelled */
        break;
    }
}

static uint64_t ctrl_read(void *opaque, hwaddr off, unsigned size)
{
    MsmQdsp5 *q = opaque;

    switch (off) {
    case 0:
        return q->write_ctrl;
    case 4:
        return q->read_ctrl;
    }
    return 0;
}

static void ctrl_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MsmQdsp5 *q = opaque;

    switch (off) {
    case 0:
        q->write_ctrl = v;
        break;
    case 4:
        q->read_ctrl = v;
        break;
    }
}

/* the doorbell: a write request, a command, or a taken event */
static void irq_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MsmQdsp5 *q = opaque;
    uint32_t queue = q->write_ctrl & 0xFFFFFF;

    switch (q->write_ctrl >> 28) {
    case 0x8:
        q->write_ctrl = CMD_BUF;
        return;
    case 0x9:
        q->write_ctrl = WR_READY;
        command(q, queue);
        return;
    }
    if (!(q->read_ctrl & 0xE0000000)) {
        event_next(q);
    }
}

static uint64_t irq_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static const MemoryRegionOps ctrl_ops = {
    .read = ctrl_read,
    .write = ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps irq_ops = {
    .read = irq_read,
    .write = irq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* RPC: audmgr and adsp rtos                                           */
/* ------------------------------------------------------------------ */

static void rpc_callbacks(void *opaque)
{
    MsmQdsp5 *q = opaque;
    MsmModemState *s = q->modem;
    gpointer p;

    if (q->am_pending == 1) {
        uint32_t ready[4] = { cpu_to_be32(q->am_cb_id), cpu_to_be32(1),
                              cpu_to_be32(0), cpu_to_be32(1) };
        uint32_t codec[4] = { cpu_to_be32(q->am_cb_id), cpu_to_be32(1),
                              cpu_to_be32(1), cpu_to_be32(0x7FFF) };

        msm_rpc_apps_call(s, AUDMGR_CB_PROG, 0, 1, ready, 4);
        msm_rpc_apps_call(s, AUDMGR_CB_PROG, 0, 1, codec, 4);
    } else if (q->am_pending == 2) {
        uint32_t dis[4] = { cpu_to_be32(q->am_cb_id), cpu_to_be32(1),
                            cpu_to_be32(6), 0 };

        msm_rpc_apps_call(s, AUDMGR_CB_PROG, 0, 1, dis, 4);
    }
    q->am_pending = 0;
    while ((p = g_queue_pop_head(q->adsp_pending))) {
        uint32_t ev = GPOINTER_TO_UINT(p) >> 16, mod = GPOINTER_TO_UINT(p) &
                      0xFFFF;
        uint32_t a[5] = { cpu_to_be32(1), cpu_to_be32(ev), cpu_to_be32(2),
                          cpu_to_be32(mod), cpu_to_be32(0) };

        msm_rpc_apps_call(s, ADSP_MTOA_PROG, ADSP_MTOA_VERS, 2, a, 5);
    }
}

static MsmQdsp5 *qdsp5(MsmModemState *s)
{
    return s->qdsp5;
}

void msm_qdsp5_audmgr_call(MsmModemState *s, uint32_t proc,
                           const uint8_t *args, uint32_t len)
{
    MsmQdsp5 *q = qdsp5(s);

    if (!q) {
        return;
    }
    if (proc == 2 && len >= 28) {           /* ENABLE_CLIENT */
        q->am_cb_id = ldl_be_p(args + 24);
        q->am_pending = 1;
    } else if (proc == 3) {                 /* DISABLE_CLIENT */
        q->am_pending = 2;
    }
    if (q->am_pending) {
        timer_mod(q->rpc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SCALE_MS);
    }
}

void msm_qdsp5_adsp_call(MsmModemState *s, uint32_t proc,
                         const uint8_t *args, uint32_t len)
{
    MsmQdsp5 *q = qdsp5(s);
    uint32_t cmd, mod, ev;

    if (!q || proc != 2 || len < 16) {
        return;
    }
    cmd = ldl_be_p(args + 4);
    mod = ldl_be_p(args + 12);
    if (s->trace) {
        qemu_log("qdsp5: adsp rtos cmd %u module %u\n", cmd, mod);
    }
    if (cmd == 1) {
        ev = ADSP_EV_READY;
    } else if (cmd == 2) {
        ev = ADSP_EV_DISABLED;
    } else {
        return;
    }
    g_queue_push_tail(q->adsp_pending, GUINT_TO_POINTER(ev << 16 | mod));
    timer_mod(q->rpc_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              SCALE_MS);
}

void msm_qdsp5_init(MsmModemState *s, MemoryRegion *sysmem, hwaddr base,
                    qemu_irq irq)
{
    MsmQdsp5 *q = g_new0(MsmQdsp5, 1);

    q->modem = s;
    q->irq = irq;
    q->events = g_queue_new();
    q->adsp_pending = g_queue_new();
    q->write_ctrl = WR_READY;
    q->read_ctrl = RD_IDLE;
    q->rpc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rpc_callbacks, q);
    q->pcm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pcm_done, q);
    memory_region_init_ram(&q->mem, NULL, "dream.adsp-mem", DSP_MEM_SIZE,
                           &error_fatal);
    q->ram = memory_region_get_ram_ptr(&q->mem);
    memory_region_add_subregion(sysmem, base, &q->mem);
    memory_region_init_io(&q->ctrl, NULL, &ctrl_ops, q, "dream.adsp-ctrl",
                          8);
    memory_region_add_subregion(sysmem, base + WRITE_CTRL, &q->ctrl);
    memory_region_init_io(&q->irqreg, NULL, &irq_ops, q, "dream.adsp-irq",
                          4);
    memory_region_add_subregion(sysmem, base + SEND_IRQ, &q->irqreg);
    s->qdsp5 = q;
}

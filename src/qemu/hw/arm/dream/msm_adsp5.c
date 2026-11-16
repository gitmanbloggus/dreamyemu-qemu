/*
 * HTC Dream (MSM7201A) application DSP, "qdsp5" / aDSP.
 *
 * The aDSP's firmware lives with AMSS on the modem side, so Linux never
 * loads it: AMSS enables tasks over RPC (adsp_rtos, 0x3000000a, answered in
 * msm_rpc.c) and the apps core talks to the running tasks through the DSP's
 * memory at MSM_AD5_PHYS (0xAC000000, msm_iomap.h):
 *
 *   +0x400034  write control word   (apps -> DSP commands)
 *   +0x400038  read control word    (DSP -> apps events)
 *   +0xC00200  doorbell to the DSP  (any write)
 *   IRQ        INT_ADSP_A9_A11 (23), rising edge
 *
 * Command (adsp.c msm_adsp_write): the kernel puts a queue address (the
 * per-image offsets in adsp_6225.c) into the write control word with the
 * mutex taken and rings; the DSP drops the mutex and answers a buffer
 * address (below +0x400000: a 16-bit buffer).  The kernel copies the
 * command there, sets CMD = WRITE_DONE, rings again, and the DSP sets the
 * word back to READY.
 *
 * Event (adsp.c adsp_get_event): the DSP writes (task << 8 | msg id),
 * length in 16-bit words and the payload into its memory, sets the read
 * control word to READY with that address and raises the IRQ.  The kernel
 * copies it out, clears READY and rings; with nothing left the word reads
 * UP_CONT|DONE and the kernel's loop ends (-EAGAIN).
 *
 * Tasks modelled (AMSS 6225, "combo" image 0):
 *   AUDPPTASK (task 9): CFG enable/sleep, and the host PCM interface
 *       (/dev/msm_pcm_out, what AudioFlinger writes to): two ARM -> DSP
 *       buffers played in turn on the host's audio backend, each asked for
 *       again with HOST_PCM_INTF_MSG once it has been played.
 *   VIDEOTASK (task 4): the H.264 command/event sequence captured from
 *       stock Android (htcdreamfulldriverdecomp dsp/PLAN.md): the init
 *       command gets msg 0 {1, 0}; each subframe packet gets msgs 2 and 4
 *       {seq, codec instance}; the b203 end packet gets msg 0.  Each
 *       packet is rebuilt into a plain H.264 stream (dream_h264_rw.c) and,
 *       when QEMU is built with libavcodec, decoded into the packet's
 *       output buffer as NV21, as the DSP does.
 * Other tasks accept their commands and say nothing (-d unimp logs them).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "system/physmem.h"
#include "qemu/units.h"
#include "msm_adsp5.h"
#include "dream_h264_rw.h"
#ifdef DREAM_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#endif

OBJECT_DECLARE_SIMPLE_TYPE(MsmAdsp5State, MSM_ADSP5)

#define AD5_SIZE            0x01000000
#define AD5_WRITE_CTRL      0x00400034
#define AD5_READ_CTRL       0x00400038
#define AD5_SEND_IRQ        0x00C00200
#define AD5_RAM_SIZE        0x4000      /* modelled DSP memory, 16-bit side */
#define AD5_CMD_BUF         0x1000      /* where commands are written */
#define AD5_EVT_BUF         0x2000      /* where events are read from */
#define AD5_CMD_MAX         0x400       /* bytes */

/* adsp.h control words */
#define WC_MUTEX_NAVAIL     0x80000000u
#define WC_CMD_M            0x70000000u
#define WC_CMD_WRITE_REQ    0x00000000u
#define WC_CMD_WRITE_DONE   0x10000000u
#define WC_READY            0x70000000u     /* NO_CMD, mutex available */
#define WC_ADDR_M           0x00FFFFFFu
#define RC_READY_M          0xE0000000u
#define RC_READY            0xA0000000u     /* UP_CONT | READY, TASK_TO_H */
#define RC_DONE             0xE0000000u     /* UP_CONT | DONE: none pending */

/* combo image task ids and queue addresses (adsp_6225.c) */
#define TASK_VIDEO          4
#define TASK_AUDPP          9
#define Q_VDEC_CMD          0x6e8
#define Q_VDEC_PKT          0x6ec
#define Q_AUDPP_CMD1        0x6f0
#define Q_AUDPP_CMD2        0x6f4
#define Q_AUDPP_CMD3        0x6f8

/* qdsp5audppcmdi.h / qdsp5audppmsg.h */
#define AUDPP_CMD_CFG               0x0004
#define AUDPP_CMD_PCM_INTF_2        0x0002
#define AUDPP_MSG_HOST_PCM_INTF_MSG 0x0003
#define AUDPP_MSG_CFG_MSG           0x0007
#define AUDPP_MSG_HOSTPCM_ID_ARM_RX 0x0003
#define PCM_INTF_OBJECT_NUM         5

#define EVT_MAX_WORDS       16

typedef struct Adsp5Event {
    uint8_t task, msg;
    uint16_t n;
    uint16_t w[EVT_MAX_WORDS];
} Adsp5Event;

typedef struct Adsp5PcmBuf {
    uint8_t idx;
    uint32_t len;
} Adsp5PcmBuf;

struct MsmAdsp5State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint8_t ram[AD5_RAM_SIZE];
    uint32_t write_ctrl, read_ctrl;
    uint32_t cur_queue;
    bool evt_inflight;
    GQueue *events;             /* Adsp5Event * */
    QEMUTimer *evt_timer;

    /* AUDPP host PCM, ARM -> DSP */
    bool pcm_on;
    uint32_t pcm_addr[2], pcm_size[2];
    uint32_t pcm_rate, pcm_channels;
    GQueue *pcm_queue;          /* Adsp5PcmBuf *, waiting to be played */
    int pcm_playing;            /* buffer index being played, or -1 */
    QEMUTimer *pcm_timer;
    AudioBackend *audio_be;
    SWVoiceOut *voice;

    /* VIDEOTASK */
    H264Rw rw;
    uint8_t *annexb;
    int32_t chroma_qp_offset;
    uint64_t vdec_frames;
#ifdef DREAM_AVCODEC
    AVCodecContext *avc;
    AVFrame *frm;
    AVPacket *avpkt;
#endif

    bool trace;
};

/* ---------------------------------------------------------------- */
/* Events                                                            */
/* ---------------------------------------------------------------- */

static void adsp5_kick_events(MsmAdsp5State *s)
{
    if (!s->evt_inflight && !g_queue_is_empty(s->events)) {
        /* not from inside the kernel's MMIO sequence: a moment later */
        timer_mod(s->evt_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  20 * SCALE_US);
    }
}

static void adsp5_load_event(MsmAdsp5State *s)
{
    g_autofree Adsp5Event *e = g_queue_pop_head(s->events);

    stw_le_p(s->ram + AD5_EVT_BUF, (e->task << 8) | e->msg);
    stw_le_p(s->ram + AD5_EVT_BUF + 2, e->n);
    for (int i = 0; i < e->n; i++) {
        stw_le_p(s->ram + AD5_EVT_BUF + 4 + 2 * i, e->w[i]);
    }
    s->read_ctrl = RC_READY | AD5_EVT_BUF;
    s->evt_inflight = true;
    if (s->trace) {
        qemu_log("adsp5: event task %u msg %u (%u words)\n",
                 e->task, e->msg, e->n);
    }
}

static void adsp5_evt_timer(void *opaque)
{
    MsmAdsp5State *s = opaque;

    if (s->evt_inflight || g_queue_is_empty(s->events)) {
        return;
    }
    adsp5_load_event(s);
    qemu_irq_pulse(s->irq);
}

static void adsp5_event(MsmAdsp5State *s, uint8_t task, uint8_t msg,
                        const uint16_t *w, int n)
{
    Adsp5Event *e = g_new0(Adsp5Event, 1);

    e->task = task;
    e->msg = msg;
    e->n = MIN(n, EVT_MAX_WORDS);
    memcpy(e->w, w, e->n * 2);
    g_queue_push_tail(s->events, e);
    adsp5_kick_events(s);
}

/* ---------------------------------------------------------------- */
/* AUDPP: host PCM out                                               */
/* ---------------------------------------------------------------- */

static void adsp5_voice_cb(void *opaque, int avail)
{
    /* paced by pcm_timer; the backend takes what it gets */
}

static void adsp5_pcm_next(MsmAdsp5State *s)
{
    g_autofree Adsp5PcmBuf *b = NULL;
    g_autofree uint8_t *pcm = NULL;
    uint32_t frame = 2 * s->pcm_channels;
    int64_t ns;

    if (!s->pcm_on || s->pcm_playing >= 0 || g_queue_is_empty(s->pcm_queue)) {
        return;
    }
    b = g_queue_pop_head(s->pcm_queue);
    b->len = MIN(b->len, s->pcm_size[b->idx]);
    if (b->len < frame) {
        b->len = frame;         /* still hand the buffer back, later */
    }
    pcm = g_malloc0(b->len);
    physical_memory_read(s->pcm_addr[b->idx], pcm, b->len);
    if (s->voice && s->audio_be) {
        audio_be_write(s->audio_be, s->voice, pcm, b->len);
    }
    s->pcm_playing = b->idx;
    ns = muldiv64(b->len / frame, NANOSECONDS_PER_SECOND, s->pcm_rate);
    timer_mod(s->pcm_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void adsp5_pcm_timer(void *opaque)
{
    MsmAdsp5State *s = opaque;
    uint16_t m[6] = {
        PCM_INTF_OBJECT_NUM, 1, AUDPP_MSG_HOSTPCM_ID_ARM_RX, 0,
        4 /* SAMP_FREQ_INDX_44100 */, 0,
    };

    if (s->pcm_playing < 0) {
        return;
    }
    m[3] = s->pcm_playing + 1;
    m[5] = s->pcm_channels;
    s->pcm_playing = -1;
    if (s->pcm_on) {
        adsp5_event(s, TASK_AUDPP, AUDPP_MSG_HOST_PCM_INTF_MSG, m, 6);
        adsp5_pcm_next(s);
    }
}

static void adsp5_pcm_queue(MsmAdsp5State *s, int idx, uint32_t len)
{
    Adsp5PcmBuf *b = g_new0(Adsp5PcmBuf, 1);

    b->idx = idx;
    b->len = len;
    g_queue_push_tail(s->pcm_queue, b);
    adsp5_pcm_next(s);
}

static void adsp5_pcm_stop(MsmAdsp5State *s)
{
    s->pcm_on = false;
    s->pcm_playing = -1;
    timer_del(s->pcm_timer);
    g_queue_clear_full(s->pcm_queue, g_free);
    if (s->voice && s->audio_be) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
}

static void adsp5_pcm_start(MsmAdsp5State *s, const uint16_t *c)
{
    /* audpp_cmd_pcm_intf, in 16-bit words */
    struct audsettings as = { .fmt = AUDIO_FORMAT_S16, .big_endian = false };

    adsp5_pcm_stop(s);
    s->pcm_addr[0] = (uint32_t)c[13] << 16 | c[12];
    s->pcm_size[0] = c[14];
    s->pcm_addr[1] = (uint32_t)c[16] << 16 | c[15];
    s->pcm_size[1] = c[17];
    s->pcm_rate = c[22] ? c[22] : 44100;
    s->pcm_channels = c[23] == 1 ? 1 : 2;
    if (s->trace) {
        qemu_log("adsp5: pcm out %u Hz x%u, buffers %08x+%u %08x+%u\n",
                 s->pcm_rate, s->pcm_channels, s->pcm_addr[0],
                 s->pcm_size[0], s->pcm_addr[1], s->pcm_size[1]);
    }
    if (s->audio_be) {
        as.freq = s->pcm_rate;
        as.nchannels = s->pcm_channels;
        s->voice = audio_be_open_out(s->audio_be, s->voice, "dream.adsp.pcm",
                                     s, adsp5_voice_cb, &as);
        if (s->voice) {
            audio_be_set_active_out(s->audio_be, s->voice, true);
        }
    }
    s->pcm_on = true;
    /* audio_enable() has filled both buffers before it enables the DSP */
    adsp5_pcm_queue(s, 0, s->pcm_size[0]);
    adsp5_pcm_queue(s, 1, s->pcm_size[1]);
}

static void adsp5_audpp_cmd(MsmAdsp5State *s, uint32_t q, const uint16_t *c)
{
    switch (c[0]) {
    case AUDPP_CMD_CFG:
        if (q == Q_AUDPP_CMD1) {
            uint16_t m = c[1] ? 0xFFFF : 0x0000;    /* ENA_ENA / ENA_DIS */

            if (!c[1]) {
                adsp5_pcm_stop(s);
            }
            adsp5_event(s, TASK_AUDPP, AUDPP_MSG_CFG_MSG, &m, 1);
            return;
        }
        break;
    case AUDPP_CMD_PCM_INTF_2:
        if (q == Q_AUDPP_CMD2 && c[1] == PCM_INTF_OBJECT_NUM) {
            if (c[2] == 0) {            /* configuration */
                if ((int16_t)c[18] == -1) {
                    adsp5_pcm_start(s, c);
                } else {
                    adsp5_pcm_stop(s);
                }
            } else if (c[5] >= 1 && c[5] <= 2) {  /* buffer update */
                adsp5_pcm_queue(s, c[5] - 1, c[6]);
            }
            return;
        }
        break;
    }
    /* volume, EQ, ADRC, IIR, AVSYNC, DEC_CTRL: accepted, no effect */
    qemu_log_mask(LOG_UNIMP, "adsp5: AUDPP queue %03x cmd %04x ignored\n",
                  q, c[0]);
}

/* ---------------------------------------------------------------- */
/* VIDEOTASK                                                         */
/* ---------------------------------------------------------------- */

#define VDEC_PKT_MAX        (2 * MiB)       /* bytes of one subframe packet */

#ifdef DREAM_AVCODEC
static void adsp5_vdec_close(MsmAdsp5State *s)
{
    avcodec_free_context(&s->avc);
    av_frame_free(&s->frm);
    av_packet_free(&s->avpkt);
}

static void adsp5_vdec_open(MsmAdsp5State *s)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);

    adsp5_vdec_close(s);
    if (!codec) {
        return;
    }
    s->avc = avcodec_alloc_context3(codec);
    s->avc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    s->avc->thread_count = 1;
    if (avcodec_open2(s->avc, codec, NULL) < 0) {
        avcodec_free_context(&s->avc);
        return;
    }
    s->frm = av_frame_alloc();
    s->avpkt = av_packet_alloc();
}

/* the decoded picture into the packet's output buffer, as NV21 */
static void adsp5_vdec_store(MsmAdsp5State *s, const H264RwPic *pic)
{
    AVFrame *f = s->frm;
    int w = MIN(pic->width, f->width), h = MIN(pic->height, f->height);
    uint32_t lw = pic->line_width, y0 = pic->out_addr;
    uint32_t c0 = y0 + lw * pic->height;
    g_autofree uint8_t *row = g_malloc(lw);

    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        return;
    }
    for (int y = 0; y < h; y++) {
        physical_memory_write(y0 + y * lw, f->data[0] + y * f->linesize[0], w);
    }
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = f->data[1] + y * f->linesize[1];
        const uint8_t *v = f->data[2] + y * f->linesize[2];

        for (int x = 0; x < w / 2; x++) {
            row[2 * x] = v[x];
            row[2 * x + 1] = u[x];
        }
        physical_memory_write(c0 + y * lw, row, w);
    }
}

static void adsp5_vdec_decode(MsmAdsp5State *s, const uint8_t *nal, int n,
                              const H264RwPic *pic)
{
    if (!s->avc && (adsp5_vdec_open(s), !s->avc)) {
        return;
    }
    s->avpkt->data = (uint8_t *)nal;
    s->avpkt->size = n;
    if (avcodec_send_packet(s->avc, s->avpkt) < 0) {
        return;
    }
    while (avcodec_receive_frame(s->avc, s->frm) == 0) {
        adsp5_vdec_store(s, pic);
        s->vdec_frames++;
    }
}
#endif

/*
 * One subframe packet: rebuilt into H.264 (dream_h264_rw.c), decoded, and
 * the picture stored in the packet's output buffer before the DSP says
 * it is done, as the real DSP has by then.
 */
static bool adsp5_vdec_packet(MsmAdsp5State *s, uint32_t addr, uint32_t words)
{
    g_autofree uint16_t *w = NULL;
    H264RwPic pic;
    int n;

    words = MIN(words, VDEC_PKT_MAX / 2);
    w = g_new(uint16_t, MAX(words, 1));
    physical_memory_read(addr, w, words * 2);
    for (uint32_t i = 0; i < words; i++) {
        w[i] = le16_to_cpu(w[i]);
    }
    n = h264rw_packet(&s->rw, w, words, s->annexb, VDEC_PKT_MAX + 4096,
                      &pic);
    if (pic.end) {
        return true;
    }
    if (n < 0) {
        qemu_log_mask(LOG_UNIMP, "adsp5: VIDEOTASK packet at %08x (%u words)"
                      " not understood\n", addr, words);
        return false;
    }
#ifdef DREAM_AVCODEC
    adsp5_vdec_decode(s, s->annexb, n, &pic);
#endif
    if (s->trace) {
        qemu_log("adsp5: vdec %dx%d -> %08x (%d bytes of H.264)\n",
                 pic.width, pic.height, pic.out_addr, n);
    }
    return false;
}

static void adsp5_vdec_cmd(MsmAdsp5State *s, uint32_t q, const uint16_t *c)
{
    if (q == Q_VDEC_CMD) {
        static const uint16_t init[2] = { 1, 0 };

        /* a new decoding session */
        s->rw = (H264Rw){ .chroma_qp_offset = s->chroma_qp_offset };
#ifdef DREAM_AVCODEC
        adsp5_vdec_close(s);
#endif
        adsp5_event(s, TASK_VIDEO, 0, init, 2);
        return;
    }
    if (c[0] == 0x0000) {               /* VIDDEC_CMD_SUBFRAME_PKT */
        uint32_t words = (uint32_t)c[3] << 16 | c[4];
        uint32_t pkt = (uint32_t)c[5] << 16 | c[6];
        uint16_t m[2] = { c[1], c[2] }; /* seq, codec instance */

        if (adsp5_vdec_packet(s, pkt, words)) {     /* end of stream */
            m[0] = 0;
            adsp5_event(s, TASK_VIDEO, 0, m, 2);
        } else {
            adsp5_event(s, TASK_VIDEO, 2, m, 2);
            adsp5_event(s, TASK_VIDEO, 4, m, 2);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "adsp5: VIDEOTASK cmd %04x ignored\n", c[0]);
}

/* ---------------------------------------------------------------- */
/* Control words                                                     */
/* ---------------------------------------------------------------- */

static void adsp5_command(MsmAdsp5State *s, uint32_t q)
{
    uint16_t c[AD5_CMD_MAX / 2];

    for (int i = 0; i < ARRAY_SIZE(c); i++) {
        c[i] = lduw_le_p(s->ram + AD5_CMD_BUF + 2 * i);
    }
    if (s->trace) {
        qemu_log("adsp5: cmd queue %03x: %04x %04x %04x %04x %04x %04x\n",
                 q, c[0], c[1], c[2], c[3], c[4], c[5]);
    }
    switch (q) {
    case Q_AUDPP_CMD1:
    case Q_AUDPP_CMD2:
    case Q_AUDPP_CMD3:
        adsp5_audpp_cmd(s, q, c);
        break;
    case Q_VDEC_CMD:
    case Q_VDEC_PKT:
        adsp5_vdec_cmd(s, q, c);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "adsp5: queue %03x cmd %04x ignored\n",
                      q, c[0]);
        break;
    }
}

static void adsp5_doorbell(MsmAdsp5State *s)
{
    uint32_t wc = s->write_ctrl;

    if (wc & WC_MUTEX_NAVAIL) {
        switch (wc & WC_CMD_M) {
        case WC_CMD_WRITE_REQ:
            s->cur_queue = wc & WC_ADDR_M;
            memset(s->ram + AD5_CMD_BUF, 0, AD5_CMD_MAX);
            s->write_ctrl = AD5_CMD_BUF;    /* mutex free, no error */
            return;
        case WC_CMD_WRITE_DONE:
            s->write_ctrl = WC_READY;
            adsp5_command(s, s->cur_queue);
            return;
        }
    }
    if (s->evt_inflight && !(s->read_ctrl & RC_READY_M)) {
        /* the kernel has read the event; the next one, if any, at once */
        s->evt_inflight = false;
        if (!g_queue_is_empty(s->events)) {
            adsp5_load_event(s);
        } else {
            s->read_ctrl = RC_DONE;
        }
    }
}

static uint64_t adsp5_read(void *opaque, hwaddr off, unsigned size)
{
    MsmAdsp5State *s = opaque;

    switch (off) {
    case AD5_WRITE_CTRL:
        return s->write_ctrl;
    case AD5_READ_CTRL:
        return s->read_ctrl;
    }
    if (off + size <= AD5_RAM_SIZE) {
        return ldn_le_p(s->ram + off, size);
    }
    qemu_log_mask(LOG_UNIMP, "adsp5: read +%06" HWADDR_PRIx "\n", off);
    return 0;
}

static void adsp5_write(void *opaque, hwaddr off, uint64_t val,
                        unsigned size)
{
    MsmAdsp5State *s = opaque;

    switch (off) {
    case AD5_WRITE_CTRL:
        s->write_ctrl = val;
        return;
    case AD5_READ_CTRL:
        s->read_ctrl = val;
        return;
    case AD5_SEND_IRQ:
        adsp5_doorbell(s);
        return;
    }
    if (off + size <= AD5_RAM_SIZE) {
        stn_le_p(s->ram + off, size, val);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "adsp5: write +%06" HWADDR_PRIx " = %08"
                  PRIx64 "\n", off, val);
}

static const MemoryRegionOps adsp5_ops = {
    .read = adsp5_read,
    .write = adsp5_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

void msm_adsp5_module_enable(void *adsp, uint32_t module, bool enable)
{
    MsmAdsp5State *s = adsp;

    if (!s) {
        return;
    }
    if (s->trace) {
        qemu_log("adsp5: module %u %s\n", module,
                 enable ? "enabled" : "disabled");
    }
    if (module == ADSP5_MOD_AUDPPTASK && !enable) {
        adsp5_pcm_stop(s);
    }
}

static void adsp5_reset(DeviceState *dev)
{
    MsmAdsp5State *s = MSM_ADSP5(dev);

    adsp5_pcm_stop(s);
    memset(s->ram, 0, sizeof(s->ram));
    s->write_ctrl = WC_READY;
    s->read_ctrl = RC_DONE;
    s->cur_queue = 0;
    s->evt_inflight = false;
    g_queue_clear_full(s->events, g_free);
    timer_del(s->evt_timer);
}

static void adsp5_realize(DeviceState *dev, Error **errp)
{
    MsmAdsp5State *s = MSM_ADSP5(dev);

    if (!audio_be_check(&s->audio_be, NULL)) {
        s->audio_be = NULL;
    }
    s->events = g_queue_new();
    s->pcm_queue = g_queue_new();
    s->pcm_playing = -1;
    s->evt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adsp5_evt_timer, s);
    s->pcm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, adsp5_pcm_timer, s);
    s->annexb = g_malloc(VDEC_PKT_MAX + 4096);
#ifndef DREAM_AVCODEC
    warn_report_once("msm-adsp5: built without libavcodec; "
                     "VIDEOTASK frames stay blank");
#endif
}

static void adsp5_init(Object *obj)
{
    MsmAdsp5State *s = MSM_ADSP5(obj);

    memory_region_init_io(&s->iomem, obj, &adsp5_ops, s, "msm-adsp5",
                          AD5_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property adsp5_props[] = {
    DEFINE_AUDIO_PROPERTIES(MsmAdsp5State, audio_be),
    DEFINE_PROP_BOOL("trace", MsmAdsp5State, trace, false),
    /* the stream's PPS chroma_qp_index_offset: not in the packets */
    DEFINE_PROP_INT32("vdec-chroma-qp-offset", MsmAdsp5State,
                      chroma_qp_offset, -2),
};

static void adsp5_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = adsp5_realize;
    device_class_set_legacy_reset(dc, adsp5_reset);
    device_class_set_props(dc, adsp5_props);
}

static const TypeInfo adsp5_info = {
    .name = TYPE_MSM_ADSP5,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmAdsp5State),
    .instance_init = adsp5_init,
    .class_init = adsp5_class_init,
};

static void adsp5_register_types(void)
{
    type_register_static(&adsp5_info);
}

type_init(adsp5_register_types)

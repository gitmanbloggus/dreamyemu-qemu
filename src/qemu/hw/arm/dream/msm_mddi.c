/*
 * MSM7x01A MDDI host (PMDH, 0xAA600000, VIC line 16), first cut.
 *
 * Layout from the trout kernel's drivers/video/msm/mddi_hw.h: CMD 0x00,
 * INT 0x18, INTEN 0x1C, STAT 0x28, CORE_VER 0x8C.  The Dream's SPL spins
 * on INT right after the I2C/GPIO setup (measured: 264255 reads of
 * +0x18 in 20 s with a plain unimplemented page), and the kernel's
 * mddi_init refuses a core version <= 31.
 *
 * This model has no client.  Every command finishes at once and latches
 * the events a real host would raise for it into INT, which the guest
 * clears by writing the bits back (W1C); the interrupt line is
 * (INT & INTEN).  An earlier cut reported every INT bit as always set:
 * enough for the SPL, but the kernel's ISR tracks the link by flipping
 * between LINK_ACTIVE and IN_HIBERNATION and stormed (measured: 2.2M
 * INT/INTEN writes in 30 s, INTEN alternating 0x2000/0x4000).
 *
 * Bits and commands: trout drivers/video/msm/mddi_hw.h as recalled;
 * NO_CMD_PKTS_PEND = 0x20000 is confirmed by the Rust run's
 * "mddi_wait_interrupt ... timeout waiting for 20000".  Reverse packets
 * (client caps, RTD) are not modeled yet.
 *
 * Client capabilities: GET_CLIENT_CAP DMAs a type-66 Client Capability
 * Packet into the reverse buffer at REV_PTR and raises REV_DATA_AVAIL with
 * REV_PKT_CNT = 1, which is how the kernel discovers the panel and binds
 * "mddi_c_<Mfr_Name>_<Product_Code>".  Without it: "mddi_pmdh: no client
 * found" and no fb0.  The Dream's panel: Mfr 0xD263, product 0x0000
 * (the "mddi_c_d263_0000" name in the trout vmlinux, per the Rust
 * bring-up), 320x480.  Packet layout per the MDDI client caps packet
 * (76 bytes, length field 74).
 *
 * Client registers: the host walks each primary list (PRI_PTR) for
 * Register Access packets (type 146).  Writes land in a small register
 * file; a read (read_write_info bit 15) is answered with a type-146
 * reverse packet {length 16, type, client, info, address, crc, data}
 * and REV_DATA_AVAIL.  Measured from the SPL's list at SMI 0x280000:
 * packed entry {u16 flags, u16 header_count, u16 data_count, u32 data
 * @+6, u32 next @+10}, packet at +16: a read is {14, 146, 0, 0x8001,
 * 0x00150000}; a write is {0x12, 146, 0, 0x0001, addr} with its value
 * at the data pointer (data_count 4).  Without answers the
 * SPL logged "MddiWaitReserve Fail [2A001]" per read and gave up on the
 * panel (bIsDisplayInited=0); the kernel logged "unknown panel id".
 * Unwritten registers read 0: the panel ID the hardware returns is not
 * known yet.
 *
 * The panel: the client keeps its own 320x480 RGB565 frame memory, shown
 * on a QEMU console.  Two writers reach it, as on the phone:
 *  - Video Stream packets (type 16) in a primary list, which is how the
 *    SPL/HBOOT draws: one packet per row, format 0x5666, window
 *    0..319 x 0..479, start (0, row), 320 pixels at the entry's data
 *    pointer (measured from its lists at SMI 0x230000, 0x28 bytes each).
 *    0x5666 is RGB 6-6-6: 18-bit pixels packed as a little-endian bit
 *    stream, blue lowest (720 bytes per 320-pixel row; read as 565 it
 *    came out as 9-pixel colour stripes).  0x5565 is plain RGB565;
 *  - the MDP's DMA2 output (the kernel's msm_fb), via msm_mddi_panel_blit.
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "msm_mddi.h"

#define MDDI_CMD        0x00
#define MDDI_INT        0x18
#define MDDI_INTEN      0x1C
#define MDDI_STAT       0x28
#define MDDI_CORE_VER   0x8C

#define MDDI_PRI_PTR    0x08
#define MDDI_REV_PTR    0x20
#define MDDI_REV_SIZE   0x24
#define MDDI_REV_CRC_ERR 0x30
#define MDDI_REV_PKT_CNT 0x44

#define MDDI_INT_REV_DATA_AVAIL     0x00000004
#define MDDI_TYPE_REG_ACCESS        146
#define MDDI_TYPE_VIDEO             16
#define MDDI_NREGS                  32
#define MDDI_LIST_NS                20000       /* 20 us */
#define MDDI_CMD_GET_CLIENT_CAP     0x0601
#define MDDI_CMD_FORCE_NEW_REV_PTR  0x0C00

#define MDDI_CLIENT_MFR     0xD263
#define MDDI_CLIENT_PRODUCT 0x0000
#define MDDI_CLIENT_WIDTH   320
#define MDDI_CLIENT_HEIGHT  480

#define MDDI_INT_PRI_PTR_READ       0x00000001
#define MDDI_INT_SEC_PTR_READ       0x00000002
#define MDDI_INT_LINK_ACTIVE        0x00002000
#define MDDI_INT_IN_HIBERNATION     0x00004000
#define MDDI_INT_PRI_LINK_LIST_DONE 0x00008000
#define MDDI_INT_NO_CMD_PKTS_PEND   0x00020000

#define MDDI_CMD_HIBERNATE          0x0300
#define MDDI_CMD_LINK_ACTIVE        0x0900

#define MDDI_STAT_IDLE  0xFFFFFFFFu
#define MDDI_VERSION    0x0020

OBJECT_DECLARE_SIMPLE_TYPE(MsmMddiState, MSM_MDDI)

struct MsmMddiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x100 / 4];
    uint32_t intr;          /* latched INT bits */
    uint32_t rev_off;       /* next write offset in the reverse buffer */
    uint32_t rev_pkts;      /* REV_PKT_CNT */
    uint32_t creg_addr[MDDI_NREGS], creg_val[MDDI_NREGS];
    int ncregs;
    QEMUTimer *list_done;   /* a primary list finishes after a moment */
    QemuConsole *con;
    uint16_t *panel;        /* client frame memory, RGB565 */
    bool dirty;
    bool trace;
};

static void mddi_update_irq(MsmMddiState *s)
{
    qemu_set_irq(s->irq, !!(s->intr & s->regs[MDDI_INTEN / 4]));
}

static void mddi_rev_packet(MsmMddiState *s, const uint8_t *pkt, int len)
{
    uint32_t base = s->regs[MDDI_REV_PTR / 4];
    uint32_t size = s->regs[MDDI_REV_SIZE / 4];

    if (!base) {
        return;
    }
    if (size && s->rev_off + len > size) {
        s->rev_off = 0;
    }
    address_space_write(&address_space_memory, base + s->rev_off,
                        MEMTXATTRS_UNSPECIFIED, pkt, len);
    if (s->trace) {
        qemu_log("msm-mddi: rev packet type %u (%d bytes) at 0x%08x\n",
                 lduw_le_p(pkt + 2), len, base + s->rev_off);
    }
    s->rev_off += len;
    s->rev_pkts++;
    s->intr |= MDDI_INT_REV_DATA_AVAIL;
}

static void mddi_send_caps(MsmMddiState *s)
{
    uint8_t p[76] = { 0 };

    stw_le_p(p + 0, sizeof(p) - 2);         /* length */
    stw_le_p(p + 2, 66);                    /* client capability packet */
    stw_le_p(p + 6, 1);                     /* protocol version */
    stw_le_p(p + 16, MDDI_CLIENT_WIDTH);    /* bitmap width */
    stw_le_p(p + 18, MDDI_CLIENT_HEIGHT);   /* bitmap height */
    stw_le_p(p + 20, MDDI_CLIENT_WIDTH);    /* display window */
    stw_le_p(p + 22, MDDI_CLIENT_HEIGHT);
    stw_le_p(p + 30, 0x8565);               /* RGB capability: 565 */
    stw_le_p(p + 62, MDDI_CLIENT_MFR);      /* Mfr_Name */
    stw_le_p(p + 64, MDDI_CLIENT_PRODUCT);  /* Product_Code */
    mddi_rev_packet(s, p, sizeof(p));
}

static uint32_t *mddi_creg(MsmMddiState *s, uint32_t addr, bool create)
{
    for (int i = 0; i < s->ncregs; i++) {
        if (s->creg_addr[i] == addr) {
            return &s->creg_val[i];
        }
    }
    if (!create || s->ncregs == MDDI_NREGS) {
        return NULL;
    }
    s->creg_addr[s->ncregs] = addr;
    s->creg_val[s->ncregs] = 0;
    return &s->creg_val[s->ncregs++];
}

static void mddi_reg_reply(MsmMddiState *s, uint32_t addr)
{
    uint8_t p[18] = { 0 };
    uint32_t *v = mddi_creg(s, addr, false);

    stw_le_p(p + 0, sizeof(p) - 2);
    stw_le_p(p + 2, MDDI_TYPE_REG_ACCESS);
    stw_le_p(p + 6, 1);
    stl_le_p(p + 8, addr);
    stl_le_p(p + 14, v ? *v : 0);
    mddi_rev_packet(s, p, sizeof(p));
}

/* A Video Stream packet: pixels into the panel's frame memory */
static void mddi_video(MsmMddiState *s, const uint8_t *pk, uint32_t dptr,
                       uint32_t dcount)
{
    uint16_t fmt = lduw_le_p(pk + 6);
    int xl = lduw_le_p(pk + 10), yt = lduw_le_p(pk + 12);
    int xr = lduw_le_p(pk + 14), yb = lduw_le_p(pk + 16);
    int x = lduw_le_p(pk + 18), y = lduw_le_p(pk + 20);
    int n = lduw_le_p(pk + 22);
    g_autofree uint8_t *raw = NULL;
    int bpp = (fmt == 0x5666) ? 18 : 16;

    if (s->trace) {
        qemu_log("msm-mddi: video fmt 0x%04x win %d,%d-%d,%d at %d,%d n %d "
                 "data 0x%x/%u\n", fmt, xl, yt, xr, yb, x, y, n, dptr, dcount);
    }
    if ((fmt != 0x5565 && fmt != 0x5666) || !dptr || !n) {
        return;     /* RGB565 and RGB666 so far */
    }
    n = MIN(n, (int)(dcount * 8 / bpp));
    raw = g_malloc0(dcount + 4);
    address_space_read(&address_space_memory, dptr, MEMTXATTRS_UNSPECIFIED,
                       raw, dcount);
    for (int i = 0; i < n; i++) {
        uint16_t p;

        if (bpp == 16) {
            p = lduw_le_p(raw + 2 * i);
        } else {
            unsigned bit = i * 18;
            uint32_t v = ldl_le_p(raw + bit / 8) >> (bit % 8);
            uint32_t b = v & 0x3F, g = (v >> 6) & 0x3F, r = (v >> 12) & 0x3F;

            p = ((r >> 1) << 11) | (g << 5) | (b >> 1);
        }
        if (x >= 0 && x < MDDI_CLIENT_WIDTH && y >= 0 &&
            y < MDDI_CLIENT_HEIGHT) {
            s->panel[y * MDDI_CLIENT_WIDTH + x] = p;
        }
        if (++x > xr) {
            x = xl;
            if (++y > yb) {
                y = yt;
            }
        }
    }
    s->dirty = true;
}

void msm_mddi_panel_blit(DeviceState *dev, uint32_t base, uint32_t stride,
                         int w, int h, int x, int y)
{
    MsmMddiState *s = MSM_MDDI(dev);

    if (x >= MDDI_CLIENT_WIDTH || y >= MDDI_CLIENT_HEIGHT) {
        return;
    }
    w = MIN(w, MDDI_CLIENT_WIDTH - x);
    h = MIN(h, MDDI_CLIENT_HEIGHT - y);
    for (int r = 0; r < h; r++) {
        address_space_read(&address_space_memory, base + r * stride,
                           MEMTXATTRS_UNSPECIFIED,
                           s->panel + (y + r) * MDDI_CLIENT_WIDTH + x, w * 2);
    }
    s->dirty = true;
}

static bool mddi_gfx_update(void *opaque)
{
    MsmMddiState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    uint8_t *dst;
    int dstride;

    if (!s->dirty) {
        return true;
    }
    if (surface_width(surf) != MDDI_CLIENT_WIDTH ||
        surface_height(surf) != MDDI_CLIENT_HEIGHT) {
        qemu_console_resize(s->con, MDDI_CLIENT_WIDTH, MDDI_CLIENT_HEIGHT);
        surf = qemu_console_surface(s->con);
    }
    dst = surface_data(surf);
    dstride = surface_stride(surf);
    for (int y = 0; y < MDDI_CLIENT_HEIGHT; y++) {
        uint32_t *d = (uint32_t *)(dst + y * dstride);
        for (int x = 0; x < MDDI_CLIENT_WIDTH; x++) {
            uint16_t p = le16_to_cpu(s->panel[y * MDDI_CLIENT_WIDTH + x]);
            uint8_t r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2,
                    b = (p & 0x1F) << 3;
            d[x] = rgb_to_pixel32(r | r >> 5, g | g >> 6, b | b >> 5);
        }
    }
    qemu_console_update(s->con, 0, 0, MDDI_CLIENT_WIDTH, MDDI_CLIENT_HEIGHT);
    s->dirty = false;
    return true;
}

static void mddi_gfx_invalidate(void *opaque)
{
    MsmMddiState *s = opaque;
    s->dirty = true;
}

static const GraphicHwOps mddi_gfx_ops = {
    .invalidate = mddi_gfx_invalidate,
    .gfx_update = mddi_gfx_update,
};

/* Walk a primary list for client register accesses and video */
static void mddi_run_list(MsmMddiState *s, uint32_t ent)
{
    for (int n = 0; ent && n < 256; n++) {
        uint8_t e[16 + 26];
        uint16_t hdr, type, info;
        uint32_t next, dptr, addr;

        address_space_read(&address_space_memory, ent, MEMTXATTRS_UNSPECIFIED,
                           e, sizeof(e));
        hdr = lduw_le_p(e + 2);
        dptr = ldl_le_p(e + 6);
        next = ldl_le_p(e + 10);
        type = lduw_le_p(e + 18);
        info = lduw_le_p(e + 22);
        addr = ldl_le_p(e + 24);
        if (hdr >= 24 && type == MDDI_TYPE_VIDEO) {
            mddi_video(s, e + 16, dptr, lduw_le_p(e + 4));
        } else if (hdr >= 12 && type == MDDI_TYPE_REG_ACCESS) {
            if (info & 0x8000) {
                if (s->trace) {
                    qemu_log("msm-mddi: client reg read 0x%x\n", addr);
                }
                mddi_reg_reply(s, addr);
            } else if (dptr && lduw_le_p(e + 4) >= 4) {
                uint32_t val = 0;
                uint32_t *r = mddi_creg(s, addr, true);

                address_space_read(&address_space_memory, dptr,
                                   MEMTXATTRS_UNSPECIFIED, &val, 4);
                val = le32_to_cpu(val);
                if (r) {
                    *r = val;
                }
                if (s->trace) {
                    qemu_log("msm-mddi: client reg 0x%x <- 0x%x\n", addr, val);
                }
            }
        }
        if (next == ent) {
            break;
        }
        ent = next;
    }
}

/* What finishing a command (or a primary packet list) raises */
static void mddi_command(MsmMddiState *s, uint32_t cmd)
{
    uint32_t ev = MDDI_INT_NO_CMD_PKTS_PEND;

    switch (cmd & 0xFF00) {
    case MDDI_CMD_HIBERNATE:
        /* bit 0 set = auto-hibernate on: the idle link sleeps */
        ev |= (cmd & 1) ? MDDI_INT_IN_HIBERNATION : MDDI_INT_LINK_ACTIVE;
        break;
    case MDDI_CMD_LINK_ACTIVE:
        ev |= MDDI_INT_LINK_ACTIVE;
        break;
    case MDDI_CMD_FORCE_NEW_REV_PTR:
        s->rev_off = 0;
        break;
    }
    if (cmd == MDDI_CMD_GET_CLIENT_CAP) {
        mddi_send_caps(s);
        ev |= MDDI_INT_LINK_ACTIVE;
    }
    s->intr |= ev;
}

static uint64_t mddi_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMddiState *s = opaque;
    uint32_t v;

    switch (off) {
    case MDDI_INT:
        v = s->intr;
        break;
    case MDDI_STAT:
        v = MDDI_STAT_IDLE;
        break;
    case MDDI_CORE_VER:
        v = MDDI_VERSION;
        break;
    case MDDI_REV_PKT_CNT:
        /*
         * Read-to-clear.  The kernel ISR (vmlinux 0xC011E3B4) writes INT
         * back to INT, clearing REV_DATA_AVAIL, and only then reads this
         * once; clearing the count with the INT bit lost every packet.
         */
        v = s->rev_pkts;
        s->rev_pkts = 0;
        break;
    case MDDI_REV_CRC_ERR:
        v = 0;
        break;
    default:
        v = off < sizeof(s->regs) ? s->regs[off / 4] : 0;
        break;
    }
    if (s->trace && off != MDDI_INT) {
        qemu_log("msm-mddi: read 0x%02" HWADDR_PRIx " = 0x%08x\n", off, v);
    }
    return v;
}

static void mddi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMddiState *s = opaque;

    if (s->trace) {
        qemu_log("msm-mddi: write 0x%02" HWADDR_PRIx " = 0x%08x\n", off,
                 (uint32_t)val);
    }
    if (off >= sizeof(s->regs) || off == MDDI_CORE_VER) {
        return;
    }
    switch (off) {
    case MDDI_INT:
        s->intr &= ~val;
        break;
    case MDDI_REV_PTR:
        s->rev_off = 0;
        break;
    case MDDI_CMD:
        mddi_command(s, val);
        break;
    case MDDI_PRI_PTR:
        /*
         * The list's effects are immediate; its completion is raised a
         * moment later, as the hardware takes time to send it.  Raised at
         * once, the kernel's ISR consumed PRI_LINK_LIST_DONE before
         * mddi_remote_write enabled it, and every write waited out
         * "mddi_wait_interrupt ... timeout waiting for 8000" (measured,
         * normal boot, from 9.7 s on).
         */
        mddi_run_list(s, val);
        s->intr |= MDDI_INT_LINK_ACTIVE;
        timer_mod(s->list_done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MDDI_LIST_NS);
        break;
    }
    s->regs[off / 4] = val;
    mddi_update_irq(s);
}

static const MemoryRegionOps mddi_ops = {
    .read = mddi_read,
    .write = mddi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mddi_reset(DeviceState *dev)
{
    MsmMddiState *s = MSM_MDDI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->intr = MDDI_INT_NO_CMD_PKTS_PEND;
    s->rev_off = s->rev_pkts = 0;
    s->ncregs = 0;
    mddi_update_irq(s);
}

static void mddi_list_done(void *opaque)
{
    MsmMddiState *s = opaque;

    s->intr |= MDDI_INT_PRI_PTR_READ | MDDI_INT_PRI_LINK_LIST_DONE |
               MDDI_INT_NO_CMD_PKTS_PEND;
    mddi_update_irq(s);
}

static void mddi_realize(DeviceState *dev, Error **errp)
{
    MsmMddiState *s = MSM_MDDI(dev);

    s->panel = g_new0(uint16_t, MDDI_CLIENT_WIDTH * MDDI_CLIENT_HEIGHT);
    s->list_done = timer_new_ns(QEMU_CLOCK_VIRTUAL, mddi_list_done, s);
    s->con = qemu_graphic_console_create(dev, 0, &mddi_gfx_ops, s);
    qemu_console_resize(s->con, MDDI_CLIENT_WIDTH, MDDI_CLIENT_HEIGHT);
}

static void mddi_init(Object *obj)
{
    MsmMddiState *s = MSM_MDDI(obj);

    memory_region_init_io(&s->iomem, obj, &mddi_ops, s, "msm-mddi", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription mddi_vmstate = {
    .name = "msm-mddi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MsmMddiState, 0x100 / 4),
        VMSTATE_UINT32(intr, MsmMddiState),
        VMSTATE_UINT32(rev_off, MsmMddiState),
        VMSTATE_UINT32(rev_pkts, MsmMddiState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mddi_props[] = {
    DEFINE_PROP_BOOL("trace", MsmMddiState, trace, false),
};

static void mddi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mddi_reset);
    dc->realize = mddi_realize;
    device_class_set_props(dc, mddi_props);
    dc->vmsd = &mddi_vmstate;
}

static const TypeInfo mddi_info = {
    .name = TYPE_MSM_MDDI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMddiState),
    .instance_init = mddi_init,
    .class_init = mddi_class_init,
};

static void mddi_register(void)
{
    type_register_static(&mddi_info);
}

type_init(mddi_register)

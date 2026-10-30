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
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "msm_mddi.h"

#define MDDI_CMD        0x00
#define MDDI_INT        0x18
#define MDDI_INTEN      0x1C
#define MDDI_STAT       0x28
#define MDDI_CORE_VER   0x8C

#define MDDI_PRI_PTR    0x08

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
    bool trace;
};

static void mddi_update_irq(MsmMddiState *s)
{
    qemu_set_irq(s->irq, !!(s->intr & s->regs[MDDI_INTEN / 4]));
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
    case MDDI_CMD:
        mddi_command(s, val);
        break;
    case MDDI_PRI_PTR:
        /* the list is sent at once; the link wakes for it */
        s->intr |= MDDI_INT_PRI_PTR_READ | MDDI_INT_PRI_LINK_LIST_DONE |
                   MDDI_INT_NO_CMD_PKTS_PEND | MDDI_INT_LINK_ACTIVE;
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
    mddi_update_irq(s);
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

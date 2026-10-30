/*
 * MSM7x01A MDDI host (PMDH, 0xAA600000, VIC line 16), first cut.
 *
 * Layout from the trout kernel's drivers/video/msm/mddi_hw.h: CMD 0x00,
 * INT 0x18, INTEN 0x1C, STAT 0x28, CORE_VER 0x8C.  The Dream's SPL spins
 * on INT right after the I2C/GPIO setup (measured: 264255 reads of
 * +0x18 in 20 s with a plain unimplemented page), and the kernel's
 * mddi_init refuses a core version <= 31.
 *
 * This model has no link and no client: every command finishes at once,
 * INT/STAT read as an idle host with the link up (all status bits set
 * except the four error bits), and the interrupt line is (INT & INTEN).
 * That is the state the dreamyemu (Rust) harness pre-filled into its page
 * (INT/STAT all-ones, version 0x20) to get the SPL and mddi_init through;
 * the error bits are cleared here because a host that reports underflow
 * and CRC errors is not idle.  Reverse packets (client caps, RTD) are not
 * modeled yet: that is the next step once the kernel asks for them.
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

/* PRI/SEC underflow, reverse overflow, CRC error */
#define MDDI_INT_ERRORS 0x000000F0u
#define MDDI_INT_IDLE   (0xFFFFFFFFu & ~MDDI_INT_ERRORS)
#define MDDI_STAT_IDLE  0xFFFFFFFFu
#define MDDI_VERSION    0x0020

OBJECT_DECLARE_SIMPLE_TYPE(MsmMddiState, MSM_MDDI)

struct MsmMddiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x100 / 4];
    bool trace;
};

static void mddi_update_irq(MsmMddiState *s)
{
    qemu_set_irq(s->irq, !!(MDDI_INT_IDLE & s->regs[MDDI_INTEN / 4]));
}

static uint64_t mddi_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMddiState *s = opaque;
    uint32_t v;

    switch (off) {
    case MDDI_INT:
        v = MDDI_INT_IDLE;
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
    s->regs[off / 4] = val;
    if (off == MDDI_INTEN || off == MDDI_CMD) {
        mddi_update_irq(s);
    }
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

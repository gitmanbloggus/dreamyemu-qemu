/*
 * HTC Dream / T-Mobile G1 (Qualcomm MSM7201A) machine: -M dream.
 *
 * Reuses the MSM device models of -M htcleo (QSD8250, the MSM7201A's
 * successor: same NAND controller, data mover, VIC, GPT/DGT, UART and MDP
 * register families) at the MSM7x01A physical map.  Addresses come from
 * the trout kernel's msm_iomap.h and from the dreamyemu (Rust) bring-up,
 * which proved them live:
 *
 *   SMI        0x00000000  64 MiB  SPL at 0 (MMU off), fb at 0x03800000
 *   SMEM       0x01F00000           (the SPL's reset probe 0x01F6A2A8)
 *   EBI1       0x10000000           kernel at 0x10008000, ramdisk 0x11000000
 *   NAND       0xA0A00000
 *   CLK_CTL    0xA8600000
 *   GPIO1/2    0xA9200000 / 0xA9300000
 *   DMOV       0xA9700000
 *   I2C        0xA9900000
 *   UART1..3   0xA9A00000 / 0xA9B00000 / 0xA9C00000
 *   MDP        0xAA200000
 *   MDDI PMDH  0xAA600000
 *   VIC        0xC0000000
 *   CSR (GPT)  0xC0100000
 *
 * There is no SIRC on the 7x01A: UARTs and GPIO groups go straight to the
 * VIC.  Anything else in the peripheral space reads 0 and is logged under
 * -d unimp, which is how the next device to model gets named.
 *
 * Boot: -bios spl.bin (the 512 KiB HBOOT/SPL) is copied to SMI 0 and the
 * core starts there with the MMU off, as on the phone once the modem has
 * released it.  -drive if=mtd takes the NAND image in msm-nand's codeword
 * layout (tools/dream_nand.py converts a dreamyemu 2048+64 image).
 *
 * This code is licensed under the GNU GPL v2 or later.
 */

#include "qemu/osdep.h"
#include "net/net.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/loader.h"
#include "hw/arm/machines-qom.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "target/arm/cpu.h"
#include "../htcleo/htcleo.h"
#include "../htcleo/msm_regfile.h"
#include "msm_mddi.h"
#include "../htcleo/msm_mdp.h"
#include "dream_input.h"
#include "dream_synaptics.h"
#include "hw/i2c/i2c.h"
#include "../htcleo/msm_modem.h"

#define DREAM_SMI_BASE          0x00000000
#define DREAM_SMI_SIZE          (64 * MiB)
#define DREAM_EBI1_BASE         0x10000000
#define DREAM_EBI1_SIZE         (128 * MiB)

#define DREAM_NAND_PHYS         0xA0A00000
#define DREAM_CLK_CTL_PHYS      0xA8600000
#define DREAM_GPIO1_PHYS        0xA9200000
#define DREAM_GPIO2_PHYS        0xA9300000
#define DREAM_DMOV_PHYS         0xA9700000
#define DREAM_I2C_PHYS          0xA9900000
#define DREAM_UART1_PHYS        0xA9A00000
#define DREAM_UART2_PHYS        0xA9B00000
#define DREAM_UART3_PHYS        0xA9C00000
#define DREAM_MDP_PHYS          0xAA200000
#define DREAM_PMDH_PHYS         0xAA600000
#define DREAM_VIC_PHYS          0xC0000000
#define DREAM_CSR_PHYS          0xC0100000
#define DREAM_SMEM_PHYS         0x01F00000      /* inside SMI */

/* MSM7x01A VIC lines (arch/arm/mach-msm/include/mach/irqs.h) */
#define DREAM_INT_A9_M2A_0          0       /* ..6: modem -> apps */
#define DREAM_INT_GP_TIMER_EXP      7
#define DREAM_INT_DEBUG_TIMER_EXP   8
#define DREAM_INT_UART1             9
#define DREAM_INT_UART2             10
#define DREAM_INT_UART3             11
#define DREAM_INT_MDDI_PRI          16
#define DREAM_INT_MDP               19
#define DREAM_INT_ADM_AARM          21
#define DREAM_INT_ADSP_A11          22
#define DREAM_INT_GPIO_GROUP1       32
#define DREAM_INT_GPIO_GROUP2       33
#define DREAM_INT_PWB_I2C           34
#define DREAM_INT_NAND_WR_ER_DONE   36
#define DREAM_INT_NAND_OP_DONE      37

#define DREAM_GPIO_LCD_VSYNC    97

/* K9K2G08: maker 0xEC, device 0xAA, 2 KiB pages, 128 KiB blocks, x8 */
#define DREAM_NAND_ID           0x1500AAECu
#define DREAM_NAND_BLOCKS       2048

/*
 * The MDDI panel's vsync: GPIO 97 ("vsync on gpio 97 now 1" in the
 * kernel log), a level interrupt with POS = 0, so the panel pulses it low
 * once a frame.  msmfb_pan_display waits on it before each DMA2 kick
 * ("timeout waiting for frame start" without it).  60 Hz, 100 us low.
 */
typedef struct DreamVsync {
    QEMUTimer *timer;
    MsmGpioState *gpio;
    bool low;
} DreamVsync;

static void dream_vsync_tick(void *opaque)
{
    DreamVsync *v = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    v->low = !v->low;
    msm_gpio_set_input(v->gpio, DREAM_GPIO_LCD_VSYNC, !v->low);
    timer_mod(v->timer, now + (v->low ? 100 * SCALE_US :
                               NANOSECONDS_PER_SECOND / 60 - 100 * SCALE_US));
}

/* SMEM as AMSS leaves it when it releases the apps core */
static void dream_modem_reset(void *opaque)
{
    msm_modem_boot(MSM_MODEM(opaque));
}

static char *dream_boot_keys;

static void dream_dma2_to_panel(void *opaque, uint32_t base, uint32_t stride,
                                int w, int h, int x, int y)
{
    msm_mddi_panel_blit(opaque, base, stride, w, h, x, y);
}

static char *dream_get_keys(Object *obj, Error **errp)
{
    return g_strdup(dream_boot_keys ? dream_boot_keys : "");
}

static void dream_set_keys(Object *obj, const char *value, Error **errp)
{
    g_free(dream_boot_keys);
    dream_boot_keys = g_strdup(value);
}

static ARMCPU *dream_cpu;

/*
 * A full reset, as after PS_HOLD drops or PCOM_RESET_CHIP: devices, the
 * SPL image (a ROM blob, copied back), and the core at 0 with the MMU off.
 * The core has to be reset by hand: it hangs off no bus.
 */
static void dream_reset(MachineState *machine, ResetType type)
{
    qemu_devices_reset(type);
    cpu_reset(CPU(dream_cpu));
}

static void dream_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *smi = g_new(MemoryRegion, 1);
    DeviceState *vic, *csr, *modem, *mdp, *mddi, *dev;
    MsmGpioState *gpio;
    I2CBus *i2c;
    DeviceState *ts;
    SysBusDevice *sbd;
    Object *cpuobj;
    ARMCPU *cpu;
    DriveInfo *dinfo;

    if (machine->ram_size != DREAM_EBI1_SIZE) {
        error_report("dream: RAM size must be %u MiB (EBI1)",
                     (unsigned)(DREAM_EBI1_SIZE / MiB));
        exit(1);
    }

    cpuobj = object_new(machine->cpu_type);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    object_unref(cpuobj);
    cpu = ARM_CPU(cpuobj);
    dream_cpu = cpu;

    memory_region_init_ram(smi, NULL, "dream.smi", DREAM_SMI_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, DREAM_SMI_BASE, smi);
    memory_region_add_subregion(sysmem, DREAM_EBI1_BASE, machine->ram);

    /*
     * The SPL hands off to the kernel from its virtual address: at
     * 0x880152A0 it writes SCTLR (MMU off) and the next instruction,
     * `mov pc, r3`, comes out of the ARM1136's prefetch buffer.  QEMU
     * refetches it from *physical* 0x880152A4, which held nothing, and the
     * core slid through zeros (measured: pc 0x887F436C with r1=0x5A0,
     * r2=0x10000100, r3=0x10008000 all correct).  Aliasing the SPL's
     * window (VA 0x88000000 -> SMI 0) there stands in for that prefetch.
     * Not a claim that the MSM7201A decodes anything at 0x88000000.
     */
    {
        MemoryRegion *spl_va = g_new(MemoryRegion, 1);

        memory_region_init_alias(spl_va, NULL, "dream.spl-prefetch", smi, 0,
                                 1 * MiB);
        memory_region_add_subregion(sysmem, 0x88000000, spl_va);
    }

    /* Below every model: unmodeled registers read 0, logged by -d unimp */
    create_unimplemented_device("dream.periph", 0x80000000, 0x80000000);

    vic = qdev_new(TYPE_MSM_VIC);
    qdev_prop_set_bit(vic, "arm11", true);
    sbd = SYS_BUS_DEVICE(vic);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_VIC_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_FIQ));

    csr = dev = qdev_new(TYPE_MSM_CSR);
    qdev_prop_set_uint32(dev, "dgt-div", 0);    /* DGT at 19.2 MHz */
    qdev_prop_set_bit(dev, "dgt-free-running", true);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_CSR_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_GP_TIMER_EXP));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(vic, DREAM_INT_DEBUG_TIMER_EXP));

    /*
     * The modem: msm-modem plays AMSS's side of proc_comm, SMSM and SMD.
     * The 2.6.25 trout kernel uses the same SMEM layout as the HD2's
     * (proc_comm at +0, heap info +0xC0, TOC +0xD0); on the Dream it sits
     * at SMI 0x01F00000, right above AMSS's own SMI image (0x00A00000-
     * 0x01E5C000, see docs/dream-modem.md).  No HD2 SMEM image is loaded:
     * with smd-v1 the model builds the heap the 2.6.25 kernel looks for
     * and speaks its inline-FIFO SMD channels.
     */
    modem = qdev_new(TYPE_MSM_MODEM);
    qdev_prop_set_bit(modem, "amss-hook", false);
    qdev_prop_set_bit(modem, "smd-v1", true);
    msm_csr_set_modem(csr, modem);
    /* packet data (rmnet over QMI) goes to -nic ...,model=msm-modem */
    qemu_configure_nic_device(modem, false, NULL);
    object_property_add_child(OBJECT(machine), "modem", OBJECT(modem));
    qdev_realize_and_unref(modem, NULL, &error_fatal);
    msm_modem_set_smem(MSM_MODEM(modem),
                       (uint8_t *)memory_region_get_ram_ptr(smi) +
                       DREAM_SMEM_PHYS);
    for (int i = 0; i < 7; i++) {
        qdev_connect_gpio_out_named(modem, "m2a", i,
                                    qdev_get_gpio_in(vic, DREAM_INT_A9_M2A_0 + i));
    }
    /* apps -> modem doorbells (CSR +0x400 + 4n): proc_comm is n = 6 */
    for (int i = 0; i < 16; i++) {
        qdev_connect_gpio_out_named(csr, "a2m", i,
                                    qdev_get_gpio_in_named(modem, "a2m", i));
    }
    qdev_connect_gpio_out_named(modem, "m2a", M2A_DSP,
                                qdev_get_gpio_in(vic, DREAM_INT_ADSP_A11));
    qemu_register_reset(dream_modem_reset, modem);

    dev = qdev_new(TYPE_MSM_GPIO);
    qdev_prop_set_bit(dev, "msm7x01a", true);
    qdev_prop_set_int32(dev, "ps-hold", 25);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_GPIO1_PHYS);
    sysbus_mmio_map(sbd, 1, DREAM_GPIO2_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_GPIO_GROUP1));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vic, DREAM_INT_GPIO_GROUP2));
    /*
     * Buttons, and the CPLD whose interrupt is GPIO 17 (active high; left
     * floating high it stormed trout_gpio_irq_handler, 1.69M POS/CLEAR
     * pairs in 25 s).  -M dream,keys=power+camera holds a combo at
     * power-on: HBOOT; power+home: recovery; power+back: fastboot.
     */
    gpio = (MsmGpioState *)dev;
    {
        DreamVsync *v = g_new0(DreamVsync, 1);

        v->gpio = (MsmGpioState *)dev;
        v->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dream_vsync_tick, v);
        timer_mod(v->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / 60);
    }

    /* UART1 is the SPL/kernel console (serial0); UART3 the trout debug port */
    for (int i = 0; i < 3; i++) {
        static const hwaddr base[3] = {
            DREAM_UART1_PHYS, DREAM_UART2_PHYS, DREAM_UART3_PHYS,
        };
        dev = qdev_new(TYPE_MSM_UART);
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, base[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_UART1 + i));
    }

    dev = qdev_new(TYPE_MSM_ADM);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_DMOV_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_ADM_AARM));

    dev = qdev_new(TYPE_MSM_NAND);
    qdev_prop_set_uint32(dev, "flash-id", DREAM_NAND_ID);
    qdev_prop_set_uint32(dev, "blocks", DREAM_NAND_BLOCKS);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(dev, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_NAND_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_NAND_OP_DONE));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(vic, DREAM_INT_NAND_WR_ER_DONE));

    /*
     * I2C: the touchscreen at 0x20; other chips (compass, ...) NACK at once.  Unmapped, every transfer timed out (the
     * synaptics probe alone took ~0.3 s of guest time).
     */
    dev = qdev_new(TYPE_MSM_I2C);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_I2C_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_PWB_I2C));
    i2c = (I2CBus *)qdev_get_child_bus(dev, "i2c");
    /* the Synaptics touchscreen, attention on CPLD GPIO 167 */
    ts = DEVICE(i2c_slave_create_simple(i2c, TYPE_DREAM_SYNAPTICS, 0x20));
    dream_input_init(gpio, dream_boot_keys, ts);

    mdp = dev = qdev_new(TYPE_MSM_MDP);
    qdev_prop_set_bit(dev, "mddi", true);
    qdev_prop_set_uint32(dev, "panel-width", 320);
    qdev_prop_set_uint32(dev, "panel-height", 480);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_MDP_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_MDP));

    mddi = dev = qdev_new(TYPE_MSM_MDDI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_PMDH_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_MDDI_PRI));
    /* DMA2 frames go over MDDI to the panel, which owns the display */
    msm_mdp_set_mddi_sink(MSM_MDP(mdp), dream_dma2_to_panel, mddi);

    /* Clock controller: plain read-back registers until something polls */
    dev = qdev_new(TYPE_MSM_REGFILE);
    qdev_prop_set_string(dev, "name", "dream.clk-ctl");
    qdev_prop_set_uint32(dev, "size", 0x1000);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, DREAM_CLK_CTL_PHYS);

    if (machine->firmware) {
        if (load_image_targphys(machine->firmware, DREAM_SMI_BASE,
                                DREAM_SMI_SIZE, NULL) < 0) {
            error_report("dream: cannot load SPL '%s'", machine->firmware);
            exit(1);
        }
    } else if (!qtest_enabled()) {
        warn_report("dream: no -bios given; the core will start on empty SMI");
    }
}

static void dream_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "HTC Dream / T-Mobile G1 (Qualcomm MSM7201A)";
    mc->init = dream_init;
    mc->reset = dream_reset;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1136");
    mc->default_ram_size = DREAM_EBI1_SIZE;
    mc->default_ram_id = "dream.ebi1";
    mc->block_default_type = IF_MTD;
    mc->ignore_memory_transaction_failures = true;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
    object_class_property_add_str(oc, "keys", dream_get_keys, dream_set_keys);
    object_class_property_set_description(oc, "keys",
        "Buttons held at power-on, e.g. power+camera (HBOOT), power+home "
        "(recovery), power+back (fastboot)");
}

static const TypeInfo dream_machine_types[] = {
    {
        .name = MACHINE_TYPE_NAME("dream"),
        .parent = TYPE_MACHINE,
        .class_init = dream_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(dream_machine_types)

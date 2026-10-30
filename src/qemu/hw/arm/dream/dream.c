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
#include "qemu/units.h"
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

#define DREAM_SMI_BASE          0x00000000
#define DREAM_SMI_SIZE          (64 * MiB)
#define DREAM_EBI1_BASE         0x10000000
#define DREAM_EBI1_SIZE         (128 * MiB)

#define DREAM_NAND_PHYS         0xA0A00000
#define DREAM_CLK_CTL_PHYS      0xA8600000
#define DREAM_GPIO1_PHYS        0xA9200000
#define DREAM_GPIO2_PHYS        0xA9300000
#define DREAM_DMOV_PHYS         0xA9700000
#define DREAM_UART1_PHYS        0xA9A00000
#define DREAM_UART2_PHYS        0xA9B00000
#define DREAM_UART3_PHYS        0xA9C00000
#define DREAM_MDP_PHYS          0xAA200000
#define DREAM_PMDH_PHYS         0xAA600000
#define DREAM_VIC_PHYS          0xC0000000
#define DREAM_CSR_PHYS          0xC0100000

/* MSM7x01A VIC lines (arch/arm/mach-msm/include/mach/irqs.h) */
#define DREAM_INT_GP_TIMER_EXP      7
#define DREAM_INT_DEBUG_TIMER_EXP   8
#define DREAM_INT_UART1             9
#define DREAM_INT_UART2             10
#define DREAM_INT_UART3             11
#define DREAM_INT_MDDI_PRI          16
#define DREAM_INT_MDP               19
#define DREAM_INT_ADM_AARM          21
#define DREAM_INT_GPIO_GROUP1       32
#define DREAM_INT_GPIO_GROUP2       33
#define DREAM_INT_NAND_WR_ER_DONE   36
#define DREAM_INT_NAND_OP_DONE      37

/* K9K2G08: maker 0xEC, device 0xAA, 2 KiB pages, 128 KiB blocks, x8 */
#define DREAM_NAND_ID           0x1500AAECu
#define DREAM_NAND_BLOCKS       2048

static void dream_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *smi = g_new(MemoryRegion, 1);
    DeviceState *vic, *dev;
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

    dev = qdev_new(TYPE_MSM_CSR);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_CSR_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_GP_TIMER_EXP));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(vic, DREAM_INT_DEBUG_TIMER_EXP));

    dev = qdev_new(TYPE_MSM_GPIO);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_GPIO1_PHYS);
    sysbus_mmio_map(sbd, 1, DREAM_GPIO2_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_GPIO_GROUP1));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(vic, DREAM_INT_GPIO_GROUP2));

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

    dev = qdev_new(TYPE_MSM_MDP);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_MDP_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_MDP));

    dev = qdev_new(TYPE_MSM_MDDI);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, DREAM_PMDH_PHYS);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(vic, DREAM_INT_MDDI_PRI));

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
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm1136");
    mc->default_ram_size = DREAM_EBI1_SIZE;
    mc->default_ram_id = "dream.ebi1";
    mc->block_default_type = IF_MTD;
    mc->ignore_memory_transaction_failures = true;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;
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

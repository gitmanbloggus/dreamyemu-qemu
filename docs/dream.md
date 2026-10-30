# HTC Dream (`-M dream`)

The T-Mobile G1 / HTC Dream (Qualcomm MSM7201A, ARM1136) on the same QEMU
tree as `-M htcleo`. The QSD8250 is the MSM7201A's successor, so the Dream
board reuses the HD2's MSM device models (NAND controller, data mover, VIC,
GPT/DGT, UART, GPIO, MDP) at the MSM7x01A physical map. The map and
everything below were first worked out in the Rust emulator, dreamyemu;
this is where that work continues.

Board: `src/qemu/hw/arm/dream/` (`dream.c`, `msm_mddi.c`).

## Run

Third-party blobs stay out of git (`images/` is ignored): the 512 KiB SPL
(`spl.bin`) and a NAND image made by dreamyemu's
`ferramentas/nand_linux.py` (stock recovery from `DREAIMG-RC29.zip` plus a
MISC boot message: `--recovery --misc-update`).

```bash
python3 tools/dream_nand.py ../dreamyemu/tests/spl/nand_recovery.img \
    images/dream_nand_recovery.img       # 2048+64 -> msm-nand codewords
src/qemu/build/qemu-system-arm -M dream -bios spl.bin \
    -drive if=mtd,format=raw,file=images/dream_nand_recovery.img,snapshot=on \
    -display none -serial stdio -d unimp -D unimp.log
```

The kernel has no `console=` on its command line, so its log is read
from `__log_buf` (VA `0xC02C9000` = PA `0x102C9000`, 128 KiB ring):
`pmemsave 0x102C9000 0x20000 klog.bin` in the monitor.

## State (2026-10-30)

- SPL: full run on UART1. MDDI/display init, NAND read of the recovery
  boot image (header, kernel 0x153004 B, ramdisk 0x5A831 B), ATAGs,
  cmdline, handoff (r1 = 0x5A0 trout, r2 = 0x10000100, r3 = 0x10008000).
- Kernel (2.6.25 trout): `Machine: trout`, IRQs, timers, calibrate delay
  (815.92 BogoMIPS), up to `trout_init() revision=170`.
- Wall: `proc_comm: TIMEOUT. modem has probably crashed`. Nothing answers
  the modem's shared-memory protocols yet. `msm_modem.c` speaks them for
  the HD2 (AMSS 15.42, SMEM at 0x00100000); the Dream needs its SMEM at
  0x01F00000 and the 2.6.25 trout proc_comm/SMD/SMSM layout.

## Board-specific fixes, and why

- **VIC `arm11` layout.** The MSM7x01A VIC has INT_MASTEREN at 0x64 and
  CONFIG at 0x68; Scorpion moved them to 0x68/0x6C. With the Scorpion
  layout the kernel's master enable was dropped and no IRQ reached the
  core (kernel parked in calibrate_delay with GPT/DGT lines enabled).
- **NAND `blocks` property.** The Dream's K9K2G08 is 2048 blocks (ID
  `EC AA 00 15`); the HD2 part is 4096.
- **SPL prefetch alias.** The SPL turns the MMU off at VA 0x880152A0 and
  its next instruction, `mov pc, r3`, comes from the ARM1136 prefetch
  buffer. QEMU refetches from physical 0x880152A4, so SMI's first MiB is
  aliased at 0x88000000. This stands in for the prefetch; it is not a
  claim about the MSM7201A's bus.
- **MDDI host.** First cut: commands finish at once, INT/STAT read as an
  idle host with the link up, CORE_VER 0x20. The SPL spun forever on
  MDDI_INT without it. No client and no reverse packets yet.

## NAND image layout

`msm-nand` keeps four 528-byte codewords per page,
`[516 user][10 parity][2]`. `tools/dream_nand.py` puts data + spare[0:16]
in the user bytes (the controller's 516-byte UD_SIZE), spare[16:26] in
codeword 3's parity (where the SPL probes data+2064), and the factory
marker at codeword 3 offset 464 with the displaced user byte at 526. The
SPL reads it correctly (measured above). The kernel's CFG1 marker position
is still unverified.

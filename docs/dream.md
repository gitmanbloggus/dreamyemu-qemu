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

- SPL: full run on UART1 (display init, NAND load of the recovery image,
  ATAGs, handoff with r1 = 0x5A0 trout).
- Kernel (2.6.25 trout, recovery ramdisk): runs to **init**. proc_comm,
  SMSM and SMD answered by `msm-modem` in `smd-v1` mode (RPCCALL, DIAG,
  DS channels open), slow-clock sync done, NAND probed as `1500aaec`
  with all six MTD partitions, USB/adb up, keypad and trackball drivers
  registered, `Freeing init memory`, init parses the recovery init.rc and
  yaffs mounts `cache` (mtdblock4) from the image.
- Open, in order:
  1. **Recovery is on QEMU's display.** MDDI answers GET_CLIENT_CAP
     (Mfr 0xD263, product 0, 320x480), the kernel binds
     `mddi_c_d263_0000` and `msmfb_probe() installing 320 x 480 panel`;
     msm-mdp in `mddi` mode scans the MDP 2.x DMA2 buffer out to a
     320x480 console on each kick (0x10044), and the board pulses the
     panel vsync on GPIO 97 at 60 Hz, so page flips no longer time out.
     `-display sdl` (or `screendump` in the monitor) shows stock recovery's
     error screen. Still open: the panel ID register read
     (`mddi_remote_read(150000) timeout` -> "unknown panel id").
  2. ~~Guest clock slow~~ **fixed.** The trout kernel's `msm_clocks[]`
     (vmlinux 0xC02A4564) has dg_timer at 600000 Hz with shift 5, i.e. the
     DGT counting at 19.2 MHz, and uses it as clocksource without ever
     setting its ENABLE bit. The HD2 CSR model counted the DGT at /4 and
     only while enabled, so the Dream's DGT read 0 forever. With
     `dgt-div=0` and `dgt-free-running=on`, jiffies advance 1000 per 10 s
     of wall time (HZ=100, measured).
  3. `msm_pm_wait_state(80, ...) failed`: the 2.6.25 power-collapse
     handshake on SMSM is not answered (harmless so far, idle falls back).
  4. No I2C slaves (touchscreen at 0x20 NACKs), no SD card (mmc1 timeouts).

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

- **GPIO `msm7x01a` layout.** Five banks (0-106) with the 7x01A register
  offsets instead of the QSD8x50's eight. GPIO 17 (the trout CPLD
  interrupt, active high) is driven low; floating high it stormed
  trout_gpio_irq_handler.
- **Modem `smd-v1`.** The 2.6.25 kernel's SMEM differs from the HD2's in
  the details: SMD channels are one item each with inline 8 KiB FIFOs,
  SMSM is two `{host, state}` pairs (16 bytes), the channel names are
  stored without the `SMD_` prefix, and time sync uses SMSM_TIMEWAIT
  0x400 / TIMEINIT 0x800 (read from the vmlinux at 0xC002B0C0). With no
  SMEM image, the model builds the heap AMSS would leave.
- **MDDI INT latched / write-1-to-clear.** Reporting every INT bit as set
  stormed the kernel ISR, which tracks the link by flipping between
  LINK_ACTIVE and IN_HIBERNATION.

- **MDP `mddi` mode.** The Dream's MDP 2.x has its DMA2 block at 0x10180
  (config, size, IBUF, stride) kicked at 0x10044 with "done" on bit 2,
  and no LCDC; the HD2's MDP 3.1 scans DMA_P while its LCDC runs. The
  mode folds DMA2 onto the DMA_P scanout and refreshes on each kick.
- **Panel vsync on GPIO 97.** Active-low level interrupt; without it every
  msmfb_pan_display waited out "timeout waiting for frame start".

## NAND image layout

`msm-nand` keeps four 528-byte codewords per page,
`[516 user][10 parity][2]`. `tools/dream_nand.py` puts data + spare[0:16]
in the user bytes (the controller's 516-byte UD_SIZE), spare[16:26] in
codeword 3's parity (where the SPL probes data+2064), and the factory
marker at codeword 3 offset 464 with the displaced user byte at 526. The
SPL reads it correctly (measured above). The kernel's CFG1 marker position
is still unverified.

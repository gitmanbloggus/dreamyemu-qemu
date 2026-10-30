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

## Normal boot: Android 1.0

```bash
python3 tools/dream_nand.py --erase-aa --spl spl.bin --nbh DREAIMG-RC29.zip \
    nand_bringup.img images/dream_nand_boot.img
src/qemu/build/qemu-system-arm -M dream -bios spl.bin \
    -drive if=mtd,format=raw,file=images/dream_nand_boot.img -display sdl
```

`--nbh` puts the RUU's boot.img and recovery.img in their own partitions
(Linux mtd offsets are NAND byte offsets: recovery 0x026C0000, boot
0x02BC0000), so the SPL boots boot.img. Measured: system, userdata and
cache mount from NAND, rild opens SMD DS, SurfaceFlinger draws through
msm_fb, and the first-boot wizard ("Welcome to T-Mobile G1") is up about
30 s after power-on; Android then sleeps the panel after its timeout
("android_power: sleep", "deinit sharp panel"). The keyboard is closed
by default (portrait); F11 slides it out (landscape) and back.

Touch: the pointer over the screen is the finger (left button), with
`-display sdl` or QMP `input-send-event` abs events. Trackball: arrow keys
or the mouse wheel (vertical), press with the middle button or F10.
Measured: tapping the android, the tip and "Next" walks the setup wizard
to its account page; trackball Down + press opens the Emergency Dialer.

### Past the setup wizard

RC29's wizard needs a Google account to continue, so an offline image is
stuck at "Welcome to T-Mobile G1". The wizard gates on the Settings
`device_provisioned` flag in `/data`, so flip it offline and the next boot
goes to the home screen. `/data` is empty in a fresh image, so the
settings database only exists after one boot:

```bash
cp images/dream_nand_boot.img images/dream_data.img          # a working copy
src/qemu/build/qemu-system-arm -M dream -bios spl.bin \
    -drive if=mtd,format=raw,file=images/dream_data.img       # no snapshot
#   ... let it reach the wizard (~30 s), then power off ...
python3 tools/yaffs.py images/dream_data.img set-provisioned  # device_provisioned=1
src/qemu/build/qemu-system-arm -M dream -bios spl.bin \
    -drive if=mtd,format=raw,file=images/dream_data.img,snapshot=on -display sdl
```

`tools/yaffs.py` reads and edits the YAFFS2 filesystems in the image
(`--part system|cache|userdata`): `list`, `extract PATH OUT`, and
`replace PATH IN` (same size or smaller, rewriting data in place and
keeping each chunk's spare). `set-provisioned` extracts `settings.db`,
sets `device_provisioned` (and `install_non_market_apps`) with the `sqlite3`
module, and writes it back. It does not recompute data-area ECC — the
msm-nand model treats parity as zero and never checks it, so edits read
back verbatim in the emulator but would fail a real controller.

## Buttons, HBOOT and recovery keys

`-M dream,keys=power+back` holds a combo from power-on (3 s): the SPL
returns boot mode 2 and HBOOT shows the Dream's tricolour "rainbow"
screen (`DREA100 DVT2-1 32A`, `HBOOT-0.95.0000`, `Serial0`) and its
`UART0:Cmd>` prompt on serial0. On the host keyboard: Esc = Back,
Home = Home, F1 = Menu, F2 = Send, F12/End = Power (red key), F9 = Camera,
F8 = Focus, F10 = trackball press, PageUp/PageDown = volume, letters and
Alt/Shift as on the QWERTY keypad. In recovery Alt+L toggles the menu and
Home+Back reboots: the trout reset hook drops PS_HOLD (GPIO 25), which
resets the board, and the SPL starts again.

How the SPL decides (read from the SPL): MISC command first, then the
reset reason at 0xAA0FC080 (0x776655xx, written by `reboot bootloader`
etc.), then board op +0x30 of the table at 0x8806FB90, which scans the
keypad itself. Power (GPIO 20) + Back (column 35 x row 42) gives mode 2.

Use an image converted with `--erase-aa --spl spl.bin`: HBOOT reads its
own header back from APPSBL (block 0x120) for its title line; with the
Rust images' 0xAA fill there the line overflowed a stack buffer.

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

- **Keypad matrix.** Columns 35,34,33,32,31,23,30 (+109/108/78) are
  driven low one at a time, rows 42..36 read with pull-ups, a 0 meaning
  pressed; keymap index = column x 7 + row. Both the SPL and the kernel
  scan this way (measured: kernel OE/OUT on the columns, level-low IRQs
  on the rows), so a pressed key is modeled as a switch. Power is GPIO 20
  (active low); Camera/Focus/trackball are CPLD +0x08 bits 6/5/0.
- **MDDI panel owns the display.** The SPL draws with MDDI Video Stream
  packets (RGB 6-6-6, one row each), the kernel through MDP DMA2; both
  write the client's 320x480 frame memory, shown on one console.
- **MDDI client registers.** Register Access packets (type 146) in the
  primary list: writes are kept, reads answered with a reverse packet.

- **PS_HOLD and reset.** GPIO 25's latch starts high; when the kernel's
  reset hook (vmlinux 0xC002DAB8, gpio_set_value(25, 0)) drops it, the
  machine resets, CPU included (it hangs off no bus, so `mc->reset` resets
  it by hand).
- **Panel ID.** The kernel takes (client reg 0x150000 >> 4) & 3 and knows
  0 and 1; with register reads answered it reads 0 and binds "sharp".
- **MDDI list completion** is raised 20 us after PRI_PTR: raised at once,
  the kernel's ISR consumed PRI_LINK_LIST_DONE before the writer enabled
  it, and register writes waited out their timeouts.
- **Partial updates.** msm_fb sends dirty rectangles: DMA2 IBUF at the
  rectangle, its size, and the panel position in OUT_XY (0x10194).
- **Keyboard slide** is SW_LID on GPIO 162 (CPLD bit 2, active low);
  low = closed = portrait.

- **Trackball** ("trout-nav"): two gpio_event_axis inputs, X on GPIO
  18/19 and Y on GPIO 94/90, 2-bit Gray code with edge interrupts; one
  detent = four transitions, emitted 2 ms apart. It is GPIO quadrature,
  not PS/2 (the "mice: PS/2 mouse device" boot line is Linux's generic
  mousedev).
- **Touchscreen**: Synaptics RMI on I2C 0x20 (`dream-synaptics`), the
  trout synaptics_i2c_rmi protocol read from the vmlinux: global
  0xE0-0xFF, max X/Y in page 0x10, a 15-byte report in page 4 whose
  status byte must be 0x41 (0x40 alone is accepted but dropped), and
  attention on CPLD GPIO 167 (IRQ 193). Version 1.0 selects the platform
  data's entry 1 (inactive edges 0x15DA/0x4C7 of 0x10000, FLIP_Y); screen
  pixels are mapped through that transform.

## NAND image layout

`msm-nand` keeps four 528-byte codewords per page,
`[516 user][10 parity][2]`. `tools/dream_nand.py` puts data + spare[0:16]
in the user bytes (the controller's 516-byte UD_SIZE), spare[16:26] in
codeword 3's parity (where the SPL probes data+2064), and the factory
marker at codeword 3 offset 464 with the displaced user byte at 526. The
SPL reads it correctly (measured above). The kernel's CFG1 marker position
is still unverified.

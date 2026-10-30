# Dream modem (ARM9 / AMSS) — what the RUU NBH carries

Source: `DREAIMG-RC29.zip` → `DREAIMG.nbh` (91 564 352 B). All offsets
below were read from that file on 2026-10-30; no bytes are in git.

## NBH container

Header at file 0x100: `HTCIMAGE` (UTF-32LE), model `DREA10000`, then
three 32-entry u32 tables: types @0x140, offsets @0x1C0, lengths @0x240.
File offset = table offset + 0x100.

| type  | file off   | length     | content |
|-------|-----------:|-----------:|---------|
| 0x200 | 0x00000300 | 0x00080000 | APPSBL = SPL/HBOOT 0.95.0000 |
| 0xB05 | 0x00080300 | 0x001AF000 | recovery.img (`ANDROID!`) |
| 0xB04 | 0x0022F300 | 0x00177000 | boot.img (same kernel as recovery) |
| 0x600 | 0x003A6300 | 0x0004B000 | splash, 320x480 RGB565 |
| 0xB02 | 0x003F1300 | 0x03E80E00 | system (YAFFS2, 2048+64 records) |
| 0xB06 | 0x04272100 | 0x00000840 | userdata (one empty record) |
| 0x301 | 0x04272940 | 0x014E0000 | **radio** (2048-byte pages, no spare) |

## The radio item

Radio version **1.22.12.29** (Q7201A6, T-Mobile US). Layout of the item
(not a literal NAND dump: the flasher places the pieces):

- 0x000000: QCSBL/boot header, codeword 0x83838383, magic 0x73D71034:
  0x1015C bytes loaded to **0x8000E000** (ARM9 internal RAM).
- 0x040020: **MIBIB** (0xFE569FAC/0xCD7F127A, version 3) and at 0x040820
  the **flash partition table** (0x55EE73AA/0xE35EBDDB, v3, 16 entries).
- 0x280020: the **AMSS ELF** (entry 0x00A00000, 18 PT_LOAD segments)
  running to the end of the item.

### NAND partition table (blocks of 128 KiB)

| name     | block | blocks | NAND addr  | note |
|----------|------:|-------:|-----------:|------|
| MIBIB    | 0x000 | 0x00A | 0x00000000 | |
| QCSBL    | 0x00A | 0x004 | 0x00140000 | |
| OEMSBL1  | 0x00E | 0x006 | 0x001C0000 | |
| OEMSBL2  | 0x014 | 0x006 | 0x00280000 | |
| HTC      | 0x01A | 0x010 | 0x00340000 | |
| AMSS     | 0x02A | 0x0C0 | 0x00540000 | |
| EFS2     | 0x0EA | 0x02C | 0x01D40000 | modem filesystem |
| FOTA     | 0x116 | 0x008 | 0x022C0000 | |
| RESERVED | 0x11E | 0x002 | 0x023C0000 | |
| APPSBL   | 0x120 | 0x006 | 0x02400000 | the SPL |
| MISC_MFG | 0x126 | 0x002 | 0x024C0000 | Linux `misc` (mtd0) |
| WLAN     | 0x128 | 0x002 | 0x02500000 | |
| SPLASH1  | 0x12A | 0x009 | 0x02540000 | |
| SPLASH2  | 0x133 | 0x00E | 0x02660000 | |
| APPS     | 0x141 | 0x278 | 0x02820000 | Linux recovery/boot/system/cache/userdata |
| EXT_ROM  | 0x3B9 | 0x047 | 0x07720000 | |

This agrees with what the Rust bring-up found the hard way: MISC at
block 0x126 and Linux's mtd offsets (`misc` 0x024C0000, `recovery`
0x026C0000, ...).

### Where AMSS lives in memory (ELF program headers)

| paddr range                | size      | what |
|----------------------------|----------:|------|
| 0x00A00000–0x00A1FDD8      | 0x1FDD8   | vectors/boot (vaddr 0xF0000000) |
| 0x00A20000–0x00AF17000     | ~0.9 MiB  | small code/data pieces (vaddr 0xB0xxxxxx) |
| 0x00B07000–0x0140A000      | 9 MiB+    | main code |
| 0x0140A000–0x01E5BA60      | 0xA51A60  | data + BSS |
| 0x16E00000–0x1763B000      | 0x83B000  | code in EBI1 |
| 0x1763B000–0x17BB2000      | 0x577000  | data/BSS in EBI1 |
| 0x17BB2000–0x17C04000      | ~0.3 MiB  | more data, 0x17C03000 page |

So the ARM9 owns **SMI 0x00A00000–~0x01E5C000** (right below SMEM at
0x01F00000) and **EBI1 0x16E00000–~0x17C04000** (above the 101 MiB the
kernel takes from 0x10000000). The SPL at SMI 0, the fb at SMI 0x03800000
and the kernel all stay clear of it, which is consistent with the modem
running alongside Linux out of the same physical RAM.

Boot chain (strings: "QCT_BOOT V622512", "OEMSBL Build Date", "Then
Boot CE, mARM goto AMSS"): ARM9 PBL → QCSBL (internal RAM) → OEMSBL →
loads AMSS and the APPSBL, releases the ARM11 ("CE") and jumps to AMSS.

## Options for -M dream

1. **HLE** (what `msm_modem.c` does for the HD2): answer proc_comm,
   SMSM and SMD from a device model, with SMEM at 0x01F00000 laid out the
   way the 2.6.25 trout kernel expects. Smallest step to get past
   `proc_comm: TIMEOUT`.
2. **LLE** (what `-M htcleo,baseband=lle` does): load these ELF segments
   into SMI/EBI1 and run AMSS on an ARM926 in a second CPU cluster from
   0x00A00000. AMSS then builds SMEM itself. Needs the modem-side
   peripherals (its VIC, timers, PMIC over SSBI...) that the HD2 work
   already models for its own AMSS. Most faithful, much more work.

Recommendation: HLE first, to get Linux and recovery running and to find
which proc_comm commands the trout kernel needs; LLE later, reusing the
HD2's second-cluster plumbing.

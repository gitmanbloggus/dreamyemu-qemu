#!/usr/bin/env python3
"""Resize the Dream's system/cache/userdata the way Firerat's CustomMTD
does (xda t=717874): append an `mtdparts=msm_nand:...` table to a boot
image's kernel command line, so the kernel's cmdlinepart parser overrides
the SPL's partitions.  CustomMTD does this on the phone (patchbootimg.sh,
run from FR-recovery-*/FR-boot-*_S.zip, sizes from /sdcard/mtdpartmap.txt
"mtd SYSTEM_MB CACHE_MB"); those zips are not archived, so this is the
same edit done offline.  Patch recovery first, wipe, flash the ROM, then
patch the ROM's boot.img (both kernels must agree on the layout).

misc/recovery/boot keep the stock SPL's NAND offsets; system starts at
0x02E40000, cache follows it, userdata takes the rest of the 256 MiB
chip ("-").  The order keeps the stock mtdblock numbers.

Usage:
    python3 tools/dream_custommtd.py SYSTEM_MB CACHE_MB IN.img OUT.img
e.g. 90 2 (the thread's example): system 90, cache 2, userdata 117.75 MiB.
"""

import sys

CMDLINE_OFF, CMDLINE_LEN = 64, 512
SYSTEM_OFF = 0x02E40000
CHIP = 0x10000000


def mtdparts(system_mb, cache_mb):
    system = system_mb * 0x100000
    cache = cache_mb * 0x100000
    cache_off = SYSTEM_OFF + system
    data_off = cache_off + cache
    if data_off >= CHIP:
        raise SystemExit("system + cache leave no room for userdata")
    return ("mtdparts=msm_nand:256k@0x024C0000(misc),5M@0x026C0000(recovery),"
            f"2560k@0x02BC0000(boot),{system_mb}M@0x{SYSTEM_OFF:08X}(system),"
            f"{cache_mb}M@0x{cache_off:08X}(cache),-@0x{data_off:08X}(userdata)")


def main(argv):
    if len(argv) != 5:
        print(__doc__.split("Usage:")[1].strip())
        return 2
    img = bytearray(open(argv[3], "rb").read())
    if img[:8] != b"ANDROID!":
        raise SystemExit(f"{argv[3]}: not an Android boot image")
    raw = bytes(img[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN]).split(b"\0")[0]
    words = [w for w in raw.decode().split() if not w.startswith("mtdparts=")]
    words.append(mtdparts(int(argv[1]), int(argv[2])))
    new = " ".join(words).encode()
    if len(new) >= CMDLINE_LEN:
        raise SystemExit(f"command line too long ({len(new)} B)")
    img[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN] = new.ljust(CMDLINE_LEN, b"\0")
    open(argv[4], "wb").write(img)
    print(new.decode())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

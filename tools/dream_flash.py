#!/usr/bin/env python3
"""Write an image into a partition of a -M dream NAND image, as fastboot
flash would: the partition is erased (0xFF) and the image programmed from
its first page, spare left erased.  Works on the msm-nand codeword images
tools/dream_nand.py writes.

Partitions (NAND byte offsets, the radio's table, docs/dream-modem.md):
    misc      0x024C0000  256 KiB
    recovery  0x026C0000  5 MiB
    boot      0x02BC0000  2.5 MiB

usage: dream_flash.py NAND.img recovery|boot|misc FILE
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dream_nand import converte_pagina, PAGE, SPARE  # noqa: E402

PARTS = {
    "misc": (0x024C0000, 0x00040000),
    "recovery": (0x026C0000, 0x00500000),
    "boot": (0x02BC0000, 0x00280000),
}
REC = PAGE + SPARE


def main(argv):
    if len(argv) != 4 or argv[2] not in PARTS:
        print(__doc__.split("usage:")[1].strip())
        return 2
    off, size = PARTS[argv[2]]
    data = open(argv[3], "rb").read()
    if len(data) > size:
        raise SystemExit(f"{argv[3]} ({len(data)} B) does not fit in "
                         f"{argv[2]} ({size} B)")
    erased = converte_pagina(b"\xff" * REC)
    with open(argv[1], "r+b") as f:
        for n in range(size // PAGE):
            chunk = data[n * PAGE:(n + 1) * PAGE]
            f.seek((off // PAGE + n) * REC)
            f.write(converte_pagina(chunk.ljust(PAGE, b"\xff") +
                                    b"\xff" * SPARE) if chunk else erased)
    print(f"flashed {argv[3]} ({len(data)} B) to {argv[2]} in {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

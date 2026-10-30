#!/usr/bin/env python3
"""Convert a dreamyemu NAND image (2048 data + 64 spare per page, linear)
into msm-nand's codeword layout for -M dream.

msm-nand keeps each page as four 528-byte codewords, the way an ECC-mode
read presents them: [516 user bytes][10 parity][2 bad-block bytes]. The
MSM controller hands the host 516 user bytes per codeword (UD_SIZE=516 in
the trout kernel's msm_nand.c), so a page's linear view is
data[0:2048] + spare[0:16] = 2064 = 4 x 516; that is also the view the
dreamyemu NAND model gave the guest (YAFFS tags at spare+4..16 resolve
there).

The rest of the spare goes where its readers expect it:
  * spare[16:26] -> parity of codeword 3. The dreamyemu SPL probe reads
    "buffer byte at data+2064" (ferramentas/cria_nand.py), which is the
    first byte past codeword 3's user data.
  * Factory bad-block marker: chip byte 2048 sits at offset 464 of the
    last codeword (2048 - 3*528). With ECC on and the marker in the user
    area, msm-nand reads the marker from raw[464] and returns the user byte
    displaced to raw[526]. So raw[464] = marker (spare[0]: 0xFF = good,
    cria_nand.py's convention) and raw[526] = user byte 464.

Hypotheses, not measured: the SPL's own read path and the kernel's CFG1
(BAD_BLOCK_BYTE_NUM = 465, marker in user area) -- check them against
`-global msm-nand.trace=on` on the first run.

Usage:
    python3 tools/dream_nand.py IN.img OUT.img
"""

import sys

PAGE, SPARE = 2048, 64
REC = PAGE + SPARE
CW_USER, CW_RAW = 516, 528
BB_POS = PAGE - 3 * CW_RAW          # 464


def converte_pagina(rec):
    dados, spare = rec[:PAGE], rec[PAGE:]
    usuario = dados + spare[:16]
    saida = bytearray()
    for cw in range(4):
        raw = bytearray(b"\xff" * CW_RAW)
        raw[:CW_USER] = usuario[cw * CW_USER:(cw + 1) * CW_USER]
        if cw == 3:
            raw[CW_USER:CW_USER + 10] = spare[16:26]
            raw[526] = raw[BB_POS]
            raw[BB_POS] = spare[0]
        saida += raw
    return bytes(saida)


def main(argv):
    if len(argv) != 3:
        print(__doc__.split("Usage:")[1].strip())
        return 2
    with open(argv[1], "rb") as fe, open(argv[2], "wb") as fs:
        n = 0
        while True:
            rec = fe.read(REC)
            if not rec:
                break
            assert len(rec) == REC, f"truncated page {n}"
            fs.write(converte_pagina(rec))
            n += 1
    print(f"{argv[2]}: {n} pages ({n // 64} blocks)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

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

--erase-aa: pages whose data is entirely 0xAA become erased 0xFF pages
(the Rust images leave the spare 0xFF on those). dreamyemu's cria_nand.py fills unwritten NAND with 0xAA;
real erased flash reads 0xFF, and HBOOT reads config from blank areas:
with 0xAA it built pointers like 0xFEAAAABC and crashed (measured).

--spl FILE: write the SPL/HBOOT image into APPSBL (block 0x120, per the
radio's partition table, docs/dream-modem.md), as on the phone. HBOOT
reads its own header back from there for the version in its title line;
with 0xAA there the line overflowed a stack buffer ("HBOOT-" + 12 bytes
of 0xAA, saved LR = "HVT\0") and HBOOT crashed after "ARM11 Boot Mode:2".

--nbh DREAIMG.nbh: write the RUU's boot.img and recovery.img into their
own partitions (Linux mtd offsets are NAND byte offsets: recovery
0x026C0000, boot 0x02BC0000), as the phone has them, so the SPL boots
boot.img normally and recovery only when asked. NBH item table: types at
0x140, offsets (+0x100) at 0x1C0, lengths at 0x240 (docs/dream-modem.md).

Usage:
    python3 tools/dream_nand.py [--erase-aa] [--spl SPL.bin] [--nbh RUU.nbh]
                                [--recovery REC.img] [--boot BOOT.img]
                                IN.img OUT.img
"""

import sys

PAGE, SPARE = 2048, 64
REC = PAGE + SPARE
CW_USER, CW_RAW = 516, 528
BB_POS = PAGE - 3 * CW_RAW          # 464
APPSBL_BLOCK = 0x120
# --recovery/--boot IMG: Linux mtd offset, size (trout: recovery 5 MiB,
# boot 2.5 MiB). Applied after --nbh, so they override its images; the
# rest of the partition is erased (0xFF), as fastboot flash leaves it.
PARTS = {"--recovery": (0x026C0000, 0x500000),
         "--boot": (0x02BC0000, 0x280000)}


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


def nbh_read(path):
    if path.endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(path) as z:
            name = next(i.filename for i in z.infolist()
                        if i.filename.lower().endswith(".nbh"))
            return z.read(name)
    with open(path, "rb") as f:
        return f.read()


def nbh_item(nbh, want):
    import struct
    types = struct.unpack_from("<32I", nbh, 0x140)
    offs = struct.unpack_from("<32I", nbh, 0x1C0)
    lens = struct.unpack_from("<32I", nbh, 0x240)
    for ty, off, ln in zip(types, offs, lens):
        if ty == want:
            return nbh[off + 0x100:off + 0x100 + ln]
    raise SystemExit(f"NBH item 0x{want:x} not found")


def main(argv):
    erase_aa = "--erase-aa" in argv
    argv = [a for a in argv if a != "--erase-aa"]
    spl = None
    if "--spl" in argv:
        i = argv.index("--spl")
        spl = open(argv[i + 1], "rb").read()
        del argv[i:i + 2]
    spl_first = APPSBL_BLOCK * 64
    parts = {}          # first page -> image bytes
    if "--nbh" in argv:
        i = argv.index("--nbh")
        nbh = nbh_read(argv[i + 1])
        del argv[i:i + 2]
        for ty, off in ((0xB05, 0x026C0000), (0xB04, 0x02BC0000)):
            parts[off // PAGE] = nbh_item(nbh, ty)
    for flag, (off, size) in PARTS.items():
        if flag in argv:
            i = argv.index(flag)
            img = open(argv[i + 1], "rb").read()
            del argv[i:i + 2]
            if len(img) > size:
                raise SystemExit(f"{flag}: {len(img)} B > partition {size} B")
            parts[off // PAGE] = img.ljust(size, b"\xff")
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
            if erase_aa and rec[:PAGE] == b"\xaa" * PAGE:
                rec = b"\xff" * REC
            for first, img in parts.items():
                if first <= n < first + (len(img) + PAGE - 1) // PAGE:
                    chunk = img[(n - first) * PAGE:(n - first + 1) * PAGE]
                    rec = chunk.ljust(PAGE, b"\xff") + b"\xff" * SPARE
            if spl is not None and spl_first <= n < spl_first + 6 * 64:
                chunk = spl[(n - spl_first) * PAGE:(n - spl_first + 1) * PAGE]
                if chunk:
                    rec = chunk.ljust(PAGE, b"\xff") + b"\xff" * SPARE
                else:
                    rec = b"\xff" * REC
            fs.write(converte_pagina(rec))
            n += 1
    print(f"{argv[2]}: {n} pages ({n // 64} blocks)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

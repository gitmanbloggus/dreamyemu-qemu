#!/usr/bin/env python3
"""Read and edit the YAFFS2 filesystems inside a dreamyemu NAND image
(2048-byte data + 64-byte spare per page, the format dream_nand.py reads).

Each page is one YAFFS2 chunk: data[0:2048], and the spare's first 16 bytes
are the packed tags {u32 sequenceNumber, objectId, chunkId, byteCount}
(validated against nand_bringup.img; the same fields dreamyemu's
src/msm7201a_yaffs.rs reads).  chunkId 0 is an object header: data[0]=type
(1 file, 3 dir, 2 symlink), data[4]=parent objId, data[10]=name, data[292]=
file size.  Data chunks are numbered from 1, 2048 bytes each, the last one
short (its tag byteCount).

Each YAFFS mount is one NAND partition and has its own objId space (root=1),
so every command works on one partition (--part name, or off:len in hex).
Partition map from the radio's table (docs/dream-modem.md), as NAND byte
offsets == offsets in this flat image:

    system    0x02e40000 .. 0x071c0000
    cache     0x071c0000 .. 0x0b540000
    userdata  0x0b540000 .. 0x10000000   (Linux /data)

`replace` keeps every chunk's spare and only rewrites data bytes, so it is
limited to a file of the same size or smaller (smaller shrinks the header's
size field; the trailing chunks fall outside the file and the kernel skips
them).  Data-area ECC is NOT recomputed: the msm-nand model treats parity
as zero and never checks it (hw/arm/htcleo/msm_nand.c), so edited data reads
back as written in the emulator.  It would fail a real controller's ECC.

`set-provisioned` is the reason this exists: Android 1.0 (RC29) sits on the
"Welcome to T-Mobile G1" wizard until a Google account is added, with no way
forward offline.  The wizard gates on Settings DEVICE_PROVISIONED, stored in
/data/data/com.android.providers.settings/databases/settings.db.  This flips
it to 1 so the next boot goes straight to the home screen.  /data is empty in
a fresh image, so the database only exists after one boot: boot the image
once (write to a working copy, no snapshot, so /data persists), let it reach
the wizard, power off, then run this on that copy.

Usage:
    python3 tools/yaffs.py IMG --part userdata list
    python3 tools/yaffs.py IMG --part userdata extract /path/to/file OUT
    python3 tools/yaffs.py IMG --part userdata replace /path/to/file IN
    python3 tools/yaffs.py IMG set-provisioned
"""

import sqlite3
import struct
import sys
import tempfile

SETTINGS_DB = ("/data/data/com.android.providers.settings/"
               "databases/settings.db")

PAGE = 2048
SPARE = 64
REC = PAGE + SPARE

PARTS = {
    "system": (0x02E40000, 0x071C0000),
    "cache": (0x071C0000, 0x0B540000),
    "userdata": (0x0B540000, 0x10000000),
    "data": (0x0B540000, 0x10000000),
}

# YAFFS reserved object ids (a file whose parent chain hits these is not live)
ROOT, LOSTFOUND, UNLINKED, DELETED = 1, 2, 3, 4


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Yaffs:
    """One partition's live object tree, newest chunk per (objId, chunkId)."""

    def __init__(self, img, first_rec, last_rec):
        self.img = img
        self.first = first_rec
        self.last = last_rec
        self.headers = {}      # objId -> (seq, rec)
        self.chunks = {}       # objId -> {chunkId: (seq, rec)}
        for rec in range(first_rec, last_rec):
            sp = rec * REC + PAGE
            if sp + 16 > len(img):
                break
            seq, obj, chunk, _ = struct.unpack_from("<4I", img, sp)
            if obj in (0, 0xFFFFFFFF) or seq in (0, 0xFFFFFFFF):
                continue
            if chunk == 0:
                old = self.headers.get(obj)
                if old is None or seq >= old[0]:
                    self.headers[obj] = (seq, rec)
            else:
                d = self.chunks.setdefault(obj, {})
                old = d.get(chunk)
                if old is None or seq >= old[0]:
                    d[chunk] = (seq, rec)

    def _hdr(self, obj):
        seq, rec = self.headers[obj]
        b = rec * REC
        return {
            "type": self.img[b],
            "parent": u32(self.img, b + 4),
            "name": self.img[b + 10:b + 10 + 256].split(b"\0")[0].decode(
                "latin1"),
            "size": u32(self.img, b + 292),
            "rec": rec,
            "seq": seq,
        }

    def path_of(self, obj):
        parts = []
        seen = set()
        while obj not in (ROOT, 0) and obj not in seen:
            seen.add(obj)
            if obj in (UNLINKED, DELETED, LOSTFOUND):
                return None     # not in the live tree
            h = self.headers.get(obj)
            if h is None:
                return None
            info = self._hdr(obj)
            parts.append(info["name"])
            obj = info["parent"]
        if obj != ROOT:
            return None
        return "/" + "/".join(reversed(parts))

    def tree(self):
        out = {}
        for obj in self.headers:
            p = self.path_of(obj)
            if p is not None:
                out[p] = (obj, self._hdr(obj))
        return out

    def find(self, path):
        path = "/" + path.strip("/")
        for p, (obj, info) in self.tree().items():
            if p == path:
                return obj, info
        return None, None

    def read_file(self, obj):
        info = self._hdr(obj)
        if info["type"] != 1:
            raise SystemExit(f"obj {obj} is not a regular file")
        size = info["size"]
        data = bytearray()
        chunks = self.chunks.get(obj, {})
        cid = 1
        while len(data) < size:
            if cid not in chunks:
                raise SystemExit(f"missing chunk {cid} of obj {obj}")
            rec = chunks[cid][1]
            take = min(PAGE, size - len(data))
            data += self.img[rec * REC:rec * REC + take]
            cid += 1
        return bytes(data)

    def replace_file(self, obj, new):
        """Overwrite a file's data in place; same size or smaller only."""
        info = self._hdr(obj)
        if info["type"] != 1:
            raise SystemExit(f"obj {obj} is not a regular file")
        old_size = info["size"]
        if len(new) > old_size:
            raise SystemExit(
                f"new file ({len(new)} B) is larger than the old one "
                f"({old_size} B); this tool only replaces in place")
        chunks = self.chunks.get(obj, {})
        need = (len(new) + PAGE - 1) // PAGE
        for cid in range(1, need + 1):
            if cid not in chunks:
                raise SystemExit(f"missing chunk {cid} of obj {obj}")
            rec = chunks[cid][1]
            off = (cid - 1) * PAGE
            piece = new[off:off + PAGE]
            self.img[rec * REC:rec * REC + len(piece)] = piece
            # keep the tag's byteCount in step with the (possibly short) chunk
            struct.pack_into("<I", self.img, rec * REC + PAGE + 12, len(piece))
        if len(new) != old_size:
            # header size field; the header's own chunk/spare are untouched
            hb = info["rec"] * REC
            struct.pack_into("<I", self.img, hb + 292, len(new))


CW_USER, CW_RAW = 516, 528
BB_POS = PAGE - 3 * CW_RAW          # 464, see tools/dream_nand.py


def cw_to_linear(raw):
    """One msm-nand codeword page (4 x 528) -> data + spare, linear.

    Inverse of dream_nand.py's converte_pagina: the 516-byte user areas
    joined are data[0:2048] + spare[0:16], with user byte 3*516+464 moved
    to raw[526] to make room for the bad-block marker."""
    user = bytearray()
    for cw in range(4):
        part = bytearray(raw[cw * CW_RAW:cw * CW_RAW + CW_USER])
        if cw == 3:
            part[BB_POS] = raw[3 * CW_RAW + 526]
        user += part
    spare = bytes(user[PAGE:]) + bytes(raw[3 * CW_RAW + CW_USER:
                                           3 * CW_RAW + CW_USER + 10])
    return bytes(user[:PAGE]) + spare.ljust(SPARE, b"\xff")


def linear_to_cw(raw, rec):
    """Write a linear page's data + spare[0:16] back into codeword page raw,
    leaving the marker and the rest of the raw bytes as they were."""
    user = rec[:PAGE + 16]
    out = bytearray(raw)
    for cw in range(4):
        piece = bytearray(user[cw * CW_USER:(cw + 1) * CW_USER])
        if cw == 3:
            out[3 * CW_RAW + 526] = piece[BB_POS]
            piece[BB_POS] = raw[3 * CW_RAW + BB_POS]
        out[cw * CW_RAW:cw * CW_RAW + CW_USER] = piece
    return bytes(out)


def plausible_tags(img, start, end, conv):
    """How many of up to 256 used pages in [start, end) carry sane tags."""
    good = seen = 0
    for rec in range(start // PAGE, end // PAGE):
        page = img[rec * REC:(rec + 1) * REC]
        if len(page) < REC:
            break
        if page == b"\xff" * REC:
            continue
        lin = conv(page) if conv else page
        seq, obj, chunk, n = struct.unpack_from("<4I", lin, PAGE)
        seen += 1
        good += (0x1000 <= seq < 0x1000000 and 0 < obj < 0x40000
                 and chunk < 0x40000 and (n <= PAGE or n == 0xFFFF))
        if seen == 256:
            break
    return good


def is_codeword(img):
    """True for a dream_nand.py (msm-nand) image, False for a linear one."""
    for start, end in (PARTS["system"], PARTS["userdata"]):
        lin = plausible_tags(img, start, end, None)
        cw = plausible_tags(img, start, end, cw_to_linear)
        if lin or cw:
            return cw > lin
    return False


def load(path):
    """The image as linear pages, and a function turning it back into the
    on-disk layout (msm-nand codewords are converted both ways)."""
    raw = open(path, "rb").read()
    if not is_codeword(raw):
        return bytearray(raw), bytes
    lin = bytearray()
    for rec in range(len(raw) // REC):
        page = raw[rec * REC:(rec + 1) * REC]
        lin += page if page == b"\xff" * REC else cw_to_linear(page)

    def save(img):
        out = bytearray(raw)
        for rec in range(len(raw) // REC):
            a, b = rec * REC, (rec + 1) * REC
            page = cw_to_linear(raw[a:b]) if raw[a:b] != b"\xff" * REC \
                else raw[a:b]
            if img[a:b] != page:
                out[a:b] = linear_to_cw(raw[a:b], img[a:b])
        return bytes(out)
    return lin, save


def open_part(path, part):
    img, save = load(path)
    if ":" in part:
        off, length = (int(x, 0) for x in part.split(":"))
        start, end = off, off + length
    elif part in PARTS:
        start, end = PARTS[part]
    else:
        raise SystemExit(f"unknown partition {part!r}; use "
                         f"{'/'.join(sorted(PARTS))} or off:len")
    return img, save, Yaffs(img, start // PAGE, end // PAGE)


def set_provisioned_db(path):
    """In a settings.db on disk, set DEVICE_PROVISIONED = 1, same file size.

    Early Android keeps settings in the `system` table (name, value); the
    `secure` table arrives with 1.5.  Set it wherever such a table exists.
    The value is text "1" replacing "0" (or a short new row when RC29 has
    not written one yet), so the file size does not change -- which is what
    lets replace() put it back in place."""
    before = open(path, "rb").read()
    con = sqlite3.connect(path)
    try:
        cur = con.cursor()
        tables = [r[0] for r in cur.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")]
        touched = []
        for tbl in ("system", "secure", "global"):
            if tbl not in tables:
                continue
            cols = [c[1] for c in cur.execute(f"PRAGMA table_info({tbl})")]
            if "name" not in cols or "value" not in cols:
                continue
            for key in ("device_provisioned", "install_non_market_apps"):
                n = cur.execute(
                    f"UPDATE {tbl} SET value='1' WHERE name=?", (key,)
                ).rowcount
                if not n and tbl == "system":
                    # RC29 only writes the row when the wizard finishes;
                    # a short row fits in the table's existing page
                    cur.execute(f"INSERT INTO {tbl} (name, value) "
                                "VALUES (?, '1')", (key,))
                    n = 1
                if n:
                    touched.append(f"{tbl}.{key}")
        con.commit()
    finally:
        con.close()
    after = open(path, "rb").read()
    if len(after) != len(before):
        raise SystemExit(
            f"settings.db changed size ({len(before)} -> {len(after)}); "
            "replace() cannot grow a file")
    return touched


def main(argv):
    if len(argv) >= 3 and argv[2] == "set-provisioned":
        path = argv[1]
        img, save, fs = open_part(path, "userdata")
        obj, info = fs.find(SETTINGS_DB)
        if obj is None:
            raise SystemExit(
                f"{SETTINGS_DB} not found in /data.\n"
                "It is created on first boot; boot a working copy of the "
                "image once (no snapshot, so /data persists), reach the "
                "wizard, power off, then run this on that copy.")
        with tempfile.NamedTemporaryFile(suffix=".db") as tf:
            tf.write(fs.read_file(obj))
            tf.flush()
            touched = set_provisioned_db(tf.name)
            if not touched:
                raise SystemExit("no device_provisioned row to update "
                                 "(boot the image once first)")
            tf.seek(0)
            fs.replace_file(obj, open(tf.name, "rb").read())
        open(path, "wb").write(save(img))
        print(f"set {', '.join(touched)} = 1 in {SETTINGS_DB}")
        return 0

    if len(argv) < 4 or argv[2] != "--part":
        print(__doc__.split("Usage:")[1].strip())
        return 2
    path, part, cmd = argv[1], argv[3], argv[4] if len(argv) > 4 else "list"
    img, save, fs = open_part(path, part)

    if cmd == "list":
        for p, (obj, info) in sorted(fs.tree().items()):
            kind = {1: "f", 2: "l", 3: "d"}.get(info["type"], "?")
            sz = info["size"] if info["type"] == 1 else 0
            print(f"{kind} {sz:9d}  {p}")
        return 0

    if cmd == "extract":
        obj, info = fs.find(argv[5])
        if obj is None:
            raise SystemExit(f"not found: {argv[5]}")
        open(argv[6], "wb").write(fs.read_file(obj))
        print(f"extracted {argv[5]} ({info['size']} B) -> {argv[6]}")
        return 0

    if cmd == "replace":
        obj, info = fs.find(argv[5])
        if obj is None:
            raise SystemExit(f"not found: {argv[5]}")
        new = open(argv[6], "rb").read()
        fs.replace_file(obj, new)
        open(path, "wb").write(save(img))
        print(f"replaced {argv[5]} ({info['size']} -> {len(new)} B) in {path}")
        return 0

    raise SystemExit(f"unknown command {cmd!r}")


if __name__ == "__main__":
    sys.exit(main(sys.argv))

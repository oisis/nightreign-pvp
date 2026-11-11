# /// script
# dependencies = ["cryptography", "zstandard"]
# ///
"""Unpack an Elden Ring / Nightreign regulation.bin into one CSV per param.

    uv run regulation.py er|nr <regulation.bin> <paramdef dir> <out dir>

Format knowledge and keys come from the open-source SoulsFormats (Smithbox, GPL-3.0).
"""
import csv, re, struct, sys, zlib
import xml.etree.ElementTree as ET
from pathlib import Path

import zstandard
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEYS = {
    "er": "99BFFC366A6BC8C6F5827D093602D676C42892A01C207FB024D3AF4E493FEF99",
    "nr": "9a8ee90c4c01a43168a17d9d75e4a7d02107ebcf43d5acb0554f941601b57918",
}
SIZES = {"s8": 1, "u8": 1, "dummy8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4,
         "b32": 4, "f32": 4, "angle32": 4, "f64": 8, "fixstr": 1, "fixstrW": 2}
FMT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I",
       "b32": "i", "f32": "f", "angle32": "f", "f64": "d"}


def decrypt(data, key):
    d = Cipher(algorithms.AES(bytes.fromhex(key)), modes.CBC(data[:16])).decryptor()
    return d.update(data[16:len(data) - len(data) % 16]) + d.finalize()


def undcx(d):
    assert d[:4] == b"DCX\0", d[:4]
    fmt = d[0x28:0x2C]
    body = d[0x4C:]
    if fmt == b"DFLT":
        return zlib.decompress(body)
    if fmt == b"ZSTD":
        return zstandard.ZstdDecompressor().decompressobj().decompress(body)
    if fmt == b"KRAK":  # Oodle Kraken via open-source ooz. ponytail: fails on some large files (talkesdbnd); use real Oodle or oozextract if those are needed
        import ooz
        usize, csize = struct.unpack_from(">II", d, 0x1C)
        return ooz.decompress(body[:csize], usize)
    raise SystemExit(f"unsupported DCX compression {fmt!r}")


def bnd4(d):
    """Yield (name, bytes). Assumes the 0x24-byte entry layout regulation files use."""
    assert d[:4] == b"BND4"
    count, = struct.unpack_from("<i", d, 0x0C)
    entry_size, = struct.unpack_from("<q", d, 0x20)
    assert entry_size == 0x24, hex(entry_size)
    for i in range(count):
        _, _, _csize, usize, off, _id, name_off = struct.unpack_from("<IiqqIiI", d, 0x40 + i * 0x24)
        end = name_off
        while d[end:end + 2] != b"\0\0":
            end += 2
        yield d[name_off:end].decode("utf-16le"), d[off:off + usize]


def load_def(path, regver):
    """Return [(name, type, count, bits)] for the fields live at this regulation version."""
    fields = []
    for f in ET.parse(path).getroot().iter("Field"):
        first, removed = int(f.get("FirstVersion", 0)), int(f.get("RemovedVersion", 0))
        if first > regver or (removed and removed <= regver):
            continue
        m = re.match(r"(\w+)\s+(\w+)(?::(\d+))?(?:\[(\d+)\])?", f.get("Def"))
        fields.append((m[2], m[1], int(m[4] or 1), int(m[3] or 0)))
    return fields


def decode_row(row, fields):
    out, pos, bit_pos, bit_size = [], 0, 0, 0
    for name, typ, count, bits in fields:
        size = SIZES[typ]
        if bits:
            if bit_size != size or bit_pos + bits > size * 8:  # start a new bitfield unit
                pos += bit_size if bit_size else 0
                bit_pos, bit_size = 0, size
            unit = int.from_bytes(row[pos:pos + size], "little")
            if typ != "dummy8":
                out.append((name, (unit >> bit_pos) & ((1 << bits) - 1)))
            bit_pos += bits
            continue
        if bit_size:
            pos, bit_pos, bit_size = pos + bit_size, 0, 0
        if typ == "dummy8":
            pass
        elif typ.startswith("fixstr"):
            raw = row[pos:pos + size * count]
            out.append((name, raw.decode("utf-16le" if size == 2 else "shift_jis", "replace").split("\0")[0]))
        elif count > 1:
            out.append((name, list(struct.unpack_from(f"<{count}{FMT[typ]}", row, pos))))
        else:
            out.append((name, struct.unpack_from("<" + FMT[typ], row, pos)[0]))
        pos += size * count
    return out, pos + bit_size


def param_rows(d):
    """Yield (id, row bytes); also return the param type via .send-less attribute on the generator."""
    strings_off, = struct.unpack_from("<I", d, 0)
    row_count, = struct.unpack_from("<H", d, 0x0A)
    f2d = d[0x2D]
    if f2d & 4:  # long offsets
        ents = [struct.unpack_from("<iiqq", d, 0x40 + i * 24) for i in range(row_count)]
        ents = [(e[0], e[2]) for e in ents]
    else:
        hdr = 0x40 if (f2d & 3) == 3 else 0x30
        ents = [struct.unpack_from("<II", d, hdr + i * 12) for i in range(row_count)]
    if not ents:
        return
    size = (ents[1][1] - ents[0][1]) if len(ents) > 1 else (strings_off - ents[0][1])
    for rid, off in ents:
        yield rid, d[off:off + size]


def param_type(d):
    if d[0x2D] & 0x80:
        off, = struct.unpack_from("<i", d, 0x10)
        return d[off:d.index(b"\0", off)].decode()
    return d[0x0C:0x2C].split(b"\0")[0].decode()


def main(game, reg, defdir, outdir):
    raw = Path(reg).read_bytes()
    bnd = undcx(decrypt(raw, KEYS[game]))
    regver = int(bnd[0x18:0x20].rstrip(b"\0").decode() or 0)
    print(f"regulation version {regver}")
    defs = {}
    for p in Path(defdir).glob("*.xml"):
        defs[ET.parse(p).getroot().findtext("ParamType")] = p
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    summary = []
    for name, data in bnd4(bnd):
        stem = re.split(r"[\\/]", name)[-1].removesuffix(".param")
        ptype = param_type(data)
        rows = list(param_rows(data))
        status = "no-def"
        if ptype in defs and rows:
            fields = load_def(defs[ptype], regver)
            try:
                decoded, used = decode_row(rows[0][1], fields)
            except struct.error:
                used = -1  # def is longer than the row
            status = "ok" if used == len(rows[0][1]) else f"size-mismatch def={used} row={len(rows[0][1])}"
            if status == "ok":
                with open(out / f"{stem}.csv", "w", newline="") as fh:
                    w = csv.writer(fh)
                    w.writerow(["ID"] + [n for n, _ in decoded])
                    for rid, row in rows:
                        w.writerow([rid] + [v for _, v in decode_row(row, fields)[0]])
        elif ptype in defs:
            status = "empty"
        summary.append((stem, ptype, len(rows), status))
    with open(out / "_summary.tsv", "w") as fh:
        for s in sorted(summary):
            fh.write("\t".join(map(str, s)) + "\n")
    bad = [s for s in summary if s[3] not in ("ok", "empty")]
    print(f"{len(summary)} params, {len(summary) - len(bad)} decoded, {len(bad)} not:")
    for s in bad:
        print("  ", *s)


if __name__ == "__main__":
    main(*sys.argv[1:5])

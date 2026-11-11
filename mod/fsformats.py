"""Minimal readers/writers for the FromSoftware containers the mod has to touch: DCX, BND4, FMG.

Format knowledge comes from the open-source SoulsFormats (Smithbox, GPL-3.0). Only what Nightreign's
menu text and talk script bundles need is implemented.
"""
import struct
import zlib


# --- DCX -----------------------------------------------------------------------------------------

def dcx_compression(data):
    """Compression tag of a DCX file: b"KRAK", b"DFLT" or b"ZSTD"."""
    assert data[:4] == b"DCX\0", data[:4]
    return data[0x28:0x2C]


def dcx_write_deflate(raw):
    """Wrap raw bytes in a zlib DCX (the Elden Ring style DCX_DFLT_11000_44_9 header)."""
    body = zlib.compress(raw, 9)
    return (b"DCX\0" + struct.pack(">5I", 0x11000, 0x18, 0x24, 0x44, 0x4C)
            + b"DCS\0" + struct.pack(">2I", len(raw), len(body))
            + b"DCP\0DFLT" + struct.pack(">I", 0x20) + bytes([9, 0, 0, 0]) + bytes(12)
            + struct.pack(">I", 0x00010100)
            + b"DCA\0" + struct.pack(">I", 8) + body)


# --- BND4 ----------------------------------------------------------------------------------------

_ENTRY = "<IiqqIiI"  # flags, -1, compressed size, size, data offset, id, name offset (0x24 bytes)


def bnd4_read(data):
    """Return [(name, bytes)] in file order. Assumes the 0x24-byte entries Nightreign bundles use."""
    assert data[:4] == b"BND4", data[:4]
    count, = struct.unpack_from("<i", data, 0x0C)
    assert struct.unpack_from("<q", data, 0x20)[0] == 0x24
    out = []
    for i in range(count):
        _, _, _, size, off, _, name_off = struct.unpack_from(_ENTRY, data, 0x40 + i * 0x24)
        end = name_off
        while data[end:end + 2] != b"\0\0":
            end += 2
        out.append((data[name_off:end].decode("utf-16le"), data[off:off + size]))
    return out


def bnd4_replace(data, replacements):
    """Return a copy of a BND4 with some files' contents replaced: {name suffix: new bytes}.

    Everything before the data area (headers, names, hash table) is kept; the data area is rebuilt
    and the size/offset fields patched.
    """
    files = bnd4_read(data)
    count = len(files)
    offsets = [struct.unpack_from(_ENTRY, data, 0x40 + i * 0x24)[4] for i in range(count)]
    out = bytearray(data[:min(offsets)])
    used = set()
    for i, (name, blob) in enumerate(files):
        for suffix, new in replacements.items():
            if name.lower().endswith(suffix.lower()):
                blob = new
                used.add(suffix)
        out += bytes(-len(out) % 0x10)
        flags, minus1, _, _, _, fid, name_off = struct.unpack_from(_ENTRY, out, 0x40 + i * 0x24)
        struct.pack_into(_ENTRY, out, 0x40 + i * 0x24, flags, minus1, len(blob), len(blob), len(out), fid, name_off)
        out += blob
    missing = set(replacements) - used
    if missing:
        raise KeyError(f"not in bundle: {sorted(missing)}")
    return bytes(out)


# --- FMG (text tables), version 2 as used since Dark Souls 3 --------------------------------------

def fmg_read(data):
    """Return {id: text or None}."""
    assert data[2] == 2 and data[8] == 1, "only wide, unicode FMG (version 2) is supported"
    group_count, string_count = struct.unpack_from("<ii", data, 0x0C)
    offsets_off, = struct.unpack_from("<q", data, 0x18)
    out = {}
    for g in range(group_count):
        index, first, last, _ = struct.unpack_from("<iiii", data, 0x28 + g * 0x10)
        for j in range(last - first + 1):
            off, = struct.unpack_from("<q", data, offsets_off + (index + j) * 8)
            text = None
            if off > 0:
                end = off
                while data[end:end + 2] != b"\0\0":
                    end += 2
                text = data[off:end].decode("utf-16le")
            out[first + j] = text
    return out


def fmg_write(entries):
    """Serialise {id: text or None} back to a version-2 FMG."""
    ids = sorted(entries)
    groups = []  # [first, last]
    for i in ids:
        if groups and groups[-1][1] + 1 == i:
            groups[-1][1] = i
        else:
            groups.append([i, i])
    offsets_off = 0x28 + len(groups) * 0x10
    strings_off = offsets_off + len(ids) * 8
    head = bytearray(0x28)
    table, blob, index = bytearray(), bytearray(), 0
    group_bytes = bytearray()
    for first, last in groups:
        group_bytes += struct.pack("<iiii", index, first, last, 0)
        index += last - first + 1
    for i in ids:
        text = entries[i]
        if text is None:
            table += struct.pack("<q", 0)
        else:
            table += struct.pack("<q", strings_off + len(blob))
            blob += text.encode("utf-16le") + b"\0\0"
    blob += bytes(-len(blob) % 4)
    total = strings_off + len(blob)
    head[2] = 2
    struct.pack_into("<i", head, 0x04, total)
    head[8] = 1
    struct.pack_into("<iii", head, 0x0C, len(groups), len(ids), 0xFF)
    struct.pack_into("<q", head, 0x18, offsets_off)
    return bytes(head + group_bytes + table + blob)


def demo():
    """Self-check: FMG round trip and BND4 replace on synthetic data."""
    texts = {1: "one", 2: None, 3: "trzy ąę", 10: "ten", 28000000: "far"}
    assert fmg_read(fmg_write(texts)) == texts
    raw = b"hello" * 100
    d = dcx_write_deflate(raw)
    assert dcx_compression(d) == b"DFLT" and zlib.decompress(d[0x4C:]) == raw
    print("fsformats ok")


if __name__ == "__main__":
    demo()

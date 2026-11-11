"""Developer tool: turn "original script vs our compiled script" into a small binary delta.

    python3 make_delta.py <original bundle, decompressed> <our bundle, decompressed> <esd name> <out.delta>

The launcher applies the delta to the player's own copy of the script, so the mod never has to ship
FromSoftware's file. Format: "NRD1", u32 base length, u64 base FNV-1a, u32 output length,
u64 output FNV-1a, then operations: 0 = copy (u32 offset, u32 length) from the base,
1 = insert (u32 length, bytes).
"""
import struct
import sys
from difflib import SequenceMatcher
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from fsformats import bnd4_read


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def make(base, new):
    out = bytearray(b"NRD1" + struct.pack("<IQIQ", len(base), fnv1a64(base), len(new), fnv1a64(new)))
    for tag, i1, i2, j1, j2 in SequenceMatcher(None, base, new, autojunk=False).get_opcodes():
        if tag == "equal":
            out += struct.pack("<BII", 0, i1, i2 - i1)
        elif j2 > j1:
            out += struct.pack("<BI", 1, j2 - j1) + new[j1:j2]
    return bytes(out)


def apply(base, delta):
    assert delta[:4] == b"NRD1"
    base_len, base_hash, out_len, out_hash = struct.unpack_from("<IQIQ", delta, 4)
    assert len(base) == base_len and fnv1a64(base) == base_hash, "base file is not the expected version"
    out, pos = bytearray(), 28
    while pos < len(delta):
        if delta[pos] == 0:
            off, n = struct.unpack_from("<II", delta, pos + 1)
            out += base[off:off + n]
            pos += 9
        else:
            n, = struct.unpack_from("<I", delta, pos + 1)
            out += delta[pos + 5:pos + 5 + n]
            pos += 5 + n
    assert len(out) == out_len and fnv1a64(out) == out_hash
    return bytes(out)


if __name__ == "__main__":
    orig, ours, name, dst = sys.argv[1:5]
    pick = lambda path: next(b for n, b in bnd4_read(Path(path).read_bytes()) if n.lower().endswith(name.lower()))
    base, new = pick(orig), pick(ours)
    delta = make(base, new)
    assert apply(base, delta) == new
    Path(dst).write_bytes(delta)
    print(f"{name}: base {len(base)} bytes, ours {len(new)} bytes, delta {len(delta)} bytes")

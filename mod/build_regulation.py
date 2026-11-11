# /// script
# dependencies = ["cryptography", "zstandard"]
# ///
"""Development build of regulation.bin with the "Invasion" entry in the expedition menu's Target list.

    uv run build_regulation.py <game's regulation.bin> <out regulation.bin>

The Target list is NightBossMenuParam. Row 100 is the game's own non-boss entry ("The Deep of Night");
our row 101 is a copy of it with our texts (ids in CL_MenuText, added by build_msg.py).
Experiment: the launcher cannot do this yet (it has no zstd), and selecting the entry is not handled.
"""
import struct
import sys
from pathlib import Path

import zstandard
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, str(Path(__file__).parent))
sys.path.insert(0, str(Path(__file__).parent.parent / "re"))
from fsformats import bnd4_read, bnd4_replace
from regulation import KEYS, decrypt, undcx

PARAM = "\\NightBossMenuParam.param"
TEMPLATE_ROW, NEW_ROW = 100, 101
# field values of the template row -> ours (each must occur exactly once in the row)
CHANGES = {131150: 131160,   # expeditionNameId
           131151: 131161,   # descriptionId
           300: 310,         # sortId: after The Deep of Night
           130: 0}           # unlockEventFlag: always available


def add_row(param):
    """Append a copy of TEMPLATE_ROW as NEW_ROW. Layout: 0x40 header, 24-byte row descriptors
    (id, pad, data offset, name offset), row data, strings."""
    strings_off, = struct.unpack_from("<I", param, 0)
    count, = struct.unpack_from("<H", param, 0x0A)
    assert param[0x2D] & 4, "expected 64-bit row offsets"
    rows = [struct.unpack_from("<iiqq", param, 0x40 + i * 24) for i in range(count)]
    data_start = rows[0][2]
    size = rows[1][2] - rows[0][2]
    assert data_start == 0x40 + count * 24 and strings_off == data_start + count * size
    assert all(r[0] < NEW_ROW for r in rows), "row id already used"
    template = next(r for r in rows if r[0] == TEMPLATE_ROW)
    row = bytearray(param[template[2]:template[2] + size])
    for old, new in CHANGES.items():
        hits = [o for o in range(0, size - 3) if struct.unpack_from("<i", row, o)[0] == old]
        assert len(hits) == 1, f"value {old} found {len(hits)} times in the template row"
        struct.pack_into("<i", row, hits[0], new)

    shift = 24 + size  # one descriptor before the data, one row before the strings
    out = bytearray(param[:0x40])
    struct.pack_into("<I", out, 0, strings_off + shift)
    struct.pack_into("<H", out, 0x0A, count + 1)
    struct.pack_into("<q", out, 0x10, struct.unpack_from("<q", param, 0x10)[0] + shift)
    struct.pack_into("<q", out, 0x30, data_start + 24)
    for rid, pad, data_off, name_off in rows:
        out += struct.pack("<iiqq", rid, pad, data_off + 24, name_off + shift)
    out += struct.pack("<iiqq", NEW_ROW, 0, data_start + 24 + count * size, template[3] + shift)
    out += param[data_start:strings_off] + row + param[strings_off:]
    return bytes(out)


def main(src, dst):
    raw = Path(src).read_bytes()
    dcx = decrypt(raw, KEYS["nr"])
    bundle = undcx(dcx)
    param = next(b for n, b in bnd4_read(bundle) if n.endswith(PARAM))
    patched = bnd4_replace(bundle, {PARAM: add_row(param)})

    body = zstandard.ZstdCompressor(level=15).compress(patched)
    header = bytearray(dcx[:0x4C])  # same DCX (ZSTD) header as the game's, with the new sizes
    struct.pack_into(">II", header, 0x1C, len(patched), len(body))
    plain = bytes(header) + body
    plain += bytes(-len(plain) % 16)
    enc = Cipher(algorithms.AES(bytes.fromhex(KEYS["nr"])), modes.CBC(raw[:16])).encryptor()
    out = raw[:16] + enc.update(plain) + enc.finalize()

    # Read our own output back the way the game would.
    check = undcx(decrypt(out, KEYS["nr"]))
    p = next(b for n, b in bnd4_read(check) if n.endswith(PARAM))
    count, = struct.unpack_from("<H", p, 0x0A)
    ids = [struct.unpack_from("<i", p, 0x40 + i * 24)[0] for i in range(count)]
    assert ids[-1] == NEW_ROW and len(bnd4_read(check)) == len(bnd4_read(bundle))
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(out)
    print(f"regulation.bin: Target list rows {ids}")


if __name__ == "__main__":
    main(*sys.argv[1:3])

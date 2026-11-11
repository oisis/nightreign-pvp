# /// script
# dependencies = ["cryptography", "zstandard"]
# ///
"""Development build of regulation.bin with the "Invasion" entry in the expedition menu's Target list.

    uv run build_regulation.py <game's regulation.bin> <out regulation.bin> [--no-row]

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
           41812: 41813,     # bossIconId: our icon (build_icon.py)
           300: 5,           # sortId: before the first Nightlord (the DLL also places it first)
           130: 0}           # unlockEventFlag: always available


FIRST_LARGE_ICON, LARGE_ICON = 41820, 41814   # row 0's largeBossIconId; ours (build_icon.py)


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
    # largeBossIconId (-1 in the template, like several other fields): find the field in the first
    # Nightlord's row, whose value is unique there.
    first = param[rows[0][2]:rows[0][2] + size]
    hits = [o for o in range(0, size - 3, 4) if struct.unpack_from("<i", first, o)[0] == FIRST_LARGE_ICON]
    assert len(hits) == 1 and struct.unpack_from("<i", row, hits[0])[0] == -1
    struct.pack_into("<i", row, hits[0], LARGE_ICON)

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


def main(src, dst, no_row=False):
    raw = Path(src).read_bytes()
    dcx = decrypt(raw, KEYS["nr"])
    bundle = undcx(dcx)
    param = next(b for n, b in bnd4_read(bundle) if n.endswith(PARAM))
    # --no-row: repack without changing anything, to tell packing problems from param problems.
    patched = bundle if no_row else bnd4_replace(bundle, {PARAM: add_row(param)})

    # Compress the way Smithbox does (level 15, 64 KB window, no content size in the frame header).
    # Frames with a larger window, even ones that are otherwise valid, crash the game at start-up.
    level = 15
    params = zstandard.ZstdCompressionParameters.from_level(level, window_log=16, write_content_size=False)
    body = zstandard.ZstdCompressor(compression_params=params).compress(patched)
    header = bytearray(dcx[:0x4C])  # the game's DCX (ZSTD) header with our sizes and level
    struct.pack_into(">II", header, 0x1C, len(patched), len(body))
    header[0x30] = level
    plain = bytes(header) + body
    plain += bytes(-len(plain) % 16)
    enc = Cipher(algorithms.AES(bytes.fromhex(KEYS["nr"])), modes.CBC(raw[:16])).encryptor()
    out = raw[:16] + enc.update(plain) + enc.finalize()

    # Read our own output back the way the game would.
    check = undcx(decrypt(out, KEYS["nr"]))
    p = next(b for n, b in bnd4_read(check) if n.endswith(PARAM))
    count, = struct.unpack_from("<H", p, 0x0A)
    ids = [struct.unpack_from("<i", p, 0x40 + i * 24)[0] for i in range(count)]
    assert (ids[-1] == NEW_ROW) != no_row and len(bnd4_read(check)) == len(bnd4_read(bundle))
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(out)
    print(f"regulation.bin: Target list rows {ids}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], "--no-row" in sys.argv)

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

# Team test (DLL, "team-test" file): a SpEffect of ours that puts its target on team 16 for 180 s.
# Copied from row 10101, which equals the most common value in every field (no icon, no vfx, no
# category, no stat change, never saved: saveCategory -1). Row size 0x400 as in the game's file;
# field offsets from Smithbox's NR paramdef, checked on the binary rows of the PVP mod (its 300 s
# team-16 row 707163 has 300.0 at +0x08 and 16 at +0x16d), and +0x16d is what the game checks
# (0x1403db410: < 81 -> temporary team).
SPEFFECT_PARAM = "\\SpEffectParam.param"
SPEFFECT_TEMPLATE, SPEFFECT_TEAM_ROW = 10101, 90071610
SPEFFECT_SIZE = 0x400
SPEFFECT_ENDURANCE, SPEFFECT_TEAM = 0x08, 0x16D   # effectEndurance f32 (s), changeTeamType u8
SPEFFECT_VFX1 = 0x18C   # vfxId1 s32: the slot the PVP mod's team-16 row uses (150053 found at +0x18c)
TEAM_INVADER, TEAM_TEST_SECONDS = 16, 180.0
# Red look: SpEffectVfxParam 300050, a vanilla row that only sets phantomParamOverwriteType 2 and
# phantomParamOverwriteId 60 (PhantomParam 60, "Red Phantom" in Smithbox's row names); vanilla
# SpEffects 14559, 18698 and 99860 use it.
RED_PHANTOM_VFX = 300050


# Scarabs (decision 13): an invader on team 16 can hit them. NpcParam rows 41900000-41919999 (the
# 30 rows Smithbox names "Scarab" / "Large Scarab"; all team 6) get team 17. In the game's 81x81
# team table 17 is like 6 toward the squad (1, 2: enemy both ways) and toward other monsters (6,
# 7, 33, 48, 50: same as 6), enemy of 16 both ways, friend of itself; it differs from 6 only toward
# 16, 18, 32 (unused by any NPC) and becomes neutral instead of enemy toward 22, 25-28, 46 (team 26:
# merchants and a few special NPCs) and friend toward 21, 24, 35, 41. +0x133 is the field the game
# reads as the NPC's team (0x1403d77d0 via 0x140e95440), checked on rows of teams 6, 7 and 33.
NPC_PARAM = "\\NpcParam.param"
NPC_TEAM, SCARAB_TEAM = 0x133, 17
SCARAB_IDS = range(41900000, 41920000)


def scarab_rows(param):
    count, = struct.unpack_from("<H", param, 0x0A)
    out = bytearray(param)
    changed = 0
    for i in range(count):
        rid, _, off, _ = struct.unpack_from("<iiqq", param, 0x40 + i * 24)
        if rid in SCARAB_IDS:
            assert out[off + NPC_TEAM] == 6, f"NpcParam {rid}: team {out[off + NPC_TEAM]}, expected 6"
            out[off + NPC_TEAM] = SCARAB_TEAM
            changed += 1
    assert changed == 30, f"{changed} scarab rows"
    return bytes(out)


# No rain (test sessions only, --no-rain): PlayAreaCreateCommonParam row 0 names the SpEffects the
# rain puts on a character outside the circle, day 1 at +0x28 (99810) and day 2 at +0x2c (99811):
# 2 % of HP plus 15 / 30 per tick, 110 s, chaining to 99871 / 99876. Both become -1, the value rows
# 1 and 2 of the same param carry. The circle still shrinks on its own schedule; standing outside it
# simply costs nothing. Every player needs the same file: the effect is applied on each machine.
PLAY_AREA_PARAM = "\\PlayAreaCreateCommonParam.param"
RAIN_FIELDS = {0x28: 99810, 0x2C: 99811}


def no_rain(param):
    count, = struct.unpack_from("<H", param, 0x0A)
    out = bytearray(param)
    rid, _, off, _ = struct.unpack_from("<iiqq", param, 0x40)
    assert rid == 0
    for field, old in RAIN_FIELDS.items():
        assert struct.unpack_from("<i", out, off + field)[0] == old
        struct.pack_into("<i", out, off + field, -1)
    return bytes(out)


def insert_row(param, template_id, new_id, size, patch):
    """Copy row template_id as new_id, kept in id order (the game binary-searches ids); patch(row)
    edits the copy. Same layout as add_row; the data area is rebuilt in descriptor order."""
    strings_off, = struct.unpack_from("<I", param, 0)
    count, = struct.unpack_from("<H", param, 0x0A)
    assert param[0x2D] & 4, "expected 64-bit row offsets"
    rows = [struct.unpack_from("<iiqq", param, 0x40 + i * 24) for i in range(count)]
    data_start = rows[0][2]
    assert rows[1][2] - rows[0][2] == size, "row size differs from the paramdef"
    assert all(r[2] == data_start + i * size for i, r in enumerate(rows)), "data not in descriptor order"
    assert data_start == 0x40 + count * 24 and strings_off == data_start + count * size
    assert all(rows[i][0] < rows[i + 1][0] for i in range(count - 1)), "ids not sorted"
    assert all(r[0] != new_id for r in rows), "row id already used"
    template = next(r for r in rows if r[0] == template_id)
    row = bytearray(param[template[2]:template[2] + size])
    patch(row)
    shift = 24 + size
    entries = [(r[0], param[r[2]:r[2] + size], r[3] + shift) for r in rows] + [(new_id, bytes(row), template[3] + shift)]
    entries.sort(key=lambda e: e[0])
    out = bytearray(param[:0x40])
    struct.pack_into("<I", out, 0, strings_off + shift)
    struct.pack_into("<H", out, 0x0A, count + 1)
    struct.pack_into("<q", out, 0x10, struct.unpack_from("<q", param, 0x10)[0] + shift)
    struct.pack_into("<q", out, 0x30, data_start + 24)
    new_start = data_start + 24
    for i, (rid, _, name_off) in enumerate(entries):
        out += struct.pack("<iiqq", rid, 0, new_start + i * size, name_off)
    for _, data, _ in entries:
        out += data
    out += param[strings_off:]
    return bytes(out)


def team_row(row):
    assert struct.unpack_from("<f", row, SPEFFECT_ENDURANCE)[0] == -1.0 and row[SPEFFECT_TEAM] == 0xFF
    assert struct.unpack_from("<i", row, SPEFFECT_VFX1)[0] == -1
    struct.pack_into("<f", row, SPEFFECT_ENDURANCE, TEAM_TEST_SECONDS)
    row[SPEFFECT_TEAM] = TEAM_INVADER
    struct.pack_into("<i", row, SPEFFECT_VFX1, RED_PHANTOM_VFX)


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


def main(src, dst, no_row=False, rain=True):
    raw = Path(src).read_bytes()
    dcx = decrypt(raw, KEYS["nr"])
    bundle = undcx(dcx)
    param = next(b for n, b in bnd4_read(bundle) if n.endswith(PARAM))
    speffect = next(b for n, b in bnd4_read(bundle) if n.endswith(SPEFFECT_PARAM))
    npc = next(b for n, b in bnd4_read(bundle) if n.endswith(NPC_PARAM))
    # --no-row: repack without changing anything, to tell packing problems from param problems.
    patched = bundle if no_row else bnd4_replace(bundle, {
        PARAM: add_row(param),
        SPEFFECT_PARAM: insert_row(speffect, SPEFFECT_TEMPLATE, SPEFFECT_TEAM_ROW, SPEFFECT_SIZE, team_row),
        NPC_PARAM: scarab_rows(npc),
        **({} if rain else {PLAY_AREA_PARAM: no_rain(next(b for n, b in bnd4_read(bundle) if n.endswith(PLAY_AREA_PARAM)))})})

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
    if not no_row:  # our SpEffect row reads back with its two fields, between its neighbours
        p = next(b for n, b in bnd4_read(check) if n.endswith(SPEFFECT_PARAM))
        count, = struct.unpack_from("<H", p, 0x0A)
        rows = [struct.unpack_from("<iiqq", p, 0x40 + i * 24) for i in range(count)]
        assert all(rows[i][0] < rows[i + 1][0] for i in range(count - 1))
        off = next(r[2] for r in rows if r[0] == SPEFFECT_TEAM_ROW)
        assert struct.unpack_from("<f", p, off + SPEFFECT_ENDURANCE)[0] == TEAM_TEST_SECONDS
        assert p[off + SPEFFECT_TEAM] == TEAM_INVADER
        assert struct.unpack_from("<i", p, off + SPEFFECT_VFX1)[0] == RED_PHANTOM_VFX
        print(f"regulation.bin: SpEffectParam row {SPEFFECT_TEAM_ROW} (team {TEAM_INVADER}, {TEAM_TEST_SECONDS:g} s), {count} rows")
        p = next(b for n, b in bnd4_read(check) if n.endswith(NPC_PARAM))
        count, = struct.unpack_from("<H", p, 0x0A)
        teams = [p[struct.unpack_from("<q", p, 0x40 + i * 24 + 8)[0] + NPC_TEAM] for i in range(count)
                 if struct.unpack_from("<i", p, 0x40 + i * 24)[0] in SCARAB_IDS]
        assert teams == [SCARAB_TEAM] * 30
        print(f"regulation.bin: {len(teams)} scarab NpcParam rows on team {SCARAB_TEAM}")
        p = next(b for n, b in bnd4_read(check) if n.endswith(PLAY_AREA_PARAM))
        off = struct.unpack_from("<q", p, 0x48)[0]
        values = [struct.unpack_from("<i", p, off + f)[0] for f in RAIN_FIELDS]
        assert values == ([99810, 99811] if rain else [-1, -1])
        print(f"regulation.bin: rain {'as in the game' if rain else 'OFF (test build)'}")
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(out)
    print(f"regulation.bin: Target list rows {ids}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], "--no-row" in sys.argv, "--no-rain" not in sys.argv)

"""Add the mod's menu lines to a decompressed menu_dlc01.msgbnd and write it as a DCX.

    python3 build_msg.py <language, e.g. engus> <decompressed bundle> <out .dcx>

The "Commence expedition" line is copied from the same bundle (the table's own prompt), so it is
in the player's language. The other lines are ours: Polish for polpl, English otherwise.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from fsformats import bnd4_read, bnd4_replace, dcx_write_deflate, fmg_read, fmg_write

TEXT_EXPEDITION = 28100001  # ids used by mod/talk/t600001000.py
EXPEDITION_SOURCE = ("ActionButtonText.fmg", 1882)  # "Commence expedition"
# Our own lines: text table -> id -> (English, Polish). Other languages get English.
# Keep in sync with launcher/core.c.
OUR_TEXT = {
    "EventTextForTalk.fmg": {
        28100002: ("Invade another world", "Najedź inny świat"),
    },
    # Entry in the expedition menu's Target list (NightBossMenuParam row 101, mod/build_regulation.py).
    "CL_MenuText.fmg": {
        131160: ("Invasion", "Najazd"),
        131161: ("Enter another squad's expedition as an invader.\nDefeat its host to win.",
                 "Wejdź do wyprawy innej drużyny jako najeźdźca.\nPokonaj hosta, aby wygrać."),
    },
}


def patch(lang, bundle):
    files = {name.split("\\")[-1]: blob for name, blob in bnd4_read(bundle)}
    expedition = (fmg_read(files[EXPEDITION_SOURCE[0]]).get(EXPEDITION_SOURCE[1]) or "Commence expedition").strip(" \u00a0")
    replacements = {}
    for table, lines in OUR_TEXT.items():
        texts = fmg_read(files[table])
        if table == "EventTextForTalk.fmg":
            texts[TEXT_EXPEDITION] = expedition
        for text_id, (english, polish) in lines.items():
            texts[text_id] = polish if lang == "polpl" else english
        replacements["\\" + table] = fmg_write(texts)
    return bnd4_replace(bundle, replacements)


if __name__ == "__main__":
    lang, src, dst = sys.argv[1:4]
    out = patch(lang, Path(src).read_bytes())
    files = dict((n.split("\\")[-1], b) for n, b in bnd4_read(out))
    print(lang, [fmg_read(files[t])[i].split("\n")[0] for t, lines in OUR_TEXT.items() for i in lines])
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(dcx_write_deflate(out))

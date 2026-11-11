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
# Our own lines: id -> (English, Polish). Other languages get English. Keep in sync with launcher/core.c.
OUR_TEXT = {
    28100002: ("Invade another world", "Najedź inny świat"),
}


def patch(lang, bundle):
    files = {name.split("\\")[-1]: blob for name, blob in bnd4_read(bundle)}
    expedition = (fmg_read(files[EXPEDITION_SOURCE[0]]).get(EXPEDITION_SOURCE[1]) or "Commence expedition").strip(" \u00a0")
    talk = fmg_read(files["EventTextForTalk.fmg"])
    talk[TEXT_EXPEDITION] = expedition
    for text_id, (english, polish) in OUR_TEXT.items():
        talk[text_id] = polish if lang == "polpl" else english
    return bnd4_replace(bundle, {"\\EventTextForTalk.fmg": fmg_write(talk)})


if __name__ == "__main__":
    lang, src, dst = sys.argv[1:4]
    out = patch(lang, Path(src).read_bytes())
    check = fmg_read(dict((n.split("\\")[-1], b) for n, b in bnd4_read(out))["EventTextForTalk.fmg"])
    print(lang, repr(check[TEXT_EXPEDITION]), [check[i] for i in OUR_TEXT])
    Path(dst).parent.mkdir(parents=True, exist_ok=True)
    Path(dst).write_bytes(dcx_write_deflate(out))

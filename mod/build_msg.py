"""Add the mod's menu lines to a decompressed menu_dlc01.msgbnd and write it as a DCX.

    python3 build_msg.py <language, e.g. engus> <decompressed bundle> <out .dcx>

Polish for polpl, English for every other language.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from fsformats import bnd4_read, bnd4_replace, dcx_write_deflate, fmg_read, fmg_write

# Our own lines: text table -> id -> (English, Polish). Keep in sync with launcher/core.c.
OUR_TEXT = {
    # Entry in the expedition menu's Target list (NightBossMenuParam row 101, mod/build_regulation.py).
    "CL_MenuText.fmg": {
        130460: ("4 players", "4 graczy"),  # fourth "Number of Players" option, added by the DLL
        131160: ("Invasion", "Najazd"),
        131161: ("Enter another squad's expedition as an invader.\nDefeat its host to win.",
                 "Wejdź do wyprawy innej drużyny jako najeźdźca.\nPokonaj hosta, aby wygrać."),
        # Notice shown by the DLL when Invasion is chosen (until the search exists).
        131162: ("Invasion chosen.\nSearching for an expedition to invade does not work in this version of the mod yet.",
                 "Wybrano najazd.\nSzukanie wyprawy do najechania nie działa jeszcze w tej wersji moda."),
    },
}


def patch(lang, bundle):
    files = {name.split("\\")[-1]: blob for name, blob in bnd4_read(bundle)}
    replacements = {}
    for table, lines in OUR_TEXT.items():
        texts = fmg_read(files[table])
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

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
    # Entries in the expedition menu's Target list (NightBossMenuParam rows 101 / 102, mod/build_regulation.py).
    "CL_MenuText.fmg": {
        # "Number of Players" (the DLL: 2 Solo, no session; 1 the game's solo start plus our host session; 0 party)
        130305: ("Solo + invaders", "Solo + najeźdźcy"),  # the start button for value 1
        130408: ("Team", "Drużyna"),  # the game's "three players" (value 0)
        130409: ("Solo", "Solo"),  # the game's "one player" (value 2)
        130417: ("Solo + invaders", "Solo + najeźdźcy"),  # the game's "two players" (value 1)
        131170: ("A newer mod version (%s) is available. Update to play with others.",
                 "Dostępna jest nowsza wersja moda (%s). Zaktualizuj, żeby grać z innymi."),  # %s: filled in by the DLL
        131171: ("Your team (same password) plays mod version %s. Everyone needs the same version.",
                 "Twoja drużyna (to samo hasło) gra na wersji moda %s. Wszyscy potrzebują tej samej wersji."),
        131172: ("Squads in an expedition: %d", "Drużyny w wyprawie: %d"),  # counts overlay, %d filled in by the DLL
        131173: ("Squads looking for a player: %d", "Drużyny szukające gracza: %d"),
        131174: ("Hosts waiting in the hub: %d", "Hosty czekające w Twierdzy: %d"),
        131160: ("Invasion", "Najazd"),
        131161: ("Enter another squad's expedition as an invader.\nDefeat its host to win.",
                 "Wejdź do wyprawy innej drużyny jako najeźdźca.\nPokonaj hosta, aby wygrać."),
        # Small notices shown by the DLL: repeated while searching, and once when the search is called off.
        131162: ("Searching for an expedition to invade...", "Szukanie wyprawy do najechania..."),
        131163: ("Invasion search cancelled", "Anulowano szukanie najazdu"),
        # The start button's label when Invasion is the chosen target: while searching, and otherwise
        # (the DLL puts them in place of "Begin expedition (n players)").
        131164: ("Cancel invasion", "Anuluj najazd"),
        131166: ("Begin invasion", "Rozpocznij najazd"),
        131167: ("Waiting for the squad's host...", "Oczekiwanie na hosta drużyny..."),  # party: the host is not in the field yet
        131168: ("The invader has left", "Najeźdźca odszedł"),  # host / allies: an invader left without dying
        131169: ("You have invaded another squad's expedition!", "Wkroczyłeś do wyprawy innej drużyny!"),  # the invader on arrival
        # "Play as a summon" (NightBossMenuParam row 102): name, description, searching / cancelled notices and the
        # start button's labels while searching / otherwise
        131175: ("Play as a summon", "Graj jako summon"),
        131176: ("Join another squad's expedition as its host's summon.\nWith a password: only that squad.",
                 "Dołącz do wyprawy innej drużyny jako summon jej hosta.\nZ hasłem: tylko do tej drużyny."),
        131177: ("Searching for a host to join...", "Szukanie hosta do dołączenia..."),
        131178: ("Summon search cancelled", "Anulowano szukanie hosta"),
        131194: ("You are offline: no host can be found", "Jesteś offline: nie można znaleźć hosta"),
        131179: ("Cancel search", "Anuluj szukanie"),
        131185: ("Begin search", "Rozpocznij szukanie"),
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

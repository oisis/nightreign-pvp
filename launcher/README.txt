Nightreign Invasions - test build
=================================

What this is
  An experimental mod for ELDEN RING NIGHTREIGN (PC, Steam). This build installs the mod's loader
  and client and changes the expedition menu: "Invasion" is the first entry of the Target list
  (with its own icon and description) and "Number of Players" has a fourth option.
  Choosing "Invasion" starts a search for an expedition to invade: a small notice keeps coming
  back while it runs, and the start button reads "Begin invasion" / "Cancel invasion" while the
  cursor is on "Invasion". Choosing it again, or starting an ordinary expedition, ends the search.
  No invasion will be found yet: joining another squad is not built, so do not wait for one.
  "4 players" asks for a squad of four (the mod lets a session hold up to six players), but a
  boss expedition with it is still refused: "cannot start a multiplayer session in offline mode".

How to start
  Windows: run NightreignInvasions.exe
  Linux:   run ./nightreign-invasions   (needs GTK 3; uses the Proton that Steam already installed)

  Steam must be running. Press "Play with the mod". The launcher:
    1. finds the game (or lets you choose its "Game" folder),
    2. backs up your save into work/backups/,
    3. builds the mod's files from your own game files into work/build/,
    4. starts the game through me3, offline, with a separate save file (NR0000-invasion.sl2).

  Nothing in the game's folder is changed. Your normal save is not used by the mod.
  While an invasion search runs, the mod turns on the game's network layer, which talks through
  Steam only. The mod writes a log to work/native/invasion.log in this folder.

Uninstall
  Press "Uninstall" (removes the built files and the mod's own save), then delete this folder.
  Backups in work/backups/ are kept until you delete the folder.

Seamless Co-op (optional, needed for multiplayer tests)
  Download Seamless Co-op for Nightreign yourself and put its "SeamlessCoop" folder next to the
  launcher. It is then loaded together with the mod and uses its own save file. While Seamless
  Co-op is loaded, the mod changes nothing in the menu.

Important
  - Game version 1.3.3 only. After a game update the mod does nothing until it is updated.
  - The mod never connects to FromSoftware's servers. A guard built into the mod refuses the
    game's own connections to those servers (web requests, their addresses, and direct
    connections the game opens itself), so do not try to use the mod online.
  - Included third-party software: me3 (mod loader, MIT / Apache-2.0, see me3/LICENSE-*).

---------------------------------------------------------------------------------------------

Po polsku
  Eksperymentalny mod do ELDEN RING NIGHTREIGN. Uruchom NightreignInvasions.exe (Windows) albo
  ./nightreign-invasions (Linux), przy włączonym Steamie, i kliknij „Graj z modem”. Launcher
  znajdzie grę, zrobi kopię zapisu w work/backups/, zbuduje pliki moda z Twoich plików gry i
  uruchomi grę przez me3 w trybie offline z osobnym plikiem zapisu. Folder gry nie jest zmieniany.
  „Odinstaluj” usuwa zbudowane pliki i osobny zapis moda. W menu wyprawy jest nowa pozycja
  „Najazd” (pierwsza na liście „Cel”) i opcja „4 graczy”.
  Wybranie „Najazdu” rozpoczyna szukanie wyprawy do najechania: w trakcie szukania wraca
  krótki komunikat, a przycisk startu, gdy kursor stoi na „Najeździe”, brzmi „Rozpocznij najazd”
  albo „Anuluj najazd”. Ponowne wybranie „Najazdu” albo start zwykłej wyprawy kończy szukanie.
  Najazd na razie nigdy się nie znajdzie: dołączanie do innej drużyny nie jest jeszcze gotowe,
  więc nie ma na co czekać. W trakcie szukania mod włącza warstwę sieciową gry, która łączy się
  wyłącznie przez Steam. „4 graczy” prosi o drużynę czteroosobową (mod pozwala na sesję do sześciu
  graczy), ale wyprawa na bossa z tą opcją jest nadal odrzucana komunikatem „Nie można
  rozpocząć sesji wieloosobowej w trybie offline”.
  Gdy załadowany jest Seamless Co-op, mod niczego w menu nie zmienia.
  Mod nigdy nie łączy się z serwerami FromSoftware: wbudowana w niego blokada odrzuca własne
  połączenia gry z tymi serwerami (zapytania sieciowe, ich adresy i bezpośrednie połączenia gry),
  więc nie próbuj grać z modem online. Dziennik moda: work/native/invasion.log w tym folderze.

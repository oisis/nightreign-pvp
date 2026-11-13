Nightreign Invasions - test build
=================================

What this is
  An experimental mod for ELDEN RING NIGHTREIGN (PC, Steam) that adds player invasions between mod
  players. Every expedition you start opens a session other mod players can invade. In the
  Roundtable Hold, "Invasion" (the first entry of the Target list) searches for another mod
  player's expedition and joins it as a red invader. A killed host ends the expedition and sends
  the invader home with a murk reward; a killed invader goes home with a smaller one.
  No settings are needed.

How to start
  Windows: run NightreignInvasions.exe
  Linux:   run ./nightreign-invasions   (needs GTK 3; uses the Proton that Steam already installed)

  Steam must be running. Press "Play with the mod". The launcher:
    1. finds the game (or lets you choose its "Game" folder),
    2. backs up your saves into backups/<date_time>/ (next to the launcher),
    3. builds the mod's files from your own game files into work/build/,
    4. starts the game through me3, offline, with a separate save file (NR0000-invasion.sl2).

  Nothing in the game's folder is changed. Your normal save is not used by the mod.
  While an invasion search runs, the mod turns on the game's network layer, which talks through
  Steam only. The mod writes a log to work/native/invasion.log in this folder.

Other buttons
  Backup: copies your saves (the normal one and the mod's own) into a new folder
  backups/<date_time>/ next to the launcher. It only copies; nothing is deleted or overwritten.
  Help: explains every button in the window's language.
  Debug log: the mod writes a short log by default; ticked, it logs everything (for bug reports).
  The log is work/native/invasion.log in this folder: send that file when you report a problem.
  Language: English, Polski, Deutsch, Français or Español; the choice is remembered.

Uninstall
  Press "Uninstall" (removes the built files and the mod's own save), then delete this folder.
  Backups in backups/ (and older ones in work/backups/) are kept until you delete the folder.

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
  znajdzie grę, zrobi kopię zapisu w backups/<data_godzina>/, zbuduje pliki moda z Twoich plików gry i
  uruchomi grę przez me3 w trybie offline z osobnym plikiem zapisu. Folder gry nie jest zmieniany.
  „Odinstaluj” usuwa zbudowane pliki i osobny zapis moda. W menu wyprawy jest nowa pozycja
  „Najazd” (pierwsza na liście „Cel”) i opcja „4 graczy”.
  Każda Twoja wyprawa otwiera sesję, którą inni gracze z modem mogą najechać. Wybranie
  „Najazdu” w Twierdzy szuka wyprawy innego gracza z modem i dołącza do niej jako czerwony
  najeźdźca. Zabity gospodarz kończy wyprawę, a najeźdźca wraca z nagrodą w czerniach; zabity
  najeźdźca wraca z mniejszą. Żadne ustawienia nie są potrzebne. W trakcie szukania i gry mod
  włącza warstwę sieciową gry, która łączy się wyłącznie przez Steam.
  „Kopia zapasowa” kopiuje zapisy do nowego folderu backups/<data_godzina>/ (tylko kopiuje).
  „Szczegółowy log” zapisuje wszystko do dziennika (do zgłaszania błędów); bez niego dziennik
  jest krótki. „Pomoc” opisuje wszystkie przyciski.
  Gdy załadowany jest Seamless Co-op, mod niczego w menu nie zmienia.
  Mod nigdy nie łączy się z serwerami FromSoftware: wbudowana w niego blokada odrzuca własne
  połączenia gry z tymi serwerami (zapytania sieciowe, ich adresy i bezpośrednie połączenia gry),
  więc nie próbuj grać z modem online. Dziennik moda: work/native/invasion.log w tym folderze.

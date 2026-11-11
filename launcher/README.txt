Nightreign Invasions - test build
=================================

What this is
  An experimental mod for ELDEN RING NIGHTREIGN (PC, Steam). This build installs the mod's loader
  and client and changes the expedition menu: "Invasion" is the first entry of the Target list
  (with its own icon and description) and "Number of Players" has a fourth option.
  Invasions do not work yet: choosing "Invasion" does nothing special, and "4 players" still
  behaves like 3.

How to start
  Windows: run NightreignInvasions.exe
  Linux:   run ./nightreign-invasions   (needs GTK 3; uses the Proton that Steam already installed)

  Steam must be running. Press "Play with the mod". The launcher:
    1. finds the game (or lets you choose its "Game" folder),
    2. backs up your save into work/backups/,
    3. builds the mod's files from your own game files into work/build/,
    4. starts the game through me3, offline, with a separate save file (NR0000-invasion.sl2).

  Nothing in the game's folder is changed. Your normal save is not used by the mod.

Uninstall
  Press "Uninstall" (removes the built files and the mod's own save), then delete this folder.
  Backups in work/backups/ are kept until you delete the folder.

Seamless Co-op (optional, needed for multiplayer tests)
  Download Seamless Co-op for Nightreign yourself and put its "SeamlessCoop" folder next to the
  launcher. It is then loaded together with the mod and uses its own save file.

Important
  - Game version 1.3.3 only. After a game update the mod does nothing until it is updated.
  - The mod never connects to the official servers. Do not try to use it online.
  - Included third-party software: me3 (mod loader, MIT / Apache-2.0, see me3/LICENSE-*).

---------------------------------------------------------------------------------------------

Po polsku
  Eksperymentalny mod do ELDEN RING NIGHTREIGN. Uruchom NightreignInvasions.exe (Windows) albo
  ./nightreign-invasions (Linux), przy wlaczonym Steamie, i kliknij "Graj z modem". Launcher
  znajdzie gre, zrobi kopie zapisu w work/backups/, zbuduje pliki moda z Twoich plikow gry i
  uruchomi gre offline z osobnym plikiem zapisu. Folder gry nie jest zmieniany.
  "Odinstaluj" usuwa zbudowane pliki i osobny zapis moda. W menu wyprawy jest nowa pozycja
  "Najazd" (pierwsza na liscie "Cel") i opcja "4 graczy". Inwazje jeszcze nie dzialaja: wybranie
  "Najazd" nic nie uruchamia, a "4 graczy" dziala jak 3.

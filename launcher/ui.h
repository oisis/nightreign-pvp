// Window texts shared by both front ends. English, and Polish when the system language is Polish.
#ifndef UI_H
#define UI_H

enum { T_TITLE, T_GAME_FOLDER, T_BROWSE, T_PLAY, T_UNINSTALL, T_CLOSE, T_NOT_FOUND, T_FOUND, T_PICK, T_BAD_FOLDER,
       T_CONFIRM_UNINSTALL, T_NO_SAVE, T_COUNT };

static const char *const UI_EN[T_COUNT] = {
    "Nightreign Invasions",
    "Game folder (the one with nightreign.exe):",
    "Browse...",
    "Play with the mod",
    "Uninstall",
    "Close",
    "The game was not found automatically. Choose its folder with Browse.",
    "Game found.",
    "Choose the game's \"Game\" folder",
    "This folder does not contain nightreign.exe and data0.bhd.",
    "Remove the mod's files and its separate save file? Your normal save and the backups stay.",
    "Save file not found; continuing without a backup.",
};
static const char *const UI_PL[T_COUNT] = {
    "Nightreign Invasions",
    "Folder gry (ten z plikiem nightreign.exe):",
    "Wybierz...",
    "Graj z modem",
    "Odinstaluj",
    "Zamknij",
    "Nie znaleziono gry automatycznie. Wska\xc5\xbc jej folder przyciskiem Wybierz.",
    "Znaleziono gr\xc4\x99.",
    "Wska\xc5\xbc folder \"Game\" gry",
    "W tym folderze nie ma plik\xc3\xb3w nightreign.exe i data0.bhd.",
    "Usun\xc4\x85\xc4\x87 pliki moda i jego osobny zapis gry? Zwyk\xc5\x82y zapis i kopie zapasowe zostaj\xc4\x85.",
    "Nie znaleziono pliku zapisu; kontynuuj\xc4\x99 bez kopii zapasowej.",
};

#endif

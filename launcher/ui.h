// Window texts shared by both front ends: one table, five languages. English is the default; the system
// language picks another when it is one of these, and the choice made in the window is remembered in
// language.txt next to the launcher. Strings are UTF-8. %s in T_BACKUP_DONE is the backup folder.
#ifndef UI_H
#define UI_H
#include <string.h>

enum { L_EN, L_PL, L_DE, L_FR, L_ES, LANG_COUNT };
static const char *const LANG_CODE[LANG_COUNT] = {"en", "pl", "de", "fr", "es"};
static const char *const LANG_NAME[LANG_COUNT] = {"English", "Polski", "Deutsch", "Français", "Español"};

enum { T_TITLE, T_GAME_FOLDER, T_BROWSE, T_PLAY, T_BACKUP, T_HELP, T_UNINSTALL, T_CLOSE, T_LANGUAGE, T_NOT_FOUND,
       T_FOUND, T_PICK, T_BAD_FOLDER, T_CONFIRM_UNINSTALL, T_NO_SAVE, T_BACKUP_DONE, T_BACKUP_FAILED, T_HELP_TEXT,
       T_DEBUG_LOG, T_COUNT };

static const char *const UI_TEXT[LANG_COUNT][T_COUNT] = {
    [L_EN] = {
        "Nightreign Invasions",
        "Game folder (the one with nightreign.exe):",
        "Browse...",
        "Play with the mod",
        "Backup",
        "Help",
        "Uninstall",
        "Close",
        "Language",
        "The game was not found automatically. Choose its folder with Browse.",
        "Game found.",
        "Choose the game's \"Game\" folder",
        "This folder does not contain nightreign.exe and data0.bhd.",
        "Remove the mod's files and its separate save file? Your normal save and the backups stay.",
        "No save file found; nothing was backed up.",
        "Backup made in %s (next to the launcher). Nothing was changed or deleted.",
        "The backup could not be made; nothing was changed or deleted.",
        "Browse: choose the game's folder by hand (the one with nightreign.exe), if it was not found.\n\n"
        "Play with the mod: first makes a backup of your saves (like Backup), then prepares the mod's files "
        "in the launcher's work folder (your game files are only read, never changed) and starts the game "
        "with the mod. The mod uses its own save file, so your normal save is not used while you play with it.\n\n"
        "Backup: copies your save files (the normal save and the mod's own save, if they exist) into a new "
        "folder backups/<date_time> next to the launcher. It only copies: nothing is deleted or overwritten.\n\n"
        "Uninstall: removes the mod's prepared files from the work folder and the mod's own save file. "
        "Your normal save and the backups stay.\n\n"
        "Debug log: the mod writes a short log by default; with this box ticked it writes everything it does "
        "(much longer), which helps when something goes wrong. The choice is remembered and applies from the "
        "next \"Play with the mod\". The log is the file work/native/invasion.log in the launcher's folder: "
        "when you report a problem, send that file.\n\n"
        "Language: the language of this window. Your choice is remembered.\n\n"
        "Close: closes this window.",
        "Debug log"
    },
    [L_PL] = {
        "Nightreign Invasions",
        "Folder gry (ten z plikiem nightreign.exe):",
        "Wybierz...",
        "Graj z modem",
        "Kopia zapasowa",
        "Pomoc",
        "Odinstaluj",
        "Zamknij",
        "Język",
        "Nie znaleziono gry automatycznie. Wskaż jej folder przyciskiem Wybierz.",
        "Znaleziono grę.",
        "Wskaż folder \"Game\" gry",
        "W tym folderze nie ma plików nightreign.exe i data0.bhd.",
        "Usunąć pliki moda i jego osobny zapis gry? Zwykły zapis i kopie zapasowe zostają.",
        "Nie znaleziono pliku zapisu; nic nie skopiowano.",
        "Kopia zapasowa zrobiona w %s (obok launchera). Niczego nie zmieniono ani nie usunięto.",
        "Nie udało się zrobić kopii zapasowej; niczego nie zmieniono ani nie usunięto.",
        "Wybierz: ręczne wskazanie folderu gry (tego z nightreign.exe), jeśli nie został znaleziony.\n\n"
        "Graj z modem: najpierw robi kopię zapasową zapisów (jak Kopia zapasowa), potem przygotowuje pliki moda "
        "w folderze roboczym launchera (pliki gry są tylko czytane, nigdy zmieniane) i uruchamia grę z modem. "
        "Mod używa własnego pliku zapisu, więc zwykły zapis nie jest używany podczas gry z modem.\n\n"
        "Kopia zapasowa: kopiuje pliki zapisu (zwykły zapis i osobny zapis moda, jeśli istnieją) do nowego "
        "folderu backups/<data_godzina> obok launchera. Tylko kopiuje: niczego nie usuwa ani nie nadpisuje.\n\n"
        "Odinstaluj: usuwa przygotowane pliki moda z folderu roboczego i osobny zapis moda. "
        "Zwykły zapis i kopie zapasowe zostają.\n\n"
        "Szczegółowy log: domyślnie mod zapisuje krótki log; z zaznaczonym polem zapisuje wszystko, co robi "
        "(dużo dłuższy), co pomaga, gdy coś nie działa. Wybór jest zapamiętywany i działa od następnego "
        "\"Graj z modem\". Log to plik work/native/invasion.log w folderze launchera: zgłaszając problem, "
        "wyślij ten plik.\n\n"
        "Język: język tego okna. Wybór jest zapamiętywany.\n\n"
        "Zamknij: zamyka to okno.",
        "Szczegółowy log"
    },
    [L_DE] = {
        "Nightreign Invasions",
        "Spielordner (der mit nightreign.exe):",
        "Durchsuchen...",
        "Mit dem Mod spielen",
        "Sicherung",
        "Hilfe",
        "Deinstallieren",
        "Schließen",
        "Sprache",
        "Das Spiel wurde nicht automatisch gefunden. Wähle seinen Ordner mit Durchsuchen.",
        "Spiel gefunden.",
        "Wähle den Ordner \"Game\" des Spiels",
        "Dieser Ordner enthält nightreign.exe und data0.bhd nicht.",
        "Die Dateien des Mods und seinen eigenen Spielstand entfernen? Dein normaler Spielstand und die "
        "Sicherungen bleiben.",
        "Kein Spielstand gefunden; nichts wurde gesichert.",
        "Sicherung erstellt in %s (neben dem Launcher). Nichts wurde geändert oder gelöscht.",
        "Die Sicherung konnte nicht erstellt werden; nichts wurde geändert oder gelöscht.",
        "Durchsuchen: den Spielordner (den mit nightreign.exe) von Hand wählen, falls er nicht gefunden wurde.\n\n"
        "Mit dem Mod spielen: sichert zuerst deine Spielstände (wie Sicherung), bereitet dann die Dateien des "
        "Mods im Arbeitsordner des Launchers vor (deine Spieldateien werden nur gelesen, nie verändert) und "
        "startet das Spiel mit dem Mod. Der Mod nutzt einen eigenen Spielstand, dein normaler Spielstand wird "
        "dabei nicht verwendet.\n\n"
        "Sicherung: kopiert deine Spielstände (den normalen und den eigenen des Mods, falls vorhanden) in einen "
        "neuen Ordner backups/<Datum_Uhrzeit> neben dem Launcher. Es wird nur kopiert: nichts wird gelöscht "
        "oder überschrieben.\n\n"
        "Deinstallieren: entfernt die vorbereiteten Dateien des Mods aus dem Arbeitsordner und den eigenen "
        "Spielstand des Mods. Dein normaler Spielstand und die Sicherungen bleiben.\n\n"
        "Ausführliches Protokoll: standardmäßig schreibt der Mod ein kurzes Protokoll; mit diesem Häkchen schreibt "
        "er alles, was er tut (viel länger), was bei Problemen hilft. Die Wahl wird gespeichert und gilt ab dem "
        "nächsten \"Mit dem Mod spielen\". Das Protokoll ist die Datei work/native/invasion.log im Ordner des "
        "Launchers: wenn du ein Problem meldest, schicke diese Datei.\n\n"
        "Sprache: die Sprache dieses Fensters. Die Wahl wird gespeichert.\n\n"
        "Schließen: schließt dieses Fenster.",
        "Ausführliches Protokoll"
    },
    [L_FR] = {
        "Nightreign Invasions",
        "Dossier du jeu (celui qui contient nightreign.exe) :",
        "Parcourir...",
        "Jouer avec le mod",
        "Sauvegarde",
        "Aide",
        "Désinstaller",
        "Fermer",
        "Langue",
        "Le jeu n'a pas été trouvé automatiquement. Choisissez son dossier avec Parcourir.",
        "Jeu trouvé.",
        "Choisissez le dossier \"Game\" du jeu",
        "Ce dossier ne contient pas nightreign.exe et data0.bhd.",
        "Supprimer les fichiers du mod et sa sauvegarde séparée ? Votre sauvegarde normale et les copies de "
        "sécurité restent.",
        "Aucun fichier de sauvegarde trouvé ; rien n'a été copié.",
        "Copie de sécurité faite dans %s (à côté du lanceur). Rien n'a été modifié ni supprimé.",
        "La copie de sécurité n'a pas pu être faite ; rien n'a été modifié ni supprimé.",
        "Parcourir : choisir à la main le dossier du jeu (celui qui contient nightreign.exe), s'il n'a pas été "
        "trouvé.\n\n"
        "Jouer avec le mod : fait d'abord une copie de vos sauvegardes (comme Sauvegarde), prépare ensuite les "
        "fichiers du mod dans le dossier de travail du lanceur (vos fichiers de jeu sont seulement lus, jamais "
        "modifiés) et lance le jeu avec le mod. Le mod utilise sa propre sauvegarde ; votre sauvegarde normale "
        "n'est pas utilisée pendant que vous jouez avec lui.\n\n"
        "Sauvegarde : copie vos fichiers de sauvegarde (la sauvegarde normale et celle du mod, s'ils existent) "
        "dans un nouveau dossier backups/<date_heure> à côté du lanceur. Elle ne fait que copier : rien n'est "
        "supprimé ni écrasé.\n\n"
        "Désinstaller : supprime les fichiers préparés du mod dans le dossier de travail et la sauvegarde propre "
        "au mod. Votre sauvegarde normale et les copies de sécurité restent.\n\n"
        "Journal détaillé : par défaut le mod écrit un journal court ; avec cette case cochée il écrit tout ce "
        "qu'il fait (beaucoup plus long), ce qui aide en cas de problème. Le choix est mémorisé et s'applique à "
        "partir du prochain \"Jouer avec le mod\". Le journal est le fichier work/native/invasion.log dans le "
        "dossier du lanceur : pour signaler un problème, envoyez ce fichier.\n\n"
        "Langue : la langue de cette fenêtre. Le choix est mémorisé.\n\n"
        "Fermer : ferme cette fenêtre.",
        "Journal détaillé"
    },
    [L_ES] = {
        "Nightreign Invasions",
        "Carpeta del juego (la que contiene nightreign.exe):",
        "Examinar...",
        "Jugar con el mod",
        "Copia de seguridad",
        "Ayuda",
        "Desinstalar",
        "Cerrar",
        "Idioma",
        "No se encontró el juego automáticamente. Elige su carpeta con Examinar.",
        "Juego encontrado.",
        "Elige la carpeta \"Game\" del juego",
        "Esta carpeta no contiene nightreign.exe ni data0.bhd.",
        "¿Quitar los archivos del mod y su partida guardada propia? Tu partida normal y las copias de "
        "seguridad se conservan.",
        "No se encontró ninguna partida guardada; no se copió nada.",
        "Copia de seguridad hecha en %s (junto al lanzador). No se cambió ni se borró nada.",
        "No se pudo hacer la copia de seguridad; no se cambió ni se borró nada.",
        "Examinar: elegir a mano la carpeta del juego (la que contiene nightreign.exe), si no se encontró.\n\n"
        "Jugar con el mod: primero hace una copia de seguridad de tus partidas (como Copia de seguridad), luego "
        "prepara los archivos del mod en la carpeta de trabajo del lanzador (tus archivos del juego solo se "
        "leen, nunca se cambian) e inicia el juego con el mod. El mod usa su propia partida guardada; tu "
        "partida normal no se usa mientras juegas con él.\n\n"
        "Copia de seguridad: copia tus partidas guardadas (la normal y la propia del mod, si existen) en una "
        "carpeta nueva backups/<fecha_hora> junto al lanzador. Solo copia: no borra ni sobrescribe nada.\n\n"
        "Desinstalar: quita los archivos preparados del mod de la carpeta de trabajo y la partida propia del "
        "mod. Tu partida normal y las copias de seguridad se conservan.\n\n"
        "Registro detallado: por defecto el mod escribe un registro corto; con esta casilla marcada escribe todo "
        "lo que hace (mucho más largo), lo que ayuda cuando algo falla. La elección se recuerda y se aplica desde "
        "el siguiente \"Jugar con el mod\". El registro es el archivo work/native/invasion.log en la carpeta "
        "del lanzador: al informar de un problema, envía ese archivo.\n\n"
        "Idioma: el idioma de esta ventana. La elección se recuerda.\n\n"
        "Cerrar: cierra esta ventana.",
        "Registro detallado"
    },
};

// A language code ("pl_PL.UTF-8", "de", ...) to a table index; English when it is none of the five.
static int ui_lang_from_code(const char *code) {
    for (int i = 0; code && i < LANG_COUNT; i++)
        if (!strncmp(code, LANG_CODE[i], 2)) return i;
    return L_EN;
}

#endif

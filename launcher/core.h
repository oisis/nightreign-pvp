// Nightreign invasion mod launcher: everything except the window. Shared by the Windows and Linux
// front ends and by the command-line build used for testing.
#ifndef CORE_H
#define CORE_H

#include <stddef.h>

#define APP_NAME "Nightreign Invasions"
#define STEAM_APP_ID "2622380"
#define MOD_SAVE_NAME "NR0000-invasion.sl2"

// Progress and error text goes through this callback (one line per call, UTF-8, no newline).
typedef void (*log_fn)(const char *line);
void core_set_log(log_fn fn);

// Directory holding the launcher and its data/ and me3/ folders. Must be set first.
void core_set_app_dir(const char *dir);

// Find the game's "Game" folder through Steam. Returns 1 and fills out on success.
int core_find_game(char *out, size_t cap);
// 1 if dir looks like Nightreign's Game folder (has nightreign.exe and data0.bhd).
int core_check_game(const char *dir);
// Find the folder with the player's save file (NR0000.sl2). game_dir helps under Proton.
int core_find_save_dir(const char *game_dir, char *out, size_t cap);

// Copy the save files into <app>/work/backups/<timestamp>/. Returns 1 on success.
int core_backup_save(const char *save_dir);
// Build the mod's files from the player's own game files into <app>/work/build and write the
// me3 profile. Returns 1 on success.
int core_build(const char *game_dir);
// Start the game through me3 with the mod. Returns 1 if the launcher process was started.
int core_launch(void);
// Remove everything core_build created, and the mod's own save file if save_dir is given.
// The vanilla save and the backups are left alone. Returns 1 on success.
int core_uninstall(const char *save_dir);

// Runs the built-in checks (RSA, AES, containers). Returns 1 if all pass.
int core_selftest(void);

#endif

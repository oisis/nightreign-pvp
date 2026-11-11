// Command-line front end for the launcher core, used for testing:
//   cli <app dir> selftest | find | backup | build [game dir] | launch | uninstall
#include "core.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) return fprintf(stderr, "usage: cli <app dir> selftest|find|backup|build|launch|uninstall [game dir]\n"), 2;
    core_set_app_dir(argv[1]);
    char game[1024] = "", save[1024] = "";
    if (argc > 3) snprintf(game, sizeof game, "%s", argv[3]);
    else core_find_game(game, sizeof game);
    if (*game) core_find_save_dir(game, save, sizeof save);
    const char *cmd = argv[2];
    if (!strcmp(cmd, "selftest")) return !core_selftest();
    if (!strcmp(cmd, "find")) return printf("game: %s\nsave: %s\n", game, save), !*game;
    if (!strcmp(cmd, "backup")) return !core_backup_save(save);
    if (!strcmp(cmd, "build")) return !core_build(game);
    if (!strcmp(cmd, "launch")) return !core_launch();
    if (!strcmp(cmd, "uninstall")) return !core_uninstall(save);
    return 2;
}

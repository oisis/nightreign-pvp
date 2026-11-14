// Dry check of map-dump file handling (mapdump.h), no game needed:
//   zig cc -target x86_64-windows-gnu -O1 -o t.exe test_mapdump.c && wine t.exe <empty dir>
// Order as in run 32: hub layout, expedition layout, flag writes before InGame, open, writes, a leaving
// layout (must not touch the file), close, next expedition in its own file.
#include <stdlib.h>
#include "mapdump.h"
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static md_state s;
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}
int main(int argc, char **argv) {
    strcpy(s.dir, argc > 1 ? argv[1] : ".");
    md_layout_begin(&s), md_layout_print(&s, "== layout 0 (hub)\n");
    md_print(&s, "hub write\n");
    md_layout_begin(&s), md_layout_print(&s, "== layout 1 (expedition)\n");
    md_print(&s, "load write\n");
    CHECK(md_open(&s, 21, 17, 14, 49));
    md_print(&s, "ingame write\n");
    md_layout_begin(&s), md_layout_print(&s, "== layout 0 (leaving)\n");  // must not touch the file
    md_print(&s, "after leaving layout\n");
    md_close(&s, "expedition over");
    md_print(&s, "hub write 2\n");
    md_layout_begin(&s), md_layout_print(&s, "== layout 1 (expedition 2)\n");
    CHECK(md_open(&s, 31, 17, 30, 0));
    md_close(&s, "expedition over");

    char p1[400], p2[400];
    snprintf(p1, sizeof p1, "%s\\map-dump-p21-171449.txt", s.dir);
    snprintf(p2, sizeof p2, "%s\\map-dump-p31-173000.txt", s.dir);
    char *a = slurp(p1);
    CHECK(strstr(a, "== layout 1 (expedition)\nload write\ningame write\n"));
    CHECK(strstr(a, "after leaving layout\n") && strstr(a, "== file closed: expedition over"));
    CHECK(!strstr(a, "hub write") && !strstr(a, "layout 0") && !strstr(a, "expedition 2"));
    char *b = slurp(p2);
    CHECK(strstr(b, "== layout 1 (expedition 2)\n") && !strstr(b, "hub write") && !strstr(b, "ingame write"));
    puts("mapdump test ok");
    return 0;
}

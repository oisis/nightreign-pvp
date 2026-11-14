// map-dump file handling (no game access; test: mod/dll/test_mapdump.c). One file per expedition,
// map-dump-p<pattern>-<hhmmss>.txt next to the DLL, opened at the expedition's first InGame frame, appended
// and flushed as it goes, closed when the expedition ends. Before it is open, lines go to a buffer that is
// written at the top of the file; the last flag layout's text is kept apart and written first. A layout or
// flag write while no expedition file is open never touches a file (the hub's and the leaving layouts).
#ifndef NRPVP_MAPDUMP_H
#define NRPVP_MAPDUMP_H
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MD_PRE_MAX (1 << 20)
#define MD_LAYOUT_MAX (1 << 15)
typedef struct {
    char dir[260], name[300];
    FILE *f;
    char pre[MD_PRE_MAX];
    size_t pre_n, pre_lost;
    char layout[MD_LAYOUT_MAX];
    size_t layout_n;
} md_state;

static void md_vadd(char *buf, size_t cap, size_t *n, size_t *lost, const char *fmt, va_list ap) {
    if (*n + 1 >= cap) { if (lost) (*lost)++; return; }
    int w = vsnprintf(buf + *n, cap - *n, fmt, ap);
    if (w < 0) return;
    if (*n + (size_t)w >= cap) { buf[*n] = 0; if (lost) (*lost)++; return; }
    *n += (size_t)w;
}
// A line of the dump: to the open file, or to the buffer until it is opened.
static void md_print(md_state *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s->f) vfprintf(s->f, fmt, ap);
    else md_vadd(s->pre, sizeof s->pre, &s->pre_n, &s->pre_lost, fmt, ap);
    va_end(ap);
}
// A new flag layout: its text replaces the kept one; while no file is open the buffer restarts with it.
static void md_layout_begin(md_state *s) {
    s->layout_n = 0, s->layout[0] = 0;
    if (!s->f) s->pre_n = 0, s->pre_lost = 0, s->pre[0] = 0;
}
static void md_layout_print(md_state *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    md_vadd(s->layout, sizeof s->layout, &s->layout_n, NULL, fmt, ap);
    va_end(ap);
}
static int md_open(md_state *s, int pattern, int hh, int mm, int ss) {
    if (s->f) return 1;
    snprintf(s->name, sizeof s->name, "%s%smap-dump-p%d-%02d%02d%02d.txt", s->dir, s->dir[0] ? "\\" : "", pattern, hh, mm, ss);
    s->f = fopen(s->name, "ab");  // binary: lines end in \n, as written
    if (!s->f) return 0;
    fputs(s->layout, s->f);
    fwrite(s->pre, 1, s->pre_n, s->f);
    if (s->pre_lost) fprintf(s->f, "== %zu lines before the start did not fit the buffer\n", s->pre_lost);
    s->pre_n = 0, s->pre_lost = 0, s->pre[0] = 0;
    fflush(s->f);
    return 1;
}
static void md_flush(md_state *s) { if (s->f) fflush(s->f); }
static void md_close(md_state *s, const char *why) {
    if (!s->f) return;
    fprintf(s->f, "== file closed: %s\n", why);
    fclose(s->f);
    s->f = NULL;
    s->pre_n = 0, s->pre[0] = 0;
}
#endif

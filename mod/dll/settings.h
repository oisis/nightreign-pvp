// Test settings (test phase only), shared by the DLL and the launcher. Text of "key = true|false"
// lines: spaces, tabs, CRLF, "#" comments (whole line or after the value) and TRUE/False in any case
// are fine. Anything else on a line is reported back, not used.
#ifndef NRPVP_SETTINGS_H
#define NRPVP_SETTINGS_H
#include <stddef.h>
#include <string.h>

enum { S_HOST_TEST, S_JOIN_TEST, S_ENTER_TEST, S_MOVE_TEST, S_TEAM_TEST, S_NO_RAIN, S_SLOW_TIME, S_FULL_SYNC, S_FINISH_TEST, S_LOOT_TEST, S_WATCH_TEST, S_NAME_TEST, S_REWARD_TEST, S_MARKER_TEST, SETTINGS_COUNT };
static const char *const SETTING_NAMES[SETTINGS_COUNT] = {"host-test", "join-test", "enter-test", "move-test",
                                                          "team-test", "no-rain",   "slow-time", "full-sync",
                                                          "finish-test", "loot-test", "watch-test", "name-test", "reward-test", "marker-test"};
// Settings that are on when the file does not mention them.
static int setting_default(int i) { return i == S_FULL_SYNC; }

static int settings_word(const char *s, size_t n, const char *w) {  // case-insensitive, whole word
    size_t i = 0;
    for (; i < n && w[i]; i++)
        if ((s[i] | 0x20) != (w[i] | 0x20)) return 0;
    return i == n && !w[i];
}

// v[i]: 1 or 0 for a key found, -1 for a key not in the text. Lines it cannot use go into bad
// (", "-separated, cut to cap). Returns how many there were.
static int settings_parse(const char *t, size_t n, signed char v[SETTINGS_COUNT], char *bad, size_t cap) {
    int nbad = 0;
    for (int i = 0; i < SETTINGS_COUNT; i++) v[i] = -1;
    if (cap) bad[0] = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && t[end] != '\n') end++;
        const char *s = t + at, *e = t + end;
        at = end + 1;
        for (const char *c = s; c < e; c++)
            if (*c == '#') { e = c; break; }
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        if (s == e) continue;
        const char *eq = memchr(s, '=', (size_t)(e - s));
        int key = -1, val = -1;
        if (eq) {
            const char *ke = eq, *vs = eq + 1;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
            for (int i = 0; i < SETTINGS_COUNT; i++)
                if (settings_word(s, (size_t)(ke - s), SETTING_NAMES[i])) key = i;
            val = settings_word(vs, (size_t)(e - vs), "true") ? 1 : settings_word(vs, (size_t)(e - vs), "false") ? 0 : -1;
        }
        if (key >= 0 && val >= 0) {
            v[key] = (signed char)val;
            continue;
        }
        nbad++;
        size_t have = cap ? strlen(bad) : 0, len = (size_t)(e - s);
        if (have + 3 < cap) {
            if (len > cap - have - 3) len = cap - have - 3;
            if (have) memcpy(bad + have, ", ", 2), have += 2;
            memcpy(bad + have, s, len);
            bad[have + len] = 0;
        }
    }
    return nbad;
}
#endif

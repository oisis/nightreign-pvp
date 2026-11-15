// Test settings (test phase only), shared by the DLL and the launcher. Text of "key = true|false"
// lines: spaces, tabs, CRLF, "#" comments (whole line or after the value) and TRUE/False in any case
// are fine. Anything else on a line is reported back, not used.
#ifndef NRPVP_SETTINGS_H
#define NRPVP_SETTINGS_H
#include <stddef.h>
#include <string.h>

enum { S_TEAM_TEST, S_SLOW_TIME, S_FULL_SYNC, S_LOOT_TEST, S_DEBUG_LOG, S_NO_HOST, S_TEST_LEVEL, S_FLAG_LOG, S_MAP_DUMP, S_MAP_GROUP, S_ONE_REGION, S_BOSS_FLOW, S_REVIVE_HITS, S_NO_SPRINT, S_ULT_PVP, S_PAIR, S_ALLY, S_PARTY, SETTINGS_COUNT };
static const char *const SETTING_NAMES[SETTINGS_COUNT] = {"team-test", "slow-time", "full-sync", "loot-test",
                                                          "debug-log", "no-host", "test-level", "flag-log",
                                                          "map-dump", "map-group", "one-region", "boss-flow", "revive-hits", "no-sprint", "ult-pvp", "pair", "ally", "party"};
// Keys of settings that became fixed behaviour or were dropped; old files may still have them: skipped silently.
// (no-rain: decision 27, the rain outside the circle never does damage. two-regions, map-test-control, map-region,
// map-pair-a / -b: every map is a Mountaintop + Rotted Woods map now (decision 35). no-circle: to be redone.)
static const char *const SETTING_RETIRED[] = {"no-rain", "two-regions", "map-test-control", "map-region", "map-pair-a", "map-pair-b", "no-circle"};
// Settings that take a whole number 0..999 instead of true / false (0 = off).
static int setting_is_number(int i) { return i == S_TEST_LEVEL || i == S_MAP_GROUP || i == S_ONE_REGION || i == S_BOSS_FLOW || i == S_ULT_PVP || i == S_PAIR || i == S_PARTY; }
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
static int settings_parse(const char *t, size_t n, short v[SETTINGS_COUNT], char *bad, size_t cap) {
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
            if (key >= 0 && setting_is_number(key)) {  // digits only, 0..999
                val = e - vs >= 1 && e - vs <= 3 ? 0 : -1;
                for (const char *c = vs; val >= 0 && c < e; c++) val = *c >= '0' && *c <= '9' ? val * 10 + (*c - '0') : -1;
            }
        }
        if (key >= 0 && val >= 0) {
            v[key] = (short)val;
            continue;
        }
        int retired = 0;
        for (size_t i = 0; eq && i < sizeof SETTING_RETIRED / sizeof *SETTING_RETIRED; i++) {
            const char *ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if (settings_word(s, (size_t)(ke - s), SETTING_RETIRED[i])) retired = 1;
        }
        if (retired) continue;
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

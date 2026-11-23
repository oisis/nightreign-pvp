// Test settings (test phase only), shared by the DLL and the launcher. Text of "key = true|false"
// lines: spaces, tabs, CRLF, "#" comments (whole line or after the value) and TRUE/False in any case
// are fine. Anything else on a line is reported back, not used.
#ifndef NRPVP_SETTINGS_H
#define NRPVP_SETTINGS_H
#include <stddef.h>
#include <string.h>

enum { S_LOOT_TEST, S_DEBUG_LOG, S_NO_HOST, S_TEST_LEVEL, S_FLAG_LOG, S_MAP_DUMP, S_MAP_GROUP, S_ONE_REGION, S_BOSS_FLOW, S_REVIVE_HITS, S_ULT_PVP, S_PARTY, S_SLOT_TEST,
       S_VOICE, S_VOICE_KEY, S_VOICE_TEAM_KEY, S_VOICE_RANGE, S_VOICE_VOLUME, S_VOICE_PAD, S_VOICE_TEAM_PAD, S_VOICE_LOOPBACK,
       S_VOICE_LEVEL, S_VOICE_DUCK, S_VOICE_DUCK_AMOUNT, SETTINGS_COUNT };
static const char *const SETTING_NAMES[SETTINGS_COUNT] = {"loot-test",
                                                          "debug-log", "no-host", "test-level", "flag-log",
                                                          "map-dump", "map-group", "one-region", "boss-flow", "revive-hits", "ult-pvp", "party", "slot-test",
                                                          "voice", "voice-key", "voice-team-key", "voice-range", "voice-volume",
                                                          "voice-pad", "voice-team-pad", "voice-loopback",
                                                          "voice-level", "voice-duck", "voice-duck-amount"};
// Settings whose value is free text (not true / false or a number): read with settings_text, skipped by settings_parse.
// voice-mute: players never heard, Steam names or 17-digit Steam ids, separated by commas.
static const char *const SETTING_TEXT[] = {"voice-mute"};
// Keys of settings that became fixed behaviour or were dropped; old files may still have them: skipped silently.
// (no-rain: decision 27, the rain outside the circle never does damage. two-regions, map-test-control, map-region,
// map-pair-a / -b: every map is a Mountaintop + Rotted Woods map now (decision 35). no-circle: to be redone. ally: the
// menu's "team" choice joins as an ally now. challenge-bosses: the challenge always needs 6 bosses and the special one.
// map / pair: the five maps are all in every build and the pattern decides (owner, 2026-11-21). variant-test, pack-test:
// their measurements became the pairs' far halves and map screen packs.)
static const char *const SETTING_RETIRED[] = {"pair", "map", "variant-test", "pack-test", "team-test", "slow-time", "full-sync", "no-sprint", "show-ping", "show-counts",
                                               "rain-hooks-legacy", "ally", "no-rain", "two-regions", "map-test-control", "map-region", "map-pair-a", "map-pair-b", "no-circle",
                                               "challenge-bosses"};
// Settings that take a whole number 0..999 instead of true / false (0 = off).
static int setting_is_number(int i) { return i == S_TEST_LEVEL || i == S_MAP_GROUP || i == S_ONE_REGION || i == S_BOSS_FLOW || i == S_ULT_PVP || i == S_PARTY ||
                                         (i >= S_VOICE_KEY && i <= S_VOICE_TEAM_PAD) || i == S_VOICE_LEVEL || i == S_VOICE_DUCK_AMOUNT; }
// Settings that are on when the file does not mention them.
// boss-flow: 4, the day flow. Voice: push-to-talk V (Windows virtual-key 0x56), team B (0x42), 40 m, volume 100 %;
// voice-level: the AGC's speech target in dB below full scale (10 "loud", 18 "normal"); voice-duck on, the game's sound
// down by voice-duck-amount 50 % while someone is heard.
static int setting_default(int i) {
    return i == S_BOSS_FLOW ? 4 : i == S_VOICE_KEY ? 0x56 : i == S_VOICE_TEAM_KEY ? 0x42 : i == S_VOICE_RANGE ? 40 : i == S_VOICE_VOLUME ? 100 :
           i == S_VOICE_LEVEL ? 10 : i == S_VOICE_DUCK ? 1 : i == S_VOICE_DUCK_AMOUNT ? 50 : 0;
}

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
        if (key == S_BOSS_FLOW && val != 0 && val != 4) val = -1;  // flows 1..3 retired: only 0 (the game's clock) and 4
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
        for (size_t i = 0; eq && i < sizeof SETTING_TEXT / sizeof *SETTING_TEXT; i++) {
            const char *ke = eq;
            while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if (settings_word(s, (size_t)(ke - s), SETTING_TEXT[i])) retired = 1;
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
// The value of text setting key (the last line with it; "#" ends it, spaces around cut) into out. 1 if found.
static int settings_text(const char *t, size_t n, const char *key, char *out, size_t cap) {
    int found = 0;
    if (cap) out[0] = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && t[end] != '\n') end++;
        const char *s = t + at, *e = t + end;
        at = end + 1;
        for (const char *c = s; c < e; c++)
            if (*c == '#') { e = c; break; }
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        const char *eq = memchr(s, '=', (size_t)(e - s)), *ke = eq, *vs = eq ? eq + 1 : NULL;
        if (!eq) continue;
        while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
        if (!settings_word(s, (size_t)(ke - s), key) || !cap) continue;
        size_t len = (size_t)(e - vs) < cap - 1 ? (size_t)(e - vs) : cap - 1;
        memcpy(out, vs, len), out[len] = 0, found = 1;
    }
    return found;
}
#endif

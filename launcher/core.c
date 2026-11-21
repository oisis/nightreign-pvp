// Nightreign invasion mod launcher core. See core.h.
//
// What it does when building: reads the player's own game files (the data0 archive and
// regulation.bin), adds the mod's menu lines, its Target list row and its two icons, and writes the
// results into work/build, which me3 then overlays on the game. Nothing in the game folder is modified.
//
// Format knowledge and the archive's public key come from the open-source Smithbox/SoulsFormats.
#define _FILE_OFFSET_BITS 64
#include "core.h"
#include "data0_key.h"
#include "data2_key.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define SEP "\\"
#define fseek64 _fseeki64
#else
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define SEP "/"
#define fseek64 fseeko
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { u8 *p; size_t n; } buf;

#define TEXT_TARGET_FMG "\\CL_MenuText.fmg"
#define BUILD_FORMAT 40
#define BOSS_DEATHS_MIN 100  // the game (1.3.3) has 158 such flags; fewer = its event scripts changed

static const char *LANGS[] = {"araae", "deude", "engus", "frafr", "itait", "jpnjp", "korkr", "polpl",
                              "porbr", "rusru", "spaar", "spaes", "thath", "zhocn", "zhotw"};
// Our own menu lines (UTF-8): the "Number of Players" option's entries and the "Invasion" entry of the
// expedition menu's Target list.
// Polish for the Polish game, English for every other language. Keep in sync with mod/build_msg.py.
// (Adjacent string literals keep \x escapes from swallowing the letters that follow them.)
static const struct { u32 id; const char *english, *polish; } OUR_TEXT[] = {
    // "Number of Players" (the DLL: 2 Solo, no session; 1 the game's solo start plus our host session; 0 party)
    {130305, "Solo + invaders", "Solo + naje\xc5\xba" "d\xc5\xba" "cy"},  // the start button for value 1
    {130408, "Team", "Dru\xc5\xbcyna"},  // the game's "three players" (value 0)
    {130409, "Solo", "Solo"},  // the game's "one player" (value 2)
    {130417, "Solo + invaders", "Solo + naje\xc5\xba" "d\xc5\xba" "cy"},  // the game's "two players" (value 1)
    {131170, "A newer mod version (%s) is available. Update to play with others.",  // %s: the DLL puts the number in
             "Dost\xc4\x99pna jest nowsza wersja moda (%s). Zaktualizuj, \xc5\xbc" "eby gra\xc4\x87 z innymi."},
    {131171, "Your team (same password) plays mod version %s. Everyone needs the same version.",
             "Twoja dru\xc5\xbcyna (to samo has\xc5\x82o) gra na wersji moda %s. Wszyscy potrzebuj\xc4\x85 tej samej wersji."},
    {131172, "Squads in an expedition: %d", "Dru\xc5\xbcyny w wyprawie: %d"},  // counts overlay (%d: the DLL)
    {131173, "Squads looking for a player: %d", "Dru\xc5\xbcyny szukaj\xc4\x85" "ce gracza: %d"},
    {131174, "Hosts waiting in the hub: %d", "Hosty czekaj\xc4\x85" "ce w Twierdzy: %d"},
    {131160, "Invasion", "Najazd"},
    {131161, "Enter another squad's expedition as an invader.\nDefeat its host to win.",
             "Wejd\xc5\xba do wyprawy innej dru\xc5\xbcyny jako naje\xc5\xba" "d\xc5\xba" "ca.\n"
             "Pokonaj hosta, aby wygra\xc4\x87."},
    {131162, "Searching for an expedition to invade...", "Szukanie wyprawy do najechania..."},
    {131163, "Invasion search cancelled", "Anulowano szukanie najazdu"},
    {131164, "Cancel invasion", "Anuluj najazd"},
    {131166, "Begin invasion", "Rozpocznij najazd"},
    {131167, "Waiting for the squad's host...", "Oczekiwanie na hosta dru\xc5\xbcyny..."},
    {131168, "The invader has left", "Naje\xc5\xba" "d\xc5\xba" "ca odszed\xc5\x82"},
    {131169, "You have invaded another squad's expedition!", "Wkroczy\xc5\x82" "e\xc5\x9b do wyprawy innej dru\xc5\xbcyny!"},
    // day flow (boss-flow 4): the challenge's state
    // 131180 / 131181 / 131183: per pair (pair_texts)
    {131182, "Challenge available: use Challenge the Night to face the night boss",
             "Wyzwanie dost\xc4\x99pne: u\xc5\xbcyj Staw wyzwanie, by stan\xc4\x85\xc4\x87 przed bossem nocy"},
    {131184, "The challenge is taken: the night falls", "Wyzwanie podj\xc4\x99te: zapada noc"},
};
#define OUR_TEXT_COUNT (sizeof OUR_TEXT / sizeof *OUR_TEXT)
// Our item's texts (item_dlc01.msgbnd: GoodsName / GoodsInfo / GoodsCaption, id = the goods id).
#define CHALLENGE_GOODS 1291
static const struct { const char *fmg, *english, *polish; } ITEM_TEXT[] = {
    {"\\GoodsName.fmg", "Challenge the Night", "Staw wyzwanie"},
    {"\\GoodsInfo.fmg", "Takes the whole squad to the night boss", "Przenosi ca\xc5\x82\xc4\x85 dru\xc5\xbcyn\xc4\x99 do bossa nocy"},
    {"\\GoodsCaption.fmg",
     "Two crossed blades, a call to the Night.\n\nUsable once the squad has defeated six bosses and the special boss of a region "
     "(the Mountaintop's dragon or the Rotted Woods' Captain); on day 2, six bosses (or all those left, if fewer) and both special "
     "bosses. Takes the whole squad to the night boss's arena, and the night "
     "falls.\n\nUsed too early, it stays in the pouch.",
     "Dwa skrzy\xc5\xbcowane ostrza, wezwanie Nocy.\n\nMo\xc5\xbcna u\xc5\xbcy\xc4\x87, gdy dru\xc5\xbcyna pokona sze\xc5\x9b" "ciu boss\xc3\xb3w "
     "i bossa specjalnego regionu (smoka Szczytu albo Kapitana Zgni\xc5\x82" "ego Lasu); drugiego dnia sze\xc5\x9b" "ciu boss\xc3\xb3w (albo wszystkich pozosta\xc5\x82ych, je\xc5\x9bli jest ich mniej) i obu "
     "boss\xc3\xb3w specjalnych. Przenosi ca\xc5\x82\xc4\x85 dru\xc5\xbcyn\xc4\x99 "
     "na aren\xc4\x99 bossa nocy i zapada noc.\n\nU\xc5\xbcyte za wcze\xc5\x9bnie zostaje w sakwie."},
};

// The four region pairs (owner, 2026-11-19), one drawn at each launcher start (read_settings): their names, and the
// special boss of the challenge, one per region (owner, 2026-11-19: the Mountaintop's dragon, the Rotted Woods'
// Captain, the Crater's Magma Wyrm, Noklateo's Naturalborn of the Void; DLL df_special), English / Polish.
static const char *const PAIR_NAME[4][2] = {
    {"Frozen Thicket", "Zamarzni\xc4\x99ty G\xc4\x85szcz"},
    {"City Beneath the Peak", "Miasto pod Szczytem"},
    {"Ash and Rot", "Popi\xc3\xb3\xc5\x82 i Zgnilizna"},
    {"Fire over the City", "Ogie\xc5\x84 nad Miastem"},
};
static const char *const PAIR_SPECIAL[4][2] = {
    {"the dragon or the Captain", "smoka lub Kapitana"},
    {"the dragon or the Naturalborn of the Void", "smoka lub Naturalborn of the Void"},
    {"the Magma Wyrm or the Captain", "Magmowego Wyrma lub Kapitana"},
    {"the Magma Wyrm or the Naturalborn of the Void", "Magmowego Wyrma lub Naturalborn of the Void"},
};
static const char *const PAIR_SPECIAL_BOTH[4][2] = {  // day 2 needs both (owner, 2026-11-21)
    {"the dragon and the Captain", "smoka i Kapitana"},
    {"the dragon and the Naturalborn of the Void", "smoka i Naturalborn of the Void"},
    {"the Magma Wyrm and the Captain", "Magmowego Wyrma i Kapitana"},
    {"the Magma Wyrm and the Naturalborn of the Void", "Magmowego Wyrma i Naturalborn of the Void"},
};
static int g_pair_given;  // test-settings.toml names the map (map = 1..4); otherwise it is drawn
static int pair_now(void);
// Texts that depend on the pair: 131183 (the special boss); 131180 / 131181 (kill notices, here with them), 131185 (the map's name, at the expedition start),
// 131186 + k (a team with our password plays pair k), 131191 (131183 on day 2), the item's caption. (id, text) for the
// language, n of them.
static int pair_texts(int polish, u32 *ids, char text[][400]) {  // up to 10
    int p = pair_now(), l = polish != 0, n = 0;
    const char *sp = PAIR_SPECIAL[p][l];
    // 131180: %s = "M/M", a special boss still needed; 131181: %s = "N/M", M the day's target (owner, 2026-11-19 /
    // 2026-11-21: 6 on day 1, on day 2 min(6, ordinary bosses alive at its start)); 131183 / 131191: %s = M
    ids[n] = 131180, snprintf(text[n++], 400, l ? "Pokonano boss\xc3\xb3w: %%s. Zabij bossa regionu specjalnego" : "Bosses defeated: %%s. Defeat the special region boss");
    ids[n] = 131181, snprintf(text[n++], 400, l ? "Pokonano boss\xc3\xb3w: %%s" : "Bosses defeated: %%s");
    ids[n] = 131183, snprintf(text[n++], 400, l ? "Wyzwanie nie jest jeszcze dost\xc4\x99pne: pokonaj %%s boss\xc3\xb3w i bossa specjalnego (%s)"
                                                  : "The challenge is not available yet: defeat %%s bosses and the special boss (%s)", sp);
    ids[n] = 131191, snprintf(text[n++], 400, l ? "Wyzwanie nie jest jeszcze dost\xc4\x99pne: pokonaj %%s boss\xc3\xb3w i obu boss\xc3\xb3w specjalnych (%s)"
                                                  : "The challenge is not available yet: defeat %%s bosses and both special bosses (%s)", PAIR_SPECIAL_BOTH[p][l]);
    ids[n] = 131185, snprintf(text[n++], 400, l ? "Mapa: %s" : "Map: %s", PAIR_NAME[p][l]);
    for (int k = 0; k < 4; k++)
        ids[n] = 131186 + (u32)k, snprintf(text[n++], 400, l ? "Twoja dru\xc5\xbcyna (to samo has\xc5\x82o) gra na mapie %s. Ustaw map = %d w test-settings.toml albo uruchom launcher ponownie, aby wylosowa\xc4\x87 map\xc4\x99."
                                                             : "Your team (same password) plays the map %s. Set map = %d in test-settings.toml, or start the launcher again to draw a map.", PAIR_NAME[k][l], k + 1);
    return n;
}
static void pair_caption(int polish, char *out, size_t cap) {
    int l = polish != 0;
    snprintf(out, cap, l ? "Dwa skrzy\xc5\xbcowane ostrza, wezwanie Nocy.\n\nMo\xc5\xbcna u\xc5\xbcy\xc4\x87, gdy dru\xc5\xbcyna pokona sze\xc5\x9b" "ciu boss\xc3\xb3w "
                           "i bossa specjalnego (%s); drugiego dnia sze\xc5\x9b" "ciu boss\xc3\xb3w (albo wszystkich pozosta\xc5\x82ych, je\xc5\x9bli jest ich mniej) "
                           "i obu boss\xc3\xb3w specjalnych. Przenosi ca\xc5\x82\xc4\x85 dru\xc5\xbcyn\xc4\x99 na aren\xc4\x99 bossa nocy i zapada noc.\n\n"
                           "U\xc5\xbcyte za wcze\xc5\x9bnie zostaje w sakwie."
                         : "Two crossed blades, a call to the Night.\n\nUsable once the squad has defeated six bosses and the special boss (%s); on day 2, six "
                           "bosses (or all those left, if fewer) and both special bosses. Takes the whole squad to the night boss's arena, and the night falls.\n\nUsed too early, it stays in the pouch.",
             PAIR_SPECIAL[pair_now()][l]);
}

// --- small utilities ----------------------------------------------------------------------------

static log_fn g_log;
static char g_app[1024];

void core_set_log(log_fn fn) { g_log = fn; }
void core_set_app_dir(const char *dir) {  // stored as an absolute path
#ifdef _WIN32
    if (!GetFullPathNameA(dir, sizeof g_app, g_app, NULL)) snprintf(g_app, sizeof g_app, "%s", dir);
#else
    char full[4096];
    snprintf(g_app, sizeof g_app, "%s", realpath(dir, full) ? full : dir);
#endif
}

static void logf_(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_log) g_log(line);
    else fprintf(stderr, "%s\n", line);
}

static void *xalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { logf_("out of memory"); exit(1); }
    return p;
}

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static int dir_exists(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}

// Create every directory on the way to a file path.
static void make_parents(const char *file) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", file);
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/' || *c == '\\') {
            char keep = *c;
            *c = 0;
            make_dir(tmp);
            *c = keep;
        }
}

static buf read_file(const char *path) {
    buf b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    fseek64(f, 0, SEEK_END);
    long long n = ftell(f);
    fseek64(f, 0, SEEK_SET);
    b.p = xalloc(n);
    b.n = fread(b.p, 1, n, f);
    fclose(f);
    return b;
}

static int write_file(const char *path, const u8 *p, size_t n) {
    make_parents(path);
    FILE *f = fopen(path, "wb");
    if (!f) return logf_("cannot write %s", path), 0;
    size_t w = fwrite(p, 1, n, f);
    fclose(f);
    return w == n;
}

static int copy_file(const char *from, const char *to) {
    buf b = read_file(from);
    if (!b.p) return 0;
    int ok = write_file(to, b.p, b.n);
    free(b.p);
    return ok;
}

static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static void wr16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static u64 fnv1a64(const u8 *p, size_t n) {
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// --- RSA public-key operation (Montgomery, 2048 bit) for the archive header ------------------------

#define LIMBS 64
static u32 g_n0inv, g_r2[LIMBS];
// The archive header key in use (data0 unless a map-pair test reads data2: rsa_use).
static const u32 *g_rsa_n = DATA0_N;
static u32 g_rsa_e = DATA0_E;

static void mont_mul(u32 *r, const u32 *a, const u32 *b) {
    const u32 *n = g_rsa_n;
    u32 t[LIMBS + 2] = {0};
    for (int i = 0; i < LIMBS; i++) {
        u64 c = 0, s;
        for (int j = 0; j < LIMBS; j++) {
            s = (u64)t[j] + (u64)a[j] * b[i] + c;
            t[j] = (u32)s;
            c = s >> 32;
        }
        s = (u64)t[LIMBS] + c;
        t[LIMBS] = (u32)s;
        t[LIMBS + 1] = (u32)(s >> 32);
        u32 m = t[0] * g_n0inv;
        c = ((u64)t[0] + (u64)m * n[0]) >> 32;
        for (int j = 1; j < LIMBS; j++) {
            s = (u64)t[j] + (u64)m * n[j] + c;
            t[j - 1] = (u32)s;
            c = s >> 32;
        }
        s = (u64)t[LIMBS] + c;
        t[LIMBS - 1] = (u32)s;
        t[LIMBS] = t[LIMBS + 1] + (u32)(s >> 32);
    }
    int ge = t[LIMBS] != 0;
    if (!ge) {
        ge = 1;
        for (int j = LIMBS - 1; j >= 0; j--)
            if (t[j] != n[j]) { ge = t[j] > n[j]; break; }
    }
    if (ge) {
        u64 borrow = 0;
        for (int j = 0; j < LIMBS; j++) {
            u64 d = (u64)t[j] - n[j] - borrow;
            r[j] = (u32)d;
            borrow = (d >> 32) & 1;
        }
    } else {
        memcpy(r, t, LIMBS * 4);
    }
}

static void rsa_init(void) {
    u32 x = g_rsa_n[0];
    for (int i = 0; i < 5; i++) x *= 2 - g_rsa_n[0] * x;  // inverse of n mod 2^32 (Newton)
    g_n0inv = 0 - x;
    u32 r[LIMBS] = {1};
    for (int i = 0; i < 2 * 32 * LIMBS; i++) {  // r = 2^(2*2048) mod n by repeated doubling
        u32 carry = 0;
        for (int j = 0; j < LIMBS; j++) {
            u32 next = r[j] >> 31;
            r[j] = r[j] << 1 | carry;
            carry = next;
        }
        int ge = carry;
        if (!ge) {
            ge = 1;
            for (int j = LIMBS - 1; j >= 0; j--)
                if (r[j] != g_rsa_n[j]) { ge = r[j] > g_rsa_n[j]; break; }
        }
        if (ge) {
            u64 borrow = 0;
            for (int j = 0; j < LIMBS; j++) {
                u64 d = (u64)r[j] - g_rsa_n[j] - borrow;
                r[j] = (u32)d;
                borrow = (d >> 32) & 1;
            }
        }
    }
    memcpy(g_r2, r, sizeof r);
}

static void rsa_use(const u32 *n, u32 e) {  // switch the archive header key (data0 / data2)
    g_rsa_n = n, g_rsa_e = e;
    rsa_init();
}

// in: 256 bytes big-endian. out: 255 bytes big-endian (the value is below 2^2040 for valid data).
static void rsa_block(const u8 *in, u8 *out) {
    u32 c[LIMBS], cm[LIMBS], acc[LIMBS], one[LIMBS] = {1};
    for (int i = 0; i < LIMBS; i++) c[i] = be32(in + (LIMBS - 1 - i) * 4);
    mont_mul(cm, c, g_r2);
    mont_mul(acc, one, g_r2);
    for (int bit = 31; bit >= 0; bit--) {
        mont_mul(acc, acc, acc);
        if (g_rsa_e >> bit & 1) mont_mul(acc, acc, cm);
    }
    mont_mul(acc, acc, one);
    u8 full[256];
    for (int i = 0; i < LIMBS; i++) wbe32(full + (LIMBS - 1 - i) * 4, acc[i]);
    memcpy(out, full + 1, 255);
}

static buf rsa_decrypt(buf in) {
    size_t blocks = in.n / 256;
    buf out = {xalloc(blocks * 255), blocks * 255};
    for (size_t i = 0; i < blocks; i++) rsa_block(in.p + i * 256, out.p + i * 255);
    return out;
}

// --- AES: 128-bit keys for the archive (ECB), a 256-bit key for regulation.bin (CBC) ---------------

static u8 g_sbox[256], g_inv_sbox[256];

static void aes_init(void) {
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1B : 0);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        q ^= q & 0x80 ? 0x09 : 0;
        u8 x = q ^ (u8)(q << 1 | q >> 7) ^ (u8)(q << 2 | q >> 6) ^ (u8)(q << 3 | q >> 5) ^ (u8)(q << 4 | q >> 4);
        g_sbox[p] = x ^ 0x63;
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_inv_sbox[g_sbox[i]] = (u8)i;
}

static u8 gmul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = (u8)(a << 1) ^ (a & 0x80 ? 0x1B : 0);
        b >>= 1;
    }
    return r;
}

// key_len 16 -> 10 rounds, 32 -> 14 rounds. rk holds (rounds + 1) * 16 bytes (at most 240).
static void aes_expand(const u8 *key, int key_len, u8 *rk) {
    int total = (key_len / 4 + 7) * 16;
    memcpy(rk, key, key_len);
    u8 rcon = 1;
    for (int i = key_len; i < total; i += 4) {
        u8 t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % key_len == 0) {
            u8 first = t[0];
            t[0] = g_sbox[t[1]] ^ rcon;
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[first];
            rcon = (u8)(rcon << 1) ^ (rcon & 0x80 ? 0x1B : 0);
        } else if (key_len == 32 && i % 32 == 16) {
            for (int j = 0; j < 4; j++) t[j] = g_sbox[t[j]];
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - key_len + j] ^ t[j];
    }
}

static void aes_decrypt_block(const u8 *rk, int rounds, u8 s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[rounds * 16 + i];
    for (int round = rounds - 1;; round--) {
        u8 t[16];
        for (int c = 0; c < 4; c++)  // inverse ShiftRows + inverse SubBytes
            for (int r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = g_inv_sbox[s[c * 4 + r]];
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
        if (round == 0) break;
        for (int c = 0; c < 4; c++) {  // inverse MixColumns
            u8 *col = s + c * 4, a = col[0], b = col[1], d = col[2], e = col[3];
            col[0] = gmul(a, 14) ^ gmul(b, 11) ^ gmul(d, 13) ^ gmul(e, 9);
            col[1] = gmul(a, 9) ^ gmul(b, 14) ^ gmul(d, 11) ^ gmul(e, 13);
            col[2] = gmul(a, 13) ^ gmul(b, 9) ^ gmul(d, 14) ^ gmul(e, 11);
            col[3] = gmul(a, 11) ^ gmul(b, 13) ^ gmul(d, 9) ^ gmul(e, 14);
        }
    }
}

static void aes_encrypt_block(const u8 *rk, int rounds, u8 s[16]) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1;; round++) {
        u8 t[16];
        for (int c = 0; c < 4; c++)  // SubBytes + ShiftRows
            for (int r = 0; r < 4; r++) t[c * 4 + r] = g_sbox[s[((c + r) % 4) * 4 + r]];
        if (round < rounds)
            for (int c = 0; c < 4; c++) {  // MixColumns
                u8 *col = t + c * 4, a = col[0], b = col[1], d = col[2], e = col[3];
                col[0] = gmul(a, 2) ^ gmul(b, 3) ^ d ^ e;
                col[1] = a ^ gmul(b, 2) ^ gmul(d, 3) ^ e;
                col[2] = a ^ b ^ gmul(d, 2) ^ gmul(e, 3);
                col[3] = gmul(a, 3) ^ b ^ d ^ gmul(e, 2);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
        if (round == rounds) break;
    }
}

// --- the archive: decrypted BHD5 header + BDT data file --------------------------------------------

static u64 path_hash(const char *path) {
    u64 h = 0;
    for (; *path; path++) {
        char c = *path == '\\' ? '/' : *path;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = h * 0x85 + (u8)c;
    }
    return h;
}

// Read one file out of the archive. bhd is the decrypted header. Returns an empty buf if missing.
static buf archive_read(buf bhd, const char *bdt_path, const char *path) {
    buf out = {0};
    u64 want = path_hash(path);
    u32 buckets = rd32(bhd.p + 0x10), buckets_off = rd32(bhd.p + 0x14);
    const u8 *bucket = bhd.p + buckets_off + (want % buckets) * 8;
    u32 count = rd32(bucket), off = rd32(bucket + 4);
    for (u32 i = 0; i < count; i++) {
        const u8 *e = bhd.p + off + i * 0x28;
        if (rd64(e) != want) continue;
        u32 padded = rd32(e + 8);
        u64 file_off = rd64(e + 0x10), aes_off = rd64(e + 0x20);
        FILE *f = fopen(bdt_path, "rb");
        if (!f) return logf_("cannot open %s", bdt_path), out;
        out.p = xalloc(padded);
        fseek64(f, (long long)file_off, SEEK_SET);
        out.n = fread(out.p, 1, padded, f);
        fclose(f);
        if (out.n != padded) { free(out.p); out.p = NULL; out.n = 0; return out; }
        if (aes_off) {
            u8 rk[176];
            aes_expand(bhd.p + aes_off, 16, rk);
            u32 ranges = rd32(bhd.p + aes_off + 16);
            for (u32 r = 0; r < ranges; r++) {
                long long start = (long long)rd64(bhd.p + aes_off + 20 + r * 16);
                long long end = (long long)rd64(bhd.p + aes_off + 28 + r * 16);
                if (start < 0 || end < 0 || start == end || (u64)end > out.n) continue;
                for (long long p = start; p + 16 <= end; p += 16) aes_decrypt_block(rk, 10, out.p + p);
            }
        }
        return out;
    }
    return out;
}

// --- DCX ----------------------------------------------------------------------------------------------

#ifdef _WIN32
typedef long long (__cdecl *oodle_fn)(const void *, long long, void *, long long, int, int, int, void *,
                                      long long, void *, void *, void *, long long, int);
// Kraken, with the Oodle library that ships with the game.
static int kraken(const char *game_dir, const u8 *src, size_t n, u8 *dst, size_t dn) {
    static oodle_fn fn;
    if (!fn) {
        char dll[1024];
        snprintf(dll, sizeof dll, "%s\\oo2core_9_win64.dll", game_dir);
        HMODULE h = LoadLibraryA(dll);
        fn = h ? (oodle_fn)GetProcAddress(h, "OodleLZ_Decompress") : NULL;
        if (!fn) return logf_("cannot load %s", dll), 0;
    }
    return fn(src, (long long)n, dst, (long long)dn, 1, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 3) == (long long)dn;
}
#else
static int run(char *const argv[], const char *extra_env) {
    char *env[256];
    int n = 0;
    for (char **e = environ; *e && n < 250; e++) env[n++] = *e;
    char prefix[1100], debug[] = "WINEDEBUG=-all";
    if (extra_env) { snprintf(prefix, sizeof prefix, "%s", extra_env); env[n++] = prefix; env[n++] = debug; }
    env[n] = NULL;
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, env)) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void wine_path(char *out, size_t cap, const char *unix_path) {
    snprintf(out, cap, "Z:%s", unix_path);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}

// First entry of `parent` whose name starts with `prefix` and which contains `inner`. Fills out.
static int find_child(const char *parent, const char *prefix, const char *inner, char *out, size_t cap) {
    DIR *d = opendir(parent);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strncmp(e->d_name, prefix, strlen(prefix))) continue;
        snprintf(out, cap, "%s/%s/%s", parent, e->d_name, inner);
        found = exists(out);
    }
    closedir(d);
    return found;
}

// A wine binary to run the Oodle helper with: Proton's own (every Linux player of the game has
// Proton through Steam), else whatever "wine" is installed.
static const char *find_wine(const char *game_dir) {
    static char wine[1700];
    if (*wine) return wine;
    char dir[1100];
    const char *tail = strstr(game_dir, "/steamapps/common/"), *home = getenv("HOME");
    static const char *inner[] = {"files/bin/wine", "dist/bin/wine"};
    static const char *roots[] = {"/.local/share/Steam", "/.steam/steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    for (size_t i = 0; i < 2; i++) {
        if (tail) {  // the Steam library the game is in
            snprintf(dir, sizeof dir, "%.*s/steamapps/common", (int)(tail - game_dir), game_dir);
            if (find_child(dir, "Proton", inner[i], wine, sizeof wine)) return wine;
        }
        for (size_t r = 0; home && r < sizeof roots / sizeof *roots; r++) {
            snprintf(dir, sizeof dir, "%s%s/steamapps/common", home, roots[r]);
            if (find_child(dir, "Proton", inner[i], wine, sizeof wine)) return wine;
            snprintf(dir, sizeof dir, "%s%s/compatibilitytools.d", home, roots[r]);
            if (find_child(dir, "", inner[i], wine, sizeof wine)) return wine;
        }
    }
    snprintf(wine, sizeof wine, "wine");
    return wine;
}

// Kraken on Linux: the game's Oodle library is a Windows DLL, so a tiny helper (data/unkrak.exe)
// runs it under Proton's wine, in a private prefix (work/wineprefix) that is deleted after the build.
static int kraken(const char *game_dir, const u8 *dcx, size_t n, u8 *dst, size_t dn) {
    char in[1100], out[1100], exe[1100], dll[1100], win_in[1200], win_out[1200], win_dll[1200], prefix[1100];
    snprintf(in, sizeof in, "%s/work/tmp.dcx", g_app);
    snprintf(out, sizeof out, "%s/work/tmp.bin", g_app);
    snprintf(exe, sizeof exe, "%s/data/unkrak.exe", g_app);
    snprintf(dll, sizeof dll, "%s/oo2core_9_win64.dll", game_dir);
    snprintf(prefix, sizeof prefix, "WINEPREFIX=%s/work/wineprefix", g_app);
    if (!write_file(in, dcx, n)) return 0;
    wine_path(win_in, sizeof win_in, in);
    wine_path(win_out, sizeof win_out, out);
    wine_path(win_dll, sizeof win_dll, dll);
    char *argv[] = {(char *)find_wine(game_dir), exe, win_dll, win_in, win_out, NULL};
    int rc = run(argv, prefix);
    remove(in);
    if (rc != 0) return logf_("Could not run the unpacking helper with %s (code %d). Proton or wine is needed.", argv[0], rc), 0;
    buf b = read_file(out);
    remove(out);
    int ok = b.n == dn;
    if (ok) memcpy(dst, b.p, dn);
    free(b.p);
    return ok;
}
#endif

static void dcx_read_many_krak(const char *game_dir, buf *in, buf *out, int n);
// Many DCX files at once: zlib ones here; Kraken on Linux with one start of the helper for up to 64 files (each start of wine is slow), on
// Windows the Oodle library directly. out[i] is empty for a file that failed.
size_t nr_zlib_decompress(const u8 *src, size_t n, u8 *dst, size_t cap);
static int dcx_zlib(buf in, buf *out) {  // a DFLT DCX (some of the game's event scripts): 1 when unpacked
    if (!in.p || in.n < 0x4C || memcmp(in.p + 0x28, "DFLT", 4)) return 0;
    u32 usize = be32(in.p + 0x1C), csize = be32(in.p + 0x20);
    if (csize > in.n - 0x4C) return 0;
    out->p = xalloc(usize), out->n = usize;
    if (nr_zlib_decompress(in.p + 0x4C, csize, out->p, usize) != usize) free(out->p), out->p = NULL, out->n = 0;
    return 1;
}
static void dcx_read_many(const char *game_dir, buf *in, buf *out, int n) {
    for (int i = 0; i < n; i++) out[i].p = NULL, out[i].n = 0;
    static buf krak[4096];
    int map[4096], k = 0;
    for (int i = 0; i < n && k < 4096; i++)
        if (!dcx_zlib(in[i], &out[i])) map[k] = i, krak[k++] = in[i];
    if (k < n) {  // the Kraken ones through the helper below
        static buf got[4096];
        dcx_read_many_krak(game_dir, krak, got, k);
        for (int j = 0; j < k; j++) out[map[j]] = got[j];
        return;
    }
    dcx_read_many_krak(game_dir, in, out, n);
}
static void dcx_read_many_krak(const char *game_dir, buf *in, buf *out, int n) {
    for (int i = 0; i < n; i++) out[i].p = NULL, out[i].n = 0;
#ifdef _WIN32
    for (int i = 0; i < n; i++) {
        if (!in[i].p || in[i].n < 0x4C || memcmp(in[i].p + 0x28, "KRAK", 4)) continue;
        u32 usize = be32(in[i].p + 0x1C), csize = be32(in[i].p + 0x20);
        if (csize > in[i].n - 0x4C) continue;
        out[i].p = xalloc(usize), out[i].n = usize;
        if (!kraken(game_dir, in[i].p + 0x4C, csize, out[i].p, usize)) free(out[i].p), out[i].p = NULL, out[i].n = 0;
    }
#else
    char exe[1100], dll[1100], prefix[1100], win_dll[1200];
    snprintf(exe, sizeof exe, "%s/data/unkrak.exe", g_app);
    snprintf(dll, sizeof dll, "%s/oo2core_9_win64.dll", game_dir);
    snprintf(prefix, sizeof prefix, "WINEPREFIX=%s/work/wineprefix", g_app);
    wine_path(win_dll, sizeof win_dll, dll);
    static char paths[128][2][1200];
    for (int at = 0; at < n; at += 64) {
        int m = n - at < 64 ? n - at : 64, k = 0;
        char *argv[4 + 128 + 1];
        argv[k++] = (char *)find_wine(game_dir), argv[k++] = exe, argv[k++] = win_dll;
        for (int i = 0; i < m; i++) {
            char in_path[1100], out_path[1100];
            snprintf(in_path, sizeof in_path, "%s/work/tmp%d.dcx", g_app, i);
            snprintf(out_path, sizeof out_path, "%s/work/tmp%d.bin", g_app, i);
            remove(out_path);
            if (!in[at + i].p || !write_file(in_path, in[at + i].p, in[at + i].n)) continue;
            wine_path(paths[2 * i][0], sizeof paths[0][0], in_path);
            wine_path(paths[2 * i][1], sizeof paths[0][1], out_path);
            argv[k++] = paths[2 * i][0], argv[k++] = paths[2 * i][1];
        }
        argv[k] = NULL;
        if (k > 3) run(argv, prefix);  // a file the helper could not unpack just has no output
        for (int i = 0; i < m; i++) {
            char in_path[1100], out_path[1100];
            snprintf(in_path, sizeof in_path, "%s/work/tmp%d.dcx", g_app, i);
            snprintf(out_path, sizeof out_path, "%s/work/tmp%d.bin", g_app, i);
            remove(in_path);
            if (in[at + i].p && in[at + i].n >= 0x24) {
                buf b = read_file(out_path);
                if (b.p && b.n == be32(in[at + i].p + 0x1C)) out[at + i] = b;
                else free(b.p);
            }
            remove(out_path);
        }
    }
#endif
}

// Decompress a DCX file. Only Kraken (what the game ships) and zlib (what we write) are handled.
static buf dcx_read(const char *game_dir, buf d) {
    buf out = {0};
    if (d.n < 0x4C || memcmp(d.p, "DCX", 3)) return logf_("not a DCX file"), out;
    u32 usize = be32(d.p + 0x1C), csize = be32(d.p + 0x20);
    if (csize > d.n - 0x4C) return logf_("truncated DCX"), out;
    if (memcmp(d.p + 0x28, "KRAK", 4)) return logf_("unsupported DCX compression %.4s", d.p + 0x28), out;
    out.p = xalloc(usize);
    out.n = usize;
#ifdef _WIN32
    int ok = kraken(game_dir, d.p + 0x4C, csize, out.p, usize);
#else
    int ok = kraken(game_dir, d.p, 0x4C + csize, out.p, usize);  // the helper takes the whole DCX
#endif
    if (!ok) { free(out.p); out.p = NULL; out.n = 0; logf_("decompression failed"); }
    return out;
}

// Zig's standard library, see zstd.zig. Both return 0 on error.
size_t nr_zstd_decompress(const u8 *src, size_t n, u8 *dst, size_t cap);  // cap: content size + 128 KB
size_t nr_zlib_compress(const u8 *src, size_t n, u8 *dst, size_t cap);

// Write a zlib DCX, which the game accepts. (Really compressed: the game loads small files made of
// stored deflate blocks, but crashed on a 26 MB texture file written that way.)
static buf dcx_write(buf raw) {
    size_t cap = raw.n + raw.n / 8 + 1024;
    buf out = {xalloc(0x4C + cap), 0};
    size_t zlen = nr_zlib_compress(raw.p, raw.n, out.p + 0x4C, cap);
    if (!zlen) { logf_("compression failed"); exit(1); }
    out.n = 0x4C + zlen;
    u8 *h = out.p;
    memcpy(h, "DCX\0", 4);
    wbe32(h + 4, 0x11000); wbe32(h + 8, 0x18); wbe32(h + 12, 0x24); wbe32(h + 16, 0x44); wbe32(h + 20, 0x4C);
    memcpy(h + 0x18, "DCS\0", 4);
    wbe32(h + 0x1C, (u32)raw.n); wbe32(h + 0x20, (u32)zlen);
    memcpy(h + 0x24, "DCP\0DFLT", 8);
    wbe32(h + 0x2C, 0x20);
    h[0x30] = 9;
    wbe32(h + 0x40, 0x00010100);
    memcpy(h + 0x44, "DCA\0", 4);
    wbe32(h + 0x48, 8);
    return out;
}

// --- BND4 bundles ----------------------------------------------------------------------------------------

// Does the UTF-16 name at p end with the ASCII suffix (case-insensitive)?
static int name_ends_with(const u8 *p, const char *suffix) {
    size_t len = 0, sl = strlen(suffix);
    while (p[len * 2] || p[len * 2 + 1]) len++;
    if (len < sl) return 0;
    for (size_t i = 0; i < sl; i++) {
        char a = (char)p[(len - sl + i) * 2], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b || p[(len - sl + i) * 2 + 1]) return 0;
    }
    return 1;
}

static int bnd4_find(buf b, const char *suffix) {
    if (b.n < 0x40 || memcmp(b.p, "BND4", 4) || rd64(b.p + 0x20) != 0x24) return -1;
    u32 count = rd32(b.p + 0x0C);
    for (u32 i = 0; i < count; i++)
        if (name_ends_with(b.p + rd32(b.p + 0x40 + i * 0x24 + 0x20), suffix)) return (int)i;
    return -1;
}

static buf bnd4_file(buf b, int i) {
    const u8 *e = b.p + 0x40 + i * 0x24;
    buf f = {b.p + rd32(e + 0x18), (size_t)rd64(e + 0x10)};
    return f;
}

// Copy of the bundle with file `index` replaced. Headers and names are kept, the data area rebuilt.
static buf bnd4_copy(buf f) {
    buf c = {xalloc(f.n ? f.n : 1), f.n};
    memcpy(c.p, f.p, f.n);
    return c;
}
static buf bnd4_replace(buf b, int index, buf replacement) {
    u32 count = rd32(b.p + 0x0C), data_start = 0xFFFFFFFF;
    for (u32 i = 0; i < count; i++) {
        u32 off = rd32(b.p + 0x40 + i * 0x24 + 0x18);
        if (off < data_start) data_start = off;
    }
    buf out = {xalloc(b.n + replacement.n + count * 16), 0};
    memcpy(out.p, b.p, data_start);
    out.n = data_start;
    for (u32 i = 0; i < count; i++) {
        buf f = (int)i == index ? replacement : bnd4_file(b, (int)i);
        while (out.n % 16) out.p[out.n++] = 0;
        u8 *e = out.p + 0x40 + i * 0x24;
        wr64(e + 8, f.n);
        wr64(e + 0x10, f.n);
        wr32(e + 0x18, (u32)out.n);
        memcpy(out.p + out.n, f.p, f.n);
        out.n += f.n;
    }
    return out;
}

// --- FMG text tables (version 2: unicode, 64-bit offsets) ---------------------------------------------------

typedef struct { u32 id; const u8 *text; size_t bytes; } fmg_entry;  // text: UTF-16LE without terminator, or NULL
typedef struct { fmg_entry *e; size_t n, cap; } fmg;

static size_t utf16_bytes(const u8 *p) {
    size_t n = 0;
    while (p[n] || p[n + 1]) n += 2;
    return n;
}

static int fmg_parse(buf d, fmg *out) {
    if (d.n < 0x28 || d.p[2] != 2 || d.p[8] != 1) return 0;
    u32 groups = rd32(d.p + 0x0C), strings = rd32(d.p + 0x10);
    u64 offsets = rd64(d.p + 0x18);
    out->cap = strings + 8;
    out->e = xalloc(out->cap * sizeof *out->e);
    out->n = 0;
    for (u32 g = 0; g < groups; g++) {
        const u8 *grp = d.p + 0x28 + g * 0x10;
        u32 index = rd32(grp), first = rd32(grp + 4), last = rd32(grp + 8);
        for (u32 j = 0; j <= last - first && out->n < strings; j++) {
            u64 off = rd64(d.p + offsets + (u64)(index + j) * 8);
            fmg_entry *e = &out->e[out->n++];
            e->id = first + j;
            e->text = off ? d.p + off : NULL;
            e->bytes = off ? utf16_bytes(d.p + off) : 0;
        }
    }
    return 1;
}

static const fmg_entry *fmg_get(const fmg *f, u32 id) {
    for (size_t i = 0; i < f->n; i++) if (f->e[i].id == id) return &f->e[i];
    return NULL;
}

// Insert or replace, keeping the table sorted by id; the table grows when it is full. (Until 2026-11-15 the
// parse left room for 8 additions and this function assumed there was always room: our 9th line (131167) wrote
// one entry past the allocation, the 11th (131169) three, and the PC's build aborted in malloc.)
static void fmg_set(fmg *f, u32 id, const u8 *text, size_t bytes) {
    size_t i = 0;
    while (i < f->n && f->e[i].id < id) i++;
    if (i == f->n || f->e[i].id != id) {
        if (f->n == f->cap) {
            size_t cap = f->cap * 2 + 8;
            fmg_entry *e = xalloc(cap * sizeof *e);
            if (f->n) memcpy(e, f->e, f->n * sizeof *e);
            free(f->e), f->e = e, f->cap = cap;
        }
        memmove(&f->e[i + 1], &f->e[i], (f->n - i) * sizeof *f->e);
        f->n++;
    }
    f->e[i].id = id;
    f->e[i].text = text;
    f->e[i].bytes = bytes;
}

static buf fmg_write(const fmg *f) {
    size_t groups = 0, text_bytes = 0;
    for (size_t i = 0; i < f->n; i++) {
        if (i == 0 || f->e[i].id != f->e[i - 1].id + 1) groups++;
        if (f->e[i].text) text_bytes += f->e[i].bytes + 2;
    }
    size_t offsets = 0x28 + groups * 0x10, strings = offsets + f->n * 8;
    size_t total = strings + text_bytes + (4 - text_bytes % 4) % 4;
    buf out = {xalloc(total), total};
    u8 *p = out.p;
    p[2] = 2;
    wr32(p + 4, (u32)total);
    p[8] = 1;
    wr32(p + 0x0C, (u32)groups);
    wr32(p + 0x10, (u32)f->n);
    wr32(p + 0x14, 0xFF);
    wr64(p + 0x18, offsets);
    size_t g = 0, pos = strings;
    for (size_t i = 0; i < f->n; i++) {
        if (i == 0 || f->e[i].id != f->e[i - 1].id + 1) {
            u8 *grp = p + 0x28 + g++ * 0x10;
            wr32(grp, (u32)i);
            wr32(grp + 4, f->e[i].id);
        }
        wr32(p + 0x28 + (g - 1) * 0x10 + 8, f->e[i].id);  // last id of the current group
        if (f->e[i].text) {
            wr64(p + offsets + i * 8, pos);
            memcpy(p + pos, f->e[i].text, f->e[i].bytes);
            pos += f->e[i].bytes + 2;
        }
    }
    return out;
}

// UTF-8 (2-byte sequences at most, enough for our own lines) to UTF-16LE. Returns byte count.
static size_t utf8_to_utf16(const char *s, u8 *out) {
    size_t n = 0;
    for (const u8 *c = (const u8 *)s; *c;) {
        u32 cp = *c < 0x80 ? *c++ : (c += 2, ((c[-2] & 0x1F) << 6) | (c[-1] & 0x3F));
        out[n++] = (u8)cp;
        out[n++] = (u8)(cp >> 8);
    }
    return n;
}

// --- finding the game and the save ----------------------------------------------------------------------------

// --- regulation.bin: the game's params, AES-256-CBC around a DCX (zstd in the game's file) around a BND4 -------

// The regulation key is public knowledge (Smithbox).
static const u8 REGULATION_KEY[32] = {
    0x9a, 0x8e, 0xe9, 0x0c, 0x4c, 0x01, 0xa4, 0x31, 0x68, 0xa1, 0x7d, 0x9d, 0x75, 0xe4, 0xa7, 0xd0,
    0x21, 0x07, 0xeb, 0xcf, 0x43, 0xd5, 0xac, 0xb0, 0x55, 0x4f, 0x94, 0x16, 0x01, 0xb5, 0x79, 0x18};

// The Target list is NightBossMenuParam. Row 100 is the game's own non-boss entry ("The Deep of
// Night"); our row 101 is a copy of it with these fields changed. Keep in sync with
// mod/build_regulation.py, which explains the fields.
#define TARGET_PARAM "\\NightBossMenuParam.param"
#define TEMPLATE_ROW 100
#define INVASION_ROW 101
static const int ROW_CHANGES[][2] = {{131150, 131160}, {131151, 131161}, {41812, 41813}, {300, 5}, {130, 0}};
#define FIRST_LARGE_ICON 41820  // row 0's largeBossIconId, to find the field
#define LARGE_ICON 41814

// Offset of the only place where a row holds this 32-bit value (step: 1 or 4 bytes), -1 otherwise.
static int find_value(const u8 *row, u32 size, int value, int step) {
    int found = -1;
    for (u32 o = 0; o + 4 <= size; o += step)
        if ((int)rd32(row + o) == value) {
            if (found >= 0) return -1;
            found = (int)o;
        }
    return found;
}

// Param layout: 0x40 header, 24-byte row descriptors (id, pad, data offset, name offset), row data,
// strings. Returns the param with our row appended, or an empty buf if it does not look as expected.
static buf param_add_row(buf p) {
    buf out = {0};
    if (p.n < 0x40 + 48) return out;
    u32 strings_off = rd32(p.p), count = rd16(p.p + 0x0A);
    if (!(p.p[0x2D] & 4) || count < 2 || 0x40 + (size_t)count * 24 > p.n) return out;
    u64 data_start = rd64(p.p + 0x40 + 8), size = rd64(p.p + 0x40 + 24 + 8) - data_start;
    if (data_start != 0x40 + (u64)count * 24 || strings_off != data_start + count * size || strings_off > p.n) return out;
    const u8 *tmpl = NULL;
    for (u32 i = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24;
        if ((int)rd32(d) >= INVASION_ROW) return out;
        if (rd32(d) == TEMPLATE_ROW) tmpl = d;
    }
    if (!tmpl) return out;

    u32 shift = 24 + (u32)size;  // one descriptor before the data, one row before the strings
    out.p = xalloc(p.n + shift);
    memcpy(out.p, p.p, 0x40);
    wr32(out.p, strings_off + shift);
    wr16(out.p + 0x0A, count + 1);
    wr64(out.p + 0x10, rd64(p.p + 0x10) + shift);
    wr64(out.p + 0x30, data_start + 24);
    u8 *d = out.p + 0x40;
    for (u32 i = 0; i < count; i++, d += 24) {
        const u8 *src = p.p + 0x40 + i * 24;
        memcpy(d, src, 8);
        wr64(d + 8, rd64(src + 8) + 24);
        wr64(d + 16, rd64(src + 16) + shift);
    }
    wr32(d, INVASION_ROW);
    wr64(d + 8, data_start + 24 + count * size);
    wr64(d + 16, rd64(tmpl + 16) + shift);
    d += 24;
    memcpy(d, p.p + data_start, count * size);
    u8 *row = d + count * size;
    memcpy(row, p.p + rd64(tmpl + 8), size);
    memcpy(row + size, p.p + strings_off, p.n - strings_off);
    out.n = p.n + shift;

    int ok = 1;
    for (size_t i = 0; i < sizeof ROW_CHANGES / sizeof *ROW_CHANGES; i++) {
        int at = find_value(row, (u32)size, ROW_CHANGES[i][0], 1);
        if (at < 0) ok = 0;
        else wr32(row + at, (u32)ROW_CHANGES[i][1]);
    }
    int large = find_value(p.p + data_start, (u32)size, FIRST_LARGE_ICON, 4);
    if (large < 0 || (int)rd32(row + large) != -1) ok = 0;
    else wr32(row + large, LARGE_ICON);
    if (!ok) { free(out.p); out.p = NULL; out.n = 0; }
    return out;
}

// Our SpEffect row (team test, later the invader's team), the scarabs' team and the no-rain
// change. Keep in sync with mod/build_regulation.py, which explains the values and how they were
// checked.
#define SPEFFECT_PARAM "\\SpEffectParam.param"
#define SPEFFECT_TEMPLATE 10101
#define SPEFFECT_TEAM_ROW 90071610
// The ally's look (no side change): our row 90071611, a copy of the same template with vfxId1 54220 (the game's
// SpEffectVfxParam row like 300050, but phantomParamOverwriteId 62: PhantomParam 62, the gold phantom: edge
// colour 200 160 120) and the template's effectEndurance -1 (no time limit; at 30 s it lapsed and blinked); the
// DLL puts it on in the host's world and removes it when the ally's part ends. changeTeamType stays none (0xff).
#define SPEFFECT_ALLY_ROW 90071611
#define ALLY_PHANTOM_VFX 54220
#define SPEFFECT_SIZE 0x400
#define NPC_PARAM "\\NpcParam.param"
#define NPC_TEAM 0x133
#define PLAY_AREA_PARAM "\\PlayAreaCreateCommonParam.param"

// Copy of the param with row new_id (a copy of template_id) inserted in id order; *row points at
// the new row's data. Empty buf if the param does not look as expected.
static buf param_insert_sorted(buf p, int template_id, int new_id, u64 size, u8 **row) {
    buf out = {0};
    if (p.n < 0x40 + 48 || !(p.p[0x2D] & 4)) return out;
    u32 strings_off = rd32(p.p), count = rd16(p.p + 0x0A);
    if (count < 2 || 0x40 + (size_t)count * 24 > p.n) return out;
    u64 data_start = rd64(p.p + 0x48);
    if (rd64(p.p + 0x40 + 24 + 8) - data_start != size || data_start != 0x40 + (u64)count * 24 ||
        strings_off != data_start + count * size || strings_off > p.n) return out;
    const u8 *tmpl = NULL;
    u32 insert_at = count;
    for (u32 i = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24;
        int id = (int)rd32(d);
        if (rd64(d + 8) != data_start + i * size || (i && id <= (int)rd32(d - 24)) || id == new_id) return out;
        if (id == template_id) tmpl = d;
        if (id > new_id && insert_at == count) insert_at = i;
    }
    if (!tmpl) return out;
    u32 shift = 24 + (u32)size;
    out.p = xalloc(p.n + shift);
    out.n = p.n + shift;
    memcpy(out.p, p.p, 0x40);
    wr32(out.p, strings_off + shift);
    wr16(out.p + 0x0A, count + 1);
    wr64(out.p + 0x10, rd64(p.p + 0x10) + shift);
    wr64(out.p + 0x30, data_start + 24);
    u64 new_start = data_start + 24;
    for (u32 j = 0, i = 0; j <= count; j++) {
        u8 *d = out.p + 0x40 + j * 24, *data = out.p + new_start + j * size;
        const u8 *src = j == insert_at ? tmpl : p.p + 0x40 + i++ * 24;
        wr32(d, j == insert_at ? (u32)new_id : rd32(src));
        wr32(d + 4, 0);
        wr64(d + 8, new_start + j * size);
        wr64(d + 16, rd64(src + 16) + shift);
        memcpy(data, p.p + rd64(src + 8), size);
        if (j == insert_at) *row = data;
    }
    memcpy(out.p + strings_off + shift, p.p + strings_off, p.n - strings_off);
    return out;
}

// Copy of the param with n rows first_id .. first_id + n - 1 (copies of template_id) inserted in id order; *row
// points at the first new row's data (the n rows follow each other). Empty buf if the param does not look as
// expected or one of the ids is taken.
static buf param_insert_range(buf p, int template_id, int first_id, u32 n, u64 size, u8 **row) {
    buf out = {0};
    if (n == 0 || p.n < 0x40 + 48 || !(p.p[0x2D] & 4)) return out;
    u32 strings_off = rd32(p.p), count = rd16(p.p + 0x0A);
    if (count < 2 || count + n > 0xffff || 0x40 + (size_t)count * 24 > p.n) return out;
    u64 data_start = rd64(p.p + 0x48);
    if (rd64(p.p + 0x40 + 24 + 8) - data_start != size || data_start != 0x40 + (u64)count * 24 ||
        strings_off != data_start + count * size || strings_off > p.n) return out;
    const u8 *tmpl = NULL;
    u32 insert_at = count;
    for (u32 i = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24;
        int id = (int)rd32(d);
        if (rd64(d + 8) != data_start + i * size || (i && id <= (int)rd32(d - 24)) || (id >= first_id && id < first_id + (int)n)) return out;
        if (id == template_id) tmpl = d;
        if (id > first_id && insert_at == count) insert_at = i;
    }
    if (!tmpl) return out;
    u64 shift = (24 + size) * n;
    out.p = xalloc(p.n + shift);
    out.n = p.n + shift;
    memcpy(out.p, p.p, 0x40);
    wr32(out.p, (u32)(strings_off + shift));
    wr16(out.p + 0x0A, count + n);
    wr64(out.p + 0x10, rd64(p.p + 0x10) + shift);
    wr64(out.p + 0x30, data_start + 24 * n);
    u64 new_start = data_start + 24 * n;
    for (u32 j = 0, i = 0; j < count + n; j++) {
        u8 *d = out.p + 0x40 + j * 24, *data = out.p + new_start + j * size;
        int fresh = j >= insert_at && j < insert_at + n;
        const u8 *src = fresh ? tmpl : p.p + 0x40 + i++ * 24;
        wr32(d, fresh ? (u32)(first_id + (int)(j - insert_at)) : rd32(src));
        wr32(d + 4, 0);
        wr64(d + 8, new_start + j * size);
        wr64(d + 16, rd64(src + 16) + shift);
        memcpy(data, p.p + rd64(src + 8), size);
        if (j == insert_at) *row = data;
    }
    memcpy(out.p + strings_off + shift, p.p + strings_off, p.n - strings_off);
    return out;
}

// The day flow's rows (boss-flow 4; DLL: day_flow). Offsets read from the game's own rows (1.3.3):
// - SpEffectParam (0x400): +0x08 effectEndurance, +0x10 maxHpRate, +0x48 / +0x4c / +0x50 / +0x54 physics / magic /
//   fire / thunder attack, +0x1e0 dark attack (row 7430, the game's day-2 effect: 1.4 at exactly these fields).
//   CHALLENGE_SPEFFECT: the item's use marker (template 10101, 1 s). TIER_FIRST + k (k = 1 .. TIER_COUNT): an enemy
//   tier of +5 % * k HP and damage (template 10101, no time limit; the DLL puts one on each enemy and removes the
//   one before).
// - EquipParamGoods (0xe0): our item CHALLENGE_GOODS, a copy of Starlight Shards 1290 (a consumable whose use
//   applies refId_default +0x04 as a SpEffect, refCategory 2): +0x04 our marker, +0x30 iconId, +0xb8 / +0xc0 its
//   level-2 / 3 SpEffects off (-1), +0x48 bit 7 isConsume off.
// - ShopLineupParam (0x40): the field merchant (talk t800006000: 3000-3499, 3500-3999 with 7800) sells it as its
//   fixed rows 3000-3007 (release 0): 3007 / 3507 (the Small Pouch) sell it instead: +0x04 equipId, +0x08 price,
//   +0x14 release 0, +0x18 quantity 1 (the pouch's own stock flag 90470 stays).
#define GOODS_PARAM "\\EquipParamGoods.param"
#define SHOP_PARAM "\\ShopLineupParam.param"
#define CHALLENGE_SPEFFECT 90071612
#define CHALLENGE_FALL_SPEFFECT 90071613
#define TIER_FIRST 90071700
#define TIER_COUNT 200
#define CHALLENGE_ICON 21990
#define CHALLENGE_PRICE 15000
static int day_flow_rows(buf *sp, buf *goods, buf *shop) {
    u8 *row = NULL;
    buf a = param_insert_sorted(*sp, SPEFFECT_TEMPLATE, CHALLENGE_SPEFFECT, SPEFFECT_SIZE, &row);
    if (!a.p || rd32(row + 0x08) != 0xBF800000) return free(a.p), 0;
    float one = 1.0f;
    memcpy(row + 0x08, &one, 4);
    // the challenge's landing: no fall damage for 15 s (fallDamageRate +0xdc, read against all 13472 rows of the game's
    // file; the game has rows with 0, 0.1 and 0.5 there), as the DLL drops the player onto the arena from above
    {
        u8 *fr = NULL;
        buf c = param_insert_sorted(a, SPEFFECT_TEMPLATE, CHALLENGE_FALL_SPEFFECT, SPEFFECT_SIZE, &fr);
        if (!c.p || rd32(fr + 0x08) != 0xBF800000) return free(a.p), free(c.p), 0;
        float t = 15.0f, zero = 0.0f;
        memcpy(fr + 0x08, &t, 4), memcpy(fr + 0xdc, &zero, 4);
        free(a.p);
        a = c;
    }
    buf b = param_insert_range(a, SPEFFECT_TEMPLATE, TIER_FIRST + 1, TIER_COUNT, SPEFFECT_SIZE, &row);
    free(a.p);
    if (!b.p) return 0;
    static const int RATE_AT[] = {0x10, 0x48, 0x4c, 0x50, 0x54, 0x1e0};
    for (int k = 1; k <= TIER_COUNT; k++, row += SPEFFECT_SIZE) {
        float v = 1.0f + 0.05f * (float)k;
        for (size_t f = 0; f < sizeof RATE_AT / sizeof *RATE_AT; f++) {
            float old;
            memcpy(&old, row + RATE_AT[f], 4);
            if (old != 1.0f) return free(b.p), 0;
            memcpy(row + RATE_AT[f], &v, 4);
        }
    }
    buf g = param_insert_sorted(*goods, 1290, CHALLENGE_GOODS, 0xe0, &row);
    if (!g.p || rd32(row + 0x04) != 501290) return free(b.p), free(g.p), 0;
    wr32(row + 0x04, CHALLENGE_SPEFFECT);
    wr16(row + 0x30, CHALLENGE_ICON);
    wr32(row + 0xb8, 0xFFFFFFFF), wr32(row + 0xc0, 0xFFFFFFFF);
    // isConsume (+0x48 bit 7, read against all 433 rows of the game's file): off, so a use before the condition loses
    // nothing; the DLL removes the item itself when the challenge starts (deck test: our give-back did not reach it)
    if (!(row[0x48] & 0x80)) return free(b.p), free(g.p), 0;
    row[0x48] &= 0x7f;
    // the merchant's fixed Small Pouch rows (3007 / 3507) sell our item instead: with 8 item slots from the start
    // (PlayerCommonParam, below) the pouch has nothing left to add
    int replaced = 0;
    for (u32 k = 0; k < rd16(shop->p + 0x0A); k++) {
        int id = (int)rd32(shop->p + 0x40 + k * 24);
        u8 *r = shop->p + rd64(shop->p + 0x40 + k * 24 + 8);
        if ((id == 3007 || id == 3507) && rd32(r + 0x04) == 10050) {
            wr32(r + 0x04, CHALLENGE_GOODS), wr32(r + 0x08, CHALLENGE_PRICE), wr16(r + 0x18, 1), wr32(r + 0x14, 0);
            replaced++;
        }
    }
    if (replaced != 2) return free(b.p), free(g.p), 0;
    free(sp->p), free(goods->p);
    *sp = b, *goods = g;
    return 1;
}
// PlayerCommonParam row 0: +0xe item slots at the start (4), +0xf the most pouches can reach (8); the count is
// 0x1401f2370(PlayerGameData + 0x1d8) = min(+0xe + pouches, +0xf) + extra, at most 10 (a constant in the code).
// Both 8: every slot from the start, a count the game already reaches with pouches. In place.
static int player_common_slots(buf p) {
    if (p.n < 0x40 + 24 || rd16(p.p + 0x0A) < 1 || rd32(p.p + 0x40) != 0) return 0;
    u8 *r = p.p + rd64(p.p + 0x48);
    if ((u64)(r - p.p) + 0x10 > p.n || r[0xe] != 4 || r[0xf] != 8) return 0;
    r[0xe] = 8, r[0xf] = 8;
    return 1;
}

// Scarabs (NpcParam 41900000-41919999, 30 rows, all team 6) to team 17, in place. 0 if unexpected.
static int scarab_rows(buf p) {
    u32 count = rd16(p.p + 0x0A), changed = 0;
    for (u32 i = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24;
        int id = (int)rd32(d);
        if (id < 41900000 || id >= 41920000) continue;
        u8 *team = p.p + rd64(d + 8) + NPC_TEAM;
        if (*team != 6) return 0;
        *team = 17;
        changed++;
    }
    return changed == 30;
}

// The rain's damage: PlayAreaCreateCommonParam row 0's rain SpEffects 99810 (+0x28) and 99811 (+0x2c) stay the game's
// (until the fire ring they were set to -1, decision 27). By day the DLL holds the circle at 5000 m, so nobody is
// outside it; after the challenge the ring around the arena does damage (owner, BRIEF 2026-11-18). Only checked here:
// 0 if the row is not as expected.
static int no_rain(buf p) {
    const u8 *d = p.p + 0x40;
    if (rd16(p.p + 0x0A) < 1 || rd32(d) != 0) return 0;
    const u8 *row = p.p + rd64(d + 8);
    return rd32(row + 0x28) == 99810 && rd32(row + 0x2C) == 99811;
}

// Test settings (test phase only): test-settings.toml next to the launcher, read at every build
// (each "Play with the mod"). A missing file means the defaults; the
// resolved values of all of them go to work/native/test-settings.toml, which the DLL reads.
#include "../mod/dll/settings.h"
#include "ui.h"
static short g_set[SETTINGS_COUNT];
static int setting(int i) { return g_set[i] > 0; }
static int load_settings(const char *path, short v[SETTINGS_COUNT]) {  // 0: no file
    buf t = read_file(path);
    char bad[300];
    g_pair_given = 0;
    if (!t.p) {
        for (int i = 0; i < SETTINGS_COUNT; i++) v[i] = setting_default(i);
        return 0;
    }
    if (settings_parse((const char *)t.p, t.n, v, bad, sizeof bad)) logf_("test-settings.toml: ignored: %s", bad);
    g_pair_given = v[S_MAP] >= 1 && v[S_MAP] <= 4;
    for (int i = 0; i < SETTINGS_COUNT; i++) if (v[i] < 0) v[i] = setting_default(i);
    free(t.p);
    return 1;
}
static int read_settings(void) {
    char path[1300], on[200] = "";
    snprintf(path, sizeof path, "%s" SEP "test-settings.toml", g_app);
    int found = load_settings(path, g_set);
    g_set[S_DEBUG_LOG] = (short)core_load_debug();  // the window's "Debug log" box decides
    // ponytail: rand() seeded from the clock and a stack address; an OS random source if the draw ever needs more.
    // The map: test-settings.toml's map = 1..4 (owner, 2026-11-19), else drawn at each start, each with the same chance.
    if (!g_pair_given) {
        srand((unsigned)time(NULL) ^ (unsigned)clock() ^ (unsigned)(uintptr_t)&found);
        g_set[S_MAP] = (short)(rand() % 4 + 1);
    }
    if (g_pair_given) logf_("Map this start: %s (set in test-settings.toml: map = %d)", PAIR_NAME[g_set[S_MAP] - 1][0], g_set[S_MAP]);
    else logf_("Map this start: %s (drawn at random)", PAIR_NAME[g_set[S_MAP] - 1][0]);
    for (int i = 0; i < SETTINGS_COUNT; i++)
        if (g_set[i]) snprintf(on + strlen(on), sizeof on - strlen(on), " %s", SETTING_NAMES[i]);
    logf_(found ? "Test settings:%s" : "Test settings:%s (no test-settings.toml next to the launcher)", on[0] ? on : " none");
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "test-settings.toml", g_app);
    make_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) return logf_("cannot write %s", path), 0;
    fprintf(f, "# Written by the launcher from test-settings.toml at every start; changes here are overwritten.\n");
    for (int i = 0; i < SETTINGS_COUNT; i++)
        if (setting_is_number(i)) fprintf(f, "%s = %d\n", SETTING_NAMES[i], g_set[i]);
        else fprintf(f, "%s = %s\n", SETTING_NAMES[i], g_set[i] ? "true" : "false");
    fclose(f);
    return 1;
}
#define TWO_PATTERNS 320
#define TWO_PATTERNS_ TWO_PATTERNS
// Every expedition map is a Mountaintop + Rotted Woods one (decision 35; 09 §12-18). Each region is a set of
// whole map tiles of its own (m60_XX_YY_V0, V = 1 Mountaintop, 3 Rotted Woods), and the two share no tile. The
// game picks a tile's variant per tile (CalcGetReplaceMapIdByDisaster 0x1406ab7a0: the first flag in gm +0xbb8
// whose variant the tile has); the map request puts the record's last region flag (7601) there, the DLL adds
// 7603 and gives the woods tiles their own flag space. The launcher builds the rest, for every group g:
//   slots 40g+0..19 (plain set) and 40g+20..24 (Mountaintop set) hold the 25 compositions of a Mountaintop
//   pattern 40g+20+i with a Rotted Woods pattern 40g+30+j, each once: slot 40g+20+i takes (i, i); slot
//   40g+n, n < 20, the n-th pair with i != j in (i, j) order. Only those two sets are drawn (weights 20 : 5
//   per pattern); Crater, Rotted Woods and Noklateo sets weigh 0 (the Rotted Woods patterns stay as sources).
// one-region (measurement only): 1..5 = a real one-region map of that region digit (1 Mountaintop, 3 Rotted Woods):
// no pattern merge, the game's own patterns and weights, only that region's sets drawn; 0 = the two-region map.
static int one_region(void) { int r = g_set[S_ONE_REGION]; return r >= 1 && r <= 5 ? r : 0; }
static int two_regions_wanted(void) { return !one_region(); }
static int map_draw_group(void) { int g = g_set[S_MAP_GROUP]; return g >= 1 && g <= 8 ? g - 1 : -1; }
static int merged_slot(int pat);
// Regions and pairs (pair = 0 the released Mountaintop + Rotted Woods; 1..3 test pairs, dist/pair-test). A region:
// its patterns' position in each night-boss group of 40, map type (760x - 7600 = set digit = generator region
// argument), region flag, area code of SmallBaseAndSpotAttachPoint +0xc, field tiles (0x1406ab3c0), name.
typedef struct { int off, kind, flag, area, x0, x1, z0, z1; const char *name, *of, *tiles; } region_t;
static const region_t R_MT = {20, 1, 7601, 11, 42, 43, 38, 39, "Mountaintop", "Mountaintop's", "Mountaintop"},
                      R_CR = {25, 2, 7602, 12, 43, 44, 38, 39, "Crater", "Crater's", "Crater"},
                      R_RW = {30, 3, 7603, 13, 44, 45, 36, 37, "Rotted Woods", "Rotted Woods'", "woods"},
                      R_NK = {35, 5, 7605, 15, 42, 43, 36, 37, "Noklateo", "Noklateo's", "Noklateo"};
// A pair: base region a (its patterns are the drawn slots' sources and its set is drawn; map type a), added region
// b (its tiles take b's rows; 44_37, which the game gives to Noklateo, keeps a's small bases), start group
// (SortieStartingPoint +0xc for set 190; its exclusion column +0x10 must name neither region), whether b's
// tree-site rows (set 500) are copied, and the far halves to compose (west 10_09 / east 11_09: a's variant file
// with b's tiles of that half taken from b's).
typedef struct { const region_t *a, *b; int start, tree, halves; } two_pair_t;  // halves: bit 0 west, bit 1 east
static const two_pair_t TWO_PAIRS[4] = {{&R_MT, &R_RW, 701, 1, 0}, {&R_MT, &R_NK, 708, 0, 1}, {&R_CR, &R_RW, 701, 1, 2}, {&R_CR, &R_NK, 704, 0, 3}};
static int pair_now(void) { int m = g_set[S_MAP]; return m >= 2 && m <= 4 ? m - 1 : 0; }  // map 1..4 -> pair 0..3
static const two_pair_t *two_pair(void) { return &TWO_PAIRS[pair_now()]; }
static const region_t *pair_a(void) { return two_pair()->a; }
static const region_t *pair_b(void) { return two_pair()->b; }
// the drawn slots: plain 40g+0..19 and the base region's 40g+a..a+4
static int merged_slot(int pat) { return pat >= 0 && pat < TWO_PATTERNS_ && (pat % 40 < 20 || (pat % 40 >= pair_a()->off && pat % 40 < pair_a()->off + 5)); }
static int a_slot(int pat) { return pat >= 0 && pat < TWO_PATTERNS && pat % 40 >= pair_a()->off && pat % 40 < pair_a()->off + 5; }
static void merge_sources(int slot, int *a_pat, int *b_pat) {
    int g = slot / 40, n = slot % 40, i = 0, j = 0;
    if (n >= 20) i = j = n - pair_a()->off;
    else {
        for (int k = 0, at = 0; k < 25; k++)
            if (k / 5 != k % 5 && at++ == n) { i = k / 5, j = k % 5; break; }
    }
    *a_pat = 40 * g + pair_a()->off + i, *b_pat = 40 * g + pair_b()->off + j;
}

// game's regulation.bin -> work/build/regulation.bin with our Target list row.
// Map patterns: MapPatternSet rows below 1000 hold, in id order, how many patterns each set has (+4):
// patterns 0..319, 40 per night boss group (set ids g0..g5: tens digit the group, last digit the region:
// 0 plain, 1 Mountaintop, 2 Crater, 3 Rotted Woods, 5 Noklateo). Per group: 0..19 plain, 20..24
// Mountaintop, 25..29 Crater, 30..34 Rotted Woods, 35..39 Noklateo. Every table keyed by the pattern
// number (patternId +4: LotResultMapPatternFlag, LotResultPlayAreaParam, LotResultSmallBaseAndSpot) is
// read in row order, pattern by pattern (one cursor through the small bases, 0x1406ac1d6), so a
// pattern's rows stay together.
// The generator (0x1406b2aa0, at SingleSetup) draws the expedition's set: candidates are the sets with a
// weight (+0, or the u16 at +0xe in one mode) whose region digit is plain or the region the player
// chose (the generator's rare-map-layer argument); one is drawn by weight; then one of its patterns.
// Everything it derives from the pattern then belongs to that pattern (map test runs 30..36: forcing
// gm +0xb48 afterwards, even to the game's own pattern 20, left items unpickable all expedition).
// So the map tests let the generator draw natively: the DLL passes the Mountaintop (1) as the region
// argument, and the launcher leaves a weight only on the Mountaintop sets (x1; all others 0):
//   map-test-control: that alone (the game's own Mountaintop patterns 20..24, 60..64, ...);
//   two-regions: the same, and each Mountaintop pattern 40g+20+k gets, in place, the Rotted Woods of
//     pattern 40g+30+k (same group, same position):
//     LotResultMapPatternFlag: its rows of modifier sets the Mountaintop pattern lacks (500: the Rotted
//       Woods' event flag). Not its map-type row (13 / 7603; with two the map type was the Rotted Woods'
//       next to the Mountaintop's play area, runs 33/34); the DLL adds 7603 for the tiles and scripts;
//     LotResultPlayAreaParam: the Mountaintop's (room for one region's play area; the Rotted Woods'
//       1021 / 1022 is left out);
//     LotResultSmallBaseAndSpot: the Mountaintop's rows except at points in the Rotted Woods' area
//       (removed), plus the Rotted Woods pattern's rows at points the Mountaintop pattern leaves empty
//       and not in the Mountaintop's area (see attach_points).
// Ids of rows after an insertion move up just enough (copy_pattern_rows); contents stay.
#define ATTACH_MAX 4096
static u64 param_row_size(buf p) { return p.n > 0x40 + 48 ? rd64(p.p + 0x40 + 24 + 8) - rd64(p.p + 0x48) : 0; }
// Rows of pattern src (filter: keep row?) copied as pattern dst; 0 if a step fails. The game reads these
// tables in row order, pattern by pattern (0..319, 1000..; one cursor through LotResultSmallBaseAndSpot,
// 0x1406ac1d6 ff.), so a pattern's rows must stay together and in pattern order: the copies go right
// after the last row of a pattern <= dst, and if the ids there are taken, the following rows' ids move
// up just enough (in the tables seen, only patterns 21..319 for an in-place pattern 20; their ids are
// not referenced anywhere we know of).
static int copy_pattern_rows(buf *p, int src, int dst, int (*keep)(const u8 *row, void *ctx), void *ctx) {
    u64 size = param_row_size(*p);
    u32 count = rd16(p->p + 0x0A), after = 0;
    int n = 0, have_after = 0;
    for (u32 i = 0; i < count; i++) {
        const u8 *row = p->p + rd64(p->p + 0x40 + i * 24 + 8);
        if ((int)rd32(row + 4) == src && (!keep || keep(row, ctx))) n++;
        if ((int)rd32(row + 4) <= dst) after = i, have_after = 1;
    }
    if (!n || n > 512) return 0;
    // no row of a pattern <= dst (dst 0 after its own rows were removed): the copies go first, ids from 0
    int first = have_after ? (int)rd32(p->p + 0x40 + after * 24) + 1 : 0, need = first + n;
    for (u32 i = have_after ? after + 1 : 0; i < count; i++) {  // make room: ids first .. first + n - 1
        int id = (int)rd32(p->p + 0x40 + i * 24);
        if (id >= need) break;
        wr32(p->p + 0x40 + i * 24, (u32)need);
        need++;
    }
    int ids[512], k = 0;
    for (u32 i = 0; i < count && k < n; i++) {
        const u8 *d = p->p + 0x40 + i * 24, *row = p->p + rd64(d + 8);
        if ((int)rd32(row + 4) == src && (!keep || keep(row, ctx))) ids[k++] = (int)rd32(d);
    }
    for (int i = 0; i < n; i++) {
        u8 *row = NULL;
        buf next = param_insert_sorted(*p, ids[i], first + i, size, &row);
        if (!next.p) return 0;
        wr32(row + 4, (u32)dst);
        free(p->p);
        *p = next;
    }
    return n;
}
static int keep_map_type_row(const u8 *row, void *ctx) { (void)ctx; return rd32(row + 8) == 0 && rd32(row + 0x10) >= 7600 && rd32(row + 0x10) <= 7605; }
static int keep_tree_row(const u8 *row, void *ctx) { (void)ctx; return rd32(row + 8) == 500 && rd32(row + 0x10) != 0; }
static buf param_remove_rows(buf p, int (*drop)(const u8 *row, void *ctx), void *ctx, int *removed);
static int drop_pattern(const u8 *row, void *ctx) { return (int)rd32(row + 4) == *(int *)ctx; }
static int remove_pattern(buf *p, int pat) {  // all rows of pattern pat; 0 if the table is not plain
    int removed = 0;
    buf less = param_remove_rows(*p, drop_pattern, &pat, &removed);
    if (!less.p) return 0;
    free(p->p), *p = less;
    return 1;
}
// p without the rows for which drop(row) is true; 0 if p is not a plain fixed-size param.
static buf param_remove_rows(buf p, int (*drop)(const u8 *row, void *ctx), void *ctx, int *removed) {
    buf out = {0};
    u32 strings_off = rd32(p.p), count = rd16(p.p + 0x0A);
    u64 size = param_row_size(p), data_start = rd64(p.p + 0x48);
    if (!size || data_start != 0x40 + (u64)count * 24 || strings_off != data_start + count * size) return out;
    u32 keep = 0;
    for (u32 i = 0; i < count; i++) keep += !drop(p.p + data_start + i * size, ctx);
    u64 shift = (u64)(count - keep) * (24 + size), new_start = 0x40 + (u64)keep * 24;
    out.n = p.n - shift;
    out.p = xalloc(out.n);
    memcpy(out.p, p.p, 0x40);
    wr32(out.p, (u32)(strings_off - shift));
    wr16(out.p + 0x0A, (uint16_t)keep);
    wr64(out.p + 0x10, rd64(p.p + 0x10) - shift);
    wr64(out.p + 0x30, new_start);
    for (u32 i = 0, j = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24, *row = p.p + data_start + i * size;
        if (drop(row, ctx)) continue;
        u8 *nd = out.p + 0x40 + j * 24;
        wr32(nd, rd32(d));
        wr32(nd + 4, 0);
        wr64(nd + 8, new_start + j * size);
        wr64(nd + 16, rd64(d + 16) - shift);
        memcpy(out.p + new_start + j * size, row, size);
        j++;
    }
    memcpy(out.p + strings_off - shift, p.p + strings_off, p.n - strings_off);
    *removed = (int)(count - keep);
    return out;
}
// Map type of each pattern 0..319 (0 plain, 1..5 the region) from its modifier set 0 row.
static void pattern_kinds(buf flags, signed char kind[TWO_PATTERNS]) {
    memset(kind, -1, TWO_PATTERNS);
    for (u32 i = 0; i < rd16(flags.p + 0x0A); i++) {
        const u8 *row = flags.p + rd64(flags.p + 0x40 + i * 24 + 8);
        int pat = (int)rd32(row + 4);
        if (pat >= 0 && pat < TWO_PATTERNS && keep_map_type_row(row, NULL)) kind[pat] = (signed char)(rd32(row + 0x10) - 7600);
    }
}
static int two_pairs_ok(const signed char kind[TWO_PATTERNS]) {  // 40g+20..24 Mountaintop, 40g+30..34 Rotted Woods
    for (int g = 0; g < 8; g++)
        for (int k = 0; k < 5; k++)
            if (kind[40 * g + pair_a()->off + k] != pair_a()->kind || kind[40 * g + pair_b()->off + k] != pair_b()->kind) return 0;
    return 1;
}
#define SPOT_PAIRS_MAX 2048
typedef struct {
    u8 gone_a[ATTACH_MAX], gone_b[ATTACH_MAX], a_has[ATTACH_MAX], woods[ATTACH_MAX];
    int pairs;                                    // attach points on one physical spot that the game never combines
    unsigned short pa[SPOT_PAIRS_MAX], pb[SPOT_PAIRS_MAX];
} attach_sets;
// Small-map instances (+0xc smallBaseMapId, +0x10 u8 mapIndex): in the game's 320 patterns no small map is placed
// twice with the same index; a second copy gets 1, a third 2 (m30_30 #0 / #1, m41_00 #2). The index chooses the
// instance's own flag block (index * 10^8 + AABB * 10^4 + d * 1000 + nnn): boss death nnn 800, which the scripts
// (common 9005810 / 90015006) wait for to enable the site of grace nnn 1500. Our merge can bring the same map with
// index 0 from both sources (pattern 8: m46_68 from Mountaintop 22 at point 756 and from Rotted Woods 30 at 315), so
// both copies would share one block and one set of entities. A repeat gets the lowest index its map does not have in
// that pattern, never above the highest index the game itself gives that map.
#define DROPPED_PATTERN 0x7fffffff  // distinct_instances: a row to remove
static int distinct_instances(buf p) {  // returns how many rows it marked for removal
    static u8 top[10000];
    memset(top, 0, sizeof top);
    u32 count = rd16(p.p + 0x0A);
    for (u32 i = 0; i < count; i++) {
        const u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
        u32 m = rd32(row + 0xc);
        if (m < 10000 && row[0x10] > top[m]) top[m] = row[0x10];
    }
    int moved = 0, stuck = 0;
    for (int slot = 0; slot < TWO_PATTERNS; slot++) {
        if (!merged_slot(slot)) continue;
        static u8 used[10000][16];
        memset(used, 0, sizeof used);
        for (u32 i = 0; i < count; i++) {
            u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            u32 m = rd32(row + 0xc);
            if ((int)rd32(row + 4) != slot || m >= 10000 || row[0x10] >= 16) continue;
            if (!used[m][row[0x10]]) { used[m][row[0x10]] = 1; continue; }
            int k = 0;
            while (k < 16 && used[m][k]) k++;
            if (k < 16 && k <= top[m]) row[0x10] = (u8)k, used[m][k] = 1, moved++;
            else {  // more copies than the game gives that map indexes (pair 2, pattern 200: m45_55 nine times, 0..7): dropped
                if (stuck++ < 4) logf_("two-regions: pattern %d: small map m%02u_%02u placed %d times, the game gives it index 0..%d only: "
                                       "this copy dropped", slot, m / 100, m % 100, k + 1, top[m]);
                wr32(row + 4, (u32)DROPPED_PATTERN);
            }
        }
    }
    logf_("two-regions: small maps placed twice in one pattern with one index: %d given the next free index, %d dropped (no index the "
          "game uses is free)", moved, stuck);
    return stuck;
}
// Per-tile composition (user's method, analysis/merge/compose.py): the four tiles with Rotted Woods terrain
// (44..45 x 36..37) take their whole content from the Rotted Woods pattern (40g+30+k), every other tile from
// the Mountaintop pattern (40g+20+k); no tile mixes rows of both. (A plain pattern of the same index is no
// base: patterns of one index share at most 13 % of their small bases.) In the game's tables a Mountaintop
// pattern's rows on those tiles are exactly its rows at Rotted Woods-area points (+0xc == 13), checked below.
static int drop_gone_b(const u8 *row, void *ctx) {  // a Mountaintop pattern's small base on a woods tile
    attach_sets *s = ctx;
    int pat = (int)rd32(row + 4);
    u32 at = rd32(row + 8);
    return a_slot(pat) && at < ATTACH_MAX && s->woods[at];
}
static int spot_taken(const attach_sets *s, u32 at, const u8 *has) {  // another point of at's spot is in has
    for (int i = 0; i < s->pairs; i++)
        if ((s->pa[i] == at && has[s->pb[i]]) || (s->pb[i] == at && has[s->pa[i]])) return 1;
    return 0;
}
static int keep_b_base(const u8 *row, void *ctx) {  // a Rotted Woods pattern's small base on a woods tile
    attach_sets *s = ctx;
    u32 at = rd32(row + 8);
    return at < ATTACH_MAX && s->woods[at] && !s->a_has[at] && !spot_taken(s, at, s->a_has);
}
// Physical spots (decision 31): several attach points share one position (e.g. 100 / 122, 111 / 129,
// 301 / 351, 314 / 364: same tile, same x y z), and no game pattern uses two of them (run 47: our merge
// put a Rotted Woods base on the alias of a spot the Mountaintop base already held: two bases in one
// place). A pair counts when both points are on the same tile, under 1 m apart (SmallBaseAndSpotAttachPoint
// +0x38 / +0x3c / +0x40), and never in one of the game's patterns 0..319 together; points the game does
// combine at one position (a base and its "spot", e.g. 305 + 755) stay allowed.
static int spot_pairs(buf points, buf bases, attach_sets *s) {
    static u32 tile[ATTACH_MAX];
    static float xyz[ATTACH_MAX][3];
    static u8 have[ATTACH_MAX], in[TWO_PATTERNS][ATTACH_MAX / 8];
    memset(have, 0, sizeof have);
    memset(in, 0, sizeof in);
    s->pairs = 0;
    if (param_row_size(points) < 0x44) return 0;
    for (u32 i = 0; i < rd16(points.p + 0x0A); i++) {
        u32 id = rd32(points.p + 0x40 + i * 24);
        const u8 *row = points.p + rd64(points.p + 0x40 + i * 24 + 8);
        if (id >= ATTACH_MAX) continue;
        have[id] = 1, tile[id] = rd32(row + 4);
        for (int k = 0; k < 3; k++) memcpy(&xyz[id][k], row + 0x38 + 4 * k, 4);
    }
    for (u32 i = 0; i < rd16(bases.p + 0x0A); i++) {
        const u8 *row = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
        int pat = (int)rd32(row + 4);
        u32 at = rd32(row + 8);
        if (pat >= 0 && pat < TWO_PATTERNS && at < ATTACH_MAX) in[pat][at / 8] |= (u8)(1 << (at % 8));
    }
    for (u32 a = 0; a < ATTACH_MAX; a++)
        for (u32 b = a + 1; have[a] && b < ATTACH_MAX; b++) {
            if (!have[b] || tile[a] != tile[b]) continue;
            float dx = xyz[a][0] - xyz[b][0], dy = xyz[a][1] - xyz[b][1], dz = xyz[a][2] - xyz[b][2];
            if (dx * dx + dy * dy + dz * dz >= 1.0f) continue;
            int together = 0;
            for (int pat = 0; pat < TWO_PATTERNS && !together; pat++)
                together = (in[pat][a / 8] >> (a % 8) & 1) && (in[pat][b / 8] >> (b % 8) & 1);
            if (!together && s->pairs < SPOT_PAIRS_MAX) s->pa[s->pairs] = (unsigned short)a, s->pb[s->pairs] = (unsigned short)b, s->pairs++;
        }
    logf_("two-regions: %d pairs of attach points on one spot that the game never combines", s->pairs);
    return s->pairs > 0;
}
// Check (decision 31): no merged pattern holds two attach points of one spot. Returns how many do.
static int spot_doubles(buf bases, const attach_sets *s) {
    static u8 has[ATTACH_MAX];
    int bad = 0;
    for (int pat = 0; pat < TWO_PATTERNS; pat++) {
        if (!merged_slot(pat)) continue;
            memset(has, 0, sizeof has);
            for (u32 i = 0; i < rd16(bases.p + 0x0A); i++) {
                const u8 *row = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
                if ((int)rd32(row + 4) == pat && rd32(row + 8) < ATTACH_MAX) has[rd32(row + 8)] = 1;
            }
            for (int i = 0; i < s->pairs; i++)
                if (has[s->pa[i]] && has[s->pb[i]]) bad++, logf_("two-regions: check: pattern %d has attach points %u and %u on one spot", pat, s->pa[i], s->pb[i]);
        }
    return bad;
}

// Which attach points lie in a region's area: SmallBaseAndSpotAttachPoint +0xc holds the map type
// (11 Mountaintop, 12 Crater, 13 Rotted Woods, 15 Noklateo, 0 none) whose area holds the point; a
// region's own patterns never use the points in its area (checked on the tables: 0 of 80 / 66 / 104 /
// 112), the other patterns do. (Run 37 used a usage statistic instead and kept 32 Mountaintop small
// bases inside the Rotted Woods.)
// The added region's field tiles for its small bases (Noklateo without 44_37: see the pair table).
static int two_added_tile(int x, int z) {
    const region_t *b = pair_b();
    return x >= b->x0 && x <= b->x1 && z >= b->z0 && z <= b->z1;
}
static int attach_points(buf points, attach_sets *s) {
    memset(s, 0, sizeof *s);
    int in_a = 0, in_b = 0;
    if (param_row_size(points) < 0x10) return 0;
    for (u32 i = 0; i < rd16(points.p + 0x0A); i++) {
        u32 id = rd32(points.p + 0x40 + i * 24);
        const u8 *row = points.p + rd64(points.p + 0x40 + i * 24 + 8);
        if (id >= ATTACH_MAX) continue;
        if (rd32(row + 0xc) == (u32)pair_a()->area) s->gone_a[id] = 1, in_a++;
        if (rd32(row + 0xc) == (u32)pair_b()->area) s->gone_b[id] = 1, in_b++;
        s->woods[id] = row[4] == 60 && two_added_tile(row[5], row[6]);  // +4 area, +5 X, +6 Z
    }
    logf_("two-regions: attach points in the %s area %d, in the %s %d", pair_a()->of, in_a, pair_b()->of, in_b);
    return in_a > 0 && in_b > 0;
}
// Churches (SmallBaseAndSpotDefine rows with map icon 3, "Church": +0x2c; their small-base ids as
// int16s at +4..+0x2b) per two-regions pattern -> work/native/two-regions.txt ("pattern churches"),
// which the DLL reads for decision 26 (more HP flask charges at the start).
// Every church of the composed pattern counts (run 49: church 104 in Noklateo's area stands on pattern 22).
// Also one line per church of each pattern: "church <pattern> <attach point> <area> <X> <Z> <x> <z>" (the
// point's tile +4..+6 and position +0x38 / +0x40 in SmallBaseAndSpotAttachPoint), for the DLL's debug log.
static int write_churches(buf define, buf bases, buf points) {
    short church[64];
    int nc = 0;
    u64 dsize = param_row_size(define);
    if (dsize < 0x30) return 0;
    for (u32 i = 0; i < rd16(define.p + 0x0A); i++) {
        const u8 *row = define.p + rd64(define.p + 0x40 + i * 24 + 8);
        if (rd32(row + 0x2c) != 3) continue;
        for (int k = 0; k < 20 && nc < 64; k++) {
            short v = (short)rd16(row + 4 + 2 * k);
            if (v != -1) church[nc++] = v;
        }
    }
    int count[TWO_PATTERNS] = {0};
    for (u32 i = 0; i < rd16(bases.p + 0x0A); i++) {
        const u8 *row = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
        int pat = (int)rd32(row + 4), m = (int)rd32(row + 0xc);
        if (pat < 0 || pat >= TWO_PATTERNS) continue;
        for (int k = 0; k < nc; k++)
            if (church[k] == m) {
                count[pat]++;
                break;
            }
    }
    char path[1300], line[64];
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "two-regions.txt", g_app);
    make_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Written by the launcher: two-regions pattern, churches on it.\n");
    int lo = 99, hi = 0;
    for (int pat = 0; pat < TWO_PATTERNS; pat++) {
        if (!merged_slot(pat)) continue;
            snprintf(line, sizeof line, "%d %d\n", pat, count[pat]);
            fputs(line, f);
            if (count[pat] < lo) lo = count[pat];
            if (count[pat] > hi) hi = count[pat];
        }
    for (u32 i = 0; param_row_size(points) >= 0x44 && i < rd16(bases.p + 0x0A); i++) {
        const u8 *row = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
        int pat = (int)rd32(row + 4), m = (int)rd32(row + 0xc);
        u32 at = rd32(row + 8);
        int is_church = 0;
        for (int k = 0; k < nc && !is_church; k++) is_church = church[k] == m;
        if (pat < 0 || pat >= TWO_PATTERNS || !merged_slot(pat) || !is_church) continue;
        for (u32 j = 0; j < rd16(points.p + 0x0A); j++) {
            if (rd32(points.p + 0x40 + j * 24) != at) continue;
            const u8 *pt = points.p + rd64(points.p + 0x40 + j * 24 + 8);
            float x, z;
            memcpy(&x, pt + 0x38, 4), memcpy(&z, pt + 0x40, 4);
            fprintf(f, "church %d %u %u %u %u %d %d\n", pat, at, pt[4], pt[5], pt[6], (int)x, (int)z);
            break;
        }
    }
    fclose(f);
    logf_("two-regions: churches on the 200 merged patterns: %d..%d (%d church small-base ids)", lo, hi, nc);
    char per[200 * 9] = "";
    for (int pat = 0; pat < TWO_PATTERNS; pat++)
        if (merged_slot(pat)) snprintf(per + strlen(per), sizeof per - strlen(per), " %d:%d", pat, count[pat]);
    logf_("two-regions: churches per pattern:%s", per);
    return nc > 0;
}
// Night boss arenas (run 44, pattern 19: day-1 site 1009 on 45_36, its arena under the woods' terrain, 7501 never
// came). LotResultPlayAreaParam (row id = pattern): +8 / +0xc the day-1 / day-2 site (PlayAreaCreateParam row),
// +0x10 / +0x12 the night bosses = their arena small maps (4924 = m49_24 ...; SmallBaseMapVariationParam row of that
// id, its modifier = +0x18 / +0x1c), so the boss line-up stays with the row and only the site moves. A site
// (PlayAreaCreateParam): +4..+6 tile, +8 / +0xc position, +0x10 / +0x14 day flags, +0x18 required map type,
// +0x1c / +0x20 excluded map types, +0x24 the arena's attach point (same tile). The arena small map is placed for
// the terrain of its source's map, so on a merged map a site may not lie on the added region's tiles (Noklateo:
// with 44_37, which the DLL gives Noklateo's variant) and may not be one the game excludes for either region.
// A slot's site that fails is replaced, per day, by the same day's site of the base region's patterns of the
// same night boss group (40g + off .. +4, in order), then of the group's other patterns, always a site the base
// region's own patterns use that day (so its terrain is the one that map shows); never the other day's.
static buf g_pac;  // PlayAreaCreateParam (map_tables)
static const u8 *pac_row(int id) {
    for (u32 i = 0; g_pac.p && i < rd16(g_pac.p + 0x0A); i++)
        if ((int)rd32(g_pac.p + 0x40 + i * 24) == id) return g_pac.p + rd64(g_pac.p + 0x40 + i * 24 + 8);
    return NULL;
}
static int site_added_tile(int x, int z) { return two_added_tile(x, z) || (pair_b() == &R_NK && x == 44 && z == 37); }
static int site_ok(int id) {
    const u8 *r = pac_row(id);
    if (!r || param_row_size(g_pac) < 0x28 || r[4] != 60 || site_added_tile(r[5], r[6])) return 0;
    int req = (int)rd32(r + 0x18), e1 = (int)rd32(r + 0x1c), e2 = (int)rd32(r + 0x20), aa = pair_a()->area, ba = pair_b()->area;
    return e1 != aa && e1 != ba && e2 != aa && e2 != ba && (req == 0 || req == aa);
}
static int arena_sites(buf p, u8 rows[][256]) {
    int fixed[2] = {0, 0}, bad = 0, nd = 0;
    int dsite[64], dday[64], dcount[64];
    for (u32 i = 0; i < rd16(p.p + 0x0A); i++) {
        int id = (int)rd32(p.p + 0x40 + i * 24);
        if (!merged_slot(id)) continue;
        u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
        int g = id / 40;
        for (int d = 0; d < 2; d++) {
            int cur = (int)rd32(row + 8 + 4 * d), other = (int)rd32(row + 12 - 4 * d);
            if (!site_ok(cur)) {
                int pick = -1;
                for (int k = 0; k < 45 && pick < 0; k++) {
                    int pat = 40 * g + (k < 5 ? pair_a()->off + k : k - 5);
                    int c = (int)rd32(rows[pat] + 8 + 4 * d), base_uses = 0;
                    for (int q = 0; q < TWO_PATTERNS && !base_uses; q++)  // a site the base region's own maps use that day
                        base_uses = q % 40 >= pair_a()->off && q % 40 < pair_a()->off + 5 && (int)rd32(rows[q] + 8 + 4 * d) == c;
                    if (site_ok(c) && base_uses && c != other) pick = c;
                }
                if (pick < 0) { bad++, logf_("two-regions: check failed: pattern %d: no day-%d site for its arena off the %s tiles", id, d + 1, pair_b()->tiles); continue; }
                wr32(row + 8 + 4 * d, (u32)pick), fixed[d]++, cur = pick;
            }
            int k = 0;
            while (k < nd && (dsite[k] != cur || dday[k] != d)) k++;
            if (k == nd && nd < 64) dsite[nd] = cur, dday[nd] = d, dcount[nd++] = 0;
            if (k < 64) dcount[k]++;
        }
    }
    char line[1400];
    int at = 0;
    for (int k = 0; k < nd && at < (int)sizeof line - 64; k++) {  // stays inside line (snprintf returns the full length)
        const u8 *r = pac_row(dsite[k]);
        at += snprintf(line + at, sizeof line - (size_t)at, " day %d: %d (%02d_%02d) x%d;", dday[k] + 1, dsite[k], r ? r[5] : 0, r ? r[6] : 0, dcount[k]);
    }
    logf_("two-regions: arena sites: day 1 moved on %d slots, day 2 on %d (sites on the %s tiles or excluded for a region); check: %s; sites:%s",
          fixed[0], fixed[1], pair_b()->tiles, bad ? "FAILED" : "every site off those tiles", line);
    return !bad;
}
// table: 0 map flags, 1 play area (unchanged), 2 small bases, 3 MapPatternSet. flags: the game's map flag table.
static buf map_test_table(buf src, int table, buf flags, buf points, int two) {
    buf p = {xalloc(src.n), src.n}, none = {0};
    memcpy(p.p, src.p, src.n);
    u32 count = rd16(p.p + 0x0A);
    int ok = 1;
    static signed char kind[TWO_PATTERNS];
    pattern_kinds(flags, kind);
    if (table == 3 && one_region()) {  // one-region: the game's weights on that region's sets, every other set 0
        int kept = 0, zeroed = 0, group = map_draw_group(), want = one_region();
        for (u32 i = 0; i < count; i++) {
            int id = (int)rd32(p.p + 0x40 + i * 24);
            u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            if (id >= 1000) continue;
            if (id % 10 == want && (group < 0 || id / 10 == group) && rd32(row)) kept++;
            else wr32(row, 0), wr16(row + 0xe, 0), zeroed++;
        }
        ok = kept > 0;
        logf_("one-region (test): MapPatternSet: %d sets of region %d kept with the game's weights (group %d, 0 any), %d weighted 0",
              kept, want, group + 1, zeroed);
    } else if (table == 3) {  // weights below 1000: plain set (digit 0) 20 x 400, Mountaintop set (1) 5 x 400 (both modes), others 0
        int kept = 0, zeroed = 0, group = map_draw_group();  // map-group (test): only that night boss group
        for (u32 i = 0; i < count; i++) {
            int id = (int)rd32(p.p + 0x40 + i * 24);
            u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            if (id >= 1000) continue;
            int digit = id % 10, n = (int)rd32(row + 4);
            if ((digit == 0 || digit == pair_a()->kind) && (group < 0 || id / 10 == group) && n == (digit ? 5 : 20))
                wr32(row, (u32)(n * 400)), wr16(row + 0xe, (unsigned short)(n * 400)), kept++;
            else wr32(row, 0), wr16(row + 0xe, 0), zeroed++;
        }
        ok = kept == (group < 0 ? 16 : 2);
        logf_("two-regions: MapPatternSet: %d sets (plain 20 x 400, %s 5 x 400; group %d, -1 any) drawn, %d weighted 0", kept, pair_a()->name, group + 1, zeroed);
    } else if (table == 0 && two) {
        // Record rules (09 §12-14): the record's flags are the pattern's rows in row order (rows with modifier 0
        // and flag 0 left out; measurements 1-3); SetupMapPatternInfo (0x6b06f0) takes the LAST 7601..7606 of
        // them as map type and first region; OnMapEnter (0x68c990) sets every one. So a merged slot gets: a 7603
        // row first (set 500, modifier 0, as the tree rows), then all rows of its Mountaintop source (7601 stays
        // the last region flag: map type 1, region list [7601] + the DLL's 7603), then the Rotted Woods source's
        // tree-site rows (set 500). Nothing else from the Rotted Woods: sudden events (30xx), chests (800),
        // night boss (99999) and the rest are map-wide and stay the Mountaintop source's.
        ok = two_pairs_ok(kind);
        int got = 0;
        for (int slot = 0; ok && slot < TWO_PATTERNS; slot++) {  // stage each slot's Mountaintop source
            if (!merged_slot(slot)) continue;
            int a_pat, b_pat;
            merge_sources(slot, &a_pat, &b_pat);
            ok = copy_pattern_rows(&p, a_pat, 50000 + slot, NULL, NULL) > 0;
        }
        for (int slot = 0; ok && slot < TWO_PATTERNS; slot++) {
            if (!merged_slot(slot)) continue;
            int a_pat, b_pat;
            merge_sources(slot, &a_pat, &b_pat);
            ok = remove_pattern(&p, slot) && copy_pattern_rows(&p, b_pat, slot, keep_map_type_row, NULL) == 1;
            int n = ok ? copy_pattern_rows(&p, 50000 + slot, slot, NULL, NULL) : 0;
            ok = ok && n > 0 && remove_pattern(&p, 50000 + slot);
            if (ok && two_pair()->tree) n += copy_pattern_rows(&p, b_pat, slot, keep_tree_row, NULL);
            got += n;
            u32 done = 0;  // the copied Rotted Woods region row (the slot's first row): set 500, modifier 0
            for (u32 i = 0; ok && i < rd16(p.p + 0x0A); i++) {
                u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
                if ((int)rd32(row + 4) != slot) continue;
                if (!done) {
                    done = 1;
                    ok = rd32(row + 0x10) == (u32)pair_b()->flag;
                    wr32(row + 8, 500), wr32(row + 0xc, 0);
                }
            }
            // every row of the slot carries the Mountaintop source's set / boss / region values (+0x14, +0x16, +0x18)
            const u8 *ref = NULL;
            for (u32 i = 0; ok && i < rd16(p.p + 0x0A); i++) {
                const u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
                if ((int)rd32(row + 4) == slot && keep_map_type_row(row, NULL)) ref = row;
            }
            ok = ok && ref;
            for (u32 i = 0; ok && i < rd16(p.p + 0x0A); i++) {
                u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
                if ((int)rd32(row + 4) == slot) memcpy(row + 0x14, ref + 0x14, 8);
            }
        }
        // check: per merged slot the region flags in row order are 7603 then 7601 (last), and at most one
        // sudden event (sets 3000..3199) per day (7700..7719 day 1, 7720..7739 day 2)
        for (int slot = 0; ok && slot < TWO_PATTERNS; slot++) {
            if (!merged_slot(slot)) continue;
            int last = 0, saw7603 = 0, day[2] = {0, 0};
            for (u32 i = 0; i < rd16(p.p + 0x0A); i++) {
                const u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
                if ((int)rd32(row + 4) != slot) continue;
                u32 f = rd32(row + 0x10), set = rd32(row + 8);
                if (f >= 7601 && f <= 7606) last = (int)f, saw7603 |= f == (u32)pair_b()->flag;
                if (set >= 3000 && set < 3200 && f >= 7700 && f < 7740) day[f >= 7720]++;
            }
            if (last != pair_a()->flag || !saw7603 || day[0] > 1 || day[1] > 1)
                ok = 0, logf_("two-regions: check failed: pattern %d region flags end in %d (%d %s), sudden events %d / %d", slot, last, pair_b()->flag, saw7603 ? "before" : "missing", day[0], day[1]);
        }
        if (ok) logf_("two-regions: %d map flag rows in the 200 merged patterns; check: %d before %d (last) everywhere, at most one sudden event per day", got, pair_b()->flag, pair_a()->flag);
        // Start point group (modifier set 190 -> SortieStartingPoint +0xc): with the Rotted Woods added, the
        // start flight showed neither the hawk nor the character for group 708 (45_38 / 45_39, tiles that
        // then load the Rotted Woods' variant: runs 33..35, 43, 44) and was fine for group 701 (west edge,
        // 41_38 / 42_36: run 37, pattern 22). Groups 704 / 705 / 707 start inside the Rotted Woods (their
        // conflict column says 13) and 706 on a Rotted Woods variant tile (44_39). So every merged pattern
        // starts from group 701 (conflicts only with Noklateo, not on this map).
        int moved = 0;
        for (u32 i = 0; ok && i < rd16(p.p + 0x0A); i++) {
            u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            int pat = (int)rd32(row + 4);
            if (merged_slot(pat) && rd32(row + 8) == 190 && rd32(row + 0xc) != (u32)two_pair()->start)
                wr32(row + 0xc, (u32)two_pair()->start), moved++;
        }
        if (two_pair()->start == 701) logf_("two-regions: start point group set to 701 (west edge) on %d of the 200 merged patterns", moved);
        else logf_("two-regions: start point group set to %d on %d of the 200 merged patterns", two_pair()->start, moved);
    } else if (table == 1 && two) {  // each plain slot takes its Mountaintop source's play-area row (id = pattern)
        u64 size = param_row_size(p);
        int copied = 0;
        static u8 src_rows[TWO_PATTERNS][256];
        for (u32 i = 0; i < count && size <= 256; i++) {
            int id = (int)rd32(p.p + 0x40 + i * 24);
            if (id >= 0 && id < TWO_PATTERNS) memcpy(src_rows[id], p.p + rd64(p.p + 0x40 + i * 24 + 8), size);
        }
        for (u32 i = 0; ok && size <= 256 && i < count; i++) {
            int id = (int)rd32(p.p + 0x40 + i * 24), a_pat, b_pat;
            if (!merged_slot(id)) continue;
            merge_sources(id, &a_pat, &b_pat);
            if (a_pat == id) continue;
            u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            memcpy(row, src_rows[a_pat], size), wr32(row + 4, (u32)id), copied++;
        }
        ok = size <= 256 && copied == 160;
        logf_("two-regions: play-area rows of the %s sources copied into %d plain slots", pair_a()->name, copied);
        if (ok) ok = arena_sites(p, src_rows);
    } else if (table == 2 && two) {
        static attach_sets sets;
        ok = two_pairs_ok(kind) && attach_points(points, &sets) && spot_pairs(points, p, &sets);
        int removed = 0, added = 0, most = 0;
        int odd = 0;  // Mountaintop rows where "on a woods tile" and "at a Rotted Woods-area point" disagree
        for (u32 i = 0; ok && i < rd16(p.p + 0x0A); i++) {
            const u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
            int pat = (int)rd32(row + 4);
            u32 at = rd32(row + 8);
            if (a_slot(pat) && at < ATTACH_MAX && sets.woods[at] != sets.gone_b[at]) odd++;
        }
        if (odd) logf_("two-regions: check failed: %d %s small bases where %s tile and %s area disagree", odd, pair_a()->name, pair_b()->tiles, pair_b()->name), ok = 0;
        if (ok) {
            buf less = param_remove_rows(p, drop_gone_b, &sets, &removed);
            ok = less.p != NULL;
            if (ok) free(p.p), p = less;
        }
        // plain slots first (they copy their Mountaintop source while it still has only its own rows, woods rows
        // gone), then the Mountaintop slots get their Rotted Woods rows
        for (int pass = 0; ok && pass < 2; pass++)
            for (int slot = 0; ok && slot < TWO_PATTERNS; slot++) {
                if (!merged_slot(slot) || (slot % 40 >= 20) != pass) continue;
                int a_pat, b_pat, total = 0;
                merge_sources(slot, &a_pat, &b_pat);
                if (a_pat != slot)
                    ok = remove_pattern(&p, slot) && copy_pattern_rows(&p, a_pat, slot, NULL, NULL) > 0;
                memset(sets.a_has, 0, sizeof sets.a_has);
                for (u32 i = 0; i < rd16(p.p + 0x0A); i++) {
                    const u8 *row = p.p + rd64(p.p + 0x40 + i * 24 + 8);
                    if ((int)rd32(row + 4) == slot && rd32(row + 8) < ATTACH_MAX) sets.a_has[rd32(row + 8)] = 1, total++;
                }
                int n = ok ? copy_pattern_rows(&p, b_pat, slot, keep_b_base, &sets) : 0;
                added += n;
                total += n;
                if (total > most) most = total;
                ok = total < 128;  // the game reads at most 127 per pattern
            }
        logf_("two-regions: small bases per tile: %d %s ones on the %s tiles removed, %d of the %s on them added (most on one pattern %d)",
              removed, pair_a()->name, pair_b()->tiles, added, pair_b()->of, most);
        if (ok && distinct_instances(p) > 0) {
            int removed = 0, mark = DROPPED_PATTERN;
            buf less = param_remove_rows(p, drop_pattern, &mark, &removed);
            ok = less.p != NULL;
            if (ok) free(p.p), p = less;
        }
        if (ok && spot_doubles(p, &sets)) ok = 0, logf_("two-regions: check failed: two small bases on one spot");
        else if (ok) logf_("two-regions: check: no merged pattern has two small bases on one spot");
    }
    if (!ok) { free(p.p); return none; }
    return p;
}
// Check only (build log): buried-treasure chest groups whose site lies on a woods tile. common_func 90015100 called
// from common.emevd with (group flag, site flag, ...) turns every site of a group on when the group flag (a pattern
// record flag) is set; the site's base-terrain map row (WorldMapPointParam, eventFlagId1 = site) gives its tile. Of the
// groups 8000..8011, these have one site on 44-45 x 36-37 (our own table, from those scripts and rows): the group
// keeps its other sites on a merged map, so it is kept; the line only counts the slots that carry such a group.
static const int CHEST_WOODS[][3] = {{8000, 9111, 4436}, {8001, 9110, 4537}, {8004, 9110, 4537},
                                     {8005, 9111, 4436}, {8010, 9110, 4537}, {8011, 9111, 4436}};
static void chest_group_report(buf flags) {
    int per[6] = {0}, slots = 0;
    u32 count = rd16(flags.p + 0x0A);
    static unsigned char seen[TWO_PATTERNS];
    memset(seen, 0, sizeof seen);
    for (u32 i = 0; i < count; i++) {
        const u8 *row = flags.p + rd64(flags.p + 0x40 + i * 24 + 8);
        int pat = (int)rd32(row + 4), flag = (int)rd32(row + 0x10);
        if (!merged_slot(pat)) continue;
        for (int g = 0; g < 6; g++)
            if (flag == CHEST_WOODS[g][0]) per[g]++, slots += !seen[pat], seen[pat] = 1;
    }
    char line[400];
    int at = snprintf(line, sizeof line, "two-regions: check: chest groups with a site on a woods tile (kept, the other sites stay): %d merged slots;", slots);
    for (int g = 0; g < 6; g++)
        at += snprintf(line + at, sizeof line - (size_t)at, " %d (site %d on %d_%d) x%d", CHEST_WOODS[g][0], CHEST_WOODS[g][1],
                       CHEST_WOODS[g][2] / 100, CHEST_WOODS[g][2] % 100, per[g]);
    logf_("%s", line);
}
// The map-test tables, changed (maps[t].p stays empty for a table left alone); 0 if one fails.
// Rows added to a pattern-keyed table (rows of one pattern stay together, in the game's row order): each extra
// row goes after the last row of its pattern; ids are kept and moved up just enough to stay increasing (as
// copy_pattern_rows; the ids of these tables are not referenced anywhere we know of). The extra rows' name
// offsets are those of the row they follow. Empty buf if the table is not a plain fixed-size param.
typedef struct { int pattern; u8 data[0x20]; } extra_row;
static buf param_add_pattern_rows(buf p, const extra_row *extra, int n_extra) {
    buf out = {0};
    u32 strings_off = rd32(p.p), count = rd16(p.p + 0x0A);
    u64 size = param_row_size(p), data_start = rd64(p.p + 0x48);
    if (!size || size > 0x20 || data_start != 0x40 + (u64)count * 24 || strings_off != data_start + count * size || count + n_extra > 0xffff)
        return out;
    u32 total = count + (u32)n_extra;
    u64 shift = (u64)n_extra * (24 + size), new_start = 0x40 + (u64)total * 24;
    out.n = p.n + shift;
    out.p = xalloc(out.n);
    memcpy(out.p, p.p, 0x40);
    wr32(out.p, (u32)(strings_off + shift));
    wr16(out.p + 0x0A, total);
    wr64(out.p + 0x10, rd64(p.p + 0x10) + shift);
    wr64(out.p + 0x30, new_start);
    char *used = xalloc((size_t)n_extra + 1);
    memset(used, 0, (size_t)n_extra + 1);
    u32 j = 0;
    int last_id = -1;
    for (u32 i = 0; i < count; i++) {
        const u8 *d = p.p + 0x40 + i * 24, *row = p.p + rd64(d + 8);
        int id = (int)rd32(d) > last_id ? (int)rd32(d) : last_id + 1, pat = (int)rd32(row + 4);
        u8 *e = out.p + 0x40 + j * 24;
        wr32(e, (u32)id), wr32(e + 4, 0), wr64(e + 8, new_start + j * size), wr64(e + 16, rd64(d + 16) + shift);
        memcpy(out.p + new_start + j * size, row, size);
        j++, last_id = id;
        int next_pat = i + 1 < count ? (int)rd32(p.p + rd64(p.p + 0x40 + (i + 1) * 24 + 8) + 4) : -1;
        if (next_pat == pat) continue;
        for (int k = 0; k < n_extra; k++) {
            if (used[k] || extra[k].pattern != pat) continue;
            used[k] = 1, last_id++;
            e = out.p + 0x40 + j * 24;
            wr32(e, (u32)last_id), wr32(e + 4, 0), wr64(e + 8, new_start + j * size), wr64(e + 16, rd64(d + 16) + shift);
            memcpy(out.p + new_start + j * size, extra[k].data, size);
            wr32(out.p + new_start + j * size + 4, (u32)pat);
            j++;
        }
    }
    free(used);
    if (j != total) return free(out.p), (buf){0};  // an extra row for a pattern the table does not have
    memcpy(out.p + strings_off + shift, p.p + strings_off, p.n - strings_off);
    return out;
}

// Day flow, the map tables (after the two-region merge):
// - MapPatternSet +8 / +0xa / +0xc: the weights of 0 / 1 / 2 camps with a Nightfarer-like invader (rolled once in
//   the generator: 0x1406bb760 from 0x1406aef1a, RNG 0x140f563c0; params by index 0xc6 SmallBaseAndSpotDefine, 0xc9
//   SmallbaseInvationNpcParam). Every set of the 320 patterns: (0, 0, 100), always two (the DLL opens them one per
//   boss).
// - LotResultMapPatternFlag (0x1c: +4 pattern, +8 set, +0xc modifier, +0x10 event flag, +0x14..+0x1b the pattern's
//   set / night boss / region, as its own rows): every composition gets the day-1 flag of each event family it
//   lacks, so the family's handler (common 1121 meteorite, 1124 hordes, 1127 frenzy, 1125 the special one) does not
//   end at map load; the DLL then starts them one by one (8040, 8055, 8060, 8071). Only day-1 flags: a handler
//   with both day flags waits for day 2 (7505) first (1121 / 1124: SKIP IF 7701 off / IF 7500; SKIP IF 7721 off /
//   IF 7505). A composition without a special event gets one kind (pattern % 4): its kind row (set 0, modifier
//   601..604, flag 8075..8079, as the game's) and its small map (m46_78 Fell Omen, m45_52 bubbles, m45_53 locusts,
//   m45_55 curse) on up to 8 free attach points among those the game uses for it, outside both regions' tiles and
//   area codes (LotResultSmallBaseAndSpot 0x18: +4 pattern, +8 attach point, +0xc small map, +0x10 instance).
static const struct { int flag, modifier, map; } DF_KINDS[4] = {{8075, 604, 4678}, {8076, 603, 4552}, {8078, 601, 4553}, {8079, 602, 4555}};
static const struct { int set, flag, flag2; } DF_FAMILIES[3] = {{3010, 7701, 7721}, {3030, 7704, 7724}, {3080, 7707, 7727}};
static int point_outside_regions(buf points, int at) {
    for (u32 i = 0; i < rd16(points.p + 0x0A); i++) {
        if ((int)rd32(points.p + 0x40 + i * 24) != at) continue;
        const u8 *r = points.p + rd64(points.p + 0x40 + i * 24 + 8);
        int code = (int)rd32(r + 0xc), area = r[4], x = r[5], z = r[6];
        const region_t *g[2] = {pair_a(), pair_b()};
        for (int k = 0; k < 2; k++)
            if (code == g[k]->area || (area == 60 && x >= g[k]->x0 && x <= g[k]->x1 && z >= g[k]->z0 && z <= g[k]->z1)) return 0;
        return 1;
    }
    return 0;
}
static int day_flow_maps(buf maps[4], buf points) {
    buf flags = maps[0], bases = maps[2], sets = maps[3];
    if (!flags.p || !bases.p || !sets.p) return 0;
    int camps = 0;
    for (u32 i = 0; i < rd16(sets.p + 0x0A); i++) {
        u8 *r = sets.p + rd64(sets.p + 0x40 + i * 24 + 8);
        if ((int)rd32(sets.p + 0x40 + i * 24) >= 1000) continue;
        wr16(r + 8, 0), wr16(r + 0xa, 0), wr16(r + 0xc, 100), camps++;
    }
    static extra_row xf[TWO_PATTERNS * 6], xb[TWO_PATTERNS * 40];
    int nf = 0, nb = 0, fam_added = 0, kinds_added = 0, sites = 0, hordes_added = 0, hordes_sites = 0;
    u64 fsize = param_row_size(flags), bsize = param_row_size(bases);
    if (fsize != 0x1c || bsize != 0x18) return 0;
    for (int pat = 0; pat < TWO_PATTERNS; pat++) {
        if (!merged_slot(pat)) continue;
        const u8 *meta = NULL;
        int has[8000 - 7700] = {0}, special = 0;
        for (u32 i = 0; i < rd16(flags.p + 0x0A); i++) {
            const u8 *r = flags.p + rd64(flags.p + 0x40 + i * 24 + 8);
            if ((int)rd32(r + 4) != pat) continue;
            if (!meta) meta = r;
            int f = (int)rd32(r + 0x10);
            if (f >= 7700 && f < 8000) has[f - 7700] = 1;
            if (f >= 8075 && f <= 8079) special = 1;
        }
        if (!meta) continue;
        for (int k = 0; k < 3; k++) {
            if (has[DF_FAMILIES[k].flag - 7700] || has[DF_FAMILIES[k].flag2 - 7700]) continue;
            u8 *d = xf[nf].data;
            memset(d, 0, sizeof xf[nf].data);
            wr32(d + 8, (u32)DF_FAMILIES[k].set), wr32(d + 0xc, 800), wr32(d + 0x10, (u32)DF_FAMILIES[k].flag);
            memcpy(d + 0x14, meta + 0x14, 8);
            xf[nf++].pattern = pat, fam_added++;
        }
        // the hordes' camps (m46_00..06): the hordes family needs them (common_func 90035040 / 90035045 / 90035042 in
        // those maps wait for 8055, then a player in the camp's area or 8056; the notice waits for 8057), and the game
        // places them (32-48, on points 615-662) only in its 16 hordes patterns. A composition without any gets one
        // camp on each free such point outside the regions, maps 4600..4606 in turn, instances counted per map.
        int hordes = 0;
        for (u32 i = 0; i < rd16(bases.p + 0x0A) && !hordes; i++) {
            const u8 *r = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
            hordes = (int)rd32(r + 4) == pat && rd32(r + 0xc) >= 4600 && rd32(r + 0xc) <= 4606;
        }
        if (!hordes && nb < TWO_PATTERNS * 40 - 64) {
            static const u8 *donor_of[7];
            int seen_pt[64] = {0}, inst[7] = {0}, placed = 0;
            for (u32 i = 0; i < rd16(bases.p + 0x0A); i++) {
                const u8 *r = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
                int m = (int)rd32(r + 0xc), at = (int)rd32(r + 8);
                if (m < 4600 || m > 4606) continue;
                if (!donor_of[m - 4600]) donor_of[m - 4600] = r;
                if (at < 615 || at > 662 || seen_pt[at - 615]) continue;
                seen_pt[at - 615] = 1;
                int taken = 0;
                for (u32 j = 0; j < rd16(bases.p + 0x0A) && !taken; j++) {
                    const u8 *q = bases.p + rd64(bases.p + 0x40 + j * 24 + 8);
                    taken = (int)rd32(q + 4) == pat && (int)rd32(q + 8) == at;
                }
                if (taken || !point_outside_regions(points, at)) continue;
                int mm = placed % 7;
                const u8 *d = donor_of[mm] ? donor_of[mm] : r;
                memset(xb[nb].data, 0, sizeof xb[nb].data);
                memcpy(xb[nb].data, d, 0x18);
                wr32(xb[nb].data + 8, (u32)at), wr32(xb[nb].data + 0xc, (u32)(4600 + mm)), xb[nb].data[0x10] = (u8)inst[mm]++;
                xb[nb++].pattern = pat, placed++;
            }
            if (placed) hordes_added++, hordes_sites += placed;
        }
        if (special || has[7705 - 7700] || has[7725 - 7700]) continue;
        // the kind's small map: free attach points the game uses for it, outside the regions
        // a kind whose small map the composition already has (rows the merge took from the added region's pattern,
        // without that pattern's flag rows): that kind, its flag rows only
        int kind = pat % 4, picked = 0, donor = -1, have_map = 0;
        for (int kk = 0; kk < 4 && !have_map; kk++)
            for (u32 i = 0; i < rd16(bases.p + 0x0A) && !have_map; i++) {
                const u8 *r = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
                if ((int)rd32(r + 4) == pat && (int)rd32(r + 0xc) == DF_KINDS[kk].map) kind = kk, have_map = 1;
            }
        if (have_map) picked = -1;  // no new sites
        for (u32 i = 0; i < rd16(bases.p + 0x0A) && picked >= 0 && picked < 8; i++) {
            const u8 *r = bases.p + rd64(bases.p + 0x40 + i * 24 + 8);
            if ((int)rd32(r + 0xc) != DF_KINDS[kind].map) continue;
            if (donor < 0) donor = (int)i;
            int at = (int)rd32(r + 8), taken = 0;
            for (u32 j = 0; j < rd16(bases.p + 0x0A) && !taken; j++) {
                const u8 *q = bases.p + rd64(bases.p + 0x40 + j * 24 + 8);
                taken = (int)rd32(q + 4) == pat && (int)rd32(q + 8) == at;
            }
            for (int k = nb - picked; k < nb && !taken; k++) taken = xb[k].pattern == pat && (int)rd32(xb[k].data + 8) == at;
            if (taken || !point_outside_regions(points, at)) continue;
            memset(xb[nb].data, 0, sizeof xb[nb].data);
            memcpy(xb[nb].data, r, 0x18);
            wr32(xb[nb].data + 8, (u32)at), xb[nb].data[0x10] = (u8)picked;
            xb[nb++].pattern = pat, picked++;
        }
        if (!picked) continue;
        if (picked > 0) sites += picked;
        kinds_added++;
        u8 *d = xf[nf].data;
        memset(d, 0, sizeof xf[nf].data);
        wr32(d + 8, 0), wr32(d + 0xc, (u32)DF_KINDS[kind].modifier), wr32(d + 0x10, (u32)DF_KINDS[kind].flag);
        memcpy(d + 0x14, meta + 0x14, 8);
        xf[nf++].pattern = pat;
        d = xf[nf].data;
        memset(d, 0, sizeof xf[nf].data);
        wr32(d + 8, 3040), wr32(d + 0xc, 800), wr32(d + 0x10, 7705);
        memcpy(d + 0x14, meta + 0x14, 8);
        xf[nf++].pattern = pat;
    }
    buf f2 = param_add_pattern_rows(flags, xf, nf), b2 = nb ? param_add_pattern_rows(bases, xb, nb) : bnd4_copy(bases);
    if (!f2.p || !b2.p) return free(f2.p), free(b2.p), 0;
    free(maps[0].p), free(maps[2].p);
    maps[0] = f2, maps[2] = b2;
    logf_("Day flow: map sets %d with 2 invader camps; %d family day flags added; %d compositions given a special event kind on %d sites; "
          "%d given hordes camps (%d sites)", camps, fam_added, kinds_added, sites, hordes_added, hordes_sites);
    return 1;
}

static int write_pattern_bosses(buf bases);  // with the boss deaths below
static int map_tables(buf bundle, int index[4], buf maps[4]) {
    static const char *const NAMES[4] = {"\\LotResultMapPatternFlag.param", "\\LotResultPlayAreaParam.param",
                                         "\\LotResultSmallBaseAndSpot.param", "\\MapPatternSet.param"};
    int two = two_regions_wanted();
    for (int t = 0; t < 4; t++)
        if ((index[t] = bnd4_find(bundle, NAMES[t])) < 0) return 0;
    int ap = bnd4_find(bundle, "\\SmallBaseAndSpotAttachPoint.param"), pc = bnd4_find(bundle, "\\PlayAreaCreateParam.param");
    if (ap < 0 || pc < 0) return 0;
    g_pac = bnd4_file(bundle, pc);
    for (int t = 0; t < 4; t++) {
        if (!two && t != 3) continue;
        if (!(maps[t] = map_test_table(bnd4_file(bundle, index[t]), t, bnd4_file(bundle, index[0]), bnd4_file(bundle, ap), two)).p) return 0;
    }
    if (two) {
        if (!pair_now()) chest_group_report(maps[0]);  // its table is the Rotted Woods' sites
        int d = bnd4_find(bundle, "\\SmallBaseAndSpotDefine.param");
        if (d < 0 || !write_churches(bnd4_file(bundle, d), maps[2], bnd4_file(bundle, ap))) return 0;
        if (!day_flow_maps(maps, bnd4_file(bundle, ap)) || !write_pattern_bosses(maps[2])) return 0;
    }
    return 1;
}

static buf camp_variation_rows(buf p);  // with the camp scripts below
static int build_regulation(const char *game_dir) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "regulation.bin", game_dir);
    buf enc = read_file(path);
    if (enc.n < 16 + 0x4C || enc.n % 16) return free(enc.p), logf_("cannot read %s", path), 0;
    u8 rk[240], iv[16];
    aes_expand(REGULATION_KEY, 32, rk);
    memcpy(iv, enc.p, 16);
    for (size_t at = enc.n - 16; at >= 16; at -= 16) {  // CBC, last block first
        aes_decrypt_block(rk, 14, enc.p + at);
        for (int i = 0; i < 16; i++) enc.p[at + i] ^= enc.p[at - 16 + i];
    }
    u8 *dcx = enc.p + 16;
    u32 usize = be32(dcx + 0x1C), csize = be32(dcx + 0x20);
    if (memcmp(dcx, "DCX", 3) || memcmp(dcx + 0x28, "ZSTD", 4) || csize > enc.n - 16 - 0x4C)
        return free(enc.p), logf_("regulation.bin is not in the expected format"), 0;
    buf bundle = {xalloc((size_t)usize + (1 << 17)), usize};
    buf patched = {0}, body = {0}, param = {0}, speffect = {0}, speffect2 = {0}, step = {0}, maps[4] = {{0}}, cur = {0}, goods = {0}, shop = {0};
    int ok = 0, index, sp_index, npc_index, area_index, goods_index = -1, shop_index = -1, map_index[4] = {-1, -1, -1, -1}, var_index = -1;
    buf variation = {0};
    u8 *team_row = NULL, *ally_row = NULL;
    if (nr_zstd_decompress(dcx + 0x4C, csize, bundle.p, (size_t)usize + (1 << 17)) != usize) {
        logf_("regulation.bin could not be unpacked");
    } else if ((index = bnd4_find(bundle, TARGET_PARAM)) < 0 || !(param = param_add_row(bnd4_file(bundle, index))).p) {
        logf_("The game's Target list looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((sp_index = bnd4_find(bundle, SPEFFECT_PARAM)) < 0 ||
               !(speffect = param_insert_sorted(bnd4_file(bundle, sp_index), SPEFFECT_TEMPLATE, SPEFFECT_TEAM_ROW,
                                                SPEFFECT_SIZE, &team_row)).p ||
               rd32(team_row + 0x08) != 0xBF800000 || team_row[0x16D] != 0xFF || rd32(team_row + 0x18C) != 0xFFFFFFFF) {
        logf_("The game's SpEffectParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if (!(speffect2 = param_insert_sorted(speffect, SPEFFECT_TEMPLATE, SPEFFECT_ALLY_ROW, SPEFFECT_SIZE, &ally_row)).p ||
               rd32(ally_row + 0x08) != 0xBF800000 || ally_row[0x16D] != 0xFF || rd32(ally_row + 0x18C) != 0xFFFFFFFF) {
        logf_("The game's SpEffectParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((npc_index = bnd4_find(bundle, NPC_PARAM)) < 0 || !scarab_rows(bnd4_file(bundle, npc_index))) {
        logf_("The game's NpcParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((area_index = bnd4_find(bundle, PLAY_AREA_PARAM)) < 0 || !no_rain(bnd4_file(bundle, area_index))) {
        logf_("The game's PlayAreaCreateCommonParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((goods_index = bnd4_find(bundle, GOODS_PARAM)) < 0 || (shop_index = bnd4_find(bundle, SHOP_PARAM)) < 0 ||
               !(goods = bnd4_copy(bnd4_file(bundle, goods_index))).p || !(shop = bnd4_copy(bnd4_file(bundle, shop_index))).p ||
               !day_flow_rows(&speffect2, &goods, &shop) || bnd4_find(bundle, "\\PlayerCommonParam.param") < 0 ||
               !player_common_slots(bnd4_file(bundle, bnd4_find(bundle, "\\PlayerCommonParam.param")))) {
        logf_("The game's goods, shop or SpEffect params look different than expected (day flow rows). The game may have been updated; the mod needs an update too.");
    } else if ((var_index = bnd4_find(bundle, "\\SmallBaseMapVariationParam.param")) < 0 ||
               !(variation = camp_variation_rows(bnd4_file(bundle, var_index))).p) {
        logf_("The game's SmallBaseMapVariationParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if (!map_tables(bundle, map_index, maps)) {
        logf_("The game's map pattern tables look different than expected; the two-region maps cannot be built. The game may have been updated; the mod needs an update too.");
    } else {
        // effectEndurance stays -1 (the template's): until removed; death removes it
        team_row[0x16D] = 16;                // changeTeamType 16
        wr32(team_row + 0x18C, 300050);      // vfxId1: red phantom look
        // the ally's row, in the second copy (team_row points into the first, which is not used further)
        for (u32 i = 0; i < rd16(speffect2.p + 0x0A); i++)
            if (rd32(speffect2.p + 0x40 + i * 24) == SPEFFECT_TEAM_ROW) {
                u8 *r = speffect2.p + rd64(speffect2.p + 0x40 + i * 24 + 8);
                r[0x16D] = 16, wr32(r + 0x18C, 300050);
            }
        // effectEndurance stays -1 (the template's): no time limit; the DLL removes it when the ally's part ends
        for (u32 i = 0; i < rd16(speffect2.p + 0x0A); i++)  // (by id: day_flow_rows made a new copy after ally_row was found)
            if (rd32(speffect2.p + 0x40 + i * 24) == SPEFFECT_ALLY_ROW)
                wr32(speffect2.p + rd64(speffect2.p + 0x40 + i * 24 + 8) + 0x18C, ALLY_PHANTOM_VFX);  // vfxId1: gold phantom look; team unchanged
        // Written as a zlib DCX, like our other files (the launcher has no zstd compressor). It has
        // to be compressed: the game crashes at start-up on a 55 MB regulation.bin of stored zstd
        // blocks. ponytail: unlike the game's own file this is not zstd; if the game ever stops
        // taking zlib here, a zstd compressor is the upgrade path.
        if (one_region()) logf_("one-region (test): maps of region %d only, the game's own patterns%s", one_region(),
                                map_draw_group() >= 0 ? "; test: only one night boss group (map-group)" : "");
        else logf_("Two-region maps: every map is the %s with the %s (200 compositions)%s", pair_a()->name, pair_b()->name,
                   map_draw_group() >= 0 ? "; test: only one night boss group (map-group)" : "");
        step = bnd4_replace(bundle, index, param);
        patched = bnd4_replace(step, sp_index, speffect2);
        cur = bnd4_replace(patched, goods_index, goods), free(patched.p), patched = cur;
        cur = bnd4_replace(patched, shop_index, shop), free(patched.p), patched = cur;
        cur = bnd4_replace(patched, var_index, variation), free(patched.p), patched = cur;
        logf_("Day flow: item %d at the field merchant (rows 3007 / 3507 instead of the Small Pouch, %d runes), 8 item slots from the start, %d enemy tiers", CHALLENGE_GOODS, CHALLENGE_PRICE, TIER_COUNT);
        for (int t = 0; t < 4; t++)
            if (maps[t].p) cur = bnd4_replace(patched, map_index[t], maps[t]), free(patched.p), patched = cur;
        body = dcx_write(patched);
        size_t padded = (body.n + 15) / 16 * 16;
        buf out = {xalloc(16 + padded), 16 + padded};
        memcpy(out.p, iv, 16);
        memcpy(out.p + 16, body.p, body.n);
        for (size_t at = 16; at < out.n; at += 16) {
            for (int i = 0; i < 16; i++) out.p[at + i] ^= out.p[at - 16 + i];
            aes_encrypt_block(rk, 14, out.p + at);
        }
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "regulation.bin", g_app);
        ok = write_file(path, out.p, out.n);
        free(out.p);
    }
    free(enc.p); free(bundle.p); free(param.p); free(speffect.p); free(speffect2.p); free(step.p); free(patched.p); free(body.p);
    free(goods.p); free(shop.p); free(variation.p);
    for (int t = 0; t < 4; t++) free(maps[t].p);
    return ok;
}

// --- menu icons ------------------------------------------------------------------------------------------------
//
// Menu icons are sub-rectangles of big atlas textures; a ".layout" XML file per atlas names them
// ("MENU_MenuIcon_<id>.png"). Our two icons go into an unused corner of the DLC atlas and get one
// layout line each, as in mod/build_icon.py, which also produces their BC7 blocks (data/icon_<id>.bc7).

// Two atlases: the Target list's icons (menu icons) and our item's icon (item icons, 160 x 160 like the game's).
#define DDS_HEADER 148
static const struct { const char *atlas, *name; int id, side; } ICONS[] = {
    {"SB_MenuIcon_DLC01_0", "MENU_MenuIcon_%d", 41813, 248},
    {"SB_MenuIcon_DLC01_0", "MENU_MenuIcon_%d", 41814, 460},
    {"SB_ItemIcon_DLC01_0", "MENU_ItemIcon_%05d", 21990, 160},  // CHALLENGE_ICON
};
#define ICON_COUNT (sizeof ICONS / sizeof *ICONS)
typedef struct { int x, w, y, h; } rect;

// Integer value of attribute `name` (given as " name=\"") inside the tag starting at `tag`, or -1.
static int attr_int(const char *tag, const char *name) {
    const char *end = strchr(tag, '>'), *a = strstr(tag, name);
    return a && end && a < end ? atoi(a + strlen(name)) : -1;
}

// Top-left corner of an unused side x side square, searched from the bottom right on the 4-pixel
// block grid. Returns 1 if found.
static int free_spot(const rect *used, size_t n, int side, int width, int height, int *ox, int *oy) {
    const int pad = 4;
    for (int y = height - side - pad; y > pad; y -= 4)
        for (int x = width - side - pad; x > pad; x -= 4) {
            size_t i = 0;
            for (; i < n; i++)
                if (!(x + side + pad <= used[i].x || used[i].x + used[i].w + pad <= x ||
                      y + side + pad <= used[i].y || used[i].y + used[i].h + pad <= y)) break;
            if (i == n) { *ox = x; *oy = y; return 1; }
        }
    return 0;
}

// quality: 'h' or 'l', the game's two texture sets. Returns 1 on success.
// two-regions, the map screen: it shows one texture pack per map type, menu:/71_MapTile_M%02d.tpf
// (0x140ec2cc0), 256 x 256 BC7 tiles named MENU_MapTile_L<level>_<x>_<y>. The Mountaintop's (01) and the
// Rotted Woods' (03) each redraw their own part over the plain map's (00). Both packs are replaced by one
// with the Mountaintop's tiles and, in every 4 x 4 block where the Rotted Woods' tile differs from the
// plain one, the Rotted Woods' block (BC7 blocks stand alone, so no re-encoding). Written only with the
// setting on; removed otherwise. ponytail: textures only the Rotted Woods' pack has (3 "_B1" tiles) are
// not added; adding them means rebuilding the TPF's tables.
typedef struct { const u8 *dds; u32 size; } tpf_tex;
static tpf_tex tpf_find(buf tpf, const u8 *name_utf16, size_t name_bytes) {
    tpf_tex none = {0};
    for (u32 i = 0; tpf.n >= 0x10 && i < rd32(tpf.p + 8) && 0x10 + (size_t)(i + 1) * 0x14 <= tpf.n; i++) {
        const u8 *e = tpf.p + 0x10 + i * 0x14;
        u32 off = rd32(e), size = rd32(e + 4), name = rd32(e + 0x0C);
        if ((u64)off + size <= tpf.n && (u64)name + name_bytes + 2 <= tpf.n && !memcmp(tpf.p + name, name_utf16, name_bytes) &&
            !tpf.p[name + name_bytes] && !tpf.p[name + name_bytes + 1]) {
            tpf_tex t = {tpf.p + off, size};
            return t;
        }
    }
    return none;
}
static int build_two_region_map(const char *game_dir, buf bhd, const char *bdt) {
    char path[1300];
    buf pack[3] = {{0}};  // 00 plain, 01 the Mountaintop, 03 the Rotted Woods
    const int IDS[3] = {0, pair_a()->kind, pair_b()->kind};
    int ok = 1, merged = 0;
    for (int k = 0; k < 3; k++) {
        snprintf(path, sizeof path, "/menu/71_maptile_m%02d.tpf.dcx", IDS[k]);
        buf dcx = archive_read(bhd, bdt, path);
        pack[k] = dcx.p ? dcx_read(game_dir, dcx) : dcx;
        free(dcx.p);
        ok = ok && pack[k].p && pack[k].n >= 0x10 && !memcmp(pack[k].p, "TPF", 3);
    }
    for (u32 i = 0; ok && i < rd32(pack[1].p + 8) && 0x10 + (size_t)(i + 1) * 0x14 <= pack[1].n; i++) {
        const u8 *e = pack[1].p + 0x10 + i * 0x14;
        u32 off = rd32(e), size = rd32(e + 4), name = rd32(e + 0x0C);
        size_t len = 0;
        while ((u64)name + len + 1 < pack[1].n && (pack[1].p[name + len] || pack[1].p[name + len + 1])) len += 2;
        tpf_tex plain = tpf_find(pack[0], pack[1].p + name, len), rot = tpf_find(pack[2], pack[1].p + name, len);
        u8 *mine = pack[1].p + off;
        if ((u64)off + size > pack[1].n || size <= DDS_HEADER || !plain.dds || !rot.dds || plain.size != size || rot.size != size ||
            memcmp(mine, plain.dds, DDS_HEADER) || memcmp(mine, rot.dds, DDS_HEADER) || memcmp(mine + 84, "DX10", 4) ||
            rd32(mine + 128) != 98 || (size - DDS_HEADER) % 16)
            continue;
        for (u32 at = DDS_HEADER; at < size; at += 16)
            if (memcmp(rot.dds + at, plain.dds + at, 16)) memcpy(mine + at, rot.dds + at, 16), merged++;
    }
    if (ok) {
        buf out = dcx_write(pack[1]);
        for (int k = 1; k < 3; k++) {
            snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "71_maptile_m%02d.tpf.dcx", g_app, IDS[k]);
            ok = ok && write_file(path, out.p, out.n);
        }
        free(out.p);
        logf_("two-regions: map screen built (%d blocks of the %s over the %s tiles)", merged, pair_b()->name, pair_a()->of);
    } else
        logf_("The game's map screen textures look different than expected; two-regions cannot be applied.");
    for (int k = 0; k < 3; k++) free(pack[k].p);
    return ok;
}
static void remove_two_region_map(void) {
    char path[1300];
    for (int k = 1; k <= 3; k += 2) {
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "71_maptile_m%02d.tpf.dcx", g_app, k);
        remove(path);
    }
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "pattern-bosses.txt", g_app);  // another map's patterns
    remove(path);
}

static int place_icons(buf *bundle, buf tpf, const char *atlas_name, char quality) {
    char key[64], path[1300];
    snprintf(key, sizeof key, "%s.layout", atlas_name);
    int index = bnd4_find(*bundle, key), ok = 0;
    if (index < 0) return logf_("menu icon layout %s not found", atlas_name), 0;
    buf lay = bnd4_file(*bundle, index);
    char *layout = xalloc(lay.n + 1), *text = NULL;
    rect *used = NULL;
    memcpy(layout, lay.p, lay.n);
    layout[lay.n] = 0;
    const char *atlas = strstr(layout, "<TextureAtlas "), *close = strstr(layout, "</TextureAtlas>");
    int width = atlas ? attr_int(atlas, " width=\"") : -1, height = atlas ? attr_int(atlas, " height=\"") : -1;
    size_t n = 0, cap = 0;
    for (const char *t = layout; (t = strstr(t, "<SubTexture ")); t++) {
        if (n + ICON_COUNT >= cap) {
            rect *grown = xalloc((cap = cap * 2 + 64) * sizeof *grown);
            if (n) memcpy(grown, used, n * sizeof *grown);
            free(used);
            used = grown;
        }
        rect r = {attr_int(t, " x=\""), attr_int(t, " width=\""), attr_int(t, " y=\""), attr_int(t, " height=\"")};
        used[n++] = r;
    }
    // The atlas texture inside the TPF: a BC7 DDS without mipmaps.
    u8 *dds = NULL;
    for (u32 i = 0; tpf.n >= 0x10 && i < rd32(tpf.p + 8) && 0x10 + (size_t)(i + 1) * 0x14 <= tpf.n; i++) {
        const u8 *e = tpf.p + 0x10 + i * 0x14;
        if (rd32(e + 0x0C) < tpf.n && name_ends_with(tpf.p + rd32(e + 0x0C), atlas_name) &&
            (u64)rd32(e) + rd32(e + 4) <= tpf.n && rd32(e + 4) > DDS_HEADER) dds = tpf.p + rd32(e);
    }
    if (!atlas || !close || width < 0 || height < 0 || !dds || memcmp(dds + 84, "DX10", 4) || rd32(dds + 128) != 98 ||
        (int)rd32(dds + 12) != height || (int)rd32(dds + 16) != width) {
        logf_("The game's menu icon atlas %s looks different than expected. The game may have been updated; the mod needs an update too.", atlas_name);
        goto done;
    }
    char lines[ICON_COUNT * 256];
    size_t len = 0;
    for (size_t i = 0; i < ICON_COUNT; i++) {
        if (strcmp(ICONS[i].atlas, atlas_name)) continue;
        int side = ICONS[i].side, x, y;
        char name[64], probe[72];
        snprintf(name, sizeof name, ICONS[i].name, ICONS[i].id);
        snprintf(probe, sizeof probe, "%s.", name);
        snprintf(path, sizeof path, "%s" SEP "data" SEP "icon_%d.bc7", g_app, ICONS[i].id);
        buf blocks = read_file(path);
        size_t per_row = (size_t)side / 4 * 16;
        if (strstr(layout, probe) || blocks.n != per_row * (side / 4) || !free_spot(used, n, side, width, height, &x, &y)) {
            free(blocks.p);
            logf_("menu icon %d cannot be added (%s, %c)", ICONS[i].id, atlas_name, quality);
            goto done;
        }
        rect r = {x, side, y, side};
        used[n++] = r;
        for (int by = 0; by < side / 4; by++)
            memcpy(dds + DDS_HEADER + ((size_t)(y / 4 + by) * (width / 4) + x / 4) * 16, blocks.p + by * per_row, per_row);
        free(blocks.p);
        len += (size_t)snprintf(lines + len, sizeof lines - len,
                                "\t<SubTexture name=\"%s.png\" x=\"%d\" width=\"%d\" y=\"%d\" height=\"%d\""
                                " originalWidth=\"%d\" originalHeight=\"%d\" half=\"0\"/>\r\n",
                                name, x, side, y, side, side, side);
    }
    size_t before = (size_t)(close - layout);
    text = xalloc(lay.n + len);
    memcpy(text, layout, before);
    memcpy(text + before, lines, len);
    memcpy(text + before + len, close, lay.n - before);
    buf new_layout = {(u8 *)text, lay.n + len};
    buf patched = bnd4_replace(*bundle, index, new_layout);
    free(bundle->p);
    *bundle = patched;
    ok = 1;
done:
    free(layout); free(text); free(used);
    return ok;
}
static int build_icons(const char *game_dir, buf bhd, const char *bdt, char quality) {
    char path[1300];
    snprintf(path, sizeof path, "/menu/21_common_dlc01_%c.sblytbnd.dcx", quality);
    buf dcx = archive_read(bhd, bdt, path);
    buf bundle = dcx.p ? dcx_read(game_dir, dcx) : dcx;
    free(dcx.p);
    snprintf(path, sizeof path, "/menu/21_common_dlc01_%c.tpf.dcx", quality);
    dcx = archive_read(bhd, bdt, path);
    buf tpf = dcx.p ? dcx_read(game_dir, dcx) : dcx;
    free(dcx.p);
    int ok = 0;
    if (!bundle.p || !tpf.p) {
        logf_("menu icon files not found in the game archive");
    } else if (place_icons(&bundle, tpf, "SB_MenuIcon_DLC01_0", quality) && place_icons(&bundle, tpf, "SB_ItemIcon_DLC01_0", quality)) {
        buf out = dcx_write(bundle);
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "21_common_dlc01_%c.sblytbnd.dcx", g_app, quality);
        ok = write_file(path, out.p, out.n);
        free(out.p);
        out = dcx_write(tpf);
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "21_common_dlc01_%c.tpf.dcx", g_app, quality);
        ok = ok && write_file(path, out.p, out.n);
        free(out.p);
    }
    free(bundle.p); free(tpf.p);
    return ok;
}

// --- finding the game and the save ------------------------------------------------------------------------------

int core_check_game(const char *dir) {
    char a[1100], b[1100];
    snprintf(a, sizeof a, "%s" SEP "nightreign.exe", dir);
    snprintf(b, sizeof b, "%s" SEP "data0.bhd", dir);
    return exists(a) && exists(b);
}

// Look through one Steam installation's library list for the game.
static int find_in_steam(const char *steam, char *out, size_t cap) {
    char vdf[1100];
    snprintf(vdf, sizeof vdf, "%s" SEP "steamapps" SEP "libraryfolders.vdf", steam);
    buf b = read_file(vdf);
    if (!b.p) return 0;
    int found = 0;
    char *text = xalloc(b.n + 1);
    memcpy(text, b.p, b.n);
    for (char *p = text; !found && (p = strstr(p, "\"path\"")); p += 6) {
        char *q = strchr(p + 6, '"');
        if (!q) break;
        char lib[900];
        size_t n = 0;
        for (q++; *q && *q != '"' && n < sizeof lib - 1; q++) {
            if (*q == '\\' && q[1] == '\\') q++;  // "C:\\Games" in the file means C:\Games
            lib[n++] = *q;
        }
        lib[n] = 0;
        snprintf(out, cap, "%s" SEP "steamapps" SEP "common" SEP "ELDEN RING NIGHTREIGN" SEP "Game", lib);
        found = core_check_game(out);
    }
    free(text);
    free(b.p);
    return found;
}

int core_find_game(char *out, size_t cap) {
#ifdef _WIN32
    char steam[900] = "";
    DWORD n = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &n) ==
            ERROR_SUCCESS) {
        for (char *c = steam; *c; c++) if (*c == '/') *c = '\\';
        if (find_in_steam(steam, out, cap)) return 1;
    }
    return find_in_steam("C:\\Program Files (x86)\\Steam", out, cap);
#else
    const char *home = getenv("HOME"), *roots[] = {"/.local/share/Steam", "/.steam/steam",
                                                    "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    for (size_t i = 0; home && i < sizeof roots / sizeof *roots; i++) {
        char steam[900];
        snprintf(steam, sizeof steam, "%s%s", home, roots[i]);
        if (find_in_steam(steam, out, cap)) return 1;
    }
    return 0;
#endif
}

// The save lives in <...>/Nightreign/<steam id>/NR0000.sl2; find the <steam id> folder.
static int find_save_under(const char *parent, char *out, size_t cap) {
    char probe[1700];
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        snprintf(probe, sizeof probe, "%s\\%s\\NR0000.sl2", parent, fd.cFileName);
        if (fd.cFileName[0] != '.' && exists(probe)) {
            snprintf(out, cap, "%s\\%s", parent, fd.cFileName);
            found = 1;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR *d = opendir(parent);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        snprintf(probe, sizeof probe, "%s/%s/NR0000.sl2", parent, e->d_name);
        if (e->d_name[0] != '.' && exists(probe)) {
            snprintf(out, cap, "%s/%s", parent, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
#endif
}

int core_find_save_dir(const char *game_dir, char *out, size_t cap) {
    char parent[1100];
#ifdef _WIN32
    (void)game_dir;
    const char *appdata = getenv("APPDATA");
    if (!appdata) return 0;
    snprintf(parent, sizeof parent, "%s\\Nightreign", appdata);
#else
    // Under Proton the save is inside the game's compatibility prefix, in the same Steam library.
    const char *tail = strstr(game_dir, "/steamapps/common/");
    if (!tail) return 0;
    snprintf(parent, sizeof parent, "%.*s/steamapps/compatdata/" STEAM_APP_ID
             "/pfx/drive_c/users/steamuser/AppData/Roaming/Nightreign", (int)(tail - game_dir), game_dir);
#endif
    return find_save_under(parent, out, cap);
}

// --- the four actions -------------------------------------------------------------------------------------------

static void remove_tree(const char *dir);

int core_backup_save(const char *save_dir, char *where, size_t cap) {
    static const char *names[] = {"NR0000.sl2", "NR0000.sl2.bak", MOD_SAVE_NAME, MOD_SAVE_NAME ".bak"};
    char stamp[48], folder[64], dir[1300], from[1400], to[1500];
    if (where && cap) where[0] = 0;
    if (!save_dir || !*save_dir) return 0;
    int any = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        snprintf(from, sizeof from, "%s" SEP "%s", save_dir, names[i]);
        any |= exists(from);
    }
    if (!any) return 0;
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", localtime(&now));
    // a new folder: the time stamp, or with _2, _3 ... when that name is taken
    for (int n = 1;; n++) {
        if (n == 1) snprintf(folder, sizeof folder, "%s", stamp);
        else snprintf(folder, sizeof folder, "%s_%d", stamp, n);
        snprintf(dir, sizeof dir, "%s" SEP "backups" SEP "%s", g_app, folder);
        if (!dir_exists(dir)) break;
        if (n > 999) return -1;
    }
    const char *base = save_dir + strlen(save_dir);
    while (base > save_dir && base[-1] != '/' && base[-1] != '\\') base--;
    int copied = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        snprintf(from, sizeof from, "%s" SEP "%s", save_dir, names[i]);
        snprintf(to, sizeof to, "%s" SEP "%s" SEP "%s", dir, base, names[i]);
        if (!exists(from)) continue;
        if (exists(to) || !copy_file(from, to)) return -1;  // never write over a file
        copied++;
    }
    if (where) snprintf(where, cap, "backups" SEP "%s", folder);
    logf_("Save backed up (%d files) to backups" SEP "%s", copied, folder);
    return 1;
}


int core_load_debug(void) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "debug-log.txt", g_app);
    buf b = read_file(path);
    int on = b.p && b.n && b.p[0] == '1';
    free(b.p);
    return on;
}

void core_save_debug(int on) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "debug-log.txt", g_app);
    write_file(path, (const u8 *)(on ? "1" : "0"), 1);
}

void core_load_lang(char *code, size_t cap) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "language.txt", g_app);
    buf b = read_file(path);
    size_t n = 0;
    while (b.p && n < b.n && n + 1 < cap && b.p[n] >= 'a' && b.p[n] <= 'z') code[n] = (char)b.p[n], n++;
    if (cap) code[n] = 0;
    free(b.p);
}

void core_save_lang(const char *code) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "language.txt", g_app);
    write_file(path, (const u8 *)code, strlen(code));
}

// Per language: our menu lines into CL_MenuText (menu_dlc01) and our item's three lines into the goods tables
// (item_dlc01), both bundles unpacked in one go.
static int build_text(const char *game_dir, buf bhd, const char *bdt, const char *lang) {
    char path[1300];
    buf in[2], out[2];
    snprintf(path, sizeof path, "/msg/%s/menu_dlc01.msgbnd.dcx", lang);
    in[0] = archive_read(bhd, bdt, path);
    if (!in[0].p) return -1;  // language not installed
    snprintf(path, sizeof path, "/msg/%s/item_dlc01.msgbnd.dcx", lang);
    in[1] = archive_read(bhd, bdt, path);
    if (!in[1].p) return free(in[0].p), logf_("%s: no item texts in the game archive", lang), 0;
    dcx_read_many(game_dir, in, out, 2);
    free(in[0].p), free(in[1].p);
    int ok = out[0].p && out[1].p;
    int polish = !strcmp(lang, "polpl");
    if (ok) {
        buf bundle = out[0];
        int target = bnd4_find(bundle, TEXT_TARGET_FMG);
        fmg dst = {0};
        if (target < 0 || !fmg_parse(bnd4_file(bundle, target), &dst)) {
            ok = 0, logf_("%s: unexpected text bundle layout", lang);
        } else {
            static u8 wide[OUR_TEXT_COUNT][512];
            for (size_t i = 0; i < OUR_TEXT_COUNT; i++)
                fmg_set(&dst, OUR_TEXT[i].id, wide[i], utf8_to_utf16(polish ? OUR_TEXT[i].polish : OUR_TEXT[i].english, wide[i]));
            static u8 pwide[10][1024];
            static char ptext[10][400];
            u32 pids[10];
            int pn = pair_texts(polish, pids, ptext);
            for (int i = 0; i < pn; i++) fmg_set(&dst, pids[i], pwide[i], utf8_to_utf16(ptext[i], pwide[i]));
            buf table = fmg_write(&dst), patched = bnd4_replace(bundle, target, table), dcx = dcx_write(patched);
            snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "msg" SEP "%s" SEP "menu_dlc01.msgbnd.dcx", g_app, lang);
            ok = write_file(path, dcx.p, dcx.n);
            free(table.p); free(patched.p); free(dcx.p);
        }
        free(dst.e);
    }
    if (ok) {  // the item's name, info and caption (goods id CHALLENGE_GOODS)
        buf bundle = bnd4_copy(out[1]);
        static u8 wide[sizeof ITEM_TEXT / sizeof *ITEM_TEXT][2048];
        for (size_t t = 0; ok && t < sizeof ITEM_TEXT / sizeof *ITEM_TEXT; t++) {
            int target = bnd4_find(bundle, ITEM_TEXT[t].fmg);
            fmg dst = {0};
            if (target < 0 || !fmg_parse(bnd4_file(bundle, target), &dst)) {
                ok = 0, logf_("%s: unexpected item text bundle layout (%s)", lang, ITEM_TEXT[t].fmg);
            } else {
                static char cap_text[1200];
                const char *txt = polish ? ITEM_TEXT[t].polish : ITEM_TEXT[t].english;
                if (!strcmp(ITEM_TEXT[t].fmg, "\\GoodsCaption.fmg")) pair_caption(polish, cap_text, sizeof cap_text), txt = cap_text;
                fmg_set(&dst, CHALLENGE_GOODS, wide[t], utf8_to_utf16(txt, wide[t]));
                buf table = fmg_write(&dst), patched = bnd4_replace(bundle, target, table);
                free(bundle.p), free(table.p);
                bundle = patched;
            }
            free(dst.e);
        }
        if (ok) {
            buf dcx = dcx_write(bundle);
            snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "msg" SEP "%s" SEP "item_dlc01.msgbnd.dcx", g_app, lang);
            ok = write_file(path, dcx.p, dcx.n);
            free(dcx.p);
        }
        free(bundle.p);
    }
    free(out[0].p), free(out[1].p);
    return ok;
}

static int write_profile(void) {
    char path[1300], seamless[1300];
    snprintf(path, sizeof path, "%s" SEP "work" SEP "invasion.me3", g_app);
    snprintf(seamless, sizeof seamless, "%s" SEP "SeamlessCoop" SEP "nrsc.dll", g_app);
    make_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) return logf_("cannot write %s", path), 0;
    fprintf(f, "profileVersion = \"v1\"\n");
    // Seamless Co-op keeps its own save file; without it the mod uses a save of its own.
    if (!exists(seamless)) fprintf(f, "savefile = \"" MOD_SAVE_NAME "\"\n");
    // me3's own option ("Neutralize Arxan"), as in the dev profile: with the game's code protection
    // active, hooked calls were restored behind the mod's back (owner's decision).
    fprintf(f, "disable_arxan = true\n");
    fprintf(f, "\n[[supports]]\ngame = \"nightreign\"\n\n[[packages]]\npath = \"build\"\n");
    if (exists(seamless)) {
        fprintf(f, "\n[[natives]]\npath = \"../SeamlessCoop/nrsc.dll\"\nload_early = true\n");
        logf_("Seamless Co-op found next to the launcher: it will be loaded too");
    }
    fprintf(f, "\n[[natives]]\npath = \"native/invasion.dll\"\n");
    fclose(f);
    return 1;
}

// --- Map pair test (pair = 1: Mountaintop + Noklateo), offline parts -------------------------------------
// The west half of the grid (X 42-43) is one far map, m60_10_09_V2 (V = the region's tile variant digit:
// 1 Mountaintop, 5 Noklateo); a half loads one variant, so this pair needs one composed half: the Mountaintop
// file (m60_10_09_12) with the Noklateo tiles 42-43 x 36-37 taken from m60_10_09_52. Map files and pieces are in
// data2. Rules from the game's own files (analysis/flagcat/msbw.py, compose.py: every map reproduced byte for byte):
// MSB = 16-byte header, then lists {i32 version, i32 count+1, i64 name, i64 entries[count], i64 next}, the name
// right after the table, entries in order; an entry is the bytes up to the next one (offsets inside are relative).
// Index fields: model +0x8 type / +0xc index in type, point +0x8 / +0xc, event +0xc / +0x10, part +0xc / +0x10
// (sequential per type), part +0x14 model index, model +0x18 number of parts using it, asset parts (type 13)
// +0x368 / +0x36c / +0x374 their own index or -1, treasure (event type 4) type data +8 and ObjAct (7) +4 a part index
// (event type data offset at +0x20). Entry names begin with the tile, "m60_XX_ZZ_VV-...", except map pieces
// "mGGXXZZNN_nnnn" (group, tile, piece number; the model is "mGGXXZZNN").
// Pieces: the loader asks for "%s:/%s/%s/%s.mapbnd" = map:/m60/<half>/<half>_<model digits>.mapbnd, so a Noklateo
// piece goes into the composed half's folder under the half's name. Lighting: the environment maps
// ("maptpf:/%s/%s/%s_Envmap_%02d.tpf") and probes ("%s:/%s/%s/%s%s.ivinfobnd") are per half name, so the composed
// half keeps the Mountaintop's (the Noklateo tiles lit with the Mountaintop's probes; to be seen in the game).
typedef struct { int ver; buf name; int n, cap; buf *e; } msb_list;
typedef struct { u8 head[16]; int nl; msb_list l[8]; } msb_file;
static int msb_parse(buf d, msb_file *m) {
    memset(m, 0, sizeof *m);
    if (d.n < 32 || memcmp(d.p, "MSB ", 4)) return 0;
    memcpy(m->head, d.p, 16);
    for (u64 off = 16; off && m->nl < 8;) {
        if (off + 16 > d.n) return 0;
        msb_list *L = &m->l[m->nl++];
        L->ver = (int)rd32(d.p + off);
        int cnt1 = (int)rd32(d.p + off + 4);
        u64 no = rd64(d.p + off + 8), table = off + 16, end;
        if (cnt1 < 1 || table + 8ull * cnt1 > d.n || no != table + 8ull * cnt1) return 0;
        u64 next = rd64(d.p + table + 8ull * (cnt1 - 1));
        end = next ? next : d.n;
        L->n = L->cap = cnt1 - 1;
        L->e = xalloc(sizeof(buf) * (L->n + 1));
        u64 first = L->n ? rd64(d.p + table) : end;
        if (first < no || first > end) return 0;
        L->name.p = d.p + no, L->name.n = first - no;
        for (int i = 0; i < L->n; i++) {
            u64 a = rd64(d.p + table + 8ull * i), b = i + 1 < L->n ? rd64(d.p + table + 8ull * (i + 1)) : end;
            if (a >= b || b > d.n) return 0;
            L->e[i].p = d.p + a, L->e[i].n = b - a;
        }
        off = next;
    }
    return m->nl == 6;
}
static buf msb_write(const msb_file *m) {
    size_t n = 16;
    for (int i = 0; i < m->nl; i++) {
        n += 16 + 8 * (m->l[i].n + 1) + m->l[i].name.n;
        for (int k = 0; k < m->l[i].n; k++) n += m->l[i].e[k].n;
    }
    buf o = {xalloc(n), n};
    memcpy(o.p, m->head, 16);
    size_t at = 16;
    for (int i = 0; i < m->nl; i++) {
        const msb_list *L = &m->l[i];
        size_t start = at, size = 16 + 8 * (L->n + 1) + L->name.n;
        for (int k = 0; k < L->n; k++) size += L->e[k].n;
        wr32(o.p + at, (u32)L->ver), wr32(o.p + at + 4, (u32)(L->n + 1));
        at += 16 + 8 * (L->n + 1);
        wr64(o.p + start + 8, at);
        memcpy(o.p + at, L->name.p, L->name.n), at += L->name.n;
        for (int k = 0; k < L->n; k++) {
            wr64(o.p + start + 16 + 8 * k, at);
            memcpy(o.p + at, L->e[k].p, L->e[k].n), at += L->e[k].n;
        }
        wr64(o.p + start + 16 + 8 * L->n, i + 1 < m->nl ? start + size : 0);
    }
    return o;
}
static void msb_name(buf e, char *out, size_t cap) {  // the entry's UTF-16 name, ASCII only
    size_t k = 0;
    u64 no = e.n >= 8 ? rd64(e.p) : 0;
    for (u64 i = no; i + 1 < e.n && k + 1 < cap && (e.p[i] || e.p[i + 1]); i += 2) out[k++] = (char)e.p[i];
    out[k] = 0;
}
static int msb_tile(buf e, int *x, int *z) {  // "m60_XX_ZZ_VV-..." or a map piece "mGGXXZZNN_nnnn"
    char nm[64];
    int g, n;
    msb_name(e, nm, sizeof nm);
    if (sscanf(nm, "m60_%2d_%2d_", x, z) == 2) return 1;
    return nm[0] == 'm' && strlen(nm) >= 9 && nm[9] == '_' && sscanf(nm + 1, "%2d%2d%2d%2d", &g, x, z, &n) == 4;
}
static int g_half_x0, g_half_x1;  // the X range of the half being composed (west 42-43, east 44-45)
// Index within type: sequential per type value in list order, any type value.
typedef struct { u32 type[128]; int count[128], n; } type_counter;
static int type_next(type_counter *c, u32 t) {
    for (int i = 0; i < c->n; i++) if (c->type[i] == t) return c->count[i]++;
    if (c->n < 128) c->type[c->n] = t, c->count[c->n++] = 1;
    return 0;
}
static int pair_b_tile(int x, int z) {  // the added region's tiles in that half (Noklateo with 44_37, as the game's table)
    const region_t *b = pair_b();
    int in = (x >= b->x0 && x <= b->x1 && z >= b->z0 && z <= b->z1) || (b == &R_NK && x == 44 && z == 37);
    return in && x >= g_half_x0 && x <= g_half_x1;
}
static buf dup_buf(buf b) { buf o = {xalloc(b.n ? b.n : 1), b.n}; memcpy(o.p, b.p, b.n); return o; }
// A (first variant, kept outside B's tiles) + B (on B's tiles) -> composed MSB; *from_b gets B's map-piece model
// names (as "digits") that the composed file uses and A does not have.
static buf msb_compose(buf da, buf db, char names_b[][16], int *nb, int cap) {
    msb_file A, B, M;
    buf none = {0};
    if (!msb_parse(da, &A) || !msb_parse(db, &B) || A.nl != B.nl) return logf_("pair: map file not in the expected format"), none;
    enum { MODEL = 0, EVENT = 1, POINT = 2, PARTS = 5 };
    memcpy(&M, &A, sizeof M);
    // models: union by name (A first), then only the ones parts use
    int na = A.l[MODEL].n, nbm = B.l[MODEL].n, nm = 0;
    buf *mod = xalloc(sizeof(buf) * (na + nbm + 1));
    int *map_a = xalloc(sizeof(int) * (na + 1)), *map_b = xalloc(sizeof(int) * (nbm + 1)), *from_b = xalloc(sizeof(int) * (na + nbm + 1));
    char a_name[64], b_name[64];
    for (int i = 0; i < na; i++) mod[nm] = A.l[MODEL].e[i], from_b[nm] = 0, map_a[i] = nm++;
    for (int i = 0; i < nbm; i++) {
        msb_name(B.l[MODEL].e[i], b_name, sizeof b_name);
        int k = 0;
        for (; k < nm; k++) {
            msb_name(mod[k], a_name, sizeof a_name);
            if (!strcmp(a_name, b_name)) break;
        }
        if (k == nm) mod[nm] = B.l[MODEL].e[i], from_b[nm] = 1, nm++;
        map_b[i] = k;
    }
    // parts: A's outside B's tiles, B's on them; model index remapped; own-index fields; index in type
    int pa = A.l[PARTS].n, pb = B.l[PARTS].n, np = 0, x, z;
    buf *parts = xalloc(sizeof(buf) * (pa + pb + 1));
    int *src_is_b = xalloc(sizeof(int) * (pa + pb + 1)), *src_i = xalloc(sizeof(int) * (pa + pb + 1));
    for (int i = 0; i < pa; i++)
        if (!(msb_tile(A.l[PARTS].e[i], &x, &z) && pair_b_tile(x, z))) parts[np] = dup_buf(A.l[PARTS].e[i]), src_is_b[np] = 0, src_i[np++] = i;
    for (int i = 0; i < pb; i++)
        if (msb_tile(B.l[PARTS].e[i], &x, &z) && pair_b_tile(x, z)) parts[np] = dup_buf(B.l[PARTS].e[i]), src_is_b[np] = 1, src_i[np++] = i;
    int *use = xalloc(sizeof(int) * (nm + 1));
    memset(use, 0, sizeof(int) * (nm + 1));
    for (int i = 0; i < np; i++) {
        int old = (int)rd32(parts[i].p + 0x14), nw = src_is_b[i] ? (old >= 0 && old < nbm ? map_b[old] : -1) : (old >= 0 && old < na ? map_a[old] : -1);
        if (nw < 0) return logf_("pair: a part's model index is out of range"), none;
        wr32(parts[i].p + 0x14, (u32)nw), use[nw]++;
    }
    int *renum = xalloc(sizeof(int) * (nm + 1)), kept = 0;
    buf *mout = xalloc(sizeof(buf) * (nm + 1));
    for (int k = 0; k < nm; k++) {
        renum[k] = use[k] ? kept : -1;
        if (!use[k]) continue;
        mout[kept] = dup_buf(mod[k]);
        wr32(mout[kept].p + 0x18, (u32)use[k]);
        if (from_b[k] && rd32(mout[kept].p + 8) == 0 && *nb < cap) {  // a map piece of B's
            msb_name(mout[kept], b_name, sizeof b_name);
            if (b_name[0] == 'm' && strlen(b_name) < 16) strcpy(names_b[(*nb)++], b_name + 1);
        }
        kept++;
    }
    type_counter tc;
    memset(&tc, 0, sizeof tc);  // model index in type
    for (int k = 0; k < kept; k++) wr32(mout[k].p + 0xc, (u32)type_next(&tc, rd32(mout[k].p + 8)));
    memset(&tc, 0, sizeof tc);
    for (int i = 0; i < np; i++) {
        wr32(parts[i].p + 0x14, (u32)renum[rd32(parts[i].p + 0x14)]);
        u32 t = rd32(parts[i].p + 0xc);
        wr32(parts[i].p + 0x10, (u32)type_next(&tc, t));
        if (t == 13 && parts[i].n >= 0x378)
            for (int o = 0x368; o <= 0x374; o += 4) if (o != 0x370 && (int)rd32(parts[i].p + o) >= 0) wr32(parts[i].p + o, (u32)i);
    }
    // events: A's outside, B's on B's tiles; part references by name
    int ea = A.l[EVENT].n, eb = B.l[EVENT].n, ne = 0, lost = 0;
    buf *ev = xalloc(sizeof(buf) * (ea + eb + 1));
    for (int s2 = 0; s2 < 2; s2++) {
        const msb_file *S = s2 ? &B : &A;
        for (int i = 0; i < S->l[EVENT].n; i++) {
            int on_b = msb_tile(S->l[EVENT].e[i], &x, &z) && pair_b_tile(x, z);
            if (on_b != s2) continue;
            buf e = dup_buf(S->l[EVENT].e[i]);
            u32 et = rd32(e.p + 0xc);
            u64 to = e.n >= 0x30 ? rd64(e.p + 0x20) : 0;
            int fo = et == 4 ? 8 : et == 7 ? 4 : -1;
            if (fo >= 0 && to && to + fo + 4 <= e.n) {
                int old = (int)rd32(e.p + to + fo), nw = -1;
                if (old >= 0 && old < S->l[PARTS].n) {
                    char want[64], have[64];
                    msb_name(S->l[PARTS].e[old], want, sizeof want);
                    for (int k = 0; k < np && nw < 0; k++) {
                        msb_name(parts[k], have, sizeof have);
                        if (!strcmp(want, have)) nw = k;
                    }
                    lost += nw < 0;
                }
                wr32(e.p + to + fo, (u32)nw);
            }
            ev[ne++] = e;
        }
    }
    memset(&tc, 0, sizeof tc);
    for (int i = 0; i < ne; i++) wr32(ev[i].p + 0x10, (u32)type_next(&tc, rd32(ev[i].p + 0xc)));
    // points: by tile, index in type renumbered
    int qa = A.l[POINT].n, qb = B.l[POINT].n, nq = 0;
    buf *pt = xalloc(sizeof(buf) * (qa + qb + 1));
    for (int i = 0; i < qa; i++) if (!(msb_tile(A.l[POINT].e[i], &x, &z) && pair_b_tile(x, z))) pt[nq++] = dup_buf(A.l[POINT].e[i]);
    for (int i = 0; i < qb; i++) if (msb_tile(B.l[POINT].e[i], &x, &z) && pair_b_tile(x, z)) pt[nq++] = dup_buf(B.l[POINT].e[i]);
    memset(&tc, 0, sizeof tc);
    for (int i = 0; i < nq; i++) wr32(pt[i].p + 0xc, (u32)type_next(&tc, rd32(pt[i].p + 8)));
    M.l[MODEL].e = mout, M.l[MODEL].n = kept;
    M.l[PARTS].e = parts, M.l[PARTS].n = np;
    M.l[EVENT].e = ev, M.l[EVENT].n = ne;
    M.l[POINT].e = pt, M.l[POINT].n = nq;
    buf out = msb_write(&M);
    // read back: counts, model indices in range and use counts, own-index fields
    msb_file R;
    int bad = !msb_parse(out, &R) || R.l[PARTS].n != np || R.l[MODEL].n != kept;
    for (int i = 0; !bad && i < np; i++) {
        u32 mi = rd32(R.l[PARTS].e[i].p + 0x14);
        bad = mi >= (u32)kept;
    }
    int from_b_parts = 0;
    for (int i = 0; i < np; i++) from_b_parts += src_is_b[i];
    logf_("pair: composed half: %d parts (%d %s, %d %s), %d models, %d events, %d points, %d %s pieces; check: %s%s",
          np, np - from_b_parts, pair_a()->name, from_b_parts, pair_b()->name, kept, ne, nq, *nb, pair_b()->name, bad ? "FAILED" : "read back fine",
          lost ? ", event part references lost" : "");
    return bad ? none : out;
}
static int compose_one_half(const char *game_dir, buf bhd, const char *bdt_path, int east) {
    char to[1300], from[300], an[64], bn[64], half[8];
    int av = pair_a()->kind, bv = pair_b()->kind;
    snprintf(half, sizeof half, "%s", east ? "11_09" : "10_09");
    g_half_x0 = east ? 44 : 42, g_half_x1 = east ? 45 : 43;
    snprintf(an, sizeof an, "m60_%s_%d2", half, av), snprintf(bn, sizeof bn, "m60_%s_%d2", half, bv);
    snprintf(from, sizeof from, "/map/mapstudio/%s.msb.dcx", an);
    buf a = archive_read(bhd, bdt_path, from);
    snprintf(from, sizeof from, "/map/mapstudio/%s.msb.dcx", bn);
    buf b = archive_read(bhd, bdt_path, from);
    buf ra = a.p ? dcx_read(game_dir, a) : a, rb = b.p ? dcx_read(game_dir, b) : b;
    static char names[512][16];
    int n = 0, ok = 0, copied = 0;
    if (ra.p && rb.p) {
        logf_("pair: composing %s from %s with the %s tiles of %s", an, an, pair_b()->name, bn);
        buf m = msb_compose(ra, rb, names, &n, 512);
        if (m.p) {
            buf d = dcx_write(m);
            snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "mapstudio" SEP "%s.msb.dcx", g_app, an);
            make_parents(to);
            ok = d.p && write_file(to, d.p, d.n);
            free(d.p), free(m.p);
        }
    } else logf_("pair: %s or %s not found in data2", an, bn);
    for (int i = 0; ok && i < n; i++) {  // b's pieces, under the composed half's name (bytes as in the archive)
        snprintf(from, sizeof from, "/map/m60/%s/%s_%s.mapbnd.dcx", bn, bn, names[i]);
        buf p = archive_read(bhd, bdt_path, from);
        if (!p.p) { logf_("pair: piece %s missing in data2", from); ok = 0; break; }
        snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "m60" SEP "%s" SEP "%s_%s.mapbnd.dcx", g_app, an, an, names[i]);
        make_parents(to);
        ok = write_file(to, p.p, p.n), copied += ok;
        free(p.p);
    }
    free(a.p), free(b.p), free(ra.p), free(rb.p);
    if (ok) logf_("pair (test): %s + %s %s half %s written (map file + %d %s pieces under the half's name; lighting: the %s)", pair_a()->name,
                  pair_b()->name, east ? "east" : "west", an, copied, pair_b()->name, pair_a()->of);
    return ok;
}
// variant-test (measurement, four pairs side by side): the west half as this start has it (composed on pairs 1 / 3,
// else the base region's own) is written again under variant 6, m60_10_09_62: the map file, and every file of the
// half's folder (data2 list, 1.3.3: pieces m60_10_09_V2_<x-40><z-32><xx><zz><nn>, envmap_00..06_{high,middle,low}_00,
// {high,middle,low}.ivinfobnd) under the new name; pieces of the added region already written for the composed half
// are taken from work/build. The DLL (same setting) gives the west half variant 6.
static void variant_test_half(buf bhd, const char *bdt) {
    char an[24], from[300], to[1300], name[64];
    int av = pair_a()->kind, copied = 0;
    snprintf(an, sizeof an, "m60_10_09_%d2", av);
    snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "mapstudio" SEP "%s.msb.dcx", g_app, an);
    buf m = read_file(to);
    if (!m.p) snprintf(from, sizeof from, "/map/mapstudio/%s.msb.dcx", an), m = archive_read(bhd, bdt, from);
    snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "mapstudio" SEP "m60_10_09_62.msb.dcx", g_app);
    if (!m.p || !write_file(to, m.p, m.n)) { free(m.p); logf_("variant test: west half %s not written as variant 6", an); return; }
    free(m.p);
    for (int k = 0; k < 2 * 4 * 100 + 21 + 3; k++) {
        if (k < 800) {
            int x = 42 + k / 400, zz = 36 + (k / 100) % 4, nn = k % 100;
            snprintf(name, sizeof name, "%d%d%02d%02d%02d", x - 40, zz - 32, x, zz, nn);
        } else if (k < 821) {
            static const char *const Q[3] = {"high", "low", "middle"};
            snprintf(name, sizeof name, "envmap_%02d_%s_00", (k - 800) / 3, Q[(k - 800) % 3]);
        } else {
            static const char *const Q[3] = {"high", "low", "middle"};
            snprintf(name, sizeof name, "%s", Q[k - 821]);
        }
        const char *ext = k < 800 ? "mapbnd" : k < 821 ? "tpfbnd" : "ivinfobnd";
        char sep = k < 821 ? '_' : '_';
        snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "m60" SEP "%s" SEP "%s%c%s.%s.dcx", g_app, an, an, sep, name, ext);
        buf p = read_file(to);
        if (!p.p) snprintf(from, sizeof from, "/map/m60/%s/%s%c%s.%s.dcx", an, an, sep, name, ext), p = archive_read(bhd, bdt, from);
        if (!p.p) continue;
        snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "map" SEP "m60" SEP "m60_10_09_62" SEP "m60_10_09_62_%s.%s.dcx", g_app, name, ext);
        copied += write_file(to, p.p, p.n);
        free(p.p);
    }
    logf_("variant test: west half %s written as variant 6 (m60_10_09_62: map file and %d files of its folder)", an, copied);
}
static int build_pair_half(const char *game_dir) {
    char bhd_path[1100], bdt_path[1100];
    snprintf(bhd_path, sizeof bhd_path, "%s" SEP "data2.bhd", game_dir);
    snprintf(bdt_path, sizeof bdt_path, "%s" SEP "data2.bdt", game_dir);
    rsa_use(DATA2_N, DATA2_E);
    buf enc = read_file(bhd_path), bhd = enc.p ? rsa_decrypt(enc) : enc;
    free(enc.p);
    rsa_use(DATA0_N, DATA0_E);
    if (!bhd.p || memcmp(bhd.p, "BHD5", 4)) return logf_("pair: data2 could not be read"), 0;
    int ok = 1;
    for (int east = 0; ok && east < 2; east++)
        if (two_pair()->halves >> east & 1) ok = compose_one_half(game_dir, bhd, bdt_path, east);
    if (ok && setting(S_VARIANT_TEST)) variant_test_half(bhd, bdt_path);
    free(bhd.p);
    return ok;
}

// Boss deaths for the DLL's day clock (boss-flow 3): every flag a site-of-grace chain waits for, i.e. the first
// parameter of common_func 9005810 and the third of 90015006 in "Initialize (Common) Event" (2000[0] / 2000[6]:
// slot +0, event id +4, parameters from +8) of the field tiles' scripts (m60_42..45_36..39, every variant) and the
// small maps' (mAA_BB_00_00), read from the game archive. Written to work/native/boss-deaths.txt (header line with the
// count, one id per line); the game's ids are never shipped. Returns the number of flags, -1 on failure.
// EMEVD: +0x10 event count, +0x18 events (0x30 each: id u64, instruction count u64, offset u64, ...), +0x28
// instructions (0x20 each: bank u32, id u32, argument length u64, argument offset s64), +0x78 argument data.
// Day 2's count (day flow, owner 2026-11-21: needed = min(6, ordinary bosses still alive)): the ordinary bosses of each
// script, by source (g_day_src: small map AABB, or 100000 + tile X * 1000 + Z * 10 + variant; -1 not recorded). A boss
// is a kill template's call that always runs: 90045002 (its shared flag), or 90015002 / 90015025 / 90015407 whose first
// two arguments are 0 (the others wait for an event flag, e.g. 8076 in m45_52, or 8081 the interference: event bosses).
// Not counted: the special bosses (DLL df_special). Flags that may be alternatives (Noklateo's Royal Revenants
// 1026500820 / 860, the Tree Sentinels 42380800 / 805) count one each (owner, 2026-11-21: an error of 1-2 is fine).
static const u32 DAY_NOT_COUNTED[] = {1028100800, 1029100800, 1039100800, 1039100801, 1056300800, 1048202800, 1026500800};
#define DAY_BOSS_MAX 1024
static struct { int src; u32 flag; } g_day_boss[DAY_BOSS_MAX];
static int g_nday_boss, g_day_src = -1;
static u8 g_tile_file[4][4][6];  // m60_X_Z_V0.emevd exists (X 42.., Z 36.., V): the tile's variant (as its msb: same set)
static void day_boss_add(u32 flag) {
    for (unsigned i = 0; i < sizeof DAY_NOT_COUNTED / sizeof *DAY_NOT_COUNTED; i++) if (DAY_NOT_COUNTED[i] == flag) return;
    for (int i = 0; i < g_nday_boss; i++) if (g_day_boss[i].src == g_day_src && g_day_boss[i].flag == flag) return;
    if (g_nday_boss < DAY_BOSS_MAX) g_day_boss[g_nday_boss].src = g_day_src, g_day_boss[g_nday_boss++].flag = flag;
}
static int emevd_boss_deaths(buf d, u32 *flags, int have, int cap, int night_arena) {
    if (d.n < 0x80 || memcmp(d.p, "EVD", 3)) return have;
    u64 ec = rd64(d.p + 0x10), eo = rd64(d.p + 0x18), io = rd64(d.p + 0x28), ao = rd64(d.p + 0x78);
    for (u64 e = 0; e < ec && eo + (e + 1) * 0x30 <= d.n; e++) {
        const u8 *ev = d.p + eo + e * 0x30;
        u64 n = rd64(ev + 8), ioff = rd64(ev + 0x10);
        for (u64 k = 0; k < n && io + ioff + (k + 1) * 0x20 <= d.n; k++) {
            const u8 *in = d.p + io + ioff + k * 0x20;
            u32 bank = rd32(in), id = rd32(in + 4);
            u64 alen = rd64(in + 8), aoff = rd64(in + 0x10);
            if (bank != 2000 || (id != 0 && id != 6) || ao + aoff + alen > d.n || alen < 12) continue;
            const u8 *a = d.p + ao + aoff;
            u32 ev_id = rd32(a + 4), flag = 0;
            // The game's own boss kills: the common_func templates that run "Increment Team Bosses Killed" (2003[101])
            // and then set the boss's death flag (2003[69]) from the caller's arguments (common_func, 1.3.3): 90015002 /
            // 90015025 / 90015407 its third argument, 90035013 its first, 90045002 its fourth (its first is the site's
            // shared flag). Run of dev.67 (pair 3): bosses outside the grace chains below went uncounted (e.g. the
            // Equilibrious Beast 1044360851, the event bosses of m45_5x, the evergaol-like m46_50 / 60 / 70 duels).
            // The night bosses' templates (90065900, 90075901 / 904) have no such argument and stay out; so do the
            // night-boss arenas' scripts (m47, m48, m49, m52: night_arena) and 90035013 (its first argument is a camp
            // invader's death, m30-m38 / m50-m52 xx705: invaders are not bosses of the day).
            // 90045002 (the m46_50 / 60 / 70 / 80 duels, one call per variant): after one Increment it sets both its first
            // argument (the site's shared flag, the same as the site's 9005810 grace flag) and its fourth (the variant's;
            // variant 0 of m46_70 / m46_80 passes the shared flag there too), and ends at once when the shared flag is on
            // (1003[2]). So only the shared flag is taken: one duel = one flag whatever the variant (run of 2026-11-21:
            // 46600800 + 46600820 counted 2). Its boss characters are listed under it (emevd_boss_entities).
            static const struct { u32 ev; u32 at; } KILL[] = {{90015002, 8}, {90015025, 8}, {90015407, 8}, {90045002, 0}};
            int kill = -1;
            for (unsigned t = 0; t < sizeof KILL / sizeof *KILL; t++) if (KILL[t].ev == ev_id) kill = (int)t;
            if (ev_id == 9005810) flag = rd32(a + 8);
            else if (ev_id == 90015006 && alen >= 20) flag = rd32(a + 16);
            else if (kill >= 0 && !night_arena && alen >= 8 + KILL[kill].at + 4) flag = rd32(a + 8 + KILL[kill].at);
            else continue;
            if (flag < 10000000u) continue;
            if (kill >= 0 && g_day_src >= 0 && (ev_id == 90045002 || (alen >= 16 && !rd32(a + 8) && !rd32(a + 12)))) day_boss_add(flag);
            int seen = 0;
            for (int i = 0; i < have && !seen; i++) seen = flags[i] == flag;
            if (!seen && have < cap) flags[have++] = flag;
        }
    }
    return have;
}
static int night_arena_file(const char *name) {  // "/event/mAA_BB_00_00.emevd.dcx" of AA 47, 48, 49 or 52
    const char *m = strstr(name, "/m");
    int aa = m ? atoi(m + 2) : 0;
    return aa == 47 || aa == 48 || aa == 49 || aa == 52;
}
static int cmp_u32(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }
// The boss character of each death flag (for hiding bosses while the night boss is active): a boss's death is set by
// the event that watches the boss die, either a common_func template (an instruction "IF Character Dead/Alive"
// 4[0], "IF Character HP Value" 4[14], "IF Character Ratio Dead/Alive" 4[15] or "IF Character Ratio HP Value" 4[23]
// on an entity from the caller's arguments, and "Set (Network-Connected) Event Flag" 2003[66] / 2003[69] with a flag
// from the caller's arguments: 90015002, 90015407, 90035013, 90045002 ...) or a map script's own event with both
// literal. Parameter table of an event: +0x18 count, +0x20 offset into the table at header +0x58, entries 0x20: index
// of the instruction u64, offset in its arguments u64, offset in the caller's arguments u64, size u32.
#define BOSS_TMPL_MAX 8192
static struct { u32 id; int nf, ne; u32 fsrc[4], esrc[4]; } g_tmpl[BOSS_TMPL_MAX];
static int g_ntmpl;
static int is_death_cond(u32 bank, u32 id) { return bank == 4 && (id == 0 || id == 14 || id == 15 || id == 23); }
static int is_flag_set(u32 bank, u32 id) { return bank == 2003 && (id == 66 || id == 69); }
static void emevd_templates(buf d) {
    if (d.n < 0x80 || memcmp(d.p, "EVD", 3)) return;
    u64 ec = rd64(d.p + 0x10), eo = rd64(d.p + 0x18), io = rd64(d.p + 0x28), po = rd64(d.p + 0x58);
    for (u64 e = 0; e < ec && eo + (e + 1) * 0x30 <= d.n && g_ntmpl < BOSS_TMPL_MAX; e++) {
        const u8 *ev = d.p + eo + e * 0x30;
        u64 ioff = rd64(ev + 0x10), pc = rd64(ev + 0x18), poff = rd64(ev + 0x20);
        int nf = 0, ne = 0;
        u32 fsrc[4], esrc[4];
        for (u64 k = 0; k < pc && po + poff + (k + 1) * 0x20 <= d.n; k++) {
            const u8 *pe = d.p + po + poff + k * 0x20;
            u64 idx = rd64(pe), tgt = rd64(pe + 8), src = rd64(pe + 0x10);
            if (io + ioff + (idx + 1) * 0x20 > d.n || tgt != 4) continue;
            const u8 *in = d.p + io + ioff + idx * 0x20;
            if (is_death_cond(rd32(in), rd32(in + 4)) && ne < 4) esrc[ne++] = (u32)src;
            if (is_flag_set(rd32(in), rd32(in + 4)) && nf < 4) fsrc[nf++] = (u32)src;
        }
        if (nf && ne) {
            g_tmpl[g_ntmpl].id = (u32)rd64(ev), g_tmpl[g_ntmpl].nf = nf, g_tmpl[g_ntmpl].ne = ne;
            memcpy(g_tmpl[g_ntmpl].fsrc, fsrc, sizeof fsrc), memcpy(g_tmpl[g_ntmpl].esrc, esrc, sizeof esrc), g_ntmpl++;
        }
    }
}
#define BOSS_ENT_MAX 8
static u32 g_boss_ent[2048][BOSS_ENT_MAX];
static int g_boss_nent[2048];
static void boss_entity_add(const u32 *flags, int have, u32 flag, u32 ent) {
    const u32 *at = bsearch(&flag, flags, (size_t)have, sizeof *flags, cmp_u32);
    if (!at || ent < 1000 || ent == 10000 || ent == 20000) return;
    int i = (int)(at - flags);
    for (int k = 0; k < g_boss_nent[i]; k++) if (g_boss_ent[i][k] == ent) return;
    if (g_boss_nent[i] < BOSS_ENT_MAX) g_boss_ent[i][g_boss_nent[i]++] = ent;
}
static void emevd_boss_entities(buf d, const u32 *flags, int have) {
    if (d.n < 0x80 || memcmp(d.p, "EVD", 3)) return;
    u64 ec = rd64(d.p + 0x10), eo = rd64(d.p + 0x18), io = rd64(d.p + 0x28), ao = rd64(d.p + 0x78);
    for (u64 e = 0; e < ec && eo + (e + 1) * 0x30 <= d.n; e++) {
        const u8 *ev = d.p + eo + e * 0x30;
        u64 n = rd64(ev + 8), ioff = rd64(ev + 0x10);
        u32 ents[16], fls[16];
        int ne = 0, nf = 0;
        for (u64 k = 0; k < n && io + ioff + (k + 1) * 0x20 <= d.n; k++) {
            const u8 *in = d.p + io + ioff + k * 0x20;
            u32 bank = rd32(in), id = rd32(in + 4);
            u64 alen = rd64(in + 8), aoff = rd64(in + 0x10);
            if (ao + aoff + alen > d.n) continue;
            const u8 *a = d.p + ao + aoff;
            if (bank == 2000 && (id == 0 || id == 6) && alen >= 8) {  // a call: through its template
                u32 ev_id = rd32(a + 4);
                for (int t = 0; t < g_ntmpl; t++) {
                    if (g_tmpl[t].id != ev_id) continue;
                    for (int f = 0; f < g_tmpl[t].nf; f++)
                        for (int x = 0; x < g_tmpl[t].ne; x++)
                            if (8 + g_tmpl[t].fsrc[f] + 4 <= alen && 8 + g_tmpl[t].esrc[x] + 4 <= alen)
                                boss_entity_add(flags, have, rd32(a + 8 + g_tmpl[t].fsrc[f]), rd32(a + 8 + g_tmpl[t].esrc[x]));
                    break;
                }
            } else if (is_death_cond(bank, id) && alen >= 8 && ne < 16) ents[ne++] = rd32(a + 4);
            else if (is_flag_set(bank, id) && alen >= 8 && nf < 16) fls[nf++] = rd32(a + 4);
        }
        for (int f = 0; f < nf; f++)  // the script's own event, both literal
            for (int x = 0; x < ne; x++) boss_entity_add(flags, have, fls[f], ents[x]);
    }
}
// Day flow, invader camps (owner, 2026-11-19: one invader from the start, a second after 5 bosses). How many invaders a
// camp gets is read twice, both 1 for the caves m30_00 / m30_30 and m38_00 / m38_10 (2 for m32_* / m34_*):
// - the generator's camp roll (0x1406bb760): per camp, SmallBaseMapVariationParam (param 0xca, 0x140e80950) row = the
//   small map id, +0x08 (capped at 2) invaders, which get the SmallbaseInvationNpcParam rows and entity ids +50, +51;
// - the camp's own script: event 0 of mAA_BB_00_00.emevd starts common 90015310 (2000[06]: slot, event id, then
//   chr +50, chr +51, count, ...); the script warps and turns on +51 only when count is 2.
// Every one of the four has a c0100 character +51 (msb). So both become 2: the param row in the regulation, and the
// count in each script, written to work/build/event (me3 overlays build/; the game ships some event scripts as DFLT
// DCX itself, which dcx_write writes). The DLL keeps +51 off until the day's fifth boss.
#define CAMP_SCRIPT_EVENT 90015310
static const int CAMP_ONE_MAPS[4] = {3000, 3030, 3800, 3810};
// The count in event 0's start of 90015310 for small map aabb (entities aabb0050 / aabb0051): 1 -> 2. 1 if changed.
static int emevd_camp_count(buf d, int map) {
    if (d.n < 0x80 || memcmp(d.p, "EVD", 3)) return 0;
    u64 ev = rd64(d.p + 0x18), ins = rd64(d.p + 0x28), args = rd64(d.p + 0x78);
    u32 n = (u32)rd64(d.p + 0x10);
    for (u32 e = 0; e < n && ev + (u64)(e + 1) * 0x30 <= d.n; e++) {
        const u8 *E = d.p + ev + e * 0x30;
        if (rd64(E) != 0) continue;
        u64 cnt = rd64(E + 8), off = rd64(E + 0x10);
        for (u64 k = 0; k < cnt && ins + off + (k + 1) * 0x20 <= d.n; k++) {
            const u8 *I = d.p + ins + off + k * 0x20;
            u64 alen = rd64(I + 8), aoff = rd64(I + 0x10);
            if (rd32(I) != 2000 || rd32(I + 4) != 6 || alen < 20 || args + aoff + alen > d.n) continue;
            u8 *a = d.p + args + aoff;
            if (rd32(a + 4) == CAMP_SCRIPT_EVENT && rd32(a + 8) == (u32)map * 10000 + 50 && rd32(a + 12) == (u32)map * 10000 + 51 &&
                rd32(a + 16) == 1)
                return wr32(a + 16, 2), 1;
        }
    }
    return 0;
}
static int build_camp_scripts(const char *game_dir, buf bhd, const char *bdt) {
    char names[4][48], path[1300];
    buf in[4], out[4];
    for (int i = 0; i < 4; i++) {
        snprintf(names[i], sizeof names[i], "/event/m%02d_%02d_00_00.emevd.dcx", CAMP_ONE_MAPS[i] / 100, CAMP_ONE_MAPS[i] % 100);
        in[i] = archive_read(bhd, bdt, names[i]);
        if (!in[i].p) {
            for (int j = 0; j < i; j++) free(in[j].p);
            return logf_("Day flow: %s not in the game archive", names[i]), 0;
        }
    }
    dcx_read_many(game_dir, in, out, 4);
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (!out[i].p || !emevd_camp_count(out[i], CAMP_ONE_MAPS[i])) {
            if (ok) logf_("Day flow: %s: the camp script's start looks different than expected", names[i]);
            ok = 0;
            continue;
        }
        buf z = dcx_write(out[i]);
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "event" SEP "%s", g_app, names[i] + 7);
        ok = write_file(path, z.p, z.n) && ok;
        free(z.p);
    }
    for (int i = 0; i < 4; i++) free(in[i].p), free(out[i].p);
    if (ok) logf_("Day flow: invader camps of 4 small maps take 2 invaders (camp scripts written)");
    return ok;
}
// SmallBaseMapVariationParam rows of the four: +0x08 1 -> 2 (see above). A copy, 0 if a row is not as expected.
static buf camp_variation_rows(buf p) {
    buf c = bnd4_copy(p);
    int done = 0;
    for (u32 i = 0; c.p && i < rd16(c.p + 0x0A); i++) {
        int id = (int)rd32(c.p + 0x40 + i * 24);
        u8 *r = c.p + rd64(c.p + 0x40 + i * 24 + 8);
        for (int k = 0; k < 4; k++)
            if (id == CAMP_ONE_MAPS[k] && rd32(r + 8) == 1) wr32(r + 8, 2), done++;
    }
    if (done != 4) free(c.p), c.p = NULL;
    return c;
}
static int build_boss_deaths(const char *game_dir, buf bhd, const char *bdt) {
    static char names[1 + 4 * 4 * 6 + 40 * 100][48];
    static buf in[sizeof names / sizeof names[0]], out[sizeof names / sizeof names[0]];
    int n = 0;
    snprintf(names[n++], sizeof names[0], "/event/common_func.emevd.dcx");  // the templates (first)
    for (int x = 42; x <= 45; x++)
        for (int y = 36; y <= 39; y++)
            for (int v = 0; v <= 5; v++) snprintf(names[n++], sizeof names[0], "/event/m60_%02d_%02d_%d0.emevd.dcx", x, y, v);
    for (int aa = 20; aa <= 59; aa++)
        for (int bb = 0; bb <= 99; bb++) snprintf(names[n++], sizeof names[0], "/event/m%02d_%02d_00_00.emevd.dcx", aa, bb);
    int found = 0;
    for (int i = 0; i < n; i++) {
        in[i] = archive_read(bhd, bdt, names[i]);
        if (in[i].p) in[found] = in[i], memmove(names[found], names[i], sizeof names[0]), found += 1;
    }
    dcx_read_many(game_dir, in, out, found);
    static u32 flags[2048];
    int have = 0, read = 0;
    int failed = -1;
    char kind[8] = "";
    g_nday_boss = 0;
    memset(g_tile_file, 0, sizeof g_tile_file);
    for (int i = 0; i < found; i++) {
        int x, z, v, aa, bb;
        g_day_src = -1;
        if (sscanf(names[i], "/event/m60_%d_%d_%1d0.emevd", &x, &z, &v) == 3 && x >= 42 && x <= 45 && z >= 36 && z <= 39 && v <= 5)
            g_tile_file[x - 42][z - 36][v] = 1, g_day_src = 100000 + x * 1000 + z * 10 + v;
        else if (sscanf(names[i], "/event/m%d_%d_00_00.emevd", &aa, &bb) == 2) g_day_src = aa * 100 + bb;
        if (out[i].p) read++, have = emevd_boss_deaths(out[i], flags, have, 2048, night_arena_file(names[i]));
        else if (failed < 0) failed = i, snprintf(kind, sizeof kind, "%.4s", in[i].p && in[i].n >= 0x2C ? (const char *)in[i].p + 0x28 : "?");
    }
    g_day_src = -1;
    if (failed >= 0) logf_("Boss deaths: %s not unpacked (compression %s)", names[failed], kind);
    qsort(flags, (size_t)have, sizeof *flags, cmp_u32);
    g_ntmpl = 0;
    memset(g_boss_nent, 0, sizeof g_boss_nent);
    if (found && out[0].p && strstr(names[0], "common_func")) emevd_templates(out[0]);
    for (int i = 0; i < found; i++) if (out[i].p) emevd_boss_entities(out[i], flags, have);
    for (int i = 0; i < found; i++) free(in[i].p), free(out[i].p);
    int with = 0;
    for (int i = 0; i < have; i++) with += g_boss_nent[i] > 0;
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "boss-deaths.txt", g_app);
    size_t cap = 128 + (size_t)have * 12 * (1 + BOSS_ENT_MAX), at = 0;
    char *text = xalloc(cap);
    at += (size_t)snprintf(text + at, cap - at, "# boss death flags: %d (each: flag, then the boss characters' entity ids)\n", have);
    for (int i = 0; i < have; i++) {
        at += (size_t)snprintf(text + at, cap - at, "%u", flags[i]);
        for (int k = 0; k < g_boss_nent[i]; k++) at += (size_t)snprintf(text + at, cap - at, " %u", g_boss_ent[i][k]);
        at += (size_t)snprintf(text + at, cap - at, "\n");
    }
    int ok = read == found && have > 0 && write_file(path, (const u8 *)text, at);
    free(text);
    logf_("Boss deaths for the day clock: %d flags (%d with their boss character, %d templates) from %d of %d event scripts", have, with,
          g_ntmpl, read, found);
    return ok ? have : -1;
}

// Day 2 (day flow): every merged pattern's number of ordinary bosses -> work/native/pattern-bosses.txt, "pattern count"
// (the DLL: alive at 7505 = count - ordinary bosses killed on day 1). A pattern's bosses: one per small map placed for
// good (LotResultSmallBaseAndSpot rows of modifier 0, +0x14; +0xc map) that has a boss, counted once (its several flags,
// e.g. m32_10's 800 / 810 / 820, may be its variants' alternatives; owner 2026-11-21: an error of 1-2 is fine), and
// every boss of its field tiles' scripts, each tile in the variant the map shows: the added region's on its tiles (the
// DLL's variant_hook), elsewhere the first of (base, added) region that has a file for the tile (CalcGetReplaceMapIdByDisaster over gm
// +0xbb8 = [base, added]), else the plain one. Also the counts' range in the build log (offline check).
static int day_tile_variant(int x, int z) {
    const u8 *has = g_tile_file[x - 42][z - 36];
    if (site_added_tile(x, z)) return pair_b()->kind;
    return has[pair_a()->kind] ? pair_a()->kind : has[pair_b()->kind] ? pair_b()->kind : 0;
}
static int write_pattern_bosses(buf bases) {
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "pattern-bosses.txt", g_app);
    if (!g_nday_boss || param_row_size(bases) != 0x18) return logf_("Day flow: no boss list for day 2 (pattern-bosses.txt not written)"), remove(path), 1;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Written by the launcher: pattern, the number of its ordinary bosses (day 2's count).\n");
    int lo = 999, hi = 0, tiles = 0;
    for (int x = 42; x <= 45; x++)
        for (int z = 36; z <= 39; z++)
            for (int i = 0; i < g_nday_boss; i++) tiles += g_day_boss[i].src == 100000 + x * 1000 + z * 10 + day_tile_variant(x, z);
    for (int pat = 0; pat < TWO_PATTERNS; pat++) {
        if (!merged_slot(pat)) continue;
        int n = tiles;
        for (u32 r = 0; r < rd16(bases.p + 0x0A); r++) {
            const u8 *row = bases.p + rd64(bases.p + 0x40 + r * 24 + 8);
            int m = (int)rd32(row + 0xc), boss = 0;
            if ((int)rd32(row + 4) != pat || rd32(row + 0x14) || m <= 0 || m >= 10000) continue;
            for (int i = 0; i < g_nday_boss && !boss; i++) boss = g_day_boss[i].src == m;
            n += boss;
        }
        fprintf(f, "%d %d\n", pat, n);
        if (n < lo) lo = n;
        if (n > hi) hi = n;
    }
    fclose(f);
    logf_("Day flow: ordinary bosses per pattern (day 2's count): %d..%d on the 200 merged patterns (%d of them on the field tiles)", lo, hi, tiles);
    return 1;
}

// The fire ring: the circle that closes around the arena after the challenge (its border effect 807011, and 807800 on
// a player outside it: SpEffect 99810 / 99811 -> 99871 / 99876 -> SpEffectVfx 200050) is blue in the game. Its
// textures are grey, the blue is only in the effects' colour floats, so those are rewritten in place (sizes stay the
// same): each blue colour gets a fire hue (45 - 37 * saturation degrees: pale -> yellow-orange, deep -> red; bloom 38,
// yellow-orange), its brightest channel and alpha kept, saturation raised to 1 - (1 - s)^2. 807800's white bloom too.
// Each entry: offset in the .fxr, the game's value, ours. A file whose values differ (a game update) is left as it is.
static const u32 FIRE_807011[][3] = {
    {0x3b3c, 0x3f3fbfc0, 0x3f800000}, {0x3b40, 0x3f4acacb, 0x3f52836a}, {0x3b44, 0x3f800000, 0x3f0f9fb0}, {0x3b4c, 0x3f3fbfc0, 0x3f800000},
    {0x3b50, 0x3f4acacb, 0x3f52836a}, {0x3b54, 0x3f800000, 0x3f0f9fb0}, {0x3b64, 0x3f3fbfc0, 0x3f800000}, {0x3b68, 0x3f4acacb, 0x3f52836a},
    {0x3b6c, 0x3f800000, 0x3f0f9fb0}, {0x3b74, 0x3f3fbfc0, 0x3f800000}, {0x3b78, 0x3f4acacb, 0x3f52836a}, {0x3b7c, 0x3f800000, 0x3f0f9fb0},
    {0x3be4, 0x3e800000, 0x3f800000}, {0x3be8, 0x3f000000, 0x3f280000}, {0x3bec, 0x3f800000, 0x3d800000}, {0x3ccc, 0x3e808081, 0x3f800000},
    {0x3cd0, 0x3ec0c0c1, 0x3eaa781f}, {0x3cd4, 0x3f800000, 0x3d810183}, {0x3cdc, 0x3f008081, 0x3f800000}, {0x3ce0, 0x3f3fbfc0, 0x3f154ff7},
    {0x3ce4, 0x3f800000, 0x3e810183}, {0x3cfc, 0x3f008081, 0x3f800000}, {0x3d00, 0x3f3fbfc0, 0x3f154ff7}, {0x3d04, 0x3f800000, 0x3e810183},
    {0x3d0c, 0x3eda740e, 0x3f800000}, {0x3d10, 0x3f232323, 0x3f019e15}, {0x3d14, 0x3f800000, 0x3e3a69dc}, {0x3d1c, 0x3ec0c0c2, 0x3f800000},
    {0x3d20, 0x3f101010, 0x3ee92cba}, {0x3d24, 0x3f800000, 0x3e1121b4}, {0x3d2c, 0x3e808081, 0x3f800000}, {0x3d30, 0x3ec0c0c1, 0x3eaa781f},
    {0x3d34, 0x3f800000, 0x3d810183}, {0x3f30, 0x3f008081, 0x3f800000}, {0x3f34, 0x3f3fbfc0, 0x3f154ff7}, {0x3f38, 0x3f800000, 0x3e810183},
    {0x3f74, 0x3e800000, 0x3f800000}, {0x3f78, 0x3f000000, 0x3f280000}, {0x3f7c, 0x3f800000, 0x3d800000}, {0x4010, 0x3e808081, 0x3f800000},
    {0x4014, 0x3ec0c0c1, 0x3eaa781f}, {0x4018, 0x3f800000, 0x3d810183}, {0x4020, 0x3f008081, 0x3f800000}, {0x4024, 0x3f3fbfc0, 0x3f154ff7},
    {0x4028, 0x3f800000, 0x3e810183}, {0x4038, 0x3f008081, 0x3f800000}, {0x403c, 0x3f3fbfc0, 0x3f154ff7}, {0x4040, 0x3f800000, 0x3e810183},
    {0x4048, 0x3e808081, 0x3f800000}, {0x404c, 0x3ec0c0c1, 0x3eaa781f}, {0x4050, 0x3f800000, 0x3d810183}, {0x40c4, 0x3e808081, 0x3f800000},
    {0x40c8, 0x3ec0c0c1, 0x3eaa781f}, {0x40cc, 0x3f800000, 0x3d810183}, {0x40d4, 0x3f008081, 0x3f800000}, {0x40d8, 0x3f3fbfc0, 0x3f154ff7},
    {0x40dc, 0x3f800000, 0x3e810183}, {0x40ec, 0x3f008081, 0x3f800000}, {0x40f0, 0x3f3fbfc0, 0x3f154ff7}, {0x40f4, 0x3f800000, 0x3e810183},
    {0x40fc, 0x3e808081, 0x3f800000}, {0x4100, 0x3ec0c0c1, 0x3eaa781f}, {0x4104, 0x3f800000, 0x3d810183}, {0x422c, 0x3f3fbfc0, 0x3f800000},
    {0x4230, 0x3f4acacb, 0x3f52836a}, {0x4234, 0x3f800000, 0x3f0f9fb0}, {0x423c, 0x3f3fbfc0, 0x3f800000}, {0x4240, 0x3f4acacb, 0x3f52836a},
    {0x4244, 0x3f800000, 0x3f0f9fb0}, {0x4254, 0x3f3fbfc0, 0x3f800000}, {0x4258, 0x3f4acacb, 0x3f52836a}, {0x425c, 0x3f800000, 0x3f0f9fb0},
    {0x4264, 0x3f3fbfc0, 0x3f800000}, {0x4268, 0x3f4acacb, 0x3f52836a}, {0x426c, 0x3f800000, 0x3f0f9fb0}, {0x42d4, 0x3e800000, 0x3f800000},
    {0x42d8, 0x3f000000, 0x3f280000}, {0x42dc, 0x3f800000, 0x3d800000}, {0x43f4, 0x3e808081, 0x3f800000}, {0x43f8, 0x3ec0c0c1, 0x3eaa781f},
    {0x43fc, 0x3f800000, 0x3d810183}, {0x4404, 0x3f008081, 0x3f800000}, {0x4408, 0x3f3fbfc0, 0x3f154ff7}, {0x440c, 0x3f800000, 0x3e810183},
    {0x4424, 0x3f008081, 0x3f800000}, {0x4428, 0x3f3fbfc0, 0x3f154ff7}, {0x442c, 0x3f800000, 0x3e810183}, {0x4434, 0x3ed62b82, 0x3f800000},
    {0x4438, 0x3f1ff54b, 0x3efedf0f}, {0x443c, 0x3f800000, 0x3e332cc5}, {0x4444, 0x3eab5601, 0x3f800000}, {0x4448, 0x3f002ad6, 0x3ed3c876},
    {0x444c, 0x3f800000, 0x3de55804}, {0x4454, 0x3e808081, 0x3f800000}, {0x4458, 0x3ec0c0c1, 0x3eaa781f}, {0x445c, 0x3f800000, 0x3d810183},
};
static const u32 FIRE_807800[][3] = {
    {0x40d0, 0x3f800000, 0x3f39999a}, {0x40d4, 0x3f800000, 0x3e800000}, {0x41c8, 0x3ef8f8f9, 0x3f6bebec}, {0x41cc, 0x3f09898a, 0x3f0fc6f9},
    {0x41d0, 0x3f6bebec, 0x3e835f6c}, {0x4474, 0x3f800000, 0x3f39999a}, {0x4478, 0x3f800000, 0x3e800000}, {0x456c, 0x3ef8f8f9, 0x3f6bebec},
    {0x4570, 0x3f09898a, 0x3f0fc6f9}, {0x4574, 0x3f6bebec, 0x3e835f6c}, {0x4814, 0x3e4ccccd, 0x3f800000}, {0x4818, 0x3e4ccccd, 0x3f25e354},
    {0x481c, 0x3f800000, 0x3d23d70b}, {0x4910, 0x3d20a0a1, 0x3e48c8c9}, {0x4914, 0x3d20a0a1, 0x3d6604bd}, {0x4918, 0x3e48c8c9, 0x3c008081},
};
static void build_fire_ring(const char *game_dir, buf bhd, const char *bdt) {
    static const struct { const char *name; const u32 (*w)[3]; int n; } FX[] = {
        {"\\f000807011.fxr", FIRE_807011, sizeof FIRE_807011 / sizeof *FIRE_807011},
        {"\\f000807800.fxr", FIRE_807800, sizeof FIRE_807800 / sizeof *FIRE_807800}};
    char path[1300];
    snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "sfx" SEP "sfxbnd_commoneffects.ffxbnd.dcx", g_app);
    remove(path);  // the last build's, in case nothing is changed this time
    buf dcx = archive_read(bhd, bdt, "/sfx/sfxbnd_commoneffects.ffxbnd.dcx");
    buf bnd = dcx.p ? dcx_read(game_dir, dcx) : dcx;
    free(dcx.p);
    int done = 0;
    for (size_t k = 0; bnd.p && k < sizeof FX / sizeof *FX; k++) {
        int i = bnd4_find(bnd, FX[k].name), ok = i >= 0;
        buf f = ok ? bnd4_file(bnd, i) : bnd;
        for (int j = 0; ok && j < FX[k].n; j++) ok = FX[k].w[j][0] + 4 <= f.n && rd32(f.p + FX[k].w[j][0]) == FX[k].w[j][1];
        for (int j = 0; ok && j < FX[k].n; j++) wr32(f.p + FX[k].w[j][0], FX[k].w[j][2]);
        if (ok) done++;
        else logf_("Fire ring: %s not as expected (game update?), left blue", FX[k].name + 1);
    }
    if (!bnd.p) logf_("Fire ring: the game's common effects not found, left blue");
    if (done) {
        buf out = dcx_write(bnd);
        free(bnd.p), bnd.p = NULL;  // the unpacked archive is large (316 MB)
        int written = write_file(path, out.p, out.n);
        logf_("Fire ring: %d of 2 effects recoloured (%zu bytes)%s", done, out.n, written ? "" : ", file NOT written");
        free(out.p);
    }
    free(bnd.p);
}

int core_build(const char *game_dir) {
    char bhd_path[1100], bdt_path[1100], from[1300], to[1300];
    if (!core_check_game(game_dir)) return logf_("This is not the game's folder: %s", game_dir), 0;
    if (!read_settings()) return 0;
    snprintf(bhd_path, sizeof bhd_path, "%s" SEP "data0.bhd", game_dir);
    snprintf(bdt_path, sizeof bdt_path, "%s" SEP "data0.bdt", game_dir);
    logf_("Nightreign Invasions " MOD_VERSION);
    logf_("Reading the game archive...");
    rsa_init();
    aes_init();
    buf enc = read_file(bhd_path);
    if (!enc.p) return logf_("cannot read %s", bhd_path), 0;
    // Nothing to do if the game archive and the mod's own files are the ones we built from last time.
    char stamp_path[1300], stamp[40];
    snprintf(stamp_path, sizeof stamp_path, "%s" SEP "work" SEP "build.stamp", g_app);
    snprintf(from, sizeof from, "%s" SEP "data" SEP "invasion.dll", g_app);
    buf dll = read_file(from);
    // BUILD_FORMAT: bump when the set of files the launcher builds changes.
    snprintf(stamp, sizeof stamp, "%016llx%016llx", (unsigned long long)fnv1a64(enc.p, enc.n),
             (unsigned long long)(fnv1a64(dll.p, dll.n) + BUILD_FORMAT +
                                     ((u64)(map_draw_group() + 1) << 49) + ((u64)one_region() << 53) + ((u64)pair_now() << 57)));
    free(dll.p);
    buf old_stamp = read_file(stamp_path);
    int same = old_stamp.n == 32 && !memcmp(old_stamp.p, stamp, 32);
    free(old_stamp.p);
    snprintf(to, sizeof to, "%s" SEP "work" SEP "build" SEP "regulation.bin", g_app);
    if (same && exists(to)) {
        free(enc.p);
        logf_("Mod files are up to date");
        return write_profile();
    }
    remove(stamp_path);
    {  // the last start's pair files go: its composed map halves and map screen packs (another pair writes others)
        char dir[1300];
        snprintf(dir, sizeof dir, "%s" SEP "work" SEP "build" SEP "map", g_app);
        remove_tree(dir);
        for (int k = 1; k <= 5; k++) {
            snprintf(dir, sizeof dir, "%s" SEP "work" SEP "build" SEP "menu" SEP "71_maptile_m%02d.tpf.dcx", g_app, k);
            remove(dir);
        }
    }
    buf bhd = rsa_decrypt(enc);
    free(enc.p);
    if (memcmp(bhd.p, "BHD5", 4))
        return logf_("The game archive could not be read. The game may have been updated; the mod needs an update too."), 0;

    int ok = 1, langs = 0;
    for (size_t i = 0; ok && i < sizeof LANGS / sizeof *LANGS; i++) {
        int r = build_text(game_dir, bhd, bdt_path, LANGS[i]);
        if (r == 0) ok = 0;
        if (r > 0) langs++;
    }
    if (ok) logf_("Menu texts built for %d languages", langs);
    ok = ok && build_icons(game_dir, bhd, bdt_path, 'h') && build_icons(game_dir, bhd, bdt_path, 'l');
    if (ok && one_region()) remove_two_region_map(), logf_("one-region (test): region %d only, no two-region map", one_region());
    else if (ok) ok = build_two_region_map(game_dir, bhd, bdt_path);
    if (ok && setting(S_PACK_TEST)) {  // measurement: the game's plain map screen pack as M06 (the DLL loads M06)
        buf p = archive_read(bhd, bdt_path, "/menu/71_maptile_m00.tpf.dcx");
        char path[1300];
        snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "71_maptile_m06.tpf.dcx", g_app);
        logf_("pack test: the game's plain map screen pack %s as 71_maptile_m06", p.p && write_file(path, p.p, p.n) ? "written" : "NOT written");
        free(p.p);
    }
    if (ok && build_boss_deaths(game_dir, bhd, bdt_path) < BOSS_DEATHS_MIN)
        ok = 0, logf_("The game's event scripts look different than expected (too few boss deaths). The game may have been updated; the mod needs an update too.");
    if (ok && !build_camp_scripts(game_dir, bhd, bdt_path))
        ok = 0, logf_("The game's camp scripts look different than expected. The game may have been updated; the mod needs an update too.");
    if (ok) build_fire_ring(game_dir, bhd, bdt_path);
    free(bhd.p);
    if (ok && two_pair()->halves) ok = build_pair_half(game_dir);
    if (!ok) return 0;
    logf_("Menu icons added");
    if (!build_regulation(game_dir)) return 0;
    logf_("Target list entry added");

    snprintf(from, sizeof from, "%s" SEP "data" SEP "invasion.dll", g_app);
    snprintf(to, sizeof to, "%s" SEP "work" SEP "native" SEP "invasion.dll", g_app);
    if (!copy_file(from, to)) return logf_("missing %s", from), 0;
    if (!write_profile()) return 0;
    write_file(stamp_path, (const u8 *)stamp, 32);
#ifndef _WIN32
    snprintf(to, sizeof to, "%s/work/wineprefix", g_app);  // only needed for unpacking; it is large
    remove_tree(to);
#endif
    logf_("Mod files are ready");
    return 1;
}

int core_launch(void) {
    char profile[1300], me3[1300];
    snprintf(profile, sizeof profile, "%s" SEP "work" SEP "invasion.me3", g_app);
    if (!exists(profile)) return logf_("Build the mod files first"), 0;
#ifdef _WIN32
    char cmd[4000];
    snprintf(me3, sizeof me3, "%s\\me3\\bin\\me3.exe", g_app);
    if (!exists(me3)) return logf_("missing %s", me3), 0;
    snprintf(cmd, sizeof cmd, "\"%s\" launch -g nightreign -p \"%s\"", me3, profile);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return logf_("could not start me3 (error %lu)", GetLastError()), 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    char win64[1300];
    snprintf(me3, sizeof me3, "%s/me3/bin/me3", g_app);
    snprintf(win64, sizeof win64, "%s/me3/bin/win64", g_app);
    if (!exists(me3)) return logf_("missing %s", me3), 0;
    char *argv[] = {me3, "--windows-binaries-dir", win64, "launch", "-g", "nightreign", "-p", profile, NULL};
    pid_t pid;
    if (posix_spawn(&pid, me3, NULL, NULL, argv, environ)) return logf_("could not start me3"), 0;
#endif
    logf_("Game is starting through me3 (Steam must be running)");
    return 1;
}

int core_uninstall(const char *save_dir) {
    char path[1300];
    static const char *parts[] = {"build", "native", "wineprefix"};
    snprintf(path, sizeof path, "%s" SEP "work" SEP "build.stamp", g_app);
    remove(path);
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        snprintf(path, sizeof path, "%s" SEP "work" SEP "%s", g_app, parts[i]);
        remove_tree(path);
    }
    snprintf(path, sizeof path, "%s" SEP "work" SEP "invasion.me3", g_app);
    remove(path);
    if (save_dir && *save_dir) {
        snprintf(path, sizeof path, "%s" SEP MOD_SAVE_NAME, save_dir);
        remove(path);
        snprintf(path, sizeof path, "%s" SEP MOD_SAVE_NAME ".bak", save_dir);
        remove(path);
    }
    logf_("Mod files removed. Your normal save and the backups were not touched.");
    return 1;
}

static void remove_tree(const char *dir) {
    char path[1400];
#ifdef _WIN32
    char pattern[1400];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(path);
        else DeleteFileA(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (!lstat(path, &st) && S_ISDIR(st.st_mode)) remove_tree(path);
        else unlink(path);
    }
    closedir(d);
    rmdir(dir);
#endif
}

// --- self-check ----------------------------------------------------------------------------------------------------

int core_selftest(void) {
    int ok = 1;
    // AES-128, FIPS-197 appendix C.1.
    aes_init();
    u8 key[16], block[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    u8 rk[240];
    for (int i = 0; i < 16; i++) key[i] = (u8)i;
    aes_expand(key, 16, rk);
    aes_decrypt_block(rk, 10, block);
    for (int i = 0; i < 16; i++) if (block[i] != (u8)(i * 0x11)) ok = 0;
    // AES-256, FIPS-197 appendix C.3, both directions.
    static const u8 c3[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    u8 key256[32];
    for (int i = 0; i < 32; i++) key256[i] = (u8)i;
    aes_expand(key256, 32, rk);
    aes_encrypt_block(rk, 14, block);
    if (memcmp(block, c3, 16)) ok = 0;
    aes_decrypt_block(rk, 14, block);
    for (int i = 0; i < 16; i++) if (block[i] != (u8)(i * 0x11)) ok = 0;
    if (!ok) logf_("selftest: AES failed");

    // camp script patch: event 0 starting 90015310 for m30_30 with count 1 -> 2, once
    {
        u8 e[0xE4] = {0};
        memcpy(e, "EVD", 3);
        e[0x10] = 1, e[0x18] = 0x80, e[0x28] = 0xB0, e[0x78] = 0xD0;
        e[0x88] = 1;                                            // event 0: 1 instruction at 0
        wr32(e + 0xB0, 2000), wr32(e + 0xB4, 6), e[0xB8] = 20;  // 2000[06], 20 bytes of arguments at 0
        wr32(e + 0xD4, CAMP_SCRIPT_EVENT), wr32(e + 0xD8, 30300050), wr32(e + 0xDC, 30300051), wr32(e + 0xE0, 1);
        buf b = {e, sizeof e};
        if (!emevd_camp_count(b, 3030) || rd32(e + 0xE0) != 2 || emevd_camp_count(b, 3030) || emevd_camp_count(b, 3810))
            ok = 0, logf_("selftest: camp script patch failed");
    }
    // zstd decoder: a frame with one raw block holding "abc".
    static const u8 frame[] = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x03, 0x19, 0x00, 0x00, 'a', 'b', 'c'};
    u8 *back = xalloc(3 + (1 << 17));
    if (nr_zstd_decompress(frame, sizeof frame, back, 3 + (1 << 17)) != 3 || memcmp(back, "abc", 3)) {
        ok = 0;
        logf_("selftest: zstd failed");
    }
    free(back);

    // RSA: (m^e mod n) for m = 2 must equal 2^e reduced the slow way; check m = 1 and Montgomery identity.
    rsa_init();
    u8 one[256] = {0}, out[255];
    one[255] = 1;
    rsa_block(one, out);
    for (int i = 0; i < 254; i++) if (out[i]) ok = 0;
    if (out[254] != 1) ok = 0;
    if (!ok) logf_("selftest: RSA failed");

    // FMG: build, add, write, parse.
    fmg f = {xalloc(8 * sizeof(fmg_entry)), 0, 8};
    u8 a[16], b[16];
    fmg_set(&f, 10, a, utf8_to_utf16("ten", a));
    fmg_set(&f, 5, b, utf8_to_utf16("pi\xc4\x99\xc4\x87", b));
    fmg_set(&f, 11, NULL, 0);
    buf w = fmg_write(&f);
    fmg g = {0};
    if (!fmg_parse(w, &g) || g.n != 3 || g.e[0].id != 5 || g.e[0].bytes != 8 || g.e[1].id != 10 ||
        memcmp(g.e[1].text, a, 6) || g.e[2].text) { ok = 0; logf_("selftest: FMG failed"); }
    {  // more additions than the parse's spare room (the bug of 2026-11-15): the table must grow, all lines kept in order
        static u8 txt[2] = {'x', 0};
        size_t want = g.n + OUR_TEXT_COUNT + 16;
        for (u32 id = 1000; g.n < want; id += 3) fmg_set(&g, id, txt, 2);
        buf w2 = fmg_write(&g);
        fmg h = {0};
        int good = fmg_parse(w2, &h) && h.n == want;
        for (size_t i = 1; good && i < h.n; i++) good = h.e[i - 1].id < h.e[i].id;
        if (!good) { ok = 0; logf_("selftest: FMG growth failed"); }
        free(w2.p), free(h.e);
    }

    // DCX writer: header fields and adler of a known string ("Wikipedia" -> 0x11E60398).
    buf raw = {(u8 *)"Wikipedia", 9}, d = dcx_write(raw);
    if (be32(d.p + 0x1C) != 9 || be32(d.p + 0x20) != d.n - 0x4C || be32(d.p + d.n - 4) != 0x11E60398) {
        ok = 0;
        logf_("selftest: DCX failed");
    }
    free(f.e); free(g.e); free(w.p); free(d.p);
    // Param row insertion: ids 1, 5, 9 (8-byte rows, row data = id), insert 7 as a copy of 5.
    {
        u8 pp[0x40 + 3 * 24 + 3 * 8 + 4] = {0};
        buf src = {pp, sizeof pp};
        wr32(pp, 0x40 + 3 * 24 + 3 * 8);
        wr16(pp + 0x0A, 3);
        pp[0x2D] = 4;
        for (u32 i = 0; i < 3; i++) {
            wr32(pp + 0x40 + i * 24, 1 + 4 * i);
            wr64(pp + 0x40 + i * 24 + 8, 0x40 + 3 * 24 + i * 8);
            wr32(pp + 0x40 + 3 * 24 + i * 8, 1 + 4 * i);
        }
        u8 *row = NULL;
        buf q = param_insert_sorted(src, 5, 7, 8, &row);
        int good = q.p && rd16(q.p + 0x0A) == 4 && row && rd32(row) == 5;
        for (u32 i = 0; good && i < 4; i++) {
            static const u32 ids[] = {1, 5, 7, 9}, data[] = {1, 5, 5, 9};
            good = rd32(q.p + 0x40 + i * 24) == ids[i] && rd32(q.p + rd64(q.p + 0x40 + i * 24 + 8)) == data[i];
        }
        if (!good) { ok = 0; logf_("selftest: param row insertion failed"); }
        free(q.p);
    }
    {  // boss deaths from an event script: 9005810's first and 90015006's third parameter, others ignored, no repeats
        static u8 ev[0x200];
        memset(ev, 0, sizeof ev);
        memcpy(ev, "EVD", 3);
        wr64(ev + 0x10, 1), wr64(ev + 0x18, 0x80), wr64(ev + 0x28, 0x100), wr64(ev + 0x78, 0x180);
        wr64(ev + 0x80, 1), wr64(ev + 0x88, 4), wr64(ev + 0x90, 0);
        const u32 ins[4][4] = {{2000, 6, 12, 0}, {2000, 0, 20, 16}, {2000, 6, 12, 40}, {2000, 0, 24, 52}};  // bank, id, length, offset
        for (int i = 0; i < 4; i++) wr32(ev + 0x100 + i * 0x20, ins[i][0]), wr32(ev + 0x104 + i * 0x20, ins[i][1]), wr64(ev + 0x108 + i * 0x20, ins[i][2]), wr64(ev + 0x110 + i * 0x20, ins[i][3]);
        const u32 args[] = {0, 9005810, 46500800, 0, 0, 90015006, 30301510, 30300510, 30300810, 0, 0, 9005810, 46500800,
                            0, 90045002, 46600800, 2, 46605820, 46600820};  // a duel: its shared flag only, not the variant's
        for (unsigned i = 0; i < sizeof args / sizeof *args; i++) wr32(ev + 0x180 + i * 4, args[i]);
        u32 got[4];
        int k = emevd_boss_deaths((buf){ev, sizeof ev}, got, 0, 4, 0);
        if (k != 3 || got[0] != 46500800 || got[1] != 30300810 || got[2] != 46600800) { ok = 0; logf_("selftest: boss deaths from an event script failed"); }
    }
    {  // test settings: values, comments, CRLF, case, retired keys (no-rain, challenge-bosses), unknown keys and bad values, a missing file
        static const char t[] = "# c\r\nteam-test = true\r\n  NO-RAIN=FALSE # x\r\nslow-time\t= True\r\ntwo-regions = true\r\n"
                                "nope = true\r\nloot-test = yes\r\n\r\ndebug-log = false\r\ntest-level = 15\r\nmap-group = 3\r\nult-pvp = 250\r\none-region = 1000\r\n"
                                "pair = -1\r\nboss-flow = 3\r\nchallenge-bosses = 2";
        short v[SETTINGS_COUNT];
        char bad[96];
        static const short want[SETTINGS_COUNT] = {-1, 0, -1, 15, -1, -1, 3, -1, -1, -1, 250, -1, -1, -1, -1, -1};
        int good = settings_parse(t, sizeof t - 1, v, bad, sizeof bad) == 4 && !memcmp(v, want, sizeof v) &&
                   !strcmp(bad, "nope = true, loot-test = yes, one-region = 1000, boss-flow = 3");
        char bad2[8];
        good = good && settings_parse("pair = 3", 8, v, bad2, sizeof bad2) == 0 && v[S_MAP] == 4;     // the retired pair: map = pair + 1
        good = good && settings_parse("map = 2\npair = 3", 17, v, bad2, sizeof bad2) == 0 && v[S_MAP] == 2;  // map wins
        good = good && !load_settings("/nonexistent/test-settings.toml", v);
        for (int i = 0; i < SETTINGS_COUNT; i++) good = good && v[i] == setting_default(i);
        if (!good) { ok = 0; logf_("selftest: test settings parser failed"); }
    }
    {  // window texts: every key in every language, none empty; T_BACKUP_DONE has exactly one %s
        int good = 1;
        for (int l = 0; l < LANG_COUNT; l++)
            for (int t = 0; t < T_COUNT; t++) {
                const char *x = UI_TEXT[l][t];
                if (!x || !*x) good = 0;
                if (t == T_BACKUP_DONE && (!x || !strstr(x, "%s") || strstr(strstr(x, "%s") + 2, "%"))) good = 0;
            }
        good = good && ui_lang_from_code("pl_PL.UTF-8") == L_PL && ui_lang_from_code("xx") == L_EN && ui_lang_from_code(NULL) == L_EN;
        if (!good) { ok = 0; logf_("selftest: window texts incomplete"); }
    }
    {  // backup: into a scratch app folder; copies only, a second backup goes to a new folder, nothing
       // already there is written over, the save files are unchanged
        char keep[sizeof g_app], tmp[900], save[1000], path[1500], w1[256], w2[256];
        snprintf(keep, sizeof keep, "%s", g_app);
        snprintf(tmp, sizeof tmp, "%.800s" SEP "selftest-backup", keep);
        remove_tree(tmp);
        snprintf(g_app, sizeof g_app, "%s", tmp);
        snprintf(save, sizeof save, "%.900s" SEP "save" SEP "76500000000000000", tmp);
        snprintf(path, sizeof path, "%s" SEP "NR0000.sl2", save);
        int good = write_file(path, (const u8 *)"vanilla", 7);
        snprintf(path, sizeof path, "%s" SEP MOD_SAVE_NAME, save);
        good = good && write_file(path, (const u8 *)"modded", 6);
        good = good && core_backup_save(save, w1, sizeof w1) == 1 && core_backup_save(save, w2, sizeof w2) == 1 && strcmp(w1, w2);
        snprintf(path, sizeof path, "%s" SEP "%s" SEP "76500000000000000" SEP "NR0000.sl2", tmp, w1);
        buf a = read_file(path);
        snprintf(path, sizeof path, "%s" SEP "%s" SEP "76500000000000000" SEP MOD_SAVE_NAME, tmp, w2);
        buf b = read_file(path);
        good = good && a.n == 7 && !memcmp(a.p, "vanilla", 7) && b.n == 6 && !memcmp(b.p, "modded", 6);
        free(a.p), free(b.p);
        snprintf(path, sizeof path, "%s" SEP "NR0000.sl2", save);
        buf c = read_file(path);
        good = good && c.n == 7 && !memcmp(c.p, "vanilla", 7);
        free(c.p);
        char empty[1000];
        snprintf(empty, sizeof empty, "%.900s" SEP "nosave", tmp);
        good = good && core_backup_save(empty, w1, sizeof w1) == 0;
        snprintf(g_app, sizeof g_app, "%s", keep);
        remove_tree(tmp);
        if (!good) { ok = 0; logf_("selftest: backup failed"); }
    }
    if (ok) logf_("selftest ok");
    return ok;
}

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
#define BUILD_FORMAT 10

static const char *LANGS[] = {"araae", "deude", "engus", "frafr", "itait", "jpnjp", "korkr", "polpl",
                              "porbr", "rusru", "spaar", "spaes", "thath", "zhocn", "zhotw"};
// Our own menu lines (UTF-8): a fourth "Number of Players" option and the "Invasion" entry of the
// expedition menu's Target list.
// Polish for the Polish game, English for every other language. Keep in sync with mod/build_msg.py.
// (Adjacent string literals keep \x escapes from swallowing the letters that follow them.)
static const struct { u32 id; const char *english, *polish; } OUR_TEXT[] = {
    {130460, "4 players", "4 graczy"},
    {131160, "Invasion", "Najazd"},
    {131161, "Enter another squad's expedition as an invader.\nDefeat its host to win.",
             "Wejd\xc5\xba do wyprawy innej dru\xc5\xbcyny jako naje\xc5\xba" "d\xc5\xba" "ca.\n"
             "Pokonaj hosta, aby wygra\xc4\x87."},
    {131162, "Searching for an expedition to invade...", "Szukanie wyprawy do najechania..."},
    {131163, "Invasion search cancelled", "Anulowano szukanie najazdu"},
    {131164, "Cancel invasion", "Anuluj najazd"},
    {131166, "Begin invasion", "Rozpocznij najazd"},
};
#define OUR_TEXT_COUNT (sizeof OUR_TEXT / sizeof *OUR_TEXT)

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

static void mont_mul(u32 *r, const u32 *a, const u32 *b) {
    const u32 *n = DATA0_N;
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
    u32 x = DATA0_N[0];
    for (int i = 0; i < 5; i++) x *= 2 - DATA0_N[0] * x;  // inverse of n mod 2^32 (Newton)
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
                if (r[j] != DATA0_N[j]) { ge = r[j] > DATA0_N[j]; break; }
        }
        if (ge) {
            u64 borrow = 0;
            for (int j = 0; j < LIMBS; j++) {
                u64 d = (u64)r[j] - DATA0_N[j] - borrow;
                r[j] = (u32)d;
                borrow = (d >> 32) & 1;
            }
        }
    }
    memcpy(g_r2, r, sizeof r);
}

// in: 256 bytes big-endian. out: 255 bytes big-endian (the value is below 2^2040 for valid data).
static void rsa_block(const u8 *in, u8 *out) {
    u32 c[LIMBS], cm[LIMBS], acc[LIMBS], one[LIMBS] = {1};
    for (int i = 0; i < LIMBS; i++) c[i] = be32(in + (LIMBS - 1 - i) * 4);
    mont_mul(cm, c, g_r2);
    mont_mul(acc, one, g_r2);
    for (int bit = 31; bit >= 0; bit--) {
        mont_mul(acc, acc, acc);
        if (DATA0_E >> bit & 1) mont_mul(acc, acc, cm);
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

// Insert or replace, keeping the table sorted by id. There is always room for the few lines we add.
static void fmg_set(fmg *f, u32 id, const u8 *text, size_t bytes) {
    size_t i = 0;
    while (i < f->n && f->e[i].id < id) i++;
    if (i == f->n || f->e[i].id != id) {
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

// Our SpEffect row (team test, later the invader's team), the scarabs' team and the optional no-rain
// change. Keep in sync with mod/build_regulation.py, which explains the values and how they were
// checked.
#define SPEFFECT_PARAM "\\SpEffectParam.param"
#define SPEFFECT_TEMPLATE 10101
#define SPEFFECT_TEAM_ROW 90071610
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

// No rain (test sessions only): PlayAreaCreateCommonParam row 0's rain SpEffects 99810 (+0x28) and
// 99811 (+0x2c) become -1, in place. 0 if unexpected.
static int no_rain(buf p) {
    const u8 *d = p.p + 0x40;
    if (rd16(p.p + 0x0A) < 1 || rd32(d) != 0) return 0;
    u8 *row = p.p + rd64(d + 8);
    if (rd32(row + 0x28) != 99810 || rd32(row + 0x2C) != 99811) return 0;
    wr32(row + 0x28, 0xFFFFFFFF);
    wr32(row + 0x2C, 0xFFFFFFFF);
    return 1;
}

// Slow clock (test sessions only): an empty file "slow-time" in work/native/ makes the profile set
// the game's debug property Game.IngameTime.TimeScale to 7.29, a tenth of the expedition clock's
// rate (GameSystemCommonParam +0x1bc = 72.9; the game reads the property in 0x140f884b0 and lets it
// replace that rate when the clock is set up). The circles run on a second clock, the play-area
// manager's, with its own property Game.PlayAreaTime.TimeScale (read in 0x14068b130 when above 0;
// otherwise its rate comes from param table 0x92 row 0 +0x1b4): set to 7.29 as well. The host's
// clocks are the ones clients follow.
// Test settings (test phase only): test-settings.toml next to the launcher, read at every build
// (each "Play with the mod"). A missing file means all false. no-rain and slow-time act here; the
// resolved values of all of them go to work/native/test-settings.toml, which the DLL reads.
#include "../mod/dll/settings.h"
static signed char g_set[SETTINGS_COUNT];
static int setting(int i) { return g_set[i] > 0; }
static int load_settings(const char *path, signed char v[SETTINGS_COUNT]) {  // 0: no file
    buf t = read_file(path);
    char bad[300];
    if (!t.p) {
        for (int i = 0; i < SETTINGS_COUNT; i++) v[i] = setting_default(i);
        return 0;
    }
    if (settings_parse((const char *)t.p, t.n, v, bad, sizeof bad)) logf_("test-settings.toml: ignored: %s", bad);
    for (int i = 0; i < SETTINGS_COUNT; i++) if (v[i] < 0) v[i] = setting_default(i);
    free(t.p);
    return 1;
}
static int read_settings(void) {
    char path[1300], on[200] = "";
    snprintf(path, sizeof path, "%s" SEP "test-settings.toml", g_app);
    int found = load_settings(path, g_set);
    for (int i = 0; i < SETTINGS_COUNT; i++)
        if (g_set[i]) snprintf(on + strlen(on), sizeof on - strlen(on), " %s", SETTING_NAMES[i]);
    logf_(found ? "Test settings:%s" : "Test settings:%s (no test-settings.toml next to the launcher)", on[0] ? on : " none");
    snprintf(path, sizeof path, "%s" SEP "work" SEP "native" SEP "test-settings.toml", g_app);
    make_parents(path);
    FILE *f = fopen(path, "w");
    if (!f) return logf_("cannot write %s", path), 0;
    fprintf(f, "# Written by the launcher from test-settings.toml at every start; changes here are overwritten.\n");
    for (int i = 0; i < SETTINGS_COUNT; i++) fprintf(f, "%s = %s\n", SETTING_NAMES[i], g_set[i] ? "true" : "false");
    fclose(f);
    return 1;
}
static int slow_time_wanted(void) { return setting(S_SLOW_TIME); }
static int no_rain_wanted(void) { return setting(S_NO_RAIN); }

// game's regulation.bin -> work/build/regulation.bin with our Target list row.
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
    buf patched = {0}, body = {0}, param = {0}, speffect = {0}, step = {0};
    int ok = 0, index, sp_index, npc_index, area_index;
    u8 *team_row = NULL;
    if (nr_zstd_decompress(dcx + 0x4C, csize, bundle.p, (size_t)usize + (1 << 17)) != usize) {
        logf_("regulation.bin could not be unpacked");
    } else if ((index = bnd4_find(bundle, TARGET_PARAM)) < 0 || !(param = param_add_row(bnd4_file(bundle, index))).p) {
        logf_("The game's Target list looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((sp_index = bnd4_find(bundle, SPEFFECT_PARAM)) < 0 ||
               !(speffect = param_insert_sorted(bnd4_file(bundle, sp_index), SPEFFECT_TEMPLATE, SPEFFECT_TEAM_ROW,
                                                SPEFFECT_SIZE, &team_row)).p ||
               rd32(team_row + 0x08) != 0xBF800000 || team_row[0x16D] != 0xFF || rd32(team_row + 0x18C) != 0xFFFFFFFF) {
        logf_("The game's SpEffectParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if ((npc_index = bnd4_find(bundle, NPC_PARAM)) < 0 || !scarab_rows(bnd4_file(bundle, npc_index))) {
        logf_("The game's NpcParam looks different than expected. The game may have been updated; the mod needs an update too.");
    } else if (no_rain_wanted() && ((area_index = bnd4_find(bundle, PLAY_AREA_PARAM)) < 0 || !no_rain(bnd4_file(bundle, area_index)))) {
        logf_("The game's PlayAreaCreateCommonParam looks different than expected; no-rain cannot be applied.");
    } else {
        // effectEndurance stays -1 (the template's): until removed; death removes it
        team_row[0x16D] = 16;                // changeTeamType 16
        wr32(team_row + 0x18C, 300050);      // vfxId1: red phantom look
        if (no_rain_wanted()) logf_("Test option: no rain damage");
        // Written as a zlib DCX, like our other files (the launcher has no zstd compressor). It has
        // to be compressed: the game crashes at start-up on a 55 MB regulation.bin of stored zstd
        // blocks. ponytail: unlike the game's own file this is not zstd; if the game ever stops
        // taking zlib here, a zstd compressor is the upgrade path.
        step = bnd4_replace(bundle, index, param);
        patched = bnd4_replace(step, sp_index, speffect);
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
    free(enc.p); free(bundle.p); free(param.p); free(speffect.p); free(step.p); free(patched.p); free(body.p);
    return ok;
}

// --- menu icons ------------------------------------------------------------------------------------------------
//
// Menu icons are sub-rectangles of big atlas textures; a ".layout" XML file per atlas names them
// ("MENU_MenuIcon_<id>.png"). Our two icons go into an unused corner of the DLC atlas and get one
// layout line each, as in mod/build_icon.py, which also produces their BC7 blocks (data/icon_<id>.bc7).

#define ICON_ATLAS "SB_MenuIcon_DLC01_0"
#define DDS_HEADER 148
static const struct { int id, side; } ICONS[] = {{41813, 248}, {41814, 460}};
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
    char *layout = NULL, *text = NULL;
    rect *used = NULL;
    int ok = 0, index = bundle.p ? bnd4_find(bundle, ICON_ATLAS ".layout") : -1;
    if (index < 0 || !tpf.p) { logf_("menu icon files not found in the game archive"); goto done; }

    buf lay = bnd4_file(bundle, index);
    layout = xalloc(lay.n + 1);
    memcpy(layout, lay.p, lay.n);
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
        if (rd32(e + 0x0C) < tpf.n && name_ends_with(tpf.p + rd32(e + 0x0C), ICON_ATLAS) &&
            (u64)rd32(e) + rd32(e + 4) <= tpf.n && rd32(e + 4) > DDS_HEADER) dds = tpf.p + rd32(e);
    }
    if (!atlas || !close || width < 0 || height < 0 || !dds || memcmp(dds + 84, "DX10", 4) || rd32(dds + 128) != 98 ||
        (int)rd32(dds + 12) != height || (int)rd32(dds + 16) != width) {
        logf_("The game's menu icon atlas looks different than expected. The game may have been updated; the mod needs an update too.");
        goto done;
    }

    char lines[ICON_COUNT * 256];
    size_t len = 0;
    for (size_t i = 0; i < ICON_COUNT; i++) {
        int side = ICONS[i].side, x, y;
        char name[64];
        snprintf(name, sizeof name, "MENU_MenuIcon_%d.", ICONS[i].id);
        snprintf(path, sizeof path, "%s" SEP "data" SEP "icon_%d.bc7", g_app, ICONS[i].id);
        buf blocks = read_file(path);
        size_t per_row = (size_t)side / 4 * 16;
        if (strstr(layout, name) || blocks.n != per_row * (side / 4) || !free_spot(used, n, side, width, height, &x, &y)) {
            free(blocks.p);
            logf_("menu icon %d cannot be added", ICONS[i].id);
            goto done;
        }
        rect r = {x, side, y, side};
        used[n++] = r;
        for (int by = 0; by < side / 4; by++)
            memcpy(dds + DDS_HEADER + ((size_t)(y / 4 + by) * (width / 4) + x / 4) * 16, blocks.p + by * per_row, per_row);
        free(blocks.p);
        len += (size_t)snprintf(lines + len, sizeof lines - len,
                                "\t<SubTexture name=\"MENU_MenuIcon_%d.png\" x=\"%d\" width=\"%d\" y=\"%d\" height=\"%d\""
                                " originalWidth=\"%d\" originalHeight=\"%d\" half=\"0\"/>\r\n",
                                ICONS[i].id, x, side, y, side, side, side);
    }
    size_t before = (size_t)(close - layout);
    text = xalloc(lay.n + len);
    memcpy(text, layout, before);
    memcpy(text + before, lines, len);
    memcpy(text + before + len, close, lay.n - before);
    buf new_layout = {(u8 *)text, lay.n + len};
    buf patched = bnd4_replace(bundle, index, new_layout), out = dcx_write(patched);
    snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "21_common_dlc01_%c.sblytbnd.dcx", g_app, quality);
    ok = write_file(path, out.p, out.n);
    free(patched.p); free(out.p);
    out = dcx_write(tpf);
    snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "menu" SEP "21_common_dlc01_%c.tpf.dcx", g_app, quality);
    ok = ok && write_file(path, out.p, out.n);
    free(out.p);
done:
    free(layout); free(text); free(used); free(bundle.p); free(tpf.p);
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

int core_backup_save(const char *save_dir) {
    static const char *names[] = {"NR0000.sl2", "NR0000.sl2.bak", MOD_SAVE_NAME, MOD_SAVE_NAME ".bak"};
    char stamp[32], from[1300], to[1300];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    int copied = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        snprintf(from, sizeof from, "%s" SEP "%s", save_dir, names[i]);
        snprintf(to, sizeof to, "%s" SEP "work" SEP "backups" SEP "%s" SEP "%s", g_app, stamp, names[i]);
        if (exists(from)) {
            if (!copy_file(from, to)) return logf_("backup failed for %s", names[i]), 0;
            copied++;
        }
    }
    if (!copied) return logf_("no save file found in %s", save_dir), 0;
    logf_("Save backed up (%d files) to work" SEP "backups" SEP "%s", copied, stamp);
    return 1;
}

static int build_text(const char *game_dir, buf bhd, const char *bdt, const char *lang) {
    char path[1300];
    snprintf(path, sizeof path, "/msg/%s/menu_dlc01.msgbnd.dcx", lang);
    buf dcx = archive_read(bhd, bdt, path);
    if (!dcx.p) return -1;  // language not installed
    buf bundle = dcx_read(game_dir, dcx);
    free(dcx.p);
    if (!bundle.p) return 0;
    int target = bnd4_find(bundle, TEXT_TARGET_FMG);
    fmg dst = {0};
    if (target < 0 || !fmg_parse(bnd4_file(bundle, target), &dst))
        return logf_("%s: unexpected text bundle layout", lang), 0;

    u8 wide[OUR_TEXT_COUNT][512];
    for (size_t i = 0; i < OUR_TEXT_COUNT; i++) {
        const char *line = !strcmp(lang, "polpl") ? OUR_TEXT[i].polish : OUR_TEXT[i].english;
        fmg_set(&dst, OUR_TEXT[i].id, wide[i], utf8_to_utf16(line, wide[i]));
    }

    buf table = fmg_write(&dst), patched = bnd4_replace(bundle, target, table), out = dcx_write(patched);
    snprintf(path, sizeof path, "%s" SEP "work" SEP "build" SEP "msg" SEP "%s" SEP "menu_dlc01.msgbnd.dcx", g_app, lang);
    int ok = write_file(path, out.p, out.n);
    free(dst.e); free(table.p); free(patched.p); free(out.p); free(bundle.p);
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
    if (slow_time_wanted()) {
        fprintf(f, "\n[debug_properties]\n\"Game.IngameTime.TimeScale\" = 7.29\n\"Game.PlayAreaTime.TimeScale\" = 7.29\n");
        logf_("Test option: slow day and circle clocks");
    }
    fclose(f);
    return 1;
}

int core_build(const char *game_dir) {
    char bhd_path[1100], bdt_path[1100], from[1300], to[1300];
    if (!core_check_game(game_dir)) return logf_("This is not the game's folder: %s", game_dir), 0;
    if (!read_settings()) return 0;
    snprintf(bhd_path, sizeof bhd_path, "%s" SEP "data0.bhd", game_dir);
    snprintf(bdt_path, sizeof bdt_path, "%s" SEP "data0.bdt", game_dir);
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
             (unsigned long long)(fnv1a64(dll.p, dll.n) + BUILD_FORMAT + ((u64)no_rain_wanted() << 40) +
                                     ((u64)slow_time_wanted() << 41)));
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
    free(bhd.p);
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
    {  // test settings: values, comments, CRLF, case, unknown keys and bad values, a missing file
        static const char t[] = "# c\r\nhost-test = true\r\n  JOIN-TEST=FALSE # x\r\nslow-time\t= True\r\n"
                                "nope = true\r\nteam-test = yes\r\n\r\nno-rain = false";
        signed char v[SETTINGS_COUNT];
        char bad[64];
        static const signed char want[SETTINGS_COUNT] = {1, 0, -1, -1, -1, 0, 1, -1, -1, -1, -1, -1, -1, -1};
        int good = settings_parse(t, sizeof t - 1, v, bad, sizeof bad) == 2 && !memcmp(v, want, sizeof v) &&
                   !strcmp(bad, "nope = true, team-test = yes");
        good = good && !load_settings("/nonexistent/test-settings.toml", v);
        for (int i = 0; i < SETTINGS_COUNT; i++) good = good && v[i] == setting_default(i);
        if (!good) { ok = 0; logf_("selftest: test settings parser failed"); }
    }
    if (ok) logf_("selftest ok");
    return ok;
}

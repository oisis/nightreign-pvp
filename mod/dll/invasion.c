// Nightreign invasion mod, client DLL.
//
// What it does today:
//   - logs the session manager's state (lobby state, player count, join data) whenever it changes,
//     and how many invaders the current squad would allow;
//   - adds an "Invasion" entry to the expedition menu's Target list (row 101 of NightBossMenuParam,
//     which must exist in regulation.bin: mod/build_regulation.py, or the launcher); the "Number of
//     Players" option shows only "1 player" and "Team";
//   - when that Invasion entry is the chosen target: cancels the expedition start and enters the
//     "searching" state, with a small repeating notice, brings the game's network up (Steam only,
//     behind the network guard) and lists Steam lobbies carrying the mod's marker. Nothing is joined;
//   - with a file "host-test" next to the DLL: an ordinary expedition opens a host session on that
//     network for its whole length and marks the session's Steam lobby as a mod session.
//
// ponytail: addresses are hard-coded for nightreign.exe 1.3.3 (PE timestamp below) and the DLL does
// nothing on any other build. Replace with signature scans when a second game version has to work.
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The mod's version: the release workflow passes the final VERSION (-DMOD_VERSION="\"0.1.37\""); local builds say "dev".
// Only the same version pairs up (lobby key nrpvp_ver); "dev" is never a release, so it is never "newer" than one.
#ifndef MOD_VERSION
#define MOD_VERSION "dev"
#endif
static int g_cap_remote = 2;  // remote player slots in effect (4 once the capacity patch is in, capacity.h)
#define GAME_TIMESTAMP 0x69e0dc2a  // nightreign.exe 1.3.3, Steam build 22818764

// Session manager (same first fields as Elden Ring's CSSessionManager).
#define RVA_SESSION_MAN_PTR 0x3c21ec8  // CSSessionManager*
#define RVA_SESSION_IMPL    0x257db50  // impl* get(CSSessionManager + 0x60)
#define SM_HANDLE        0x08   // u32 session handle
#define SM_LOBBY_STATE   0x0c   // u32: 0 none, 1 creating, 3 host, 4 joining, 6 client, 7 leaving
#define SM_IMPL          0x60
#define SM_PLAYERS_BEGIN 0x78   // players vector: begin, end (+0x80); entries are 0xF0 bytes
#define SM_PLAYER_SIZE   0xF0
#define SM_PLAYER_LIMIT  0x170  // u32, 3 in vanilla
#define SESSION_PLAYERS  SESSION_PLAYERS_MAX  // one per remote slot plus the local player: 5 with the capacity patch, else 3
#define IMPL_VT_EXPORT_JOIN_DATA 0x40  // bool (impl*, u32 handle, void *buf, u32 *size)

// Expedition menu, Target list (CS::MatchingBossSelectDialog::_CreateBossList and helpers).
// _CreateBossList (0x9e9330) fills the dialog's item list and ends with a tail jump that hands the
// list to the grid: "jmp show(grid, list, 0, 0)". We hook that jump, so the grid, the cursor and the
// description panel only ever see the list with our entry already in place.
#define RVA_SHOW_BOSS_LIST_JMP    0x9e9b59  // e9 12 ba ff ff
#define RVA_SHOW_BOSS_LIST        0x9e5570  // void (GridControl*, list*, 0, 0)
#define RVA_BOSS_ITEM_CTOR        0x8dac40  // item* (buf*, int row, u8 enhanced, u8)
#define RVA_BOSS_ITEM_PUSH        0x9e77c0  // void (dialog + 0x19a0, item*)
#define RVA_BOSS_ITEM_DTOR        0x8dad70  // void (buf*)
#define INVASION_ROW              101       // our row in NightBossMenuParam

static char log_path[MAX_PATH];
static char *g_base;

// The log. With debug-log off (the default) only the lines a support case needs are written: start
// and settings, the network guard, session opened / closed, invasion joined / ended with its reason,
// win / loss and the reward, the end of an expedition, and errors and watchdogs; a line counts as
// one of those when it contains one of KEY_LINES. With debug-log on, every line. Before the settings
// are read, every line.
static int g_debug_log = 1;
static const char *const KEY_LINES[] = {
    "loaded, game base", "steam: ready", "unsupported game build", "net guard", "settings:", "Seamless Co-op", "guard broken",
    "not hooked", "not as expected", "not expected", "cannot", "failed", "NOT ", "not started", "not written",
    "host: session up", "host: expedition reached", "host: expedition over", "host: member admitted", "role: ",
    "host: no member left", "leave done", "join: search started", "joining lobby", "join -> ", "stopping (",
    "in the host's world", "world sync at step", "invasion won", "invasion lost", "invasion neither", "level: ", "map test", "two-regions", "expedition: result", "reward test: won", "reward test: lost",
    "expedition end requested", "flags: ", "lag: ", "move check: ", "map pin: ", "name tag: ", "scaling: ", "version: ", "counts overlay: ", "capacity: ", "slot: ", "map test",  "still down", "watchdog", "no map load", "went back", "code patch", "crash"};
static void logf(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!g_debug_log) {
        size_t k = 0;
        while (k < sizeof KEY_LINES / sizeof *KEY_LINES && !strstr(line, KEY_LINES[k])) k++;
        if (k == sizeof KEY_LINES / sizeof *KEY_LINES) return;
    }
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, line);
    fclose(f);
}


// --- Crash logger ---------------------------------------------------------------------------------------
// A fatal exception leaves its address in invasion.log, with the short log too. Last-chance only: the unhandled
// exception filter (SetUnhandledExceptionFilter), so exceptions the game catches itself never reach it; C++
// exceptions (0xe06d7363) and debugger codes are passed on untouched. If something else installs its own filter
// later, crash_watch() (every second from our thread) puts ours back on top and chains to the other one (logged
// once, by module + offset). Written with WriteFile on a handle opened in the handler: no heap, no CRT locks;
// a static buffer and a one-shot guard. No paths and no ids: modules by base name only.
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static volatile LONG g_crash_once;
static int g_ff_hooked;  // bit 0 import slot, bit 1 the pointer at 0x142bc21f0
static char g_crash_buf[8192];
static size_t g_crash_at;
static char *g_dll_base;
static size_t g_dll_size, g_exe_size;
static void cb_str(const char *t) { while (*t && g_crash_at < sizeof g_crash_buf - 1) g_crash_buf[g_crash_at++] = *t++; }
static void cb_hex(unsigned long long v) {
    char t[19] = "0x", d[16];
    int n = 0;
    do d[n++] = "0123456789abcdef"[v & 15], v >>= 4; while (v && n < 16);
    for (int i = 0; i < n; i++) t[2 + i] = d[n - 1 - i];
    t[2 + n] = 0;
    cb_str(t);
}
static size_t image_size(char *base) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    return ((IMAGE_NT_HEADERS *)(base + dos->e_lfanew))->OptionalHeader.SizeOfImage;
}
static void cb_addr(unsigned long long a) {  // module + offset, or raw
    if (g_base && a >= (unsigned long long)g_base && a < (unsigned long long)g_base + g_exe_size) {
        cb_str("exe+"), cb_hex(a - (unsigned long long)g_base), cb_str(" ("), cb_hex(a - (unsigned long long)g_base + 0x140000000ull), cb_str(")");
        return;
    }
    if (g_dll_base && a >= (unsigned long long)g_dll_base && a < (unsigned long long)g_dll_base + g_dll_size) {
        cb_str("invasion.dll+"), cb_hex(a - (unsigned long long)g_dll_base);
        return;
    }
    HMODULE m = NULL;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m &&
        GetModuleFileNameA(m, name, sizeof name)) {
        char *x = strrchr(name, '\\'), *y = strrchr(name, '/'), *base = x > y ? x : y;
        cb_str(base ? base + 1 : name), cb_str("+"), cb_hex(a - (unsigned long long)m);
        return;
    }
    cb_hex(a);
}
static int fatal_code(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT: case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: case EXCEPTION_INVALID_DISPOSITION: case 0xc0000409: /* stack buffer overrun */
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_STACK_CHECK:
        return 1;
    }
    return 0;
}
static volatile DWORD g_last_frame;  // GetTickCount() at the last step hook (game thread), 0 before the first
static void cb_stack(unsigned long long *sp, unsigned long long *top) {  // return addresses into the game or our DLL
    int found = 0;
    for (int i = 0; sp + i < top && i < 4096 && found < 24; i++) {
        unsigned long long v = sp[i];
        int ours = (v >= (unsigned long long)g_base && v < (unsigned long long)g_base + g_exe_size) ||
                   (g_dll_base && v >= (unsigned long long)g_dll_base && v < (unsigned long long)g_dll_base + g_dll_size);
        if (ours) cb_str(" "), cb_addr(v), found++;
    }
    cb_str("\n");
}
static void cb_dec(unsigned v) {
    char d[12];
    int n = 0;
    do d[n++] = (char)('0' + v % 10), v /= 10; while (v && n < 11);
    while (n) { char c[2] = {d[--n], 0}; cb_str(c); }
}
static void cb_frame(const char *hms) {  // how long ago the game's last frame tick was
    cb_str(hms), cb_str("crash: last frame tick ");
    if (g_last_frame) cb_dec(GetTickCount() - g_last_frame), cb_str(" ms ago\n");
    else cb_str("none yet\n");
}
static void cb_flush(void) {
    HANDLE h = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, g_crash_buf, (DWORD)g_crash_at, &w, NULL);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}
static void cb_hms(char hms[10]) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    hms[0] = (char)('0' + t.wHour / 10), hms[1] = (char)('0' + t.wHour % 10), hms[2] = ':', hms[3] = (char)('0' + t.wMinute / 10);
    hms[4] = (char)('0' + t.wMinute % 10), hms[5] = ':', hms[6] = (char)('0' + t.wSecond / 10), hms[7] = (char)('0' + t.wSecond % 10);
    hms[8] = ' ', hms[9] = 0;
}
static void crash_write(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char hms[10] = {(char)('0' + t.wHour / 10), (char)('0' + t.wHour % 10), ':', (char)('0' + t.wMinute / 10), (char)('0' + t.wMinute % 10), ':',
                    (char)('0' + t.wSecond / 10), (char)('0' + t.wSecond % 10), ' ', 0};
    g_crash_at = 0;
    cb_str(hms), cb_str("crash: exception "), cb_hex(r->ExceptionCode), cb_str(" at "), cb_addr((unsigned long long)r->ExceptionAddress);
    cb_str(" thread "), cb_hex(GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR k = r->ExceptionInformation[0];
        cb_str(k == 0 ? " | reading " : k == 1 ? " | writing " : k == 8 ? " | executing " : " | access "), cb_hex(r->ExceptionInformation[1]);
    }
    cb_str("\n");
    static const char *const RN[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    DWORD64 rv[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
    cb_str(hms), cb_str("crash: registers");
    for (int i = 0; i < 16; i++) cb_str(" "), cb_str(RN[i]), cb_str("="), cb_hex(rv[i]);
    cb_str(" rip="), cb_hex(c->Rip), cb_str("\n");
    // return addresses on the stack that point into the game or our DLL, from rsp up to the stack's base
    unsigned long long *sp = (unsigned long long *)c->Rsp, *top = (unsigned long long *)((NT_TIB *)NtCurrentTeb())->StackBase;
    cb_str(hms), cb_str("crash: stack");
    cb_stack(sp, top);
    cb_frame(hms);
    cb_flush();
}
static volatile LONG g_crash_inside;
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    if (InterlockedExchange(&g_crash_inside, 1)) return EXCEPTION_CONTINUE_SEARCH;  // a filter behind us called us again
    if (ep && ep->ExceptionRecord && fatal_code(ep->ExceptionRecord->ExceptionCode) && !InterlockedExchange(&g_crash_once, 1))
        crash_write(ep);
    LONG r = g_prev_filter && g_prev_filter != crash_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    g_crash_inside = 0;
    return r;
}
static void crash_install(HINSTANCE self) {
    g_dll_base = (char *)self, g_dll_size = image_size((char *)self), g_exe_size = image_size(g_base);
    g_prev_filter = SetUnhandledExceptionFilter(crash_filter);
}
static void crash_watch(void) {  // our filter stays on top; one replacing it is chained behind ours
    LPTOP_LEVEL_EXCEPTION_FILTER now = SetUnhandledExceptionFilter(crash_filter);
    if (now != crash_filter) {
        static int told;
        g_prev_filter = now;
        if (told++) return;
        g_crash_at = 0, cb_str(""), cb_addr((unsigned long long)now);
        g_crash_buf[g_crash_at] = 0;
        logf("crash logger: another unhandled-exception filter (%s) was put in; ours is on top again and calls it", g_crash_buf);
    }
}

// Fast fail: on real Windows the CRT ends the process with int 0x29 (__fastfail), which reaches no exception
// filter or handler, so it left no record. Every fast-fail path of the game's CRT asks IsProcessorFeaturePresent(23)
// (PF_FASTFAIL_AVAILABLE) right before int 0x29: __report_gsfailure 0x142691050 (code 2, stack cookie), the
// __fastfail wrapper 0x142691138, 0x142691a80, abort 0x1426cb855 (code 7), invalid parameter 0x14269ccc0 (code 5);
// without fast fail they set the filter to NULL, call UnhandledExceptionFilter and TerminateProcess(0xc0000409)
// (0x14269101a / 0x1472d664b), again past ours. So the import slot (and the pointer 0x142bc21f0 the last two call
// through, when it holds the same function) goes through pf_hook: feature 23 writes one record, then the real call.
static BOOL (WINAPI *g_pf_real)(DWORD);
static volatile DWORD g_ff_tick;  // when pf_hook wrote its record (the run thread checks the process went on)
static BOOL WINAPI pf_hook(DWORD feature) {
    if (feature == 23 && !InterlockedExchange(&g_crash_once, 1)) {
        char hms[10];
        cb_hms(hms);
        g_crash_at = 0;
        cb_str(hms), cb_str("crash: fast fail next (the game's CRT asked for PF_FASTFAIL_AVAILABLE; int 0x29 or TerminateProcess follows), called from ");
        cb_addr((unsigned long long)__builtin_return_address(0)), cb_str(" thread "), cb_hex(GetCurrentThreadId()), cb_str("\n");
        cb_str(hms), cb_str("crash: stack");
        cb_stack((unsigned long long *)__builtin_frame_address(0), (unsigned long long *)((NT_TIB *)NtCurrentTeb())->StackBase);
        cb_frame(hms);
        cb_flush();
        g_ff_tick = GetTickCount() | 1;
    }
    return g_pf_real(feature);
}
static void fastfail_install(void) {  // DllMain: import slot and the second pointer, same function only
    HMODULE k = GetModuleHandleA("kernel32.dll");
    void *real = k ? (void *)GetProcAddress(k, "IsProcessorFeaturePresent") : NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void **slots[2] = {NULL, (void **)(g_base + 0x2bc21f0)};
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress); dir.VirtualAddress && d->Name && !slots[0]; d++) {
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + d->OriginalFirstThunk);
        for (size_t i = 0; d->OriginalFirstThunk && names[i].u1.Ordinal && !slots[0]; i++)
            if (!IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) &&
                !strcmp(((IMAGE_IMPORT_BY_NAME *)(g_base + names[i].u1.Ordinal))->Name, "IsProcessorFeaturePresent"))
                slots[0] = (void **)(g_base + d->FirstThunk) + i;
    }
    for (int i = 0; i < 2; i++) {
        DWORD old;
        if (!slots[i] || IsBadReadPtr(slots[i], 8)) continue;
        void *cur = *slots[i];
        if (cur == (void *)pf_hook) { g_ff_hooked |= 1 << i; continue; }
        if (i == 1 && cur != real && cur != (void *)g_pf_real) continue;  // only when it is the same function
        if (!VirtualProtect(slots[i], 8, PAGE_READWRITE, &old)) continue;
        if (!g_pf_real) g_pf_real = (BOOL(WINAPI *)(DWORD))cur;
        *slots[i] = (void *)pf_hook;
        VirtualProtect(slots[i], 8, old, &old);
        g_ff_hooked |= 1 << i;
    }
}
// Steam going away (dev.46 runs 74 / 61: at every game exit our echo thread read a freed Steam interface, "reading
// 0xbeef" in lsteamclient, after the game's last frame). The game shuts Steam down once, at exit, through its import
// SteamAPI_Shutdown (steam_api64.dll); going back to the Roundtable Hold does not call it. Our wrapper first stops
// every Steam use of ours (g_steam_down: the echo thread leaves its loop, steam_ready() answers no) and waits until
// the echo thread is out of its Steam calls (at most 500 ms), then lets the game's call through.
static volatile LONG g_steam_down, g_steam_busy;
static void (*g_steam_shutdown_real)(void);
static void steam_shutdown_hook(void) {
    InterlockedExchange(&g_steam_down, 1);
    for (int i = 0; i < 250 && g_steam_busy; i++) Sleep(2);
    logf("steam: the game shuts Steam down; our Steam use stopped first%s", g_steam_busy ? " (the echo thread did not finish in 500 ms)" : "");
    if (g_steam_shutdown_real) g_steam_shutdown_real();
}
static void steam_shutdown_install(void) {  // DllMain: the exe's import slot of SteamAPI_Shutdown, only if it holds the real one
    HMODULE m = GetModuleHandleA("steam_api64.dll");
    void *real = m ? (void *)GetProcAddress(m, "SteamAPI_Shutdown") : NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void **slot = NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress); real && dir.VirtualAddress && d->Name && !slot; d++) {
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + d->OriginalFirstThunk);
        for (size_t i = 0; d->OriginalFirstThunk && names[i].u1.Ordinal && !slot; i++)
            if (!IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) &&
                !strcmp(((IMAGE_IMPORT_BY_NAME *)(g_base + names[i].u1.Ordinal))->Name, "SteamAPI_Shutdown"))
                slot = (void **)(g_base + d->FirstThunk) + i;
    }
    DWORD old;
    if (!slot || *slot != real || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        logf("steam: SteamAPI_Shutdown import not found or not the real one: not wrapped");
        return;
    }
    g_steam_shutdown_real = (void (*)(void))real;
    *slot = (void *)steam_shutdown_hook;
    VirtualProtect(slot, 8, old, &old);
    logf("steam: SteamAPI_Shutdown wrapped (our Steam use stops before it)");
}
// Exit line (DLL_PROCESS_DETACH, written the same allocation-free way): process exit or unload. No line and no
// crash record = the process was ended from outside (TerminateProcess, killed) or died where we cannot see.
static void exit_line(int process_exit) {
    char hms[10];
    cb_hms(hms);
    g_crash_at = 0;
    cb_str(hms), cb_str(process_exit ? "exit: the game process is ending normally (DLL detach at process exit)\n" : "exit: our DLL is unloaded (FreeLibrary)\n");
    cb_frame(hms);
    cb_flush();
}

// Test settings: test-settings.toml next to the DLL (the launcher writes it at every start from the
// file next to the launcher; the dev build copies $W/test-settings.toml). Read once, at load.
#include "settings.h"
static short g_settings[SETTINGS_COUNT];
static int setting(int i) { return g_settings[i] > 0; }
static void next_to_dll(char *path, size_t cap, const char *name) {
    snprintf(path, cap, "%s", log_path);
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
    if (dir) snprintf(dir + 1, cap - (size_t)(dir + 1 - path), "%s", name);
}
static int g_settings_early;  // read in DllMain already
static void settings_load(void) {
    char path[MAX_PATH], text[4096], bad[300], old[200] = "";  // path is never logged (it holds the OS user name)
    next_to_dll(path, sizeof path, "test-settings.toml");
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(text, 1, sizeof text, f) : 0;
    if (f) fclose(f);
    if (settings_parse(text, n, g_settings, bad, sizeof bad)) logf("settings: lines not used: %s", bad);
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        if (g_settings[i] < 0 || !f) g_settings[i] = g_settings[i] < 0 || !f ? (short)(-1 - setting_default(i)) : g_settings[i];
        int v = g_settings[i] < 0 ? -1 - g_settings[i] : g_settings[i];
        char num[8];
        snprintf(num, sizeof num, "%d", v);
        logf("settings: %s = %s (%s)", SETTING_NAMES[i], setting_is_number(i) ? num : v ? "true" : "false",
             !f ? "no test-settings.toml next to the DLL, default" : g_settings[i] < 0 ? "not in the file, default" : "test-settings.toml");
        g_settings[i] = (short)v;
    }
    for (int i = 0; i < SETTINGS_COUNT; i++)  // the empty marker files of earlier test builds
        for (int txt = 0; txt < 2; txt++) {
            char name[40], p[MAX_PATH];
            snprintf(name, sizeof name, "%s%s", SETTING_NAMES[i], txt ? ".txt" : "");
            next_to_dll(p, sizeof p, name);
            if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) snprintf(old + strlen(old), sizeof old - strlen(old), " %s", name);
        }
    if (old[0]) logf("settings: old marker files ignored (test-settings.toml replaces them):%s", old);
    g_debug_log = setting(S_DEBUG_LOG);
    logf("settings: log %s", g_debug_log ? "full (debug-log)" : "short (key lines only; debug-log gives everything)");
}

// --- network guard -------------------------------------------------------------------------------
//
// The mod must never let the game reach FromSoftware's servers, and reading code cannot prove that
// (parts of the game's network code are encrypted). So the game executable is made unable to do it:
// its own imports of the functions that open connections or resolve names point at the functions
// below instead. Steam's traffic (lobbies, P2P) runs inside steamclient through steam_api64.dll and
// does not use these imports.
//   - WINHTTP!WinHttpConnect: always refused. The game's one WinHTTP user (0x2109fa1) is its HTTP
//     request runner, and the only URL in the executable is https://cl-steam.fromsoftware-game.net.
//   - WS2_32!getaddrinfo, gethostbyname (#52): names containing "fromsoftware" refused, others passed
//     through; every name is logged the first time it is seen.
//   - WS2_32!connect (#4), WSAConnect, sendto (#20, UDP to an address): every destination logged the
//     first time and refused while NET_GUARD_REFUSE_OUTBOUND is 1. Their callers in readable code are
//     libcurl, Havok's debug socket and Dantelion's DevConnect / TCPServerSocket tools; nothing there
//     belongs to Steam P2P (the session layer uses ISteamNetworking through steam_api64).
// The rest of WS2_32 (socket, bind, listen, accept, send/recv on sockets that already exist, select,
// byte order, getnameinfo, ...) cannot open a connection to a new address and is left alone.
// Code reads these slots at each call ("call [rip+slot]"); no readable code copies them elsewhere.
// The rdata copy of the import table (0x2bc2000) has no readable references for these functions.
//
// Installed from DllMain, before the game's code runs on (me3 loads natives right after the
// protection's start-up, before the game's main), and checked again: if any slot is missing, holds
// something unexpected or is later overwritten, set_network(1) refuses to bring the network up.
#define NET_GUARD_REFUSE_OUTBOUND 1  // 0: only log connect/WSAConnect/sendto destinations
static int g_guard_ok;               // all slots patched
static volatile LONG g_guard_broken; // a patched slot no longer points at our function
static char g_guard_msg[200];        // why the guard failed, for the log (DllMain cannot log)
static SRWLOCK g_seen_lock = SRWLOCK_INIT;

// True the first time `key` is seen. ponytail: 128 keys, after that nothing new is logged.
static int first_time(const char *key) {
    static char seen[128][80];
    static int count;
    int fresh = 1;
    AcquireSRWLockExclusive(&g_seen_lock);
    for (int i = 0; i < count && fresh; i++) fresh = strcmp(seen[i], key) != 0;
    if (fresh && count < 128) snprintf(seen[count++], sizeof seen[0], "%s", key);
    else fresh = 0;
    ReleaseSRWLockExclusive(&g_seen_lock);
    return fresh;
}
static int is_fromsoftware(const char *name) {
    static const char word[] = "fromsoftware";
    for (const char *p = name; p && *p; p++) {
        size_t i = 0;
        while (word[i] && p[i] && (p[i] | 0x20) == word[i]) i++;
        if (!word[i]) return 1;
    }
    return 0;
}
// "1.2.3.4:443" or "[0:0:...:1]:443" from a sockaddr.
static void addr_text(const unsigned char *sa, int len, char *out, size_t cap) {
    unsigned family = sa && len >= 4 ? (unsigned)(sa[0] | sa[1] << 8) : 0, port = sa && len >= 4 ? (unsigned)(sa[2] << 8 | sa[3]) : 0;
    if (family == 2 && len >= 16) snprintf(out, cap, "%u.%u.%u.%u:%u", sa[4], sa[5], sa[6], sa[7], port);
    else if (family == 23 && len >= 24) {
        snprintf(out, cap, "[");
        for (int i = 0; i < 8; i++)
            snprintf(out + strlen(out), cap - strlen(out), "%s%x", i ? ":" : "", (unsigned)(sa[8 + 2 * i] << 8 | sa[9 + 2 * i]));
        snprintf(out + strlen(out), cap - strlen(out), "]:%u", port);
    } else snprintf(out, cap, "family %u", family);
}

typedef void *(WINAPI *winhttp_connect_fn)(void *, const wchar_t *, unsigned short, DWORD);
typedef int(WINAPI *getaddrinfo_fn)(const char *, const char *, const void *, void **);
typedef void *(WINAPI *gethostbyname_fn)(const char *);
typedef int(WINAPI *connect_fn)(UINT_PTR, const void *, int);
typedef int(WINAPI *wsaconnect_fn)(UINT_PTR, const void *, int, void *, void *, void *, void *);
typedef int(WINAPI *sendto_fn)(UINT_PTR, const char *, int, int, const void *, int);
static getaddrinfo_fn g_getaddrinfo;
static gethostbyname_fn g_gethostbyname;
static connect_fn g_connect;
static wsaconnect_fn g_wsaconnect;
static sendto_fn g_sendto;

static void *WINAPI guard_winhttp_connect(void *session, const wchar_t *server, unsigned short port, DWORD reserved) {
    (void)session, (void)reserved;
    char host[80] = "", key[100];
    for (int i = 0; server && server[i] && i < 79; i++) host[i] = server[i] < 128 ? (char)server[i] : '?';
    snprintf(key, sizeof key, "http %s:%u", host, port);
    if (first_time(key)) logf("net guard: WinHttpConnect %s:%u refused", host, port);
    SetLastError(12029);  // ERROR_WINHTTP_CANNOT_CONNECT
    return NULL;
}
static int name_refused(const char *api, const char *name) {
    int refuse = is_fromsoftware(name);
    char key[100];
    snprintf(key, sizeof key, "dns %s", name ? name : "(null)");
    if (first_time(key)) logf("net guard: %s %s %s", api, name ? name : "(null)", refuse ? "refused" : "passed");
    if (refuse) SetLastError(11001);  // WSAHOST_NOT_FOUND
    return refuse;
}
static int WINAPI guard_getaddrinfo(const char *node, const char *service, const void *hints, void **result) {
    if (name_refused("getaddrinfo", node)) {
        if (result) *result = NULL;
        return 11001;
    }
    return g_getaddrinfo(node, service, hints, result);
}
static void *WINAPI guard_gethostbyname(const char *name) {
    return name_refused("gethostbyname", name) ? NULL : g_gethostbyname(name);
}
static int outbound_refused(const char *api, const void *to, int len) {
    char addr[64], key[100];
    addr_text(to, len, addr, sizeof addr);
    snprintf(key, sizeof key, "%s %s", api, addr);
    if (first_time(key)) logf("net guard: %s %s %s", api, addr, NET_GUARD_REFUSE_OUTBOUND ? "refused" : "passed");
    if (NET_GUARD_REFUSE_OUTBOUND) SetLastError(10061);  // WSAECONNREFUSED
    return NET_GUARD_REFUSE_OUTBOUND;
}
static int WINAPI guard_connect(UINT_PTR s, const void *to, int len) {
    return outbound_refused("connect", to, len) ? -1 : g_connect(s, to, len);
}
static int WINAPI guard_wsaconnect(UINT_PTR s, const void *to, int len, void *a, void *b, void *c, void *d) {
    return outbound_refused("WSAConnect", to, len) ? -1 : g_wsaconnect(s, to, len, a, b, c, d);
}
static int WINAPI guard_sendto(UINT_PTR s, const char *buf, int len, int flags, const void *to, int tolen) {
    if (to && outbound_refused("sendto", to, tolen)) return -1;  // no address: an existing connection
    return g_sendto(s, buf, len, flags, to, tolen);
}

static const struct {
    const char *dll, *name;
    unsigned ordinal;  // how the executable imports it; 0 = by name
    void *hook, **original;
} GUARDED[] = {
    {"winhttp.dll", "WinHttpConnect", 0, (void *)guard_winhttp_connect, NULL},
    {"ws2_32.dll", "getaddrinfo", 0, (void *)guard_getaddrinfo, (void **)&g_getaddrinfo},
    {"ws2_32.dll", "gethostbyname", 52, (void *)guard_gethostbyname, (void **)&g_gethostbyname},
    {"ws2_32.dll", "connect", 4, (void *)guard_connect, (void **)&g_connect},
    {"ws2_32.dll", "WSAConnect", 0, (void *)guard_wsaconnect, (void **)&g_wsaconnect},
    {"ws2_32.dll", "sendto", 20, (void *)guard_sendto, (void **)&g_sendto},
};
#define GUARDED_COUNT (sizeof GUARDED / sizeof *GUARDED)
static void **g_guard_slot[GUARDED_COUNT];

// The main module's import slot for GUARDED[i], or NULL.
static void **import_slot(unsigned i) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void **found = NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress); dir.VirtualAddress && d->Name; d++) {
        if (_stricmp(g_base + d->Name, GUARDED[i].dll)) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + d->OriginalFirstThunk);
        for (size_t k = 0; d->OriginalFirstThunk && names[k].u1.Ordinal; k++) {
            ULONGLONG v = names[k].u1.Ordinal;
            int match = IMAGE_SNAP_BY_ORDINAL64(v)
                            ? GUARDED[i].ordinal && IMAGE_ORDINAL64(v) == GUARDED[i].ordinal
                            : !GUARDED[i].ordinal && !strcmp(((IMAGE_IMPORT_BY_NAME *)(g_base + v))->Name, GUARDED[i].name);
            if (!match) continue;
            if (found) return NULL;  // imported twice: one slot would stay open
            found = (void **)(g_base + d->FirstThunk) + k;
        }
    }
    return found;
}

// Runs in DllMain (loader lock held): only module lookups of loaded DLLs, VirtualProtect and writes.
static void install_net_guard(void) {
    unsigned patched = 0;
    for (unsigned i = 0; i < GUARDED_COUNT; i++) {
        HMODULE m = GetModuleHandleA(GUARDED[i].dll);
        void *real = m ? (void *)GetProcAddress(m, GUARDED[i].name) : NULL;
        void **slot = import_slot(i);
        // An ordinal import must be the function its name says.
        if (real && GUARDED[i].ordinal && (void *)GetProcAddress(m, (LPCSTR)(ULONG_PTR)GUARDED[i].ordinal) != real) real = NULL;
        DWORD old;
        if (!real || !slot || *slot != real || !VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &old)) {
            snprintf(g_guard_msg, sizeof g_guard_msg, "%s!%s: %s", GUARDED[i].dll, GUARDED[i].name,
                     !real ? "function not found" : !slot ? "import slot not found" : *slot != real ? "slot does not hold the function" : "cannot unprotect");
            return;
        }
        if (GUARDED[i].original) *GUARDED[i].original = real;
        *slot = GUARDED[i].hook;
        VirtualProtect(slot, sizeof *slot, old, &old);
        g_guard_slot[i] = slot;
        patched++;
    }
    g_guard_ok = patched == GUARDED_COUNT;
}
static int guard_intact(void) {
    if (!g_guard_ok) return 0;
    for (unsigned i = 0; i < GUARDED_COUNT; i++)
        if (*g_guard_slot[i] != GUARDED[i].hook) {
            if (!InterlockedExchange(&g_guard_broken, 1)) logf("net guard: BROKEN, %s!%s was overwritten", GUARDED[i].dll, GUARDED[i].name);
            return 0;
        }
    return 1;
}

// --- session observer -----------------------------------------------------------------------------

// The session implementation behind the session manager (object inside the network library), or
// NULL. Its counter at +0x10 must be non-zero before a session can be created.
static void **session_impl(char *sm) {
    if (!*(void **)(sm + SM_IMPL)) return NULL;  // the accessor dereferences this
    void **impl = ((void **(*)(void *))(g_base + RVA_SESSION_IMPL))(sm + SM_IMPL);
    return impl && *impl ? impl : NULL;
}

// How many invaders a squad of this size may get. Design rule: 1:1, 2:1, 3:2, 4:2.
// Team play: the game has 2 remote player-data slots (0x1401fe4c0 returns 2; 0x1401fe530, 0x1401fe150 stop at 2),
// so a session holds 3 players. Line-ups within that: solo host + 1 invader, squad of 2 + 1 invader; a squad of 3
// takes no invader (3:1, 2:2, 3:2 need more slots).
// With the capacity patch (4 remote slots, 5 players): solo host 1 invader, squad of 2 or 3 up to 2; squad <= 3.
#define SESSION_PLAYERS_MAX ((unsigned)g_cap_remote + 1)
static int g_df_used;  // day_flow.h: this day's challenge was taken
static unsigned invaders_allowed(unsigned squad) {
    static const unsigned table3[] = {0, 1, 1, 0}, table5[] = {0, 1, 2, 2};
    const unsigned *t = g_cap_remote == 4 ? table5 : table3;
    return squad < 4 ? t[squad] : 0;
}

// Log the session manager's state whenever it changes.
// ponytail: polled from our own thread, not the game's update thread. Fine for reading a few
// fields; move onto a game-thread hook before doing anything heavier.
// The game's three network managers. Each embeds the same starter object: state at +0x10 (0 off,
// 1 starting, 2 ready, 3 and up: stopped with an error), "enabled" byte at +0x20. The game's
// "network ready" test (0xdecac0) wants all three at 2; 0xdecc10(flag) enables or disables all
// three and 0xdecc70() starts them. Observed only: which of them a session needs is not known yet.
#define RVA_NET_OFFLINE_FLAG 0x3c174ac  // u8, the inverse of the last enable flag
static const struct { unsigned ptr_rva, starter; } NET_MANAGERS[3] = {
    {0x3c21e50, 0x60}, {0x3c21638, 0x18}, {RVA_SESSION_MAN_PTR, 0x48}};

// Class name of a C++ object from its RTTI, or "?".
static const char *class_name(void *object) {
    char *vt = object ? *(char **)object : NULL;
    if (!vt || vt < g_base || vt > g_base + 0x8000000) return "?";
    char *locator = *(char **)(vt - 8);
    if (locator < g_base || locator > g_base + 0x8000000) return "?";
    unsigned type = *(unsigned *)(locator + 0x0c);
    return type && type < 0x8000000 ? g_base + type + 0x10 : "?";
}

// Bringing the network up from the Roundtable Hold.
//
// Up: enable the three managers, then the game's own sign-in (CSAutoControlAPI::SignIn, 0xf18d30),
// which copies the local user's identity into the sign-in manager and starts the managers. Seamless
// Co-op makes the same two calls. Down: stop the managers and disable them again.
//
// The game does not expect this to happen while the player stands on a map. CS::CSNetworkFlowStep
// watches the managers; its OfflineMode step sees them become ready and moves on to
// NotifyLeaveOfflineModeWait, which starts CSEventNetworkErrorReturnTitleStep: a message box (empty,
// the game has no text for it here) and a return to the title screen. So while the network is up
// because we started it, the OfflineMode step is not run and the flow stays where it is.
// ponytail: the flow never reaches its OnlineMode step this way. That step's server requests are
// exactly what the mod must not make; revisit only if a session turns out to need that step.
#define RVA_NET_ENABLE_ALL   0xdecc10   // void (u8 on); also keeps the global offline flag in step
#define RVA_NET_STOP_ALL     0xdecb70   // void (void)
#define RVA_SIGN_IN          0xf18d30   // bool (int user)
#define RVA_FLOW_STEP_TABLE  0x39b2360  // CSNetworkFlowStep: 16-byte entries (function, name)
#define FLOW_STEP_OFFLINE    1
#define RVA_FLOW_OFFLINE_FN  0xde7f90
static int g_network_on;  // we brought the network up
static void (*g_flow_offline_step)(void *, void *);
static void flow_offline_hook(void *flow, void *time) {
    if (!g_network_on) g_flow_offline_step(flow, time);
}
static void hook_network_flow(void) {
    void **entry = (void **)(g_base + RVA_FLOW_STEP_TABLE) + FLOW_STEP_OFFLINE * 2;
    if (*entry != (void *)(g_base + RVA_FLOW_OFFLINE_FN)) {
        logf("network flow: step table does not look as expected, not hooked");
        return;
    }
    g_flow_offline_step = (void (*)(void *, void *))*entry;
    *entry = (void *)flow_offline_hook;
    logf("network flow: hooked");
}
// Every map load (both sides, always on): MoveMapStep::STEP_MsbLoad (table entry 0x39af8b0, function
// 0xc36840) loads the map whose id is at step +0xdc; it is also what tells a joining client's world
// sync (phase 3) that its own map load has begun. Logged once per step object and map.
#define RVA_MSB_LOAD_ENTRY 0x39af8b0
#define RVA_MSB_LOAD_FN    0xc36840
static void (*g_msb_load)(void *);
static int g_step_back_to_hub;
static int g_move, g_join;   // defined with the join test below
static DWORD g_move_load_tick;
static void msb_load_hook(char *step) {
    static char *last_step;
    static unsigned last_map;
    unsigned map = *(unsigned *)(step + 0xdc);
    if (step != last_step || map != last_map) {
        last_step = step, last_map = map;
        char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
        logf("map load: map %08x (+e4 %08x), lobby_state %d", map, *(unsigned *)(step + 0xe4),
             sm ? (int)*(unsigned *)(sm + SM_LOBBY_STATE) : -1);
        if (g_move >= 2 && g_join == 4) {
            if (g_move == 2) g_move = 3, g_move_load_tick = GetTickCount();
            if (map == 0x0a000000) {
                g_step_back_to_hub = 1;
                logf("move test: this load is the Roundtable Hold again");
            }
        }
    }
    g_msb_load(step);
}
static void hook_map_load(void) {
    void **entry = (void **)(g_base + RVA_MSB_LOAD_ENTRY);
    if (*entry != (void *)(g_base + RVA_MSB_LOAD_FN)) {
        logf("map load: step table does not look as expected, not hooked");
        return;
    }
    g_msb_load = (void (*)(void *))*entry;
    *entry = (void *)msb_load_hook;
    logf("map load: hooked");
}
// Host session: a mod player's expedition opens a session as host on the network we start, so that
// invaders can find it later. For now only while a file named "host-test" lies next to the DLL
// (looked for when the expedition reaches its world); without it expeditions stay as before.
// When: CSQuickMatchingCtrl's InGame step (23), first frame. Not earlier, because for a solo
// expedition the game itself leaves any session in MoveMapStart (0xb593a0: context mode +0x9d != 2
// or online -> 0xdf1180 unless 0xdf0a80), and MoveMapStep's LeaveSessionWait (0xc357c0) is protected
// code. Ended by the first step that is not InGame (Unregister after the run; see host_expedition).
//
// Create: 0xdefbd0, the game's own create-session (SessionCreateJob's lambda 0xb1ab00 calls it with
// the job's multiplay type and a pointer to a byte 1). It refuses while lobby_state is 1, 3, 4 or 6,
// builds the session's ciphers (0xdf6e70, protected), fills the parameters (0xdf4830: player limit
// from +0x170) and calls the session implementation's create (0x25846a0), which needs the started
// counter at impl+0x10 and starts a Steam session object; that object's step 0x2591950 calls
// ISteamMatchmaking::CreateLobby. Success: lobby_state 1, then 3 (host) once Steam has made the lobby.
// Leave: 0xdf1180(manager, 0), the way SessionLeaveJob (0xb1aa90) and CSQuickMatchingCtrl
// (0xb59332, 0xb5944f) leave: lobby_state 7 and an asynchronous leave in the implementation.
// The network goes down only once the leave is through (finish_pending_stop below).
#define RVA_NET_READY       0xdecac0  // bool (void): all three managers at state 2 (reads only)
#define RVA_SESSION_CREATE  0xdefbd0  // bool (CSSessionManager*, u8 multiplay type, u8 *one)
#define RVA_SESSION_LEAVE   0xdf1180  // void (CSSessionManager*, u8 reason); the game passes 0
#define SM_CREATE_HANDLE    0x24      // u32, what the implementation's create returned
#define MULTIPLAY_WHITE     0         // multiplay type 0, as in the run with Seamless Co-op
static int g_host_test;  // 0 off, 1 waiting for the network, 2 create called, 3 skipped

// Why the session falls back to "none" (first host test: lobby_state 1 -> 0 within a second, one
// member). The manager's update (0xdf2a20) polls the create handle (+0x24) through the
// implementation (vt+8). Status 0: the session handle goes to +8 and 0xdf5500 runs: lobby_state = 3,
// +0x10 = 4, +0x6b8 = 0, ..., and at its very end 0xf271d0(EAC manager, 2), which begins the EOS
// anti-cheat session (0xf28cd0). If that returns false, 0xdf61e0 cleans up: lobby_state 0, handle 0.
// Members are added after 0xdf5500 either way (that is the one member). Other status: 0xdf61e0
// straight away, no members. Later, an implementation event 4 also ends in 0xdf61e0, storing its
// code at +0x6b8 first.
// 0xf28cd0 fails, and leaves a code at EAC+0xa0, when: EOS platform handle (+8) is null (0xf4241),
// the local EOS user id is empty (0xf4242), the id is invalid (0xf4243), BeginSession fails
// (0xf424a), other (0xf4244). The cleanup (0xf27270) clears +0x98 but not +0xa0.
// So after a failure: EAC code set and +0x6b8 zero = the anti-cheat step; +0x6b8 non-zero = a
// session event from the implementation. Read every frame on the game thread while the test runs.
#define RVA_EAC_MAN_PTR   0x3c2c1a0  // EOS anti-cheat manager (EOS_AntiCheatClient_* calls)
#define EAC_PLATFORM      0x08       // EOS platform handle
#define EAC_SESSION_FLAGS 0x98       // u8, 2 = session begun for P2P
#define EAC_ERROR         0xa0       // u32, last error of the begin-session step
#define SM_END_CODE       0x6b8      // u64, code of the implementation event that ended the session
static DWORD g_host_test_tick;
// The session's member list (0xF0-byte entries at +0x78), as the member-add code 0xdf5ae0 fills it:
// +0xd0 player slot (0xdfd6a0), +0xd4 a value from 0x4d33c0 (host only), +0xe1 "this is us",
// +0xe2 admitted remotely, +0xe3 set for others when we join as client (0xdf5930), +0xea a value
// from the quick-match context (0xb67000 / 0xb66fd0). What +0xd4 and +0xea mean is not known; they
// are logged to tell members apart. No ids or names. Also the manager's admission flags: a host
// with +0x740 or +0x18 set, or whose block list (0xdcba00) names the member, removes a member that
// arrives (implementation vt+0x28); +0x19 makes a client leave right after joining.
#define SM_CLOSED   0x18
#define SM_LEAVING  0x19
#define SM_REFUSE   0x740
static void log_members(char *sm, const char *who) {
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    unsigned count = begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0;
    logf("%s: %u members, admission flags closed %u leaving %u refuse %u", who, count, (unsigned char)sm[SM_CLOSED],
         (unsigned char)sm[SM_LEAVING], (unsigned char)sm[SM_REFUSE]);
    for (unsigned i = 0; i < count && i < 8; i++) {
        unsigned char *m = (unsigned char *)begin + i * SM_PLAYER_SIZE;
        logf("%s:   member %u: us %u slot %d d4 %d remote %u e3 %u e4 %u ea %u", who, i, m[0xe1], *(int *)(m + 0xd0),
             *(int *)(m + 0xd4), m[0xe2], m[0xe3], m[0xe4], m[0xea]);
    }
}
static void host_test_watch(void) {
    static unsigned last[7];
    static unsigned long long last_end;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR), *eac = *(char **)(g_base + RVA_EAC_MAN_PTR);
    if (!sm) return;
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    unsigned now[7] = {*(unsigned *)(sm + SM_LOBBY_STATE), *(unsigned *)(sm + SM_HANDLE),
                       *(unsigned *)(sm + SM_CREATE_HANDLE), *(unsigned *)(sm + 0x10),
                       begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0,
                       eac ? (unsigned char)eac[EAC_SESSION_FLAGS] : 0xffffffff, eac ? *(unsigned *)(eac + EAC_ERROR) : 0xffffffff};
    unsigned long long end_code = *(unsigned long long *)(sm + SM_END_CODE);
    if (!memcmp(now, last, sizeof now) && end_code == last_end && g_host_test_tick) return;
    if (!g_host_test_tick) g_host_test_tick = GetTickCount();
    int members_changed = now[4] != last[4];
    memcpy(last, now, sizeof now);
    last_end = end_code;
    logf("host test: +%lums lobby_state %u handle %u create %u role %u players %u end_code %llx | eac %s, platform %s, flags %u, error %u (0x%x)",
         GetTickCount() - g_host_test_tick, now[0], now[1], now[2], now[3], now[4], end_code, eac ? "present" : "missing",
         eac && *(void **)(eac + EAC_PLATFORM) ? "set" : "null", now[5], now[6], now[6]);
    if (members_changed) log_members(sm, "host test");
}
static unsigned long long g_lobby;      // Steam lobby id of our host session, once read
static unsigned g_marked_squad;         // squad size last written to the lobby, ~0 = not yet
static int g_someone_joining;           // Steam lobby members beyond the session's (set by lobby_mark)
static void host_test_start(void) {
    g_lobby = 0;
    g_someone_joining = 0;
    g_marked_squad = ~0u;
    // every player hosts its own expeditions (no switch since 14c271a); no-host (test setting): the own
    // expedition opens no host session (to test whether item pickup breaks inside our host session)
    g_host_test = setting(S_NO_HOST) ? 0 : 1;
    g_host_test_tick = 0;
}
// InGame step, every frame until called: create once the network is ready.
static void host_test_create(void) {
    if (g_host_test != 1 || !((unsigned char (*)(void))(g_base + RVA_NET_READY))()) return;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    void **impl = sm ? session_impl(sm) : NULL;
    if (!impl || !*(unsigned *)((char *)impl + 0x10)) return;  // layer not started yet: create would fail
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    if (state != 0 && state != 2 && state != 5) {  // the join's own test for "no session"
        g_host_test = 3;
        logf("host test: lobby_state %u, a session exists, not creating", state);
        return;
    }
    g_host_test = 2;
    host_test_watch();  // snapshot just before the call, +0ms
    unsigned char one = 1;
    unsigned char ok = ((unsigned char (*)(void *, unsigned char, unsigned char *))(g_base + RVA_SESSION_CREATE))(
        sm, MULTIPLAY_WHITE, &one);
    logf("host test: create session (type %d) -> %u, lobby_state %u -> %u, create handle %u", MULTIPLAY_WHITE, ok,
         state, *(unsigned *)(sm + SM_LOBBY_STATE), *(unsigned *)(sm + SM_CREATE_HANDLE));
}
// The EOS anti-cheat step at the end of "host created" (0xdf5500): "call 0xf271d0" at 0xdf581c, with
// the manager in rcx and 2 in dl; false makes the game clean the session up (0xdf61e0). With EAC off
// the EOS platform handle (manager+8) is null and it always fails (0xf28cd0: error 0xf4241). Owner's
// rule (BRIEF.md): the game's EAC checks on our own sessions are made to pass. While our network is up
// and the guard is intact, the call reports success without running; otherwise the game's function
// runs. What is skipped: with flags +0x98 at 0 the function would call 0xf2a060 (EndSession when a
// platform exists; with none it only zeroes +0x98/+0x9c/+0x58/+0x60), set +0x98 to the mode and call
// 0xf28cd0 (EOS BeginSession). We leave +0x98 at 0, so the leave (0xf27270) has nothing to end.
// Other callers, not covered: 0xdf2d3c (joined as client, result ignored, so only error +0xa0 is
// set), 0xdfe95e and 0xdfeb76 (mode 1, another manager's session at 0xdfexxx, not ours).
// Peer register/unregister for members (0xf27280 from 0xdf61a6, 0xf272a0 from 0xdf6865) return at
// once while the platform handle is null, their results are ignored, so nothing kicks anyone.
#define RVA_EAC_BEGIN       0xf271d0  // bool (EAC manager*, u8 mode): 1 team, 2 P2P session
#define RVA_EAC_BEGIN_CALL  0xdf581c  // e8 af 19 13 00 in 0xdf5500
static unsigned char eac_begin_hook(void *manager, unsigned char mode) {
    if (g_network_on && guard_intact()) {
        logf("host test: EAC begin-session (mode %u) skipped, reported as begun", mode);
        return 1;
    }
    return ((unsigned char (*)(void *, unsigned char))(g_base + RVA_EAC_BEGIN))(manager, mode);
}
static unsigned char *near_alloc(unsigned size);
static int patch_rel32(unsigned char *field, unsigned char *next, unsigned char *target);
static void hook_eac_begin(void) {
    unsigned char *site = (unsigned char *)g_base + RVA_EAC_BEGIN_CALL;
    int expected = (int)(RVA_EAC_BEGIN - (RVA_EAC_BEGIN_CALL + 5));
    unsigned char *stub = near_alloc(12);
    if (site[0] != 0xE8 || memcmp(site + 1, &expected, 4) || !stub) {
        logf("EAC begin-session: call site does not look as expected, not hooked");
        return;
    }
    void *hook = (void *)eac_begin_hook;
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, hook; jmp rax
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 1, site + 5, stub)) logf("EAC begin-session: hooked (host-created call site)");
    else logf("EAC begin-session: cannot patch the call site");
}

// Who may join a running expedition. A new client sends packet 0x48 (8 bytes: two ints); the host
// answers each one in 0xdde430 with packet 0x49 (4 bytes) through 0xdf2380: 0 accepted; 1 when its
// block list (0xdcba00) names the joiner or the manager's +0x743 is set; 2 when the host's
// quick-match context is active (0xb67000, true during an expedition) and 0xb66fb0 does not find
// the joiner on the context's list of expected members (ctx(+0xa8) +0x70..+0x78, 32-byte entries
// {u64 id, ..., +0xc int a, +0x10 int b}; checked when matching (0xb4f600) or in InGame, step 0x17).
// The same check guards the joiner's player data (packet 0x60, 0xdddc10): data of an unexpected
// member is dropped. Our invader is never on that list (it was filled by the expedition's own
// matchmaking), so the first two-player test got answer 2. With host-test, while our host session
// and our network are up and the guard is intact, both calls report "expected" for a member the
// check turned down; everything else (block list, +0x743, sessions not ours) stays as the game has
// it. What we give up: the host no longer limits a running expedition to the players its
// matchmaking brought in; for our sessions that is the point, and the session is ours only.
#define RVA_EXPECTED_MEMBER     0xb66fb0  // bool (qm ctrl**, u64 *member id, int a, int b)
#define RVA_EXPECTED_CALL_JOIN  0xdde7ad  // in 0xdde430 (answer to packet 0x48)
#define RVA_EXPECTED_CALL_DATA  0xddddd6  // in 0xdddc10 (player data, packet 0x60)
#define SM_REFUSE_ALL           0x743
typedef unsigned char (*expected_fn)(void *, unsigned long long *, int, int);
static int g_host_test, g_join;  // defined below
static DWORD g_admit_tick;        // host: last time the join check (bit 1) / player-data check (bit 2) passed
static int g_admit_kind;
static unsigned char expected_check(void *qm, unsigned long long *id, int a, int b, const char *what) {
    unsigned char ok = ((expected_fn)(g_base + RVA_EXPECTED_MEMBER))(qm, id, a, b);
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    int ours = (g_host_test == 2 || g_join == 4) && g_network_on && guard_intact();
    static unsigned long long seen[2][8];
    static int count[2];
    int k = what[0] == 'j' ? 0 : 1, fresh = 1;
    for (int i = 0; i < count[k]; i++) fresh &= seen[k][i] != (id ? *id : 0);
    if (fresh && count[k] < 8) seen[k][count[k]++] = id ? *id : 0;
    if (ok || ours) g_admit_tick = GetTickCount(), g_admit_kind |= k ? 2 : 1;
    if (fresh || !ok)  // every refusal and every acceptance of ours is logged, not only the first per member
        logf("host: %s check for a member (a %d, b %d): expected %u%s; manager +0x743 %u", what, a, b, ok,
             !ok && ours ? " -> accepted (our session)" : !ok ? " -> refused (answer 2)" : "",
             sm ? (unsigned char)sm[SM_REFUSE_ALL] : 0);
    return ok || ours;
}
static unsigned char expected_join_hook(void *qm, unsigned long long *id, int a, int b) {
    return expected_check(qm, id, a, b, "join");
}
static unsigned char expected_data_hook(void *qm, unsigned long long *id, int a, int b) {
    return expected_check(qm, id, a, b, "player data");
}
// Joiner: the host's answer as the client reads it (0xdde8a0 at 0xdf37e3 and 0xdf3922 in the
// session update): 0 ready, 1 -> notice 0x2789 and leave, 2 -> notice 0x27a7 and leave. The notices
// come from NetworkMsgParam, which in Nightreign has no rows 10121 / 10151, so nothing is shown.
#define RVA_READ_ANSWER        0xdde8a0  // bool (ctx*, int *answer, u64 *member id)
#define RVA_READ_ANSWER_CALL1  0xdf37e3
#define RVA_READ_ANSWER_CALL2  0xdf3922
static int g_move_test;
static unsigned char read_answer_hook(void *ctx, int *answer, unsigned long long *member) {
    unsigned char got = ((unsigned char (*)(void *, int *, unsigned long long *))(g_base + RVA_READ_ANSWER))(ctx, answer, member);
    if (got && answer)
        logf("join test: answer from a member: %d (%s)", *answer,
             *answer == 0 ? "ready" : *answer == 1 ? "refused: blocked / host closed (notice 0x2789)" :
             *answer == 2 ? "refused: not an expected member (notice 0x27a7)" : "unknown");
    // Run 98a15ff1: second and third arrivals in one host expedition got answer 2 while the host wrote no
    // join check at all and kept the member (it went only when this side left). The host now logs every
    // answer it sends; until the cause is known, a move-test joiner takes 2 as 0 (logged).
    if (got && answer && *answer == 2 && g_move_test) {
        logf("join test: answer 2 taken as 0 (move test; compare with the host's 'join answer' lines)");
        *answer = 0;
    }
    return got;
}
// A call site that is not yet what we expect is tried again (hooks_retry) for a while: run of dev.63, the Deck
// host at 20:51:56 found 12 call sites in the session code (0xdd..-0xdf.., 0xb58f.., 0x7c8c / 0x7dab) "not as
// expected" that every other start hooked, among them the join check 0xdde7ad, so its joins got answer 2 (the
// game's expected-member refusal). The same code hooked in the starts before and after: the code was not in its
// final state yet at that moment (it is rewritten at run time).
static struct { unsigned site, target; void *hook; const char *what; } g_hook_retry[32];
static int g_hook_retry_n;
static void hook_call(unsigned rva_site, unsigned rva_target, void *hook, const char *what) {
    unsigned char *site = (unsigned char *)g_base + rva_site;
    int expected = (int)(rva_target - (rva_site + 5));
    unsigned char *stub = site[0] == 0xE8 && !memcmp(site + 1, &expected, 4) ? near_alloc(12) : NULL;
    if (!stub) {
        logf("%s: call site %x does not look as expected (%02x %02x %02x %02x %02x), not hooked%s", what, rva_site, site[0], site[1],
             site[2], site[3], site[4], g_hook_retry_n < 32 ? " yet: tried again for 2 minutes" : "");
        if (g_hook_retry_n < 32)
            g_hook_retry[g_hook_retry_n].site = rva_site, g_hook_retry[g_hook_retry_n].target = rva_target,
            g_hook_retry[g_hook_retry_n].hook = hook, g_hook_retry[g_hook_retry_n].what = what, g_hook_retry_n++;
        return;
    }
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, hook; jmp rax
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 1, site + 5, stub)) logf("%s: hooked (%x)", what, rva_site);
    else logf("%s: cannot patch %x", what, rva_site);
}
static void hooks_retry(void) {  // every second for 2 minutes after the start, while some site is still pending
    static DWORD start, next;
    if (!g_hook_retry_n || (int)(GetTickCount() - next) < 0) return;
    if (!start) start = GetTickCount();
    next = GetTickCount() + 1000;
    for (int i = 0; i < g_hook_retry_n; i++) {
        unsigned char *site = (unsigned char *)g_base + g_hook_retry[i].site;
        int expected = (int)(g_hook_retry[i].target - (g_hook_retry[i].site + 5));
        int done = site[0] == 0xE8 && !memcmp(site + 1, &expected, 4);
        if (!done && GetTickCount() - start < 120000) continue;
        if (done) {
            unsigned char *stub = near_alloc(12);
            if (stub) {
                stub[0] = 0x48, stub[1] = 0xB8, memcpy(stub + 2, &g_hook_retry[i].hook, 8), stub[10] = 0xFF, stub[11] = 0xE0;
                logf("%s: %s (%x, %lu ms after the start)", g_hook_retry[i].what,
                     patch_rel32(site + 1, site + 5, stub) ? "hooked late" : "cannot patch", g_hook_retry[i].site, GetTickCount() - start);
            }
        } else
            logf("%s: call site %x still not as expected after 2 minutes: NOT hooked", g_hook_retry[i].what, g_hook_retry[i].site);
        g_hook_retry[i] = g_hook_retry[--g_hook_retry_n], i--;
    }
}
// Host, controller step InGame (0xb58f60): as host it reads ready packets 0x21 (1 byte) from members
// with 0xddbbc0 (marks the member's player data +0x86c ready) and, when one came, sends 0x23 with 1
// through 0xddf930 (0xdf24d0 to the session). So a member who turns ready mid-expedition is answered
// in InGame, nothing is kept for the expedition start only. Both calls are logged when they happen.
#define RVA_READY_READ       0xddbbc0  // bool (void *)
#define RVA_READY_READ_CALL  0xb58fba
#define RVA_GO_SEND          0xddf930  // void (void *, int *value)
#define RVA_GO_SEND_CALL     0xb58fd5
static unsigned char ready_read_hook(void *a) {
    unsigned char got = ((unsigned char (*)(void *))(g_base + RVA_READY_READ))(a);
    if (got) logf("host: ready packet 0x21 from a member read (InGame)");
    return got;
}
static void go_send_hook(void *a, int *value) {
    logf("host: sending 0x23 (%d) to the session (InGame)", value ? *value : -1);
    ((void (*)(void *, int *))(g_base + RVA_GO_SEND))(a, value);
}
// Player data 0x60 (0x30 bytes, sent to each member at the join by 0xde15b0 from 0xdf5ae0): the
// receiver 0xdddc10 drops a packet whose first int equals its own value from the matchmaking manager
// (0xafe130 on DAT_143c17248: its object's vt+0x50, 0 without one). Between our two players both are
// 0 (no matchmaking), so every 0x60 was dropped, the player info never reached the other side, and the
// client's world sync waited at WaitNetworkChrIns. In our sessions the value it compares with is
// changed so the packet passes; then the expected-member check (hooked above) and the game applies
// the info to the member's player data (0x1fe530 slot: +0xc, +0xec, +0x720, name) and 0xdf2730.
#define RVA_MATCH_VALUE       0xafe130  // int (matching manager*)
#define RVA_MATCH_VALUE_CALL  0xdddd37  // in 0xdddc10
static int match_value_hook(void *mgr) {
    int v = ((int (*)(void *))(g_base + RVA_MATCH_VALUE))(mgr);
    if ((g_host_test == 2 || g_join == 4) && g_network_on && guard_intact()) {
        logf("player data 0x60 received: matching value %d, compared as %d so it is applied", v, v ^ 0x55aa55aa);
        return v ^ 0x55aa55aa;
    }
    return v;
}
// World sync requests (packet 0x51, 1 byte: the subtype). Clients send them from their sync steps
// through 0xdf2550 (HostInitInfo 0, ProgressInfo 1, EventState 2, MapOwnerInitSync 3, MapInitState 6,
// and the others); the host's sync step Host (0xdcdc60) reads them through 0xdf1de0 and answers a
// member flagged +0xea (a rejoining member) for every subtype, others only for 0, 4, 5, 7, 8.
#define RVA_SESSION_SEND   0xdf2550  // (sm, packet, data, ...)
#define RVA_SESSION_READ   0xdf1de0  // int (sm, packet, buffer, size, u64 *sender)
static const unsigned SYNC_SEND_SITES[] = {0xdcd60b, 0xdcdbf0, 0xdce787, 0xdcec70, 0xdceff0, 0xdcf1b3, 0xdcf530, 0xdcf6eb, 0xdd0565, 0xdd0f62};
static void sync_send_hook(void *sm, int packet, unsigned char *data, void *a4) {
    if (packet == 0x51) logf("sync: request 0x%x subtype %d sent to the host", packet, data ? data[0] : -1);
    ((void (*)(void *, int, void *, void *))(g_base + RVA_SESSION_SEND))(sm, packet, data, a4);
}
static int sync_read_hook(void *sm, int packet, unsigned char *buf, int size, unsigned long long *sender) {
    int got = ((int (*)(void *, int, void *, int, void *))(g_base + RVA_SESSION_READ))(sm, packet, buf, size, sender);
    if (got > 0) logf("host: sync request 0x%x subtype %d from a member (%d bytes)", packet, buf ? buf[0] : -1, got);
    return got;
}
// Expedition generation (both sides, always on, log only). SingleSetup's first job (0xb4b9f0) calls
// 0x6aec40 (a jump to protected 0x7019a5) at 0xb4bce6; 0xb4cc40 calls it too, at 0xb4d937. Arguments,
// as 0xb4b9f0 builds them: four context bytes (vt+0x198, +0x1b8, +0x1c8, +0x188: context +0xc0 ? 2 :
// +0x17c == 7, +0x17f, +0x180, +0x17c), four vectors {allocator, begin, end, capacity} (u16 from
// 0x1e7820, u16 player data +0xe2, u8 0x6b1010 rare map layer, u8 0x6b12e0 rare map unlock bits) and
// the context byte vt+0x1a8 (+0x17e). Logged with the game manager's start block before and after.
#define RVA_GENERATE        0x6aec40
static char *g_qm_ctrl;  // the controller (set by the step hook, below)
static const char *g_generate_where;
static void vec_text(char *out, size_t cap, void **v, int wide) {
    out[0] = 0;
    if (!v || IsBadReadPtr(v, 32)) return;
    unsigned char *b = v[1], *e = v[2];
    for (int i = 0; b && e > b && b + (i + 1) * (wide ? 2 : 1) <= e && i < 8; i++)
        snprintf(out + strlen(out), cap - strlen(out), "%s%d", i ? "," : "", wide ? *(short *)(b + 2 * i) : b[i]);
}
// Play state in the player-data manager *(0x3c078d0): +0xd8 current play mode (0 hub, 1 the hub's
// sparring set-up when gm+0xaf0 asks for it, 2 expedition, 4 title list), +0xd9 the requested one;
// +0xda/+0xdb (context vt+0x198), +0xdc/+0xdd (context mode: 2 multiplayer) current/requested;
// +0xde. RequestMoveMap (0xc27870) copies requested to current when they differ (0x202160, 0x202290,
// 0x2022b0, 0x202230). Equipment (packet 0xc) is sent only with +0xdc == 2 (0xddfca0, 0xde20e0).
// The current expedition as 0x6b06f0 sets it from the generator's result: +0xb4c pattern, +0xb50,
// +0xb58, +0xb60, +0xb64; and the result itself +0xb48 / +0xb54 / +0xb5c.
static void expedition_log(const char *who) {
    char *gm = *(char **)(g_base + 0x3c13258);
    if (gm) logf("%s: expedition: result %x,%x,%x | current +b4c %x +b50 %x +b58 %x +b60 %x +b64 %u", who,
                 *(unsigned *)(gm + 0xb48), *(unsigned *)(gm + 0xb54), *(unsigned *)(gm + 0xb5c), *(unsigned *)(gm + 0xb4c),
                 *(unsigned *)(gm + 0xb50), *(unsigned *)(gm + 0xb58), *(unsigned *)(gm + 0xb60), (unsigned char)gm[0xb64]);
}
static void play_state_log(const char *who) {
    char *pd = *(char **)(g_base + 0x3c078d0), *gm = *(char **)(g_base + 0x3c13258);
    if (!pd) return;
    logf("%s: play state: mode %u (req %u) +da %u (req %u) +dc %u (req %u) +de %u +df %u +e0 %u | gm+0xaf0 %u", who,
         (unsigned char)pd[0xd8], (unsigned char)pd[0xd9], (unsigned char)pd[0xda], (unsigned char)pd[0xdb],
         (unsigned char)pd[0xdc], (unsigned char)pd[0xdd], (unsigned char)pd[0xde], (unsigned char)pd[0xdf],
         (unsigned char)pd[0xe0], gm ? (unsigned char)gm[0xaf0] : 0);
}
static void list_state_log(const char *who) {
    char *ctx = g_qm_ctrl && !IsBadReadPtr(g_qm_ctrl + 0xa8, 8) ? *(char **)(g_qm_ctrl + 0xa8) : NULL;
    char *gm = *(char **)(g_base + 0x3c13258), *sm = *(char **)(g_base + 0x3c21ec8);
    char *sy = sm ? *(char **)(sm + 0x1b8) : NULL;
    logf("%s: state: context mode %d expected %d | gm+0xb40 %d | controller +0x10d %d | sync +0xe5 %d",
         who, ctx && !IsBadReadPtr(ctx, 0xa0) ? (unsigned char)ctx[0x9d] : -1, ctx && !IsBadReadPtr(ctx, 0xa0) ? *(int *)(ctx + 0x98) : -1,
         gm ? (unsigned char)gm[0xb40] : -1, g_qm_ctrl ? (unsigned char)g_qm_ctrl[0x10d] : -1,
         sy && !IsBadReadPtr(sy, 0xe8) ? (unsigned char)sy[0xe5] : -1);
    play_state_log(who);
}
static void diff_log(const char *what, const unsigned *before, const unsigned *after, unsigned words, unsigned base) {
    char text[600] = "";
    int n = 0;
    for (unsigned i = 0; i < words && n < 40; i++)
        if (before[i] != after[i])
            snprintf(text + strlen(text), sizeof text - strlen(text), " +%x:%x->%x", base + i * 4, before[i], after[i]), n++;
    logf("generate: %s changed:%s", what, n ? text : " nothing");
}
static void ctx_log(const char *who) {
    char *ctx = g_qm_ctrl && !IsBadReadPtr(g_qm_ctrl + 0xa8, 8) ? *(char **)(g_qm_ctrl + 0xa8) : NULL;
    if (!ctx || IsBadReadPtr(ctx, 0x190)) {
        logf("%s: context: none", who);
        return;
    }
    logf("%s: context: mode %u rejoin %u expected %d +c0 %u | +17c %u +17d %u +17e %u +17f %u +180 %u", who,
         (unsigned char)ctx[0x9d], (unsigned char)ctx[0x9c], *(int *)(ctx + 0x98), (unsigned char)ctx[0xc0],
         (unsigned char)ctx[0x17c], (unsigned char)ctx[0x17d], (unsigned char)ctx[0x17e], (unsigned char)ctx[0x17f],
         (unsigned char)ctx[0x180]);
}
static void generate_hook(unsigned long long a1, unsigned long long a2, unsigned long long a3, unsigned long long a4,
                          void **v1, void **v2, void **v3, void **v4, unsigned long long a9) {
    char t1[64], t2[64], t3[64], t4[64];
    vec_text(t1, sizeof t1, v1, 1), vec_text(t2, sizeof t2, v2, 1), vec_text(t3, sizeof t3, v3, 0), vec_text(t4, sizeof t4, v4, 0);
    char *gm = *(char **)(g_base + 0x3c13258);
    logf("generate (%s): args %u %u %u %u | [%s] [%s] [%s] [%s] | %u; start block before %08x", g_generate_where,
         (unsigned)(a1 & 0xff), (unsigned)(a2 & 0xff), (unsigned)(a3 & 0xff), (unsigned)(a4 & 0xff), t1, t2, t3, t4,
         (unsigned)(a9 & 0xff), gm ? *(unsigned *)(gm + 0xa74) : 0);
    ctx_log("generate");
    play_state_log("generate");
    static unsigned gm_before[0x100], pa_before[0x1c0];  // gm +0xa00..+0xe00, play area +0..+0x700
    char *pa = *(char **)(g_base + 0x3c13158);
    if (gm) memcpy(gm_before, gm + 0xa00, sizeof gm_before);
    if (pa) memcpy(pa_before, pa, sizeof pa_before);
    // map-test-control / two-regions: the generator draws the pattern itself (a forced gm +0xb48 left items
    // unpickable, runs 30..36). Its region argument v3 (rare map layer, u8: the region the player chose)
    // becomes the Mountaintop (1); the launcher left a weight only on the Mountaintop sets, so one of the
    // Mountaintop patterns is drawn (two-regions: with the Rotted Woods added by the launcher).
    // map-region (test): the region digit asked for (1..5); 9 (plain sets) leaves the argument alone, plain
    // sets are always candidates. The launcher left weights only on that region's sets.
    // one-region (measurement only): that region's digit; the launcher left weights only on its sets.
    int want = g_settings[S_ONE_REGION] >= 1 && g_settings[S_ONE_REGION] <= 5 ? g_settings[S_ONE_REGION]
               : g_settings[S_PAIR] == 2 || g_settings[S_PAIR] == 3 ? 2 : 1;  // decision 35: Mountaintop / plain sets; pairs 2 / 3: the Crater
    if (want && g_join != 4 && v3 && !IsBadReadPtr(v3, 32)) {
        unsigned char *b = v3[1], *e = v3[2], *cap = v3[3];
        if (b && e > b) {
            for (unsigned char *q = b; q < e; q++) *q = (unsigned char)want;
            logf("map test: generator region argument set to %d", want);
        } else if (b && e == b && cap > e) {
            *e = (unsigned char)want, v3[2] = e + 1;
            logf("map test: generator region argument was empty; %d added", want);
        } else
            logf("map test: generator region argument not usable; the draw stays the game's own");
    }
    ((void (*)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, void *, void *, void *,
               void *, unsigned long long))(g_base + RVA_GENERATE))(a1, a2, a3, a4, v1, v2, v3, v4, a9);
    logf("generate (%s): done; start block after %08x", g_generate_where, gm ? *(unsigned *)(gm + 0xa74) : 0);
    if (gm) diff_log("game manager +0xa00..+0xe00", gm_before, (unsigned *)(gm + 0xa00), 0x100, 0xa00);
    if (pa) diff_log("play area +0..+0x700", pa_before, (unsigned *)pa, 0x1c0, 0);
}
static void generate_hook_setup(unsigned long long a1, unsigned long long a2, unsigned long long a3, unsigned long long a4,
                                void **v1, void **v2, void **v3, void **v4, unsigned long long a9) {
    g_generate_where = "SingleSetup 0xb4b9f0";
    generate_hook(a1, a2, a3, a4, v1, v2, v3, v4, a9);
}
static void generate_hook_other(unsigned long long a1, unsigned long long a2, unsigned long long a3, unsigned long long a4,
                                void **v1, void **v2, void **v3, void **v4, unsigned long long a9) {
    g_generate_where = "0xb4cc40";
    generate_hook(a1, a2, a3, a4, v1, v2, v3, v4, a9);
}
// Host: every join answer 0x49 as it is sent (0xdde430 -> 0xdf2380 at 0xdde88b), so a refusal is
// never silent whatever path produced it.
#define RVA_SESSION_SEND_TO 0xdf2380  // (sm, u64 *to, packet, data, size)
static void answer_send_hook(void *sm, unsigned long long *to, int packet, int *data, unsigned long long size) {
    logf("host: join answer 0x%x = %d sent to a member (0 ready, 1 blocked, 2 not expected)", packet, data ? *data : -1);
    ((void (*)(void *, void *, int, void *, unsigned long long))(g_base + RVA_SESSION_SEND_TO))(sm, to, packet, data, size);
}
// Party HUD (health bars of the other players). 0x7da8b0 fills the HUD's seven member slots
// (CSFeMan +0xcf28, 0x98 each, 0x7c4a80) from the party member list at game manager +0xdc8
// (PartyMemberInfo: count +0x14, ids at +0x28 + i*0x30, 0xaed2a0); each id is turned into a ChrIns
// with 0x518f30 (call at 0x7daaa4); no character, the local player, or a -27 join reason leaves the
// slot hidden. The list holds every session player whatever its team, so team 16 alone does not
// hide anything. Here: the invader (our effect on the local player) gets no character for any slot,
// and a host or squad member gets none for a character carrying our effect.
static int our_speffect_active(char *chr);
// Run 09f36c57: hidden both ways until the invader was downed, then shown for good. Two more party
// list readers fill other member displays (0x7d9f80 lookup 0x7da1f9, 0x7dba10 lookup 0x7dbb1f), and
// the effect's state may read inactive while downed; so the invader hides all members for the whole
// invasion (in the host's world), and the host hides every character that has once carried our effect
// in this session.
static int g_join_in_world(void);
// Allies (the menu's "team" choice): the same join, without our team effect, so the joiner keeps the base
// team 2 both machines give a remote player (0x3d77d0) and the game's relation 2 <-> 2 is Friend (table
// 0x14398d410). Each joiner writes its role into its Steam lobby member data (nrpvp_role); a member without one
// counts as an invader, as before. Rules that were "someone is in my session" now ask for an invader in it.
static int g_as_ally;                       // joiner: this join is as an ally
// Party (test setting "party" = a code 1..999, both players the same): the menu's 2- / 3-player expedition is
// gathered over our Steam lobby instead of FromSoftware's matchmaking. Phases below (party code).
enum { PARTY_OFF, PARTY_PENDING, PARTY_GUEST_WAIT, PARTY_HOST_CREATE, PARTY_HOST_STARTED, PARTY_GUEST };
static int g_party, g_party_host, g_party_guest;
static char g_party_code[16];  // the pool key: "p" + hash of base, game version, protocol, password, test salt
static unsigned long long g_party_old_lobby;  // our own last lobby: never taken for another host's
static unsigned long long g_party_lowest;  // the lowest-id lobby with our key in the last party search (not ours)
static int g_in_expedition;
static void party_host_over(void);
#define LOBBY_PARTY_KEY "nrpvp_party"   // a gathering host's party code
#define LOBBY_HP_KEY    "nrpvp_hp"      // the host's HP flask charges (+0xff), once its start value is final
#define ALLY_FLASK_CUT  2                // an ally's charges at arrival = max(ALLY_FLASK_MIN, host's - this)
#define ALLY_FLASK_MIN  3                // owner, 2026-11-17: a summon never has fewer than 3 HP charges
#define LOBBY_GO_KEY    "nrpvp_go"      // "1" once the host's expedition is generated (start block, generator results final)
static int g_invader_in_world(void);        // joined as an invader and in the host's world
static int side_count(int want_allies);     // remote session members that are allies / invaders (not the host)
static int member_no_is_invader(int no);    // a remote session member with this player number is an invader
#define FINISH_GUARD_MS 10000  // ponytail: a guess at death animation + fade; the bleed-out death was never timed
// decision 23: the invader's reward for a won invasion. Provisional amount (the user sets the real one).
#define REWARD_MURK 5000
#define REWARD_MURK_MIN 100    // owner, 2026-11-19: every expedition gives at least this much murk
#define REWARD_MURK_LOST 1000  // a lost invasion (decision 23, updated): the reward screen always has content
static int g_host_killed, g_reward_pending;  // decision 23, defined with join_over
static int g_host_player_no = -1;  // invader: the session host's player number (member +0xd4), -1 unknown
static char *g_hud_invaders[4];
static unsigned long long pin_read_73(void *a, unsigned long long *sender, void *buf);
static unsigned long long pin_read_74(void *a, unsigned long long *sender, void *buf);
static void remote_tags_hook(void *a, void *b, void *c, void *d);
static int squad_count_hook(void);
static void squad_count_tick(void);
static int g_host_switched, g_host_old_dc;
// Who ends a session: every readable call of the session leave (0xdf1180, 44 call sites) and of the
// client's role-1 timeout leave (0xdf5840) is logged with its call site (the return address) and
// arguments, before the game's function runs.
static const unsigned LEAVE_SITES[] = {
    0xaece55, 0xb1a8d5, 0xb1a932, 0xb1aad5, 0xb4c972, 0xb50169, 0xb54f69, 0xb59332, 0xb5944f, 0xb59991, 0xc37d13,
    0xc42c03, 0xdcd3da, 0xdcd54a, 0xdcdb2a, 0xdce6ca, 0xddd06c, 0xdce892, 0xdcebaa, 0xdced7a, 0xdcef2a, 0xdcf0ea,
    0xdcf2ba, 0xdcf46a, 0xdcf62a, 0xdcfc0a, 0xdcfeda, 0xdf17ae, 0xdf2db8, 0xdf339a, 0xdf343c, 0xdf3461, 0xdf5404,
    0xdf695d, 0xdf5898, 0xdf58c0, 0xdf58d5, 0xdfb75b, 0xdfba0b, 0xdfdd1a, 0xdf3855, 0xdf389c, 0xdf3bbb, 0x58f9c1};
static unsigned long long leave_hook(char *sm, unsigned long long arg) {
    unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
    logf("session leave (0xdf1180) called from %x, argument %llu, lobby_state %u role %u", site, arg & 0xff,
         sm ? *(unsigned *)(sm + 0xc) : 0, sm ? *(unsigned *)(sm + 0x10) : 0);
    return ((unsigned long long (*)(char *, unsigned long long))(g_base + 0xdf1180))(sm, arg);
}
static unsigned long long leave_timeout_hook(char *sm, unsigned long long arg) {
    logf("session leave (0xdf5840, the client's 30 s answer timeout) called, argument %llu, lobby_state %u role %u", arg & 0xff,
         sm ? *(unsigned *)(sm + 0xc) : 0, sm ? *(unsigned *)(sm + 0x10) : 0);
    return ((unsigned long long (*)(char *, unsigned long long))(g_base + 0xdf5840))(sm, arg);
}
// Every front-end lookup of a character by id (0x518f30 from the HUD code at 0x7c..-0x7d..). Only the
// party bar panel's lookups hide anything on the host (0x7daaa4, the slot fill, and 0x7ced59, the
// slot draw, every frame: a character that carries our effect);
// on the invader the three party-list displays hide every other player. All other sites are passed
// through and only counted, logged on the invader every 10 s, to name the display that still shows
// the host's bar.
static const unsigned HUD_SITES[] = {0x7c6fe1, 0x7c817b, 0x7c84cb, 0x7c8c74, 0x7ced59, 0x7cf1d2, 0x7d81ac, 0x7d8dc7,
                                     0x7d9242, 0x7d93fb, 0x7d9484, 0x7da1f9, 0x7daaa4, 0x7dae8d, 0x7db44b, 0x7db636, 0x7dbb1f};
#define HUD_SITE_COUNT (sizeof HUD_SITES / sizeof *HUD_SITES)
static unsigned g_hud_calls[HUD_SITE_COUNT], g_hud_hidden[HUD_SITE_COUNT];
// Run 11: the slot the draw loop 0x7ceca0 reads was drawn every frame although every lookup in it
// came back empty and the panel's slots at CSFeMan +0xcf28 were cleared: the draw does not stop at an
// empty lookup, it renders the slot (0x7cfe40, call at 0x7cef6b) with the slot's text and an empty bar.
// The lookup at 0x7ced59 receives the slot's id (slot +0x50, rsi); the render call gets rsi + 0x10 as
// its fourth argument and the element (rdi + 0xa8, whose visible word is at rdi - 0xa8) as its third.
// For a slot whose lookup we emptied, the render is skipped and the element's visible word cleared,
// as the loop itself does for a slot with no flags (0x7c3940 writes 0 there first).
static unsigned long long *g_hud_skip;
static unsigned g_hud_renders, g_hud_render_skips;
static void hud_render_hook(char *fe, unsigned long long frame, char *element, char *slot_text) {
    g_hud_renders++;
    if (g_hud_skip && slot_text == (char *)g_hud_skip + 0x10) {
        g_hud_render_skips++;
        *(unsigned short *)(element - 0x150) = 0;
        g_hud_skip = NULL;
        return;
    }
    ((void (*)(char *, unsigned long long, char *, char *))(g_base + 0x7cfe40))(fe, frame, element, slot_text);
}
static char *hud_lookup_hook(void *wcm, unsigned long long *id) {
    unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
    char *c = ((char *(*)(void *, unsigned long long *))(g_base + 0x518f30))(wcm, id);
    unsigned k = 0;
    while (k < HUD_SITE_COUNT && HUD_SITES[k] != site) k++;
    if (k < HUD_SITE_COUNT) g_hud_calls[k]++;
    char *me = wcm ? *(char **)((char *)wcm + 0x174e8) : NULL;
    int party = site == 0x7daaa4 || site == 0x7da1f9 || site == 0x7dbb1f || site == 0x7ced59;
    // 0x7ceca0 (lookup 0x7ced59) draws the party panel from the seven slots 0x7da8b0 fills (CSFeMan
    // +0xcf28, 0x98 each): a slot is drawn while its +0 (shown) or +0x90 (kept) flag is set, so hiding
    // the lookup in 0x7da8b0 alone left a kept slot on screen. On the invader both flags go to 0.
    if (site == 0x7ced59 && (g_invader_in_world() || (me && !IsBadReadPtr(me, 0x200) && our_speffect_active(me) == 1))) {
        char *fe = *(char **)(g_base + 0x3c14a70);
        if (fe && !IsBadReadPtr(fe + 0xcf28, 7 * 0x98))
            for (int i = 0; i < 7; i++) fe[0xcf28 + i * 0x98] = 0, fe[0xcf28 + i * 0x98 + 0x90] = 0;
    }
    char *out = c;
    // name-test (run 19, player tag on the invader): the tag list builder 0x7c8a90 looks up each of its 8
    // tag entries (FE +0xca60, 0x70 each; the handle at +0) here. One line per entry when its handle or the
    // lookup result changes: handle, found or not, team, our effect, and the entry's bytes +0x20..+0x38.
    if (site == 0x7c8c74 && 1 && id) {
        char *fe = *(char **)(g_base + 0x3c14a70);
        long long idx = fe ? ((char *)id - (fe + 0xca60)) / 0x70 : -1;
        static unsigned long long last_h[8];
        static char *last_c[8];
        if (idx >= 0 && idx < 8 && (last_h[idx] != *id || last_c[idx] != c)) {
            last_h[idx] = *id, last_c[idx] = c;
            unsigned char team = 0;
            if (c && !IsBadReadPtr(c, 0x200)) ((unsigned char *(*)(void *, unsigned char *))(g_base + 0x3d1b30))(c, &team);
            char hex[0x18 * 2 + 1] = "";
            if (!IsBadReadPtr((char *)id + 0x20, 0x18))
                for (int k = 0; k < 0x18; k++) sprintf(hex + 2 * k, "%02x", ((unsigned char *)id)[0x20 + k]);
            logf("name test: tag %lld: handle %llx, character %s (team %u, our effect %d), entry +20: %s", idx, *id,
                 !c ? "not found" : c == me ? "(local)" : "found", team, c && !IsBadReadPtr(c, 0x200) ? our_speffect_active(c) : -1, hex);
        }
        // Run 20: the invader's tag for the host has +0x35 = 0x7f, the host's tag for the invader 0x00, and the
        // invader's tag shows no name or rescue gauge. Logged on change for every found entry, both sides;
        // on the invader the host's entry gets +0x35 = 0 each frame (experiment), logged once.
        static unsigned char last_35[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        if (idx >= 0 && idx < 8 && c && c != me && !IsBadReadPtr(c, 0x200)) {
            unsigned char b35 = ((unsigned char *)id)[0x35];
            if (b35 != last_35[idx]) {
                last_35[idx] = b35;
                unsigned char tm = 0;
                ((unsigned char *(*)(void *, unsigned char *))(g_base + 0x3d1b30))(c, &tm);
                logf("name test: tag %lld: byte +0x35 now %02x (team %u, state %d)", idx, b35, tm, !IsBadReadPtr(c, 0x740) ? (unsigned char)c[0x710] : -1);
            }
            if (g_invader_in_world()) {
                char num[8];
                int pn = *(int *)((char *(*)(void *, void *))(g_base + 0x66c6f0))(c, num);
                if (pn == (g_host_player_no >= 0 ? g_host_player_no : 1) && b35) {
                    static int told;
                    if (!told) told = 1, logf("name test: host's tag %lld: byte +0x35 %02x -> 00 (experiment, each frame)", idx, b35);
                    ((unsigned char *)id)[0x35] = 0;
                }
            }
        }
    }
    // name-test, host: the over-head name tags (0x7c8a90 fills eight tags at FE +0xca60, lookup 0x7c8c74;
    // an empty lookup builds no name) are left out for a character carrying our effect; the name over
    // its health bar stays.
    // (run 15: an empty lookup here drops the whole tag, the HP bar with it: 0x7c8a90 fills a tag's HP,
    // name and render only for a found character. Not filtered any more; the over-head name comes back.)
    if (c && c != me && party) {
        int invading = g_invader_in_world() || (me && !IsBadReadPtr(me, 0x200) && our_speffect_active(me) == 1);
        if (invading) out = NULL;
        else if (site == 0x7daaa4 || site == 0x7ced59) {
            for (int i = 0; i < 4; i++)
                if (g_hud_invaders[i] == c) out = NULL;
            if (out && !IsBadReadPtr(c, 0x200) && our_speffect_active(c) == 1) {
                for (int i = 0; i < 4; i++)
                    if (!g_hud_invaders[i]) {
                        g_hud_invaders[i] = c;
                        logf("party HUD: character %p carries our team effect: no bar for it in this session", (void *)c);
                        break;
                    }
                out = NULL;
            }
        }
    }
    if (!out && c && k < HUD_SITE_COUNT) g_hud_hidden[k]++;
    if (site == 0x7ced59) {
        // the draw goes on for this frame with no character (name and empty bar): the slot's render
        // call is skipped below, and its flags are cleared in the slot the draw reads (id - 0x50)
        g_hud_skip = !out && c ? id : NULL;
        if (g_hud_skip) ((char *)id)[-0x50] = 0, ((char *)id)[0x40] = 0;
    }
    if (g_join_in_world() || g_host_switched) {
        static DWORD next;
        if ((int)(GetTickCount() - next) >= 0) {
            next = GetTickCount() + 10000;
            char text[600] = "";
            for (unsigned i = 0; i < HUD_SITE_COUNT; i++)
                if (g_hud_calls[i])
                    snprintf(text + strlen(text), sizeof text - strlen(text), " %x:%u/%u", HUD_SITES[i], g_hud_calls[i], g_hud_hidden[i]);
            logf("party HUD (%s): lookups per site calls/hidden:%s | slot renders %u, skipped %u", g_host_switched ? "host" : "invader",
                 text, g_hud_renders, g_hud_render_skips);
            g_hud_renders = g_hud_render_skips = 0;
            memset(g_hud_calls, 0, sizeof g_hud_calls), memset(g_hud_hidden, 0, sizeof g_hud_hidden);
        }
    }
    return out;
}
// Who sets controller step 12 (SingleStart): the controller's set-step (vtable 0x2c76bd8 entry 4) is
// wrapped in the vtable itself (a data entry, like the step table), and a request for step 12 is
// logged with its return address. The game's own function then runs unchanged.
static int g_invasion_pending;
static void (*g_set_step_original)(void *, int);
static void set_step_wrap(void *ctrl, int step) {
    if (step == 12)
        logf("controller: set-step(12) called from %x (a start request seen: %d)",
             (unsigned)((char *)__builtin_return_address(0) - g_base), g_invasion_pending);
    g_set_step_original(ctrl, step);
}
static void hook_set_step(void) {
    void **entry = (void **)(g_base + 0x2c76bd8) + 4;
    if (*entry != (void *)(g_base + 0xb5d840)) {
        logf("controller: set-step vtable entry not as expected, not wrapped");
        return;
    }
    DWORD old;
    if (!VirtualProtect(entry, 8, PAGE_READWRITE, &old)) return;
    g_set_step_original = (void (*)(void *, int))*entry;
    *entry = (void *)set_step_wrap;
    VirtualProtect(entry, 8, old, &old);
    logf("controller: set-step wrapped (requests for SingleStart are logged)");
}
// The two tail jumps into the leave (not calls, so not in LEAVE_SITES): 0xdf19c0 (a session manager
// method: leaves when the Steam implementation's flag 8 at +0xc70 is clear and the quick-match
// controller is in steps 1-11, or idle with its +0x108 set, 0xb66fe0) and 0xdf2910 (role 8).
static int g_qm_step;
static unsigned long long leave_tail_19c0(char *sm, unsigned long long arg) {
    char *impl = sm ? *(char **)(sm + 0x60) : NULL;
    logf("session leave (0xdf1180) by tail jump from 0xdf19c0 (caller %x), argument %llu, lobby_state %u role %u | impl +0xc70 %x, controller step %d +0x108 %u",
         (unsigned)((char *)__builtin_return_address(0) - g_base), arg & 0xff, sm ? *(unsigned *)(sm + 0xc) : 0,
         sm ? *(unsigned *)(sm + 0x10) : 0, impl && !IsBadReadPtr(impl + 0xc70, 4) ? *(unsigned *)(impl + 0xc70) : 0, g_qm_step,
         g_qm_ctrl ? (unsigned char)g_qm_ctrl[0x108] : 0xff);
    return ((unsigned long long (*)(char *, unsigned long long))(g_base + 0xdf1180))(sm, arg);
}
static unsigned long long leave_tail_2910(char *sm, unsigned long long arg) {
    logf("session leave (0xdf1180) by tail jump from 0xdf2910 (role 8, every member waiting), caller %x, lobby_state %u",
         (unsigned)((char *)__builtin_return_address(0) - g_base), sm ? *(unsigned *)(sm + 0xc) : 0);
    return ((unsigned long long (*)(char *, unsigned long long))(g_base + 0xdf1180))(sm, arg);
}
static void hook_jump(unsigned rva_site, unsigned rva_target, void *hook, const char *what) {
    unsigned char *site = (unsigned char *)g_base + rva_site;
    int expected = (int)(rva_target - (rva_site + 5));
    unsigned char *stub = near_alloc(12);
    if (site[0] != 0xE9 || memcmp(site + 1, &expected, 4) || !stub) {
        logf("%s: jump site %x does not look as expected, not hooked", what, rva_site);
        return;
    }
    stub[0] = 0x48; stub[1] = 0xB8;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 1, site + 5, stub)) logf("%s: hooked (%x)", what, rva_site);
}
// finish-test: a downed player whose rescue gauge is emptied by the other side dies instead of standing
// up (decisions 3 and 16). The downed state lives on the character: +0x710 state (0 none, 1 down,
// 2 revived), +0x714 gauge (float, required GameSystemCommonParam +0x380/+0x384/+0x388 by down count),
// +0x718 bleed-out timer, +0x728 handle of the character whose hit emptied the gauge (0x66dec0 writes
// attacker+8 there when the gauge reaches 0), +0x730 down count, +0x731/+0x732 "still down" / "stand
// up" set every frame by the behaviour (0x40d390 cases 0x90/0x91). Hits lower the gauge on the downed
// player's own machine (0x43d260, behind 0x3d3fe0); the stand-up is decided there too: 0x674eb0 calls
// 0x66f3b0(chr, 2) at 0x67516f (state 2, HP back) and sends event 2 to the others at 0x6751e1. Here,
// for our local player when (a) it carries our effect (a downed invader) or (b) the character that
// emptied its gauge carries our effect (an invader finishing a squad member), the stand-up is refused
// and the bleed-out timer is set to 0, so the behaviour takes its bleed-out death; event 2 is not
// sent, so the others see no revive. Every other revive is passed through unchanged.
static char *g_finish_chr;
static DWORD g_finish_tick;
static int g_finish_invader;
static char *local_player(void);
static int finish_wanted(char *chr, char **by) {
    *by = NULL;
    if (!chr || chr != local_player() || IsBadReadPtr(chr, 0x740)) return 0;
    if (our_speffect_active(chr) == 1) return 1;
    unsigned long long h = *(unsigned long long *)(chr + 0x728);
    char *wcm = *(char **)(g_base + 0x3c0f0a8);
    if (h == ~0ull || !wcm) return 0;
    char *a = ((char *(*)(void *, unsigned long long *))(g_base + 0x518f30))(wcm, &h);
    *by = a;
    return a && a != chr && !IsBadReadPtr(a, 0x200) && our_speffect_active(a) == 1 ? 2 : 0;
}
static void finish_revive_hook(char *chr, char state, float t) {
    char *by;
    int why = state == 2 ? finish_wanted(chr, &by) : 0;
    if (!why) {
        ((void (*)(char *, char, float))(g_base + 0x66f3b0))(chr, state, t);
        return;
    }
    if (g_finish_chr != chr || !g_finish_tick) {
        logf("finish test: stand-up refused for %s: gauge %.1f, down count %u, bleed-out timer %.2f -> 0, event 2 not sent",
             why == 1 ? "the downed invader (our effect on us)" : "a squad member whose gauge an invader emptied",
             *(float *)(chr + 0x714), (unsigned char)chr[0x730], *(float *)(chr + 0x718));
        g_finish_chr = chr, g_finish_tick = GetTickCount(), g_finish_invader = why == 1;
    }
    *(float *)(chr + 0x718) = 0;
}
static void finish_event_hook(void *msg, char *chr, unsigned long long kind, unsigned long long arg) {
    if (chr && chr == g_finish_chr && g_finish_tick && (unsigned char)kind == 2) return;
    ((void (*)(void *, char *, unsigned long long, unsigned long long))(g_base + 0x4f2d50))(msg, chr, kind, arg);
}
// Every 250 ms while our local player is down (this thread, reads only): the fields above.
static int chr_hp(char *c);
static int ring_on(void);
static void ring_where(const char *why);
static void finish_tick(void) {
    static DWORD next, down_at;
    static float last_timer, last_grace;
    char *chr = local_player();
    if (!chr || IsBadReadPtr(chr, 0x740)) return;
    // Run 65: allies died about 3 s after going down (the hit grace +0x71c is 3.5 s, the timer 15 s). The way out of
    // the down: one line with the last timer and grace seen, every 250 ms read, to see what ends it.
    if (chr[0x710] == 1) {
        if (!down_at) down_at = GetTickCount() | 1;
        last_timer = *(float *)(chr + 0x718), last_grace = *(float *)(chr + 0x71c);
    } else if (down_at) {
        logf("finish test: out of the down after %lu ms: state %u, HP %d, last seen timer %.2f grace %.2f, gauge %.1f (%s)",
             GetTickCount() - down_at, (unsigned char)chr[0x710], chr_hp(chr), last_timer, last_grace, *(float *)(chr + 0x714),
             g_as_ally && g_join == 4 ? "ally" : g_join == 4 ? "invader" : "host");
        down_at = 0;
    }
    if ((int)(GetTickCount() - next) < 0) return;
    if (chr[0x710] != 1 && !(g_finish_tick && chr == g_finish_chr)) return;
    next = GetTickCount() + 250;
    static DWORD ring_next;
    if (ring_on() && (int)(GetTickCount() - ring_next) >= 0) ring_next = GetTickCount() + 1000, ring_where("while down");
    logf("finish test: state %u gauge %.1f timer %.2f grace %.2f HP %d still-down %u stand-up %u down count %u%s", (unsigned char)chr[0x710],
         *(float *)(chr + 0x714), *(float *)(chr + 0x718), *(float *)(chr + 0x71c), chr_hp(chr), (unsigned char)chr[0x731],
         (unsigned char)chr[0x732], (unsigned char)chr[0x730], g_finish_tick && chr == g_finish_chr ? " (stand-up refused)" : "");
    if (chr == g_finish_chr && chr[0x710] != 1) g_finish_tick = 0, g_finish_chr = NULL;
}
// loot-test (logging only): what a killed invader leaves and what the local player can take.
// CSGeomPickUpItemModule::Update (0x724420, called per module from 0x6fd220 with the module at
// rbx+0x540) offers a pickup prompt; taking it runs 0x7247d0(module, action) (call at 0x72464c), which
// gives the item lot. A module: +0 the asset (geometry instance: +8 its handle, +0x18 its parts, whose
// +8 is the param row: +0xb4 the pickup action id), +9/+0xa/+0xb/+0xd state bytes. The equipment
// dialog CS::PickupEquipDialog is built by 0x9827d0 from the menu job 0x85d060 (call at 0x85d0b2: job
// at r8, job+0x108 at r9, job+0x148 on the stack). Every 5 s the set of pickup modules that ran is
// compared with the last one and the differences are logged; each pickup and each dialog is logged.
static void loot_describe(char *module, char *out, size_t cap) {
    char *geom = module && !IsBadReadPtr(module, 0x10) ? *(char **)module : NULL;
    char *row = geom && !IsBadReadPtr(geom, 0x28) && *(char **)(geom + 0x20) && !IsBadReadPtr(*(char **)(geom + 0x20), 0xc0)
                    ? *(char **)(geom + 0x20) : NULL;
    snprintf(out, cap, "module %p asset %p handle %llx action %d (row +b0 %d +b8 %d +bc %d) state %u/%u/%u/%u", (void *)module,
             (void *)geom, geom && !IsBadReadPtr(geom, 0x28) ? *(unsigned long long *)(geom + 8) : 0, row ? *(int *)(row + 0xb4) : -1,
             row ? *(int *)(row + 0xb0) : 0, row ? *(int *)(row + 0xb8) : 0, row ? *(int *)(row + 0xbc) : 0,
             module ? (unsigned char)module[9] : 0, module ? (unsigned char)module[10] : 0, module ? (unsigned char)module[11] : 0,
             module ? (unsigned char)module[13] : 0);
}
#define LOOT_MAX 256
static char *g_loot_seen[LOOT_MAX], *g_loot_last[LOOT_MAX];
static int g_loot_seen_n, g_loot_last_n;
static int loot_in(char **set, int n, char *m) {
    for (int i = 0; i < n; i++) if (set[i] == m) return 1;
    return 0;
}
static void loot_update_hook(char *module) {
    if (g_loot_seen_n < LOOT_MAX && !loot_in(g_loot_seen, g_loot_seen_n, module)) g_loot_seen[g_loot_seen_n++] = module;
    ((void (*)(char *))(g_base + 0x724420))(module);
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 5000;
    int shown = 0;
    char text[200];
    for (int i = 0; i < g_loot_seen_n; i++)
        if (!loot_in(g_loot_last, g_loot_last_n, g_loot_seen[i]) && shown++ < 16) {
            loot_describe(g_loot_seen[i], text, sizeof text);
            logf("loot test: pickup object appeared: %s", text);
        }
    for (int i = 0; i < g_loot_last_n; i++)
        if (!loot_in(g_loot_seen, g_loot_seen_n, g_loot_last[i]) && shown++ < 32)
            logf("loot test: pickup object gone: module %p", (void *)g_loot_last[i]);
    if (shown) logf("loot test: %d pickup objects running now (%d changes)", g_loot_seen_n, shown);
    memcpy(g_loot_last, g_loot_seen, sizeof g_loot_last);
    g_loot_last_n = g_loot_seen_n, g_loot_seen_n = 0;
}
static void loot_pickup_hook(char *module, float value) {  // second argument goes in xmm1 (movss at 0x724643)
    char text[200];
    loot_describe(module, text, sizeof text);
    unsigned bits;
    memcpy(&bits, &value, 4);
    static char *last_module;  // run 12: called every frame while a pickup runs; one line per object
    static DWORD last_tick;
    if (module != last_module || GetTickCount() - last_tick > 3000)
        logf("loot test: pickup taken: %s, second argument %08x", text, bits);
    last_module = module, last_tick = GetTickCount();
    ((void (*)(char *, float))(g_base + 0x7247d0))(module, value);
}
static void *loot_dialog_hook(void *a, void *b, char *job, char *j108, char *j148) {
    char hex[3][100];
    char *p[3] = {job, j108, j148};
    for (int k = 0; k < 3; k++) {
        hex[k][0] = 0;
        if (p[k] && !IsBadReadPtr(p[k], 32))
            for (int i = 0; i < 32; i += 4) snprintf(hex[k] + strlen(hex[k]), sizeof hex[k] - strlen(hex[k]), "%08x ", *(unsigned *)(p[k] + i));
    }
    void *r = ((void *(*)(void *, void *, char *, char *, char *))(g_base + 0x9827d0))(a, b, job, j108, j148);
    logf("loot test: equipment pickup dialog built (%p): job %s| +108 %s| +148 %s", r, hex[0], hex[1], hex[2]);
    return r;
}
// Decision 20 (finish-test). On the downed player's own machine:
// - going down (0x66f3b0(chr, 1), call at 0x674f37 in 0x674eb0) sets the gauge from the down count
//   +0x730 (GameSystemCommonParam +0x380/+0x384/+0x388); the gauge refills only up to the count's value,
//   so a full gauge needs count 3 as well. Who downed the player is not on the character at that moment
//   (+0x728 is reset to -1 there), so: the invader always (it has no team-mates), the host whenever an
//   invader has been admitted to its session.
// - a hit lowers the gauge in 0x66dec0 (call at 0x43dac3 in the hit handler 0x43d260, with the attacker).
//   At 0 the behaviour asks to stand up. When the other side empties it, the gauge is left at 0.5 (shown
//   as 1, so no stand-up) and the bleed-out timer +0x718 is set to 0: the bleed-out death the host showed
//   in run 12 (timer 0 with a non-empty gauge: death animation, state 1 -> 0).
static float gscp_float(unsigned off) {
    char tmp[16];
    char **p = ((char **(*)(void *))(g_base + 0xe76d50))(tmp);
    return p && *p ? (float)*(int *)(*p + off) : 0;
}
static int g_invader_downs;  // the invader's downs in this invasion (decision 22), reset at its end
static float gscp_u16(unsigned off) {
    char tmp[16];
    char **p = ((char **(*)(void *))(g_base + 0xe76d50))(tmp);
    return p && *p ? (float)*(unsigned short *)(*p + off) : 0;
}
// The invader's reward for finishing one of the host's summons (the user's rule; recipient to be confirmed: kept in
// summon_kill_reward). Who finished a downed player is known only on that player's own machine: the rescue gauge
// is lowered there (0x66dec0, our finish_hit_hook; the gauge calls on the attacker's machine are never for a remote
// character, logs), and an empty gauge by the other side is where we end the down (+0x728 = the hitter's handle,
// timer 0 -> death). So the summon's machine names the finisher in its Steam lobby member data by its session
// player number only (nrpvp_finished = "<player number>,<count>": no Steam id or name in lobby data); the numbers
// are the same on every machine of the session (member +0xd4: host 1, joiners 2.. in every log of both sides) and
// differ between two invaders. An invader whose own number (its own member's +0xd4) is named, with a count it has
// not seen since it arrived, gets REWARD_RUNES_SUMMON_KILL through the game's rune add (0x2098c0, player data
// +0x6c), outside the one-tenth rule. Once per finish; with two invaders only the named one.
#define LOBBY_FINISHED_KEY "nrpvp_finished"
#define REWARD_RUNES_SUMMON_KILL 10000
static void summon_finished_by(char *attacker);
static void summon_kill_reward_give(unsigned long long summon);  // the one place that decides who gets it and how much
static int finish_side_ok(char *chr) {  // a down of this local character counts as by the other side
    return chr && chr == local_player() && (our_speffect_active(chr) == 1 || side_count(0) > 0);
}
static void finish_down_hook(char *chr, char state, float t) {
    ((void (*)(char *, char, float))(g_base + 0x66f3b0))(chr, state, t);
    if (state != 1 || chr != local_player() || IsBadReadPtr(chr, 0x740) || chr[0x710] != 1) return;
    if (!finish_side_ok(chr)) {
        logf("finish test: went down (count %u, gauge %.1f): left to the game (no invader in the session)", (unsigned char)chr[0x730],
             *(float *)(chr + 0x714));
        return;
    }
    // Decision 22. The timer +0x718 (seconds, float) is set here by the game from GameSystemCommonParam
    // +0x2f4 and only counted down by 0x674eb0 (frame time, after the hit grace +0x71c), so one write
    // holds (except under SpEffect 102120, which resets it to the param every frame). The gauge refills
    // in the down at +0x38c.. % of its cap per second, capped by the value for the count +0x730, so the
    // count is set to match. The game lowers +0x730 by one every +0x2ca seconds out of the down (+0x720).
    float was = *(float *)(chr + 0x714), timer_was = *(float *)(chr + 0x718);
    unsigned count = (unsigned char)chr[0x730];
    if (our_speffect_active(chr) == 1) {
        int n = ++g_invader_downs;
        if (n >= 4) {
            *(float *)(chr + 0x714) = 0.5f, *(float *)(chr + 0x718) = 0;
            logf("finish test: invader down %d (game count %u): the last chance was the third: gauge 0.5, timer %.2f -> 0 (death)", n, count,
                 timer_was);
            return;
        }
        static const unsigned off[3] = {0x380, 0x384, 0x388};
        static const float secs[3] = {5, 10, 15};
        float g = gscp_float(off[n - 1]);
        if (g > 0) chr[0x730] = (char)n, *(float *)(chr + 0x714) = g;
        *(float *)(chr + 0x718) = secs[n - 1];
        logf("finish test: invader down %d (game count %u -> %d): gauge %.1f -> %.1f, timer %.2f -> %.0f s", n, count, n, was,
             *(float *)(chr + 0x714), timer_was, secs[n - 1]);
        return;
    }
    float full = gscp_float(0x388);
    if (full > 0) chr[0x730] = 3, *(float *)(chr + 0x714) = full;
    *(float *)(chr + 0x718) = 15;
    logf("finish test: host down with an invader in the session: count %u -> 3, gauge %.1f -> %.1f, timer %.2f -> 15 s "
         "(count decay every %.0f s out of the down)", count, was, *(float *)(chr + 0x714), timer_was, gscp_u16(0x2ca));
}
static void finish_hit_hook(char *chr, char *attacker, char *hit) {
    float before = chr && !IsBadReadPtr(chr, 0x740) ? *(float *)(chr + 0x714) : -1;
    ((void (*)(char *, char *, char *))(g_base + 0x66dec0))(chr, attacker, hit);
    // revive-hits (test setting): 0x66dec0 lowers the gauge by the attack's rescue amount (hit +0x150, times
    // hit +0x258, times GameSystemCommonParam +0x40c.. by down count in one mode), whoever hits. Here, on the
    // downed player's own machine and only in an invasion, a hit that counted takes a fixed share of the
    // gauge's cap instead (cap = +0x380 / +0x384 / +0x388 by down count +0x730): the other side 0.55 (two hits
    // finish), the own side 0.2 (five hits raise). Between hits the gauge refills at +0x38c.. % of its cap per
    // second once the hit grace (+0x71c, GameSystemCommonParam +0x2f8) is over (0x674eb0).
    if (setting(S_REVIVE_HITS) && chr && chr == local_player() && !IsBadReadPtr(chr, 0x740) && chr[0x710] == 1 && before > 0 &&
        *(float *)(chr + 0x714) < before && attacker && attacker != chr && !IsBadReadPtr(attacker, 0x200)) {
        int victim_invader = our_speffect_active(chr) == 1, attacker_invader = our_speffect_active(attacker) == 1;
        if (victim_invader || side_count(0) > 0) {
            int other = victim_invader ? !attacker_invader : attacker_invader;
            unsigned count = (unsigned char)chr[0x730];
            float cap = gscp_float(count <= 1 ? 0x380 : count == 2 ? 0x384 : 0x388), left = before - cap * (other ? 0.55f : 0.2f);
            // A reviving ultimate (AtkParam_Pc reviveDamageCorrection 9999: Guardian's dive / landing, 700000801 / 807;
            // 4000: Recluse; ordinary ultimates 225, skills up to 600): its rescue amount (hit +0x150 times +0x258, as
            // 0x66dec0 takes it) is taken as such when it reaches the full gauge of a third down (+0x388, 240).
            // ponytail: threshold from the data's ratios, not from a measured hit scale; the log line shows the amount.
            float amount = *(float *)(hit + 0x150) * *(float *)(hit + 0x258), full3 = gscp_float(0x388);
            int ult = full3 > 0 && amount >= full3;
            if (ult) left = 0;
            if (cap > 0) {
                logf("revive hits: %s hit on the downed %s (%s): gauge %.1f -> %.1f (game %.1f, cap %.0f, hit amount %.1f)",
                     other ? "other side's" : "own side's", victim_invader ? "invader" : "host",
                     ult ? (other ? "reviving ultimate: finished at once" : "reviving ultimate: raised at once") : other ? "2-hit rule" : "5-hit rule",
                     before, left > 0 ? left : 0, *(float *)(chr + 0x714), cap, amount);
                *(float *)(chr + 0x714) = left > 0 ? left : 0;
                // as 0x66dec0 does: the hitter's handle (+8) goes to +0x728 when the gauge reaches 0, else -1
                *(unsigned long long *)(chr + 0x728) = left > 0 ? 0xffffffffffffffffull : *(unsigned long long *)(attacker + 8);
            }
        }
    }
    logf("finish test: gauge call (0x43dac3): %s character, attacker %p (ours %d), gauge %.1f -> %.1f", chr == local_player() ? "local" : "remote",
         (void *)attacker, attacker && !IsBadReadPtr(attacker, 0x200) ? our_speffect_active(attacker) : -1, before,
         chr ? *(float *)(chr + 0x714) : -1);
    if (!chr || chr != local_player() || IsBadReadPtr(chr, 0x740) || chr[0x710] != 1 || *(float *)(chr + 0x714) > 0) return;
    int ok_attacker = attacker && attacker != chr && !IsBadReadPtr(attacker, 0x200);
    int by_other = our_speffect_active(chr) == 1 ? ok_attacker && our_speffect_active(attacker) != 1
                                                 : side_count(0) > 0 && ok_attacker && our_speffect_active(attacker) == 1;
    if (!by_other) {
        logf("finish test: gauge emptied by a team-mate: left to the game (stand-up)");
        return;
    }
    float timer = *(float *)(chr + 0x718);
    if (timer <= 0) {  // run 14: a second hit after the finish logged it again; already finished
        *(float *)(chr + 0x714) = 0.5f;
        return;
    }
    *(float *)(chr + 0x714) = 0.5f;
    *(float *)(chr + 0x718) = 0;
    logf("finish test: emptied by the other side: gauge 0 -> 0.5 (no stand-up), timer %.2f -> 0 (bleed-out death)", timer);
    if (g_as_ally && g_join == 4) summon_finished_by(attacker);
}
// Task 2 (finish-test). Whether an attack may hit a character is decided on the attacker's machine in
// 0x436370 (called from the hit dispatch 0x43ed80 before the target's damage module, slot 7 = 0x437010,
// registers the hit): 0x52cb90(attacker, target, hit) asks the ATTACK relation table at 0x398d410
// (81 x 81, raw effective teams, no 77 -> 2 conversion) with the attack's own flags (AtkParam byte
// +0x81: bit 0 enemies, bit 1 friends, bit 2 neutral). There 1/2 -> 77 (a squad member against a
// downed player) is Enemy, so the host's blows land on a downed invader; 16 -> 77 (our invader against
// a downed player) is Neutral, so its blows never land on a downed host (run 16: no hit registered at
// all). Here [16][77] of that table gets the Enemy object of [2][77]. Only team 16, our invader, attacks
// with that row. (Runs 13-16 changed the same entry of the other table, 0x399a120, the "is it an
// enemy" table of the HUD and the hit-source checks, which this path does not ask.)
// ult-pvp (test setting, percent 0..999; 0 and 100 = unchanged, above 100 stronger): the player damage module's hit receive (vtable
// CSPlayerDamageModule 0x2c11d00 slot 7, 0x440fb0: module, attacker, hit) hands the hit to 0x436530; there the
// damage is applied either directly (HP -= (int) hit +0x144, 0x490320) or through 0x43d260 -> 0x43be60, which
// subtracts hit +0x280 (int) from HP (0x426130 on the data module, HP at **(chr+0x1b8) +0x140). Nothing on that path
// writes +0x280 / +0x144, so both arrive final. The attack row: hit +0x44 = 1 for AtkParam_Pc (param 7 + kind,
// 0x6d1b60 -> 0xe6d630), +0x40 = row id. Ultimates: AtkParam_Pc 700000000-700999999, 790000000-790999999 and the
// Executor's projectile rows. Scaled when the victim and the attacker are players (vfunc +0x118) of different sides
// (our team effect on exactly one of them). debug-log: every such ultimate hit, and the first 10 other
// player-versus-player hits unchanged, with the victim's HP before and after the original call.
#define RVA_PLAYER_DMG_VTABLE 0x2c11d00
#define RVA_PLAYER_HIT_RECV   0x440fb0
static int ult_row(int kind, int id) {
    static const int EXECUTOR[] = {10135, 10175, 10255, 10400, 10405, 10455, 10700};
    if (kind != 1) return 0;
    if ((id >= 700000000 && id < 701000000) || (id >= 790000000 && id < 791000000)) return 1;
    for (unsigned i = 0; i < sizeof EXECUTOR / sizeof *EXECUTOR; i++) if (id == EXECUTOR[i]) return 1;
    return 0;
}
// Bullets: the bullet hit dispatch 0x33c120(hit, victim, bullet) takes the owner from the bullet (0x63f840 on
// bullet +0xb0, a handle) and calls slot 7 with that owner when it is a character (entity type 1; CSBulletIns is
// type 3, CSWorldGeomIns type 6), so an ultimate projectile reaches this hook with its owner as the attacker.
// The Executor's beast form: its melee rows (behaviour variation 3000, AtkParam_Pc 3000xxx) are shared with 160
// weapons, so the form is recognised by its marker SpEffect 707100 ("Is Beast Form") on the attacker instead.
static int speffect_on(char *chr, unsigned id);
#define EXECUTOR_BEAST_SPEFFECT 707100
static int chr_is_player(char *c) {
    return c && !IsBadReadPtr(c, 0x200) && ((char (*)(void *))(*(void ***)c)[0x118 / 8])(c);
}
static int chr_hp(char *c) {
    char **m = c && !IsBadReadPtr(c + 0x1b8, 8) ? *(char ***)(c + 0x1b8) : NULL;
    return m && *m && !IsBadReadPtr(*m + 0x140, 4) ? *(int *)(*m + 0x140) : -1;
}
static void player_hit_hook(char *module, long long *attacker, char *hit) {
    char *victim = module ? ((char *(*)(void *))(g_base + 0x42c120))(module) : NULL;
    char *att = NULL;
    if (attacker && !IsBadReadPtr(attacker, 8) && ((int (*)(void *))(*(void ***)attacker)[0x10 / 8])(attacker) == 1) att = (char *)attacker;
    int pvp = hit && victim && att && att != victim && chr_is_player(victim) && chr_is_player(att) &&
              (our_speffect_active(victim) == 1) != (our_speffect_active(att) == 1);
    int kind = pvp ? *(int *)(hit + 0x44) : -1, id = pvp ? *(int *)(hit + 0x40) : -1;
    int beast = pvp && speffect_on(att, EXECUTOR_BEAST_SPEFFECT), ult = pvp && (ult_row(kind, id) || beast);
    int n = g_settings[S_ULT_PVP], d0 = pvp ? *(int *)(hit + 0x280) : 0, hp0 = pvp ? chr_hp(victim) : 0;
    float f0 = pvp ? *(float *)(hit + 0x144) : 0;
    if (ult && n > 0 && n != 100) *(int *)(hit + 0x280) = (int)((long long)d0 * n / 100), *(float *)(hit + 0x144) = f0 * (float)n / 100.0f;
    ((void (*)(char *, long long *, char *))(g_base + RVA_PLAYER_HIT_RECV))(module, attacker, hit);
    static int others;
    if (pvp && g_debug_log && (ult || others < 10)) {
        others += !ult;
        logf("ult-pvp: %s%s hit on a player: AtkParam_%s %d, damage +0x280 %d -> %d, +0x144 %.1f -> %.1f, victim HP %d -> %d",
             ult ? "ultimate" : "ordinary", beast ? " (Executor beast form)" : "", kind == 1 ? "Pc" : "Npc", id, d0, *(int *)(hit + 0x280), f0, *(float *)(hit + 0x144), hp0,
             chr_hp(victim));
    }
}
// Runes from a kill (read 2026-11-15). Every machine pays the kill itself: 0x3de710 (the dying enemy, once: its
// +500 bit 4) takes NpcParam +0x2c (row via vt+0x1a0) times the enemy's own rune multiplier (0x506e30 on its
// effects) and calls 0x668380(local player, amount, enemy, flag) at 0x3de800; 0x51da30 does the same for a kill
// reported in a session packet (the amount from the packet, before any player's multiplier) at 0x51dc0b.
// 0x668380 rounds the float and calls PlayerIns vt+0x5d0 (0x668300), which multiplies by the player's own rune
// rate (0x508510 on its effects: 99240 soulRate etc.) and adds through 0x2098c0 (player data +0x6c, capped
// 999999999; also +0x70 and +0x110 totals). So the host and the invader start from the same base amount; their
// gains differ only by their own rune multipliers. Other rune sources (items 0x668060, event awards 0x600d20,
// our reward, level-up spending in 0xfdee30) do not pass here. The invader (our team effect on it) gets a
// tenth of the base: floor(base / 10), at least 1 when the base was above 0; then its own multiplier applies.
#define RUNE_KILL_FN 0x668380
#define INVADER_RUNE_SHARE 10  // the invader gets 1 / this of a kill's runes
static char *local_player(void);
static int rune_kill_hook(char *chr, float amount, char *enemy, unsigned char flag) {
    float full = amount;
    int cut = chr && chr == local_player() && amount > 0 && our_speffect_active(chr) == 1;
    if (cut) {
        amount = (float)(int)(amount / INVADER_RUNE_SHARE);
        if (amount < 1) amount = 1;
    }
    int got = ((int (*)(char *, float, char *, unsigned char))(g_base + RUNE_KILL_FN))(chr, amount, enemy, flag);
    if (cut && g_debug_log) logf("runes: invader's kill reward cut to 1/%d: base %.1f -> %.1f, gained %d (own multiplier applied)", INVADER_RUNE_SHARE,
                                 full, amount, got);
    return got;
}
// Flask charges (measurement, pattern 16: 3 charges, churches 101 / 104 cleared): every write of the HP / FP
// charge counts goes through 0x20d9d0(PlayerGameData, hp, fp) (+0xff / +0x100). Each call is logged with
// its site (the call's address, or for the two tail jumps the jump and the caller of the function that
// jumps), the counts before and after and +0x1a8 (charges added in the expedition), whether it is the
// local player's data (GameDataMan +8), at most 200 lines a session. Sites: 0x200ed7 load (CharaInitParam
// +0xfe), 0x20123f / 0x20260e / 0x2027b1 / 0x202a6c / 0x202c94 player-data set-up, 0x5fdafa 2004[86],
// 0x60acb6 (-1, EMEVD), 0xfd912e (0xfd90e0, from 0xfdee30), jumps 0x7ea74c (0x7ea6f0, lambda 0xa02660) and
// 0xfd9112.
static int g_flask_lines;
static void flask_set_logged(unsigned site, unsigned caller, char *pgd, unsigned hp, unsigned fp) {
    char *gdm = *(char **)(g_base + 0x3c078d0), *mine = gdm ? *(char **)(gdm + 8) : NULL;
    unsigned b_hp = pgd ? (unsigned char)pgd[0xff] : 0, b_fp = pgd ? (unsigned char)pgd[0x100] : 0;
    int b_add = pgd ? *(int *)(pgd + 0x1a8) : 0;
    ((void (*)(char *, unsigned, unsigned))(g_base + 0x20d9d0))(pgd, hp, fp);
    if (!pgd || g_flask_lines >= 200) return;
    g_flask_lines++;
    char by[32] = "";
    if (caller) snprintf(by, sizeof by, ", caller %x", caller);
    logf("flasks: set at %x%s (%s): HP %u -> %u (asked %u), FP %u -> %u, added in the expedition (+0x1a8) %d -> %d", site, by,
         pgd == mine ? "local player" : "other player data", b_hp, (unsigned char)pgd[0xff], hp & 0xff, b_fp,
         (unsigned char)pgd[0x100], b_add, *(int *)(pgd + 0x1a8));
    if (site == 0xfd912e && pgd == mine && (unsigned char)pgd[0xff] > b_hp)
        logf("flasks: church charge +%u: HP flask charges now %u", (unsigned char)pgd[0xff] - b_hp, (unsigned char)pgd[0xff]);
}
static void flask_call_hook(char *pgd, unsigned hp, unsigned fp) {
    flask_set_logged((unsigned)((char *)__builtin_return_address(0) - g_base) - 5, 0, pgd, hp, fp);
}
static void flask_jmp_7ea_hook(char *pgd, unsigned hp, unsigned fp) {
    flask_set_logged(0x7ea74c, (unsigned)((char *)__builtin_return_address(0) - g_base), pgd, hp, fp);
}
static void flask_jmp_fd9_hook(char *pgd, unsigned hp, unsigned fp) {
    flask_set_logged(0xfd9112, (unsigned)((char *)__builtin_return_address(0) - g_base), pgd, hp, fp);
}
static void flask_log_install(void) {
    static const unsigned CALLS[] = {0x200ed7, 0x20123f, 0x20260e, 0x2027b1, 0x202a6c, 0x202c94, 0x5fdafa, 0x60acb6, 0xfd912e};
    for (unsigned i = 0; i < sizeof CALLS / sizeof *CALLS; i++) hook_call(CALLS[i], 0x20d9d0, (void *)flask_call_hook, "flasks: charge write log");
    hook_jump(0x7ea74c, 0x20d9d0, (void *)flask_jmp_7ea_hook, "flasks: charge write log (jump)");
    hook_jump(0xfd9112, 0x20d9d0, (void *)flask_jmp_fd9_hook, "flasks: charge write log (jump)");
}
static void rune_install(void) {
    hook_call(0x3de800, RUNE_KILL_FN, (void *)rune_kill_hook, "runes: kill reward (own kill)");
    hook_call(0x51dc0b, RUNE_KILL_FN, (void *)rune_kill_hook, "runes: kill reward (reported kill)");
}
// Packets about another player's character, read every frame by 0x521ff0: 0x2b (16 bytes, loop 0x41c7c0) and
// 0x72 (48 bytes, loop 0x4472a0). Each packet starts with the sender's own character handle (PlayerIns +0x78/+0x7c;
// 0x2b is sent only by the machine whose local player that is, from PlayerIns update 0x675d20 via 0x41c9d0, to
// every member). Both loops look the handle up with 0x519140 and call vt+0x120 on the result without a null
// check: a packet for a character we do not have crashed the 3-player run at 0x41c8bf. When the lookup finds
// nothing we hand back a dummy whose vt+0x120 says no, so the loop drops that packet (its je to the loop head),
// and log the handle, the payload and the ids in our player set (PlayerIns +0x5a0 -> +8, what 0x5197e0 matches
// the handle's low 11 bits against). Nothing else of the dummy is read: both loops stop at vt+0x120.
static unsigned char pkt_no(void *self) { (void)self; return 0; }
static void *pkt_dummy_vt[0x130 / 8];
static void *pkt_dummy = pkt_dummy_vt;
static void *pkt_lookup(void *wcm, unsigned long long *id, int kind) {
    unsigned long long h = id ? *id : 0;  // the lookup releases the handle copy, so read it first
    char *c = ((char *(*)(void *, unsigned long long *))(g_base + 0x519140))(wcm, id);
    if (c) return c;
    static DWORD next;
    static unsigned skipped;
    skipped++;
    if ((int)(GetTickCount() - next) >= 0) {
        next = GetTickCount() + 2000;
        unsigned char *p = (unsigned char *)id + 0x10;  // both loops read the packet 0x10 above the handle copy
        char ids[96] = "";
        void **set = wcm ? (void **)((char *)wcm + 0x9a40) : NULL;
        if (set && !IsBadReadPtr(set, 8) && !IsBadReadPtr(*(void ***)set, 0x18)) {
            void **vt = *(void ***)set;
            int n = ((int (*)(void *))vt[0])(set);
            for (int k = 0; k < n && k < 8; k++) {
                char *pc = ((char *(*)(void *, int))vt[2])(set, k), *gd = pc && !IsBadReadPtr(pc, 0x5a8) ? *(char **)(pc + 0x5a0) : NULL;
                size_t l = strlen(ids);
                snprintf(ids + l, sizeof ids - l, " %d", gd && !IsBadReadPtr(gd, 0xc) ? *(int *)(gd + 8) : -1);
            }
        }
        char *me = wcm && !IsBadReadPtr((char *)wcm + 0x174e8, 8) ? *(char **)((char *)wcm + 0x174e8) : NULL;
        char *cat = wcm && !IsBadReadPtr((char *)wcm + 0x9910, 8) ? *(char **)((char *)wcm + 0x9910) : NULL;
        logf("crash watch: packet 0x%x for a character we do not have, dropped (%u so far): handle %08x %08x "
             "(group %u, number %u), payload %08x %02x | our handle %08x %08x, player group %d, player set ids:%s",
             kind, skipped, (unsigned)h, (unsigned)(h >> 32), (unsigned)(h >> 43) & 0x7f, (unsigned)(h >> 32) & 0x7ff,
             IsBadReadPtr(p, 0xd) ? 0 : *(unsigned *)(p + 8), IsBadReadPtr(p, 0xd) ? 0 : p[0xc],
             me && !IsBadReadPtr(me, 0x80) ? *(unsigned *)(me + 0x78) : 0, me && !IsBadReadPtr(me, 0x80) ? *(unsigned *)(me + 0x7c) : 0,
             cat && !IsBadReadPtr(cat + 0x120, 4) ? *(int *)(cat + 0x120) : -1, ids);
    }
    return &pkt_dummy;
}
static void *pkt_lookup_2b(void *wcm, unsigned long long *id) { return pkt_lookup(wcm, id, 0x2b); }
static void *pkt_lookup_72(void *wcm, unsigned long long *id) { return pkt_lookup(wcm, id, 0x72); }
static void pkt_guard_install(void) {
    pkt_dummy_vt[0x120 / 8] = (void *)pkt_no;
    hook_call(0x41c8b7, 0x519140, (void *)pkt_lookup_2b, "crash guard: packet 0x2b character lookup");
    hook_call(0x4473b7, 0x519140, (void *)pkt_lookup_72, "crash guard: packet 0x72 character lookup");
}
static void ult_pvp_install(void) {
    void **slot = (void **)(g_base + RVA_PLAYER_DMG_VTABLE) + 7;
    DWORD old;
    if (*slot != (void *)(g_base + RVA_PLAYER_HIT_RECV) || !VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        logf("ult-pvp: player damage module slot 7 not as expected (%p), not hooked", *slot);
        return;
    }
    *slot = (void *)player_hit_hook;
    VirtualProtect(slot, 8, old, &old);
    int n = g_settings[S_ULT_PVP];
    logf("ult-pvp: player hit receive hooked (ultimates against players at %d %%%s)", n ? n : 100,
         n && n != 100 ? "" : ", unchanged; log only with debug-log");
}
static void finish_relation_patch(void) {
    void **table = (void **)(g_base + 0x398d410);
    void **entry = table + 16 * 0x51 + 77, *enemy = table[2 * 0x51 + 77];
    if (*entry != (void *)(g_base + 0x398d408) || enemy != (void *)(g_base + 0x398d3f8)) {
        logf("finish test: attack relation 16 -> 77 not as expected (%p, enemy %p), not changed", *entry, enemy);
        return;
    }
    DWORD old;
    if (!VirtualProtect(entry, 8, PAGE_READWRITE, &old)) return;
    *entry = enemy;
    VirtualProtect(entry, 8, old, &old);
    logf("finish test: attack relation 16 -> 77 (invader's attacks against a downed player) Neutral -> Enemy, as 1/2 -> 77");
    // Lock-on (run 17: the invader cannot lock on to a downed host). The "is it an enemy" table at 0x399a120
    // (0x52c990 for two characters, raw effective teams) has the same 16 -> 77 Neutral; the lock-on
    // target search is assumed to ask it (not traced). Same change there.
    void **t2 = (void **)(g_base + 0x399a120), **e2 = t2 + 16 * 0x51 + 77;
    void *before = *e2;
    if (before == (void *)(g_base + 0x398d408) && t2[2 * 0x51 + 77] == (void *)(g_base + 0x398d3f8) && VirtualProtect(e2, 8, PAGE_READWRITE, &old)) {
        *e2 = t2[2 * 0x51 + 77];
        VirtualProtect(e2, 8, old, &old);
        logf("finish test: enemy relation 16 -> 77 (lock-on) was %p (Neutral) -> %p (Enemy), as 1/2 -> 77", before, *e2);
    } else
        logf("finish test: enemy relation 16 -> 77 (lock-on) not written: entry %p, 2 -> 77 %p", before, t2[2 * 0x51 + 77]);
}
// finish-test diagnostic, second round (run 15: no line from 0x44e190 / 0x44b970 / 0x44bb30 in either
// direction). The victim's side receives a hit in 0x436530(module, attacker, hit, flag) (5 call sites);
// for a downed victim it goes on to 0x43d260, which lowers the gauge through 0x66dec0 (calls at 0x43dac3
// with the attacker, 0x44c6ac without). Logged: every received hit whose victim is downed or whose
// attacker carries our effect (once a second per call site), and every gauge call.
static const unsigned RECV_SITES[] = {0x4373d9, 0x437518, 0x441157, 0x51f2cd, 0x53420b};
static void recv_hit_hook(char *module, char *attacker, char *hit, unsigned long long flag) {
    char *victim = module ? ((char *(*)(void *))(g_base + 0x42c120))(module) : NULL;
    unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
    int vdown = victim && !IsBadReadPtr(victim, 0x740) && victim[0x710] == 1;
    int aours = attacker && !IsBadReadPtr(attacker, 0x200) && our_speffect_active(attacker) == 1;
    if (vdown || aours) {
        static DWORD next[5];
        unsigned k = 0;
        while (k < 5 && RECV_SITES[k] != site) k++;
        if (k < 5 && (int)(GetTickCount() - next[k]) >= 0) {
            next[k] = GetTickCount() + 1000;
            logf("finish test: hit received at %x: victim %s state %d (ours %d), attacker %p (ours %d), hit +0x121 %u +0x280 %d",
                 site, victim == local_player() ? "local" : "remote", victim ? (unsigned char)victim[0x710] : -1,
                 victim ? our_speffect_active(victim) : -1, (void *)attacker, aours, hit ? (unsigned char)hit[0x121] : 0,
                 hit ? *(int *)(hit + 0x280) : 0);
        }
    }
    ((void (*)(char *, char *, char *, unsigned long long))(g_base + 0x436530))(module, attacker, hit, flag);
}
static void gauge_nohit_hook(char *chr, char *attacker, char *hit) {
    float before = chr && !IsBadReadPtr(chr, 0x740) ? *(float *)(chr + 0x714) : -1;
    ((void (*)(char *, char *, char *))(g_base + 0x66dec0))(chr, attacker, hit);
    logf("finish test: gauge call (0x44c6ac, no attacker): %s character, gauge %.1f -> %.1f", chr == local_player() ? "local" : "remote",
         before, chr ? *(float *)(chr + 0x714) : -1);
}
// marker-test (logging first): the world map's pins. CS::WorldMapViewModel (vtable 0x2c4a548, built by
// 0x90dc80 when the map opens, called at 0x84d8d8, 0x84d91e, 0xa4e3c0) holds one WorldMapPlayerData pin for
// the local player (pointer vector, begin at +0x88) and a fixed two for other players (begin at +0xb0, end
// +0xb8), 0xb0 bytes each; the code that fills them is protected (0x1473502da). Logged while the map is
// open, on change: each other-player pin's first 0x60 bytes, to find what makes it visible.
// Run 18: hiding the pins of the last-built model took the COMPASS markers away, not the map's: the
// model built at 0xa4e3c0 (kind 2, inside the HUD at +0xb1c8, built once when the HUD is) is the
// compass's; the map screen's models are the ones of kinds 0/1 (0x84d8d8, 0x84d91e). Each kind is kept.
static char *volatile g_map_model;
static char *volatile g_view_models[3];
static char *map_model_hook(char *model, unsigned long long a2) {
    char *r = ((char *(*)(char *, unsigned long long))(g_base + 0x90dc80))(model, a2);
    g_map_model = model;
    if ((a2 & 0xff) < 3) g_view_models[a2 & 0xff] = model;
    logf("marker test: world map view model %p built, kind %u (%s)", (void *)model, (unsigned)(a2 & 0xff),
         (a2 & 0xff) == 2 ? "HUD: compass" : "map screen");
    return r;
}
// The map screen (CS::WorldMapDialogBase, built by 0xac53a0, called at 0xad1f00) takes a view model as its
// 5th argument and draws other players from "Body/Base_1/NetPlayerList", built by 0xacd1f0 over that
// model's +0xa0 list, the same other-player pins the compass reads (self: "PlayerList" over +0x78).
// Logged: which model the map screen gets; the pin dump below follows that model.
static char *volatile g_map_dialog_model;
static void md_pins(const char *when);
static void church_pins_log(char *model);
static void *map_dialog_hook(void *a, void *b, void *c, unsigned long long d, char *model) {
    g_map_dialog_model = model;
    int kind = -1;
    for (int k = 0; k < 3; k++)
        if (g_view_models[k] == model) kind = k;
    logf("marker test: map screen built with view model %p (kind %d of the models seen)", (void *)model, kind);
    void *r = ((void *(*)(void *, void *, void *, unsigned long long, char *))(g_base + 0xac53a0))(a, b, c, d, model);
    md_pins("map screen built");
    church_pins_log(model);
    return r;
}
// Map-only hide (decision 19). The map screen's list controls (CS::WorldMapItemControl, built by 0xacd1f0
// over a pin list) draw through 0xace8e0(control+0x38, list, selected, ...), called at 0xacedf2; the item
// code reads each pin's +0x0c "shown" and its visibility method. NetPlayerList's list is the view model's
// other-player list at model+0xa0, the one the compass reads too. So the map rule is applied only around
// that draw call: the pins that must not be on the map get +0x0c = 0 for the call and their own value
// back right after, so the compass rule (step hook) is untouched.
static void map_list_draw(const char *where, char *ctl, char *list, int selected, float f3, float f5, unsigned long long b6);
static void map_list_draw_hook(char *ctl, char *list, int selected, float f3, float f5, unsigned long long b6) {
    map_list_draw("acedf2", ctl, list, selected, f3, f5, b6);
}
static void map_list_draw(const char *where, char *ctl, char *list, int selected, float f3, float f5, unsigned long long b6) {
    char *model = list ? list - 0xa0 : NULL;
    char saved[8];
    char *pins[8];
    int n = 0;
    int ours = model && !IsBadReadPtr(model, 0xc0) && *(char **)model == g_base + 0x2c4a548 &&
               (g_join_in_world() || g_host_switched);
    if (ours) {
        char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
        char *mb = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *me = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
        char **b = *(char ***)(model + 0xb0), **e = *(char ***)(model + 0xb8);
        for (int i = 0; b && b + i < e && i < 8; i++) {
            char *pin = b[i];
            if (!pin || IsBadReadPtr(pin, 0x20) || !pin[0xc]) continue;
            int hide = g_invader_in_world() || member_no_is_invader(*(int *)(pin + 0x1c));
            (void)mb, (void)me;
            if (!hide) continue;
            pins[n] = pin, saved[n++] = pin[0xc];
            pin[0xc] = 0;
        }
        static DWORD next[3];
        int w = where[2] == 'e' ? 0 : where[4] == '7' ? 1 : 2;
        if (n && (int)(GetTickCount() - next[w]) >= 0) {
            next[w] = GetTickCount() + 5000;
            logf("marker test: map screen (call at %s): %d other-player pin(s) left out of the list draw (%s)", where, n,
                 g_invader_in_world() ? "invader: all" : "host / ally: the invaders");
        }
    }
    ((void (*)(char *, char *, int, float, float, unsigned long long))(g_base + 0xace8e0))(ctl, list, selected, f3, f5, b6);
    for (int i = 0; i < n; i++) pins[i][0xc] = saved[i];
}
static void marker_tick(void) {
    static DWORD next;
    static char last[4][0x60];
    char *m = g_map_dialog_model ? g_map_dialog_model : g_view_models[0] ? g_view_models[0] : g_view_models[1];
    if (!m || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 2000;
    if (IsBadReadPtr(m, 0xc0) || *(char **)m != g_base + 0x2c4a548) {
        g_map_model = NULL;
        return;
    }
    char **b = *(char ***)(m + 0xb0), **e = *(char ***)(m + 0xb8);
    for (int i = 0; b && b + i < e && i < g_cap_remote; i++) {  // one pin per remote slot (0x14090dc80: 0x1401fe4c0 pins)
        char *pin = b[i];
        if (!pin || IsBadReadPtr(pin, 0x60) || !memcmp(last[i], pin, 0x60)) continue;
        memcpy(last[i], pin, 0x60);
        char hex[0x60 * 2 + 1];
        for (int k = 0; k < 0x60; k++) sprintf(hex + 2 * k, "%02x", (unsigned char)pin[k]);
        logf("marker test: other-player pin %d %p: %s", i, (void *)pin, hex);
    }
}
// finish-test log at that gate: each 0x436370 call (4 sites) with a downed target or an attacker
// carrying our effect, at most once a second: teams, the attack's relation flags (hit +0x10 -> +0x81)
// and the result.
static const unsigned CANHIT_SITES[] = {0x43f1de, 0x43f306, 0x43f452, 0x33c318};
static unsigned char can_hit_hook(char *module, char *target, char *hit, unsigned long long flag) {
    unsigned char r = ((unsigned char (*)(char *, char *, char *, unsigned long long))(g_base + 0x436370))(module, target, hit, flag);
    char *attacker = module ? ((char *(*)(void *))(g_base + 0x42c120))(module) : NULL;
    if (!attacker || !target || IsBadReadPtr(attacker, 0x740) || IsBadReadPtr(target, 0x740)) return r;
    if (target[0x710] != 1 && our_speffect_active(attacker) != 1) return r;
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return r;
    next = GetTickCount() + 1000;
    unsigned char ta = 0, tt = 0;
    ((unsigned char *(*)(void *, unsigned char *))(g_base + 0x3d1b30))(attacker, &ta);
    ((unsigned char *(*)(void *, unsigned char *))(g_base + 0x3d1b30))(target, &tt);
    char *atk = hit && !IsBadReadPtr(hit, 0x18) ? *(char **)(hit + 0x10) : NULL;
    logf("finish test: can-hit 0x436370 from %x: attacker team %u (ours %d), target team %u state %u, attack flags %02x, mode %u -> %u",
         (unsigned)((char *)__builtin_return_address(0) - g_base) - 5, ta, our_speffect_active(attacker), tt,
         (unsigned char)target[0x710], atk && !IsBadReadPtr(atk + 0x81, 1) ? (unsigned char)atk[0x81] : 0xff, (unsigned)(flag & 0xff), r);
    return r;
}
// marker-test (decision 19), from run 17's dumps: an other-player pin has +0x0c "shown" (1/0), +0x10..+0x18
// the position (moves with that player), +0x1c the player's session number (member +0xd4: host 1,
// invader 2; -1 unused). Each frame (game thread, from the step hook) while the map is open: on the
// invader every other-player pin is set not shown; on the host a pin is set not shown when its number
// belongs to a remote member of our session (in our sessions every remote member is an invader).
// Decision 19. Compass (kind 2): the invader keeps only the session host's pin (the member whose Steam id is
// the lobby owner's), the host drops the invader's. Map screen (kinds 0/1): the invader drops every
// other-player pin, the host the invader's.
static int g_host_player_no;
static void host_player_no_update(char *mb, char *me);  // invader: the session host's player number (member +0xd4), -1 unknown
// Runs 19-20: these view models (kinds 1 and 0, built at the start of the session by 0x84d840) feed the
// COMPASS; the map screen draws other players from elsewhere (not found yet). The compass rule, each
// frame from the step hook: the invader keeps only the session host's pin, the host drops the
// invader's. (Run 20 removed the menu-time map rule: the game step runs while the map is open.)
static volatile DWORD g_last_step_tick;
static void marker_apply(int map_rule);
static void marker_clear(void) {
    g_last_step_tick = GetTickCount();
    marker_apply(0);
}
static void marker_apply(int map_rule) {
    int invader = g_invader_in_world();
    if (!invader && !g_host_switched && !g_join_in_world()) return;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    char *mb = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *me = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    if (invader) host_player_no_update(mb, me);
    for (int k = 0; k < 3; k++) {
        char *m = g_view_models[k];
        if (!m || IsBadReadPtr(m, 0xc0) || *(char **)m != g_base + 0x2c4a548) continue;
        char **b = *(char ***)(m + 0xb0), **e = *(char ***)(m + 0xb8);
        for (int i = 0; b && b + i < e && i < g_cap_remote; i++) {  // one pin per remote slot (0x14090dc80: 0x1401fe4c0 pins)
            char *pin = b[i];
            if (!pin || IsBadReadPtr(pin, 0x20) || !pin[0xc]) continue;
            int no = *(int *)(pin + 0x1c), hide = 0;
            if (invader) {
                int host_no = g_host_player_no;
                if (host_no < 0) {  // not found from the lobby owner: the host has been player 1 in every run
                    host_no = 1;
                    static int told;
                    if (!told) told = 1, logf("marker test: session host's player number not found, player 1 assumed");
                }
                hide = map_rule || no != host_no;
            }
            else hide = member_no_is_invader(no);
            if (!hide) continue;
            static DWORD next[3];
            if ((int)(GetTickCount() - next[k]) >= 0) {
                next[k] = GetTickCount() + 5000;
                logf("marker test: pin of player %d (model kind %d) set not shown by the %s rule (%s; session host is player %d)", no, k,
                     map_rule ? "map (game step idle)" : "compass (game stepping)", invader ? "invader" : "host", g_host_player_no);
            }
            pin[0xc] = 0;
        }
    }
}
// name-test (rescue gauge on the invader). A downed player's state reaches the others through the downed
// event (receiver 0x4f3a98 area): event 1 sets state, timer, gauge (0x66f3b0, 0x66f3a0) and the down count;
// event 3 (a hit on the gauge) also adds the downed player to the attacker's rescue-target list,
// 0x3d7610(attacker, downed), which feeds the rescue gauge HUD ("PlayerHUD/Rescue/RescuePointGauge").
// Logged: every add to a rescue-target list (from the event at 0x4f3a98, and locally from the gauge
// function at 0x66e03e) with attacker and target; and, on both sides, once a second while another player
// is downed, its +0x710 state, +0x714 gauge, +0x718 timer and +0x730 count as this machine sees them.
static void rescue_add_log(const char *where, char *attacker, char *downed) {
    char *me = local_player();
    logf("name test: rescue target added (%s): attacker %s (our effect %d), downed %s (our effect %d, state %d, gauge %.1f)", where,
         attacker == me ? "local" : "remote", attacker && !IsBadReadPtr(attacker, 0x200) ? our_speffect_active(attacker) : -1,
         downed == me ? "local" : "remote", downed && !IsBadReadPtr(downed, 0x200) ? our_speffect_active(downed) : -1,
         downed && !IsBadReadPtr(downed, 0x740) ? (unsigned char)downed[0x710] : -1, downed && !IsBadReadPtr(downed, 0x740) ? *(float *)(downed + 0x714) : -1);
}
static void rescue_add_event_hook(char *attacker, char *downed) {
    rescue_add_log("downed event", attacker, downed);
    ((void (*)(char *, char *))(g_base + 0x3d7610))(attacker, downed);
}
static void rescue_add_local_hook(char *attacker, char *downed) {
    rescue_add_log("local gauge hit", attacker, downed);
    ((void (*)(char *, char *))(g_base + 0x3d7610))(attacker, downed);
}
static void other_downed_watch(const char *side) {
    static DWORD next;
    static float last[8][3];
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 1000;
    char *wcm = *(char **)(g_base + 0x3c0f0a8), *me = wcm ? *(char **)(wcm + 0x174e8) : NULL;
    if (!wcm) return;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    for (int i = 0; i < count && i < 8; i++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i);
        if (!c || c == me || IsBadReadPtr(c, 0x740) || c[0x710] != 1) continue;
        float now[3] = {*(float *)(c + 0x714), *(float *)(c + 0x718), (float)(unsigned char)c[0x730]};
        if (!memcmp(now, last[i], sizeof now)) continue;
        memcpy(last[i], now, sizeof now);
        logf("name test (%s): other player %d downed as seen here: state %u gauge %.1f timer %.2f count %u (our effect %d)", side, i,
             (unsigned char)c[0x710], now[0], now[1], (unsigned char)c[0x730], our_speffect_active(c));
    }
}
// The play state's reward part, as STEP_Result's reward screen reads it (0x1fc5e0): +0x36c amount of kind
// 0x8000000a, +0x36e kind 0x8000000b (bit 0 an extra entry), +0x372/+0x374 up to five goods, +0x373/+0x388
// relics. The game's computation 0x1fd620 grants (relics 0x1f0160, goods, currency 0x209730 on the player
// data at play state +8: +0xd0, capped at 999999999) and records what it granted there; +0x36c is the
// amount given to 0x209730. For the invader's ending it is replaced: every reward field emptied, and on a
// win REWARD_MURK granted through 0x209730 and recorded in +0x36c.
static void reward_hook(unsigned long long a1, unsigned a2) {
    char *ps = *(char **)(g_base + 0x3c078d0);
    if (!g_reward_pending || !ps) {
        ((void (*)(unsigned long long, unsigned))(g_base + 0x1fd620))(a1, a2);
        // The owner (2026-11-19): every expedition gives at least REWARD_MURK_MIN murk, however it ends; more from the
        // game's own result stays. The game recorded what it gave in +0x36c (the amount it passed to 0x209730).
        char *pd = ps ? *(char **)(ps + 8) : NULL;
        if (ps && pd && !IsBadReadPtr(pd, 0x1a0)) {
            unsigned got = *(unsigned short *)(ps + 0x36c);
            if (got < REWARD_MURK_MIN) {
                int before = *(int *)(pd + 0xd0);
                int added = ((int (*)(char *, int))(g_base + 0x209730))(pd, (int)(REWARD_MURK_MIN - got));
                *(unsigned short *)(ps + 0x36c) = (unsigned short)(got + (unsigned)added);
                logf("reward: the expedition gave %u murk, raised to the minimum %d: murk %d -> %d, reward screen amount %u", got,
                     REWARD_MURK_MIN, before, *(int *)(pd + 0xd0), *(unsigned short *)(ps + 0x36c));
            } else
                logf("reward: the expedition gave %u murk (at least the minimum %d)", got, REWARD_MURK_MIN);
        }
        return;
    }
    int won = g_reward_pending == 1;
    g_reward_pending = 0, g_host_killed = 0;
    logf("reward test: game's reward computation skipped; was +0x36c %u +0x36e %u goods %u relics %u (result id %u)",
         *(unsigned short *)(ps + 0x36c), *(unsigned short *)(ps + 0x36e), (unsigned char)ps[0x372], (unsigned char)ps[0x373],
         *(unsigned *)(ps + 0x368));
    *(unsigned short *)(ps + 0x36c) = 0, *(unsigned short *)(ps + 0x36e) = 0;
    ps[0x372] = 0, ps[0x373] = 0;
    char *pd = *(char **)(ps + 8);
    if (pd && !IsBadReadPtr(pd, 0x1a0)) {
        int before = *(int *)(pd + 0xd0);
        int added = ((int (*)(char *, int))(g_base + 0x209730))(pd, won ? REWARD_MURK : REWARD_MURK_LOST);
        *(unsigned short *)(ps + 0x36c) = (unsigned short)added;
        logf("reward test: %s: murk %d -> %d (+%d), reward screen amount +0x36c = %d", won ? "won" : "lost", before, *(int *)(pd + 0xd0),
             added, added);
    } else
        logf("reward test: %s: no player data, nothing granted", won ? "won" : "lost");
}
// two-regions: the map request (0x6b06f0, called once, from RequestMoveMap at 0xc27a41) turns the pattern
// into the current expedition and puts one region flag (7601..7606, the pattern's map type) into gm +0xbb8
// (int[4], count int64 at +0xbd0). Map tiles take their variant from those flags, per tile
// (CalcGetReplaceMapIdByDisaster, 0x6ab7a0: the first flag whose variant the tile has), and so do the
// tile scripts (run 35: the Rotted Woods' rot works with the Mountaintop as the map type). The patterns
// are the game's Mountaintop ones (40g+20..24) with the Rotted Woods added by the launcher; the map type
// stays the Mountaintop's (what its cold and ice dragon need, runs 33..35). Here, once per map request:
// 7603 (the Rotted Woods, tile variant 3) appended to the list (the game puts only the record's last region
// flag, 7601, there: 0x6b06f0). The event flags come from the record (the launcher puts a 7603 row before
// 7601; OnMapEnter sets both). Host and invader alike. Event flag calls: 0x5e7260 get (man, &id, tag), 0x5e79f0 set
// (man, &id, on, tag, notify 0, 0), the manager at 0x3c115a8.
static int pair_base_off(void) { int p = g_settings[S_PAIR]; return p == 2 || p == 3 ? 25 : 20; }  // Crater base for pairs 2 / 3
static int two_pattern(unsigned pat) {  // the plain and base-region slots (decision 35; pairs 2 / 3: Crater 25..29)
    return pat < 320 && (pat % 40 < 20 || (pat % 40 >= (unsigned)pair_base_off() && pat % 40 < (unsigned)pair_base_off() + 5));
}
static int two_flag(int id, int set, int on) {  // set: 0 read, 1 write
    char *man = *(char **)(g_base + 0x3c115a8);
    if (!man) return -1;
    struct { unsigned char kind; const char *name; } tag = {1, "nrpvp two-regions"};
    if (!set) return ((char (*)(char *, int *, void *))(g_base + 0x5e7260))(man, &id, &tag) != 0;
    ((void (*)(char *, int *, char, void *, char, char))(g_base + 0x5e79f0))(man, &id, (char)on, &tag, 0, 0);
    return on;
}
// Event flags (CS::CSFD4VirtualMemoryFlag; manager 0x3c115a8, built by 0x5e3ed0 -> 0x60c500(heap, 7, 3)):
// +0x1c flags per block (1000), +0x20 bytes per block (125), +0x28 one buffer of block slots, +0x38 a
// std::map block number (id / 1000) -> node {+0x20 key, +0x28 kind, +0x30 slot or pointer}: kind 1 = slot
// in the +0x28 buffer (slot * 125), kind 2 = a buffer of its own (blocks kept from the save, map +0x160).
// Bit = 7 - (rest & 7) of byte rest >> 3. Read 0x60ce40 (from get 0x5e7260), write 0x60d330 (from the
// two setters 0x5e79f0 and 0x5e7740, which every script and code write ends in). No block: the read gives
// 0 and the write is dropped without a word. The blocks are laid out again at every map load (MoveMapStep
// 0xc2fc30 -> 0x5e7d40 tile list, 0x5e6b90 layout; mode 1 = the expedition (0x5e4f60 keeps blocks and rebuilds the tile list), mode 0 = the hub and leaving (fresh)): 0..9 (ids 0..9999),
// 60..99 (60000..99999), and for each field tile of the 4x4 grid m60_42..45_36..39 (0x7623c0), taken
// through its Shifting Earth variant (0x6ab690 -> CalcGetReplaceMapIdByDisaster 0x6ab7a0, from gm +0xbb8),
// blocks 1000000 + ((X%10*10 + Y%10)*10 + variant)*100 + d, d in {0,2,4,5,7,8,9}: a ten-digit id reads
// 10 X Y V dd nnn (1056300800: tile x5_x6, variant 3, d 0, flag 800). A tile flag of a variant that is
// not the one laid out for that tile has no block. flag_state: 1 / 0, -1 no block, -2 no manager.
static char *flag_man(void) {
    char *man = *(char **)(g_base + 0x3c115a8);
    return man && !IsBadReadPtr(man, 0x40) && *(unsigned *)(man + 0x1c) ? man : NULL;
}
static char *flag_block(char *man, unsigned key) {  // the block's bytes, NULL when there is none
    char *head = *(char **)(man + 0x38), *node = head ? *(char **)(head + 8) : NULL, *best = head;
    while (node && !node[0x19]) {
        if (*(unsigned *)(node + 0x20) < key) node = *(char **)(node + 0x10);
        else best = node, node = *(char **)node;
    }
    if (!best || best == head || best[0x19] || *(unsigned *)(best + 0x20) > key) return NULL;
    int kind = *(int *)(best + 0x28);
    if (kind == 1) return *(char **)(man + 0x28) ? *(char **)(man + 0x28) + (unsigned)(*(int *)(best + 0x30) * *(int *)(man + 0x20)) : NULL;
    return kind == 2 ? *(char **)(best + 0x30) : NULL;
}
static int flag_state_in(char *man, unsigned id) {
    unsigned div = *(unsigned *)(man + 0x1c), key = id / div, bit = id - key * div;
    char *data = flag_block(man, key);
    return data ? (data[bit >> 3] >> (7 - (bit & 7))) & 1 : -1;
}
// Every block key in order (std::map: node +0 left, +8 parent, +0x10 right, +0x19 nil). Returns the count.
static int flag_blocks(char *man, unsigned *keys, int cap) {
    char *head = *(char **)(man + 0x38), *n = head ? *(char **)head : NULL;
    int k = 0;
    while (n && n != head && !n[0x19] && k < 20000) {
        if (k < cap) keys[k] = *(unsigned *)(n + 0x20);
        k++;
        char *r = *(char **)(n + 0x10);
        if (!r[0x19]) {
            n = r;
            while (!(*(char **)n)[0x19]) n = *(char **)n;
        } else {
            char *p = *(char **)(n + 8);
            while (!p[0x19] && n == *(char **)(p + 0x10)) n = p, p = *(char **)(p + 8);
            n = p;
        }
    }
    return k;
}
// flag-log: one line per change (id, new value, seconds since the map load). Writes that change nothing
// are skipped; a flag changing more than 4 times in 2 s is counted instead and reported once it is quiet.
// A write whose block is missing is logged once per flag and map load ("dropped").
#define FLOG_SLOTS 4096
static struct { unsigned id; DWORD t0, last; int n, hidden, dropped; signed char value; } g_flog[FLOG_SLOTS];
static CRITICAL_SECTION g_flog_lock;
static DWORD g_flog_layout_tick;
static int g_flog_last_mode = -1;  // mode of the last layout (run 32: 1 = the expedition, 0 = hub / leaving), -1 none seen
static struct { unsigned id; DWORD t; } g_flog_back[8];  // ten-digit flags turned on, read back 5 s later
static int g_flog_back_n, g_flog_back_logged;
static double flog_t(void) { return (GetTickCount() - g_flog_layout_tick) / 1000.0; }
// Boss deaths: every flag a site-of-grace chain waits for (common_func 9005810's first and 90015006's third parameter
// in the game's tile and small-map scripts), listed by the launcher at the mod build from the player's own game files:
// work/native/boss-deaths.txt (a header line, one id per line). One per boss: a small map's boss set-up (90045002)
// also sets a flag of its variant (m46_50: 46500830 beside 46500800), which is not listed. Small-map ids of instance
// n are n * 10^8 + the script's id; tile ids are exact. Without the file: any nnn 800..899 with d = 0 (logged).
static unsigned *g_boss_death;
static int g_boss_deaths = -1;
#define HIDE_MAX 1024
static unsigned g_hide_ent[HIDE_MAX];  // every listed boss's characters
static int g_hide_n;  // -1: no list, the fallback rule
static void boss_deaths_load(void) {
    char path[MAX_PATH];
    next_to_dll(path, sizeof path, "boss-deaths.txt");
    FILE *f = fopen(path, "rb");
    static unsigned list[4096];
    int n = 0;
    char line[256];
    while (f && fgets(line, sizeof line, f))
        if (line[0] >= '0' && line[0] <= '9' && n < 4096) {
            char *at = line;
            list[n++] = (unsigned)strtoul(at, &at, 10);
            for (;;) {  // then the boss's characters (entity ids, launcher: the event that sets the flag on its death)
                unsigned e = (unsigned)strtoul(at, &at, 10);
                if (!e) break;
                int dup = 0;
                for (int k = 0; k < g_hide_n; k++) dup |= g_hide_ent[k] == e;
                if (!dup && g_hide_n < HIDE_MAX) g_hide_ent[g_hide_n++] = e;
            }
        }
    if (f) fclose(f);
    for (int i = 1; i < n; i++)  // keep it sorted (the launcher writes it sorted)
        for (int j = i; j > 0 && list[j - 1] > list[j]; j--) { unsigned t = list[j]; list[j] = list[j - 1], list[j - 1] = t; }
    if (n > 0) g_boss_death = list, g_boss_deaths = n, logf("clock: %d boss death flags loaded (%d boss characters)", n, g_hide_n);
    else logf("clock: no boss death list (boss-deaths.txt%s): any nnn 800..899 with d = 0 counts as a boss", f ? " empty" : " missing");
}
static int boss_death(unsigned id) {
    if (g_boss_deaths < 0) return id % 1000 >= 800 && id % 1000 < 900 && id / 1000 % 10 == 0;
    unsigned key = id >= 1000000000u ? id : id % 100000000u;
    int lo = 0, hi = g_boss_deaths - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_boss_death[mid] == key) return 1;
        if (g_boss_death[mid] < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static void df_kill(unsigned id);
static void df_chr_pass_hook(char *chr);
static void df_death_restart_install(void);
static void flag_note(char *man, unsigned id, int on, int before) {
    (void)man;
    if (on && before == 0 && id >= 10000000u && boss_death(id)) df_kill(id);
    if (!setting(S_FLAG_LOG)) {
        // always (boss-grace check): a boss-family flag (nnn 800..899 in a block with d = 0, the d digit being
        // (id / 1000) % 10 in tile ids 10XXYYdnnn / 10XYVddnnn and small-map ids) turned on, or dropped for want of a
        // block; whether it is a boss death (a grace chain's flag, counted by boss-flow 3) is said on the line.
        if (on && before != 1 && id >= 10000000u && id % 1000 >= 800 && id % 1000 < 900 && id / 1000 % 10 == 0)
            logf("flags: boss-family flag %u -> 1%s%s", id, before < 0 ? " DROPPED, no block" : "",
                 boss_death(id) ? " (a boss death)" : " (not a grace chain's death flag: not counted)");
        return;
    }
    if (before == (on != 0)) return;
    EnterCriticalSection(&g_flog_lock);
    unsigned h = (id * 2654435761u) % FLOG_SLOTS;
    for (int k = 0; k < 64 && g_flog[h].id && g_flog[h].id != id; k++) h = (h + 1) % FLOG_SLOTS;
    DWORD now = GetTickCount();
    if (!g_flog[h].id || g_flog[h].id == id) {
        if (!g_flog[h].id) g_flog[h].id = id, g_flog[h].t0 = now, g_flog[h].n = 0;
        if (before < 0) {
            if (!g_flog[h].dropped++) logf("flags: %u -> %d dropped, no block (t %.1f)", id, on != 0, flog_t());
        } else {
            if (now - g_flog[h].t0 > 2000) g_flog[h].t0 = now, g_flog[h].n = 0;
            g_flog[h].last = now, g_flog[h].value = (signed char)(on != 0);
            if (++g_flog[h].n <= 4) logf("flags: %u -> %d (t %.1f)", id, on != 0, flog_t());
            else g_flog[h].hidden++;
        }
    } else if (before >= 0)
        logf("flags: %u -> %d (t %.1f; table full)", id, on != 0, flog_t());
    if (on && before == 0 && id >= 1000000000u && g_flog_back_n < 8) g_flog_back[g_flog_back_n].id = id, g_flog_back[g_flog_back_n++].t = now;
    LeaveCriticalSection(&g_flog_lock);
}
// map-dump (test, read only): one file per own expedition, work/native/map-dump-p<pattern>-<hhmmss>.txt
// (mapdump.h: opened at the first InGame frame, appended and flushed, closed when the expedition ends; what
// comes before the opening, i.e. the expedition's flag layout and the flag writes of the map load, is kept in
// memory and written at the top; layouts and writes outside an expedition never touch a file). Run 32: the
// expedition's own layout is mode 1; mode 0 is the hub's and the one when leaving. At the layout: the 16 tiles. At the first InGame frame: pattern, seed, map type, region list,
// start point, the pattern record (0x6ad3e0(0x6ad850()): +0 / +4 play-area rows, +0x18 flag count, +0x24 + 8i
// flags, +0x19 small-base count, entries 0x3c bytes from +0xc0: +0xc0 i32, +0xc6 AA, +0xc7 BB, +0xcc instance),
// the flag blocks and every flag that is on; again every flag on at +30 s and +180 s. All the time: every
// flag write that changes a value or is dropped (no block), with the two callers above the setter (return
// addresses as offsets in the game), more than 4 changes of one flag in 2 s counted instead. Nothing written.
#include "mapdump.h"
static md_state g_mds;
static int g_md;  // an expedition file is open
static DWORD g_md_t0;
static struct { unsigned id; DWORD t0, last; int n, hidden; signed char value; } g_md_seen[4096];
static void mdf(const char *fmt, ...) {
    if (!setting(S_MAP_DUMP)) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    md_print(&g_mds, "%s", line);
}
static void md_flags_on(char *man, const char *when) {
    static unsigned keys[4096];
    int n = flag_blocks(man, keys, 4096), total = 0;
    mdf("== flags on (%s), %d blocks\n", when, n);
    for (int i = 0; i < n && i < 4096; i++) {
        char *data = flag_block(man, keys[i]);
        int first = 1;
        for (unsigned b = 0; data && b < 1000; b++)
            if (data[b >> 3] >> (7 - (b & 7)) & 1) {
                if (first) mdf("block %u:", keys[i]), first = 0;
                mdf(" %u", keys[i] * 1000 + b), total++;
            }
        if (!first) mdf("\n");
    }
    mdf("== %d flags on\n", total);
    md_flush(&g_mds);
}
static void md_layout(char *man, int mode) {  // under g_flog_lock
    if (!setting(S_MAP_DUMP)) return;
    if (!g_mds.dir[0]) {
        next_to_dll(g_mds.dir, sizeof g_mds.dir, "x");
        char *slash = strrchr(g_mds.dir, '\\');
        if (slash) *slash = 0;
    }
    if (g_md) {  // the expedition's file stays as it is; only a note
        mdf("== layout %d at %.1f s during the expedition (not kept)\n", mode, (GetTickCount() - g_md_t0) / 1000.0);
        return;
    }
    md_layout_begin(&g_mds);
    g_md_t0 = GetTickCount();
    memset(g_md_seen, 0, sizeof g_md_seen);
    SYSTEMTIME t;
    GetLocalTime(&t);
    md_layout_print(&g_mds, "== layout %d at %02d:%02d:%02d (times below: seconds since this layout)\n", mode, t.wHour, t.wMinute, t.wSecond);
    for (char *e = *(char **)(man + 0x118); e && e < *(char **)(man + 0x120); e += 8) {
        unsigned m = *(unsigned *)e;
        md_layout_print(&g_mds, "tile m%02u_%02u_%02u_%u%u own-variant-flags %d\n", m >> 24, m >> 16 & 0xff, m >> 8 & 0xff, m >> 4 & 0xf, m & 0xf, e[4]);
    }
    static unsigned keys[4096];
    int n = flag_blocks(man, keys, 4096);
    md_layout_print(&g_mds, "blocks %d:", n);
    for (int i = 0; i < n && i < 4096; i++) md_layout_print(&g_mds, " %u", keys[i]);
    md_layout_print(&g_mds, "\n");
}
static void md_end(const char *why) {  // the expedition is over
    if (!g_md) return;
    EnterCriticalSection(&g_flog_lock);
    md_close(&g_mds, why);
    g_md = 0;
    LeaveCriticalSection(&g_flog_lock);
}
static void md_hex(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) mdf("%02x", p[i]);
}
static void md_entry(void) {  // first InGame frame of an expedition
    char *man = flag_man(), *gm = *(char **)(g_base + 0x3c13258);
    if (!g_md || !man || !gm) return;
    mdf("== map entry (first InGame frame) at %.1f s\n", (GetTickCount() - g_md_t0) / 1000.0);
    mdf("pattern +b4c %u +b48 %u seed +b54 %u +b5c %u map type +b46 %u +b50 %d\n", *(unsigned *)(gm + 0xb4c),
        *(unsigned *)(gm + 0xb48), *(unsigned *)(gm + 0xb54), *(unsigned *)(gm + 0xb5c), (unsigned char)gm[0xb46], *(int *)(gm + 0xb50));
    long long cnt = *(long long *)(gm + 0xbd0);
    mdf("region list (%lld):", cnt);
    for (long long i = 0; i < cnt && i < 4; i++) mdf(" %d", ((int *)(gm + 0xbb8))[i]);
    mdf("\nstart block +a74 %08x pos %.2f %.2f %.2f\n", *(unsigned *)(gm + 0xa74), *(float *)(gm + 0xa78), *(float *)(gm + 0xa7c), *(float *)(gm + 0xa80));
    int pat = ((int (*)(void))(g_base + 0x6ad850))();
    unsigned char *rec = pat >= 0 ? ((unsigned char *(*)(int))(g_base + 0x6ad3e0))(pat) : NULL;
    mdf("record of pattern %d: %p\n", pat, (void *)rec);
    if (rec && !IsBadReadPtr(rec, 0xc0)) {
        mdf("record head "), md_hex(rec, 0xc0), mdf("\n");
        mdf("play area rows %d %d, values %d %d, flags %u, small bases %u\n", ((int *)rec)[0], ((int *)rec)[1], ((int *)rec)[2],
            ((int *)rec)[3], rec[0x18], rec[0x19]);
        for (int i = 0; i < rec[0x18]; i++) mdf("record flag %d %d\n", i, *(int *)(rec + 0x24 + 8 * i));
        for (int i = 0; i < rec[0x19] && !IsBadReadPtr(rec + 0xc0 + 0x3c * i, 0x3c); i++) {
            unsigned char *e = rec + 0xc0 + 0x3c * i;
            mdf("small base %d: m%02u_%02u #%u value %d raw ", i, e[6], e[7], e[0xc], *(int *)e);
            md_hex(e, 0x3c);
            mdf("\n");
        }
    }
    md_flags_on(man, "map entry");
}
__attribute__((noinline)) static void md_note(unsigned id, int on, int before) {
    if (before == (on != 0)) return;
    void *fr[4] = {0};
    USHORT got = RtlCaptureStackBackTrace(2, 3, fr, NULL);  // skip us and flag_store_hook: setter, its caller, one up
    DWORD now = GetTickCount();
    unsigned h = (id * 2654435761u) % 4096;
    for (int k = 0; k < 64 && g_md_seen[h].id && g_md_seen[h].id != id; k++) h = (h + 1) % 4096;
    if (!g_md_seen[h].id) g_md_seen[h].id = id, g_md_seen[h].t0 = now;
    if (now - g_md_seen[h].t0 > 2000) g_md_seen[h].t0 = now, g_md_seen[h].n = 0;
    g_md_seen[h].last = now, g_md_seen[h].value = (signed char)(on != 0);
    if (++g_md_seen[h].n > 4) { g_md_seen[h].hidden++; return; }
    char call[3][24];
    for (int i = 0; i < 3; i++) {
        unsigned long long a = (unsigned long long)fr[i];
        if (i < got && a >= (unsigned long long)g_base && a < (unsigned long long)g_base + 0x8000000)
            snprintf(call[i], sizeof call[i], "%llx", a - (unsigned long long)g_base);
        else snprintf(call[i], sizeof call[i], "-");
    }
    mdf("%.3f flag %u -> %d%s by %s < %s < %s\n", (now - g_md_t0) / 1000.0, id, on != 0, before < 0 ? " DROPPED (no block)" : "", call[0], call[1], call[2]);
}
// Pin listing (map-dump, read only): the map view model (CS::WorldMapViewModel, vtable 0x2c4a548; the one the
// map screen got, else kind 0, else 1) holds a WorldMapPinDataList<WorldMapPointPinData*> (vtable 0x2c4a510),
// begin/end at list +0x10/+0x18 (as the other-player list at +0xa0 -> +0xb0/+0xb8). A WorldMapPointPinData
// (vtable 0x2c4a148): +0x78 row id, +0x80 the WorldMapPointParam row, +0x390 IsOpen last frame, +0x391 visible
// (UpdateVisible 0x909450). Row flag fields, IsOpen 0xeaaa70: +0xc (eventFlagId5) on hides, +4 / +8 must be on.
static void md_pins(const char *when) {
    static const unsigned short OFF[] = {4, 8, 0xc, 0x44, 0x48, 0x4c, 0x60, 0x64, 0x6c, 0x70};
    static const char *const NAME[] = {"id0", "id1", "id5", "id2", "id3", "id4", "id6", "id7", "id8", "fh0"};
    static const unsigned TREE[] = {64443630, 64443730, 64453730};
    if (!setting(S_MAP_DUMP) || !g_md) return;
    char *man = flag_man();
    char *m = g_map_dialog_model ? g_map_dialog_model : g_view_models[0] ? g_view_models[0] : g_view_models[1];
    if (!man || !m) return;
    EnterCriticalSection(&g_flog_lock);
    int size = !IsBadReadPtr(m, 0x800) ? 0x800 : !IsBadReadPtr(m, 0x300) ? 0x300 : 0;
    if (!size || *(char **)m != g_base + 0x2c4a548) {
        mdf("== pins (%s): no map view model\n", when);
        LeaveCriticalSection(&g_flog_lock);
        return;
    }
    int lists = 0, tree_seen[3] = {0};
    for (int off = 8; off + 0x20 <= size; off += 8) {
        char *list = m + off;
        if (*(char **)list != g_base + 0x2c4a510) continue;
        char **b = *(char ***)(list + 0x10), **e = *(char ***)(list + 0x18);
        long long n = b && e >= b ? e - b : -1;
        mdf("== pins (%s, %.1f s): point pin list at view model %p +0x%x, %lld pins\n", when,
            (GetTickCount() - g_md_t0) / 1000.0, (void *)m, off, n);
        lists++;
        for (long long i = 0; i < n && i < 4096 && !IsBadReadPtr(b + i, 8); i++) {
            char *pin = b[i];
            if (!pin || IsBadReadPtr(pin, 0x398)) { mdf("pin %lld: unreadable %p\n", i, (void *)pin); continue; }
            unsigned id = *(unsigned *)(pin + 0x78);
            char *row = *(char **)(pin + 0x80);
            for (int t = 0; t < 3; t++) tree_seen[t] |= id == TREE[t];
            mdf("pin %lld row %u open %u visible %u", i, id, (unsigned char)pin[0x390], (unsigned char)pin[0x391]);
            if (*(char **)pin != g_base + 0x2c4a148) mdf(" vtable +%llx", (unsigned long long)(*(char **)pin - g_base));
            if (row && !IsBadReadPtr(row, 0x80)) {
                mdf(" attach %d 3c %u:", *(int *)(row + 0x10), (unsigned char)row[0x3c]);
                for (int k = 0; k < 10; k++) {
                    unsigned f = *(unsigned *)(row + OFF[k]);
                    if (f && f != 0xffffffffu) mdf(" %s %u=%d", NAME[k], f, flag_state_in(man, f));
                }
            } else mdf(" row %p unreadable", (void *)row);
            mdf("\n");
        }
    }
    if (!lists) mdf("== pins (%s): no point pin list in view model %p (first 0x%x bytes)\n", when, (void *)m, size);
    else for (int t = 0; t < 3; t++) mdf("tree row %u: %s\n", TREE[t], tree_seen[t] ? "pin present" : "NO pin");
    md_flush(&g_mds);
    LeaveCriticalSection(&g_flog_lock);
}
// Circle effects log (map-dump, read only), once a second while the circle exists (+0x159) and on any change:
// StageRend +0xc553 before the circle update; play-area manager (0x3c13158): phase +0x154, mode +0x160, player
// beyond radius + margin +0x430, outside-blend state +0x473, around-player effect id +0x424 (807400/2/4) and its
// handle +0x428 (0x695f80), edge effects (0x692350): tree head +0x410 walked as std::map (node +0 left, +8 parent,
// +0x10 right, +0x19 nil), radius +0x260.
static volatile int g_c553_pre;  // defined with the no-rain hooks below
static int md_map_count(char *head) {
    if (!head || IsBadReadPtr(head, 0x20)) return -1;
    char *n = *(char **)head;
    int k = 0;
    while (n && n != head && !IsBadReadPtr(n, 0x20) && !n[0x19] && k < 1000) {
        k++;
        char *r = *(char **)(n + 0x10);
        if (r && !IsBadReadPtr(r, 0x20) && !r[0x19]) {
            n = r;
            while (*(char **)n && !IsBadReadPtr(*(char **)n, 0x20) && !(*(char **)n)[0x19]) n = *(char **)n;
        } else {
            char *p = *(char **)(n + 8);
            while (p && !IsBadReadPtr(p, 0x20) && !p[0x19] && n == *(char **)(p + 0x10)) n = p, p = *(char **)(n + 8);
            n = p;
        }
    }
    return k;
}
// Player special effects (map-dump, measurement): on every change of +0x430 (beyond radius + margin) the local
// player's active effect entries (ChrIns +0x190 module, list from +8, next at entry +0x38): each row pointer as
// (row - row of 90071610) / 0x400, i.e. its index offset in SpEffectParam (rows 0x400 apart, in id order); the
// ids follow offline from the regulation's row order.
static int md_player_effects_text(char *line, size_t cap, const char *when);
static void md_player_effects(const char *when) {
    char line[1900];
    md_player_effects_text(line, sizeof line, when);
    mdf("%s\n", line);
}
// The same as text (returns its length); also the character's play-area effect id ChrIns +0x780, which the circle
// sets on every character from the phase table (+0x190 entries, +0x18) when outside and -1 inside (0x66dcf0 called
// at 0x697566, 0x698f90, 0x69919c: removes the old effect, applies the new one).
static int md_player_effects_text(char *line, size_t cap, const char *when) {
    char *chr = local_player();
    struct { char *row; unsigned id; unsigned char flag; } ref = {0};
    ((void (*)(void *, unsigned))(g_base + 0xea17e0))(&ref, 90071610);
    char *module = chr && !IsBadReadPtr(chr, 0x790) ? *(char **)(chr + 0x190) : NULL;
    if (!module || !ref.row || IsBadReadPtr(module, 0x10)) return snprintf(line, cap, "player effects (%s): none readable", when);
    int at = snprintf(line, cap, "player effects (%s), play-area effect %d, offsets from row 90071610:", when,
                      *(int *)(chr + 0x780)), n = 0;
    for (char **e = *(char ***)(module + 8); e && n < 64 && !IsBadReadPtr(e, 0x70) && at < (int)cap - 24; e = (char **)e[7], n++)
        at += snprintf(line + at, cap - (size_t)at, " %lld%s", (long long)(e[0] - ref.row) / 0x400,
                       (*(unsigned *)((char *)e + 0x68) & 0x800c0003) ? "(off)" : "");
    return at;
}
// Circle asset log (map-dump, measurement): the play-area code's asset state calls, first time per (site, instance,
// value) and every change: giant assets (0x653be0: 0x7019e0 at 0x653ef4 / 0x654194, level 1 or 4 by the phase
// rule at 0x653e0c), the cloud asset AEG596_500 and the +0x30c assets (0x693930 / 0x694050: 0x704cd0 at 0x693ae8,
// 0x693dc3, 0x6942db, 0x69451e). The game's call is made unchanged.
static void md_asset_note(unsigned site, void *inst, unsigned value) {
    if (!g_md) return;
    static struct { unsigned site; void *inst; unsigned value; } last[256];
    static int n;
    static DWORD seen_for;
    EnterCriticalSection(&g_flog_lock);
    if (seen_for != g_md_t0) seen_for = g_md_t0, n = 0;
    int k = 0;
    while (k < n && !(last[k].site == site && last[k].inst == inst)) k++;
    if (k == n && n < 256) last[n].site = site, last[n].inst = inst, last[n].value = ~value, n++;
    if (k < n && last[k].value != value) {
        last[k].value = value;
        char *pa = *(char **)(g_base + 0x3c13158);
        mdf("%.3f circle asset %x inst %p value %u (phase %d out430 %d)\n", (GetTickCount() - g_md_t0) / 1000.0, site, inst,
            value, pa ? (signed char)pa[0x154] : -9, pa ? pa[0x430] : -1);
    }
    LeaveCriticalSection(&g_flog_lock);
}
static void asset_level_hook(void *inst, unsigned long long level) {
    md_asset_note((unsigned)((char *)__builtin_return_address(0) - g_base) - 5, inst, (unsigned)(level & 0xff));
    ((void (*)(void *, unsigned long long))(g_base + 0x7019e0))(inst, level);
}
static void norain_first(int k, const char *what);
static void asset_enable_hook(void *inst, unsigned long long on) {
    unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
    // No rain (fixed): the circle's far asset AEG599_960_8900 (m60_00_00_99, the distant-view map) is enabled at
    // 0x693ae8 / 0x693dc3 (0x693930), then faded in and placed at the circle centre, scaled to the radius. It is kept
    // disabled (0), as the game disables the cloud asset in 0x694050.
    if (site == 0x693ae8 || site == 0x693dc3) on = 0, norain_first(10, "circle far asset kept disabled");
    md_asset_note(site, inst, (unsigned)(on & 0xff));
    ((void (*)(void *, unsigned long long))(g_base + 0x704cd0))(inst, on);
}
static void md_circle_tick(void) {
    static DWORD next;
    static char last[160];
    char *pa = *(char **)(g_base + 0x3c13158);
    if (!pa || IsBadReadPtr(pa, 0x480)) return;
    char line[160];
    snprintf(line, sizeof line, "c553 %d phase %d mode %u out430 %u out473 %u id %d handle %llx edge %d far %d/%u/%.1f",
             g_c553_pre, (signed char)pa[0x154], (unsigned char)pa[0x160], (unsigned char)pa[0x430], (unsigned char)pa[0x473],
             *(int *)(pa + 0x424), *(unsigned long long *)(pa + 0x428), md_map_count(*(char **)(pa + 0x410)),
             *(int *)(pa + 0x30c), (unsigned char)pa[0x310], *(float *)(pa + 0x314));
    DWORD now = GetTickCount();
    int changed = strcmp(line, last) != 0;
    // effects one second before and one second after each crossing of +0x430 (both directions)
    static int last430 = -1;
    static char snap[1900];
    static DWORD snap_at, after_at;
    DWORD t = GetTickCount();
    if (pa[0x430] != last430) {
        if (last430 >= 0 && snap[0]) mdf("%s (taken %.1f s before the change)\n", snap, (t - snap_at) / 1000.0);
        last430 = pa[0x430];
        md_player_effects(last430 ? "player beyond the circle, now" : "player inside the circle, now");
        after_at = t + 1000;
    } else if (after_at && (int)(t - after_at) >= 0) {
        after_at = 0;
        md_player_effects(last430 ? "player beyond the circle, 1 s after" : "player inside the circle, 1 s after");
    }
    if ((int)(t - snap_at) >= 1000) snap_at = t, md_player_effects_text(snap, sizeof snap, "1 s before the change");
    if (!pa[0x159] && !changed) return;
    if (!changed && (int)(now - next) < 0) return;
    next = now + 1000;
    memcpy(last, line, sizeof last);
    mdf("%.3f circle %s %s radius %.1f\n", (now - g_md_t0) / 1000.0, changed ? "change" : "tick", line, *(float *)(pa + 0x260));
}
// Effect creation log (map-dump, measurement): every call of the effect manager's create (0xedce20, 30 call sites)
// passes through; while an expedition file is open, the first time each (call site, effect id) pair is seen it is
// written with the circle state. Read only: the game's call is made unchanged.
static const unsigned SFX_SITES[] = {0x25826e, 0x353812, 0x41eb8c, 0x447c24, 0x447cb3, 0x53e99e, 0x53eab9, 0x57b65e,
    0x57b924, 0x65ffec, 0x6600f8, 0x6601fe, 0x66067c, 0x664262, 0x664355, 0x66449b, 0x664596, 0x69345e, 0x6937a7,
    0x69517a, 0x6952b1, 0x6960cd, 0x6963d2, 0x72ae96, 0x74178c, 0xb93da8, 0xb93f89, 0xba6673, 0xbf4ba6, 0xbf533c};
static void *sfx_create_hook(void *mgr, void *out, int *id, void *extra) {
    void *r = ((void *(*)(void *, void *, int *, void *))(g_base + 0xedce20))(mgr, out, id, extra);
    if (!g_md || !id || IsBadReadPtr(id, 4)) return r;
    unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
    static unsigned long long seen[512];
    static int nseen;
    static DWORD seen_for;
    unsigned long long key = (unsigned long long)site << 32 | (unsigned)*id;
    EnterCriticalSection(&g_flog_lock);
    if (seen_for != g_md_t0) seen_for = g_md_t0, nseen = 0;
    int k = 0;
    while (k < nseen && seen[k] != key) k++;
    if (k == nseen && nseen < 512) {
        seen[nseen++] = key;
        char *pa = *(char **)(g_base + 0x3c13158);
        int circle = pa && !IsBadReadPtr(pa, 0x480) ? pa[0x159] : -1;
        mdf("%.3f effect created %d at %x (circle %d)\n", (GetTickCount() - g_md_t0) / 1000.0, *id, site, circle);
    }
    LeaveCriticalSection(&g_flog_lock);
    return r;
}
static void md_frame(DWORD expedition) {  // own expedition, InGame, every frame; expedition = its start tick
    if (!setting(S_MAP_DUMP)) return;
    static DWORD opened_for, open_tick, at30, at180, next;
    DWORD now = GetTickCount();
    char *man = flag_man(), *gm = *(char **)(g_base + 0x3c13258);
    if (!man || !gm) return;
    EnterCriticalSection(&g_flog_lock);
    if (opened_for != expedition) {
        opened_for = expedition, open_tick = now;
        SYSTEMTIME t;
        GetLocalTime(&t);
        g_md = md_open(&g_mds, (int)*(unsigned *)(gm + 0xb4c), t.wHour, t.wMinute, t.wSecond);
        logf("map test: map-dump %s %s", g_md ? "writing" : "cannot open", g_mds.name);
        if (g_md) md_entry();
    }
    if (g_md) {
        if (at30 != expedition && now - open_tick > 30000) at30 = expedition, md_flags_on(man, "+30 s after InGame"), md_pins("+30 s after InGame");
        if (at180 != expedition && now - open_tick > 180000) at180 = expedition, md_flags_on(man, "+180 s after InGame"), md_pins("+180 s after InGame");
        md_circle_tick();
        if ((int)(now - next) >= 0) {
            next = now + 1000;
            for (int i = 0; i < 4096; i++)
                if (g_md_seen[i].hidden && now - g_md_seen[i].last > 2000)
                    mdf("%.3f flag %u changed %d more times (now %d)\n", (now - g_md_t0) / 1000.0, g_md_seen[i].id, g_md_seen[i].hidden, g_md_seen[i].value), g_md_seen[i].hidden = 0;
            md_flush(&g_mds);
        }
    }
    LeaveCriticalSection(&g_flog_lock);
}
static void flag_store_hook(char *man, unsigned id, int on) {  // replaces the two calls of 0x60d330
    if (id == 9013 && on && g_settings[S_BOSS_FLOW] == 4) {  // day flow: the game's all-at-once event start (common 1129)
        static DWORD told;
        if (GetTickCount() - told > 60000) told = GetTickCount(), logf("day flow: 9013 (the game's start of every event at once) kept off");
        return;
    }
    int before = flag_state_in(man, id);
    ((void (*)(char *, unsigned, int))(g_base + 0x60d330))(man, id, on);
    flag_note(man, id, on, before);
    if (setting(S_MAP_DUMP)) {
        EnterCriticalSection(&g_flog_lock);
        md_note(id, on, before);
        LeaveCriticalSection(&g_flog_lock);
    }
}
// After a layout: the 16 field tiles as laid out (map id, variant), the tile blocks, and 20 s later every
// ten-digit flag that is on (proves the read of tile flags without a boss kill).
static void flag_layout_log(char *man, int mode) {
    char line[900] = "";
    for (char *e = *(char **)(man + 0x118); e && e < *(char **)(man + 0x120) && strlen(line) < sizeof line - 40; e += 8) {
        unsigned m = *(unsigned *)e;
        snprintf(line + strlen(line), sizeof line - strlen(line), " m%02u_%02u_%02u_%u%u%s", m >> 24, m >> 16 & 0xff, m >> 8 & 0xff, m >> 4 & 0xf, m & 0xf, e[4] ? "" : "(d)");
    }
    logf("flags: layout %d (map load): tiles%s", mode, line);
    static unsigned keys[4096];
    int n = flag_blocks(man, keys, 4096), tiles = 0;
    line[0] = 0;
    for (int i = 0; i < n && i < 4096; i++)
        if (keys[i] >= 1000000 && (i == 0 || keys[i] / 100 != keys[i - 1] / 100) && strlen(line) < sizeof line - 12)
            tiles++, snprintf(line + strlen(line), sizeof line - strlen(line), " %u", keys[i] / 100);
    logf("flags: %d blocks, %d tile block groups (10XYV: ids 10XYVddnnn):%s", n, tiles, line);
}
static void flag_layout_hook(char *man, int mode) {  // replaces the calls of 0x5e6b90 in MoveMapStep
    ((void (*)(char *, int))(g_base + 0x5e6b90))(man, mode);
    EnterCriticalSection(&g_flog_lock);
    for (int i = 0; i < FLOG_SLOTS; i++) g_flog[i].dropped = 0;
    g_flog_back_n = g_flog_back_logged = 0;
    g_flog_layout_tick = GetTickCount();
    g_flog_last_mode = mode;
    md_layout(man, mode);
    LeaveCriticalSection(&g_flog_lock);
    flag_layout_log(man, mode);
}
static void flag_log_frame(void) {  // InGame, every frame
    if (!setting(S_FLAG_LOG)) return;
    static DWORD next;
    DWORD now = GetTickCount();
    if ((int)(now - next) < 0) return;
    next = now + 1000;
    char *man = flag_man();
    if (!man) return;
    EnterCriticalSection(&g_flog_lock);
    for (int i = 0; i < FLOG_SLOTS; i++)
        if (g_flog[i].hidden && now - g_flog[i].last > 2000)
            logf("flags: %u changed %d more times (now %d)", g_flog[i].id, g_flog[i].hidden, g_flog[i].value), g_flog[i].hidden = 0;
    for (int i = 0; i < g_flog_back_n; i++)
        if (g_flog_back[i].t && now - g_flog_back[i].t > 5000) {
            if (g_flog_back_logged++ < 8)
                logf("flags: read back %u (on 5 s ago): game get %d, storage %d", g_flog_back[i].id, two_flag((int)g_flog_back[i].id, 0, 0), flag_state_in(man, g_flog_back[i].id));
            g_flog_back[i].t = 0;
        }
    LeaveCriticalSection(&g_flog_lock);
    static DWORD scanned_for;
    if (scanned_for != g_flog_layout_tick && now - g_flog_layout_tick > 20000) {
        scanned_for = g_flog_layout_tick;
        static unsigned keys[4096];
        int n = flag_blocks(man, keys, 4096), on = 0;
        char line[900] = "";
        for (int i = 0; i < n && i < 4096; i++) {
            if (keys[i] < 1000000) continue;
            char *data = flag_block(man, keys[i]);
            for (unsigned b = 0; data && b < 1000; b++)
                if (data[b >> 3] >> (7 - (b & 7)) & 1) {
                    if (strlen(line) < sizeof line - 14) snprintf(line + strlen(line), sizeof line - strlen(line), " %u", keys[i] * 1000 + b);
                    on++;
                }
        }
        logf("flags: 20 s after the map load, %d tile flags on (read from storage):%s", on, line);
    }
}
// The two regions of the two-region map, as data (decision 35's pair; other pairs need their own rows, and
// their tile sets from 0x6ab3c0: 1 Mountaintop 42-43 x 38-39, 2 Crater 43-44 x 38-39, 3 Rotted Woods 44-45 x
// 36-37, 4 every field tile, 5 Noklateo 42-43 x 36-37 and 44_37): region flag (7601..7606), variant digit, tiles.
typedef struct { int flag, variant, x0, x1, z0, z1, xe, ze; const char *name; } two_region;  // xe/ze: one extra tile (-1 none)
static const two_region TWO_BASE_REGIONS[2] = {{7601, 1, 42, 43, 38, 39, -1, -1, "Mountaintop"}, {7602, 2, 43, 44, 38, 39, -1, -1, "Crater"}};
#define TWO_BASE (TWO_BASE_REGIONS[g_settings[S_PAIR] == 2 || g_settings[S_PAIR] == 3])  // the pattern's own region (map type)
// The region laid over its tiles: pair = 0 the Rotted Woods; pair = 1 (test) Noklateo, with 44_37, which the game's
// table gives to Noklateo and whose variant-5 file it loads once 7605 is in the region list (its tile scripts
// m60_44_37_50 need that tile's own flag space; small bases there stay the Mountaintop's, launcher).
static const two_region TWO_ADDED_PAIRS[2] = {{7603, 3, 44, 45, 36, 37, -1, -1, "Rotted Woods"},
                                              {7605, 5, 42, 43, 36, 37, 44, 37, "Noklateo"}};
#define TWO_ADDED (TWO_ADDED_PAIRS[g_settings[S_PAIR] == 1 || g_settings[S_PAIR] == 3])  // pairs 1 / 3 Noklateo, 0 / 2 the Rotted Woods
static int two_on_tile(const two_region *r, unsigned x, unsigned z) {
    return ((int)x >= r->x0 && (int)x <= r->x1 && (int)z >= r->z0 && (int)z <= r->z1) || ((int)x == r->xe && (int)z == r->ze);
}
static int g_two_active, g_two_requests;  // the current map is a two-regions one (not the hub); map requests
// The tile list (0x5e7d40) asks 0x6ab300(&tile map id, -1) at 0x5e7ff6 whether a tile has flags of its own
// variant (ids 10 X%10 Y%10 V dd nnn) or the base ones (10 XX YY d nnn). 0x6ab3c0 says yes only for the
// tiles of the region of the FIRST flag in gm +0xbb8 (7601: 42..43 x 38..39; 7603 would give 44..45 x
// 36..37). Two-regions: the Rotted Woods tiles (variant 3) say yes too, as on a Rotted Woods map, or every
// flag of their scripts (Captain's death 1056300800, the tree 1046300590, the treasure map 1056304580)
// has no block (run 50).
static unsigned char tile_flag_space_hook(unsigned *id, int a) {
    unsigned char r = ((unsigned char (*)(unsigned *, int))(g_base + 0x6ab300))(id, a);
    unsigned m = id ? *id : 0, x = m >> 16 & 0xff, y = m >> 8 & 0xff;
    if (!r && g_two_active && m >> 24 == 60 && two_on_tile(&TWO_ADDED, x, y) && (int)(m >> 4 & 0xf) == TWO_ADDED.variant) r = 1;
    return r;
}
// Per-tile variant (pair >= 1, test): CS::GameMan::CalcGetReplaceMapIdByDisaster 0x6ab7a0(out, &map id, mode) gives a
// tile or half the variant of the FIRST region flag in gm +0xbb8 whose variant file exists (mode 0; 0x6b4cb0 checks
// the file table). For Mountaintop + Rotted Woods that already gives each tile its region (the Mountaintop has
// variant files only on X 42-43, the Rotted Woods only on X 44-45), but the Crater and Noklateo have variant files
// on all 16 tiles and the Mountaintop on every west tile, so for the other pairs the second region's tiles would
// load the first region's variant. Here (27 call sites, all calls): a field tile (area 60, low nibble 0) of the
// second region's set gets that region's variant; everything else stays the game's choice.
static const unsigned VARIANT_SITES[] = {0x6362a7, 0x6363e6, 0x6363fb, 0x6396ae, 0x6396c3, 0x639ac2, 0x646ce1, 0x646d2f, 0x64a112,
                                         0x64aa02, 0x64b5c0, 0x64cafc, 0x64cb71, 0x64cc25, 0x64d9b7, 0x64db8c, 0x64dc47, 0x64dcf6,
                                         0x64dd98, 0x69ceb3, 0x69cfa2, 0x6ab5c6, 0x6ab63d, 0x6ab723, 0x6b0dcb, 0xbedc9f, 0xe7b48c};
static int g_variant_forced;
static unsigned *variant_hook(unsigned *out, unsigned *id, unsigned long long mode) {
    ((unsigned *(*)(unsigned *, unsigned *, unsigned long long))(g_base + 0x6ab7a0))(out, id, mode);
    if ((mode & 0xff) || !g_two_active || !id) return out;
    unsigned m = *id, x = m >> 16 & 0xff, z = m >> 8 & 0xff;
    if (m >> 24 == 60 && (m & 0xf) == 0 && two_on_tile(&TWO_ADDED, x, z)) {
        unsigned want = (m & 0xffffff00u) | ((unsigned)TWO_ADDED.variant << 4);
        if (*out != want && g_variant_forced++ < 40)
            logf("pair: tile m60_%02u_%02u: variant %u -> %d (%s)", x, z, *out >> 4 & 0xf, TWO_ADDED.variant, TWO_ADDED.name);
        *out = want;
    }
    return out;
}
// No rain (fixed, every player): the raindrops outside the circle and the circle's wall are one effect, the
// play-area effect (GPARAM group "PlayAreaEffect": RainDrop and Border fields, values in mapbase.gparam). The
// render object CS::StageRend (0x3c22750 +0x10 -> 0xe15f20) draws it while its byte +0xc553 is 1; the circle
// code sets that byte every frame while the circle exists: 0x696170 (called only at 0x68e1d2) and 0x698c90
// (only at 0x68e1ea), both under the game's guard StageRend +0x48 bit 19. Only drawing reads it (0x1bbff5a,
// 0x1c1609, 0x1ce655c), never the circle, clock or phase code. So after each of the two calls it goes back to 0.
// The circle's edge sound "CircleBorderSe" (sounds 600010000/10/20 at the edge point nearest the player) is
// started only by 0x6953b0 (called only at 0x68e229): that call is skipped.
static DWORD g_expedition_start_tick;  // defined with g_expedition_tick below
static DWORD g_norain_seen[13];
static DWORD g_enter_tick, g_enter_world;  // joiner: join time, in the host's world (defined with the join code)
static int g_join;
static DWORD norain_key(void) {  // the host's own expedition, or a joiner's stay in the host's world (0: neither)
    return g_join == 4 && g_enter_world ? g_enter_tick | 1 : g_expedition_start_tick;
}
static void norain_first(int k, const char *what) {
    if (k < 0 || k >= (int)(sizeof g_norain_seen / sizeof *g_norain_seen)) return;
    // (until 17.11 joiners never logged: the key was the host-only expedition start, 0 on a joiner)
    DWORD key = norain_key();
    if (g_debug_log && key && g_norain_seen[k] != key) {
        g_norain_seen[k] = key;
        logf("no rain: %s (first time this expedition%s)", what, key == g_expedition_start_tick ? "" : ", joined");
    }
}
static char *stage_rend(void) {
    char *g = *(char **)(g_base + 0x3c22750);
    void *a = g ? *(void **)(g + 0x10) : NULL;
    char *sr = a ? ((char *(*)(void *))(g_base + 0xe15f20))(a) : NULL;
    return sr && (*(unsigned *)(sr + 0x48) >> 19 & 1) ? sr : NULL;
}
static void norain_effect_off(void) {
    char *sr = stage_rend();
    if (sr) sr[0xc553] = 0;
}
// The circle held at full size (owner's go, 17.11): the play-area manager's three phase entries (0x20 each from
// +0x190: +0x10 radius, so +0x1a0 / +0x1c0 / +0x1e0) and the three pending ones the update copies in (+0x200 /
// +0x220 / +0x240) get radius 5000 before each circle update (0x696170 / 0x698c90, our wrappers below), on every
// machine (a joiner's entries come from the host; the update runs everywhere). The current radius +0x260 is the
// update's interpolation of those, so every test of the circle (character outside bit, player inside, the
// per-character map, the phase effect on characters, EcTestIsInsidePlayArea, the map) answers "inside" by the game's
// own geometry. Written only while +0x159 says a circle exists; nothing else is touched. Phases and the clock are
// time-driven and unchanged.
#define CIRCLE_HELD_RADIUS 5000.0f
// The fire ring (owner, BRIEF 2026-11-18: "around the arena the rain comes back as fire"): after this day's challenge
// (day flow, g_df_used) the radius is no longer held: the entries get back the radii the game gave them (kept here
// as they were before each first overwrite) and the game's own circle closes around the arena; the circle's border
// effect 807011 (the one that looked like fire in run 39), its edge sound and the map circle come back; the rain
// (edge 807421/3/5, around the player 807400/2/4) stays off, and so do the play-area effect, the far asset and the
// phase notice. Damage is the game's (launcher: PlayAreaCreateCommonParam's 99810 / 99811 kept; by day the held
// radius leaves nobody outside). Day 2 (7505 clears g_df_used) holds the radius again.
static int ring_on(void) { return g_settings[S_BOSS_FLOW] == 4 && g_df_used; }
// Where the local player is against the circle (run of dev.60: an ally downed at the arena with the ring at 40 m, its
// down timer frozen, the game ended its session 10 s later): the game's own "outside" bit (chr+0x1f9 bit 3, set by
// the character update from 0x68c500), the radius, the circle centre as the manager holds it (+0x250 / +0x254 /
// +0x258, raw) and the player's block and position.
static int player_position(float out[3], unsigned *block);
static char *local_player(void);
static void ring_where(const char *why) {
    char *pa = *(char **)(g_base + 0x3c13158), *me = local_player();
    float p[3] = {0};
    unsigned blk = 0;
    int have = player_position(p, &blk);
    if (!pa || IsBadReadPtr(pa, 0x264)) return;
    logf("fire ring (%s): radius %.1f, centre %.1f %.1f %.1f; us: outside bit %d, block %08x position %s%.1f %.1f %.1f", why,
         *(float *)(pa + 0x260), *(float *)(pa + 0x250), *(float *)(pa + 0x254), *(float *)(pa + 0x258),
         me && !IsBadReadPtr(me + 0x1f9, 1) ? (me[0x1f9] >> 3) & 1 : -1, blk, have ? "" : "(none) ", p[0], p[1], p[2]);
}
static float g_circle_orig[6];
static unsigned char g_circle_have[6];
static void circle_hold(char *mgr) {
    if (!mgr || IsBadReadPtr(mgr, 0x264) || !mgr[0x159]) return;
    static const int OFF[6] = {0x1a0, 0x1c0, 0x1e0, 0x200, 0x220, 0x240};
    static DWORD told_for, next;
    static int ring_was;
    DWORD key = norain_key();
    int ring = ring_on();
    if (ring != ring_was) {
        ring_was = ring;
        if (ring) {
            int back = 0;
            for (int i = 0; i < 6; i++)
                if (g_circle_have[i]) *(float *)(mgr + OFF[i]) = g_circle_orig[i], back++;
            logf("fire ring: the challenge: radius released (%d entries back to the game's: %.1f/%.1f/%.1f, pending %.1f/%.1f/%.1f; current "
                 "+0x260 %.1f); border effect 807011, edge sound and map circle on; rain 807400-807425, play-area effect, far asset "
                 "and phase notice stay off; damage the game's",
                 back, *(float *)(mgr + 0x1a0), *(float *)(mgr + 0x1c0), *(float *)(mgr + 0x1e0), *(float *)(mgr + 0x200),
                 *(float *)(mgr + 0x220), *(float *)(mgr + 0x240), *(float *)(mgr + 0x260));
        } else
            logf("fire ring: off (day 2 or a new expedition): radius held at %.0f again", CIRCLE_HELD_RADIUS);
    }
    if (ring) {
        if ((int)(GetTickCount() - next) >= 0) next = GetTickCount() + 30000, ring_where("every 30 s");
        return;
    }
    if (key && told_for != key) {
        told_for = key, next = GetTickCount() + 30000;
        memset(g_circle_have, 0, sizeof g_circle_have);
        logf("no rain: circle radius held at %.0f (phase radii were %.1f/%.1f/%.1f, current +0x260 %.1f)", CIRCLE_HELD_RADIUS,
             *(float *)(mgr + 0x1a0), *(float *)(mgr + 0x1c0), *(float *)(mgr + 0x1e0), *(float *)(mgr + 0x260));
    } else if (key && (int)(GetTickCount() - next) >= 0)
        next = GetTickCount() + 30000, logf("no rain: circle radius now %.1f (every 30 s)", *(float *)(mgr + 0x260));
    for (int i = 0; i < 6; i++) {
        float v = *(float *)(mgr + OFF[i]);
        if (v != CIRCLE_HELD_RADIUS) g_circle_orig[i] = v, g_circle_have[i] = 1;  // the game's (new phase entries too)
        *(float *)(mgr + OFF[i]) = CIRCLE_HELD_RADIUS;
    }
}
static volatile int g_c553_pre = -1;  // measurement: StageRend +0xc553 just before the circle update (after last frame's clear)
static void norain_circle_hook(char *mgr, float dt, unsigned long long c) {
    circle_hold(mgr);
    char *sr = stage_rend();
    g_c553_pre = sr ? (unsigned char)sr[0xc553] : -1;
    ((void (*)(char *, float, unsigned long long))(g_base + 0x696170))(mgr, dt, c);
    norain_effect_off();
    norain_first(0, "circle update 0x696170, effect switched off");
}
static void norain_circle2_hook(char *mgr, float dt) {
    circle_hold(mgr);
    ((void (*)(char *, float))(g_base + 0x698c90))(mgr, dt);
    norain_effect_off();
    norain_first(1, "circle update 0x698c90, effect switched off");
}
static void norain_sound_hook(char *mgr, float dt) {
    if (ring_on()) return ((void (*)(char *, float))(g_base + 0x6953b0))(mgr, dt);
    norain_first(2, "edge sound 0x6953b0 skipped");
}
static void norain_player_rain_hook(char *mgr) {  // the fire ring: no rain around a player outside it
    (void)mgr;
    norain_first(3, "rain around the player 0x695f80 skipped");
}
// The rain itself (run 38, one-region 3: rain only outside the circle = effect 807400/2/4 active, handle +0x428):
// both circle effects use only s51300_a.tif, the texture of the rain weather effect 806100. 0x695f80 (called
// only at 0x68e208) creates the effect around the player while beyond radius + margin (+0x430); its fields
// +0x424/+0x428 are read by nothing else. 0x692350 (called only at 0x68e213) keeps the rain instances along the
// edge near the player (807421/3/5, list +0x408/+0x410/+0x418, else only a clean-up walk); its callees are
// math (0x68f1d0) and ground queries (0xdac580, 0xdad400). The edge rain is skipped (never created); the rain around the
// player needs no hook: with the radius held it is never beyond the radius (rain-hooks-legacy, retired).
static void norain_edge_rain_hook(char *mgr, float dt) {
    (void)mgr, (void)dt;
    norain_first(4, "rain along the edge 0x692350 skipped");
}
// The border (run 39: still drawn, "like fire", with no edge rain): 0x694580 (called only at 0x68e21e) splits the
// whole circumference into +0x340 = radius * k / +0x32c segments and creates effect 807011 (textures s31021 /
// s34030 / s31031, not the rain texture) at each segment point (0x695123 -> 0xedce20), kept in the container
// +0x358/+0x360 (else only a clean-up walk); its callees are container helpers and math. Skipped: never created.
static void norain_border_hook(char *mgr, float dt) {
    if (ring_on()) return ((void (*)(char *, float))(g_base + 0x694580))(mgr, dt);  // the fire ring's look
    norain_first(5, "circle border effects 0x694580 skipped");
}
// Circle-start notice: when the circle phase (+0x154) changes, 0x696170 calls the HUD (0x3c14a70) at 0x69722c:
// 0x7c52f0(hud, phase) shows text effect 4200 (phase 1, "The Night's tide grows stronger") or 4201 (phase 2, "Rains
// gather, portending the tide of Night") through 0x8b4c30 and does nothing else; its only caller. Skipped.
static void hud_circle_notice_hook(void *hud, unsigned long long phase) {
    (void)hud, (void)phase;
    norain_first(7, "circle notice 0x7c52f0 skipped");
}
// Map pins: 0x6af290(map type T, &map id) decides whether a WorldMapPointParam row gets a pin (pin
// loop 0x910fb3, call 0x911081; second row loop 0xa5bba1). The map id is built by 0x6772b0 from the row's +0x1c..+0x1f:
// byte 3 area, byte 2 gridX, byte 1 gridZ, byte 0 = variant << 4 | ones digit. The game shows a row on a field map
// tile of region T's own terrain iff its variant is T, elsewhere iff its variant is 0. Our maps are the Mountaintop
// (type 1) with the Rotted Woods on 44-45 x 36-37: on those tiles a row is shown iff its variant is 3, as on a real
// Rotted Woods map; everything else goes to the game. Not for the one-region measurement setting.
#define RVA_PIN_ROW_SHOWN 0x6af290
static unsigned long long pin_row_shown_hook(unsigned long long type, unsigned *id) {
    unsigned v = *id, area = v >> 24, x = (v >> 16) & 0xff, z = (v >> 8) & 0xff;
    if (g_settings[S_ONE_REGION] < 1 && area >= 0x3c && area <= 0x58 && (v & 0xf) == 0 && two_on_tile(&TWO_ADDED, x, z)) {
        norain_first(12, "map pins: a woods-tile row decided by the Rotted Woods rule");
        return (int)((v >> 4) & 0xf) == TWO_ADDED.variant;
    }
    return ((unsigned long long (*)(unsigned long long, unsigned *))(g_base + RVA_PIN_ROW_SHOWN))(type, id);
}
// Map circles: the world map view model update 0x140910300 (every model: map screen and compass) asks the play-area
// manager for the current circle (0x14068bab0 at 0x14091079b -> model +0x134 shown, +0x138 centre, +0x140 radius) and
// for the circle of the current phase entry, the next-circle preview (0x14068bcd0 at 0x1409108e6, gated by 0x14068c650
// "shrinking" -> model +0x14c, +0x150, +0x158); the map screen 0x140ac9410 draws each only while its flag is set
// (0x140ac9843 / 0x140ac98c6, else 0x140ad3cc0(.., 0) hides it). Both answered "no circle": neither is drawn. The
// phase-2 marker (+0x168, the final site from manager +0x2f8, 0x14068b8a0 at 0x1409109f6) stays.
static unsigned long long map_no_circle_hook(void *mgr, void *a, void *b, void *c) {
    if (ring_on()) {  // the fire ring on the map
        unsigned site = (unsigned)((char *)__builtin_return_address(0) - g_base) - 5;
        return ((unsigned long long (*)(void *, void *, void *, void *))(g_base + (site == 0x9108e6 ? 0x68bcd0 : 0x68bab0)))(mgr, a, b, c);
    }
    norain_first(8, "map: circle and next-circle preview not drawn");
    return 0;
}
static unsigned *map_request_hook(unsigned *out, unsigned *request) {
    unsigned req = request ? *request : 0;
    unsigned *r = ((unsigned *(*)(unsigned *, unsigned *))(g_base + 0x6b06f0))(out, request);
    char *gm = *(char **)(g_base + 0x3c13258);
    // 0xa000000: the hub (the pattern stays in +0xb4c there; nothing of ours belongs in the hub)
    int one = g_settings[S_ONE_REGION] >= 1 && g_settings[S_ONE_REGION] <= 5;
    g_two_active = !one && gm && req != 0xa000000 && two_pattern(*(unsigned *)(gm + 0xb4c));
    if (one && gm && req != 0xa000000) {
        logf("one-region (test): map request: pattern %u, map type +b46 %d, region list left as the game's", *(unsigned *)(gm + 0xb4c), (int)(unsigned char)gm[0xb46]);
        return r;
    }
    if (!g_two_active) {
        if (gm && req != 0xa000000) logf("two-regions: map request: pattern %u is not a Mountaintop pattern; nothing added", *(unsigned *)(gm + 0xb4c));
        return r;
    }
    g_two_requests++;
    int *flags = (int *)(gm + 0xbb8);
    long long *count = (long long *)(gm + 0xbd0);
    const int WANT[2] = {TWO_BASE.flag, TWO_ADDED.flag};
    for (int w = 0; w < 2; w++) {
        int have = 0;
        for (long long i = 0; i < *count && i < 4; i++) have |= flags[i] == WANT[w];
        if (!have && *count >= 0 && *count < 4) flags[(*count)++] = WANT[w];
    }
    logf("two-regions: map request: pattern %u, map type +b46 %d, +b50 %d, region flags (%lld) %d %d %d %d",
         *(unsigned *)(gm + 0xb4c), (int)(unsigned char)gm[0xb46], *(int *)(gm + 0xb50), *count,
         *count > 0 ? flags[0] : -1, *count > 1 ? flags[1] : -1, *count > 2 ? flags[2] : -1, *count > 3 ? flags[3] : -1);
    logf("two-regions: event flags come from the pattern record (7603 row before 7601, OnMapEnter); none set here");
    return r;
}
// Decision 26: on a two-regions map the player starts with more HP flask charges, so that with every
// church taken the total is at least 7: start = 3 + max(0, 7 - 3 - churches on the pattern). The game:
// player game data = *(GameDataMan 0x3c078d0 + 8); +0xff HP flask charges (maximum), +0x100 FP; set at
// load from CharaInitParam +0xfe (3 for the heroes) through 0x20d9d0 (0x200ed7). A church is script
// 2004[86] -> 0x5fdad0 (0x20d9d0(pgd, +0xff + 1, +0x100), +0x1a8 += 1) and 2004[87] -> 0x60ad60 (fill
// to the new maximum). Churches per pattern: work/native/two-regions.txt from the launcher.
static int g_flasks_final_for = -1;  // the host's start value of its charges is final for this map request
static int two_churches(unsigned pat) {
    char path[MAX_PATH], line[64];
    next_to_dll(path, sizeof path, "two-regions.txt");
    FILE *f = fopen(path, "r");
    int found = -1;
    while (f && fgets(line, sizeof line, f)) {
        unsigned p;
        int c;
        if (line[0] != '#' && sscanf(line, "%u %d", &p, &c) == 2 && p == pat) found = c;
    }
    if (f) fclose(f);
    return found;
}
// debug-log: the churches of this expedition's pattern, once, from the launcher's "church" lines in
// two-regions.txt (attach point, its tile and local x / z from SmallBaseAndSpotAttachPoint). The compass
// word is the church's place on the 4 x 4 field grid m60_42..45_36..39 (centre on the corner of 43/44 and
// 37/38), taking gridX as west -> east and gridZ as south -> north, tile-local x / z with the same axes
// and the tile centre as 0 (an assumption from the ids, not read from code). The player's own position
// (block and raw position) is logged next to it.
static int player_position(float out[3], unsigned *block);
static void two_regions_churches_log(unsigned pat) {
    if (!g_debug_log) return;
    char path[MAX_PATH], line[128];
    next_to_dll(path, sizeof path, "two-regions.txt");
    FILE *f = fopen(path, "r");
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned p, at, area, gx, gz;
        int x, z;
        if (sscanf(line, "church %u %u %u %u %u %d %d", &p, &at, &area, &gx, &gz, &x, &z) != 7 || p != pat) continue;
        float ex = (float)((int)gx - 44) * 256.0f + (float)x, nz = (float)((int)gz - 38) * 256.0f + (float)z;
        const char *ns = nz > 128 ? "north" : nz < -128 ? "south" : "", *ew = ex > 128 ? "east" : ex < -128 ? "west" : "";
        logf("churches: %d: attach point %u, tile m%02u_%02u_%02u, in the tile x %d z %d; on the map: %s%s%s (%.0f m east, %.0f m north of the centre)",
             ++n, at, area, gx, gz, x, z, ns, *ns && *ew ? "-" : "", *ns || *ew ? ew : "centre", ex, nz);
    }
    if (f) fclose(f);
    float pp[3];
    unsigned block = 0;
    if (player_position(pp, &block)) logf("churches: %d on pattern %u; you: block %08x, position %.0f %.0f %.0f", n, pat, block, pp[0], pp[1], pp[2]);
    else logf("churches: %d on pattern %u", n, pat);
}
// Run 44 (pattern 19): 4 churches in the record, the user saw 1 on the map. Every church's map rows
// (WorldMapPointParam 100+ / 2100+, +0x10 the attach point) say tile variant 0, which the pin rule 0x6af290 passes
// for map type 1 on all four tiles (42_37, 43_36, 44_38, 45_39 are not Mountaintop terrain for 0x6ab3c0); their
// attach entities exist in the loaded halves (10_09_12: 101 / 104, 11_09_32: 110 / 117). What is left: IsOpen
// (eventFlagId0 73000 + point must be on; 74000 + point is logged too) and the view model's +0x240 map (std::map,
// key = attach point at node +0x1c, value at +0x20: the pin constructor takes u16 +0 / +2 / +4, u8 +0x17, u16
// +0x20 from it, defaults when the key is missing). Logged each time the map screen is built, per church.
static void church_pins_log(char *model) {
    char *gm = *(char **)(g_base + 0x3c13258), *man = flag_man();
    if (!g_two_active || !gm || !man || !model || IsBadReadPtr(model, 0x250)) return;  // also without debug-log (pattern 16 measurement)
    unsigned pat = *(unsigned *)(gm + 0xb4c);
    char path[MAX_PATH], line[128], out[700] = "";
    next_to_dll(path, sizeof path, "two-regions.txt");
    FILE *f = fopen(path, "r");
    int n = 0;
    char *head = *(char **)(model + 0x240);
    unsigned long long size = *(unsigned long long *)(model + 0x248);
    while (f && fgets(line, sizeof line, f)) {
        unsigned p, at, area, gx, gz;
        int x, z;
        if (sscanf(line, "church %u %u %u %u %u %d %d", &p, &at, &area, &gx, &gz, &x, &z) != 7 || p != pat) continue;
        unsigned num = 0, *np = ((unsigned *(*)(unsigned *, unsigned, unsigned, unsigned, unsigned))(g_base + 0x6772b0))(&num, area, gx, gz, 0);
        unsigned key = np ? *np : num;
        int gate = ((unsigned char (*)(unsigned char, unsigned *))(g_base + 0x6af290))((unsigned char)model[0x60], &key) != 0;
        char *found = NULL;  // lower_bound in the std::map, then key check
        if (head && !IsBadReadPtr(head, 0x20)) {
            char *nd = *(char **)(head + 8), *best = head;
            for (int k = 0; nd && !IsBadReadPtr(nd, 0x30) && !nd[0x19] && k < 64; k++)
                if (*(int *)(nd + 0x1c) < (int)at) nd = *(char **)(nd + 0x10);
                else best = nd, nd = *(char **)nd;
            if (best != head && *(int *)(best + 0x1c) == (int)at) found = best + 0x20;
        }
        char ent[64] = "missing";
        if (found && !IsBadReadPtr(found, 0x22))
            snprintf(ent, sizeof ent, "%u/%u/%u/%u/%u", *(unsigned short *)found, *(unsigned short *)(found + 2), *(unsigned short *)(found + 4),
                     (unsigned char)found[0x17], *(unsigned short *)(found + 0x20));
        snprintf(out + strlen(out), sizeof out - strlen(out), " | %u (m%02u_%02u_%02u): rule %d, %u=%d %u=%d %u=%d, +0x240 %s", at, area, gx, gz,
                 gate, 73000 + at, flag_state_in(man, 73000 + at), 74000 + at, flag_state_in(man, 74000 + at), 73810 + at,
                 flag_state_in(man, 73810 + at), ent);
        n++;
    }
    if (f) fclose(f);
    logf("churches: map pins on pattern %u (map type %u, +0x240 map size %llu): %d churches%s", pat, (unsigned char)model[0x60], size, n, out);
}
static void two_regions_flasks(void) {
    if (g_as_ally && g_join == 4) return;  // an ally's charges come from the host's (enter_watch)
    // Run 44: the log read 3 at the first InGame frame, the user had 4 at the start (relics or the game
    // add to it after that frame). So the count is read once the start flight is over (TWO_FLASK_DELAY_MS
    // into the expedition) and topped up from what it is then: total = charges + churches >= 7.
#define TWO_FLASK_DELAY_MS 30000
    static int done_for = -1, seen_for = -1;
    static DWORD since;
    static int last_hp = -1;
    if (seen_for != g_two_requests) seen_for = g_two_requests, since = GetTickCount(), last_hp = -1;
    if (done_for != g_two_requests) {  // when does the game's start value become final? logged until we adjust
        char *gdm0 = *(char **)(g_base + 0x3c078d0), *pgd0 = gdm0 ? *(char **)(gdm0 + 8) : NULL;
        if (pgd0 && (unsigned char)pgd0[0xff] != last_hp)
            last_hp = (unsigned char)pgd0[0xff], logf("two-regions: flasks: HP charges now %d (%lu ms into the expedition)", last_hp, GetTickCount() - since);
    }
    if (done_for == g_two_requests || GetTickCount() - since < TWO_FLASK_DELAY_MS) return;
    char *gm = *(char **)(g_base + 0x3c13258), *gdm = *(char **)(g_base + 0x3c078d0), *pgd = gdm ? *(char **)(gdm + 8) : NULL;
    if (!gm || !pgd) return;
    done_for = g_two_requests;
    g_flasks_final_for = g_two_requests;
    unsigned pat = *(unsigned *)(gm + 0xb4c);
    two_regions_churches_log(pat);
    int churches = two_churches(pat), extra;
    unsigned char hp = (unsigned char)pgd[0xff], fp = (unsigned char)pgd[0x100];
    // Churches already taken are in the charges (run 48: one taken before this ran was counted twice). The
    // church script raises +0x1a8 by one for each (0x5fdad0), so: charges now + churches not yet taken >= 7.
    int taken = *(int *)(pgd + 0x1a8), left;
    // day flow (boss-flow 4, the owner's rule): the start stays the game's (3) and every church gives its +1 through
    // the game's own church handling (0xfd90e0 -> 0x20d9d0, logged as "church charge"). Deck test of 9330160: this
    // rule raised 3 -> 6 at the start (1 church counted on pattern 45, the owner found 2), the church then 6 -> 7.
    if (g_settings[S_BOSS_FLOW] == 4) {
        logf("two-regions: flasks: %u HP charges at the start, left as the game has it (boss-flow 4: +1 per church)", hp);
        return;
    }
    if (churches < 0) { logf("two-regions: flasks: no church count for pattern %u; left alone", pat); return; }
    left = churches - taken > 0 ? churches - taken : 0;
    extra = 7 - hp - left;
    if (extra <= 0) {
        logf("two-regions: flasks: %u HP charges, %d churches on pattern %u, %d taken; left as it is", hp, churches, pat, taken);
        return;
    }
    if (hp + extra > 14) extra = 14 - hp;
    ((void (*)(char *, unsigned char, unsigned char))(g_base + 0x20d9d0))(pgd, (unsigned char)(hp + extra), fp);
    ((void (*)(void))(g_base + 0x60ad60))();  // fill to the new maximum, as a church does
    logf("two-regions: flasks: HP charges %u -> %u (pattern %u: %d churches counted, %d already taken; at least 7 with the rest)", hp,
         (unsigned char)pgd[0xff], pat, churches, taken);
}
// Item pickup while hosting (runs 38..41: with our host session open, part of the items and all weapons
// showed the prompt but pressing did nothing; with no-host everything could be picked up). The game's
// pickup actions (0x1405c85a0, per frame) build an action entry for each map item near the player and
// ask the action-button manager (0x140b8fc70 -> 0x140b8d870 on 0x143c177d8) whether it is pressed; if so
// the pickup runs (0x1405ca0b0 -> 0x140575490). For the items of the second list (0x140578110), entry
// byte +9 is set when a session exists (CSSessionManager +0x8 handle != 0) and its role (+0x10) is not 7
// (call at 0x1405c92f0); the first list never sets it. Our solo host session has handle 1, role 5, so
// those items got the byte; no-host leaves it 0 and they work. The pickup itself handles a session on
// its own (0x140575490: a client, lobby_state 6, asks the host with packet 0x2c; a host removes the item
// and tells the members, 0x14057fb50). So on the host (lobby_state 3) the byte is cleared before the
// manager reads it. The invader (lobby_state 6) is left as the game has it and only logged (debug-log).
static unsigned char pickup_action_hook(void *mgr, unsigned char *action) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    unsigned state = sm ? *(unsigned *)(sm + SM_LOBBY_STATE) : 0, role = sm ? *(unsigned *)(sm + 0x10) : 0;
    unsigned char before = action ? action[9] : 0;
    static int cleared_told;
    if (action && before && state == 3) {
        action[9] = 0;
        if (cleared_told < 3) cleared_told++, logf("pickup: session byte of a pickup action cleared on the host (lobby_state %u role %u)", state, role);
    }
    unsigned char r = ((unsigned char (*)(void *, unsigned char *))(g_base + 0xb8fc70))(mgr, action);
    if (r && setting(S_DEBUG_LOG))
        logf("pickup: action pressed: entry %08x byte+9 was %u now %u, +0xb %u, lobby_state %u role %u", action ? *(unsigned *)(action + 0x40) : 0,
             before, action ? action[9] : 0, action ? action[0xb] : 0, state, role);
    return r;
}
static void hook_acceptance(void) {
    if (1) hook_call(0xc32971, 0x1fd620, (void *)reward_hook, "reward test: expedition reward");
    if (1) {
        hook_call(0x4f3a98, 0x3d7610, (void *)rescue_add_event_hook, "name test: rescue target log (event)");
        hook_call(0x66e03e, 0x3d7610, (void *)rescue_add_local_hook, "name test: rescue target log (local)");
    }
    if (1)
        for (unsigned i = 0; i < 4; i++) hook_call(CANHIT_SITES[i], 0x436370, (void *)can_hit_hook, "finish test: can-hit log");
    if (1) {
        hook_call(0x84d8d8, 0x90dc80, (void *)map_model_hook, "marker test: world map");
        hook_call(0x84d91e, 0x90dc80, (void *)map_model_hook, "marker test: world map");
        hook_call(0xa4e3c0, 0x90dc80, (void *)map_model_hook, "marker test: world map");
        hook_call(0xad1f00, 0xac53a0, (void *)map_dialog_hook, "marker test: map screen");
        hook_call(0xacedf2, 0xace8e0, (void *)map_list_draw_hook, "marker test: map list draw");
    }
    if (1) {
        for (unsigned i = 0; i < 5; i++) hook_call(RECV_SITES[i], 0x436530, (void *)recv_hit_hook, "finish test: hit receive log");
        hook_call(0x44c6ac, 0x66dec0, (void *)gauge_nohit_hook, "finish test: gauge call log");
    }
    if (1) {
        hook_call(0x674f37, 0x66f3b0, (void *)finish_down_hook, "finish test: going down");
        hook_call(0x43dac3, 0x66dec0, (void *)finish_hit_hook, "finish test: gauge hit");
        if ((g_settings[S_ULT_PVP] && g_settings[S_ULT_PVP] != 100) || g_debug_log) ult_pvp_install();
        rune_install();
        flask_log_install();
        hook_call(0x5186ac, 0x3c7e50, (void *)df_chr_pass_hook, "day flow: enemy tiers (correction re-apply pass)");
        df_death_restart_install();
        pkt_guard_install();
        hook_call(0xba0e42, 0xddb9c0, (void *)pin_read_73, "map pin: pin packet read (1)");
        hook_call(0xba11aa, 0xddb9c0, (void *)pin_read_73, "map pin: pin packet read (2)");
        hook_call(0xba11e6, 0xddb900, (void *)pin_read_74, "map pin: pin reaction read (1)");
        hook_call(0xba130c, 0xddb900, (void *)pin_read_74, "map pin: pin reaction read (2)");
        hook_call(0xc3bb92, 0x7d9f80, (void *)remote_tags_hook, "name tag: over-head names");
        hook_call(0x3db2b9, 0x519660, (void *)squad_count_hook, "scaling: player count (apply)");
        hook_call(0x3c7ee8, 0x519660, (void *)squad_count_hook, "scaling: player count (re-apply check)");
        hook_call(0x3d2d3d, 0x519660, (void *)squad_count_hook, "scaling: player count (creation)");
    }
    hook_call(0xc27a41, 0x6b06f0, (void *)map_request_hook, "two-regions: map request");
    hook_call(0x5e7ff6, 0x6ab300, (void *)tile_flag_space_hook, "two-regions: tile flag space");
    if (g_settings[S_PAIR] >= 1)
        for (unsigned i = 0; i < sizeof VARIANT_SITES / sizeof *VARIANT_SITES; i++)
            hook_call(VARIANT_SITES[i], 0x6ab7a0, (void *)variant_hook, "pair: per-tile variant");
    // the held radius makes the game answer the rain around the player, the health bar's circle state, the weather side
    // and "outside the circle" by itself (rain-hooks-legacy, retired: its 8 hooks went)
    hook_call(0x68e1d2, 0x696170, (void *)norain_circle_hook, "no rain: circle radius held, play-area effect off after the circle update");
    hook_call(0x68e1ea, 0x698c90, (void *)norain_circle2_hook, "no rain: play-area effect off after the second circle update");
    hook_call(0x68e229, 0x6953b0, (void *)norain_sound_hook, "no rain: circle edge sound skipped");
    hook_call(0x68e208, 0x695f80, (void *)norain_player_rain_hook, "no rain: rain around the player skipped (the fire ring)");
    hook_call(0x68e213, 0x692350, (void *)norain_edge_rain_hook, "no rain: rain along the circle edge skipped");
    hook_call(0x68e21e, 0x694580, (void *)norain_border_hook, "no rain: circle border effects skipped");
    hook_call(0x69722c, 0x7c52f0, (void *)hud_circle_notice_hook, "no rain: circle notice skipped");
    hook_call(0x91079b, 0x68bab0, (void *)map_no_circle_hook, "no rain: map circle not drawn");
    hook_call(0x9108e6, 0x68bcd0, (void *)map_no_circle_hook, "no rain: map next-circle preview not drawn");
    hook_call(0x693ae8, 0x704cd0, (void *)asset_enable_hook, "no rain: circle far asset disabled (1)");
    hook_call(0x693dc3, 0x704cd0, (void *)asset_enable_hook, "no rain: circle far asset disabled (2)");
    hook_call(0x911081, RVA_PIN_ROW_SHOWN, (void *)pin_row_shown_hook, "map pins: woods tiles by the Rotted Woods rule (pin loop)");
    hook_call(0xa5bba1, RVA_PIN_ROW_SHOWN, (void *)pin_row_shown_hook, "map pins: woods tiles by the Rotted Woods rule (second row loop)");
    {
        static const unsigned site[11] = {0x68e1d2, 0x68e1ea, 0x68e229, 0x68e208, 0x68e213, 0x68e21e, 0x69722c, 0x693ae8, 0x693dc3, 0x911081,
                                         0xa5bba1},
                              orig[11] = {0x696170, 0x698c90, 0x6953b0, 0x695f80, 0x692350, 0x694580, 0x7c52f0, 0x704cd0, 0x704cd0,
                                          RVA_PIN_ROW_SHOWN, RVA_PIN_ROW_SHOWN};
        int n = 0, pins = 0;
        for (int i = 0; i < 11; i++) {
            unsigned char *c = (unsigned char *)g_base + site[i];
            int rel = (int)(orig[i] - (site[i] + 5));
            int hooked = c[0] == 0xE8 && memcmp(c + 1, &rel, 4) != 0;
            if (i < 9) n += hooked; else pins += hooked;
        }
        logf("no rain: %s (%d of 9 hooks: circle radius held at %.0f and play-area effect off at both circle updates; edge sound, rain around the player and along the edge, border effects and circle notice skipped; far asset disabled (2); after the challenge the fire ring)",
             n == 9 ? "in place" : "NOT complete", n, CIRCLE_HELD_RADIUS);
        logf("map pins: %s (%d of 2 hooks: woods tiles 44-45 x 36-37 show the Rotted Woods rows%s)", pins == 2 ? "in place" : "NOT complete", pins,
             g_settings[S_ONE_REGION] >= 1 ? "; off for one-region" : "");
    }
    hook_call(0x5c92f0, 0xb8fc70, (void *)pickup_action_hook, "pickup: map item action (session byte)");
    InitializeCriticalSection(&g_flog_lock);
    if (setting(S_MAP_DUMP)) {
        for (unsigned i = 0; i < sizeof SFX_SITES / sizeof *SFX_SITES; i++) hook_call(SFX_SITES[i], 0xedce20, (void *)sfx_create_hook, "map-dump: effect creation log");
        hook_call(0x653ef4, 0x7019e0, (void *)asset_level_hook, "map-dump: circle giant asset log (1)");
        hook_call(0x654194, 0x7019e0, (void *)asset_level_hook, "map-dump: circle giant asset log (2)");
        static const unsigned EN[] = {0x6942db, 0x69451e};  // 0x693ae8 / 0x693dc3: hooked always (far asset)
        for (unsigned i = 0; i < 2; i++) hook_call(EN[i], 0x704cd0, (void *)asset_enable_hook, "map-dump: circle asset enable log");
    }
    {  // always: flag-log / map-dump log every write; otherwise only boss-death flags (flag_note)
        hook_call(0x5e789d, 0x60d330, (void *)flag_store_hook, "flags: storage write (0x5e7740)");
        hook_call(0x5e7ac0, 0x60d330, (void *)flag_store_hook, "flags: storage write (0x5e79f0)");
    }
    if (setting(S_FLAG_LOG) || setting(S_MAP_DUMP)) {
        hook_call(0xc303b4, 0x5e6b90, (void *)flag_layout_hook, "flags: layout at map load (keep)");
        hook_call(0xc304f2, 0x5e6b90, (void *)flag_layout_hook, "flags: layout at map load (fresh)");
        hook_call(0xc4a48d, 0x5e6b90, (void *)flag_layout_hook, "flags: layout (other)");
    }
    if (setting(S_LOOT_TEST)) {
        hook_call(0x6fd220, 0x724420, (void *)loot_update_hook, "loot test: pickup objects");
        hook_call(0x72464c, 0x7247d0, (void *)loot_pickup_hook, "loot test: pickup");
        hook_call(0x85d0b2, 0x9827d0, (void *)loot_dialog_hook, "loot test: equipment dialog");
    }
    if (1) {
        hook_call(0x67516f, 0x66f3b0, (void *)finish_revive_hook, "finish test: stand-up");
        hook_call(0x6751e1, 0x4f2d50, (void *)finish_event_hook, "finish test: stand-up event");
    }
    hook_jump(0xdf1a6f, 0xdf1180, (void *)leave_tail_19c0, "session leave log (0xdf19c0)");
    hook_jump(0xdf29e2, 0xdf1180, (void *)leave_tail_2910, "session leave log (0xdf2910)");
    hook_set_step();
    for (unsigned i = 0; i < sizeof LEAVE_SITES / sizeof *LEAVE_SITES; i++)
        hook_call(LEAVE_SITES[i], 0xdf1180, (void *)leave_hook, "session leave log");
    hook_call(0xdf3ebd, 0xdf5840, (void *)leave_timeout_hook, "session leave log (timeout)");
    for (unsigned i = 0; i < HUD_SITE_COUNT; i++) hook_call(HUD_SITES[i], 0x518f30, (void *)hud_lookup_hook, "party HUD lookup");
    hook_call(0x7cef6b, 0x7cfe40, (void *)hud_render_hook, "party HUD slot render");
    hook_call(0xdde88b, RVA_SESSION_SEND_TO, (void *)answer_send_hook, "host join answer send");
    hook_call(0xb4bce6, RVA_GENERATE, (void *)generate_hook_setup, "expedition generation (SingleSetup)");
    hook_call(0xb4d937, RVA_GENERATE, (void *)generate_hook_other, "expedition generation (0xb4cc40)");
    for (unsigned i = 0; i < sizeof SYNC_SEND_SITES / sizeof *SYNC_SEND_SITES; i++)
        hook_call(SYNC_SEND_SITES[i], RVA_SESSION_SEND, (void *)sync_send_hook, "sync request send");
    hook_call(0xdcdd52, RVA_SESSION_READ, (void *)sync_read_hook, "host sync request read");
    hook_call(RVA_MATCH_VALUE_CALL, RVA_MATCH_VALUE, (void *)match_value_hook, "player data receive");
    hook_call(RVA_READY_READ_CALL, RVA_READY_READ, (void *)ready_read_hook, "host ready read");
    hook_call(RVA_GO_SEND_CALL, RVA_GO_SEND, (void *)go_send_hook, "host go send");
    hook_call(RVA_EXPECTED_CALL_JOIN, RVA_EXPECTED_MEMBER, (void *)expected_join_hook, "host join check");
    hook_call(RVA_EXPECTED_CALL_DATA, RVA_EXPECTED_MEMBER, (void *)expected_data_hook, "host player-data check");
    hook_call(RVA_READ_ANSWER_CALL1, RVA_READ_ANSWER, (void *)read_answer_hook, "join answer log");
    hook_call(RVA_READ_ANSWER_CALL2, RVA_READ_ANSWER, (void *)read_answer_hook, "join answer log");
}

// Returns 1 if a leave was requested.
static int host_test_leave(void) {
    int created = g_host_test == 2;
    g_host_test = 0;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!created || !sm) return 0;
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    ((void (*)(void *, unsigned char))(g_base + RVA_SESSION_LEAVE))(sm, 0);
    logf("host test: leave session, lobby_state %u -> %u", state, *(unsigned *)(sm + SM_LOBBY_STATE));
    return 1;
}

// The leave is asynchronous: 0xdf1180 hands it to the implementation (0x2585140 -> 0x258dbd0 queues
// a task on the Steam session object), the session's own step (slot 24, 0x2592480) calls
// ISteamMatchmaking::LeaveLobby, and the manager's update (0xdf2a20) sees the leave handle (+0x2c)
// finish and cleans up to lobby_state 0. Stopping the managers (0xdecb70) goes into protected code
// (0x7475608), so nothing readable shows that a stop still leaves the lobby. So when the search is
// called off from the hub, the network is stopped only once lobby_state is back to "none", or after
// LEAVE_WAIT_MS, checked on every step of the machine.
#define LEAVE_WAIT_MS 5000
static void set_network(int on);
static int join_leave(void);
static DWORD g_stop_pending;  // tick of the leave request; network still up until it is through
static void finish_pending_stop(int now) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    unsigned state = sm ? *(unsigned *)(sm + SM_LOBBY_STATE) : 0;
    DWORD waited = GetTickCount() - g_stop_pending;
    if (!now && state != 0 && state != 2 && state != 5 && waited < LEAVE_WAIT_MS) return;
    logf("host test: leave %s after %lums, lobby_state %u; stopping the network",
         state == 0 || state == 2 || state == 5 ? "done" : now ? "cut short" : "timed out", waited, state);
    g_stop_pending = 0;
    set_network(0);
}

// Must run on the game's thread.
static void set_network(int on) {
    if (!g_flow_offline_step) return;  // without the hook the game would throw the player out
    if (on && !guard_intact()) {
        logf("network: not started, the network guard is not in place");
        return;
    }
    if (!on) {
        host_test_leave();
        join_leave();
        g_stop_pending = 0;
    }
    g_network_on = on;
    if (on) {
        ((void (*)(unsigned char))(g_base + RVA_NET_ENABLE_ALL))(1);
        unsigned char ok = ((unsigned char (*)(int))(g_base + RVA_SIGN_IN))(0);
        logf("network: sign-in %s", ok ? "requested" : "refused (managers not there)");
    } else {
        ((void (*)(void))(g_base + RVA_NET_STOP_ALL))();
        ((void (*)(unsigned char))(g_base + RVA_NET_ENABLE_ALL))(0);
        logf("network: stopped");
    }
}

static void watch_network(void) {
    static unsigned last[7] = {0xffffffff};
    unsigned now[7];
    char *obj[3];
    for (int i = 0; i < 3; i++) {
        obj[i] = *(char **)(g_base + NET_MANAGERS[i].ptr_rva);
        now[i * 2] = obj[i] ? *(unsigned *)(obj[i] + NET_MANAGERS[i].starter + 0x10) : 0xfffffffe;
        now[i * 2 + 1] = obj[i] ? (unsigned char)obj[i][NET_MANAGERS[i].starter + 0x20] : 0xfffffffe;
    }
    now[6] = *(unsigned char *)(g_base + RVA_NET_OFFLINE_FLAG);
    if (!memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    for (int i = 0; i < 3; i++)
        logf("network %d (%s): state %d, enabled %d", i, class_name(obj[i]), (int)now[i * 2], (int)now[i * 2 + 1]);
    logf("network: offline flag %u", now[6]);
}

static unsigned g_squad;  // host: squad size fixed when the lobby is first marked (set below)
static void watch_session(void) {
    static unsigned last_state = 0xffffffff, last_handle = 0xffffffff, last_started = 0xffffffff;
    static unsigned last_players = 0xffffffff;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!sm) return;
    // The game allows three players per session (set once, in the manager's constructor). With the capacity patch
    // ours hold a squad of up to three plus up to two invaders: one player per remote slot plus the local one.
    if (*(unsigned *)(sm + SM_PLAYER_LIMIT) == 3 && SESSION_PLAYERS != 3) {
        *(unsigned *)(sm + SM_PLAYER_LIMIT) = SESSION_PLAYERS;
        logf("session: player limit raised from 3 to %u", SESSION_PLAYERS);
    }
    void **impl = session_impl(sm);
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE), handle = *(unsigned *)(sm + SM_HANDLE);
    unsigned started = impl ? *(unsigned *)((char *)impl + 0x10) : 0;
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    unsigned players = begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0;
    if (state == last_state && handle == last_handle && started == last_started && players == last_players) return;
    last_state = state;
    last_handle = handle;
    last_started = started;
    last_players = players;
    logf("session: lobby_state=%u handle=%u player_limit=%u network_started=%u players=%u", state, handle,
         *(unsigned *)(sm + SM_PLAYER_LIMIT), started, players);
    // Host: the number of invaders follows from the squad that is actually in the session.
    // ponytail: counts every session member as squad; subtract invaders once they can join.
    if (state == 3 && players)
        logf("host: %u session members (squad at marking %u), up to %u invaders allowed", players, g_squad ? g_squad : players,
             invaders_allowed(g_squad ? g_squad : players));
    if (!impl || (state != 3 && state != 6)) return;  // only a host or a client has a session
    typedef char (*export_fn)(void *, unsigned, void *, unsigned *);
    export_fn export_join_data = *(export_fn *)((char *)*impl + IMPL_VT_EXPORT_JOIN_DATA);
    unsigned char buf[64] = {0};
    unsigned size = 0;
    if (!export_join_data(impl, handle, NULL, &size) || size == 0 || size > sizeof buf) {
        logf("session: no join data (size %u)", size);
        return;
    }
    export_join_data(impl, handle, buf, &size);
    char hex[2 * sizeof buf + 1];
    for (unsigned i = 0; i < size; i++) sprintf(hex + 2 * i, "%02x", buf[i]);
    logf("session: join data, %u bytes: %s (as u64: %llu)", size, hex, *(unsigned long long *)buf);
}

// --- Target list entry ----------------------------------------------------------------------------

// Runs in place of the game's call to _CreateBossList: let the game build its list (Nightlords from
// fixed row numbers), then add our row the same way the game adds its own, and move it to the top.
// The game remembers the chosen target as a boss number, and our row has none (it becomes boss 0),
// so on the next visit the menu comes up without Invasion chosen. During a search the choice is put
// back before the list is shown, so that the entry carries its tick: the chosen rows live in two u32 vectors, in the menu's settings object (dialog+0xfc0, vector
// at +0x68) and in the dialog's start request (dialog+0xfc8, vector at +0x50); the game copies the
// first into the second with its vector assignment (0x9e7110), which we use for both.
// A vector here is {allocator, begin, end, capacity end}. The grid is embedded in the dialog at +0xff0.
static int g_searching;      // defined with the search code below
#define DIALOG_FROM_GRID   0xff0
#define DIALOG_SETTINGS    0xfc0
#define DIALOG_REQUEST     0xfc8
#define RVA_VECTOR_ASSIGN  0x9e7110  // vector* (vector *dst, vector *src)
static void rows_text(char *vector, char *out, size_t cap) {
    out[0] = 0;
    if (IsBadReadPtr(vector, 32)) return;
    unsigned *begin = *(unsigned **)(vector + 8), *end = *(unsigned **)(vector + 16);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    if (count > 16 || IsBadReadPtr(begin, count * 4)) return;
    for (size_t i = 0; i < count; i++) snprintf(out + strlen(out), cap - strlen(out), "%s%u", i ? "," : "", begin[i]);
}
static void preselect_invasion(char *grid) {
    char *dialog = grid - DIALOG_FROM_GRID;
    if (IsBadReadPtr(dialog + DIALOG_SETTINGS, 16)) return;
    char *settings = *(char **)(dialog + DIALOG_SETTINGS), *request = *(char **)(dialog + DIALOG_REQUEST);
    if (!settings || !request) return;
    char a[120], b[120];
    rows_text(settings + 0x68, a, sizeof a);
    rows_text(request + 0x50, b, sizeof b);
    int want = g_searching && !g_party;
    logf("target list: chosen rows: settings [%s], request [%s]%s", a, b, want ? "; choosing Invasion" : "");
    if (!want || IsBadReadPtr(settings + 0x68, 32) || IsBadReadPtr(request + 0x50, 32)) return;
    static unsigned ours[1] = {INVASION_ROW};
    void *source[4] = {NULL, ours, ours + 1, ours + 1};
    ((void *(*)(void *, void *))(g_base + RVA_VECTOR_ASSIGN))(settings + 0x68, source);
    ((void *(*)(void *, void *))(g_base + RVA_VECTOR_ASSIGN))(request + 0x50, source);
}

// Diagnostics for the Target list (an entry missing after an invasion): which builder the dialog's
// selector (0x9e7ee5, a fragment) calls, at 0x9e7fd3 (_CreateBossList 0x9e9330) or 0x9e7fcc (the
// second builder 0x9e9b60), and the state an invasion changes, logged with every list shown.
static const char *g_list_builder = "?";
static void list_state_log(const char *who);
static void builder_main_hook(void *dialog) {
    g_list_builder = "_CreateBossList";
    ((void (*)(void *))(g_base + 0x9e9330))(dialog);
}
static void builder_second_hook(void *dialog) {
    g_list_builder = "second builder";
    ((void (*)(void *))(g_base + 0x9e9b60))(dialog);
}
static void show_boss_list_hook(void *grid, char *list, void *a3, void *a4) {
    {
        char *b1 = *(char **)(list + 0x10), *e1 = *(char **)(list + 0x18);
        int has = 0;
        for (char *it = b1; b1 && it + 0x28 <= e1; it += 0x28) has |= *(int *)(it + 0x10) == INVASION_ROW;
        logf("target list: built by %s, %u items, row %d %s", g_list_builder, b1 && e1 >= b1 ? (unsigned)((e1 - b1) / 0x28) : 0,
             INVASION_ROW, has ? "present" : "not present");
        list_state_log("target list");
        g_list_builder = "?";
    }
    preselect_invasion(grid);
    {  // the second builder (_CreateBossList_Debug, 0x9e9b60) may already list our row
        char *b0 = *(char **)(list + 0x10), *e0 = *(char **)(list + 0x18);
        for (char *it = b0; b0 && it + 0x28 <= e0; it += 0x28)
            if (*(int *)(it + 0x10) == INVASION_ROW) {
                logf("target list: Invasion already in the list (%u items)", (unsigned)((e0 - b0) / 0x28));
                ((void (*)(void *, void *, void *, void *))(g_base + RVA_SHOW_BOSS_LIST))(grid, list, a3, a4);
                return;
            }
    }
    _Alignas(16) char item[0x40];  // a MatchingNightBossViewItem is 0x28 bytes
    ((void *(*)(void *, int, unsigned char, unsigned char))(g_base + RVA_BOSS_ITEM_CTOR))(item, INVASION_ROW, 0, 0);
    ((void (*)(void *, void *))(g_base + RVA_BOSS_ITEM_PUSH))(list, item);
    ((void (*)(void *))(g_base + RVA_BOSS_ITEM_DTOR))(item);
    char *begin = *(char **)(list + 0x10), *end = *(char **)(list + 0x18);
    unsigned count = begin && end >= begin ? (unsigned)((end - begin) / 0x28) : 0;
    // Our entry goes first (user's choice): it was appended last, so rotate it to the front.
    // Items are plain data (vtable, id, flags, param row), safe to move as bytes.
    if (count >= 2 && *(int *)(end - 0x28 + 0x10) == INVASION_ROW) {
        char ours[0x28];
        memcpy(ours, end - 0x28, 0x28);
        memmove(begin + 0x28, begin, (size_t)(count - 1) * 0x28);
        memcpy(begin, ours, 0x28);
    }
    logf("target list: Invasion entry added, %u items", count);
    ((void (*)(void *, void *, void *, void *))(g_base + RVA_SHOW_BOSS_LIST))(grid, list, a3, a4);
}

// Executable memory within +-2 GB of the game image, so rel32 calls and rip-relative loads in the
// game's code can be pointed at it. One page, handed out in pieces.
static unsigned char *near_alloc(unsigned size) {
    static unsigned char *page;
    static unsigned used;
    for (int i = 1; i < 0x4000 && !page; i++)
        page = VirtualAlloc(g_base - (SIZE_T)i * 0x10000, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page || used + size > 0x1000) return NULL;
    unsigned char *p = page + used;
    used += (size + 15) & ~15u;
    return p;
}

// Overwrite a 32-bit displacement in the game's code so that it refers to `target`.
// `next` is the address of the instruction after the one being patched.
// Every displacement we write is remembered and checked once a second (code_patch_check): runs 9-11
// lost the fill-context hook after the first full invasion of a game session (no "start request"
// line, the start caught only by the safety net), although nothing of ours undoes it and the only
// way to the start (0x9eb363 -> 0xb67190 -> fill context at 0xb67199 -> 0xb50b70 -> set-step 12 at
// 0xb51035) goes through that call. A changed displacement is logged with what is there now and
// written again.
#define CODE_PATCH_MAX 160
static struct { unsigned char *field; int rel; } g_code_patch[CODE_PATCH_MAX];
static int g_code_patch_count, g_code_patch_rewriting;
static int patch_rel32(unsigned char *field, unsigned char *next, unsigned char *target) {
    int rel = (int)(target - next);
    if (!g_code_patch_rewriting && g_code_patch_count < CODE_PATCH_MAX)
        g_code_patch[g_code_patch_count].field = field, g_code_patch[g_code_patch_count++].rel = rel;
    DWORD old;
    if (!VirtualProtect(field, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(field, &rel, 4);
    VirtualProtect(field, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), field, 4);
    return 1;
}

static void code_patch_check(void) {
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 1000;
    for (int i = 0; i < g_code_patch_count; i++) {
        int now;
        memcpy(&now, g_code_patch[i].field, 4);
        if (now == g_code_patch[i].rel) continue;
        unsigned char *f = g_code_patch[i].field;
        logf("code patch: the displacement at %llx (instruction %02x at %llx) was changed back: now goes to %llx, ours %llx; written again",
             (unsigned long long)(f - (unsigned char *)g_base), f[-1], (unsigned long long)(f - 1 - (unsigned char *)g_base),
             (unsigned long long)(f + 4 + now - (unsigned char *)g_base), (unsigned long long)(f + 4 + g_code_patch[i].rel - (unsigned char *)g_base));
        g_code_patch_rewriting = 1;
        patch_rel32(f, f + 4, f + 4 + g_code_patch[i].rel);
        g_code_patch_rewriting = 0;
    }
}

// Point the game's one "call _CreateBossList" at our function, through a 12-byte stub:
// mov rax, hook; jmp rax.
static void hook_target_list(void) {
    unsigned char *site = (unsigned char *)g_base + RVA_SHOW_BOSS_LIST_JMP;
    int expected = (int)(RVA_SHOW_BOSS_LIST - (RVA_SHOW_BOSS_LIST_JMP + 5));
    if (site[0] != 0xE9 || memcmp(site + 1, &expected, 4)) {
        logf("target list: jump site does not look as expected, not hooking");
        return;
    }
    unsigned char *stub = near_alloc(12);
    if (!stub) {
        logf("target list: no memory near the game image for the stub");
        return;
    }
    void *hook = (void *)show_boss_list_hook;
    stub[0] = 0x48; stub[1] = 0xB8;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 1, site + 5, stub)) logf("target list: hooked (stub at %p)", (void *)stub);
    else logf("target list: cannot patch the jump site");
    // The other builder the dialog uses (0x9e9b60, chosen by a flag in 0x9e7ee5) ends with the same
    // jump to the list view at 0x9e9d82: after an invasion the list came without our entry and without
    // any line from the hook above, so that path is covered too.
    unsigned char *site2 = (unsigned char *)g_base + 0x9e9d82;
    int expected2 = (int)(RVA_SHOW_BOSS_LIST - (0x9e9d82 + 5));
    if (site2[0] == 0xE9 && !memcmp(site2 + 1, &expected2, 4) && patch_rel32(site2 + 1, site2 + 5, stub))
        logf("target list: second builder hooked (0x9e9d82)");
    else logf("target list: second builder jump not as expected, not hooked");
    hook_call(0x9e7fd3, 0x9e9330, (void *)builder_main_hook, "target list builder (_CreateBossList)");
    hook_call(0x9e7fcc, 0x9e9b60, (void *)builder_second_hook, "target list builder (second)");
}

// --- "Number of Players": only "1 player" and "Team" ---------------------------------------------
//
// The option is a CS::ComboItemList<MENU_MATCHING_SORTIE_NUM, 3>: a vtable, three 0x50-byte items
// stored inline at +8 and the item count at +0x100; the menu reads it through its virtual methods
// (1 = count, 2..5 = item by index). The game fills it itself (0x146dbe7b9 -> 0x1409fb0xx): outside its
// own team session [1 player (value 2), 2 players (1), 3 players (0)]; in a team session [2, 3] or [3],
// putting the request's value (+0x49) on a remaining item first. Our vtable shows [1 player, 3 players]
// for the full list and [3 players] for [2, 3]; 130408 reads "Team". Value 1 becomes 0 the same way.
// The list sits at dialog +0x3f58, the request pointer at dialog +0x3e08 (both from the refresh code); the refresh's
// own count calls (returns 0x1409fb070 / 0x1409fb175 / 0x1409fb318) get the real count.

#define RVA_COMBO_LIST_VTABLE     0x2c5efe0  // ComboItemList<MENU_MATCHING_SORTIE_NUM,3>
#define RVA_COMBO_LIST_VTABLE_LEA 0x9f1b7b   // "lea rax,[vtable]" in the dialog's constructor (7 bytes)
#define COMBO_ITEMS   0x08
#define COMBO_ITEM_SIZE 0x50
#define COMBO_COUNT   0x100
#define COMBO_REQUEST (0x3e08 - 0x3f58)      // the dialog's request pointer, from the list

static unsigned combo_count(char *list) {
    unsigned n = *(unsigned *)(list + COMBO_COUNT);
    // the game's own refresh compares the real count (3 / 2 / 1) to decide whether to rebuild the list
    unsigned from = (unsigned)((char *)__builtin_return_address(0) - g_base);
    if (from == 0x9fb070 || from == 0x9fb175 || from == 0x9fb318) return n;
    if (n == 2 || n == 3) {
        char *req = *(char **)(list + COMBO_REQUEST);
        if (req && !IsBadReadPtr(req + 0x49, 1) && (req[0x49] == 1 || req[0x49] == 3)) req[0x49] = 0;  // not shown (3: the old fourth option): "Team"
    }
    return n == 3 ? 2 : n == 2 ? 1 : n;
}

static char *combo_item(char *list, int index) {
    unsigned n = *(unsigned *)(list + COMBO_COUNT);
    if (n == 3 && index == 1) index = 2;  // [1 player, 3 players]
    else if (n == 2 && index == 0) index = 1;  // [3 players]
    char *items = list + COMBO_ITEMS;
    return items + ((0 - (unsigned)(SIZE_T)items) & 7) + (long long)index * COMBO_ITEM_SIZE;
}

static void hook_players_option(void) {
    unsigned char *lea = (unsigned char *)g_base + RVA_COMBO_LIST_VTABLE_LEA;
    void **game_vt = (void **)(g_base + RVA_COMBO_LIST_VTABLE);
    int expected = (int)((unsigned char *)game_vt - (lea + 7));
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x05 || memcmp(lea + 3, &expected, 4)) {
        logf("players option: constructor does not look as expected, not hooking");
        return;
    }
    void **vt = (void **)near_alloc(9 * sizeof(void *));
    if (!vt) {
        logf("players option: no memory near the game image");
        return;
    }
    memcpy(vt, game_vt - 1, 9 * sizeof(void *));  // RTTI pointer, six methods, two spare slots
    vt[1 + 1] = (void *)combo_count;
    vt[1 + 2] = vt[1 + 3] = vt[1 + 4] = vt[1 + 5] = (void *)combo_item;
    if (patch_rel32(lea + 3, lea + 7, (unsigned char *)(vt + 1))) logf("players option: hooked (vtable at %p)", (void *)(vt + 1));
    else logf("players option: cannot patch the constructor");
}

// --- "No. of Password Players": the row is left out ------------------------------------------------
//
// MatchingDialog's constructor 0x1409f1960 adds its rows one after another (0x1409f0470 and others): a row's place
// and widget come from its index (the grid at dialog +0xde8, 0x140781b00). In the usual layout (0x140f8be90 false):
// 0 Number of Players, 1 team session, 2 (0x14092ae10), 3 password, 4 No. of Password Players (add at 0x1409f35ab),
// 5..9 group passwords (icon "Item_%d_0/Widgets/TextInput/Icon" at index i+5, 0x1409f387a), 10 the optional +0x4a
// row (only when [+0x3df8]+0x1d1). We skip the add at 0x1409f35ab and the icon index becomes i+4. The dialog's own
// code reads the cursor (grid +0xd4, 0x140781a50) at the sites below and compares it with fixed old indices (2, 3,
// 4, 5..9 with -5 for the group-password slots, 10); there the cursor is given back in the old numbering (>= 4 ->
// +1). Every other user of the grid (input, drawing, rows by index) sees the real rows. The other layout (add at
// 0x1409f2562) is left as the game builds it. request +0x48 keeps the value the game holds; our team ignores it.
static const unsigned MD_CURSOR_SITES[] = {0x9f7995, 0x9f79a6, 0x9f79b7, 0x9f7af0, 0x9f7b46, 0x9f7f91, 0x9f8251,
                                           0x9f899c, 0x9f8bad, 0x9f8bbe, 0x9f99b0, 0x9facbc, 0x9fae27, 0x9faf18};
static char *g_md_without_row;  // the dialog built without the row
static int md_cursor_old(char *grid) {
    int i = *(int *)(grid + 0xd4);
    return grid == g_md_without_row + 0xde8 && i >= 4 ? i + 1 : i;
}
static void *md_skip_row(char *dialog) {  // in place of 0x1409f0470 for the No. of Password Players row
    g_md_without_row = dialog;
    logf("menu: \"No. of Password Players\" row left out");
    return dialog;
}
static void *md_other_layout_row(char *dialog, void *a, void *b, void *c, void *d, void *e, void *f, unsigned long long g) {
    if (g_md_without_row == dialog) g_md_without_row = NULL;
    return ((void *(*)(char *, void *, void *, void *, void *, void *, void *, unsigned long long))(g_base + 0x9f0470))(dialog, a, b, c, d, e, f, g);
}
static void hook_password_players_row(void) {
    unsigned char *icon = (unsigned char *)g_base + 0x9f387a;
    int ok = !memcmp(icon, "\x44\x8d\x4b\x05", 4);
    unsigned sites[2] = {0x9f35ab, 0x9f2562};
    for (int i = 0; i < 2; i++) {
        int rel = (int)(0x9f0470 - (sites[i] + 5));
        ok = ok && g_base[sites[i]] == (char)0xE8 && !memcmp(g_base + sites[i] + 1, &rel, 4);
    }
    for (unsigned i = 0; i < sizeof MD_CURSOR_SITES / sizeof *MD_CURSOR_SITES; i++) {
        int rel = (int)(0x781a50 - (MD_CURSOR_SITES[i] + 5));
        ok = ok && g_base[MD_CURSOR_SITES[i]] == (char)0xE8 && !memcmp(g_base + MD_CURSOR_SITES[i] + 1, &rel, 4);
    }
    DWORD old;
    if (!ok || !VirtualProtect(icon, 4, PAGE_EXECUTE_READWRITE, &old)) {
        logf("menu: password players row: code not as expected, row kept");
        return;
    }
    icon[3] = 4;
    VirtualProtect(icon, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), icon, 4);
    for (unsigned i = 0; i < sizeof MD_CURSOR_SITES / sizeof *MD_CURSOR_SITES; i++)
        hook_call(MD_CURSOR_SITES[i], 0x781a50, (void *)md_cursor_old, "menu: dialog cursor (old row numbers)");
    hook_call(0x9f2562, 0x9f0470, (void *)md_other_layout_row, "menu: password players row (other layout, kept)");
    hook_call(0x9f35ab, 0x9f0470, (void *)md_skip_row, "menu: password players row (left out)");
}

// --- entry -----------------------------------------------------------------------------------------

// --- Expedition start ---------------------------------------------------------------------------
//
// CS::CSQuickMatchingCtrl is the step machine behind "Begin expedition". Its step table (16-byte
// entries: function, name) sits in writable data; a step's function runs every frame while the
// machine is in that step. We wrap every step, and when the chosen target is our Invasion row we send
// the machine back to its idle step instead of letting an expedition begin.
#define RVA_QM_STEP_TABLE        0x39ae9d0
#define QM_STEP_NONE             0
#define RVA_QM_STEP_NONE_FN            0xb59820
#define RVA_QM_STEP_SEARCH_REGISTER_FN  0xb59880
#define QM_CTRL_CONTEXT          0xa8       // CS::CSQuickMatchLobbyContext*
#define QM_CTRL_VT_SET_STEP      4          // void (ctrl*, int step); the game calls it with 0 on failure
#define RVA_QM_CONTEXT_VTABLE    0x2c75d20
#define QM_CTX_PLAYERS           0x98       // u32 squad size asked for
#define QM_CTX_TARGETS_BEGIN     0xa8       // vector of u32 NightBossMenuParam row ids: begin, end (+8)
#define QM_CTX_TARGET_INDEX      0x178      // u32 index into that vector

typedef void (*step_fn)(void *ctrl, void *time);
#define QM_STEP_COUNT 26
static const char *const STEP_NAMES[QM_STEP_COUNT] = {
    "None", "SearchRegister", "SearchRegister_Wait", "GuestInviteWait", "GuestWaitSession", "GuestReadyWait",
    "HostWaitSesstion", "HostInvite_forKeywordScope", "HostReadyWait_forKeywordScope", "HostInvite",
    "HostReadyWait", "HostMultiGameIdWait", "SingleStart", "MoveMapWait", "HeroReSelect", "HeroReSelectWait",
    "SingleHeroReSelect", "SingleHeroReSelectWait", "SingleSetup", "StartCutscene", "WaitCutscene",
    "MoveMapStart", "MoveMap", "InGame", "FailedStartMultiplay", "Unregister"};
static step_fn g_step_original[QM_STEP_COUNT];

static int g_invasion_pending;  // the request just handed to the machine chose our Invasion row

// One log line describing what the machine is about to start.
static void log_start(char *ctrl, int step) {
    char *ctx = *(char **)(ctrl + QM_CTRL_CONTEXT);
    if (!ctx || *(char **)ctx != g_base + RVA_QM_CONTEXT_VTABLE) {
        logf("step %s: unexpected context object", STEP_NAMES[step]);
        return;
    }
    unsigned *begin = *(unsigned **)(ctx + QM_CTX_TARGETS_BEGIN), *end = *(unsigned **)(ctx + QM_CTX_TARGETS_BEGIN + 8);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    if (count > 64) count = 0;
    char list[400] = "";
    for (size_t i = 0; i < count; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%u", i ? "," : "", begin[i]);
    logf("step %s: bosses [%s] index %u, players %u, invasion %d", STEP_NAMES[step], list,
         *(unsigned *)(ctx + QM_CTX_TARGET_INDEX), *(unsigned *)(ctx + QM_CTX_PLAYERS), g_invasion_pending);
}

// Small on-screen notices: the game's "summon message" box. CS::SummonMsgQueue (in the menu manager)
// holds the message on screen and up to four waiting ones; the box (CS::FeSummonMessageView) fades
// one message in three times, about six seconds in all, then takes the next or hides. So the "searching" line is queued again only when the queue has run empty, which keeps
// it pulsing at the game's own pace without a backlog, and ending the search empties the queue.
#define RVA_MENU_MAN_PTR    0x3c14a70 // CSMenuMan*
#define MENU_MAN_SUMMON_QUEUE 0xd628
#define SMQ_CURRENT_PRIORITY 0x10     // i16 priority of the message on screen, -1 when there is none
#define SMQ_WAITING_LIST    0x58      // CSFixedList<SummonMsgData,4>
#define SMQ_WAITING_COUNT   0x204     // i32
#define RVA_SMQ_CLEAR_WAITING 0x8b38d0  // void (list*)
#define RVA_SMQ_NEXT        0x8b3960  // void (queue*): take the next waiting message, or none
#define RVA_MENU_TEXT_CTOR  0x7ac800  // text* (text *out, int id): CL_MenuText line by id
#define RVA_SUMMON_MESSAGE  0x86b8d0  // bool (text*): queue a summon message (the queue copies the text)
#define TEXT_SEARCHING      131162    // added to CL_MenuText by build_msg.py / the launcher
#define TEXT_SEARCH_CANCELLED 131163
#define TEXT_PARTY_WAIT     131167    // "Waiting for the squad's host..."
#define TEXT_INVADER_LEFT   131168    // "The invader has left"
#define TEXT_YOU_INVADED    131169    // "You have invaded another squad's expedition!"
static char *summon_queue(void) {
    char *menu = *(char **)(g_base + RVA_MENU_MAN_PTR);
    return menu ? menu + MENU_MAN_SUMMON_QUEUE : NULL;
}
// 0 searching, 1 cancelled, 2 party: waiting for the host / joining it, 3 an invader arrived (the game's own line
// 338280 "Another Condemned has invaded!"), 4 the invader died (the game's 336101 "INVADER VANQUISHED"), 5 the
// invader left (ours), 6 the invader's own arrival (ours). The game's lines come in every language it has.
static void notice(int which) {
    static _Alignas(16) char text[7][0x100];  // built once and kept: the game never frees them for us
    static void *ready[7];
    static const int ID[7] = {TEXT_SEARCHING, TEXT_SEARCH_CANCELLED, TEXT_PARTY_WAIT, 338280, 336101, TEXT_INVADER_LEFT, TEXT_YOU_INVADED};
    if (which < 0 || which > 6) return;
    if (!ready[which]) ready[which] = ((void *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(text[which], ID[which]);
    ((unsigned char (*)(void *))(g_base + RVA_SUMMON_MESSAGE))(ready[which]);
}
static int notices_idle(void) {
    char *q = summon_queue();
    return q && *(short *)(q + SMQ_CURRENT_PRIORITY) < 0 && *(int *)(q + SMQ_WAITING_COUNT) == 0;
}
static void clear_notices(void) {
    char *q = summon_queue();
    if (!q) return;
    ((void (*)(void *))(g_base + RVA_SMQ_CLEAR_WAITING))(q + SMQ_WAITING_LIST);
    ((void (*)(void *))(g_base + RVA_SMQ_NEXT))(q);
}
// The invasion search. Choosing Invasion starts it, choosing it again or starting an ordinary
// expedition ends it. While it runs the "searching" notice keeps coming back.
// The search brings the game's network up and its end takes it down again. While it runs, Steam is
// asked for lobbies carrying the mod's marker (lobby_search_frame); nothing is joined yet.
static int g_searching;
static DWORD g_request_tick;  // when the last start request arrived

// --- Steam lobby marker and lobby search --------------------------------------------------------
//
// Through steam_api64.dll's flat API (the game's own copy; exports checked in the shipped DLL, each
// one a jump to the interface slot the game itself uses: ISteamMatchmaking009 SetLobbyData 0xa0,
// GetLobbyData 0x98, RequestLobbyList 0x20, ...; ISteamUtils010 IsAPICallCompleted 0x58 and
// GetAPICallResult 0x68, called by the game the same way for LobbyCreated_t at 0x25919e0).
// Results are polled from the game thread; nothing is registered in the game's callback dispatch.
// The game imports SteamAPI_RunCallbacks / RegisterCallResult (no manual dispatch), and polls its
// own CreateLobby result the same way, so an unregistered call result stays readable this way.
// Lobby type: the game creates its lobby invisible (3), or public (2) when a session flag says so
// (0x2591950: (owner+0xa0 & 0x400) and session+0x60 bit 4). Steam's documentation: public lobbies
// are "returned by search and visible to friends", invisible ones "returned by search, but not
// visible to other friends". Both are listed, so the type is left as the game made it; making it
// public would only add a "join" entry for the host's Steam friends, who may run the vanilla game.
// The game never lists lobbies (its matchmaking slots: 0x68 0x70 0x78 0x88 0x90 0x98 0xa0 0xe0
// 0x118) and its only lobby data key is "inviteParam", so our keys do not meet its own handling.
// Keys: no names, no ids; the squad size and the free invader slots under decision 5 of the brief.
// ponytail: no password yet. It would be one more key, "nrpvp_pass" = a hash of the squad's
// password (never the password), written in lobby_mark next to the others and matched by the
// invader with one more string filter in lobby_search_frame; lobbies without it stay open to all.
#define LOBBY_MARK_KEY   "nrpvp"         // "1" on every mod host lobby
#define LOBBY_PROTO_KEY  "nrpvp_proto"
#define LOBBY_PROTO      "2"             // 2: with nrpvp_ver (proto-1 clients filter on "1" and never see these lobbies)
#define LOBBY_GAME_KEY   "nrpvp_game"
#define LOBBY_GAME       "1.3.3"         // GAME_TIMESTAMP's build
#define LOBBY_SQUAD_KEY  "nrpvp_squad"
#define LOBBY_FREE_KEY   "nrpvp_free"
#define LOBBY_BLOCK_KEY  "nrpvp_block"   // host's map block id, 8 hex digits (ChrIns+0x38)
#define LOBBY_POS_KEY    "nrpvp_pos"     // host's position "x,y,z" from the game's getter 0x3d0fd0
#define LOBBY_GEN_KEY    "nrpvp_gen"     // host's generator results gm +0xb48,+0xb54,+0xb5c (hex)
#define LOBBY_START_KEY  "nrpvp_start"   // host's expedition start block (game manager +0xa74), 8 hex digits
#define LOBBY_ALLY_FREE_KEY "nrpvp_ally_free"  // free places in the host's squad for allies (squad of three)
#define LOBBY_ROLE_KEY   "nrpvp_role"    // a joiner's own lobby member data: "ally" or "invader"
#define LOBBY_VER_KEY    "nrpvp_ver"     // the mod version (MOD_VERSION); only the same version pairs up
#define LOBBY_LEVEL_KEY  "nrpvp_lvl"     // host's level (player data +0x68)
#define LOBBY_PHASE_KEY  "nrpvp_phase"   // host's expedition phase: "hub", "field", "boss" (a night boss fight), "late" (day-2 boss beaten)
#define LOBBY_PAIR_KEY   "nrpvp_pair"    // the host's region pair this launcher start (0..3); only the same pair pairs up
#define LOBBY_BORN_KEY   "nrpvp_born"    // when the lobby was marked: seconds since 2026-01-01 UTC (its age for the pick order)
#define SQUAD_MAX        3               // the game's squad
#define LOBBY_EQUAL      0               // ELobbyComparison
#define LOBBY_WORLDWIDE  3               // ELobbyDistanceFilter
#define LOBBY_MATCH_LIST 510             // LobbyMatchList_t::k_iCallback (500 + 10; LobbyCreated_t is 513)
#define SEARCH_PERIOD_MS 10000
#define SEARCH_TIMEOUT_MS 30000
typedef unsigned long long u64;
static struct {
    void *mm, *utils;  // ISteamMatchmaking*, ISteamUtils*
    unsigned char (*set_data)(void *, u64, const char *, const char *);
    const char *(*get_data)(void *, u64, const char *);
    int (*members)(void *, u64);
    int (*limit)(void *, u64);
    void (*string_filter)(void *, const char *, const char *, int);
    void (*distance_filter)(void *, int);
    u64 (*request_list)(void *);
    u64 (*by_index)(void *, int);
    unsigned char (*completed)(void *, u64, unsigned char *);
    unsigned char (*result)(void *, u64, void *, int, int, unsigned char *);
} g_steam;
static struct {  // optional, for steam_watch
    void *user, *net;
    unsigned char (*logged_on)(void *);
    u64 (*self)(void *);
    u64 (*owner)(void *, u64);
    u64 (*member)(void *, u64, int);
    unsigned char (*p2p)(void *, u64, void *);
    void (*set_member)(void *, u64, const char *, const char *);        // SetLobbyMemberData (own data)
    const char *(*get_member)(void *, u64, u64, const char *);         // GetLobbyMemberData
} g_diag;
static int steam_ready(void) {
    static int state;  // 0 not tried, 1 ready, -1 failed
    if (g_steam_down) return 0;
    if (state) return state > 0;
    HMODULE m = GetModuleHandleA("steam_api64.dll");
    void *(*mm)(void) = m ? (void *(*)(void))GetProcAddress(m, "SteamAPI_SteamMatchmaking_v009") : NULL;
    void *(*utils)(void) = m ? (void *(*)(void))GetProcAddress(m, "SteamAPI_SteamUtils_v010") : NULL;
    static const char *const names[] = {
        "SteamAPI_ISteamMatchmaking_SetLobbyData", "SteamAPI_ISteamMatchmaking_GetLobbyData",
        "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
        "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit", "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter",
        "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter", "SteamAPI_ISteamMatchmaking_RequestLobbyList",
        "SteamAPI_ISteamMatchmaking_GetLobbyByIndex", "SteamAPI_ISteamUtils_IsAPICallCompleted",
        "SteamAPI_ISteamUtils_GetAPICallResult"};
    void **fn = (void **)&g_steam.set_data;
    state = mm && utils ? 1 : -1;
    for (unsigned i = 0; state > 0 && i < sizeof names / sizeof *names; i++)
        if (!(fn[i] = m ? (void *)GetProcAddress(m, names[i]) : NULL)) {
            logf("steam: %s not exported", names[i]);
            state = -1;
        }
    if (state > 0) {
        g_steam.mm = mm();
        g_steam.utils = utils();
        if (!g_steam.mm || !g_steam.utils) state = -1;
    }
    // Lobbies are per Steam app: a game reporting another app id (a non-Steam copy on Spacewar 480, say)
    // never sees lobbies of Steam copies of Nightreign, and they never see its lobbies.
    unsigned (*app_id)(void *) = m ? (unsigned (*)(void *))GetProcAddress(m, "SteamAPI_ISteamUtils_GetAppID") : NULL;
    unsigned app = state > 0 && app_id ? app_id(g_steam.utils) : 0;
    logf("steam: %s (matchmaking %p, utils %p), app id %u%s", state > 0 ? "ready" : "NOT available", g_steam.mm, g_steam.utils,
         app, app == 2622380 ? " (Nightreign)" : " - NOT Nightreign's 2622380: lobbies of Steam copies are not seen");
    if (state > 0) {  // for the connection log only; missing ones just leave it shorter
        void *(*user)(void) = (void *(*)(void))GetProcAddress(m, "SteamAPI_SteamUser_v021");
        void *(*net)(void) = (void *(*)(void))GetProcAddress(m, "SteamAPI_SteamNetworking_v006");
        g_diag.user = user ? user() : NULL;
        g_diag.net = net ? net() : NULL;
        g_diag.logged_on = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_BLoggedOn");
        g_diag.self = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_GetSteamID");
        g_diag.owner = (void *)GetProcAddress(m, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        g_diag.member = (void *)GetProcAddress(m, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
        g_diag.p2p = (void *)GetProcAddress(m, "SteamAPI_ISteamNetworking_GetP2PSessionState");
        g_diag.set_member = (void *)GetProcAddress(m, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
        g_diag.get_member = (void *)GetProcAddress(m, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
        if (!g_diag.set_member || !g_diag.get_member) logf("steam: lobby member data not exported: every joiner counts as an invader");
        if (!g_diag.user || !g_diag.net || !g_diag.logged_on || !g_diag.self || !g_diag.owner || !g_diag.member || !g_diag.p2p)
            logf("steam: connection log incomplete (an export is missing)");
    }
    return state > 0;
}

// Connection log (host and joiner, every STEAM_WATCH_MS, a line when anything changed): whether this
// Steam client is logged on to Steam, the lobby's member count and limit as this client sees them,
// the owner (a 16-bit tag of its id, the same on both sides, so the logs can be matched without
// writing ids; "us" when it is this player), and the P2P session (ISteamNetworking 006, the one the
// game uses) with each other member: a active, c connecting, e error, r relay.
#define STEAM_WATCH_MS 5000
static unsigned id_tag(u64 id) {
    unsigned h = 2166136261u;
    for (int i = 0; i < 8; i++) h = (h ^ (unsigned char)(id >> (i * 8))) * 16777619u;
    return (h ^ h >> 16) & 0xffff;
}
static void steam_watch(const char *who, u64 lobby) {
    static DWORD next;
    static char last[256];
    if (!lobby || !g_diag.user || !g_diag.net || !g_diag.logged_on || !g_diag.self || !g_diag.owner || !g_diag.member ||
        !g_diag.p2p || (int)(GetTickCount() - next) < 0)
        return;
    next = GetTickCount() + STEAM_WATCH_MS;
    u64 me = g_diag.self(g_diag.user), owner = g_diag.owner(g_steam.mm, lobby);
    int n = g_steam.members(g_steam.mm, lobby);
    char text[256];
    int at = snprintf(text, sizeof text, "logged on %u, members %d of %d, owner %s", g_diag.logged_on(g_diag.user), n,
                      g_steam.limit(g_steam.mm, lobby), !owner ? "none" : owner == me ? "us" : "");
    if (owner && owner != me) at += snprintf(text + at, sizeof text - at, "%04x", id_tag(owner));
    for (int i = 0; i < n && i < 6 && at < (int)sizeof text - 40; i++) {
        u64 peer = g_diag.member(g_steam.mm, lobby, i);
        if (!peer || peer == me) continue;
        struct { unsigned char active, connecting, error, relay; int bytes, packets; unsigned ip; unsigned short port; } st = {0};
        if (g_diag.p2p(g_diag.net, peer, &st))
            at += snprintf(text + at, sizeof text - at, "; p2p %04x a%u c%u e%u r%u", id_tag(peer), st.active, st.connecting,
                           st.error, st.relay);
        else at += snprintf(text + at, sizeof text - at, "; p2p %04x none", id_tag(peer));
    }
    if (strcmp(text, last)) {
        snprintf(last, sizeof last, "%s", text);
        logf("%s: steam: %s", who, text);
    }
}

// Session member flags, both sides, a line when any changes. Per member of the session manager
// (+0x78, 0xf0 each): +0xe1 us, +0xe2 remote, +0xe3, +0xe4, +0xe8, +0xe9, +0xd4 player number.
// The client's MoveMap sends ready 0x21 only when every other member has +0xe4, or +0xe8 and +0xe9 and
// +0xd4 != -1 (0xdf0cb0); sync step 17 WaitNetworkChrIns waits until no remote member lacks +0xe4
// (0xdf03a0; 120 s, then it leaves). Also the second manager at 0x3c21fc8 (it reads and sends the
// 0x38-byte player info packet 0x93, 0xdf9830 / 0xdfa270, and marks a member's +0xe4 there): its
// first word and each member's +0xe4.
#define RVA_PARTY_MAN_PTR 0x3c21fc8
static void member_flags_log(const char *who, char *sm) {
    static char last[400];
    char text[400];
    int at = 0;
    char *b = *(char **)(sm + 0x78), *e = *(char **)(sm + 0x80);
    for (int i = 0; b && e > b && b + i * 0xf0 < e && i < 6; i++) {
        char *m = b + i * 0xf0;
        // slot state: byte [slot +0xd0] of the table at *(0x3c078d0)+0x18; the in-world step 0xc43900
        // creates a waiting member's character (and clears its +0xe2) once it is 0x1f (0x1feab0)
        char *pd = *(char **)(g_base + 0x3c078d0);
        int slot = *(int *)(m + 0xd0), st = -1;
        if (pd && slot >= 0 && slot < 8 && *(char **)(pd + 0x18) && !IsBadReadPtr(*(char **)(pd + 0x18) + slot, 1))
            st = (unsigned char)(*(char **)(pd + 0x18))[slot];
        at += snprintf(text + at, sizeof text - at,
                       "%s[us %u wait %u e3 %u e4 %u e8 %u e9 %u rejoin %u asks %02x/%02x no %d slot %d state %02x]",
                       i ? " " : "", (unsigned char)m[0xe1], (unsigned char)m[0xe2], (unsigned char)m[0xe3],
                       (unsigned char)m[0xe4], (unsigned char)m[0xe8], (unsigned char)m[0xe9], (unsigned char)m[0xea],
                       (unsigned char)m[0xeb], (unsigned char)m[0xec], *(int *)(m + 0xd4), slot, st & 0xff);
    }
    char *pm = *(char **)(g_base + RVA_PARTY_MAN_PTR);
    if (pm && !IsBadReadPtr(pm, 0x40)) {
        char *pb = *(char **)(pm + 0x30), *pe = *(char **)(pm + 0x38);
        at += snprintf(text + at, sizeof text - at, " | party man %d:", *(int *)pm);
        for (int i = 0; pb && pe > pb && pb + i * 0xf0 < pe && i < 6 && at < (int)sizeof text - 20; i++)
            at += snprintf(text + at, sizeof text - at, " e4 %u", (unsigned char)pb[i * 0xf0 + 0xe4]);
    }
    if (strcmp(text, last)) {
        snprintf(last, sizeof last, "%s", text);
        logf("%s: members: %s", who, text);
    }
}
// Host: the player-state exchange with a member who joins mid-expedition. Each side asks the members
// it waits for (+0xe2) for their state with packet 10 (0xdf20c0 -> 0xddeb50; the game does it in
// MoveMapStep's CreateDrawPlan, 0xc334e0, i.e. while loading a map). The answer (0xdde260: level 0xb,
// 8, 0x3d, equipment 0xc through 0xddfca0) is sent only while the session role (+0x10) is 7; before
// that requests are queued and answered when it becomes 7. Role 7 is set only by 0xdf28e0, from
// MoveMapStep's HorseWait / TexStabilizeWait: a map load inside the session. Our host loaded its map
// before its session existed, so it stays at 5, queues the joiner's request and never answers; and it
// never loads a map again, so it never asks the joiner either. Those four packets set bits 0x10, 2, 4,
// 8 of the member's slot state (0x202f10, 0x202f50, 0x202f70, 0x202f30; bit 1 at the join,
// 0x200aa0); at 0x1f the in-world step creates the member's character. So: once a member has been
// admitted and waits (+0xe2), role 5 -> 7 with the game's own setter (not before, so the admission runs
// in the role it always ran in), then packet 10 to each newly waiting member.
#define RVA_SM_SET_INGAME     0xdf28e0  // sm+0x10 = 7, sm+0x74c = 0
#define RVA_SM_REQUEST_STATE  0xdf20c0  // packet 10 to every member with +0xe2
static int g_host_switched, g_host_old_dc;
static char *g_hud_invaders[4];
static int our_speffect_active(char *chr);
// Host: the invader's character, once it exists. Network players are in the player set at the world
// character manager +0x9a40: vt[0] the count, vt[1](set, i) the i-th ChrIns, as the play-area update
// walks it (0x696170); the network character is created into it by 0x518280 (0xc445a0). Logged on
// change: base team +0x74, effective team (0x3d1b30) and whether our team SpEffect is on it.
static void remote_chr_log(void) {
    static DWORD next;
    static char last[300] = "?";
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 5000;
    char *wcm = *(char **)(g_base + 0x3c0f0a8), *me = wcm ? *(char **)(wcm + 0x174e8) : NULL;
    if (!wcm) return;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    char text[300] = "";
    for (int i = 0; i < count && i < 8; i++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i);
        if (!c || c == me || IsBadReadPtr(c, 0x200)) continue;
        unsigned char team = 0;
        ((unsigned char *(*)(void *, unsigned char *))(g_base + 0x3d1b30))(c, &team);
        snprintf(text + strlen(text), sizeof text - strlen(text), " [%d: base team %u effective %u our effect %d]", i,
                 (unsigned char)c[0x74], team, our_speffect_active(c));
    }
    if (strcmp(text, last)) {
        snprintf(last, sizeof last, "%s", text);
        logf("host: other player characters (%d in the player set):%s", count, text[0] ? text : " none");
    }
}
// Decision 21 (finish-test): the host killed while an invader is in its session loses. A down that
// ends in death goes from state 1 straight to 0 (a revive goes 1 -> 2); with an invader admitted
// (g_host_switched) such a death ends the host's expedition through the same request the invader's
// way home uses (gm+0xaf1, 0x6af8f0): InGameStep then runs Result / EndFlow back to the Roundtable
// Hold, the session closes with the world, and the invader's "no longer a client" path sends it home.
// ponytail: a solo host only; with squad mates the condition must become "the whole squad is dead".
// Both sides (finish-test): the downed state of the other player characters as this machine sees it,
// one line per change (0 none, 1 down, 2 revived; 1 -> 0 is a death).
static void other_down_watch(const char *side) {
    static char *chr_[8];
    static int last_[8];
    char *wcm = *(char **)(g_base + 0x3c0f0a8), *me = wcm ? *(char **)(wcm + 0x174e8) : NULL;
    if (!wcm) return;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    for (int i = 0; i < count && i < 8; i++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i);
        if (!c || c == me || IsBadReadPtr(c, 0x740)) continue;
        int st = (unsigned char)c[0x710];
        if (chr_[i] != c) chr_[i] = c, last_[i] = st;
        else if (st != last_[i]) {
            if (last_[i] == 1 && st == 0 && g_invader_in_world() && our_speffect_active(c) != 1) g_host_killed = 1;
            logf("finish test (%s): other player %d (%s): downed state %d -> %d%s", side, i,
                 our_speffect_active(c) == 1 ? "the invader" : "not ours", last_[i], st, last_[i] == 1 && st == 0 ? " (died)" : "");
            last_[i] = st;
        }
    }
}
static void host_death_watch(void) {
    if (1) other_down_watch("host");
    static int last = -1;
    char *chr = local_player();
    int now = chr && !IsBadReadPtr(chr, 0x740) ? (unsigned char)chr[0x710] : -1;
    // with an ally in the session the squad lives on: left to the game (its own rule for a dead squad member)
    // day flow (boss-flow 4, the owner's rule): the host's final death (downed, then finished or bled out) ends the
    // expedition for the whole squad, whoever is in the session
    if (last == 1 && now == 0 && (g_settings[S_BOSS_FLOW] == 4 || (g_host_switched && side_count(0) > 0 && side_count(1) == 0))) {
        ((void (*)(void))(g_base + 0x6af8f0))();
        char *gm = *(char **)(g_base + 0x3c13258);
        logf("finish test: the host's final death: expedition end requested (gm+0xaf1 = %u)",
             gm ? (unsigned char)gm[0xaf1] : 0);
    }
    last = now;
}
// name-test: the HUD shows another player's character name only when 0x7b14c0(chr) says so: the
// game's name option (play state +0x98, 0x207900) and either a local character or a valid platform id
// at *(chr+0x770)+0x10 (0x66e620) that the block list (0xdcba00) does not name; otherwise "Player"
// (0x7ade50). Logged per other player on change: the option, the id (16-bit tag), the result, and our
// session member's Steam id (16-bit tag). The fill: an empty id of a remote player in our session gets
// that member's Steam id (member +0x10), with a log line before and after.
// ponytail: one remote member per session (an invasion of one); several invaders need the mapping
// from character to member (not read).
static void name_watch(char *sm, const char *side, int fill) {
    static unsigned long long last_key[8];
    char *wcm = *(char **)(g_base + 0x3c0f0a8), *me = wcm ? *(char **)(wcm + 0x174e8) : NULL;
    if (!wcm || !sm) return;
    unsigned long long member = 0;
    int remote = 0;
    char *b = *(char **)(sm + SM_PLAYERS_BEGIN), *e = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    for (char *m = b; b && m < e && !IsBadReadPtr(m, SM_PLAYER_SIZE); m += SM_PLAYER_SIZE)
        if (!m[0xe1]) member = *(unsigned long long *)(m + 0x10), remote++;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    char *opt = *(char **)(g_base + 0x3c078d0) ? *(char **)(*(char **)(g_base + 0x3c078d0) + 0x98) : NULL;
    for (int i = 0; i < count && i < 8; i++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i);
        if (!c || c == me || IsBadReadPtr(c, 0x780)) continue;
        char *net = *(char **)(c + 0x770);
        if (!net || IsBadReadPtr(net, 0x18)) continue;
        unsigned long long *id = (unsigned long long *)(net + 0x10);
        if (fill && *id == 0 && remote == 1 && member) {
            logf("name test (%s): player %d: platform id empty; set to our session member's Steam id (tag %04x)", side, i,
                 (unsigned)(member & 0xffff));
            *id = member;
            logf("name test (%s): player %d: platform id now tag %04x", side, i, (unsigned)(*id & 0xffff));
        }
        int shown = ((char (*)(void *))(g_base + 0x7b14c0))(c) != 0;
        unsigned long long key = (*id & 0xffff) | (unsigned long long)shown << 16 | (unsigned long long)(opt ? (unsigned char)opt[0x15] : 0xff) << 17 |
                                 (member & 0xffff) << 32 | (unsigned long long)remote << 48 | 1ull << 56;
        if (key == last_key[i]) continue;
        last_key[i] = key;
        logf("name test (%s): player %d: name option byte %u (0 = names on), platform id tag %04x, real name shown %d | "
             "session: %d remote member(s), member Steam id tag %04x", side, i, opt ? (unsigned char)opt[0x15] : 0xff,
             (unsigned)(*id & 0xffff), shown, remote, (unsigned)(member & 0xffff));
    }
}
// name-test diagnostic (player tag on the invader): the front end adds a player's tag (bar, name,
// rescue gauge) when that player's HP in the play state's other-player slot drops (0x7c5710: slot =
// 0x1fe530(i) = *(play state +0x10) + i * 0x8a0; +8 handle, +0x10 HP) and the local player counts it as an
// enemy. The slots are filled from the session's packets (0xddad10 packet 0xc, 0xddd680 packet 0xb,
// 0xde32a0 packet 0x62, ...) into the slot named by the member's +0xd0. Logged every 2 s on change, both
// sides: each slot's handle and HP, and the HP of the character with that handle as this machine sees it.
static void slot_hp_watch(const char *side) {
    static DWORD next;
    static int last[4][2] = {{-9, -9}, {-9, -9}, {-9, -9}, {-9, -9}};
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 2000;
    char *ps = *(char **)(g_base + 0x3c078d0), *wcm = *(char **)(g_base + 0x3c0f0a8);
    char *slots = ps ? *(char **)(ps + 0x10) : NULL;
    if (!slots || !wcm || IsBadReadPtr(slots, g_cap_remote * 0x8a0)) return;
    for (int i = 0; i < g_cap_remote; i++) {
        char *sl = slots + i * 0x8a0;
        int hp = *(int *)(sl + 0x10), chp = -1;
        unsigned long long h = *(unsigned long long *)(sl + 8);  // run 19: the player number, not a handle
        char *c = ((char *(*)(void *, int))(g_base + 0x5197e0))(wcm, (int)h);  // lookup by player number, as 0x7c5710
        if (c && !IsBadReadPtr(c, 0x1c0)) {
            char **pd = *(char ***)(c + 0x1b8);
            if (pd && !IsBadReadPtr(pd, 8) && *pd && !IsBadReadPtr(*pd, 0x148)) chp = *(int *)(*pd + 0x140);
        }
        if (hp == last[i][0] && chp == last[i][1]) continue;
        last[i][0] = hp, last[i][1] = chp;
        logf("name test (%s): other-player slot %d: player number %lld, slot HP %d, character %s HP %d", side, i, (long long)(int)h, hp,
             c ? (c == *(char **)(wcm + 0x174e8) ? "(local)" : "found") : "not found", chp);
    }
}
static void host_member_sync(char *sm) {
    static unsigned long long for_lobby;
    static int last_waiting;
    if (for_lobby != g_lobby) for_lobby = g_lobby, last_waiting = 0, g_admit_kind = 0, g_host_switched = 0;
    if (!(g_host_test == 2 && g_network_on && guard_intact())) return;
    int waiting = 0, remote = 0;
    char *b = *(char **)(sm + 0x78), *e = *(char **)(sm + 0x80);
    for (char *m = b; b && m < e; m += 0xf0) waiting += !m[0xe1] && m[0xe2], remote += !m[0xe1];
    // Only once the member's admission is complete on our side: the join check answered (bit 1) and
    // its player data 0x60 checked and applied (bit 2), and a second later. The run of b593722b showed
    // that switching as soon as the member appears refuses it (answer 2). With nobody of ours left in
    // the session the role goes back to 5, so the next arrival is admitted in role 5 again.
    // The join check is enough: a member who was in this session before sends no new player data 0x60
    // (run 0a6833c2: second arrival, no 0x60, no player-data check), its data is already there.
    // Admission: the join check passed a second ago, or (run 10b: an arrival for which the game ran no
    // join check we could see) the waiting member has stayed in the session for two seconds. Every
    // new arrival gets one line with the state of this decision.
    static DWORD waiting_since;
    if (!waiting) waiting_since = 0;
    else if (!waiting_since) {
        waiting_since = GetTickCount();
        logf("host: admission: a member waits (role %d, join check %s)", *(int *)(sm + 0x10),
             (g_admit_kind & 1) ? "passed" : "not seen yet");
    }
    int admitted = ((g_admit_kind & 1) && GetTickCount() - g_admit_tick > 1000) ||
                   (waiting_since && GetTickCount() - waiting_since > 2000);
    if (waiting && *(int *)(sm + 0x10) == 5 && admitted) {
        logf("host: admission: switching (%s)", (g_admit_kind & 1) ? "join check passed" : "member stayed 2 s in the session");
        ((void (*)(void *))(g_base + RVA_SM_SET_INGAME))(sm);
        logf("host: member admitted; session role 5 -> %d (0xdf28e0, as a map load inside the session sets it)",
             *(int *)(sm + 0x10));
        // a group host's play mode +0xdc/+0xdd is 2 (multiplayer); a solo host's is its context mode, and
        // the equipment packet 0xc goes out only at 2 (0xddfca0)
        char *pd = *(char **)(g_base + 0x3c078d0);
        if (pd && pd[0xdc] != 2) {
            g_host_old_dc = (unsigned char)pd[0xdc] | 0x100;
            ((void (*)(unsigned char))(g_base + 0x202230))(2);
            play_state_log("host, member admitted");
        }
    }
    if (!remote && *(int *)(sm + 0x10) == 7 && g_host_switched) {
        *(int *)(sm + 0x10) = 5;
        g_admit_kind = 0;
        g_host_switched = 0;
        if (g_host_old_dc) {
            ((void (*)(unsigned char))(g_base + 0x202230))((unsigned char)g_host_old_dc);
            g_host_old_dc = 0;
            play_state_log("host, no member left");
        }
        logf("host: no member left; session role back to 5 for the next arrival");
        memset(g_hud_invaders, 0, sizeof g_hud_invaders);
    }
    if (*(int *)(sm + 0x10) == 7) g_host_switched = 1;
    host_death_watch();
    if (1) name_watch(sm, "host", g_host_switched), slot_hp_watch("host"), other_downed_watch("host");
    if (g_host_switched) remote_chr_log();
    // packet 10 to each newly waiting member, once the role is 7 (requests are answered only then)
    if (*(int *)(sm + 0x10) == 7 && waiting > last_waiting) {
        ((void (*)(void *))(g_base + RVA_SM_REQUEST_STATE))(sm);
        logf("host: state request (packet 10) sent to %d waiting member(s) (0xdf20c0)", waiting);
        last_waiting = waiting;
    }
    if (waiting < last_waiting) last_waiting = waiting;
}
// The session's join data is the lobby id (0xdf0560: implementation vt+0x40; 8 bytes from the Steam
// session object's +0x870).
static u64 session_lobby_id(char *sm) {
    void **impl = session_impl(sm);
    if (!impl) return 0;
    typedef char (*export_fn)(void *, unsigned, void *, unsigned *);
    export_fn export_join_data = *(export_fn *)((char *)*impl + IMPL_VT_EXPORT_JOIN_DATA);
    unsigned char buf[64] = {0};
    unsigned size = 0;
    unsigned handle = *(unsigned *)(sm + SM_HANDLE);
    if (!export_join_data(impl, handle, NULL, &size) || size < 8 || size > sizeof buf) return 0;
    export_join_data(impl, handle, buf, &size);
    return *(u64 *)buf;
}
static void lobby_set(const char *key, const char *value) {
    unsigned char ok = g_steam.set_data(g_steam.mm, g_lobby, key, value);
    logf("host: lobby data %s=%s %s", key, value, ok ? "accepted" : "REFUSED");
}
// Invader's lobby list (below); the host also sends one, as a check that its own lobby is listed.
static u64 g_list_call;
static DWORD g_list_tick;
static unsigned g_list_count;
static int g_self_check;  // host: 0 not yet, 1 request done
static u64 g_candidate;    // invader: the lobby the last list picked, 0 = none
static void host_player_no_update(char *mb, char *me) {  // the member whose Steam id is the lobby owner's
    if (!g_diag.owner || !g_steam.mm || !g_candidate) return;
    u64 owner = g_diag.owner(g_steam.mm, g_candidate);
    for (char *x = mb; owner && mb && x < me && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE)
        if (!x[0xe1] && *(u64 *)(x + 0x10) == owner) g_host_player_no = *(int *)(x + 0xd4);
}
// The lobby this machine is in: its own (host) or the one it joined.
static u64 our_lobby(void) { return g_lobby ? g_lobby : g_candidate; }
static int member_ally(u64 lobby, u64 id) {
    const char *r = g_diag.get_member && g_steam.mm && lobby ? g_diag.get_member(g_steam.mm, lobby, id, LOBBY_ROLE_KEY) : NULL;
    return r && !strcmp(r, "ally");
}
// --- Lag measurement (log "lag: ", every LAG_LOG_MS per other lobby member; tags only) ----------------------------
// The game's transport is ISteamNetworking 006 (its only Steam networking interface: "SteamNetworking006"), which has
// no round-trip time. The game's own "rtt" exists only as a field of a telemetry record (0x142101810 writes it as
// JSON for the server; who fills it was not found), so we measure ourselves:
// - ping: a 16-byte echo on our own P2P channel LAG_CHANNEL, sent once a second to each other lobby member and
//   answered by its DLL. The game reads packets with ReadP2PPacket(..., channel); it never asks for this channel
//   (the hook below logs every channel the game reads and stops the echo if it ever reads ours), so a peer without
//   this DLL just leaves our 16 bytes a second unread in its Steam queue: nothing reaches its game code.
// - passive, per member: packets and bytes the game reads from it and sends to it (vtable hooks on the same
//   ISteamNetworking object, its slots 0 SendP2PPacket and 2 ReadP2PPacket), the largest gap between two packets
//   read from it, and from GetP2PSessionState relay yes/no and the bytes / packets queued for send to it.
// Nothing goes anywhere but to the session's own members, over the same Steam P2P sessions the game uses.
#define LAG_CHANNEL 20050
#define LAG_MAGIC 0x4750524eu  // "NRPG"
#define LAG_LOG_MS 15000
typedef unsigned char (*p2p_send_t)(void *, u64, const void *, unsigned, int, int);
typedef unsigned char (*p2p_avail_t)(void *, unsigned *, int);
typedef unsigned char (*p2p_read_t)(void *, void *, unsigned, unsigned *, u64 *, int);
static p2p_send_t g_p2p_send;
static p2p_avail_t g_p2p_avail;
static p2p_read_t g_p2p_read;
static volatile int g_lag_off;  // 1: the game read our channel itself, no more echo
static volatile u64 g_lag_channels;  // bit c: the game read channel c (0..62); bit 63: a channel above 62
static struct lag_peer {
    volatile u64 id;
    volatile LONG rx, rx_bytes, tx, tx_bytes;
    volatile double last_rx, max_gap;
    int rtt_n, rtt_min, rtt_max, rtt_sum, rtt_last, lost;
    volatile int rtt_shown, rtt_last_ok;  // for the bars: the last answer, and 1 while answers keep coming
    unsigned sent;
} g_lag[8];
static double lag_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static struct lag_peer *lag_find(u64 id) {
    for (int i = 0; i < 8; i++)
        if (g_lag[i].id == id) return &g_lag[i];
    return NULL;
}
static unsigned char lag_send_hook(void *self, u64 to, const void *data, unsigned n, int type, int channel) {
    struct lag_peer *p = lag_find(to);
    if (p) InterlockedIncrement(&p->tx), InterlockedExchangeAdd(&p->tx_bytes, (LONG)n);
    return g_p2p_send(self, to, data, n, type, channel);
}
static unsigned char lag_read_hook(void *self, void *data, unsigned cap, unsigned *n, u64 *from, int channel) {
    unsigned char ok = g_p2p_read(self, data, cap, n, from, channel);
    if (channel == LAG_CHANNEL && !g_lag_off) g_lag_off = 1, logf("lag: the game reads P2P channel %d itself: echo stopped", LAG_CHANNEL);
    g_lag_channels |= channel >= 0 && channel < 63 ? 1ull << channel : 1ull << 63;
    struct lag_peer *p = ok && from ? lag_find(*from) : NULL;
    if (p) {
        double now = lag_ms(), last = p->last_rx;
        if (last > 0 && now - last > p->max_gap) p->max_gap = now - last;
        p->last_rx = now;
        InterlockedIncrement(&p->rx), InterlockedExchangeAdd(&p->rx_bytes, n ? (LONG)*n : 0);
    }
    return ok;
}
static void lag_log(u64 lobby, u64 me, double window) {
    u64 owner = g_diag.owner ? g_diag.owner(g_steam.mm, lobby) : 0;
    const char *we = owner == me ? "host" : g_join == 4 ? (g_as_ally ? "ally" : "invader") : "member";
    char ch[96] = "";
    u64 m = g_lag_channels;
    for (int c = 0; c < 64; c++)
        if (m >> c & 1) snprintf(ch + strlen(ch), sizeof ch - strlen(ch), c == 63 ? " >62" : " %d", c);
    for (int i = 0; i < 8; i++) {
        struct lag_peer *p = &g_lag[i];
        if (!p->id) continue;
        struct { unsigned char active, connecting, error, relay; int bytes, packets; unsigned ip; unsigned short port; } st = {0};
        int have = g_diag.p2p && g_diag.p2p(g_diag.net, p->id, &st);
        char ping[96];
        if (p->rtt_n) snprintf(ping, sizeof ping, "ping %d ms (avg %d, min %d, max %d, %d of %u answered; our echo)", p->rtt_last,
                               p->rtt_sum / p->rtt_n, p->rtt_min, p->rtt_max, p->rtt_n, p->sent);
        else snprintf(ping, sizeof ping, "ping none (0 of %u echoes answered: no DLL there, or lost)", p->sent);
        LONG rx = InterlockedExchange(&p->rx, 0), rxb = InterlockedExchange(&p->rx_bytes, 0);
        LONG tx = InterlockedExchange(&p->tx, 0), txb = InterlockedExchange(&p->tx_bytes, 0);
        double s = window / 1000.0;
        logf("lag: we are the %s; member %04x (%s): %s | game in %.0f pkt/s %.0f B/s, largest gap %.0f ms | game out %.0f pkt/s "
             "%.0f B/s | p2p %s, relay %s, queued %d B in %d pkts | game channels:%s",
             we, id_tag(p->id), p->id == owner ? "host" : member_ally(lobby, p->id) ? "ally" : "invader", ping, rx / s, rxb / s,
             p->max_gap, tx / s, txb / s, have ? (st.active ? "active" : st.connecting ? "connecting" : "down") : "none",
             have ? (st.relay ? "yes" : "no") : "?", st.bytes, st.packets, ch);
        p->max_gap = 0, p->rtt_n = p->rtt_sum = p->rtt_max = p->sent = 0, p->rtt_min = 1 << 30;
    }
}
static void lag_step(double *next_ping_, double *next_log_, double *window_from_);
static DWORD WINAPI lag_thread(LPVOID unused) {
    (void)unused;
    double next_ping = 0, next_log = lag_ms() + LAG_LOG_MS, window_from = lag_ms();
    for (;;) {
        Sleep(2);
        InterlockedIncrement(&g_steam_busy);
        if (g_steam_down) {  // Steam is being shut down (process exit): no more Steam calls from this thread
            InterlockedDecrement(&g_steam_busy);
            logf("lag: echo thread stopped (Steam shutting down)");
            return 0;
        }
        lag_step(&next_ping, &next_log, &window_from);
        InterlockedDecrement(&g_steam_busy);
    }
}
static void lag_step(double *next_ping_, double *next_log_, double *window_from_) {
    double next_ping = *next_ping_, next_log = *next_log_, window_from = *window_from_;
    do {
        u64 lobby = our_lobby(), me = g_diag.self ? g_diag.self(g_diag.user) : 0;
        double now = lag_ms();
        struct { unsigned magic, type, seq, t; } pk;
        unsigned n = 0;
        u64 from = 0;
        while (g_p2p_avail(g_diag.net, &n, LAG_CHANNEL)) {
            if (!g_p2p_read(g_diag.net, &pk, sizeof pk, &n, &from, LAG_CHANNEL)) break;
            if (n != sizeof pk || pk.magic != LAG_MAGIC) continue;
            if (pk.type == 0) {
                pk.type = 1;
                if (!g_lag_off) g_p2p_send(g_diag.net, from, &pk, sizeof pk, 0, LAG_CHANNEL);
            } else if (pk.type == 1) {
                struct lag_peer *p = lag_find(from);
                double rtt = (unsigned)((u64)(lag_ms() * 1000.0) - pk.t) / 1000.0;  // t: our clock in us, mod 2^32
                if (p && rtt < 60000) {
                    int r = (int)(rtt + 0.5);
                    p->rtt_last = r, p->rtt_sum += r, p->rtt_n++, p->rtt_shown = r, p->rtt_last_ok = 1, p->lost = 0;
                    if (r < p->rtt_min) p->rtt_min = r;
                    if (r > p->rtt_max) p->rtt_max = r;
                }
            }
        }
        if (!lobby || !me) {
            memset(g_lag, 0, sizeof g_lag);
            next_log = now + LAG_LOG_MS, window_from = now;
            continue;
        }
        if (now >= next_ping) {
            next_ping = now + 1000;
            int count = g_steam.members(g_steam.mm, lobby);
            for (int i = 0; i < count && i < 8; i++) {  // keep the slots in step with the lobby
                u64 id = g_diag.member(g_steam.mm, lobby, i);
                if (!id || id == me) continue;
                struct lag_peer *p = lag_find(id);
                for (int k = 0; !p && k < 8; k++)
                    if (!g_lag[k].id) memset(&g_lag[k], 0, sizeof g_lag[k]), g_lag[k].rtt_min = 1 << 30, g_lag[k].id = id, p = &g_lag[k];
                if (!p) continue;
                struct { unsigned magic, type, seq, t; } out = {LAG_MAGIC, 0, p->sent, (unsigned)(u64)(lag_ms() * 1000.0)};
                if (++p->lost > 5) p->rtt_last_ok = 0;  // no answer for 5 s: the bar shows the name only
                if (!g_lag_off) p->sent++, g_p2p_send(g_diag.net, id, &out, sizeof out, 0, LAG_CHANNEL);
            }
            for (int k = 0; k < 8; k++) {  // members who left
                int in = 0;
                for (int i = 0; i < count && !in; i++) in = g_lag[k].id == g_diag.member(g_steam.mm, lobby, i);
                if (g_lag[k].id && !in) g_lag[k].id = 0;
            }
        }
        if (now >= next_log) {
            lag_log(lobby, me, now - window_from);
            next_log = now + LAG_LOG_MS, window_from = now;
        }
    } while (0);  // ("continue" above ends the step)
    *next_ping_ = next_ping, *next_log_ = next_log, *window_from_ = window_from;
}

// Ping at the party bars (show-ping, default on): the party panel's 7 slots (CSFeMan +0xcf28, 0x98 each) are filled
// by 0x7da8b0; for each shown member it gets the name with 0x7aa960(out, chr, 0) at 0x7dab36 and copies the text
// it returns (out[0], or out[2] when out[0] is 0) into the slot's string at +8. The rest of out (+8: the
// DLAllocatedStr the caller destroys) is not touched: only out[0] is pointed at our copy, "name  N ms", while in
// an expedition and once an echo from that member came back. The member: the character's player id (0x66c6f0)
// = session member +0xd4, its Steam id +0x10 -> our lag figures. Bars we hide (the invader's on the host) are not
// filled at all, so their ping stays in the log only.
static int g_in_exped;  // step hook: in the field of an expedition (host InGame or joined in the host's world)
static unsigned short g_name_buf[96];
static void **name_hook(void **out, char *chr, unsigned char flag, void *a4) {  // a4: passed on as the site has it
    void **r = ((void **(*)(void **, char *, unsigned char, void *))(g_base + 0x7aa960))(out, chr, flag, a4);
    if (!r || !chr || !g_in_exped || IsBadReadPtr(chr, 0x600)) return r;
    int no = -1;
    ((int *(*)(char *, int *))(g_base + 0x66c6f0))(chr, &no);
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    struct lag_peer *p = NULL;
    for (char *m = b; no != -1 && b && m < e && !IsBadReadPtr(m, SM_PLAYER_SIZE); m += SM_PLAYER_SIZE)
        if (*(int *)(m + 0xd4) == no && !m[0xe1]) p = lag_find(*(u64 *)(m + 0x10));
    if (!p || !p->rtt_last_ok) return r;
    const unsigned short *name = r[0] ? (const unsigned short *)r[0] : (const unsigned short *)r[2];
    int k = 0;
    while (name && !IsBadReadPtr(name + k, 2) && name[k] && k < 64) g_name_buf[k] = name[k], k++;
    char tail[24];
    snprintf(tail, sizeof tail, "  %d ms", p->rtt_shown);
    for (int i = 0; tail[i] && k < 95; i++) g_name_buf[k++] = (unsigned char)tail[i];
    g_name_buf[k] = 0;
    r[0] = g_name_buf;
    return r;
}
// Ping in the enemy tag (show-ping): the tag over a hostile player (CSFeMan +0xca60, 8 entries of 0x70) is added
// when it takes damage (0x1407c5710 -> 0x1407dd3f0, which also sets +0x2c to 0) and dropped by the per-frame update
// 0x1407d8c40 once +0x2c, the time since the last hit, reaches 1.5 s (0x142be32b4). The view 0x1407c8a90 (from
// 0x1407c8f83) rebuilds the eight tags each frame and gets each name with 0x7aa960(out, chr, 0) at 0x7c8cb4; it keeps
// out[0] as the text pointer, so the tag's text lives in a buffer per character here. Only for a member of the
// other side (lobby member data: owner and "ally" = host's side) and while its echo answers ("name  N ms").
static void **tag_name_hook(void **out, char *chr, unsigned char flag, void *a4) {
    void **r = ((void **(*)(void **, char *, unsigned char, void *))(g_base + 0x7aa960))(out, chr, flag, a4);
    if (!r || !chr || !g_in_exped || IsBadReadPtr(chr, 0x600)) return r;
    int no = -1;
    ((int *(*)(char *, int *))(g_base + 0x66c6f0))(chr, &no);
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    u64 id = 0, lobby = our_lobby(), owner = lobby && g_steam.mm && g_diag.owner ? g_diag.owner(g_steam.mm, lobby) : 0;
    for (char *m = b; no != -1 && b && m < e && !IsBadReadPtr(m, SM_PLAYER_SIZE); m += SM_PLAYER_SIZE)
        if (*(int *)(m + 0xd4) == no && !m[0xe1]) id = *(u64 *)(m + 0x10);
    if (!id || !owner) return r;
    int invader = id != owner && !member_ally(lobby, id), we_invader = g_join == 4 && !g_as_ally;
    struct lag_peer *p = invader != we_invader ? lag_find(id) : NULL;
    if (!p || !p->rtt_last_ok) return r;
    static char *who[8];
    static unsigned short buf[8][96];
    int k = 0;
    while (k < 8 && who[k] != chr) k++;
    if (k == 8) {
        static int next;
        k = next, next = (next + 1) % 8, who[k] = chr;
    }
    const unsigned short *name = r[0] ? (const unsigned short *)r[0] : (const unsigned short *)r[2];
    int n = 0;
    while (name && !IsBadReadPtr(name + n, 2) && name[n] && n < 64) buf[k][n] = name[n], n++;
    char tail[24];
    snprintf(tail, sizeof tail, "  %d ms", p->rtt_shown);
    for (int i = 0; tail[i] && n < 95; i++) buf[k][n++] = (unsigned char)tail[i];
    buf[k][n] = 0;
    r[0] = buf[k];
    return r;
}
static void lag_tick(void) {  // run thread; once Steam is ready: hooks and the echo thread
    static int done;
    if (done || !steam_ready() || !g_diag.net || !g_diag.member || !g_diag.self || !g_diag.user) return;
    done = 1;
    void **vt = *(void ***)g_diag.net;
    DWORD old;
    if (IsBadReadPtr(vt, 3 * 8) || !VirtualProtect(vt, 3 * 8, PAGE_READWRITE, &old)) {
        logf("lag: cannot hook the P2P interface: no lag log");
        return;
    }
    g_p2p_send = (p2p_send_t)vt[0], g_p2p_avail = (p2p_avail_t)vt[1], g_p2p_read = (p2p_read_t)vt[2];
    vt[0] = (void *)lag_send_hook, vt[2] = (void *)lag_read_hook;
    VirtualProtect(vt, 3 * 8, old, &old);
    CreateThread(NULL, 0, lag_thread, NULL, 0, NULL);
    logf("lag: measuring (echo on P2P channel %d, game packets counted per member, a line every %d s)", LAG_CHANNEL, LAG_LOG_MS / 1000);
    hook_call(0x7dab36, 0x7aa960, (void *)name_hook, "lag: party bar name (ping after it)");
    hook_call(0x7c8cb4, 0x7aa960, (void *)tag_name_hook, "lag: enemy tag name (ping after it)");
}
// Remote session members other than the session host (the lobby owner): allies (want_allies 1) or invaders (0;
// a member without a role is an invader). Recounted at most every 250 ms.
static int side_count(int want_allies) {
    static DWORD at;
    static int n[2];
    if (GetTickCount() - at < 250) return n[want_allies != 0];
    at = GetTickCount(), n[0] = n[1] = 0;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    u64 lobby = our_lobby(), owner = lobby && g_diag.owner && g_steam.mm ? g_diag.owner(g_steam.mm, lobby) : 0;
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    for (char *x = b; b && x < e && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE) {
        u64 id = *(u64 *)(x + 0x10);
        if (x[0xe1] || (owner && id == owner)) continue;
        n[member_ally(lobby, id)]++;
    }
    return n[want_allies != 0];
}
static int member_no_is_invader(int no) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    u64 lobby = our_lobby(), owner = lobby && g_diag.owner && g_steam.mm ? g_diag.owner(g_steam.mm, lobby) : 0;
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    for (char *x = b; b && x < e && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE)
        if (!x[0xe1] && *(int *)(x + 0xd4) == no) return !(owner && *(u64 *)(x + 0x10) == owner) && !member_ally(lobby, *(u64 *)(x + 0x10));
    return 0;
}
static int g_join_test;    // invader: join-test setting, taken when the search started
static unsigned g_squad;   // host: the expedition's squad size, fixed when the lobby is first marked

static void log_pending_state(const char *who);
// Host, every frame while the session runs: mark the lobby once, then keep squad / free slots current.
static void lobby_mark(void) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!sm) return;
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    if (state != 3) {
        if (g_lobby) logf("host: lobby_state %u, no longer host of lobby %llu (its marker goes with it)", state, g_lobby);
        g_lobby = 0;
        g_marked_squad = ~0u;
        g_someone_joining = 0;
        return;
    }
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    unsigned members = begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0;
    if (!members || !steam_ready()) return;
    if (!g_lobby) {
        static int told;
        if (!(g_lobby = session_lobby_id(sm))) {
            if (!told++) logf("host: no lobby id from the session yet, lobby not marked (tried again every frame)");
            return;
        }
        told = 0;
        logf("host: session up, Steam lobby %llu (%016llx), type left as the game made it", g_lobby, g_lobby);
        log_pending_state("host");
        lobby_set(LOBBY_MARK_KEY, "1");
        lobby_set(LOBBY_PROTO_KEY, LOBBY_PROTO);
        lobby_set(LOBBY_GAME_KEY, LOBBY_GAME);
        lobby_set(LOBBY_VER_KEY, MOD_VERSION);
        {
            char pt[8];
            snprintf(pt, sizeof pt, "%d", g_settings[S_PAIR]);
            lobby_set(LOBBY_PAIR_KEY, pt);
        }
        {  // the expedition's start block: what a solo start put in the pending block (SingleStart ->
           // context vt+8 0xb44630 -> 0xb47470), kept by the game manager for the whole expedition
            char *gm = *(char **)(g_base + 0x3c13258), text[16];
            if (gm) {
                snprintf(text, sizeof text, "%08x", *(unsigned *)(gm + 0xa74));
                lobby_set(LOBBY_START_KEY, text);
                // what the expedition generator (0x6aec40) changed in the game manager on the host's solo
                // start: +0xb48, +0xb54, +0xb5c (run of 5c775057); published to compare with the invader's
                char gen[40];
                snprintf(gen, sizeof gen, "%x,%x,%x", *(unsigned *)(gm + 0xb48), *(unsigned *)(gm + 0xb54), *(unsigned *)(gm + 0xb5c));
                lobby_set(LOBBY_GEN_KEY, gen);
                expedition_log("host");
            }
        }
        g_squad = g_party_host ? 1 : members;  // a party host's squad mates are allies (their member data says so)
        g_list_tick = GetTickCount();  // self-check one period after marking
    }
    {  // team play: level, phase and age for the joiners' pick order (no personal data)
        static DWORD next;
        static u64 born_for;
        if (born_for != g_lobby) {
            born_for = g_lobby;
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull;
            char text[24];
            snprintf(text, sizeof text, "%llu", t > 13410806400ull ? t - 13410806400ull : 0);  // FILETIME seconds of 2026-01-01
            lobby_set(LOBBY_BORN_KEY, text);
        }
        if ((int)(GetTickCount() - next) >= 0) {
            next = GetTickCount() + 2000;
            char *gdm = *(char **)(g_base + 0x3c078d0), *pd = gdm ? *(char **)(gdm + 8) : NULL, text[16];
            snprintf(text, sizeof text, "%d", pd && !IsBadReadPtr(pd + 0x68, 4) ? *(int *)(pd + 0x68) : 0);
            lobby_set(LOBBY_LEVEL_KEY, text);
            // EventFlag.txt names: 7504 / 7509 night boss fight of day 1 / 2, 7502 / 7507 that boss beaten
            const char *phase = !g_in_expedition ? "hub" : two_flag(7507, 0, 0) ? "late"
                              : (two_flag(7504, 0, 0) && !two_flag(7502, 0, 0)) || two_flag(7509, 0, 0) ? "boss" : "field";
            lobby_set(LOBBY_PHASE_KEY, phase);
        }
    }
    // Squad: the session's members when the lobby was first marked (the expedition's own players are
    // all in it by InGame). Session members beyond it are invaders; Steam lobby members beyond the
    // session's are someone in the middle of joining, and hold a slot too.
    // ponytail: a squad member who rejoins after a drop-out would count as an invader.
    steam_watch("host", g_lobby);
    member_flags_log("host", sm);
    host_member_sync(sm);
    int in_lobby = g_steam.members(g_steam.mm, g_lobby);
    g_someone_joining = in_lobby > (int)members;
    // Allies and invaders by their lobby member data (members still joining included; no role = invader).
    unsigned allies = 0, others = 0;
    u64 self = g_diag.self && g_diag.user ? g_diag.self(g_diag.user) : 0;
    for (int i = 0; g_diag.member && i < in_lobby && i < 16; i++) {
        u64 id = g_diag.member(g_steam.mm, g_lobby, i);
        if (!id || id == self) continue;
        if (member_ally(g_lobby, id)) allies++;
        else others++;
    }
    unsigned key = members | (unsigned)(in_lobby & 0xff) << 8 | allies << 16 | (unsigned)(g_df_used != 0) << 24;
    if (key == g_marked_squad) return;
    g_marked_squad = key;
    unsigned squad = g_squad + allies, allowed = invaders_allowed(squad);
    unsigned room = SESSION_PLAYERS_MAX > squad + others ? SESSION_PLAYERS_MAX - squad - others : 0;  // places left in the session
    unsigned free_slots = allowed > others ? allowed - others : 0, ally_free = SQUAD_MAX > squad ? SQUAD_MAX - squad : 0;
    if (free_slots > room) free_slots = room;
    if (ally_free > room) ally_free = room;
    if (g_party_host && !g_in_expedition) free_slots = 0;  // no invaders before the host is in its expedition
    // nor after this day's challenge (day_flow.h: a joining invader leaves at once then, df_lobby_watch); from day 2
    // (7505) on there is room again, until day 2's challenge
    if (g_df_used) free_slots = 0;
    char text[16];
    snprintf(text, sizeof text, "%u", squad);
    lobby_set(LOBBY_SQUAD_KEY, text);
    snprintf(text, sizeof text, "%u", free_slots);
    lobby_set(LOBBY_FREE_KEY, text);
    snprintf(text, sizeof text, "%u", ally_free);
    lobby_set(LOBBY_ALLY_FREE_KEY, text);
    logf("host: lobby members %d of %d; session members %u; squad %u (%u + allies %u), invaders and joiners without a role %u; "
         "free invader slots %u, free ally places %u", in_lobby, g_steam.limit(g_steam.mm, g_lobby), members, squad, g_squad, allies,
         others, free_slots, ally_free);
}

// One lobby list request at a time, every SEARCH_PERIOD_MS: the invader every idle frame while
// searching, the host once (who = "host self-check"). Returns 1 when a request has finished.
// Version notice (131170 newer release seen, 131171 the team with our password plays another version). The text is
// the launcher's FMG line with "%s"; the notice path copies a MenuString's first field (the text pointer) as is
// (0x1405bea10 in the summon-message record 0x1408b3240), so the line is built with the game's ctor and its pointer
// is set to our formatted copy, like the ping after a name. Once per hub visit and once per search start.
static int g_ver_notice_armed = 1;
static char g_ver_newest[24], g_ver_team[24];
static int g_pair_team = -1;  // a lobby with our password and version, on another region pair (its pair), this search
static int version_parse(const char *v, int out[3]) {  // a release is "a.b.c", digits only
    if (!v) return 0;
    int n = 0, k = 0;
    out[0] = out[1] = out[2] = 0;
    for (const char *c = v; *c; c++) {
        if (*c == '.') { if (++k > 2) return 0; n = 0; continue; }
        if (*c < '0' || *c > '9' || ++n > 6) return 0;
        out[k] = out[k] * 10 + (*c - '0');
    }
    return k == 2 && n > 0;
}
static int version_newer(const char *a, const char *b) {  // a newer release than b (both releases)
    int x[3], y[3];
    if (!version_parse(a, x) || !version_parse(b, y)) return 0;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}
static void version_seen(const char *ver, int party) {
    if (!ver || !*ver || !strcmp(ver, MOD_VERSION)) return;
    if (version_newer(ver, MOD_VERSION) && (!*g_ver_newest || version_newer(ver, g_ver_newest)))
        snprintf(g_ver_newest, sizeof g_ver_newest, "%s", ver);
    if (party && !*g_ver_team) snprintf(g_ver_team, sizeof g_ver_team, "%s", ver);  // same password key, other version
}
static void notice_with(int id, const char *arg) {
    static _Alignas(16) char text[8][0x100];  // one slot per line: 131170 / 131171 and the day flow's 131180-131184
    static unsigned short line[8][256];
    int k = id & 7;
    char *t = ((char *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(text[k], id);
    const unsigned short *tpl = t && *(unsigned short **)t ? *(unsigned short **)t : t ? *(unsigned short **)(t + 0x10) : NULL;
    int n = 0;
    const char *a = arg;  // "x|y": the first %s gets x, the next y
    for (int i = 0; tpl && !IsBadReadPtr(tpl + i, 2) && tpl[i] && i < 200 && n < 250; i++) {
        if (tpl[i] == '%' && tpl[i + 1] == 's') {
            for (; a && *a && *a != '|' && n < 250; a++) line[k][n++] = (unsigned char)*a;
            if (a && *a == '|') a++;
            i++;
        } else line[k][n++] = tpl[i];
    }
    line[k][n] = 0;
    if (!n || !t) return (void)logf("notice %d: no text (line not in CL_MenuText)", id);
    *(unsigned short **)t = line[k];
    unsigned char shown = ((unsigned char (*)(void *))(g_base + RVA_SUMMON_MESSAGE))(t);  // 0x86b8d0: queue (priority 99)
    char *q = summon_queue();
    logf("notice %d%s%s: queued %s (on screen priority %d, waiting %d)", id, arg ? " " : "", arg ? arg : "", shown ? "yes" : "NO",
         q ? *(short *)(q + SMQ_CURRENT_PRIORITY) : -2, q ? *(int *)(q + SMQ_WAITING_COUNT) : -1);
}
static void version_result(int found_same) {
    if (g_ver_notice_armed && g_party && !found_same && g_pair_team >= 0 && !*g_ver_team) {
        g_ver_notice_armed = 0;
        logf("pair: the team with our password plays region pair %d (ours %d): notice shown", g_pair_team, g_settings[S_PAIR]);
        notice_with(131186 + g_pair_team, NULL);
    } else if (g_ver_notice_armed && *g_ver_newest) {
        g_ver_notice_armed = 0;
        logf("version: a newer mod version %s is in use (ours %s): notice shown", g_ver_newest, MOD_VERSION);
        notice_with(131170, g_ver_newest);
    } else if (g_ver_notice_armed && g_party && !found_same && *g_ver_team) {
        g_ver_notice_armed = 0;
        logf("version: the team with our password plays %s (ours %s): notice shown", g_ver_team, MOD_VERSION);
        notice_with(131171, g_ver_team);
    }
    g_ver_newest[0] = g_ver_team[0] = 0;
    g_pair_team = -1;
}
static int counts_in_hub(void);
static void counts_from_list(unsigned n);
static u64 g_count_call;
static void lobby_search_reset(void) {
    if (g_list_call) logf("search: request %u abandoned", g_list_count);
    g_list_call = 0;
    g_list_tick = 0;
}
static int lobby_search_frame(const char *who) {
    if (!steam_ready()) return 0;
    DWORD now = GetTickCount();
    if (!g_list_call) {
        if (g_list_tick && now - g_list_tick < SEARCH_PERIOD_MS) return 0;
        if (g_count_call) return 0;  // the counts' own request first (one list at a time)
        g_steam.string_filter(g_steam.mm, LOBBY_MARK_KEY, "1", LOBBY_EQUAL);
        // no proto / version filter: lobbies of other versions are listed too, so an older client learns of a
        // newer release (one request per period as before). ponytail: Steam returns at most 50 lobbies; with many
        // lobbies of other versions ours could fall out of the list — a version filter then, and a second request.
        if (g_party) g_steam.string_filter(g_steam.mm, LOBBY_PARTY_KEY, g_party_code, LOBBY_EQUAL);
        g_steam.distance_filter(g_steam.mm, LOBBY_WORLDWIDE);
        g_list_call = g_steam.request_list(g_steam.mm);
        g_list_tick = now;
        g_list_count++;
        logf("%s: request %u sent (%s=1, %s=%s, worldwide), call %llx", who, g_list_count, LOBBY_MARK_KEY,
             LOBBY_PROTO_KEY, LOBBY_PROTO, g_list_call);
        if (!g_list_call) logf("%s: request %u refused by Steam (no call handle)", who, g_list_count);
        return !g_list_call;
    }
    unsigned char failed = 0;
    if (!g_steam.completed(g_steam.utils, g_list_call, &failed)) {
        if (now - g_list_tick <= SEARCH_TIMEOUT_MS) return 0;
        logf("%s: request %u not completed after %lums (failed %u), dropped", who, g_list_count, now - g_list_tick, failed);
        g_list_call = 0;
        return 1;
    }
    struct { unsigned matching; } result = {0};  // LobbyMatchList_t
    unsigned char got = g_steam.result(g_steam.utils, g_list_call, &result, sizeof result, LOBBY_MATCH_LIST, &failed);
    g_list_call = 0;
    logf("%s: request %u completed after %lums: result %s, failed %u, %u lobbies", who, g_list_count, now - g_list_tick,
         got ? "read" : "NOT read", failed, result.matching);
    if (!got || failed) return 1;
    u64 chosen = 0, fit[50];
    int nfit = 0;
    g_party_lowest = 0;
    for (unsigned i = 0; i < result.matching && i < 50; i++) {
        u64 id = g_steam.by_index(g_steam.mm, (int)i);
        const char *game = g_steam.get_data(g_steam.mm, id, LOBBY_GAME_KEY), *squad = g_steam.get_data(g_steam.mm, id, LOBBY_SQUAD_KEY);
        const char *free = g_steam.get_data(g_steam.mm, id, LOBBY_FREE_KEY), *proto = g_steam.get_data(g_steam.mm, id, LOBBY_PROTO_KEY);
        const char *block = g_steam.get_data(g_steam.mm, id, LOBBY_BLOCK_KEY), *pos = g_steam.get_data(g_steam.mm, id, LOBBY_POS_KEY);
        logf("%s:   lobby %llu: host block '%s' pos '%s'", who, id, block ? block : "?", pos ? pos : "?");
        const char *afree = g_steam.get_data(g_steam.mm, id, LOBBY_ALLY_FREE_KEY);
        int own = id == g_lobby && g_lobby, slots = g_as_ally ? (afree ? atoi(afree) : 0) : free ? atoi(free) : 0;
        const char *ver = g_steam.get_data(g_steam.mm, id, LOBBY_VER_KEY);
        const char *pr = g_steam.get_data(g_steam.mm, id, LOBBY_PAIR_KEY);
        int same = proto && !strcmp(proto, LOBBY_PROTO) && ver && !strcmp(ver, MOD_VERSION);
        int pair_ok = pr && *pr && atoi(pr) == g_settings[S_PAIR];
        version_seen(ver, g_party != 0);
        if (same && !own && !pair_ok && g_party && pr && *pr && g_pair_team < 0) g_pair_team = atoi(pr) & 3;
        if (same && !own && !pair_ok) logf("%s:   lobby %llu: map (region pair) '%s', ours %d: not for us", who, id, pr ? pr : "?", g_settings[S_PAIR]);
        same = same && pair_ok;
        int fits = same && !own && slots > 0 && game && !strcmp(game, LOBBY_GAME);
        // GetLobbyData gives "" for a key that is not set.
        logf("%s:   lobby %llu: proto '%s' game '%s' squad '%s' free '%s', members %d of %d -> %s", who, id, proto ? proto : "?",
             game ? game : "?", squad ? squad : "?", free ? free : "?", g_steam.members(g_steam.mm, id),
             g_steam.limit(g_steam.mm, id), own ? "our own lobby" : fits ? (g_as_ally ? "has a free ally place" : "has a free invader slot") : "no slot for us");
        if (fits && !chosen) chosen = id;  // Steam sorts by distance: the first that fits
        if (fits && !(g_party && id == g_party_old_lobby)) fit[nfit++] = id;
        if (g_party && !own && id != g_party_old_lobby && (!g_party_lowest || id < g_party_lowest)) g_party_lowest = id;
    }
    if (g_party) {  // team play: lowest host level, then the oldest lobby; never in a night boss fight or after day 2's
        chosen = 0;
        long best_lvl = 0, best_born = 0;
        for (int i = 0; i < nfit; i++) {
            const char *ph = g_steam.get_data(g_steam.mm, fit[i], LOBBY_PHASE_KEY), *lv = g_steam.get_data(g_steam.mm, fit[i], LOBBY_LEVEL_KEY);
            const char *bo = g_steam.get_data(g_steam.mm, fit[i], LOBBY_BORN_KEY);
            if (ph && (!strcmp(ph, "boss") || !strcmp(ph, "late"))) {
                logf("%s:   lobby %llu left out: phase %s", who, fit[i], ph);
                continue;
            }
            long l = lv && *lv ? atol(lv) : 0, b = bo && *bo ? atol(bo) : 0;
            if (!chosen || l < best_lvl || (l == best_lvl && b < best_born)) chosen = fit[i], best_lvl = l, best_born = b;
        }
        if (nfit > 1 && chosen) logf("%s: %d hosts with a free place: lobby %llu taken (host level %ld)", who, nfit, chosen, best_lvl);
    }
    version_result(chosen != 0);
    if (counts_in_hub()) counts_from_list(result.matching);
    if (chosen) logf("%s: %s lobby %llu", who, g_join_test ? "candidate" : "would join (join-test is off)", chosen);
    else logf("%s: no lobby to join", who);
    g_candidate = chosen;
    return 1;
}

// --- Join test (invader side), only with join-test = true in the test settings ------------------
//
// Join: 0xdf0f70(manager, u8 multiplay type, vector *join data, u8 title flow, u8 *one), the manager
// half of the game's joins (JoinSessionJob through 0x73fa60 passes title flow 0, the title screen's
// JoinSessionByTitleFlowJob 0xb1a3e0 passes 1, 0xdfd830 passes 0). Read: refuses unless lobby_state
// is none/failed and +0x19 is clear; same ciphers and parameters as create (0xdf6e70, 0xdf4830);
// implementation vt+0x10 (params, data, length) starts the Steam session, whose step 0x25921e0
// takes an 8-byte lobby id and calls ISteamMatchmaking::JoinLobby; join handle at +0x28,
// lobby_state 4. The manager's update (0xdf2a20) then sees the handle finish: success -> session
// handle, EAC begin (0xdf2d3c, result ignored), members added, 0xdf5930 lobby_state 6 (client);
// failure -> cleanup 0xdf61e0, a "could not join" notice (0x5b69e0), lobby_state 0.
// Title flow 1 is used: after a started join it only calls 0xb831a0(0) (one global set to 0), where
// 0 also shows a notice (0x60a8e0 text 0x2775), sets a flag in the game manager (0x6af640) and the
// same global. The game's own join (0x73fa60) first stores the 0x22-byte join parameters as
// pending multiplayer state (setters 0x6b0410..0x6b0620: multiplay type +0xdc0, ...); we have no
// such parameters for a lobby found ourselves and leave them alone.
// ponytail: multiplay type 0 (white summon), as the host's session; which type an invader should
// carry is an open choice (1 = the old red invasion entry of the type table).
// Where it stops: once lobby_state is 6 we are a member of the host's session, still in our hub.
// Nothing readable shows what takes a client into a host's running expedition (the title-screen
// rejoin's map entry and the quick-match guest steps are not traced to an entry point we can
// drive), so the test holds the session JOIN_HOLD_MS, logging everything, then leaves and ends the
// search. Any failure or a join stuck longer than JOIN_TIMEOUT_MS ends it the same way.
#define RVA_SESSION_JOIN 0xdf0f70
#define SM_JOIN_HANDLE   0x28
#define JOIN_TIMEOUT_MS  30000
#define JOIN_HOLD_MS     30000
static int g_join;            // 0 waiting for a candidate, 1 join called, 2 joined (holding), 3 over
static DWORD g_join_tick;
static u64 g_join_data[1];    // the lobby id, kept for as long as the implementation might read it
#define QM_CTRL_REFUSED 0x10d
static char *g_qm_ctrl;  // the controller, as seen by the step hook
static int g_qm_step = -1;  // its current step
static int g_move_test;     // move-test setting (with enter-test): force the map move, see move_force
static int g_move;          // 0 not yet, 1 wanted (sync at EndLobbyExit), 2 forced, 3 a map load began
static DWORD g_move_tick;
static int g_move_online;   // we set gm+0xb40
static int g_enter_test;    // enter-test setting (with join-test): stay a client and watch the game's way into the host's world
static void join_test_start(void) {
    g_join = 0;
    g_candidate = 0;
    g_join_test = g_enter_test = g_move_test = 1;  // "Invasion" always joins (no switches since 14c271a)
    g_as_ally = 0;  // the menu's "team" sets it after this (party_frame); "Invasion" is always an invader
    logf("join: search started as %s, the first candidate is joined; controller +0x10d %d", g_as_ally ? "an ally" : "an invader",
         g_qm_ctrl ? (unsigned char)g_qm_ctrl[QM_CTRL_REFUSED] : -1);
}
// Every frame while joining: one line per change of the session's state, with ms since the call.
static void join_watch(char *sm) {
    static unsigned last[12];
    static unsigned long long last_end;
    static DWORD logged_for;  // g_join_tick of the join last logged: a new join always gets a first line
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    char *sy = *(char **)(sm + 0x1b8);
    if (sy && IsBadReadPtr(sy, 0xe8)) sy = NULL;
    unsigned now[12] = {*(unsigned *)(sm + SM_LOBBY_STATE), *(unsigned *)(sm + SM_HANDLE), *(unsigned *)(sm + SM_JOIN_HANDLE),
                        *(unsigned *)(sm + 0x10), begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0,
                        (unsigned char)sm[SM_LEAVING], (unsigned char)sm[0x1a], (unsigned char)sm[0x1b], (unsigned char)sm[0x1d],
                        (unsigned char)sm[0x1f], sy ? (unsigned char)sy[0xd8] : 0xff, sy ? *(unsigned *)(sy + 0x40) : 0xff};
    unsigned long long end_code = *(unsigned long long *)(sm + SM_END_CODE);
    if (!memcmp(now, last, sizeof now) && end_code == last_end && logged_for == g_join_tick) return;
    logged_for = g_join_tick;
    int members_changed = now[4] != last[4];
    memcpy(last, now, sizeof now);
    last_end = end_code;
    char *eac = *(char **)(g_base + RVA_EAC_MAN_PTR);
    logf("join test: +%lums lobby_state %u handle %u join %u role %u players %u leaving %u end_code %llx | eac error %u (0x%x) | "
         "sm +1a %u +1b %u +1d %u +1f %u | sync phase %u step %u | impl +0xc70 %x controller +0x108 %u",
         GetTickCount() - g_join_tick, now[0], now[1], now[2], now[3], now[4], now[5], end_code,
         eac ? *(unsigned *)(eac + EAC_ERROR) : 0, eac ? *(unsigned *)(eac + EAC_ERROR) : 0, now[6], now[7], now[8], now[9],
         now[10], now[11], *(char **)(sm + 0x60) && !IsBadReadPtr(*(char **)(sm + 0x60) + 0xc70, 4) ? *(unsigned *)(*(char **)(sm + 0x60) + 0xc70) : 0,
         g_qm_ctrl ? (unsigned char)g_qm_ctrl[0x108] : 0xff);
    if (members_changed) log_members(sm, "join test");
}
// The game manager's pending multiplayer state: where a joining player is put on the next map
// load. Setters read (0x6b0410..0x6b0620, filled by the game's join 0x73fa60 and the title-screen
// rejoin 0xb1a3e0 from the 0x22-byte join parameters); readers: the spawn choice 0xc38d31 (+0xaac
// flag, +0xaa0, +0xa74 a map block id m{AA}_{BB}_{CC}_{DD}, +0xa78..+0xa80 three floats, as a
// position) and MoveMapStep's constructor 0xc2fc30 (+0xdc0 multiplay type copied to the local
// player's data +0xe6, +0xdbc join reason: -27..-24 are rejoin cases). +0xa78..+0xa80 is written by a
// protected fragment (0x1afca8d), so the position reading is inferred from its readers.
// The rejoin record (session info +0x150, 0x1fe9b0): what the title-screen rejoin reads (join data
// at +0x40, checked through the session layer by CheckSessionExistenceJob); a quick-match guest
// writes it in GuestReadyWait (0x2022d0). Logged only, to compare host and joiner.
#define RVA_GAME_MAN_PTR    0x3c13258
#define RVA_SESSION_INFO    0x3c078d0
static void log_pending_state(const char *who) {
    char *gm = *(char **)(g_base + RVA_GAME_MAN_PTR), *info = *(char **)(g_base + RVA_SESSION_INFO);
    if (!gm) return;
    float *pos = (float *)(gm + 0xa78);
    logf("%s: game manager +a70 %08x block +a74 %08x pos %.2f %.2f %.2f +a94 %d +aa0 %d +aa4 %d +aa8 %d +aac %u join reason %d "
         "type %u +dc1 %u next map +adc %08x", who, *(unsigned *)(gm + 0xa70), *(unsigned *)(gm + 0xa74), pos[0], pos[1], pos[2],
         *(int *)(gm + 0xa94), *(int *)(gm + 0xaa0), *(int *)(gm + 0xaa4), *(int *)(gm + 0xaa8), (unsigned char)gm[0xaac],
         *(int *)(gm + 0xdbc), (unsigned char)gm[0xdc0], (unsigned char)gm[0xdc1], *(unsigned *)(gm + 0xadc));
    char *record = info ? *(char **)(info + 0x150) : NULL;
    if (!record || IsBadReadPtr(record, 0x58)) return;
    char hex[0x40 * 2 + 1];
    for (unsigned i = 0; i < 0x40; i++) sprintf(hex + 2 * i, "%02x", (unsigned char)record[i]);
    char *begin = *(char **)(record + 0x40), *end = *(char **)(record + 0x48);
    logf("%s: rejoin record %s, join data %lld bytes", who, hex, begin && end >= begin ? (long long)(end - begin) : -1LL);
}
// A refused client: the session update (0xdf2a20, client role 1) reads the host's 4-byte answer
// in packet 0x49 (0xdde8a0); answer 1 shows notice 0x2789, leaves and calls 0xb67140, which sets
// CSQuickMatchingCtrl+0x10d = 1; answer 2 shows notice 0x27a7 and leaves. The controller's own
// join set-up (0xb51070) clears +0x10d first. Inferred: +0x10d left at 1 is why the next
// "Invasion" sent no start request; so it is logged, and cleared when the join test ends.
static void toggle_search(const char *where);
static int g_move, g_move_online, g_ctx_old_mode;
static unsigned g_saved_pending[4];  // pending block, +0xaac, type +0xdc0, reason +0xdbc before the forced step
static int g_saved_pending_valid;
#define RVA_QM_UNREGISTER 0xb52ef0  // void (ctrl*): if step != 0, cancel the job chain, set step 25
static int g_invasion_ended;
static int g_invader_downs;
static int g_host_killed;    // the invader saw the session host die in this invasion
static int g_reward_pending; // 1 won, 2 lost: applied at the expedition's reward computation
// Decision 24: the invader joins at the host's level, set once on arrival and put back when the invasion
// ends. The game's own level change (0x670f80, the character's level-effect update) does, for the player
// data (play state +8): 0x209e20(data, level) (level +0x68 and the stats +0x34..+0x50 from the level
// table), 0x209ef0 (the expedition's highest level +0x10c), 0x20e170 (level-bound equipment), 0x20dcd0;
// and recomputes the maxima: 0x502300(character +0x190, bonuses) -> 0x20da60(data, bonuses), then max HP
// / FP / stamina 0x6c90e0 / 0x6c9180 / 0x6c9290 into +0x18 / +0x24 / +0x30 and onto the character's data
// module (0x427b70 / 0x427b80 / 0x427b90, 0x428bf0). The same calls here. Runes (+0x6c) are not touched;
// the highest level +0x10c is put back to its own value when the level is restored.
static int g_level_set, g_own_level, g_own_top_level;
#define JOIN_LEVEL_CUT_PERCENT 40  // a cut joiner's level = max(1, floor(host level * (100 - this) / 100))
#define JOIN_LEVEL_CUT_ALLIES 1    // the host's summons: cut (the user's rule)
#define JOIN_LEVEL_CUT_INVADERS 0  // invaders: the host's level exactly (decision 24)
#define JOIN_CUT_FROM_LEVEL 4      // the cuts (level and flasks) apply only when the host is at least this level
static int g_join_cut = -1;        // the decision at arrival: 1 cut, 0 the host's values, -1 not decided yet
static int level_apply(int level, const char *why) {
    char *ps = *(char **)(g_base + 0x3c078d0), *chr = local_player();
    char *pd = ps ? *(char **)(ps + 8) : NULL;
    if (!pd || IsBadReadPtr(pd, 0x860) || !chr || IsBadReadPtr(chr, 0x1c0)) return -1;
    if (level < 1) level = 1;
    if (level > 15) level = 15;
    int before = *(int *)(pd + 0x68);
    ((void (*)(char *, int))(g_base + 0x209e20))(pd, level);
    ((void (*)(char *))(g_base + 0x209ef0))(pd);
    ((void (*)(char *))(g_base + 0x20e170))(pd);
    ((void (*)(char *))(g_base + 0x20dcd0))(pd);
    unsigned long long bonus[4] = {0};
    ((void (*)(void *, void *))(g_base + 0x502300))(*(void **)(chr + 0x190), bonus);
    if (((char (*)(char *, void *))(g_base + 0x20da60))(pd, bonus)) {
        *(int *)(pd + 0x18) = ((int (*)(char *))(g_base + 0x6c90e0))(pd);
        *(int *)(pd + 0x24) = ((int (*)(char *))(g_base + 0x6c9180))(pd);
        *(int *)(pd + 0x30) = ((int (*)(char *))(g_base + 0x6c9290))(pd);
        char **mod = *(char ***)(chr + 0x1b8);
        if (mod && !IsBadReadPtr(mod, 8) && *mod) {
            ((void (*)(char *, int))(g_base + 0x427b70))(*mod, *(int *)(pd + 0x18));
            ((void (*)(char *, int))(g_base + 0x427b80))(*mod, *(int *)(pd + 0x24));
            ((void (*)(char *, int))(g_base + 0x427b90))(*mod, *(int *)(pd + 0x30));
            ((void (*)(char *, int))(g_base + 0x428bf0))(*mod, 0);
        }
    }
    logf("level: %s: level %d -> %d (max HP %d, FP %d, stamina %d)", why, before, *(int *)(pd + 0x68), *(int *)(pd + 0x18),
         *(int *)(pd + 0x24), *(int *)(pd + 0x30));
    return before;
}
static void level_restore(void) {
    if (!g_level_set) return;
    g_level_set = 0;
    char *ps = *(char **)(g_base + 0x3c078d0);
    char *pd = ps ? *(char **)(ps + 8) : NULL;
    if (level_apply(g_own_level, "invasion over, own level back") >= 0 && pd && !IsBadReadPtr(pd, 0x110))
        *(int *)(pd + 0x10c) = g_own_top_level;
    logf("level: restore done (own level %d, highest level %d)", g_own_level, g_own_top_level);
}
static void join_over(const char *why) {
    level_restore();
    g_ver_notice_armed = 1;  // back in the hub: the version notice may show once more
    if (g_as_ally) {  // the ally's gold look (row 90071611) has no time limit: it goes when our part ends
        char *me = local_player();
        // 0x3cdd40 (chr, id): SpecialEffect remove by id, the shape of Elden Ring's ChrIns remove_speffect
        // (chr+0x190 -> 0x504e80: find the entry with that id, unlink it with 0x50b5b0); 24 calls in the game
        if (me && !IsBadReadPtr(me, 0x200))
            logf("ally: gold look removed at the end (%s): %d", why, ((unsigned char (*)(void *, int))(g_base + 0x3cdd40))(me, 90071611));
    }
    memset(g_hud_invaders, 0, sizeof g_hud_invaders);  // the host clears it in host_member_sync; a joiner here
    if (g_party_guest) g_party = PARTY_OFF, g_party_guest = 0, logf("party: guest's part over (%s)", why);
    g_join = 3;
    g_invader_downs = 0;
    if (g_saved_pending_valid) {  // the pending map state we set at the forced step goes back
        unsigned blk = g_saved_pending[0];
        ((void (*)(unsigned *))(g_base + 0x6b0610))(&blk);
        ((void (*)(unsigned char))(g_base + 0x6b05f0))((unsigned char)g_saved_pending[1]);
        ((void (*)(unsigned char))(g_base + 0x6b0410))((unsigned char)g_saved_pending[2]);
        ((void (*)(int))(g_base + 0x6b0520))((int)g_saved_pending[3]);
        g_saved_pending_valid = 0;
        logf("join test: pending map state back to block %08x +aac %u type %u reason %d", g_saved_pending[0],
             g_saved_pending[1], g_saved_pending[2], (int)g_saved_pending[3]);
    }
    if (g_ctx_old_mode && g_qm_ctrl) {  // the context mode we forced to 1 at the forced step goes back
        char *cx = *(char **)(g_qm_ctrl + 0xa8);
        if (cx && !IsBadReadPtr(cx, 0xa0) && *(char **)cx == g_base + 0x2c75d20) {
            ((void (*)(void *, unsigned char))(*(void ***)cx)[0x48 / 8])(cx, (unsigned char)g_ctx_old_mode);
            logf("join test: context mode back to %u", (unsigned char)cx[0x9d]);
        }
        g_ctx_old_mode = 0;
    }
    {  // a clean start for the next invasion: our +0xe5 on the world sync object goes
        char *sm0 = *(char **)(g_base + RVA_SESSION_MAN_PTR), *sy = sm0 ? *(char **)(sm0 + 0x1b8) : NULL;
        if (sy && !IsBadReadPtr(sy, 0xe8) && sy[0xe5]) sy[0xe5] = 0, logf("join test: sync +0xe5 cleared");
    }
    if (g_move >= 3) g_invasion_ended = 1;
    logf("join test: stopping (%s), leaving and ending the search", why);
    // A forced MoveMapStart / MoveMap would be left behind with no session: back to None, the call
    // our expedition-start cancel makes. FailedStartMultiplay and Unregister are the game's own exits.
    if (g_move_online) {
        g_move_online = 0;
        ((void (*)(unsigned char))(g_base + 0x6b0530))(0);
        logf("move test: online-mode flag gm+0xb40 put back to 0");
    }
    // After a map load the player is in another world: Unregister (0xb52ef0, what the game's return to
    // the title uses: cancels the controller's job chain at +0xb0, then step 25) so the game ends that
    // expedition and goes back to the Roundtable Hold, instead of leaving a stray solo world.
    if (g_move >= 3 && g_qm_ctrl && g_qm_step != QM_STEP_NONE && g_qm_step != 25) {
        logf("move test: map was loaded; controller step %d -> Unregister (0xb52ef0)", g_qm_step);
        ((void (*)(void *))(g_base + RVA_QM_UNREGISTER))(g_qm_ctrl);
        logf("move test: controller +0x40 %d +0x44 %d after Unregister", *(int *)(g_qm_ctrl + 0x40), *(int *)(g_qm_ctrl + 0x44));
        // The way an expedition ends: MoveMapStep's _CheckEndingRequest (0xc37420) turns ending event
        // flags into the game manager requests +0xaf1 / +0xaf2 / +0xaf3 (0x6af8f0, 0x6af900, 0x6af8d0);
        // with one set, MoveMapStep's ending sequence (0xc37850, +0x132 1..4) runs and InGameStep's
        // MoveMap_Update (0xc284b0) goes to Result / EndFlow, which brings the player back to the hub.
        // Every ending, the invader's death included (decision 3: a killed invader returns to the
        // Roundtable Hold and may start another invasion).
        if (1) {
            // reward-test (decision 23): the result screens stay (play state +0x368 is left alone); the reward
            // computation at the expedition's end (0x1fd620, call at 0xc32971) is replaced for this ending by
            // ours: murk only on a win, nothing on a loss.
            // decision 23: won = the host was killed, or the host left the expedition / session on its own;
            // lost = the invader's own death (finished, bled out, fourth down); anything else (a broken
            // connection or a watchdog) counts as neither and gives the smaller amount so the screen has content.
            int died = strstr(why, "invader died") != NULL;
            int host_left = strstr(why, "host migration") || strstr(why, "no longer a client");
            const char *kind = died ? "lost: the invader died" : g_host_killed ? "won: the host was killed"
                             : host_left ? "won: the host left the expedition" : "neither: the session broke off";
            g_reward_pending = g_as_ally ? 0 : !died && (g_host_killed || host_left) ? 1 : 2;
            if (g_as_ally) logf("reward test: ally: the expedition's own result (%s)", why);
            if (!g_as_ally) logf("reward test: invasion %s (%s): reward at the expedition's end: %d murk", kind, why,
                 g_reward_pending == 1 ? REWARD_MURK : REWARD_MURK_LOST);
        }
        {
            ((void (*)(void))(g_base + 0x6af8f0))();
            char *gm = *(char **)(g_base + 0x3c13258);
            logf("move test: expedition end requested (gm+0xaf1 = %u, 0x6af8f0): back to the Roundtable Hold (%s)",
                 gm ? (unsigned char)gm[0xaf1] : 0, strstr(why, "died") ? "invader died" : "invasion over");
        }
    } else if (g_move >= 2 && g_qm_ctrl && (g_qm_step == 21 || g_qm_step == 22)) {
        ((void (*)(void *, int))(*(void ***)g_qm_ctrl)[QM_CTRL_VT_SET_STEP])(g_qm_ctrl, QM_STEP_NONE);
        logf("move test: controller step %d set back to None", g_qm_step);
    }
    toggle_search("join test over");
    if (g_qm_ctrl && g_qm_ctrl[QM_CTRL_REFUSED]) {
        logf("join test: quick-match controller +0x10d was %u (set by a refused join), cleared", (unsigned char)g_qm_ctrl[QM_CTRL_REFUSED]);
        g_qm_ctrl[QM_CTRL_REFUSED] = 0;
    }
}
#define ENTER_WATCHDOG_MS  180000
static DWORD g_enter_tick, g_enter_world;
static int g_enter_effect;
static void enter_watch(char *sm);

// Idle step, every frame while searching with the join test on (and every step while an enter test watches).
static void join_frame(void) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!sm || g_join == 3) return;
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    if (g_join == 0) {
        if (!g_candidate) return;
        void **impl = session_impl(sm);
        if (!((unsigned char (*)(void))(g_base + RVA_NET_READY))() || !impl || !*(unsigned *)((char *)impl + 0x10)) {
            static DWORD told;
            if (!told || GetTickCount() - told > 10000) logf("join test: candidate %llu waiting for the network", g_candidate);
            told = GetTickCount();
            return;
        }
        if ((state != 0 && state != 2 && state != 5) || sm[SM_LEAVING]) {
            logf("join test: lobby_state %u leaving %u, the manager would refuse a join", state, (unsigned char)sm[SM_LEAVING]);
            join_over("session manager busy");
            return;
        }
        {   // Run 09f36c57: every repeated join in one game session failed at once, and its sync object
            // still held the last invasion's phase (5 or 6; a first join starts at 0). The phase is
            // written by the sync start only for the role-8 path and by the steps; put it back to 0.
            char *sy = *(char **)(sm + 0x1b8);
            if (sy && !IsBadReadPtr(sy, 0xe8) && sy[0xd8]) {
                logf("join test: sync phase %u left from the last session, set back to 0", (unsigned char)sy[0xd8]);
                sy[0xd8] = 0;
            }
            if (sm[0x1d]) logf("join test: session manager +0x1d (a deferred leave) is %u before the join", (unsigned char)sm[0x1d]);
        }
        g_join_data[0] = g_candidate;
        void *vector[2] = {g_join_data, g_join_data + 1};  // {begin, end}, as 0xdf0f70 reads it
        unsigned char one = 1;
        g_join_tick = GetTickCount();
        logf("join test: joining lobby %llu (type %d, title flow 1)", g_candidate, MULTIPLAY_WHITE);
        unsigned char ok = ((unsigned char (*)(void *, unsigned char, void *, unsigned char, unsigned char *))(
            g_base + RVA_SESSION_JOIN))(sm, MULTIPLAY_WHITE, vector, 1, &one);
        logf("join test: join -> %u, lobby_state %u -> %u, join handle %u", ok, state, *(unsigned *)(sm + SM_LOBBY_STATE),
             *(unsigned *)(sm + SM_JOIN_HANDLE));
        join_watch(sm);
        if (!ok) {
            join_over("join refused");
            return;
        }
        g_join = 1;
        return;
    }
    join_watch(sm);
    steam_watch("join test", g_candidate);
    DWORD waited = GetTickCount() - g_join_tick;
    if (g_join == 1) {
        // Our role goes into our lobby member data as soon as Steam lists us in the lobby: the host's member
        // add (and its slot choice, capacity.h slot_alloc) comes only at the session join, ~4 s later (host run
        // 76: "lobby members 2" at 21:05:57, "got slot" at 21:06:01 with our lobby_state 6). State 6 sets it
        // again in case the member list was not readable.
        static DWORD role_for;  // g_join_tick of the join whose role is set
        if (role_for != g_join_tick && g_diag.set_member && g_diag.member && g_diag.self && g_diag.user && g_steam.mm) {
            u64 me = g_diag.self(g_diag.user);
            int n = g_steam.members(g_steam.mm, g_candidate);
            for (int i = 0; i < n && i < 8; i++)
                if (g_diag.member(g_steam.mm, g_candidate, i) == me) {
                    role_for = g_join_tick;
                    g_diag.set_member(g_steam.mm, g_candidate, LOBBY_ROLE_KEY, g_as_ally ? "ally" : "invader");
                    g_diag.set_member(g_steam.mm, g_candidate, LOBBY_VER_KEY, MOD_VERSION);
                    logf("join: in the Steam lobby after %lums, lobby member data %s=%s set", waited, LOBBY_ROLE_KEY,
                         g_as_ally ? "ally" : "invader");
                    break;
                }
        }
        if (state == 6) {
            g_join = 2;
            g_join_tick = GetTickCount();
            if (g_diag.set_member && g_steam.mm) {
                g_diag.set_member(g_steam.mm, g_candidate, LOBBY_ROLE_KEY, g_as_ally ? "ally" : "invader");
                g_diag.set_member(g_steam.mm, g_candidate, LOBBY_VER_KEY, MOD_VERSION);
                logf("join: lobby member data %s=%s set", LOBBY_ROLE_KEY, g_as_ally ? "ally" : "invader");
            }
            if (g_enter_test) {
                g_join = 4;
                g_enter_tick = GetTickCount();
                g_enter_world = 0;
                g_enter_effect = 0;
                g_move = 0;
                logf("join test: joined as client after %lums; enter test: watching for up to %ums", waited, ENTER_WATCHDOG_MS);
            } else
                logf("join test: joined as client after %lums. Entering the host's world is not built: holding %ums, then leaving",
                     waited, JOIN_HOLD_MS);
            log_members(sm, "join test");
            log_pending_state("join test");
        } else if (state == 0 || state == 2 || state == 5) join_over("join failed");
        else if (waited > JOIN_TIMEOUT_MS) join_over("join timed out");
    } else if (state == 3) join_over("the host left and the session passed to us (host migration): invasion over");
    else if (state != 6) join_over("no longer a client");
    else if (g_join == 4) {
        enter_watch(sm);
        if (!g_enter_world && GetTickCount() - g_enter_tick > ENTER_WATCHDOG_MS) join_over("enter test watchdog: no way into the world");
    } else if (waited > JOIN_HOLD_MS) join_over("hold time over");
}
// Leave a session we joined. Returns 1 if a leave was requested (the network then waits for it).
static int join_leave(void) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!g_join_test || !sm) return 0;
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    if (state == 7) {  // the game is already leaving: wait for it before the network goes down
        logf("join test: the session is already leaving (lobby_state 7); waiting for it");
        return 1;
    }
    if (state != 4 && state != 6) return 0;
    ((void (*)(void *, unsigned char))(g_base + RVA_SESSION_LEAVE))(sm, 0);
    logf("join test: leave session, lobby_state %u -> %u", state, *(unsigned *)(sm + SM_LOBBY_STATE));
    return 1;
}

// --- Team test, only with team-test = true in the test settings ---------------------------------
//
// Puts the local player on team 16 for 180 s, 90 s after the expedition reaches its world, through
// our SpEffect row 90071610 (build_regulation.py: copy of the neutral row 10101 with
// effectEndurance 180, changeTeamType 16, vfxId1 300050 = red phantom look).
// Calls, all read:
//   0x3c7fd0 (ChrIns*, int SpEffect id, u8 0): the game's "add SpEffect to this character" (136
//     call sites, nearly all (chr, constant id, 0)); it fills default vectors and calls the full
//     apply 0x3c80a0 with the character as its own source. Only the wrapper is read in full; the
//     apply behind it is long and read only in part (the team step is in 0x3db410: SpEffect row
//     +0x16d < 81 -> temporary-team slot at ChrIns+0x338).
//   0x3d1b30 (ChrIns*, u8 *out): effective team = byte ChrIns+0x74, 15 while a SpEffect with
//     state 0x84 is on (0x508e20), else the temporary-team slot's value when one is set.
//   0xea17e0 (ref*, u32 id): SpEffectParam row lookup (param 15), to recognise our effect.
// Local player: WorldChrMan (0x3c0f0a8) +0x174e8, as the game's own code tests it (e.g. 0x3db410).
// Active effects: the character's SpEffect module (ChrIns+0x190), list from +8, next at entry+0x38,
// entry+0 the param row, entry+0x68 flags (0x800c0003 = not active), as 0x508e20 walks it.
// The effect ends by itself (effectEndurance); nothing removes it. Its row has saveCategory -1.
#define RVA_WORLD_CHR_MAN  0x3c0f0a8
#define WCM_LOCAL_PLAYER   0x174e8
#define RVA_ADD_SPEFFECT   0x3c7fd0
#define RVA_CHR_TEAM       0x3d1b30
#define RVA_SPEFFECT_ROW   0xea17e0
#define CHR_BASE_TEAM      0x74
#define CHR_SPEFFECTS      0x190
#define TEAM_TEST_SPEFFECT 90071610
#define ALLY_LOOK_SPEFFECT 90071611  // the ally's gold phantom look only (launcher: SPEFFECT_ALLY_ROW)
#define ALLY_LOOK_RENEW_MS 20000
static DWORD g_team_tick;
static char *local_player(void) {
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    return wcm ? *(char **)(wcm + WCM_LOCAL_PLAYER) : NULL;
}
// The active entry of the effect row on chr, or NULL. Every pointer of the walk is checked first: these walks run
// on other players' characters too (invasion notices, HUD, finish rules), which may be half built while a new
// member's character is created (3-player sessions, 15.11: two joiners crashed around a newcomer's arrival).
static char *effect_entry_of(char *chr, char *row) {
    if (!chr || !row || IsBadReadPtr(chr + CHR_SPEFFECTS, 8)) return NULL;
    char *module = *(char **)(chr + CHR_SPEFFECTS);
    if (!module || IsBadReadPtr(module + 8, 8)) return NULL;
    int n = 0;
    for (char **e = *(char ***)(module + 8); e && n < 512; e = (char **)e[7], n++) {
        if (IsBadReadPtr(e, 0x70)) return NULL;
        if (e[0] == row && !(*(unsigned *)((char *)e + 0x68) & 0x800c0003)) return (char *)e;
    }
    return NULL;
}
static int our_speffect_active(char *chr) {
    struct { char *row; unsigned id; unsigned char flag; } ref = {0};
    ((void (*)(void *, unsigned))(g_base + RVA_SPEFFECT_ROW))(&ref, TEAM_TEST_SPEFFECT);
    if (!ref.row) return -1;  // row not in regulation.bin
    return effect_entry_of(chr, ref.row) != NULL;
}
// no-sprint (test setting, default on): no sprint for our character. env_wrap makes the script skip the sprint
// start (ExecSprintStart is not called, so the button changes nothing); the script also sets MoveSpeedIndex 3
// directly (act 148), which act_wrap turns into 2.
// Until 0998865 the start was blocked with SpEffect 102610 instead: in the game that is the spirit-hawk ride marker
// (common.emevd event 1161: player has 102610 and not 99230 for 2 s -> OperationGuideParam 101, "Disembark";
// event 1111 and 90035204/90035221/90035227 of common_func wait for it to end), so the HUD showed "Disembark".
// Before that it was 100220, which made SpeedUpdate set run speed before the stick (no walking).
static int speffect_on(char *chr, unsigned id) {
    struct { char *row; unsigned id; unsigned char flag; } ref = {0};
    ((void (*)(void *, unsigned))(g_base + RVA_SPEFFECT_ROW))(&ref, id);
    return effect_entry_of(chr, ref.row) != NULL;
}
static char *speffect_entry(char *chr, unsigned id) {  // the active entry of SpEffect id on chr, or NULL
    struct { char *row; unsigned id; unsigned char flag; } ref = {0};
    ((void (*)(void *, unsigned))(g_base + RVA_SPEFFECT_ROW))(&ref, id);
    return effect_entry_of(chr, ref.row);
}

// No sprint, second half: the script also puts our character into the sprint without ExecSprintStart, by
// SetVariable('MoveSpeedIndex', 3) in Departure_FallEnd_onActivate / onUpdate and BirdAct_FallEnd_onUpdate (after a
// hawk landing), Sprint180_onActivate and SpeedUpdate pc 446 / 463; while the index is 3 the sprint lasts as long as
// the character moves (Update pc 454-611 keeps it). The script's SetVariable is act(148, name, value). HKS `act`
// (0x141624140) calls the pointer at 0x14467afe8 (set to 0x140d40310 by 0x140d400b0): it takes the owner from
// 0x1415c93c0(L), the id with 0x140d405d0(L, 1) and calls the handler 0x140407ff0 (registered by 0x140d7df96 through
// 0x140d585b0). We wrap that pointer: for act 148 "MoveSpeedIndex" = 3 on our own character (see act_wrap for the
// chain to the ChrIns) the value on the HKS stack becomes 2 (run) before the game reads it. Stack layout from
// 0x14167dc40 (push number) and 0x141676f10: slots of 16 bytes from L+0x50, type at +0 (3 = number), float at +8.
// Name from 0x140d40640(L, 2), as act 148 in the dispatcher 0x1403f5870 reads it.
static int (*g_act_real)(void *);
static volatile LONG g_act_seen, g_act_ours, g_act_all, g_act_148;
// L3 as a held B (owner, 2026-11-19: "L3 emitowal wcisniety przycisk kolko/B"; sprint stays off). The script's speed
// is MoveSpeedIndex, set only through ChangeMoveSpeedIndex -> SetVariable (act 148): SpeedUpdate gives 0 at a light
// tilt, 1 above 0.4, 2 above 1.1 and 3 the sprint. MoveSpeedLevel (the engine's, GetVariable) goes above 1.1 only with
// the dash held, so a held B is the 2 at full tilt; we take that from the script's own value: while the toggle is on,
// our character's 1 becomes 2 (3 becomes 2 always). The dash/sprint button is env 1108 ACTION_ARM_L3: SpeedUpdate
// pc 529-560 / SpeedUpdate2 ask env(1116, 102610) only while it is held (and stamina >= 0), every frame; our env_wrap
// answers that, so a question after more than RUN_PRESS_GAP_MS is a new press and flips the toggle. ACTION_ARM_*
// values are game-side globals (not in the script): every env(1108, x) of our character is logged once per (x, answer)
// so a run shows which x is B and which L3. ponytail: if a held B also asks the sprint question, a B hold flips the
// toggle too; the logged ids are the way to tell them apart.
#define RUN_PRESS_GAP_MS 300
static volatile int g_run_held;  // 1: the toggle is on (our character runs as with B held)

static int act_wrap(void *L) {
    if (L && !IsBadReadPtr((char *)L + 0x48, 16)) {
        char *base = *(char **)((char *)L + 0x50), *top = *(char **)((char *)L + 0x48);
        InterlockedIncrement(&g_act_all);
        if (base && top >= base + 16 && *(unsigned *)base == 3 && *(float *)(base + 8) == 148.0f) InterlockedIncrement(&g_act_148);
        if (base && top >= base + 3 * 16 && *(unsigned *)base == 3 && *(float *)(base + 8) == 148.0f && *(unsigned *)(base + 32) == 3 &&
            (*(float *)(base + 40) == 3.0f || (g_run_held && *(float *)(base + 40) == 1.0f))) {
            const char *nm = ((const char *(*)(void *, int))(g_base + 0xd40640))(L, 2);  // as act 148 reads it
            if (nm && !strcmp(nm, "MoveSpeedIndex")) {
                InterlockedIncrement(&g_act_seen);
                // The character the act works on, by the game's own chain: 0x140d40310 passes r8 = *(owner+0) +0x28
                // (owner = 0x1415c93c0(L)); the handler 0x140407ff0 takes obj = [[r8+0x1b8]+0x10]+0x10 and calls
                // the dispatcher 0x1403f5870(obj, id, L), whose act 2002 adds a SpEffect with 0x1403c7fd0(*obj, ..)
                // and act 148 (CSBehaviorScriptAct::_SetVariable) writes into (*obj)+0x1b8 -> +0x28: *obj is the
                // ChrIns. We compare that same *obj with the local player.
                void **x = ((void **(*)(void *))(g_base + 0x15c93c0))(L);
                char *r8 = x && !IsBadReadPtr(x, 8) && *x && !IsBadReadPtr((char *)*x + 0x28, 8) ? *(char **)((char *)*x + 0x28) : NULL;
                char *m = r8 && !IsBadReadPtr(r8 + 0x1b8, 8) ? *(char **)(r8 + 0x1b8) : NULL;
                char *m2 = m && !IsBadReadPtr(m + 0x10, 8) ? *(char **)(m + 0x10) : NULL;
                char **obj = m2 && !IsBadReadPtr(m2 + 0x10, 8) ? *(char ***)(m2 + 0x10) : NULL;
                char *chr = obj && !IsBadReadPtr(obj, 8) ? *obj : NULL;
                if (chr && chr == local_player()) InterlockedIncrement(&g_act_ours), *(float *)(base + 40) = 2.0f;
            }
        }
    }
    return g_act_real(L);
}
static char *script_chr(void *L) {  // the ChrIns whose script L is (act and env use the same chain up to r8)
    void **x = ((void **(*)(void *))(g_base + 0x15c93c0))(L);
    char *r8 = x && !IsBadReadPtr(x, 8) && *x && !IsBadReadPtr((char *)*x + 0x28, 8) ? *(char **)((char *)*x + 0x28) : NULL;
    char *m = r8 && !IsBadReadPtr(r8 + 0x1b8, 8) ? *(char **)(r8 + 0x1b8) : NULL;
    char *m2 = m && !IsBadReadPtr(m + 0x10, 8) ? *(char **)(m + 0x10) : NULL;
    char **obj = m2 && !IsBadReadPtr(m2 + 0x10, 8) ? *(char ***)(m2 + 0x10) : NULL;
    return obj && !IsBadReadPtr(obj, 8) ? *obj : NULL;
}
// The sprint start, without a SpEffect: SpeedUpdate pc 522-560 / SpeedUpdate2 call ExecSprintStart only while
// ISENABLE_SPRINTSTART, the button (env 1108 ACTION_ARM_L3 > 0), no SetSlowFeet, stamina env(1001) >= 0, and
// env(1116, 102610) and env(1116, 102001) are FALSE. HKS `env` (0x141624170) calls the pointer at 0x14467afe0 (set to
// 0x140d403f0 by 0x140d400b0), which passes r8 = *(0x1415c93c0(L))+0x28 to the handler 0x140408030 (registered by
// 0x140d7dfdc through 0x140d58780): obj = [[r8+0x1b8]+0x10]+0x18, dispatcher 0x1403f7b50(obj, id, L), whose env 1116
// is 0x1403d2100(*obj, id): "has SpEffect" on the ChrIns *obj (chr+0x190 list). The binding pushes the returned
// number (0x141624130); TRUE is 1. For env(1116, 102610) on our character we answer 1 without the SpEffect being
// there: the script then skips the sprint start (pc 547) exactly as with the old marker, and nothing else sees it
// (the events and the guide test the real effect). Script uses of env(1116, 102610): SpeedUpdate pc 544,
// SpeedUpdate2 pc 402, ExecDashTurn pc 42 (turn animation, only with 100170).
static int (*g_env_real)(void *);
static volatile LONG g_env_answered;
static int env_wrap(void *L) {
    if (L && !IsBadReadPtr((char *)L + 0x48, 16)) {
        char *base = *(char **)((char *)L + 0x50), *top = *(char **)((char *)L + 0x48);
        if (base && top >= base + 2 * 16 && *(unsigned *)base == 3 && *(float *)(base + 8) == 1116.0f && *(unsigned *)(base + 16) == 3 &&
            *(float *)(base + 24) == 102610.0f) {
            char *chr = script_chr(L);
            if (chr && chr == local_player()) {
                static DWORD last;
                DWORD now = GetTickCount();
                if (now - last > RUN_PRESS_GAP_MS) {
                    g_run_held = !g_run_held;
                    logf("run: L3: %s", g_run_held ? "on (as with B held)" : "off");
                }
                last = now;
                InterlockedIncrement(&g_env_answered);
                return 1;
            }
        }
    }
    int is1108 = 0;
    float arg = 0;
    if (L && !IsBadReadPtr((char *)L + 0x48, 16)) {
        char *base = *(char **)((char *)L + 0x50), *top = *(char **)((char *)L + 0x48);
        if (base && top >= base + 2 * 16 && *(unsigned *)base == 3 && *(float *)(base + 8) == 1108.0f && *(unsigned *)(base + 16) == 3)
            is1108 = 1, arg = *(float *)(base + 24);
    }
    int r = g_env_real(L);
    if (is1108) {
        char *top = *(char **)((char *)L + 0x48);
        float ans = top && !IsBadReadPtr(top - 16, 16) && *(unsigned *)(top - 16) == 3 ? *(float *)(top - 8) : -1;
        static float seen[32][2];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i][0] == arg && seen[i][1] == ans;
        if (!known && nseen < 32) {
            char *chr = script_chr(L);
            if (chr && chr == local_player()) {
                seen[nseen][0] = arg, seen[nseen][1] = ans, nseen++;
                logf("input: env(1108, %.0f) = %.0f on our character (button ids: B and L3 are among these)", arg, ans);
            }
        }
    }
    return r;
}

// Map pins placed by players (decision: the other side's pins are not seen). CS::CSPinManImp (global 0x143c17be0)
// reads them in its update 0x140ba0d90: packet 0x73 (40 bytes, the pin: kind < 3, pin id with the owner's player
// number in the high 16 bits, position; checked by 0x140de4ac0) through 0x140ddb9c0, and packet 0x74 (12 bytes,
// a reaction to a pin, sent to the pin's owner by 0x140ddf7f0) through 0x140ddb900; both use 0x140df1de0, which
// also gives the sender's Steam id. Kind 0 creates the pin in the manager's list, which the map, the compass and
// the beacon draw from (0x140ee1fb0 walks the same list). We read on in a loop and leave out every packet whose
// sender is on the other side: the invader drops the host's and the allies' pins, the host and the allies drop
// the invaders'. Side by the lobby: the owner and members with role "ally" are the host's side. System markers
// (graces, bosses, events) are not pins of this manager and are not touched.
static int pin_from_other_side(u64 sender) {
    u64 lobby = our_lobby();
    if (!lobby || !sender || !g_steam.mm || !g_diag.owner) return 0;
    u64 owner = g_diag.owner(g_steam.mm, lobby);
    int sender_invader = sender != owner && !member_ally(lobby, sender), we_invader = g_join == 4 && !g_as_ally;
    return sender_invader != we_invader;
}
static unsigned long long pin_read(unsigned rva, int type, void *a, u64 *sender, void *buf) {
    for (;;) {
        unsigned long long r = ((unsigned long long (*)(void *, u64 *, void *))(g_base + rva))(a, sender, buf);
        if (!(r & 0xff) || !sender || !pin_from_other_side(*sender)) return r;
        static DWORD next;
        if ((int)(GetTickCount() - next) >= 0)
            next = GetTickCount() + 5000, logf("map pin: packet 0x%x from member %04x of the other side left out", type, id_tag(*sender));
    }
}
static unsigned long long pin_read_73(void *a, unsigned long long *sender, void *buf) { return pin_read(0xddb9c0, 0x73, a, sender, buf); }
static unsigned long long pin_read_74(void *a, unsigned long long *sender, void *buf) { return pin_read(0xddb900, 0x74, a, sender, buf); }
// Over-head name of other players (CSFeMan 0x143c14a70): 0x1407d9f80 (called at 0x140c3bb92) fills 7 tag slots,
// FE +0xd350 + i*0x60, for the members of the party list (0x140aed2a0): name, screen position, +0x18 "shown"
// (1 while on screen; no distance limit in the code), +0x50 the character's handle. The draw 0x1407cefc0 draws a
// slot only while +0x18 is set (and looks the handle up with 0x140518f30). The invader is in the party list on the
// host, so the host saw his name at any distance. After the fill, on the host's side, a slot whose character
// carries our team effect is set not shown. The enemy tag (FE +0xca60, name with HP bar, added by 0x1407c5710 when
// a hostile player loses HP) is the game's own and stays. On the invader the party lookups already give no
// character (hud_lookup_hook), so he sees no over-head names of the host's side either.
static void remote_tags_hook(void *a, void *b, void *c, void *d) {
    ((void (*)(void *, void *, void *, void *))(g_base + 0x7d9f80))(a, b, c, d);
    if (g_join == 4 && !g_as_ally) return;
    char *fe = *(char **)(g_base + 0x3c14a70), *wcm = *(char **)(g_base + 0x3c0f0a8);
    if (!fe || !wcm || IsBadReadPtr(fe + 0xd350, 7 * 0x60)) return;
    for (int i = 0; i < 7; i++) {
        char *slot = fe + 0xd350 + i * 0x60;
        if (!slot[0x18]) continue;
        char *chr = ((char *(*)(void *, void *))(g_base + 0x518f30))(wcm, slot + 0x50);
        if (chr && !IsBadReadPtr(chr, 0x200) && our_speffect_active(chr) == 1) {
            slot[0x18] = 0;
            static DWORD next;
            if ((int)(GetTickCount() - next) >= 0) next = GetTickCount() + 10000, logf("name tag: over-head name of a character with our team effect not shown");
        }
    }
}

// Stage 1 (team play): invaders do not count toward enemy scaling. The game's count 0x140519660 = DAT_143c0f320 if > 0
// (debug), else max(1, members of the session, gm+0xdf6); 0x1403db250 applies MultiPlayCorrectionParam client1/2
// (n = 2 / 3) or removes the correction (n = 1, or 4+ where client3 is -1 in every row). Its only callers:
// 0x1403db2b9 (apply), 0x1403c7ee8 (0x1403c7e50: re-apply check against chr+0x238), 0x1403d2d3d (0x1403d27b0). The
// frame code 0x140521ff0 (0x140522180-0x14052229c) recomputes the same count inline and, when it differs from
// WCM+0x17558, re-applies to every character (ChrFinder -> 0x1405186a0 -> 0x1403c7e50) and stores it there.
// Our count: the session members on the host's side (the lobby owner and members with role "ally"), the same on
// every machine (the member list and the lobby data are the same everywhere); the debug value is kept. When it
// changes we set WCM+0x17558 to -1 (written only by 0x140515058 at creation and 0x140522295), so the game's own
// frame code re-applies with it.
static int g_squad_n;  // 0 = not computed (no lobby): the game's own count
static void squad_count_tick(void) {  // game thread (step hook), every 500 ms
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 500;
    u64 lobby = our_lobby();
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    int n = 0;
    if (lobby && sm && g_diag.owner && g_steam.mm) {
        u64 owner = g_diag.owner(g_steam.mm, lobby);
        char *b = *(char **)(sm + SM_PLAYERS_BEGIN), *e = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
        for (char *m = b; b && m < e && !IsBadReadPtr(m, SM_PLAYER_SIZE); m += SM_PLAYER_SIZE) {
            u64 id = *(u64 *)(m + 0x10);
            int invader = m[0xe1] ? (g_join == 4 && !g_as_ally) : (id != owner && !member_ally(lobby, id));
            if (!invader) n++;
        }
        if (n < 1) n = 1;
    }
    if (n != g_squad_n) {
        // 0x140519660 ends in the game's fatal-error path (write to 0) when its manager 0x143c0f338 is not there
        // yet / any more (crash of a Windows invader, exe+0x214e298 from here): read the count only when it is.
        int real = *(void **)(g_base + 0x3c0f338) ? ((int (*)(void))(g_base + 0x519660))() : -1;
        logf("scaling: squad of %d for enemy scaling (game's count with invaders %d)", n ? n : real, real);
        g_squad_n = n;
        char *wcm = *(char **)(g_base + 0x3c0f0a8);
        if (wcm && !IsBadReadPtr(wcm + 0x17558, 4)) *(int *)(wcm + 0x17558) = -1;  // the game re-applies next frame
    }
}
static int squad_count_hook(void) {
    if (*(int *)(g_base + 0x3c0f320) > 0 || !g_squad_n) return ((int (*)(void))(g_base + 0x519660))();
    return g_squad_n;
}
static void act_wrap_tick(void) {  // run thread: put the wrapper in (again, if the game set the pointer anew)
    void **slot = (void **)(g_base + 0x467afe8), **eslot = (void **)(g_base + 0x467afe0);
    if (*slot == (void *)(g_base + 0xd40310)) {
        g_act_real = (int (*)(void *))*slot;
        *slot = (void *)act_wrap;
        logf("no sprint: HKS act wrapped (MoveSpeedIndex 3 -> 2 on our character)");
    }
    if (*eslot == (void *)(g_base + 0xd403f0)) {
        g_env_real = (int (*)(void *))*eslot;
        *eslot = (void *)env_wrap;
        logf("no sprint: HKS env wrapped (sprint start refused for our character)");
    }
    static DWORD next;
    if ((g_act_all || g_env_answered) && (int)(GetTickCount() - next) >= 0) {  // every 30 s while scripts run
        next = GetTickCount() + 30000;
        logf("no sprint: act calls %ld, act 148 (SetVariable) %ld, MoveSpeedIndex = 3 %ld, on our character %ld (set to 2); "
             "sprint start refused %ld times", g_act_all, g_act_148, g_act_seen, g_act_ours, InterlockedExchange(&g_env_answered, 0));
        g_act_seen = g_act_ours = g_act_all = g_act_148 = 0;
    }
}
static double sqrt_approx(double x) {  // no math.h here (its logf clashes with ours)
    if (x <= 0) return 0;
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}
static void no_sprint_frame(void) {
    char *chr = local_player();
    if (!chr || IsBadReadPtr(chr, 0x200)) return;
    // Movement check: the local character's ground speed each frame (position from 0x3d0e60, x/z), counted in bands
    // for 15 s, with the sprint starts we dropped. Walk, run and sprint speeds come from animation root motion, not
    // from code. Frames above 30 m/s (warps, block changes) are skipped.
    static float last[4];
    static double last_t, next_move;
    static int band[6], moving, sampled;
    static float top;
    float pos[4];
    double t = lag_ms();
    ((float *(*)(char *, float *))(g_base + 0x3d0e60))(chr, pos);
    if (last_t > 0 && t > last_t) {
        float dx = pos[0] - last[0], dz = pos[2] - last[2];
        double v = sqrt_approx(dx * dx + dz * dz) / ((t - last_t) / 1000.0);
        if (v < 30) {
            int k = v < 0.5 ? 0 : v < 2 ? 1 : v < 4 ? 2 : v < 6 ? 3 : v < 8 ? 4 : 5;
            band[k]++, sampled++;
            if (k) moving++;
            if (v > top) top = (float)v;
        }
    }
    memcpy(last, pos, sizeof last), last_t = t;
    if (!next_move) next_move = t + 15000;
    if (t >= next_move) {
        if (moving)
            logf("move check: %d frames; ground speed m/s: still %d, 0.5-2 %d, 2-4 %d, 4-6 %d, 6-8 %d, 8+ %d, top %.1f",
                 sampled, band[0], band[1], band[2], band[3], band[4], band[5], top);
        memset(band, 0, sizeof band), moving = sampled = 0, top = 0, next_move = t + 15000;
    }
}
static void summon_kill_reward_give(unsigned long long summon) {
    char *gdm = *(char **)(g_base + 0x3c078d0), *pd = gdm ? *(char **)(gdm + 8) : NULL;
    if (!pd || IsBadReadPtr(pd, 0x120)) return;
    int before = *(int *)(pd + 0x6c);
    ((int (*)(char *, int))(g_base + 0x2098c0))(pd, REWARD_RUNES_SUMMON_KILL);
    logf("summon kill: we finished the host's summon (member %04x): %d runes added (%d -> %d)", id_tag(summon), REWARD_RUNES_SUMMON_KILL,
         before, *(int *)(pd + 0x6c));
}
// The summon's side: name the invader who finished us (see LOBBY_FINISHED_KEY).
static void summon_finished_by(char *attacker) {
    static int count;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR), num[8];
    if (!sm || !attacker || IsBadReadPtr(attacker, 0x200) || !g_diag.set_member || !g_candidate) return;
    int pn = *(int *)((char *(*)(void *, void *))(g_base + 0x66c6f0))(attacker, num);
    char *b = *(char **)(sm + SM_PLAYERS_BEGIN), *e = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    for (char *x = b; b && x < e && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE)
        if (!x[0xe1] && *(int *)(x + 0xd4) == pn) {
            char text[24];
            snprintf(text, sizeof text, "%d,%d", pn, ++count);
            g_diag.set_member(g_steam.mm, g_candidate, LOBBY_FINISHED_KEY, text);
            logf("summon kill: finished by player %d, count %d published", pn, count);
            return;
        }
    logf("summon kill: finisher (player %d) not found among the session's members", pn);
}
// The invader's side: one reward per new count that names us.
static void summon_kill_reward(void) {
    static DWORD next, for_enter;
    static unsigned long long who[8];
    static int seen[8];
    if (!g_invader_in_world() || !g_diag.get_member || !g_diag.member || !g_candidate) return;
    if (for_enter != g_enter_tick) for_enter = g_enter_tick, memset(who, 0, sizeof who), memset(seen, 0, sizeof seen), next = 0;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 500;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);  // our own session player number (our member's +0xd4)
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    int self = -1;
    for (char *x = b; b && x < e && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE)
        if (x[0xe1]) self = *(int *)(x + 0xd4);
    if (self < 1) return;
    int n = g_steam.members(g_steam.mm, g_candidate);
    for (int i = 0; i < n && i < 8; i++) {
        unsigned long long id = g_diag.member(g_steam.mm, g_candidate, i);
        const char *v = id ? g_diag.get_member(g_steam.mm, g_candidate, id, LOBBY_FINISHED_KEY) : NULL;
        int named = 0, count = 0;
        if (!v || sscanf(v, "%d,%d", &named, &count) != 2) count = 0;
        int k = 0;
        while (k < 8 && who[k] && who[k] != id) k++;
        if (k == 8) continue;
        if (!who[k]) { who[k] = id, seen[k] = count; continue; }  // the first look: what was there before we came
        if (count > seen[k]) {
            seen[k] = count;
            if (named == self) summon_kill_reward_give(id);
        }
    }
}
// Short-log markers for crash hunting ("crash watch: " is a key line): another member's slot state reaching 0x1f
// (the game then creates its character, 0xc43900), and each new character of another player in the world, with
// our own role. Run from the game thread (step hook) on every side.
static void crash_watch_members(void) {
    static DWORD next;
    static int last_state[8], last_no[8];
    static char *chr_seen[8];
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 100;
    const char *role = g_join == 4 ? (g_as_ally ? "ally" : "invader") : g_host_test == 2 ? "host" : "no session role";
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR), *pd = *(char **)(g_base + 0x3c078d0);
    char *b = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *e = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    int i = 0, id_stale = -1, id_had = -1, id_slot = 0;
    for (char *m = b; b && m < e && i < 8 && !IsBadReadPtr(m, SM_PLAYER_SIZE); m += SM_PLAYER_SIZE, i++) {
        int slot = *(int *)(m + 0xd0), no = *(int *)(m + 0xd4), st = -1;
        if (!m[0xe1] && pd && slot >= 0 && slot < 8 && *(char **)(pd + 0x18) && !IsBadReadPtr(*(char **)(pd + 0x18) + slot, 1))
            st = (unsigned char)(*(char **)(pd + 0x18))[slot];
        if (st == 0x1f && (last_state[i] != 0x1f || last_no[i] != no))
            logf("crash watch: member player %d (slot %d) state 1f: its character is made now (we are the %s)", no, slot, role);
        last_state[i] = st, last_no[i] = no;
        char *sd = !m[0xe1] && pd && slot >= 0 && slot < g_cap_remote && *(char **)(pd + 0x10) ? *(char **)(pd + 0x10) + slot * 0x8a0 : NULL;
        if (sd && no != -1 && !IsBadReadPtr(sd + 8, 4) && *(int *)(sd + 8) != no) id_stale = no, id_had = *(int *)(sd + 8), id_slot = slot;
    }
    for (; i < 8; i++) last_state[i] = -1;
    // Player ids (the player data +8 each player-handle lookup 0x5197e0 matches, and the id-keyed reads such as
    // 0x209aa0 / 0x209bc0): the host's member add (0xdf5ae0) writes a newcomer's session number there only in
    // lobby state 3 (the host); everywhere else only the game's sync 0x1fc260 does, from member +0xd4 for every
    // member, and the game runs it at map entry (0xc27870). A member who arrives after a joiner's map entry (ours
    // come mid-expedition) stays -1 on that joiner: its packets find no character there. So on any side, when a
    // member with a session number has another id in its slot data, we run that same sync (only online:
    // GameDataMan +0xd8 == 2, the branch that copies; the other branch resets the ids). At most once per second.
    static DWORD id_next;
    if (id_stale != -1 && pd && !IsBadReadPtr(pd + 0xd8, 1) && pd[0xd8] == 2 && (int)(GetTickCount() - id_next) >= 0) {
        id_next = GetTickCount() + 1000;
        ((void (*)(void))(g_base + 0x1fc260))();
        char *sd = *(char **)(pd + 0x10) + id_slot * 0x8a0;
        logf("crash watch: member player %d (slot %d) had player id %d here; the game's id sync run, now %d (we are the %s)", id_stale,
             id_slot, id_had, *(int *)(sd + 8), role);
    }
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN), *me = wcm ? *(char **)(wcm + WCM_LOCAL_PLAYER) : NULL;
    void **set = wcm ? (void **)(wcm + 0x9a40) : NULL;
    if (!set || IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    for (int k = 0; k < count && k < 8; k++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)k);
        if (c && c != me && c != chr_seen[k] && !IsBadReadPtr(c, 0x200))  // no game call on a character still being built
            logf("crash watch: new character of another player in the world (entry %d, base team %u, we are the %s)", k,
                 (unsigned char)c[CHR_BASE_TEAM], role);
        chr_seen[k] = c;
    }
    for (int k = count; k < 8; k++) chr_seen[k] = NULL;
}
// Invasion notices on the host's and every ally's screen: an invader is a remote player character that carries
// our team effect 90071610 (its own machine puts it on at arrival; the others get it through the effect sync, as
// the host's logs of every invasion show: "base team 2 effective 16 our effect 1"). Once per character: the
// effect appearing = arrived (the character is in our world and admitted); the effect gone while the character is
// still there = died (death removes it); the character gone with the effect still on = left.
static void invader_notice_frame(void) {
    static char *seen[8];
    static int on[8], told[8];
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 250;
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN), *me = wcm ? *(char **)(wcm + WCM_LOCAL_PLAYER) : NULL;
    if (!wcm || g_invader_in_world()) return;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    char *now[8] = {0};
    for (int i = 0; i < count && i < 8; i++) {
        char *c = ((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i);
        if (c && c != me && !IsBadReadPtr(c, 0x200)) now[i] = c;
    }
    for (int k = 0; k < 8; k++) {  // characters we followed: still there?
        if (!seen[k]) continue;
        int present = 0;
        for (int i = 0; i < 8; i++) present |= now[i] == seen[k];
        if (!present) {
            if (on[k] && told[k] == 1) notice(5), logf("invasion notice: the invader left");
            seen[k] = NULL, on[k] = told[k] = 0;
        }
    }
    for (int i = 0; i < 8; i++) {
        char *c = now[i];
        if (!c) continue;
        int k = 0;
        while (k < 8 && seen[k] != c) k++;
        if (k == 8) {
            for (k = 0; k < 8 && seen[k]; k++) {}
            if (k == 8) continue;
            seen[k] = c, on[k] = told[k] = 0;
        }
        int eff = our_speffect_active(c) == 1;
        if (eff && !on[k] && !told[k]) told[k] = 1, notice(3), logf("invasion notice: an invader has arrived (character %p)", (void *)c);
        else if (!eff && on[k] && told[k] == 1) told[k] = 2, notice(4), logf("invasion notice: the invader was vanquished");
        on[k] = eff;
    }
}
static void team_log(const char *when) {
    char *chr = local_player();
    if (!chr) {
        logf("team test: %s: no local player", when);
        return;
    }
    unsigned char team = 0;
    ((unsigned char *(*)(void *, unsigned char *))(g_base + RVA_CHR_TEAM))(chr, &team);
    int active = our_speffect_active(chr);
    logf("team test: %s: base team %u, effective team %u, our SpEffect %d %s (+%lums)", when,
         (unsigned char)chr[CHR_BASE_TEAM], team, TEAM_TEST_SPEFFECT,
         active < 0 ? "MISSING from regulation.bin" : active ? "active" : "not active", GetTickCount() - g_team_tick);
}
// Where the player is (decision 14). Map block: ChrIns+0x38, the id of the map tile the character
// is in (the first solo test showed it change as the player crossed areas: 3c2a2520 -> 3c0a0922 ->
// 26000101 -> 3c2a2420; ELDEN RING keeps block_id at the same place, 8 bytes earlier). Position:
// the game's own getter 0x3d0fd0 (ChrIns*, vec4 *out), which the play-area manager (0x696170) uses
// for the local player: ChrIns+0x60 -> +0x10 -> +0x1b8 -> +0x68 -> 16 bytes at +0x70 (0x39bd50,
// 0x466470: all three read). It writes with movaps, so the buffer must be 16-byte aligned. Whether
// x/y/z are world or block-local is not read; the log compares it with ChrIns+0x80 / +0x90, which
// moved together with a constant offset in the first test.
#define RVA_CHR_POSITION 0x3d0fd0
#define CHR_BLOCK_ID     0x38
static int player_position(float out[3], unsigned *block) {
    char *chr = local_player();
    if (!chr || IsBadReadPtr(chr, 0xa0)) return 0;
    char *ctrl = *(char **)(chr + 0x60), *a = ctrl ? *(char **)(ctrl + 0x10) : NULL;
    char *b = a && !IsBadReadPtr(a + 0x1b8, 8) ? *(char **)(a + 0x1b8) : NULL;
    if (!b || IsBadReadPtr(b + 0x68, 8) || IsBadReadPtr(*(char **)(b + 0x68), 0x80)) return 0;
    _Alignas(16) float v[4];
    ((void *(*)(void *, void *))(g_base + RVA_CHR_POSITION))(chr, v);
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    *block = *(unsigned *)(chr + CHR_BLOCK_ID);
    return 1;
}
static void log_position(const char *who) {
    float p[3];
    unsigned block;
    char *chr = local_player();
    if (!player_position(p, &block)) {
        logf("%s: position: no local player", who);
        return;
    }
    float *c80 = (float *)(chr + 0x80), *c90 = (float *)(chr + 0x90);
    logf("%s: position: block %08x (+3c %08x +40 %08x +44 %08x) pos %.1f %.1f %.1f | chr+80 %.1f %.1f %.1f | chr+90 %.1f %.1f %.1f",
         who, block, *(unsigned *)(chr + 0x3c), *(unsigned *)(chr + 0x40), *(unsigned *)(chr + 0x44), p[0], p[1], p[2],
         c80[0], c80[1], c80[2], c90[0], c90[1], c90[2]);
}

// Enter test (joiner, with join-test and enter-test). Once a client, the session update moves the
// client role (+0x10) from 1 (waiting for every member's answer 0) to 2 and starts the world sync
// CSInitSyncStep (object at manager +0x1b8, current step at +0x40, read by its vt+0x50 0xdccbe0;
// steps: 0 Wait, 1 Host, 2 StartSessionJoin, 3 WaitPlayerNo, 4 EndSessionJoin, 5 StartLobbyExit,
// 6 HostInitInfo, 7 ProgressInfo, 8 EventState, 9 MapOwnerInitSync, 10 EndLobbyExit,
// 11 StartIngameEnter, 12 DeathBoxSelection, 13 ShareEventSelection, 14 MapInitState,
// 15 WeatherInitSync, 16 IngameTimeInitSync, 17 WaitNetworkChrIns, 18 BulletInitSync,
// 19 MapItemInitSync, 20 PreEndRejoinMapEnter, 21 EndIngameEnter, 22 StartIngame,
// 23 NonActiveBlockInfo, 24 EndIngame, 25 Finish). Nothing is forced: this only watches, logs every
// change of role, sync step, controller step, controller +0x288, the game manager's next map
// (+0xadc), lobby state and the player's map block, and leaves after ENTER_WATCHDOG_MS without a
// sync step past StartIngameEnter, or whenever the client is no longer one. Once the sync is past
// StartIngame and 10 s have gone by, our team SpEffect (90071610, 180 s) is put on the player.
#define SM_INIT_SYNC       0x1b8
#define INIT_SYNC_STEP     0x40
#define QM_CTRL_ENTER      0x288
// Step 10 (EndLobbyExit, 0xdcd8e0) waits for the sync object's phase byte (+0xd8) to be 3. Its only
// writer is 0xdd0100, called only through 0xdf28b0 from MoveMapStep::STEP_MsbLoad (0xc36840): the
// joiner's own game must start loading a map. The phase is logged too (1 started, 2 session joined,
// 3 map load started), with what a map move would start from: the game manager's pending block
// (+0xa74), its map exit state (+0xb3c, set to 1 by 0x6af5d0) and the controller's context mode.
static const char *const SYNC_STEP_NAMES[26] = {
    "Wait", "Host", "StartSessionJoin", "WaitPlayerNo", "EndSessionJoin", "StartLobbyExit", "HostInitInfo",
    "ProgressInfo", "EventState", "MapOwnerInitSync", "EndLobbyExit", "StartIngameEnter", "DeathBoxSelection",
    "ShareEventSelection", "MapInitState", "WeatherInitSync", "IngameTimeInitSync", "WaitNetworkChrIns",
    "BulletInitSync", "MapItemInitSync", "PreEndRejoinMapEnter", "EndIngameEnter", "StartIngame",
    "NonActiveBlockInfo", "EndIngame", "Finish"};
static const char *sync_step_name(int i) { return i >= 0 && i < 26 ? SYNC_STEP_NAMES[i] : "?"; }
#define INIT_SYNC_PHASE 0xd8
static int g_invasion_ended;  // the invasion itself ended (not just a search): no "cancelled" notice
#define MOVE_LOAD_WAIT_MS  60000
#define MOVE_SCENE_WAIT_MS 90000
static int g_join_in_world(void) { return g_join == 4 && g_enter_world; }
static int g_invader_in_world(void) { return g_join_in_world() && !g_as_ally; }
static void enter_watch(char *sm) {
    static int last[14] = {-9, -9, -9, -9, -9, -9, -9, -9, -9, -9, -9, -9, -9, -9};
    char *sync = *(char **)(sm + SM_INIT_SYNC), *gm = *(char **)(g_base + RVA_GAME_MAN_PTR), *chr = local_player();
    if (chr && IsBadReadPtr(chr, 0xa0)) chr = NULL;  // the player is rebuilt while a world loads
    if (sync && IsBadReadPtr(sync, 0xe8)) sync = NULL;
    char *ctx = g_qm_ctrl && !IsBadReadPtr(g_qm_ctrl + 0xa8, 8) ? *(char **)(g_qm_ctrl + 0xa8) : NULL;
    if (ctx && IsBadReadPtr(ctx, 0xa0)) ctx = NULL;
    int now[14] = {(int)*(unsigned *)(sm + SM_LOBBY_STATE), *(int *)(sm + 0x10),
                   sync ? *(int *)(sync + INIT_SYNC_STEP) : -1, g_qm_step,
                   g_qm_ctrl ? *(int *)(g_qm_ctrl + QM_CTRL_ENTER) : -1, gm ? *(int *)(gm + 0xadc) : -1,
                   chr ? *(int *)(chr + CHR_BLOCK_ID) : -1, sync ? (unsigned char)sync[INIT_SYNC_PHASE] : -1,
                   gm ? *(int *)(gm + 0xa74) : -1, gm ? *(int *)(gm + 0xb3c) : -1, ctx ? (unsigned char)ctx[0x9d] : -1,
                   ctx ? *(int *)(ctx + 0x98) : -1, g_qm_ctrl ? *(int *)(g_qm_ctrl + 0x40) : -1,
                   g_qm_ctrl ? *(int *)(g_qm_ctrl + 0x44) : -1};
    DWORD t = GetTickCount() - g_enter_tick;
    if (memcmp(now, last, sizeof now)) {
        memcpy(last, now, sizeof now);
        logf("enter test: +%lums lobby_state %d role %d sync step %d %s (phase %d) controller step %d +0x288 %d | "
             "next map %08x pending block %08x map exit %d context mode %d players %d | ctrl +40 %d +44 %d | block %08x",
             t, now[0], now[1], now[2], sync_step_name(now[2]), now[7], now[3], now[4], now[5], now[8], now[9], now[10],
             now[11], now[12], now[13], now[6]);
    }
    // Full world state: the sync steps ProgressInfo, EventState, MapOwnerInitSync (7-9), MapInitState
    // (14), NonActiveBlockInfo (23) and others ask the host only when sync +0xe5 is set (0xdcd260 sets it
    // only for a role-8 rejoin, from gm+0xc96); the host answers every subtype for a member it flags
    // as rejoining (+0xea), which our joiner is. Set before the sync passes step 6.
    if (g_move_test && sync && now[0] == 6 && now[2] >= 0 && now[2] <= 6 && !sync[0xe5]) {
        sync[0xe5] = 1;
        logf("move test: sync +0xe5 set (sync step %d): the client asks the host for the full world state", now[2]);
    }
    // Step 20 PreEndRejoinMapEnter (0xdcf210) with +0xe5 waits for sync phase 4, which only a rejoin's
    // map load sets (HorseWait 0xc347d0 -> 0xdf28d0), then for an answer to subtype 11; run 98a15ff1
    // stalled there for its 30 s and left. Cleared on reaching step 20, so 20 and 23 run as before
    // while 7-9, 14, 18 and 19 have asked for the host's state.
    if (g_move_test && sync && now[2] >= 20 && sync[0xe5]) {
        sync[0xe5] = 0;
        logf("move test: sync +0xe5 cleared at sync step %d %s", now[2], sync_step_name(now[2]));
    }
    if (g_move_test && !g_move && now[0] == 6 && now[2] == 10 && now[3] == QM_STEP_NONE) {
        g_move = 1;
        logf("move test: sync at EndLobbyExit (phase %d), controller idle: forcing now", now[7]);
    }
    if (g_move == 3 && g_searching) {
        static int cleared;
        if (cleared != (int)g_move_load_tick) {
            cleared = (int)g_move_load_tick;
            clear_notices();  // the map load has begun: the "searching" notice goes
            logf("move test: map load began, search notice cleared");
        }
    }
    if (g_move >= 2 && g_qm_ctrl && sm) {
        // What MoveMapStart (0xb593a0) and MoveMap (0xb590a0) wait for, every change:
        //  MoveMapStart: mode != 2, or gm+0xb40; then sync step 0/1/10, and in mode 2 members == ctx+0x98.
        //  MoveMap (client, mode 2): ctrl+0x10b (set by MoveMapStep's HorseWait, 0xc347d0, during a map
        //  load) before it sends ready 0x21; ctrl+0x10a (the host's 0x23 "go", read in 0xb5da90) before
        //  its timer runs out (or it leaves the session); ctrl+0x10c / session +8 before it lets go.
        static int wlast[8] = {-9, -9, -9, -9, -9, -9, -9, -9};
        char *b = *(char **)(sm + SM_PLAYERS_BEGIN), *e = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
        int w[8] = {gm ? (unsigned char)gm[0xb40] : -1, now[10], now[11], b && e >= b ? (int)((e - b) / SM_PLAYER_SIZE) : -1,
                    (unsigned char)g_qm_ctrl[0x10a], (unsigned char)g_qm_ctrl[0x10b], (unsigned char)g_qm_ctrl[0x10c],
                    *(int *)(sm + 8)};
        member_flags_log("move test", sm);
        if (memcmp(w, wlast, sizeof w)) {
            memcpy(wlast, w, sizeof w);
            logf("move test: wait state: online %d mode %d expected %d members %d | ctrl +10a go %d +10b loaded %d +10c %d | "
                 "session +8 %d | sync step %d controller step %d", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], now[2], now[3]);
        }
    }
    if (g_move >= 2 && !g_enter_world) {
        DWORD now_ms = GetTickCount();
        if (now[3] == 24 || now[3] == 25) {
            join_over(now[3] == 24 ? "move test: controller went to FailedStartMultiplay" : "move test: controller went to Unregister");
            return;
        }
        if (g_step_back_to_hub) {
            g_step_back_to_hub = 0;
            join_over("move test: the map load went back to the Roundtable Hold");
            return;
        }
        if (g_move == 2 && now_ms - g_move_tick > MOVE_LOAD_WAIT_MS) {
            join_over("move test: no map load within 60 s of the forced step");
            return;
        }
        if (g_move == 3 && now[2] < 11 && now_ms - g_move_load_tick > MOVE_SCENE_WAIT_MS) {
            join_over("move test: map loading, but the sync did not move past EndLobbyExit within 90 s");
            return;
        }
    }
    if (now[2] >= 22 && !g_enter_world) {
        g_enter_world = t;
        logf("enter test: world sync at step %d: in the host's world?", now[2]);
        expedition_log("enter test");
        log_position("enter test");
        log_members(sm, "enter test");
    }
    // The invader's team for the whole invasion: the effect (no time limit: effectEndurance -1 in our
    // regulation row) is put on 10 s after the world sync reached StartIngame; its team is logged a
    // second later (the same frame still reads the old team). Death removes it (dontDeleteOnDead 0),
    // so once it is gone the invader died: decision 3, the invader leaves and returns to its hub.
    static DWORD applied, check_at;
    DWORD nowt = GetTickCount();
    // Run 13: the effect came 10 s after the world sync (a margin from the first tests, when the player
    // was still being rebuilt around the sync), and the host saw the invader uncoloured for those
    // seconds. Now at the sync itself (step 22 or later: the character of the host's world exists);
    // if the effect is gone again within the first 5 s (a late rebuild) it is put on once more, and
    // only after that does its loss count as the invader's death.
    static int reapplied;
    if (!g_enter_world) reapplied = 0;
    if (g_as_ally && g_enter_world && !g_enter_effect) {
        g_enter_effect = -1;  // allies: no team effect (base team 2, Friend to the host's 2); death is the game's own
        team_log("enter test, ally: no team effect");
    }
    // The ally's gold look: our row 90071611 (launcher: vfxId1 54220 = the game's gold phantom, PhantomParam 62;
    // no time limit, like the invader's red; no team change), removed in join_over. Run 62 had it at 30 s: it
    // lapsed every 30 s and came back a frame later (a blink), as re-adding a running effect does not restart
    // its time (seen with the 180 s team row, run 0a6833c2). Like the invader's red, it is put on
    // the ally's own character only; the others see it through the game's effect sync (the host saw our team
    // effect on the invader's character, runs 13 ff.). Not our team effect: no rule keys on it.
    // Run of 818477c: the gold came tens of seconds after landing (it waited for the world sync's StartIngame, step
    // 22) and dropped out now and then (30 s copies added every 20 s: an add while the effect is still on does not
    // seem to restart it, so the copy ended at 30 s and the next add came at 40 s; not read in code). Now: from the
    // first frame the ally's character stands in a field map of the host's world (map load under way, block area
    // 0x3c; the start flight included), every frame: if the effect is not on, it is put on at once. Each change of
    // the state and each add is logged with the effect entry's first floats (+0x8..+0x18: is one the time left?).
    if (g_as_ally && g_join == 4 && chr && g_move >= 3 && (*(unsigned *)(chr + CHR_BLOCK_ID) >> 24) == 0x3c) {
        static int was_on = -1, adds;
        int on_now = speffect_on(chr, ALLY_LOOK_SPEFFECT);
        if (!on_now) {
            ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, ALLY_LOOK_SPEFFECT, 0);
            if (adds++ < 40) logf("ally: gold look (SpEffect %d) %s (add %d, block %08x)", ALLY_LOOK_SPEFFECT,
                                  was_on < 0 ? "put on at the first frame in the field" : "was off: put on again", adds, *(unsigned *)(chr + CHR_BLOCK_ID));
        }
        static DWORD next;
        char *e = speffect_entry(chr, ALLY_LOOK_SPEFFECT);
        if (e && (int)(GetTickCount() - next) >= 0 && adds < 60) {
            next = GetTickCount() + 5000;
            logf("ally: gold look entry %p: +8 %.2f +c %.2f +10 %.2f +14 %.2f +18 %.2f", (void *)e, *(float *)(e + 8), *(float *)(e + 0xc),
                 *(float *)(e + 0x10), *(float *)(e + 0x14), *(float *)(e + 0x18));
        }
        was_on = on_now;
    }
    if (g_enter_effect > 0 && chr && !reapplied && nowt - applied < 5000 && nowt - applied > 300 && our_speffect_active(chr) == 0) {
        reapplied = 1;
        ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, TEAM_TEST_SPEFFECT, 0);
        applied = nowt;
        team_log("enter test, team effect lost right after the sync: put on again");
    }
    // The host's summons: HP flask charges = max(ALLY_FLASK_MIN, host's - ALLY_FLASK_CUT), once, as soon as the host's
    // lobby carries its final count (nrpvp_hp); set with the game's own 0x20d9d0 and filled with 0x60ad60 as a church
    // does, so later churches still add one each (0x5fdad0 raises +0xff by one). The host's slot +0xff is logged too:
    // whether the slot carries the count is not established (its level at +0x68 is).
    static DWORD flask_done_for;
    if (!g_enter_world) g_join_cut = -1;
    if (g_as_ally && g_enter_world && chr && g_join_cut >= 0 && flask_done_for != g_enter_tick) {
        const char *hp = g_candidate ? g_steam.get_data(g_steam.mm, g_candidate, LOBBY_HP_KEY) : NULL;
        char *ps = *(char **)(g_base + 0x3c078d0), *pd = ps ? *(char **)(ps + 8) : NULL, *slots = ps ? *(char **)(ps + 0x10) : NULL;
        if (hp && *hp && pd && !IsBadReadPtr(pd, 0x110)) {
            int host = atoi(hp), n = g_join_cut ? host - ALLY_FLASK_CUT : host, before = (unsigned char)pd[0xff];
            if (n < ALLY_FLASK_MIN) n = ALLY_FLASK_MIN;
            if (n > 14) n = 14;
            flask_done_for = g_enter_tick;
            ((void (*)(char *, unsigned char, unsigned char))(g_base + 0x20d9d0))(pd, (unsigned char)n, (unsigned char)pd[0x100]);
            ((void (*)(void))(g_base + 0x60ad60))();
            int s0 = slots && !IsBadReadPtr(slots, 2 * 0x8a0) ? (unsigned char)slots[0xff] : -1, s1 = slots && !IsBadReadPtr(slots, 2 * 0x8a0) ? (unsigned char)slots[0x8a0 + 0xff] : -1;
            logf("ally flasks: host has %d HP charges (its lobby); ours %d -> %d (%s), filled | slots +0xff: %d %d", host, before,
                 (unsigned char)pd[0xff], g_join_cut ? "late summon: host's - 2, at least 3" : "early summon: the host's, at least 3", s0, s1);
        }
    }
    // decision 24: once the world sync is through and the host's slot holds its level, take that level
    if (g_enter_world && chr && !g_level_set) {
        char *ps = *(char **)(g_base + 0x3c078d0);
        char *slots = ps ? *(char **)(ps + 0x10) : NULL, *pd = ps ? *(char **)(ps + 8) : NULL;
        int host_no = g_host_player_no >= 0 ? g_host_player_no : 1;
        for (int i = 0; slots && pd && !IsBadReadPtr(slots, g_cap_remote * 0x8a0) && !IsBadReadPtr(pd, 0x110) && i < g_cap_remote; i++) {
            char *sl = slots + i * 0x8a0;
            int no = *(int *)(sl + 8), lv = *(int *)(sl + 0x68);
            if (no != host_no || lv < 1 || lv > 15) continue;
            g_own_level = *(int *)(pd + 0x68), g_own_top_level = *(int *)(pd + 0x10c);
            // the user's formula: an ally gets the host's level less JOIN_LEVEL_CUT_PERCENT, never below 1 (host 1 -> 1,
            // 2 -> 1, 3 -> 1, 5 -> 3, 10 -> 6, 15 -> 9); an invader the host's level (decision 24)
            // the user's rule: a summon of a host below JOIN_CUT_FROM_LEVEL is not late: the host's level and flasks
            int cut = (g_as_ally ? JOIN_LEVEL_CUT_ALLIES : JOIN_LEVEL_CUT_INVADERS) && lv >= JOIN_CUT_FROM_LEVEL;
            int want = cut ? lv * (100 - JOIN_LEVEL_CUT_PERCENT) / 100 : lv;
            if (want < 1) want = 1;
            g_join_cut = cut;
            logf("level: host (player %d) is level %d; joiner (%s) level %d (highest %d) -> %d (%s)", no, lv, g_as_ally ? "ally" : "invader",
                 g_own_level, g_own_top_level, want,
                 cut ? "late summon (host at level 4 or more): host's level less the cut, at least 1"
                     : g_as_ally ? "early summon (host below level 4): the host's level" : "invader: the host's level");
            if (level_apply(want, "arrival: level from the host's") >= 0) g_level_set = 1;
            break;
        }
    }
    if (g_enter_world && chr && !g_enter_effect) {
        team_log("enter test, before the team effect");
        ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, TEAM_TEST_SPEFFECT, 0);
        g_enter_effect = 1;
        notice(6), logf("invasion notice: you have invaded (shown to the invader)");
        applied = nowt;
        check_at = nowt + 1000;
    }
    if (check_at && (int)(nowt - check_at) >= 0 && chr) {
        check_at = 0;
        team_log("enter test, 1 s after the team effect");
    }
    if (1 || 1) other_down_watch("invader");
    if (1) name_watch(sm, "invader", g_enter_world != 0), slot_hp_watch("invader"), other_downed_watch("invader");
    // finish-test guard: the stand-up was refused and the timer set to 0, but the character is still
    // down 10 s later (the behaviour did not take its death): the invader counts as dead.
    if (g_enter_effect > 0 && chr && chr == g_finish_chr && g_finish_invader && g_finish_tick && chr[0x710] == 1 &&
        nowt - g_finish_tick > FINISH_GUARD_MS) {
        logf("finish test: still down %u s after the refused stand-up: the invader counts as dead", FINISH_GUARD_MS / 1000);
        g_finish_tick = 0, g_finish_chr = NULL;
        g_invasion_ended = 1;
        join_over("invader died (finished while down)");
        return;
    }
    if (g_enter_effect > 0 && chr && nowt - applied > 5000 && our_speffect_active(chr) == 0) {
        team_log("enter test, team effect gone");
        g_invasion_ended = 1;
        join_over("invader died (team effect removed)");
        return;
    }
}
// --- The clocks (logged) -------------------------------------------------------------------------------
//
// The expedition's clock: singleton 0x3c125d8 (created in 0x62ed40, set up by 0x65a9c0 / 0x65af70).
// +0x08 time of day, packed (hour bits 34-38, minute 39-44, second 45-50, as 0x602f80 unpacks it);
// +0x34 base rate, from GameSystemCommonParam row 0 +0x1bc (72.9), or the debug property
// Game.IngameTime.TimeScale when it is above 0 (0xf884b0, read once, at set-up); +0x38 the rate
// the update 0x65c1d0 advances the time with; +0x4b "running". A client's rate is pulled toward
// the host's time (0x65c750: every 15 s from the host, a jump when 120 s apart), so the host's
// rate is the one that counts. Every 30 s it logs the time of day and the rates, so the log shows how fast the day goes.
// The solo test of 12 Nov showed the circles do NOT run on this clock (rain at its 07:00).
//
// The circles run on the play-area manager's own clock (singleton 0x3c13158): +0x100 packed time
// (same packing; +0xf8 its guard word), advanced in 0x698430 by dt * +0x118 * 1000 ms, unless the
// debug property Game.PlayAreaTime.DebugIsDisableProgress is set. +0x118 the rate, +0x11c the base,
// both set in 0x68b130 from param table 0x92 row 0 +0x1b4 (fallback 96), or the debug property
// Game.PlayAreaTime.TimeScale when above 0. The circle update 0x696170 places the circle by this
// time against each phase's start/end hour:minute (+0x1a4.. per 0x20-byte phase), and the event
// script's "time of day" condition 0x603070 reads it too. A host (lobby_state 3) sends it every
// 15 s (0x6987b0); a client (6) nudges its +0x118 by the difference / 15, or jumps when 120 s or
// more apart and puts +0x118 back to its own +0x11c. So the host's rate counts, and a client whose
// base is not also slowed keeps being pulled back.
#define RVA_CLOCK_PTR   0x3c125d8
#define CLOCK_TIME      0x08
#define CLOCK_BASE_RATE 0x34
#define CLOCK_RATE      0x38
#define CLOCK_RUNNING   0x4b
#define RVA_PLAY_AREA_PTR 0x3c13158
#define PA_TIME           0x100
#define PA_RATE           0x118
#define PA_BASE_RATE      0x11c
static DWORD g_clock_tick, g_clock_next;
static void clock_log(const char *when) {
    char *clock = *(char **)(g_base + RVA_CLOCK_PTR);
    if (!clock) {
        logf("clock: %s: no clock", when);
        return;
    }
    unsigned long long v = *(unsigned long long *)(clock + CLOCK_TIME);
    logf("clock: %s: time of day %02u:%02u:%02u, base rate %.3f, rate %.3f, running %u (+%lus)", when,
         (unsigned)(v >> 34) & 0x1f, (unsigned)(v >> 39) & 0x3f, (unsigned)(v >> 45) & 0x3f,
         *(float *)(clock + CLOCK_BASE_RATE), *(float *)(clock + CLOCK_RATE), (unsigned char)clock[CLOCK_RUNNING],
         (GetTickCount() - g_clock_tick) / 1000);
    char *pa = *(char **)(g_base + RVA_PLAY_AREA_PTR);
    if (!pa) return;
    v = *(unsigned long long *)(pa + PA_TIME);
    logf("clock: %s: circle clock %02u:%02u:%02u, base rate %.3f, rate %.3f, circle radius %.1f", when, (unsigned)(v >> 34) & 0x1f,
         (unsigned)(v >> 39) & 0x3f, (unsigned)(v >> 45) & 0x3f, *(float *)(pa + PA_BASE_RATE), *(float *)(pa + PA_RATE),
         *(float *)(pa + 0x260));
}
static void clock_frame(int first) {  // the clocks in the log: at the expedition start and every 30 s
    if (first) {
        g_clock_tick = GetTickCount();
        clock_log("expedition start");
        g_clock_next = 30000;
        return;
    }
    if (GetTickCount() - g_clock_tick >= g_clock_next) {
        clock_log("every 30 s");
        g_clock_next += 30000;
    }
}

// boss-flow: 4 the day flow (day_flow.h, the default), 0 the game's own clock (for comparison tests). Only the circle
// clock and the time of day are driven (bf4_frame); every phase flag stays the game's.
#define RVA_PA_SET_TIME 0x68ff30  // (manager, hour, minute, second): adds the differences to +0xf8 / +0x100
static int bf_flag(unsigned id) {
    char *m = flag_man();
    return m && flag_state_in(m, id) == 1;
}
// Night boss active (9019 on: common 1108 holds it from 23:59:00 until 9052, ...): every other living boss is
// disabled on this machine, as the game's own 23:00 field-boss events do (2004[5] "Change Character Enable State"
// = 0x608c60(chr, 0): the enable bit at [chr +0x18] +10 bit 0, the mount too; HP untouched), each machine by itself
// (9019 is driven by the clock, which every machine has). Checked every 500 ms, so a boss loaded later in the window
// is hidden too; when 9019 goes off the ones disabled here are enabled again. A boss = a character of
// boss-deaths.txt (entity id -> 0x5190c0, the world character manager's lookup), alive (HP +0x140 of [chr +0x1b8] > 0)
// and enabled. Not done: the map markers (left as they are).
static unsigned g_hidden[HIDE_MAX];
static int g_hidden_n, g_hide_on;
static char *boss_chr(unsigned ent) {
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    int e = (int)ent;
    char *c = wcm ? ((char *(*)(void *, int *))(g_base + 0x5190c0))(wcm, &e) : NULL;
    return c && !IsBadReadPtr(c, 0x1c0) && *(char **)(c + 0x18) && !IsBadReadPtr(*(char **)(c + 0x18), 0x10) ? c : NULL;
}
static int chr_enabled(char *c) { return *(unsigned char *)(*(char **)(c + 0x18) + 10) & 1; }
static void night_hide_frame(int in_world) {
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 500;
    int on = in_world && bf_flag(9019);
    if (on) {
        int now = 0;
        for (int i = 0; i < g_hide_n; i++) {
            char *c = boss_chr(g_hide_ent[i]);
            if (!c || !chr_enabled(c)) continue;
            char **pd = *(char ***)(c + 0x1b8);
            if (!pd || IsBadReadPtr(pd, 8) || !*pd || IsBadReadPtr(*pd, 0x148) || *(int *)(*pd + 0x140) <= 0) continue;
            int known = 0;
            for (int k = 0; k < g_hidden_n; k++) known |= g_hidden[k] == g_hide_ent[i];
            if (!known && g_hidden_n < HIDE_MAX) g_hidden[g_hidden_n++] = g_hide_ent[i];
            ((char (*)(char *, char))(g_base + 0x608c60))(c, 0);
            now++;
        }
        if (!g_hide_on || now) logf("night boss: 9019 on, %d boss(es) hidden now (%d in all this night)", now, g_hidden_n);
        g_hide_on = 1;
    } else if (g_hide_on) {
        int back = 0;
        for (int k = 0; k < g_hidden_n; k++) {
            char *c = in_world ? boss_chr(g_hidden[k]) : NULL;
            if (c && !chr_enabled(c)) ((char (*)(char *, char))(g_base + 0x608c60))(c, 1), back++;
        }
        logf("night boss: 9019 off%s, %d of %d hidden boss(es) enabled again (the rest not loaded)", in_world ? "" : " (out of the world)", back,
             g_hidden_n);
        g_hide_on = 0, g_hidden_n = 0;
    }
}
static void bf4_frame(int first);
static void boss_flow_frame(int first) {
    if (g_settings[S_BOSS_FLOW] == 4) bf4_frame(first);
}

// Host: publish where the host is. Checked every LOBBY_POS_PERIOD_MS; written when the block changed,
// otherwise only when the host moved at least LOBBY_POS_MIN_MOVE and LOBBY_POS_MIN_GAP_MS passed since
// the last write (or LOBBY_POS_REFRESH_MS passed), and never while someone is joining. Kept sparse
// since the 12 Nov run, where the host's Steam client lost its lobby (cause not known; these writes
// were the only new Steam traffic).
#define LOBBY_POS_PERIOD_MS  10000
#define LOBBY_POS_MIN_GAP_MS 30000
#define LOBBY_POS_REFRESH_MS 60000
#define LOBBY_POS_MIN_MOVE   5.0f
static void lobby_position(void) {
    static DWORD next, last_write;
    static unsigned last_block;
    static float last[3];
    static u64 for_lobby;
    DWORD now = GetTickCount();
    if (for_lobby != g_lobby) for_lobby = g_lobby, next = 0, last_write = 0, last_block = 0;
    if (!g_lobby || !steam_ready() || now < next || g_someone_joining) return;
    next = now + LOBBY_POS_PERIOD_MS;
    float p[3];
    unsigned block;
    if (!player_position(p, &block)) return;
    float dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
    if (block == last_block && last_write &&
        (now - last_write < LOBBY_POS_MIN_GAP_MS ||
         (dx * dx + dy * dy + dz * dz < LOBBY_POS_MIN_MOVE * LOBBY_POS_MIN_MOVE && now - last_write < LOBBY_POS_REFRESH_MS)))
        return;
    char text[64];
    snprintf(text, sizeof text, "%08x", block);
    lobby_set(LOBBY_BLOCK_KEY, text);
    snprintf(text, sizeof text, "%.1f,%.1f,%.1f", p[0], p[1], p[2]);
    lobby_set(LOBBY_POS_KEY, text);
    last_block = block, last[0] = p[0], last[1] = p[1], last[2] = p[2], last_write = now;
}

// Host session over the expedition (called for every step of CSQuickMatchingCtrl).
// Start: the first frame of InGame (23), after MoveMapStart has made its own solo-expedition leave
// and after every loading screen of the start. End: the first frame of any other step. InGame
// (0xb58f60) never changes the step itself; the way out found in readable code is 0xb52ef0, which
// sets Unregister (25) and is what the game's return to the title screen calls (0xc4a230 ->
// 0xb67240 -> 0xb52ef0). Logs show InGame -> Unregister -> None for a finished or abandoned run and
// for quitting the game from an expedition.
// While InGame runs with lobby_state 3 it reads session packets of type 0x21 (0xddbbc0) and answers
// with 0x23 (0xddf930); a session of one never receives any.
#define QM_STEP_INGAME 23
#define NET_READY_WAIT_MS 15000  // only for the log: say once if the network is slow to come up
static DWORD g_expedition_tick, g_expedition_start_tick;
static void host_test_create(void);
static int host_test_leave(void);

#define QM_STEP_MOVE_MAP_START 21
#define RVA_SET_ONLINE_MODE   0x6b0530  // gm+0xb40 = arg
// --- Party (a password typed in the menu, or the test setting "party" = code) -----------------------
// The game's own squad start (read, 2026-11-15): the menu's 2- / 3-player choice asks 0x6aeae0 = gm+0xb40;
// CSQuickMatchingCtrl then pairs players through FromSoftware's matching jobs (steps 1-11), the host makes the
// session in step 6 and both reach MoveMapWait (13); mode 2 then needs session members == ctx +0x98 at
// MoveMapWait and MoveMapStart, and MoveMap runs ready (0x21) / go (0x23).
// Run 55 / 46 (DLL c7c62ab4, the party gathered in the Roundtable Hold): the guest joined in 4 s, its sync went
// Wait -> EndSessionJoin -> HostInitInfo and asked the host for subtype 0; the host's sync step Host (0xdcdc60)
// answers subtype 0 (0xdf23d0) only while 0x68b5e0() is true: the host's current map is a field map (area 0x3c,
// play-area phase not -1 / 3) or one of 0x0c / 0x13 / 0x12 / 0x10 / 0x100a with 0x143c12c02 set, and the
// play-area manager's +0x140 / +0x144 are 0. In the Roundtable Hold it is false, so the guest's HostInitInfo
// waits (up to 30 s, then the client leaves, 0xdce620). The session also stays in role 4 there (role 7 comes
// only from a map load inside the session, 0xdf28e0 in MoveMapStep). So a hub session cannot take a member
// into a world: a member can only enter once the host is in the expedition's field map.
// So: the first player (no lobby with this code) hosts: its session is made in the Roundtable Hold so that the
// others find it at once (nrpvp_party = code, nrpvp_go = 0), and its expedition starts right away on the solo
// path (SingleStart, mode 1); the session goes through the map load (MoveMapStart leaves it only while its sync
// is busy; LeaveSessionWait 0x22c57e1 only waits for a leave already under way). In the field (InGame) the
// host sets nrpvp_go = 1. A player who finds the lobby waits in the Roundtable Hold (a notice) until go is 1,
// then joins it as an ally: the join of the ally build into a running world (proven, runs 50-54).
// The code: the menu's password when one is typed (request +0x48 == 1, the wide string at *(request +8), else
// *(request +0x18), as 0xb67cc0 reads it; only its FNV-1a hash goes into the lobby, "k%08x"), else the test
// setting's number. Not established: what the matching server would have written into the context (logged).
static DWORD g_party_tick;
static DWORD g_party_confirm;  // host: 0 not marked; tick of the marking (odd); 3 confirm search sent; 2 confirmed
#define PARTY_CONFIRM_MS 3000
static int g_party_marked_for;
static char *party_ctx(char *ctrl) {
    char *ctx = ctrl ? *(char **)(ctrl + QM_CTRL_CONTEXT) : NULL;
    return ctx && !IsBadReadPtr(ctx, 0x190) && *(char **)ctx == g_base + RVA_QM_CONTEXT_VTABLE ? ctx : NULL;
}
static void party_ctx_log(char *ctrl, const char *why) {
    char *c = party_ctx(ctrl);
    if (!c) return;
    logf("party: %s: context mode +9d %u players +98 %u +9c %u +c0 %u +128 %d +12c %u +130 %d +134 %d +138 %d +13c %u +140 %d "
         "+145 %u +17c..181 %u %u %u %u %u %u +188 %08x +18c %08x", why, (unsigned char)c[0x9d], *(unsigned *)(c + 0x98),
         (unsigned char)c[0x9c], (unsigned char)c[0xc0], *(int *)(c + 0x128), (unsigned char)c[0x12c], *(int *)(c + 0x130),
         *(int *)(c + 0x134), *(int *)(c + 0x138), (unsigned char)c[0x13c], *(int *)(c + 0x140), (unsigned char)c[0x145],
         (unsigned char)c[0x17c], (unsigned char)c[0x17d], (unsigned char)c[0x17e], (unsigned char)c[0x17f], (unsigned char)c[0x180],
         (unsigned char)c[0x181], *(unsigned *)(c + 0x188), *(unsigned *)(c + 0x18c));
}
static void party_end(const char *why) {
    logf("party: over (%s)", why);
    if (g_party_host && !g_in_expedition) {
        if (g_lobby) g_party_old_lobby = g_lobby;
        if (host_test_leave()) g_stop_pending = GetTickCount();
        else if (g_network_on) set_network(0);
        g_lobby = 0, g_host_test = 0;
    }
    g_party = PARTY_OFF, g_party_host = g_party_guest = 0;
}
static void party_host_over(void) {
    logf("party: host's expedition over");
    if (g_lobby) g_party_old_lobby = g_lobby;
    g_party = PARTY_OFF, g_party_host = 0;
}
// The menu's password (UTF-16 at *(request +8), else *(request +0x18), as 0xb67cc0 reads it); NULL when none is
// typed. The game also asks for request +0x48 == 1 ("No. of Password Players": two); our team uses it either way.
static unsigned short *party_password(char *request) {
    unsigned short *w = *(unsigned short **)(request + 8);
    if (!w) w = *(unsigned short **)(request + 0x18);
    return w && !IsBadReadPtr(w, 2) && *w ? w : NULL;
}
// Matchmaking pools (the user's design): key = FNV-1a of a fixed base ("oisis"), the game version, our lobby
// protocol, the password (none = the common pool of every mod player) and the test setting "party" when it is
// above 0 (an extra salt, only to keep test runs apart). Only the hash goes into the lobby.
static unsigned party_key_hash(char *request) {
    unsigned h = 2166136261u;
    const char *base[2] = {"oisis", LOBBY_GAME};  // no protocol / version: friends on another version share the code
    for (int k = 0; k < 2; k++)
        for (const char *c = base[k]; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    h = (h ^ '|') * 16777619u;
    unsigned short *w = party_password(request);
    for (int i = 0; w && i < 64 && !IsBadReadPtr(w + i, 2) && w[i]; i++) h = (h ^ (w[i] & 0xff)) * 16777619u, h = (h ^ (w[i] >> 8)) * 16777619u;
    h = (h ^ '|') * 16777619u;
    for (int salt = g_settings[S_PARTY]; salt > 0; salt /= 10) h = (h ^ (unsigned)('0' + salt % 10)) * 16777619u;
    return h;
}
// Request seen (fill context): the menu's squad choice with a password or a party code.
static void party_request(char *self, char *request) {
    int pw = party_password(request) != NULL;
    char code[16];
    snprintf(code, sizeof code, "p%08x", party_key_hash(request));
    if (g_party) {  // already hosting or waiting: keep it (a reopened menu must not restart the party)
        logf("party: start request again while the party is under way (phase %d): kept as it is", g_party);
        return;
    }
    char *ctx = *(char **)(self + 8);
    snprintf(g_party_code, sizeof g_party_code, "%s", code);
    g_party = PARTY_PENDING, g_party_tick = GetTickCount(), g_party_marked_for = -1, g_party_confirm = 0;
    g_ver_notice_armed = 1;
    g_as_ally = 1;
    if (!g_network_on) set_network(1);
    logf("party: start request for a squad of %u, pool: %s%s: network %s; looking for a host in it",
         ctx && *(char **)ctx == g_base + RVA_QM_CONTEXT_VTABLE ? *(unsigned *)(ctx + QM_CTX_PLAYERS) : 0,
         pw ? "the menu's password" : "the common pool (no password)", g_settings[S_PARTY] > 0 ? " + test salt" : "",
         g_network_on ? "up" : "NOT up");
}
static int party_lobby_go(u64 lobby) {
    const char *go = lobby ? g_steam.get_data(g_steam.mm, lobby, LOBBY_GO_KEY) : NULL;
    return go && !strcmp(go, "1");
}
// Idle step (None) while the party gathers.
static void party_frame(char *ctrl) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    unsigned state = sm ? *(unsigned *)(sm + SM_LOBBY_STATE) : 0;
    if (g_party == PARTY_PENDING || g_party == PARTY_GUEST_WAIT) {
        void **impl = sm ? session_impl(sm) : NULL;
        if (!g_network_on || !((unsigned char (*)(void))(g_base + RVA_NET_READY))() || !impl || !*(unsigned *)((char *)impl + 0x10) ||
            (state != 0 && state != 2 && state != 5)) {  // also: our own last session still leaving
            if (g_party == PARTY_PENDING && GetTickCount() - g_party_tick > 30000) party_end("network not ready within 30 s");
            return;
        }
        if (g_party == PARTY_GUEST_WAIT && notices_idle()) notice(2);
        if (!lobby_search_frame("party search")) return;
        u64 c = g_candidate == g_party_old_lobby ? 0 : g_candidate;
        if (c && party_lobby_go(c)) {  // the host is in its expedition's field map: join it as an ally
            join_test_start();
            g_candidate = c, g_as_ally = 1, g_searching = 1;
            g_party = PARTY_GUEST, g_party_guest = 1;
            logf("party: the host (lobby %llu) is in the field: joining as its squad's ally", c);
        } else if (c) {
            if (g_party != PARTY_GUEST_WAIT) logf("party: host found (lobby %llu), still on its way into the expedition: waiting", c);
            g_party = PARTY_GUEST_WAIT;
            if (notices_idle()) notice(2);
        } else if (g_party == PARTY_GUEST_WAIT) {
            party_end("the host's lobby is gone");
        } else {
            g_party = PARTY_HOST_CREATE, g_party_host = 1, g_host_test = 1;
            logf("party: no host with this code: hosting (session in the Roundtable Hold, so the others find it)");
        }
        return;
    }
    if (g_party == PARTY_HOST_CREATE) {
        host_test_create();
        if (g_host_test == 3) { party_end("a session already exists"); return; }
        if (g_host_test != 2 || state != 3) return;
        lobby_mark();
        if (!g_lobby) return;
        if (!g_party_confirm) {
            lobby_set(LOBBY_PARTY_KEY, g_party_code), lobby_set(LOBBY_GO_KEY, "0");
            g_party_confirm = GetTickCount() | 1;
            logf("party: hosting in lobby %llu; one more search in %u s for another host who started at the same moment",
                 g_lobby, PARTY_CONFIRM_MS / 1000);
            return;
        }
        if (GetTickCount() - g_party_confirm < PARTY_CONFIRM_MS) return;
        if (g_party_confirm != 2) {  // the confirming search, sent at once
            if (g_party_confirm != 3) lobby_search_reset(), g_party_confirm = 3;
            if (!lobby_search_frame("party confirm")) return;
            g_party_confirm = 2;
            if (g_party_lowest && g_party_lowest < g_lobby) {  // another host at the same moment, with the lower id: join it
                logf("party: another host started at the same moment (lobby %llu, lower id than ours %llu): ours is given up",
                     g_party_lowest, g_lobby);
                g_party_old_lobby = g_lobby;
                host_test_leave();
                g_lobby = 0, g_host_test = 0, g_party_host = 0, g_party_confirm = 0;
                g_party = PARTY_GUEST_WAIT, g_party_tick = GetTickCount();
                lobby_search_reset();
                return;
            }
            logf("party: no other host with a lower lobby id: ours stays");
        }
        char *ctx = party_ctx(ctrl);
        if (!ctx) { party_end("context not as expected"); return; }
        ((void (*)(void *, unsigned char))(*(void ***)ctx)[0x48 / 8])(ctx, 1);  // the solo path
        party_ctx_log(ctrl, "host, before SingleStart (mode 1: the solo path; the others join in the field)");
        g_party = PARTY_HOST_STARTED, g_party_tick = GetTickCount();
        ((void (*)(void *, int))(*(void ***)ctrl)[QM_CTRL_VT_SET_STEP])(ctrl, 12);
        logf("party: host starts its expedition (SingleStart); lobby %llu marked with the party code", g_lobby);
    }
}
// Host, every step after its start: in the field the lobby says go (again whenever the lobby was re-marked).
static void party_host_step(char *ctrl, int step) {
    (void)ctrl;
    if (step == 24 || step == 25 || (step == QM_STEP_NONE && GetTickCount() - g_party_tick > 3000)) {
        party_end(step == QM_STEP_NONE ? "host back to None" : "host's start failed");
        return;
    }
    if (step == QM_STEP_INGAME && g_lobby && g_party_marked_for != (int)(g_lobby & 0x7fffffff)) {
        g_party_marked_for = (int)(g_lobby & 0x7fffffff);
        lobby_set(LOBBY_PARTY_KEY, g_party_code), lobby_set(LOBBY_GO_KEY, "1");
        logf("party: host in the field: go published (lobby %llu)", g_lobby);
    }
}

static void host_expedition(int step) {
    if (step == QM_STEP_INGAME) {
        int first_frame = !g_in_expedition;
        if (!g_in_expedition) {
            g_team_tick = GetTickCount();
            clock_frame(1);
            g_in_expedition = 1;
            g_expedition_tick = GetTickCount();
            g_expedition_start_tick = GetTickCount() | 1;
            g_self_check = 0;
            lobby_search_reset();
            host_test_start();
            if (g_party_host) {
                char *sm0 = *(char **)(g_base + RVA_SESSION_MAN_PTR);
                int kept = sm0 && *(unsigned *)(sm0 + SM_LOBBY_STATE) == 3;
                if (kept) g_host_test = 2;
                logf("party: host's session from the Roundtable Hold %s (role %d)", kept ? "went on into the expedition" : "is gone: a new one is made",
                     sm0 ? *(int *)(sm0 + 0x10) : -1);
            }
            logf("host: expedition reached its world%s", g_host_test ? ", opening a host session" : " (no host session: no-host is on)");
            if (g_host_test) {
                if (g_stop_pending) finish_pending_stop(1);
                if (!g_network_on) set_network(1);
                if (!g_network_on) {
                    logf("host: network not started, no host session this expedition");
                    g_host_test = 3;
                }
            }
        }
        clock_frame(0);
        boss_flow_frame(first_frame);
        flag_log_frame();
        md_frame(g_expedition_start_tick);
        // test-level (test setting, own expedition only): the level set once, 5 s into the expedition,
        // through the same game path as the invader's level (level_apply).
        static DWORD test_level_done;
        if (g_settings[S_TEST_LEVEL] > 0 && test_level_done != g_expedition_start_tick && GetTickCount() - g_expedition_start_tick > 5000)
            test_level_done = g_expedition_start_tick, level_apply(g_settings[S_TEST_LEVEL], "test-level (own expedition)");
        if (g_host_test == 1) {
            host_test_create();
            if (g_host_test == 1 && g_expedition_tick && GetTickCount() - g_expedition_tick > NET_READY_WAIT_MS) {
                logf("host: network still not ready after %lums, still waiting", GetTickCount() - g_expedition_tick);
                g_expedition_tick = 0;
            }
        }
        if (g_host_test == 2) {
            lobby_mark();
            lobby_position();
            {  // the host's HP flask charges for its summons, once decision 26 has set the start value (or no two-region map)
                char *gdm = *(char **)(g_base + 0x3c078d0), *pgd = gdm ? *(char **)(gdm + 8) : NULL;
                static int published = -1;
                static u64 for_lobby;
                if (for_lobby != g_lobby) for_lobby = g_lobby, published = -1;
                if (g_lobby && pgd && (!g_two_active || g_flasks_final_for == g_two_requests) && (unsigned char)pgd[0xff] != published) {
                    char text[8];
                    published = (unsigned char)pgd[0xff];
                    snprintf(text, sizeof text, "%d", published);
                    lobby_set(LOBBY_HP_KEY, text);
                }
            }
            if (g_lobby && !g_self_check && lobby_search_frame("host self-check")) g_self_check = 1;
        }
    } else if (g_in_expedition) {
        g_in_expedition = 0;
        g_ver_notice_armed = 1;  // a new hub visit
        lobby_search_reset();
        logf("host: expedition over (step %s)%s", STEP_NAMES[step], g_host_test == 2 ? ", leaving the session" : "");
        md_end("expedition over");
        if (g_party_host) party_host_over();
        if (host_test_leave()) g_stop_pending = GetTickCount();  // network down once the leave is through
        else if (g_network_on && !g_searching) set_network(0);
        g_lobby = 0;
    }
}


#include "overlay.h"
#include "capacity.h"
#include "day_flow.h"
// Counts for the overlay (counts only: no names, no ids): our lobbies of our version, by the phase the host publishes.
// In the hub (the local player's map block 0x0a......, m10 the Roundtable Hold) at most every 30 s through its own
// list request while no search is running; a running search's result is counted instead (counts_from_list).
static DWORD g_count_tick;
static void counts_from_list(unsigned n) {
    int in_game = 0, looking = 0, waiting = 0;
    for (unsigned i = 0; i < n && i < 50; i++) {
        u64 id = g_steam.by_index(g_steam.mm, (int)i);
        const char *proto = g_steam.get_data(g_steam.mm, id, LOBBY_PROTO_KEY), *ver = g_steam.get_data(g_steam.mm, id, LOBBY_VER_KEY);
        if (!proto || strcmp(proto, LOBBY_PROTO) || !ver || strcmp(ver, MOD_VERSION) || (id == g_lobby && g_lobby)) continue;
        const char *ph = g_steam.get_data(g_steam.mm, id, LOBBY_PHASE_KEY), *af = g_steam.get_data(g_steam.mm, id, LOBBY_ALLY_FREE_KEY);
        int hub = ph && !strcmp(ph, "hub"), late = ph && (!strcmp(ph, "boss") || !strcmp(ph, "late"));
        if (hub) waiting++;
        else in_game++;
        if (!late && af && atoi(af) > 0) looking++;
    }
    static int last[3] = {-1, -1, -1};
    if (in_game == last[0] && looking == last[1] && waiting == last[2]) return;
    last[0] = in_game, last[1] = looking, last[2] = waiting;
    static _Alignas(16) char text[3][0x100];
    static unsigned short line[3][128];
    const unsigned short *lines[3];
    int value[3] = {in_game, looking, waiting};
    for (int k = 0; k < 3; k++) {
        char *t = ((char *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(text[k], 131172 + k);
        const unsigned short *tpl = t && *(unsigned short **)t ? *(unsigned short **)t : t ? *(unsigned short **)(t + 0x10) : NULL;
        char num[12];
        snprintf(num, sizeof num, "%d", value[k]);
        int m = 0;
        for (int i = 0; tpl && !IsBadReadPtr(tpl + i, 2) && tpl[i] && i < 100 && m < 120; i++) {
            if (tpl[i] == '%' && tpl[i + 1] == 'd') {
                for (const char *a = num; *a && m < 120; a++) line[k][m++] = (unsigned char)*a;
                i++;
            } else line[k][m++] = tpl[i];
        }
        line[k][m] = 0;
        lines[k] = line[k];
    }
    logf("counts: squads in an expedition %d, looking for a player %d, hosts waiting in the hub %d", in_game, looking, waiting);
    ov_set_lines(lines, 3);
}
static int counts_in_hub(void) {
    char *me = local_player();
    unsigned blk = me && !IsBadReadPtr(me + CHR_BLOCK_ID, 4) ? *(unsigned *)(me + CHR_BLOCK_ID) : 0;
    return me && (blk >> 24) == 0x0a && !g_in_expedition && g_join != 4;
}
static void counts_tick(void) {  // game thread (step hook)
    if (g_ov_off) return;
    if (!counts_in_hub()) {
        ov_hide();
        return;
    }
    if (!g_ov.shown && g_ov.has_text) g_ov.shown = 1;  // back in the hub: the last counts again
    if (!steam_ready()) return;
    DWORD now = GetTickCount();
    if (!g_count_call) {
        if (g_list_call || g_searching || g_party || (g_count_tick && now - g_count_tick < 30000)) return;  // a search counts instead
        g_steam.string_filter(g_steam.mm, LOBBY_MARK_KEY, "1", LOBBY_EQUAL);
        g_steam.distance_filter(g_steam.mm, LOBBY_WORLDWIDE);
        g_count_call = g_steam.request_list(g_steam.mm);
        g_count_tick = now;
        return;
    }
    unsigned char failed = 0;
    if (!g_steam.completed(g_steam.utils, g_count_call, &failed)) {
        if (now - g_count_tick > SEARCH_TIMEOUT_MS) g_count_call = 0;
        return;
    }
    struct { unsigned matching; } result = {0};
    unsigned char got = g_steam.result(g_steam.utils, g_count_call, &result, sizeof result, LOBBY_MATCH_LIST, &failed);
    g_count_call = 0;
    if (got && !failed) counts_from_list(result.matching);
}
static void toggle_search(const char *where) {
    g_invasion_pending = 0;
    g_searching = !g_searching;
    if (g_searching) g_ver_notice_armed = 1;
    lobby_search_reset();
    if (g_searching) {
        if (g_stop_pending) finish_pending_stop(1);
        join_test_start();
        set_network(1);
    } else if (guard_intact() && join_leave()) {
        g_stop_pending = GetTickCount();  // a joined session: the network goes down once the leave is through
    } else set_network(0);  // the search only looks: no session of ours to leave
    clear_notices();
    if (g_searching || !g_invasion_ended) notice(g_searching ? 0 : 1);  // after an invasion: no "search cancelled"
    if (!g_searching) g_invasion_ended = 0;
    logf("invasion: search %s (%s)", g_searching ? "started" : "called off", where);
}

// Every step of the machine runs through here (step: its index in the table).
// With one player a request moves the machine to SingleStart at once, and an Invasion request is
// stopped there. With more players the machine stays idle offline (the game waits for its own
// matchmaking), so an Invasion request still pending a moment later is taken up from the idle step.
// Move test (joiner, at sync step 10 EndLobbyExit, controller idle in the Roundtable Hold). In order:
//  1. pending map state through the game's one-line setters (0x6b0610 and the others 0x740110 /
//     0xdf8a00 use; field matched by reading each setter): block gm+0xa74 = the host's nrpvp_block,
//     gm+0xaac = 1 (0x6b05f0) and type gm+0xdc0 = 14 (0x6b0410), both as the host has them in its
//     expedition. Join reason, +0xdc1 and the position stay as they are (no value we know; the
//     position goes through protected code).
//  2. context (vtable 0x2c75d20): mode +0x9d (vt+0x50), expected players +0x98 (vt+0x30). MoveMapStart
//     (0xb593a0) with mode 2 sends a member-count mismatch to FailedStartMultiplay, so then the
//     expected count is set to the session's member count with the context's own setter (vt+0x28,
//     0xb454a0: one write). With mode 2 it also waits until gm+0xb40 is set (logged only).
//  3. the controller's own set-step, vtable 0x2c76bd8 entry 4 (0xb5d840, a thunk into the game's
//     protected code; the same call our expedition-start cancel makes, and what the game's steps do,
//     e.g. SingleStart 0xb5c010 ends with vt+0x20(ctrl, 13)), with 21 MoveMapStart, made from inside
//     the controller's step like the game's own calls; this frame's None step is then skipped.
//     MoveMapStart accepts sync steps 0, 1 and 10 (0xdf0a80 -> 0xdcd030) and otherwise leaves.
// Entry at MoveMapWait (13) was not chosen: it only waits (timer, cutscenes, members' +0xe7) and
// then goes to HeroReSelect (14, the group's hero choice, which needs the host there too) in mode 2,
// or to SingleSetup (18, a solo set-up job chain) otherwise.
#define RVA_QM_CTRL_VTABLE  0x2c76bd8
#define RVA_QM_SET_STEP     0xb5d840
#define RVA_QM_CTX_VTABLE   0x2c75d20
#define RVA_SET_PENDING_BLOCK 0x6b0610  // gm+0xa74 = *arg
#define RVA_SET_PENDING_AAC   0x6b05f0  // gm+0xaac = arg
#define RVA_SET_PENDING_TYPE  0x6b0410  // gm+0xdc0 = arg
#define RVA_SET_PENDING_REASON 0x6b0520 // gm+0xdbc = arg
static void pending_log(const char *when) {
    char *gm = *(char **)(g_base + RVA_GAME_MAN_PTR);
    if (gm) logf("move test: %s: pending block %08x +aac %u type %u reason %d +dc1 %u", when, *(unsigned *)(gm + 0xa74),
                 (unsigned char)gm[0xaac], (unsigned char)gm[0xdc0], *(int *)(gm + 0xdbc), (unsigned char)gm[0xdc1]);
}
static int move_force(char *ctrl) {
    g_move = 2;
    g_move_tick = GetTickCount();
    void **vt = *(void ***)ctrl;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR), *gm = *(char **)(g_base + RVA_GAME_MAN_PTR);
    // vt[4] is the game's set-step, or our logging wrapper around it (hook_set_step), which calls the
    // game's function exactly once through the saved pointer.
    void *set_step = vt[QM_CTRL_VT_SET_STEP];
    int set_step_ok = set_step == (void *)(g_base + RVA_QM_SET_STEP) ||
                      (set_step == (void *)set_step_wrap && g_set_step_original == (void (*)(void *, int))(g_base + RVA_QM_SET_STEP));
    if ((char *)vt != g_base + RVA_QM_CTRL_VTABLE || !set_step_ok || !sm || !gm) {
        join_over("move test: controller or managers not as expected");
        return 0;
    }
    // The expedition's start block (as a solo start uses, with +0xaac 1 and reason -14), so the game
    // places the player at the expedition's start; the host's current block only if that key is absent.
    const char *text = g_steam.get_data(g_steam.mm, g_candidate, LOBBY_START_KEY);
    if (!text || !*text) {
        logf("move test: the host's lobby has no %s, using %s", LOBBY_START_KEY, LOBBY_BLOCK_KEY);
        text = g_steam.get_data(g_steam.mm, g_candidate, LOBBY_BLOCK_KEY);
    }
    char *end = NULL;
    unsigned block = text && *text ? (unsigned)strtoul(text, &end, 16) : 0;
    if (!text || !*text || !end || *end || block == 0 || block == 0xffffffff) {
        logf("move test: the host's lobby has no usable %s ('%s')", LOBBY_BLOCK_KEY, text ? text : "");
        join_over("move test: no host block");
        return 0;
    }
    ctx_log("move test");
    play_state_log("move test, before");
    {
        const char *hg = g_steam.get_data(g_steam.mm, g_candidate, LOBBY_GEN_KEY);
        logf("move test: generator results: ours %x,%x,%x, host's %s", *(unsigned *)(gm + 0xb48), *(unsigned *)(gm + 0xb54),
             *(unsigned *)(gm + 0xb5c), hg && *hg ? hg : "(none)");
        // The generator (0x6aec40) leaves its result in gm +0xb48 / +0xb54 / +0xb5c; the next map request
        // (0xc27870 -> 0x6b06f0) turns them into the current expedition (+0xb4c.. +0xb64) and the map to
        // load. They have no readable setter (written only by protected code), so the host's values are
        // written straight into the same fields.
        unsigned g1, g2, g3;
        if (hg && sscanf(hg, "%x,%x,%x", &g1, &g2, &g3) == 3) {
            *(unsigned *)(gm + 0xb48) = g1, *(unsigned *)(gm + 0xb54) = g2, *(unsigned *)(gm + 0xb5c) = g3;
            logf("move test: generator results set to the host's: %x,%x,%x", g1, g2, g3);
        }
    }
    // Play state as a group guest's start sets it (SingleStart 0xb5c010 / GuestReadyWait 0xb545a0:
    // 0x200a20(2) requested mode expedition; 0x200a30 requested +0xdd = 2 so that after the switch
    // +0xdc is 2 and the equipment packet 0xc is sent; gm+0xaf0 cleared with 0x6abae0, or the move
    // request turns the requested mode into the sparring set-up 1).
    ((void (*)(unsigned char))(g_base + 0x200a20))(2);
    ((void (*)(unsigned char))(g_base + 0x200a30))(2);
    if (gm[0xaf0]) ((void (*)(void))(g_base + 0x6abae0))();
    play_state_log("move test, after");
    pending_log("before");
    {
        char *g0 = *(char **)(g_base + 0x3c13258);
        g_saved_pending[0] = *(unsigned *)(g0 + 0xa74), g_saved_pending[1] = (unsigned char)g0[0xaac];
        g_saved_pending[2] = (unsigned char)g0[0xdc0], g_saved_pending[3] = *(unsigned *)(g0 + 0xdbc);
        g_saved_pending_valid = 1;
    }
    ((void (*)(unsigned *))(g_base + RVA_SET_PENDING_BLOCK))(&block);
    ((void (*)(unsigned char))(g_base + RVA_SET_PENDING_AAC))(1);
    ((void (*)(unsigned char))(g_base + RVA_SET_PENDING_TYPE))(14);
    // Join reason as a solo start sets it: SingleStart (0xb5c010) fills the join parameters with the
    // context's vt+8 (0xb44630: reason -14 = 0xfffffff2 at +0x1c, type 14 at +0x20, +0x18 = 1 when the
    // context is not a rejoin) and applies them with 0x740110, which passes the reason to 0x6b0520.
    ((void (*)(int))(g_base + RVA_SET_PENDING_REASON))(-14);
    pending_log("after (start block from the host's lobby)");
    char *ctx = *(char **)(ctrl + 0xa8);
    if (!ctx || *(char **)ctx != g_base + RVA_QM_CTX_VTABLE) {
        join_over("move test: controller context not as expected");
        return 0;
    }
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *stop = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    int members = begin && stop >= begin ? (int)((stop - begin) / SM_PLAYER_SIZE) : 0;
    // Decision 11: the player count in the menu does not apply to invasions. The invader's context is
    // put in mode 1 (what "1 player" gives; the run of 5c775057 entered the host's world that way): no
    // count check and no online-flag wait in MoveMapStart, MoveMap lets go at once (+0x10a).
    g_ctx_old_mode = (unsigned char)ctx[0x9d] | 0x100;
    if ((unsigned char)ctx[0x9d] != 1) {
        unsigned char was = (unsigned char)ctx[0x9d];
        ((void (*)(void *, unsigned char))(*(void ***)ctx)[0x48 / 8])(ctx, 1);
        logf("move test: context mode %u -> %u (context setter vt+0x48)", was, (unsigned char)ctx[0x9d]);
    }
    unsigned char mode = (unsigned char)ctx[0x9d];
    int expected = *(int *)(ctx + 0x98);
    logf("move test: context mode %u, expected players %d, session members %d, gm+0xb40 %u", mode, expected, members,
         (unsigned char)gm[0xb40]);
    if (mode == 2 && expected != members) {
        ((void (*)(void *, int))(*(void ***)ctx)[0x28 / 8])(ctx, members);
        logf("move test: expected players set to %d (was %d), so MoveMapStart's count check passes", *(int *)(ctx + 0x98), expected);
    }
    // gm+0xb40 is the game's online-mode flag. Its only writers are the network flow's steps (0xde72f0
    // Init, 0xde7f90 OfflineMode: 1 once the network is ready, 0xde7330 LeaveOnlineMode: 0), all through
    // the one-line setter 0x6b0530. Our network runs with OfflineMode held (flow_offline_hook), so the
    // flag stays 0, and MoveMapStart in mode 2 waits for it. Set it the way OfflineMode would; the flow
    // itself stays held (no NotifyLeaveOfflineModeWait / OnlineMode / ServerRequestWait). Put back at 0
    // when the test stops.
    if (mode == 2 && !gm[0xb40]) {
        if (g_network_on && guard_intact()) {
            ((void (*)(unsigned char))(g_base + RVA_SET_ONLINE_MODE))(1);
            g_move_online = 1;
            logf("move test: online-mode flag gm+0xb40 set to %u (as the network flow's OfflineMode does)", (unsigned char)gm[0xb40]);
        } else logf("move test: mode 2 with gm+0xb40 0 and no network of ours: MoveMapStart will wait");
    }
    logf("move test: controller +0x40 %d +0x44 %d before set-step(%d)", *(int *)(ctrl + 0x40), *(int *)(ctrl + 0x44),
         QM_STEP_MOVE_MAP_START);
    ((void (*)(void *, int))vt[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_MOVE_MAP_START);
    logf("move test: controller +0x40 %d +0x44 %d after", *(int *)(ctrl + 0x40), *(int *)(ctrl + 0x44));
    return 1;
}

static int cursor_on_invasion(char *dialog);
static char *g_dialog;
static DWORD g_request_tick;
static void step_hook(void *ctrl, void *time, int step) {
    g_last_frame = GetTickCount();
    if (1) marker_clear();
    static int last_seen_start = -1;
    if (step == QM_STEP_NONE) last_seen_start = -1;
    static int last = -1;
    g_qm_ctrl = ctrl;
    g_qm_step = step;
    if (step != last) {
        if (step != QM_STEP_NONE) log_start(ctrl, step);
        else logf("step None");
        last = step;
        if (g_party) party_ctx_log(ctrl, STEP_NAMES[step]);
    }
    // party: the matching steps (FromSoftware's server) never run; the party gathers in None
    // while a player waits for / joins its party's host, nothing else starts from the menu (a solo start or
    // Invasion ends the party first, in the fill-context hook)
    if (g_party && !g_party_host && step >= 12 && step <= 20) {
        logf("party: step %s while waiting for the party's host: set back to None", STEP_NAMES[step]);
        ((void (*)(void *, int))(*(void ***)ctrl)[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_NONE);
        return;
    }
    if (g_party && step >= 1 && step <= 11) {
        logf("party: matching step %s set back to None (gathered over our lobby)", STEP_NAMES[step]);
        ((void (*)(void *, int))(*(void ***)ctrl)[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_NONE);
        return;
    }
    if (g_party == PARTY_HOST_STARTED) party_host_step(ctrl, step);
    if (g_network_on && g_guard_broken) {
        if (g_searching) toggle_search("network guard broken");
        else {
            logf("network: guard broken, taking the network down");
            set_network(0);
        }
    }
    // Both roles live in every player (no switches since 14c271a): its own expeditions host; while it is
    // an invader in another player's world (g_join 4) it opens and advertises nothing of its own.
    // map-test-control / two-regions: the generator draws the pattern (generate_hook); here only a check.
    if (g_join != 4 && step == 21) {
        char *gm = *(char **)(g_base + 0x3c13258);
        static unsigned told = 0xffffffff;
        unsigned pat = gm ? *(unsigned *)(gm + 0xb48) : 0xffffffff;
        if (gm && pat != told)
            told = pat, logf("map test: generator pattern %u (%s)", pat,
                             two_pattern(pat) ? "a two-region slot, as wanted" : "NOT a two-region slot");
    }
    if (step == QM_STEP_INGAME && g_two_active) {
        two_regions_flasks();
        // 7603 comes from the merged pattern record (launcher: a 7603 row before 7601), set by the game's
        // OnMapEnter at map entry like on a Rotted Woods map; nothing is written here (measurement 3: our
        // delayed write was the only DLL flag write; the hawk problem of runs 33..35 was start group 708).
        // Watched, never written here (logged when they change): 7601, the powers 9193 / 9192, and the
        // Rotted Woods' pattern flag 1046300590 (cleared by the game every frame in run 35).
        static const int WATCH[5] = {7601, 7603, 9193, 9192, 1046300590};
        static int seen[5] = {-1, -1, -1, -1, -1}, changes;
        for (int k = 0; k < 5; k++) {
            int v = two_flag(WATCH[k], 0, 0);
            if (v != seen[k] && changes < 40) changes++, logf("two-regions: event flag %d is %s", WATCH[k], v ? "on" : "off"), seen[k] = v;
        }
    }
    no_sprint_frame();
    g_in_exped = step == QM_STEP_INGAME || g_join_in_world();
    night_hide_frame(g_in_exped);
    day_flow_frame(g_in_exped);
    if (step == QM_STEP_INGAME || g_join_in_world()) invader_notice_frame();
    summon_kill_reward();
    crash_watch_members();
    squad_count_tick();
    counts_tick();
    static int told_invader;
    if (g_join != 4) {
        if (told_invader) told_invader = 0, logf("role: no longer an invader; own expeditions host again");
        host_expedition(step);
    } else if (!told_invader) {
        told_invader = 1;
        logf("role: invader in another player's world: no host session of our own (host role %d, in expedition %d)", g_host_test,
             g_in_expedition);
    }
    if (g_host_test == 2 || g_stop_pending) host_test_watch();
    if (g_stop_pending) finish_pending_stop(0);
    if (step != QM_STEP_NONE && g_searching && g_join == 4) {
        join_frame();  // enter test: the game may be taking the client into the host's world; network stays up
        if (g_join == 3) return;  // just stopped (and a forced step may have been set back): skip this frame's step
    } else if (step != QM_STEP_NONE) {
        // Safety net (run 6e9bdd30: after an invasion the menu's start reached SingleStart without the
        // fill-context call our choice is read in): a start with the cursor on Invasion is an invasion,
        // whatever path led to it.
        if (step == 12 && !g_invasion_pending && !g_searching && step != last_seen_start && cursor_on_invasion(g_dialog) == 1) {
            logf("invasion: SingleStart reached with the cursor on Invasion without a start request we saw; treated as an invasion");
            g_invasion_pending = 1;
            g_request_tick = GetTickCount();
        }
        last_seen_start = step;
        if (g_invasion_pending) {
            void **vt = *(void ***)ctrl;
            ((void (*)(void *, int))vt[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_NONE);
            toggle_search("expedition start cancelled");
            return;
        }
        if (g_searching) {
            g_searching = 0;
            lobby_search_reset();
            set_network(0);
            clear_notices();
            logf("invasion: search ended, an ordinary expedition is starting");
        }
    } else {  // the idle step runs every frame in the Roundtable Hold
        if (g_party == PARTY_PENDING || g_party == PARTY_GUEST_WAIT || g_party == PARTY_HOST_CREATE) party_frame(ctrl);
        if (g_invasion_pending && GetTickCount() - g_request_tick > 300) toggle_search("machine stayed idle");
        if (g_searching && notices_idle()) notice(g_party ? 2 : 0);
        // No lobby list requests while a join is under way: nothing to choose then, and one less
        // thing talking to Steam during the join.
        if (g_searching && !(g_join_test && (g_join == 1 || g_join == 2 || g_join == 4))) lobby_search_frame("search");
        if (g_searching && g_join_test) join_frame();
        if (g_move == 1 && g_join == 4 && g_searching && move_force(ctrl)) return;
    }
    g_step_original[step](ctrl, time);
}

// The menu's start request reaches the context through "fill context" (0xb67cc0, two call sites):
// u64 (self*, request*), with the context at self+8. In the request: the "Number of Players" choice
// at +0x49 (0 three, 1 two, 2 one) and the chosen Target rows as a u32
// vector at +0x58 (begin) / +0x60 (end). The function turns rows into boss numbers, where our row
// becomes boss 0, and the choice into a player count.
#define RVA_FILL_CONTEXT       0xb67cc0
#define RVA_FILL_CONTEXT_CALL1 0xb67199  // e8 22 0b 00 00
#define RVA_FILL_CONTEXT_CALL2 0xb671e0  // e8 db 0a 00 00
#define REQ_PLAYERS_CHOICE     0x49
#define REQ_ROWS_BEGIN         0x58
#define PLAYERS_CHOICE_FOUR    3         // the old fourth option's value; the offline compare below still admits it (harmless)
static char *g_dialog;                       // defined with the start button code below
static int cursor_on_invasion(char *dialog);
static unsigned long long fill_context_hook(char *self, char *request) {
    unsigned *begin = *(unsigned **)(request + REQ_ROWS_BEGIN), *end = *(unsigned **)(request + REQ_ROWS_BEGIN + 8);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    // Same rule as the button's label: the entry under the cursor. If the dialog cannot be read,
    // fall back on the chosen rows, where the game itself looks at the last one for its special row.
    int invasion = cursor_on_invasion(g_dialog);
    if (invasion < 0) invasion = count && count <= 64 && begin[count - 1] == INVASION_ROW;
    unsigned long long result = ((unsigned long long (*)(char *, char *))(g_base + RVA_FILL_CONTEXT))(self, request);
    g_invasion_pending = invasion;
    g_request_tick = GetTickCount();
    unsigned char choice = (unsigned char)request[REQ_PLAYERS_CHOICE];
    if (g_party && !g_party_host && (invasion || choice == 2)) {  // something else chosen: the party wait ends
        if (g_searching) toggle_search(invasion ? "party: Invasion chosen" : "party: a solo expedition chosen");
        party_end(invasion ? "Invasion chosen" : "a solo expedition chosen");
    }
    if (!invasion && (choice == 0 || choice == 1)) party_request(self, request);
    logf("start request: players choice %u, %u target rows, invasion %d", (unsigned char)request[REQ_PLAYERS_CHOICE],
         (unsigned)count, invasion);
    return result;
}

// "Begin expedition" in the menu first checks the request (rax) for offline play:
//     cmp byte [rax+0x49], 1 ; ja no      choices 0 and 1 (three, two players) ask "is online play on?"
//     call is_online ; test al, al ; jne ok
// no: cmp byte [rax+0x49], 2 ; je ok      choice 2 (one player) is always fine
//     ... "cannot start a multiplayer session in offline mode"
// Two changes: the first comparison takes our choice 3 as well (it was refused even online), and
// the online question is answered "yes" when Invasion is the chosen target, because an invasion is
// started by us, not by the game's matchmaking, whatever the squad size option says.
#define RVA_OFFLINE_CMP_IMM   0x9eee9a  // the "1" of: 80 78 49 01
#define RVA_OFFLINE_CALL      0x9eee9d  // e8 3e fc cb ff
#define RVA_IS_ONLINE         0x6aeae0  // bool (void)
static unsigned char online_check_hook(char *request) {
    unsigned *begin = *(unsigned **)(request + REQ_ROWS_BEGIN), *end = *(unsigned **)(request + REQ_ROWS_BEGIN + 8);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    for (size_t i = 0; i < count && i < 64; i++)
        if (begin[i] == INVASION_ROW) return 1;
    if (request[REQ_PLAYERS_CHOICE] <= 1) return 1;  // a squad expedition: gathered by us over Steam (party), never by the server
    return ((unsigned char (*)(void))(g_base + RVA_IS_ONLINE))();
}

static void hook_offline_check(void) {
    unsigned char *imm = (unsigned char *)g_base + RVA_OFFLINE_CMP_IMM, *site = (unsigned char *)g_base + RVA_OFFLINE_CALL;
    int expected = (int)(RVA_IS_ONLINE - (RVA_OFFLINE_CALL + 5));
    unsigned char *stub = near_alloc(16);
    static const unsigned char cmp[] = {0x80, 0x78, 0x49, 0x01};
    if (memcmp(imm - 3, cmp, 4) || site[0] != 0xE8 || memcmp(site + 1, &expected, 4) || !stub) {
        logf("offline check: code does not look as expected, not hooking");
        return;
    }
    void *hook = (void *)online_check_hook;
    stub[0] = 0x48; stub[1] = 0x89; stub[2] = 0xC1;  // mov rcx, rax   (the request)
    stub[3] = 0x48; stub[4] = 0xB8;                  // mov rax, hook
    memcpy(stub + 5, &hook, 8);
    stub[13] = 0xFF; stub[14] = 0xE0;                // jmp rax
    DWORD old;
    if (!patch_rel32(site + 1, site + 5, stub) || !VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) {
        logf("offline check: cannot patch");
        return;
    }
    *imm = PLAYERS_CHOICE_FOUR;
    VirtualProtect(imm, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), imm, 1);
    logf("offline check: hooked");
}

// The start button's label. The dialog builds it from one of several CL_MenuText lines ("Begin
// expedition", "... 1 player", "... 2 players", ...) through seven calls to the text constructor,
// with the dialog in rbx. With the cursor on Invasion the line is replaced: "Begin invasion", or
// "Cancel invasion" while a search is running (pressing the button then calls the search off).
#define TEXT_BEGIN_INVASION  131166
#define TEXT_CANCEL_INVASION 131164
// Which target a press of the start button would use, as the player sees it: the entry the list
// cursor is on (F chooses that entry and moves to the button). The dialog holds the grid at +0xff0
// and the item list at +0x19a0 (virtual slot 5: item by index; the row id is at item+0x10).
#define DIALOG_GRID        0xff0
#define DIALOG_ITEM_LIST   0x19a0
#define RVA_GRID_CURSOR    0x781a50  // int (GridControl*): index of the entry under the cursor
static char *g_dialog;  // the dialog the label was last built for
static int cursor_on_invasion(char *dialog) {
    if (!dialog || IsBadReadPtr(dialog + DIALOG_ITEM_LIST, 8)) return -1;
    char *list = dialog + DIALOG_ITEM_LIST;
    void **vt = *(void ***)list;
    if (IsBadReadPtr(vt, 6 * 8)) return -1;
    int index = ((int (*)(void *))(g_base + RVA_GRID_CURSOR))(dialog + DIALOG_GRID);
    char *begin = *(char **)(list + 0x10), *end = *(char **)(list + 0x18);
    if (index < 0 || !begin || end < begin || index >= (end - begin) / 0x28) return -1;
    char *item = ((char *(*)(void *, int))vt[5])(list, index);
    return item && !IsBadReadPtr(item + 0x10, 4) ? *(int *)(item + 0x10) == INVASION_ROW : -1;
}
static void *button_text_hook(void *out, int id, char *dialog) {
    g_dialog = dialog;
    if (cursor_on_invasion(dialog) == 1) id = g_searching ? TEXT_CANCEL_INVASION : TEXT_BEGIN_INVASION;
    return ((void *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(out, id);
}
static void hook_button_text(void) {
    static const unsigned sites[] = {0x9ef45e, 0x9ef4b8, 0x9ef509, 0x9ef563, 0x9ef5ce, 0x9ef61f, 0x9ef676};
    unsigned char *stub = near_alloc(16);
    void *hook = (void *)button_text_hook;
    if (!stub) return;
    stub[0] = 0x49; stub[1] = 0x89; stub[2] = 0xD8;  // mov r8, rbx   (the dialog)
    stub[3] = 0x48; stub[4] = 0xB8;                  // mov rax, hook
    memcpy(stub + 5, &hook, 8);
    stub[13] = 0xFF; stub[14] = 0xE0;                // jmp rax
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        unsigned char *site = (unsigned char *)g_base + sites[i];
        int expected = (int)(RVA_MENU_TEXT_CTOR - (sites[i] + 5));
        if (site[0] != 0xE8 || memcmp(site + 1, &expected, 4)) {
            logf("start button: site %x does not look as expected, not hooked", sites[i]);
            continue;
        }
        patch_rel32(site + 1, site + 5, stub);
    }
    logf("start button: hooked");
}

static void hook_expedition_start(void) {
    static const unsigned sites[] = {RVA_FILL_CONTEXT_CALL1, RVA_FILL_CONTEXT_CALL2};
    unsigned char *stub = near_alloc(12);
    for (int i = 0; i < 2; i++) {
        unsigned char *site = (unsigned char *)g_base + sites[i];
        int expected = (int)(RVA_FILL_CONTEXT - (sites[i] + 5));
        if (site[0] != 0xE8 || memcmp(site + 1, &expected, 4) || !stub) {
            logf("expedition start: fill-context call does not look as expected, not hooking");
            return;
        }
    }
    void *hook = (void *)fill_context_hook;
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, hook; jmp rax
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    for (int i = 0; i < 2; i++) {
        unsigned char *site = (unsigned char *)g_base + sites[i];
        if (!patch_rel32(site + 1, site + 5, stub)) {
            logf("expedition start: cannot patch a fill-context call");
            return;
        }
    }

    // One small stub per table entry: "mov r8d, index; mov rax, step_hook; jmp rax".
    step_fn *table = (step_fn *)(g_base + RVA_QM_STEP_TABLE);
    if (table[QM_STEP_NONE * 2] != (step_fn)(g_base + RVA_QM_STEP_NONE_FN) ||
        table[1 * 2] != (step_fn)(g_base + RVA_QM_STEP_SEARCH_REGISTER_FN)) {
        logf("expedition start: step table does not look as expected, not hooking");
        return;
    }
    unsigned char *stubs = near_alloc(QM_STEP_COUNT * 32);
    if (!stubs) {
        logf("expedition start: no memory for the step stubs");
        return;
    }
    void *common = (void *)step_hook;
    for (int i = 0; i < QM_STEP_COUNT; i++) {
        unsigned char *st = stubs + i * 32;
        st[0] = 0x41; st[1] = 0xB8;  // mov r8d, i
        memcpy(st + 2, &i, 4);
        st[6] = 0x48; st[7] = 0xB8;  // mov rax, step_hook
        memcpy(st + 8, &common, 8);
        st[16] = 0xFF; st[17] = 0xE0;  // jmp rax
        g_step_original[i] = table[i * 2];
    }
    for (int i = 0; i < QM_STEP_COUNT; i++) table[i * 2] = (step_fn)(void *)(stubs + i * 32);
    logf("expedition start: hooked");
}

// Level investigation (debug-log only, logging only). About once a second, the 32-bit words that changed
// in: the local player data (play state +8, first 0x200 bytes), the local character's data module
// (*(ChrIns +0x1b8), first 0x200 bytes: HP and maxima), and on the invader each other-player slot (play
// state +0x10, 0x8a0 bytes). One line per region per check, at most 12 changes on it; a word that has
// changed 30 times is muted (timers, positions), logged once.
#define DIFF_WORDS (0x8a0 / 4)
typedef struct { const char *name; void *base; unsigned n; unsigned old[DIFF_WORDS]; unsigned char hits[DIFF_WORDS]; int primed; } diff_t;
static void diff_region(diff_t *d, void *base, unsigned bytes) {
    if (!base || IsBadReadPtr(base, bytes)) { d->primed = 0; return; }
    unsigned n = bytes / 4;
    if (base != d->base || n != d->n) d->base = base, d->n = n, d->primed = 0, memset(d->hits, 0, sizeof d->hits);
    unsigned *now = (unsigned *)base;
    if (!d->primed) {
        memcpy(d->old, now, n * 4);
        d->primed = 1;
        return;
    }
    // run 27: a cap of 12 changes per line hid the rest of a level-up; now every change, 10 per line
    char line[600];
    int at = 0, shown = 0;
    for (unsigned i = 0; i < n; i++) {
        if (now[i] == d->old[i]) continue;
        if (d->hits[i] < 255) d->hits[i]++;
        if (d->hits[i] == 30) logf("level test: %s +0x%03x changes often, muted", d->name, i * 4);
        if (d->hits[i] < 30) {
            if (!at) at = snprintf(line, sizeof line, "level test: %s:", d->name);
            at += snprintf(line + at, sizeof line - at, " +0x%03x %d->%d", i * 4, (int)d->old[i], (int)now[i]);
            if (++shown % 10 == 0) logf("%s", line), at = 0;
        }
        d->old[i] = now[i];
    }
    if (at) logf("%s", line);
}
static void level_watch(void) {
    static DWORD next;
    static diff_t pd, cm, sl[4];
    pd.name = "player data", cm.name = "character data", sl[0].name = "other slot 0", sl[1].name = "other slot 1";
    sl[2].name = "other slot 2", sl[3].name = "other slot 3";
    if (!g_debug_log || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 1000;
    char *ps = *(char **)(g_base + 0x3c078d0);
    if (!ps || IsBadReadPtr(ps, 0x20)) return;
    diff_region(&pd, *(char **)(ps + 8), 0x800);
    char *me = local_player();
    char *mod = me && !IsBadReadPtr(me, 0x1c0) ? *(char **)(me + 0x1b8) : NULL;
    diff_region(&cm, mod && !IsBadReadPtr(mod, 8) ? *(char **)mod : NULL, 0x400);
    static diff_t st;  // the play state itself (+0x000..+0x400): an expedition level may live there
    st.name = "play state";
    diff_region(&st, ps, 0x400);
    if (g_join_in_world()) {
        char *slots = *(char **)(ps + 0x10);
        for (int i = 0; i < g_cap_remote; i++) diff_region(&sl[i], slots ? slots + i * 0x8a0 : NULL, 0x8a0);
    }
}
static DWORD WINAPI run(LPVOID self) {
    (void)self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP) {
        logf("unsupported game build (timestamp %08lx), doing nothing", nt->FileHeader.TimeDateStamp);
        return 0;
    }
    logf("loaded, game base %p, mod version %s", g_base, MOD_VERSION);
    if (!g_settings_early) settings_load();
    boss_deaths_load();
    if (g_guard_ok) logf("net guard: installed, %u of %u imports patched (outbound %s)", (unsigned)GUARDED_COUNT,
                         (unsigned)GUARDED_COUNT, NET_GUARD_REFUSE_OUTBOUND ? "refused" : "logged only");
    else logf("net guard: NOT installed (%s); the network will not be started", g_guard_msg);
    // Seamless Co-op changes the same menu and lobby code. Next to it we only watch and log.
    if (GetModuleHandleA("nrsc.dll")) {
        logf("Seamless Co-op is loaded: observing only, no menu changes");
    } else {
        hook_target_list();
        hook_map_load();
        hook_players_option();
        hook_password_players_row();
        hook_expedition_start();
        hook_offline_check();
        hook_network_flow();
        hook_button_text();
        hook_eac_begin();
        hook_acceptance();
    }
    fastfail_install();  // again: the second pointer may be filled only after DllMain
    logf("crash logger: unhandled-exception filter in place (fatal codes only, chained to the one before); fast-fail record %s "
         "(import slot %d, CRT pointer %d); exit line at DLL detach", g_ff_hooked ? "on" : "NOT on", g_ff_hooked & 1, g_ff_hooked >> 1 & 1);
    for (int tick = 0;; tick++) {
        Sleep(250);
        if (tick % 4 == 0) crash_watch();
        if (g_ff_tick && GetTickCount() - g_ff_tick > 2000) {  // the record above was not followed by an end
            g_ff_tick = 0, g_crash_once = 0;
            logf("crash watch: the fast-fail check of the record above was not followed by an end; record armed again");
        }
        {  // one line per stall: no frame tick for more than 10 s while the game had been ticking
            static int stalled;
            DWORD last = g_last_frame, gap = last ? GetTickCount() - last : 0;
            if (gap > 10000 && !stalled) stalled = 1, logf("crash watch: no frame tick for %lu s (a loading screen can do this too)", gap / 1000);
            if (gap < 1000) stalled = 0;
        }
        guard_intact();
        watch_network();
        watch_session();
        code_patch_check();
        level_watch();
        lag_tick();
        act_wrap_tick();
        hooks_retry();
        if (1) marker_tick();
        if (1) finish_tick();
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        // Log next to the DLL, so it is easy to find from outside the game.
        GetModuleFileNameA(self, log_path, MAX_PATH);
        char *dot = strrchr(log_path, '.');
        if (dot) strcpy(dot, ".log");
        g_base = (char *)GetModuleHandleA(NULL);
        crash_install(self);
        if (((IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew))->FileHeader.TimeDateStamp == GAME_TIMESTAMP) fastfail_install(), ov_install(), capacity_install(), steam_shutdown_install();
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
        // The guard goes in first, here rather than in run(): this runs before me3 lets the game go
        // on, while run() starts only later, alongside the game's own start-up.
        if (nt->FileHeader.TimeDateStamp == GAME_TIMESTAMP) {
            install_net_guard();
            // Run 16: the invader's hits are dropped on its own machine before the hit is even registered,
            // although 16 -> 77 had been made Enemy at hook time. If the game derives its hit filters
            // from the relation table while it starts, a change made later is never seen; so the
            // settings are read here and the entry is changed before the game starts (experiment).
            settings_load();
            g_settings_early = 1;
            if (1) finish_relation_patch();
        }
        CreateThread(NULL, 0, run, self, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        exit_line(reserved != NULL);
    }
    return TRUE;
}

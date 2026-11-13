// Nightreign invasion mod, client DLL.
//
// What it does today:
//   - logs the session manager's state (lobby state, player count, join data) whenever it changes,
//     and how many invaders the current squad would allow;
//   - adds an "Invasion" entry to the expedition menu's Target list (row 101 of NightBossMenuParam,
//     which must exist in regulation.bin: mod/build_regulation.py, or the launcher) and a fourth
//     entry to the "Number of Players" option;
//   - makes that fourth option ask for a squad of four and raises the session's player limit to six;
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
#define SESSION_PLAYERS  6      // squad of four + two invaders
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

static void logf(const char *fmt, ...) {
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Test settings: test-settings.toml next to the DLL (the launcher writes it at every start from the
// file next to the launcher; the dev build copies $W/test-settings.toml). Read once, at load.
#include "settings.h"
static signed char g_settings[SETTINGS_COUNT];
static int setting(int i) { return g_settings[i] > 0; }
static void next_to_dll(char *path, size_t cap, const char *name) {
    snprintf(path, cap, "%s", log_path);
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
    if (dir) snprintf(dir + 1, cap - (size_t)(dir + 1 - path), "%s", name);
}
static int g_settings_early;  // read in DllMain already
static void settings_load(void) {
    char path[MAX_PATH], text[4096], bad[300], old[200] = "";
    next_to_dll(path, sizeof path, "test-settings.toml");
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(text, 1, sizeof text, f) : 0;
    if (f) fclose(f);
    if (settings_parse(text, n, g_settings, bad, sizeof bad)) logf("settings: lines not used: %s", bad);
    for (int i = 0; i < SETTINGS_COUNT; i++) {
        if (g_settings[i] < 0 || !f) g_settings[i] = g_settings[i] < 0 || !f ? (signed char)(-1 - setting_default(i)) : g_settings[i];
        int v = g_settings[i] < 0 ? -1 - g_settings[i] : g_settings[i];
        logf("settings: %s = %s (%s)", SETTING_NAMES[i], v ? "true" : "false",
             !f ? "no test-settings.toml next to the DLL, default" : g_settings[i] < 0 ? "not in the file, default" : path);
        g_settings[i] = (signed char)v;
    }
    for (int i = 0; i < SETTINGS_COUNT; i++)  // the empty marker files of earlier test builds
        for (int txt = 0; txt < 2; txt++) {
            char name[40], p[MAX_PATH];
            snprintf(name, sizeof name, "%s%s", SETTING_NAMES[i], txt ? ".txt" : "");
            next_to_dll(p, sizeof p, name);
            if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) snprintf(old + strlen(old), sizeof old - strlen(old), " %s", name);
        }
    if (old[0]) logf("settings: old marker files ignored (test-settings.toml replaces them):%s", old);
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
static unsigned invaders_allowed(unsigned squad) {
    static const unsigned table[] = {0, 1, 1, 2, 2};
    return squad < sizeof table / sizeof *table ? table[squad] : 2;
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
    g_host_test = setting(S_HOST_TEST);
    g_host_test_tick = 0;
    if (g_host_test) logf("host test: on: this expedition opens a host session");
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
static void hook_call(unsigned rva_site, unsigned rva_target, void *hook, const char *what) {
    unsigned char *site = (unsigned char *)g_base + rva_site;
    int expected = (int)(rva_target - (rva_site + 5));
    unsigned char *stub = near_alloc(12);
    if (site[0] != 0xE8 || memcmp(site + 1, &expected, 4) || !stub) {
        logf("%s: call site %x does not look as expected, not hooked", what, rva_site);
        return;
    }
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, hook; jmp rax
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 1, site + 5, stub)) logf("%s: hooked (%x)", what, rva_site);
    else logf("%s: cannot patch %x", what, rva_site);
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
#define FINISH_GUARD_MS 10000  // ponytail: a guess at death animation + fade; the bleed-out death was never timed
static char *g_hud_invaders[4];
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
    if (site == 0x7ced59 && (g_join_in_world() || (me && !IsBadReadPtr(me, 0x200) && our_speffect_active(me) == 1))) {
        char *fe = *(char **)(g_base + 0x3c14a70);
        if (fe && !IsBadReadPtr(fe + 0xcf28, 7 * 0x98))
            for (int i = 0; i < 7; i++) fe[0xcf28 + i * 0x98] = 0, fe[0xcf28 + i * 0x98 + 0x90] = 0;
    }
    char *out = c;
    // name-test (run 19, player tag on the invader): the tag list builder 0x7c8a90 looks up each of its 8
    // tag entries (FE +0xca60, 0x70 each; the handle at +0) here. One line per entry when its handle or the
    // lookup result changes: handle, found or not, team, our effect, and the entry's bytes +0x20..+0x38.
    if (site == 0x7c8c74 && setting(S_NAME_TEST) && id) {
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
    }
    // name-test, host: the over-head name tags (0x7c8a90 fills eight tags at FE +0xca60, lookup 0x7c8c74;
    // an empty lookup builds no name) are left out for a character carrying our effect; the name over
    // its health bar stays.
    // (run 15: an empty lookup here drops the whole tag, the HP bar with it: 0x7c8a90 fills a tag's HP,
    // name and render only for a found character. Not filtered any more; the over-head name comes back.)
    if (c && c != me && party) {
        int invading = g_join_in_world() || (me && !IsBadReadPtr(me, 0x200) && our_speffect_active(me) == 1);
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
// Once a second while our local player is down (this thread, reads only): the fields above.
static void finish_tick(void) {
    static DWORD next;
    char *chr = local_player();
    if (!chr || IsBadReadPtr(chr, 0x740) || (int)(GetTickCount() - next) < 0) return;
    if (chr[0x710] != 1 && !(g_finish_tick && chr == g_finish_chr)) return;
    next = GetTickCount() + 1000;
    logf("finish test: state %u gauge %.1f timer %.2f still-down %u stand-up %u down count %u%s", (unsigned char)chr[0x710],
         *(float *)(chr + 0x714), *(float *)(chr + 0x718), (unsigned char)chr[0x731], (unsigned char)chr[0x732],
         (unsigned char)chr[0x730], g_finish_tick && chr == g_finish_chr ? " (stand-up refused)" : "");
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
static int finish_side_ok(char *chr) {  // a down of this local character counts as by the other side
    return chr && chr == local_player() && (our_speffect_active(chr) == 1 || g_host_switched);
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
    logf("finish test: gauge call (0x43dac3): %s character, attacker %p (ours %d), gauge %.1f -> %.1f", chr == local_player() ? "local" : "remote",
         (void *)attacker, attacker && !IsBadReadPtr(attacker, 0x200) ? our_speffect_active(attacker) : -1, before,
         chr ? *(float *)(chr + 0x714) : -1);
    if (!chr || chr != local_player() || IsBadReadPtr(chr, 0x740) || chr[0x710] != 1 || *(float *)(chr + 0x714) > 0) return;
    int ok_attacker = attacker && attacker != chr && !IsBadReadPtr(attacker, 0x200);
    int by_other = our_speffect_active(chr) == 1 ? ok_attacker && our_speffect_active(attacker) != 1
                                                 : g_host_switched && ok_attacker && our_speffect_active(attacker) == 1;
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
static void marker_tick(void) {
    static DWORD next;
    static char last[2][0x60];
    char *m = g_view_models[0] ? g_view_models[0] : g_view_models[1];  // the map screen's model
    if (!m || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 2000;
    if (IsBadReadPtr(m, 0xc0) || *(char **)m != g_base + 0x2c4a548) {
        g_map_model = NULL;
        return;
    }
    char **b = *(char ***)(m + 0xb0), **e = *(char ***)(m + 0xb8);
    for (int i = 0; b && b + i < e && i < 2; i++) {
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
static int g_host_player_no = -1;
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
    int invader = g_join_in_world();
    if (!invader && !g_host_switched) return;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    char *mb = sm ? *(char **)(sm + SM_PLAYERS_BEGIN) : NULL, *me = sm ? *(char **)(sm + SM_PLAYERS_BEGIN + 8) : NULL;
    if (invader) host_player_no_update(mb, me);
    for (int k = 0; k < 3; k++) {
        char *m = g_view_models[k];
        if (!m || IsBadReadPtr(m, 0xc0) || *(char **)m != g_base + 0x2c4a548) continue;
        char **b = *(char ***)(m + 0xb0), **e = *(char ***)(m + 0xb8);
        for (int i = 0; b && b + i < e && i < 2; i++) {
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
            else
                for (char *x = mb; mb && x < me && !IsBadReadPtr(x, SM_PLAYER_SIZE); x += SM_PLAYER_SIZE)
                    if (!x[0xe1] && *(int *)(x + 0xd4) == no) hide = 1;
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
static void hook_acceptance(void) {
    if (setting(S_FINISH_TEST))
        for (unsigned i = 0; i < 4; i++) hook_call(CANHIT_SITES[i], 0x436370, (void *)can_hit_hook, "finish test: can-hit log");
    if (setting(S_MARKER_TEST)) {
        hook_call(0x84d8d8, 0x90dc80, (void *)map_model_hook, "marker test: world map");
        hook_call(0x84d91e, 0x90dc80, (void *)map_model_hook, "marker test: world map");
        hook_call(0xa4e3c0, 0x90dc80, (void *)map_model_hook, "marker test: world map");
    }
    if (setting(S_FINISH_TEST)) {
        for (unsigned i = 0; i < 5; i++) hook_call(RECV_SITES[i], 0x436530, (void *)recv_hit_hook, "finish test: hit receive log");
        hook_call(0x44c6ac, 0x66dec0, (void *)gauge_nohit_hook, "finish test: gauge call log");
    }
    if (setting(S_FINISH_TEST)) {
        hook_call(0x674f37, 0x66f3b0, (void *)finish_down_hook, "finish test: going down");
        hook_call(0x43dac3, 0x66dec0, (void *)finish_hit_hook, "finish test: gauge hit");
    }
    if (setting(S_LOOT_TEST)) {
        hook_call(0x6fd220, 0x724420, (void *)loot_update_hook, "loot test: pickup objects");
        hook_call(0x72464c, 0x7247d0, (void *)loot_pickup_hook, "loot test: pickup");
        hook_call(0x85d0b2, 0x9827d0, (void *)loot_dialog_hook, "loot test: equipment dialog");
    }
    if (setting(S_FINISH_TEST)) {
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
    // The game allows three players per session (set once, in the manager's constructor). Ours hold a
    // squad of up to four plus up to two invaders. Seamless Co-op raises the same field to six.
    if (*(unsigned *)(sm + SM_PLAYER_LIMIT) == 3) {
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
    int want = g_searching;
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

// --- "Number of Players": a fourth option --------------------------------------------------------
//
// The option is a CS::ComboItemList<MENU_MATCHING_SORTIE_NUM, 3>: a vtable, three 0x50-byte items
// stored inline at +8 and the item count at +0x100. There is no room for a fourth item inside it,
// but the menu reads the list through its virtual methods (1 = count, 2..5 = item by index). So we
// give the list a vtable of our own that reports one more item and serves it from this DLL.

#define RVA_COMBO_LIST_VTABLE     0x2c5efe0  // ComboItemList<MENU_MATCHING_SORTIE_NUM,3>
#define RVA_COMBO_LIST_VTABLE_LEA 0x9f1b7b   // "lea rax,[vtable]" in the dialog's constructor (7 bytes)
#define RVA_COMBO_ITEM_VTABLE     0x2c5ef88  // ComboItem<MENU_MATCHING_SORTIE_NUM>
#define RVA_TEXT_STRING_CTOR      0x7ac800   // string* (string *out, int text id): menu text by id
#define RVA_STRING_COPY_CTOR      0x5bea10   // void (string *dst, string *src)
#define COMBO_ITEMS   0x08
#define COMBO_ITEM_SIZE 0x50
#define COMBO_COUNT   0x100
#define TEXT_FOUR_PLAYERS 130460             // added to CL_MenuText by build_msg.py / the launcher

// The fourth item: vtable, value byte at +8, text at +0x10. Built once, on first use.
// Its value is one the game does not know (3); fill_context_hook turns it into a squad of four.
// The temporary string used to build it is never destroyed (one small leak per game run).
static char *fourth_item(void) {
    static _Alignas(16) char item[COMBO_ITEM_SIZE];
    static int ready;
    if (!ready) {
        static _Alignas(16) char tmp[0x100];
        void *text = ((void *(*)(void *, int))(g_base + RVA_TEXT_STRING_CTOR))(tmp, TEXT_FOUR_PLAYERS);
        *(void **)item = g_base + RVA_COMBO_ITEM_VTABLE;
        item[8] = 3;  // PLAYERS_CHOICE_FOUR
        ((void (*)(void *, void *))(g_base + RVA_STRING_COPY_CTOR))(item + 0x10, text);
        ready = 1;
        logf("players option: fourth item built");
    }
    return item;
}

static unsigned combo_count(char *list) {
    unsigned n = *(unsigned *)(list + COMBO_COUNT);
    return n == 3 ? 4 : n;  // only the full one/two/three list gets the extra entry
}

static char *combo_item(char *list, int index) {
    if (index == 3 && *(unsigned *)(list + COMBO_COUNT) == 3) return fourth_item();
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
static char *summon_queue(void) {
    char *menu = *(char **)(g_base + RVA_MENU_MAN_PTR);
    return menu ? menu + MENU_MAN_SUMMON_QUEUE : NULL;
}
static void notice(int which) {  // 0 searching, 1 cancelled
    static _Alignas(16) char text[2][0x100];  // built once and kept: the game never frees them for us
    static void *ready[2];
    if (!ready[which])
        ready[which] = ((void *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(
            text[which], which ? TEXT_SEARCH_CANCELLED : TEXT_SEARCHING);
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
#define LOBBY_PROTO      "1"             // bump when what a joiner expects changes
#define LOBBY_GAME_KEY   "nrpvp_game"
#define LOBBY_GAME       "1.3.3"         // GAME_TIMESTAMP's build
#define LOBBY_SQUAD_KEY  "nrpvp_squad"
#define LOBBY_FREE_KEY   "nrpvp_free"
#define LOBBY_BLOCK_KEY  "nrpvp_block"   // host's map block id, 8 hex digits (ChrIns+0x38)
#define LOBBY_POS_KEY    "nrpvp_pos"     // host's position "x,y,z" from the game's getter 0x3d0fd0
#define LOBBY_GEN_KEY    "nrpvp_gen"     // host's generator results gm +0xb48,+0xb54,+0xb5c (hex)
#define LOBBY_START_KEY  "nrpvp_start"   // host's expedition start block (game manager +0xa74), 8 hex digits
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
} g_diag;
static int steam_ready(void) {
    static int state;  // 0 not tried, 1 ready, -1 failed
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
    logf("steam: %s (matchmaking %p, utils %p)", state > 0 ? "ready" : "NOT available", g_steam.mm, g_steam.utils);
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
            logf("finish test (%s): other player %d (%s): downed state %d -> %d%s", side, i,
                 our_speffect_active(c) == 1 ? "the invader" : "not ours", last_[i], st, last_[i] == 1 && st == 0 ? " (died)" : "");
            last_[i] = st;
        }
    }
}
static void host_death_watch(void) {
    if (setting(S_FINISH_TEST)) other_down_watch("host");
    static int last = -1;
    char *chr = local_player();
    int now = chr && !IsBadReadPtr(chr, 0x740) ? (unsigned char)chr[0x710] : -1;
    if (setting(S_FINISH_TEST) && g_host_switched && last == 1 && now == 0) {
        ((void (*)(void))(g_base + 0x6af8f0))();
        char *gm = *(char **)(g_base + 0x3c13258);
        logf("finish test: local player died with an invader admitted: expedition end requested (gm+0xaf1 = %u)",
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
    static int last[2][2] = {{-9, -9}, {-9, -9}};
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 2000;
    char *ps = *(char **)(g_base + 0x3c078d0), *wcm = *(char **)(g_base + 0x3c0f0a8);
    char *slots = ps ? *(char **)(ps + 0x10) : NULL;
    if (!slots || !wcm || IsBadReadPtr(slots, 2 * 0x8a0)) return;
    for (int i = 0; i < 2; i++) {
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
    if (setting(S_NAME_TEST)) name_watch(sm, "host", g_host_switched), slot_hp_watch("host");
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
        g_squad = members;
        g_list_tick = GetTickCount();  // self-check one period after marking
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
    unsigned key = members | (unsigned)(in_lobby & 0xff) << 8;
    if (key == g_marked_squad) return;
    g_marked_squad = key;
    unsigned invaders = members > g_squad ? members - g_squad : 0;
    unsigned joining = in_lobby > (int)members ? (unsigned)in_lobby - members : 0, allowed = invaders_allowed(g_squad);
    unsigned free_slots = allowed > invaders + joining ? allowed - invaders - joining : 0;
    char text[16];
    snprintf(text, sizeof text, "%u", g_squad);
    lobby_set(LOBBY_SQUAD_KEY, text);
    snprintf(text, sizeof text, "%u", free_slots);
    lobby_set(LOBBY_FREE_KEY, text);
    logf("host: lobby members %d of %d; session members %u = squad %u + invaders %u, joining %u, free invader slots %u",
         in_lobby, g_steam.limit(g_steam.mm, g_lobby), members, g_squad, invaders, joining, free_slots);
}

// One lobby list request at a time, every SEARCH_PERIOD_MS: the invader every idle frame while
// searching, the host once (who = "host self-check"). Returns 1 when a request has finished.
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
        g_steam.string_filter(g_steam.mm, LOBBY_MARK_KEY, "1", LOBBY_EQUAL);
        g_steam.string_filter(g_steam.mm, LOBBY_PROTO_KEY, LOBBY_PROTO, LOBBY_EQUAL);
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
    u64 chosen = 0;
    for (unsigned i = 0; i < result.matching && i < 50; i++) {
        u64 id = g_steam.by_index(g_steam.mm, (int)i);
        const char *game = g_steam.get_data(g_steam.mm, id, LOBBY_GAME_KEY), *squad = g_steam.get_data(g_steam.mm, id, LOBBY_SQUAD_KEY);
        const char *free = g_steam.get_data(g_steam.mm, id, LOBBY_FREE_KEY), *proto = g_steam.get_data(g_steam.mm, id, LOBBY_PROTO_KEY);
        const char *block = g_steam.get_data(g_steam.mm, id, LOBBY_BLOCK_KEY), *pos = g_steam.get_data(g_steam.mm, id, LOBBY_POS_KEY);
        logf("%s:   lobby %llu: host block '%s' pos '%s'", who, id, block ? block : "?", pos ? pos : "?");
        int own = id == g_lobby && g_lobby, slots = free ? atoi(free) : 0;
        int fits = !own && slots > 0 && game && !strcmp(game, LOBBY_GAME);
        // GetLobbyData gives "" for a key that is not set.
        logf("%s:   lobby %llu: proto '%s' game '%s' squad '%s' free '%s', members %d of %d -> %s", who, id, proto ? proto : "?",
             game ? game : "?", squad ? squad : "?", free ? free : "?", g_steam.members(g_steam.mm, id),
             g_steam.limit(g_steam.mm, id), own ? "our own lobby" : fits ? "has a free invader slot" : "no slot for us");
        if (fits && !chosen) chosen = id;  // Steam sorts by distance: the first that fits
    }
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
    g_join_test = setting(S_JOIN_TEST);
    g_enter_test = g_join_test && setting(S_ENTER_TEST);
    g_move_test = g_enter_test && setting(S_MOVE_TEST);
    if (setting(S_MOVE_TEST) && !g_enter_test) logf("move test: needs join-test and enter-test = true as well; off");
    if (g_move_test) logf("move test: on: at EndLobbyExit the controller is sent to MoveMapStart");
    if (setting(S_ENTER_TEST) && !g_join_test) logf("enter test: needs join-test = true as well; off");
    if (g_enter_test) logf("enter test: on: a client stays and the way into the host's world is watched");
    if (g_join_test) logf("join test: on: the first candidate is joined; controller +0x10d %d",
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
static void join_over(const char *why) {
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
        if (setting(S_REWARD_TEST)) {
            // reward-test: the invader's play state holds no result of this session (it never ran the
            // expedition's own start), and the second end screen (rewards, 05_110_Reward) came out half
            // drawn. STEP_Result (0xc29340) builds the reward chain only when the session result id
            // (play state +0x368) is not 0: clearing it skips that screen; the summary stays.
            char *ps = *(char **)(g_base + 0x3c078d0);
            if (ps) {
                logf("reward test: session result id (play state +0x368) %u -> 0: the reward screen is skipped", *(unsigned *)(ps + 0x368));
                *(unsigned *)(ps + 0x368) = 0;
            }
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
        if (state == 6) {
            g_join = 2;
            g_join_tick = GetTickCount();
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
#define TEAM_TEST_START_MS 90000
#define TEAM_TEST_LEN_MS   180000
static int g_team_test;        // 0 off, 1 waiting, 2 applied, 3 over
static DWORD g_team_tick, g_team_next;
static int g_team_phase;
static char *local_player(void) {
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    return wcm ? *(char **)(wcm + WCM_LOCAL_PLAYER) : NULL;
}
static int our_speffect_active(char *chr) {
    struct { char *row; unsigned id; unsigned char flag; } ref = {0};
    ((void (*)(void *, unsigned))(g_base + RVA_SPEFFECT_ROW))(&ref, TEAM_TEST_SPEFFECT);
    char *module = *(char **)(chr + CHR_SPEFFECTS);
    if (!ref.row) return -1;  // row not in regulation.bin
    if (!module) return 0;
    int n = 0;
    for (char **e = *(char ***)(module + 8); e && n < 512; e = (char **)e[7], n++)
        if (e[0] == ref.row && !(*(unsigned *)((char *)e + 0x68) & 0x800c0003)) return 1;
    return 0;
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
    if (g_move_test && setting(S_FULL_SYNC) && sync && now[0] == 6 && now[2] >= 0 && now[2] <= 6 && !sync[0xe5]) {
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
    if (g_enter_effect && chr && !reapplied && nowt - applied < 5000 && nowt - applied > 300 && our_speffect_active(chr) == 0) {
        reapplied = 1;
        ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, TEAM_TEST_SPEFFECT, 0);
        applied = nowt;
        team_log("enter test, team effect lost right after the sync: put on again");
    }
    if (g_enter_world && chr && !g_enter_effect) {
        team_log("enter test, before the team effect");
        ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, TEAM_TEST_SPEFFECT, 0);
        g_enter_effect = 1;
        applied = nowt;
        check_at = nowt + 1000;
    }
    if (check_at && (int)(nowt - check_at) >= 0 && chr) {
        check_at = 0;
        team_log("enter test, 1 s after the team effect");
    }
    if (setting(S_FINISH_TEST)) other_down_watch("invader");
    if (setting(S_NAME_TEST)) name_watch(sm, "invader", g_enter_world != 0), slot_hp_watch("invader");
    // finish-test guard: the stand-up was refused and the timer set to 0, but the character is still
    // down 10 s later (the behaviour did not take its death): the invader counts as dead.
    if (g_enter_effect && chr && chr == g_finish_chr && g_finish_invader && g_finish_tick && chr[0x710] == 1 &&
        nowt - g_finish_tick > FINISH_GUARD_MS) {
        logf("finish test: still down %u s after the refused stand-up: the invader counts as dead", FINISH_GUARD_MS / 1000);
        g_finish_tick = 0, g_finish_chr = NULL;
        g_invasion_ended = 1;
        join_over("invader died (finished while down)");
        return;
    }
    if (g_enter_effect && chr && nowt - applied > 5000 && our_speffect_active(chr) == 0) {
        team_log("enter test, team effect gone");
        g_invasion_ended = 1;
        join_over("invader died (team effect removed)");
        return;
    }
}
// Death while on our team (decision 3: a dead invader leaves the session). The effect goes by rule:
// our row keeps dontDeleteOnDead at 0 (SpEffectParam field per Smithbox's paramdef; the code that
// honours it is not read). The solo test showed it gone well before its 180 s. Signal used here:
// our effect active one frame and gone the next while its time is not up; the next 15 s are logged
// to show what the game does (respawn, team back to base, character instance kept or replaced).
// ponytail: a proxy for death; HP / a death flag on the character are not located yet.
static DWORD g_team_position, g_team_death;
static int g_team_was_active;
static char *g_team_chr;
static void team_death_watch(DWORD t) {
    char *chr = local_player();
    int active = chr ? our_speffect_active(chr) == 1 : 0;
    if (g_team_was_active && !active && !g_team_death && t + 2000 < TEAM_TEST_START_MS + TEAM_TEST_LEN_MS) {
        g_team_death = t;
        logf("team test: our effect ended early at +%lums (%lus before its time; death?), character %p (was %p)", t,
             (TEAM_TEST_START_MS + TEAM_TEST_LEN_MS - t) / 1000, (void *)chr, (void *)g_team_chr);
        team_log("at the early end");
        log_position("team test");
    }
    static int phase;
    if (!g_team_death) phase = 0;
    else if (phase == 0 && t - g_team_death >= 5000) {
        team_log("5 s after the early end");
        phase = 1;
    } else if (phase == 1 && t - g_team_death >= 15000) {
        team_log("15 s after the early end");
        logf("team test: character now %p", (void *)local_player());
        phase = 2;
    }
    g_team_was_active = active;
    if (active) g_team_chr = chr;
}
// InGame step, every frame. first: the first frame of this expedition's world.
static void team_test_frame(int first) {
    if (first) {
        g_team_test = 0;
        if (!setting(S_TEAM_TEST)) return;
        g_team_test = 1;
        g_team_tick = GetTickCount();
        g_team_position = 0;
        g_team_was_active = 0;
        g_team_death = 0;
        logf("team test: on: team 16 from +%us to +%us", TEAM_TEST_START_MS / 1000,
             (TEAM_TEST_START_MS + TEAM_TEST_LEN_MS) / 1000);
        team_log("start");
        return;
    }
    if (!g_team_test) return;
    DWORD t = GetTickCount() - g_team_tick;
    if (t >= g_team_position) {
        log_position("team test");
        g_team_position = t + 10000;
    }
    if (g_team_test == 2) team_death_watch(t);
    if (g_team_test == 3) return;
    if (g_team_test == 1 && t >= TEAM_TEST_START_MS) {
        char *chr = local_player();
        if (!chr) return;  // try again next frame
        team_log("before apply");
        ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, TEAM_TEST_SPEFFECT, 0);
        logf("team test: add SpEffect %d to the local player called", TEAM_TEST_SPEFFECT);
        team_log("after apply");
        g_team_test = 2;
        g_team_phase = 0;
        g_team_next = t + 5000;
        return;
    }
    if (g_team_test == 2 && t >= g_team_next) {  // phases: 5 s after apply, effect's end, 5 s after
        static const char *const what[] = {"5 s after apply", "effect due to end", "5 s after the end"};
        team_log(what[g_team_phase]);
        g_team_phase++;
        if (g_team_phase == 1) g_team_next = TEAM_TEST_START_MS + TEAM_TEST_LEN_MS;
        else if (g_team_phase == 2) g_team_next += 5000;
        else g_team_test = 3;
    }
}

// --- Slow clock (test sessions), only with slow-time = true in the test settings -----------------
//
// The expedition's clock: singleton 0x3c125d8 (created in 0x62ed40, set up by 0x65a9c0 / 0x65af70).
// +0x08 time of day, packed (hour bits 34-38, minute 39-44, second 45-50, as 0x602f80 unpacks it);
// +0x34 base rate, from GameSystemCommonParam row 0 +0x1bc (72.9), or the debug property
// Game.IngameTime.TimeScale when it is above 0 (0xf884b0, read once, at set-up); +0x38 the rate
// the update 0x65c1d0 advances the time with; +0x4b "running". A client's rate is pulled toward
// the host's time (0x65c750: every 15 s from the host, a jump when 120 s apart), so the host's
// rate is the one that counts. With slow-time on this sets both rates to a tenth of the base,
// once per expedition, unless the base is already below 10 (the profile's TimeScale arrived).
// Every 30 s it logs the time of day and the rates, so the log shows how fast the day goes.
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
static int g_slow_time;
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
    logf("clock: %s: circle clock %02u:%02u:%02u, base rate %.3f, rate %.3f", when, (unsigned)(v >> 34) & 0x1f,
         (unsigned)(v >> 39) & 0x3f, (unsigned)(v >> 45) & 0x3f, *(float *)(pa + PA_BASE_RATE), *(float *)(pa + PA_RATE));
}
static void slow_time_frame(int first) {
    if (first) {
        g_clock_tick = GetTickCount();
        g_clock_next = 0;
        g_slow_time = setting(S_SLOW_TIME);
        clock_log(g_slow_time ? "expedition start (slow-time on)" : "expedition start");
        char *clock = *(char **)(g_base + RVA_CLOCK_PTR);
        if (g_slow_time && clock) {
            float base = *(float *)(clock + CLOCK_BASE_RATE);
            if (base >= 10.0f) {
                *(float *)(clock + CLOCK_BASE_RATE) = base / 10.0f;
                *(float *)(clock + CLOCK_RATE) = base / 10.0f;
                clock_log("rate set to a tenth");
            } else logf("clock: base rate %.3f already slow (debug property arrived), left as it is", base);
        }
        char *pa = *(char **)(g_base + RVA_PLAY_AREA_PTR);
        if (g_slow_time && pa) {
            float base = *(float *)(pa + PA_BASE_RATE);
            if (base >= 10.0f) {
                *(float *)(pa + PA_BASE_RATE) = base / 10.0f;
                *(float *)(pa + PA_RATE) = base / 10.0f;
                clock_log("circle clock rate set to a tenth");
            } else logf("clock: circle clock base rate %.3f already slow (debug property arrived), left as it is", base);
        }
        g_clock_next = 30000;
        return;
    }
    if (GetTickCount() - g_clock_tick >= g_clock_next) {
        clock_log("every 30 s");
        g_clock_next += 30000;
    }
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
static int g_in_expedition;
static DWORD g_expedition_tick;
static void host_test_create(void);
static int host_test_leave(void);
static void host_expedition(int step) {
    if (step == QM_STEP_INGAME) {
        if (!g_in_expedition) {
            team_test_frame(1);
            slow_time_frame(1);
            g_in_expedition = 1;
            g_expedition_tick = GetTickCount();
            g_self_check = 0;
            lobby_search_reset();
            host_test_start();
            logf("host: expedition reached its world%s", g_host_test ? ", opening a host session" : " (no host session: host-test is off)");
            if (g_host_test) {
                if (g_stop_pending) finish_pending_stop(1);
                if (!g_network_on) set_network(1);
                if (!g_network_on) {
                    logf("host: network not started, no host session this expedition");
                    g_host_test = 3;
                }
            }
        }
        team_test_frame(0);
        slow_time_frame(0);
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
            if (g_lobby && !g_self_check && lobby_search_frame("host self-check")) g_self_check = 1;
        }
    } else if (g_in_expedition) {
        g_in_expedition = 0;
        lobby_search_reset();
        logf("host: expedition over (step %s)%s", STEP_NAMES[step], g_host_test == 2 ? ", leaving the session" : "");
        if (host_test_leave()) g_stop_pending = GetTickCount();  // network down once the leave is through
        else if (g_network_on && !g_searching) set_network(0);
        g_lobby = 0;
    }
}

static void toggle_search(const char *where) {
    g_invasion_pending = 0;
    g_searching = !g_searching;
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
#define QM_STEP_MOVE_MAP_START 21
#define RVA_SET_ONLINE_MODE   0x6b0530  // gm+0xb40 = arg
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
    if (setting(S_MARKER_TEST)) marker_clear();
    static int last_seen_start = -1;
    if (step == QM_STEP_NONE) last_seen_start = -1;
    static int last = -1;
    g_qm_ctrl = ctrl;
    g_qm_step = step;
    if (step != last) {
        if (step != QM_STEP_NONE) log_start(ctrl, step);
        else logf("step None");
        last = step;
    }
    if (g_network_on && g_guard_broken) {
        if (g_searching) toggle_search("network guard broken");
        else {
            logf("network: guard broken, taking the network down");
            set_network(0);
        }
    }
    if (g_join != 4) host_expedition(step);  // an enter-test client opens no host session of its own
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
        if (g_invasion_pending && GetTickCount() - g_request_tick > 300) toggle_search("machine stayed idle");
        if (g_searching && notices_idle()) notice(0);
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
// at +0x49 (0 three, 1 two, 2 one; 3 is our fourth option) and the chosen Target rows as a u32
// vector at +0x58 (begin) / +0x60 (end). The function turns rows into boss numbers, where our row
// becomes boss 0, and the choice into a player count.
#define RVA_FILL_CONTEXT       0xb67cc0
#define RVA_FILL_CONTEXT_CALL1 0xb67199  // e8 22 0b 00 00
#define RVA_FILL_CONTEXT_CALL2 0xb671e0  // e8 db 0a 00 00
#define REQ_PLAYERS_CHOICE     0x49
#define REQ_ROWS_BEGIN         0x58
#define PLAYERS_CHOICE_FOUR    3         // value of our combo item
static char *g_dialog;                       // defined with the start button code below
static int cursor_on_invasion(char *dialog);
static unsigned long long fill_context_hook(char *self, char *request) {
    unsigned *begin = *(unsigned **)(request + REQ_ROWS_BEGIN), *end = *(unsigned **)(request + REQ_ROWS_BEGIN + 8);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    // Same rule as the button's label: the entry under the cursor. If the dialog cannot be read,
    // fall back on the chosen rows, where the game itself looks at the last one for its special row.
    int invasion = cursor_on_invasion(g_dialog);
    if (invasion < 0) invasion = count && count <= 64 && begin[count - 1] == INVASION_ROW;
    // Four players: let the game set everything up as for its largest squad, then correct the count.
    int four = (unsigned char)request[REQ_PLAYERS_CHOICE] == PLAYERS_CHOICE_FOUR;
    if (four) request[REQ_PLAYERS_CHOICE] = 0;
    unsigned long long result = ((unsigned long long (*)(char *, char *))(g_base + RVA_FILL_CONTEXT))(self, request);
    if (four) {
        request[REQ_PLAYERS_CHOICE] = PLAYERS_CHOICE_FOUR;
        char *ctx = *(char **)(self + 8);
        if (ctx && *(char **)ctx == g_base + RVA_QM_CONTEXT_VTABLE) *(unsigned *)(ctx + QM_CTX_PLAYERS) = 4;
    }
    g_invasion_pending = invasion;
    g_request_tick = GetTickCount();
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

// Who writes lobby_state (session manager +0xc). Run 11: on a repeated join the joiner went to 7 with
// no logged call of the leave 0xdf1180 from any of its 44 call sites or 2 tail jumps. A hardware write
// breakpoint (debug register 0, 4 bytes, writes) on the field, armed from this thread on every other
// thread of the game while our network is up (again every 5 s, for threads started since), names
// the writer whatever path it takes. The handler only records (no file or heap use there); this
// thread writes the lines: the code address after the write, the value, the thread, and the stack
// words that point into the game's code (return addresses among them, nearest first).
#define LS_RING 32
static struct { DWORD tick, tid; unsigned value; unsigned long long rip; unsigned n; unsigned long long st[14]; } g_ls_ring[LS_RING];
static volatile LONG g_ls_head, g_ls_tail;
static volatile char *g_ls_field;
static LONG CALLBACK ls_watch_handler(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 &= ~0xfull;
    LONG slot = InterlockedIncrement(&g_ls_head) - 1;
    if (slot - g_ls_tail < LS_RING) {
        int i = slot % LS_RING;
        g_ls_ring[i].tick = GetTickCount(), g_ls_ring[i].tid = GetCurrentThreadId();
        g_ls_ring[i].value = g_ls_field ? *(volatile unsigned *)g_ls_field : 0;
        g_ls_ring[i].rip = (unsigned long long)((char *)e->ContextRecord->Rip - g_base);
        unsigned long long *sp = (unsigned long long *)e->ContextRecord->Rsp;
        unsigned n = 0;
        for (int k = 0; k < 160 && n < 14; k++) {
            char *p = (char *)sp[k];
            if (p > g_base + 0x1000 && p < g_base + 0x2c00000) g_ls_ring[i].st[n++] = (unsigned long long)(p - g_base);
        }
        g_ls_ring[i].n = n;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void ls_watch_flush(void) {
    while (g_ls_tail < g_ls_head) {
        if (g_ls_head - g_ls_tail > LS_RING) {
            logf("lobby_state watch: %ld writes not recorded (ring full)", g_ls_head - g_ls_tail - LS_RING);
            g_ls_tail = g_ls_head - LS_RING;
        }
        int i = g_ls_tail % LS_RING;
        char chain[14 * 9 + 1] = "";
        for (unsigned k = 0; k < g_ls_ring[i].n; k++)
            snprintf(chain + strlen(chain), sizeof chain - strlen(chain), "%llx ", g_ls_ring[i].st[k]);
        logf("lobby_state watch: written, now %u, by code before %llx (thread %lu, +%lums since the join); code addresses on the stack: %s",
             g_ls_ring[i].value, g_ls_ring[i].rip, g_ls_ring[i].tid, g_ls_ring[i].tick - g_join_tick, chain);
        InterlockedIncrement(&g_ls_tail);
    }
}
static void ls_watch_arm(char *sm) {  // this thread only: it suspends the others one at a time
    static PVOID handler;
    static int last_count = -1;
    if (!handler) handler = AddVectoredExceptionHandler(1, ls_watch_handler);
    if (!handler) return;
    g_ls_field = sm + SM_LOBBY_STATE;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {.dwSize = sizeof te};
    int armed = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
            if (GetThreadContext(th, &c)) {
                c.Dr0 = (DWORD64)g_ls_field;
                c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);  // breakpoint 0: write, 4 bytes
                armed += SetThreadContext(th, &c) != 0;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    if (armed != last_count) logf("lobby_state watch: write breakpoint on %p armed on %d threads", (void *)g_ls_field, armed);
    last_count = armed;
}
static void ls_watch_tick(void) {
    static DWORD next;
    ls_watch_flush();
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!sm || !g_network_on || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 5000;
    ls_watch_arm(sm);
}
static DWORD WINAPI run(LPVOID self) {
    (void)self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP) {
        logf("unsupported game build (timestamp %08lx), doing nothing", nt->FileHeader.TimeDateStamp);
        return 0;
    }
    logf("loaded, game base %p", g_base);
    if (!g_settings_early) settings_load();
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
        hook_expedition_start();
        hook_offline_check();
        hook_network_flow();
        hook_button_text();
        hook_eac_begin();
        hook_acceptance();
    }
    for (;;) {
        Sleep(250);
        guard_intact();
        watch_network();
        watch_session();
        if (setting(S_WATCH_TEST)) ls_watch_tick();
        code_patch_check();
        if (setting(S_MARKER_TEST)) marker_tick();
        if (setting(S_FINISH_TEST)) finish_tick();
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        // Log next to the DLL, so it is easy to find from outside the game.
        GetModuleFileNameA(self, log_path, MAX_PATH);
        char *dot = strrchr(log_path, '.');
        if (dot) strcpy(dot, ".log");
        g_base = (char *)GetModuleHandleA(NULL);
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
            if (setting(S_FINISH_TEST)) finish_relation_patch();
        }
        CreateThread(NULL, 0, run, self, 0, NULL);
    }
    return TRUE;
}

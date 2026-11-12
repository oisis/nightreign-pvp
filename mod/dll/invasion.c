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
static void host_test_start(void) {
    g_lobby = 0;
    g_marked_squad = ~0u;
    char path[MAX_PATH];
    strcpy(path, log_path);
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
    if (!dir) return;
    strcpy(dir + 1, "host-test");
    g_host_test = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    g_host_test_tick = 0;
    if (g_host_test) logf("host test: on (%s): this expedition opens a host session", path);
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
        logf("host: squad of %u, up to %u invaders allowed", players, invaders_allowed(players));
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

static void show_boss_list_hook(void *grid, char *list, void *a3, void *a4) {
    preselect_invasion(grid);
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
static int patch_rel32(unsigned char *field, unsigned char *next, unsigned char *target) {
    int rel = (int)(target - next);
    DWORD old;
    if (!VirtualProtect(field, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(field, &rel, 4);
    VirtualProtect(field, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), field, 4);
    return 1;
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
    return state > 0;
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
static int g_join_test;    // invader: "join-test" file present when the search started
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
        g_squad = members;
        g_list_tick = GetTickCount();  // self-check one period after marking
    }
    // Squad: the session's members when the lobby was first marked (the expedition's own players are
    // all in it by InGame). Session members beyond it are invaders; Steam lobby members beyond the
    // session's are someone in the middle of joining, and hold a slot too.
    // ponytail: a squad member who rejoins after a drop-out would count as an invader.
    int in_lobby = g_steam.members(g_steam.mm, g_lobby);
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
        int own = id == g_lobby && g_lobby, slots = free ? atoi(free) : 0;
        int fits = !own && slots > 0 && game && !strcmp(game, LOBBY_GAME);
        // GetLobbyData gives "" for a key that is not set.
        logf("%s:   lobby %llu: proto '%s' game '%s' squad '%s' free '%s', members %d of %d -> %s", who, id, proto ? proto : "?",
             game ? game : "?", squad ? squad : "?", free ? free : "?", g_steam.members(g_steam.mm, id),
             g_steam.limit(g_steam.mm, id), own ? "our own lobby" : fits ? "has a free invader slot" : "no slot for us");
        if (fits && !chosen) chosen = id;  // Steam sorts by distance: the first that fits
    }
    if (chosen) logf("%s: %s lobby %llu", who, g_join_test ? "candidate" : "would join (no join-test file)", chosen);
    else logf("%s: no lobby to join", who);
    g_candidate = chosen;
    return 1;
}

// --- Join test (invader side), only with a "join-test" file next to the DLL -----------------------
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
static void join_test_start(void) {
    char path[MAX_PATH];
    strcpy(path, log_path);
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
    g_join_test = 0;
    g_join = 0;
    g_candidate = 0;
    if (!dir) return;
    strcpy(dir + 1, "join-test");
    g_join_test = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    if (g_join_test) logf("join test: on (%s): the first candidate is joined", path);
}
// Every frame while joining: one line per change of the session's state, with ms since the call.
static void join_watch(char *sm) {
    static unsigned last[6];
    static unsigned long long last_end;
    static DWORD logged_for;  // g_join_tick of the join last logged: a new join always gets a first line
    char *begin = *(char **)(sm + SM_PLAYERS_BEGIN), *end = *(char **)(sm + SM_PLAYERS_BEGIN + 8);
    unsigned now[6] = {*(unsigned *)(sm + SM_LOBBY_STATE), *(unsigned *)(sm + SM_HANDLE), *(unsigned *)(sm + SM_JOIN_HANDLE),
                       *(unsigned *)(sm + 0x10), begin && end >= begin ? (unsigned)((end - begin) / SM_PLAYER_SIZE) : 0,
                       (unsigned char)sm[SM_LEAVING]};
    unsigned long long end_code = *(unsigned long long *)(sm + SM_END_CODE);
    if (!memcmp(now, last, sizeof now) && end_code == last_end && logged_for == g_join_tick) return;
    logged_for = g_join_tick;
    int members_changed = now[4] != last[4];
    memcpy(last, now, sizeof now);
    last_end = end_code;
    char *eac = *(char **)(g_base + RVA_EAC_MAN_PTR);
    logf("join test: +%lums lobby_state %u handle %u join %u role %u players %u leaving %u end_code %llx | eac error %u (0x%x)",
         GetTickCount() - g_join_tick, now[0], now[1], now[2], now[3], now[4], now[5], end_code,
         eac ? *(unsigned *)(eac + EAC_ERROR) : 0, eac ? *(unsigned *)(eac + EAC_ERROR) : 0);
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
static void toggle_search(const char *where);
static void join_over(const char *why) {
    g_join = 3;
    logf("join test: stopping (%s), leaving and ending the search", why);
    toggle_search("join test over");
}
// Idle step, every frame while searching with the join test on.
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
    DWORD waited = GetTickCount() - g_join_tick;
    if (g_join == 1) {
        if (state == 6) {
            g_join = 2;
            g_join_tick = GetTickCount();
            logf("join test: joined as client after %lums. Entering the host's world is not built: holding %ums, then leaving",
                 waited, JOIN_HOLD_MS);
            log_members(sm, "join test");
            log_pending_state("join test");
        } else if (state == 0 || state == 2 || state == 5) join_over("join failed");
        else if (waited > JOIN_TIMEOUT_MS) join_over("join timed out");
    } else if (state != 6) join_over("no longer a client");
    else if (waited > JOIN_HOLD_MS) join_over("hold time over");
}
// Leave a session we joined. Returns 1 if a leave was requested (the network then waits for it).
static int join_leave(void) {
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!g_join_test || !sm) return 0;
    unsigned state = *(unsigned *)(sm + SM_LOBBY_STATE);
    if (state != 4 && state != 6) return 0;
    ((void (*)(void *, unsigned char))(g_base + RVA_SESSION_LEAVE))(sm, 0);
    logf("join test: leave session, lobby_state %u -> %u", state, *(unsigned *)(sm + SM_LOBBY_STATE));
    return 1;
}

// --- Team test, only with a "team-test" file next to the DLL ------------------------------------
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
// Where the host is (decision 14), not settled: which fields hold the local player's map block and
// position is not read in this executable. ELDEN RING's ChrIns (fromsoftware-rs, read only) has the
// block ids at +0x30..+0x3f and the position in map chunk coordinates at +0x80; in Nightreign the
// team byte sits 8 bytes later than ELDEN RING's (+0x74 vs +0x6c), so the same fields may sit at
// +0x38.. and +0x88/+0x90. The team test logs these candidates (and the game manager's +0xa70 /
// +0xa74) every 10 s, so walking around shows which ones follow the player. Reads only.
static void log_position_candidates(void) {
    char *chr = local_player(), *gm = *(char **)(g_base + RVA_GAME_MAN_PTR);
    if (!chr || IsBadReadPtr(chr, 0xb0)) return;
    float *a = (float *)(chr + 0x80), *b = (float *)(chr + 0x88), *c = (float *)(chr + 0x90);
    logf("team test: position candidates: chr+38 %08x +3c %08x +40 %08x +44 %08x | +80 %.1f %.1f %.1f | +88 %.1f %.1f %.1f | "
         "+90 %.1f %.1f %.1f | gm+a70 %08x +a74 %08x", *(unsigned *)(chr + 0x38), *(unsigned *)(chr + 0x3c),
         *(unsigned *)(chr + 0x40), *(unsigned *)(chr + 0x44), a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2],
         gm ? *(unsigned *)(gm + 0xa70) : 0, gm ? *(unsigned *)(gm + 0xa74) : 0);
}
// InGame step, every frame. first: the first frame of this expedition's world.
static void team_test_frame(int first) {
    if (first) {
        char path[MAX_PATH];
        strcpy(path, log_path);
        char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
        g_team_test = 0;
        if (!dir) return;
        strcpy(dir + 1, "team-test");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return;
        g_team_test = 1;
        g_team_tick = GetTickCount();
        logf("team test: on (%s): team 16 from +%us to +%us", path, TEAM_TEST_START_MS / 1000,
             (TEAM_TEST_START_MS + TEAM_TEST_LEN_MS) / 1000);
        team_log("start");
        return;
    }
    if (!g_team_test) return;
    DWORD t = GetTickCount() - g_team_tick;
    static DWORD next_position;
    if (t < 1000) next_position = 0;
    if (t >= next_position) {
        log_position_candidates();
        next_position = t + 10000;
    }
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
            g_in_expedition = 1;
            g_expedition_tick = GetTickCount();
            g_self_check = 0;
            lobby_search_reset();
            host_test_start();
            logf("host: expedition reached its world%s", g_host_test ? ", opening a host session" : " (no host session: no host-test file)");
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
        if (g_host_test == 1) {
            host_test_create();
            if (g_host_test == 1 && g_expedition_tick && GetTickCount() - g_expedition_tick > NET_READY_WAIT_MS) {
                logf("host: network still not ready after %lums, still waiting", GetTickCount() - g_expedition_tick);
                g_expedition_tick = 0;
            }
        }
        if (g_host_test == 2) {
            lobby_mark();
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
    notice(g_searching ? 0 : 1);
    logf("invasion: search %s (%s)", g_searching ? "started" : "called off", where);
}

// Every step of the machine runs through here (step: its index in the table).
// With one player a request moves the machine to SingleStart at once, and an Invasion request is
// stopped there. With more players the machine stays idle offline (the game waits for its own
// matchmaking), so an Invasion request still pending a moment later is taken up from the idle step.
static void step_hook(void *ctrl, void *time, int step) {
    static int last = -1;
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
    host_expedition(step);
    if (g_host_test == 2 || g_stop_pending) host_test_watch();
    if (g_stop_pending) finish_pending_stop(0);
    if (step != QM_STEP_NONE) {
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
        if (g_searching) lobby_search_frame("search");
        if (g_searching && g_join_test) join_frame();
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

static DWORD WINAPI run(LPVOID self) {
    (void)self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP) {
        logf("unsupported game build (timestamp %08lx), doing nothing", nt->FileHeader.TimeDateStamp);
        return 0;
    }
    logf("loaded, game base %p", g_base);
    if (g_guard_ok) logf("net guard: installed, %u of %u imports patched (outbound %s)", (unsigned)GUARDED_COUNT,
                         (unsigned)GUARDED_COUNT, NET_GUARD_REFUSE_OUTBOUND ? "refused" : "logged only");
    else logf("net guard: NOT installed (%s); the network will not be started", g_guard_msg);
    // Seamless Co-op changes the same menu and lobby code. Next to it we only watch and log.
    if (GetModuleHandleA("nrsc.dll")) {
        logf("Seamless Co-op is loaded: observing only, no menu changes");
    } else {
        hook_target_list();
        hook_players_option();
        hook_expedition_start();
        hook_offline_check();
        hook_network_flow();
        hook_button_text();
        hook_eac_begin();
    }
    for (;;) {
        Sleep(250);
        guard_intact();
        watch_network();
        watch_session();
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
        if (nt->FileHeader.TimeDateStamp == GAME_TIMESTAMP) install_net_guard();
        CreateThread(NULL, 0, run, self, 0, NULL);
    }
    return TRUE;
}

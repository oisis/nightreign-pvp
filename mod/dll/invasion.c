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
//     "searching" state, with a small repeating notice. Nothing is searched for yet.
//
// ponytail: addresses are hard-coded for nightreign.exe 1.3.3 (PE timestamp below) and the DLL does
// nothing on any other build. Replace with signature scans when a second game version has to work.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
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
// Host test (experiment): open a session as host on the network we started, once per search.
// Active only while a file named "host-test" lies next to the DLL; looked for when a search starts.
//
// Create: 0xdefbd0, the game's own create-session (SessionCreateJob's lambda 0xb1ab00 calls it with
// the job's multiplay type and a pointer to a byte 1). It refuses while lobby_state is 1, 3, 4 or 6,
// builds the session's ciphers (0xdf6e70, protected), fills the parameters (0xdf4830: player limit
// from +0x170) and calls the session implementation's create (0x25846a0), which needs the started
// counter at impl+0x10 and starts a Steam session object; that object's step 0x2591950 calls
// ISteamMatchmaking::CreateLobby. Success: lobby_state 1, then 3 (host) once Steam has made the lobby.
// Leave: 0xdf1180(manager, 0), the way SessionLeaveJob (0xb1aa90) and CSQuickMatchingCtrl
// (0xb59332, 0xb5944f) leave: lobby_state 7 and an asynchronous leave in the implementation.
// ponytail: the network is stopped right after the leave request, without waiting for lobby_state 0
// as the game's leave job does; wait for it in the idle step if the log shows the lobby left hanging.
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
    memcpy(last, now, sizeof now);
    last_end = end_code;
    logf("host test: +%lums lobby_state %u handle %u create %u role %u players %u end_code %llx | eac %s, platform %s, flags %u, error %u (0x%x)",
         GetTickCount() - g_host_test_tick, now[0], now[1], now[2], now[3], now[4], end_code, eac ? "present" : "missing",
         eac && *(void **)(eac + EAC_PLATFORM) ? "set" : "null", now[5], now[6], now[6]);
}
static void host_test_start(void) {
    char path[MAX_PATH];
    strcpy(path, log_path);
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/'), *dir = a > b ? a : b;
    if (!dir) return;
    strcpy(dir + 1, "host-test");
    g_host_test = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    g_host_test_tick = 0;
    if (g_host_test) logf("host test: on (%s)", path);
}
// Idle step, every frame while searching: create once the network is ready.
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
// LEAVE_WAIT_MS. ponytail: an ordinary expedition starting stops it at once (no frames to wait in).
#define LEAVE_WAIT_MS 5000
static void set_network(int on);
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
// The search brings the game's network up and its end takes it down again.
// ponytail: nothing is searched for yet; the matchmaking call goes where the search starts.
static int g_searching;
static DWORD g_request_tick;  // when the last start request arrived

static void toggle_search(const char *where) {
    g_invasion_pending = 0;
    g_searching = !g_searching;
    if (g_searching) {
        if (g_stop_pending) finish_pending_stop(1);
        host_test_start();
        set_network(1);
    } else if (guard_intact() && host_test_leave()) {
        g_stop_pending = GetTickCount();  // the network goes down once the leave is through
    } else set_network(0);
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
    if (step != QM_STEP_NONE) {
        if (g_invasion_pending) {
            void **vt = *(void ***)ctrl;
            ((void (*)(void *, int))vt[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_NONE);
            toggle_search("expedition start cancelled");
            return;
        }
        if (g_stop_pending) finish_pending_stop(1);
        if (g_searching) {
            g_searching = 0;
            set_network(0);
            clear_notices();
            logf("invasion: search ended, an ordinary expedition is starting");
        }
    } else {  // the idle step runs every frame in the Roundtable Hold
        if (g_invasion_pending && GetTickCount() - g_request_tick > 300) toggle_search("machine stayed idle");
        if (g_searching && notices_idle()) notice(0);
        if (g_network_on && g_guard_broken) {
            if (g_searching) toggle_search("network guard broken");
            else finish_pending_stop(1);
        }
        if (g_searching) host_test_create();
        if ((g_searching && g_host_test == 2) || g_stop_pending) host_test_watch();
        if (g_stop_pending) finish_pending_stop(0);
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

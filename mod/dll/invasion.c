// Nightreign invasion mod, client DLL.
// Step 1: notice that "Invade" was chosen at the Roundtable table.
// Step 2 (debug): log the session state and the join data of our own session.
// Step 3 (experiment, only with -DTRY_CREATE): choosing "Invade" opens a session on this client.
//
// The table's talk script (mod/talk/t600001000.py) sets event flag INVADE_FLAG. This DLL polls that
// flag through the game's own flag functions, logs the choice and clears the flag.
//
// ponytail: addresses are hard-coded for nightreign.exe 1.3.3 (PE timestamp below) and the DLL does
// nothing on any other build. Replace with signature scans when a second game version has to work.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#define GAME_TIMESTAMP   0x69e0dc2a  // nightreign.exe 1.3.3, Steam build 22818764
#define RVA_FLAG_MAN_PTR 0x3c115a8   // CSEventFlagMan* (starts with its CSFD4VirtualMemoryFlag)
#define RVA_GET_FLAG     0x60ce40    // bool get(CSFD4VirtualMemoryFlag*, uint flag)
#define RVA_SET_FLAG     0x60d330    // void set(CSFD4VirtualMemoryFlag*, uint flag, int state)
#define INVADE_FLAG      10002590
// Session manager (same first fields as Elden Ring's CSSessionManager).
#define RVA_SESSION_MAN_PTR 0x3c21ec8  // CSSessionManager*
#define RVA_SESSION_IMPL    0x257db50  // impl* get(CSSessionManager + 0x60)
#define SM_HANDLE      0x08   // u32 session handle
#define SM_LOBBY_STATE 0x0c   // u32: 0 none, 1 creating, 3 host, 4 joining, 6 client, ...
#define SM_IMPL        0x60
#define SM_PLAYER_LIMIT 0x170 // u32, 3 in vanilla
#define SM_PLAYERS_BEGIN 0x78 // players vector: begin, end (+0x80); entries are 0xF0 bytes
#define SM_PLAYER_SIZE 0xF0
#define SM_CIPHER_KEY  0x728  // DLSerialCipherKey*, then AES encrypter* (+0x730), decrypter* (+0x738)
#define IMPL_VT_EXPORT_JOIN_DATA 0x40  // bool (impl*, u32 handle, void *buf, u32 *size)
#define RVA_CREATE_SESSION  0xdefbd0   // bool (CSSessionManager*, u8 multiplay_type, u8 *one)
#define MULTIPLAY_WHITE_SUMMON 0       // index into the game's multiplay type table (same order as ER)

typedef char (*get_flag_fn)(void *, unsigned);
typedef void (*set_flag_fn)(void *, unsigned, int);

static char log_path[MAX_PATH];

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

// The session implementation behind the session manager (static object inside the network library),
// or NULL. Its counter at +0x10 must be non-zero before a session can be created.
static void **session_impl(char *base, char *sm) {
    if (!*(void **)(sm + SM_IMPL)) return NULL;  // the accessor dereferences this
    void **impl = ((void **(*)(void *))(base + RVA_SESSION_IMPL))(sm + SM_IMPL);
    return impl && *impl ? impl : NULL;
}

// Debug: log the session manager's state whenever it changes: lobby state, session handle, player
// limit, whether the network layer is started, and the join data (Steam lobby id) of our session.
// How many invaders a squad of this size may get. Design rule: 1:1, 2:1, 3:2, 4:2.
static unsigned invaders_allowed(unsigned squad) {
    static const unsigned table[] = {0, 1, 1, 2, 2};
    return squad < sizeof table / sizeof *table ? table[squad] : 2;
}

static void watch_session(char *base) {
    static unsigned last_state = 0xffffffff, last_handle = 0xffffffff, last_started = 0xffffffff;
    static unsigned last_players = 0xffffffff;
    char *sm = *(char **)(base + RVA_SESSION_MAN_PTR);
    if (!sm) return;
    void **impl = session_impl(base, sm);
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

static DWORD WINAPI run(LPVOID self) {
    // Log next to the DLL, so it is easy to find from outside the game.
    GetModuleFileNameA((HMODULE)self, log_path, MAX_PATH);
    char *dot = strrchr(log_path, '.');
    if (dot) strcpy(dot, ".log");

    char *base = (char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP) {
        logf("unsupported game build (timestamp %08lx), doing nothing", nt->FileHeader.TimeDateStamp);
        return 0;
    }
    logf("loaded, game base %p", base);

    get_flag_fn get_flag = (get_flag_fn)(base + RVA_GET_FLAG);
    set_flag_fn set_flag = (set_flag_fn)(base + RVA_SET_FLAG);
    int announced = 0;
    for (;;) {
        Sleep(250);
        watch_session(base);
        void *flags = *(void **)(base + RVA_FLAG_MAN_PTR);
        if (!flags) continue;  // not created yet (still in the title screen / loading)
        if (!announced) {
            logf("event flag manager ready at %p", flags);
            announced = 1;
        }
        // ponytail: called from our own thread, not the game's update thread. Fine for one flag;
        // move onto a game-thread hook before doing anything heavier than this.
        if (get_flag(flags, INVADE_FLAG)) {
            logf("INVADE selected at the Roundtable table");
            set_flag(flags, INVADE_FLAG, 0);
#ifdef TRY_CREATE
            // Experiment (host side): open a session ourselves. Fails in an offline game because the
            // network session layer is not started (see analysis/04-nr-session-join.md).
            // ponytail: runs on our thread, see above; this call does real work in the game.
            char *sm = *(char **)(base + RVA_SESSION_MAN_PTR);
            unsigned st = sm ? *(unsigned *)(sm + SM_LOBBY_STATE) : 99;
            if (st == 0 || st == 2 || st == 5) {  // the states the game itself accepts
                unsigned char one = 1;
                char ok = ((char (*)(void *, unsigned char, unsigned char *))(base + RVA_CREATE_SESSION))(
                    sm, MULTIPLAY_WHITE_SUMMON, &one);
                logf("create session -> %d, create_handle=%u", ok, *(unsigned *)(sm + 0x24));
            }
#endif
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        CreateThread(NULL, 0, run, self, 0, NULL);
    }
    return TRUE;
}

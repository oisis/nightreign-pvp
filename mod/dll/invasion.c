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
// Must run on the game's thread.
static void set_network(int on) {
    if (!g_flow_offline_step) return;  // without the hook the game would throw the player out
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
    set_network(g_searching);
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
        if (g_searching) {
            g_searching = 0;
            set_network(0);
            clear_notices();
            logf("invasion: search ended, an ordinary expedition is starting");
        }
    } else {  // the idle step runs every frame in the Roundtable Hold
        if (g_invasion_pending && GetTickCount() - g_request_tick > 300) toggle_search("machine stayed idle");
        if (g_searching && notices_idle()) notice(0);
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
    // Log next to the DLL, so it is easy to find from outside the game.
    GetModuleFileNameA((HMODULE)self, log_path, MAX_PATH);
    char *dot = strrchr(log_path, '.');
    if (dot) strcpy(dot, ".log");

    g_base = (char *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP) {
        logf("unsupported game build (timestamp %08lx), doing nothing", nt->FileHeader.TimeDateStamp);
        return 0;
    }
    logf("loaded, game base %p", g_base);
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
    }
    for (;;) {
        Sleep(250);
        watch_network();
        watch_session();
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

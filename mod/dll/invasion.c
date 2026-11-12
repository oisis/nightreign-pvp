// Nightreign invasion mod, client DLL.
//
// What it does today:
//   - logs the session manager's state (lobby state, player count, join data) whenever it changes,
//     and how many invaders the current squad would allow;
//   - adds an "Invasion" entry to the expedition menu's Target list (row 101 of NightBossMenuParam,
//     which must exist in regulation.bin: mod/build_regulation.py, or the launcher) and a fourth
//     entry to the "Number of Players" option;
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
static void watch_session(void) {
    static unsigned last_state = 0xffffffff, last_handle = 0xffffffff, last_started = 0xffffffff;
    static unsigned last_players = 0xffffffff;
    char *sm = *(char **)(g_base + RVA_SESSION_MAN_PTR);
    if (!sm) return;
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
static void show_boss_list_hook(void *grid, char *list, void *a3, void *a4) {
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
// ponytail: its value is 0, the same as "three players", so choosing it behaves like three players
// for now; a value of its own needs the code that consumes the choice to be found first.
// The temporary string used to build it is never destroyed (one small leak per game run).
static char *fourth_item(void) {
    static _Alignas(16) char item[COMBO_ITEM_SIZE];
    static int ready;
    if (!ready) {
        static _Alignas(16) char tmp[0x100];
        void *text = ((void *(*)(void *, int))(g_base + RVA_TEXT_STRING_CTOR))(tmp, TEXT_FOUR_PLAYERS);
        *(void **)item = g_base + RVA_COMBO_ITEM_VTABLE;
        item[8] = 0;
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
// machine is in that step. We wrap the steps a start goes through first, solo and with matchmaking,
// and when the chosen target is our Invasion row we send the machine back to its idle step instead
// of letting an expedition begin.
//
// The chosen Target rows reach the machine's context through 0xb67cc0, which converts each menu row
// into a boss number (two lookups by row id, then a combine) and hands the result to the context's
// "set targets" method. Our row converts to boss 0, so by then the choice is lost; we note it at the
// first lookup instead.
#define RVA_QM_STEP_TABLE        0x39ae9d0
#define QM_STEP_NONE             0
#define QM_CTRL_CONTEXT          0xa8       // CS::CSQuickMatchLobbyContext*
#define QM_CTRL_VT_SET_STEP      4          // void (ctrl*, int step); the game calls it with 0 on failure
#define RVA_QM_CONTEXT_VTABLE    0x2c75d20
#define QM_CTX_PLAYERS           0x98       // u32 (meaning to be confirmed from logs)
#define QM_CTX_TARGETS_BEGIN     0xa8       // vector of u32 NightBossMenuParam row ids: begin, end (+8)
#define QM_CTX_TARGET_INDEX      0x178      // u32 index into that vector

typedef void (*step_fn)(void *ctrl, void *time);
static struct { int index; unsigned rva; const char *name; step_fn original; DWORD last_tick; } g_steps[] = {
    {1, 0xb59880, "SearchRegister", NULL, 0},
    {12, 0xb5c010, "SingleStart", NULL, 0},
    {16, 0xb5ac50, "SingleHeroReSelect", NULL, 0},
    {18, 0xb5b850, "SingleSetup", NULL, 0},
};

static int g_saw_invasion_row;  // our row went through the conversion since the last "set targets"
static int g_invasion_chosen;   // the targets now in the context came from our row

// Logs what is about to start. Returns 1 if it is an invasion.
static int invasion_chosen(char *ctrl, const char *step, int log_it) {
    char *ctx = *(char **)(ctrl + QM_CTRL_CONTEXT);
    if (!ctx || *(char **)ctx != g_base + RVA_QM_CONTEXT_VTABLE) {
        if (log_it) logf("start (%s): unexpected context object, leaving the game alone", step);
        return 0;
    }
    unsigned *begin = *(unsigned **)(ctx + QM_CTX_TARGETS_BEGIN), *end = *(unsigned **)(ctx + QM_CTX_TARGETS_BEGIN + 8);
    size_t count = begin && end >= begin ? (size_t)(end - begin) : 0;
    if (count > 64) count = 0;
    char list[400] = "";
    for (size_t i = 0; i < count; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%u", i ? "," : "", begin[i]);
    if (log_it)
        logf("start (%s): bosses [%s] index %u, players %u, invasion %d", step, list,
             *(unsigned *)(ctx + QM_CTX_TARGET_INDEX), *(unsigned *)(ctx + QM_CTX_PLAYERS), g_invasion_chosen);
    return g_invasion_chosen;
}

// Small on-screen notices: the game's "summon message" box (CS::SummonMsgData pushed to the menu
// manager's SummonMsgQueue), which shows a line for a moment and fades out by itself.
#define RVA_MENU_TEXT_CTOR  0x7ac800  // text* (text *out, int id): CL_MenuText line by id
#define RVA_SUMMON_MESSAGE  0x86b8d0  // bool (text*): queue a summon message (the queue copies the text)
#define TEXT_SEARCHING      131162    // added to CL_MenuText by build_msg.py / the launcher
#define TEXT_SEARCH_CANCELLED 131163
static void notice(int which) {  // 0 searching, 1 cancelled
    static _Alignas(16) char text[2][0x100];  // built once and kept: the game never frees them for us
    static void *ready[2];
    if (!ready[which])
        ready[which] = ((void *(*)(void *, int))(g_base + RVA_MENU_TEXT_CTOR))(
            text[which], which ? TEXT_SEARCH_CANCELLED : TEXT_SEARCHING);
    ((unsigned char (*)(void *))(g_base + RVA_SUMMON_MESSAGE))(ready[which]);
}

// The invasion search. Choosing Invasion starts it, choosing it again or starting an ordinary
// expedition ends it. While it runs the "searching" notice is repeated.
// ponytail: nothing is searched for yet; the matchmaking call goes where the search starts.
#define NOTICE_PERIOD_MS 4000  // one notice every this often: shown for a moment, then a pause
static int g_searching;
static DWORD g_last_notice;

static void step_hook(int slot, void *ctrl, void *time) {
    DWORD now = GetTickCount();
    int entered = now - g_steps[slot].last_tick > 1000;  // first frame of this step
    g_steps[slot].last_tick = now;
    if (invasion_chosen(ctrl, g_steps[slot].name, entered)) {
        void **vt = *(void ***)ctrl;
        ((void (*)(void *, int))vt[QM_CTRL_VT_SET_STEP])(ctrl, QM_STEP_NONE);
        if (entered) {
            g_searching = !g_searching;
            g_last_notice = now;
            notice(g_searching ? 0 : 1);
            logf("invasion: search %s (expedition start cancelled)", g_searching ? "started" : "called off");
        }
        return;
    }
    if (entered && g_searching) {
        g_searching = 0;
        logf("invasion: search ended, an ordinary expedition is starting");
    }
    g_steps[slot].original(ctrl, time);
}

// The machine's idle step runs every frame in the Roundtable Hold: our clock for the notice.
#define RVA_QM_STEP_NONE_FN 0xb59820
static void idle_step_hook(void *ctrl, void *time) {
    DWORD now = GetTickCount();
    if (g_searching && now - g_last_notice >= NOTICE_PERIOD_MS) {
        g_last_notice = now;
        notice(0);
    }
    ((step_fn)(g_base + RVA_QM_STEP_NONE_FN))(ctrl, time);
}
static void step_hook_0(void *c, void *t) { step_hook(0, c, t); }
static void step_hook_1(void *c, void *t) { step_hook(1, c, t); }
static void step_hook_2(void *c, void *t) { step_hook(2, c, t); }
static void step_hook_3(void *c, void *t) { step_hook(3, c, t); }

// The context's "set targets" method (vtable slot 11): void (ctx*, src*), copying the u32 vector of
// boss numbers at src+8 (begin) / src+0x10 (end).
#define QM_CTX_VT_SET_TARGETS  11
#define RVA_QM_CTX_SET_TARGETS 0xb45500
static void set_targets_hook(char *ctx, char *src) {
    g_invasion_chosen = g_saw_invasion_row;
    g_saw_invasion_row = 0;
    ((void (*)(char *, char *))(g_base + RVA_QM_CTX_SET_TARGETS))(ctx, src);
}

// First lookup of the row-to-boss conversion: "call 0xe7e1a0" with the menu row id in ecx.
#define RVA_ROW_LOOKUP_CALL 0xb67e3a  // e8 61 63 31 00
#define RVA_ROW_LOOKUP      0xe7e1a0
static unsigned long long row_lookup_hook(unsigned row) {
    if (row == INVASION_ROW) g_saw_invasion_row = 1;
    return ((unsigned long long (*)(unsigned))(g_base + RVA_ROW_LOOKUP))(row);
}

static void hook_expedition_start(void) {
    void **slot = (void **)(g_base + RVA_QM_CONTEXT_VTABLE) + QM_CTX_VT_SET_TARGETS;
    DWORD old;
    if (*slot == (void *)(g_base + RVA_QM_CTX_SET_TARGETS) && VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        *slot = (void *)set_targets_hook;
        VirtualProtect(slot, 8, old, &old);
    } else {
        logf("expedition start: set-targets slot does not look as expected, not hooking");
        return;
    }
    unsigned char *site = (unsigned char *)g_base + RVA_ROW_LOOKUP_CALL, *stub = near_alloc(12);
    int expected = (int)(RVA_ROW_LOOKUP - (RVA_ROW_LOOKUP_CALL + 5));
    if (site[0] != 0xE8 || memcmp(site + 1, &expected, 4) || !stub) {
        logf("expedition start: row lookup call does not look as expected, not hooking");
        return;
    }
    void *hook = (void *)row_lookup_hook;
    stub[0] = 0x48; stub[1] = 0xB8;  // mov rax, hook; jmp rax
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (!patch_rel32(site + 1, site + 5, stub)) {
        logf("expedition start: cannot patch the row lookup call");
        return;
    }

    static const step_fn hooks[] = {step_hook_0, step_hook_1, step_hook_2, step_hook_3};
    step_fn *table = (step_fn *)(g_base + RVA_QM_STEP_TABLE);
    for (int i = 0; i < 4; i++)
        if (table[g_steps[i].index * 2] != (step_fn)(g_base + g_steps[i].rva)) {
            logf("expedition start: step table does not look as expected, not hooking");
            return;
        }
    for (int i = 0; i < 4; i++) {
        g_steps[i].original = table[g_steps[i].index * 2];
        table[g_steps[i].index * 2] = hooks[i];
    }
    if (table[QM_STEP_NONE * 2] == (step_fn)(g_base + RVA_QM_STEP_NONE_FN)) table[QM_STEP_NONE * 2] = idle_step_hook;
    else logf("expedition start: idle step does not look as expected, notices will not repeat");
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
    hook_target_list();
    hook_players_option();
    hook_expedition_start();
    for (;;) {
        Sleep(250);
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

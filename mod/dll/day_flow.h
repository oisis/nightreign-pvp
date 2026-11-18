// Day flow (boss-flow 4, the owner's design of 2026-11-18): the day is free play; the night comes when the squad
// uses our item "Challenge the Night" (goods 1291, sold by the field merchant), which works once 5 bosses and the
// region's special boss (the Mountaintop's dragon or the Rotted Woods' Captain) are dead. Each boss makes the
// enemies stronger: +5 % HP and damage per boss until the condition is met, then +50 % per boss. Rows (launcher,
// day_flow_rows): SpEffect 90071612 = the item's use marker (1 s), 90071700 + k = an enemy tier of +5 % * k.
// Every machine counts the bosses itself (death flags, synced; flag_note -> df_kill) and so has the same tier.
#define DF_GOODS        1291
#define DF_MARKER       90071612
#define DF_TIER_FIRST   90071700
#define DF_TIER_COUNT   200
#define DF_BOSSES       5
#define LOBBY_CHAL_KEY  "nrpvp_chal"   // host lobby: the challenge's number this expedition ("1", "2"); members: the same as member data
// The special bosses (death flags, 2003[69] after death; see 08-flagi-gry): the Mountaintop's ice dragon
// (m60_42_38_10, 42_39_10, 43_39_10) and the Captain of the Rotted Woods (m60_45_36_30).
static const unsigned DF_SPECIAL[] = {1028100800, 1029100800, 1039100800, 1039100801, 1056300800};
static volatile LONG g_df_kills, g_df_special_kills;
static int g_df_bosses, g_df_special, g_df_met, g_df_met_at, g_df_after, g_df_day = 1, g_df_go, g_df_used;
static int g_df_tier;  // percent: 0, 5, 10 ...
static DWORD g_df_notice_due;  // the progress notice, 6 s after a kill
static void df_kill(unsigned id) {  // flag_note, any thread: a boss death flag turned on
    for (unsigned i = 0; i < sizeof DF_SPECIAL / sizeof *DF_SPECIAL; i++)
        if (id == DF_SPECIAL[i]) { InterlockedIncrement(&g_df_special_kills); return; }
    InterlockedIncrement(&g_df_kills);
}
static int df_on(void) { return g_settings[S_BOSS_FLOW] == 4; }
static int df_needed(void) { return g_settings[S_CHALLENGE_BOSSES] > 0 ? g_settings[S_CHALLENGE_BOSSES] : DF_BOSSES; }
static int df_target_tier(void) {
    int t = g_df_met ? g_df_met_at * 5 + g_df_after * 50 : g_df_bosses * 5;
    return t > DF_TIER_COUNT * 5 ? DF_TIER_COUNT * 5 : t;
}
static void df_progress_notice(void) {
    char arg[16];
    snprintf(arg, sizeof arg, "%d/%d", g_df_bosses < df_needed() ? g_df_bosses : df_needed(), df_needed());
    if (g_df_met) notice_with(131182, NULL);
    else notice_with(g_df_special ? 131181 : 131180, arg);
}
// --- enemy tiers: through the game's own re-apply of the player-count correction (ChrFinder -> 0x1405186a0, which
// calls 0x1403c7e50(chr) for each character at 0x1405186ac; the pass runs when WCM+0x17558 differs from the count,
// so setting it to -1 makes the game run it over every character in the next frame). Our hook puts the wanted tier
// on each enemy (0x3c7fd0, as for our other rows) and removes any other tier (0x140601210(chr, id): 2004[21]'s
// per-character removal). Not on the players (WCM +0x9a40 set, the local player); not on night bosses (characters
// in the night-boss maps m47, m48, m49, m52: block >> 24) before this day's challenge.
static char *g_df_rows[DF_TIER_COUNT + 1];
static int g_df_rows_ready;
static unsigned g_df_pass_count;
static int df_is_player(char *chr) {
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    if (!wcm || chr == *(char **)(wcm + WCM_LOCAL_PLAYER)) return 1;
    void **set = (void **)(wcm + 0x9a40);
    if (IsBadReadPtr(set, 8) || IsBadReadPtr(*(void ***)set, 16)) return 0;
    void **vt = *(void ***)set;
    int count = ((int (*)(void *))vt[0])(set);
    for (int i = 0; i < count && i < 16; i++)
        if (((char *(*)(void *, unsigned long long))vt[1])(set, (unsigned long long)i) == chr) return 1;
    return 0;
}
// --- Nightfarer-like camps (the launcher makes the generator always roll two: MapPatternSet (0, 0, 100)): their
// invaders (NpcParam 600030010 ... 600030910, step 100; SmallbaseInvationNpcParam) are created when the map loads.
// Each is kept disabled (0x608c60(chr, 0), as the hidden bosses) until the squad has killed as many bosses as its
// place in the order they were first seen (one camp per boss), on every machine.
static char *g_df_inv_rows[10];
static int g_df_inv_ready, g_df_camps, g_df_kills_day, g_df_camp_open[8];
static char *df_param(const char *type);
static char *df_param_row(char *f, int id);
static int df_camp_chr(char *chr) {
    if (!g_df_inv_ready) {
        char *f = df_param("NPC_PARAM_ST");
        for (int k = 0; k < 10; k++) g_df_inv_rows[k] = f ? df_param_row(f, 600030010 + 100 * k) : NULL;
        g_df_inv_ready = f ? 1 : -1;
        logf("day flow: camps: invader NpcParam rows %s", f ? "found" : "NOT found");
    }
    if (g_df_inv_ready < 0 || IsBadReadPtr(*(void **)chr, 0x1a8)) return 0;
    char **ref = ((char **(*)(char *))(*(void ***)chr)[0x1a0 / 8])(chr);
    char *row = ref && !IsBadReadPtr(ref, 8) ? *ref : NULL;
    for (int k = 0; row && k < 10; k++)
        if (row == g_df_inv_rows[k]) return 1;
    return 0;
}
// Grouping (deck test of 5f7bece: four invaders for the two camps on the map): a camp's invaders are the ones
// in the same map block (+0x38) within 100 m of each other (position getter 0x3d0fd0). At creation the block is
// ffffffff; an invader is grouped at the first pass that sees it placed, and stays disabled until then.
static struct { char *chr; int camp; float first[3]; DWORD tick; } g_df_inv[16];
static int g_df_ninv;
static unsigned g_df_camp_block[8];
static float g_df_camp_pos[8][3];
static int df_chr_pos(char *chr, float out[3]) {
    char *ctrl = !IsBadReadPtr(chr + 0x60, 8) ? *(char **)(chr + 0x60) : NULL, *a = ctrl && !IsBadReadPtr(ctrl + 0x10, 8) ? *(char **)(ctrl + 0x10) : NULL;
    char *b = a && !IsBadReadPtr(a + 0x1b8, 8) ? *(char **)(a + 0x1b8) : NULL;
    if (!b || IsBadReadPtr(b + 0x68, 8) || IsBadReadPtr(*(char **)(b + 0x68), 0x80)) return 0;
    _Alignas(16) float v[4];
    ((void *(*)(void *, void *))(g_base + 0x3d0fd0))(chr, v);
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    return 1;
}
static void df_camp_control(char *chr) {
    int i = 0;
    while (i < g_df_ninv && g_df_inv[i].chr != chr) i++;
    unsigned block = *(unsigned *)(chr + CHR_BLOCK_ID);
    float pos[3];
    int have_pos = df_chr_pos(chr, pos);
    if (i == g_df_ninv) {
        if (g_df_ninv == 16) return;
        g_df_inv[g_df_ninv].chr = chr, g_df_inv[g_df_ninv].camp = -1, g_df_inv[g_df_ninv].tick = GetTickCount();
        if (have_pos) memcpy(g_df_inv[g_df_ninv].first, pos, sizeof pos);
        g_df_ninv++;
        logf("day flow: invader %d found: block %08x, position %s%.0f %.0f %.0f", i + 1, block, have_pos ? "" : "(none) ", have_pos ? pos[0] : 0,
             have_pos ? pos[1] : 0, have_pos ? pos[2] : 0);
    }
    int camp = g_df_inv[i].camp;
    // Deck test of 4665420: no invader was ever grouped. Detection is the old build's (which found four), and the
    // grouping waited for +0x38 != ffffffff, the value every log shows for them; so the block no longer gates it.
    // Grouped by position (within 100 m; same block only when both have one), once the invader has moved from where
    // it was created (all were created at load at a spot outside the camps: 893 39 -71 in deck-run64) or 60 s later.
    float mx = have_pos ? pos[0] - g_df_inv[i].first[0] : 0, mz = have_pos ? pos[2] - g_df_inv[i].first[2] : 0;
    int settled = have_pos && (mx * mx + mz * mz > 1.0f || GetTickCount() - g_df_inv[i].tick > 60000);
    if (camp < 0 && settled) {
        for (int c = 0; c < g_df_camps && camp < 0; c++) {
            float dx = pos[0] - g_df_camp_pos[c][0], dy = pos[1] - g_df_camp_pos[c][1], dz = pos[2] - g_df_camp_pos[c][2];
            int same_block = g_df_camp_block[c] == block || g_df_camp_block[c] == 0xffffffff || block == 0xffffffff;
            if (same_block && dx * dx + dy * dy + dz * dz < 100.0f * 100.0f) camp = c;
        }
        if (camp < 0 && g_df_camps < 8) {
            camp = g_df_camps++;
            g_df_camp_block[camp] = block, memcpy(g_df_camp_pos[camp], pos, sizeof pos), g_df_camp_open[camp] = 0;
            logf("day flow: camp %d found (block %08x, position %.0f %.0f %.0f); opens after %d boss(es)", camp + 1, block, pos[0], pos[1], pos[2], camp + 1);
        }
        g_df_inv[i].camp = camp;
        if (camp >= 0) logf("day flow: camp %d: invader %d of its camp placed", camp + 1, i + 1);
    }
    int open = camp >= 0 && (g_df_kills_day > camp || g_df_go);
    if (open) {
        if (!chr_enabled(chr)) ((char (*)(char *, char))(g_base + 0x608c60))(chr, 1);
        if (!g_df_camp_open[camp]) g_df_camp_open[camp] = 1, logf("day flow: camp %d opens (block %08x): its invaders enabled", camp + 1, block);
    } else if (chr_enabled(chr)) {
        ((char (*)(char *, char))(g_base + 0x608c60))(chr, 0);
    }
}
static void df_tier_apply(char *chr) {
    if (!chr || IsBadReadPtr(chr, 0x200) || df_is_player(chr)) return;
    if (!IsBadReadPtr(chr + 0x18, 8) && *(char **)(chr + 0x18) && df_camp_chr(chr)) df_camp_control(chr);
    if (!g_df_rows_ready) {
        for (int k = 1; k <= DF_TIER_COUNT; k++) {
            struct { char *row; unsigned id; unsigned char flag; } ref = {0};
            ((void (*)(void *, unsigned))(g_base + RVA_SPEFFECT_ROW))(&ref, DF_TIER_FIRST + k);
            g_df_rows[k] = ref.row;
        }
        g_df_rows_ready = g_df_rows[1] && g_df_rows[DF_TIER_COUNT] ? 1 : -1;
        logf("day flow: enemy tier rows %s", g_df_rows_ready > 0 ? "found" : "MISSING from regulation.bin (no scaling)");
    }
    if (g_df_rows_ready < 0) return;
    unsigned area = *(unsigned *)(chr + CHR_BLOCK_ID) >> 24;
    int night_boss_map = area == 47 || area == 48 || area == 49 || area == 52;
    int want = night_boss_map && !g_df_go ? 0 : g_df_tier / 5;
    char *module = !IsBadReadPtr(chr + CHR_SPEFFECTS, 8) ? *(char **)(chr + CHR_SPEFFECTS) : NULL;
    if (!module || IsBadReadPtr(module + 8, 8)) return;
    int have = 0, n = 0, drop[8], nd = 0;
    for (char **e = *(char ***)(module + 8); e && n < 512; e = (char **)e[7], n++) {
        if (IsBadReadPtr(e, 0x70)) return;
        if (*(unsigned *)((char *)e + 0x68) & 0x800c0003) continue;
        if (e[0] < g_df_rows[1] || e[0] > g_df_rows[DF_TIER_COUNT]) continue;
        for (int k = 1; k <= DF_TIER_COUNT; k++)
            if (e[0] == g_df_rows[k]) {
                if (k == want) have = 1;
                else if (nd < 8) drop[nd++] = k;
                break;
            }
    }
    for (int i = 0; i < nd; i++) ((unsigned char (*)(char *, int))(g_base + 0x601210))(chr, DF_TIER_FIRST + drop[i]);
    if (want && !have) ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(chr, DF_TIER_FIRST + want, 0);
    g_df_pass_count++;
}
static void df_chr_pass_hook(char *chr) {
    if (df_on()) df_tier_apply(chr);
    ((void (*)(char *))(g_base + 0x3c7e50))(chr);
}
static void df_repass(const char *why) {
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    if (wcm && !IsBadReadPtr(wcm + 0x17558, 4)) *(int *)(wcm + 0x17558) = -1;
    if (why) logf("day flow: enemy tier %d%% (%s); characters seen by the last passes: %u", g_df_tier, why, g_df_pass_count);
}
// --- the challenge's warp: the night-boss attach point of the day's final circle (play-area manager phase table
// +0x190 + 0x20 * 2: +0 block, +4 / +0xc x / z; PlayAreaCreateParam +4 block, +8 / +0xc x / z, +0x24 bossAttachPoint;
// SmallBaseAndSpotAttachPoint +0x14 entity), through 2004[41]'s path 0x1405f1a00(&10000, &entity, 1). Measured
// (deck-run64): returns 1, the character stands on the ground at the point's height in the final circle's tile.
// Params in memory: repository [0x143c28880], 0x140e9e680(rep, index, 0) -> +0x80 -> +0x80 = the param file
// (+0x10 offset of the type name, rows from +0x40: id, +8 data offset).
static char *df_param(const char *type) {
    char *rep = *(char **)(g_base + 0x3c28880);
    for (int i = 0; rep && i < 0xfe; i++) {
        char *t = ((char *(*)(char *, int, unsigned))(g_base + 0xe9e680))(rep, i, 0);
        char *a = t && !IsBadReadPtr(t + 0x80, 8) ? *(char **)(t + 0x80) : NULL;
        char *f = a && !IsBadReadPtr(a + 0x80, 8) ? *(char **)(a + 0x80) : NULL;
        if (!f || IsBadReadPtr(f, 0x40)) continue;
        unsigned long long so = *(unsigned long long *)(f + 0x10);
        if (so < 0x4000000 && !IsBadReadPtr(f + so, 40) && !strncmp(f + so, type, strlen(type) + 1)) return f;
    }
    return NULL;
}
static char *df_param_row(char *f, int id) {
    int n = f ? *(unsigned short *)(f + 0xa) : 0, lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, rid = *(int *)(f + 0x40 + mid * 0x18);
        if (rid == id) return f + *(unsigned long long *)(f + 0x40 + mid * 0x18 + 8);
        if (rid < id) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
static float df_abs(float v) { return v < 0 ? -v : v; }
static int df_warp(void) {
    char *pa = *(char **)(g_base + RVA_PLAY_AREA_PTR), *me = local_player();
    char *areas = df_param("PLAY_AREA_CREATE_PARAM_ST"), *points = df_param("SMALLBASE_AND_SPOT_ATTACH_POINT_ST");
    if (!pa || !me || !areas || !points) return logf("day flow: warp: no play-area manager, character or params"), 0;
    char *fin = pa + 0x190 + 0x20 * 2, *row = NULL;
    int n = *(unsigned short *)(areas + 0xa), row_id = -1;
    for (int i = 0; i < n; i++) {
        char *r = areas + *(unsigned long long *)(areas + 0x40 + i * 0x18 + 8);
        if ((*(unsigned *)(r + 4) & 0xffffff) == (*(unsigned *)fin & 0xffffff) && df_abs(*(float *)(r + 8) - *(float *)(fin + 4)) < 2 &&
            df_abs(*(float *)(r + 0xc) - *(float *)(fin + 0xc)) < 2)
            row = r, row_id = *(int *)(areas + 0x40 + i * 0x18);
    }
    char *pt = row ? df_param_row(points, *(int *)(row + 0x24)) : NULL;
    if (!pt) return logf("day flow: warp: no site for the final circle (block %08x, row %d)", *(unsigned *)fin, row_id), 0;
    int who = 10000, dest = *(int *)(pt + 0x14);
    unsigned char got = (unsigned char)((unsigned long long (*)(int *, int *, char))(g_base + 0x5f1a00))(&who, &dest, 1);
    logf("day flow: warp to the night boss's site (circle row %d, attach point %d, entity %d): %s", row_id, *(int *)(row + 0x24), dest,
         got ? "done" : "FAILED");
    return got;
}
// --- the clock (host; joiners take it from the host): runs to DF_HOLD_H:00:00 (18:00, the owner: daylight until the challenge) and is held there (both clocks' rates 0,
// as boss-flow 3) until the challenge; then 23:00:00 (0x68ff30, as boss-flow 1) and the game's own night follows.
// Day 2 (7505): held again at DF_HOLD_H:00 until the second challenge. The jump to 23:00 makes it dark at once.
#define DF_HOLD_H 18
static void bf4_frame(int first) {
    static int held, released_day;
    static float base_pa, base_clock;
    char *pa = *(char **)(g_base + RVA_PLAY_AREA_PTR), *clock = *(char **)(g_base + RVA_CLOCK_PTR);
    if (first) held = 0, released_day = 0, base_pa = 0;
    if (!pa || !clock) return;
    if (!base_pa) base_pa = *(float *)(pa + PA_BASE_RATE), base_clock = *(float *)(clock + CLOCK_BASE_RATE);
    if (g_df_go && released_day != g_df_day) {
        released_day = g_df_day, held = 0;
        *(float *)(pa + PA_RATE) = base_pa, *(float *)(pa + PA_BASE_RATE) = base_pa;
        *(float *)(clock + CLOCK_RATE) = base_clock, *(float *)(clock + CLOCK_BASE_RATE) = base_clock;
        ((void (*)(char *, int, int, int))(g_base + RVA_PA_SET_TIME))(pa, 23, 0, 0);
        logf("clock: boss flow 4: day %d's challenge: clock set to 23:00:00 and released (the night follows)", g_df_day);
        return;
    }
    if (released_day == g_df_day) return;
    unsigned long long v = *(unsigned long long *)(pa + PA_TIME);
    unsigned secs = (((unsigned)(v >> 34) & 0x1f) * 60 + ((unsigned)(v >> 39) & 0x3f)) * 60 + ((unsigned)(v >> 45) & 0x3f);
    if (secs >= DF_HOLD_H * 3600 && secs < 23 * 3600) {
        if (!held) {
            ((void (*)(char *, int, int, int))(g_base + RVA_PA_SET_TIME))(pa, DF_HOLD_H, 0, 0);
            held = 1, logf("clock: boss flow 4: day %d at %d:00:00, held until the challenge", g_df_day, DF_HOLD_H);
        }
        *(float *)(pa + PA_RATE) = 0.0f, *(float *)(pa + PA_BASE_RATE) = 0.0f;
        *(float *)(clock + CLOCK_RATE) = 0.0f, *(float *)(clock + CLOCK_BASE_RATE) = 0.0f;
    }
}
// --- the item's use (local player): the marker SpEffect starts. Before the condition, or after this day's
// challenge: the item is given back (0x1405fdb10(type code, id, +1): 2003[43]'s add, the type code from 0x14059a7b0(3),
// 3 = goods) and the progress is shown. Otherwise the challenge starts: the host publishes it in its lobby; an ally
// in its member data, which the host takes over.
static int g_df_seq;  // challenges published / seen this expedition
static int g_df_used_here;  // this machine's player used the item for the challenge (the item is removed when it starts)
static void df_start_local(const char *who) {
    g_df_go = 1, g_df_used = 1;
    if (g_df_used_here) {  // the one who used it: the item goes (0x1405fdb10(goods code, id, -1), 2003[24]'s removal)
        int code = ((int (*)(int))(g_base + 0x59a7b0))(3);
        ((void (*)(int, int, int))(g_base + 0x5fdb10))(code, DF_GOODS, -1);
        g_df_used_here = 0;
        logf("day flow: the item is used up (removed from the user's inventory)");
    }
    notice_with(131184, NULL);
    logf("day flow: day %d's challenge (%s): warp, night", g_df_day, who);
    df_warp();
    df_repass("challenge: night bosses get the tier");
}
static void df_item_used(void) {
    if (!g_df_met || g_df_used) {  // the item is not consumed (launcher: isConsume off), nothing to give back
        char arg[16];
        snprintf(arg, sizeof arg, "%d", df_needed());
        if (g_df_used) notice_with(131184, NULL);
        else notice_with(131183, arg);
        logf("day flow: item used %s: kept, not consumed", g_df_used ? "after this day's challenge" : "before the condition");
        return;
    }
    char text[8];
    g_df_used_here = 1;
    snprintf(text, sizeof text, "%d", ++g_df_seq);
    if (g_join != 4) {
        if (g_lobby) lobby_set(LOBBY_CHAL_KEY, text);
        df_start_local("ours, as the host");
    } else if (g_as_ally && g_diag.set_member && g_steam.mm && g_candidate) {
        g_diag.set_member(g_steam.mm, g_candidate, LOBBY_CHAL_KEY, text);
        logf("day flow: item used as an ally: challenge %s asked of the host (member data)", text);
    }
}
static void df_lobby_watch(void) {
    static DWORD next;
    static int seen = 0;
    if ((int)(GetTickCount() - next) < 0 || !g_steam.mm || g_steam_down) return;
    next = GetTickCount() + 500;
    if (g_join != 4 && g_lobby) {  // host: an ally's request
        int n = g_steam.members(g_steam.mm, g_lobby);
        for (int i = 0; i < n && g_diag.member && g_diag.get_member; i++) {
            u64 id = g_diag.member(g_steam.mm, g_lobby, i);
            const char *v = g_diag.get_member(g_steam.mm, g_lobby, id, LOBBY_CHAL_KEY);
            int k = v && *v ? atoi(v) : 0;
            if (k > g_df_seq && g_df_met && !g_df_used) {
                char text[8];
                snprintf(text, sizeof text, "%d", g_df_seq = k);
                lobby_set(LOBBY_CHAL_KEY, text);
                df_start_local("an ally's");
            }
        }
    } else if (g_join == 4 && g_candidate) {  // joiner: the host's challenge
        const char *v = g_steam.get_data(g_steam.mm, g_candidate, LOBBY_CHAL_KEY);
        int k = v && *v ? atoi(v) : 0;
        if (k > seen) {
            seen = k, g_df_seq = k > g_df_seq ? k : g_df_seq;
            if (!g_as_ally) join_over("the host's squad took the night challenge");
            else if (!g_df_used) df_start_local("the host's");
        }
    }
    if (g_join != 4) seen = 0;
}
// --- world events (host): the launcher gives every composition the day-1 flag of each family, so the handlers wait;
// from the host's level 5 on, every 2 bosses (this day) the next family's start flag goes on as a network flag
// (0x5e79f0 then 0x5e7470, as 2003[69]): meteorite 8040 (common 1121), hordes 8055 (1124: armed; the hordes rise when
// a player enters one of their camps, m46_00..06, which the launcher adds), the special one 8060 (1125). Each once:
// the handlers end once started. The
// game's own all-at-once start 9013 (common 1129, 9:25-16:43) is kept off (flag_store_hook).
static int g_df_events;
static DWORD g_df_event_tick;
static void df_net_flag(int id) {
    char *man = *(char **)(g_base + 0x3c115a8);
    if (!man) return;
    two_flag(id, 1, 1);
    ((void (*)(char *, int *, char))(g_base + 0x5e7470))(man, &id, 1);
}
static void df_events_frame(void) {
    // frenzy (8071) is not started here: 8071 is the game's "a player reached the flame" (common_func 90035073: the
    // player has SpEffect 16611), and the family waits for it by itself once its day flag is on (launcher)
    static const struct { int flag; const char *name; } FAM[3] = {{8040, "meteorite"}, {8055, "hordes"}, {8060, "special event"}};
    char *gdm = *(char **)(g_base + 0x3c078d0), *pgd = gdm ? *(char **)(gdm + 8) : NULL;
    int level = pgd && !IsBadReadPtr(pgd, 0x70) ? *(int *)(pgd + 0x68) : 0;
    if (g_df_events >= 3 || level < 5 || g_df_kills_day < 2 * (g_df_events + 1) || GetTickCount() - g_df_event_tick < 30000) return;
    int k = g_df_events++;
    g_df_event_tick = GetTickCount();
    df_net_flag(FAM[k].flag);
    logf("day flow: event %d started: %s (flag %d), level %d, %d bosses this day", k + 1, FAM[k].name, FAM[k].flag, level, g_df_kills_day);
}
// --- every frame in the field (every machine)
static void day_flow_frame(int in_world) {
    static int was, marker_was, day2_seen, last_tier = -1, down_last = -1;
    static DWORD repass_tick;
    if (!df_on()) return;
    int first = in_world && !was;
    was = in_world;
    if (!in_world) return;
    if (first) {
        g_df_bosses = g_df_special = g_df_met = g_df_met_at = g_df_after = 0, g_df_day = 1, g_df_go = g_df_used = 0, g_df_seq = 0;
        g_df_tier = 0, day2_seen = 0, last_tier = -1, marker_was = 0, down_last = -1, g_df_kills_day = 0, g_df_camps = 0, g_df_ninv = 0, g_df_notice_due = 0;
        g_df_events = 0, g_df_event_tick = 0;
        InterlockedExchange(&g_df_kills, 0), InterlockedExchange(&g_df_special_kills, 0);
        logf("day flow: expedition start: challenge after %d bosses and the special boss", df_needed());
    }
    if (!day2_seen && bf_flag(7505)) {  // day 2: the game's own day-2 level, our count starts again
        day2_seen = 1, g_df_day = 2;
        g_df_bosses = g_df_special = g_df_met = g_df_met_at = g_df_after = 0, g_df_go = g_df_used = 0, g_df_kills_day = 0;
        InterlockedExchange(&g_df_kills, 0), InterlockedExchange(&g_df_special_kills, 0);
        logf("day flow: day 2 (7505): tier back to 0, count starts again (camps already open stay open; events: each family "
             "ran at most once, none again)");
    }
    LONG k = InterlockedExchange(&g_df_kills, 0), sk = InterlockedExchange(&g_df_special_kills, 0);
    g_df_kills_day += (int)(k + sk);
    if (k > 0 || sk > 0) df_repass("a boss: camps and tier");
    if (k > 0 || sk > 0) {
        if (g_df_met) g_df_after += (int)(k + sk);
        else g_df_bosses += (int)k, g_df_special = g_df_special || sk > 0;
        if (!g_df_met && g_df_bosses >= df_needed() && g_df_special) {
            g_df_met = 1, g_df_met_at = g_df_bosses + 1;  // the special boss counts as one more for the tier
            logf("day flow: condition met (%d bosses and the special boss)", g_df_bosses);
        }
        logf("day flow: boss killed: %d bosses, special %s, after the condition %d", g_df_bosses, g_df_special ? "yes" : "no", g_df_after);
        g_df_notice_due = GetTickCount() + 6000 | 1;  // after the game's own boss banner (deck test: a notice in the same frame was not seen)
    }
    if (g_df_notice_due && (int)(GetTickCount() - g_df_notice_due) >= 0) {
        g_df_notice_due = 0;
        df_progress_notice();  // the state as it is now: later kills in the 6 s are in it
    }
    g_df_tier = df_target_tier();
    if (g_df_tier != last_tier) last_tier = g_df_tier, df_repass("bosses"), repass_tick = GetTickCount();
    else if (GetTickCount() - repass_tick > 15000) repass_tick = GetTickCount(), df_repass(NULL);  // characters loaded since
    char *me = local_player();
    int marker = me && speffect_on(me, DF_MARKER);
    if (marker && !marker_was) df_item_used();
    marker_was = marker;
    df_lobby_watch();
    if (g_join != 4) df_events_frame();
    // solo (our own expedition, nobody else in the session): every death is final (no downed state, nobody to revive),
    // so at HP 0 (+0x140 of [chr+0x1b8], before the game's revive with the level loss) the expedition ends: the way
    // home of a lost invasion, 0x6af8f0 (gm+0xaf1). In a squad the host's final death is host_death_watch's (+0x710 1 -> 0).
    {
        static DWORD ended;
        static int alive_seen;  // HP above 0 seen in this expedition (no end on a load's empty value)
        char **pd = me && !IsBadReadPtr(me + 0x1b8, 8) ? *(char ***)(me + 0x1b8) : NULL;
        int hp = pd && !IsBadReadPtr(pd, 8) && *pd && !IsBadReadPtr(*pd, 0x148) ? *(int *)(*pd + 0x140) : 1;
        if (first) ended = 0, alive_seen = 0;
        if (hp > 0 && pd) alive_seen = 1;
        if (g_join != 4 && hp <= 0 && alive_seen && !ended && side_count(0) == 0 && side_count(1) == 0) {
            ended = GetTickCount() | 1;
            ((void (*)(void))(g_base + 0x6af8f0))();
            char *gm = *(char **)(g_base + 0x3c13258);
            logf("day flow: solo death: expedition end requested (gm+0xaf1 = %u): back to the Roundtable Hold", gm ? (unsigned char)gm[0xaf1] : 0);
        }
    }
    // an ally's final death (downed, then finished or bled out: +0x710 1 -> 0): home, it may join again
    int down = me && !IsBadReadPtr(me, 0x740) ? (unsigned char)me[0x710] : -1;
    if (g_join == 4 && g_as_ally && down_last == 1 && down == 0) join_over("ally's final death");
    down_last = down;
}

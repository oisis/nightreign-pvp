// Day flow (boss-flow 4, the owner's design of 2026-11-18): the day is free play; the night comes when the squad
// uses our item "Challenge the Night" (goods 1291, sold by the field merchant), which works once 6 bosses and the
// region's special boss (the Mountaintop's dragon or the Rotted Woods' Captain) are dead; on day 2 min(6, bosses still
// alive at its start) and both regions' special bosses (owner, 2026-11-21; df_day2_count). Enemy tier (owner,
// 2026-11-21, df_target_tier): day 1 every ordinary boss past the 6th +10 % HP and damage, day 2 every ordinary boss past
// the day's need +15 %, added; special bosses never; it is kept to the end (night bosses, the final boss). Rows (launcher,
// day_flow_rows): SpEffect 90071612 = the item's use marker (1 s), 90071700 + k = an enemy tier of +5 % * k.
// Every machine counts the bosses itself (death flags, synced; flag_note -> df_kill) and so has the same tier.
#define DF_GOODS        1291
#define DF_MARKER       90071612
#define DF_TIER_FIRST   90071700
#define DF_TIER_COUNT   200
#define DF_BOSSES       6
#define LOBBY_CHAL_KEY  "nrpvp_chal"   // host lobby: the challenge's number this expedition ("1", "2"); members: the same as member data
// The special bosses, one per region (owner, 2026-11-19; death flags, 2003[69] after death through common_func 90015002 /
// 90015407; see 08-flagi-gry): the Mountaintop's ice dragon (m60_42_38_10, 42_39_10, 43_39_10), the Rotted Woods'
// Captain (m60_45_36_30), the Crater's Magma Wyrm (m60_44_38_20), Noklateo's Naturalborn of the Void (m60_42_36_50).
// Only the two regions of this map's pair (g_pair, from the pattern) count; the base map has none (owner, 2026-11-21:
// its challenge needs the 6 bosses only).
static const unsigned DF_SPECIAL_MT[] = {1028100800, 1029100800, 1039100800, 1039100801}, DF_SPECIAL_RW[] = {1056300800},
                      DF_SPECIAL_CR[] = {1048202800}, DF_SPECIAL_NK[] = {1026500800};
static const char *const DF_REGION_NAME[4] = {"Mountaintop", "Crater", "Rotted Woods", "Noklateo"};
static int df_region(int which, const unsigned **flags) {  // which 0: the base region (MT / CR), 1: the added (RW / NK); its flag count
    int p = g_pair, crater = p == 2 || p == 3, nk = p == 1 || p == 3;
    if (p < 0) return *flags = NULL, 0;  // the base map
    if (which == 0) return *flags = crater ? DF_SPECIAL_CR : DF_SPECIAL_MT, crater ? 1 : 4;
    return *flags = nk ? DF_SPECIAL_NK : DF_SPECIAL_RW, 1;
}
static int df_special(unsigned id) {
    for (int w = 0; w < 2; w++) {
        const unsigned *f;
        for (int i = 0, n = df_region(w, &f); i < n; i++) if (id == f[i]) return 1;
    }
    return 0;
}
// Day 2 (owner, 2026-11-21): both regions' special bosses must be dead, on day 1 or day 2. Read from the death flags
// themselves (bosses never respawn, their flags only go on): a region's is dead when any of its flags is on (the
// Mountaintop's one dragon has four). Bit 0 the base region, bit 1 the added one.
static int df_specials_dead(void) {
    char *man = flag_man();
    int dead = 0;
    for (int w = 0; w < 2 && man; w++) {
        const unsigned *f;
        for (int i = 0, n = df_region(w, &f); i < n; i++) if (flag_state_in(man, f[i]) == 1) dead |= 1 << w;
    }
    return dead;
}
static volatile LONG g_df_kills, g_df_special_kills;
static int g_df_bosses, g_df_special, g_df_met, g_df_met_at, g_df_after, g_df_day = 1, g_df_go, g_df_used;
// the ordinary bosses killed on day 1 (g_df_killed1: before and after the condition; every boss-death flag the day
// counted, so event bosses too: an error of 1-2 is accepted, owner 2026-11-21) and on day 2 (g_df_killed2)
static int g_df_killed1, g_df_killed2;
static int g_df_tier;  // percent: 0, 5, 10 ...
// The day's target (owner, 2026-11-21): day 1 DF_BOSSES ordinary bosses and one special boss (g_df_special: killed this
// day); day 2 g_df_need = min(DF_BOSSES, ordinary bosses alive at its start) and both special bosses (g_df_special: how
// many are dead, from their flags).
static int g_df_need = DF_BOSSES;
static int df_need(void) { return g_df_day == 2 ? g_df_need : DF_BOSSES; }
static int df_special_ok(void) { return g_pair < 0 || g_df_special >= (g_df_day == 2 ? 2 : 1); }  // the base map: none needed
static DWORD g_df_notice_due;  // the progress notice, 6 s after a kill
static void df_kill(unsigned id) {  // flag_note, any thread: a boss death flag turned on
    if (df_special(id)) { InterlockedIncrement(&g_df_special_kills); return; }
    InterlockedIncrement(&g_df_kills);
}
static int df_on(void) { return g_settings[S_BOSS_FLOW] == 4; }
static int df_target_tier(void) {
    int t = 10 * (g_df_killed1 > DF_BOSSES ? g_df_killed1 - DF_BOSSES : 0);
    if (g_df_day == 2) t += 15 * (g_df_killed2 > g_df_need ? g_df_killed2 - g_df_need : 0);
    return t > DF_TIER_COUNT * 5 ? DF_TIER_COUNT * 5 : t;
}
// After a kill (owner, 2026-11-21): the condition newly met -> 131182 (challenge available); else the enemy tier went
// up -> 131193 ("+N% (total T%)", N the rise since the last notice); else the condition met before -> 131182 again;
// M/M without the special boss(es) -> 131180 ("M/M. Defeat the special region boss"); otherwise -> 131181 ("N/M"); M the
// day's target (df_need). The game keeps a summon message up about 5.9 s by itself (run of dev.60: 5924 ms).
static int g_df_told_met, g_df_told_tier;  // the condition / the tier as the last notice told them
static void df_progress_notice(void) {
    char arg[24];
    int m = df_need(), tier = df_target_tier();
    snprintf(arg, sizeof arg, "%d/%d", g_df_bosses < m ? g_df_bosses : m, m);
    if (g_df_met && !g_df_told_met) notice_with(131182, NULL);
    else if (tier > g_df_told_tier) snprintf(arg, sizeof arg, "%d%%|%d%%", tier - g_df_told_tier, tier), notice_with(131193, arg);
    else if (g_df_met) notice_with(131182, NULL);
    else notice_with(g_df_bosses >= m && !df_special_ok() ? 131180 : 131181, arg);
    g_df_told_met = g_df_met, g_df_told_tier = tier;
}
static void df_check_met(const char *when) {
    if (g_df_met || g_df_bosses < df_need() || !df_special_ok()) return;
    g_df_met = 1, g_df_met_at = g_df_bosses + 1;  // the special boss counts as one more for the tier
    logf("day flow: condition met %s (day %d: %d of %d bosses, special bosses dead %d)", when, g_df_day, g_df_bosses, df_need(), g_df_special);
}
// Day 2's start (7505): alive = this pattern's ordinary bosses (launcher: pattern-bosses.txt, "pattern count") minus
// No line for the pattern: 6.
static void df_day2_count(void) {
    char *gm = *(char **)(g_base + 0x3c13258), path[MAX_PATH], line[128];
    unsigned pat = gm ? *(unsigned *)(gm + 0xb4c) : 0xffffffffu, p_;
    next_to_dll(path, sizeof path, "pattern-bosses.txt");
    FILE *f = fopen(path, "r");
    int total = -1, n, have_file = f != NULL;
    while (f && total < 0 && fgets(line, sizeof line, f))
        if (sscanf(line, "%u %d", &p_, &n) == 2 && p_ == pat) total = n;
    if (f) fclose(f);
    int alive = total - g_df_killed1 > 0 ? total - g_df_killed1 : 0;
    g_df_need = total < 0 ? DF_BOSSES : alive < DF_BOSSES ? alive : DF_BOSSES;
    int dead = df_specials_dead(), p = g_pair;
    g_df_special = (dead & 1) + (dead >> 1 & 1);
    if (total < 0)
        logf("day flow: day 2: pattern %u not in pattern-bosses.txt (%s): needed %d (fallback); %d ordinary bosses killed on day 1", pat,
             have_file ? "no line" : "file missing", g_df_need, g_df_killed1);
    if (p < 0) logf("day flow: day 2: pattern %u (the base map), ordinary bosses on the map %d, alive %d (%d killed on day 1), needed %d; no special boss",
                    pat, total, total < 0 ? -1 : alive, g_df_killed1, g_df_need);
    else logf("day flow: day 2: pattern %u, ordinary bosses on the map %d, alive %d (%d killed on day 1), needed %d; specials: %s %s, %s %s", pat,
              total, total < 0 ? -1 : alive, g_df_killed1, g_df_need, DF_REGION_NAME[p == 2 || p == 3], dead & 1 ? "dead" : "alive",
              DF_REGION_NAME[2 + (p == 1 || p == 3)], dead & 2 ? "dead" : "alive");
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
// They are the camp scripts' from the start (df_camp_control); we only find and log them.
static char *g_df_inv_rows[10];
static int g_df_inv_ready, g_df_camps, g_df_kills_day;
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
// Grouping, from the generator (0x1406bb760, the camp roll): for each camp it chose, it pairs an
// invader row of SmallbaseInvationNpcParam with an entity id made by 0x140207020(small map, instance, k):
// (instance * 10000 + map) * 10000 + 50 + k (map * 10 if below 1000), k = 0, 1 (at most 2 invaders a camp, from
// param 0xca). So a camp is a placed small map (LotResultSmallBaseAndSpot of this map's pattern: +4 pattern, +0xc
// map, +0x10 instance), and its invaders are the characters the world character manager's lookup (0x5190c0, the
// one the scripts use) finds under entity ids 50 / 51 / 52 of it. An invader is grouped as soon as one of those ids
// resolves to it; camps are numbered in the order their first invader is grouped.
// vetoed: we turned the camp's second invader off (the script's arrival turned it on before the fifth boss);
// seen: the last logged state (bit 31 set once logged).
static struct { char *chr; int camp, entity, vetoed; unsigned seen; } g_df_inv[16];
static int g_df_second;  // the camps' second invaders are let in (the day's fifth boss; then for the expedition)
static int g_df_ninv, g_df_camp_key[8];
static int df_invader_entity(char *chr, int *key) {
    char *lots = df_param("LOT_RESULT_SMALLBASE_AND_SPOT_ST"), *gm = *(char **)(g_base + 0x3c13258);
    char *wcm = *(char **)(g_base + RVA_WORLD_CHR_MAN);
    if (!lots || !gm || !wcm) return 0;
    unsigned pats[2] = {*(unsigned *)(gm + 0xb48), *(unsigned *)(gm + 0xb4c)};
    int n = *(unsigned short *)(lots + 0xa);
    for (int i = 0; i < n; i++) {
        char *r = lots + *(unsigned long long *)(lots + 0x40 + i * 0x18 + 8);
        unsigned pat = *(unsigned *)(r + 4);
        if (pat != pats[0] && pat != pats[1]) continue;
        int map = *(int *)(r + 0xc), inst = (unsigned char)r[0x10];
        if (map <= 0 || map >= 10000 || inst >= 10) continue;
        int base = (inst * 10000 + (map < 1000 ? map * 10 : map)) * 10000;
        for (int k = 0; k < 3; k++) {
            int e = base + 50 + k;
            if (((char *(*)(void *, int *))(g_base + 0x5190c0))(wcm, &e) == chr) return *key = base, e;
        }
    }
    return 0;
}
static int df_chr_pos(char *chr, float out[3]);
static void df_camp_control(char *chr) {
    int i = 0;
    while (i < g_df_ninv && g_df_inv[i].chr != chr) i++;
    if (i == g_df_ninv) {
        if (g_df_ninv == 16) return;
        g_df_inv[g_df_ninv].chr = chr, g_df_inv[g_df_ninv].camp = -1, g_df_inv[g_df_ninv].entity = 0, g_df_inv[g_df_ninv].vetoed = 0,
        g_df_inv[g_df_ninv].seen = 0, g_df_ninv++;
        logf("day flow: invader %d found (chr %p, block %08x); its camp is looked up by entity id", i + 1, (void *)chr, *(unsigned *)(chr + CHR_BLOCK_ID));
    }
    int camp = g_df_inv[i].camp, key = 0;
    // Only an invader with SpEffect 4450 is one of the camps the generator rolled: the camp script (90015310) turns
    // on only one that has it. Run of dev.60: m38_10 #0's +50 carries NpcParam 600030010 in the map file itself
    // (m30 / m32 / m34 and m38's +51 have placeholders the roll fills), never got 4450 and never arrived; it is no
    // camp of this expedition. Such a character is left alone (and logged once) until it gets 4450.
    if (camp < 0 && !speffect_on(chr, 4450)) {
        if (!(g_df_inv[i].seen & 0x40000000u)) {
            g_df_inv[i].seen |= 0x40000000u;
            int k2 = 0, e2 = df_invader_entity(chr, &k2);
            logf("day flow: invader %d (entity %d): no SpEffect 4450, not one of the rolled camps (its camp script keeps it away)", i + 1, e2);
        }
        return;
    }
    if (camp < 0) {
        int e = df_invader_entity(chr, &key);
        if (e) {
            for (int c = 0; c < g_df_camps && camp < 0; c++)
                if (g_df_camp_key[c] == key) camp = c;
            if (camp < 0 && g_df_camps < 8) {
                camp = g_df_camps++, g_df_camp_key[camp] = key;
                logf("day flow: camp %d is small map m%02d_%02d instance %d (entities %d..); its invader is left to the camp's script",
                     camp + 1, key / 10000 % 10000 / 100, key / 10000 % 100, key / 100000000, key + 50);
            }
            g_df_inv[i].camp = camp, g_df_inv[i].entity = e;
            logf("day flow: invader %d: entity %d, camp %d", i + 1, e, camp + 1);
        }
    }
    // Owner, 2026-11-19: "1 od poczatku, 2 po zabiciu 5 bossow". Every camp has two invaders (+50, +51; the launcher
    // makes the 1-invader caves 2, camp_variation_rows / build_camp_scripts). The camp's own script (common 90015310)
    // brings them in: it keeps them off, collision off, until a player enters one of the camp's areas or flag
    // base+200 is on; then it sets base+200, warps both to their arrival points and turns them on with collision and
    // the arrival animation 63020 (only one with SpEffect 4450). A non-"Hot Spot" instance it turns off and
    // backread-removes instead. The first (+50) is the script's alone. The second (+51) we turn off again while the
    // day has fewer than 5 bosses (from then on for the rest of the expedition); one we turned off after the arrival
    // is turned on again then, as the script left it, unless dead (base+202+k).
    if (!g_df_second && g_df_kills_day >= 5) g_df_second = 1, logf("day flow: 5 bosses this day: the camps' second invaders are let in");
    int base = camp >= 0 ? g_df_camp_key[camp] : 0, k = g_df_inv[i].entity - base - 50, open = k != 1 || g_df_second;
    int arrived = base ? two_flag(base + 200, 0, 0) : -1, dead = base && k >= 0 ? two_flag(base + 202 + k, 0, 0) : -1;
    int hp = chr_hp(chr);
    if (!open && chr_enabled(chr)) {
        ((char (*)(char *, char))(g_base + 0x608c60))(chr, 0);
        if (!g_df_inv[i].vetoed) logf("day flow: invader %d (entity %d): the second of camp %d, turned on before the fifth boss: off again",
                                      i + 1, g_df_inv[i].entity, camp + 1);
        g_df_inv[i].vetoed = 1;
    } else if (open && g_df_inv[i].vetoed) {
        g_df_inv[i].vetoed = 0;
        if (arrived == 1 && dead != 1 && hp != 0 && !chr_enabled(chr)) {
            ((char (*)(char *, char))(g_base + 0x608c60))(chr, 1);
            logf("day flow: invader %d (entity %d): turned on again (the camp's arrival ran before the fifth boss)", i + 1, g_df_inv[i].entity);
        }
    }
    int on = chr_enabled(chr), sp = speffect_on(chr, 4450);
    unsigned state = 1u << 31 | (unsigned)on | (unsigned)(arrived & 3) << 1 | (unsigned)(dead & 3) << 3 | (unsigned)sp << 5 |
                     (unsigned)(hp > 0) << 6 | (unsigned)open << 7;
    if (state != g_df_inv[i].seen) {
        g_df_inv[i].seen = state;
        float pos[3] = {0};
        int have = df_chr_pos(chr, pos);
        logf("day flow: invader %d (entity %d, camp %d %s): enabled %d, arrival flag %d=%d, death flag %d=%d, SpEffect 4450 %d, HP %d, "
             "position %s%.1f %.1f %.1f", i + 1, g_df_inv[i].entity, camp + 1, open ? "open" : "closed", on, base + 200, arrived,
             base + 202 + k, dead, sp, hp, have ? "" : "(not readable) ", pos[0], pos[1], pos[2]);
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
    // Proof of the scaling on the night boss (owner, 2026-11-19): a character in a night-boss arena with a big HP pool
    // (max HP above 3000; the arena's adds are below) is logged once per challenge, with its NpcParam row id, whether
    // it carries the tier SpEffect now and its HP data (module [chr +0x1b8] +0x140 HP, +0x144 / +0x148 raw: max HP as
    // the game keeps it, its base).
    if (night_boss_map && g_df_go) {
        static char *seen[16];
        static int nseen, for_seq = -1;
        if (for_seq != g_df_day) for_seq = g_df_day, nseen = 0;
        char **pd = !IsBadReadPtr(chr + 0x1b8, 8) ? *(char ***)(chr + 0x1b8) : NULL;
        char *m = pd && !IsBadReadPtr(pd, 8) ? *pd : NULL;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == chr;
        if (!known && nseen < 16 && m && !IsBadReadPtr(m + 0x140, 12) && *(int *)(m + 0x144) > 3000) {
            seen[nseen++] = chr;
            unsigned npc = 0;
            if (!IsBadReadPtr(*(void **)chr, 0x1a8)) {
                char **ref = ((char **(*)(char *))(*(void ***)chr)[0x1a0 / 8])(chr);
                if (ref && !IsBadReadPtr(ref, 12)) npc = *(unsigned *)(ref + 1);
            }
            logf("day flow: night boss seen (block %08x, NpcParam %u): tier SpEffect %d %s, HP %d, +0x144 %d, +0x148 %d", *(unsigned *)(chr + CHR_BLOCK_ID),
                 npc, want ? DF_TIER_FIRST + want : 0, want ? (have ? "already on" : "put on now") : "(none wanted)", *(int *)(m + 0x140),
                 *(int *)(m + 0x144), *(int *)(m + 0x148));
        }
    }
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
// The warp fails when the destination is not in the point manager (0x14060b540 returns 0 when
// 0x1405fc6f0([0x143c11848], entity), a std::map lookup by entity id, gives nothing): deck test of 4665420, final
// circle at m60_45_39, the player elsewhere. It is retried every second for 2 minutes (the point may appear as the
// player nears the circle); every attempt says whether the point was found.
static int df_point_known(int entity) {
    char *pm = *(char **)(g_base + 0x3c11848);
    return pm && !IsBadReadPtr(pm + 0x10, 8) && ((char *(*)(char *, unsigned))(g_base + 0x5fc6f0))(pm, (unsigned)entity) != NULL;
}
// Small bases against the world (run of dev.67, pair 3, pattern 29: the map showed the church at attach point 108,
// m60_44_36, and the owner found none): every 60 s, the pattern's small bases whose attach point (SmallBaseAndSpot-
// AttachPoint +0x14, its entity) the point manager does not know, with their tile and small map. A point is known only
// while its map file is loaded, so the line also lists the player's block; the ones near the player are what counts.
static void df_bases_check(void) {
    static DWORD next;
    if ((int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 60000;
    char *lots = df_param("LOT_RESULT_SMALLBASE_AND_SPOT_ST"), *points = df_param("SMALLBASE_AND_SPOT_ATTACH_POINT_ST");
    char *gm = *(char **)(g_base + 0x3c13258), *me = local_player();
    if (!lots || !points || !gm) return;
    unsigned pat = *(unsigned *)(gm + 0xb4c);
    char line[700];
    int at = 0, missing = 0, all = 0;
    for (int i = 0; i < *(unsigned short *)(lots + 0xa) && at < 640; i++) {
        char *r = lots + *(unsigned long long *)(lots + 0x40 + i * 0x18 + 8);
        if (*(unsigned *)(r + 4) != pat) continue;
        int ap = *(int *)(r + 8), map = *(int *)(r + 0xc);
        char *pt = df_param_row(points, ap);
        if (!pt || map >= 4000) continue;  // the sites (forts, churches, camps, ruins; not the spots)
        all++;
        if (df_point_known(*(int *)(pt + 0x14))) continue;
        missing++;
        at += snprintf(line + at, sizeof line - (size_t)at, " %d(m%02d_%02d, %u_%u)", ap, map / 100, map % 100, (unsigned char)pt[5],
                       (unsigned char)pt[6]);
    }
    logf("small bases: pattern %u: %d of %d sites with their point not known now (player block %08x):%s", pat, missing, all,
         me ? *(unsigned *)(me + CHR_BLOCK_ID) : 0, missing ? line : " none");
}
// Diagnostic for the crash at world teardown (deck-run3b, deck-run4: 0x14214e298 from 0x1406420d9): the
// WorldGridAreaInfo destructor 0x140641f70 fires DL_PANIC "WorldGridAreaInfo::m_smallBaseBlocks" (leak) when its
// small-base multimap is not empty (+0xd0 head, +0xd8 size; node +0x20 key, +0x28 entry, entry[0] the small base's
// block info, +8 its block id). Entries come only from 0x1406427b0 (the tile loader's MSB pass, at state 2 -> 3) and
// go per key in 0x140645db0, called with a tile's block info (+0x28 its grid area, +0x34 the key) at its unload (state
// 5) and in the WorldBlockResBase destructor 0x1406270f0, which runs for every resident tile (world +0x8870, 0x510
// each, count +0x8868; +8 the block info, +0x38 the state) before the grid areas (0x140683d30). So an entry whose key
// no resident tile of that grid area has is never erased: such "orphans" are logged with their small base and key, on
// every change, so the line before a crash names the one left (world = [0x143c123f8] +0x10, grid areas +0x20, 0xe0
// each, count +0x18).
static void df_sb_registry_frame(int in_world) {
    static DWORD next;
    static unsigned last_sig = 0xffffffff;
    static int was_in = -1;
    if (in_world != was_in) was_in = in_world, last_sig = 0xffffffff;  // the state at each change is logged
    if (in_world && (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 5000;
    char *mgr = *(char **)(g_base + 0x3c123f8), *w = mgr && !IsBadReadPtr(mgr + 0x10, 8) ? *(char **)(mgr + 0x10) : NULL;
    if (!w || IsBadReadPtr(w, 0x8878)) return;
    int areas = *(int *)(w + 0x18), nres = *(int *)(w + 0x8868);
    char *ga = *(char **)(w + 0x20), *res = *(char **)(w + 0x8870);
    if (areas <= 0 || areas > 10 || !ga || IsBadReadPtr(ga, areas * 0xe0) || nres < 0 || nres > 128 || (nres && IsBadReadPtr(res, nres * 0x510))) return;
    char line[700];
    int at = 0, total = 0, orphans = 0;
    unsigned sig = 0;
    for (int a = 0; a < areas; a++) {
        char *ai = ga + a * 0xe0, *head = *(char **)(ai + 0xd0);
        if (!head || IsBadReadPtr(head, 0x30)) continue;
        int guard = 0;
        for (char *n = *(char **)head; n && n != head && guard < 512; guard++) {  // in order: from the leftmost
            if (IsBadReadPtr(n, 0x30)) break;
            unsigned key = *(unsigned *)(n + 0x20);
            char **e = *(char ***)(n + 0x28), *sb = e && !IsBadReadPtr(e, 8) ? *e : NULL;
            unsigned sb_id = sb && !IsBadReadPtr(sb + 8, 4) ? *(unsigned *)(sb + 8) : 0;
            int st = -1, other = 0;
            for (int r = 0; r < nres; r++) {
                char *bi = *(char **)(res + r * 0x510 + 8);
                if (!bi || IsBadReadPtr(bi, 0x38) || *(unsigned *)(bi + 0x34) != key) continue;
                if (*(char **)(bi + 0x28) == ai) st = *(int *)(res + r * 0x510 + 0x38);
                else other = 1;
            }
            total++;
            if (st < 0) {
                orphans++, sig = sig * 31 + key + sb_id;
                if (at < 620)
                    at += snprintf(line + at, sizeof line - (size_t)at, " [area %d key %08x small base %08x%s]", a, key, sb_id,
                                   other ? ", key resident in another grid area" : "");
            }
            if (*(char *)(*(char **)(n + 0x10) + 0x19) == 0) {  // successor: the right subtree's leftmost
                n = *(char **)(n + 0x10);
                while (*(char *)(*(char **)n + 0x19) == 0) n = *(char **)n;
            } else {  // or up while we come from the right
                char *p = *(char **)(n + 8);
                while (*(char *)(p + 0x19) == 0 && n == *(char **)(p + 0x10)) n = p, p = *(char **)(p + 8);
                n = p;
            }
        }
    }
    sig = sig * 31 + (unsigned)orphans;
    if (sig == last_sig) return;
    last_sig = sig;
    logf("small-base registry (%s): %d entries in %d grid area(s), %d resident tile(s); %d with no resident tile of their key (never erased: the teardown leak check fires on them)%s",
         in_world ? "in the world" : "out of the world", total, areas, nres, orphans, orphans ? line : "");
}
// The game's own way to an unloaded spot: the death restart (CS::CSLuaEventScriptImitation::CSDeathRestartEvent,
// 0x1405c38f0 -> states 0x1405c3670 / 0x1405c37b0 / 0x1405c4360) asks the world for the respawn block when it is not
// loaded (0x140639150(mgr [0x143c123f8], &block) false): 0x140639760(mgr, &block, &pos, &rot) with the default
// position / rotation (0x143ab9b10 / 0x1446a9490), waits until no load is pending ([mgr +0x10] +0x8880 == -1, 0x140639220)
// and then places the character. The same queue here, without the death and its level setter (0x140729200, the revive's
// first call): the final circle's tile block as laid out (flag manager's tile list +0x118, the variant included); once
// the load is done and the site's point is known, the warp below puts the player on it.
// The queued position is not the default origin but DF_LOAD_Y above it. 0x140638d50 applies it at once with the setter
// 0x1403daa70, in the arena tile's frame (every run: "before 0.0 -4.4..-11.4 0.0" right after, and the site then read
// at about (0, ground, 0)), and the player waits there until the point is known. At y 0 he is under the ground; from an
// m60 tile he only falls, but from the cave m30_30 (block 1e1e0111, deck 0.2.8 day 2) he had HP 0 before the landing
// took effect, most likely a death collision (hit kind 0xf: 0x140461650 -> 0x140441ee0, the instant fall death that
// zeroes HP; unproven which part). Above every site seen (ground y 64.8..191.3) he touches nothing in that time.
#define DF_LOAD_Y 400.0f
static int df_hp(char *me) {  // -1: not readable
    char **pd = me && !IsBadReadPtr(me + 0x1b8, 8) ? *(char ***)(me + 0x1b8) : NULL;
    return pd && !IsBadReadPtr(pd, 8) && *pd && !IsBadReadPtr(*pd, 0x148) ? *(int *)(*pd + 0x140) : -1;
}
static int df_arena_block(unsigned *block) {
    char *pa = *(char **)(g_base + RVA_PLAY_AREA_PTR), *man = *(char **)(g_base + 0x3c115a8);
    if (!pa || !man) return 0;
    unsigned char *fin = (unsigned char *)(pa + 0x190 + 0x20 * 2);
    for (char *e = *(char **)(man + 0x118); e && e < *(char **)(man + 0x120); e += 8) {
        unsigned m = *(unsigned *)e;
        if (m >> 24 == fin[0] && (m >> 16 & 0xff) == fin[1] && (m >> 8 & 0xff) == fin[2]) return *block = m, 1;
    }
    return 0;
}
static DWORD g_df_load_until;  // waiting for the arena block's load (0: not waiting)
static int df_arena_load(void) {
    char *mgr = *(char **)(g_base + 0x3c123f8);
    unsigned block;
    if (!mgr || !df_arena_block(&block)) return logf("day flow: arena load: no world manager or no laid-out tile for the final circle"), 0;
    unsigned b = block;
    if (((char (*)(char *, unsigned *))(g_base + 0x639150))(mgr, &b)) return logf("day flow: arena block %08x already loaded", block), 1;
    b = block;
    _Alignas(16) float pos[4] = {0.0f, DF_LOAD_Y, 0.0f, 1.0f};
    ((void (*)(char *, unsigned *, void *, void *))(g_base + 0x639760))(mgr, &b, pos, (void *)(g_base + 0x46a9490));
    char *me = local_player();
    if (me) ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(me, 90071613, 0);  // no fall damage (15 s), as after the warp
    char *rp = *(char **)(g_base + 0x3c17c88);
    logf("day flow: arena block %08x not loaded: load queued as the death restart does, at 0 %.0f 0 of the block (respawn manager's block %08x; "
         "player block %08x, HP %d)", block, DF_LOAD_Y, rp ? *(unsigned *)(rp + 0x18) : 0, me ? *(unsigned *)(me + CHR_BLOCK_ID) : 0, df_hp(me));
    return 1;
}
static int df_load_pending(void) {
    char *mgr = *(char **)(g_base + 0x3c123f8), *w = mgr ? *(char **)(mgr + 0x10) : NULL;
    return !w || IsBadReadPtr(w + 0x8880, 4) || *(int *)(w + 0x8880) != -1 || *(int *)(mgr + 0xf0) != -1;
}
// After the warp (deck test of 9330160: the player fell through the ground 8 s after a successful warp, at the
// arena block's origin height). Three things:
// 1. the load we queued (world manager +0xf0 block, +0x100 position = (0, DF_LOAD_Y, 0, 1), above the block's origin)
//    is cleared if still pending: 0x140638d50 applies a pending entry by setting the local player's position to it
//    (0x1403daa70 ...), which would put him above the centre of the arena tile;
// 2. our no-fall-damage SpEffect 90071613 (15 s, fallDamageRate +0xdc = 0, launcher);
// 3. the owner's landing: the site's own position (the point's vfunc +0x18, its rotation +0x20, as 0x14060b540 takes
//    them) with y + DF_DROP_M, set with the same setter 0x1403daa70, so the player falls onto the arena.
#define DF_DROP_M 100.0f
static int df_chr_pos(char *chr, float out[3]) {  // the game's position getter 0x3d0fd0, as player_position
    char *ctrl = chr && !IsBadReadPtr(chr + 0x60, 8) ? *(char **)(chr + 0x60) : NULL;
    char *a = ctrl && !IsBadReadPtr(ctrl + 0x10, 8) ? *(char **)(ctrl + 0x10) : NULL;
    char *b = a && !IsBadReadPtr(a + 0x1b8, 8) ? *(char **)(a + 0x1b8) : NULL;
    if (!b || IsBadReadPtr(b + 0x68, 8) || IsBadReadPtr(*(char **)(b + 0x68), 0x80)) return 0;
    _Alignas(16) float v[4];
    ((void *(*)(void *, void *))(g_base + 0x3d0fd0))(chr, v);
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    return 1;
}
// The challenge's move is hidden behind the game's own fade (owner, 2026-11-19: the move through walls looked bad). As
// the scripts do it (common_func 90075770: Fade to Black 1.0 / 0.5 s / freeze, 0.5 s, the warp, ..., Fade to Black
// 0.0 / 0.5 s): 2004[77] (bank 2004 table 0x14058aa04, case 0x1405891e4) takes the fade colour 0x14015de60(out, 8)
// and calls 0x14061d490(*(0x143c117c8)+8, {r, g, b, ratio}, time, freeze, delay), the delay kept only when
// 0 <= delay < time (else time). The screen goes black first, the warp follows 0.55 s later, and it clears 0.4 s
// after the landing is set (the drop onto the arena is seen), or after 20 s at the latest.
static DWORD g_df_fade_in_at, g_df_move_at;
static void df_fade(float ratio, float time, int freeze) {
    char *m = *(char **)(g_base + 0x3c117c8);
    if (!m || IsBadReadPtr(m + 8, 8) || !*(char **)(m + 8)) return (void)logf("day flow: fade: no fade manager");
    _Alignas(16) float col[4] = {0, 0, 0, 0}, rgba[4];
    ((void (*)(float *, int))(g_base + 0x15de60))(col, 8);
    rgba[0] = col[0], rgba[1] = col[1], rgba[2] = col[2], rgba[3] = ratio;
    ((void (*)(char *, float *, float, unsigned char, float))(g_base + 0x61d490))(*(char **)(m + 8), rgba, time, (unsigned char)(freeze != 0), time);
    logf("day flow: fade %s (%.1f s)", ratio > 0 ? "to black" : "back", time);
}
static DWORD g_df_track_until, g_df_track_next;
static void df_after_warp(char *me, int dest) {
    char *mgr = *(char **)(g_base + 0x3c123f8);
    if (mgr && *(int *)(mgr + 0xf0) != -1) {
        logf("day flow: queued arena load still pending after the warp (block %08x): cleared", *(unsigned *)(mgr + 0xf0));
        *(int *)(mgr + 0xf0) = -1;
    }
    ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(me, 90071613, 0);
    char *pm = *(char **)(g_base + 0x3c11848);
    char **pt = pm ? ((char **(*)(char *, unsigned))(g_base + 0x5fc6f0))(pm, (unsigned)dest) : NULL;
    if (!pt || IsBadReadPtr(pt, 8) || IsBadReadPtr(*(void **)pt, 0x28)) {
        logf("day flow: landing: site point not found after the warp, left where the warp put him");
    } else {
        _Alignas(16) float pos[4], rot[4];
        ((void (*)(char **, float *))(*(void ***)pt)[0x18 / 8])(pt, pos);
        memcpy(rot, pos, sizeof rot);  // as 0x14060b540: vfunc +0x20 is called on a copy of the position
        ((void (*)(char **, float *))(*(void ***)pt)[0x20 / 8])(pt, rot);
        pos[3] = 1.0f, rot[3] = 0.0f;
        float site_y = pos[1];
        pos[1] += DF_DROP_M;
        ((void (*)(char *, float *, float *))(g_base + 0x3daa70))(me, pos, rot);
        logf("day flow: landing: set to the site %.1f %.1f %.1f + %.0f m up (the point's own position, setter 0x1403daa70)", pos[0], site_y, pos[2],
             DF_DROP_M);
    }
    g_df_track_until = GetTickCount() + 10000, g_df_track_next = 0;
    if (g_df_fade_in_at) g_df_fade_in_at = (GetTickCount() + 400) | 1;
}
static void df_track_frame(void) {
    if (!g_df_track_until) return;
    char *me = local_player(), *mgr = *(char **)(g_base + 0x3c123f8);
    int hp = df_hp(me);
    if ((int)(GetTickCount() - g_df_track_until) >= 0) {  // the verdict, 10 s after the landing
        unsigned arena = 0, at = me ? *(unsigned *)(me + CHR_BLOCK_ID) : 0;
        df_arena_block(&arena);
        g_df_track_until = 0;
        logf("day flow: warp check: block %08x, arena block %08x: %s; HP %d: %s", at, arena, at == arena ? "in the arena" : "NOT in the arena", hp,
             hp > 0 ? "alive" : "DEAD");
        return;
    }
    if ((int)(GetTickCount() - g_df_track_next) < 0) return;
    g_df_track_next = GetTickCount() + 500;
    float p[3];
    if (me && df_chr_pos(me, p))
        logf("day flow: after the warp: block %08x position %.1f %.1f %.1f, HP %d, downed %u, queued load %s", *(unsigned *)(me + CHR_BLOCK_ID), p[0],
             p[1], p[2], hp, (unsigned char)me[0x710], mgr && *(int *)(mgr + 0xf0) != -1 ? "pending" : "none");
}
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
    int who = 10000, dest = *(int *)(pt + 0x14), known = df_point_known(dest);
    float before[3] = {0, 0, 0};
    int had = df_chr_pos(me, before);
    unsigned char got = known ? (unsigned char)((unsigned long long (*)(int *, int *, char))(g_base + 0x5f1a00))(&who, &dest, 1) : 0;
    logf("day flow: warp to the night boss's site (circle row %d, attach point %d, entity %d): point %s, %s; before: %s%.1f %.1f %.1f, block %08x, HP %d",
         row_id, *(int *)(row + 0x24), dest, known ? "known" : "not in the point manager", got ? "done" : "not done", had ? "" : "(none) ", before[0],
         before[1], before[2], *(unsigned *)(me + CHR_BLOCK_ID), df_hp(me));
    if (got) df_after_warp(me, dest);
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
static DWORD g_df_warp_until;  // retrying the warp until then (0: done or given up)
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
    g_df_warp_until = 0, g_df_load_until = 0;
    df_fade(1.0f, 0.5f, 1);
    g_df_move_at = (GetTickCount() + 550) | 1, g_df_fade_in_at = (GetTickCount() + 20000) | 1;
    df_repass("challenge: night bosses get the tier");
}
static void df_move_frame(void) {  // the warp once the screen is black; the fade back
    if (g_df_move_at && (int)(GetTickCount() - g_df_move_at) >= 0) {
        g_df_move_at = 0;
        if (!df_warp()) {
            if (df_arena_load()) g_df_load_until = GetTickCount() + 30000;
            else g_df_warp_until = GetTickCount() + 120000;  // fallback: retry while the player nears the circle
        }
    }
    if (g_df_fade_in_at && (int)(GetTickCount() - g_df_fade_in_at) >= 0) g_df_fade_in_at = 0, df_fade(0.0f, 0.5f, 0);
}
static void df_item_used(void) {
    if (!g_df_met || g_df_used) {  // the item is not consumed (launcher: isConsume off), nothing to give back
        char arg[16];
        snprintf(arg, sizeof arg, "%d", df_need());
        if (g_df_used) notice_with(131184, NULL);
        else notice_with(g_pair < 0 ? 131192 : g_df_day == 2 ? 131191 : 131183, arg);
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
static int g_df_events, g_df_started;
static DWORD g_df_event_tick;
// From the day's challenge until day 2 (7505 clears g_df_go) no event starts here, and night_hide_frame hides the
// other bosses and the camp invaders. Ending a running event: the game's own end at night exists only for the
// night-boss interference family (common 1142: with 8061 on, at play-area time 23:00-23:59, it sets 8025 and 8062
// "finished"; its creatures, e.g. the Equilibrious Beast of m45_55 (common_func 90035247) and of the field tiles
// (90015163), get SpEffect 45831 at 23:00 and are disabled, 8110), which the challenge's jump to 23:00 triggers.
// The hordes' (90035040..48) and the meteorite's scripts have no time or night condition; their end flags 8024 /
// 8021 are the "cleared" flags that common 90015003 rewards (SpEffect 6999000), so they are not set here.
static int df_night_window(void) { return df_on() && g_df_go; }
static unsigned df_invader_entity_at(int i) { return i < g_df_ninv && g_df_inv[i].entity > 0 ? (unsigned)g_df_inv[i].entity : 0; }
static void df_night_events_log(const char *when) {
    static const int F[] = {8040, 8042, 8043, 8044, 8021, 8055, 8056, 8057, 8024, 8060, 8061, 8062, 8025, 8105, 8110, 8071, 8072, 8027};
    char t[256] = "";
    for (unsigned i = 0; i < sizeof F / sizeof *F; i++) {
        size_t n = strlen(t);
        snprintf(t + n, sizeof t - n, " %d=%d", F[i], bf_flag((unsigned)F[i]));
    }
    logf("day flow: %s: world event flags (meteorite 8040..8044 / 8021, hordes 8055..8057 / 8024, interference 8060..8062 / 8025, "
         "Libra 8105 / 8110, frenzy 8071 / 8072 / 8027):%s; our starter holds until day 2", when, t);
}
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
    if (g_df_go || g_df_events >= 3 || level < 5 || g_df_kills_day < 2 * (g_df_started + 1) || GetTickCount() - g_df_event_tick < 30000) return;
    int k = g_df_events++;
    g_df_event_tick = GetTickCount();
    // a family's scripts are in its own small maps (meteorite m20 / m21: one in the game's 19 meteorite patterns, at a
    // point 100-117; hordes m46_00..06; the special kinds m46_78 / m45_52 / 53 / 55): none on this map, no event
    static const struct { int lo, hi; } MAPS[3] = {{2000, 2199}, {4600, 4606}, {4500, 4699}};
    int present = 0;
    char *lots = df_param("LOT_RESULT_SMALLBASE_AND_SPOT_ST"), *gm = *(char **)(g_base + 0x3c13258);
    for (int i = 0; lots && gm && i < *(unsigned short *)(lots + 0xa) && !present; i++) {
        char *r = lots + *(unsigned long long *)(lots + 0x40 + i * 0x18 + 8);
        unsigned pat = *(unsigned *)(r + 4);
        int m = *(int *)(r + 0xc);
        if (pat != *(unsigned *)(gm + 0xb48) && pat != *(unsigned *)(gm + 0xb4c)) continue;
        if (k == 2) present = m == 4678 || m == 4552 || m == 4553 || m == 4555;
        else present = m >= MAPS[k].lo && m <= MAPS[k].hi;
    }
    if (!present) {
        logf("day flow: event %d (%s) skipped: its small maps are not on this map", k + 1, FAM[k].name);
        g_df_event_tick = 0;  // the next family at once, on the same 2 bosses
        return;
    }
    g_df_started++;
    df_net_flag(FAM[k].flag);
    logf("day flow: event %d started: %s (flag %d), level %d, %d bosses this day", k + 1, FAM[k].name, FAM[k].flag, level, g_df_kills_day);
}
// --- the final death (deck test of 07f56eb: ending at HP 0 came before the game's downed countdown). The game's own
// path after a death that is not revived: CS::CSLuaEventScriptImitation::CSDeathRestartEvent, whose first state
// 0x1405c38f0 sets the level (0x140729200: the death's -1) and brings the player back (the respawn block, then the
// placement). It is bound as a state at 0x1405c3ffa (lea rax, 0x1405c38f0, in 0x1405c3f80). Under boss-flow 4 that state
// is taken over: instead of the revive, our own player's expedition ends: the host (or solo) through the game's game
// over (0x6af900, gm+0xaf2), which ends it for the squad; an ally through join_over (it may join again).
// So the downed state and its countdown stay the game's; only what follows the death changes.
// The ending is the game's own game over: common.emevd 1180 / 1181 set flag 7001 (GameSystemCommonParam +0xfc) for a
// lost expedition, which MoveMapStep::_CheckEndingRequest (0xc37420) turns into gm+0xaf2 (0x6af900); +0xf8 = 7000, the
// game clear, gives gm+0xaf1 (0x6af8f0). Both lead to Result, through MoveMapStep::GameClearOrOver (0xc322e0, called
// from 0xc381e4 with (af1, af2)): with af1 the result is a clear (result +0x3c = 1 by 0x210c20, the reward 0x1fd620
// gets clear = 1, 0x2034f0 counts a won expedition: play state +0x170 wins, +0x174 the streak) - deck run 2: +0x170
// 55 -> 56, +0x174 1 -> 2 after a death. So the final death ends with af2.
// Extra lives, the game's own (common.emevd 1181 solo, 1180 squad): at a death at night (7515) or on day 3 (7510) the
// event revives the player in the near-death idle (540157), one life per death, in this order: 8970061 (an event's own
// revive, nothing used), 6999500 "[Solo] Automatic Revival Available" (given at the start of day 1 / day 2 in
// single-player only, common 1101 / 1104, with flag 82100) -> trigger 6999505 + 6999501 "Unavailable"; 540155, the
// carry effect of goods 700 Wending Grace (EquipParamGoods carrySpEffectId) -> trigger 540150 and the item removed;
// 6999100 "Favor of Noklateo" (m60_42_36_50) -> 6999101 "Unavailable" + its boosts 6999104 / 06 / 07, trigger 6999105.
// In the day the events do nothing: the game's day death is the death restart's revive, a level less. Under boss-flow 4
// that death is final, so an extra life is used up here as the events use it (without the triggers: they act in the
// near-death idle, which the death restart is past) and the game's own revive runs instead of the ending.
static int df_extra_life(char *me, int use) {
    static const struct { unsigned have, set[4]; int goods; const char *name; } L[3] = {
        {6999500, {6999501}, 0, "the solo automatic revival (6999500)"},
        {540155, {0}, 700, "Wending Grace (goods 700, 540155)"},
        {6999100, {6999101, 6999104, 6999106, 6999107}, 0, "the Favor of Noklateo (6999100)"}};
    for (int i = 0; me && i < 3; i++) {
        if (!speffect_on(me, L[i].have)) continue;
        if (!use) return i + 1;
        for (int k = 0; k < 4 && L[i].set[k]; k++) ((void (*)(void *, int, unsigned char))(g_base + RVA_ADD_SPEFFECT))(me, (int)L[i].set[k], 0);
        if (L[i].goods) {  // 2003[24]'s removal, as the challenge item's (0x5fdb10, goods code 0x59a7b0(3))
            int code = ((int (*)(int))(g_base + 0x59a7b0))(3);
            ((void (*)(int, int, int))(g_base + 0x5fdb10))(code, L[i].goods, -1);
        }
        logf("day flow: extra life used: %s; after it: 6999500 %d, 540155 %d, 6999100 %d", L[i].name, speffect_on(me, 6999500),
             speffect_on(me, 540155), speffect_on(me, 6999100));
        return i + 1;
    }
    return 0;
}
static int g_df_ended;
static char *g_df_inv_dead;  // the dead invader whose death restart is taken over
static void df_death_restart_hook(long long *ev, void *sm) {
    // An invader's death in our session (owner, 2026-11-21: its runes vanish, never a bloodstain): the restart is not run,
    // so neither its revive nor the bloodstain of its 0x1405c4360 state (0x1405c44b2 -> 0x140729920 -> 0x14072b070, packet
    // 0x7c); the invader goes home through join_over, which drops its items first (death_drop). Kept for that character
    // until it is out of the expedition (g_df_inv_dead, cleared by the step hook), so no later call runs the restart.
    char *dead = local_player();
    if ((g_join == 4 && !g_as_ally && chr_hp(dead) <= 0) || (dead && dead == g_df_inv_dead)) {
        if (dead != g_df_inv_dead || !dead)
            logf("day flow: the invader's death restart taken over (HP %d): no revive, no bloodstain", chr_hp(dead));
        g_df_inv_dead = dead;
        if (g_join == 4) join_over("invader died (the game's death restart)");
        return;
    }
    if (!df_on() || !g_in_exped) {
        ((void (*)(long long *, void *))(g_base + 0x5c38f0))(ev, sm);
        return;
    }
    char *me = local_player();
    char **pd = me && !IsBadReadPtr(me + 0x1b8, 8) ? *(char ***)(me + 0x1b8) : NULL;
    int hp = pd && !IsBadReadPtr(pd, 8) && *pd && !IsBadReadPtr(*pd, 0x148) ? *(int *)(*pd + 0x140) : -1;
    float p[3] = {0, 0, 0};
    int have = me && df_chr_pos(me, p);
    logf("day flow: death restart state: HP %d, downed %u, block %08x, position %s%.1f %.1f %.1f; extra lives: 6999500 %d (82100 %d), "
         "540155 %d, 6999100 %d, 8970061 %d; night 7515 %d, day 3 7510 %d", hp, me ? (unsigned char)me[0x710] : 0,
         me ? *(unsigned *)(me + CHR_BLOCK_ID) : 0, have ? "" : "(none) ", p[0], p[1], p[2], me && speffect_on(me, 6999500), bf_flag(82100),
         me && speffect_on(me, 540155), me && speffect_on(me, 6999100), me && speffect_on(me, 8970061), bf_flag(7515), bf_flag(7510));
    if (hp > 0) {  // not a death: the game's own state, unchanged
        ((void (*)(long long *, void *))(g_base + 0x5c38f0))(ev, sm);
        return;
    }
    if (g_df_ended) return;
    if (g_join != 4 && df_extra_life(me, 1)) {  // the host (or solo): the life goes, the game's own revive (a level less)
        logf("day flow: not the final death: the game's death restart revives (an extra life used)");
        ((void (*)(long long *, void *))(g_base + 0x5c38f0))(ev, sm);
        return;
    }
    g_df_ended = 1;
    if (g_join == 4 && g_as_ally) {
        logf("day flow: final death (the game's death restart, ally): home instead of the revive");
        join_over("ally's final death");
    } else if (g_join != 4) {
        ((void (*)(void))(g_base + 0x6af900))();
        if (g_lobby) lobby_set(LOBBY_OVER_KEY, "1");  // the invader's win (other_down_watch)
        char *gm = *(char **)(g_base + 0x3c13258);
        logf("day flow: final death (the game's death restart, no extra life): the game's game over requested (gm+0xaf2 = %u, af1 %u) "
             "instead of the revive with a level less", gm ? (unsigned char)gm[0xaf2] : 0, gm ? (unsigned char)gm[0xaf1] : 0);
    } else {
        g_df_ended = 0;
        ((void (*)(long long *, void *))(g_base + 0x5c38f0))(ev, sm);  // an invader: the game's own way
    }
}
static void df_death_restart_install(void) {
    unsigned char *site = (unsigned char *)g_base + 0x5c3ffa, *stub = near_alloc(12);
    int expected = (int)(0x5c38f0 - (0x5c3ffa + 7));
    void *hook = (void *)df_death_restart_hook;
    if (site[0] != 0x48 || site[1] != 0x8d || site[2] != 0x05 || memcmp(site + 3, &expected, 4) || !stub) {
        logf("day flow: death restart state at 5c3ffa does not look as expected, not hooked");
        return;
    }
    stub[0] = 0x48; stub[1] = 0xB8;
    memcpy(stub + 2, &hook, 8);
    stub[10] = 0xFF; stub[11] = 0xE0;
    if (patch_rel32(site + 3, site + 7, stub)) logf("day flow: death restart state: hooked (5c3ffa)");
}
// --- every frame in the field (every machine)
// The day flow's state is the host's (dev.60: a rejoined ally began its own count at its arrival): the host publishes
// it in its lobby data (LOBBY_DF_KEY "day,bosses,special,met,met_at,after,kills_day,second,need", on change); a joiner
// takes it at its arrival and on every change instead of counting itself (its boss count, the condition, the tier,
// the camps' second invaders, the progress notice). The challenge itself goes as before (LOBBY_CHAL_KEY).
#define LOBBY_DF_KEY "nrpvp_df"
static int g_df_host_state;  // joiner: the host's state was taken (from then on it does not count itself)
static void df_state_sync(void) {
    static DWORD next;
    static char last[64];
    if ((int)(GetTickCount() - next) < 0 || !g_steam.mm || g_steam_down) return;
    next = GetTickCount() + 500;
    char text[64];
    if (g_join != 4) {
        g_df_host_state = 0;
        if (!g_lobby) return;
        snprintf(text, sizeof text, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", g_df_day, g_df_bosses, g_df_special, g_df_met, g_df_met_at, g_df_after,
                 g_df_kills_day, g_df_second, g_df_need, g_df_killed1, g_df_killed2);
        if (strcmp(text, last)) snprintf(last, sizeof last, "%s", text), lobby_set(LOBBY_DF_KEY, text);
        return;
    }
    const char *v = g_candidate ? g_steam.get_data(g_steam.mm, g_candidate, LOBBY_DF_KEY) : NULL;
    int d[11];
    if (!v || sscanf(v, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7], &d[8], &d[9], &d[10]) != 11) return;
    if (g_df_host_state && !strcmp(v, last)) return;
    // a notice for a change of the host's (not at the arrival): more bosses, or the condition, also at once on day 2
    int more = g_df_host_state && (d[1] > g_df_bosses || d[2] > g_df_special || d[3] > g_df_met || (d[0] > g_df_day && d[3]));
    snprintf(last, sizeof last, "%s", v);
    g_df_day = d[0], g_df_bosses = d[1], g_df_special = d[2], g_df_met = d[3], g_df_met_at = d[4], g_df_after = d[5];
    g_df_kills_day = d[6], g_df_second = d[7], g_df_need = d[8], g_df_killed1 = d[9], g_df_killed2 = d[10];
    logf("day flow: the host's state %s: day %d, %d bosses, special %d, condition %d, after it %d, this day %d, second invaders %d, "
         "needed on day 2 %d, ordinary bosses day 1 %d / day 2 %d", g_df_host_state ? "changed" : "taken", d[0], d[1], d[2], d[3], d[5], d[6], d[7], d[8], d[9], d[10]);
    if (!g_df_host_state) g_df_told_met = g_df_met, g_df_told_tier = df_target_tier();  // arrival: nothing new to tell yet
    g_df_host_state = 1;
    if (more) g_df_notice_due = GetTickCount() + 6000 | 1;
    df_repass("the host's state");
}
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
        g_df_second = 0, g_df_host_state = 0, g_df_need = DF_BOSSES, g_df_killed1 = 0, g_df_killed2 = 0;
        g_df_events = 0, g_df_started = 0, g_df_event_tick = 0;
        InterlockedExchange(&g_df_kills, 0), InterlockedExchange(&g_df_special_kills, 0);
        char *gm0 = *(char **)(g_base + 0x3c13258);
        logf("day flow: expedition start (pattern %u / %u): %s (pair %d); challenge after %d bosses%s",
             gm0 ? *(unsigned *)(gm0 + 0xb48) : 0, gm0 ? *(unsigned *)(gm0 + 0xb4c) : 0, map_name(g_pair), g_pair, DF_BOSSES,
             g_pair < 0 ? " (no special region)" : " and the special boss");
        if (g_pair >= 0) notice_with(131186 + g_pair, NULL);  // "Map: <name>"; the base map shows none
        g_df_told_met = 0, g_df_told_tier = 0;
        g_df_warp_until = 0, g_df_load_until = 0, g_df_ended = 0, g_df_move_at = 0;
        if (g_df_fade_in_at) g_df_fade_in_at = 0, df_fade(0.0f, 0.1f, 0);
    }
    if (!day2_seen && bf_flag(7505)) {  // day 2: the game's own day-2 level, our count starts again
        // a joiner that already took the host's day-2 state (it may come before 7505 here) keeps it
        int host_day2 = g_join == 4 && g_df_host_state && g_df_day == 2;
        day2_seen = 1, g_df_day = 2;
        if (!host_day2) g_df_bosses = g_df_special = g_df_met = g_df_met_at = g_df_after = 0, g_df_kills_day = 0, g_df_killed2 = 0;
        g_df_told_met = 0;  // day 2's condition is told again (the tier is kept, and so is what was told of it)
        g_df_go = g_df_used = 0;
        InterlockedExchange(&g_df_kills, 0), InterlockedExchange(&g_df_special_kills, 0);
        logf("day flow: day 2 (7505): the count starts again, the tier from day 1 is kept (%d%%; events: each family ran at most once, none again)", df_target_tier());
        if (g_join != 4) {  // the host (or solo); a joiner takes the host's (df_state_sync)
            df_day2_count();
            df_check_met("at the start of day 2");
            if (g_df_met) g_df_notice_due = GetTickCount() + 6000 | 1;  // 131182 (the day-2 banner first)
        }
        // The merchant sells the item again: its rows 3007 / 3507 keep the Small Pouch's stock flag 90470 (ShopLineupParam
        // eventFlag_forStock). The stock rows' flags are 10 apart (90430 .. 90470, 90690, 90700), so 90470..90479 are
        // taken as this row's sold count; every one that is on goes off (each machine its own merchant).
        char was[16] = "";
        for (int f = 0; f < 10; f++)
            if (two_flag(90470 + f, 0, 0) == 1) {
                two_flag(90470 + f, 1, 0);
                size_t n = strlen(was);
                snprintf(was + n, sizeof was - n, "%d", f);
            }
        logf("day flow: day 2: the merchant's item stock reset (flags 90470+ that were on: %s)", *was ? was : "none");
    }
    LONG k = InterlockedExchange(&g_df_kills, 0), sk = InterlockedExchange(&g_df_special_kills, 0);
    // A joiner takes the host's count (df_state_sync): a late or rejoined ally starts its own count at its arrival
    // (run of dev.60: after a rejoin the host had 2 bosses, the ally 1). Its own kills only re-pass the characters.
    if (g_join == 4 && g_df_host_state) {
        if (k > 0 || sk > 0) df_repass("a boss: camps and tier");
        k = sk = 0;
    }
    g_df_kills_day += (int)(k + sk);
    if (k > 0 || sk > 0) df_repass("a boss: camps and tier");
    if (g_df_day == 1) g_df_killed1 += (int)k;
    else g_df_killed2 += (int)k;
    if (k > 0 || sk > 0) {
        if (g_df_met) g_df_after += (int)(k + sk);
        else g_df_bosses += (int)k, g_df_special = g_df_day == 2 ? g_df_special : g_df_special || sk > 0;  // day 2: below
        df_check_met("after a kill");
        logf("day flow: boss killed: %d of %d bosses, special bosses %d (of %d needed), after the condition %d", g_df_bosses, df_need(),
             g_df_special, g_df_day == 2 ? 2 : 1, g_df_after);
        g_df_notice_due = GetTickCount() + 6000 | 1;  // after the game's own boss banner (deck test: a notice in the same frame was not seen)
    }
    if (g_df_day == 2 && !g_df_met && !(g_join == 4 && g_df_host_state)) {  // day 2: the special bosses from their flags
        int dead = df_specials_dead(), n = (dead & 1) + (dead >> 1 & 1);
        if (n != g_df_special) {
            g_df_special = n;
            logf("day flow: day 2: special bosses dead %d (flags: base region %s, added region %s)", n, dead & 1 ? "on" : "off", dead & 2 ? "on" : "off");
            df_check_met("a special boss's flag");
        }
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
    if (g_df_load_until) {  // the arena block's load: warp once it is done and the site is known
        static DWORD next;
        if ((int)(GetTickCount() - g_df_load_until) >= 0) {
            char *mgr = *(char **)(g_base + 0x3c123f8);
            int left = mgr && *(int *)(mgr + 0xf0) != -1;
            if (left) *(int *)(mgr + 0xf0) = -1;  // not applied: no stale queued position for a later map move
            g_df_load_until = 0, g_df_warp_until = GetTickCount() + 120000;
            logf("day flow: arena load not done after 30 s%s: retrying the warp as a fallback", left ? " (queued position dropped)" : "");
        } else if ((int)(GetTickCount() - next) >= 0 && !df_load_pending()) {
            next = GetTickCount() + 500;
            if (df_warp()) g_df_load_until = 0;
        }
    }
    if (g_df_warp_until) {
        static DWORD next;
        if ((int)(GetTickCount() - g_df_warp_until) >= 0) g_df_warp_until = 0, logf("day flow: warp given up after 2 minutes");
        else if ((int)(GetTickCount() - next) >= 0 && (next = GetTickCount() + 1000, df_warp())) g_df_warp_until = 0;
    }
    df_lobby_watch();
    df_bases_check();
    df_state_sync();
    df_move_frame();
    df_track_frame();
    if (g_join != 4) df_events_frame();
    (void)down_last;  // the final death is the death restart's (df_death_restart_hook)
}

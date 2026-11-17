// Session capacity (team play stage 4): 4 remote player slots instead of the game's 2, so 5 players in a session
// (squad up to 3 + up to 2 invaders). Byte patches in DllMain, before the game constructs GameDataMan
// (0x1401fd5d0, its game-start init) and WorldChrMan (per expedition), all-or-nothing: every site's original bytes
// are compared first; any mismatch -> nothing written, the game stays at 2 remote slots (logged once).
// What and why (analysis/13-team-play-etapy.md, "Etap 4"):
//  - GameDataMan ctor 0x1401fa740: remote slot array (0x10 header + n·0x8a0), its header count and init loop; slot
//    state (+0x18) and the second per-slot byte array (+0x20), 2 -> 4 bytes each with their loops.
//  - every slot bound (< 2 / <= 1) in the slot helpers, the id sync and the session loop 0x140df2cfb;
//  - the per-slot bytes inside GameDataMan at +0x10a+i moved to +0x458+i (object 0x458 -> 0x460, zeroed by our
//    allocation wrapper; the ctor zeroes +0x10a as before, now unused);
//  - WorldChrMan character sets: players 0x1404ae430 3 -> 5 entries, per-player set 0x1404ae340 ((id+2)·10+k)
//    60 -> 80 entries (player ids are reused, lowest free first: <= 5 with 5 players);
//  - slot allocation (only call: 0x140df5f0e in member add 0x140df5ae0) through slot_alloc: the host's side (lobby
//    owner, allies) 0/1 first, invaders 2/3 first (lobby member data role at that moment; no role yet = invader), slot-test reverses each group;
//  - the id sync 0x1401fc260 offline branch clears slots 0 and 1 itself; our wrapper clears 2 and 3 too.
//  - the party list (3 entries, kept) gets only the local player's side: the remote add 0x140c44760 through party_add.
// Not changed (display limits accepted by the owner): named messages 0x140bd8b80 and result data 0x140211350 keep
// local + slots 0/1.
typedef struct { unsigned rva; unsigned char n; const char *orig, *repl; } cap_site;
static const cap_site CAP_SITES[] = {
    {0x1fd5e5, 5, "\xb9\x58\x04\x00\x00", "\xb9\x60\x04\x00\x00"},                        // GameDataMan size 0x458 -> 0x460
    {0x1faaba, 5, "\xba\x50\x11\x00\x00", "\xba\x90\x22\x00\x00"},                        // slot array 0x10 + 4·0x8a0
    {0x1faacb, 5, "\xb8\x02\x00\x00\x00", "\xb8\x04\x00\x00\x00"},                        // init loop count
    {0x1faafa, 8, "\x49\xc7\x47\xf8\x02\x00\x00\x00", "\x49\xc7\x47\xf8\x04\x00\x00\x00"}, // array header count
    {0x1fab14, 9, "\xba\x02\x00\x00\x00\x44\x8d\x42\xff", "\xba\x04\x00\x00\x00\x6a\x01\x41\x58"},  // +0x18: 4 B, align 1
    {0x1fab2d, 4, "\x45\x8d\x7d\x02", "\x45\x8d\x7d\x04"},                                // +0x18 init loop
    {0x1fab60, 11, "\xbf\x02\x00\x00\x00\x45\x8d\x47\x01\x8b\xd7",                        // +0x20: 4 B (edi stays 2)
     "\x6a\x02\x5f\x6a\x04\x5a\x45\x8d\x47\x01\x90"},
    {0x1fab72, 3, "\x44\x88\x28", "\x44\x89\x28"},                                        // +0x20 zeroed as a dword
    {0x1fb7e3, 5, "\x41\x8d\x74\x24\x02", "\x41\x8d\x74\x24\x04"},                        // per-slot loop
    {0x1fb2cf, 3, "\x83\xff\x02", "\x83\xff\x04"},                                        // per-slot loop
    {0x1fe4c0, 5, "\xb8\x02\x00\x00\x00", "\xb8\x04\x00\x00\x00"},                        // remote slot count
    {0x1fe530, 3, "\x83\xf9\x01", "\x83\xf9\x03"},                                        // slot getter
    {0x1fe560, 3, "\x83\xf8\x01", "\x83\xf8\x03"}, {0x1fe57d, 3, "\x83\xf8\x02", "\x83\xf8\x04"},  // slot by id
    {0x1fe16b, 4, "\x48\x83\xf9\x02", "\x48\x83\xf9\x04"},                                // free slot search
    {0x1fe19b, 4, "\x48\x83\xf9\x02", "\x48\x83\xf9\x04"},                                // free +0x20 search
    {0x1fdd84, 3, "\x83\xf9\x01", "\x83\xf9\x03"},                                        // slot clear
    {0x1fdde7, 8, "\xc6\x84\x07\x0a\x01\x00\x00\x00", "\xc6\x84\x07\x58\x04\x00\x00\x00"},
    {0x1fde00, 3, "\x83\xf9\x01", "\x83\xf9\x03"},
    {0x1fe7d0, 3, "\x83\xfa\x01", "\x83\xfa\x03"}, {0x1fe7ec, 3, "\x83\xfa\x02", "\x83\xfa\x04"},
    {0x1fe93c, 3, "\x83\xf8\x02", "\x83\xf8\x04"},
    {0x1fecc0, 3, "\x83\xf8\x01", "\x83\xf8\x03"}, {0x1fecdc, 3, "\x83\xf8\x02", "\x83\xf8\x04"},
    {0x1fece9, 9, "\x43\x0f\xb6\x84\x08\x0a\x01\x00\x00", "\x43\x0f\xb6\x84\x08\x58\x04\x00\x00"},
    {0x200a60, 3, "\x83\xf8\x01", "\x83\xf8\x03"}, {0x200a7c, 3, "\x83\xf8\x02", "\x83\xf8\x04"},
    {0x200a89, 9, "\x43\xc6\x84\x08\x0a\x01\x00\x00\x01", "\x43\xc6\x84\x08\x58\x04\x00\x00\x01"},
    {0x200aa0, 3, "\x83\xf9\x01", "\x83\xf9\x03"},
    {0x200abe, 8, "\xc6\x84\x02\x0a\x01\x00\x00\x00", "\xc6\x84\x02\x58\x04\x00\x00\x00"},
    {0x200ad0, 3, "\x83\xf9\x01", "\x83\xf9\x03"},
    {0x1fc3e1, 3, "\x83\xf8\x02", "\x83\xf8\x04"},                                        // id sync: slot < 4
    {0xdf2cfb, 3, "\x83\xfb\x02", "\x83\xfb\x04"},                                        // session: clear all slots
    {0x4ae4af, 9, "\xba\x30\x00\x00\x00\x44\x8d\x42\xd8", "\xba\x50\x00\x00\x00\x44\x8d\x42\xb8"},  // player set 5·16, align 8
    {0x4ae4c4, 5, "\xbf\x03\x00\x00\x00", "\xbf\x05\x00\x00\x00"},
    {0x4ae3c2, 5, "\xba\xc0\x03\x00\x00", "\xba\x00\x05\x00\x00"},                        // per-player set 80·16
    {0x4ae3d9, 5, "\xbf\x3c\x00\x00\x00", "\xbf\x50\x00\x00\x00"},
    {0x4ae3e3, 6, "\x41\xb8\xc0\x03\x00\x00", "\x41\xb8\x00\x05\x00\x00"},
};
#define CAP_SITE_COUNT (sizeof CAP_SITES / sizeof *CAP_SITES)

// GameDataMan allocation (0x1fd5ea: the game's allocator with 0x460 now): zeroed, so the moved bytes start at 0.
static void *cap_alloc_hook(unsigned long long size, unsigned long long align) {
    void *p = ((void *(*)(unsigned long long, unsigned long long))(g_base + 0x214e8d0))(size, align);
    if (p) memset(p, 0, (size_t)size);
    return p;
}
// Id sync (0x1401fc260) wrapper: its offline branch sets slots 0 and 1 to -1; slots 2 and 3 the same here.
static void cap_id_sync_hook(void) {
    ((void (*)(void))(g_base + 0x1fc260))();
    char *gdm = *(char **)(g_base + 0x3c078d0);
    if (gdm && !IsBadReadPtr(gdm + 0xd8, 1) && gdm[0xd8] != 2 && *(char **)(gdm + 0x10))
        for (int i = 2; i < 4; i++) *(int *)(*(char **)(gdm + 0x10) + i * 0x8a0 + 8) = -1;
}
// Slot allocation for a new session member (the steam id is the member add's [rsp+0x20], passed by our stub).
static int slot_alloc(unsigned long long *steam_id) {
    char *gdm = *(char **)(g_base + 0x3c078d0);
    unsigned char *state = gdm ? *(unsigned char **)(gdm + 0x18) : NULL;
    if (!state) return -1;
    unsigned long long id = steam_id && !IsBadReadPtr(steam_id, 8) ? *steam_id : 0;
    unsigned long long lobby = our_lobby();
    unsigned long long owner = lobby && g_steam.mm && g_diag.owner ? g_diag.owner(g_steam.mm, lobby) : 0;
    int ally = id && lobby && (member_ally(lobby, id) || id == owner);  // the host's side: the lobby owner and allies
    int test = setting(S_SLOT_TEST) != 0;
    static const int ORDER[2][2][4] = {{{0, 1, 2, 3}, {2, 3, 0, 1}}, {{1, 0, 3, 2}, {3, 2, 1, 0}}};  // [test][invader]
    const int *o = ORDER[test][!ally];
    int slot = -1;
    for (int k = 0; k < g_cap_remote && slot < 0; k++) {
        int s = g_cap_remote == 4 ? o[k] : k;
        if (s < g_cap_remote && !state[s]) slot = s;
    }
    // the session number is given right after this (0x140df5f4d), so the member is named by its tag here
    logf("slot: member %04x got slot %d (%s%s)", id_tag(id), slot, ally ? "host's side" : "invader or no role yet", test ? ", slot-test" : "");
    return slot;
}
// Party list (CS::PartyMemberInfo, [0x143c13258]+0xdc8): 3 entries of 0x30 from +0x28, every loop bounded by 3;
// the add 0x140aed5f0 writes nothing when they are taken. Added: the local player (0x140c389dd) and each remote
// character at its creation (0x140c44760, steam id at the caller's [rsp+0x28]). Its readers: party bars, over-head
// names, event conditions counting party members (0x14059e550/e710, 0x140602670, 0x14073e610), a member's death
// (0x140aedb20 -> after a timer the session remove 0x140df1130), menus (0x140a98d30, 0x140a94430, 0x1408e0cc0).
// With late summons (host, ally, invader, ally) the second ally would be left out; so a remote character of the
// other side (by lobby member data, the owner = host's side) is not added: the list holds the local player's side.
// No role yet: the game's add, logged.
static unsigned long long party_add(void *party, void *entry, unsigned long long *steam_id) {
    unsigned long long id = steam_id && !IsBadReadPtr(steam_id, 8) ? *steam_id : 0, lobby = our_lobby();
    unsigned long long owner = lobby && id && g_steam.mm && g_diag.owner ? g_diag.owner(g_steam.mm, lobby) : 0;
    const char *r = owner && id != owner && g_diag.get_member ? g_diag.get_member(g_steam.mm, lobby, id, LOBBY_ROLE_KEY) : NULL;
    int known = owner && (id == owner || (r && *r)), invader = known && id != owner && strcmp(r, "ally") != 0;
    int we_invader = g_join == 4 && !g_as_ally;
    if (known && invader != we_invader) {
        logf("party list: member %04x (%s) is of the other side: not added", id_tag(id), invader ? "invader" : "host's side");
        return 0;
    }
    unsigned long long got = ((unsigned long long (*)(void *, void *))(g_base + 0xaed5f0))(party, entry);
    logf("party list: member %04x (%s) %s, %d in the list", id_tag(id), known ? (invader ? "invader" : "host's side") : "no role yet",
         got & 0xff ? "added" : "not added (full or there)", party && !IsBadReadPtr((char *)party + 0x14, 4) ? *(int *)((char *)party + 0x14) : -1);
    return got;
}
static void capacity_install(void) {  // DllMain
    if (GetModuleHandleA("nrsc.dll")) {
        logf("capacity: Seamless Co-op is loaded: 2 remote player slots (the game's)");
        return;
    }
    for (unsigned i = 0; i < CAP_SITE_COUNT; i++)
        if (memcmp(g_base + CAP_SITES[i].rva, CAP_SITES[i].orig, CAP_SITES[i].n)) {
            logf("capacity: site %x is not as expected: nothing patched, 2 remote player slots (the game's)", CAP_SITES[i].rva);
            return;
        }
    static const unsigned SYNC_CALLS[3] = {0x200ef9, 0x201261, 0xc27a5d};
    unsigned char e8[5] = {0xE8};
    for (int i = 0; i < 3; i++) {
        int rel = (int)(0x1fc260 - (SYNC_CALLS[i] + 5));
        memcpy(e8 + 1, &rel, 4);
        if (memcmp(g_base + SYNC_CALLS[i], e8, 5)) { logf("capacity: id sync call %x not as expected: nothing patched", SYNC_CALLS[i]); return; }
    }
    {
        int rel = (int)(0x214e8d0 - (0x1fd5ea + 5)), rel2 = (int)(0x1fe150 - (0xdf5f0e + 5)), rel3 = (int)(0xaed5f0 - (0xc44760 + 5));
        unsigned char *a = (unsigned char *)g_base + 0x1fd5ea, *b = (unsigned char *)g_base + 0xdf5f0e, *c = (unsigned char *)g_base + 0xc44760;
        if (a[0] != 0xE8 || memcmp(a + 1, &rel, 4) || b[0] != 0xE8 || memcmp(b + 1, &rel2, 4) || c[0] != 0xE8 || memcmp(c + 1, &rel3, 4)) {
            logf("capacity: allocation calls not as expected: nothing patched");
            return;
        }
    }
    // the slot allocation stub: rcx = &steam id ([rsp+0x28] at our entry = the caller's [rsp+0x20]), then slot_alloc
    unsigned char *stub = near_alloc(24), *pstub = near_alloc(24);
    if (!stub || !pstub) { logf("capacity: no memory near the game: nothing patched"); return; }
    void *pf = (void *)party_add;  // the party add stub: r8 = &steam id ([rsp+0x30] at our entry = the caller's [rsp+0x28])
    memcpy(pstub, "\x4c\x8d\x44\x24\x30\x48\xb8", 7);  // lea r8,[rsp+0x30]; mov rax, imm64
    memcpy(pstub + 7, &pf, 8);
    pstub[15] = 0xFF, pstub[16] = 0xE0;  // jmp rax
    void *f = (void *)slot_alloc;
    memcpy(stub, "\x48\x8d\x4c\x24\x28\x48\xb8", 7);  // lea rcx,[rsp+0x28]; mov rax, imm64
    memcpy(stub + 7, &f, 8);
    stub[15] = 0xFF, stub[16] = 0xE0;  // jmp rax
    unsigned patched = 0;
    for (unsigned i = 0; i < CAP_SITE_COUNT; i++) {
        unsigned char *p = (unsigned char *)g_base + CAP_SITES[i].rva;
        DWORD old;
        if (!VirtualProtect(p, CAP_SITES[i].n, PAGE_EXECUTE_READWRITE, &old)) {  // undo what was written: never half
            for (unsigned k = 0; k < i; k++) {
                unsigned char *q = (unsigned char *)g_base + CAP_SITES[k].rva;
                DWORD o2;
                if (VirtualProtect(q, CAP_SITES[k].n, PAGE_EXECUTE_READWRITE, &o2)) memcpy(q, CAP_SITES[k].orig, CAP_SITES[k].n), VirtualProtect(q, CAP_SITES[k].n, o2, &o2);
            }
            logf("capacity: cannot write site %x: undone, 2 remote player slots (the game's)", CAP_SITES[i].rva);
            return;
        }
        memcpy(p, CAP_SITES[i].repl, CAP_SITES[i].n);
        VirtualProtect(p, CAP_SITES[i].n, old, &old);
        patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), g_base, 0xe00000);
    hook_call(0x1fd5ea, 0x214e8d0, (void *)cap_alloc_hook, "capacity: GameDataMan allocation");
    for (int i = 0; i < 3; i++) hook_call(SYNC_CALLS[i], 0x1fc260, (void *)cap_id_sync_hook, "capacity: id sync");
    patch_rel32((unsigned char *)g_base + 0xdf5f0e + 1, (unsigned char *)g_base + 0xdf5f0e + 5, stub);
    patch_rel32((unsigned char *)g_base + 0xc44760 + 1, (unsigned char *)g_base + 0xc44760 + 5, pstub);
    g_cap_remote = 4;
    logf("capacity: %d remote player slots (patched %u sites); party list: the local player's side only", g_cap_remote, patched + 6);
}

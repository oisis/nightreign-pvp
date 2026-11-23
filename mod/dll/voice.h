// Proximity voice chat (settings voice / voice-loopback; without them nothing here runs: no thread, no hook, no Steam
// voice call, no XAudio2).
//
// - Capture: Steam's own voice recorder (ISteamUser 021 StartVoiceRecording / GetAvailableVoice / GetVoice, compressed),
//   only while the push-to-talk key (voice-key) or the team key (voice-team-key) is held and the game's window has the
//   focus (GetAsyncKeyState); optionally an XInput pad button (voice-pad / voice-team-pad: 1..16 = bit 0..15 of
//   wButtons). Nightreign has no voice of its own to reuse.
// - Transport: our own P2P channel VOICE_CHANNEL (the ping's LAG_CHANNEL + 1) over the game's ISteamNetworking 006,
//   unreliable, only to lobby members whose DLL answers our echo (lag_peer.rtt_last_ok). The game never reads it (the
//   lag log lists every channel the game reads). Each packet carries the sender's map block, its field tile and its
//   position local to that tile (0x140631740, as nrpvp_fpos) and its world position.
// - Rules: proximity (voice-key) reaches everyone in the session: full volume up to VOICE_FULL_M, down to 0 at
//   voice-range. Team (voice-team-key): host and allies hear each other at any distance, centred; an invader never
//   sends or hears it (member_ally, the lobby owner). voice-mute: names or Steam ids never played.
// - Playback: our own XAudio2 2.9 engine (xaudio2_9.dll), one mono source voice per speaker, its volume for the distance
//   and a 1 x N output matrix for the pan; fed from our thread (GetState polling, no callbacks), with a jitter buffer
//   of VOICE_PREBUFFER_MS; packets older than the last one of their speaker are dropped (late). XAudio2 is never
//   released (process exit tears it down).
// - Pan: the camera's right vector. CSCamera's static is 0x143c0b0e8 (VERIFIED from the exe: the DLRF runtime class
//   of "CSCamera" (0x140090c80 names it, vtable 0x142c08290) returns the type tag 0x143c08b3f from vt[3]; the
//   singleton null checks that report tag 0x143c08b3f load 0x143c0b0e8). Its first qword is the current camera
//   (0x1403792b0 = mov rax,[rcx]; 0x1405795e5 copies its 4 x 16 bytes at +0x10); the 4 rows there being right / up /
//   forward / position is INFERRED from ELDEN RING's CSCam (fromsoftware-rs), checked by a log line at the first read.
// - Distance: both in field tiles of m60 (area 0x3c): tile-local difference plus 256 m per tile of the block grid
//   (INFERRED: XX = block byte 2, ZZ = byte 1, 256 m; a "voice: tile check" line measures it at the first tile change);
//   the same field tile: its local positions; else the same block: world positions (INFERRED: the same frame there);
//   else too far.
// - AGC (voice_agc_run): per speaker on the decoded PCM, before the ring, to voice-level dB below full scale (10
//   "loud", 18 "normal"); voice-volume (0..100 %) is a trim on top.
// - Ducking (voice-duck, voice-duck-amount %): while any speaker's buffer plays, the game's own sound goes down; back
//   up DUCK_HOLD_MS after the last one stops, over DUCK_RELEASE_MS. The game's sound is Wwise, statically linked, and
//   the exe exports the SDK by its mangled names (VERIFIED, export table): AK::SoundEngine::SetOutputVolume(id, vol)
//   (0x1423cc830: refuses a non-finite volume, else queues message 0x17 {id, vol} for the audio thread, AK_Success);
//   the audio thread's handler (0x14241e839) finds the device by id and stores vol at device +0x154, which the mix
//   ramps to from +0x150 over one buffer (0x1424330b0) and then copies to +0x150 (0x14243233d). GetOutputID(0, 0)
//   gives the main output's id (0x1423c5d70: the device list's first default entry); IsInitialized is the byte at
//   0x1446f7218. The game itself never sets it: the only code path to SetOutputVolume is the MagicOrchestra wrapper
//   0x1423a29d0, which nothing references (only its .pdata entry); so its value is the SDK's 1.0 (INFERRED: the device
//   constructor copies it from its settings) and the restore is 1.0. It is a runtime volume, never saved; it is set
//   back to 1.0 when the voice thread ends (Steam shutdown, while the game thread waits in SteamAPI_Shutdown). Our own
//   XAudio2 engine is not Wwise's, so the voices are not ducked.
// Log: one "voice: " line at the start, a "voice: " summary per speaker every LAG_LOG_MS, details with debug-log only.
#define sqrtf __builtin_sqrtf  // no math.h: its logf is not ours
#define fabsf __builtin_fabsf
#define log10f __builtin_log10f
#define expf __builtin_expf
#include <xaudio2.h>

#define VOICE_CHANNEL (LAG_CHANNEL + 1)
#define VOICE_MAGIC 0x4356524eu  // "NRVC"
#define VOICE_FULL_M 5.0f
#define VOICE_TILE_M 256.0f
#define VOICE_PREBUFFER_MS 120
#define VOICE_MAX_MS 400
#define VOICE_PACKET 1200  // ISteamNetworking's unreliable limit
#define VOICE_RING 65536   // samples per speaker (power of two)
#define VOICE_CHUNK 1024   // samples per XAudio2 buffer (20 ms at most 48 kHz)
#define VOICE_POOL 8
#define VOICE_SLOTS 16
#define RVA_CS_CAMERA 0x3c0b0e8
#define VOICE_TEAM 1  // header flag

typedef struct {
    unsigned magic;
    unsigned short seq;
    unsigned char flags, ver;
    unsigned block, field;   // ChrIns +0x38; the field tile the local position is in (0: none)
    float local[3], world[3];
} voice_hdr;
// Receiving-side automatic gain (Steam's decoded speech is quiet): the speech level is the RMS of 20 ms frames above
// the gate, smoothed (fast up, slow down), and the gain takes it to the target. Frames below the gate keep the gain
// but are not brought above AGC_NOISE_DB; a frame's gain never takes its peak past full scale (the limiter).
// 10^(dB/20) is expf(dB * 0.115129255f): powf would pull in api-ms-win-crt-math.
static float g_agc_target = -10;  // dBFS, -voice-level
#define AGC_MAX_DB 24.0f
#define AGC_MIN_DB -12.0f
#define AGC_GATE_DB -50.0f
#define AGC_NOISE_DB -40.0f
#define AGC_ATTACK_S 0.03f
#define AGC_RELEASE_S 1.0f
typedef struct {
    int primed;
    float lvl_db, agc_db, g;  // speech level; the gain toward the target; the last frame's applied gain (linear)
    float sum_db;              // for the 15 s summary: agc_db summed over the speech frames
    unsigned frames, limits;   // speech frames; frames the limiter turned down
} voice_agc;
static void voice_agc_run(voice_agc *a, short *x, unsigned n, unsigned rate) {
    unsigned fr = rate / 50;
    if (a->g <= 0) a->g = 1;
    for (unsigned at = 0; at < n; at += fr) {
        unsigned m = n - at < fr ? n - at : fr;
        short *p = x + at;
        float e = 0, pk = 0;
        for (unsigned i = 0; i < m; i++) {
            float v = p[i];
            e += v * v, v = fabsf(v), pk = v > pk ? v : pk;
        }
        float rms = sqrtf(e / (float)m) / 32768.0f, db = rms > 1e-6f ? 20 * log10f(rms) : -120;
        if (db > AGC_GATE_DB) {
            if (!a->primed) a->primed = 1, a->lvl_db = db;
            else a->lvl_db += (db - a->lvl_db) * (1 - expf(-(float)m / ((float)rate * (db > a->lvl_db ? AGC_ATTACK_S : AGC_RELEASE_S))));
            float t = g_agc_target - a->lvl_db;
            a->agc_db = t > AGC_MAX_DB ? AGC_MAX_DB : t < AGC_MIN_DB ? AGC_MIN_DB : t;
            a->sum_db += a->agc_db, a->frames++;
        }
        float g = expf(a->agc_db * 0.115129255f);
        if (db <= AGC_GATE_DB && db > AGC_NOISE_DB - AGC_MAX_DB) {
            float cap = expf((AGC_NOISE_DB - db) * 0.115129255f);
            g = g > cap ? cap : g;
        }
        if (pk * g > 32000) g = 32000 / pk, a->limits++;
        float gi = g < a->g ? g : a->g, step = (g - gi) / (float)m;  // down at once, up over the frame
        for (unsigned i = 0; i < m; i++) {
            float v = p[i] * (gi += step);
            p[i] = (short)(v >= 32767 ? 32767 : v <= -32768 ? -32768 : v < 0 ? v - 0.5f : v + 0.5f);
        }
        a->g = g;
    }
}
typedef struct { int ok; unsigned block, field; float local[3], world[3]; int cam; float right[2]; } voice_where;
typedef struct {
    u64 id;
    IXAudio2SourceVoice *src;
    short *ring;  // VOICE_RING
    short (*pool)[VOICE_CHUNK];
    unsigned rd, wr, next;
    int playing, seq_ok, invader, muted;
    unsigned short seq;
    float gain, pan;
    voice_agc agc;
    DWORD last_pkt, heard, invader_at, name_at, detail_at;
    unsigned rx, rx_pkts, tx, tx_pkts, late, overflow, distant, team_drop;
    wchar_t name[48];
    char name8[64];
} voice_spk;

static struct {
    int ready, loopback;
    unsigned rate, out_ch;
    void *friends;
    void (*start)(void *);
    void (*stop)(void *);
    int (*avail)(void *, unsigned *, unsigned *, unsigned);
    int (*get)(void *, unsigned char, void *, unsigned, unsigned *, unsigned char, void *, unsigned, unsigned *, unsigned);
    int (*decompress)(void *, const void *, unsigned, void *, unsigned, unsigned *, unsigned);
    unsigned (*opt_rate)(void *);
    void (*speaking)(void *, u64, unsigned char);
    const char *(*persona)(void *, u64);
    DWORD(WINAPI *xinput)(DWORD, void *);
    IXAudio2 *xa;
    IXAudio2MasteringVoice *master;
    voice_spk *spk;  // VOICE_SLOTS
    volatile int talking;  // 0 no, 1 proximity, 2 team
    volatile u64 me;       // our Steam id (read by the voice thread: the game thread makes no Steam call here)
    unsigned short seq;
    unsigned too_big, strangers;
    char mute[512];
} g_voice;
static CRITICAL_SECTION g_vw_lock;  // g_vw and the speakers' names (game thread / voice thread)
static voice_where g_vw;
static int g_voice_ov;  // the panel shows the voice lines (the counts wait)
static const unsigned short *g_counts_lines[3];  // the counts' last lines, to show again after the voice lines
static int g_counts_n;
static int counts_in_hub(void);

// Game thread (step hook), every 50 ms while voice runs: where we are and where the camera looks.
static void voice_frame(void) {
    static DWORD next;
    static unsigned field, checked;
    if (!g_voice.ready || (int)(GetTickCount() - next) < 0) return;
    next = GetTickCount() + 50;
    voice_where w = {0};
    float p[3];
    unsigned block;
    if (player_position(p, &block)) {
        w.ok = 1, w.block = block, memcpy(w.world, p, sizeof p);
        unsigned was = field;
        if (block >> 24 >= 0x3c && block >> 24 <= 0x58) field = block;
        else if (block >> 24 == 0x0a) field = 0;  // the hub: no field tile any more
        _Alignas(16) float wv[4] = {p[0], p[1], p[2], 0}, l[4];
        unsigned fb = field;
        if (field && ((unsigned char (*)(float *, float *, unsigned *))(g_base + 0x631740))(l, wv, &fb)) w.field = field, memcpy(w.local, l, sizeof w.local);
        // the tile size check (a few times per session): our position local to the old tile and to the new one
        if (w.field && was && was != field && checked < 3 && was >> 24 == 0x3c && field >> 24 == 0x3c) {
            _Alignas(16) float lo[4];
            unsigned ob = was;
            if (((unsigned char (*)(float *, float *, unsigned *))(g_base + 0x631740))(lo, wv, &ob)) {
                checked++;
                int gx = (int)(field >> 16 & 0xff) - (int)(was >> 16 & 0xff), gz = (int)(field >> 8 & 0xff) - (int)(was >> 8 & 0xff);
                logf("voice: tile check: %08x -> %08x: local in the old tile minus local in the new one %.1f %.1f %.1f; the grid "
                     "assumption says %.0f 0 %.0f", was, field, lo[0] - l[0], lo[1] - l[1], lo[2] - l[2], gx * VOICE_TILE_M, gz * VOICE_TILE_M);
            }
        }
    }
    char *cs = *(char **)(g_base + RVA_CS_CAMERA), *cam = cs && !IsBadReadPtr(cs, 8) ? *(char **)cs : NULL;
    if (cam && !IsBadReadPtr(cam + 0x10, 0x40)) {
        const float *m = (const float *)(cam + 0x10);
        float len = sqrtf(m[0] * m[0] + m[2] * m[2]);
        if (len > 0.3f && len < 1.5f) w.cam = 1, w.right[0] = m[0] / len, w.right[1] = m[2] / len;
        static int told;
        if (!told && w.ok) {
            told = 1;
            float dx = m[12] - p[0], dy = m[13] - p[1], dz = m[14] - p[2];
            logf("voice: camera (CSCamera 0x%x, rows INFERRED right / up / forward / position): right %.2f %.2f %.2f, up %.2f %.2f %.2f, "
                 "forward %.2f %.2f %.2f, right.forward %.2f, position %.1f m from the player", RVA_CS_CAMERA, m[0], m[1], m[2], m[4], m[5], m[6],
                 m[8], m[9], m[10], m[0] * m[8] + m[1] * m[9] + m[2] * m[10], sqrtf(dx * dx + dy * dy + dz * dz));
        }
    }
    EnterCriticalSection(&g_vw_lock);
    g_vw = w;
    LeaveCriticalSection(&g_vw_lock);
}

// Distance from us to a sender, d = sender - us. 0: not comparable (too far).
static int voice_delta(const voice_where *me, const voice_hdr *h, float d[3]) {
    if (!me->ok) return 0;
    if (me->field && h->field && me->field >> 24 == 0x3c && h->field >> 24 == 0x3c) {
        float gx = (float)((int)(h->field >> 16 & 0xff) - (int)(me->field >> 16 & 0xff)) * VOICE_TILE_M;
        float gz = (float)((int)(h->field >> 8 & 0xff) - (int)(me->field >> 8 & 0xff)) * VOICE_TILE_M;
        d[0] = h->local[0] - me->local[0] + gx, d[1] = h->local[1] - me->local[1], d[2] = h->local[2] - me->local[2] + gz;
        return 1;
    }
    if (me->field && me->field == h->field) {
        for (int i = 0; i < 3; i++) d[i] = h->local[i] - me->local[i];
        return 1;
    }
    if (me->block == h->block) {
        for (int i = 0; i < 3; i++) d[i] = h->world[i] - me->world[i];
        return 1;
    }
    return 0;
}

static int voice_muted(const voice_spk *s) {
    char id[24];
    snprintf(id, sizeof id, "%llu", s->id);
    for (const char *t = g_voice.mute; *t;) {
        while (*t == ',' || *t == ' ') t++;
        const char *e = t;
        while (*e && *e != ',') e++;
        const char *z = e;
        while (z > t && z[-1] == ' ') z--;
        size_t n = (size_t)(z - t);
        if (n && ((n == strlen(id) && !memcmp(t, id, n)) || (n == strlen(s->name8) && !_strnicmp(t, s->name8, n)))) return 1;
        t = e;
    }
    return 0;
}
static voice_spk *voice_slot(u64 id) {
    voice_spk *free_ = NULL;
    for (int i = 0; i < VOICE_SLOTS; i++) {
        if (g_voice.spk[i].id == id) return &g_voice.spk[i];
        if (!free_ && !g_voice.spk[i].id) free_ = &g_voice.spk[i];
    }
    if (!free_) return NULL;  // ponytail: 16 speakers per game session; a slot is never given back
    free_->id = id, free_->gain = -1;
    return free_;
}
// Name (for the panel and the mute list) and side, refreshed every few seconds. Voice thread.
static void voice_refresh(voice_spk *s, u64 me) {
    DWORD now = GetTickCount();
    if (!s->name_at || now - s->name_at > 5000) {
        s->name_at = now | 1;
        const char *n = g_voice.persona && g_voice.friends ? g_voice.persona(g_voice.friends, s->id) : NULL;
        EnterCriticalSection(&g_vw_lock);
        snprintf(s->name8, sizeof s->name8, "%s", n ? n : "");
        if (!MultiByteToWideChar(CP_UTF8, 0, s->name8, -1, s->name, 48)) s->name[0] = 0;
        s->name[47] = 0;
        LeaveCriticalSection(&g_vw_lock);
        s->muted = s->id != me && voice_muted(s);
    }
    if (!s->invader_at || now - s->invader_at > 2000) {
        s->invader_at = now | 1;
        u64 lobby = our_lobby(), owner = lobby && g_diag.owner ? g_diag.owner(g_steam.mm, lobby) : 0;
        s->invader = s->id != owner && !member_ally(lobby, s->id);
    }
}
static int voice_we_invader(void) { return g_join == 4 && !g_as_ally; }

static void voice_apply(voice_spk *s, float gain, float pan) {
    if (!s->src) return;
    if (fabsf(gain - s->gain) > 0.01f) IXAudio2SourceVoice_SetVolume(s->src, gain, XAUDIO2_COMMIT_NOW), s->gain = gain;
    if (fabsf(pan - s->pan) > 0.02f) {  // s->pan starts at 2: the first call always sets the matrix
        float mx[8] = {1, 1, 0, 0, 0, 0, 0, 0};
        if (g_voice.out_ch >= 2) mx[0] = sqrtf((1 - pan) * 0.5f), mx[1] = sqrtf((1 + pan) * 0.5f);
        IXAudio2SourceVoice_SetOutputMatrix(s->src, NULL, 1, g_voice.out_ch, mx, XAUDIO2_COMMIT_NOW);
        s->pan = pan;
    }
}
// Compressed Steam voice into the speaker's ring.
static void voice_push(voice_spk *s, const void *data, unsigned n) {
    static short pcm[24000];  // 250 ms at 48 kHz
    unsigned got = 0;
    if (!s->ring) {
        s->ring = calloc(VOICE_RING, sizeof *s->ring), s->pool = calloc(VOICE_POOL, sizeof *s->pool);
        if (!s->ring || !s->pool) return;
    }
    if (!s->src) {
        WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, g_voice.rate, g_voice.rate * 2, 2, 16, 0};
        if (FAILED(IXAudio2_CreateSourceVoice(g_voice.xa, &s->src, &f, 0, 2.0f, NULL, NULL, NULL))) {
            s->src = NULL;
            return;
        }
        IXAudio2SourceVoice_Start(s->src, 0, XAUDIO2_COMMIT_NOW);
        s->pan = 2;  // the first voice_apply sets the matrix
    }
    if (g_voice.decompress(g_diag.user, data, n, pcm, sizeof pcm, &got, g_voice.rate) != 0) return;
    got /= 2;
    voice_agc_run(&s->agc, pcm, got, g_voice.rate);
    unsigned max = g_voice.rate * VOICE_MAX_MS / 1000;
    for (unsigned i = 0; i < got; i++) s->ring[s->wr++ & (VOICE_RING - 1)] = pcm[i];
    if (s->wr - s->rd > max) s->rd = s->wr - max, s->overflow++;
    s->heard = GetTickCount();
}
static void voice_feed(voice_spk *s) {
    if (!s->src) return;
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(s->src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    unsigned have = s->wr - s->rd, chunk = g_voice.rate / 50;
    if (!s->playing && have < g_voice.rate * VOICE_PREBUFFER_MS / 1000 && !(have && GetTickCount() - s->heard > VOICE_PREBUFFER_MS)) return;
    s->playing = 1;
    while (st.BuffersQueued < 4 && have) {
        unsigned n = have < chunk ? have : chunk;
        short *b = s->pool[s->next++ % VOICE_POOL];
        for (unsigned i = 0; i < n; i++) b[i] = s->ring[s->rd++ & (VOICE_RING - 1)];
        XAUDIO2_BUFFER xb = {0};
        xb.AudioBytes = n * 2, xb.pAudioData = (const BYTE *)b;
        if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(s->src, &xb, NULL))) break;
        st.BuffersQueued++, have -= n;
    }
    if (!have && !st.BuffersQueued) s->playing = 0;  // run dry: the next words wait for the jitter buffer again
}

static int voice_key(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000); }
static unsigned voice_pad(void) {  // the buttons of the first connected pad; scanned again every 2 s when none
    static int pad = -1;
    static DWORD scan;
    if (!g_voice.xinput) return 0;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st;
    if (pad >= 0 && g_voice.xinput((DWORD)pad, &st) == 0) return st.buttons;
    pad = -1;
    if (GetTickCount() - scan < 2000) return 0;
    scan = GetTickCount();
    for (int i = 0; i < 4; i++)
        if (g_voice.xinput((DWORD)i, &st) == 0) return pad = i, st.buttons;
    return 0;
}
static int voice_pad_bit(unsigned buttons, int n) { return n >= 1 && n <= 16 && (buttons >> (n - 1) & 1); }

static void voice_log(void) {
    for (int i = 0; i < VOICE_SLOTS; i++) {
        voice_spk *s = &g_voice.spk[i];
        if (!s->id || !(s->rx_pkts | s->tx_pkts | s->late | s->overflow | s->distant | s->team_drop)) continue;
        logf("voice: %s %04x%s: sent %u B in %u pkts, received %u B in %u pkts, late %u, jitter buffer full %u, out of range %u, team "
             "refused %u, AGC %+.1f dB average, limiter %u", s->id == g_voice.me ? "us" : "member", id_tag(s->id), s->muted ? " (muted)" : "", s->tx, s->tx_pkts,
             s->rx, s->rx_pkts, s->late, s->overflow, s->distant, s->team_drop,
             s->agc.frames ? s->agc.sum_db / (float)s->agc.frames : 0.0f, s->agc.limits);
        s->agc.sum_db = 0, s->agc.frames = s->agc.limits = 0;
        s->rx = s->rx_pkts = s->tx = s->tx_pkts = s->late = s->overflow = s->distant = s->team_drop = 0;
    }
    if (g_voice.too_big || g_voice.strangers) logf("voice: %u captures too big for one packet, %u packets from outside the lobby", g_voice.too_big, g_voice.strangers);
    g_voice.too_big = g_voice.strangers = 0;
}

#define DUCK_DOWN_MS 50
#define DUCK_HOLD_MS 300
#define DUCK_RELEASE_MS 500
static struct {
    unsigned char (*ready)(void);
    u64 (*out_id)(unsigned, unsigned);
    int (*set)(u64, float);
    float now, sent;  // the envelope; the last volume sent (1: none / restored)
    DWORD heard, at;
} g_duck = {.now = 1, .sent = 1};
static void duck_send(float v) {
    if (!g_duck.ready()) return;  // no engine (not yet, or gone): nothing to scale
    g_duck.set(g_duck.out_id(0, 0), v), g_duck.sent = v;
}
// Voice thread, every step: heard = some speaker's buffer is playing.
static void voice_duck(int heard) {
    if (!g_duck.set) return;
    DWORD now = GetTickCount(), dt = now - g_duck.at;
    g_duck.at = now;
    if (dt > 100) dt = 100;
    if (heard) g_duck.heard = now | 1;
    float low = 1 - g_settings[S_VOICE_DUCK_AMOUNT] / 100.0f, want = g_duck.heard && now - g_duck.heard < DUCK_HOLD_MS ? low : 1, was = g_duck.now;
    if (g_duck.now > want) g_duck.now -= (1 - low) * (float)dt / DUCK_DOWN_MS, g_duck.now = g_duck.now < want ? want : g_duck.now;
    else if (g_duck.now < want) g_duck.now += (1 - low) * (float)dt / DUCK_RELEASE_MS, g_duck.now = g_duck.now > want ? want : g_duck.now;
    if (g_debug_log && was == 1 && g_duck.now < 1) logf("voice detail: ducking the game's sound to %.0f %%", low * 100);
    if (g_debug_log && was < 1 && g_duck.now == 1) logf("voice detail: game's sound back to 100 %%");
    if (fabsf(g_duck.now - g_duck.sent) > 0.005f || (g_duck.now == want && g_duck.sent != want)) duck_send(g_duck.now);
}
static void voice_step(void) {
    static DWORD next_log;
    u64 me = g_voice.me = g_diag.self(g_diag.user);
    DWORD now = GetTickCount();
    if (!next_log) next_log = now + LAG_LOG_MS;
    // push to talk: only while the game has the focus
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    int focus = pid == GetCurrentProcessId();
    unsigned pad = g_voice.xinput ? voice_pad() : 0;
    int prox = focus && (voice_key(g_settings[S_VOICE_KEY]) || voice_pad_bit(pad, g_settings[S_VOICE_PAD]));
    int team = !g_voice.loopback && !voice_we_invader() && focus &&
               (voice_key(g_settings[S_VOICE_TEAM_KEY]) || voice_pad_bit(pad, g_settings[S_VOICE_TEAM_PAD]));
    int talk = team ? 2 : prox ? 1 : 0;
    if (!!talk != !!g_voice.talking) {
        if (talk) g_voice.start(g_diag.user);
        else g_voice.stop(g_diag.user);
        if (g_voice.speaking && g_voice.friends) g_voice.speaking(g_voice.friends, me, talk != 0);
        if (g_debug_log) logf("voice detail: %s", talk ? (talk == 2 ? "talking (team)" : "talking") : "stopped talking");
    }
    g_voice.talking = talk;
    // capture (Steam goes on giving the last words for a moment after StopVoiceRecording)
    unsigned char pk[VOICE_PACKET];
    voice_hdr *h = (voice_hdr *)pk;
    static unsigned char cap[8192];
    unsigned cb = 0, n = 0;
    if (g_voice.avail(g_diag.user, &cb, NULL, 0) == 0 && cb && g_voice.get(g_diag.user, 1, cap, sizeof cap, &n, 0, NULL, 0, NULL, 0) == 0) {
        if (n > sizeof pk - sizeof *h) g_voice.too_big++, n = 0;  // dropped: one packet per 10 ms is far below this normally
        memcpy(pk + sizeof *h, cap, n);
    } else n = 0;
    if (n && g_voice.loopback) {
        voice_spk *s = voice_slot(me);
        if (s) {
            s->tx += n, s->tx_pkts++;
            voice_push(s, pk + sizeof *h, n);
            voice_apply(s, g_settings[S_VOICE_VOLUME] / 100.0f, 0);
        }
    } else if (n) {
        voice_where w;
        EnterCriticalSection(&g_vw_lock);
        w = g_vw;
        LeaveCriticalSection(&g_vw_lock);
        memset(h, 0, sizeof *h);
        h->magic = VOICE_MAGIC, h->seq = ++g_voice.seq, h->flags = g_voice.talking == 2 ? VOICE_TEAM : 0, h->ver = 1;
        h->block = w.block, h->field = w.field, memcpy(h->local, w.local, sizeof h->local), memcpy(h->world, w.world, sizeof h->world);
        for (int i = 0; i < 8; i++) {
            u64 id = g_lag[i].id;
            if (!id || id == me || !g_lag[i].rtt_last_ok) continue;
            voice_spk *s = voice_slot(id);
            if (!s) continue;
            voice_refresh(s, me);
            if ((h->flags & VOICE_TEAM) && s->invader) continue;
            g_p2p_send(g_diag.net, id, pk, (unsigned)sizeof *h + n, 0, VOICE_CHANNEL);
            s->tx += (unsigned)sizeof *h + n, s->tx_pkts++;
        }
    }
    // receive
    u64 from = 0;
    while (!g_voice.loopback && g_p2p_avail(g_diag.net, &n, VOICE_CHANNEL)) {
        if (!g_p2p_read(g_diag.net, pk, sizeof pk, &n, &from, VOICE_CHANNEL)) break;
        if (n <= sizeof *h || h->magic != VOICE_MAGIC) continue;
        if (!lag_find(from) || from == me) {
            g_voice.strangers++;
            continue;
        }
        voice_spk *s = voice_slot(from);
        if (!s) continue;
        voice_refresh(s, me);
        s->rx += n, s->rx_pkts++;
        if (s->seq_ok && now - s->last_pkt < 2000 && (short)(h->seq - s->seq) <= 0) {
            s->late++;
            continue;
        }
        s->seq = h->seq, s->seq_ok = 1, s->last_pkt = now;
        if (s->muted) continue;
        float gain = g_settings[S_VOICE_VOLUME] / 100.0f, pan = 0, dist = -1, d[3];
        if (h->flags & VOICE_TEAM) {
            if (voice_we_invader() || s->invader) {
                s->team_drop++;
                continue;
            }
        } else {
            voice_where w;
            EnterCriticalSection(&g_vw_lock);
            w = g_vw;
            LeaveCriticalSection(&g_vw_lock);
            float range = (float)(g_settings[S_VOICE_RANGE] > VOICE_FULL_M ? g_settings[S_VOICE_RANGE] : VOICE_FULL_M + 1);
            if (voice_delta(&w, h, d)) dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist < 0 || dist >= range) {
                s->distant++;
                continue;
            }
            if (dist > VOICE_FULL_M) gain *= 1 - (dist - VOICE_FULL_M) / (range - VOICE_FULL_M);
            float hl = sqrtf(d[0] * d[0] + d[2] * d[2]);
            if (w.cam && hl > 0.5f) pan = 0.8f * (d[0] * w.right[0] + d[2] * w.right[1]) / hl;
        }
        voice_push(s, pk + sizeof *h, n - (unsigned)sizeof *h);
        voice_apply(s, gain, pan);
        if (g_debug_log && now - s->detail_at > 1000)
            s->detail_at = now, logf("voice detail: from %04x%s: distance %.1f m, gain %.2f, pan %.2f, buffered %u ms, AGC %+.1f dB (speech %.1f dBFS)",
                                     id_tag(from), h->flags & VOICE_TEAM ? " (team)" : "", dist, gain, pan, (s->wr - s->rd) * 1000 / g_voice.rate,
                                     s->agc.agc_db, s->agc.lvl_db);
    }
    int heard = 0;
    for (int i = 0; i < VOICE_SLOTS; i++)
        if (g_voice.spk[i].id) voice_feed(&g_voice.spk[i]), heard |= g_voice.spk[i].playing && g_voice.spk[i].gain > 0.05f;
    voice_duck(heard);
    if ((int)(now - next_log) >= 0) next_log = now + LAG_LOG_MS, voice_log();
}
static DWORD WINAPI voice_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(10);
        InterlockedIncrement(&g_steam_busy);
        if (g_steam_down) {  // Steam is being shut down (process exit): no more Steam calls from this thread
            if (g_duck.set && g_duck.sent != 1) duck_send(1);  // never leave the game's sound down
            InterlockedDecrement(&g_steam_busy);
            logf("voice: stopped (Steam shutting down)%s", g_duck.sent != 1 ? "; the game's sound could NOT be restored (Wwise gone)" : "");
            return 0;
        }
        voice_step();
        InterlockedDecrement(&g_steam_busy);
    }
}

static void voice_start(void) {
    HMODULE m = GetModuleHandleA("steam_api64.dll");
    void *(*friends)(void) = m ? (void *(*)(void))GetProcAddress(m, "SteamAPI_SteamFriends_v017") : NULL;
    g_voice.start = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_StartVoiceRecording");
    g_voice.stop = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_StopVoiceRecording");
    g_voice.avail = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_GetAvailableVoice");
    g_voice.get = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_GetVoice");
    g_voice.decompress = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_DecompressVoice");
    g_voice.opt_rate = (void *)GetProcAddress(m, "SteamAPI_ISteamUser_GetVoiceOptimalSampleRate");
    g_voice.speaking = (void *)GetProcAddress(m, "SteamAPI_ISteamFriends_SetInGameVoiceSpeaking");
    g_voice.persona = (void *)GetProcAddress(m, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_voice.friends = friends ? friends() : NULL;
    g_voice.loopback = setting(S_VOICE_LOOPBACK);
    if (g_settings[S_VOICE_VOLUME] > 100) g_settings[S_VOICE_VOLUME] = 100;  // a trim on the AGC now (0.3.3 allowed 200)
    if (g_settings[S_VOICE_LEVEL] < 6 || g_settings[S_VOICE_LEVEL] > 30) g_settings[S_VOICE_LEVEL] = g_settings[S_VOICE_LEVEL] < 6 ? 6 : 30;
    if (g_settings[S_VOICE_DUCK_AMOUNT] > 90) g_settings[S_VOICE_DUCK_AMOUNT] = 90;
    g_agc_target = -(float)g_settings[S_VOICE_LEVEL];
    if (!g_voice.start || !g_voice.stop || !g_voice.avail || !g_voice.get || !g_voice.decompress || !g_voice.opt_rate) {
        logf("voice: NOT started: Steam voice exports missing from steam_api64.dll");
        return;
    }
    unsigned rate = g_voice.opt_rate(g_diag.user);
    g_voice.rate = rate >= 8000 && rate <= 48000 ? rate : 24000;
    {  // COM on this (our run) thread, in case the audio device enumeration under XAudio2Create wants it
        HMODULE ole = LoadLibraryA("ole32.dll");
        HRESULT(WINAPI * co_init)(void *, DWORD) = ole ? (void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
        if (co_init) co_init(NULL, COINIT_MULTITHREADED);
    }
    HMODULE xm = LoadLibraryA("xaudio2_9.dll");
    HRESULT(WINAPI * create)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR) = xm ? (void *)GetProcAddress(xm, "XAudio2Create") : NULL;
    HRESULT hr = create ? create(&g_voice.xa, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_FAIL;
    if (SUCCEEDED(hr)) hr = IXAudio2_CreateMasteringVoice(g_voice.xa, &g_voice.master, 0, 0, 0, NULL, NULL, AudioCategory_GameEffects);
    XAUDIO2_VOICE_DETAILS det = {0};
    if (SUCCEEDED(hr)) IXAudio2MasteringVoice_GetVoiceDetails(g_voice.master, &det);
    g_voice.out_ch = det.InputChannels >= 1 && det.InputChannels <= 8 ? det.InputChannels : 2;
    g_voice.spk = calloc(VOICE_SLOTS, sizeof *g_voice.spk);
    if (FAILED(hr) || !g_voice.spk) {
        logf("voice: NOT started: XAudio2 %s (0x%08lx)", !xm ? "xaudio2_9.dll not loaded" : !create ? "XAudio2Create missing" : "engine failed",
             (unsigned long)hr);
        return;  // ponytail: an engine that came up half is left as it is (never released, like the running one)
    }
    if (g_settings[S_VOICE_PAD] > 0 || g_settings[S_VOICE_TEAM_PAD] > 0) {
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        g_voice.xinput = x ? (void *)GetProcAddress(x, "XInputGetState") : NULL;
    }
    {  // voice-mute from the settings file (free text, see settings.h)
        char path[MAX_PATH], text[4096];
        next_to_dll(path, sizeof path, "test-settings.toml");
        FILE *f = fopen(path, "rb");
        size_t n = f ? fread(text, 1, sizeof text, f) : 0;
        if (f) fclose(f);
        settings_text(text, n, "voice-mute", g_voice.mute, sizeof g_voice.mute);
    }
    int muted = 0;
    for (const char *t = g_voice.mute; *t;) {
        while (*t == ',' || *t == ' ') t++;
        if (*t) muted++;
        while (*t && *t != ',') t++;
    }
    const char *duck = "off (voice-duck false or voice-duck-amount 0)";
    if (setting(S_VOICE_DUCK) && g_settings[S_VOICE_DUCK_AMOUNT] > 0) {  // the game's Wwise, by its exports (see the top)
        HMODULE exe = GetModuleHandleA(NULL);
        g_duck.ready = (void *)GetProcAddress(exe, "?IsInitialized@SoundEngine@AK@@YA_NXZ");
        g_duck.out_id = (void *)GetProcAddress(exe, "?GetOutputID@SoundEngine@AK@@YA_KKK@Z");
        g_duck.set = (void *)GetProcAddress(exe, "?SetOutputVolume@SoundEngine@AK@@YA?AW4AKRESULT@@_KM@Z");
        if (!g_duck.ready || !g_duck.out_id) g_duck.set = NULL;
        duck = g_duck.set ? "Wwise AK::SoundEngine::SetOutputVolume on the main output (exe export)" : "not available (Wwise exports not in the exe)";
    }
    InitializeCriticalSection(&g_vw_lock);
    g_voice.ready = 1;
    CreateThread(NULL, 0, voice_thread, NULL, 0, NULL);
    logf("voice: on%s: Steam voice ok, %u Hz (Steam's optimal %u); XAudio2 2.9 ok, %u output channels; push-to-talk key 0x%02x, team key 0x%02x, "
         "pad %d / %d%s; range %d m (full to %.0f m), volume %d %%, AGC target %.0f dBFS, %d muted; P2P channel %d; camera pan from CSCamera 0x%x",
         g_voice.loopback ? " (LOOPBACK test: your own voice, no network)" : "", g_voice.rate, rate, g_voice.out_ch, g_settings[S_VOICE_KEY],
         g_settings[S_VOICE_TEAM_KEY], g_settings[S_VOICE_PAD], g_settings[S_VOICE_TEAM_PAD],
         g_voice.xinput ? " (XInput)" : g_settings[S_VOICE_PAD] > 0 || g_settings[S_VOICE_TEAM_PAD] > 0 ? " (XInput NOT loaded)" : "",
         g_settings[S_VOICE_RANGE], VOICE_FULL_M, g_settings[S_VOICE_VOLUME], g_agc_target, muted, VOICE_CHANNEL, RVA_CS_CAMERA);
    if (g_duck.set)
        logf("voice: ducking: %s, the game's sound to %d %% while someone is heard (down %d ms, back after %d ms over %d ms)", duck,
             100 - g_settings[S_VOICE_DUCK_AMOUNT], DUCK_DOWN_MS, DUCK_HOLD_MS, DUCK_RELEASE_MS);
    else logf("voice: ducking: %s", duck);
}
// Run thread: once Steam and the P2P interface (lag_tick) are there.
static void voice_tick(void) {
    static int tried;
    if (tried || !(setting(S_VOICE) || setting(S_VOICE_LOOPBACK)) || !steam_ready() || !g_diag.user || !g_diag.self) return;
    tried = 1;
    if (!g_p2p_read && !setting(S_VOICE_LOOPBACK)) {
        logf("voice: NOT started: the P2P interface was not hooked (see the lag: lines)");
        return;
    }
    InterlockedIncrement(&g_steam_busy);
    if (!g_steam_down) voice_start();
    InterlockedDecrement(&g_steam_busy);
}

// Game thread (counts_tick): "MIC" while we talk, the names of whom we hear. 1 while the voice lines have the panel.
static int voice_ov_tick(void) {
    if (!g_voice.ready) return 0;
    static wchar_t last[2][200];
    wchar_t text[2][200] = {{0}};
    DWORD now = GetTickCount();
    if (g_voice.talking) wcscpy(text[0], g_voice.talking == 2 ? L"MIC (team)" : L"MIC");
    EnterCriticalSection(&g_vw_lock);
    for (int i = 0; i < VOICE_SLOTS; i++) {
        voice_spk *s = &g_voice.spk[i];
        if (!s->id || s->id == g_voice.me || !s->heard || now - s->heard > 300 || !s->name[0]) continue;
        size_t at = wcslen(text[1]);
        if (at + wcslen(s->name) + 4 < 200) _snwprintf(text[1] + at, 200 - at, L"%ls%ls", at ? L", " : L"\x00bb ", s->name);
    }
    LeaveCriticalSection(&g_vw_lock);
    if (!memcmp(text, last, sizeof text)) return g_voice_ov;
    memcpy(last, text, sizeof text);
    const unsigned short *lines[2];
    int k = 0;
    for (int i = 0; i < 2; i++)
        if (text[i][0]) lines[k++] = (const unsigned short *)last[i];
    if (k) ov_set_lines(lines, k), g_voice_ov = 1;
    else if (g_voice_ov) {
        g_voice_ov = 0;
        if (g_counts_n && counts_in_hub()) ov_set_lines(g_counts_lines, g_counts_n);
        else ov_hide(), g_ov.has_text = 0;
    }
    return g_voice_ov;
}

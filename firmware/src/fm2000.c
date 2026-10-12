/* SPDX-License-Identifier: GPL-3.0-only */
/* FM2000: a Music 2000-style song maker for the M-VAVE FM-1. Four tracks, a pool of riffs per track, and the
 * song as a timeline of bars you place riffs onto, left to right.
 *
 * The prototype: the song model (song.h), a test song, a simple synth and the ARRANGE screen.
 *
 *   ALGORITHM    the track (as on Felucca), clockwise: down
 *   SELECT       the bar under the cursor, clockwise: right (later in the song)
 *   PRESETS      the track's sound: tracks 1-3 a preset of their engine, track 4 (the drums) a kit
 *   KNOB 1       move the riff under the cursor along the timeline, a bar a detent; it trades places with a riff
 *                it runs into, both whole (placing riffs moves to another control, later)
 *   KNOB 2       the loop: clockwise on (from the cursor's bar), anticlockwise off
 *   KNOB 3       the loop's length, 1..32 bars
 *   KNOB 4       zoom 2x / 1x / 0.5x / 0.25x (4 / 8 / 16 / 32 bars in view): anticlockwise out (round the cursor),
 *                clockwise in (from the cursor's bar)
 *   PLAY         play the song from the cursor's bar / stop; REC arms (recording: not yet)
 *   GLO          the metronome on / off (for now)
 *   keys         play the track's sound; OCT- OCT+ the octave
 *
 * Tracks 1-3 will choose among sound engines; for now there is one, BASIC (a few waves and an envelope).
 * Track 4 is the drums: its notes pick drums (General MIDI where it has one), its sound is a kit.
 *
 * Integers only: the chip has no FPU and no libgcc (docs/RULES.md). platform.c gives the panel, the screen, the
 * sound and MIDI (docs/API.md); tests/host.c and web/ run this same file on a computer. */
#define APP_NAME "FM2000"
#include "platform.c"
#include "song.h"
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* (tests/host.c has none) */
#endif

static song_t song;

/* ================================================================== the sounds */
enum { K_SQUARE, K_PULSE, K_SAW, K_TRI, K_KICK, K_SNARE, K_CLAP, K_HAT, K_OPENHAT, K_TOM };

/* engines for tracks 1-3; each has presets. One engine so far. */
typedef struct {
    const char *name;                           /* <= 8 characters: the footer shows it */
    uint8_t kind;                               /* the wave */
    uint8_t att;                                /* attack: level per 16 frames / 16 (bigger = faster) */
    uint8_t dec;                                /* decay towards the sustain: a shift (bigger = slower) */
    uint8_t sus;                                /* sustain: 0..127 of the peak */
    uint8_t rel;                                /* release: a shift (bigger = slower) */
    uint8_t lp;                                 /* a one-pole low-pass: 1..32 / 32 of the way each frame (32 = open) */
} preset_t;
static const preset_t BASIC[] = {
    {"LEAD SQ", K_SQUARE, 120, 6, 100, 4, 18},
    {"PULSE", K_PULSE, 120, 5, 90, 4, 22},
    {"BASS SAW", K_SAW, 200, 5, 90, 3, 9},
    {"ACID", K_SAW, 200, 3, 30, 2, 6},
    {"PAD TRI", K_TRI, 4, 7, 110, 6, 32},
    {"PLUCK", K_TRI, 255, 3, 0, 3, 32},
    {"CHIP", K_SQUARE, 255, 4, 70, 2, 32},
};
typedef struct { const char *name; const preset_t *preset; uint32_t n; } engine_t;
static const engine_t ENGINE[] = {
    {"BASIC", BASIC, sizeof BASIC / sizeof BASIC[0]},
};
#define NENGINE (sizeof ENGINE / sizeof ENGINE[0])

/* the drum kits of track 4: pitch and length of each drum */
typedef struct { const char *name; uint8_t kick_hi, kick_lo, decay, tone; } kit_t;
static const kit_t KIT[] = {
    {"KIT 808", 140, 45, 6, 62},                /* a long low kick */
    {"KIT TIGHT", 170, 58, 5, 67},              /* short, higher */
    {"KIT LOFI", 120, 50, 4, 58},               /* (the noise is cut down: see render) */
};
#define NKIT (sizeof KIT / sizeof KIT[0])

static uint32_t sounds(uint32_t t)              /* how many sounds PRESETS goes through on track t */
{
    return t == SONG_DRUM_TRACK ? NKIT : ENGINE[song.track[t].engine % NENGINE].n;
}
static const char *sound_name(uint32_t t)
{
    const song_track_t *tr = &song.track[t];
    if (t == SONG_DRUM_TRACK) return KIT[tr->inst % NKIT].name;
    return ENGINE[tr->engine % NENGINE].preset[tr->inst % ENGINE[tr->engine % NENGINE].n].name;
}
static const preset_t *preset_of(uint32_t t)
{
    const song_track_t *tr = &song.track[t];
    const engine_t *e = &ENGINE[tr->engine % NENGINE];
    return &e->preset[tr->inst % e->n];
}

/* ================================================================== the voices (audio interrupt) */
#define NVOICE 12u
#define HELD 0xFFFFu                            /* a voice the keys hold (not the sequencer) */
typedef struct {
    uint32_t ph, inc, age;
    int32_t amp, peak, lp, hp;                  /* Q15 */
    uint16_t left;                              /* steps until the sequencer lets go; HELD = a key */
    uint8_t on, trk, note, kind, stage;         /* stage 0 attack, 1 decay / sustain */
} voice_t;
static voice_t voice[NVOICE];
static uint32_t ages, noise = 22222u;

static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9) as phase steps at 44.1 kHz; lower octaves shift */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t note_inc(uint32_t n) { return n > 127u ? 0 : INC_C9[n % 12u] >> (10u - n / 12u); }
#define HZ_INC 97391u                           /* phase step per Hz: 2^32 / 44100 */

static uint32_t drum_kind(uint32_t n)           /* General MIDI where it has one, else by the place in the octave */
{
    switch (n) {
    case 35: case 36: return K_KICK;
    case 38: case 40: return K_SNARE;
    case 39: return K_CLAP;
    case 42: case 44: return K_HAT;
    case 46: return K_OPENHAT;
    default: break;
    }
    switch (n % 12u) {
    case 0: return K_KICK;
    case 2: return K_SNARE;
    case 4: return K_CLAP;
    case 6: case 8: return K_HAT;
    case 10: return K_OPENHAT;
    default: return K_TOM;
    }
}

static void voice_on(uint32_t trk, uint32_t note, uint32_t vel, uint32_t steps)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, kind;
    voice_t *p;
    for (v = 0; v < NVOICE; v++) {              /* the same note on the track, else a free voice, else the oldest */
        uint32_t score = voice[v].on && voice[v].trk == trk && voice[v].note == note ? 0u
                       : voice[v].on ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    p = &voice[pick];
    kind = trk == SONG_DRUM_TRACK ? drum_kind(note) : preset_of(trk)->kind;
    p->trk = (uint8_t)trk;
    p->note = (uint8_t)note;
    p->kind = (uint8_t)kind;
    p->age = ++ages;
    p->left = (uint16_t)steps;
    p->peak = (int32_t)(vel * 64u);             /* 127 -> 8128 */
    p->stage = 0;
    p->lp = p->hp = 0;
    if (trk == SONG_DRUM_TRACK) {
        const kit_t *k = &KIT[song.track[trk].inst % NKIT];
        p->ph = 0;
        switch (kind) {
        case K_KICK: p->inc = k->kick_hi * HZ_INC; p->amp = p->peak * 2; break;
        case K_SNARE: p->inc = note_inc(k->tone); p->amp = p->peak; break;
        case K_CLAP: p->amp = p->peak; break;
        case K_HAT: case K_OPENHAT: p->amp = p->peak * 3 / 4; break;
        default: p->inc = note_inc(note + 12u); p->amp = p->peak * 3 / 2; break;
        }
    } else
        p->inc = note_inc(note);                /* (amp: from where it is, no click) */
    p->on = 1;
}
static void voice_off(uint32_t trk, uint32_t note, int held)
{
    uint32_t v;
    for (v = 0; v < NVOICE; v++)
        if (voice[v].on && voice[v].trk == trk && voice[v].note == note && (voice[v].left == HELD) == (held != 0))
            voice[v].on = 0;
}

/* the main loop asks, the audio interrupt does: a small queue (one writer each side) */
enum { Q_ON = 1, Q_OFF, Q_ALL_OFF };
static struct { uint8_t what, trk, note, vel; } q[32];
static volatile uint32_t q_w;
static uint32_t q_r;
static void ask(uint32_t what, uint32_t trk, uint32_t note, uint32_t vel)
{
    uint32_t i = q_w % 32u;
    if (q_w - q_r >= 32u) return;
    q[i].what = (uint8_t)what; q[i].trk = (uint8_t)trk; q[i].note = (uint8_t)note; q[i].vel = (uint8_t)vel;
    RING_PUBLISH();
    q_w++;
}

/* the sequencer: an absolute step (bar * 16 + step) and the phase into it (2^32 = one step) */
static volatile uint32_t seq_play, seq_from, seq_step, seq_bars, seq_click;
static uint32_t seq_ph, seq_was;
static uint32_t click_ph, click_inc;            /* the metronome: a short blip on each beat, higher on the bar */
static int32_t click_amp;
#define STEP_K_Q8 166214u                       /* 2^32 steps x 4 per beat / (600 x 44100) per frame per 0.1 BPM, x 256 */

static void seq_note(void *ctx, uint32_t trk, const song_ev_t *e) { (void)ctx; voice_on(trk, e->note, e->vel, e->len); }
static void seq_tick(void)
{
    uint32_t v, t;
    for (v = 0; v < NVOICE; v++)
        if (voice[v].on && voice[v].left != HELD && voice[v].left && --voice[v].left == 0)
            voice[v].on = 0;
    for (t = 0; t < SONG_TRACKS; t++)
        song_notes_at(&song, t, seq_step, seq_note, 0);
    if (seq_click && !(seq_step & 3u)) {
        click_inc = (seq_step & 15u) ? 2000u * HZ_INC : 3000u * HZ_INC;
        click_amp = (seq_step & 15u) ? 5000 : 7000;
        click_ph = 0;
    }
}

static int32_t sine_ish(uint32_t ph)            /* a parabola sine, Q15 */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x;
    return (x * (32768 - ax)) >> 13;
}

static int32_t render(voice_t *v, uint32_t lofi)   /* one frame of one voice, Q15 */
{
    int32_t s, n;
    noise = noise * 1664525u + 1013904223u;
    n = (int32_t)noise >> 17;                   /* -16384..16383 */
    if (lofi) n &= ~0xFFF;                      /* (a crunchier noise for the LOFI kit) */
    switch (v->kind) {
    case K_SQUARE: v->ph += v->inc; s = (int32_t)v->ph < 0 ? 12000 : -12000; break;
    case K_PULSE: v->ph += v->inc; s = v->ph < 0x40000000u ? 12000 : -12000; break;
    case K_SAW: v->ph += v->inc; s = (int32_t)v->ph >> 17; break;
    case K_TRI: { int32_t x = (int32_t)v->ph >> 16; v->ph += v->inc; s = (x < 0 ? -x : x) - 16384; break; }
    case K_KICK: s = sine_ish(v->ph); v->ph += v->inc; break;
    case K_SNARE: s = (sine_ish(v->ph) >> 1) + n; v->ph += v->inc; break;
    case K_CLAP: s = n; break;
    case K_HAT: case K_OPENHAT: s = n - v->hp; v->hp = n; break;
    default: s = sine_ish(v->ph); v->ph += v->inc; break;
    }
    if (v->trk != SONG_DRUM_TRACK) {            /* the preset's low-pass */
        uint32_t lp = preset_of(v->trk)->lp;
        if (lp < 32u) { v->lp += ((s - v->lp) * (int32_t)lp) >> 5; s = v->lp; }
    }
    return (s * v->amp) >> 15;
}

static void envelope(voice_t *v)                /* every 16 frames */
{
    if (v->trk == SONG_DRUM_TRACK) {
        uint32_t d = KIT[song.track[v->trk].inst % NKIT].decay, lo = KIT[song.track[v->trk].inst % NKIT].kick_lo;
        switch (v->kind) {
        case K_KICK: v->amp -= v->amp >> d; v->inc -= v->inc >> 5; if (v->inc < lo * HZ_INC) v->inc = lo * HZ_INC; break;
        case K_SNARE: v->amp -= v->amp >> (d - 1u); break;
        case K_CLAP: {                          /* three quick bursts, then a tail */
            uint32_t t = v->ph++;
            if (t == 12u || t == 24u) v->amp = v->peak;
            v->amp -= v->amp >> (t < 36u ? 2 : d - 1u);
            break;
        }
        case K_HAT: v->amp -= v->amp >> (d - 3u); break;
        case K_OPENHAT: v->amp -= v->amp >> (v->on ? d : d - 2u); break;
        default: v->amp -= v->amp >> d; v->inc -= v->inc >> 9; break;
        }
        if (v->amp < 8) { v->amp = 0; v->on = 0; }
        return;
    }
    {
        const preset_t *p = preset_of(v->trk);
        if (!v->on) {
            v->amp -= (v->amp >> p->rel) + 2;
        } else if (!v->stage) {                 /* attack */
            v->amp += (int32_t)p->att * 16;
            if (v->amp >= v->peak) { v->amp = v->peak; v->stage = 1; }
        } else {                                /* decay to the sustain */
            int32_t target = (v->peak * p->sus) >> 7;
            v->amp -= (v->amp - target) >> p->dec;
        }
        if (v->amp < 8 && (!v->on || v->stage)) v->amp = 0;
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k, lofi = KIT[song.track[SONG_DRUM_TRACK].inst % NKIT].name[4] == 'L';
    while (q_r != q_w) {
        uint32_t j = q_r % 32u;
        if (q[j].what == Q_ON) voice_on(q[j].trk, q[j].note, q[j].vel, HELD);
        else if (q[j].what == Q_OFF) voice_off(q[j].trk, q[j].note, 1);
        else for (k = 0; k < NVOICE; k++) voice[k].on = 0;
        q_r++;
    }
    for (i = 0; i < n; i++) {
        if (seq_play != seq_was) {
            seq_was = seq_play;
            if (seq_play) { seq_step = seq_from * STEPS_PER_BAR; seq_ph = 0; seq_tick(); }
            else for (k = 0; k < NVOICE; k++) if (voice[k].left != HELD) voice[k].on = 0;
        } else if (seq_play) {
            uint32_t p = seq_ph;
            seq_ph += (song.bpm10 * STEP_K_Q8) >> 8;
            if (seq_ph < p) {
                uint32_t st = seq_step + 1u, end = seq_bars * STEPS_PER_BAR;
                if (song.loop_end > song.loop_start && st >= song.loop_end * STEPS_PER_BAR)
                    st = song.loop_start * STEPS_PER_BAR;
                else if (st >= end)
                    st = 0;
                seq_step = st;
                seq_tick();
            }
        }
        if (click_amp) {
            out[2u * i] += (sine_ish(click_ph) * click_amp) >> 15;
            out[2u * i + 1u] += (sine_ish(click_ph) * click_amp) >> 15;
            click_ph += click_inc;
            if (!(i & 15u)) click_amp -= (click_amp >> 3) + 1;
            if (click_amp < 0) click_amp = 0;
        }
        if (!(i & 15u))
            for (k = 0; k < NVOICE; k++)
                if (voice[k].amp || voice[k].on) envelope(&voice[k]);
        for (k = 0; k < NVOICE; k++) {
            voice_t *v = &voice[k];
            const song_track_t *t;
            int32_t s;
            if (!v->amp) continue;
            t = &song.track[v->trk];
            s = (render(v, lofi && v->trk == SONG_DRUM_TRACK) * t->vol) >> 7;
            out[2u * i] += (s * (128 - (int32_t)t->pan)) >> 6;
            out[2u * i + 1u] += (s * (int32_t)t->pan) >> 6;
        }
    }
}

/* ================================================================== the test song */
static void notes(uint32_t t, uint32_t r, uint32_t steps, const char *name, const uint8_t *ev, uint32_t n)
{
    song_riff_t *f = riff_new(&song, t, r, steps);   /* ev: start, len, note, vel ... */
    uint32_t i;
    for (i = 0; name[i] && i < RIFF_NAME; i++) f->name[i] = name[i];
    if (i < RIFF_NAME) f->name[i] = 0;
    for (i = 0; i < n; i += 4u) riff_note_set(&song, t, r, ev[i], ev[i + 1u], ev[i + 2u], ev[i + 3u]);
}
#define NOTES(t, r, steps, name, ...) do { static const uint8_t E[] = {__VA_ARGS__}; notes(t, r, steps, name, E, sizeof E); } while (0)

static void test_song(void)
{
    static const uint8_t PLACE[][3] = {         /* bar, track, riff */
        {0, 2, 1}, {0, 3, 3}, {1, 3, 3}, {2, 3, 3}, {3, 3, 2},
        {4, 2, 1}, {4, 1, 1}, {4, 3, 1}, {5, 3, 1}, {6, 3, 1}, {7, 3, 2},
        {8, 2, 1}, {8, 1, 1}, {8, 0, 1}, {8, 3, 1}, {9, 3, 1}, {10, 3, 1}, {10, 0, 2}, {11, 3, 2},
        {12, 2, 1}, {12, 1, 2}, {12, 0, 1}, {12, 3, 1}, {13, 3, 1}, {14, 3, 1}, {14, 0, 3}, {15, 3, 2},
    };
    static const uint8_t BUSY[4] = {52, 48, 55, 50};
    uint32_t i;
    song_init(&song);
    song.name[0] = 'D'; song.name[1] = 'E'; song.name[2] = 'M'; song.name[3] = 'O';
    song.bpm10 = 1180;
    song.track[0].inst = 0;                     /* LEAD SQ */
    song.track[1].inst = 2;                     /* BASS SAW */
    song.track[2].inst = 4;                     /* PAD TRI */
    song.track[3].inst = 0;                     /* KIT 808 */
    song.track[0].pan = 80; song.track[2].pan = 48; song.track[2].vol = 80; song.track[3].vol = 120;
    NOTES(0, 1, 32, "HOOK", 0, 3, 76, 110, 4, 2, 74, 90, 6, 2, 72, 100, 8, 6, 69, 110, 16, 3, 72, 100, 20, 2, 74, 90,
          22, 2, 76, 100, 24, 8, 77, 110);
    NOTES(0, 2, 32, "ANSWR", 0, 3, 79, 110, 4, 2, 77, 90, 6, 2, 76, 100, 8, 4, 72, 100, 12, 4, 74, 90, 16, 6, 74, 110,
          24, 2, 71, 90, 26, 6, 67, 100);
    NOTES(0, 3, 32, "END", 0, 4, 76, 110, 4, 4, 74, 100, 8, 4, 71, 100, 12, 4, 74, 90, 16, 16, 69, 120);
    NOTES(1, 1, 64, "ROOTS", 0, 2, 45, 120, 2, 2, 45, 80, 4, 2, 57, 100, 6, 2, 45, 80, 8, 2, 45, 120, 10, 2, 45, 80,
          12, 2, 57, 100, 14, 2, 45, 80, 16, 2, 41, 120, 18, 2, 41, 80, 20, 2, 53, 100, 22, 2, 41, 80, 24, 2, 41, 120,
          26, 2, 41, 80, 28, 2, 53, 100, 30, 2, 41, 80, 32, 2, 48, 120, 34, 2, 48, 80, 36, 2, 60, 100, 38, 2, 48, 80,
          40, 2, 48, 120, 42, 2, 48, 80, 44, 2, 60, 100, 46, 2, 48, 80, 48, 2, 43, 120, 50, 2, 43, 80, 52, 2, 55, 100,
          54, 2, 43, 80, 56, 2, 43, 120, 58, 2, 43, 80, 60, 2, 55, 100, 62, 2, 47, 90);
    riff_copy(&song, 1, 1, 2);
    for (i = 0; i < 4u; i++) riff_note_set(&song, 1, 2, i * 16u + 15u, 1, BUSY[i], 110);
    song.riff[1][1].name[0] = 'B'; song.riff[1][1].name[1] = 'U'; song.riff[1][1].name[2] = 'S';
    song.riff[1][1].name[3] = 'Y'; song.riff[1][1].name[4] = 0;
    NOTES(2, 1, 64, "AMFCG", 0, 15, 57, 90, 0, 15, 60, 90, 0, 15, 64, 90, 16, 15, 57, 90, 16, 15, 60, 90,
          16, 15, 65, 90, 32, 15, 55, 90, 32, 15, 60, 90, 32, 15, 64, 90, 48, 15, 55, 90, 48, 15, 59, 90, 48, 15, 62, 90);
    NOTES(3, 1, 16, "BEAT", 0, 1, 36, 127, 2, 1, 42, 70, 4, 1, 38, 120, 6, 1, 42, 70, 8, 1, 36, 110, 10, 1, 36, 90,
          10, 1, 42, 70, 12, 1, 38, 120, 12, 1, 39, 80, 14, 1, 42, 70, 15, 1, 46, 80);
    NOTES(3, 2, 16, "FILL", 0, 1, 36, 127, 4, 1, 38, 110, 8, 1, 36, 110, 10, 1, 38, 90, 11, 1, 38, 100, 12, 1, 45, 110,
          13, 1, 43, 110, 14, 1, 41, 120, 15, 1, 38, 127);
    NOTES(3, 3, 16, "HATS", 0, 1, 42, 90, 2, 1, 42, 60, 4, 1, 42, 90, 6, 1, 42, 60, 8, 1, 42, 90, 10, 1, 42, 60,
          12, 1, 42, 90, 14, 1, 46, 70);
    for (i = 0; i < sizeof PLACE / sizeof PLACE[0]; i++)
        song_place(&song, PLACE[i][0], PLACE[i][1], PLACE[i][2]);
}

/* ================================================================== the screen: ARRANGE, time left to right */
#define C_ACC RGB(180, 140, 255)                /* lilac */
#define C_TXT RGB(220, 220, 220)
#define C_MID RGB(120, 120, 130)
#define C_LOW RGB(48, 48, 56)
#define C_BG RGB(14, 14, 20)
#define C_PLAY RGB(110, 230, 140)
static const uint16_t RIFF_C[8] = {RGB(180, 140, 255), RGB(90, 200, 190), RGB(240, 190, 80), RGB(240, 120, 150),
                                   RGB(130, 210, 110), RGB(110, 170, 250), RGB(250, 150, 90), RGB(190, 190, 190)};
static const uint16_t RIFF_D[8] = {RGB(70, 55, 105), RGB(35, 80, 76), RGB(96, 76, 32), RGB(96, 48, 60),
                                   RGB(52, 84, 44), RGB(44, 68, 100), RGB(100, 60, 36), RGB(76, 76, 76)};

#define HEAD_H 20                               /* the header */
#define RULER_Y 20                              /* the bar numbers */
#define RULER_H 16
#define LANE_Y 36                               /* the four tracks */
#define LANE_H 46
#define FOOT_Y 220
#define GX 40                                   /* the lanes start right of the track labels */
static const uint8_t ZOOM_BW[4] = {50, 25, 12, 6};   /* a bar's width at 2x 1x 0.5x 0.25x: 4 8 16 32 bars in 200 px */
static const char *const ZOOM_NAME[4] = {"ZOOM 2X", "ZOOM 1X", "ZOOM 0.5X", "ZOOM 0.25X"};
#define BW ((int32_t)ZOOM_BW[ui.zoom])
#define COLS (4u << ui.zoom)
#define ZOOM_1X 1u

static struct {
    uint32_t bar, trk, left, oct;               /* the cursor, the first bar shown, the keys' octave (0..4) */
    uint32_t keys_prev;
    uint8_t held[27];
    uint32_t drawn_pos;                         /* the playhead drawn (a step, or ~0) */
    uint32_t rec;                               /* REC armed (recording itself: not yet) */
    uint32_t zoom;                              /* 0 2x, 1 1x, 2 0.5x, 3 0.25x */
    uint32_t loop_len;                          /* the loop's length in bars: 4 each time it is switched on */
    uint32_t msg_until;                         /* the footer shows ui.msg until then (fm1_ms) */
    char msg[24];
    uint32_t dirty;
} ui;
enum { D_HEAD = 1, D_RULER = 2, D_LANES = 4, D_FOOT = 8, D_ALL = 15 };

static void put_num(char *p, uint32_t v, uint32_t digits)
{
    while (digits--) { p[digits] = (char)('0' + v % 10u); v /= 10u; }
}
static uint32_t copy_str(char *d, const char *s, uint32_t max)   /* returns the length */
{
    uint32_t i;
    for (i = 0; i < max && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return i;
}

/* text on a coloured ground: gfx.c's cv_text blends onto black, this blends from `bg` to `c` (black text on a
 * riff's block would otherwise get dark fringes) */
static int32_t cv_text_on(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg)
{
    uint16_t ramp[16];
    int32_t r0 = bg >> 11, g0 = (bg >> 5) & 63, b0 = bg & 31, r1 = c >> 11, g1 = (c >> 5) & 63, b1 = c & 31, a;
    for (a = 0; a < 16; a++)
        ramp[a] = (uint16_t)(((r0 + (r1 - r0) * a / 15) << 11) | ((g0 + (g1 - g0) * a / 15) << 5) | (b0 + (b1 - b0) * a / 15));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}

/* the header's icons: 11 x 11, a row a string, '#' = ink */
static const char *const ICON_PLAY[11] = {
    "##.........", "####.......", "######.....", "########...", "##########.", "###########",
    "##########.", "########...", "######.....", "####.......", "##........."};
static const char *const ICON_REC[11] = {
    "...#####...", ".#########.", ".#########.", "###########", "###########", "###########",
    "###########", "###########", ".#########.", ".#########.", "...#####..."};
static const char *const ICON_LOOP[11] = {
    "........#..", "..######.#.", ".#.......##", "#........#.", "#.......#..", "#.........#",
    "..#.......#", ".#........#", "##.......#.", ".#.######..", "..#........"};
static const char *const ICON_METRO[11] = {
    "....###...#", "....#.#..#.", "...#...##..", "...#..##...", "..#..##.#..", "..#.##..#..",
    ".#..#....#.", ".#.#.....#.", "#.........#", "###########", "###########"};

static void cv_icon(int32_t x, int32_t y, const char *const *rows, uint16_t c)
{
    int32_t i, j;
    for (j = 0; j < 11; j++)
        for (i = 0; i < 11; i++)
            if (rows[j][i] == '#') cv_pset(x + i, y + j, c);
}

/* the header: the screen's short name, the transport's state, the tempo. Screen names are short: ARR (the
 * arrangement), and later RIF (a riff), SND (a sound), MIX, SNG (song settings), SYS */
static void draw_head(const char *screen)
{
    char bpm[8] = "000.0";
    const char *v;
    int32_t x = (240 - (4 * 11 + 3 * 8)) / 2;   /* the four icons, 8 px apart, in the middle */
    put_num(bpm, song.bpm10 / 10u, 3);
    bpm[4] = (char)('0' + song.bpm10 % 10u);
    cv_begin(240, HEAD_H, C_BG);
    cv_text_on(6, 1, &FONT_M, screen, C_ACC, C_BG);
    cv_icon(x, 4, ICON_PLAY, seq_play ? C_PLAY : C_LOW);
    cv_icon(x + 19, 4, ICON_REC, ui.rec ? RGB(255, 80, 70) : C_LOW);
    cv_icon(x + 38, 4, ICON_LOOP, song.loop_end > song.loop_start ? C_ACC : C_LOW);
    cv_icon(x + 57, 4, ICON_METRO, seq_click ? RGB(240, 190, 80) : C_LOW);
    v = bpm[0] == '0' ? bpm + 1 : bpm;          /* the tempo, right */
    cv_text_on(240 - 6 - text_w(&FONT_M, v), 1, &FONT_M, v, C_TXT, C_BG);
    cv_blit(0, 0);
}

static void draw_ruler(void)
{
    uint32_t c;
    cv_begin(240, RULER_H, C_BLACK);
    for (c = 0; c < COLS; c++) {
        uint32_t bar = ui.left + c;
        int32_t x = GX + (int32_t)c * BW;
        char num[4];
        uint32_t len;
        if (bar >= SONG_BARS) break;
        len = bar + 1u >= 100u ? 3u : bar + 1u >= 10u ? 2u : 1u;
        put_num(num, bar + 1u, len);
        num[len] = 0;
        if (bar == ui.bar) cv_rect(x, 0, BW, RULER_H, C_LOW);
        cv_rect(x, RULER_H - (bar % 4u ? 3 : 6), 1, bar % 4u ? 3 : 6, bar % 4u ? C_LOW : C_MID);
        if (ui.zoom <= ZOOM_1X || !(bar % (ui.zoom == 3u ? 8u : 4u)) || bar == ui.bar)   /* (zoomed out: every 4th, 8th) */
            cv_text_on(x + (ui.zoom > ZOOM_1X ? 1 : 3), 0, &FONT_S, num, bar == ui.bar ? C_WHITE : bar % 4u ? C_MID : C_TXT,
                       bar == ui.bar ? C_LOW : C_BLACK);
        if (song.loop_end > song.loop_start && bar >= song.loop_start && bar < song.loop_end)
            cv_rect(x, 0, BW, 2, C_ACC);        /* the loop: a line over its bars */
    }
    cv_blit(0, RULER_Y);
}

/* a riff's notes inside its block, as a DAW shows a clip: time across, pitch up, the riff's own range filling the
 * block's height. off = the riff's step at x, vis bars of it in view, w px wide */
static void draw_notes(uint32_t t, const song_riff_t *f, uint32_t off, int32_t x, uint32_t vis, int32_t w, uint16_t ink)
{
    const int32_t top = 7, h = LANE_H - 15;     /* the area inside the block */
    uint32_t i, lo = 127, hi = 0, end = off + vis * STEPS_PER_BAR;
    int32_t range, nh;
    (void)t;
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
        if (song.ev[i].note < lo) lo = song.ev[i].note;
        if (song.ev[i].note > hi) hi = song.ev[i].note;
    }
    if (!f->count) return;
    range = (int32_t)(hi - lo);
    nh = range <= 6 ? 4 : range <= 12 ? 3 : 2;
    if (range < 4) range = 4;                   /* a few notes do not fill the block from edge to edge */
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
        const song_ev_t *e = &song.ev[i];
        uint32_t s = e->start, n = (uint32_t)e->start + e->len;
        int32_t x0, x1, y;
        if (n > f->steps) n = f->steps;
        if (n <= off || s >= end) continue;     /* outside the view */
        if (s < off) s = off;
        if (n > end) n = end;
        x0 = x + (int32_t)((s - off) * (uint32_t)BW / STEPS_PER_BAR);
        x1 = x + (int32_t)((n - off) * (uint32_t)BW / STEPS_PER_BAR);
        if (x1 > x + w) x1 = x + w;
        if (x1 - x0 > 2) x1--;                  /* a gap between notes that follow each other */
        if (x1 <= x0) x1 = x0 + 1;
        y = top + (int32_t)((range - (int32_t)(e->note - lo) - (range - (int32_t)(hi - lo)) / 2) * (h - nh) / range);
        cv_rect(x0, y, x1 - x0, nh, ink);
    }
}

static void draw_lane(uint32_t t, uint32_t pos)    /* one track, its label and its riffs; pos = the playhead step */
{
    uint32_t c, sel = t == ui.trk;
    const song_track_t *tr = &song.track[t];
    char lab[8], kind[5];
    cv_begin(240, LANE_H, C_BLACK);
    /* the label: the number, the kind, a mark when muted */
    cv_rect(0, 1, GX - 3, LANE_H - 2, sel ? C_LOW : C_BG);
    lab[0] = (char)('1' + t); lab[1] = 0;
    cv_text_on(5, 4, &FONT_S, lab, sel ? C_ACC : C_MID, sel ? C_LOW : C_BG);
    copy_str(kind, t == SONG_DRUM_TRACK ? "DRUM" : sound_name(t), 4);   /* what the track is: its sound, short */
    if (kind[3] == ' ') kind[3] = 0;
    cv_text_on(4, 24, &FONT_S, kind, tr->flags & SONG_MUTE ? C_MID : sel ? C_WHITE : C_TXT, sel ? C_LOW : C_BG);
    /* the bars: empty ones a dot, each riff one block from where it starts (or comes into view) to where it ends */
    for (c = 0; c < COLS;) {
        uint32_t bar = ui.left + c, b0 = 0, r, end, k, vis;
        int32_t x = GX + (int32_t)c * BW, w;
        if (bar >= SONG_BARS) break;
        r = song_at(&song, bar, t, &b0);
        if (!r) {
            cv_rect(x + BW / 2, LANE_H / 2, 1, 1, bar % 4u ? RGB(34, 34, 40) : C_LOW);
            c++;
            continue;
        }
        {
            const song_riff_t *f = &song.riff[t][r - 1u];
            uint16_t col = RIFF_C[f->color & 7u], dark = RIFF_D[f->color & 7u];
            end = b0 + riff_bars(f);
            for (k = b0 + 1u; k < end && k < SONG_BARS; k++)   /* cut by the next riff on the track */
                if (song.cell[k][t]) { end = k; break; }
            vis = (end < ui.left + COLS ? end : ui.left + COLS) - bar;   /* bars of it in view from here */
            w = (int32_t)(vis * BW) - 2;
            cv_rect(x + (bar == b0 ? 1 : 0), 3, w - (bar == b0 ? 1 : 0), LANE_H - 6, col);
            if (bar == b0) cv_rect(x + 1, 3, 2, LANE_H - 6, dark);
            draw_notes(t, f, (bar - b0) * STEPS_PER_BAR, x, vis, w, dark);
            c += vis;
        }
    }
    /* the cursor: a frame round the bar */
    if (sel && ui.bar >= ui.left && ui.bar < ui.left + COLS) {
        int32_t x = GX + (int32_t)(ui.bar - ui.left) * BW;
        cv_rect(x, 0, BW, 2, C_WHITE); cv_rect(x, LANE_H - 2, BW, 2, C_WHITE);
        cv_rect(x, 0, 2, LANE_H, C_WHITE); cv_rect(x + BW - 2, 0, 2, LANE_H, C_WHITE);
    }
    /* the playhead: a line at its step */
    if (pos != ~0u && pos >= ui.left * STEPS_PER_BAR && pos < (ui.left + COLS) * STEPS_PER_BAR)
        cv_rect(GX + (int32_t)((pos - ui.left * STEPS_PER_BAR) * BW / STEPS_PER_BAR), 0, 2, LANE_H, C_PLAY);
    cv_blit(0, LANE_Y + t * LANE_H);
}

static void draw_foot(void)
{
    char s[32];
    uint32_t b0 = 0, r = song_at(&song, ui.bar, ui.trk, &b0), n;
    cv_begin(240, 20, C_BG);
    if (ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) > 0) {   /* a short message after a change */
        cv_text_on(6, 2, &FONT_S, ui.msg, C_WHITE, C_BG);
        cv_blit(0, FOOT_Y);
        return;
    }
    n = copy_str(s, sound_name(ui.trk), 9);     /* the track's sound, left */
    cv_text_on(6, 2, &FONT_S, s, ui.trk == SONG_DRUM_TRACK ? RIFF_C[2] : C_ACC, C_BG);
    if (r) {                                    /* the riff under the cursor, right */
        const song_riff_t *f = &song.riff[ui.trk][r - 1u];
        s[0] = 'R';
        put_num(s + 1, r, 2);
        s[3] = ' ';
        n = 4u + copy_str(s + 4, f->name, RIFF_NAME);
        if (b0 != ui.bar) { s[n++] = ' '; s[n++] = '+'; s[n++] = (char)('0' + (ui.bar - b0)); s[n] = 0; }
    } else
        copy_str(s, "--", 3);
    cv_text_on(240 - 6 - text_w(&FONT_S, s), 2, &FONT_S, s, C_TXT, C_BG);
    cv_blit(0, FOOT_Y);
}

/* ================================================================== the main loop */
static void say(const char *s)                  /* a message in the footer for 1.5 s */
{
    copy_str(ui.msg, s, sizeof ui.msg - 1u);
    ui.msg_until = fm1_ms + 1500u;
    if (!ui.msg_until) ui.msg_until = 1;
    ui.dirty |= D_FOOT;
}
static uint32_t put_dec(char *p, uint32_t v)    /* v in decimal, no padding: the length */
{
    uint32_t n = v >= 100u ? 3u : v >= 10u ? 2u : 1u;
    put_num(p, v, n);
    return n;
}
static void say_loop(void)                      /* "LOOP 5-8  4 BARS" or "LOOP OFF  4 BARS" */
{
    char s[24];
    uint32_t n = copy_str(s, "LOOP ", 8);
    if (song.loop_end > song.loop_start) {
        n += put_dec(s + n, song.loop_start + 1u);
        s[n++] = '-';
        n += put_dec(s + n, song.loop_end);
    } else
        n += copy_str(s + n, "OFF", 4);
    s[n++] = ' '; s[n++] = ' ';
    n += put_dec(s + n, ui.loop_len);
    copy_str(s + n, ui.loop_len == 1u ? " BAR" : " BARS", 6);
    say(s);
}
static void loop_set(uint32_t start, uint32_t len)   /* len 0 = off */
{
    if (!len) { song.loop_start = song.loop_end = 0; return; }
    if (start + len > SONG_BARS) start = SONG_BARS - len;
    song.loop_start = (uint8_t)start;
    song.loop_end = (uint8_t)(start + len);
}

static void scroll_to(uint32_t bar)
{
    if (bar < ui.left) ui.left = bar;
    else if (bar >= ui.left + COLS) ui.left = bar - COLS + 1u;
}

static uint32_t next_riff(uint32_t t, uint32_t r, int32_t d)   /* 0 and the riffs the track has, round (for placing, later) */
{
    uint32_t k;
    for (k = 0; k <= SONG_RIFFS; k++) {
        r = d > 0 ? (r + 1u) % (SONG_RIFFS + 1u) : (r + SONG_RIFFS) % (SONG_RIFFS + 1u);
        if (!r || song.riff[t][r - 1u].steps) return r;
    }
    return 0;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void app_init(void)
{
    test_song();
    seq_bars = song_bars(&song);
    ui.oct = 2;
    ui.zoom = ZOOM_1X;
    ui.loop_len = 4;
    ui.drawn_pos = ~0u;
    ui.dirty = D_ALL;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

static void app_step(void)
{
    uint32_t b, rel, keys, down, up, k, pos;
    int32_t d;
    uint8_t st, d1, d2;

    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {       /* the track */
        ui.trk = (uint32_t)clamp((int32_t)ui.trk + d, 0, SONG_TRACKS - 1);
        ui.dirty |= D_LANES | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {          /* the bar */
        ui.bar = (uint32_t)clamp((int32_t)ui.bar + d, 0, SONG_BARS - 1);
        scroll_to(ui.bar);
        ui.dirty |= D_RULER | D_LANES | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {         /* the track's sound */
        song_track_t *tr = &song.track[ui.trk];
        tr->inst = (uint8_t)clamp((int32_t)tr->inst + d, 0, (int32_t)sounds(ui.trk) - 1);
        ui.dirty |= D_LANES | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_KNOB1)) != 0) {           /* move the riff under the cursor a bar a detent */
        uint32_t b0 = 0, a;
        if (!song_at(&song, ui.bar, ui.trk, &b0))
            say("NO RIFF HERE");
        else {
            uint32_t off = ui.bar - b0;         /* the cursor stays on the same bar of the riff */
            a = b0;
            for (; d > 0; d--) { uint32_t n = song_move_riff(&song, ui.trk, a, 1); if (n == a) break; a = n; }
            for (; d < 0; d++) { uint32_t n = song_move_riff(&song, ui.trk, a, -1); if (n == a) break; a = n; }
            if (a == b0) say("NO ROOM");
            ui.bar = a + off < SONG_BARS ? a + off : SONG_BARS - 1u;
            scroll_to(ui.bar);
            seq_bars = song_bars(&song);
            ui.dirty |= D_RULER | D_LANES | D_FOOT;
        }
    }
    if ((d = fm1_enc_take(ENC_KNOB2)) != 0) {           /* the loop: clockwise on (from the cursor's bar), back off */
        if (d > 0 && song.loop_end <= song.loop_start) { ui.loop_len = 4; loop_set(ui.bar, ui.loop_len); }
        else if (d < 0) loop_set(0, 0);
        say_loop();
        ui.dirty |= D_HEAD | D_RULER;
    }
    if ((d = fm1_enc_take(ENC_KNOB3)) != 0) {           /* the loop's length, 1..32 bars */
        ui.loop_len = (uint32_t)clamp((int32_t)ui.loop_len + d, 1, 32);
        if (song.loop_end > song.loop_start) loop_set(song.loop_start, ui.loop_len);
        say_loop();
        ui.dirty |= D_RULER;
    }
    if ((d = fm1_enc_take(ENC_KNOB4)) != 0) {           /* zoom: anticlockwise out, clockwise in, a step a detent */
        int32_t z = clamp((int32_t)ui.zoom - d, 0, 3);
        if ((uint32_t)z != ui.zoom) {
            ui.zoom = (uint32_t)z;
            if (d < 0) ui.left = ui.bar >= COLS / 2u ? ui.bar - COLS / 2u : 0u;   /* out: the cursor in the middle */
            else ui.left = ui.bar;              /* in: from where the cursor is */
            if (ui.left > SONG_BARS - COLS) ui.left = SONG_BARS - COLS;
            say(ZOOM_NAME[ui.zoom]);
            ui.dirty |= D_RULER | D_LANES;
        }
    }

    b = fm1_input_edges(&rel);
    if (b & (1u << BTN_REC)) { ui.rec = !ui.rec; ui.dirty |= D_HEAD; }
    if (b & (1u << BTN_GLO)) { seq_click = !seq_click; ui.dirty |= D_HEAD; }   /* the metronome (for now on GLO) */
    if (b & (1u << BTN_PLAY)) {
        if (!seq_play) {
            seq_bars = song_bars(&song);
            seq_from = ui.bar < seq_bars ? ui.bar : 0u;
            if (song.loop_end > song.loop_start && (seq_from < song.loop_start || seq_from >= song.loop_end))
                seq_from = song.loop_start;     /* a loop: start in it */
        }
        seq_play = !seq_play && seq_bars;
        ui.dirty |= D_HEAD | D_LANES;
    }
    if (b & (1u << BTN_OCT_DOWN) && ui.oct > 0u) ui.oct--;
    if (b & (1u << BTN_OCT_UP) && ui.oct < 4u) ui.oct++;

    keys = fm1_in.notes;                        /* the keys play the track's sound */
    down = (keys & ~ui.keys_prev) | fm1_input_note_edges();
    up = ui.keys_prev & ~keys;
    ui.keys_prev = keys;
    for (k = 0; k < 27u; k++) {
        if (up >> k & 1u) { ask(Q_OFF, ui.trk, ui.held[k], 0); midi_send(0x80u | ui.trk, ui.held[k], 0); }
        if (down >> k & 1u) {
            ui.held[k] = (uint8_t)(29u + 12u * ui.oct + k);   /* oct 2: key 0 = F3; on the drums F3 .. are GM drums 53 .. */
            if (ui.trk == SONG_DRUM_TRACK) ui.held[k] = (uint8_t)(35u + k);   /* the drums: key 0 = kick (35) */
            ask(Q_ON, ui.trk, ui.held[k], 100);
            midi_send(0x90u | ui.trk, ui.held[k], 100);
        }
    }
    while (midi_get(&st, &d1, &d2)) {           /* MIDI in, channels 1-4 = tracks 1-4 */
        uint32_t ch = st & 15u;
        if (ch >= SONG_TRACKS) continue;
        if ((st & 0xF0u) == 0x90u && d2) ask(Q_ON, ch, d1, d2);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) ask(Q_OFF, ch, d1, 0);
    }

    fm1_led_key(BTN_PLAY, seq_play != 0);
    fm1_led_key(BTN_REC, ui.rec != 0);
    fm1_led_key(BTN_GLO, seq_click != 0);
    fm1_led_key(BTN_OCT_DOWN, ui.oct < 2u);
    fm1_led_key(BTN_OCT_UP, ui.oct > 2u);
    for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), keys >> k & 1u);

    /* the playhead: redraw the lanes when it moves a step; page along when it leaves the view */
    pos = seq_play ? seq_step : ~0u;
    if (pos != ui.drawn_pos) {
        if (pos != ~0u) {
            uint32_t bar = pos / STEPS_PER_BAR;
            if (bar < ui.left || bar >= ui.left + COLS) {
                ui.left = bar > SONG_BARS - COLS ? SONG_BARS - COLS : bar;
                ui.dirty |= D_RULER;
            }
        }
        ui.drawn_pos = pos;
        ui.dirty |= D_LANES;
    }
    if (ui.dirty & D_HEAD) draw_head("ARR");
    if (ui.dirty & D_RULER) draw_ruler();
    if (ui.dirty & D_LANES) for (k = 0; k < SONG_TRACKS; k++) draw_lane(k, pos);
    if (ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) <= 0) { ui.msg_until = 0; ui.dirty |= D_FOOT; }
    if (ui.dirty & D_FOOT) draw_foot();
    ui.dirty = 0;
}

static void app_main(void)
{
    app_init();
    for (;;) {
        platform_poll();
        app_step();
    }
}

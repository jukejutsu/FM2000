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
 *   KNOB 2       repeats of the riff under the cursor: clockwise one more right after it (when the whole riff
 *                fits), anticlockwise one less; a riff and its repeats are one block, a line where each begins
 *   KNOB 3       the loop: anticlockwise on (4 bars from the cursor) and longer, clockwise shorter down to off
 *   KNOB 4       zoom 2x / 1x / 0.5x / 0.25x (4 / 8 / 16 / 32 bars in view): anticlockwise out (round the cursor),
 *                clockwise in (from the cursor's bar)
 *   PLAY         play the song from the cursor's bar / stop; REC arms (recording: not yet)
 *   SAVE         tap: PRJ, the projects (HOME back); hold 2 s: save (see PRJ below)
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
#include "project.h"
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
    uint32_t mode;                              /* M_ARR, M_PRJ */
    uint32_t msg_until;                         /* the footer shows ui.msg until then (fm1_ms) */
    char msg[24];
    uint32_t dirty;
} ui;
enum { D_HEAD = 1, D_RULER = 2, D_LANES = 4, D_FOOT = 8, D_ALL = 15 };
enum { M_ARR, M_PRJ };

/* the project: which slot it is in, what is not saved, the PRJ screen's state */
enum { ASK_NONE, ASK_CLEAR, ASK_LOAD, ASK_NEW, ASK_OVERWRITE };
static struct {
    uint32_t cur;                               /* the slot the project is in (1..8), or PRJ_NONE */
    uint32_t unsaved;                           /* changed since it was saved to its slot */
    uint32_t auto_due;                          /* changed since the last autosave */
    uint32_t touched;                           /* fm1_ms of the last input */
    uint32_t sel;                               /* PRJ: the row (0 AUTO, 1..8) */
    uint32_t ask, ask_slot;                     /* PRJ: a question waiting for OCT+ / OCT- */
    uint32_t used[PRJ_SLOTS];                   /* the slots' headers, read when PRJ opens and after a change */
    prj_head_t head[PRJ_SLOTS];
} prj;
static void edited(void) { prj.unsaved = prj.auto_due = 1; }

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

/* a riff's block: a 1 px frame with rounded corners (radius 2), filled with `fill`; open_l / open_r: it goes on past
 * the view's edge there, so no side and no corners there */
#define C_RIFF_EDGE RGB(150, 150, 162)          /* not full white: easy on the eyes */
#define C_RIFF_SEL_EDGE RGB(196, 172, 255)
#define C_RIFF_SEL RGB(52, 42, 80)              /* the riff under the cursor: a pale violet */
#define C_NOTE RGB(128, 128, 140)
#define C_NOTE_SEL RGB(214, 196, 255)
/* ARR: the grid, very faint, and the riffs as solid blocks with dark notes */
#define C_GRID_BAR RGB(18, 18, 24)              /* a bar */
#define C_GRID_4 RGB(30, 30, 38)                /* every 4 bars */
#define C_GRID_LANE RGB(30, 30, 38)             /* between the tracks */
#define C_BLOCK RGB(104, 104, 118)              /* a riff */
#define C_BLOCK_SEL RGB(182, 156, 250)          /* the riff under the cursor */
#define C_BLOCK_NOTE RGB(44, 44, 54)
#define C_BLOCK_NOTE_SEL RGB(62, 38, 118)
#define C_BLOCK_SPLIT RGB(64, 64, 76)           /* where a repeat begins */
#define C_BLOCK_SPLIT_SEL RGB(120, 92, 196)

/* a solid block with its corners rounded (1 px off each); open_l / open_r: it goes on past the view's edge there */
static void riff_block(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t open_l, uint32_t open_r, uint16_t c)
{
    if (w <= 0) return;
    cv_rect(x, y, w, h, c);
    if (w < 3) return;
    if (!open_l) { cv_pset(x, y, C_BLACK); cv_pset(x, y + h - 1, C_BLACK); }
    if (!open_r) { cv_pset(x + w - 1, y, C_BLACK); cv_pset(x + w - 1, y + h - 1, C_BLACK); }
}
static void riff_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t open_l, uint32_t open_r, uint16_t edge,
                       uint16_t fill)
{
    int32_t l = open_l ? 0 : 2, r = open_r ? 0 : 2;   /* where the straight top / bottom start and end */
    if (w < 3) { cv_rect(x, y, w > 0 ? w : 1, h, edge); return; }
    if (fill != C_BLACK) {
        int32_t x0 = open_l ? x : x + 1, x1 = open_r ? x + w : x + w - 1;
        cv_rect(x0, y + 1, x1 - x0, h - 2, fill);
    }
    cv_rect(x + l, y, w - l - r, 1, edge);                 /* top */
    cv_rect(x + l, y + h - 1, w - l - r, 1, edge);         /* bottom */
    if (!open_l) {
        cv_rect(x, y + 2, 1, h - 4, edge);                 /* left side, and its corners */
        cv_pset(x + 1, y + 1, edge);
        cv_pset(x + 1, y + h - 2, edge);
    }
    if (!open_r) {
        cv_rect(x + w - 1, y + 2, 1, h - 4, edge);
        cv_pset(x + w - 2, y + 1, edge);
        cv_pset(x + w - 2, y + h - 2, edge);
    }
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
    /* the grid under it all: a faint line on every bar, a less faint one every 4 bars, one under the track
     * (zoomed out to 0.25x, only the 4-bar lines: a line every 6 px would be a grey wash) */
    for (c = 0; c <= COLS; c++) {
        uint32_t bar = ui.left + c;
        if (bar % 4u && ui.zoom == 3u) continue;
        cv_rect(GX + (int32_t)c * BW, 0, 1, LANE_H, bar % 4u ? C_GRID_BAR : C_GRID_4);
    }
    cv_rect(GX, LANE_H - 1, 240 - GX, 1, C_GRID_LANE);
    /* the riffs: a riff and its repeats are one block, a line where each repeat begins; the repeat under the cursor
     * lit; the notes drawn dark in it at 2x and 1x, the block alone further out */
    for (c = 0; c < COLS;) {
        uint32_t bar = ui.left + c, b0 = 0, r, end, vis;
        if (bar >= SONG_BARS) break;
        r = song_at(&song, bar, t, &b0);
        if (!r) { c++; continue; }
        {
            const song_riff_t *f = &song.riff[t][r - 1u];
            uint32_t first, last, view_end = ui.left + COLS, len = riff_bars(f), open_r, a, hb = ~0u;
            int32_t y = 3, h = LANE_H - 7;
            song_chain(&song, t, b0, &first, &last, &end);
            if (end > view_end) end = view_end;
            vis = end - bar;                    /* bars of it in view from here */
            open_r = song_block_end(&song, t, last) > view_end;   /* it goes on past the view */
            if (sel && ui.bar >= first && ui.bar < end) song_at(&song, ui.bar, t, &hb);   /* the lit repeat */
            for (a = first; a <= last; a += len) {          /* each repeat: its block, its line, its notes */
                uint32_t s0 = a > bar ? a : bar, s1 = a + len < end ? a + len : end, lit = a == hb;
                int32_t xa = GX + (int32_t)(s0 - ui.left) * BW, wa = (int32_t)((s1 - s0) * BW);
                if (s1 <= s0) continue;
                riff_block(xa + (s0 == first ? 1 : 0), y, wa - (s0 == first ? 1 : 0) - (s1 == end && !open_r ? 1 : 0), h,
                           s0 != first, s1 != end || open_r, lit ? C_BLOCK_SEL : C_BLOCK);
                if (a > bar) cv_rect(xa, y + 3, 1, h - 6, lit ? C_BLOCK_SPLIT_SEL : C_BLOCK_SPLIT);
                if (ui.zoom <= ZOOM_1X)
                    draw_notes(t, f, (s0 - a) * STEPS_PER_BAR, xa + 1, s1 - s0, wa - 3, lit ? C_BLOCK_NOTE_SEL : C_BLOCK_NOTE);
            }
            c += vis;
        }
    }
    /* the cursor: a frame round the bar */
    if (sel && ui.bar >= ui.left && ui.bar < ui.left + COLS) {
        int32_t x = GX + (int32_t)(ui.bar - ui.left) * BW;
        cv_rect(x, 0, BW, 1, C_TXT); cv_rect(x, LANE_H - 1, BW, 1, C_TXT);
        cv_rect(x, 0, 1, LANE_H, C_TXT); cv_rect(x + BW - 1, 0, 1, LANE_H, C_TXT);
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
    cv_text_on(6, 2, &FONT_S, s, C_ACC, C_BG);
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
static void say_loop(void)                      /* "LOOP 5-8  4 BARS" or "LOOP OFF" */
{
    char s[24];
    uint32_t n = copy_str(s, "LOOP ", 8), len = song.loop_end - song.loop_start;
    if (song.loop_end > song.loop_start) {
        n += put_dec(s + n, song.loop_start + 1u);
        s[n++] = '-';
        n += put_dec(s + n, song.loop_end);
        s[n++] = ' '; s[n++] = ' ';
        n += put_dec(s + n, len);
        copy_str(s + n, len == 1u ? " BAR" : " BARS", 6);
    } else
        copy_str(s + n, "OFF", 4);
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

/* ================================================================== PRJ: the projects (SAVE) */
/* SAVE tapped: the PRJ screen (HOME leaves it). ALGORITHM picks a row: AUTO, then slots 1..8. OCT+ loads the
 * row's project, or makes a new one in an empty slot; OCT- clears the slot. A question (clear? discard the
 * changes? overwrite?) waits for OCT+ yes / OCT- no. SAVE held 2 s, anywhere, saves: on PRJ into the row's slot
 * (save as), elsewhere into the project's own slot; not while playing (an erase stops the sound). */
#define PRJ_ROW_Y 22
#define PRJ_ROW_H 22
#define SAVE_HOLD_MS 2000u
#define SAVE_TAP_MS 300u                        /* shorter: a tap (the menu); longer: the countdown shows */

static void prj_refresh(void)                   /* the slots' headers */
{
    uint32_t s;
    for (s = 0; s < PRJ_SLOTS; s++) prj.used[s] = !prj_info(s, &prj.head[s]);
}

static const char *const ICON_DISK[16] = {      /* a floppy disk, 16 x 16 */
    "##############..", "#..#######..#.#.", "#..#######..#..#", "#..#######..#..#", "#..#######.....#",
    "#..##########..#", "#..............#", "#..............#", "#.############.#", "#.#..........#.#",
    "#.#.######...#.#", "#.#..........#.#", "#.#.#######..#.#", "#.#..........#.#", "#.#..........#.#",
    "################"};

/* a box in the middle of the screen: the disk, a title, a line under it (the countdown, the slot) */
static struct { uint32_t on, until; } splash;
static void draw_splash(const char *title, const char *sub, uint16_t ink)
{
    int32_t i, j, tx;
    cv_begin(240, 64, C_BLACK);
    riff_frame(30, 2, 180, 60, 0, 0, ink, C_BG);
    for (j = 0; j < 16; j++)
        for (i = 0; i < 16; i++)
            if (ICON_DISK[j][i] == '#') { cv_pset(48 + i * 2, 16 + j * 2, ink); cv_pset(49 + i * 2, 16 + j * 2, ink);
                                          cv_pset(48 + i * 2, 17 + j * 2, ink); cv_pset(49 + i * 2, 17 + j * 2, ink); }
    tx = 92;
    cv_text_on(tx, 13, &FONT_M, title, C_TXT, C_BG);
    if (sub && *sub) cv_text_on(tx, 34, &FONT_S, sub, ink, C_BG);
    cv_blit(0, 88);
    splash.on = 1;
}
static void splash_for(const char *title, const char *sub, uint16_t ink, uint32_t ms)   /* shown, then gone */
{
    draw_splash(title, sub, ink);
    splash.until = fm1_ms + ms;
    if (!splash.until) splash.until = 1;
}

static void slot_name(char *s, uint32_t slot)   /* "SLOT 3" */
{
    uint32_t n = copy_str(s, slot == PRJ_AUTO ? "AUTO" : "SLOT ", 6);
    if (slot != PRJ_AUTO) { s[n++] = (char)('0' + slot); s[n] = 0; }
}

static void draw_prj_row(uint32_t slot)
{
    const prj_head_t *h = &prj.head[slot];
    uint32_t sel = slot == prj.sel, cur = slot == prj.cur && slot != PRJ_AUTO;
    uint16_t bg = sel ? C_RIFF_SEL : C_BLACK;
    char s[24], lab[2] = {(char)(slot == PRJ_AUTO ? 'A' : '0' + slot), 0};
    uint32_t n;
    cv_begin(240, PRJ_ROW_H, C_BLACK);
    if (sel) riff_frame(4, 1, 232, PRJ_ROW_H - 2, 0, 0, C_RIFF_SEL_EDGE, C_RIFF_SEL);
    cv_text_on(12, 3, &FONT_S, lab, cur ? C_ACC : C_MID, bg);
    if (!prj.used[slot]) {
        cv_text_on(32, 3, &FONT_S, slot == PRJ_AUTO ? "NO AUTOSAVE" : "EMPTY", sel ? C_MID : C_LOW, bg);
    } else {
        n = copy_str(s, h->name, SONG_NAME);
        if (!n) n = copy_str(s, "UNTITLED", 9);
        if (cur && prj.unsaved) { s[n++] = '*'; s[n] = 0; }
        cv_text_on(32, 3, &FONT_S, s, cur ? C_ACC : slot == PRJ_AUTO ? C_MID : C_TXT, bg);
        n = put_dec(s, h->bars);
        copy_str(s + n, h->bars == 1u ? " BAR" : " BARS", 6);
        cv_text_on(228 - text_w(&FONT_S, s), 3, &FONT_S, s, C_MID, bg);
    }
    cv_blit(0, PRJ_ROW_Y + slot * PRJ_ROW_H);
}

static void draw_prj_foot(void)
{
    char s[32];
    uint32_t n;
    cv_begin(240, 20, C_BG);
    if (ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) > 0)
        cv_text_on(6, 2, &FONT_S, ui.msg, C_WHITE, C_BG);
    else if (prj.ask) {                         /* the question, and its two answers */
        static const char *const Q[] = {"", "CLEAR ", "DISCARD CHANGES?", "DISCARD CHANGES?", "OVERWRITE "};
        n = copy_str(s, Q[prj.ask], 20);
        if (prj.ask == ASK_CLEAR || prj.ask == ASK_OVERWRITE) {
            slot_name(s + n, prj.ask_slot);
            n += 6u;
            s[n++] = '?'; s[n] = 0;
        }
        cv_text_on(6, 2, &FONT_S, s, C_WHITE, C_BG);
        cv_text_on(234 - text_w(&FONT_S, "+ YES  - NO"), 2, &FONT_S, "+ YES  - NO", C_ACC, C_BG);
    } else if (prj.sel == PRJ_AUTO)
        cv_text_on(6, 2, &FONT_S, prj.used[0] ? "OCT+ LOAD" : "", C_MID, C_BG);
    else if (prj.used[prj.sel])
        cv_text_on(6, 2, &FONT_S, "OCT+ LOAD   OCT- CLEAR", C_MID, C_BG);
    else
        cv_text_on(6, 2, &FONT_S, "OCT+ NEW   HOLD SAVE: SAVE", C_MID, C_BG);
    cv_blit(0, FOOT_Y);
}

static void draw_prj(void)
{
    uint32_t s;
    lcd_fill(0, HEAD_H, 240, PRJ_ROW_Y - HEAD_H, C_BLACK);   /* (the strip ARR's ruler leaves) */
    for (s = 0; s < PRJ_SLOTS; s++) draw_prj_row(s);
    draw_prj_foot();
}

static void after_load(void)                    /* a new song in place: the view and the sequencer from the start */
{
    seq_play = 0;
    seq_bars = song_bars(&song);
    ui.bar = ui.left = 0;
    prj.auto_due = 0;
    ui.dirty = D_ALL;
}

static void load_slot(uint32_t slot)
{
    prj_head_t h;
    seq_play = 0;
    if (prj_load(slot, &song, &h)) {
        song_init(&song);
        prj.cur = PRJ_NONE;
        prj.unsaved = 1;
        say("LOAD FAILED");
    } else if (slot == PRJ_AUTO) {              /* the autosave: back to the slot it came from, its changes unsaved */
        prj.cur = h.origin >= 1u && h.origin < PRJ_SLOTS ? h.origin : PRJ_NONE;
        prj.unsaved = (h.flags & PRJ_UNSAVED) != 0;
        say("AUTOSAVE LOADED");
    } else {
        prj.cur = slot;
        prj.unsaved = 0;
        say("LOADED");
    }
    after_load();
}

static void new_project(uint32_t slot)          /* empty, default sounds, 120 BPM, saved into its slot at once */
{
    seq_play = 0;
    song_init(&song);
    copy_str(song.name, "SONG ", 6);
    song.name[5] = (char)('0' + slot);
    song.track[0].inst = 0;                     /* LEAD SQ, BASS SAW, PAD TRI, KIT 808 */
    song.track[1].inst = 2;
    song.track[2].inst = 4;
    prj.cur = slot;
    prj.unsaved = 0;
    if (prj_save(slot, &song, slot, 0)) { prj.unsaved = 1; say("SAVE FAILED"); }
    else say("NEW PROJECT");
    after_load();
}

static void save_to(uint32_t slot)
{
    char s[8];
    if (prj_save(slot, &song, slot, 0)) {
        splash_for("SAVE FAILED", "", RGB(255, 90, 80), 1200);
        return;
    }
    prj.cur = slot;
    prj.unsaved = prj.auto_due = 0;
    prj_refresh();
    slot_name(s, slot);
    splash_for("SAVED", s, C_ACC, 800);
}

static void clear_slot(uint32_t slot)
{
    prj_clear(slot);
    if (slot == prj.cur) { prj.cur = PRJ_NONE; prj.unsaved = 1; say("NOT SAVED"); }
    else say("CLEARED");
}

/* SAVE held to the end of the countdown */
static void save_now(void)
{
    uint32_t t = ui.mode == M_PRJ && prj.sel != PRJ_AUTO ? prj.sel : prj.cur;
    if (t == PRJ_NONE || t == PRJ_AUTO) {       /* nowhere yet: pick a slot on PRJ */
        uint32_t s;
        splash_for("PICK A SLOT", "THEN HOLD SAVE", C_ACC, 1200);
        ui.mode = M_PRJ;
        prj_refresh();
        for (s = 1; s < PRJ_SLOTS && prj.used[s]; s++) {}
        prj.sel = s < PRJ_SLOTS ? s : 1u;
        return;
    }
    prj_refresh();
    if (prj.used[t] && t != prj.cur) {          /* another project there: ask first */
        splash.on = 0;
        ui.mode = M_PRJ;
        prj.sel = t;
        prj.ask = ASK_OVERWRITE;
        prj.ask_slot = t;
        ui.dirty = D_ALL;
        return;
    }
    save_to(t);
}

/* PRJ's controls: ALGORITHM the row, OCT+ / OCT- the actions and the answers */
static void prj_input(int32_t d, uint32_t b)
{
    uint32_t s = prj.sel;
    if (d) {
        prj.sel = (uint32_t)clamp((int32_t)prj.sel + d, 0, PRJ_SLOTS - 1);
        prj.ask = ASK_NONE;
        ui.dirty |= D_LANES;
    }
    if (b & (1u << BTN_OCT_DOWN)) {
        if (prj.ask) prj.ask = ASK_NONE;        /* no */
        else if (s != PRJ_AUTO && prj.used[s]) { prj.ask = ASK_CLEAR; prj.ask_slot = s; }
        ui.dirty |= D_LANES;
    }
    if (b & (1u << BTN_OCT_UP)) {
        uint32_t a = prj.ask;
        prj.ask = ASK_NONE;
        if (a == ASK_CLEAR) clear_slot(prj.ask_slot);
        else if (a == ASK_LOAD) load_slot(s);
        else if (a == ASK_NEW) new_project(s);
        else if (a == ASK_OVERWRITE) {
            if (seq_play) say("STOP TO SAVE");
            else save_to(prj.ask_slot);
        } else if (prj.used[s]) {               /* load: ask first if there is work to lose */
            if (prj.unsaved) prj.ask = ASK_LOAD;
            else load_slot(s);
        } else if (s != PRJ_AUTO) {             /* an empty slot: a new project */
            if (prj.unsaved) prj.ask = ASK_NEW;
            else new_project(s);
        }
        prj_refresh();
        ui.dirty |= D_LANES | D_HEAD;
    }
}

static void app_init(void)
{
    uint32_t r;
    prj_scan();
    r = prj_resume();
    prj.cur = PRJ_NONE;
    prj.sel = 1;
    if (r != PRJ_NONE) load_slot(r);            /* where we were */
    else test_song();                           /* nothing saved yet: the demo */
    ui.msg_until = 0;
    seq_bars = song_bars(&song);
    ui.oct = 2;
    ui.zoom = ZOOM_1X;
    ui.drawn_pos = ~0u;
    ui.dirty = D_ALL;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

static int32_t enc(uint32_t e)                   /* an encoder's detents, and the input noted (for the autosave) */
{
    int32_t d = fm1_enc_take(e);
    if (d) prj.touched = fm1_ms;
    return d;
}

/* SAVE: a tap opens PRJ; held, a countdown, then the save (not while playing) */
static struct { uint32_t t0, state, shown; } hold;   /* state 0 up, 1 down, 2 counting, 3 done */
static void save_button(uint32_t b, uint32_t rel)
{
    uint32_t el = fm1_ms - hold.t0;
    if (b & (1u << BTN_SAVE)) { hold.t0 = fm1_ms; hold.state = 1; hold.shown = ~0u; el = 0; }
    if (hold.state == 1 && el >= SAVE_TAP_MS) {
        if (seq_play) { splash_for("STOP TO SAVE", "", RGB(255, 90, 80), 1000); hold.state = 3; }
        else hold.state = 2;
    }
    if (hold.state == 2) {
        if (el >= SAVE_HOLD_MS) {
            hold.state = 3;
            splash.on = 0;
            save_now();
        } else {
            uint32_t left = (SAVE_HOLD_MS - el + 99u) / 100u;   /* tenths of a second */
            if (left != hold.shown) {
                char s[8];
                hold.shown = left;
                s[0] = (char)('0' + left / 10u); s[1] = '.'; s[2] = (char)('0' + left % 10u); s[3] = 0;
                draw_splash("SAVING", s, C_ACC);
                splash.until = 0;               /* (until the countdown ends or SAVE is let go) */
            }
        }
    }
    if (rel & (1u << BTN_SAVE)) {
        if (hold.state == 1 && ui.mode != M_PRJ) {   /* a tap: the menu */
            ui.mode = M_PRJ;
            prj.ask = ASK_NONE;
            prj.sel = prj.cur != PRJ_NONE ? prj.cur : prj.sel;
            prj_refresh();
            ui.dirty = D_ALL;
        }
        if (hold.state == 2) { splash.on = 0; ui.dirty = D_ALL; }   /* let go too soon: nothing saved */
        hold.state = 0;
    }
}

/* the autosave: stopped, nothing touched for 10 s, and something changed since the last one */
static void autosave(void)
{
    if (!prj.auto_due || seq_play || hold.state || (fm1_ms - prj.touched) < 10000u) return;
    prj.auto_due = 0;
    prj_save(PRJ_AUTO, &song, prj.cur, prj.unsaved ? PRJ_UNSAVED : 0);
    if (ui.mode == M_PRJ) { prj_refresh(); ui.dirty |= D_LANES; }
}

static void app_step(void)
{
    uint32_t b, rel, keys, down, up, k, pos;
    int32_t d, pd = 0;
    uint8_t st, d1, d2;

    if (ui.mode == M_PRJ) {                     /* PRJ: ALGORITHM the row; SELECT (pages, later) and the knobs idle */
        pd = enc(ENC_ALGORITHM);
        enc(ENC_SELECT); enc(ENC_PRESETS); enc(ENC_KNOB1); enc(ENC_KNOB2); enc(ENC_KNOB3); enc(ENC_KNOB4);
    } else {
        if ((d = enc(ENC_ALGORITHM)) != 0) {       /* the track */
            ui.trk = (uint32_t)clamp((int32_t)ui.trk + d, 0, SONG_TRACKS - 1);
            ui.dirty |= D_LANES | D_FOOT;
        }
        if ((d = enc(ENC_SELECT)) != 0) {          /* the bar */
            ui.bar = (uint32_t)clamp((int32_t)ui.bar + d, 0, SONG_BARS - 1);
            scroll_to(ui.bar);
            ui.dirty |= D_RULER | D_LANES | D_FOOT;
        }
        if ((d = enc(ENC_PRESETS)) != 0) {         /* the track's sound */
            song_track_t *tr = &song.track[ui.trk];
            tr->inst = (uint8_t)clamp((int32_t)tr->inst + d, 0, (int32_t)sounds(ui.trk) - 1);
            edited();
            ui.dirty |= D_LANES | D_FOOT;
        }
        if ((d = enc(ENC_KNOB1)) != 0) {           /* move the riff under the cursor a bar a detent */
            uint32_t b0 = 0, a;
            if (!song_at(&song, ui.bar, ui.trk, &b0))
                say("NO RIFF HERE");
            else {
                uint32_t off = ui.bar - b0;         /* the cursor stays on the same bar of the riff */
                a = b0;
                for (; d > 0; d--) { uint32_t n = song_move_riff(&song, ui.trk, a, 1); if (n == a) break; a = n; }
                for (; d < 0; d++) { uint32_t n = song_move_riff(&song, ui.trk, a, -1); if (n == a) break; a = n; }
                if (a == b0) say("NO ROOM");
                else edited();
                ui.bar = a + off < SONG_BARS ? a + off : SONG_BARS - 1u;
                scroll_to(ui.bar);
                seq_bars = song_bars(&song);
                ui.dirty |= D_RULER | D_LANES | D_FOOT;
            }
        }
        if ((d = enc(ENC_KNOB2)) != 0) {           /* repeats of the riff under the cursor: clockwise one more, back one less */
            uint32_t b0 = 0, changed = 0;
            if (!song_at(&song, ui.bar, ui.trk, &b0))
                say("NO RIFF HERE");
            else {
                for (; d > 0; d--) { if (!song_repeat_add(&song, ui.trk, b0)) { say("NO ROOM"); break; } changed = 1; }
                for (; d < 0; d++) { if (!song_repeat_remove(&song, ui.trk, b0)) break; changed = 1; }
                if (changed) {
                    uint32_t first, last, end;
                    edited();
                    song_chain(&song, ui.trk, b0, &first, &last, &end);
                    if (!song_at(&song, ui.bar, ui.trk, 0)) ui.bar = end - 1u;   /* (the repeat it was on went) */
                    scroll_to(ui.bar);
                    {
                        char s[24];
                        uint32_t n = copy_str(s, "REPEATS ", 9);
                        n += put_dec(s + n, (last - first) / riff_bars(&song.riff[ui.trk][song.cell[first][ui.trk] - 1u]));
                        s[n] = 0;
                        say(s);
                    }
                }
                seq_bars = song_bars(&song);
                ui.dirty |= D_LANES | D_FOOT;
            }
        }
        if ((d = enc(ENC_KNOB3)) != 0) {           /* the loop: anticlockwise on (4 bars) and longer, clockwise shorter, off */
            uint32_t on = song.loop_end > song.loop_start, len = on ? song.loop_end - song.loop_start : 0u, start = on ? song.loop_start : ui.bar;
            for (; d < 0; d++) len = len ? (len < 32u ? len + 1u : 32u) : 4u;
            for (; d > 0; d--) if (len) len--;
            loop_set(start, len);
            edited();
            say_loop();
            ui.dirty |= D_HEAD | D_RULER;
        }
        if ((d = enc(ENC_KNOB4)) != 0) {           /* zoom: anticlockwise out, clockwise in, a step a detent */
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
    }

    b = fm1_input_edges(&rel);
    if (b || rel) prj.touched = fm1_ms;
    save_button(b, rel);
    if (b & (1u << BTN_HOME) && ui.mode != M_ARR) { ui.mode = M_ARR; prj.ask = ASK_NONE; ui.dirty = D_ALL; }
    if (ui.mode == M_PRJ) {
        prj_input(pd, b);
        b &= ~((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP));   /* (the octave keys answer here, they do not shift) */
    }
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
    if (down) prj.touched = fm1_ms;
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
    fm1_led_key(BTN_OCT_DOWN, ui.mode == M_PRJ ? prj.ask != 0 || (prj.sel != PRJ_AUTO && prj.used[prj.sel]) : ui.oct < 2u);
    fm1_led_key(BTN_OCT_UP, ui.mode == M_PRJ ? 1 : ui.oct > 2u);
    fm1_led_key(BTN_SAVE, ui.mode == M_PRJ);
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
    autosave();
    if (splash.on && splash.until && (int32_t)(splash.until - fm1_ms) <= 0) {   /* a splash's time is up */
        splash.on = 0;
        splash.until = 0;
        ui.dirty = D_ALL;
    }
    if (splash.on) return;                      /* (the splash stays on top: nothing drawn under it) */
    if (ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) <= 0) { ui.msg_until = 0; ui.dirty |= D_FOOT; }
    if (ui.mode == M_PRJ) {
        if (ui.dirty & D_HEAD) draw_head("PRJ");
        if (ui.dirty & (D_RULER | D_LANES)) draw_prj();
        else if (ui.dirty & D_FOOT) draw_prj_foot();
    } else {
        if (ui.dirty & D_HEAD) draw_head("ARR");
        if (ui.dirty & D_RULER) draw_ruler();
        if (ui.dirty & D_LANES) for (k = 0; k < SONG_TRACKS; k++) draw_lane(k, pos);
        if (ui.dirty & D_FOOT) draw_foot();
    }
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

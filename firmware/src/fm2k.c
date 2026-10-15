/* SPDX-License-Identifier: GPL-3.0-only */
/* FM2K: a Music 2000-style song maker for the M-VAVE FM-1. Four tracks, a pool of riffs per track, and the
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
 *   KNOB 3       the loop: clockwise on (4 bars from the cursor) and longer, anticlockwise shorter down to off
 *   KNOB 4       zoom 2x / 1x / 0.5x / 0.25x (4 / 8 / 16 / 32 bars in view): anticlockwise out (round the cursor),
 *                clockwise in (from the cursor's bar)
 *   PLAY         play the song from the cursor's bar / stop; REC arms (recording: not yet)
 *   SAVE         tap: PRJ, the projects (HOME back); hold 2 s: save (see PRJ below)
 *   SEQ          on an empty bar: a new riff (16 steps); on a riff: the riff deleted
 *   EDIT         on a riff: RIF, its piano roll (HOME back)
 *   GLO          the metronome on / off (for now)
 *   keys         play the track's sound; OCT- OCT+ the octave
 *
 * Tracks 1-3 will choose among sound engines; for now there is one, BASIC (a few waves and an envelope).
 * Track 4 is the drums: its notes pick drums (General MIDI where it has one), its sound is a kit.
 *
 * Integers only: the chip has no FPU and no libgcc (docs/RULES.md). platform.c gives the panel, the screen, the
 * sound and MIDI (docs/API.md); tests/host.c and web/ run this same file on a computer. */
#define APP_NAME "FM2K"
#include "platform.c"
#include "song.h"
#include "project.h"
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* (tests/host.c has none) */
#endif

static song_t song __attribute__((section(".pool")));   /* (18 KB: in the pool, not the 96 KB of RAM) */

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
    uint16_t left;                              /* ticks until the sequencer lets go; HELD = a key */
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

static void voice_on(uint32_t trk, uint32_t note, uint32_t vel, uint32_t ticks)
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
    p->left = (uint16_t)ticks;
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

/* the sequencer: an absolute step (bar * 16 + step), the tick in it (0..5) and the phase into the tick (2^32 = one
 * tick). It plays the song, or (seq_riff, recording) one riff over and over from its start. */
static volatile uint32_t seq_play, seq_from, seq_step, seq_sub, seq_bars, seq_click;
static volatile uint32_t seq_ticks;             /* ticks since it started (for a recorded note's length) */
static volatile uint32_t seq_riff, seq_rtrk;    /* 0: the song; else this riff of track seq_rtrk, looping */
static volatile uint32_t seq_restart;           /* start again from the top (the main loop asks) */
static volatile uint32_t seq_busy, seq_late;    /* the main loop is moving notes: a tick waits for it */
static uint32_t seq_ph, seq_was;
static uint32_t click_ph, click_inc;            /* the metronome: a short blip on each beat, higher on the bar */
static int32_t click_amp;
#define STEP_K_Q8 166214u                       /* 2^32 steps x 4 per beat / (600 x 44100) per frame per 0.1 BPM, x 256 */

/* recording: the notes not to play from the riff: those held on the keys now (they sound already), and those just
 * put a little ahead of the playhead (quantised forward: heard when played, not again a moment later) */
static volatile uint32_t rec_mute[4];           /* 128 notes, a bit each */
static volatile uint16_t rec_skip[8];           /* (tick in the riff << 7 | note) + 1; 0 = free */
static void seq_note(void *ctx, uint32_t trk, const song_ev_t *e)
{
    (void)ctx;
    if (seq_riff && trk == seq_rtrk) {
        uint32_t k, key = ((e->start * SONG_TPS + e->tick) << 7 | e->note) + 1u;
        if (rec_mute[e->note >> 5] >> (e->note & 31u) & 1u) return;
        for (k = 0; k < 8u; k++) if (rec_skip[k] == key) { rec_skip[k] = 0; return; }
    }
    voice_on(trk, e->note, e->vel, e->len * SONG_TPS);
}
static void seq_tick(void)
{
    uint32_t v, t;
    if (seq_busy) { seq_late = 1; return; }     /* (the notes are moving: in a moment) */
    seq_late = 0;
    for (v = 0; v < NVOICE; v++)
        if (voice[v].on && voice[v].left != HELD && voice[v].left && --voice[v].left == 0)
            voice[v].on = 0;
    if (seq_riff) riff_notes_at(&song, seq_rtrk, seq_riff, seq_step, seq_sub, seq_note, 0);
    else for (t = 0; t < SONG_TRACKS; t++) song_notes_at_tick(&song, t, seq_step, seq_sub, seq_note, 0);
    if (seq_click && !seq_sub && !(seq_step & 3u)) {
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
    if (seq_restart) { seq_restart = 0; seq_was = 0; }
    for (i = 0; i < n; i++) {
        if (seq_play != seq_was) {
            seq_was = seq_play;
            if (seq_play) {
                seq_step = seq_riff ? 0u : seq_from * STEPS_PER_BAR;
                seq_sub = 0; seq_ph = 0; seq_ticks = 0;
                for (k = 0; k < NVOICE; k++) if (voice[k].left != HELD) voice[k].on = 0;
                seq_tick();
            } else for (k = 0; k < NVOICE; k++) if (voice[k].left != HELD) voice[k].on = 0;
        } else if (seq_play) {
            uint32_t p = seq_ph;
            seq_ph += ((song.bpm10 * STEP_K_Q8) >> 8) * SONG_TPS;
            if (seq_ph < p) {
                seq_ticks++;
                if (++seq_sub >= SONG_TPS) {
                    uint32_t st = seq_step + 1u, end = seq_bars * STEPS_PER_BAR;
                    seq_sub = 0;
                    if (seq_riff) {             /* one riff: round and round */
                        if (st >= song.riff[seq_rtrk][seq_riff - 1u].steps) st = 0;
                    } else if (song.loop_end > song.loop_start && st >= song.loop_end * STEPS_PER_BAR)
                        st = song.loop_start * STEPS_PER_BAR;
                    else if (st >= end)
                        st = 0;
                    seq_step = st;
                }
                seq_tick();
            } else if (seq_late && !seq_busy)
                seq_tick();                     /* the tick that waited */
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

static void test_song(void)                     /* a chiptune at 140: Am F C G, an arpeggio, an octave bass, a lead */
{
    static const uint8_t PLACE[][3] = {         /* bar, track, riff */
        {0, 2, 1}, {0, 3, 3}, {1, 3, 3}, {2, 3, 3}, {3, 3, 2},
        {4, 2, 1}, {4, 1, 1}, {4, 3, 1}, {5, 3, 1}, {6, 3, 1}, {7, 3, 2},
        {8, 2, 1}, {8, 1, 1}, {8, 0, 1}, {8, 3, 1}, {9, 3, 1}, {10, 3, 1}, {11, 3, 2},
        {12, 2, 1}, {12, 1, 1}, {12, 0, 2}, {12, 3, 1}, {13, 3, 1}, {14, 3, 1}, {15, 3, 2},
    };
    static const uint8_t ROOT[4] = {45, 41, 48, 43};                 /* A F C G */
    static const uint8_t CHORD[4][4] = {{69, 72, 76, 81}, {65, 69, 72, 77}, {64, 67, 72, 76}, {67, 71, 74, 79}};
    uint32_t i, b;
    song_init(&song);
    song.name[0] = 'D'; song.name[1] = 'E'; song.name[2] = 'M'; song.name[3] = 'O';
    song.bpm10 = 1400;
    song.track[0].inst = 1;                     /* PULSE */
    song.track[1].inst = 5;                     /* PLUCK */
    song.track[2].inst = 6;                     /* CHIP */
    song.track[3].inst = 1;                     /* KIT TIGHT */
    song.track[0].pan = 76; song.track[2].pan = 44; song.track[2].vol = 70; song.track[1].vol = 120;
    song.track[3].vol = 115;
    NOTES(0, 1, 64, "HOOK", 0, 3, 81, 115, 3, 3, 79, 100, 6, 2, 76, 105, 8, 2, 72, 95, 10, 4, 76, 110, 14, 2, 74, 95,
          16, 3, 72, 110, 19, 3, 69, 100, 22, 2, 72, 105, 24, 6, 77, 115, 30, 2, 76, 95,
          32, 3, 76, 110, 35, 3, 74, 100, 38, 2, 72, 105, 40, 2, 67, 95, 42, 4, 72, 110, 46, 2, 74, 95,
          48, 3, 71, 110, 51, 3, 74, 100, 54, 4, 79, 115, 58, 2, 76, 95, 60, 4, 74, 105);
    NOTES(0, 2, 64, "HIGH", 0, 2, 84, 115, 2, 2, 83, 95, 4, 4, 81, 110, 8, 2, 76, 95, 10, 2, 81, 100, 12, 4, 84, 115,
          16, 2, 84, 110, 18, 2, 81, 95, 20, 4, 77, 105, 24, 2, 81, 100, 26, 2, 77, 95, 28, 4, 72, 105,
          32, 2, 79, 110, 34, 2, 76, 95, 36, 4, 72, 105, 40, 2, 76, 100, 42, 2, 79, 100, 44, 4, 84, 115,
          48, 4, 83, 115, 52, 4, 79, 105, 56, 4, 74, 100, 60, 4, 71, 95);
    notes(1, 1, 64, "OCTS", 0, 0);              /* the bass: eighths, jumping the octave */
    for (b = 0; b < 4u; b++)
        for (i = 0; i < 8u; i++)
            riff_note_set(&song, 1, 1, b * 16u + i * 2u, 2, (uint8_t)(ROOT[b] + (i & 1u ? 12u : 0u)), i & 1u ? 90 : 125);
    notes(2, 1, 64, "ARP", 0, 0);               /* sixteenths up the chord, accent on the beat */
    for (b = 0; b < 4u; b++)
        for (i = 0; i < 16u; i++)
            riff_note_set(&song, 2, 1, b * 16u + i, 1, CHORD[b][i & 3u], i & 3u ? 70 : 105);
    NOTES(3, 1, 16, "BEAT", 0, 1, 36, 127, 0, 1, 42, 80, 1, 1, 42, 40, 2, 1, 42, 80, 3, 1, 42, 40, 4, 1, 38, 120,
          4, 1, 42, 80, 5, 1, 42, 40, 6, 1, 42, 80, 7, 1, 36, 105, 7, 1, 42, 40, 8, 1, 36, 120, 8, 1, 42, 80,
          9, 1, 42, 40, 10, 1, 46, 80, 11, 1, 42, 40, 12, 1, 38, 120, 12, 1, 42, 80, 13, 1, 42, 40, 14, 1, 42, 80,
          15, 1, 42, 50);
    NOTES(3, 2, 16, "FILL", 0, 1, 36, 127, 2, 1, 42, 80, 4, 1, 38, 110, 6, 1, 36, 110, 8, 1, 38, 90, 10, 1, 38, 100,
          12, 1, 45, 110, 13, 1, 43, 110, 14, 1, 41, 120, 15, 1, 38, 127);
    NOTES(3, 3, 16, "HATS", 0, 1, 36, 120, 2, 1, 42, 80, 4, 1, 42, 60, 6, 1, 42, 80, 8, 1, 36, 110, 10, 1, 42, 80,
          12, 1, 42, 60, 14, 1, 46, 70);
    for (i = 0; i < sizeof PLACE / sizeof PLACE[0]; i++)
        song_place(&song, PLACE[i][0], PLACE[i][1], PLACE[i][2]);
}

/* ================================================================== the design system (docs: the FM2K design system page)
 * Music 2000's look, made for 240 x 240: a navy ground with broad faint diagonal bands; content in WELLS (recessed:
 * dark edge top-left, lit edge bottom-right); controls and chrome on METAL (raised: a grey gradient, lit edge on
 * top, shaded under; pressed: darker, the edges swapped); readouts in small black displays (the tempo in red LED).
 * Type: Unbounded, wide and heavy (FONT_XS 8, FONT_S 10, FONT_M 13, FONT_L 22). One accent (blue); green plays,
 * red records. Riffs are raised green tiles, the one under the cursor mint. The footer shows the four knobs.
 * Everything is drawn in screen coordinates into strips of the canvas (strip_begin / strip_end): a strip is a band
 * of the screen, at most 64 rows, and anything outside it is clipped. */
/* the ground */
#define C_BG RGB(6, 10, 34)
#define C_BG_STRIPE RGB(10, 16, 48)
/* wells */
#define C_WELL RGB(10, 18, 56)
#define C_WELL_DARK RGB(3, 5, 26)               /* a well's top and left edge */
#define C_WELL_LIT RGB(28, 42, 110)             /* its bottom and right edge */
#define C_CELL RGB(14, 26, 82)                  /* the grid's cells */
#define C_CELL_ALT RGB(12, 22, 74)              /* every other 4 bars; the black keys' rows */
#define C_LINE_BEAT RGB(22, 36, 110)            /* a bar (ARR), a step (RIF) */
#define C_LINE_BAR RGB(36, 83, 214)             /* a beat (RIF), the ruler's rule */
#define C_LINE_BAR4 RGB(79, 134, 255)           /* every 4 bars (ARR), a bar (RIF) */
/* metal */
#define C_METAL_TOP RGB(169, 175, 194)
#define C_METAL_BOT RGB(102, 108, 130)
#define C_METAL_LIT RGB(226, 230, 240)          /* the lit edge */
#define C_METAL_SHADE RGB(43, 47, 64)           /* the shaded edge */
#define C_INK RGB(14, 18, 38)                   /* text on metal */
#define C_INK_SOFT RGB(58, 64, 88)              /* off, secondary, on metal */
/* displays */
#define C_LCD RGB(4, 7, 26)
#define C_LCD_INK RGB(233, 238, 255)
#define C_LED RGB(255, 59, 48)
#define C_LED_BG RGB(20, 4, 10)
#define C_LED_OFF RGB(36, 44, 76)               /* an unlit icon or LED in a display */
/* content */
#define C_RIFF_TOP RGB(95, 219, 116)            /* a riff: its lit edge, its body, its shade, its notes */
#define C_RIFF RGB(45, 170, 74)
#define C_RIFF_BOT RGB(22, 104, 43)
#define C_RIFF_NOTE RGB(10, 58, 22)
#define C_SEL_TOP RGB(210, 255, 240)            /* the riff (the note) under the cursor */
#define C_SEL RGB(124, 242, 198)
#define C_SEL_BOT RGB(59, 184, 146)
#define C_SEL_NOTE RGB(13, 74, 57)
#define C_TXT RGB(242, 245, 255)                /* text on the dark */
#define C_TXT2 RGB(142, 155, 200)               /* labels */
#define C_TXT3 RGB(72, 85, 143)                 /* idle, empty */
#define C_ACC RGB(61, 123, 255)                 /* the accent: chosen values, the loop */
#define C_PLAY RGB(92, 255, 122)
#define C_RED RGB(255, 59, 48)
#define C_KEY_W RGB(201, 206, 221)              /* the piano's keys */
#define C_KEY_W2 RGB(174, 180, 200)
#define C_KEY_B RGB(11, 14, 30)
#define C_KEY_B2 RGB(58, 64, 96)
#define C_KEY_GAP RGB(107, 114, 144)

/* a + (b - a) * t / 16, in RGB565 */
static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t)
{
    int32_t ra = a >> 11, ga = (a >> 5) & 63, ba = a & 31, rb = b >> 11, gb = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ra + (rb - ra) * (int32_t)t / 16) << 11) | ((ga + (gb - ga) * (int32_t)t / 16) << 5) |
                      (ba + (bb - ba) * (int32_t)t / 16));
}
static uint16_t cv_get(int32_t x, int32_t y)    /* the canvas's pixel (screen coordinates in a strip), 0 outside */
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h) return swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
    return 0;
}

/* a strip: rows sy .. sy + h - 1 of the screen, the ground in it; drawn into in screen coordinates */
static uint32_t strip_y;
static void ground(int32_t x, int32_t y, int32_t w, int32_t h)   /* the navy, its bands 18 px wide */
{
    int32_t i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            cv_pset(i, j, ((i + j) / 18) & 1 ? C_BG_STRIPE : C_BG);
}
static void strip_begin(uint32_t sy, uint32_t h)
{
    cv_oy = 0;
    cv_begin(240, h, C_BG);
    cv_oy = -(int32_t)sy;
    strip_y = sy;
    ground(0, (int32_t)sy, 240, (int32_t)h);
}
static void strip_end(void) { cv_oy = 0; cv_blit(0, strip_y); }
static void strip_begin_flat(uint32_t sy, uint32_t h)   /* a strip something covers whole (no ground under it) */
{
    cv_oy = 0;
    cv_begin(240, h, C_WELL);
    cv_oy = -(int32_t)sy;
    strip_y = sy;
}

/* soft corners (radius 4): a shape's corner pixels blended towards the ground g */
static const uint8_t CORNER[4][4] = {{0, 4, 11, 15}, {4, 15, 16, 16}, {11, 16, 16, 16}, {15, 16, 16, 16}};
static void corners(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t g, uint32_t left, uint32_t right)
{
    int32_t i, j;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            uint32_t t = CORNER[j][i];
            if (t == 16u) continue;
            if (left) {
                cv_pset(x + i, y + j, mix565(g, cv_get(x + i, y + j), t));
                cv_pset(x + i, y + h - 1 - j, mix565(g, cv_get(x + i, y + h - 1 - j), t));
            }
            if (right) {
                cv_pset(x + w - 1 - i, y + j, mix565(g, cv_get(x + w - 1 - i, y + j), t));
                cv_pset(x + w - 1 - i, y + h - 1 - j, mix565(g, cv_get(x + w - 1 - i, y + h - 1 - j), t));
            }
        }
}
static void vgrad(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c0, uint16_t c1)   /* top c0 to bottom c1 */
{
    int32_t j;
    for (j = 0; j < h; j++) cv_rect(x, y + j, w, 1, mix565(c0, c1, h > 1 ? (uint32_t)(j * 16 / (h - 1)) : 0u));
}
/* raised: a gradient, a lit edge on top (and, softer, left), a shaded one under (and right) */
static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t top, uint16_t bot, uint16_t lit, uint16_t shade,
                  uint16_t g, uint32_t round_l, uint32_t round_r)
{
    vgrad(x, y, w, h, top, bot);
    cv_rect(x + 2, y, w - 4, 1, lit);
    cv_rect(x + 2, y + h - 1, w - 4, 1, shade);
    if (round_l) cv_rect(x, y + 2, 1, h - 4, mix565(lit, top, 6));
    if (round_r) cv_rect(x + w - 1, y + 2, 1, h - 4, mix565(shade, bot, 6));
    if (w >= 8 && h >= 8) corners(x, y, w, h, g, round_l, round_r);
}
static void metal(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t g)
{
    bevel(x, y, w, h, C_METAL_TOP, C_METAL_BOT, C_METAL_LIT, C_METAL_SHADE, g, 1, 1);
}
static void metal_pressed(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t g)
{
    bevel(x, y, w, h, C_METAL_BOT, mix565(C_METAL_BOT, C_METAL_TOP, 6), C_METAL_SHADE, C_METAL_LIT, g, 1, 1);
}
/* recessed: dark edges top and left, lit bottom and right; dark / lit 0: the defaults */
static void well(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t g, uint16_t dark, uint16_t lit)
{
    if (!dark) dark = C_WELL_DARK;
    if (!lit) lit = C_WELL_LIT;
    cv_rect(x, y, w, h, fill);
    cv_rect(x + 1, y, w - 2, 1, dark);
    cv_rect(x, y + 1, 1, h - 2, dark);
    cv_rect(x + 1, y + 1, w - 2, 1, mix565(dark, fill, 8));
    cv_rect(x + 1, y + h - 1, w - 2, 1, lit);
    cv_rect(x + w - 1, y + 1, 1, h - 2, mix565(lit, fill, 6));
    if (w >= 8 && h >= 8) corners(x, y, w, h, g, 1, 1);
}
static void display(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill)   /* a black display set in metal */
{
    well(x, y, w, h, fill, C_METAL_TOP, C_METAL_SHADE, C_METAL_LIT);
}
static int32_t txt(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c);
/* an LED: 5 x 5, a black socket and a 3 x 3 light in it, lit in the accent (a glint on its corner) or off */
static void led(int32_t x, int32_t y, uint32_t on, uint16_t ground)
{
    (void)ground;
    cv_rect(x, y, 5, 5, C_METAL_SHADE);
    cv_rect(x + 1, y + 1, 3, 3, on ? C_ACC : C_LED_OFF);
    if (on) cv_pset(x + 1, y + 1, mix565(C_ACC, C_TXT, 8));
    cv_rect(x + 1, y + 5, 4, 1, C_METAL_LIT);   /* the socket's lit lower edge */
}
/* a knob's name ("K1"): flat, light letters in a small black display, so it reads as a label, not a knob */
static int32_t knob_chip(int32_t x, int32_t y, uint32_t k, uint16_t ground)
{
    char s[3] = {'K', (char)('1' + k), 0};
    well(x, y, 17, 12, C_LCD, ground, C_METAL_SHADE, C_METAL_LIT);
    txt(x + 3, y + 1, &FONT_XS, s, C_LCD_INK);
    return x + 17;
}

/* the layout: a 22 px header, the body (a well, 23 .. 214), a 24 px footer */
#define HEAD_H 22
#define BODY_Y 23
#define BODY_H 192
#define FOOT_Y 216
#define FOOT_H 24
#define RULER_Y BODY_Y                          /* ARR: the ruler (with the well's top edge) */
#define RULER_H 16
#define LANE_Y (RULER_Y + RULER_H)              /* the four tracks */
#define LANE_H 43
#define GX 40                                   /* the lanes start right of the track tabs */
static uint32_t put_dec(char *p, uint32_t v);
static const uint8_t ZOOM_BW[4] = {50, 25, 12, 6};   /* a bar's width at 2x 1x 0.5x 0.25x: 4 8 16 32 bars in 200 px */
static const char *const ZOOM_NAME[4] = {"ZOOM 2X", "ZOOM 1X", "ZOOM 0.5X", "ZOOM 0.25X"};
#define BW ((int32_t)ZOOM_BW[ui.zoom])
#define COLS (4u << ui.zoom)
#define ZOOM_1X 1u
/* where the view starts, in steps: a bar's start when still; while playing, wherever the playhead has slid it */
#define VIEW (ui.scrolling ? ui.vstep : ui.left * STEPS_PER_BAR)

static struct {
    uint32_t bar, trk, left, oct;               /* the cursor, the first bar shown, the keys' octave (0..4) */
    uint32_t keys_prev;
    uint8_t held[27];
    uint32_t drawn_pos;                         /* the playhead drawn (a step, or ~0) */
    uint32_t rec;                               /* REC armed (recording itself: not yet) */
    uint32_t zoom;                              /* 0 2x, 1 1x, 2 0.5x, 3 0.25x */
    uint32_t marq_t0, marq_trk, marq_inst, marq_drawn;   /* the chosen track's name scrolling: since when, for what */
    uint32_t scrolling, vstep, vtarget;         /* playing: the view starts at vstep (sliding towards vtarget, a bar) */
    uint32_t mode;                              /* M_ARR, M_PRJ, M_RIF, M_EDT */
    uint32_t msg_until;                         /* the footer shows ui.msg until then (fm1_ms) */
    char msg[24];
    uint32_t dirty;
} ui;
enum { D_HEAD = 1, D_RULER = 2, D_LANES = 4, D_FOOT = 8, D_ALL = 15, D_PH = 16, D_LABEL = 32 };

/* recording a riff (REC on RIF): REC records over the riff as it loops; REC again: REHEARSAL (it loops, the keys
 * play, nothing is written; REC blinks), REC again: recording. PLAY or leaving RIF stops it. */
enum { REC_OFF, REC_ON, REC_REHEARSE };
#define REC_HOLDS 16u
static struct {
    uint32_t state, lit;                        /* lit: REC's light as last drawn */
    struct { uint8_t on, start, tick, note; uint32_t t0; } hold[REC_HOLDS];   /* notes being recorded, held */
} rec;
static const char *const QUANT_NAME[RIFF_QUANTS] = {"1/2", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static uint32_t rec_lit(void)                   /* REC's light: on recording, blinking in rehearsal, else armed */
{
    return rec.state == REC_ON ? 1u : rec.state == REC_REHEARSE ? (fm1_ms / 250u & 1u) == 0u : ui.rec != 0;
}
/* D_PH: the playhead alone; D_LABEL: the chosen track's name, scrolling */

/* a page sliding on by a bar: from -> to in a few eased frames */
#define SLIDE_MS 120u
typedef struct { uint32_t from, to, t0, on; } slide_t;
static slide_t sl_arr, sl_rif;
static uint32_t ph_only;                        /* draw_lane: draw, but leave sending it to draw_lane_cols */
static void slide_start(slide_t *s, uint32_t from, uint32_t to) { s->from = from; s->to = to; s->t0 = fm1_ms; s->on = 1; }
static int slide_step(slide_t *s, uint32_t *v)  /* the slide's position now in *v: 1 while it moves (and on its last frame) */
{
    static const uint8_t EASE[4] = {8, 13, 15, 16};   /* sixteenths of the way at 0, 30, 60, 90 ms: fast, then settling */
    uint32_t k, nv;
    if (!s->on) return 0;
    k = (fm1_ms - s->t0) / (SLIDE_MS / 4u);
    if (k >= 4u) { nv = s->to; s->on = 0; }
    else nv = s->from + (s->to - s->from) * EASE[k] / 16u;
    if (nv == *v && s->on) return 0;
    *v = nv;
    return 1;
}
enum { M_ARR, M_PRJ, M_RIF, M_EDT };

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

/* text blended onto whatever is under it (metal, a gradient, a riff), drawn only between x0 and x1 */
static int32_t text_x(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, int32_t x0, int32_t x1)
{
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                int32_t px = x - f->pad + (int32_t)gx, py = y + (int32_t)gy;
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v && px >= x0 && px < x1) cv_pset(px, py, v == 15u ? c : mix565(cv_get(px, py), c, v * 16u / 15u));
            }
        x += f->adv[gi];
    }
    return x;
}
static int32_t txt(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return text_x(x, y, f, s, c, -10000, 10000);
}
/* (the older two: the ground they took is no longer needed) */
static int32_t cv_text_on(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg)
{
    (void)bg;
    return txt(x, y, f, s, c);
}
static void cv_text_clip(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint16_t bg,
                         int32_t x0, int32_t x1)
{
    (void)bg;
    text_x(x, y, f, s, c, x0, x1);
}

/* small icons: a row a string, '#' = ink */
static const char *const ICON_PLAY[7] = {".#.......", ".###.....", ".#####...", ".#######.", ".#####...", ".###.....", ".#......."};
static const char *const ICON_REC[7] = {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."};
static const char *const ICON_LOOP[7] = {"..#####..", ".#.....#.", "#.......#", "#.......#", "#....#..#", ".#..###..", "....#####"};
static const char *const ICON_METRO[7] = {"...###...", "...#.#..#", "..#...##.", "..#..##..", ".#..#..#.", ".#.....#.", "#########"};
static const char *const ICON_FLAG[6] = {"#.....", "####..", "#####.", "####..", "#.....", "#....."};

static int32_t cv_icon(int32_t x, int32_t y, const char *const *rows, uint32_t n, uint16_t c)   /* returns its width */
{
    int32_t i, j, w = 0;
    for (j = 0; j < (int32_t)n; j++)
        for (i = 0; rows[j][i]; i++) {
            if (rows[j][i] == '#') cv_pset(x + i, y + j, c);
            if (i + 1 > w) w = i + 1;
        }
    return w;
}

/* the header: metal; the screen's name (and, small, what it is about), the transport in the middle, the tempo in
 * a red LED display. Screen names are short: ARR, PRJ, RIF, EDT; later SND, MIX, SNG, SYS */
static const char *head_ctx;                    /* the context after the name (RIF and EDT: the riff), or 0 */
static void draw_head(const char *screen)
{
    char bpm[8];
    int32_t x, w;
    uint32_t n, i;
    strip_begin(0, HEAD_H + 1);
    metal(0, 0, 240, HEAD_H, C_BG);
    x = txt(7, 3, &FONT_M, screen, C_INK);
    if (head_ctx) txt(x + 6, 6, &FONT_S, head_ctx, C_INK_SOFT);
    {   /* the transport: play, record, loop, metronome; lit when on */
        const char *const *ic[4] = {ICON_PLAY, ICON_REC, ICON_LOOP, ICON_METRO};
        uint16_t on[4] = {seq_play ? C_PLAY : 0, rec_lit() ? C_RED : 0, song.loop_end > song.loop_start ? C_ACC : 0,
                          seq_click ? C_ACC : 0};
        display(88, 3, 64, 16, C_LCD);           /* lit in a black display: colour never sits on the metal */
        x = 95;
        for (i = 0; i < 4u; i++) x += cv_icon(x, 7, ic[i], 7, on[i] ? on[i] : C_LED_OFF) + 6;
    }
    n = put_dec(bpm, song.bpm10 / 10u);         /* the tempo: "145", "145.5" */
    if (song.bpm10 % 10u) { bpm[n++] = '.'; bpm[n++] = (char)('0' + song.bpm10 % 10u); }
    bpm[n] = 0;
    w = text_w(&FONT_M, bpm) + 12;
    display(237 - w, 3, w, 16, C_LED_BG);
    txt(237 - w + 6, 3, &FONT_M, bpm, C_LED);
    strip_end();
}

static int32_t xstep(uint32_t step)             /* a step's x on ARR (left of GX: off the view, under the tabs) */
{
    return GX + ((int32_t)step - (int32_t)VIEW) * BW / (int32_t)STEPS_PER_BAR;
}

static void body_well(void) { well(0, BODY_Y, 240, BODY_H, C_WELL, C_BG, 0, 0); }   /* (clipped to the strip) */
static void body_edge_r(int32_t y, int32_t h)   /* the well's right edge again, over what slid past it */
{
    cv_rect(238, y, 1, h, C_WELL);
    cv_rect(239, y, 1, h, mix565(C_WELL_LIT, C_WELL, 6));
}

/* the ruler: bar numbers (every 4th with a flag, as on Music 2000; zoomed out, fewer), the cursor's in the accent,
 * the loop as a line over its bars, a rule under it all */
static void draw_ruler(void)
{
    uint32_t bar, fb = VIEW / STEPS_PER_BAR, lb = (VIEW + COLS * STEPS_PER_BAR + STEPS_PER_BAR - 1u) / STEPS_PER_BAR;
    int32_t y = RULER_Y;
    strip_begin(RULER_Y, RULER_H);
    body_well();
    for (bar = fb; bar < lb && bar < SONG_BARS; bar++) {
        int32_t x = xstep(bar * STEPS_PER_BAR);
        uint32_t four = !(bar % 4u), cur = bar == ui.bar, every = ui.zoom == 3u ? 8u : ui.zoom == 2u ? 4u : 1u;
        char num[4];
        if (song.loop_end > song.loop_start && bar >= song.loop_start && bar < song.loop_end)
            cv_rect(x, y + 2, BW, 2, C_ACC);
        if (!(bar % every) || cur) {
            num[put_dec(num, bar + 1u)] = 0;
            if (four) x += cv_icon(x + 2, y + 5, ICON_FLAG, 6, C_LINE_BAR4) + 3;
            txt(x + 3, y + 4, &FONT_XS, num, cur ? C_ACC : four ? C_TXT : C_TXT3);
        }
    }
    cv_rect(1, y + 2, GX - 1, RULER_H - 2, C_WELL);   /* (what slid off the left) */
    cv_rect(GX, y + RULER_H - 1, 238 - GX, 1, C_LINE_BAR);
    body_edge_r(y + 1, RULER_H - 1);
    strip_end();
}

/* a riff's tile: raised, green (mint under the cursor); open_l / open_r: it goes on past the view's edge there */
static void riff_tile(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t open_l, uint32_t open_r, uint32_t lit, uint16_t g)
{
    uint16_t top = lit ? C_SEL_TOP : C_RIFF_TOP, mid = lit ? C_SEL : C_RIFF, bot = lit ? C_SEL_BOT : C_RIFF_BOT;
    if (w <= 0) return;
    bevel(x, y, w, h, mix565(mid, top, 6), mid, top, bot, g, !open_l, !open_r);
}

/* a riff's notes inside its tile, as a DAW shows a clip: time across, pitch up, the riff's own range filling the
 * tile's height. off = the riff's step at x, vis bars of it in view, w px wide, ly the lane's top */
static void draw_notes(int32_t ly, const song_riff_t *f, uint32_t off, int32_t x, uint32_t vis, int32_t w, uint16_t ink)
{
    const int32_t top = ly + 7, h = LANE_H - 15;
    uint32_t i, lo = 127, hi = 0, end = off + vis * STEPS_PER_BAR;
    int32_t range, nh;
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
        if (song.ev[i].note < lo) lo = song.ev[i].note;
        if (song.ev[i].note > hi) hi = song.ev[i].note;
    }
    if (!f->count) return;
    range = (int32_t)(hi - lo);
    nh = range <= 6 ? 4 : range <= 12 ? 3 : 2;
    if (range < 4) range = 4;                   /* a few notes do not fill the tile from edge to edge */
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
#define NOTES_MIN_W 24                          /* a riff narrower than this (its whole length, in px) shows no notes */

static void draw_lane(uint32_t t, uint32_t pos)    /* one track: its tab, its cells, its riffs; pos = the playhead step */
{
    uint32_t bar, sel = t == ui.trk;
    uint32_t fb = VIEW / STEPS_PER_BAR, lb = (VIEW + COLS * STEPS_PER_BAR + STEPS_PER_BAR - 1u) / STEPS_PER_BAR;
    const song_track_t *tr = &song.track[t];
    int32_t ly = LANE_Y + (int32_t)t * LANE_H;
    char lab[4];
    if (lb > SONG_BARS) lb = SONG_BARS;
    strip_begin_flat((uint32_t)ly, LANE_H);   /* (the well covers a lane's strip from edge to edge) */
    body_well();
    /* the cells: a bar each, every other 4 bars a shade darker; a line on each bar, a bright one every 4 (zoomed
     * out to 0.25x only those: a line every 6 px would be a wash); a dark line under the track */
    for (bar = fb; bar <= lb; bar++) {
        int32_t x = xstep(bar * STEPS_PER_BAR);
        cv_rect(x, ly, BW, LANE_H, (bar / 4u) & 1u ? C_CELL_ALT : C_CELL);
        if (bar % 4u && ui.zoom == 3u) continue;
        cv_rect(x, ly, 1, LANE_H, bar % 4u ? C_LINE_BEAT : C_LINE_BAR4);
    }
    cv_rect(GX, ly + LANE_H - 1, 238 - GX, 1, C_WELL_DARK);
    /* the riffs: a riff and its repeats are one row of tiles, a line where each repeat begins; the repeat under the
     * cursor lit; its notes drawn dark in it when the riff is at least NOTES_MIN_W px long on screen */
    for (bar = fb; bar < lb;) {
        uint32_t b0 = 0, r, end;
        r = song_at(&song, bar, t, &b0);
        if (!r) { bar++; continue; }
        {
            const song_riff_t *f = &song.riff[t][r - 1u];
            uint32_t first, last, len = riff_bars(f), open_r, a, hb = ~0u;
            int32_t y = ly + 2, h = LANE_H - 5;
            song_chain(&song, t, b0, &first, &last, &end);
            if (end > lb) end = lb;
            open_r = song_block_end(&song, t, last) > lb;   /* it goes on past the view */
            if (sel && ui.bar >= first && ui.bar < end) song_at(&song, ui.bar, t, &hb);   /* the lit repeat */
            for (a = first; a <= last; a += len) {
                uint32_t s0 = a > bar ? a : bar, s1 = a + len < end ? a + len : end, lit = a == hb;
                uint32_t full = f->steps % STEPS_PER_BAR == 0u, e_st = a * STEPS_PER_BAR + f->steps;
                int32_t xa = xstep(s0 * STEPS_PER_BAR), wa = xstep(s1 * STEPS_PER_BAR) - xa;
                uint16_t g = (s0 / 4u) & 1u ? C_CELL_ALT : C_CELL;
                if (s1 <= s0) continue;
                if (!full) {                    /* a riff that ends inside a bar: its own tile, as long as it is */
                    if (e_st < s1 * STEPS_PER_BAR) wa = xstep(e_st) - xa;
                    riff_tile(xa + (s0 == a ? 1 : 0), y, wa - (s0 == a ? 1 : 0) - 1, h, s0 != a, e_st > s1 * STEPS_PER_BAR, lit, g);
                } else {
                    riff_tile(xa + (s0 == first ? 1 : 0), y, wa - (s0 == first ? 1 : 0) - (s1 == end && !open_r ? 1 : 0), h,
                              s0 != first, s1 != end || open_r, lit, g);
                    if (a > bar) cv_rect(xa, y + 3, 1, h - 6, lit ? C_SEL_BOT : C_RIFF_BOT);
                }
                if ((int32_t)len * BW >= NOTES_MIN_W)
                    draw_notes(ly, f, (s0 - a) * STEPS_PER_BAR, xa + 1, s1 - s0, wa - 3, lit ? C_SEL_NOTE : C_RIFF_NOTE);
            }
            bar = end;
        }
    }
    if (sel) {                                  /* the cursor: a white frame round the bar */
        int32_t x = xstep(ui.bar * STEPS_PER_BAR);
        cv_rect(x - 1, ly, BW + 2, 1, C_TXT);
        cv_rect(x - 1, ly + LANE_H - 2, BW + 2, 1, C_TXT);
        cv_rect(x - 1, ly, 1, LANE_H - 1, C_TXT);
        cv_rect(x + BW, ly, 1, LANE_H - 1, C_TXT);
    }
    if (pos != ~0u) cv_rect(xstep(pos), ly, 2, LANE_H - 1, C_PLAY);   /* the playhead */
    body_edge_r(ly, LANE_H);
    {   /* the track's tab: metal (pressed in when chosen), its number large, its sound's name in a small display
         * under it, cut at the display's edges; on the chosen track a name too long for it scrolls */
        const char *name = sound_name(t);
        int32_t nw = text_w(&FONT_XS, name), x0 = 8, x1 = GX - 7, off = 0;
        uint16_t ink = tr->flags & SONG_MUTE ? C_TXT3 : sel ? C_LCD_INK : C_TXT2;
        cv_rect(1, ly, GX - 1, LANE_H, C_WELL);
        if (sel) metal_pressed(2, ly + 1, GX - 4, LANE_H - 2, C_WELL);
        else metal(2, ly + 1, GX - 4, LANE_H - 2, C_WELL);
        lab[0] = '0'; lab[1] = (char)('1' + t); lab[2] = 0;
        txt(4, ly + 4, &FONT_M, lab, C_INK);
        if (sel) led(GX - 10, ly + 7, 1, C_METAL_BOT);   /* the chosen track: a small LED, lit, 2 px in from the edge */
        well(5, ly + 22, GX - 10, 15, C_LCD, sel ? C_METAL_BOT : C_METAL_BOT, C_METAL_SHADE, C_METAL_LIT);
        if (sel && nw > x1 - x0) {              /* scrolls: a second still at the start, then a pixel every 60 ms */
            uint32_t el = fm1_ms - ui.marq_t0, loop = (uint32_t)nw + 12u;
            off = el < 1000u ? 0 : (int32_t)(((el - 1000u) / 60u) % loop);
            text_x(x0 - off + (int32_t)loop, ly + 25, &FONT_XS, name, ink, x0, x1);
        }
        text_x(x0 - off, ly + 25, &FONT_XS, name, ink, x0, x1);
    }
    if (!ph_only) strip_end();
    else cv_oy = 0;
}
static void draw_lanes_end(void)                /* under the last track: the well's bottom edge */
{
    strip_begin(LANE_Y + SONG_TRACKS * LANE_H, FOOT_Y - (LANE_Y + SONG_TRACKS * LANE_H));
    body_well();
    strip_end();
}

/* the lane drawn as ever, but only columns x0 .. x0 + w - 1 sent to the screen (the playhead's bar, between slides) */
static uint16_t colbuf[64u * LANE_H];
static void draw_lane_cols(uint32_t t, uint32_t pos, int32_t x0, int32_t w)
{
    uint32_t r, i;
    draw_lane(t, pos);                          /* (draw_lane leaves the blit to us when ph_only is set) */
    if (x0 < 0) { w += x0; x0 = 0; }
    if (x0 + w > (int32_t)cv_w) w = (int32_t)cv_w - x0;
    if (w <= 0 || w > 64) return;
    for (r = 0; r < cv_h; r++)
        for (i = 0; i < (uint32_t)w; i++) colbuf[r * (uint32_t)w + i] = cv_px[r * cv_w + (uint32_t)x0 + i];
    lcd_blit((uint32_t)x0, LANE_Y + t * LANE_H, (uint32_t)w, cv_h, colbuf);
    ph_only = 0;
}

/* the footer: metal; the four knobs and what each does on this screen; a message takes it for a moment */
static int msg_on(void) { return ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) > 0; }
static void foot_text(const char *s, uint16_t c)   /* a line in a display across the footer */
{
    display(3, FOOT_Y + 4, 234, 16, C_LCD);
    txt(9, FOOT_Y + 5, &FONT_S, s, c);
}
static void draw_foot_knobs(const char *k1, const char *k2, const char *k3, const char *k4)
{
    const char *lab[4] = {k1, k2, k3, k4};
    uint32_t k;
    strip_begin(FOOT_Y, FOOT_H);
    metal(0, FOOT_Y, 240, FOOT_H, C_BG);
    if (msg_on()) foot_text(ui.msg, C_LCD_INK);
    else
        for (k = 0; k < 4u; k++) {
            int32_t x = (int32_t)k * 60;
            if (!lab[k]) continue;              /* (a knob with nothing to do here: nothing) */
            knob_chip(x + 3, FOOT_Y + 6, k, C_METAL_TOP);
            txt(x + 23, FOOT_Y + 7, &FONT_XS, lab[k], C_INK);
        }
    strip_end();
}
static void draw_foot(void) { draw_foot_knobs("MOVE", "REPS", "LOOP", "ZOOM"); }

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

static const char *const ICON_TRASH[16] = {     /* a bin, 16 x 16 */
    "......####......", "..############..", "................", "...##########...",
    "...#..#..#..#...", "...#..#..#..#...", "...#..#..#..#...", "...#..#..#..#...",
    "...#..#..#..#...", "...#..#..#..#...", "...#..#..#..#...", "...#..#..#..#...",
    "...#..#..#..#...", "...#..#..#..#...", "...##########...", "................"};

/* a plate in the middle of the screen: metal, the icon lit in a dark display, a title, a line under it, and (for a
 * countdown) a bar filling up */
static struct { uint32_t on, until; } splash;
static const char *const *splash_icon = ICON_DISK;
static int32_t splash_prog = -1;                /* the bar: 0..16, or -1 for none */
#define SPLASH_X 24
#define SPLASH_Y 84
static void draw_splash(const char *title, const char *sub, uint16_t ink)
{
    int32_t i, j;
    cv_oy = 0;
    cv_begin(192, 74, C_BG);
    metal(0, 0, 192, 74, C_BG);
    display(10, 10, 54, 54, C_LCD);
    for (j = 0; j < 16; j++)
        for (i = 0; i < 16; i++)
            if (splash_icon[j][i] == '#') cv_rect(21 + i * 2, 21 + j * 2, 2, 2, ink);
    txt(76, 14, &FONT_M, title, C_INK);
    if (sub && *sub) txt(76, 34, &FONT_S, sub, C_INK_SOFT);
    if (splash_prog >= 0) {
        display(76, 52, 104, 8, C_LCD);
        cv_rect(77, 53, 102 * splash_prog / 16, 6, ink);
    }
    cv_blit(SPLASH_X, SPLASH_Y);
    splash.on = 1;
    splash_icon = ICON_DISK;                    /* (the disk, no bar, unless the next caller says otherwise) */
    splash_prog = -1;
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

/* PRJ: the slots as rows in the well, the chosen one a metal key; the open project's number in the accent */
#define PRJ_ROW_Y (BODY_Y + 3)
#define PRJ_ROW_H 21
static void paint_prj(void)
{
    uint32_t slot;
    body_well();
    for (slot = 0; slot < PRJ_SLOTS; slot++) {
        const prj_head_t *h = &prj.head[slot];
        uint32_t sel = slot == prj.sel, cur = slot == prj.cur && slot != PRJ_AUTO, n;
        int32_t y = PRJ_ROW_Y + (int32_t)slot * PRJ_ROW_H;
        char s[24], lab[2] = {(char)(slot == PRJ_AUTO ? 'A' : '0' + slot), 0};
        if (sel) metal(4, y, 232, PRJ_ROW_H - 2, C_WELL);
        else { cv_rect(4, y, 232, PRJ_ROW_H - 2, C_CELL); corners(4, y, 232, PRJ_ROW_H - 2, C_WELL, 1, 1); }
        txt(11, y + 1, &FONT_M, lab, cur ? C_ACC : sel ? C_INK : C_TXT2);
        if (!prj.used[slot]) {
            txt(32, y + 3, &FONT_S, slot == PRJ_AUTO ? "NO AUTOSAVE" : "EMPTY", sel ? C_INK_SOFT : C_TXT3);
            continue;
        }
        n = copy_str(s, h->name, SONG_NAME);
        if (!n) n = copy_str(s, "UNTITLED", 9);
        if (cur && prj.unsaved) { s[n++] = '*'; s[n] = 0; }
        txt(32, y + 3, &FONT_S, s, sel ? C_INK : slot == PRJ_AUTO ? C_TXT2 : C_TXT);
        n = put_dec(s, h->bars);
        copy_str(s + n, h->bars == 1u ? " BAR" : " BARS", 6);
        txt(228 - text_w(&FONT_XS, s), y + 5, &FONT_XS, s, sel ? C_INK_SOFT : C_TXT2);
    }
}
static void draw_prj_foot(void)
{
    char s[32];
    uint32_t n;
    strip_begin(FOOT_Y, FOOT_H);
    metal(0, FOOT_Y, 240, FOOT_H, C_BG);
    if (msg_on()) foot_text(ui.msg, C_LCD_INK);
    else if (prj.ask) {                         /* the question, and its two answers */
        static const char *const Q[] = {"", "CLEAR ", "DISCARD CHANGES?", "DISCARD CHANGES?", "OVERWRITE "};
        n = copy_str(s, Q[prj.ask], 20);
        if (prj.ask == ASK_CLEAR || prj.ask == ASK_OVERWRITE) {
            slot_name(s + n, prj.ask_slot);
            n += 6u;
            s[n++] = '?'; s[n] = 0;
        }
        foot_text(s, C_LCD_INK);
        txt(231 - text_w(&FONT_XS, "+ YES  - NO"), FOOT_Y + 8, &FONT_XS, "+ YES  - NO", C_TXT2);
    } else if (prj.sel == PRJ_AUTO)
        foot_text(prj.used[0] ? "OCT+ LOAD" : "", C_TXT2);
    else if (prj.used[prj.sel])
        foot_text("OCT+ LOAD   OCT- CLEAR", C_TXT2);
    else
        foot_text("OCT+ NEW   HOLD SAVE: SAVE", C_TXT2);
    strip_end();
}
static void paint_body(void (*paint)(void))     /* the body, strip by strip */
{
    uint32_t y;
    for (y = BODY_Y; y < FOOT_Y; y += 64u) {
        strip_begin(y, FOOT_Y - y < 64u ? FOOT_Y - y : 64u);
        paint();
        strip_end();
    }
}
static void draw_prj(void)
{
    paint_body(paint_prj);
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

/* ================================================================== RIF: a riff's piano roll */
/* EDIT on a riff in ARR opens it here; HOME goes back. Time across, pitch up the screen (20 notes in view);
 * the cursor is a step and a note. The drum track's rows are named by their drums.
 *   SELECT / ALGORITHM   the cursor: the step / the note (as on ARR)
 *   OCT+                 a note in at the cursor (1 step); on a note: the note out
 *   OCT-                 undo (the riff's last change; recording: the last time round)
 *   KNOB 1 / 2 / 3       the note under the cursor: along the steps / up and down / its length
 *   KNOB 4               zoom, as on ARR: 8 / 16 / 32 / 64 steps in view
 *   REC                  record: the riff loops alone, the keys write onto its quantise grid (EDT); REC again:
 *                        rehearsal (REC blinks, nothing written), again: recording. PLAY or another screen: stop */
#define RIF_KW 30                               /* the keyboard column */
#define RIF_SW ((int32_t)(24 >> rif.zoom))      /* a step's width: 24 12 6 3 px at 2x 1x 0.5x 0.25x */
#define RIF_STEPS (8u << rif.zoom)              /* steps in view: 8 16 32 64 */
#define RIF_RH 9                                /* a note row's height */
#define RIF_ROWS 20                             /* rows in view */
#define RIF_Y (BODY_Y + 6)

static struct {
    uint32_t trk, r, at;                        /* the riff (track, id) and the bar the opened copy starts at */
    uint32_t step, note;                        /* the cursor */
    uint32_t left, top;                         /* the first step and the highest note in view */
    uint32_t zoom;                              /* as ARR's: 0 2x, 1 1x, 2 0.5x, 3 0.25x */
    uint32_t target;                            /* playing: the page the view slides to */
    uint32_t aud_note, aud_until;               /* a note sounding for a moment (put in, moved) */
} rif;

/* the note of the riff under the cursor (its pitch, a step it covers): its index in song.ev, or -1 */
static int32_t rif_note_here(void)
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    uint32_t i;
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
        const song_ev_t *e = &song.ev[i];
        if (e->note == rif.note && rif.step >= e->start && rif.step < (uint32_t)e->start + e->len) return (int32_t)i;
    }
    return -1;
}
static void audition_vel(uint32_t note, uint32_t vel)   /* the note, briefly, on the riff's track */
{
    if (rif.aud_until) ask(Q_OFF, rif.trk, rif.aud_note, 0);
    ask(Q_ON, rif.trk, note, vel);
    rif.aud_note = note;
    rif.aud_until = fm1_ms + 180u;
    if (!rif.aud_until) rif.aud_until = 1;
}
static void audition(uint32_t note) { audition_vel(note, 100); }

static const char *drum_name(uint32_t n)        /* General MIDI's names, short; 0 for notes without one */
{
    switch (n) {
    case 35: case 36: return "KICK";
    case 37: return "RIM";
    case 38: case 40: return "SNR";
    case 39: return "CLAP";
    case 41: case 43: return "TOM L";
    case 45: case 47: return "TOM M";
    case 48: case 50: return "TOM H";
    case 42: return "HH";
    case 44: return "PHH";
    case 46: return "OH";
    case 49: case 57: return "CRSH";
    case 51: case 59: return "RIDE";
    default: return 0;
    }
}
static void note_name(char *s, uint32_t n)      /* "C#3" (C4 = 60) */
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t k = copy_str(s, NAME[n % 12u], 3);
    if (n >= 12u) { s[k++] = (char)('0' + n / 12u - 1u); s[k] = 0; }
}
static int black_key(uint32_t n) { return (0x54Au >> (n % 12u)) & 1u; }
static uint16_t rif_row_bg(uint32_t n)          /* a note row's ground: a cell's, a shade darker for the black keys */
{
    if (rif.trk == SONG_DRUM_TRACK) return n & 1u ? C_CELL_ALT : C_CELL;
    return black_key(n) ? C_CELL_ALT : C_CELL;
}
static int key_down(uint32_t n)                 /* a key (or MIDI note on the track) plays note n now */
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if ((ui.keys_prev >> k & 1u) && ui.held[k] == n) return 1;
    return 0;
}

static void rif_follow(void)                    /* the view keeps the cursor in it */
{
    if (rif.step < rif.left) rif.left = rif.step;
    else if (rif.step >= rif.left + RIF_STEPS) rif.left = rif.step - RIF_STEPS + 1u;
    if (rif.note > rif.top) rif.top = rif.note;
    else if (rif.note + RIF_ROWS <= rif.top) rif.top = rif.note + RIF_ROWS - 1u;
}

static void undo_reset(void);
static void rif_open(uint32_t trk, uint32_t r, uint32_t at, uint32_t step)
{
    const song_riff_t *f = &song.riff[trk][r - 1u];
    uint32_t i, lo = 127, hi = 0;
    rif.trk = trk; rif.r = r; rif.at = at;
    rif.step = step < f->steps ? step : 0u;
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
        if (song.ev[i].note < lo) lo = song.ev[i].note;
        if (song.ev[i].note > hi) hi = song.ev[i].note;
    }
    if (!f->count) lo = hi = trk == SONG_DRUM_TRACK ? 38u : 60u;
    rif.note = (lo + hi) / 2u;                  /* the middle of what is there */
    rif.top = (lo + hi) / 2u + RIF_ROWS / 2u;
    if (rif.top > 127u) rif.top = 127u;
    if (rif.top < RIF_ROWS - 1u) rif.top = RIF_ROWS - 1u;
    rif.left = rif.step / RIF_STEPS * RIF_STEPS;
    rif_follow();
    undo_reset();                               /* (undo is this riff's) */
    ui.mode = M_RIF;
    ui.dirty = D_ALL;
}

/* undo (OCT- on RIF): the opened riff as it was before each change, newest last, UNDO_N deep. Turns of one knob in
 * a row (less than a second apart) are one change; recording, everything put in on one time round is one. */
#define UNDO_N 16u
#define UNDO_NOTES 384u                         /* a riff with more notes than this: its changes cannot be undone */
enum { U_NONE, U_ADD, U_DEL, U_MOVE, U_PITCH, U_LEN, U_STEPS, U_QUANT, U_VEL, U_REC };
typedef struct { uint16_t count; uint8_t steps, quant; song_ev_t ev[UNDO_NOTES]; } undo_t;
static undo_t undo_buf[UNDO_N] __attribute__((section(".pool")));
static struct { uint32_t n, top, kind, at, ms, big; } undo;   /* n held, top the next slot; kind/at/ms: the last change */

static void undo_reset(void) { undo.n = 0; undo.kind = U_NONE; }
/* before a change of `kind` (at: what it is about, a note's index or a pass): the riff kept, unless it continues
 * the change before */
static void undo_push(uint32_t kind, uint32_t at)
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    undo_t *u;
    uint32_t i;
    if (kind == undo.kind && at == undo.at && (kind == U_REC || fm1_ms - undo.ms < 1000u) && kind != U_ADD && kind != U_DEL) {
        undo.ms = fm1_ms;
        return;
    }
    undo.kind = kind; undo.at = at; undo.ms = fm1_ms;
    if (f->count > UNDO_NOTES) { undo.n = 0; undo.big = 1; return; }   /* too big to keep: no undo past here */
    undo.big = 0;
    u = &undo_buf[undo.top];
    u->count = f->count; u->steps = f->steps; u->quant = f->quant;
    for (i = 0; i < f->count; i++) u->ev[i] = song.ev[f->first + i];
    undo.top = (undo.top + 1u) % UNDO_N;
    if (undo.n < UNDO_N) undo.n++;
}
static int undo_pop(void)                       /* the riff back as it was: 1 if there was something to undo */
{
    song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    const undo_t *u;
    uint32_t i;
    if (!undo.n) return 0;
    undo.top = (undo.top + UNDO_N - 1u) % UNDO_N;
    undo.n--;
    u = &undo_buf[undo.top];
    seq_busy = 1;
    riff_clear(&song, rif.trk, rif.r);
    f->steps = u->steps; f->quant = u->quant;
    for (i = 0; i < u->count; i++)
        riff_note_put(&song, rif.trk, rif.r, u->ev[i].start, u->ev[i].tick, u->ev[i].len, u->ev[i].note, u->ev[i].vel);
    seq_busy = 0;
    undo.kind = U_NONE;                         /* (the next change is a change of its own) */
    if (rif.step >= f->steps) rif.step = f->steps - 1u;
    if (rif.left >= f->steps) rif.left = 0;
    seq_bars = song_bars(&song);
    return 1;
}

static uint32_t rif_pos(void)                   /* the playhead's step in the opened copy of the riff, or ~0 */
{
    uint32_t st;
    if (!seq_play) return ~0u;
    if (seq_riff) return seq_riff == rif.r && seq_rtrk == rif.trk ? seq_step : ~0u;   /* (the riff alone, recording) */
    st = seq_step;
    if (st < rif.at * STEPS_PER_BAR) return ~0u;
    st -= rif.at * STEPS_PER_BAR;
    return st < song.riff[rif.trk][rif.r - 1u].steps ? st : ~0u;
}

/* RIF drawn: the keyboard (ivory keys, ebony ones laid over them; on the drum track plain rows named by their
 * drums), the rows (a shade darker for the black keys), the grid (a line a step, brighter a beat, brightest a bar,
 * shaded past the riff's end), the notes as raised tiles (mint under the cursor), the cursor, the playhead */
static void paint_rif(void)
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    uint32_t row, s, i, drums = rif.trk == SONG_DRUM_TRACK, pos = rif_pos();
    const int32_t y0 = RIF_Y, gh = RIF_ROWS * RIF_RH;
    body_well();
    for (row = 0; row < RIF_ROWS; row++) {
        uint32_t n = rif.top - row, lit, blk;
        int32_t y = y0 + (int32_t)(row * RIF_RH);
        if ((int32_t)n < 0 || n > 127u) continue;
        lit = key_down(n);
        blk = !drums && black_key(n);
        cv_rect(RIF_KW, y, 238 - RIF_KW, RIF_RH, rif_row_bg(n));
        if (!drums && n % 12u == 0u) cv_rect(RIF_KW, y + RIF_RH - 1, 238 - RIF_KW, 1, C_LINE_BAR);   /* under each C */
        if (drums) cv_rect(2, y, RIF_KW - 4, RIF_RH, n & 1u ? C_KEY_W2 : C_KEY_W);
        else vgrad(2, y, RIF_KW - 4, RIF_RH, C_KEY_W, C_KEY_W2);
        if (blk) {                              /* an ebony key over the ivory, 60 % of the width, raised */
            vgrad(2, y + 1, 16, RIF_RH - 1, C_KEY_B2, C_KEY_B);
            cv_rect(17, y + 1, 1, RIF_RH - 1, 0);
            cv_rect(2, y + RIF_RH - 1, 16, 1, 0);
        }
        if (!drums && (n % 12u == 0u || n % 12u == 5u)) cv_rect(2, y + RIF_RH - 1, RIF_KW - 4, 1, C_KEY_GAP);
        if (lit) {                              /* played now: the key mint, its row lit faintly */
            cv_rect(2, y, blk ? 16 : RIF_KW - 4, RIF_RH - 1, C_SEL);
            cv_rect(RIF_KW, y, 238 - RIF_KW, RIF_RH, mix565(rif_row_bg(n), C_ACC, 5));
        }
        if (n == rif.note) cv_rect(RIF_KW - 3, y + 1, 2, RIF_RH - 2, C_ACC);   /* the cursor's row: a mark */
    }
    for (row = 0; row < RIF_ROWS; row++) {      /* the keys' labels: C on each octave, the cursor's note; drums by name */
        uint32_t n = rif.top - row, used = 0, blk;
        int32_t y = y0 + (int32_t)(row * RIF_RH);
        char s_[8];
        const char *lab = 0;
        if ((int32_t)n < 0 || n > 127u) continue;
        blk = !drums && black_key(n);
        if (drums) {
            for (i = f->first; i < (uint32_t)f->first + f->count && !used; i++) used = song.ev[i].note == n;
            if (used || n == rif.note || key_down(n)) lab = drum_name(n);
        } else if (n % 12u == 0u || n == rif.note) { note_name(s_, n); lab = s_; }
        if (lab) text_x(drums || blk ? 4 : 13, y, &FONT_XS, lab, blk && !key_down(n) ? C_TXT : C_INK, 2, RIF_KW - 2);
    }
    for (s = 0; s <= RIF_STEPS; s++) {          /* the grid; past the riff's end, shaded */
        uint32_t st = rif.left + s;
        int32_t x = RIF_KW + (int32_t)(s * (uint32_t)RIF_SW);
        if (st % 16u == 0u) cv_rect(x, y0, 1, gh, C_LINE_BAR4);
        else if (st % 4u == 0u) { if (RIF_SW >= 3) cv_rect(x, y0, 1, gh, C_LINE_BAR); }
        else if (RIF_SW >= 6) cv_rect(x, y0, 1, gh, C_LINE_BEAT);
        if (s < RIF_STEPS && st >= f->steps) cv_rect(x + (RIF_SW > 3 ? 1 : 0), y0, RIF_SW > 3 ? RIF_SW - 1 : RIF_SW, gh, C_WELL_DARK);
    }
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {   /* the notes (in ticks: a recorded one may sit between steps) */
        const song_ev_t *e = &song.ev[i];
        uint32_t a = e->start * SONG_TPS + e->tick, t0 = a, t1 = a + e->len * SONG_TPS, row_ = rif.top - e->note;
        uint32_t vl = rif.left * SONG_TPS, vr = (rif.left + RIF_STEPS) * SONG_TPS, sel;
        int32_t x0, x1, y;
        if (e->note > rif.top || row_ >= RIF_ROWS) continue;
        if (t1 > f->steps * SONG_TPS) t1 = f->steps * SONG_TPS;
        if (t1 <= vl || t0 >= vr) continue;
        if (t0 < vl) t0 = vl;
        if (t1 > vr) t1 = vr;
        x0 = RIF_KW + 1 + (int32_t)((t0 - vl) * (uint32_t)RIF_SW / SONG_TPS);
        x1 = RIF_KW + (int32_t)((t1 - vl) * (uint32_t)RIF_SW / SONG_TPS);
        if (x1 <= x0) x1 = x0 + 1;
        y = y0 + (int32_t)(row_ * RIF_RH) + 1;
        sel = e->note == rif.note && rif.step >= e->start && rif.step < (uint32_t)e->start + e->len;
        bevel(x0, y, x1 - x0, RIF_RH - 1, sel ? mix565(C_SEL, C_SEL_TOP, 6) : mix565(C_RIFF, C_RIFF_TOP, 6),
              sel ? C_SEL : C_RIFF, sel ? C_SEL_TOP : C_RIFF_TOP, sel ? C_SEL_BOT : C_RIFF_BOT, rif_row_bg(e->note),
              t0 == a, t1 < vr || (uint32_t)e->start + e->len <= f->steps);
    }
    if (rif.note <= rif.top && rif.top - rif.note < RIF_ROWS) {   /* the cursor: a white frame on its step */
        int32_t x = RIF_KW + (int32_t)((rif.step - rif.left) * (uint32_t)RIF_SW), y = y0 + (int32_t)((rif.top - rif.note) * RIF_RH);
        cv_rect(x, y, RIF_SW + 1, 1, C_TXT); cv_rect(x, y + RIF_RH, RIF_SW + 1, 1, C_TXT);
        cv_rect(x, y, 1, RIF_RH + 1, C_TXT); cv_rect(x + RIF_SW, y, 1, RIF_RH + 1, C_TXT);
    }
    if (pos != ~0u && pos >= rif.left && pos < rif.left + RIF_STEPS)   /* the playhead: red while recording */
        cv_rect(RIF_KW + (int32_t)((pos - rif.left) * (uint32_t)RIF_SW), y0, 2, gh, rec.state == REC_ON ? C_RED : C_PLAY);
    body_edge_r(BODY_Y + 1, BODY_H - 2);
}

/* RIF's footer: what K1-K4 do to the note under the cursor (its length shown when there is one), and the zoom */
static void draw_rif_foot(void)
{
    char k3[8];
    int32_t i = rif_note_here();
    if (i >= 0) k3[copy_str(k3, "LEN ", 5) + put_dec(k3 + 4, song.ev[i].len)] = 0;
    else copy_str(k3, "LEN", 4);
    draw_foot_knobs("MOVE", "PITCH", k3, ZOOM_NAME[rif.zoom] + 5);
}
static void draw_rif(void) { paint_body(paint_rif); }

/* ================================================================== EDT: the riff's settings (EDIT on RIF) */
/* Tabs (SELECT): RIFF, and NOTE when the cursor is on a note. ALGORITHM picks a row, KNOB 1 changes its value.
 * EDIT goes back to the roll, HOME to ARR.
 *   RIFF   LENGTH     1..64 steps (notes past a shorter end go, notes running past it are cut)
 *          QUANTIZE   the grid recording puts notes on: 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 (T: triplets)
 *   NOTE   VELOCITY   1..127 (sounded as it changes) */
enum { TAB_RIFF, TAB_NOTE };
static struct { uint32_t tab, row; } edt;
static const char *const EDT_TAB[2] = {"RIFF", "NOTE"};
static const uint32_t EDT_ROWS[2] = {2, 1};

static void edt_value(char *s, uint32_t tab, uint32_t row)   /* the row's value as text */
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    uint32_t n;
    (void)row;
    if (tab == TAB_RIFF) {                      /* "24 STEPS  1.5 BARS" */
        n = put_dec(s, f->steps);
        n += copy_str(s + n, f->steps == 1u ? " STEP" : " STEPS", 7);
        s[n] = 0;
    } else {
        int32_t i = rif_note_here();
        if (i < 0) copy_str(s, "-", 2);
        else { n = put_dec(s, song.ev[i].vel); s[n] = 0; }
    }
}

/* EDT drawn: the tabs as metal keys (the open one pressed in, its LED lit), each setting in a well of its own (its
 * name, its value large, the chosen one's in the accent with K1's knob beside it, a gauge along the bottom) */
#define EDT_TAB_Y (BODY_Y + 4)
#define EDT_ROW_Y (BODY_Y + 31)
#define EDT_ROW_H 74
static void paint_edt(void)
{
    uint32_t tb, r, has_note = rif_note_here() >= 0;
    char s[24];
    for (tb = 0; tb < 2u; tb++) {
        int32_t x = 4 + (int32_t)tb * 80, on = tb == edt.tab, off = tb == TAB_NOTE && !has_note;
        if (on) metal_pressed(x, EDT_TAB_Y, 76, 22, C_BG);
        else metal(x, EDT_TAB_Y, 76, 22, C_BG);
        led(x + 9, EDT_TAB_Y + 8, (uint32_t)on, C_METAL_TOP);   /* the key's LED */
        txt(x + 20, EDT_TAB_Y + 5, &FONT_S, EDT_TAB[tb], off ? C_INK_SOFT : C_INK);
    }
    for (r = 0; r < EDT_ROWS[edt.tab]; r++) {
        static const char *const NAME[2][2] = {{"LENGTH", "QUANTIZE"}, {"VELOCITY", ""}};
        static const char *const UNIT[2][2] = {{"STEPS", ""}, {"", ""}};
        const char *unit = UNIT[edt.tab][r];
        const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
        uint32_t sel = r == edt.row, v = 0, vmax = 1, k;
        int32_t x, y = EDT_ROW_Y + (int32_t)r * (EDT_ROW_H + 6);
        char nn[16];
        well(4, y, 232, EDT_ROW_H, sel ? mix565(C_WELL, C_CELL, 14) : C_WELL, C_BG, 0, 0);
        txt(14, y + 8, &FONT_S, NAME[edt.tab][r], sel ? C_TXT : C_TXT2);
        if (edt.tab == TAB_RIFF && r == 1u) {   /* the grid: "1/8" and TRIPLET */
            const char *q = QUANT_NAME[f->quant];
            for (k = 0; q[k] && q[k] != 'T'; k++) s[k] = q[k];
            s[k] = 0;
            if (q[k] == 'T') unit = "TRIPLET";
        } else {
            if (edt.tab == TAB_RIFF) { v = f->steps; vmax = RIFF_MAX_STEPS; }
            else if (has_note) {                /* the note it is about, after the value: "C4 @ 5" */
                uint32_t n;
                v = song.ev[rif_note_here()].vel; vmax = 127;
                if (rif.trk == SONG_DRUM_TRACK && drum_name(rif.note)) n = copy_str(nn, drum_name(rif.note), 6);
                else { note_name(nn, rif.note); n = copy_str(nn, nn, 5); }
                n += copy_str(nn + n, " @ ", 4);
                nn[n + put_dec(nn + n, rif.step + 1u)] = 0;
                unit = nn;
            }
            if (edt.tab == TAB_NOTE && !has_note) copy_str(s, "-", 2);
            else { k = put_dec(s, v); s[k] = 0; }
        }
        x = txt(14, y + 24, &FONT_L, s, sel ? C_ACC : C_TXT);
        txt(x + 6, y + 36, &FONT_S, unit, C_TXT2);
        if (sel) {                              /* the knob that turns it */
            knob_chip(212, y + 8, 0, mix565(C_WELL, C_CELL, 14));
        }
        if (edt.tab == TAB_RIFF && r == 1u) {   /* a bar, a mark on each place of the grid, the beats taller */
            uint32_t g = RIFF_Q_TICKS[f->quant];
            cv_rect(14, y + 64, 212, 1, C_LINE_BEAT);
            for (k = 0; k < STEPS_PER_BAR * SONG_TPS; k += g) {
                int32_t bx = 14 + (int32_t)(k * 212u / (STEPS_PER_BAR * SONG_TPS)), beat = k % 24u == 0u;
                cv_rect(bx, y + (beat ? 58 : 60), 2, beat ? 8 : 5, beat ? C_TXT : C_TXT2);
            }
        } else if (edt.tab == TAB_RIFF) {       /* the steps, a cell each, the riff's lit, a gap every bar */
            for (k = 0; k < RIFF_MAX_STEPS; k++)
                cv_rect(14 + (int32_t)(k * 3u) + (int32_t)(k / 16u) * 4, y + 60, 2, 6,
                        k < v ? (k % 4u ? C_RIFF : C_RIFF_TOP) : C_CELL);
        } else {                                /* a level */
            cv_rect(14, y + 62, 212, 3, C_CELL);
            cv_rect(14, y + 62, (int32_t)(212u * v / vmax), 3, C_ACC);
        }
    }
}
static void draw_edt(void) { paint_body(paint_edt); }
static void draw_edt_foot(void) { draw_foot_knobs("VALUE", 0, 0, 0); }

static void edt_input(int32_t dsel, int32_t dalg, int32_t dk1)
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    if (dsel) {                                 /* the tabs: NOTE only with a note under the cursor */
        uint32_t tab = (uint32_t)clamp((int32_t)edt.tab + dsel, 0, 1);
        if (tab == TAB_NOTE && rif_note_here() < 0) say("NO NOTE AT THE CURSOR");
        else { edt.tab = tab; edt.row = 0; }
        ui.dirty |= D_LANES | D_FOOT;
    }
    if (dalg) {
        edt.row = (uint32_t)clamp((int32_t)edt.row + dalg, 0, (int32_t)EDT_ROWS[edt.tab] - 1);
        ui.dirty |= D_LANES;
    }
    if (dk1) {
        if (edt.tab == TAB_RIFF && edt.row == 1u) {   /* the quantise */
            song_riff_t *fq = &song.riff[rif.trk][rif.r - 1u];
            uint32_t q = (uint32_t)clamp((int32_t)fq->quant + dk1, 0, (int32_t)RIFF_QUANTS - 1);
            if (q != fq->quant) { undo_push(U_QUANT, 0); fq->quant = (uint8_t)q; edited(); }
        } else if (edt.tab == TAB_RIFF) {       /* the riff's length */
            uint32_t st = (uint32_t)clamp((int32_t)f->steps + dk1, 1, (int32_t)RIFF_MAX_STEPS);
            if (st != f->steps) {
                undo_push(U_STEPS, 0);
                seq_busy = 1;
                riff_set_steps(&song, rif.trk, rif.r, st);
                seq_busy = 0;
                if (rif.step >= st) rif.step = st - 1u;
                if (rif.left >= st) rif.left = 0;
                seq_bars = song_bars(&song);
                edited();
            }
        } else {
            int32_t i = rif_note_here();
            if (i >= 0) {
                undo_push(U_VEL, (uint32_t)i);
                song.ev[i].vel = (uint8_t)clamp((int32_t)song.ev[i].vel + dk1, 1, 127);
                audition_vel(song.ev[i].note, song.ev[i].vel);
                edited();
            }
        }
        ui.dirty |= D_LANES;
    }
}

/* ARR's SEQ: on an empty bar a new riff (16 steps); on a riff: the riff deleted. (EDIT on a riff opens RIF.) */
static void new_riff_here(void)
{
    uint32_t r = riff_free(&song, ui.trk);
    if (!r) { say("NO FREE RIFF"); return; }
    riff_new(&song, ui.trk, r, 16);
    song_place(&song, ui.bar, ui.trk, r);
    seq_bars = song_bars(&song);
    edited();
    say("NEW RIFF");
    ui.dirty |= D_LANES | D_FOOT;
}
static void delete_riff_here(void)              /* the copy under the cursor; the riff itself too once nothing uses it */
{
    uint32_t b0 = 0, r = song_at(&song, ui.bar, ui.trk, &b0), b;
    if (!r) return;
    song.cell[b0][ui.trk] = 0;
    for (b = 0; b < SONG_BARS && song.cell[b][ui.trk] != r; b++) {}
    if (b == SONG_BARS) riff_delete(&song, ui.trk, r);
    seq_bars = song_bars(&song);
    edited();
}
static void seq_button(uint32_t b)
{
    if (!(b & (1u << BTN_SEQ)) || ui.mode != M_ARR) return;
    if (!song_at(&song, ui.bar, ui.trk, 0)) { new_riff_here(); return; }   /* an empty bar: a new riff */
    delete_riff_here();                         /* on a riff: gone */
    splash_icon = ICON_TRASH;
    splash_for("DELETED", "", RGB(255, 110, 100), 700);
}

/* ================================================================== recording (REC on RIF) */
/* The riff loops alone from its start; a key pressed while recording puts its note on the riff's quantise grid
 * (the nearest place), at once; letting go sets its length (in steps, at least 1). Round again, notes go on over
 * what is there: the same note at the same place is replaced, an earlier one of the same pitch running into it is
 * cut where it starts. */
static uint32_t rec_fine(void)                  /* where the playhead is, in 1/256 ticks of the riff */
{
    uint32_t st = seq_step, sub = seq_sub, ph = seq_ph;
    return ((st * SONG_TPS + sub) << 8) + (ph >> 24);
}
static void rec_mute_set(uint32_t note, uint32_t on)
{
    if (on) rec_mute[note >> 5] |= 1u << (note & 31u);
    else rec_mute[note >> 5] &= ~(1u << (note & 31u));
}
static void rec_down(uint32_t note, uint32_t vel)
{
    song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    uint32_t g = RIFF_Q_TICKS[f->quant], len = f->steps * SONG_TPS, fine = rec_fine(), q, i, k;
    int32_t ok;
    if (rec.state != REC_ON || note > 127u) return;
    q = (fine + g * 128u) / (g * 256u) * g;     /* the nearest place on the grid */
    if (q >= len) q = 0;                        /* (past the end: the riff's start, next time round) */
    undo_push(U_REC, seq_ticks / len);          /* (a time round: one undo) */
    seq_busy = 1;
    for (i = f->first; i < (uint32_t)f->first + f->count; i++) {   /* the same pitch running into it: cut */
        song_ev_t *e = &song.ev[i];
        uint32_t a = e->start * SONG_TPS + e->tick;
        if (e->note == note && a < q && a + e->len * SONG_TPS > q)
            e->len = (uint8_t)((q - a) / SONG_TPS ? (q - a) / SONG_TPS : 1u);
    }
    ok = riff_note_put(&song, rif.trk, rif.r, q / SONG_TPS, q % SONG_TPS, 1, note, vel);
    seq_busy = 0;
    if (ok < 0) { say("NO ROOM"); return; }
    rec_mute_set(note, 1);                      /* (it sounds from the key: the riff leaves it be) */
    if ((q << 8) > fine)                        /* put ahead of the playhead: not again when it gets there */
        for (k = 0; k < 8u; k++) if (!rec_skip[k]) { rec_skip[k] = (uint16_t)((q << 7 | note) + 1u); break; }
    for (k = 0; k < REC_HOLDS; k++)
        if (!rec.hold[k].on) {
            rec.hold[k].on = 1; rec.hold[k].note = (uint8_t)note;
            rec.hold[k].start = (uint8_t)(q / SONG_TPS); rec.hold[k].tick = (uint8_t)(q % SONG_TPS);
            rec.hold[k].t0 = seq_ticks;
            break;
        }
    edited();
    ui.dirty |= D_LANES;
}
static void rec_up(uint32_t note)               /* the key let go: the note's length */
{
    const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
    uint32_t k, i, len;
    int found;
    if (note > 127u) return;
    rec_mute_set(note, 0);
    for (k = 0; k < REC_HOLDS; k++) {
        if (!rec.hold[k].on || rec.hold[k].note != note) continue;
        rec.hold[k].on = 0;
        len = (seq_ticks - rec.hold[k].t0 + SONG_TPS / 2u) / SONG_TPS;
        if (len < 1u) len = 1u;
        if (len > f->steps) len = f->steps;
        i = riff_find(&song, f, rec.hold[k].start, rec.hold[k].tick, note, &found);
        if (found) song.ev[i].len = (uint8_t)len;
        ui.dirty |= D_LANES;
    }
}
static void rec_stop(void)
{
    uint32_t k;
    for (k = 0; k < REC_HOLDS; k++) if (rec.hold[k].on) rec_up(rec.hold[k].note);
    for (k = 0; k < 4u; k++) rec_mute[k] = 0;
    for (k = 0; k < 8u; k++) rec_skip[k] = 0;
    rec.state = REC_OFF;
    seq_play = 0;
    ui.dirty |= D_HEAD | D_LANES | D_FOOT;
}
static void rec_button(void)                    /* REC on RIF: record, then rehearsal and back */
{
    if (rec.state == REC_OFF) {
        uint32_t k;
        for (k = 0; k < 8u; k++) rec_skip[k] = 0;
        seq_rtrk = rif.trk;
        seq_riff = rif.r;
        seq_restart = 1;                        /* (from the top, also when something was playing) */
        seq_play = 1;
        rec.state = REC_ON;
    } else if (rec.state == REC_ON) {
        uint32_t k;
        for (k = 0; k < REC_HOLDS; k++) if (rec.hold[k].on) rec_up(rec.hold[k].note);
        rec.state = REC_REHEARSE;
    } else rec.state = REC_ON;
    ui.dirty |= D_HEAD | D_LANES | D_FOOT;
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
    rif.zoom = ZOOM_1X;
    ui.drawn_pos = ~0u;
    ui.dirty = D_ALL;
    lcd_fill(0, 0, 240, 240, C_BG);
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
                uint32_t t = ui.mode == M_PRJ && prj.sel != PRJ_AUTO ? prj.sel : prj.cur;   /* (as save_now) */
                hold.shown = left;
                if (t == PRJ_NONE || t == PRJ_AUTO) copy_str(s, "HOLD", 5);
                else slot_name(s, t);
                splash_prog = (int32_t)(el * 16u / SAVE_HOLD_MS);   /* the bar fills as SAVE is held */
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
    } else if (ui.mode == M_EDT) {              /* EDT: SELECT the tab, ALGORITHM the row, KNOB 1 the value */
        int32_t ds = enc(ENC_SELECT), da = enc(ENC_ALGORITHM), d1 = enc(ENC_KNOB1);
        edt_input(ds, da, d1);
        enc(ENC_PRESETS); enc(ENC_KNOB2); enc(ENC_KNOB3); enc(ENC_KNOB4);
    } else if (ui.mode == M_RIF) {              /* RIF: SELECT the step, ALGORITHM the note (clockwise: down) */
        const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
        if ((d = enc(ENC_SELECT)) != 0) {
            rif.step = (uint32_t)clamp((int32_t)rif.step + d, 0, (int32_t)f->steps - 1);
            rif_follow();
            ui.dirty |= D_LANES | D_FOOT;
        }
        if ((d = enc(ENC_ALGORITHM)) != 0) {
            rif.note = (uint32_t)clamp((int32_t)rif.note - d, 0, 127);
            rif_follow();
            ui.dirty |= D_LANES | D_FOOT;
        }
        if ((d = enc(ENC_KNOB1)) != 0) {   /* the note under the cursor along the steps */
            int32_t i = rif_note_here();
            if (i < 0) say("NO NOTE HERE");
            else {
                song_ev_t e = song.ev[i];
                int32_t ns = clamp((int32_t)e.start + d, 0, (int32_t)f->steps - 1);
                uint32_t off = rif.step - e.start;
                int found = 0;
                if (ns != (int32_t)e.start) {
                    riff_find(&song, f, (uint32_t)ns, e.tick, e.note, &found);
                    if (found) say("NO ROOM");
                    else {
                        undo_push(U_MOVE, e.note);
                        seq_busy = 1;
                        riff_note_del_at(&song, rif.trk, rif.r, e.start, e.tick, e.note);
                        riff_note_put(&song, rif.trk, rif.r, (uint32_t)ns, e.tick, e.len, e.note, e.vel);
                        seq_busy = 0;
                        rif.step = (uint32_t)ns + off < f->steps ? (uint32_t)ns + off : (uint32_t)ns;
                        rif_follow();
                        edited();
                    }
                }
                ui.dirty |= D_LANES | D_FOOT;
            }
        }
        if ((d = enc(ENC_KNOB2)) != 0) {        /* the note under the cursor up / down (clockwise: up) */
            int32_t i = rif_note_here();
            if (i < 0) say("NO NOTE HERE");
            else {
                song_ev_t e = song.ev[i];
                int32_t nn = clamp((int32_t)e.note + d, 0, 127);
                int found = 0;
                if (nn != (int32_t)e.note) {
                    riff_find(&song, f, e.start, e.tick, (uint32_t)nn, &found);
                    if (found) say("NO ROOM");
                    else {
                        undo_push(U_PITCH, e.start * SONG_TPS + e.tick);
                        seq_busy = 1;
                        riff_note_del_at(&song, rif.trk, rif.r, e.start, e.tick, e.note);
                        riff_note_put(&song, rif.trk, rif.r, e.start, e.tick, e.len, (uint32_t)nn, e.vel);
                        seq_busy = 0;
                        rif.note = (uint32_t)nn;
                        rif_follow();
                        audition(rif.note);
                        edited();
                    }
                }
                ui.dirty |= D_LANES | D_FOOT;
            }
        }
        if ((d = enc(ENC_KNOB3)) != 0) {        /* its length, 1 step up to the riff's end */
            int32_t i = rif_note_here();
            if (i < 0) say("NO NOTE HERE");
            else {
                song_ev_t *e = &song.ev[i];
                undo_push(U_LEN, (uint32_t)i);
                e = &song.ev[i];
                e->len = (uint8_t)clamp((int32_t)e->len + d, 1, (int32_t)(f->steps - e->start));
                if (rif.step >= (uint32_t)e->start + e->len) { rif.step = (uint32_t)e->start + e->len - 1u; rif_follow(); }
                edited();
                ui.dirty |= D_LANES | D_FOOT;
            }
        }
        if ((d = enc(ENC_KNOB4)) != 0) {        /* zoom, as on ARR: anticlockwise out, clockwise in */
            int32_t z = clamp((int32_t)rif.zoom - d, 0, 3);
            if ((uint32_t)z != rif.zoom) {
                rif.zoom = (uint32_t)z;
                rif.left = rif.step >= RIF_STEPS / 2u ? rif.step - RIF_STEPS / 2u : 0u;
                if (d > 0) rif.left = rif.step / RIF_STEPS * RIF_STEPS;
                rif_follow();
                say(ZOOM_NAME[rif.zoom]);
                ui.dirty |= D_LANES | D_FOOT;
            }
        }
        enc(ENC_PRESETS);
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
        if ((d = enc(ENC_KNOB3)) != 0) {           /* the loop: clockwise on (4 bars) and longer, anticlockwise shorter, off */
            uint32_t on = song.loop_end > song.loop_start, len = on ? song.loop_end - song.loop_start : 0u, start = on ? song.loop_start : ui.bar;
            for (; d > 0; d--) len = len ? (len < 32u ? len + 1u : 32u) : 4u;
            for (; d < 0; d++) if (len) len--;
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
    seq_button(b);
    if (b & (1u << BTN_EDIT)) {                 /* EDIT: on ARR a riff's piano roll; on RIF its settings, and back */
        if (ui.mode == M_ARR) {
            uint32_t b0 = 0, r = song_at(&song, ui.bar, ui.trk, &b0);
            if (r) rif_open(ui.trk, r, b0, (ui.bar - b0) * STEPS_PER_BAR);
            else say("NO RIFF HERE");
        } else if (ui.mode == M_RIF) {
            ui.mode = M_EDT;
            edt.tab = rif_note_here() >= 0 ? TAB_NOTE : TAB_RIFF;   /* on a note: its settings first */
            edt.row = 0;
            ui.dirty = D_ALL;
        } else if (ui.mode == M_EDT) { ui.mode = M_RIF; ui.dirty = D_ALL; }
    }
    if (b & (1u << BTN_HOME) && ui.mode != M_ARR) { ui.mode = M_ARR; prj.ask = ASK_NONE; ui.dirty = D_ALL; }
    if (ui.mode == M_PRJ) {
        prj_input(pd, b);
        b &= ~((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP));   /* (the octave keys answer here, they do not shift) */
    }
    if (ui.mode == M_RIF && b & ((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP))) {   /* RIF: a note in / out */
        if (b & (1u << BTN_OCT_UP)) {           /* a note in; on a note: out */
            int32_t i = rif_note_here(), ok;
            if (i >= 0) {
                undo_push(U_DEL, 0);
                seq_busy = 1;
                riff_note_del_at(&song, rif.trk, rif.r, song.ev[i].start, song.ev[i].tick, song.ev[i].note);
                seq_busy = 0;
                edited();
            } else {
                undo_push(U_ADD, 0);
                seq_busy = 1; ok = riff_note_set(&song, rif.trk, rif.r, rif.step, 1, rif.note, 100); seq_busy = 0;
                if (ok < 0) { say("NO ROOM"); undo_pop(); }
                else { audition(rif.note); edited(); }
            }
        }
        if (b & (1u << BTN_OCT_DOWN)) {         /* undo */
            uint32_t k;
            for (k = 0; k < REC_HOLDS; k++) if (rec.hold[k].on) rec_up(rec.hold[k].note);
            if (undo_pop()) { edited(); say("UNDO"); }
            else say(undo.big ? "TOO MANY NOTES TO UNDO" : "NOTHING TO UNDO");
        }
        b &= ~((1u << BTN_OCT_DOWN) | (1u << BTN_OCT_UP));
        ui.dirty |= D_LANES | D_FOOT;
    }
    if (rif.aud_until && (int32_t)(rif.aud_until - fm1_ms) <= 0) { ask(Q_OFF, rif.trk, rif.aud_note, 0); rif.aud_until = 0; }
    if (rec.state && ui.mode != M_RIF) rec_stop();   /* another screen: the recording stops */
    if (b & (1u << BTN_REC)) {
        if (ui.mode == M_RIF) rec_button();
        else { ui.rec = !ui.rec; ui.dirty |= D_HEAD; }
    }
    if (b & (1u << BTN_GLO)) { seq_click = !seq_click; ui.dirty |= D_HEAD; }   /* the metronome (for now on GLO) */
    if (b & (1u << BTN_PLAY) && rec.state) { rec_stop(); b &= ~(1u << BTN_PLAY); }   /* PLAY / STOP: recording ends */
    if (b & (1u << BTN_PLAY)) {
        seq_riff = 0;                           /* the song */
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
    if (ui.mode == M_RIF && (down | up)) ui.dirty |= D_LANES;   /* the played keys light up in the roll */
    for (k = 0; k < 27u; k++) {
        if (up >> k & 1u) {
            ask(Q_OFF, ui.trk, ui.held[k], 0); midi_send(0x80u | ui.trk, ui.held[k], 0);
            if (rec.state) rec_up(ui.held[k]);
        }
        if (down >> k & 1u) {
            ui.held[k] = (uint8_t)(29u + 12u * ui.oct + k);   /* oct 2: key 0 = F3; on the drums F3 .. are GM drums 53 .. */
            if (ui.trk == SONG_DRUM_TRACK) ui.held[k] = (uint8_t)(35u + k);   /* the drums: key 0 = kick (35) */
            ask(Q_ON, ui.trk, ui.held[k], 100);
            midi_send(0x90u | ui.trk, ui.held[k], 100);
            if (rec.state && ui.trk == rif.trk) rec_down(ui.held[k], 100);
        }
    }
    while (midi_get(&st, &d1, &d2)) {           /* MIDI in, channels 1-4 = tracks 1-4 */
        uint32_t ch = st & 15u;
        if (ch >= SONG_TRACKS) continue;
        if ((st & 0xF0u) == 0x90u && d2) {
            ask(Q_ON, ch, d1, d2);
            if (rec.state && ch == rif.trk) rec_down(d1, d2);   /* MIDI on the riff's channel records too */
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            ask(Q_OFF, ch, d1, 0);
            if (rec.state && ch == rif.trk) rec_up(d1);
        }
    }

    fm1_led_key(BTN_PLAY, seq_play != 0);
    fm1_led_key(BTN_REC, rec_lit() != 0);
    if (rec_lit() != rec.lit) { rec.lit = rec_lit(); ui.dirty |= D_HEAD; }   /* (blinking in rehearsal) */
    fm1_led_key(BTN_GLO, seq_click != 0);
    fm1_led_key(BTN_OCT_DOWN, ui.mode == M_PRJ ? prj.ask != 0 || (prj.sel != PRJ_AUTO && prj.used[prj.sel])
                              : ui.mode == M_RIF ? undo.n != 0 : ui.oct < 2u);
    fm1_led_key(BTN_OCT_UP, ui.mode == M_PRJ ? 1 : ui.mode == M_RIF ? 1 : ui.oct > 2u);
    fm1_led_key(BTN_SAVE, ui.mode == M_PRJ);
    fm1_led_key(BTN_EDIT, ui.mode == M_EDT || ui.mode == M_RIF);
    for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), keys >> k & 1u);

    /* the playhead: redraw the lanes when it moves a step; page along when it leaves the view */
    /* playing, as in Music 2000: the playhead crosses the first bar of the view; when it reaches the bar's end the
     * page slides one bar on (quickly, in a few frames) and the playhead starts again from the left. Between two
     * slides only the first bar's column is redrawn. A jump (the loop, the song's end) moves the page at once. */
    pos = seq_play ? seq_step : ~0u;
    if (seq_play) {
        uint32_t span = COLS * STEPS_PER_BAR, page = pos / STEPS_PER_BAR * STEPS_PER_BAR, v = ui.vstep;
        if (page > SONG_BARS * STEPS_PER_BAR - span) page = SONG_BARS * STEPS_PER_BAR - span;   /* (the song's end) */
        if (!ui.scrolling) {                    /* the page from where it starts */
            ui.scrolling = 1;
            ui.vstep = ui.vtarget = page;
            sl_arr.on = 0;
            ui.dirty |= D_RULER | D_LANES;
        } else if (page != ui.vtarget) {
            if (page == ui.vtarget + STEPS_PER_BAR) slide_start(&sl_arr, ui.vtarget, page);   /* the next bar: slide */
            else { sl_arr.on = 0; ui.vstep = page; ui.dirty |= D_RULER | D_LANES; }           /* a jump */
            ui.vtarget = page;
        }
        if (slide_step(&sl_arr, &v)) { ui.vstep = v; ui.dirty |= D_RULER | D_LANES; }
    } else if (ui.scrolling) {                  /* stopped: the page stays, on a bar */
        ui.left = ui.vtarget / STEPS_PER_BAR;
        ui.scrolling = 0;
        sl_arr.on = 0;
        ui.dirty |= D_RULER | D_LANES;
    }
    if (ui.trk != ui.marq_trk || song.track[ui.trk].inst != ui.marq_inst) {   /* the name scrolls from its start again */
        ui.marq_trk = ui.trk;
        ui.marq_inst = song.track[ui.trk].inst;
        ui.marq_t0 = fm1_ms;
    }
    if (ui.mode == M_ARR && text_w(&FONT_S, sound_name(ui.trk)) > GX - 7 && fm1_ms - ui.marq_drawn >= 60u) {
        ui.marq_drawn = fm1_ms;
        ui.dirty |= D_LABEL;
    }
    if (ui.mode == M_RIF) {                     /* RIF: the same, within the opened riff, a page a bar (or the view) */
        uint32_t rp = rif_pos();
        if (rp != ~0u) {
            const song_riff_t *f = &song.riff[rif.trk][rif.r - 1u];
            uint32_t pg = RIF_STEPS < STEPS_PER_BAR ? RIF_STEPS : STEPS_PER_BAR, l = rp / pg * pg, v = rif.left;
            uint32_t maxl = f->steps > RIF_STEPS ? (f->steps - RIF_STEPS + pg - 1u) / pg * pg : 0u;
            if (l > maxl) l = maxl;
            if (l != rif.target) {
                if (l == rif.target + pg) slide_start(&sl_rif, rif.target, l);
                else { sl_rif.on = 0; rif.left = l; }
                rif.target = l;
            }
            if (slide_step(&sl_rif, &v)) rif.left = v;
        } else {
            sl_rif.on = 0;
            rif.target = rif.left;
        }
        if (rp != ui.drawn_pos || sl_rif.on) { ui.drawn_pos = rp; ui.dirty |= D_LANES; }
    } else if (pos != ui.drawn_pos) {
        ui.drawn_pos = pos;
        if (!(ui.dirty & D_LANES)) ui.dirty |= D_PH;   /* only the playhead moved: its column */
    }
    autosave();
    if (splash.on && splash.until && (int32_t)(splash.until - fm1_ms) <= 0) {   /* a splash's time is up */
        splash.on = 0;
        splash.until = 0;
        ui.dirty = D_ALL;
    }
    if (splash.on) return;                      /* (the splash stays on top: nothing drawn under it) */
    if (ui.msg_until && (int32_t)(ui.msg_until - fm1_ms) <= 0) { ui.msg_until = 0; ui.dirty |= D_FOOT; }
    {   /* the header's context: on RIF and EDT the riff ("R03") */
        static char ctx[4];
        ctx[0] = 'R'; put_num(ctx + 1, rif.r, 2); ctx[3] = 0;
        head_ctx = ui.mode == M_RIF || ui.mode == M_EDT ? ctx : 0;
    }
    if (ui.mode == M_EDT) {
        if (ui.dirty & D_HEAD) draw_head("EDT");
        if (ui.dirty & (D_RULER | D_LANES)) draw_edt();
        if (ui.dirty & (D_FOOT | D_RULER | D_LANES)) draw_edt_foot();
    } else if (ui.mode == M_RIF) {
        if (ui.dirty & D_HEAD) draw_head("RIF");
        if (ui.dirty & (D_RULER | D_LANES)) draw_rif();
        if (ui.dirty & (D_FOOT | D_RULER | D_LANES)) draw_rif_foot();
    } else if (ui.mode == M_PRJ) {
        if (ui.dirty & D_HEAD) draw_head("PRJ");
        if (ui.dirty & (D_RULER | D_LANES)) draw_prj();
        else if (ui.dirty & D_FOOT) draw_prj_foot();
    } else {
        if (ui.dirty & D_HEAD) draw_head("ARR");
        if (ui.dirty & D_RULER) draw_ruler();
        if (ui.dirty & D_LANES) { for (k = 0; k < SONG_TRACKS; k++) draw_lane(k, pos); draw_lanes_end(); }
        else if (ui.dirty & D_PH) {             /* only the playhead moved: the column it moves in */
            int32_t x0 = xstep(ui.vtarget) - 1, w = BW + 4;
            if (!ui.scrolling || pos == ~0u || pos / STEPS_PER_BAR * STEPS_PER_BAR != ui.vtarget)
                for (k = 0; k < SONG_TRACKS; k++) draw_lane(k, pos);
            else for (k = 0; k < SONG_TRACKS; k++) { ph_only = 1; draw_lane_cols(k, pos, x0, w); }
        }
        if ((ui.dirty & D_LABEL) && !(ui.dirty & D_LANES)) { ph_only = 1; draw_lane_cols(ui.trk, pos, 0, GX); }
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

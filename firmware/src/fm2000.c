/* SPDX-License-Identifier: GPL-3.0-only */
/* FM2000: a Music 2000-style song maker for the M-VAVE FM-1. Four tracks, a pool of riffs per track, and the
 * song as a grid of bars you place riffs into.
 *
 * This is the first prototype: the song model (song.h), a test song, a simple four-voice-per-track synth and the
 * ARRANGE screen, to try the model in the simulator (web/) and on the FM-1.
 *
 *   SELECT       the bar under the cursor (clockwise: down the song)
 *   ALGORITHM    the track
 *   PRESETS      the riff that starts at the cursor (0 = none)
 *   KNOB 1       the tempo
 *   PLAY         play the song from the cursor's bar / stop
 *   keys         play the track's instrument; OCT- OCT+ the octave
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

/* ================================================================== the sound (audio interrupt) */
#define NVOICE 12u
enum { K_SQUARE, K_SAW, K_TRI, K_KICK, K_SNARE, K_HAT, K_OPENHAT, K_TOM };
#define HELD 0xFFFFu                            /* a voice the keys hold (not the sequencer) */

typedef struct {
    uint32_t ph, inc, age;
    int32_t amp, peak;                          /* Q15: the level now, and while the note is held */
    int32_t lp;                                 /* a one-pole low-pass, for the bass */
    uint16_t left;                              /* steps until the sequencer lets go; HELD = a key */
    uint8_t on, trk, note, kind;
} voice_t;
static voice_t voice[NVOICE];
static uint32_t ages, noise = 22222u;

/* the instruments, by song.track[].inst (this prototype has one per track) */
typedef struct { const char *name; uint8_t kind; int8_t att, rel; } inst_t;
static const inst_t INST[] = {
    {"LEAD", K_SQUARE, 60, 8},
    {"BASS", K_SAW, 80, 12},
    {"CHRD", K_TRI, 4, 3},
    {"DRUM", K_KICK, 0, 0},
};
#define NINST (sizeof INST / sizeof INST[0])

static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9) as phase steps at 44.1 kHz; lower octaves shift */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t note_inc(uint32_t n) { return n > 127u ? 0 : INC_C9[n % 12u] >> (10u - n / 12u); }

/* the drum on a note: General MIDI where it has one, else by the note's place in the octave */
static uint32_t drum_kind(uint32_t n)
{
    switch (n) {
    case 35: case 36: return K_KICK;
    case 38: case 40: case 39: return K_SNARE;
    case 42: case 44: return K_HAT;
    case 46: return K_OPENHAT;
    default: break;
    }
    switch (n % 12u) {
    case 0: return K_KICK;
    case 2: case 4: return K_SNARE;
    case 6: case 8: return K_HAT;
    case 10: return K_OPENHAT;
    default: return K_TOM;
    }
}

static void voice_on(uint32_t trk, uint32_t note, uint32_t vel, uint32_t steps)
{
    const inst_t *in = &INST[song.track[trk].inst % NINST];
    uint32_t v, pick = 0, best = 0xFFFFFFFFu, kind = in->kind;
    for (v = 0; v < NVOICE; v++) {              /* the same note on the track, else a free voice, else the oldest */
        uint32_t score = voice[v].on && voice[v].trk == trk && voice[v].note == note ? 0u
                       : voice[v].on ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    if (kind == K_KICK) kind = drum_kind(note);
    voice[pick].trk = (uint8_t)trk;
    voice[pick].note = (uint8_t)note;
    voice[pick].kind = (uint8_t)kind;
    voice[pick].age = ++ages;
    voice[pick].left = (uint16_t)steps;
    voice[pick].peak = (int32_t)(vel * 64u);    /* 127 -> 8128 */
    voice[pick].lp = 0;
    switch (kind) {
    case K_KICK: voice[pick].inc = 14000000u; voice[pick].ph = 0; voice[pick].amp = voice[pick].peak * 2; break;
    case K_SNARE: voice[pick].inc = note_inc(62); voice[pick].amp = voice[pick].peak; break;
    case K_HAT: case K_OPENHAT: voice[pick].amp = voice[pick].peak * 3 / 4; break;
    case K_TOM: voice[pick].inc = note_inc(note + 12u); voice[pick].amp = voice[pick].peak * 3 / 2; break;
    default: voice[pick].inc = note_inc(note); break;
    }
    voice[pick].on = 1;
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
    if (q_w - q_r >= 32u) return;               /* full: drop (never happens at key speed) */
    q[i].what = (uint8_t)what; q[i].trk = (uint8_t)trk; q[i].note = (uint8_t)note; q[i].vel = (uint8_t)vel;
    RING_PUBLISH();
    q_w++;
}

/* the sequencer: an absolute step (bar * 16 + step) and the phase into it (2^32 = one step) */
static volatile uint32_t seq_play, seq_from, seq_step, seq_bars;
static uint32_t seq_ph, seq_was;
#define STEP_K_Q8 166214u                       /* 2^32 steps x 4 per beat / (600 x 44100) per frame per 0.1 BPM, x 256 */

static void seq_note(void *ctx, uint32_t trk, const song_ev_t *e) { (void)ctx; voice_on(trk, e->note, e->vel, e->len); }

static void seq_tick(void)                      /* a new step: let go of what ends, start what begins */
{
    uint32_t v, t;
    for (v = 0; v < NVOICE; v++)
        if (voice[v].on && voice[v].left != HELD && voice[v].left && --voice[v].left == 0)
            voice[v].on = 0;
    for (t = 0; t < SONG_TRACKS; t++)
        song_notes_at(&song, t, seq_step, seq_note, 0);
}

static int32_t render(voice_t *v)               /* one frame of one voice, Q15 */
{
    int32_t s;
    noise = noise * 1664525u + 1013904223u;
    switch (v->kind) {
    case K_SQUARE: v->ph += v->inc; s = (int32_t)v->ph < 0 ? 12000 : -12000; break;
    case K_SAW:
        v->ph += v->inc;
        s = ((int32_t)v->ph >> 17);             /* -16384..16383 */
        v->lp += ((s - v->lp) * 9) >> 5;        /* soften it */
        s = v->lp;
        break;
    case K_TRI: { int32_t x = (int32_t)v->ph >> 16; v->ph += v->inc; s = (x < 0 ? -x : x) - 16384; break; }
    case K_KICK: {                              /* a sine that drops */
        int32_t x = (int32_t)v->ph >> 16, ax = x < 0 ? -x : x;
        v->ph += v->inc;
        s = (x * (32768 - ax)) >> 13;
        break;
    }
    case K_SNARE: { int32_t x = (int32_t)v->ph >> 16, ax = x < 0 ? -x : x; v->ph += v->inc;
                    s = (((x * (32768 - ax)) >> 14) + ((int32_t)noise >> 17)); break; }
    case K_HAT: case K_OPENHAT: { int32_t n = (int32_t)noise >> 16; s = n - v->lp; v->lp = n; break; }
    default: { int32_t x = (int32_t)v->ph >> 16, ax = x < 0 ? -x : x; v->ph += v->inc; s = (x * (32768 - ax)) >> 13; break; }
    }
    return (s * v->amp) >> 15;
}

static void envelope(voice_t *v)                /* every 16 frames */
{
    const inst_t *in = &INST[song.track[v->trk].inst % NINST];
    switch (v->kind) {
    case K_KICK: v->amp -= v->amp >> 6; v->inc -= v->inc >> 5; if (v->inc < 4500000u) v->inc = 4500000u; break;
    case K_SNARE: v->amp -= v->amp >> 5; break;
    case K_HAT: v->amp -= v->amp >> 3; break;
    case K_OPENHAT: v->amp -= v->amp >> (v->on ? 6 : 4); break;
    case K_TOM: v->amp -= v->amp >> 6; v->inc -= v->inc >> 9; break;
    default:
        if (v->on) {                            /* attack to the peak, a little decay for the bass */
            int32_t target = v->kind == K_SAW ? v->peak * 3 / 4 : v->peak;
            if (v->amp < v->peak) v->amp += (int32_t)in->att * 16;
            if (v->kind == K_SAW && v->amp > target) v->amp -= (v->amp - target) >> 4;
            if (v->amp > v->peak) v->amp = v->peak;
        } else
            v->amp -= (v->amp >> 4) + in->rel;
        break;
    }
    if (v->amp < 8) { v->amp = 0; if (v->kind >= K_KICK) v->on = 0; }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, k;
    while (q_r != q_w) {                        /* what the main loop asked for */
        uint32_t j = q_r % 32u;
        if (q[j].what == Q_ON) voice_on(q[j].trk, q[j].note, q[j].vel, HELD);
        else if (q[j].what == Q_OFF) voice_off(q[j].trk, q[j].note, 1);
        else for (k = 0; k < NVOICE; k++) voice[k].on = 0;
        q_r++;
    }
    for (i = 0; i < n; i++) {
        if (seq_play != seq_was) {              /* started or stopped */
            seq_was = seq_play;
            if (seq_play) { seq_step = seq_from * STEPS_PER_BAR; seq_ph = 0; seq_tick(); }
            else for (k = 0; k < NVOICE; k++) if (voice[k].left != HELD) voice[k].on = 0;
        } else if (seq_play) {
            uint32_t p = seq_ph;
            seq_ph += (song.bpm10 * STEP_K_Q8) >> 8;
            if (seq_ph < p) {                   /* the next step */
                uint32_t st = seq_step + 1u, end = seq_bars * STEPS_PER_BAR;
                if (song.loop_end > song.loop_start && st >= song.loop_end * STEPS_PER_BAR)
                    st = song.loop_start * STEPS_PER_BAR;
                else if (st >= end)
                    st = 0;                     /* the song's end: round again */
                seq_step = st;
                seq_tick();
            }
        }
        if (!(i & 15u))
            for (k = 0; k < NVOICE; k++)
                if (voice[k].amp || voice[k].on) envelope(&voice[k]);
        for (k = 0; k < NVOICE; k++) {
            voice_t *v = &voice[k];
            int32_t s;
            const song_track_t *t;
            if (!v->amp) continue;
            t = &song.track[v->trk];
            s = (render(v) * t->vol) >> 7;
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
    uint32_t i;
    song_init(&song);
    song.name[0] = 'D'; song.name[1] = 'E'; song.name[2] = 'M'; song.name[3] = 'O';
    song.bpm10 = 1180;
    song.track[0].pan = 80; song.track[2].pan = 48; song.track[2].vol = 80; song.track[3].vol = 120;
    /* LEAD: two 2-bar phrases over Am F C G, and an ending */
    NOTES(0, 1, 32, "HOOK", 0, 3, 76, 110, 4, 2, 74, 90, 6, 2, 72, 100, 8, 6, 69, 110, 16, 3, 72, 100, 20, 2, 74, 90,
          22, 2, 76, 100, 24, 8, 77, 110);
    NOTES(0, 2, 32, "ANSWR", 0, 3, 79, 110, 4, 2, 77, 90, 6, 2, 76, 100, 8, 4, 72, 100, 12, 4, 74, 90, 16, 6, 74, 110,
          24, 2, 71, 90, 26, 6, 67, 100);
    NOTES(0, 3, 32, "END", 0, 4, 76, 110, 4, 4, 74, 100, 8, 4, 71, 100, 12, 4, 74, 90, 16, 16, 69, 120);
    /* BASS: Am F C G in eighths, and a busier version */
    NOTES(1, 1, 64, "ROOTS", 0, 2, 45, 120, 2, 2, 45, 80, 4, 2, 57, 100, 6, 2, 45, 80, 8, 2, 45, 120, 10, 2, 45, 80,
          12, 2, 57, 100, 14, 2, 45, 80, 16, 2, 41, 120, 18, 2, 41, 80, 20, 2, 53, 100, 22, 2, 41, 80, 24, 2, 41, 120,
          26, 2, 41, 80, 28, 2, 53, 100, 30, 2, 41, 80, 32, 2, 48, 120, 34, 2, 48, 80, 36, 2, 60, 100, 38, 2, 48, 80,
          40, 2, 48, 120, 42, 2, 48, 80, 44, 2, 60, 100, 46, 2, 48, 80, 48, 2, 43, 120, 50, 2, 43, 80, 52, 2, 55, 100,
          54, 2, 43, 80, 56, 2, 43, 120, 58, 2, 43, 80, 60, 2, 55, 100, 62, 2, 47, 90);
    riff_copy(&song, 1, 1, 2);
    for (i = 0; i < 4u; i++) riff_note_set(&song, 1, 2, i * 16u + 15u, 1, (uint32_t[]){52, 48, 55, 50}[i], 110);
    song.riff[1][1].name[0] = 'B'; song.riff[1][1].name[1] = 'U'; song.riff[1][1].name[2] = 'S';
    song.riff[1][1].name[3] = 'Y'; song.riff[1][1].name[4] = 0;
    /* CHRD: four bars of chords */
    NOTES(2, 1, 64, "AMFCG", 0, 15, 57, 90, 0, 15, 60, 90, 0, 15, 64, 90, 16, 15, 57, 90, 16, 15, 60, 90,
          16, 15, 65, 90, 32, 15, 55, 90, 32, 15, 60, 90, 32, 15, 64, 90, 48, 15, 55, 90, 48, 15, 59, 90, 48, 15, 62, 90);
    /* DRUM: a beat, a fill, the intro's hats */
    NOTES(3, 1, 16, "BEAT", 0, 1, 36, 127, 2, 1, 42, 70, 4, 1, 38, 120, 6, 1, 42, 70, 8, 1, 36, 110, 10, 1, 36, 90,
          10, 1, 42, 70, 12, 1, 38, 120, 14, 1, 42, 70, 15, 1, 46, 80);
    NOTES(3, 2, 16, "FILL", 0, 1, 36, 127, 4, 1, 38, 110, 8, 1, 36, 110, 10, 1, 38, 90, 11, 1, 38, 100, 12, 1, 45, 110,
          13, 1, 43, 110, 14, 1, 41, 120, 15, 1, 38, 127);
    NOTES(3, 3, 16, "HATS", 0, 1, 42, 90, 2, 1, 42, 60, 4, 1, 42, 90, 6, 1, 42, 60, 8, 1, 42, 90, 10, 1, 42, 60,
          12, 1, 42, 90, 14, 1, 46, 70);
    for (i = 0; i < sizeof PLACE / sizeof PLACE[0]; i++)
        song_place(&song, PLACE[i][0], PLACE[i][1], PLACE[i][2]);
}

/* ================================================================== the screen: ARRANGE */
#define C_ACC RGB(180, 140, 255)                /* lilac */
#define C_TXT RGB(220, 220, 220)
#define C_MID RGB(120, 120, 130)
#define C_LOW RGB(48, 48, 56)
#define C_BG RGB(14, 14, 20)
static const uint16_t RIFF_C[8] = {RGB(180, 140, 255), RGB(90, 200, 190), RGB(240, 190, 80), RGB(240, 120, 150),
                                   RGB(130, 210, 110), RGB(110, 170, 250), RGB(250, 150, 90), RGB(190, 190, 190)};
static const uint16_t RIFF_D[8] = {RGB(70, 55, 105), RGB(35, 80, 76), RGB(96, 76, 32), RGB(96, 48, 60),
                                   RGB(52, 84, 44), RGB(44, 68, 100), RGB(100, 60, 36), RGB(76, 76, 76)};

#define GX 32                                   /* the grid: bar numbers left of it, 4 columns */
#define CW 52
#define GY 38
#define RH 16
#define ROWS 11

static struct {
    uint32_t bar, trk, top, oct;                /* the cursor, the first bar shown, the keys' octave (0..4) */
    uint32_t keys_prev;
    uint8_t held[27];
    uint32_t drawn_head;                        /* the playhead's bar drawn (or ~0) */
    uint32_t dirty;                             /* D_ bits */
} ui;
enum { D_HEAD = 1, D_COLS = 2, D_GRID = 4, D_FOOT = 8, D_ALL = 15 };

static void put_num(char *p, uint32_t v, uint32_t digits)    /* v as `digits` digits, 0-padded */
{
    while (digits--) { p[digits] = (char)('0' + v % 10u); v /= 10u; }
}
static void riff_label(char *s, const song_riff_t *f)
{
    uint32_t i;
    for (i = 0; i < RIFF_NAME && f->name[i]; i++) s[i] = f->name[i];
    s[i] = 0;
}

static void draw_head(void)
{
    char bpm[12] = "000.0 BPM";
    put_num(bpm, song.bpm10 / 10u, 3);
    bpm[4] = (char)('0' + song.bpm10 % 10u);
    if (bpm[0] == '0') bpm[0] = ' ';
    cv_begin(240, 20, C_BG);
    cv_text(6, 2, &FONT_S, "FM2000", C_ACC);
    cv_text(64, 2, &FONT_S, "ARRANGE", C_MID);
    cv_text(240 - 6 - text_w(&FONT_S, bpm), 2, &FONT_S, bpm, C_TXT);
    if (seq_play) {                             /* a play triangle */
        int32_t y;
        for (y = 0; y < 10; y++) cv_rect(136, 5 + y, y < 5 ? y + 1 : 10 - y, 1, RGB(110, 230, 140));
    }
    cv_blit(0, 0);
}

static void draw_cols(void)
{
    uint32_t t;
    cv_begin(240, 18, C_BLACK);
    cv_text(4, 1, &FONT_S, "BAR", C_MID);
    for (t = 0; t < SONG_TRACKS; t++) {
        const char *n = INST[song.track[t].inst % NINST].name;
        int32_t x = GX + (int32_t)t * CW, sel = t == ui.trk;
        if (sel) cv_rect(x + 1, 0, CW - 2, 17, C_LOW);
        cv_text(x + (CW - text_w(&FONT_S, n)) / 2, 1, &FONT_S, n,
                song.track[t].flags & SONG_MUTE ? C_LOW : sel ? C_WHITE : C_TXT);
    }
    cv_blit(0, 20);
}

static void draw_rows(uint32_t r0, uint32_t nr, uint32_t head)   /* screen rows r0 .. r0 + nr - 1 */
{
    uint32_t r, t;
    cv_begin(240, nr * RH, C_BLACK);
    for (r = 0; r < nr; r++) {
        uint32_t bar = ui.top + r0 + r;
        int32_t y = (int32_t)(r * RH);
        char num[4] = "000";
        if (bar >= SONG_BARS) continue;
        put_num(num, bar + 1u, 3);
        if (bar == head) cv_rect(0, y, GX - 2, RH, RGB(40, 90, 55));
        cv_text(4, y, &FONT_S, num, bar == head ? RGB(140, 255, 170) : (bar & 3u) ? C_LOW : C_MID);
        for (t = 0; t < SONG_TRACKS; t++) {
            int32_t x = GX + (int32_t)t * CW;
            uint32_t b0 = 0, rf = song_at(&song, bar, t, &b0);
            if (rf) {
                const song_riff_t *f = &song.riff[t][rf - 1u];
                int last = bar + 1u >= b0 + riff_bars(f) || song.cell[bar + 1u < SONG_BARS ? bar + 1u : bar][t];
                if (b0 == bar) {                /* the riff starts: its block, its name */
                    char s[RIFF_NAME + 1];
                    cv_rect(x + 2, y + 1, CW - 4, last ? RH - 2 : RH - 1, RIFF_C[f->color & 7u]);
                    riff_label(s, f);
                    cv_text(x + 5, y, &FONT_S, s, C_BLACK);
                } else                          /* it plays on */
                    cv_rect(x + 2, y, CW - 4, last ? RH - 1 : RH, RIFF_D[f->color & 7u]);
            } else
                cv_rect(x + CW / 2 - 1, y + RH / 2 - 1, 2, 2, (bar & 3u) ? RGB(30, 30, 36) : C_LOW);
            if (bar == ui.bar && t == ui.trk) {   /* the cursor: a frame */
                cv_rect(x, y, CW, 1, C_WHITE); cv_rect(x, y + RH - 1, CW, 1, C_WHITE);
                cv_rect(x, y, 1, RH, C_WHITE); cv_rect(x + CW - 1, y, 1, RH, C_WHITE);
            }
        }
        if (bar == head) cv_rect(GX, y, 2, RH, RGB(110, 230, 140));
    }
    cv_blit(0, GY + r0 * RH);
}

static void draw_grid(uint32_t head)
{
    draw_rows(0, 4, head);                      /* in strips: the canvas holds 240 x 64 */
    draw_rows(4, 4, head);
    draw_rows(8, ROWS - 8u, head);
}

static void draw_foot(void)
{
    char s[32];
    uint32_t b0 = 0, rf = song_at(&song, ui.bar, ui.trk, &b0), n = 0;
    cv_begin(240, 20, C_BG);
    s[n++] = 'T'; s[n++] = (char)('1' + ui.trk); s[n++] = ' ';
    if (rf) {
        const song_riff_t *f = &song.riff[ui.trk][rf - 1u];
        s[n++] = 'R'; put_num(s + n, rf, 2); n += 2;
        s[n++] = ' '; s[n++] = ' ';
        put_num(s + n, f->steps, 2); n += 2;
        s[n++] = 'S'; s[n++] = 'T'; s[n++] = ' '; s[n++] = ' ';
        put_num(s + n, f->count, 3); n += 3;
        s[n++] = 'N';
        if (b0 != ui.bar) { s[n++] = ' '; s[n++] = '+'; s[n++] = (char)('0' + (ui.bar - b0)); }
    } else {
        s[n++] = '-'; s[n++] = '-';
    }
    s[n] = 0;
    cv_text(6, 2, &FONT_S, s, C_TXT);
    {
        char len[12] = "LEN 000";
        put_num(len + 4, song_bars(&song), 3);
        cv_text(240 - 6 - text_w(&FONT_S, len), 2, &FONT_S, len, C_MID);
    }
    cv_blit(0, 220);
}

/* ================================================================== the main loop */
static void scroll_to(uint32_t bar)
{
    if (bar < ui.top) ui.top = bar;
    else if (bar >= ui.top + ROWS) ui.top = bar - ROWS + 1u;
}

/* the next riff id for PRESETS: d = +1 / -1 through 0 and the riffs the track has */
static uint32_t next_riff(uint32_t t, uint32_t r, int32_t d)
{
    uint32_t k;
    for (k = 0; k <= SONG_RIFFS; k++) {
        r = d > 0 ? (r + 1u) % (SONG_RIFFS + 1u) : (r + SONG_RIFFS) % (SONG_RIFFS + 1u);
        if (!r || song.riff[t][r - 1u].steps) return r;
    }
    return 0;
}

static void app_init(void)
{
    test_song();
    seq_bars = song_bars(&song);
    ui.oct = 2;
    ui.drawn_head = ~0u;
    ui.dirty = D_ALL;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

static void app_step(void)
{
    uint32_t b, rel, keys, down, up, k, head;
    int32_t d;
    uint8_t st, d1, d2;

    /* the encoders */
    if ((d = fm1_enc_take(ENC_SELECT)) != 0) {
        int32_t nb = (int32_t)ui.bar + d;
        ui.bar = nb < 0 ? 0u : nb >= (int32_t)SONG_BARS ? SONG_BARS - 1u : (uint32_t)nb;
        scroll_to(ui.bar);
        ui.dirty |= D_GRID | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0) {
        int32_t nt = (int32_t)ui.trk + d;
        ui.trk = nt < 0 ? 0u : nt >= (int32_t)SONG_TRACKS ? SONG_TRACKS - 1u : (uint32_t)nt;
        ui.dirty |= D_COLS | D_GRID | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
        uint32_t r = song.cell[ui.bar][ui.trk];
        while (d > 0) { r = next_riff(ui.trk, r, 1); d--; }
        while (d < 0) { r = next_riff(ui.trk, r, -1); d++; }
        song_place(&song, ui.bar, ui.trk, r);
        seq_bars = song_bars(&song);
        ui.dirty |= D_GRID | D_FOOT;
    }
    if ((d = fm1_enc_take(ENC_KNOB1)) != 0) {
        int32_t bpm = (int32_t)song.bpm10 + d * 10;
        song.bpm10 = (uint16_t)(bpm < 600 ? 600 : bpm > 3000 ? 3000 : bpm);
        ui.dirty |= D_HEAD;
    }

    /* the buttons */
    b = fm1_input_edges(&rel);
    if (b & (1u << BTN_PLAY)) {
        if (!seq_play) { seq_bars = song_bars(&song); seq_from = ui.bar < seq_bars ? ui.bar : 0u; }
        seq_play = !seq_play && seq_bars;
        ui.dirty |= D_HEAD | D_GRID;
    }
    if (b & (1u << BTN_OCT_DOWN) && ui.oct > 0u) ui.oct--;
    if (b & (1u << BTN_OCT_UP) && ui.oct < 4u) ui.oct++;

    /* the keys: the track's instrument */
    keys = fm1_in.notes;
    down = (keys & ~ui.keys_prev) | fm1_input_note_edges();
    up = ui.keys_prev & ~keys;
    ui.keys_prev = keys;
    for (k = 0; k < 27u; k++) {
        if (up >> k & 1u) { ask(Q_OFF, ui.trk, ui.held[k], 0); midi_send(0x80u | ui.trk, ui.held[k], 0); }
        if (down >> k & 1u) {
            ui.held[k] = (uint8_t)(29u + 12u * ui.oct + k);   /* oct 2: key 0 = F3 */
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

    /* the lights */
    fm1_led_key(BTN_PLAY, seq_play != 0);
    fm1_led_key(BTN_OCT_DOWN, ui.oct < 2u);
    fm1_led_key(BTN_OCT_UP, ui.oct > 2u);
    for (k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), keys >> k & 1u);

    /* the screen: what changed */
    head = seq_play ? seq_step / STEPS_PER_BAR : ~0u;
    if (head != ui.drawn_head) {
        if (head != ~0u && (head < ui.top || head >= ui.top + ROWS)) {   /* follow the playhead */
            ui.top = head > 1u ? head - 1u : 0u;
            if (ui.top > SONG_BARS - ROWS) ui.top = SONG_BARS - ROWS;
        }
        ui.drawn_head = head;
        ui.dirty |= D_GRID;
    }
    if (ui.dirty & D_HEAD) draw_head();
    if (ui.dirty & D_COLS) draw_cols();
    if (ui.dirty & D_GRID) draw_grid(head);
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

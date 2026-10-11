/* SPDX-License-Identifier: GPL-3.0-only */
/* FM2000's song: the data model, nothing else (no screen, no sound, no flash). Plain C, integers only, so the
 * same file runs on the FM-1, in tests/test_song.c and in the browser.
 *
 * The idea, after Music 2000: every track has its own pool of riffs (short phrases, 1..64 steps of 1/16), and the
 * song is a grid of bars x tracks; a cell either starts a riff or is empty. A riff that starts in a bar plays on
 * through as many bars as it is long (a 64-step riff covers 4 bars) unless another riff starts on the same track
 * first. A bar is 16 steps.
 *
 *   song.cell[bar][track]   0 = nothing starts here, 1..SONG_RIFFS = this riff starts here
 *   song.riff[track][r-1]   its length in steps, colour, name, and where its notes are in song.ev
 *   song.ev[]               every note of every riff, in one pool, kept in order (track, riff, start, note):
 *                           a riff's notes are song.ev[first .. first + count)
 *
 * Notes are polyphonic (chords are notes with the same start) and have a length in steps, so a riff is a small
 * piano roll, not a step pattern. The pool is shared: one long busy riff and many short ones fit the same space.
 *
 * Sizes: 4 tracks x 32 riffs, 128 bars, 4096 notes: about 18.5 KB in RAM, a few KB packed (song_pack). */
#ifndef FM2000_SONG_H
#define FM2000_SONG_H
#include <stdint.h>

#define SONG_TRACKS 4u
#define SONG_RIFFS 32u                  /* per track: ids 1..32, 0 = none */
#define RIFF_MAX_STEPS 64u              /* 4 bars of 1/16 */
#define STEPS_PER_BAR 16u
#define SONG_BARS 128u
#define SONG_EVENTS 4096u               /* notes in all riffs together */
#define SONG_NAME 8u
#define RIFF_NAME 6u

typedef struct {
    uint8_t start;                      /* 0..63: the step it starts on */
    uint8_t len;                        /* 1..64 steps (it may run past the riff's end: it is cut there) */
    uint8_t note;                       /* MIDI 0..127 */
    uint8_t vel;                        /* 1..127 */
} song_ev_t;

typedef struct {
    uint16_t first, count;              /* its notes: ev[first .. first + count) (first is kept by song_reindex) */
    uint8_t steps;                      /* 1..64; 0 = the slot is unused */
    uint8_t color;                      /* 0..7, for the grid */
    char name[RIFF_NAME];               /* not 0-terminated when full */
} song_riff_t;

typedef struct {
    uint8_t inst;                       /* the instrument (engine + patch), meaning set by the app */
    uint8_t vol;                        /* 0..127 */
    uint8_t pan;                        /* 0..127, 64 = centre */
    uint8_t flags;                      /* SONG_MUTE, SONG_SOLO */
} song_track_t;
#define SONG_MUTE 1u
#define SONG_SOLO 2u

typedef struct {
    char name[SONG_NAME];
    uint16_t bpm10;                     /* tempo x 10: 600..3000 (60.0..300.0 BPM) */
    uint8_t swing;                      /* 0..100 % (50 would be a full triplet feel; 0 = straight) */
    uint8_t loop_start, loop_end;       /* bars, end exclusive; loop_end 0 = loop off */
    song_track_t track[SONG_TRACKS];
    song_riff_t riff[SONG_TRACKS][SONG_RIFFS];
    uint8_t cell[SONG_BARS][SONG_TRACKS];
    uint16_t nev;                       /* notes in use */
    song_ev_t ev[SONG_EVENTS];
} song_t;

/* ------------------------------------------------------------------ helpers */
static void song_bytes_set(void *d, uint8_t v, uint32_t n) { uint8_t *p = d; while (n--) *p++ = v; }
static void song_bytes_move(void *d, const void *s, uint32_t n)   /* overlapping is fine (no libc memmove here) */
{
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
}
static int song_ok_track(uint32_t t) { return t < SONG_TRACKS; }
static int song_ok_riff(uint32_t r) { return r >= 1u && r <= SONG_RIFFS; }

/* the riff r (1..32) of track t, or 0 */
static song_riff_t *song_riff(song_t *s, uint32_t t, uint32_t r)
{
    return song_ok_track(t) && song_ok_riff(r) ? &s->riff[t][r - 1u] : 0;
}

/* first = the notes of every riff before it: call after anything that changes a count */
static void song_reindex(song_t *s)
{
    uint32_t t, r, at = 0;
    for (t = 0; t < SONG_TRACKS; t++)
        for (r = 0; r < SONG_RIFFS; r++) {
            s->riff[t][r].first = (uint16_t)at;
            at += s->riff[t][r].count;
        }
}

/* ------------------------------------------------------------------ the song */
static void song_init(song_t *s)
{
    uint32_t t;
    song_bytes_set(s, 0, sizeof *s);
    s->name[0] = 'S'; s->name[1] = 'O'; s->name[2] = 'N'; s->name[3] = 'G';
    s->bpm10 = 1200;
    for (t = 0; t < SONG_TRACKS; t++) {
        s->track[t].inst = (uint8_t)t;
        s->track[t].vol = 100;
        s->track[t].pan = 64;
    }
}

/* ------------------------------------------------------------------ riffs */
/* make riff r of track t usable (an empty riff of `steps`); a riff already there keeps its notes */
static song_riff_t *riff_new(song_t *s, uint32_t t, uint32_t r, uint32_t steps)
{
    song_riff_t *f = song_riff(s, t, r);
    if (!f || steps < 1u || steps > RIFF_MAX_STEPS) return 0;
    if (!f->steps) {
        f->steps = (uint8_t)steps;
        f->color = (uint8_t)((r - 1u + 3u * t) & 7u);   /* neighbours differ, across tracks too */
        f->name[0] = 'R';
        f->name[1] = (char)('0' + r / 10u);
        f->name[2] = (char)('0' + r % 10u);
        f->name[3] = 0;
    }
    return f;
}

/* the first free riff id of track t, or 0 */
static uint32_t riff_free(const song_t *s, uint32_t t)
{
    uint32_t r;
    if (!song_ok_track(t)) return 0;
    for (r = 1; r <= SONG_RIFFS; r++)
        if (!s->riff[t][r - 1u].steps) return r;
    return 0;
}

/* where in ev a note (start, note) is or would go in riff f: the index, *found = 1 if it is there */
static uint32_t riff_find(const song_t *s, const song_riff_t *f, uint32_t start, uint32_t note, int *found)
{
    uint32_t i = f->first, end = (uint32_t)f->first + f->count;
    *found = 0;
    for (; i < end; i++) {
        const song_ev_t *e = &s->ev[i];
        if (e->start > start || (e->start == start && e->note >= note)) {
            *found = e->start == start && e->note == note;
            break;
        }
    }
    return i;
}

/* put a note into a riff (an existing note on the same step and pitch is replaced): its index, or -1 when the
 * riff does not exist, the values are out of range or the pool is full */
static int32_t riff_note_set(song_t *s, uint32_t t, uint32_t r, uint32_t start, uint32_t len, uint32_t note, uint32_t vel)
{
    song_riff_t *f = song_riff(s, t, r);
    uint32_t i;
    int found;
    if (!f || !f->steps || start >= f->steps || len < 1u || len > RIFF_MAX_STEPS || note > 127u || vel < 1u || vel > 127u)
        return -1;
    i = riff_find(s, f, start, note, &found);
    if (!found) {
        if (s->nev >= SONG_EVENTS) return -1;
        song_bytes_move(&s->ev[i + 1u], &s->ev[i], (s->nev - i) * sizeof(song_ev_t));
        s->nev++;
        f->count++;
        song_reindex(s);
    }
    s->ev[i].start = (uint8_t)start;
    s->ev[i].len = (uint8_t)len;
    s->ev[i].note = (uint8_t)note;
    s->ev[i].vel = (uint8_t)vel;
    return (int32_t)i;
}

/* take the note (start, note) out of a riff: 1 if it was there */
static int riff_note_del(song_t *s, uint32_t t, uint32_t r, uint32_t start, uint32_t note)
{
    song_riff_t *f = song_riff(s, t, r);
    uint32_t i;
    int found;
    if (!f) return 0;
    i = riff_find(s, f, start, note, &found);
    if (!found) return 0;
    song_bytes_move(&s->ev[i], &s->ev[i + 1u], (s->nev - i - 1u) * sizeof(song_ev_t));
    s->nev--;
    f->count--;
    song_reindex(s);
    return 1;
}

/* the notes of a riff removed (the riff stays, with its length) */
static void riff_clear(song_t *s, uint32_t t, uint32_t r)
{
    song_riff_t *f = song_riff(s, t, r);
    if (!f || !f->count) return;
    song_bytes_move(&s->ev[f->first], &s->ev[f->first + f->count],
                    (s->nev - f->first - f->count) * sizeof(song_ev_t));
    s->nev = (uint16_t)(s->nev - f->count);
    f->count = 0;
    song_reindex(s);
}

/* the riff gone: its notes, its slot, and every place it starts in the song */
static void riff_delete(song_t *s, uint32_t t, uint32_t r)
{
    song_riff_t *f = song_riff(s, t, r);
    uint32_t b;
    if (!f) return;
    riff_clear(s, t, r);
    song_bytes_set(f->name, 0, RIFF_NAME);
    f->steps = 0;
    for (b = 0; b < SONG_BARS; b++)
        if (s->cell[b][t] == r) s->cell[b][t] = 0;
}

/* a new length; notes that start past it go, notes that run past it are cut to it */
static int riff_set_steps(song_t *s, uint32_t t, uint32_t r, uint32_t steps)
{
    song_riff_t *f = song_riff(s, t, r);
    uint32_t i;
    if (!f || !f->steps || steps < 1u || steps > RIFF_MAX_STEPS) return -1;
    f->steps = (uint8_t)steps;
    for (i = f->first + f->count; i-- > f->first;) {
        song_ev_t *e = &s->ev[i];
        if (e->start >= steps)
            riff_note_del(s, t, r, e->start, e->note);
        else if (e->start + e->len > steps)
            e->len = (uint8_t)(steps - e->start);
    }
    return 0;
}

/* riff src copied over riff dst of the same track (dst made if it was free): 0, or -1 (no src, pool full) */
static int riff_copy(song_t *s, uint32_t t, uint32_t src, uint32_t dst)
{
    song_riff_t *a = song_riff(s, t, src), *b = song_riff(s, t, dst);
    uint32_t i, n;
    if (!a || !b || !a->steps || src == dst) return -1;
    n = a->count;
    if ((uint32_t)s->nev - b->count + n > SONG_EVENTS) return -1;
    riff_clear(s, t, dst);
    b->steps = a->steps;
    b->color = a->color;
    for (i = 0; i < n; i++) {                   /* a may move as b grows: read it by index each time */
        song_ev_t e = s->ev[a->first + i];
        riff_note_set(s, t, dst, e.start, e.len, e.note, e.vel);
    }
    return 0;
}

/* ------------------------------------------------------------------ the arrangement */
static void song_place(song_t *s, uint32_t bar, uint32_t t, uint32_t r)    /* r = 0: clears the cell */
{
    if (bar < SONG_BARS && song_ok_track(t) && (r == 0u || (song_ok_riff(r) && s->riff[t][r - 1u].steps)))
        s->cell[bar][t] = (uint8_t)r;
}

static uint32_t riff_bars(const song_riff_t *f) { return (f->steps + STEPS_PER_BAR - 1u) / STEPS_PER_BAR; }

/* what track t plays in bar `bar`: the riff (0 = nothing) and the bar it started in */
static uint32_t song_at(const song_t *s, uint32_t bar, uint32_t t, uint32_t *bar0)
{
    uint32_t b = bar, k;
    if (bar >= SONG_BARS || !song_ok_track(t)) return 0;
    for (k = 0; k < RIFF_MAX_STEPS / STEPS_PER_BAR; k++, b--) {   /* a riff covers at most 4 bars */
        uint32_t r = s->cell[b][t];
        if (r) {
            const song_riff_t *f = &s->riff[t][r - 1u];
            if (f->steps && bar < b + riff_bars(f)) {
                if (bar0) *bar0 = b;
                return r;
            }
            return 0;                           /* the riff before ended: silence */
        }
        if (!b) break;
    }
    return 0;
}

/* the song's length in bars: past the last bar anything plays in (a riff cut short by the next one counts as cut) */
static uint32_t song_bars(const song_t *s)
{
    uint32_t b, t;
    for (b = SONG_BARS; b-- > 0;)
        for (t = 0; t < SONG_TRACKS; t++)
            if (song_at(s, b, t, 0)) return b + 1u;
    return 0;
}

/* insert / delete a whole bar (every track) at `bar`: the rest moves down / up; the last bar falls off */
static void song_insert_bar(song_t *s, uint32_t bar)
{
    if (bar >= SONG_BARS) return;
    song_bytes_move(s->cell[bar + 1u], s->cell[bar], (SONG_BARS - bar - 1u) * SONG_TRACKS);
    song_bytes_set(s->cell[bar], 0, SONG_TRACKS);
}
static void song_delete_bar(song_t *s, uint32_t bar)
{
    if (bar >= SONG_BARS) return;
    song_bytes_move(s->cell[bar], s->cell[bar + 1u], (SONG_BARS - bar - 1u) * SONG_TRACKS);
    song_bytes_set(s->cell[SONG_BARS - 1u], 0, SONG_TRACKS);
}

/* ------------------------------------------------------------------ playing */
/* the notes track t starts on absolute step `step` of the song (bar * 16 + step in bar): calls on(ctx, ev) for
 * each; a note's length is cut where its riff ends. Returns how many. Safe to call from the audio interrupt as
 * long as the main loop does not edit the song meanwhile (the app pauses edits or accepts a wrong note then). */
typedef void (*song_note_fn)(void *ctx, uint32_t track, const song_ev_t *ev);
static uint32_t song_notes_at(const song_t *s, uint32_t track, uint32_t step, song_note_fn on, void *ctx)
{
    uint32_t bar = step / STEPS_PER_BAR, bar0 = 0, r, off, i, end, n = 0;
    const song_riff_t *f;
    if (s->track[track].flags & SONG_MUTE) return 0;
    r = song_at(s, bar, track, &bar0);
    if (!r) return 0;
    f = &s->riff[track][r - 1u];
    off = step - bar0 * STEPS_PER_BAR;
    if (off >= f->steps) return 0;
    for (i = f->first, end = (uint32_t)f->first + f->count; i < end; i++) {
        const song_ev_t *e = &s->ev[i];
        if (e->start > off) break;              /* (in order of start) */
        if (e->start == off) {
            song_ev_t c = *e;
            if (c.start + c.len > f->steps) c.len = (uint8_t)(f->steps - c.start);
            on(ctx, track, &c);
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ saving */
/* The packed song, for the flash or a file: little-endian, only what is used.
 *   "F2KS" ver(1) name[8] bpm10(2) swing loop_start loop_end
 *   tracks: inst vol pan flags            x4
 *   riffs:  steps color count(2) name[6]  x4x32  (steps 0: unused, nothing else follows)
 *   cells:  128 x 4
 *   notes:  start len note vel            x nev (riff by riff, as counted above)
 *   crc32 of everything before it (4) */
#define SONG_MAGIC "F2KS"
#define SONG_VERSION 1u
#define SONG_PACK_MAX (4u + 1u + SONG_NAME + 5u + SONG_TRACKS * 4u + SONG_TRACKS * SONG_RIFFS * 10u + \
                       SONG_BARS * SONG_TRACKS + SONG_EVENTS * 4u + 4u)

static uint32_t song_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* the size written, or 0 if it does not fit in max */
static uint32_t song_pack(const song_t *s, uint8_t *out, uint32_t max)
{
    uint32_t t, r, i, n = 0, crc;
#define PUT(v) do { if (n >= max) return 0; out[n++] = (uint8_t)(v); } while (0)
    for (i = 0; i < 4u; i++) PUT(SONG_MAGIC[i]);
    PUT(SONG_VERSION);
    for (i = 0; i < SONG_NAME; i++) PUT(s->name[i]);
    PUT(s->bpm10); PUT(s->bpm10 >> 8); PUT(s->swing); PUT(s->loop_start); PUT(s->loop_end);
    for (t = 0; t < SONG_TRACKS; t++) { PUT(s->track[t].inst); PUT(s->track[t].vol); PUT(s->track[t].pan); PUT(s->track[t].flags); }
    for (t = 0; t < SONG_TRACKS; t++)
        for (r = 0; r < SONG_RIFFS; r++) {
            const song_riff_t *f = &s->riff[t][r];
            PUT(f->steps);
            if (!f->steps) continue;
            PUT(f->color); PUT(f->count); PUT(f->count >> 8);
            for (i = 0; i < RIFF_NAME; i++) PUT(f->name[i]);
        }
    for (i = 0; i < SONG_BARS; i++)
        for (t = 0; t < SONG_TRACKS; t++) PUT(s->cell[i][t]);
    for (i = 0; i < s->nev; i++) { PUT(s->ev[i].start); PUT(s->ev[i].len); PUT(s->ev[i].note); PUT(s->ev[i].vel); }
    crc = song_crc32(out, n);
    PUT(crc); PUT(crc >> 8); PUT(crc >> 16); PUT(crc >> 24);
#undef PUT
    return n;
}

/* 0 = loaded; -1 not a song, -2 broken (CRC), -3 a newer version, -4 inconsistent. On an error s is left empty. */
static int song_unpack(song_t *s, const uint8_t *in, uint32_t n)
{
    uint32_t t, r, i, at = 0, total = 0, crc;
#define GET(v) do { if (at >= n) goto bad; (v) = in[at++]; } while (0)
    if (n < 9u || in[0] != 'F' || in[1] != '2' || in[2] != 'K' || in[3] != 'S') { song_init(s); return -1; }
    if (in[4] > SONG_VERSION) { song_init(s); return -3; }
    song_bytes_set(s, 0, sizeof *s);
    at = 5;
    for (i = 0; i < SONG_NAME; i++) GET(s->name[i]);
    { uint32_t lo, hi; GET(lo); GET(hi); s->bpm10 = (uint16_t)(lo | hi << 8); }
    GET(s->swing); GET(s->loop_start); GET(s->loop_end);
    for (t = 0; t < SONG_TRACKS; t++) { GET(s->track[t].inst); GET(s->track[t].vol); GET(s->track[t].pan); GET(s->track[t].flags); }
    for (t = 0; t < SONG_TRACKS; t++)
        for (r = 0; r < SONG_RIFFS; r++) {
            song_riff_t *f = &s->riff[t][r];
            uint32_t lo, hi;
            GET(f->steps);
            if (!f->steps) continue;
            if (f->steps > RIFF_MAX_STEPS) goto bad;
            GET(f->color); GET(lo); GET(hi);
            f->count = (uint16_t)(lo | hi << 8);
            total += f->count;
            for (i = 0; i < RIFF_NAME; i++) GET(f->name[i]);
        }
    if (total > SONG_EVENTS) goto bad;
    for (i = 0; i < SONG_BARS; i++)
        for (t = 0; t < SONG_TRACKS; t++) {
            GET(s->cell[i][t]);
            if (s->cell[i][t] > SONG_RIFFS) goto bad;
        }
    s->nev = (uint16_t)total;
    for (i = 0; i < total; i++) { GET(s->ev[i].start); GET(s->ev[i].len); GET(s->ev[i].note); GET(s->ev[i].vel); }
    if (at + 4u > n) goto bad;
    crc = (uint32_t)in[at] | (uint32_t)in[at + 1u] << 8 | (uint32_t)in[at + 2u] << 16 | (uint32_t)in[at + 3u] << 24;
    if (crc != song_crc32(in, at)) { song_init(s); return -2; }
    if (s->bpm10 < 600u || s->bpm10 > 3000u) s->bpm10 = 1200;
    song_reindex(s);
    return 0;
#undef GET
bad:
    song_init(s);
    return -4;
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* Tests of the song model (firmware/src/song.h), on a computer: tests/run_tests.sh */
#include <stdio.h>
#include <string.h>
#include "song.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static song_t s, s2;
static uint8_t buf[SONG_PACK_MAX];

typedef struct { uint32_t n; song_ev_t ev[16]; } got_t;
static void collect(void *ctx, uint32_t track, const song_ev_t *e) { got_t *g = ctx; (void)track; if (g->n < 16u) g->ev[g->n++] = *e; }
static got_t at(uint32_t t, uint32_t step) { got_t g = {0}; song_notes_at(&s, t, step, collect, &g); return g; }

/* every riff's notes in order and inside it, first / count consistent with nev */
static int consistent(const song_t *x)
{
    uint32_t t, r, i, sum = 0;
    for (t = 0; t < SONG_TRACKS; t++)
        for (r = 0; r < SONG_RIFFS; r++) {
            const song_riff_t *f = &x->riff[t][r];
            if (f->first != sum) return 0;
            for (i = f->first; i < (uint32_t)f->first + f->count; i++) {
                const song_ev_t *e = &x->ev[i];
                if (e->start >= f->steps || !e->len || !e->vel) return 0;
                if (i > f->first) {
                    const song_ev_t *p = &x->ev[i - 1u];
                    if (p->start > e->start || (p->start == e->start && p->note >= e->note)) return 0;
                }
            }
            sum += f->count;
        }
    return sum == x->nev;
}

static void test_riffs(void)
{
    song_init(&s);
    CHECK(s.bpm10 == 1200 && s.nev == 0);
    CHECK(riff_note_set(&s, 0, 1, 0, 1, 60, 100) < 0);          /* no riff yet */
    CHECK(riff_new(&s, 0, 1, 16) != 0);
    CHECK(riff_new(&s, 0, 0, 16) == 0 && riff_new(&s, 0, 33, 16) == 0 && riff_new(&s, 4, 1, 16) == 0);
    CHECK(riff_new(&s, 0, 2, 65) == 0 && riff_new(&s, 0, 2, 0) == 0);
    CHECK(riff_free(&s, 0) == 2);
    CHECK(riff_note_set(&s, 0, 1, 4, 2, 64, 90) >= 0);
    CHECK(riff_note_set(&s, 0, 1, 0, 1, 60, 100) >= 0);
    CHECK(riff_note_set(&s, 0, 1, 0, 1, 67, 100) >= 0);          /* a chord on step 0 */
    CHECK(riff_note_set(&s, 0, 1, 0, 4, 60, 50) >= 0);           /* replaces, no new note */
    CHECK(riff_note_set(&s, 0, 1, 16, 1, 60, 100) < 0);          /* past its end */
    CHECK(riff_note_set(&s, 0, 1, 0, 1, 128, 100) < 0 && riff_note_set(&s, 0, 1, 0, 0, 60, 100) < 0);
    CHECK(s.nev == 3 && s.riff[0][0].count == 3);
    CHECK(s.ev[0].note == 60 && s.ev[0].len == 4 && s.ev[0].vel == 50 && s.ev[1].note == 67 && s.ev[2].start == 4);
    /* a riff on another track goes after, and moves when track 0 grows */
    riff_new(&s, 2, 5, 32);
    CHECK(riff_note_set(&s, 2, 5, 31, 1, 36, 127) >= 0);
    CHECK(s.riff[2][4].first == 3);
    riff_note_set(&s, 0, 1, 8, 1, 62, 100);
    CHECK(s.riff[2][4].first == 4 && s.ev[4].note == 36);
    CHECK(consistent(&s));
    CHECK(riff_note_del(&s, 0, 1, 8, 62) == 1 && riff_note_del(&s, 0, 1, 8, 62) == 0);
    CHECK(s.nev == 4 && s.riff[2][4].first == 3 && consistent(&s));
    /* length: shorter cuts and drops */
    riff_new(&s, 1, 1, 64);
    riff_note_set(&s, 1, 1, 0, 64, 48, 100);
    riff_note_set(&s, 1, 1, 40, 4, 50, 100);
    CHECK(riff_set_steps(&s, 1, 1, 32) == 0);
    CHECK(s.riff[1][0].count == 1 && s.ev[s.riff[1][0].first].len == 32 && consistent(&s));
    /* copy, clear, delete */
    CHECK(riff_copy(&s, 0, 1, 3) == 0);
    CHECK(s.riff[0][2].steps == 16 && s.riff[0][2].count == 3 && consistent(&s));
    CHECK(riff_copy(&s, 0, 9, 4) < 0);
    riff_clear(&s, 0, 1);
    CHECK(s.riff[0][0].count == 0 && s.riff[0][0].steps == 16 && s.riff[0][2].count == 3 && consistent(&s));
    song_place(&s, 0, 0, 3);
    song_place(&s, 7, 0, 3);
    riff_delete(&s, 0, 3);
    CHECK(s.riff[0][2].steps == 0 && s.cell[0][0] == 0 && s.cell[7][0] == 0 && consistent(&s));
}

static void test_full_pool(void)
{
    uint32_t i, ok = 0;
    song_init(&s);
    for (i = 1; i <= SONG_RIFFS; i++) riff_new(&s, i & 3u, i, 64);
    for (i = 0; i < SONG_EVENTS + 10u; i++)
        if (riff_note_set(&s, i & 3u, 1u + (i >> 2) % SONG_RIFFS, (i >> 7) & 63u, 1, i & 127u, 100) >= 0) ok++;
    /* (riffs that do not exist on a track refuse; the rest fills the pool to the brim, never past) */
    CHECK(s.nev <= SONG_EVENTS && ok == s.nev);
    CHECK(consistent(&s));
    /* to the brim: 64 notes on each of 64 steps of one riff = 4096 */
    song_init(&s);
    riff_new(&s, 3, 32, 64);
    for (i = 0, ok = 0; i < 64u * 65u; i++)
        if (riff_note_set(&s, 3, 32, i / 65u, 1, 30u + i % 65u, 100) >= 0) ok++;
    CHECK(ok == SONG_EVENTS && s.nev == SONG_EVENTS && consistent(&s));
    CHECK(riff_note_set(&s, 3, 32, 0, 3, 30, 1) >= 0);          /* replacing still works when full */
}

static void test_arrangement(void)
{
    uint32_t b0 = 99;
    got_t g;
    song_init(&s);
    riff_new(&s, 0, 1, 16);                 /* 1 bar */
    riff_new(&s, 0, 2, 64);                 /* 4 bars */
    riff_new(&s, 1, 1, 24);                 /* 1.5 bars: covers 2 */
    riff_note_set(&s, 0, 1, 0, 2, 60, 100);
    riff_note_set(&s, 0, 2, 0, 64, 40, 100);
    riff_note_set(&s, 0, 2, 50, 30, 41, 100);   /* runs past 64: cut */
    riff_note_set(&s, 1, 1, 20, 8, 70, 100);    /* runs past 24: cut */
    song_place(&s, 0, 0, 1);
    song_place(&s, 2, 0, 2);
    song_place(&s, 6, 0, 1);                /* right after riff 2's 4 bars */
    song_place(&s, 1, 1, 1);
    song_place(&s, 9, 0, 7);                /* riff 7 does not exist: ignored */
    CHECK(s.cell[9][0] == 0);
    CHECK(song_at(&s, 0, 0, &b0) == 1 && b0 == 0);
    CHECK(song_at(&s, 1, 0, 0) == 0);       /* riff 1 is one bar: silence after */
    CHECK(song_at(&s, 3, 0, &b0) == 2 && b0 == 2);
    CHECK(song_at(&s, 5, 0, &b0) == 2 && b0 == 2);
    CHECK(song_at(&s, 6, 0, &b0) == 1 && b0 == 6);
    CHECK(song_at(&s, 7, 0, 0) == 0);
    CHECK(song_at(&s, 2, 1, &b0) == 1 && b0 == 1 && song_at(&s, 3, 1, 0) == 0);
    CHECK(song_bars(&s) == 7);
    song_place(&s, 126, 0, 2);
    CHECK(song_bars(&s) == SONG_BARS);      /* (a 4-bar riff at 126 ends at the grid's end) */
    song_place(&s, 126, 0, 0);
    g = at(0, 0);
    CHECK(g.n == 1 && g.ev[0].note == 60);
    g = at(0, 2 * 16 + 50);
    CHECK(g.n == 1 && g.ev[0].note == 41 && g.ev[0].len == 14);
    g = at(0, 3 * 16 + 2);                  /* bar 3, riff 2 offset 18: nothing */
    CHECK(g.n == 0);
    song_place(&s, 4, 0, 1);                /* a riff that starts cuts the one before */
    CHECK(song_at(&s, 4, 0, &b0) == 1 && b0 == 4 && song_at(&s, 5, 0, 0) == 0 && at(0, 2 * 16 + 50).n == 0);
    song_place(&s, 4, 0, 0);
    g = at(1, 16 + 20);
    CHECK(g.n == 1 && g.ev[0].len == 4);
    s.track[1].flags |= SONG_MUTE;
    CHECK(at(1, 16 + 20).n == 0);
    s.track[1].flags = 0;
    song_insert_bar(&s, 0);
    CHECK(s.cell[0][0] == 0 && s.cell[1][0] == 1 && s.cell[3][0] == 2 && s.cell[7][0] == 1 && s.cell[2][1] == 1);
    song_delete_bar(&s, 0);
    CHECK(s.cell[0][0] == 1 && s.cell[2][0] == 2 && s.cell[1][1] == 1 && s.cell[SONG_BARS - 1u][0] == 0);
}

static void test_pack(void)
{
    uint32_t n, i;
    test_arrangement();
    s.bpm10 = 1375; s.swing = 20; s.track[2].vol = 33; s.track[3].pan = 10; s.loop_end = 4;
    n = song_pack(&s, buf, sizeof buf);
    CHECK(n > 0 && n < 1200u);
    printf("  packed demo song: %u B (in RAM: %u B)\n", n, (unsigned)sizeof(song_t));
    CHECK(song_unpack(&s2, buf, n) == 0);
    CHECK(memcmp(&s, &s2, sizeof s) == 0);
    CHECK(song_pack(&s, buf, 10) == 0);     /* too small */
    buf[n / 2] ^= 1;
    CHECK(song_unpack(&s2, buf, n) == -2 && s2.nev == 0);
    buf[n / 2] ^= 1;
    CHECK(song_unpack(&s2, buf, n - 3) == -4);
    buf[0] = 'X';
    CHECK(song_unpack(&s2, buf, n) == -1);
    buf[0] = 'F'; buf[4] = 9;
    CHECK(song_unpack(&s2, buf, n) == -3);
    buf[4] = 1;
    /* the fullest song still fits SONG_PACK_MAX */
    test_full_pool();
    for (i = 0; i < SONG_BARS; i++) s.cell[i][i & 3u] = (uint8_t)(1u + i % SONG_RIFFS);
    n = song_pack(&s, buf, sizeof buf);
    CHECK(n > 0 && n <= SONG_PACK_MAX);
    CHECK(song_unpack(&s2, buf, n) == 0 && memcmp(&s, &s2, sizeof s) == 0);
    printf("  fullest song: %u B packed (max %u)\n", n, (unsigned)SONG_PACK_MAX);
}

int main(void)
{
    test_riffs();
    test_full_pool();
    test_arrangement();
    test_pack();
    printf("song: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}

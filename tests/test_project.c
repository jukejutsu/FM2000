/* SPDX-License-Identifier: GPL-3.0-only */
/* Tests of the project slots (firmware/src/project.h) against a RAM flash that behaves as NOR does. */
#include <stdio.h>
#include <string.h>

#define STORE_LEN 0x36000u
static unsigned char nor[STORE_LEN];
static int erases, fail_after = -1;             /* fail_after: the store fails from that operation on (a cut save) */
static int ops;
static int store_read(unsigned off, void *p, unsigned n)
{
    if (off > STORE_LEN || n > STORE_LEN - off) return -1;
    memcpy(p, nor + off, n);
    return 0;
}
static int store_erase(unsigned off)
{
    if (fail_after >= 0 && ops++ >= fail_after) return -1;
    if ((off & 0xFFFu) || off >= STORE_LEN) return -1;
    memset(nor + off, 0xFF, 4096);
    erases++;
    return 0;
}
static int store_write(unsigned off, const void *p, unsigned n)
{
    const unsigned char *s = p;
    unsigned i;
    if (fail_after >= 0 && ops++ >= fail_after) return -1;
    if (off > STORE_LEN || n > STORE_LEN - off) return -1;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
#define PRJ_POOL
#include "project.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static song_t a, b;

static void make(song_t *s, const char *name, unsigned bars)
{
    unsigned i;
    song_init(s);
    for (i = 0; name[i] && i < SONG_NAME; i++) s->name[i] = name[i];
    riff_new(s, 0, 1, 16);
    riff_note_set(s, 0, 1, 0, 4, 60, 100);
    for (i = 0; i < bars; i++) song_place(s, i, 0, 1);
}

int main(void)
{
    prj_head_t h;
    memset(nor, 0xFF, sizeof nor);
    prj_scan();
    CHECK(prj_gen == 0 && prj_resume() == PRJ_NONE);
    {
        unsigned s;
        for (s = 0; s < PRJ_SLOTS; s++) CHECK(prj_info(s, &h) != 0);
    }
    /* save, read back */
    make(&a, "ONE", 8);
    CHECK(prj_save(1, &a, 1, 0) == 0);
    CHECK(prj_info(1, &h) == 0 && h.bars == 8 && h.origin == 1 && h.gen == 1 && !memcmp(h.name, "ONE", 3));
    CHECK(prj_load(1, &b, 0) == 0 && !memcmp(&a, &b, sizeof a));
    CHECK(prj_resume() == 1);                   /* only a slot saved: that slot */
    CHECK(erases == 1);                         /* a small song: one sector */
    /* slots do not touch each other */
    make(&a, "TWO", 3);
    CHECK(prj_save(2, &a, 2, 0) == 0 && prj_info(1, &h) == 0 && h.bars == 8);
    CHECK(prj_save(8, &a, 8, 0) == 0 && prj_info(8, &h) == 0);
    /* clear */
    CHECK(prj_clear(2) == 0 && prj_info(2, &h) != 0 && prj_load(2, &b, 0) != 0);
    /* the autosave and resume */
    make(&a, "ONE*", 9);
    CHECK(prj_save(PRJ_AUTO, &a, 1, PRJ_UNSAVED) == 0);
    CHECK(prj_resume() == PRJ_AUTO);
    CHECK(prj_save(1, &a, 1, 0) == 0 && prj_resume() == 1);      /* saved by hand after: the slot wins */
    CHECK(prj_save(PRJ_AUTO, &a, 1, PRJ_UNSAVED) == 0 && prj_resume() == PRJ_AUTO);
    CHECK(prj_clear(1) == 0 && prj_resume() == PRJ_AUTO);        /* its origin gone: the autosave */
    /* the generation survives a restart */
    {
        unsigned g = prj_gen;
        prj_gen = 0;
        prj_scan();
        CHECK(prj_gen == g);
    }
    /* a save cut after the erase, before the header: the slot reads as empty, not broken */
    make(&a, "CUT", 4);
    CHECK(prj_save(3, &a, 3, 0) == 0);
    ops = 0; fail_after = 2;                    /* erase ok, the song ok, the header fails */
    CHECK(prj_save(3, &a, 3, 0) != 0);
    fail_after = -1;
    CHECK(prj_info(3, &h) != 0);
    /* the fullest song fits a slot */
    {
        unsigned i;
        song_init(&a);
        riff_new(&a, 3, 32, 64);
        for (i = 0; i < SONG_EVENTS; i++) riff_note_set(&a, 3, 32, i / 64u, 1, i % 64u, 100);
        for (i = 0; i < SONG_BARS; i++) a.cell[i][i & 3u] = 0;
        CHECK(a.nev == SONG_EVENTS);
        CHECK(prj_save(4, &a, 4, 0) == 0 && prj_load(4, &b, &h) == 0 && !memcmp(&a, &b, sizeof a));
        printf("  fullest project: %u B of %u\n", (unsigned)(PRJ_HEAD + h.len), (unsigned)PRJ_SLOT_SIZE);
    }
    printf("project: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}

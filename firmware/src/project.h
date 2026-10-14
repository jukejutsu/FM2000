/* SPDX-License-Identifier: GPL-3.0-only */
/* FM2K's projects in the flash: 8 slots and an autosave, each a fixed 24 KiB area of the store (the platform's
 * store_read / store_erase / store_write, offsets from 0):
 *
 *   slot 0           AUTO: written by itself when the music is stopped and nothing has been touched for a while
 *   slots 1..8       the projects
 *
 * A slot: a 32-byte header, then the song as song_pack writes it (with its own CRC). The header is written last,
 * so a save cut short (power, a crash) leaves the slot reading as empty or as it was not, never half-loaded.
 * Clearing a slot erases its first sector: the header goes, the slot reads as empty.
 *
 * Each save takes the next generation number (gen); at power-on the newest save is opened, the autosave only
 * when nothing was saved by hand after it. */
#ifndef FM2K_PROJECT_H
#define FM2K_PROJECT_H
#include "song.h"

#define PRJ_SLOTS 9u                            /* 0 = AUTO, 1..8 */
#define PRJ_AUTO 0u
#define PRJ_NONE 0xFFu                          /* a project that is in no slot (new, or its slot cleared) */
#define PRJ_SLOT_SIZE 0x6000u                   /* 24 KiB: 6 sectors */
#define PRJ_HEAD 32u
#define PRJ_MAGIC 0x484B3246u                   /* "F2KH" */
#define PRJ_VERSION 1u
#define PRJ_UNSAVED 1u                          /* flags: the autosave holds changes not saved to its origin */

typedef struct {                                /* 32 bytes, little-endian on every target */
    uint32_t magic;
    uint8_t version, origin, bars, flags;       /* origin: the slot the work belongs to (PRJ_NONE: none) */
    uint32_t len;                               /* the packed song's bytes */
    uint32_t gen;                               /* the save's generation */
    char name[SONG_NAME];
    uint8_t pad[8];
} prj_head_t;

#ifndef PRJ_POOL
#define PRJ_POOL __attribute__((section(".pool")))
#endif
static uint8_t prj_buf[PRJ_HEAD + SONG_PACK_MAX] PRJ_POOL;   /* the packed song on its way to or from the flash */
static uint32_t prj_gen;                        /* the newest generation in the store */

/* the slot's header: 0 when it holds a project */
static int prj_info(uint32_t slot, prj_head_t *h)
{
    if (slot >= PRJ_SLOTS || store_read(slot * PRJ_SLOT_SIZE, h, sizeof *h)) return -1;
    if (h->magic != PRJ_MAGIC || h->version > PRJ_VERSION || h->len < 9u || h->len > SONG_PACK_MAX) return -1;
    return 0;
}

/* the newest generation of all the slots: call once at power-on, before the first save */
static void prj_scan(void)
{
    prj_head_t h;
    uint32_t s;
    prj_gen = 0;
    for (s = 0; s < PRJ_SLOTS; s++)
        if (!prj_info(s, &h) && h.gen > prj_gen) prj_gen = h.gen;
}

/* the song into a slot (erasing as much of it as the song needs): 0, or < 0 */
static int prj_save(uint32_t slot, const song_t *s, uint32_t origin, uint32_t flags)
{
    prj_head_t h;
    uint32_t n, off = slot * PRJ_SLOT_SIZE, sec, i;
    if (slot >= PRJ_SLOTS) return -1;
    n = song_pack(s, prj_buf + PRJ_HEAD, SONG_PACK_MAX);
    if (!n || PRJ_HEAD + n > PRJ_SLOT_SIZE) return -2;
    sec = (PRJ_HEAD + n + 4095u) / 4096u;
    for (i = 0; i < sec; i++)
        if (store_erase(off + i * 4096u)) return -3;
    if (store_write(off + PRJ_HEAD, prj_buf + PRJ_HEAD, n)) return -3;
    song_bytes_set(&h, 0, sizeof h);
    h.magic = PRJ_MAGIC;
    h.version = PRJ_VERSION;
    h.origin = (uint8_t)origin;
    h.bars = (uint8_t)(song_bars(s) > 255u ? 255u : song_bars(s));
    h.flags = (uint8_t)flags;
    h.len = n;
    h.gen = ++prj_gen;
    for (i = 0; i < SONG_NAME; i++) h.name[i] = s->name[i];
    return store_write(off, &h, sizeof h) ? -3 : 0;    /* the header last */
}

/* the slot's song into s: 0, or < 0 (s is then left as song_unpack leaves it: empty) */
static int prj_load(uint32_t slot, song_t *s, prj_head_t *h)
{
    prj_head_t tmp;
    if (!h) h = &tmp;
    if (prj_info(slot, h)) return -1;
    if (store_read(slot * PRJ_SLOT_SIZE + PRJ_HEAD, prj_buf, h->len)) return -2;
    return song_unpack(s, prj_buf, h->len) ? -3 : 0;
}

static int prj_clear(uint32_t slot)             /* the header's sector erased: the slot reads as empty */
{
    if (slot >= PRJ_SLOTS) return -1;
    return store_erase(slot * PRJ_SLOT_SIZE);
}

/* what to open at power-on: the newest save of all, the autosave included (so a manual save made after the last
 * autosave wins). Returns the slot (PRJ_AUTO or 1..8), or PRJ_NONE when the store holds nothing. */
static uint32_t prj_resume(void)
{
    prj_head_t h;
    uint32_t s, best = PRJ_NONE, gen = 0;
    for (s = 0; s < PRJ_SLOTS; s++)
        if (!prj_info(s, &h) && (best == PRJ_NONE || h.gen > gen)) { best = s; gen = h.gen; }
    return best;
}
#endif

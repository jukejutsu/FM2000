/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 in a browser: what tests/host.c is for a script, this is for a page. platform.c includes it instead
 * of the hardware when FM2K_WEB is set; the app (firmware/src/fm2k.c) runs unchanged on it, compiled to
 * WebAssembly (web/build.sh) and driven by an AudioWorklet (web/worklet.js):
 *
 *   - time: the page asks for audio, web_render(n) runs the app's main loop once per millisecond of it
 *     (app_step) and the audio interrupt (app_audio) for the frames: the device clock is the sound's clock;
 *   - the panel: buttons, keys and encoders as the page sends them (web_buttons, web_keys, web_enc);
 *   - the LEDs: fm1_led_key into two masks the page reads back;
 *   - the screen: a 240 x 240 RGB565 frame the page copies when web_screen_draws() changes;
 *   - settings_load / settings_save: a 256-byte area the page keeps in the browser (web_nv*);
 *   - the store (store_read / _erase / _write): a 216 KiB flash image, its used sectors kept by the page (web_store*).
 * After the browser builds of X0X (charlesvestal/fm1-x0x) and Felucca (hugelton/Felucca, web/emu), GPL-3.0.
 * Not here: USB, the updater, the rescue, the crash screen, MIDI out (counted, not sent). */
#include <stdint.h>
#define WEB_EXPORT(name) __attribute__((export_name(#name)))

#ifndef APP_NAME
#define APP_NAME "FM2K"
#endif
#ifndef APP_VER
#define APP_VER "web"
#endif
#define FS 44100u
#define HALF_FRAMES 256u
#define RING_PUBLISH() do {} while (0)

#include "libc.c"                               /* memset, memcpy, memcmp: the compiler's own calls */

static volatile uint32_t fm1_ms;
static struct { uint32_t notes, buttons; } fm1_in;

/* ---- the panel */
static uint32_t web_pressed, web_released, web_note_edges;
static int32_t web_enc_steps[7];
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = web_pressed;
    if (released) *released = web_released;
    web_pressed = web_released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void) { uint32_t n = web_note_edges; web_note_edges = 0; return n; }
static int32_t fm1_enc_take(uint32_t e) { int32_t s = web_enc_steps[e % 7u]; web_enc_steps[e % 7u] = 0; return s; }

static uint32_t web_led_buttons, web_led_keys;
static void fm1_led_key(uint32_t id, int on)
{
    if (id < 14u) { if (on) web_led_buttons |= 1u << id; else web_led_buttons &= ~(1u << id); }
    else if (id < 14u + 27u) { if (on) web_led_keys |= 1u << (id - 14u); else web_led_keys &= ~(1u << (id - 14u)); }
}

/* ---- the screen: RGB565 as the colour values (gfx.c sends canvases byte-swapped, as the panel wants them) */
static uint16_t web_fb[240 * 240];
static uint32_t web_draws;
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            web_fb[j * 240u + i] = c;
    web_draws++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                web_fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
    web_draws++;
}
#define CV_MAX (240u * 64u)
#include "gfx.c"

/* ---- settings, kept by the page */
static uint8_t web_nv[4 + 256];                 /* [1][len][0][0] then the bytes */
static uint32_t web_nv_writes;
static int settings_load(void *p, uint32_t n)
{
    if (web_nv[0] != 1u || web_nv[1] != (uint8_t)n || n > 256u) return -1;
    memcpy(p, web_nv + 4, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    if (n > 256u) return -1;
    web_nv[0] = 1; web_nv[1] = (uint8_t)n;
    memcpy(web_nv + 4, p, n);
    web_nv_writes++;
    return 0;
}

/* ---- the store: a flash image the page keeps (web_store*: it hands the saved sectors back before web_init) */
#define STORE_LEN 0x00036000u
static uint8_t web_nor[STORE_LEN];
static uint32_t web_store_writes;
static int store_read(uint32_t off, void *p, uint32_t n)
{
    if (off > STORE_LEN || n > STORE_LEN - off) return -1;
    memcpy(p, web_nor + off, n);
    return 0;
}
static int store_erase(uint32_t off)
{
    if ((off & 0xFFFu) || off >= STORE_LEN) return -1;
    memset(web_nor + off, 0xFF, 4096);
    web_store_writes++;
    return 0;
}
static int store_write(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    uint32_t i;
    if (off > STORE_LEN || n > STORE_LEN - off) return -1;
    for (i = 0; i < n; i++) web_nor[off + i] &= s[i];
    web_store_writes++;
    return 0;
}

/* ---- MIDI: in from the page (Web MIDI), out only counted */
static uint8_t web_mq[64][3];
static uint32_t web_mq_w, web_mq_r, web_midi_out;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (web_mq_r == web_mq_w) return 0;
    *st = web_mq[web_mq_r % 64u][0]; *d1 = web_mq[web_mq_r % 64u][1]; *d2 = web_mq[web_mq_r % 64u][2];
    web_mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; web_midi_out++; }

static void platform_poll(void) {}              /* (the page runs the loop: web_render) */

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);
static void app_init(void);
static void app_step(void);

/* ---- what the page calls */
#define WEB_MAX 1024u                           /* frames per web_render at most */
static int32_t web_mix[2u * WEB_MAX];
static float web_l[WEB_MAX], web_r[WEB_MAX];
static uint32_t web_owed, web_master_q = 700u * 64u;   /* MASTER: 0..1023 -> gain Q16 */

WEB_EXPORT(web_init) void web_init(void) { app_init(); }
WEB_EXPORT(web_render) void web_render(uint32_t n)
{
    uint32_t i, done = 0;
    if (n > WEB_MAX) n = WEB_MAX;
    while (done < n) {                          /* the main loop once a millisecond, the audio in between */
        uint32_t k;
        if (web_owed >= FS) {
            web_owed -= FS;
            fm1_ms++;
            app_step();
            continue;
        }
        k = (FS - web_owed + 999u) / 1000u;     /* frames until the next millisecond */
        if (k > n - done) k = n - done;
        if (k > 64u) k = 64u;
        memset(web_mix + 2u * done, 0, k * 8u);
        app_audio(web_mix + 2u * done, k);
        done += k;
        web_owed += k * 1000u;
    }
    for (i = 0; i < n; i++) {
        int32_t l = (int32_t)(((int64_t)web_mix[2u * i] * web_master_q) >> 16);
        int32_t r = (int32_t)(((int64_t)web_mix[2u * i + 1u] * web_master_q) >> 16);
        l = l > 32767 ? 32767 : l < -32767 ? -32767 : l;
        r = r > 32767 ? 32767 : r < -32767 ? -32767 : r;
        web_l[i] = (float)l * (1.0f / 32768.0f);
        web_r[i] = (float)r * (1.0f / 32768.0f);
    }
}
WEB_EXPORT(web_out_l) float *web_out_l(void) { return web_l; }
WEB_EXPORT(web_out_r) float *web_out_r(void) { return web_r; }
WEB_EXPORT(web_sample_rate) uint32_t web_sample_rate(void) { return FS; }
WEB_EXPORT(web_now_ms) uint32_t web_now_ms(void) { return fm1_ms; }
WEB_EXPORT(web_buttons) void web_buttons(uint32_t mask)
{
    mask &= 0x3FFFu;
    web_pressed |= mask & ~fm1_in.buttons;
    web_released |= fm1_in.buttons & ~mask;
    fm1_in.buttons = mask;
}
WEB_EXPORT(web_keys) void web_keys(uint32_t mask)
{
    mask &= 0x7FFFFFFu;
    web_note_edges |= mask & ~fm1_in.notes;
    fm1_in.notes = mask;
}
WEB_EXPORT(web_enc) void web_enc(uint32_t e, int32_t n) { if (e < 7u) web_enc_steps[e] += n; }
WEB_EXPORT(web_master) void web_master(uint32_t v) { web_master_q = (v > 1023u ? 1023u : v) * 64u; }
WEB_EXPORT(web_midi) void web_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (web_mq_w - web_mq_r >= 64u) return;
    web_mq[web_mq_w % 64u][0] = (uint8_t)st; web_mq[web_mq_w % 64u][1] = (uint8_t)d1; web_mq[web_mq_w % 64u][2] = (uint8_t)d2;
    web_mq_w++;
}
WEB_EXPORT(web_screen) uint16_t *web_screen(void) { return web_fb; }
WEB_EXPORT(web_screen_draws) uint32_t web_screen_draws(void) { return web_draws; }
WEB_EXPORT(web_lit_buttons) uint32_t web_lit_buttons(void) { return web_led_buttons; }
WEB_EXPORT(web_lit_keys) uint32_t web_lit_keys(void) { return web_led_keys; }
WEB_EXPORT(web_nv) uint8_t *web_nv_ptr(void) { return web_nv; }
WEB_EXPORT(web_nv_size) uint32_t web_nv_size(void) { return sizeof web_nv; }
WEB_EXPORT(web_store) uint8_t *web_store_ptr(void) { return web_nor; }
WEB_EXPORT(web_store_size) uint32_t web_store_size(void) { return STORE_LEN; }
WEB_EXPORT(web_store_writes) uint32_t web_store_writes_count(void) { return web_store_writes; }
WEB_EXPORT(web_store_erase_all) void web_store_erase_all(void) { memset(web_nor, 0xFF, sizeof web_nor); }
WEB_EXPORT(web_nv_writes) uint32_t web_nv_writes_count(void) { return web_nv_writes; }

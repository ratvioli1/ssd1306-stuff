/* Shared SSD1306 (128x64, I2C) driver layer.
 *
 * Transport-agnostic: the caller supplies a write callback that knows how to
 * push bytes at 0x3C over whatever bus it owns (Linux i2c-dev, raw DesignWare
 * registers, Arduino Wire, an RTOS task, ...). This is the layer that is
 * identical across every example in the deck.
 *
 * I2C framing used here (SSD1306 datasheet section 8):
 *   control byte 0x00 -> the following bytes are COMMANDS
 *   control byte 0x40 -> the following bytes are GDDRAM DATA
 * plus the 7-bit address 0x3C (wire byte 0x78).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "font5x7.h"

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)
#define OLED_ADDR 0x3C

/* Send `len` payload bytes. is_data selects the 0x40 vs 0x00 control byte. */
typedef void (*oled_write_fn)(void *ctx, const uint8_t *buf, size_t len,
                              int is_data);

/* Standard 128x64 SSD1306 power-on sequence (no data bytes mixed in). */
static const uint8_t OLED_INIT[] = {
    0xAE,        /* display off            */
    0xD5, 0x80,  /* clock divide / osc     */
    0xA8, 0x3F,  /* multiplex ratio = 64   */
    0xD3, 0x00,  /* display offset 0       */
    0x40,        /* start line 0           */
    0x8D, 0x14,  /* charge pump on         */
    0x20, 0x00,  /* memory mode = horizontal */
    0xA1,        /* segment remap          */
    0xC8,        /* COM scan remapped      */
    0xDA, 0x12,  /* COM pins config        */
    0x81, 0xCF,  /* contrast               */
    0xD9, 0xF1,  /* pre-charge             */
    0xDB, 0x40,  /* VCOMH deselect         */
    0xA4,        /* resume RAM content     */
    0xA6,        /* normal (not inverted)  */
    0x2E,        /* deactivate scroll      */
    0xAF,        /* display on             */
};

typedef struct {
    oled_write_fn write;
    void *ctx;
    uint8_t fb[OLED_W * OLED_PAGES]; /* 1 bpp, 1024 bytes */
} oled_t;

static inline void oled_send(oled_t *o, int is_data, const uint8_t *p,
                             size_t n) {
    o->write(o->ctx, p, n, is_data);
}

static inline void oled_init(oled_t *o, oled_write_fn write, void *ctx) {
    o->write = write;
    o->ctx = ctx;
    memset(o->fb, 0, sizeof o->fb);
    oled_send(o, 0, OLED_INIT, sizeof OLED_INIT);
}

static inline void oled_clear(oled_t *o) { memset(o->fb, 0, sizeof o->fb); }

static inline void oled_pixel(oled_t *o, int x, int y) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H)
        return;
    o->fb[x + (y / 8) * OLED_W] |= (uint8_t)(1u << (y & 7));
}

static inline void oled_char(oled_t *o, int x, int y, char ch) {
    const glyph_t *g = NULL;
    for (int i = 0; i < FONT5X7_N; i++) {
        if (FONT5X7[i].ch == ch) {
            g = &FONT5X7[i];
            break;
        }
    }
    if (!g) {
        if (ch >= 'a' && ch <= 'z')
            return oled_char(o, x, y, (char)(ch - 32)); /* fold to upper */
        g = &FONT5X7[0];
    }
    for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 7; cy++)
            if (g->col[cx] & (1u << cy))
                oled_pixel(o, x + cx, y + cy);
}

static inline void oled_text(oled_t *o, int x, int y, const char *s) {
    for (; *s; s++, x += 6)
        oled_char(o, x, y, *s);
}

/* Draw multi-line text. Both a real newline and the literal two characters
 * backslash-n are treated as line breaks, so shells can pass "a\nb". */
static inline void oled_draw_lines(oled_t *o, int x, int y, const char *s) {
    char line[80];
    size_t k = 0;
    while (*s) {
        if (*s == '\\' && s[1] == 'n') {
            s += 2;
        } else if (*s == '\n') {
            s += 1;
        } else {
            if (k < sizeof line - 1)
                line[k++] = *s;
            s++;
            continue;
        }
        line[k] = '\0';
        oled_text(o, x, y, line);
        y += 10;
        k = 0;
    }
    line[k] = '\0';
    oled_text(o, x, y, line);
}

/* Set the address window to the whole panel, then stream the framebuffer. */
static inline void oled_flush(oled_t *o) {
    const uint8_t win[] = {0x21, 0, OLED_W - 1, 0x22, 0, OLED_PAGES - 1};
    oled_send(o, 0, win, sizeof win);
    oled_send(o, 1, o->fb, sizeof o->fb);
}

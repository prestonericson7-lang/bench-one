/*
 * font8x8.h -- 8x8 bitmap font (ASCII 0x20..0x7E) and text rendering into RGB565 buffers.
 * Glyph data: "font8x8_basic" by Daniel Hepper, public domain (derived from the IBM PC BIOS
 * font); row r of glyph c = font8x8_basic[c][r], bit 0 = leftmost pixel.
 */
#ifndef PI_FONT8X8_H
#define PI_FONT8X8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t font8x8_basic[128][8];

/* width/height in pixels of text at `scale` (lines separated by '\n') */
int font_text_w(const char *s, int scale);
int font_text_h(const char *s, int scale);

/* Draw text into a bw x bh RGB565 buffer with its top-left at (x, y) (clipped to the buffer).
 * fg is drawn where the glyph has a pixel; bg too where it has none unless transparent_bg.
 * Characters outside 0x20..0x7E are drawn as '?'. */
void font_draw(uint16_t *buf, int bw, int bh, int x, int y, const char *s, int scale, uint16_t fg,
               uint16_t bg, int transparent_bg);

/* The same with a 1-pixel (x scale) outline in `outline` around every glyph pixel (readable on
 * any background); the glyphs themselves in fg. Background untouched. */
void font_draw_outlined(uint16_t *buf, int bw, int bh, int x, int y, const char *s, int scale,
                        uint16_t fg, uint16_t outline);

#ifdef __cplusplus
}
#endif
#endif

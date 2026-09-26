/*
 * img.h -- image files for the Pi tools: RGB565 frames -> PPM (P6) / PNG, PPM -> RGB888.
 * The PNG writer is self-contained (zlib stream with LZ77 + fixed-Huffman deflate, CRC32,
 * Adler-32), so the static binaries need no libpng/zlib.
 * RGB565 -> RGB888 uses the scanout's expansion (SPEC 8): r8 = r5<<3 | r5>>2, g8 = g6<<2 | g6>>4.
 */
#ifndef PI_IMG_H
#define PI_IMG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rgb565_to_rgb888(uint16_t p, uint8_t out[3]);

int img_write_ppm(const char *path, const uint16_t *px, int w, int h);
int img_write_png(const char *path, const uint16_t *px, int w, int h);
/* .png (any case) -> PNG, anything else -> PPM. 0 = ok, -1 = error (errno set). */
int img_write(const char *path, const uint16_t *px, int w, int h);

/* PNG of an RGB888 buffer into memory (malloc'ed *out). 0 = ok. */
int png_encode_rgb(const uint8_t *rgb, int w, int h, uint8_t **out, size_t *outlen);

/* Read a PPM (P6 or P3, maxval 1..65535) as RGB888 (malloc'ed *rgb). 0 = ok, else -1 + err. */
int img_read_ppm(const char *path, uint8_t **rgb, int *w, int *h, char *err, int errlen);

#ifdef __cplusplus
}
#endif
#endif

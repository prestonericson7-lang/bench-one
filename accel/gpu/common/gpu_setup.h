/*
 * gpu_setup.h -- triangle / rect / sprite setup: screen-space primitives -> 24-word PL records.
 * Portable C99. Used by the Teensy firmware, the Zynq daemon, the Pi tools and the golden tests.
 * Compile with -ffp-contract=off (or #pragma GCC optimize("fp-contract=off")) on every target so
 * results are bit-identical across x86-64, AArch64, ARMv7 (Zynq) and Cortex-M7 (Teensy).
 * Exact math: SPEC.md section 4.
 */
#ifndef GPU_SETUP_H
#define GPU_SETUP_H

#include <stdint.h>
#include "gpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GPU_PACKED {
    float   x, y, z;          /* x,y in pixels (pixel centres at +0.5), z in [0,1] (clamped) */
    uint8_t r, g, b, a;       /* a unused */
} gpu_vtx;                    /* 16 bytes; same layout as net_vtx */

#define GPU_CULL_NONE 0
#define GPU_CULL_CW   1       /* cull if area2 > 0 (clockwise on screen, y down) = OpenGL back faces */
#define GPU_CULL_CCW  2       /* cull if area2 < 0 */

#define GPU_SETUP_MAX_OUT 8   /* a guard-band-clipped triangle yields at most 5 */

/* One triangle, any coordinates: clips against the guard band if needed, then sets up.
 * Writes up to max_out records to out; returns how many (0 = culled / empty / degenerate). */
int gpu_setup_tri(const gpu_vtx v[3], uint32_t flags, int cull,
                  uint32_t out[][GPU_REC_WORDS], int max_out);

/* One triangle that must already lie inside the guard band.
 * Returns 1 = record written, 0 = culled/empty/degenerate, -1 = a vertex outside the guard band. */
int gpu_setup_tri_noclip(const gpu_vtx v[3], uint32_t flags, int cull, uint32_t out[GPU_REC_WORDS]);

/* Solid rectangle [x0,x1) x [y0,y1), clipped to the screen. Returns 1 = written, 0 = empty. */
int gpu_setup_rect(int x0, int y0, int x1, int y1, uint16_t rgb565, float z, uint32_t flags,
                   uint32_t out[GPU_REC_WORDS]);

/* Sprite at (x,y) of w x h pixels from DDR src (8-byte aligned) with row stride (multiple of 8).
 * x and w must be multiples of 4 (x may be negative); negative x/y are clipped by advancing src.
 * Returns 1 = written, 0 = fully off-screen, -1 = bad alignment/arguments. */
int gpu_setup_sprite(int x, int y, int w, int h, uint32_t src_addr, uint32_t stride,
                     int colorkey_en, uint16_t colorkey, uint32_t out[GPU_REC_WORDS]);

void gpu_make_end(uint32_t frame_no, uint32_t out[GPU_REC_WORDS]);

static inline uint16_t gpu_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#ifdef __cplusplus
}
#endif
#endif

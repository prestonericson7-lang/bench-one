/*
 * gpu_refrast.h -- bit-exact golden model of the PL renderer (SPEC.md section 5).
 * Given the ordered list of records that the PL renders for one frame, produces the exact
 * RGB565 framebuffer the hardware writes. Used by RTL testbenches (via hex files), the
 * x86 daemon simulator, and gpu_selftest on the Pi.
 */
#ifndef GPU_REFRAST_H
#define GPU_REFRAST_H

#include <stdint.h>
#include "gpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read one RGB565 pixel (little-endian u16) at a DDR physical address (2-byte aligned). */
typedef uint16_t (*gpu_ddr_read16_fn)(void *user, uint32_t addr);

/* Render one frame. recs = nrecs consecutive records (GPU_REC_WORDS words each), in list order.
 * TRI and SPRITE records draw; NOP, END and unknown types are ignored.
 * fb receives GPU_W*GPU_H pixels, row-major, pixel (x,y) at fb[y*GPU_W + x]. */
void gpu_refrast_frame(const uint32_t *recs, int nrecs, uint16_t clear_color,
                       gpu_ddr_read16_fn rd, void *user, uint16_t *fb);

#ifdef __cplusplus
}
#endif
#endif

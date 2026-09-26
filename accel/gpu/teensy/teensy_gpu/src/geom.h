/*
 * geom.h -- the Teensy 4.1 geometry stage, as portable C99.
 * Runs on the Teensy (the real GPU stage), and is also linked into the Pi tools (to predict the
 * exact records the Teensy emits, for bit-exact self-test) and the x86 Teensy simulator.
 * Exact math: SPEC.md section 6. Compile with fp-contract off on every target.
 */
#ifndef GEOM_H
#define GEOM_H

#include <stdint.h>
#include "gpu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GEOM_MAX_MESHES   16
#define GEOM_MAX_VERTS    8192      /* total over all meshes */
#define GEOM_MAX_INDICES  24576     /* total over all meshes (8192 triangles) */

typedef struct GPU_PACKED {
    float   px, py, pz;             /* object-space position */
    float   nx, ny, nz;             /* object-space normal (need not be unit length) */
    uint8_t r, g, b, a;             /* vertex colour */
} geom_vertex;                      /* 28 bytes */

#define GEOM_DRAW_ZTEST     0x0001u
#define GEOM_DRAW_ZWRITE    0x0002u
#define GEOM_DRAW_CULL_BACK 0x0004u /* cull OpenGL back faces (meshes are CCW-front) */
#define GEOM_DRAW_LIGHTING  0x0008u

typedef struct GPU_PACKED {
    uint16_t mesh_id, flags;
    float    model[16];             /* column-major (OpenGL) */
    uint8_t  color_mul[4];          /* r,g,b multipliers 0..255 (255 = 1.0); [3] unused */
} geom_draw;                        /* 72 bytes */

typedef struct GPU_PACKED {
    uint32_t frame_no;
    float    viewproj[16];          /* column-major, OpenGL clip space (-w<=z<=w) */
    float    light_dir[3];          /* direction the light travels, unit length */
    float    ambient;               /* 0..1 */
    uint16_t ndraws, flags;         /* flags: GEOM_FRAME_* (0 = normal) */
} geom_frame_hdr;                   /* 88 bytes */

/* geom_frame_hdr.flags -- only the Teensy firmware / teensy_sim act on these; geom_frame() ignores them */
#define GEOM_FRAME_RETURN  0x0001u  /* also send every emitted record back to the Pi in the T_FRAME reply */
#define GEOM_FRAME_NO_BUS  0x0002u  /* do not send to the FPGA (geometry-only service for the Pi) */

typedef struct {
    uint32_t tris_in;               /* triangles submitted by draws */
    uint32_t tris_out;              /* TRI records emitted */
    uint32_t tris_culled;           /* rejected by culling, trivial reject, or empty after setup */
    uint32_t tris_clipped;          /* triangles that needed polygon clipping */
} geom_stats;

/* Called once per emitted record, in order. Return 0 to continue, <0 to abort the frame. */
typedef int (*geom_emit_fn)(void *user, const uint32_t rec[GPU_REC_WORDS]);

void geom_reset(void);              /* forget all meshes */

/* Store a mesh (copies the data). Re-uploading an id replaces it; returns GPU_ERR_* */
int  geom_mesh_upload(uint16_t id, uint32_t nverts, uint32_t nidx,
                      const geom_vertex *verts, const uint16_t *idx);

/* Transform, light, clip, project, set up and emit every triangle of every draw, in order,
 * then emit one END record carrying hdr->frame_no. Returns 0 or a GPU_ERR_* / emit error. */
int  geom_frame(const geom_frame_hdr *hdr, const geom_draw *draws,
                geom_emit_fn emit, void *user, geom_stats *st);

#ifdef __cplusplus
}
#endif
#endif

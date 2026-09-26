/*
 * tg_geom_ext.h -- extra mesh-store entry points implemented in common/geom.c.
 * They let the firmware stream a T_MESH payload straight into the (278 KB) mesh store instead of
 * buffering it: begin -> write nverts*28 vertex bytes to *vdst and nidx*2 index bytes to *idst
 * -> commit (validates the indices) or abort. geom_mesh_upload() is begin + memcpy + commit.
 * Nothing else may touch the mesh store (upload, reset, frame) between begin and commit/abort.
 */
#ifndef TG_GEOM_EXT_H
#define TG_GEOM_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve room for mesh `id` (an existing mesh with that id is deleted once the new one is known
 * to fit). Returns GPU_ERR_OK, GPU_ERR_NOMEM (limits / store full; old mesh kept) or GPU_ERR_ARG
 * (nidx not a multiple of 3, or indices without vertices; old mesh kept). */
int  geom_x_mesh_begin(uint16_t id, uint32_t nverts, uint32_t nidx, void **vdst, void **idst);
/* GPU_ERR_OK, or GPU_ERR_ARG if an index >= nverts (then the id is not stored). */
int  geom_x_mesh_commit(void);
void geom_x_mesh_abort(void);
int  geom_x_mesh_exists(uint16_t id);
void geom_x_usage(uint32_t *nmeshes, uint32_t *nverts, uint32_t *nidx);

#ifdef __cplusplus
}
#endif
#endif

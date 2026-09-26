/*
 * mesh.h -- procedural meshes in the Teensy's geom_vertex format (common/geom.h).
 * Convention (SPEC 6): counter-clockwise front faces (OpenGL, seen from outside) and outward
 * unit normals. mesh_check() verifies both.
 */
#ifndef PI_MESH_H
#define PI_MESH_H

#include <stdint.h>
#include "geom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t     nverts, nidx;
    geom_vertex *v;
    uint16_t    *idx;
} mesh_t;

/* Colour: rgb = NULL -> smooth rainbow gradient over the surface, else a solid colour. */

/* Cube [-h,h]^3, 24 vertices (flat per-face normals), 12 triangles. face_rgb[6] in the order
 * +X, -X, +Y, -Y, +Z, -Z; NULL = red, cyan, green, magenta, blue, yellow. */
int  mesh_cube(mesh_t *m, float h, const uint8_t (*face_rgb)[3]);
/* UV sphere of radius r: (stacks+1)*(slices+1) vertices; degenerate pole triangles omitted. */
int  mesh_sphere(mesh_t *m, float r, int stacks, int slices, const uint8_t *rgb);
/* Torus around the Y axis: ring radius R, tube radius r, nu segments around Y, nv around the tube. */
int  mesh_torus(mesh_t *m, float R, float r, int nu, int nv, const uint8_t *rgb);
void mesh_free(mesh_t *m);

/* Every triangle non-degenerate, CCW seen from the side its vertex normals point to, and every
 * vertex normal of unit length and pointing away from `inside(p)` (the nearest interior point:
 * centre for cube/sphere, ring centre for the torus -- pass kind 0 = cube/sphere, 1 = torus
 * with ring radius R). Returns 0 = ok, else -1 with a reason in why. */
int  mesh_check(const mesh_t *m, int kind, float R, char *why, int whylen);

#ifdef __cplusplus
}
#endif
#endif

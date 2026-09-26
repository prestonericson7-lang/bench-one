#pragma GCC optimize("fp-contract=off")
/*
 * geom.c -- the Teensy 4.1 geometry stage (SPEC.md section 6, exact). Portable C99.
 *
 * Runs on the Teensy (Cortex-M7, FPv5-D16), in teensy_sim and in the Pi tools (x86-64 / AArch64),
 * which link this same file to predict the Teensy's records bit-exactly. Requirements for
 * bit-identical results on every target: IEEE-754 binary32/binary64, round-to-nearest-even,
 * FLT_EVAL_METHOD == 0 (no excess precision), no fused multiply-add (the pragma above plus
 * -ffp-contract=off where the build allows it), no -ffast-math, denormals not flushed.
 * Every float expression below is written in exactly the evaluation order of SPEC 6.
 *
 * Choices where SPEC 6 leaves room (reported to the integrator):
 *  - Clipping (Sutherland-Hodgman, planes near, far, left, right, bottom, top): a vertex is inside a
 *    plane iff its distance d >= 0 (NaN counts as outside). The intersection on an edge between an
 *    inside vertex I and an outside vertex O is ALWAYS computed from I towards O,
 *    t = dI/(dI-dO), a = aI + t*(aO-aI), whatever direction the polygon walks the edge, so two
 *    triangles that share an edge get bit-identical clip vertices (no cracks along clipped edges).
 *    A plane that no polygon vertex is outside of is skipped (identical to running the pass).
 *  - A clipped polygon left with fewer than 3 vertices counts once in tris_culled.
 *  - Colours are clamped with NaN -> 0 (so a NaN matrix / light can never cause an undefined
 *    float -> integer conversion); d = NaN stays NaN and ends as colour 0.
 *  - A draw that names a mesh id that is not stored makes geom_frame return GPU_ERR_ARG before
 *    anything is emitted (all draws are validated first, so the FPGA never sees half a frame).
 *  - Mesh ids are any uint16 value; at most GEOM_MAX_MESHES distinct ids are stored at a time.
 *    Meshes are stored compactly (vertex and index pools); replacing or deleting a mesh moves the
 *    meshes behind it down (memmove) -- other meshes' contents never change.
 *    nidx must be a multiple of 3 and every index < nverts (checked at upload, GPU_ERR_ARG).
 *    A re-upload whose data turns out to be invalid leaves that id deleted (never half-stored).
 *
 * Memory: the mesh store (GEOM_MAX_VERTS * 28 + GEOM_MAX_INDICES * 2 = 278,528 bytes) is marked
 * DMAMEM, which on the Teensy 4.x puts it in RAM2 (OCRAM, not zero-initialised -- nothing here
 * relies on that); the per-draw transformed-vertex cache (8192 * 29 bytes) stays in normal .bss
 * (RAM1 / DTCM on the Teensy: single-cycle, and RAM2 has no room for both).
 */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "geom.h"

#if defined(FLT_EVAL_METHOD) && (FLT_EVAL_METHOD != 0)
#error "geom.c requires FLT_EVAL_METHOD == 0 (float arithmetic without excess precision)"
#endif

/* RAM2 placement on the Teensy 4.x; empty everywhere else. */
#ifndef DMAMEM
#if defined(__IMXRT1062__)
#define DMAMEM __attribute__((section(".dmabuffers"), used))
#else
#define DMAMEM
#endif
#endif

/* ---------------------------------------------------------------------------------------- */
/* Mesh store                                                                                */
/* ---------------------------------------------------------------------------------------- */

/* Same byte layout as the packed geom_vertex, but naturally aligned (fast float loads). */
typedef struct {
    float   px, py, pz;
    float   nx, ny, nz;
    uint8_t r, g, b, a;
} gvtx;

typedef char geom_chk_gvtx_size[(sizeof(gvtx) == 28 && sizeof(geom_vertex) == 28) ? 1 : -1];
typedef char geom_chk_gvtx_col[(offsetof(gvtx, r) == offsetof(geom_vertex, r)) ? 1 : -1];
typedef char geom_chk_draw_size[(sizeof(geom_draw) == 72 && sizeof(geom_frame_hdr) == 88) ? 1 : -1];

typedef struct {
    uint32_t vbase, nverts;     /* range in g_verts */
    uint32_t ibase, nidx;       /* range in g_idx (indices are local to the mesh: 0..nverts-1) */
    uint16_t id;
    uint8_t  used;
} mesh_slot;

DMAMEM static gvtx     g_verts[GEOM_MAX_VERTS];
DMAMEM static uint16_t g_idx[GEOM_MAX_INDICES];
static mesh_slot g_mesh[GEOM_MAX_MESHES];
static uint32_t  g_vused, g_iused;          /* g_verts[0..g_vused) / g_idx[0..g_iused) are in use */

/* A streamed upload in progress (see geom_x_mesh_begin). Its data lives right after the used
 * part of the pools and becomes a mesh only in geom_x_mesh_commit. */
static struct {
    int      active;
    uint16_t id;
    uint32_t nverts, nidx;
} g_pend;

static mesh_slot *find_mesh(uint16_t id)
{
    int i;
    for (i = 0; i < GEOM_MAX_MESHES; i++)
        if (g_mesh[i].used && g_mesh[i].id == id)
            return &g_mesh[i];
    return NULL;
}

/* Remove a stored mesh and close the gap it leaves in both pools. */
static void delete_mesh(mesh_slot *m)
{
    uint32_t vb = m->vbase, nv = m->nverts, ib = m->ibase, ni = m->nidx;
    int i;
    if (nv && g_vused > vb + nv)
        memmove(&g_verts[vb], &g_verts[vb + nv], (size_t)(g_vused - vb - nv) * sizeof(gvtx));
    if (ni && g_iused > ib + ni)
        memmove(&g_idx[ib], &g_idx[ib + ni], (size_t)(g_iused - ib - ni) * sizeof(uint16_t));
    g_vused -= nv;
    g_iused -= ni;
    m->used = 0;
    for (i = 0; i < GEOM_MAX_MESHES; i++) {
        if (!g_mesh[i].used)
            continue;
        if (g_mesh[i].vbase > vb) g_mesh[i].vbase -= nv;
        if (g_mesh[i].ibase > ib) g_mesh[i].ibase -= ni;
    }
}

void geom_reset(void)
{
    memset(g_mesh, 0, sizeof g_mesh);
    g_vused = g_iused = 0;
    g_pend.active = 0;
}

/* ---- streamed upload API (declared in teensy/teensy_gpu/tg_geom_ext.h) -------------------- */

void geom_x_mesh_abort(void)
{
    g_pend.active = 0;          /* the reserved space simply stays unused */
}

/* Reserve room for mesh `id` (replacing any stored mesh with that id) and return where the raw
 * little-endian geom_vertex array (nverts * 28 bytes) and u16 index array (nidx * 2 bytes) must be
 * written. The old mesh is deleted only once the new one is known to fit. Returns GPU_ERR_*. */
int geom_x_mesh_begin(uint16_t id, uint32_t nverts, uint32_t nidx, void **vdst, void **idst)
{
    mesh_slot *old;
    uint32_t vfree, ifree;
    int i, have_slot = 0;

    g_pend.active = 0;
    if (nverts > GEOM_MAX_VERTS || nidx > GEOM_MAX_INDICES)
        return GPU_ERR_NOMEM;
    if (nidx % 3u != 0 || (nidx > 0 && nverts == 0))
        return GPU_ERR_ARG;
    old = find_mesh(id);
    vfree = GEOM_MAX_VERTS - g_vused + (old ? old->nverts : 0);
    ifree = GEOM_MAX_INDICES - g_iused + (old ? old->nidx : 0);
    for (i = 0; i < GEOM_MAX_MESHES; i++)
        if (!g_mesh[i].used || &g_mesh[i] == old)
            have_slot = 1;
    if (!have_slot || nverts > vfree || nidx > ifree)
        return GPU_ERR_NOMEM;
    if (old)
        delete_mesh(old);
    g_pend.active = 1;
    g_pend.id = id;
    g_pend.nverts = nverts;
    g_pend.nidx = nidx;
    if (vdst) *vdst = (void *)&g_verts[g_vused];
    if (idst) *idst = (void *)&g_idx[g_iused];
    return GPU_ERR_OK;
}

/* Validate the streamed data and make it a stored mesh. Returns GPU_ERR_*. */
int geom_x_mesh_commit(void)
{
    uint32_t i;
    int s;
    if (!g_pend.active)
        return GPU_ERR_ARG;
    g_pend.active = 0;
    for (i = 0; i < g_pend.nidx; i++)
        if (g_idx[g_iused + i] >= g_pend.nverts)
            return GPU_ERR_ARG;
    for (s = 0; s < GEOM_MAX_MESHES; s++)
        if (!g_mesh[s].used)
            break;
    if (s == GEOM_MAX_MESHES)
        return GPU_ERR_NOMEM;           /* unreachable: begin checked for a free slot */
    g_mesh[s].used = 1;
    g_mesh[s].id = g_pend.id;
    g_mesh[s].vbase = g_vused;
    g_mesh[s].nverts = g_pend.nverts;
    g_mesh[s].ibase = g_iused;
    g_mesh[s].nidx = g_pend.nidx;
    g_vused += g_pend.nverts;
    g_iused += g_pend.nidx;
    return GPU_ERR_OK;
}

int geom_x_mesh_exists(uint16_t id)
{
    return find_mesh(id) != NULL;
}

/* Store usage: meshes stored, vertices used, indices used. */
void geom_x_usage(uint32_t *nmeshes, uint32_t *nverts, uint32_t *nidx)
{
    uint32_t n = 0;
    int i;
    for (i = 0; i < GEOM_MAX_MESHES; i++)
        if (g_mesh[i].used)
            n++;
    if (nmeshes) *nmeshes = n;
    if (nverts) *nverts = g_vused;
    if (nidx) *nidx = g_iused;
}

int geom_mesh_upload(uint16_t id, uint32_t nverts, uint32_t nidx,
                     const geom_vertex *verts, const uint16_t *idx)
{
    void *vd, *id_;
    int r;
    if ((nverts && !verts) || (nidx && !idx))
        return GPU_ERR_ARG;
    r = geom_x_mesh_begin(id, nverts, nidx, &vd, &id_);
    if (r != GPU_ERR_OK)
        return r;
    if (nverts)
        memcpy(vd, verts, (size_t)nverts * sizeof(geom_vertex));
    if (nidx)
        memcpy(id_, idx, (size_t)nidx * sizeof(uint16_t));
    return geom_x_mesh_commit();
}

/* ---------------------------------------------------------------------------------------- */
/* Per-draw transformed-vertex cache                                                        */
/* ---------------------------------------------------------------------------------------- */

typedef struct {
    float x, y, z, w;           /* clip coordinates */
    float r, g, b;              /* lit colour, clamped to [0,255] */
} cvtx;

static cvtx    g_cv[GEOM_MAX_VERTS];
static uint8_t g_oc[GEOM_MAX_VERTS];    /* bit k: distance to plane k < 0; bit 7: all distances >= 0 */

#define OC_ALL_IN 0x80u
#define OC_PLANES 0x3Fu

static const float GX = 1.35f;
static const float GY = 1.65f;

/* Signed distance to clip plane k, SPEC 6 order: near, far, left, right, bottom, top. */
static float plane_dist(const cvtx *v, int k)
{
    switch (k) {
    case 0:  return v->z + v->w;
    case 1:  return v->w - v->z;
    case 2:  return v->x + GX * v->w;
    case 3:  return GX * v->w - v->x;
    case 4:  return v->y + GY * v->w;
    default: return GY * v->w - v->y;
    }
}

static uint8_t outcode(const cvtx *v)
{
    uint8_t oc = 0;
    int k, all_in = 1;
    for (k = 0; k < 6; k++) {
        float d = plane_dist(v, k);
        if (d < 0.0f)
            oc |= (uint8_t)(1u << k);
        if (!(d >= 0.0f))
            all_in = 0;
    }
    return all_in ? (uint8_t)(oc | OC_ALL_IN) : oc;
}

/* clamp to [0,255], NaN -> 0 */
static float clamp255(float c)
{
    if (!(c > 0.0f))
        return 0.0f;
    if (c > 255.0f)
        return 255.0f;
    return c;
}

static uint8_t col8(float c)
{
    return (uint8_t)(clamp255(c) + 0.5f);
}

/* ---------------------------------------------------------------------------------------- */
/* Clipping                                                                                  */
/* ---------------------------------------------------------------------------------------- */

#define CLIP_MAXV 16    /* a convex triangle clipped by 6 planes has at most 9 vertices */

/* Intersection of edge (in -> out) with plane k: din >= 0, dout < 0 (or NaN). */
static void clip_isect(const cvtx *in, const cvtx *out, float din, float dout, cvtx *p)
{
    float t = din / (din - dout);
    p->x = in->x + t * (out->x - in->x);
    p->y = in->y + t * (out->y - in->y);
    p->z = in->z + t * (out->z - in->z);
    p->w = in->w + t * (out->w - in->w);
    p->r = in->r + t * (out->r - in->r);
    p->g = in->g + t * (out->g - in->g);
    p->b = in->b + t * (out->b - in->b);
}

/* One Sutherland-Hodgman pass against plane k. Returns the new vertex count. */
static int clip_pass(const cvtx *src, int n, cvtx *dst, int k)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const cvtx *c = &src[i];
        const cvtx *nx = &src[(i + 1) % n];
        float dc = plane_dist(c, k);
        float dn = plane_dist(nx, k);
        int cin = dc >= 0.0f, nin = dn >= 0.0f;
        if (cin && m < CLIP_MAXV)
            dst[m++] = *c;
        if (cin != nin && m < CLIP_MAXV) {
            if (cin)
                clip_isect(c, nx, dc, dn, &dst[m++]);
            else
                clip_isect(nx, c, dn, dc, &dst[m++]);
        }
    }
    return m;
}

/* Clip triangle (a,b,c) against all six planes; result in *res (points into bufa or bufb). */
static int clip_tri(const cvtx *a, const cvtx *b, const cvtx *c,
                    cvtx *bufa, cvtx *bufb, cvtx **res)
{
    cvtx *poly = bufa, *tmp = bufb;
    int n = 3, k, i;
    poly[0] = *a;
    poly[1] = *b;
    poly[2] = *c;
    for (k = 0; k < 6 && n >= 3; k++) {
        int any_out = 0;
        for (i = 0; i < n; i++)
            if (!(plane_dist(&poly[i], k) >= 0.0f))
                any_out = 1;
        if (!any_out)
            continue;
        n = clip_pass(poly, n, tmp, k);
        { cvtx *s = poly; poly = tmp; tmp = s; }
    }
    *res = poly;
    return n;
}

/* ---------------------------------------------------------------------------------------- */
/* Projection, setup, frame                                                                  */
/* ---------------------------------------------------------------------------------------- */

static void project(const cvtx *p, gpu_vtx *o)
{
    float iw = 1.0f / p->w;
    o->x = (p->x * iw * 0.5f + 0.5f) * 1280.0f;
    o->y = (0.5f - p->y * iw * 0.5f) * 720.0f;
    o->z = p->z * iw * 0.5f + 0.5f;
    o->r = col8(p->r);
    o->g = col8(p->g);
    o->b = col8(p->b);
    o->a = 0;
}

/* Transform + light every vertex of mesh m for one draw (fills g_cv / g_oc). */
static void transform_mesh(const mesh_slot *m, const float mvp[16], const float mdl[16],
                           int lighting, const float L[3], float ambient, const float mulf[3])
{
    uint32_t i;
    const gvtx *src = &g_verts[m->vbase];
    for (i = 0; i < m->nverts; i++) {
        const gvtx *v = &src[i];
        cvtx *o = &g_cv[i];
        float k = 1.0f;
        o->x = ((mvp[0] * v->px + mvp[4] * v->py) + mvp[8]  * v->pz) + mvp[12];
        o->y = ((mvp[1] * v->px + mvp[5] * v->py) + mvp[9]  * v->pz) + mvp[13];
        o->z = ((mvp[2] * v->px + mvp[6] * v->py) + mvp[10] * v->pz) + mvp[14];
        o->w = ((mvp[3] * v->px + mvp[7] * v->py) + mvp[11] * v->pz) + mvp[15];
        if (lighting) {
            float nx = (mdl[0] * v->nx + mdl[4] * v->ny) + mdl[8]  * v->nz;
            float ny = (mdl[1] * v->nx + mdl[5] * v->ny) + mdl[9]  * v->nz;
            float nz = (mdl[2] * v->nx + mdl[6] * v->ny) + mdl[10] * v->nz;
            float len = sqrtf((nx * nx + ny * ny) + nz * nz);
            float d;
            if (len > 0.0f) {
                nx = nx / len;
                ny = ny / len;
                nz = nz / len;
            }
            d = -((nx * L[0] + ny * L[1]) + nz * L[2]);
            if (d < 0.0f)
                d = 0.0f;
            k = ambient + (1.0f - ambient) * d;
        }
        o->r = clamp255((float)v->r * mulf[0] * k);
        o->g = clamp255((float)v->g * mulf[1] * k);
        o->b = clamp255((float)v->b * mulf[2] * k);
        g_oc[i] = outcode(o);
    }
}

int geom_frame(const geom_frame_hdr *hdr, const geom_draw *draws,
               geom_emit_fn emit, void *user, geom_stats *st)
{
    geom_stats lst;
    float VP[16], L[3], ambient;
    uint32_t rec[GPU_REC_WORDS];
    uint32_t ndraws, di;
    int r;

    if (!st)
        st = &lst;
    memset(st, 0, sizeof *st);
    if (!hdr)
        return GPU_ERR_ARG;
    ndraws = hdr->ndraws;
    if (ndraws && !draws)
        return GPU_ERR_ARG;
    for (di = 0; di < ndraws; di++) {
        geom_draw d;
        memcpy(&d, &draws[di], sizeof d);
        if (!find_mesh(d.mesh_id))
            return GPU_ERR_ARG;         /* nothing emitted yet */
    }
    memcpy(VP, hdr->viewproj, sizeof VP);
    memcpy(L, hdr->light_dir, sizeof L);
    memcpy(&ambient, &hdr->ambient, sizeof ambient);

    for (di = 0; di < ndraws; di++) {
        geom_draw d;
        const mesh_slot *m;
        const uint16_t *ix;
        float M[16], MVP[16], mulf[3];
        uint32_t flags = 0, t, ntri;
        int cull, c, rr;

        memcpy(&d, &draws[di], sizeof d);
        m = find_mesh(d.mesh_id);
        memcpy(M, d.model, sizeof M);
        for (c = 0; c < 4; c++)
            for (rr = 0; rr < 4; rr++)
                MVP[c * 4 + rr] = ((VP[0 * 4 + rr] * M[c * 4 + 0] + VP[1 * 4 + rr] * M[c * 4 + 1])
                                   + VP[2 * 4 + rr] * M[c * 4 + 2]) + VP[3 * 4 + rr] * M[c * 4 + 3];
        for (c = 0; c < 3; c++)
            mulf[c] = (float)d.color_mul[c] / 255.0f;
        if (d.flags & GEOM_DRAW_ZTEST)  flags |= GPU_F_ZTEST;
        if (d.flags & GEOM_DRAW_ZWRITE) flags |= GPU_F_ZWRITE;
        cull = (d.flags & GEOM_DRAW_CULL_BACK) ? GPU_CULL_CW : GPU_CULL_NONE;

        transform_mesh(m, MVP, M, (d.flags & GEOM_DRAW_LIGHTING) != 0, L, ambient, mulf);

        ix = &g_idx[m->ibase];
        ntri = m->nidx / 3u;
        for (t = 0; t < ntri; t++) {
            uint32_t i0 = ix[3 * t], i1 = ix[3 * t + 1], i2 = ix[3 * t + 2];
            uint8_t o0 = g_oc[i0], o1 = g_oc[i1], o2 = g_oc[i2];
            cvtx bufa[CLIP_MAXV], bufb[CLIP_MAXV];
            cvtx tri3[3];
            const cvtx *poly;
            gpu_vtx pv[CLIP_MAXV];
            int n, i, bad;

            st->tris_in++;
            if (o0 & o1 & o2 & OC_PLANES) {             /* all outside one plane */
                st->tris_culled++;
                continue;
            }
            if (o0 & o1 & o2 & OC_ALL_IN) {             /* no clipping needed */
                tri3[0] = g_cv[i0];
                tri3[1] = g_cv[i1];
                tri3[2] = g_cv[i2];
                poly = tri3;
                n = 3;
            } else {
                cvtx *res;
                st->tris_clipped++;
                n = clip_tri(&g_cv[i0], &g_cv[i1], &g_cv[i2], bufa, bufb, &res);
                if (n < 3) {
                    st->tris_culled++;
                    continue;
                }
                bad = 0;
                for (i = 0; i < n; i++)
                    if (res[i].w <= 1e-6f)
                        bad = 1;
                if (bad) {
                    st->tris_culled++;
                    continue;
                }
                poly = res;
            }
            for (i = 0; i < n; i++)
                project(&poly[i], &pv[i]);
            for (i = 1; i + 1 < n; i++) {
                gpu_vtx tv[3];
                tv[0] = pv[0];
                tv[1] = pv[i];
                tv[2] = pv[i + 1];
                if (gpu_setup_tri_noclip(tv, flags, cull, rec) == 1) {
                    if (emit) {
                        r = emit(user, rec);
                        if (r < 0)
                            return r;
                    }
                    st->tris_out++;
                } else {
                    st->tris_culled++;
                }
            }
        }
    }

    gpu_make_end(hdr->frame_no, rec);
    if (emit) {
        r = emit(user, rec);
        if (r < 0)
            return r;
    }
    return GPU_ERR_OK;
}

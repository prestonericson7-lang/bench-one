/*
 * gpu_setup.c -- triangle / rect / sprite setup (SPEC.md section 4, exact).
 *
 * Portable C99 + <math.h> (llrint, isfinite). No FMA contraction anywhere: the pragma below plus
 * -ffp-contract=off on the command line. With IEEE-754 binary64 doubles, round-to-nearest-even
 * and no contraction, every target (x86-64 SSE2, AArch64, ARMv7 VFP, Cortex-M7 FPv5-D16)
 * produces bit-identical records.
 *
 * No undefined behaviour for any input: float coordinates are range-checked before llrint,
 * all products that can exceed 32 bits are done in int64, every value stored into a record is
 * range-checked; gradients are clamped exactly as the spec says; attribute START values are
 * stored modulo 2^32 (see round_wrap_u32 -- deliberate, reported deviation from SPEC 4.6).
 *
 * gpu_setup_tri (guard-band clipper) details beyond the spec text:
 *  - a vertex is inside a clip plane iff its float distance d >= 0; the edge intersection is
 *    always computed from the inside vertex towards the outside one, so neighbouring triangles
 *    get bit-identical clip vertices (watertight); the coordinate on the plane is set exactly;
 *  - after clipping, any vertex that snaps outside the guard band is clamped inside (this can
 *    only move vertices within 1/32 px of x=1536 / y=976, or rounding strays);
 *  - facing (culling) is decided once on the original float triangle; fan pieces whose snapped
 *    orientation flipped (sub-pixel slivers) are dropped, never drawn back-to-front;
 *  - NaN/Inf x or y -> 0 records; a triangle needing no clipping gives exactly the noclip record.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "gpu_setup.h"

/* |x*16| must stay below 2^29 so that coordinate differences (< 2^30) and the int64 area2
 * (< 2^61) cannot overflow. Anything beyond that is far outside the guard band anyway. */
#define SNAP_LIMIT 536870912.0

#define GB_XMIN16 (GPU_GUARD_XMIN * 16)   /* -4096, inclusive */
#define GB_XMAX16 (GPU_GUARD_XMAX * 16)   /* 24576, exclusive */
#define GB_YMIN16 (GPU_GUARD_YMIN * 16)   /* -4096, inclusive */
#define GB_YMAX16 (GPU_GUARD_YMAX * 16)   /* 15616, exclusive */

/* Snap a float pixel coordinate to 1/16 px: (int32)llrint((double)f * 16).
 * Returns 0 (and leaves *o untouched) for NaN/Inf/huge values. */
static int snap16(float f, int32_t *o)
{
    double d = (double)f * 16.0;
    if (!(d > -SNAP_LIMIT && d < SNAP_LIMIT))
        return 0;
    *o = (int32_t)llrint(d);
    return 1;
}

/* clamp(z,0,1) in float, NaN -> 0 */
static float clamp01f(float z)
{
    if (!(z > 0.0f))
        return 0.0f;
    if (z > 1.0f)
        return 1.0f;
    return z;
}

/* Z_i = (double)clamp(z_i,0,1) * 65535.0 * 4096.0 (evaluated left to right) */
static double z_fixed(float z)
{
    return (double)clamp01f(z) * 65535.0 * 4096.0;
}

/* (int32)clamp(llrint(v), INT32_MIN, INT32_MAX) without ever calling llrint out of range. */
static int32_t round_clamp_i32(double v)
{
    if (v != v)
        return 0;                         /* NaN: cannot happen for valid setups */
    if (v >= 2147483647.0)
        return INT32_MAX;
    if (v <= -2147483648.0)
        return INT32_MIN;
    return (int32_t)llrint(v);
}

/* Start value (value at the bbox corner px_min,py_min), stored MODULO 2^32 (not clamped).
 * The corner can be far outside the triangle, so with a steep gradient the extrapolated start can
 * exceed int32 even though every covered pixel's value is in range. The renderer evaluates
 * v0 + dvdx*dx + dvdy*dy modulo 2^32 (SPEC 5), so a wrapped start yields the exact value at every
 * covered pixel, whereas a clamped start corrupts the whole triangle.  (Deviation from the
 * literal text of SPEC 4.6, which says clamp; identical whenever the start fits in int32.)
 * |v| < ~1e18 always holds (|gradient| <= 16*2*2^28*2^15 / 1, times <= 2^11 pixels), so llrint
 * is in range; the guard below only protects against impossible inputs. */
static uint32_t round_wrap_u32(double v)
{
    if (!(v > -4.0e18 && v < 4.0e18))
        return 0;
    return (uint32_t)llrint(v);           /* int64 -> uint32: modulo 2^32, well defined */
}

/* True floor / ceil division by 16 (C99 '/' truncates toward zero). */
static int32_t floor_div16(int32_t a)
{
    int32_t q = a / 16;
    if ((a % 16) != 0 && a < 0)
        q -= 1;
    return q;
}

static int32_t ceil_div16(int32_t a)
{
    int32_t q = a / 16;
    if ((a % 16) != 0 && a > 0)
        q += 1;
    return q;
}

/* Edge a->b (SPEC 4.5). Writes A, B, E into w[0..2]. Returns 0 if E does not fit in int32
 * (impossible inside the guard band; kept as a hard guard against UB/garbage). */
static int setup_edge(int32_t Xa, int32_t Ya, int32_t Xb, int32_t Yb,
                      int32_t px_min, int32_t py_min, uint32_t *w)
{
    int32_t A = -16 * (Yb - Ya);
    int32_t B = 16 * (Xb - Xa);
    int64_t E = (int64_t)(Xb - Xa) * (int64_t)(16 * py_min + 8 - Ya)
              - (int64_t)(Yb - Ya) * (int64_t)(16 * px_min + 8 - Xa);
    if (!(A > 0 || (A == 0 && B > 0)))
        E -= 1;
    if (E < (int64_t)INT32_MIN || E > (int64_t)INT32_MAX)
        return 0;
    w[0] = (uint32_t)A;
    w[1] = (uint32_t)B;
    w[2] = (uint32_t)E;       /* modulo 2^32, value already known to fit int32 */
    return 1;
}

/* Gradients of one attribute (SPEC 4.6). V are the fixed-point values after the swap. */
static void setup_grad(const double V[3], const int32_t X[3], const int32_t Y[3], int64_t area2,
                       int32_t px_min, int32_t py_min, uint32_t *w)
{
    double a    = (double)area2;
    double dx10 = (double)(X[1] - X[0]);
    double dx20 = (double)(X[2] - X[0]);
    double dy10 = (double)(Y[1] - Y[0]);
    double dy20 = (double)(Y[2] - Y[0]);
    double dvdx, dvdy, vstart;

    dvdx = 16.0 * ((V[1] - V[0]) * dy20 - (V[2] - V[0]) * dy10) / a;
    dvdy = 16.0 * ((V[2] - V[0]) * dx10 - (V[1] - V[0]) * dx20) / a;
    vstart = V[0] + dvdx * (double)(16 * px_min + 8 - X[0]) / 16.0
                  + dvdy * (double)(16 * py_min + 8 - Y[0]) / 16.0;

    w[0] = round_wrap_u32(vstart);                 /* modulo 2^32, see round_wrap_u32 */
    w[1] = (uint32_t)round_clamp_i32(dvdx);
    w[2] = (uint32_t)round_clamp_i32(dvdy);
}

int gpu_setup_tri_noclip(const gpu_vtx v[3], uint32_t flags, int cull, uint32_t out[GPU_REC_WORDS])
{
    int32_t X[3], Y[3];
    double Zv[3], Rv[3], Gv[3], Bv[3];
    int64_t area2;
    int32_t minX, maxX, minY, maxY, px_min, px_max, py_min, py_max;
    uint32_t rec[GPU_REC_WORDS];
    int i;

    /* 1. snap (NaN / Inf / absurdly large coordinates are outside the guard band) */
    for (i = 0; i < 3; i++) {
        if (!snap16(v[i].x, &X[i]) || !snap16(v[i].y, &Y[i]))
            return -1;
        Zv[i] = z_fixed(v[i].z);
        Rv[i] = (double)v[i].r * 65536.0;
        Gv[i] = (double)v[i].g * 65536.0;
        Bv[i] = (double)v[i].b * 65536.0;
    }

    /* 2. area, cull, swap */
    area2 = (int64_t)(X[1] - X[0]) * (int64_t)(Y[2] - Y[0])
          - (int64_t)(X[2] - X[0]) * (int64_t)(Y[1] - Y[0]);
    if (area2 == 0)
        return 0;
    if (cull == GPU_CULL_CW && area2 > 0)
        return 0;
    if (cull == GPU_CULL_CCW && area2 < 0)
        return 0;
    if (area2 < 0) {
        int32_t ti; double td;
        ti = X[1]; X[1] = X[2]; X[2] = ti;
        ti = Y[1]; Y[1] = Y[2]; Y[2] = ti;
        td = Zv[1]; Zv[1] = Zv[2]; Zv[2] = td;
        td = Rv[1]; Rv[1] = Rv[2]; Rv[2] = td;
        td = Gv[1]; Gv[1] = Gv[2]; Gv[2] = td;
        td = Bv[1]; Bv[1] = Bv[2]; Bv[2] = td;
        area2 = -area2;
    }

    /* 3. guard band */
    for (i = 0; i < 3; i++) {
        if (X[i] < GB_XMIN16 || X[i] >= GB_XMAX16 || Y[i] < GB_YMIN16 || Y[i] >= GB_YMAX16)
            return -1;
    }

    /* 4. bounding box of pixel centres, intersected with the screen */
    minX = maxX = X[0];
    minY = maxY = Y[0];
    for (i = 1; i < 3; i++) {
        if (X[i] < minX) minX = X[i];
        if (X[i] > maxX) maxX = X[i];
        if (Y[i] < minY) minY = Y[i];
        if (Y[i] > maxY) maxY = Y[i];
    }
    px_min = ceil_div16(minX - 8);
    px_max = floor_div16(maxX - 8);
    py_min = ceil_div16(minY - 8);
    py_max = floor_div16(maxY - 8);
    if (px_min < 0) px_min = 0;
    if (py_min < 0) py_min = 0;
    if (px_max > GPU_W - 1) px_max = GPU_W - 1;
    if (py_max > GPU_H - 1) py_max = GPU_H - 1;
    if (px_min > px_max || py_min > py_max)
        return 0;

    /* 5. edges: edge0 = v1->v2, edge1 = v2->v0, edge2 = v0->v1 */
    memset(rec, 0, sizeof rec);
    rec[0] = (GPU_REC_TRI << 28) | (flags & (GPU_F_ZTEST | GPU_F_ZWRITE))
           | ((uint32_t)px_max << 11) | (uint32_t)px_min;
    rec[1] = ((uint32_t)py_max << 11) | (uint32_t)py_min;
    if (!setup_edge(X[1], Y[1], X[2], Y[2], px_min, py_min, &rec[2]) ||
        !setup_edge(X[2], Y[2], X[0], Y[0], px_min, py_min, &rec[5]) ||
        !setup_edge(X[0], Y[0], X[1], Y[1], px_min, py_min, &rec[8]))
        return -1;   /* unreachable inside the guard band */

    /* 6. gradients */
    setup_grad(Zv, X, Y, area2, px_min, py_min, &rec[11]);
    setup_grad(Rv, X, Y, area2, px_min, py_min, &rec[14]);
    setup_grad(Gv, X, Y, area2, px_min, py_min, &rec[17]);
    setup_grad(Bv, X, Y, area2, px_min, py_min, &rec[20]);
    rec[23] = 0;

    memcpy(out, rec, sizeof rec);
    return 1;
}

/* ---------------------------------------------------------------------------------------- */
/* Guard-band clipper                                                                        */
/* ---------------------------------------------------------------------------------------- */

#define CLIP_MAXV 16   /* a triangle clipped by 4 planes has at most 7 vertices */

/* Signed distance to clip plane k (>= 0 = inside), planes in SPEC order:
 * x = -256 (keep x >= -256), x = 1536 (keep x <= 1536), y = -256, y = 976. */
static double plane_dist(const gpu_vtx *p, int k)
{
    switch (k) {
    case 0:  return (double)p->x - (double)GPU_GUARD_XMIN;
    case 1:  return (double)GPU_GUARD_XMAX - (double)p->x;
    case 2:  return (double)p->y - (double)GPU_GUARD_YMIN;
    default: return (double)GPU_GUARD_YMAX - (double)p->y;
    }
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, double t)
{
    long long c = llrint((double)a + t * ((double)b - (double)a));   /* t in [0,1): finite, small */
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    return (uint8_t)c;
}

static float lerp_f(float a, float b, double t)
{
    return (float)((double)a + t * ((double)b - (double)a));
}

/* Intersection of the edge between an inside vertex `in` (din >= 0) and an outside vertex
 * `out` (dout < 0) with plane k. ALWAYS called with (inside, outside) in that order, whatever
 * direction the polygon traverses the edge, so two triangles sharing an edge get bit-identical
 * clip vertices (watertightness). t = d0/(d0-d1) in double; the coordinate normal to the plane
 * is set exactly to the plane value. */
static void clip_vertex(const gpu_vtx *in, const gpu_vtx *out, double din, double dout, int k,
                        gpu_vtx *p)
{
    double t = din / (din - dout);        /* din >= 0 > dout, so 0 <= t < 1 */
    p->x = lerp_f(in->x, out->x, t);
    p->y = lerp_f(in->y, out->y, t);
    p->z = lerp_f(in->z, out->z, t);
    p->r = lerp_u8(in->r, out->r, t);
    p->g = lerp_u8(in->g, out->g, t);
    p->b = lerp_u8(in->b, out->b, t);
    p->a = in->a;
    switch (k) {
    case 0:  p->x = (float)GPU_GUARD_XMIN; break;
    case 1:  p->x = (float)GPU_GUARD_XMAX; break;
    case 2:  p->y = (float)GPU_GUARD_YMIN; break;
    default: p->y = (float)GPU_GUARD_YMAX; break;
    }
}

/* One Sutherland-Hodgman pass. Returns the new vertex count. */
static int clip_pass(const gpu_vtx *src, int n, gpu_vtx *dst, int k)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const gpu_vtx *c = &src[i];
        const gpu_vtx *nx = &src[(i + 1) % n];
        double dc = plane_dist(c, k);
        double dn = plane_dist(nx, k);
        int cin = dc >= 0.0, nin = dn >= 0.0;
        if (cin && m < CLIP_MAXV)
            dst[m++] = *c;
        if (cin != nin && m < CLIP_MAXV) {
            if (cin)
                clip_vertex(c, nx, dc, dn, k, &dst[m++]);
            else
                clip_vertex(nx, c, dn, dc, k, &dst[m++]);
        }
    }
    return m;
}

/* Clamp a (clipped) vertex so that its snapped position is inside the guard band.
 * Depends only on the vertex itself (consistent between neighbouring triangles).
 * Returns 1 if the vertex was changed. */
static int nudge_inside(gpu_vtx *p)
{
    int32_t X, Y;
    int changed = 0;
    if (!snap16(p->x, &X) || !snap16(p->y, &Y))
        return 0;   /* cannot happen after clipping (|coords| <= ~1536) */
    if (X < GB_XMIN16) { p->x = (float)GB_XMIN16 / 16.0f; changed = 1; }
    if (X >= GB_XMAX16) { p->x = (float)(GB_XMAX16 - 1) / 16.0f; changed = 1; }
    if (Y < GB_YMIN16) { p->y = (float)GB_YMIN16 / 16.0f; changed = 1; }
    if (Y >= GB_YMAX16) { p->y = (float)(GB_YMAX16 - 1) / 16.0f; changed = 1; }
    return changed;
}

int gpu_setup_tri(const gpu_vtx v[3], uint32_t flags, int cull,
                  uint32_t out[][GPU_REC_WORDS], int max_out)
{
    gpu_vtx bufa[CLIP_MAXV], bufb[CLIP_MAXV];
    gpu_vtx *poly = bufa, *tmp = bufb;
    int n = 3, k, i, cnt, modified = 0, piece_cull;
    double ao;

    if (max_out <= 0)
        return 0;
    for (i = 0; i < 3; i++) {
        if (!isfinite(v[i].x) || !isfinite(v[i].y))
            return 0;
        poly[i] = v[i];
    }

    for (k = 0; k < 4; k++) {
        int any_out = 0;
        for (i = 0; i < n; i++)
            if (plane_dist(&poly[i], k) < 0.0)
                any_out = 1;
        if (!any_out)
            continue;
        modified = 1;
        n = clip_pass(poly, n, tmp, k);
        { gpu_vtx *s = poly; poly = tmp; tmp = s; }
        if (n < 3)
            return 0;
    }
    for (i = 0; i < n; i++)
        modified |= nudge_inside(&poly[i]);

    if (!modified) {
        /* The triangle lies inside the guard band: identical to the noclip variant. */
        return gpu_setup_tri_noclip(v, flags, cull, out[0]) == 1 ? 1 : 0;
    }

    /* Facing is decided once, on the original triangle; fan pieces whose snapped orientation
     * flipped (sub-pixel slivers) are dropped rather than drawn back-to-front. */
    ao = ((double)v[1].x - (double)v[0].x) * ((double)v[2].y - (double)v[0].y)
       - ((double)v[2].x - (double)v[0].x) * ((double)v[1].y - (double)v[0].y);
    if (ao == 0.0 || ao != ao)
        return 0;
    if (cull == GPU_CULL_CW && ao > 0.0)
        return 0;
    if (cull == GPU_CULL_CCW && ao < 0.0)
        return 0;
    piece_cull = ao > 0.0 ? GPU_CULL_CCW : GPU_CULL_CW;

    cnt = 0;
    for (i = 1; i + 1 < n && cnt < max_out; i++) {
        gpu_vtx t[3];
        t[0] = poly[0];
        t[1] = poly[i];
        t[2] = poly[i + 1];
        if (gpu_setup_tri_noclip(t, flags, piece_cull, out[cnt]) == 1)
            cnt++;
    }
    return cnt;
}

/* ---------------------------------------------------------------------------------------- */

int gpu_setup_rect(int x0, int y0, int x1, int y1, uint16_t rgb565, float z, uint32_t flags,
                   uint32_t out[GPU_REC_WORDS])
{
    int64_t xmin = x0 > 0 ? x0 : 0;
    int64_t ymin = y0 > 0 ? y0 : 0;
    int64_t xmax = (int64_t)(x1 < GPU_W ? x1 : GPU_W) - 1;
    int64_t ymax = (int64_t)(y1 < GPU_H ? y1 : GPU_H) - 1;
    uint32_t r5, g6, b5, r8, g8, b8;
    int32_t z0;

    if (xmin > xmax || ymin > ymax)
        return 0;

    r5 = (rgb565 >> 11) & 31u;
    g6 = (rgb565 >> 5) & 63u;
    b5 = rgb565 & 31u;
    r8 = (r5 << 3) | (r5 >> 2);
    g8 = (g6 << 2) | (g6 >> 4);
    b8 = (b5 << 3) | (b5 >> 2);
    z0 = (int32_t)llrint((double)clamp01f(z) * 65535.0 * 4096.0);   /* 0..268431360 */

    memset(out, 0, GPU_REC_WORDS * sizeof(uint32_t));
    out[0] = (GPU_REC_TRI << 28) | GPU_F_NOEDGE | (flags & (GPU_F_ZTEST | GPU_F_ZWRITE))
           | ((uint32_t)xmax << 11) | (uint32_t)xmin;
    out[1] = ((uint32_t)ymax << 11) | (uint32_t)ymin;
    out[11] = (uint32_t)z0;
    out[14] = r8 << 16;
    out[17] = g8 << 16;
    out[20] = b8 << 16;
    return 1;
}

int gpu_setup_sprite(int x, int y, int w, int h, uint32_t src_addr, uint32_t stride,
                     int colorkey_en, uint16_t colorkey, uint32_t out[GPU_REC_WORDS])
{
    int64_t X = x, Y = y, Wd = w, Ht = h;
    uint32_t src = src_addr;

    if ((x % 4) != 0 || (w % 4) != 0 || w < 4 || h < 1 || (src_addr % 8u) != 0 || (stride % 8u) != 0)
        return -1;
    if (Y < 0) {
        if (Ht + Y <= 0)
            return 0;                                   /* fully above the screen */
        src += (uint32_t)(-Y) * stride;                 /* modulo 2^32, well defined */
        Ht += Y;
        Y = 0;
    }
    if (X < 0) {
        if (Wd + X <= 0)
            return 0;                                   /* fully left of the screen */
        src += (uint32_t)(-X) * 2u;
        Wd += X;
        X = 0;
    }
    if (X >= GPU_W || Y >= GPU_H || Wd <= 0 || Ht <= 0)
        return 0;
    if (Wd > GPU_W) Wd = GPU_W;
    if (Ht > GPU_H) Ht = GPU_H;

    memset(out, 0, GPU_REC_WORDS * sizeof(uint32_t));
    out[0] = (GPU_REC_SPRITE << 28) | (colorkey_en ? GPU_F_COLORKEY : 0u);
    out[1] = ((uint32_t)Y << 11) | (uint32_t)X;
    out[2] = ((uint32_t)Ht << 11) | (uint32_t)Wd;
    out[3] = src;
    out[4] = stride;
    out[5] = colorkey;
    return 1;
}

void gpu_make_end(uint32_t frame_no, uint32_t out[GPU_REC_WORDS])
{
    memset(out, 0, GPU_REC_WORDS * sizeof(uint32_t));
    out[0] = GPU_REC_END << 28;
    out[1] = frame_no;
}

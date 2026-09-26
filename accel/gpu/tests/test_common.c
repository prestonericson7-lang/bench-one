/*
 * test_common.c -- unit tests for common/gpu_setup.c and common/gpu_refrast.c.
 *
 * Coverage is checked with a test-only counter that applies the SPEC 5 coverage formula to the
 * produced records (in int64, so it also proves no int32 wrap-around happens), and against an
 * independent geometric model (exact integer edge functions of the snapped vertices + the
 * top-left rule). gpu_refrast_frame is checked against a literal, full-frame transcription of
 * SPEC 5 on random record lists.
 *
 * Prints one line per test and a hash of every record produced by the deterministic tests, so the
 * same binary built for x86-64 / AArch64 / ARMv7 can be compared bit-for-bit.
 * Exit status 0 = all checks passed.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_setup.h"
#include "gpu_refrast.h"

#define W  GPU_W
#define H  GPU_H
#define RW GPU_REC_WORDS

/* ------------------------------------------------------------------------------------------ */
/* harness                                                                                     */
/* ------------------------------------------------------------------------------------------ */
static long g_checks, g_fails, g_test_fails_at_start;
static const char *g_test = "";

#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        g_checks++;                                                                          \
        if (!(cond)) {                                                                       \
            g_fails++;                                                                       \
            if (g_fails <= 60) {                                                             \
                printf("  FAIL [%s] line %d: ", g_test, __LINE__);                           \
                printf(__VA_ARGS__);                                                         \
                printf("\n");                                                                \
            }                                                                                \
        }                                                                                    \
    } while (0)

static long g_test_checks_at_start;
static void begin(const char *name)
{
    g_test = name;
    g_test_checks_at_start = g_checks;
    g_test_fails_at_start = g_fails;
}
static void end_test(const char *extra)
{
    printf("%-26s %9ld checks  %s%s%s\n", g_test, g_checks - g_test_checks_at_start,
           g_fails == g_test_fails_at_start ? "PASS" : "FAIL",
           extra ? "  " : "", extra ? extra : "");
    fflush(stdout);
}

/* record hash (FNV-1a 64) over every record produced by deterministic tests */
static uint64_t g_hash = 1469598103934665603ull;
static long g_hashed;
static void hash_rec(const uint32_t *r)
{
    int i, b;
    for (i = 0; i < RW; i++)
        for (b = 0; b < 4; b++) {
            g_hash ^= (r[i] >> (8 * b)) & 0xFFu;
            g_hash *= 1099511628211ull;
        }
    g_hashed++;
}

/* frame hash over every frame rendered by the refrast-vs-naive test */
static uint64_t g_fhash = 1469598103934665603ull;
static void hash_frame(const uint16_t *fb)
{
    int i;
    for (i = 0; i < GPU_W * GPU_H; i++) {
        g_fhash ^= fb[i];
        g_fhash *= 1099511628211ull;
    }
}

/* deterministic PRNG (splitmix64) */
static uint64_t g_rs;
static void rseed(uint64_t s) { g_rs = s; }
static uint64_t rnext(void)
{
    uint64_t z = (g_rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double runif(void) { return (double)(rnext() >> 11) * (1.0 / 9007199254740992.0); }
static int rint_in(int lo, int hi) { return lo + (int)(rnext() % (uint64_t)(hi - lo + 1)); }

/* coordinate quantisation modes: 0 pixel centre, 1 pixel edge, 2 1/16 grid, 3 1/256, 4 raw */
static float quant(double v, int mode)
{
    switch (mode) {
    case 0:  return (float)(floor(v) + 0.5);
    case 1:  return (float)floor(v);
    case 2:  return (float)(floor(v * 16.0) / 16.0);
    case 3:  return (float)(floor(v * 256.0) / 256.0);
    default: return (float)v;
    }
}

static int s32(uint32_t u)   /* int is 32-bit on every target; int so printf %d is portable */
{
    if (u <= 0x7FFFFFFFu)
        return (int32_t)u;
    return (int32_t)(u - 0x80000000u) - 0x7FFFFFFF - 1;
}

static gpu_vtx mkv(float x, float y, float z, int r, int g, int b)
{
    gpu_vtx v;
    v.x = x; v.y = y; v.z = z;
    v.r = (uint8_t)r; v.g = (uint8_t)g; v.b = (uint8_t)b; v.a = 0;
    return v;
}

/* C leaves the evaluation order of function arguments unspecified, so the PRNG is never called
 * twice within one expression/argument list (else the inputs, and the record hash compared
 * across compilers/ISAs, would depend on the compiler). Random colours are drawn here, in order. */
static gpu_vtx mkv_rgb(float x, float y, float z)
{
    int r = (int)(rnext() & 255);
    int g = (int)(rnext() & 255);
    int b = (int)(rnext() & 255);
    return mkv(x, y, z, r, g, b);
}

/* record field decode */
#define R_XMIN(r) ((int)((r)[0] & 0x7FFu))
#define R_XMAX(r) ((int)(((r)[0] >> 11) & 0x7FFu))
#define R_YMIN(r) ((int)((r)[1] & 0x7FFu))
#define R_YMAX(r) ((int)(((r)[1] >> 11) & 0x7FFu))

/* ------------------------------------------------------------------------------------------ */
/* test-only coverage counter (SPEC 5 coverage formula, int64, with int32 range check)          */
/* ------------------------------------------------------------------------------------------ */
static uint16_t g_cnt[W * H];
static long g_range_viol;

static void count_record(const uint32_t *r)
{
    int x, y, i, xmin, xmax, ymin, ymax, noedge;
    if (GPU_W0_TYPE(r[0]) != GPU_REC_TRI)
        return;
    xmin = R_XMIN(r); xmax = R_XMAX(r); ymin = R_YMIN(r); ymax = R_YMAX(r);
    CHECK(xmin <= xmax && ymin <= ymax && xmax < W && ymax < H,
          "bbox not inside screen: x %d..%d y %d..%d", xmin, xmax, ymin, ymax);
    if (xmax >= W) xmax = W - 1;
    if (ymax >= H) ymax = H - 1;
    noedge = (r[0] & GPU_F_NOEDGE) != 0;
    for (y = ymin; y <= ymax; y++) {
        int64_t dy = y - ymin;
        for (x = xmin; x <= xmax; x++) {
            int64_t dx = x - xmin;
            int cov = 1;
            if (!noedge) {
                for (i = 0; i < 3; i++) {
                    int64_t e = (int64_t)s32(r[4 + 3 * i]) + (int64_t)s32(r[2 + 3 * i]) * dx
                              + (int64_t)s32(r[3 + 3 * i]) * dy;
                    if (e < INT32_MIN || e > INT32_MAX)
                        g_range_viol++;
                    if (e < 0)
                        cov = 0;
                }
            }
            if (cov)
                g_cnt[y * W + x]++;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* independent geometric model                                                                  */
/* ------------------------------------------------------------------------------------------ */
/* snap + orient CW (spec area2 > 0). Returns sign of the original area2 (0 = degenerate). */
static int snap_tri(const gpu_vtx v[3], int32_t X[3], int32_t Y[3], int perm[3])
{
    int i, sgn;
    int64_t a;
    for (i = 0; i < 3; i++) {
        X[i] = (int32_t)llrint((double)v[i].x * 16.0);
        Y[i] = (int32_t)llrint((double)v[i].y * 16.0);
        perm[i] = i;
    }
    a = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(X[2] - X[0]) * (Y[1] - Y[0]);
    sgn = a > 0 ? 1 : a < 0 ? -1 : 0;
    if (a < 0) {
        int32_t t;
        t = X[1]; X[1] = X[2]; X[2] = t;
        t = Y[1]; Y[1] = Y[2]; Y[2] = t;
        perm[1] = 2; perm[2] = 1;
    }
    return sgn;
}

static int topleft(int64_t dxab, int64_t dyab)
{
    int64_t A = -dyab, B = dxab;
    return A > 0 || (A == 0 && B > 0);
}

/* pixel centre inside the (CW) snapped triangle per the top-left rule */
static int geo_cov(const int32_t X[3], const int32_t Y[3], int px, int py)
{
    int64_t cx = 16 * (int64_t)px + 8, cy = 16 * (int64_t)py + 8;
    int e;
    for (e = 0; e < 3; e++) {
        int a = (e + 1) % 3, b = (e + 2) % 3;
        int64_t f = (int64_t)(X[b] - X[a]) * (cy - Y[a]) - (int64_t)(Y[b] - Y[a]) * (cx - X[a]);
        if (topleft(X[b] - X[a], Y[b] - Y[a]) ? f < 0 : f <= 0)
            return 0;
    }
    return 1;
}

/* record coverage at one pixel, SPEC 5 with uint32 wrap arithmetic */
static int rec_cov(const uint32_t *r, int px, int py)
{
    uint32_t dx, dy;
    int i;
    if (px < R_XMIN(r) || px > R_XMAX(r) || py < R_YMIN(r) || py > R_YMAX(r))
        return 0;
    if (r[0] & GPU_F_NOEDGE)
        return 1;
    dx = (uint32_t)(px - R_XMIN(r));
    dy = (uint32_t)(py - R_YMIN(r));
    for (i = 0; i < 3; i++)
        if (s32(r[4 + 3 * i] + r[2 + 3 * i] * dx + r[3 + 3 * i] * dy) < 0)
            return 0;
    return 1;
}

/* attribute value (channel word index w = 11 z, 14 r, 17 g, 20 b) at a pixel exactly as the
 * hardware computes it: v0 + dvdx*dx + dvdy*dy modulo 2^32, read as int32 */
static int64_t rec_attr64(const uint32_t *r, int w, int px, int py)
{
    uint32_t dx = (uint32_t)(px - R_XMIN(r)), dy = (uint32_t)(py - R_YMIN(r));
    return (int64_t)s32(r[w] + r[w + 1] * dx + r[w + 2] * dy);
}

/* exact plane-equation value of attribute ch (0 z, 1 r, 2 g, 3 b) at subpixel point (cx,cy),
 * from the snapped vertices (independent of gpu_setup's arithmetic) */
static long double exact_attr(const gpu_vtx v[3], int ch, double cx, double cy)
{
    int32_t X[3], Y[3];
    int perm[3], i;
    double V[3];
    int64_t area2;
    long double a, b;
    snap_tri(v, X, Y, perm);
    area2 = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(X[2] - X[0]) * (Y[1] - Y[0]);
    for (i = 0; i < 3; i++) {
        const gpu_vtx *q = &v[perm[i]];
        float z = q->z > 0 ? (q->z > 1 ? 1.0f : q->z) : 0.0f;
        V[i] = ch == 0 ? (double)z * 65535.0 * 4096.0 : (double)(ch == 1 ? q->r : ch == 2 ? q->g : q->b) * 65536.0;
    }
    a = ((long double)(V[1] - V[0]) * (Y[2] - Y[0]) - (long double)(V[2] - V[0]) * (Y[1] - Y[0])) / (long double)area2;
    b = ((long double)(V[2] - V[0]) * (X[1] - X[0]) - (long double)(V[1] - V[0]) * (X[2] - X[0])) / (long double)area2;
    return V[0] + a * (cx - X[0]) + b * (cy - Y[0]);
}

/* 1 if any gradient word is saturated (extreme sliver: values inside it are not exact) */
static int grad_clamped(const uint32_t *r)
{
    static const int gw[8] = { 12, 13, 15, 16, 18, 19, 21, 22 };
    int i;
    for (i = 0; i < 8; i++)
        if (s32(r[gw[i]]) == INT32_MAX || s32(r[gw[i]]) == INT32_MIN)
            return 1;
    return 0;
}

static int c8_of(int64_t c)
{
    int32_t v = (int32_t)c;   /* callers pass in-range values */
    if (v < 0) return 0;
    v >>= 16;
    return v > 255 ? 255 : v;
}

/* ------------------------------------------------------------------------------------------ */
/* literal full-frame transcription of SPEC 5 (reference for gpu_refrast_frame)                 */
/* ------------------------------------------------------------------------------------------ */
static uint16_t g_nzb[W * H];

static void naive_frame(const uint32_t *recs, int n, uint16_t clear, gpu_ddr_read16_fn rd,
                        void *user, uint16_t *fb)
{
    int i, k;
    for (k = 0; k < W * H; k++) { fb[k] = clear; g_nzb[k] = 0xFFFF; }
    for (i = 0; i < n; i++) {
        const uint32_t *r = recs + (size_t)i * RW;
        uint32_t type = r[0] >> 28;
        if (type == GPU_REC_TRI) {
            int x, y;
            int xmin = R_XMIN(r), xmax = R_XMAX(r), ymin = R_YMIN(r), ymax = R_YMAX(r);
            for (y = ymin; y <= ymax && y < H; y++)
                for (x = xmin; x <= xmax && x < W; x++) {
                    uint32_t dx = (uint32_t)(x - xmin), dy = (uint32_t)(y - ymin);
                    int covered = (r[0] & GPU_F_NOEDGE) ||
                                  (s32(r[4] + r[2] * dx + r[3] * dy) >= 0 &&
                                   s32(r[7] + r[5] * dx + r[6] * dy) >= 0 &&
                                   s32(r[10] + r[8] * dx + r[9] * dy) >= 0);
                    int32_t z, c[3];
                    int zp, pass, ch, c8[3];
                    if (!covered) continue;
                    z = s32(r[11] + r[12] * dx + r[13] * dy);
                    zp = z < 0 ? 0 : (z >> 12) > 65535 ? 65535 : (z >> 12);
                    pass = !(r[0] & GPU_F_ZTEST) || zp <= g_nzb[y * W + x];
                    if (!pass) continue;
                    if (r[0] & GPU_F_ZWRITE) g_nzb[y * W + x] = (uint16_t)zp;
                    for (ch = 0; ch < 3; ch++) {
                        c[ch] = s32(r[14 + 3 * ch] + r[15 + 3 * ch] * dx + r[16 + 3 * ch] * dy);
                        c8[ch] = c[ch] < 0 ? 0 : (c[ch] >> 16) > 255 ? 255 : (c[ch] >> 16);
                    }
                    fb[y * W + x] = (uint16_t)(((c8[0] >> 3) << 11) | ((c8[1] >> 2) << 5) | (c8[2] >> 3));
                }
        } else if (type == GPU_REC_SPRITE) {
            int sx = (int)(r[1] & 0x7FF), sy = (int)((r[1] >> 11) & 0x7FF);
            int sw = (int)(r[2] & 0x7FF), sh = (int)((r[2] >> 11) & 0x7FF);
            int row, col;
            for (row = 0; row < sh; row++) {
                if (sy + row >= H) continue;
                for (col = 0; col < sw; col++) {
                    uint16_t p;
                    if (sx + col >= W) continue;
                    p = rd(user, r[3] + (uint32_t)row * r[4] + (uint32_t)col * 2u);
                    if ((r[0] & GPU_F_COLORKEY) && p == (uint16_t)(r[5] & 0xFFFF)) continue;
                    fb[(sy + row) * W + sx + col] = p;
                }
            }
        }
    }
}

/* hashed DDR model: frequent occurrences of 0xF81F so colour keys matter */
static uint16_t hash_ddr(void *user, uint32_t addr)
{
    uint32_t h = addr * 2654435761u;
    (void)user;
    h ^= h >> 13;
    h *= 0x85EBCA6Bu;
    h ^= h >> 16;
    if ((h & 7u) == 0) return 0xF81Fu;
    return (uint16_t)(h >> 8);
}

static uint16_t g_fb[W * H], g_fb2[W * H];

/* ------------------------------------------------------------------------------------------ */
/* tests                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static void test_known_record(void)
{
    gpu_vtx v[3], w[3];
    uint32_t r[RW], r2[RW];
    int ret;
    begin("known_record");
    v[0] = mkv(10.5f, 10.5f, 0.25f, 0, 10, 200);
    v[1] = mkv(20.5f, 10.5f, 0.25f, 160, 10, 200);
    v[2] = mkv(10.5f, 20.5f, 0.25f, 0, 10, 200);
    ret = gpu_setup_tri_noclip(v, GPU_F_ZTEST | GPU_F_ZWRITE | GPU_F_NOEDGE | 0xF0000000u,
                               GPU_CULL_NONE, r);
    CHECK(ret == 1, "ret %d", ret);
    CHECK(r[0] == ((1u << 28) | GPU_F_ZTEST | GPU_F_ZWRITE | (20u << 11) | 10u), "w0 %08x", (unsigned)r[0]);
    CHECK(r[1] == ((20u << 11) | 10u), "w1 %08x", (unsigned)r[1]);
    CHECK(s32(r[2]) == -2560 && s32(r[3]) == -2560 && s32(r[4]) == 25599, "edge0 %d %d %d",
          s32(r[2]), s32(r[3]), s32(r[4]));
    CHECK(s32(r[5]) == 2560 && s32(r[6]) == 0 && s32(r[7]) == 0, "edge1 %d %d %d",
          s32(r[5]), s32(r[6]), s32(r[7]));
    CHECK(s32(r[8]) == 0 && s32(r[9]) == 2560 && s32(r[10]) == 0, "edge2 %d %d %d",
          s32(r[8]), s32(r[9]), s32(r[10]));
    CHECK(r[11] == 67107840u && r[12] == 0 && r[13] == 0, "z %u %u %u", (unsigned)r[11], (unsigned)r[12], (unsigned)r[13]);
    CHECK(r[14] == 0 && r[15] == 1048576u && r[16] == 0, "r %u %u %u", (unsigned)r[14], (unsigned)r[15], (unsigned)r[16]);
    CHECK(r[17] == 10u * 65536u && r[18] == 0 && r[19] == 0, "g");
    CHECK(r[20] == 200u * 65536u && r[21] == 0 && r[22] == 0, "b");
    CHECK(r[23] == 0, "w23");
    hash_rec(r);
    /* the other winding gives the identical record (vertices 1,2 swapped back) */
    w[0] = v[0]; w[1] = v[2]; w[2] = v[1];
    ret = gpu_setup_tri_noclip(w, GPU_F_ZTEST | GPU_F_ZWRITE, GPU_CULL_NONE, r2);
    CHECK(ret == 1 && memcmp(r, r2, sizeof r) == 0, "swapped winding differs");
    /* clip variant of an in-guard-band triangle == noclip */
    {
        uint32_t o[GPU_SETUP_MAX_OUT][RW];
        ret = gpu_setup_tri(v, GPU_F_ZTEST | GPU_F_ZWRITE, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
        CHECK(ret == 1 && memcmp(o[0], r, sizeof r) == 0, "setup_tri differs from noclip");
    }
    /* END record */
    gpu_make_end(0xDEADBEEFu, r);
    {
        int i, ok = r[0] == 0xF0000000u && r[1] == 0xDEADBEEFu;
        for (i = 2; i < RW; i++) ok &= r[i] == 0;
        CHECK(ok, "END record");
    }
    end_test(NULL);
}

/* geometric model vs record coverage for random triangles (edges, bias, bbox) */
static void test_geo_vs_record(void)
{
    int iter, mism = 0, tris = 0;
    begin("coverage_vs_geometry");
    rseed(0x1001);
    for (iter = 0; iter < 6000; iter++) {
        gpu_vtx v[3];
        uint32_t r[RW];
        int32_t X[3], Y[3];
        int perm[3], i, ret, sgn, mode = (int)(rnext() % 5);
        double size = iter % 10 == 0 ? 400.0 : iter % 3 == 0 ? 60.0 : 12.0;
        double cx = -40 + runif() * (W + 80), cy = -40 + runif() * (H + 80);
        int bx0, bx1, by0, by1, x, y;
        for (i = 0; i < 3; i++) {
            double x = cx + (runif() - 0.5) * size;
            double y = cy + (runif() - 0.5) * size;
            v[i] = mkv(quant(x, mode), quant(y, mode), 0.5f, 1, 2, 3);
        }
        ret = gpu_setup_tri_noclip(v, 0, GPU_CULL_NONE, r);
        sgn = snap_tri(v, X, Y, perm);
        CHECK(ret != -1, "in-guard-band triangle rejected");
        if (sgn == 0) {
            CHECK(ret == 0, "degenerate triangle produced a record");
            continue;
        }
        if (ret == 1)
            hash_rec(r);
        tris++;
        /* region: vertex bbox pixels +-2, clamped to screen */
        bx0 = (int)floor((double)v[0].x); bx1 = bx0; by0 = (int)floor((double)v[0].y); by1 = by0;
        for (i = 1; i < 3; i++) {
            int fx = (int)floor((double)v[i].x), fy = (int)floor((double)v[i].y);
            if (fx < bx0) bx0 = fx;
            if (fx > bx1) bx1 = fx;
            if (fy < by0) by0 = fy;
            if (fy > by1) by1 = fy;
        }
        bx0 -= 2; by0 -= 2; bx1 += 2; by1 += 2;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > W - 1) bx1 = W - 1;
        if (by1 > H - 1) by1 = H - 1;
        for (y = by0; y <= by1; y++)
            for (x = bx0; x <= bx1; x++) {
                int g = geo_cov(X, Y, x, y);
                int rc = ret == 1 ? rec_cov(r, x, y) : 0;
                if (g != rc) {   /* ret == 0 -> no pixel centre may be covered geometrically */
                    mism++;
                    CHECK(0, "pixel (%d,%d) geo %d rec %d (ret %d)", x, y, g, rc, ret);
                }
            }
    }
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%d triangles, %d mismatches", tris, mism);
        end_test(buf);
    }
}

/* squares with vertices on pixel centres / edges, both diagonals and windings: half-open */
static void test_topleft(void)
{
    int iter;
    begin("top_left_rule");
    /* hand-counted: right triangle with legs on the top and left edges */
    {
        gpu_vtx v[3];
        uint32_t r[RW];
        int x, y, n = 0, ok = 1;
        v[0] = mkv(10.5f, 10.5f, 0, 255, 255, 255);
        v[1] = mkv(20.5f, 10.5f, 0, 255, 255, 255);
        v[2] = mkv(10.5f, 20.5f, 0, 255, 255, 255);
        CHECK(gpu_setup_tri_noclip(v, 0, GPU_CULL_NONE, r) == 1, "setup");
        for (y = 0; y < 40; y++)
            for (x = 0; x < 40; x++) {
                int exp = x >= 10 && y >= 10 && x + y < 30;
                int c = rec_cov(r, x, y);
                n += c;
                if (c != exp) ok = 0;
            }
        CHECK(ok && n == 55, "top-left triangle covers %d (want 55)", n);
        /* complementary triangle: right and bottom edges excluded */
        v[0] = mkv(20.5f, 10.5f, 0, 255, 255, 255);
        v[1] = mkv(20.5f, 20.5f, 0, 255, 255, 255);
        v[2] = mkv(10.5f, 20.5f, 0, 255, 255, 255);
        CHECK(gpu_setup_tri_noclip(v, 0, GPU_CULL_NONE, r) == 1, "setup");
        n = 0; ok = 1;
        for (y = 0; y < 40; y++)
            for (x = 0; x < 40; x++) {
                int exp = x < 20 && y < 20 && x + y >= 30;
                int c = rec_cov(r, x, y);
                n += c;
                if (c != exp) ok = 0;
            }
        CHECK(ok && n == 45, "bottom-right triangle covers %d (want 45)", n);
    }
    rseed(0x2002);
    for (iter = 0; iter < 3000; iter++) {
        /* axis-aligned rectangle with corners on pixel centres, pixel edges or 1/16 positions */
        int mode = iter % 3;
        double x0 = -20 + runif() * 1200, y0 = -20 + runif() * 650;
        double x1 = x0 + 1 + runif() * 90, y1 = y0 + 1 + runif() * 90;
        float fx0 = quant(x0, mode), fy0 = quant(y0, mode), fx1 = quant(x1, mode), fy1 = quant(y1, mode);
        gpu_vtx a = mkv(fx0, fy0, 0, 0, 0, 0), b = mkv(fx1, fy0, 0, 0, 0, 0);
        gpu_vtx c = mkv(fx1, fy1, 0, 0, 0, 0), d = mkv(fx0, fy1, 0, 0, 0, 0);
        gpu_vtx t[2][3];
        uint32_t r[2][RW];
        int k, x, y, bad = 0;
        int32_t X0 = (int32_t)llrint(fx0 * 16.0), X1 = (int32_t)llrint(fx1 * 16.0);
        int32_t Y0 = (int32_t)llrint(fy0 * 16.0), Y1 = (int32_t)llrint(fy1 * 16.0);
        int px0, px1, py0, py1;
        if (rnext() & 1) {
            t[0][0] = a; t[0][1] = b; t[0][2] = c; t[1][0] = a; t[1][1] = c; t[1][2] = d;
        } else {
            t[0][0] = a; t[0][1] = b; t[0][2] = d; t[1][0] = b; t[1][1] = c; t[1][2] = d;
        }
        for (k = 0; k < 2; k++) {
            if (rnext() & 1) { gpu_vtx s = t[k][1]; t[k][1] = t[k][2]; t[k][2] = s; }
            if (gpu_setup_tri_noclip(t[k], 0, GPU_CULL_NONE, r[k]) != 1)
                memset(r[k], 0, sizeof r[k]);          /* type 0 = no coverage */
            else
                hash_rec(r[k]);
        }
        /* check a window around the rectangle */
        px0 = (int)floor(fx0) - 2; px1 = (int)floor(fx1) + 2;
        py0 = (int)floor(fy0) - 2; py1 = (int)floor(fy1) + 2;
        for (y = py0 < 0 ? 0 : py0; y <= py1 && y < H; y++)
            for (x = px0 < 0 ? 0 : px0; x <= px1 && x < W; x++) {
                int cx = 16 * x + 8, cy = 16 * y + 8;
                int exp = cx >= X0 && cx < X1 && cy >= Y0 && cy < Y1;
                int got = (GPU_W0_TYPE(r[0][0]) == GPU_REC_TRI ? rec_cov(r[0], x, y) : 0)
                        + (GPU_W0_TYPE(r[1][0]) == GPU_REC_TRI ? rec_cov(r[1], x, y) : 0);
                if (got != exp) bad++;
            }
        CHECK(bad == 0, "rect %g,%g-%g,%g mode %d: %d bad pixels", fx0, fy0, fx1, fy1, mode, bad);
    }
    end_test(NULL);
}

/* winding number of an integer polygon; returns 1 inside, 0 outside, 2 on a vertex,
 * 3 + edge index on the open segment of that edge */
static int poly_class(const int64_t *X, const int64_t *Y, int n, int64_t px, int64_t py)
{
    int i, wn = 0;
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int64_t cr = (X[j] - X[i]) * (py - Y[i]) - (Y[j] - Y[i]) * (px - X[i]);
        if (px == X[i] && py == Y[i])
            return 2;
        if (cr == 0 &&
            px >= (X[i] < X[j] ? X[i] : X[j]) && px <= (X[i] > X[j] ? X[i] : X[j]) &&
            py >= (Y[i] < Y[j] ? Y[i] : Y[j]) && py <= (Y[i] > Y[j] ? Y[i] : Y[j]))
            return (px == X[j] && py == Y[j]) ? 2 : 3 + i;
        if (Y[i] <= py) {
            if (Y[j] > py && cr > 0) wn++;
        } else {
            if (Y[j] <= py && cr < 0) wn--;
        }
    }
    return wn != 0;
}

/* Watertightness: jittered grids of quads split along random diagonals, random windings. */
static void test_watertight_grid(void)
{
    int iter, bad_total = 0;
    long recs = 0;
    begin("watertight_grid");
    rseed(0x3003);
    for (iter = 0; iter < 60; iter++) {
        int nx = rint_in(1, 16), ny = rint_in(1, 12), mode = iter % 6; /* 5 = mixed per vertex */
        double ox = -250 + runif() * 1300, oy = -250 + runif() * 800;
        double cw = 12 + runif() * 90, ch = 12 + runif() * 70;
        static float VX[20][20], VY[20][20];
        int32_t BX0, BX1, BY0, BY1;
        int i, j, x, y, bad = 0;
        if (ox + nx * cw > 1530) cw = (1530 - ox) / nx;
        if (oy + ny * ch > 970) ch = (970 - oy) / ny;
        if (cw < 12 || ch < 12) { ox = 100; oy = 100; cw = 40; ch = 30; }
        BX0 = (int32_t)llrint(ox * 16.0); BX1 = (int32_t)llrint((ox + nx * cw) * 16.0);
        BY0 = (int32_t)llrint(oy * 16.0); BY1 = (int32_t)llrint((oy + ny * ch) * 16.0);
        for (j = 0; j <= ny; j++)
            for (i = 0; i <= nx; i++) {
                int m = mode == 5 ? (int)(rnext() % 5) : mode;
                double x0 = ox + i * cw + (runif() - 0.5) * 0.25 * cw;
                double y0 = oy + j * ch + (runif() - 0.5) * 0.25 * ch;
                VX[j][i] = quant(x0, m);
                VY[j][i] = quant(y0, m);
                if (i == 0) VX[j][i] = (float)BX0 / 16.0f;
                if (i == nx) VX[j][i] = (float)BX1 / 16.0f;
                if (j == 0) VY[j][i] = (float)BY0 / 16.0f;
                if (j == ny) VY[j][i] = (float)BY1 / 16.0f;
            }
        memset(g_cnt, 0, sizeof g_cnt);
        for (j = 0; j < ny; j++)
            for (i = 0; i < nx; i++) {
                gpu_vtx a = mkv(VX[j][i], VY[j][i], 0, 0, 0, 0);
                gpu_vtx b = mkv(VX[j][i + 1], VY[j][i + 1], 0, 0, 0, 0);
                gpu_vtx c = mkv(VX[j + 1][i + 1], VY[j + 1][i + 1], 0, 0, 0, 0);
                gpu_vtx d = mkv(VX[j + 1][i], VY[j + 1][i], 0, 0, 0, 0);
                gpu_vtx t[2][3];
                int k;
                if (rnext() & 1) {
                    t[0][0] = a; t[0][1] = b; t[0][2] = c; t[1][0] = a; t[1][1] = c; t[1][2] = d;
                } else {
                    t[0][0] = a; t[0][1] = b; t[0][2] = d; t[1][0] = b; t[1][1] = c; t[1][2] = d;
                }
                for (k = 0; k < 2; k++) {
                    uint32_t r[RW];
                    int ret;
                    if (rnext() & 1) { gpu_vtx s = t[k][1]; t[k][1] = t[k][2]; t[k][2] = s; }
                    ret = gpu_setup_tri_noclip(t[k], 0, GPU_CULL_NONE, r);
                    CHECK(ret != -1, "grid triangle outside guard band");
                    if (ret == 1) { count_record(r); hash_rec(r); recs++; }
                }
            }
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                int cx = 16 * x + 8, cy = 16 * y + 8;
                int exp = cx >= BX0 && cx < BX1 && cy >= BY0 && cy < BY1;
                if (g_cnt[y * W + x] != exp) {
                    if (bad < 3)
                        CHECK(0, "grid %d mode %d: pixel (%d,%d) count %d want %d", iter, mode, x, y,
                              g_cnt[y * W + x], exp);
                    bad++;
                }
            }
        CHECK(bad == 0, "grid %d: %d bad pixels", iter, bad);
        bad_total += bad;
    }
    CHECK(g_range_viol == 0, "int32 range violations in edge evaluation: %ld", g_range_viol);
    {
        char buf[80];
        snprintf(buf, sizeof buf, "%ld records, %d bad pixels", recs, bad_total);
        end_test(buf);
    }
}

/* sin/cos from a Taylor series using only IEEE + - * / (fp-contract off): bit-identical on every
 * platform, unlike libm (glibc and newlib may differ in the last ulp -> different test inputs). */
static void det_sincos(double a, double *s, double *c)
{
    double x = a, t, ss = 0, cc = 0;
    int k;
    while (x > 3.141592653589793) x -= 6.283185307179586;
    while (x < -3.141592653589793) x += 6.283185307179586;
    t = x;
    for (k = 1; k < 41; k += 2) { ss += t; t = t * (-x * x) / (double)((k + 1) * (k + 2)); }
    t = 1;
    for (k = 0; k < 40; k += 2) { cc += t; t = t * (-x * x) / (double)((k + 1) * (k + 2)); }
    *s = ss;
    *c = cc;
}

/* Watertightness: fans around a centre point (centre often exactly on a pixel centre).
 * maxgap: largest angular gap between ring vertices. use_clip: 1 = gpu_setup_tri, 0 = noclip.
 * near_tol > 0: pixel centres within near_tol subpixel units of a ring edge may be 0 or 1 (used
 * when ring edges are clipped: the clip vertex is snapped, so the in-screen edge line moves by
 * < 1/32 px); everything else is exact. all_one: the ring surrounds the screen (every pixel 1). */
static void run_fan(double cxv, double cyv, int n, double rmin, double rmax, int mode, double maxgap,
                    int use_clip, double near_tol, int all_one, int *bad_out, long *recs)
{
    double ang[40];
    int64_t PX[40], PY[40], CX, CY;
    gpu_vtx ring[40], c;
    int i, k, x, y, bad = 0, tries = 0;
    int bx0 = 0, bx1 = W - 1, by0 = 0, by1 = H - 1;
    double sum;
    double S = 0;
    for (;;) {                                   /* generate until the snapped fan is valid */
        double g[40], tot = 0, a0 = runif() * 6.283185307179586, turn = 0;
        int okg = 1, sgn = 0;
        tries++;
        for (i = 0; i < n; i++) { g[i] = 0.3 + runif(); tot += g[i]; }
        sum = a0;
        for (i = 0; i < n; i++) {
            ang[i] = sum;
            g[i] = g[i] / tot * 6.283185307179586;
            if (g[i] > maxgap) okg = 0;
            sum += g[i];
        }
        if (!okg) continue;
        c = mkv(quant(cxv, mode), quant(cyv, mode), 0, 0, 0, 0);
        CX = llrint((double)c.x * 16.0);
        CY = llrint((double)c.y * 16.0);
        for (i = 0; i < n; i++) {
            double rr = rmin + runif() * (rmax - rmin), sn, cs;
            det_sincos(ang[i], &sn, &cs);
            ring[i] = mkv(quant(c.x + rr * cs, mode), quant(c.y + rr * sn, mode), 0, 0, 0, 0);
            PX[i] = llrint((double)ring[i].x * 16.0);
            PY[i] = llrint((double)ring[i].y * 16.0);
        }
        /* every (C, Pi, Pi+1) must keep one orientation and the ring must turn once around C */
        for (i = 0; i < n && okg; i++) {
            int j = (i + 1) % n;
            double ax = (double)(PX[i] - CX), ay = (double)(PY[i] - CY);
            double bx = (double)(PX[j] - CX), by = (double)(PY[j] - CY);
            double cr = ax * by - ay * bx;
            int s = cr > 0 ? 1 : cr < 0 ? -1 : 0;
            if (s == 0 || (sgn != 0 && s != sgn)) okg = 0;
            sgn = s;
            turn += atan2(fabs(cr), ax * bx + ay * by);
        }
        if (okg && fabs(turn - 6.283185307179586) < 0.01)
            break;
        CHECK(tries < 1000, "cannot generate a valid fan");
        if (tries >= 1000) return;
    }
    S = 0;   /* orientation sign only; double avoids int64 overflow for the 5e7 px rings */
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        S += (double)PX[i] * (double)PY[j] - (double)PX[j] * (double)PY[i];
    }
    if (!use_clip) {   /* ring inside the guard band: only its bbox (+margin) can be covered */
        bx0 = W; bx1 = -1; by0 = H; by1 = -1;
        for (i = 0; i < n; i++) {
            int fx = (int)floor((double)ring[i].x), fy = (int)floor((double)ring[i].y);
            if (fx < bx0) bx0 = fx;
            if (fx > bx1) bx1 = fx;
            if (fy < by0) by0 = fy;
            if (fy > by1) by1 = fy;
        }
        bx0 = bx0 - 1 < 0 ? 0 : bx0 - 1;
        by0 = by0 - 1 < 0 ? 0 : by0 - 1;
        bx1 = bx1 + 1 > W - 1 ? W - 1 : bx1 + 1;
        by1 = by1 + 1 > H - 1 ? H - 1 : by1 + 1;
    }
    memset(g_cnt, 0, sizeof g_cnt);
    for (i = 0; i < n; i++) {
        gpu_vtx t[3];
        t[0] = c;
        t[1] = ring[i];
        t[2] = ring[(i + 1) % n];
        if (rnext() & 1) { gpu_vtx s = t[1]; t[1] = t[2]; t[2] = s; }
        if (use_clip) {
            uint32_t o[GPU_SETUP_MAX_OUT][RW];
            int m = gpu_setup_tri(t, 0, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
            CHECK(m >= 0 && m <= 5, "clip pieces %d", m);
            for (k = 0; k < m; k++) { count_record(o[k]); hash_rec(o[k]); (*recs)++; }
        } else {
            uint32_t r[RW];
            int ret = gpu_setup_tri_noclip(t, 0, GPU_CULL_NONE, r);
            CHECK(ret != -1, "fan triangle outside guard band");
            if (ret == 1) { count_record(r); hash_rec(r); (*recs)++; }
        }
    }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int64_t cx = 16 * x + 8, cy = 16 * y + 8;
            int cnt = g_cnt[y * W + x], ok, cls;
            if (all_one) {
                ok = cnt == 1;
                cls = 1;
            } else if (x < bx0 || x > bx1 || y < by0 || y > by1) {
                ok = cnt == 0;
                cls = 0;
            } else {
                int near = 0;
                cls = poly_class(PX, PY, n, cx, cy);
                if (near_tol > 0) {
                    for (i = 0; i < n && !near; i++) {
                        int j = (i + 1) % n;
                        double ex = (double)(PX[j] - PX[i]), ey = (double)(PY[j] - PY[i]);
                        double qx = (double)(cx - PX[i]), qy = (double)(cy - PY[i]);
                        double t = (qx * ex + qy * ey) / (ex * ex + ey * ey), dx, dy;
                        if (t < 0) t = 0;
                        if (t > 1) t = 1;
                        dx = qx - t * ex; dy = qy - t * ey;
                        if (dx * dx + dy * dy <= near_tol * near_tol) near = 1;
                    }
                }
                if (near)
                    ok = cnt <= 1;
                else if (cls == 0 || cls == 1)
                    ok = cnt == cls;
                else if (cls == 2)
                    ok = cnt <= 1;
                else {
                    int e = cls - 3, f = (e + 1) % n;
                    int tl = S > 0 ? topleft(PX[f] - PX[e], PY[f] - PY[e])
                                   : topleft(PX[e] - PX[f], PY[e] - PY[f]);
                    ok = cnt == tl;
                }
            }
            if (!ok) {
                if (bad < 3)
                    CHECK(0, "fan c=(%g,%g) n=%d mode %d: pixel (%d,%d) count %d class %d", c.x, c.y,
                          n, mode, x, y, cnt, cls);
                bad++;
            }
        }
    CHECK(bad == 0, "fan: %d bad pixels", bad);
    *bad_out += bad;
}

static void test_watertight_fan(void)
{
    int iter, bad = 0;
    long recs = 0;
    begin("watertight_fan");
    rseed(0x4004);
    for (iter = 0; iter < 120; iter++) {
        int mode = iter % 5, n = rint_in(3, 24);
        double rmax = 6 + runif() * 244;   /* ring stays inside the guard band */
        double fx = runif() * W;
        double fy = runif() * H;
        run_fan(fx, fy, n, rmax * 0.5, rmax, mode, 0.9 * 3.14159265, 0, 0.0, 0,
                &bad, &recs);
    }
    CHECK(g_range_viol == 0, "int32 range violations: %ld", g_range_viol);
    {
        char buf[80];
        snprintf(buf, sizeof buf, "%ld records, %d bad pixels", recs, bad);
        end_test(buf);
    }
}

/* Huge triangles through the guard-band clipper: the screen must be covered exactly once. */
static void test_watertight_clipped(void)
{
    int iter, bad_total = 0, maxpieces = 0;
    long recs = 0;
    begin("watertight_clipped");
    rseed(0x5005);
    /* single huge triangles, both windings, all four "big" shapes */
    for (iter = 0; iter < 8; iter++) {
        static const float S[8][6] = {
            { -10000, -10000, 30000, -10000, -10000, 30000 },
            { -1e9f, -1e9f, 1e9f, -1e9f, 0, 1e9f },
            { 640, -5000, 9000, 4000, -8000, 4000 },
            { -300, -300, 3000, -300, -300, 3000 },
            { -256, -256, 2816, -256, -256, 2208 },     /* vertices on guard lines */
            { -1e30f, 360, 1e30f, -1e30f, 1e30f, 1e30f },
            { 1536, 976, -3000, 976, 1536, -3000 },
            { -257, -257, 5000, 100, 100, 5000 },
        };
        gpu_vtx t[3];
        uint32_t o[GPU_SETUP_MAX_OUT][RW];
        int k, m, x, y, bad = 0;
        for (k = 0; k < 3; k++)
            t[k] = mkv(S[iter][2 * k], S[iter][2 * k + 1], 0.5f, 10, 20, 30);
        if (iter & 1) { gpu_vtx s = t[1]; t[1] = t[2]; t[2] = s; }
        memset(g_cnt, 0, sizeof g_cnt);
        m = gpu_setup_tri(t, 0, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
        CHECK(m >= 1 && m <= 5, "huge %d: %d pieces", iter, m);
        if (m > maxpieces) maxpieces = m;
        for (k = 0; k < m; k++) { count_record(o[k]); hash_rec(o[k]); recs++; }
        /* every shape contains the whole screen (no edge crosses it) */
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (g_cnt[y * W + x] != 1) {
                    if (bad < 3) CHECK(0, "huge %d: pixel (%d,%d) count %d", iter, x, y, g_cnt[y * W + x]);
                    bad++;
                }
        CHECK(bad == 0, "huge %d: %d bad pixels", iter, bad);
        bad_total += bad;
    }
    /* non-uniform jittered grids whose outer boundary lies far outside the screen */
    for (iter = 0; iter < 60; iter++) {
        double gx[24], gy[24];
        int nx = 0, ny = 0, i, j, x, y, bad = 0;
        static float VX[24][24], VY[24][24];
        double span = iter % 4 == 0 ? 1e6 : iter % 4 == 1 ? 20000 : 3000;
        /* lines: far-out boundary, a few random lines in between (some on the guard lines) */
        gx[nx++] = -300 - runif() * span;
        for (i = 0; i < 20; i++) {
            double v;
            int pick = (int)(rnext() % 8);
            if (pick == 0) v = -256; else if (pick == 1) v = 1536;
            else if (pick == 2) v = 1535.99;
            else v = -600 + runif() * 2500;
            if (rnext() % 3 == 0) gx[nx++] = v;
        }
        gx[nx++] = 1280 + 300 + runif() * span;
        gy[ny++] = -300 - runif() * span;
        for (i = 0; i < 20; i++) {
            double v;
            int pick = (int)(rnext() % 8);
            if (pick == 0) v = -256; else if (pick == 1) v = 976;
            else if (pick == 2) v = 975.99;
            else v = -600 + runif() * 1900;
            if (rnext() % 3 == 0) gy[ny++] = v;
        }
        gy[ny++] = 720 + 300 + runif() * span;
        /* sort + drop lines closer than 8 px */
        for (i = 1; i < nx; i++) for (j = i; j > 0 && gx[j] < gx[j - 1]; j--) { double s = gx[j]; gx[j] = gx[j - 1]; gx[j - 1] = s; }
        for (i = 1; i < ny; i++) for (j = i; j > 0 && gy[j] < gy[j - 1]; j--) { double s = gy[j]; gy[j] = gy[j - 1]; gy[j - 1] = s; }
        for (i = 1, j = 1; i < nx; i++) if (gx[i] - gx[j - 1] >= 8 || i == nx - 1) gx[j++] = gx[i];
        nx = j;
        if (gx[nx - 1] - gx[nx - 2] < 8) { gx[nx - 2] = gx[nx - 1]; nx--; }
        for (i = 1, j = 1; i < ny; i++) if (gy[i] - gy[j - 1] >= 8 || i == ny - 1) gy[j++] = gy[i];
        ny = j;
        if (gy[ny - 1] - gy[ny - 2] < 8) { gy[ny - 2] = gy[ny - 1]; ny--; }
        for (j = 0; j < ny; j++)
            for (i = 0; i < nx; i++) {
                double jx = 0, jy = 0;
                if (i > 0 && i < nx - 1) {
                    double gap = gx[i] - gx[i - 1] < gx[i + 1] - gx[i] ? gx[i] - gx[i - 1] : gx[i + 1] - gx[i];
                    if (rnext() % 3) jx = (runif() - 0.5) * 0.25 * gap;
                }
                if (j > 0 && j < ny - 1) {
                    double gap = gy[j] - gy[j - 1] < gy[j + 1] - gy[j] ? gy[j] - gy[j - 1] : gy[j + 1] - gy[j];
                    if (rnext() % 3) jy = (runif() - 0.5) * 0.25 * gap;
                }
                VX[j][i] = (float)(gx[i] + jx);
                VY[j][i] = (float)(gy[j] + jy);
            }
        memset(g_cnt, 0, sizeof g_cnt);
        for (j = 0; j + 1 < ny; j++)
            for (i = 0; i + 1 < nx; i++) {
                gpu_vtx a = mkv(VX[j][i], VY[j][i], 0.3f, 255, 0, 0);
                gpu_vtx b = mkv(VX[j][i + 1], VY[j][i + 1], 0.6f, 0, 255, 0);
                gpu_vtx c = mkv(VX[j + 1][i + 1], VY[j + 1][i + 1], 0.9f, 0, 0, 255);
                gpu_vtx d = mkv(VX[j + 1][i], VY[j + 1][i], 0.1f, 255, 255, 255);
                gpu_vtx t[2][3];
                int k, q;
                if (rnext() & 1) {
                    t[0][0] = a; t[0][1] = b; t[0][2] = c; t[1][0] = a; t[1][1] = c; t[1][2] = d;
                } else {
                    t[0][0] = a; t[0][1] = b; t[0][2] = d; t[1][0] = b; t[1][1] = c; t[1][2] = d;
                }
                for (k = 0; k < 2; k++) {
                    uint32_t o[GPU_SETUP_MAX_OUT][RW];
                    int m;
                    if (rnext() & 1) { gpu_vtx s = t[k][1]; t[k][1] = t[k][2]; t[k][2] = s; }
                    m = gpu_setup_tri(t[k], GPU_F_ZTEST, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
                    CHECK(m >= 0 && m <= 5, "pieces %d", m);
                    if (m > maxpieces) maxpieces = m;
                    for (q = 0; q < m; q++) { count_record(o[q]); hash_rec(o[q]); recs++; }
                }
            }
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (g_cnt[y * W + x] != 1) {
                    if (bad < 3) CHECK(0, "clip grid %d: pixel (%d,%d) count %d", iter, x, y, g_cnt[y * W + x]);
                    bad++;
                }
        CHECK(bad == 0, "clip grid %d: %d bad pixels", iter, bad);
        bad_total += bad;
    }
    /* fans with a huge ring: always clipped, must cover the whole screen once */
    for (iter = 0; iter < 40; iter++) {
        int bad = 0;
        double r = iter % 3 == 0 ? 3000 : iter % 3 == 1 ? 50000 : 5e7;
        double fx = runif() * W;
        double fy = runif() * H;
        int n = rint_in(4, 12);
        run_fan(fx, fy, n, r, 2 * r, iter % 5, 0.9 * 2.0943951, 1, 0.0, 1, &bad, &recs);
        bad_total += bad;
    }
    /* mixed fans: ring partly inside, partly far outside the guard band. Exact except within
     * 1 subpixel unit of a ring edge (clipped edges end at a snapped clip vertex). */
    for (iter = 0; iter < 16; iter++) {
        int bad = 0;
        double fx = runif() * W;
        double fy = runif() * H;
        int n = rint_in(3, 10);
        run_fan(fx, fy, n, 100, 4000, iter % 5, 0.9 * 3.14159265, 1, 1.0, 0, &bad, &recs);
        bad_total += bad;
    }
    CHECK(g_range_viol == 0, "int32 range violations: %ld", g_range_viol);
    {
        char buf[96];
        snprintf(buf, sizeof buf, "%ld records, max %d pieces/tri, %d bad pixels", recs, maxpieces, bad_total);
        end_test(buf);
    }
}

static void test_cull(void)
{
    gpu_vtx cw[3], ccw[3], big_cw[3], big_ccw[3];
    uint32_t r[RW], o[GPU_SETUP_MAX_OUT][RW];
    int32_t X[3], Y[3];
    int perm[3];
    begin("culling");
    cw[0] = mkv(10, 10, 0, 1, 1, 1); cw[1] = mkv(50, 10, 0, 1, 1, 1); cw[2] = mkv(10, 50, 0, 1, 1, 1);
    ccw[0] = cw[0]; ccw[1] = cw[2]; ccw[2] = cw[1];
    CHECK(snap_tri(cw, X, Y, perm) == 1, "cw area2 > 0");
    CHECK(gpu_setup_tri_noclip(cw, 0, GPU_CULL_NONE, r) == 1, "cw none");
    CHECK(gpu_setup_tri_noclip(cw, 0, GPU_CULL_CW, r) == 0, "cw cull_cw");
    CHECK(gpu_setup_tri_noclip(cw, 0, GPU_CULL_CCW, r) == 1, "cw cull_ccw");
    CHECK(gpu_setup_tri_noclip(ccw, 0, GPU_CULL_NONE, r) == 1, "ccw none");
    CHECK(gpu_setup_tri_noclip(ccw, 0, GPU_CULL_CW, r) == 1, "ccw cull_cw");
    CHECK(gpu_setup_tri_noclip(ccw, 0, GPU_CULL_CCW, r) == 0, "ccw cull_ccw");
    CHECK(gpu_setup_tri(cw, 0, GPU_CULL_CW, o, 8) == 0, "tri cw cull_cw");
    CHECK(gpu_setup_tri(cw, 0, GPU_CULL_CCW, o, 8) == 1, "tri cw cull_ccw");
    CHECK(gpu_setup_tri(ccw, 0, GPU_CULL_CCW, o, 8) == 0, "tri ccw cull_ccw");
    CHECK(gpu_setup_tri(ccw, 0, GPU_CULL_CW, o, 8) == 1, "tri ccw cull_cw");
    /* culling is decided before the guard band: culled triangles outside it return 0 */
    big_cw[0] = mkv(-5000, -5000, 0, 1, 1, 1); big_cw[1] = mkv(9000, -5000, 0, 1, 1, 1);
    big_cw[2] = mkv(-5000, 9000, 0, 1, 1, 1);
    big_ccw[0] = big_cw[0]; big_ccw[1] = big_cw[2]; big_ccw[2] = big_cw[1];
    CHECK(gpu_setup_tri_noclip(big_cw, 0, GPU_CULL_CW, r) == 0, "big cw culled -> 0");
    CHECK(gpu_setup_tri_noclip(big_cw, 0, GPU_CULL_CCW, r) == -1, "big cw not culled -> -1");
    CHECK(gpu_setup_tri(big_cw, 0, GPU_CULL_CW, o, 8) == 0, "clip big cw cull_cw");
    CHECK(gpu_setup_tri(big_cw, 0, GPU_CULL_CCW, o, 8) >= 1, "clip big cw cull_ccw");
    CHECK(gpu_setup_tri(big_ccw, 0, GPU_CULL_CCW, o, 8) == 0, "clip big ccw cull_ccw");
    CHECK(gpu_setup_tri(big_ccw, 0, GPU_CULL_CW, o, 8) >= 1, "clip big ccw cull_cw");
    CHECK(gpu_setup_tri(big_ccw, 0, GPU_CULL_NONE, o, 8) >= 1, "clip big ccw none");
    /* max_out limits the output */
    CHECK(gpu_setup_tri(big_ccw, 0, GPU_CULL_NONE, o, 1) == 1, "max_out 1");
    CHECK(gpu_setup_tri(big_ccw, 0, GPU_CULL_NONE, o, 0) == 0, "max_out 0");
    end_test(NULL);
}

static void test_degenerate(void)
{
    gpu_vtx v[3];
    uint32_t r[RW], o[GPU_SETUP_MAX_OUT][RW];
    float nan = (float)NAN, inf = (float)INFINITY;
    begin("degenerate_and_guard");
    v[0] = mkv(10, 10, 0, 0, 0, 0); v[1] = mkv(20, 20, 0, 0, 0, 0); v[2] = mkv(30, 30, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "collinear");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) == 0, "collinear clip");
    v[2] = v[1];
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "coincident");
    v[0] = mkv(10.001f, 10, 0, 0, 0, 0); v[1] = mkv(10.002f, 20, 0, 0, 0, 0); v[2] = mkv(10.003f, 30, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "collinear after snapping");
    v[0] = mkv(10.6f, 10.6f, 0, 0, 0, 0); v[1] = mkv(10.9f, 10.6f, 0, 0, 0, 0); v[2] = mkv(10.6f, 10.9f, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "tiny, no pixel centre in bbox");
    v[0] = mkv(-100, -100, 0, 0, 0, 0); v[1] = mkv(-50, -100, 0, 0, 0, 0); v[2] = mkv(-100, -50, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "off-screen (left/top) in guard band");
    v[0] = mkv(100, 800, 0, 0, 0, 0); v[1] = mkv(300, 800, 0, 0, 0, 0); v[2] = mkv(100, 900, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "off-screen (below)");
    v[0] = mkv(1279.6f, 100, 0, 0, 0, 0); v[1] = mkv(1400, 100, 0, 0, 0, 0); v[2] = mkv(1279.6f, 300, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 0, "right of the last pixel centre");
    v[0] = mkv(1279.5f, 100, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1 && R_XMIN(r) == 1279 && R_XMAX(r) == 1279,
          "on the last pixel centre column");
    /* guard band edges (X = llrint(x*16), round-half-even) */
    v[0] = mkv(100, 100, 0, 0, 0, 0); v[1] = mkv(200, 100, 0, 0, 0, 0);
    v[2] = mkv(-256.0f, 300, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "x=-256 inside");
    v[2].x = -256.03125f;   /* *16 = -4096.5 -> -4096 (even) */
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "x=-256.03125 rounds inside");
    v[2].x = -256.0625f;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "x=-256.0625 outside");
    v[2].x = 1535.9375f;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "x=1535.9375 inside");
    v[2].x = 1535.96875f;   /* *16 = 24575.5 -> 24576 (even) */
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "x=1535.96875 rounds outside");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) >= 1, "... but the clip variant nudges it inside");
    v[2].x = 1536;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "x=1536 outside");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) >= 1, "x=1536 clip variant");
    v[2] = mkv(300, 975.9375f, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "y=975.9375 inside");
    v[2].y = 976;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "y=976 outside");
    v[2].y = -256.0625f;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "y=-256.0625 outside");
    /* NaN / Inf never crash; noclip reports -1, clip variant 0 */
    v[2] = mkv(nan, 300, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "NaN x");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) == 0, "NaN x clip");
    v[2] = mkv(300, inf, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "Inf y");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) == 0, "Inf y clip");
    v[2] = mkv(300, -inf, 0, 0, 0, 0);
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) == 0, "-Inf y clip");
    v[2] = mkv(3e38f, 3e38f, 0, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == -1, "3e38 x");
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) >= 1, "3e38 x clip");
    v[2] = mkv(3e38f, 300, 0, 0, 0, 0);   /* sliver thinner than float precision: degenerate */
    CHECK(gpu_setup_tri(v, 0, 0, o, 8) == 0, "3e38 sliver clip");
    /* z NaN / out of range are clamped (NaN -> 0) */
    v[0] = mkv(100.5f, 100.5f, nan, 0, 0, 0); v[1] = mkv(200.5f, 100.5f, 5.0f, 0, 0, 0);
    v[2] = mkv(100.5f, 200.5f, -3.0f, 0, 0, 0);
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "z nan");
    CHECK(rec_attr64(r, 11, 100, 100) >= -1 && rec_attr64(r, 11, 100, 100) <= 1, "z nan -> 0 at v0: %lld",
          (long long)rec_attr64(r, 11, 100, 100));
    v[0].z = inf;
    CHECK(gpu_setup_tri_noclip(v, 0, 0, r) == 1, "z inf");
    CHECK(llabs((long long)rec_attr64(r, 11, 100, 100) - 268431360LL) <= 1, "z inf -> 1 at v0");
    end_test(NULL);
}

static void test_z(void)
{
    uint32_t recs[32 * RW];
    int n = 0, x, y, bad = 0;
    begin("z_semantics");
#define ADD_RECT(x0, y0, x1, y1, c, z, f) \
    CHECK(gpu_setup_rect(x0, y0, x1, y1, c, z, f, recs + n * RW) == 1, "rect"); n++
    ADD_RECT(0, 0, 100, 100, 0xF800, 0.5f, GPU_F_ZTEST | GPU_F_ZWRITE);          /* A red   */
    ADD_RECT(50, 50, 150, 150, 0x07E0, 0.7f, GPU_F_ZTEST | GPU_F_ZWRITE);        /* B green behind A */
    ADD_RECT(80, 0, 120, 40, 0x001F, 0.3f, GPU_F_ZTEST);                         /* C blue, no write */
    ADD_RECT(80, 20, 120, 60, 0xFFE0, 0.4f, GPU_F_ZTEST | GPU_F_ZWRITE);         /* D yellow over C */
    ADD_RECT(0, 0, 20, 20, 0x07FF, 0.5f, GPU_F_ZTEST);                           /* E equal z passes */
    ADD_RECT(130, 130, 140, 140, 0xFFFF, 1.0f, 0);                               /* F no test: drawn */
    ADD_RECT(200, 0, 220, 20, 0x8410, 2.0f, GPU_F_ZTEST);                        /* z>1 -> 65535 passes clear */
    ADD_RECT(200, 0, 210, 20, 0x0000, -1.0f, GPU_F_ZTEST | GPU_F_ZWRITE);        /* z<0 -> 0 */
    ADD_RECT(200, 0, 220, 20, 0xF81F, 0.0f, GPU_F_ZTEST);                        /* 0 <= 0 passes everywhere */
    ADD_RECT(200, 0, 220, 20, 0x39E7, 0.00002f, GPU_F_ZTEST);                    /* zp=1: fails where zb=0 */
    ADD_RECT(300, 0, 320, 20, 0x1234, 0.5f, GPU_F_ZWRITE);                       /* write without test */
    ADD_RECT(300, 0, 320, 20, 0x4321, 0.9f, GPU_F_ZTEST);                        /* fails vs 0.5 */
    /* raw records: z0 negative -> zp 0 ; z0 huge -> zp 65535 */
    memset(recs + n * RW, 0, RW * 4);
    recs[n * RW + 0] = (1u << 28) | GPU_F_NOEDGE | GPU_F_ZTEST | GPU_F_ZWRITE | (409u << 11) | 400u;
    recs[n * RW + 1] = (9u << 11) | 0u;
    recs[n * RW + 11] = 0x80000000u;          /* INT32_MIN -> zp 0 */
    recs[n * RW + 14] = 255u << 16;
    n++;
    memset(recs + n * RW, 0, RW * 4);
    recs[n * RW + 0] = (1u << 28) | GPU_F_NOEDGE | GPU_F_ZTEST | (419u << 11) | 400u;
    recs[n * RW + 1] = (9u << 11) | 0u;
    recs[n * RW + 11] = 0x7FFFFFFFu;          /* >>12 = 524287 -> 65535: passes clear, fails zb 0 */
    recs[n * RW + 17] = 255u << 16;
    n++;
    gpu_refrast_frame(recs, n, 0x0000, NULL, NULL, g_fb);
    for (y = 0; y < 160; y++)
        for (x = 0; x < 430; x++) {
            uint16_t exp = 0x0000;
            int inA = x < 100 && y < 100, inB = x >= 50 && x < 150 && y >= 50 && y < 150;
            int inC = x >= 80 && x < 120 && y < 40, inD = x >= 80 && x < 120 && y >= 20 && y < 60;
            if (inA) exp = 0xF800;
            else if (inB) exp = 0x07E0;
            if (inC) exp = 0x001F;
            if (inD) exp = 0xFFE0;
            if (x < 20 && y < 20) exp = 0x07FF;
            if (x >= 130 && x < 140 && y >= 130 && y < 140) exp = 0xFFFF;
            if (x >= 200 && x < 220 && y < 20) exp = x < 210 ? 0xF81F : 0x39E7;
            if (x >= 300 && x < 320 && y < 20) exp = 0x1234;
            if (x >= 400 && x < 420 && y < 10) exp = x < 410 ? 0xF800 : 0x07E0;
            if (g_fb[y * W + x] != exp) {
                if (bad < 5) CHECK(0, "pixel (%d,%d) = %04x want %04x", x, y, g_fb[y * W + x], exp);
                bad++;
            }
        }
    CHECK(bad == 0, "%d bad pixels", bad);
    end_test(NULL);
#undef ADD_RECT
}

static void test_gouraud(void)
{
    int iter, clamped = 0, checked = 0, maxerr = 0;
    begin("gouraud_endpoints");
    rseed(0x6006);
    for (iter = -2; iter < 4000; iter++) {
        gpu_vtx v[3];
        uint32_t r[RW];
        int i, ret;
        double size = iter % 4 == 0 ? 1200 : iter % 4 == 1 ? 200 : 30;
        double cx = runif() * W, cy = runif() * H;
        for (i = 0; i < 3; i++) {
            double x = cx + (runif() - 0.5) * size, y = cy + (runif() - 0.5) * size;
            if (x < 0) x = 0;
            if (x > W - 1) x = W - 1;
            if (y < 0) y = 0;
            if (y > H - 1) y = H - 1;
            {
                float z = (float)runif();
                v[i] = mkv_rgb(quant(x, 0), quant(y, 0), z);
            }
            if (iter % 7 == 0) { v[i].r = i == 0 ? 255 : 0; v[i].g = i == 1 ? 255 : 0; v[i].b = 0; }
        }
        if (iter < 0) {
            /* regression: bbox corner (0,0) ~500 px from this anti-diagonal wedge with a steep
             * z / colour gradient -> the start values extrapolate far beyond int32. A clamped
             * start (literal SPEC 4.6) corrupts every pixel; the modulo-2^32 start is exact. */
            v[0] = mkv(0.5f, 719.5f, 0.5f, 128, 0, 255);
            v[1] = mkv(1279.5f, 0.5f, 0.0f, 0, 255, 0);
            v[2] = mkv(1279.5f, iter == -2 ? 40.5f : 12.5f, 1.0f, 255, 0, 128);
        }
        ret = gpu_setup_tri_noclip(v, GPU_F_ZTEST | GPU_F_ZWRITE, GPU_CULL_NONE, r);
        CHECK(ret != -1, "gouraud: -1");
        if (ret != 1)
            continue;
        hash_rec(r);
        if (iter < 0) {
            /* make sure the case really overflows: exact z at the bbox corner pixel centre */
            long double zc = exact_attr(v, 0, 16.0 * R_XMIN(r) + 8, 16.0 * R_YMIN(r) + 8);
            CHECK(zc > 2147483648.0L || zc < -2147483648.0L, "regression wedge: corner z %Lg fits int32", zc);
            CHECK(!grad_clamped(r), "regression wedge: gradient clamped");
        }
        if (grad_clamped(r)) { clamped++; continue; }
        for (i = 0; i < 3; i++) {
            int px = (int)floor(v[i].x), py = (int)floor(v[i].y), ch;
            int64_t zv = rec_attr64(r, 11, px, py);
            int zp = zv < 0 ? 0 : (int)(zv >> 12) > 65535 ? 65535 : (int)(zv >> 12);
            int zexp = (int)floor((double)v[i].z * 65535.0);
            CHECK(px >= R_XMIN(r) && px <= R_XMAX(r) && py >= R_YMIN(r) && py <= R_YMAX(r),
                  "vertex pixel outside bbox");
            CHECK(abs(zp - zexp) <= 1, "iter %d z at vertex %d: %d want %d", iter, i, zp, zexp);
            for (ch = 0; ch < 3; ch++) {
                int64_t cv = rec_attr64(r, 14 + 3 * ch, px, py);
                int want = ch == 0 ? v[i].r : ch == 1 ? v[i].g : v[i].b;
                int got = c8_of(cv), err = abs(got - want);
                CHECK(err <= 1, "iter %d vertex %d ch %d: %d want %d", iter, i, ch, got, want);
                if (err > maxerr) maxerr = err;
                checked++;
            }
        }
        /* rendered pixels at covered vertex pixels hold exactly the record's colour */
        if (iter % 50 == 0) {
            gpu_refrast_frame(r, 1, 0x0000, NULL, NULL, g_fb);
            for (i = 0; i < 3; i++) {
                int px = (int)floor(v[i].x), py = (int)floor(v[i].y);
                if (rec_cov(r, px, py)) {
                    int cr = c8_of(rec_attr64(r, 14, px, py)), cg = c8_of(rec_attr64(r, 17, px, py));
                    int cb = c8_of(rec_attr64(r, 20, px, py));
                    CHECK(g_fb[py * W + px] == gpu_rgb565((unsigned)cr, (unsigned)cg, (unsigned)cb),
                          "rendered vertex pixel colour");
                }
            }
        }
    }
    {
        char buf[96];
        snprintf(buf, sizeof buf, "%d channel checks, max err %d, %d clamped-gradient slivers skipped",
                 checked, maxerr, clamped);
        end_test(buf);
    }
}

static void test_rect(void)
{
    uint32_t r[RW];
    int c, ok = 1;
    begin("rect");
    CHECK(gpu_setup_rect(10, 20, 30, 40, 0x1234, 0.5f, GPU_F_ZTEST, r) == 1, "basic");
    CHECK(r[0] == ((1u << 28) | GPU_F_NOEDGE | GPU_F_ZTEST | (29u << 11) | 10u), "w0 %08x", (unsigned)r[0]);
    CHECK(r[1] == ((39u << 11) | 20u), "w1");
    CHECK(r[11] == 134215680u, "z0 %u", (unsigned)r[11]);
    {
        int i, zero = 1;
        for (i = 2; i <= 10; i++) zero &= r[i] == 0;
        zero &= r[12] == 0 && r[13] == 0 && r[15] == 0 && r[16] == 0 && r[18] == 0 && r[19] == 0;
        zero &= r[21] == 0 && r[22] == 0 && r[23] == 0;
        CHECK(zero, "edges/gradients not zero");
    }
    hash_rec(r);
    CHECK(gpu_setup_rect(-5, -5, 5, 5, 1, 0, 0, r) == 1 && R_XMIN(r) == 0 && R_XMAX(r) == 4 &&
          R_YMIN(r) == 0 && R_YMAX(r) == 4, "clip top-left");
    CHECK(gpu_setup_rect(1275, 715, 2000, 2000, 1, 0, 0, r) == 1 && R_XMIN(r) == 1275 &&
          R_XMAX(r) == 1279 && R_YMIN(r) == 715 && R_YMAX(r) == 719, "clip bottom-right");
    CHECK(gpu_setup_rect(-2147483647 - 1, -2147483647 - 1, 2147483647, 2147483647, 1, 0, 0, r) == 1 &&
          R_XMIN(r) == 0 && R_XMAX(r) == 1279 && R_YMIN(r) == 0 && R_YMAX(r) == 719, "INT extremes");
    CHECK(gpu_setup_rect(1280, 0, 1300, 10, 1, 0, 0, r) == 0, "right of screen");
    CHECK(gpu_setup_rect(0, 720, 10, 800, 1, 0, 0, r) == 0, "below screen");
    CHECK(gpu_setup_rect(10, 10, 10, 20, 1, 0, 0, r) == 0, "zero width");
    CHECK(gpu_setup_rect(10, 10, 20, 9, 1, 0, 0, r) == 0, "negative height");
    CHECK(gpu_setup_rect(-100, 0, 0, 10, 1, 0, 0, r) == 0, "left of screen");
    CHECK(gpu_setup_rect(2147483647, 2147483647, -2147483647 - 1, 0, 1, 0, 0, r) == 0, "INT extremes empty");
    CHECK(gpu_setup_rect(0, 0, 1, 1, 1, 0, 0xFFFFFFFFu, r) == 1 &&
          r[0] == ((1u << 28) | GPU_F_NOEDGE | GPU_F_ZTEST | GPU_F_ZWRITE), "flags masked: %08x", (unsigned)r[0]);
    CHECK(gpu_setup_rect(0, 0, 1, 1, 1, 7.0f, 0, r) == 1 && r[11] == 268431360u, "z clamp high");
    CHECK(gpu_setup_rect(0, 0, 1, 1, 1, (float)NAN, 0, r) == 1 && r[11] == 0, "z NaN");
    /* every RGB565 colour round-trips exactly through the record + SPEC 5 colour path */
    for (c = 0; c < 65536; c++) {
        uint32_t c8r, c8g, c8b;
        gpu_setup_rect(0, 0, 1, 1, (uint16_t)c, 0, 0, r);
        c8r = (uint32_t)c8_of(s32(r[14])); c8g = (uint32_t)c8_of(s32(r[17])); c8b = (uint32_t)c8_of(s32(r[20]));
        if ((((c8r >> 3) << 11) | ((c8g >> 2) << 5) | (c8b >> 3)) != (uint32_t)c) ok = 0;
    }
    CHECK(ok, "RGB565 round trip");
    /* rendered */
    {
        int x, y, bad = 0;
        gpu_setup_rect(-3, 700, 7, 730, 0xABCD, 0, 0, r);
        gpu_refrast_frame(r, 1, 0x0101, NULL, NULL, g_fb);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                uint16_t exp = (x < 7 && y >= 700) ? 0xABCD : 0x0101;
                if (g_fb[y * W + x] != exp) bad++;
            }
        CHECK(bad == 0, "rendered rect %d bad", bad);
    }
    end_test(NULL);
}

/* sprite DDR model: 64 KB at 0x1E400000 */
#define SPR_BASE 0x1E400000u
static uint16_t g_spr[131072];
static uint16_t spr_rd(void *user, uint32_t addr)
{
    (void)user;
    if (addr >= SPR_BASE && addr - SPR_BASE < sizeof g_spr && (addr & 1) == 0)
        return g_spr[(addr - SPR_BASE) / 2];
    return 0xDEAD;
}

static void test_sprite(void)
{
    uint32_t r[RW];
    int i, x, y, bad;
    begin("sprite");
    for (i = 0; i < 131072; i++)
        g_spr[i] = (i % 5 == 0) ? 0xF81F : (uint16_t)((uint32_t)i * 7919u + 1u);
    CHECK(gpu_setup_sprite(2, 0, 8, 8, SPR_BASE, 16, 0, 0, r) == -1, "x %% 4");
    CHECK(gpu_setup_sprite(-2, 0, 8, 8, SPR_BASE, 16, 0, 0, r) == -1, "negative x %% 4");
    CHECK(gpu_setup_sprite(0, 0, 6, 8, SPR_BASE, 16, 0, 0, r) == -1, "w %% 4");
    CHECK(gpu_setup_sprite(0, 0, 0, 8, SPR_BASE, 16, 0, 0, r) == -1, "w 0");
    CHECK(gpu_setup_sprite(0, 0, -4, 8, SPR_BASE, 16, 0, 0, r) == -1, "w < 4");
    CHECK(gpu_setup_sprite(0, 0, 8, 0, SPR_BASE, 16, 0, 0, r) == -1, "h 0");
    CHECK(gpu_setup_sprite(0, 0, 8, 8, SPR_BASE + 4, 16, 0, 0, r) == -1, "src %% 8");
    CHECK(gpu_setup_sprite(0, 0, 8, 8, SPR_BASE, 12, 0, 0, r) == -1, "stride %% 8");
    CHECK(gpu_setup_sprite(100, 50, 64, 32, SPR_BASE, 128, 1, 0xF81F, r) == 1, "basic");
    CHECK(r[0] == ((2u << 28) | GPU_F_COLORKEY) && r[1] == ((50u << 11) | 100u) && r[2] == ((32u << 11) | 64u) &&
          r[3] == SPR_BASE && r[4] == 128 && r[5] == 0xF81F, "basic fields");
    for (i = 6; i < RW; i++) CHECK(r[i] == 0, "w%d", i);
    hash_rec(r);
    CHECK(gpu_setup_sprite(100, -10, 64, 32, SPR_BASE, 256, 0, 0, r) == 1 && r[1] == 100u &&
          r[2] == ((22u << 11) | 64u) && r[3] == SPR_BASE + 2560 && r[0] == (2u << 28), "neg y");
    CHECK(gpu_setup_sprite(-8, 0, 64, 32, SPR_BASE, 128, 0, 0, r) == 1 && r[1] == 0 &&
          r[2] == ((32u << 11) | 56u) && r[3] == SPR_BASE + 16, "neg x");
    CHECK(gpu_setup_sprite(-12, -3, 64, 32, SPR_BASE, 128, 0, 0, r) == 1 && r[1] == 0 &&
          r[2] == ((29u << 11) | 52u) && r[3] == SPR_BASE + 3 * 128 + 24, "neg x and y");
    CHECK(gpu_setup_sprite(-64, 0, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "fully left");
    CHECK(gpu_setup_sprite(-4, 0, 4, 32, SPR_BASE, 128, 0, 0, r) == 0, "fully left, w 4");
    CHECK(gpu_setup_sprite(0, -32, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "fully above");
    CHECK(gpu_setup_sprite(1280, 0, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "right of screen");
    CHECK(gpu_setup_sprite(0, 720, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "below screen");
    CHECK(gpu_setup_sprite(-2147483647 - 1, 0, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "INT_MIN x");
    CHECK(gpu_setup_sprite(0, -2147483647 - 1, 64, 32, SPR_BASE, 128, 0, 0, r) == 0, "INT_MIN y");
    CHECK(gpu_setup_sprite(1276, 719, 4096, 4000, SPR_BASE, 8192, 0, 0, r) == 1 &&
          r[1] == ((719u << 11) | 1276u) && r[2] == ((720u << 11) | 1280u), "w/h clamps");
    /* rendered: clipped at negative x/y by setup, at right/bottom by the renderer */
    {
        static const int P[4][5] = {   /* x, y, w, h, stride */
            { -12, -3, 64, 32, 136 }, { 1256, 704, 40, 30, 96 }, { 600, 300, 4, 1, 8 }, { 1240, -7, 48, 740, 104 } };
        int k;
        for (k = 0; k < 4; k++) {
            int ck = k != 2, ret;
            uint32_t src0 = SPR_BASE + 8u * (uint32_t)k;
            ret = gpu_setup_sprite(P[k][0], P[k][1], P[k][2], P[k][3], src0, (uint32_t)P[k][4], ck, 0xF81F, r);
            CHECK(ret == 1, "render sprite %d setup", k);
            gpu_refrast_frame(r, 1, 0x0842, spr_rd, NULL, g_fb);
            naive_frame(r, 1, 0x0842, spr_rd, NULL, g_fb2);
            CHECK(memcmp(g_fb, g_fb2, sizeof g_fb) == 0, "sprite %d refrast != naive", k);
            bad = 0;
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    int sx = x - P[k][0], sy = y - P[k][1];
                    uint16_t exp = 0x0842;
                    if (sx >= 0 && sx < P[k][2] && sy >= 0 && sy < P[k][3]) {
                        uint16_t p = spr_rd(NULL, src0 + (uint32_t)sy * (uint32_t)P[k][4] + (uint32_t)sx * 2u);
                        if (!(ck && p == 0xF81F)) exp = p;
                    }
                    if (g_fb[y * W + x] != exp) bad++;
                }
            CHECK(bad == 0, "sprite %d: %d bad pixels", k, bad);
        }
    }
    end_test(NULL);
}

/* thousands of random triangles inside the guard band: int32 ranges + value accuracy */
static void check_values(const uint32_t *r, const gpu_vtx v[3], long *samples, int *clampcnt,
                         double *maxerr)
{
    int32_t X[3], Y[3];
    int perm[3], i, s, ch;
    int64_t area2;
    int xmin = R_XMIN(r), xmax = R_XMAX(r), ymin = R_YMIN(r), ymax = R_YMAX(r);
    if (snap_tri(v, X, Y, perm) == 0) { CHECK(0, "record from degenerate triangle"); return; }
    area2 = (int64_t)(X[1] - X[0]) * (Y[2] - Y[0]) - (int64_t)(X[2] - X[0]) * (Y[1] - Y[0]);
    CHECK(area2 > 0, "orientation");
    CHECK(xmin <= xmax && ymin <= ymax && xmax < W && ymax < H, "bbox");
    /* edges: A, B exact; E at the four bbox corners == exact edge function - bias, in int32 */
    for (i = 0; i < 3; i++) {
        int a = (i + 1) % 3, b = (i + 2) % 3, k;
        int64_t A = -16 * (int64_t)(Y[b] - Y[a]), B = 16 * (int64_t)(X[b] - X[a]);
        int bias = topleft(X[b] - X[a], Y[b] - Y[a]) ? 0 : 1;
        CHECK(s32(r[2 + 3 * i]) == A && s32(r[3 + 3 * i]) == B, "A/B edge %d", i);
        for (k = 0; k < 4; k++) {
            int px = k & 1 ? xmax : xmin, py = k & 2 ? ymax : ymin;
            int64_t cx = 16 * (int64_t)px + 8, cy = 16 * (int64_t)py + 8;
            int64_t f = (int64_t)(X[b] - X[a]) * (cy - Y[a]) - (int64_t)(Y[b] - Y[a]) * (cx - X[a]) - bias;
            int64_t e = (int64_t)s32(r[4 + 3 * i]) + (int64_t)s32(r[2 + 3 * i]) * (px - xmin) +
                        (int64_t)s32(r[3 + 3 * i]) * (py - ymin);
            CHECK(e == f, "edge %d corner %d: %lld != %lld", i, k, (long long)e, (long long)f);
            CHECK(e >= INT32_MIN && e <= INT32_MAX, "edge value out of int32");
        }
    }
    if (grad_clamped(r)) { (*clampcnt)++; return; }
    /* attribute values at random covered pixels, evaluated like the hardware (modulo 2^32, as
     * int32), vs the exact plane equation: proves no overflow corrupts a covered pixel */
    for (s = 0; s < 48; s++) {
        int px = xmin + (int)(rnext() % (uint64_t)(xmax - xmin + 1));
        int py = ymin + (int)(rnext() % (uint64_t)(ymax - ymin + 1));
        double cx = 16.0 * px + 8, cy = 16.0 * py + 8;
        if (!rec_cov(r, px, py))
            continue;
        for (ch = 0; ch < 4; ch++) {
            long double ex = exact_attr(v, ch, cx, cy);
            int64_t got = rec_attr64(r, 11 + 3 * ch, px, py);
            double err = fabs((double)((long double)got - ex));
            double tol = 1.0 + 0.5 * ((px - xmin) + (py - ymin)) + 1e-6;
            CHECK(err <= tol, "attr %d err %g tol %g", ch, err, tol);
            if (err > *maxerr) *maxerr = err;
        }
        (*samples)++;
    }
}

static void test_range_random(void)
{
    int iter, clamped = 0, recs = 0, pieces_max = 0;
    long samples = 0;
    double maxerr = 0;
    begin("int32_ranges_random");
    rseed(0x7007);
    /* 1: vertices anywhere in the guard band (huge and small triangles), noclip + clip identical */
    for (iter = 0; iter < 30000; iter++) {
        gpu_vtx v[3];
        uint32_t r[RW], o[GPU_SETUP_MAX_OUT][RW];
        int i, ret, m, mode = (int)(rnext() % 5);
        int kind = iter % 3;
        double cx = -256 + runif() * 1792, cy = -256 + runif() * 1232;
        double sz = kind == 0 ? 4000 : kind == 1 ? 300 : 20;
        for (i = 0; i < 3; i++) {
            double x = cx + (runif() - 0.5) * sz, y = cy + (runif() - 0.5) * sz;
            if (x < -256) x = -256;
            if (x > 1535.9) x = 1535.9;
            if (y < -256) y = -256;
            if (y > 975.9) y = 975.9;
            {
                float z = (float)(runif() * 1.2 - 0.1);
                v[i] = mkv_rgb(quant(x, mode), quant(y, mode), z);
            }
        }
        ret = gpu_setup_tri_noclip(v, GPU_F_ZTEST, GPU_CULL_NONE, r);
        CHECK(ret == 0 || ret == 1, "noclip returned %d inside guard band", ret);
        m = gpu_setup_tri(v, GPU_F_ZTEST, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
        CHECK(m == ret && (m == 0 || memcmp(o[0], r, sizeof r) == 0), "clip variant differs inside guard band");
        if (ret == 1) {
            hash_rec(r);
            recs++;
            check_values(r, v, &samples, &clamped, &maxerr);
        }
    }
    /* 2: triangles far outside: every piece goes through the same checks */
    for (iter = 0; iter < 6000; iter++) {
        gpu_vtx v[3];
        uint32_t o[GPU_SETUP_MAX_OUT][RW];
        int i, m, k;
        double sz = iter % 2 ? 20000 : 5000;
        for (i = 0; i < 3; i++) {
            float x = (float)(640 + (runif() - 0.5) * sz);
            float y = (float)(360 + (runif() - 0.5) * sz);
            float z = (float)runif();
            v[i] = mkv_rgb(x, y, z);
        }
        m = gpu_setup_tri(v, 0, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
        CHECK(m >= 0 && m <= 5, "pieces %d", m);
        if (m > pieces_max) pieces_max = m;
        for (k = 0; k < m; k++) {
            hash_rec(o[k]);
            recs++;
            /* edge/bbox range checks on the piece (values vs the piece's own vertices are
             * covered by the watertight tests; here: bbox + int32 at corners) */
            {
                int xmin = R_XMIN(o[k]), xmax = R_XMAX(o[k]), ymin = R_YMIN(o[k]), ymax = R_YMAX(o[k]), c, e;
                CHECK(xmin <= xmax && ymin <= ymax && xmax < W && ymax < H, "piece bbox");
                for (e = 0; e < 3; e++)
                    for (c = 0; c < 4; c++) {
                        int64_t dx = c & 1 ? xmax - xmin : 0, dy = c & 2 ? ymax - ymin : 0;
                        int64_t ev = (int64_t)s32(o[k][4 + 3 * e]) + (int64_t)s32(o[k][2 + 3 * e]) * dx +
                                     (int64_t)s32(o[k][3 + 3 * e]) * dy;
                        CHECK(ev >= INT32_MIN && ev <= INT32_MAX, "piece edge out of int32");
                    }
            }
        }
    }
    {
        char buf[128];
        snprintf(buf, sizeof buf, "%d records, %ld pixel samples, max attr err %.3f, %d clamped slivers, max %d pieces",
                 recs, samples, maxerr, clamped, pieces_max);
        end_test(buf);
    }
}

/* gpu_refrast_frame (row-wise, incremental) == literal full-frame SPEC 5 on random lists */
static void test_refrast_vs_naive(void)
{
    static uint32_t recs[400 * RW];
    int iter, frames_bad = 0;
    begin("refrast_vs_naive");
    rseed(0x8008);
    for (iter = 0; iter < 24; iter++) {
        int n = 0, target = 20 + (int)(rnext() % 300), k;
        while (n < target) {
            int what = (int)(rnext() % 10);
            uint32_t *r = recs + n * RW;
            if (what < 4) {                         /* setup triangle, random flags */
                gpu_vtx v[3];
                uint32_t o[GPU_SETUP_MAX_OUT][RW];
                int i, m;
                double cx = runif() * W, cy = runif() * H, sz = what == 0 ? 3000 : 150;
                uint32_t fl = (uint32_t)(rnext() & 3) << 24;
                for (i = 0; i < 3; i++) {
                    float x = (float)(cx + (runif() - 0.5) * sz);
                    float y = (float)(cy + (runif() - 0.5) * sz);
                    float z = (float)runif();
                    v[i] = mkv_rgb(x, y, z);
                }
                m = gpu_setup_tri(v, fl, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
                for (i = 0; i < m && n < target; i++)
                    memcpy(recs + (n++) * RW, o[i], sizeof o[i]);
            } else if (what == 4) {                 /* rect */
                int x0 = rint_in(-50, 1300), y0 = rint_in(-50, 740);
                int rw = rint_in(0, 300), rh = rint_in(0, 200);
                uint16_t col = (uint16_t)rnext();
                float z = (float)runif();
                uint32_t fl = (uint32_t)(rnext() & 3) << 24;
                if (gpu_setup_rect(x0, y0, x0 + rw, y0 + rh, col, z, fl, r) == 1)
                    n++;
            } else if (what == 5) {                 /* sprite, hashed DDR */
                int x = 4 * rint_in(-20, 330), y = rint_in(-50, 740);
                int sw = 4 * rint_in(1, 64), sh = rint_in(1, 120);
                uint32_t src = 0x1E400000u + 8u * (uint32_t)rint_in(0, 1000);
                uint32_t stride = 8u * (uint32_t)rint_in(0, 200);
                int ck = (int)(rnext() & 1);
                if (gpu_setup_sprite(x, y, sw, sh, src, stride, ck, 0xF81F, r) == 1)
                    n++;
            } else if (what == 6) {                 /* raw TRI: random words, bbox inside screen */
                int i, x0 = rint_in(0, W - 1), y0 = rint_in(0, H - 1);
                int x1 = x0 + rint_in(0, 200), y1 = y0 + rint_in(0, 100);
                for (i = 0; i < RW; i++) r[i] = (uint32_t)rnext();
                if (rnext() & 1) {   /* small signed magnitudes: mixed coverage */
                    for (i = 2; i <= 10; i++) r[i] = (uint32_t)((int32_t)(r[i] & 0xFFFFFu) - 0x80000);
                }
                if (x1 > W - 1) x1 = W - 1;
                if (y1 > H - 1) y1 = H - 1;
                r[0] = (1u << 28) | (r[0] & (0xFu << 24)) | ((uint32_t)x1 << 11) | (uint32_t)x0;
                r[1] = ((uint32_t)y1 << 11) | (uint32_t)y0;
                n++;
            } else if (what == 7) {                 /* raw SPRITE, random geometry */
                int i;
                for (i = 0; i < RW; i++) r[i] = 0;
                r[0] = (2u << 28) | (uint32_t)(rnext() & 1) << 24;
                r[1] = (uint32_t)rint_in(0, 719) << 11;
                r[1] |= (uint32_t)(4 * rint_in(0, 319));
                r[2] = (uint32_t)rint_in(1, 200) << 11;
                r[2] |= (uint32_t)(4 * rint_in(1, 100));
                r[3] = 0x1E400000u + 8u * (uint32_t)rint_in(0, 100000);
                r[4] = 8u * (uint32_t)rint_in(0, 400);
                r[5] = rnext() & 1 ? 0xF81Fu : (uint32_t)rnext();
                n++;
            } else {                                /* NOP / END / unknown types: ignored */
                int i;
                for (i = 0; i < RW; i++) r[i] = (uint32_t)rnext();
                r[0] = (r[0] & 0x0FFFFFFFu) | ((uint32_t)(what == 8 ? 0 : (rnext() & 1) ? 15 : rint_in(3, 14)) << 28);
                n++;
            }
        }
        {
            uint16_t clear = (uint16_t)(0x1111u * (uint32_t)(iter + 1));
            gpu_refrast_frame(recs, n, clear, hash_ddr, NULL, g_fb);
            naive_frame(recs, n, clear, hash_ddr, NULL, g_fb2);
            hash_frame(g_fb);
        }
        k = memcmp(g_fb, g_fb2, sizeof g_fb) != 0;
        if (k) {
            int p;
            for (p = 0; p < W * H; p++)
                if (g_fb[p] != g_fb2[p]) {
                    CHECK(0, "frame %d: first diff at (%d,%d): %04x vs %04x", iter, p % W, p / W, g_fb[p], g_fb2[p]);
                    break;
                }
        }
        frames_bad += k;
        CHECK(!k, "frame %d differs", iter);
    }
    /* refrast coverage == counter coverage for single setup triangles */
    for (iter = 0; iter < 200; iter++) {
        gpu_vtx v[3];
        uint32_t r[RW];
        int i, bad = 0;
        for (i = 0; i < 3; i++) {
            double x = runif() * 1300 - 10;
            double y = runif() * 740 - 10;
            v[i] = mkv(quant(x, iter % 5), quant(y, iter % 5), 0, 255, 255, 255);
        }
        if (gpu_setup_tri_noclip(v, 0, GPU_CULL_NONE, r) != 1)
            continue;
        memset(g_cnt, 0, sizeof g_cnt);
        count_record(r);
        gpu_refrast_frame(r, 1, 0x0000, NULL, NULL, g_fb);
        for (i = 0; i < W * H; i++)
            if ((g_fb[i] != 0) != (g_cnt[i] != 0)) bad++;
        CHECK(bad == 0, "coverage mismatch %d pixels", bad);
    }
    /* nrecs <= 0 / NULL list: just the clear colour */
    gpu_refrast_frame(NULL, 5, 0x1234, NULL, NULL, g_fb);
    CHECK(g_fb[0] == 0x1234 && g_fb[W * H - 1] == 0x1234, "NULL list");
    gpu_refrast_frame(recs, -1, 0x4321, NULL, NULL, g_fb);
    CHECK(g_fb[0] == 0x4321 && g_fb[W * H - 1] == 0x4321, "negative count");
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%d frames differ", frames_bad);
        end_test(buf);
    }
}

/* hand-made records exercising uint32 wrap-around and the clamps */
static void test_refrast_wrap(void)
{
    uint32_t r[RW];
    begin("refrast_wrap_clamp");
    memset(r, 0, sizeof r);
    /* E0 = INT32_MAX - 5, A = 1: covered for dx <= 5, wraps negative after -> not covered */
    r[0] = (1u << 28) | (20u << 11) | 0u;
    r[1] = (0u << 11) | 0u;
    r[2] = 1; r[3] = 0; r[4] = 0x7FFFFFFAu;
    r[5] = 0; r[6] = 0; r[7] = 0;
    r[8] = 0; r[9] = 0; r[10] = 0;
    r[14] = 0xFFFFFFFFu;        /* -1 -> 0 */
    r[15] = 0x00800000u;        /* +128 per pixel */
    r[17] = 300u << 16;         /* > 255 -> 255 */
    r[20] = 0x7FFFFFFFu;        /* -> 255 */
    r[21] = 0x80000000u;        /* wraps: dx odd -> negative -> 0 */
    gpu_refrast_frame(r, 1, 0x0000, NULL, NULL, g_fb);
    {
        int x, ok = 1;
        for (x = 0; x <= 20; x++) {
            uint16_t exp = 0;
            if (x <= 5) {
                int32_t cr = s32(0xFFFFFFFFu + 0x00800000u * (uint32_t)x);
                int32_t cb = s32(0x7FFFFFFFu + 0x80000000u * (uint32_t)x);
                int r8 = cr < 0 ? 0 : (cr >> 16) > 255 ? 255 : cr >> 16;
                int b8 = cb < 0 ? 0 : (cb >> 16) > 255 ? 255 : cb >> 16;
                exp = (uint16_t)(((r8 >> 3) << 11) | ((255 >> 2) << 5) | (b8 >> 3));
            }
            if (g_fb[x] != exp) ok = 0;
        }
        CHECK(ok, "wrap-around coverage/colour");
        CHECK(g_fb[6] == 0 && g_fb[21] == 0 && g_fb[W] == 0, "outside");
    }
    /* bbox beyond the screen (never produced by setup) is clipped, no out-of-bounds writes */
    memset(r, 0, sizeof r);
    r[0] = (1u << 28) | GPU_F_NOEDGE | (2047u << 11) | 1270u;
    r[1] = (2047u << 11) | 710u;
    r[14] = 255u << 16;
    gpu_refrast_frame(r, 1, 0x0000, NULL, NULL, g_fb);
    CHECK(g_fb[719 * W + 1279] == 0xF800 && g_fb[710 * W + 1270] == 0xF800 && g_fb[709 * W + 1270] == 0,
          "clipped bbox");
    /* xmin > xmax draws nothing */
    r[0] = (1u << 28) | GPU_F_NOEDGE | (10u << 11) | 20u;
    r[1] = (10u << 11) | 0u;
    gpu_refrast_frame(r, 1, 0x0000, NULL, NULL, g_fb);
    {
        int i, nz = 0;
        for (i = 0; i < W * H; i++) nz += g_fb[i] != 0;
        CHECK(nz == 0, "inverted bbox drew %d pixels", nz);
    }
    end_test(NULL);
}

int main(void)
{
    printf("gpu common tests (sizeof(gpu_vtx)=%u, sizeof(long double)=%u)\n",
           (unsigned)sizeof(gpu_vtx), (unsigned)sizeof(long double));
    if (sizeof(gpu_vtx) != 16) { printf("gpu_vtx is not 16 bytes\n"); return 1; }
    test_known_record();
    test_topleft();
    test_geo_vs_record();
    test_cull();
    test_degenerate();
    test_watertight_grid();
    test_watertight_fan();
    test_watertight_clipped();
    test_z();
    test_gouraud();
    test_rect();
    test_sprite();
    test_range_random();
    test_refrast_wrap();
    test_refrast_vs_naive();
    printf("record hash %016llx over %ld records, frame hash %016llx\n", (unsigned long long)g_hash, g_hashed,
           (unsigned long long)g_fhash);
    printf("TOTAL %ld checks, %ld failures: %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}

/*
 * gen_scene.c -- deterministic test scenes for the RTL testbench (and previews).
 *
 *   gen_scene <scene> <outdir>      write one scene directory
 *   gen_scene all <outroot>         write every scene to <outroot>/<name>/
 *   gen_scene list                  list scene names
 *
 * Scene directory:
 *   rec.hex       one 32-bit word per line (8 lowercase hex digits), the scene's records back to
 *                 back (24 lines each) in list order, then one END record (frame_no 1)
 *   ddr.hex       $readmemh image of a 64-bit x 1M-word DDR model of 0x1E000000..0x1E7FFFFF:
 *                 '@<hex word index>' lines, then 16-hex-digit words; word index = (addr-0x1E000000)/8.
 *                 Only sprite source data is present (a scene without sprites gets one zero word
 *                 at the pool base so the file is never empty); the testbench zero-fills the rest.
 *   cfg.txt       clear_color=XXXX (hex), nrec=N (records before END, as sent),
 *                 nrender=M (records the PL keeps = min(N,1536)); decimal N, M
 *   expected.hex  230400 lines of 16 hex digits: the frame as 64-bit DDR beats, beat k holds
 *                 pixels 4k..4k+3, pixel 4k in bits [15:0]
 *   expected.ppm  preview (RGB565 expanded like the scanout: r8 = {r5, r5[4:2]} ...)
 *
 * All scenes consist of TRI and SPRITE records only (every record occupies a list slot).
 * Deterministic: integer PRNG, no libm transcendental functions, fp-contract off; the output is
 * byte-identical on x86-64, AArch64 and ARMv7 (checked by tests/Makefile 'crosstest').
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gpu_setup.h"
#include "gpu_refrast.h"

#define RW          GPU_REC_WORDS
#define WIN_BASE    0x1E000000u
#define WIN_SIZE    0x00800000u                 /* 8 MB modelled by the testbench */
#define WIN_WORDS   (WIN_SIZE / 8u)             /* 1M 64-bit words */
#define POOL_LO     GPU_POOL_ADDR               /* 0x1E400000 */
#define POOL_HI     (WIN_BASE + WIN_SIZE)       /* 0x1E800000, exclusive */
#define KEY         0xF81Fu                     /* magenta colour key */

typedef struct {
    const char *name;
    uint32_t   *recs;
    int         n, cap;
    uint16_t    clear;
    uint16_t   *ddr;         /* WIN_SIZE/2 pixels */
    uint8_t    *present;     /* per 64-bit word: holds sprite data */
    uint32_t    pool_next;
    uint64_t    rs;
} scene;

static void die(const char *msg)
{
    fprintf(stderr, "gen_scene: %s\n", msg);
    exit(2);
}

/* ---- PRNG (splitmix64) ------------------------------------------------------------------- */
static uint64_t rnext(scene *s)
{
    uint64_t z = (s->rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double runif(scene *s) { return (double)(rnext(s) >> 11) * (1.0 / 9007199254740992.0); }
static uint32_t rgb_rand(scene *s) { return (uint32_t)(rnext(s) & 0xFFFFFFu); }

/* ---- record list ------------------------------------------------------------------------- */
static void add_rec(scene *s, const uint32_t *r)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->recs = (uint32_t *)realloc(s->recs, (size_t)s->cap * RW * sizeof(uint32_t));
        if (!s->recs) die("out of memory");
    }
    memcpy(s->recs + (size_t)s->n * RW, r, RW * sizeof(uint32_t));
    s->n++;
}

static gpu_vtx V(double x, double y, double z, uint32_t rgb)
{
    gpu_vtx v;
    v.x = (float)x; v.y = (float)y; v.z = (float)z;
    v.r = (uint8_t)(rgb >> 16); v.g = (uint8_t)(rgb >> 8); v.b = (uint8_t)rgb; v.a = 0;
    return v;
}

/* triangle through the clipping setup; returns the number of records appended */
static int tri(scene *s, gpu_vtx a, gpu_vtx b, gpu_vtx c, uint32_t flags)
{
    gpu_vtx v[3];
    uint32_t o[GPU_SETUP_MAX_OUT][RW];
    int m, i;
    v[0] = a; v[1] = b; v[2] = c;
    m = gpu_setup_tri(v, flags, GPU_CULL_NONE, o, GPU_SETUP_MAX_OUT);
    for (i = 0; i < m; i++)
        add_rec(s, o[i]);
    return m;
}

static void rect(scene *s, int x0, int y0, int x1, int y1, uint32_t rgb, double z, uint32_t flags)
{
    uint32_t r[RW];
    if (gpu_setup_rect(x0, y0, x1, y1, gpu_rgb565(rgb >> 16, (rgb >> 8) & 255, rgb & 255), (float)z,
                       flags, r) == 1)
        add_rec(s, r);
}

static uint32_t rainbow(int k, int n)
{
    int t = (int)((long)k * 1536 / n) % 1536, u = t % 256;
    switch (t / 256) {
    case 0:  return 0xFF0000u | (uint32_t)u << 8;
    case 1:  return (uint32_t)(255 - u) << 16 | 0x00FF00u;
    case 2:  return 0x00FF00u | (uint32_t)u;
    case 3:  return (uint32_t)(255 - u) << 8 | 0x0000FFu;
    case 4:  return (uint32_t)u << 16 | 0x0000FFu;
    default: return 0xFF0000u | (uint32_t)(255 - u);
    }
}

/* ---- sprite pool -------------------------------------------------------------------------- */
enum { PAT_GRAD, PAT_RING, PAT_CHECK, PAT_STRIPE };

static uint16_t pattern(int pat, int x, int y, int w, int h)
{
    uint16_t p;
    switch (pat) {
    case PAT_GRAD:
        p = (uint16_t)(((x * 31 / (w - 1)) << 11) | ((y * 63 / (h - 1)) << 5) | (31 - x * 31 / (w - 1)));
        break;
    case PAT_RING: {
        long dx = 2 * x + 1 - w, dy = 2 * y + 1 - h, d2 = dx * dx + dy * dy;
        long ro = (long)(h < w ? h : w) * (h < w ? h : w), ri = ro / 4;
        if (d2 > ro || d2 < ri)
            return KEY;
        p = (uint16_t)((((x / 8 + y / 8) & 1) ? 0xFFE0u : 0x07FFu) ^ (uint16_t)(x & 3));
        break;
    }
    case PAT_CHECK:
        if (((x / 10) + (y / 10)) % 3 == 0)
            return KEY;
        p = (uint16_t)(0x8000u | (uint16_t)(x * 64 + y));
        break;
    default:
        p = (uint16_t)((y & 4) ? 0xFFFFu : (uint16_t)(0x001Fu | ((x & 31) << 11)));
        break;
    }
    return p == KEY ? (uint16_t)(p ^ 1u) : p;
}

/* Place a w x h sprite with the given stride at src (8-aligned, inside the pool). */
static void sprite_fill(scene *s, uint32_t src, int w, int h, uint32_t stride, int pat)
{
    int x, y;
    if ((src & 7u) || (stride & 7u) || (w & 3) || src < POOL_LO ||
        (uint64_t)src + (uint64_t)(h - 1) * stride + (uint64_t)w * 2 > POOL_HI)
        die("bad sprite placement");
    for (y = 0; y < h; y++) {
        uint32_t row = src + (uint32_t)y * stride - WIN_BASE;
        for (x = 0; x < w; x++)
            s->ddr[(row + 2u * (uint32_t)x) / 2u] = pattern(pat, x, y, w, h);
        for (x = 0; x < w; x += 4)
            s->present[(row + 2u * (uint32_t)x) / 8u] = 1;
    }
}

/* bump-allocate: stride = w*2 rounded up to 8 (+ extra), optional 4 KB phase */
static uint32_t sprite_alloc(scene *s, int w, int h, uint32_t extra_stride, int phase4k, int pat,
                             uint32_t *stride_out)
{
    uint32_t stride = (((uint32_t)w * 2u + 7u) & ~7u) + extra_stride;
    uint32_t src = (s->pool_next + 7u) & ~7u;
    if (phase4k >= 0) {
        src = (src + 4095u) & ~4095u;
        src += (uint32_t)phase4k;
    }
    sprite_fill(s, src, w, h, stride, pat);
    s->pool_next = src + (uint32_t)h * stride;
    *stride_out = stride;
    return src;
}

static void sprite(scene *s, int x, int y, int w, int h, uint32_t src, uint32_t stride, int ck)
{
    uint32_t r[RW];
    int ret = gpu_setup_sprite(x, y, w, h, src, stride, ck, KEY, r);
    if (ret < 0) die("sprite setup rejected arguments");
    if (ret == 1) add_rec(s, r);
}

/* ---- scene content ------------------------------------------------------------------------ */
static void c_one_tri(scene *s)
{
    tri(s, V(340.3, 100.7, 0, 0xFF8000), V(980.6, 250.2, 0, 0xFF8000), V(500.1, 620.9, 0, 0xFF8000), 0);
}

static void c_rects(scene *s)
{
    const uint32_t ZTW = GPU_F_ZTEST | GPU_F_ZWRITE;
    int i, j;
    /* 1-pixel border */
    rect(s, 0, 0, 1280, 1, 0xFFFF00, 0, 0);
    rect(s, 0, 719, 1280, 720, 0xFFFF00, 0, 0);
    rect(s, 0, 0, 1, 720, 0xFFFF00, 0, 0);
    rect(s, 1279, 0, 1280, 720, 0xFFFF00, 0, 0);
    /* rows straddling strip boundaries (15|16, 31|32) */
    rect(s, 100, 15, 1100, 17, 0xFFFFFF, 0, 0);
    rect(s, 100, 31, 1100, 33, 0xC0C0C0, 0, 0);
    /* palette tiles, odd sizes and positions */
    for (j = 0; j < 4; j++)
        for (i = 0; i < 16; i++) {
            uint32_t c = ((uint32_t)(i * 17) << 16) | ((uint32_t)(j * 85) << 8) | (uint32_t)(255 - i * 17);
            rect(s, 41 + i * 75, 41 + j * 48, 41 + i * 75 + 61 + (i & 1), 41 + j * 48 + 39 + (j & 1), c, 0, 0);
        }
    /* clipped at every edge */
    rect(s, -50, -30, 200, 100, 0xFF0000, 0, 0);
    rect(s, 1180, 650, 1400, 800, 0x00FF00, 0, 0);
    rect(s, -10, 690, 60, 740, 0x00FFFF, 0, 0);
    rect(s, 1250, -20, 1300, 30, 0xFF00FF, 0, 0);
    /* thin columns */
    rect(s, 3, 250, 4, 650, 0xFF00FF, 0, 0);
    rect(s, 5, 250, 7, 650, 0x00FF00, 0, 0);
    rect(s, 1275, 250, 1276, 650, 0xFFFFFF, 0, 0);
    /* Z: A blue z.5, B orange z.7 behind A, C red z.3 test-only, D green equal z, E white z.4 */
    rect(s, 300, 280, 700, 500, 0x0000FF, 0.5, ZTW);
    rect(s, 500, 380, 900, 600, 0xFF8000, 0.7, ZTW);
    rect(s, 600, 300, 800, 350, 0xFF0000, 0.3, GPU_F_ZTEST);
    rect(s, 350, 450, 450, 560, 0x00FF00, 0.5, GPU_F_ZTEST);
    rect(s, 650, 320, 760, 340, 0xFFFFFF, 0.4, ZTW);
    /* F grey z.9, G hidden behind F (fails everywhere), H in front */
    rect(s, 950, 300, 1200, 600, 0x808080, 0.9, ZTW);
    rect(s, 1000, 350, 1150, 550, 0xFF0000, 0.95, GPU_F_ZTEST);
    rect(s, 1050, 400, 1100, 500, 0x00FFFF, 0.85, GPU_F_ZTEST);
    /* write-only Z then a test against it */
    rect(s, 100, 620, 300, 700, 0x404000, 0.2, GPU_F_ZWRITE);
    rect(s, 150, 600, 250, 715, 0xFFFF00, 0.25, GPU_F_ZTEST);
    /* empty / off-screen (produce no record) */
    rect(s, 10, 10, 10, 20, 0xFFFFFF, 0, 0);
    rect(s, 1300, 10, 1400, 20, 0xFFFFFF, 0, 0);
}

static void c_zbuf(scene *s, int count)
{
    int k, i;
    for (k = 0; k < count; k++) {
        double cx = -100 + runif(s) * 1480, cy = -100 + runif(s) * 920;
        double u = runif(s), sz = 20 + 330 * u * u;
        int flat = (int)(rnext(s) & 1);
        uint32_t base = rgb_rand(s);
        gpu_vtx v[3];
        for (i = 0; i < 3; i++) {
            /* one PRNG call per statement: argument evaluation order is unspecified in C */
            double x = cx + (runif(s) - 0.5) * 2 * sz;
            double y = cy + (runif(s) - 0.5) * 2 * sz;
            double z = 0.02 + 0.96 * runif(s);
            uint32_t c = flat ? base : rgb_rand(s);
            v[i] = V(x, y, z, c);
        }
        tri(s, v[0], v[1], v[2], GPU_F_ZTEST | GPU_F_ZWRITE);
    }
}

/* 16 directions from Pythagorean triples (exact rationals, no libm) */
static const double DIR[16][2] = {
    { 1, 0 }, { 0.96, 0.28 }, { 0.8, 0.6 }, { 0.6, 0.8 }, { 0.28, 0.96 }, { 0, 1 },
    { -0.6, 0.8 }, { -0.8, 0.6 }, { -1, 0 }, { -0.8, -0.6 }, { -0.6, -0.8 }, { -0.28, -0.96 },
    { 0, -1 }, { 0.6, -0.8 }, { 0.8, -0.6 }, { 0.96, -0.28 } };

static void c_gouraud(scene *s)
{
    int k;
    /* big RGB triangle */
    tri(s, V(640.5, 30.5, 0, 0xFF0000), V(80.5, 700.5, 0, 0x00FF00), V(1200.5, 700.5, 0, 0x0000FF), 0);
    /* quad with four corner colours */
    tri(s, V(20, 20, 0, 0xFFFFFF), V(300, 20, 0, 0xFF0000), V(300, 200, 0, 0x000000), 0);
    tri(s, V(20, 20, 0, 0xFFFFFF), V(300, 200, 0, 0x000000), V(20, 200, 0, 0x0000FF), 0);
    /* grey ramp */
    tri(s, V(900, 20, 0, 0x000000), V(1260, 20, 0, 0xFFFFFF), V(1260, 80, 0, 0xFFFFFF), 0);
    tri(s, V(900, 20, 0, 0x000000), V(1260, 80, 0, 0xFFFFFF), V(900, 80, 0, 0x000000), 0);
    /* rainbow fan with a white centre, z-tested against each other */
    for (k = 0; k < 16; k++) {
        int j = (k + 1) % 16;
        tri(s, V(1040.25, 330.75, 0.1, 0xFFFFFF),
            V(1040.25 + 170 * DIR[k][0], 330.75 + 170 * DIR[k][1], 0.9, rainbow(k, 16)),
            V(1040.25 + 170 * DIR[j][0], 330.75 + 170 * DIR[j][1], 0.9, rainbow(j, 16)),
            GPU_F_ZTEST | GPU_F_ZWRITE);
    }
    /* a thin wedge whose bbox corner is far away (start values beyond int32) */
    tri(s, V(0.5, 719.5, 0.5, 0x8000FF), V(1279.5, 0.5, 0.0, 0x00FF00), V(1279.5, 40.5, 1.0, 0xFF0080),
        GPU_F_ZTEST | GPU_F_ZWRITE);
    /* saturation: colours extrapolated past 0/255 inside the bbox but not the triangle */
    tri(s, V(100, 300, 0, 0x00FF00), V(400, 330, 0, 0xFF00FF), V(120, 560, 0, 0xFFFF00), 0);
}

static void c_sprites(scene *s)
{
    uint32_t st, a;
    uint32_t ring, ring_st;
    /* S1: gradient, no key, rows cross strip boundaries */
    a = sprite_alloc(s, 64, 48, 0, -1, PAT_GRAD, &st);
    sprite(s, 100, 100, 64, 48, a, st, 0);
    /* S2: ring with key holes, clipped at the right edge */
    ring = sprite_alloc(s, 128, 96, 0, -1, PAT_RING, &ring_st);
    sprite(s, 1200, 300, 128, 96, ring, ring_st, 1);
    /* S3: checker with key, padded stride, clipped at right and bottom */
    a = sprite_alloc(s, 100, 80, 56, -1, PAT_CHECK, &st);
    sprite(s, 1180, 660, 100, 80, a, st, 1);
    /* S4: negative x/y (setup advances src) */
    a = sprite_alloc(s, 64, 40, 0, -1, PAT_CHECK, &st);
    sprite(s, -20, -12, 64, 40, a, st, 1);
    /* S5: the ring again without colour key -> magenta visible */
    sprite(s, 400, 400, 128, 96, ring, ring_st, 0);
    /* S6: source starts 8 bytes before a 4 KB boundary, odd stride: rows cross 4 KB pages */
    a = sprite_alloc(s, 32, 20, 8, 4088, PAT_STRIPE, &st);
    sprite(s, 700, 150, 32, 20, a, st, 1);
    /* S7: last bytes of the modelled window (ends exactly at 0x1E800000) */
    a = POOL_HI - 16u * 64u;
    sprite_fill(s, a, 32, 16, 64, PAT_GRAD);
    sprite(s, 200, 600, 32, 16, a, 64, 0);
}

static void c_sprites_background(scene *s)
{
    tri(s, V(-100, -100, 0, 0x203060), V(1500, 200, 0, 0x602030), V(300, 900, 0, 0x206030), 0);
    rect(s, 1150, 280, 1280, 420, 0x00FF00, 0, 0);     /* behind S2's visible part */
    rect(s, 1170, 640, 1280, 720, 0xFFFFFF, 0, 0);     /* behind S3 */
    rect(s, 0, 0, 40, 30, 0xFF0000, 0, 0);             /* behind S4 */
    rect(s, 690, 140, 740, 180, 0x0000FF, 0, 0);       /* behind S6 */
}

static void c_strips(scene *s, int mesh_nx, int mesh_ny)
{
    int k, i, j;
    /* triangles needing guard-band clipping, with edges crossing the screen (drawn first) */
    tri(s, V(-3000, -200, 0, 0x800000), V(2500, 300, 0, 0x008000), V(-500, 3000, 0, 0x000080), 0);
    tri(s, V(1100, -1000, 0, 0xFFFF00), V(1700, 400, 0, 0x00FFFF), V(900, 900, 0, 0xFF00FF), 0);
    tri(s, V(-600, 500, 0, 0xFFFFFF), V(300, 650, 0, 0x000000), V(100, 1400, 0, 0x808080), 0);
    tri(s, V(-1e6, 700, 0, 0x00FF80), V(1e6, 710, 0, 0x00FF80), V(0, 719.5, 0, 0x00FF80), 0);
    /* rotated checkerboard mesh (cos .96, sin .28), crosses many strip boundaries */
    {
        double cx = 640, cy = 360, cs = 0.96, sn = 0.28, cell = 44;
        static double MX[40][40], MY[40][40];
        for (j = 0; j <= mesh_ny; j++)
            for (i = 0; i <= mesh_nx; i++) {
                double u = (i - mesh_nx / 2.0) * cell, w = (j - mesh_ny / 2.0) * cell;
                MX[j][i] = cx + u * cs - w * sn;
                MY[j][i] = cy + u * sn + w * cs;
            }
        for (j = 0; j < mesh_ny; j++)
            for (i = 0; i < mesh_nx; i++) {
                uint32_t c1 = ((i + j) & 1) ? 0x3050A0 : 0xA05030, c2 = ((i + j) & 1) ? 0x4060B0 : 0xB06040;
                gpu_vtx a = V(MX[j][i], MY[j][i], 0, c1), b = V(MX[j][i + 1], MY[j][i + 1], 0, c1);
                gpu_vtx c = V(MX[j + 1][i + 1], MY[j + 1][i + 1], 0, c2), d = V(MX[j + 1][i], MY[j + 1][i], 0, c2);
                if ((i ^ j) & 2) { tri(s, a, b, c, 0); tri(s, a, c, d, 0); }
                else { tri(s, a, b, d, 0); tri(s, b, c, d, 0); }
            }
    }
    /* tall thin triangles through every strip and past the top/bottom edges */
    for (k = 0; k < 20; k++) {
        double x = 40 + k * 61.3;
        tri(s, V(x, -30, 0, rainbow(k, 20)), V(x + 4 + k * 0.37, 750, 0, 0xFFFFFF),
            V(x + 1.5, 750, 0, rainbow(k, 20)), 0);
    }
    /* 1-px slivers with the top edge exactly on the pixel centres of a strip's first row, and
     * quads split horizontally exactly on a strip boundary */
    for (k = 1; k < 45; k += 4) {
        double y = 16.0 * k;
        tri(s, V(200, y + 0.5, 0, 0xFFFFFF), V(1000, y + 0.5, 0, 0xFFFFFF), V(600, y + 1.5, 0, 0xFFFFFF), 0);
        tri(s, V(1050, y - 6, 0, 0xFF0000), V(1250, y, 0, 0xFF0000), V(1050, y, 0, 0xFF0000), 0);
        tri(s, V(1050, y, 0, 0x00FF00), V(1250, y, 0, 0x00FF00), V(1250, y + 6, 0, 0x00FF00), 0);
    }
    /* vertices exactly on the screen edges / corners */
    tri(s, V(0, 0, 0, 0xFF0000), V(200, 0, 0, 0xFF0000), V(0, 200, 0, 0xFF0000), 0);
    tri(s, V(1280, 720, 0, 0x00FF00), V(1080, 720, 0, 0x00FF00), V(1280, 520, 0, 0x00FF00), 0);
    tri(s, V(1280, 0, 0, 0x0000FF), V(1280, 150, 0, 0x0000FF), V(1130, 0, 0, 0x0000FF), 0);
    tri(s, V(0, 720, 0, 0xFFFF00), V(0, 570, 0, 0xFFFF00), V(150, 720, 0, 0xFFFF00), 0);
}

static void c_fill_small(scene *s, int target)
{
    while (s->n < target) {
        double cx = runif(s) * 1280, cy = runif(s) * 720, sz = 4 + runif(s) * 30;
        double z0 = runif(s), z1, z2, oy, ox;
        uint32_t c0 = rgb_rand(s), c1, c2;
        oy = runif(s); z1 = runif(s); c1 = rgb_rand(s);
        ox = runif(s); z2 = runif(s); c2 = rgb_rand(s);
        tri(s, V(cx, cy, z0, c0), V(cx + sz, cy + oy * sz, z1, c1), V(cx + ox * sz, cy + sz, z2, c2),
            GPU_F_ZTEST | GPU_F_ZWRITE);
    }
}

/* ---- scenes ------------------------------------------------------------------------------- */
static void s_one_tri(scene *s)  { s->clear = 0x0010; c_one_tri(s); }
static void s_rects(scene *s)    { s->clear = 0x2104; c_rects(s); }
static void s_zbuf(scene *s)     { s->clear = 0x0000; while (s->n < 300) c_zbuf(s, 1); }
static void s_gouraud(scene *s)  { s->clear = 0x0000; c_gouraud(s); }
static void s_sprites(scene *s)  { s->clear = 0x0841; c_sprites_background(s); c_sprites(s); }
static void s_strips(scene *s)   { s->clear = 0x0000; c_strips(s, 22, 14); }
static void s_empty(scene *s)    { s->clear = 0x39E7; }

static void s_mixed(scene *s)
{
    s->clear = 0x0010;
    /* far background, z 0.99 */
    tri(s, V(-4000, -4000, 0.99, 0x202040), V(6000, -4000, 0.99, 0x402020), V(-4000, 6000, 0.99, 0x204020),
        GPU_F_ZTEST | GPU_F_ZWRITE);
    tri(s, V(6000, -4000, 0.99, 0x402020), V(6000, 6000, 0.99, 0x202020), V(-4000, 6000, 0.99, 0x204020),
        GPU_F_ZTEST | GPU_F_ZWRITE);
    c_gouraud(s);
    c_rects(s);
    c_zbuf(s, 110);
    c_strips(s, 12, 8);
    c_sprites(s);            /* sprites over most things, then more z-tested triangles over them */
    c_zbuf(s, 110);
    c_fill_small(s, 800);
}

static void s_overflow(scene *s)
{
    int i, j, k;
    s->clear = 0x0000;
    /* 1536 small triangles on a 48 x 32 grid (exactly one record each) */
    for (j = 0; j < 32; j++)
        for (i = 0; i < 48; i++) {
            double x = 16 + i * 26, y = 8 + j * 22;
            uint32_t c = rainbow(i * 32 + j, 48 * 32);
            int before = s->n;
            if ((i + j) & 1)
                tri(s, V(x + 2, y + 2, 0, c), V(x + 24, y + 2, 0, c), V(x + 2, y + 20, 0, 0xFFFFFF), 0);
            else
                tri(s, V(x + 24, y + 2, 0, c), V(x + 24, y + 20, 0, c), V(x + 2, y + 20, 0, 0x000000), 0);
            if (s->n != before + 1) die("overflow: grid triangle did not give exactly one record");
        }
    /* 64 more: bright tall triangles that must NOT appear (list full at 1536) */
    for (k = 0; k < 64; k++) {
        int before = s->n;
        tri(s, V(100 + k * 16, 100, 0, 0xFFFFFF), V(116 + k * 16, 100, 0, 0xFF00FF),
            V(108 + k * 16, 620, 0, 0xFFFFFF), 0);
        if (s->n != before + 1) die("overflow: extra triangle did not give exactly one record");
    }
}

typedef struct { const char *name; void (*build)(scene *); uint64_t seed; } scene_def;
static const scene_def SCENES[] = {
    { "one_tri",  s_one_tri,  1 },
    { "rects",    s_rects,    2 },
    { "zbuf",     s_zbuf,     3 },
    { "gouraud",  s_gouraud,  4 },
    { "sprites",  s_sprites,  5 },
    { "strips",   s_strips,   6 },
    { "mixed",    s_mixed,    7 },
    { "overflow", s_overflow, 8 },
    { "empty",    s_empty,    9 },
};
#define NSCENES ((int)(sizeof SCENES / sizeof SCENES[0]))

/* ---- output ------------------------------------------------------------------------------- */
static uint16_t ddr_rd(void *user, uint32_t addr)
{
    const scene *s = (const scene *)user;
    if (addr >= WIN_BASE && addr - WIN_BASE < WIN_SIZE && !(addr & 1u))
        return s->ddr[(addr - WIN_BASE) / 2u];
    return 0;
}

static int mkdir_p(const char *path)
{
    char buf[1024];
    size_t i, len = strlen(path);
    if (len == 0 || len >= sizeof buf) return -1;
    memcpy(buf, path, len + 1);
    for (i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
            buf[i] = c;
        }
    }
    return 0;
}

static FILE *open_out(const char *dir, const char *file)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gen_scene: cannot write %s: %s\n", path, strerror(errno)); exit(2); }
    setvbuf(f, NULL, _IOFBF, 1 << 20);   /* few large writes (fast on /mnt/c under WSL) */
    return f;
}

static void close_out(FILE *f)
{
    if (ferror(f) || fclose(f) != 0) die("write error");
}

static int write_scene(const scene_def *d, const char *dir)
{
    scene sc;
    uint16_t *fb;
    uint32_t end[RW];
    int i, nrender, nonclear = 0, ddr_words = 0;
    uint64_t fbhash = 1469598103934665603ull;
    FILE *f;

    memset(&sc, 0, sizeof sc);
    sc.name = d->name;
    sc.rs = 0x5EED0000ull + d->seed;
    sc.pool_next = POOL_LO;
    sc.ddr = (uint16_t *)calloc(WIN_SIZE / 2, sizeof(uint16_t));
    sc.present = (uint8_t *)calloc(WIN_WORDS, 1);
    fb = (uint16_t *)malloc((size_t)GPU_W * GPU_H * sizeof(uint16_t));
    if (!sc.ddr || !sc.present || !fb) die("out of memory");
    d->build(&sc);
    nrender = sc.n < GPU_LIST_SLOTS ? sc.n : GPU_LIST_SLOTS;
    if (mkdir_p(dir) != 0) { fprintf(stderr, "gen_scene: cannot create %s\n", dir); exit(2); }

    /* rec.hex: records as sent + END(frame 1) */
    f = open_out(dir, "rec.hex");
    for (i = 0; i < sc.n * RW; i++)
        fprintf(f, "%08x\n", (unsigned)sc.recs[i]);
    gpu_make_end(1, end);
    for (i = 0; i < RW; i++)
        fprintf(f, "%08x\n", (unsigned)end[i]);
    close_out(f);

    /* cfg.txt */
    f = open_out(dir, "cfg.txt");
    fprintf(f, "clear_color=%04x\nnrec=%d\nnrender=%d\n", (unsigned)sc.clear, sc.n, nrender);
    close_out(f);

    /* ddr.hex */
    f = open_out(dir, "ddr.hex");
    {
        long w, last = -2;
        for (w = 0; w < (long)WIN_WORDS; w++) {
            const uint16_t *p;
            uint64_t word;
            if (!sc.present[w]) continue;
            p = sc.ddr + (size_t)w * 4;
            word = (uint64_t)p[0] | (uint64_t)p[1] << 16 | (uint64_t)p[2] << 32 | (uint64_t)p[3] << 48;
            if (w != last + 1) fprintf(f, "@%lx\n", w);
            fprintf(f, "%016llx\n", (unsigned long long)word);
            last = w;
            ddr_words++;
        }
        if (ddr_words == 0)
            fprintf(f, "@%x\n%016x\n", (unsigned)((POOL_LO - WIN_BASE) / 8u), 0u);
    }
    close_out(f);

    /* render the records the PL keeps */
    gpu_refrast_frame(sc.recs, nrender, sc.clear, ddr_rd, &sc, fb);

    f = open_out(dir, "expected.hex");
    for (i = 0; i < GPU_W * GPU_H; i += 4) {
        uint64_t word = (uint64_t)fb[i] | (uint64_t)fb[i + 1] << 16 | (uint64_t)fb[i + 2] << 32 |
                        (uint64_t)fb[i + 3] << 48;
        fprintf(f, "%016llx\n", (unsigned long long)word);
    }
    close_out(f);

    f = open_out(dir, "expected.ppm");
    fprintf(f, "P6\n%d %d\n255\n", GPU_W, GPU_H);
    for (i = 0; i < GPU_W * GPU_H; i++) {
        unsigned p = fb[i], r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
        unsigned char rgb[3];
        rgb[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
        rgb[1] = (unsigned char)((g6 << 2) | (g6 >> 4));
        rgb[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
        fwrite(rgb, 1, 3, f);
        nonclear += p != sc.clear;
        fbhash ^= p;
        fbhash *= 1099511628211ull;
    }
    close_out(f);

    {
        int ntri = 0, nspr = 0;
        for (i = 0; i < sc.n; i++) {
            uint32_t t = GPU_W0_TYPE(sc.recs[(size_t)i * RW]);
            ntri += t == GPU_REC_TRI;
            nspr += t == GPU_REC_SPRITE;
        }
        printf("%-9s nrec=%-5d nrender=%-5d tri=%-5d sprite=%-2d ddr_words=%-6d non_clear_px=%-7d fb_hash=%016llx\n",
               d->name, sc.n, nrender, ntri, nspr, ddr_words, nonclear, (unsigned long long)fbhash);
    }
    free(sc.recs);
    free(sc.ddr);
    free(sc.present);
    free(fb);
    return 0;
}

/* ---- verify: re-read a scene directory the way the RTL testbench does and re-render it ------ */
static uint64_t *g_vddr;   /* WIN_WORDS 64-bit words, parsed from ddr.hex */

static uint16_t vddr_rd(void *user, uint32_t addr)
{
    (void)user;
    if (addr < WIN_BASE || addr - WIN_BASE >= WIN_SIZE || (addr & 1u))
        return 0;
    return (uint16_t)(g_vddr[(addr - WIN_BASE) / 8u] >> (16u * ((addr >> 1) & 3u)));
}

static FILE *open_in(const char *dir, const char *file)
{
    char path[1100];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    f = fopen(path, "rb");
    if (!f) fprintf(stderr, "verify: cannot open %s\n", path);
    return f;
}

/* reads the next whitespace-separated token; returns 0 at EOF */
static int next_tok(FILE *f, char *buf, size_t n)
{
    int c;
    size_t k = 0;
    do { c = fgetc(f); } while (c != EOF && (c == ' ' || c == '\t' || c == '\r' || c == '\n'));
    while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        if (k + 1 < n) buf[k++] = (char)c;
        c = fgetc(f);
    }
    buf[k] = '\0';
    return k > 0;
}

static int is_hex_n(const char *t, size_t n)
{
    size_t i;
    if (strlen(t) != n) return 0;
    for (i = 0; i < n; i++)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
    return 1;
}

static int verify_dir(const char *dir)
{
    FILE *f;
    char tok[64];
    unsigned clear = 0;
    int nrec = -1, nrender = -1, nwords = 0, cap = 0, errors = 0, i;
    uint32_t *words = NULL;
    uint16_t *fb = NULL;
    long addr = 0, beats = 0, mism = 0;

    /* cfg.txt */
    if (!(f = open_in(dir, "cfg.txt"))) return 1;
    while (next_tok(f, tok, sizeof tok)) {
        if (sscanf(tok, "clear_color=%x", &clear) == 1) continue;
        if (sscanf(tok, "nrec=%d", &nrec) == 1) continue;
        if (sscanf(tok, "nrender=%d", &nrender) == 1) continue;
        fprintf(stderr, "verify %s: bad cfg token %s\n", dir, tok); errors++;
    }
    fclose(f);
    if (nrec < 0 || nrender != (nrec < GPU_LIST_SLOTS ? nrec : GPU_LIST_SLOTS) || clear > 0xFFFF) {
        fprintf(stderr, "verify %s: bad cfg values\n", dir); return 1;
    }
    /* rec.hex */
    if (!(f = open_in(dir, "rec.hex"))) return 1;
    while (next_tok(f, tok, sizeof tok)) {
        if (!is_hex_n(tok, 8)) { fprintf(stderr, "verify %s: bad rec.hex token %s\n", dir, tok); errors++; break; }
        if (nwords == cap) {
            cap = cap ? cap * 2 : 4096;
            words = (uint32_t *)realloc(words, (size_t)cap * sizeof(uint32_t));
            if (!words) die("out of memory");
        }
        words[nwords++] = (uint32_t)strtoul(tok, NULL, 16);
    }
    fclose(f);
    if (nwords != (nrec + 1) * RW) {
        fprintf(stderr, "verify %s: rec.hex has %d words, want %d\n", dir, nwords, (nrec + 1) * RW); return 1;
    }
    {
        uint32_t end[RW];
        gpu_make_end(1, end);
        if (memcmp(words + (size_t)nrec * RW, end, sizeof end) != 0) {
            fprintf(stderr, "verify %s: last record is not END(1)\n", dir); errors++;
        }
        for (i = 0; i < nrec; i++) {
            uint32_t t = GPU_W0_TYPE(words[(size_t)i * RW]);
            if (t != GPU_REC_TRI && t != GPU_REC_SPRITE) {
                fprintf(stderr, "verify %s: record %d has type %u\n", dir, i, (unsigned)t); errors++;
            }
        }
    }
    /* ddr.hex ($readmemh: '@' sets the word address, every data token fills the next word) */
    g_vddr = (uint64_t *)calloc(WIN_WORDS, sizeof(uint64_t));
    if (!g_vddr) die("out of memory");
    if (!(f = open_in(dir, "ddr.hex"))) return 1;
    while (next_tok(f, tok, sizeof tok)) {
        if (tok[0] == '@') {
            addr = strtol(tok + 1, NULL, 16);
            continue;
        }
        if (!is_hex_n(tok, 16) || addr < 0 || addr >= (long)WIN_WORDS) {
            fprintf(stderr, "verify %s: bad ddr.hex token %s at %lx\n", dir, tok, addr); errors++; break;
        }
        g_vddr[addr++] = (uint64_t)strtoull(tok, NULL, 16);
    }
    fclose(f);
    /* render from the parsed files, compare with expected.hex and expected.ppm */
    fb = (uint16_t *)malloc((size_t)GPU_W * GPU_H * sizeof(uint16_t));
    if (!fb) die("out of memory");
    gpu_refrast_frame(words, nrender, (uint16_t)clear, vddr_rd, NULL, fb);
    if (!(f = open_in(dir, "expected.hex"))) return 1;
    while (next_tok(f, tok, sizeof tok)) {
        uint64_t want;
        if (!is_hex_n(tok, 16) || beats >= GPU_W * GPU_H / 4) { errors++; break; }
        want = (uint64_t)fb[4 * beats] | (uint64_t)fb[4 * beats + 1] << 16 |
               (uint64_t)fb[4 * beats + 2] << 32 | (uint64_t)fb[4 * beats + 3] << 48;
        if ((uint64_t)strtoull(tok, NULL, 16) != want) mism++;
        beats++;
    }
    fclose(f);
    if (beats != GPU_W * GPU_H / 4 || mism) {
        fprintf(stderr, "verify %s: expected.hex %ld beats, %ld mismatches\n", dir, beats, mism); errors++;
    }
    if (!(f = open_in(dir, "expected.ppm"))) return 1;
    {
        int w = 0, h = 0, mx = 0;
        long bad = 0;
        if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w != GPU_W || h != GPU_H || mx != 255 || fgetc(f) != '\n') {
            fprintf(stderr, "verify %s: bad ppm header\n", dir); errors++;
        } else {
            for (i = 0; i < GPU_W * GPU_H; i++) {
                unsigned p = fb[i], r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
                int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                if (r != (int)((r5 << 3) | (r5 >> 2)) || g != (int)((g6 << 2) | (g6 >> 4)) ||
                    b != (int)((b5 << 3) | (b5 >> 2))) bad++;
            }
            if (bad || fgetc(f) != EOF) { fprintf(stderr, "verify %s: ppm differs (%ld px)\n", dir, bad); errors++; }
        }
    }
    fclose(f);
    printf("verify %-24s nrec=%-5d nrender=%-5d %s\n", dir, nrec, nrender, errors ? "FAIL" : "OK");
    free(words);
    free(fb);
    free(g_vddr);
    return errors ? 1 : 0;
}

static void usage(void)
{
    int i;
    fprintf(stderr, "usage: gen_scene <scene> <outdir> | gen_scene all <outroot> | gen_scene list\n"
                    "       gen_scene verify <scenedir> | gen_scene verify-all <outroot>\nscenes:");
    for (i = 0; i < NSCENES; i++) fprintf(stderr, " %s", SCENES[i].name);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    int i;
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        for (i = 0; i < NSCENES; i++) printf("%s\n", SCENES[i].name);
        return 0;
    }
    if (argc != 3) { usage(); return 1; }
    if (strcmp(argv[1], "verify") == 0)
        return verify_dir(argv[2]);
    if (strcmp(argv[1], "verify-all") == 0) {
        char dir[1024];
        int bad = 0;
        for (i = 0; i < NSCENES; i++) {
            if (snprintf(dir, sizeof dir, "%s/%s", argv[2], SCENES[i].name) >= (int)sizeof dir) die("path too long");
            bad |= verify_dir(dir);
        }
        printf("verify-all: %s\n", bad ? "FAIL" : "all scenes OK");
        return bad;
    }
    if (strcmp(argv[1], "all") == 0) {
        char dir[1024];
        for (i = 0; i < NSCENES; i++) {
            if (snprintf(dir, sizeof dir, "%s/%s", argv[2], SCENES[i].name) >= (int)sizeof dir) die("path too long");
            write_scene(&SCENES[i], dir);
        }
        return 0;
    }
    for (i = 0; i < NSCENES; i++)
        if (strcmp(argv[1], SCENES[i].name) == 0)
            return write_scene(&SCENES[i], argv[2]);
    usage();
    return 1;
}

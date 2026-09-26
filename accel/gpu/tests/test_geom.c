/*
 * test_geom.c -- tests for common/geom.c (SPEC.md section 6) on x86-64 (also runs under qemu).
 *
 *   test_geom [outdir]      writes the rendered frames as PPM files into outdir (default ".")
 *                           exit status 0 = all checks passed
 *
 * Every frame goes geom_frame -> records -> gpu_refrast_frame (the golden PL model) -> RGB565 fb.
 * Checks:
 *  - meshes (cube, UV sphere, torus) really are CCW-front with outward normals;
 *  - every emitted record is well formed (TRI bbox inside the screen, END last with frame_no),
 *    and the geom_stats are consistent with the records (tris_out == TRI records, in/out/culled);
 *  - bit-exact agreement of geom_frame with an independent straightforward per-triangle
 *    transcription of SPEC 6 (no vertex cache, no outcodes, every clip plane run) on every frame;
 *  - back-face culling: a cube seen from each of its 8 corners shows exactly the 3 expected face
 *    colours; from inside, CULL_BACK removes everything;
 *  - near-plane / guard-band clipping: camera inside a cube / a sphere covers every pixel, a camera
 *    flying through a torus (240 frames, some with a 0.001 near plane) never produces a pixel of a
 *    colour that is not in the scene, a ground plane that extends behind the camera fills exactly
 *    the rows below the horizon, a screen-covering quad (and a 12x12 grid) fills every pixel;
 *  - partly off-screen objects: coverage equals a screen-space reference (vertices projected the
 *    same way, clipped by gpu_setup_tri's guard-band clipper) except for a handful of edge pixels;
 *  - lighting sanity, mesh store (limits, replacement, compaction, bad data, streaming API),
 *    GPU_ERR_ARG for unknown meshes (nothing emitted), emit abort propagation, determinism.
 * Prints a hash of all records so that builds for other ISAs can be compared with x86-64.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gpu_proto.h"
#include "gpu_setup.h"
#include "gpu_refrast.h"
#include "geom.h"
#include "tg_geom_ext.h"

static int g_fail, g_checks;
static const char *g_outdir = ".";

#define CHECK(cond, ...)                                                            \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_fail++;                                                               \
            if (g_fail < 60) {                                                      \
                printf("  FAIL %s:%d: ", __FILE__, __LINE__);                       \
                printf(__VA_ARGS__);                                                \
                printf("\n");                                                       \
            }                                                                       \
        }                                                                           \
    } while (0)

/* ---------------------------------------------------------------------------------------- */
/* small vector / matrix helpers (inputs only: any rounding is fine here)                    */
/* ---------------------------------------------------------------------------------------- */
typedef struct { double x, y, z; } v3;
static v3 V3(double x, double y, double z) { v3 r; r.x = x; r.y = y; r.z = z; return r; }
static v3 vsub(v3 a, v3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 vadd(v3 a, v3 b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 vscale(v3 a, double s) { return V3(a.x * s, a.y * s, a.z * s); }
static double vdot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 vcross(v3 a, v3 b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static v3 vnorm(v3 a) { double l = sqrt(vdot(a, a)); return l > 0 ? vscale(a, 1.0 / l) : a; }

static void m_ident(double m[16]) { int i; for (i = 0; i < 16; i++) m[i] = (i % 5) == 0; }
static void m_mul(const double a[16], const double b[16], double o[16])
{
    double t[16];
    int c, r, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            double s = 0;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(o, t, sizeof t);
}
static void m_persp(double fovy_deg, double aspect, double n, double f, double m[16])
{
    double t = 1.0 / tan(fovy_deg * 3.14159265358979323846 / 360.0);
    memset(m, 0, 16 * sizeof(double));
    m[0] = t / aspect;
    m[5] = t;
    m[10] = (f + n) / (n - f);
    m[11] = -1.0;
    m[14] = 2.0 * f * n / (n - f);
}
static void m_lookat(v3 eye, v3 at, v3 up, double m[16])
{
    v3 F = vnorm(vsub(at, eye)), s = vnorm(vcross(F, up)), u = vcross(s, F);
    m_ident(m);
    m[0] = s.x; m[4] = s.y; m[8] = s.z;
    m[1] = u.x; m[5] = u.y; m[9] = u.z;
    m[2] = -F.x; m[6] = -F.y; m[10] = -F.z;
    m[12] = -vdot(s, eye); m[13] = -vdot(u, eye); m[14] = vdot(F, eye);
}
static void m_trs(v3 t, v3 axis, double ang, double sc, double m[16])
{
    v3 a = vnorm(axis);
    double c = cos(ang), s = sin(ang), C = 1 - c;
    m_ident(m);
    m[0] = (a.x * a.x * C + c) * sc;       m[4] = (a.x * a.y * C - a.z * s) * sc; m[8]  = (a.x * a.z * C + a.y * s) * sc;
    m[1] = (a.y * a.x * C + a.z * s) * sc; m[5] = (a.y * a.y * C + c) * sc;       m[9]  = (a.y * a.z * C - a.x * s) * sc;
    m[2] = (a.z * a.x * C - a.y * s) * sc; m[6] = (a.z * a.y * C + a.x * s) * sc; m[10] = (a.z * a.z * C + c) * sc;
    m[12] = t.x; m[13] = t.y; m[14] = t.z;
}
static void m_tof(const double m[16], float o[16]) { int i; for (i = 0; i < 16; i++) o[i] = (float)m[i]; }

/* ---------------------------------------------------------------------------------------- */
/* meshes                                                                                    */
/* ---------------------------------------------------------------------------------------- */
typedef struct {
    int nv, ni;
    geom_vertex *v;
    uint16_t *ix;
} mesh;

static void mesh_alloc(mesh *m, int nv, int ni)
{
    m->nv = 0;
    m->ni = 0;
    m->v = (geom_vertex *)calloc((size_t)(nv ? nv : 1), sizeof(geom_vertex));
    m->ix = (uint16_t *)calloc((size_t)(ni ? ni : 1), sizeof(uint16_t));
    if (!m->v || !m->ix) { printf("out of memory\n"); exit(2); }
}
static void mesh_free(mesh *m) { free(m->v); free(m->ix); m->v = NULL; m->ix = NULL; }
static int addv(mesh *m, v3 p, v3 n, int r, int g, int b)
{
    geom_vertex *v = &m->v[m->nv];
    v->px = (float)p.x; v->py = (float)p.y; v->pz = (float)p.z;
    v->nx = (float)n.x; v->ny = (float)n.y; v->nz = (float)n.z;
    v->r = (uint8_t)r; v->g = (uint8_t)g; v->b = (uint8_t)b; v->a = 255;
    return m->nv++;
}
static void addt(mesh *m, int a, int b, int c)
{
    m->ix[m->ni++] = (uint16_t)a; m->ix[m->ni++] = (uint16_t)b; m->ix[m->ni++] = (uint16_t)c;
}

/* face colours: +X red, -X cyan, +Y green, -Y magenta, +Z blue, -Z yellow */
static const uint8_t face_rgb[6][3] = {
    {255, 0, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {0, 0, 255}, {255, 255, 0}
};
static const int face_n[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
/* tangents with u x v = n, so corners (-u-v),(+u-v),(+u+v),(-u+v) are CCW seen from outside */
static const int face_u[6][3] = { {0,1,0}, {0,0,1}, {0,0,1}, {1,0,0}, {1,0,0}, {0,1,0} };
static const int face_v[6][3] = { {0,0,1}, {0,1,0}, {1,0,0}, {0,0,1}, {0,1,0}, {1,0,0} };

static void build_cube(mesh *m, double h, int white)
{
    int f, k;
    mesh_alloc(m, 24, 36);
    for (f = 0; f < 6; f++) {
        v3 n = V3(face_n[f][0], face_n[f][1], face_n[f][2]);
        v3 u = V3(face_u[f][0], face_u[f][1], face_u[f][2]);
        v3 v = V3(face_v[f][0], face_v[f][1], face_v[f][2]);
        static const int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        int base = m->nv;
        for (k = 0; k < 4; k++) {
            v3 p = vscale(vadd(n, vadd(vscale(u, su[k]), vscale(v, sv[k]))), h);
            if (white)
                addv(m, p, n, 255, 255, 255);
            else
                addv(m, p, n, face_rgb[f][0], face_rgb[f][1], face_rgb[f][2]);
        }
        addt(m, base, base + 1, base + 2);
        addt(m, base, base + 2, base + 3);
    }
}

/* colour modes: 0 uniform (r,g,b), 1 smooth rainbow-ish gradient */
static void grad_col(int mode, double s, double t, int rgb[3], const int base[3])
{
    if (mode == 0) { rgb[0] = base[0]; rgb[1] = base[1]; rgb[2] = base[2]; return; }
    rgb[0] = (int)(127.5 + 127.5 * cos(6.2831853 * s));
    rgb[1] = (int)(127.5 + 127.5 * cos(6.2831853 * (s + t) + 2.1));
    rgb[2] = (int)(127.5 + 127.5 * cos(6.2831853 * t + 4.2));
}

/* UV sphere: p(th,ph) = (sin th cos ph, cos th, sin th sin ph) * r. With a=(i,j), b=(i,j+1),
 * c=(i+1,j+1), d=(i+1,j) (i along th, j along ph) the triangles (a,b,d) and (b,c,d) are CCW seen
 * from outside (dp/dph x dp/dth points outwards). Pole triangles that would be degenerate are
 * skipped. */
static void build_sphere(mesh *m, double r, int stacks, int slices, int mode, const int base[3])
{
    int i, j;
    mesh_alloc(m, (stacks + 1) * (slices + 1), stacks * slices * 6);
    for (i = 0; i <= stacks; i++)
        for (j = 0; j <= slices; j++) {
            double th = 3.14159265358979323846 * i / stacks, ph = 6.28318530717958647692 * j / slices;
            v3 n = V3(sin(th) * cos(ph), cos(th), sin(th) * sin(ph));
            int c[3];
            grad_col(mode, (double)j / slices, (double)i / stacks, c, base);
            addv(m, vscale(n, r), n, c[0], c[1], c[2]);
        }
    for (i = 0; i < stacks; i++)
        for (j = 0; j < slices; j++) {
            int a = i * (slices + 1) + j, b = a + 1, d = a + slices + 1, cc = d + 1;
            if (i != 0)
                addt(m, a, b, d);
            if (i != stacks - 1)
                addt(m, b, cc, d);
        }
}

/* torus around the Y axis: p(u,v) = ((R + r cos v) cos u, r sin v, (R + r cos v) sin u). */
static void build_torus(mesh *m, double R, double rr, int nu, int nv, int mode, const int base[3])
{
    int i, j;
    mesh_alloc(m, (nu + 1) * (nv + 1), nu * nv * 6);
    for (i = 0; i <= nu; i++)
        for (j = 0; j <= nv; j++) {
            double u = 6.28318530717958647692 * i / nu, v = 6.28318530717958647692 * j / nv;
            v3 n = V3(cos(v) * cos(u), sin(v), cos(v) * sin(u));
            v3 p = V3((R + rr * cos(v)) * cos(u), rr * sin(v), (R + rr * cos(v)) * sin(u));
            int c[3];
            grad_col(mode, (double)i / nu, (double)j / nv, c, base);
            addv(m, p, n, c[0], c[1], c[2]);
        }
    for (i = 0; i < nu; i++)
        for (j = 0; j < nv; j++) {
            int a = i * (nv + 1) + j, b = a + 1, d = a + nv + 1, cc = d + 1;
            addt(m, a, b, d);
            addt(m, b, cc, d);
        }
}

/* y = y0 plane, tiles over [x0,x1] x [z0,z1], facing +Y (CCW seen from above) */
static void build_ground(mesh *m, double x0, double x1, double z0, double z1, double y0, int nx, int nz,
                         int r, int g, int b)
{
    int i, j;
    mesh_alloc(m, (nx + 1) * (nz + 1), nx * nz * 6);
    for (j = 0; j <= nz; j++)
        for (i = 0; i <= nx; i++)
            addv(m, V3(x0 + (x1 - x0) * i / nx, y0, z0 + (z1 - z0) * j / nz), V3(0, 1, 0), r, g, b);
    for (j = 0; j < nz; j++)
        for (i = 0; i < nx; i++) {
            int a = j * (nx + 1) + i, bb = a + 1, c = a + nx + 1, d = c + 1;
            /* seen from +Y (looking down -Y, x right, z towards the viewer's bottom) */
            addt(m, a, c, bb);
            addt(m, bb, c, d);
        }
}

/* z = z0 plane facing +Z, n x n tiles over [-e,e]^2 */
static void build_wall(mesh *m, double e, double z0, int n, int r, int g, int b)
{
    int i, j;
    mesh_alloc(m, (n + 1) * (n + 1), n * n * 6);
    for (j = 0; j <= n; j++)
        for (i = 0; i <= n; i++)
            addv(m, V3(-e + 2 * e * i / n, -e + 2 * e * j / n, z0), V3(0, 0, 1), r, g, b);
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            int a = j * (n + 1) + i, bb = a + 1, c = a + n + 1, d = c + 1;
            addt(m, a, bb, d);
            addt(m, a, d, c);
        }
}

/* every triangle: (p1-p0)x(p2-p0) . outward > 0, where outward = mean vertex normal */
static int check_orientation(const mesh *m, const char *name)
{
    int t, bad = 0, degen = 0;
    for (t = 0; t < m->ni / 3; t++) {
        const geom_vertex *a = &m->v[m->ix[3 * t]], *b = &m->v[m->ix[3 * t + 1]], *c = &m->v[m->ix[3 * t + 2]];
        v3 pa = V3(a->px, a->py, a->pz), pb = V3(b->px, b->py, b->pz), pc = V3(c->px, c->py, c->pz);
        v3 n = vadd(vadd(V3(a->nx, a->ny, a->nz), V3(b->nx, b->ny, b->nz)), V3(c->nx, c->ny, c->nz));
        v3 cr = vcross(vsub(pb, pa), vsub(pc, pa));
        double l = sqrt(vdot(cr, cr));
        if (l < 1e-12) { degen++; continue; }
        if (vdot(cr, n) <= 0)
            bad++;
    }
    CHECK(bad == 0 && degen == 0, "%s: %d triangles not CCW-front/outward, %d degenerate", name, bad, degen);
    return bad == 0;
}

/* ---------------------------------------------------------------------------------------- */
/* record collection, validation, rendering, images                                          */
/* ---------------------------------------------------------------------------------------- */
typedef struct {
    uint32_t *r;
    int n, cap;
    int calls, abort_at;
} reclist;

static int collect(void *u, const uint32_t rec[GPU_REC_WORDS])
{
    reclist *rl = (reclist *)u;
    rl->calls++;
    if (rl->abort_at && rl->calls == rl->abort_at)
        return -7;
    if (rl->n == rl->cap) {
        rl->cap = rl->cap ? rl->cap * 2 : 1024;
        rl->r = (uint32_t *)realloc(rl->r, (size_t)rl->cap * GPU_REC_BYTES);
        if (!rl->r) { printf("out of memory\n"); exit(2); }
    }
    memcpy(rl->r + (size_t)rl->n * GPU_REC_WORDS, rec, GPU_REC_BYTES);
    rl->n++;
    return 0;
}
static void rl_clear(reclist *rl) { rl->n = 0; rl->calls = 0; rl->abort_at = 0; }

static uint64_t g_hash = 1469598103934665603ull;
static void hash_recs(const reclist *rl)
{
    size_t i, n = (size_t)rl->n * GPU_REC_WORDS;
    for (i = 0; i < n; i++) {
        uint32_t w = rl->r[i];
        int b;
        for (b = 0; b < 4; b++) {
            g_hash ^= (w >> (8 * b)) & 0xFFu;
            g_hash *= 1099511628211ull;
        }
    }
}

/* structure + stats consistency; returns number of TRI records */
static int validate(const reclist *rl, const geom_stats *st, uint32_t frame_no, const char *name)
{
    int i, ntri = 0, bad = 0, badend = 0;
    for (i = 0; i < rl->n; i++) {
        const uint32_t *r = rl->r + (size_t)i * GPU_REC_WORDS;
        uint32_t type = GPU_W0_TYPE(r[0]);
        if (i == rl->n - 1) {
            int k;
            if (type != GPU_REC_END || r[1] != frame_no || r[0] != (GPU_REC_END << 28))
                badend++;
            for (k = 2; k < GPU_REC_WORDS; k++)
                if (r[k]) badend++;
        } else if (type == GPU_REC_TRI) {
            uint32_t xmin = r[0] & 0x7FF, xmax = (r[0] >> 11) & 0x7FF, ymin = r[1] & 0x7FF, ymax = (r[1] >> 11) & 0x7FF;
            ntri++;
            if (xmin > xmax || xmax > 1279 || ymin > ymax || ymax > 719 || (r[0] & 0x0CC00000u) ||
                (r[1] & 0xFFC00000u) || r[23] != 0)
                bad++;
        } else {
            bad++;
        }
    }
    CHECK(rl->n >= 1 && badend == 0, "%s: END record missing/wrong (n=%d)", name, rl->n);
    CHECK(bad == 0, "%s: %d malformed records", name, bad);
    CHECK((uint32_t)ntri == st->tris_out, "%s: %d TRI records but tris_out=%u", name, ntri, st->tris_out);
    CHECK(st->tris_out + st->tris_culled >= st->tris_in, "%s: out %u + culled %u < in %u", name,
          st->tris_out, st->tris_culled, st->tris_in);
    CHECK(st->tris_out + st->tris_culled <= st->tris_in + 14u * st->tris_clipped,
          "%s: out+culled too large (in %u out %u culled %u clipped %u)", name, st->tris_in, st->tris_out,
          st->tris_culled, st->tris_clipped);
    if (st->tris_clipped == 0)
        CHECK(st->tris_out + st->tris_culled == st->tris_in, "%s: out+culled != in without clipping", name);
    return ntri;
}

static uint16_t *g_fb, *g_fb2;

static void render(const reclist *rl, uint16_t clear, uint16_t *fb)
{
    gpu_refrast_frame(rl->r, rl->n, clear, NULL, NULL, fb);
}

static void write_ppm(const uint16_t *fb, const char *name)
{
    char path[1024];
    FILE *f;
    int i;
    unsigned char *row = (unsigned char *)malloc(GPU_W * 3);
    snprintf(path, sizeof path, "%s/%s.ppm", g_outdir, name);
    f = fopen(path, "wb");
    if (!f || !row) { printf("  (cannot write %s)\n", path); free(row); if (f) fclose(f); return; }
    fprintf(f, "P6\n%d %d\n255\n", GPU_W, GPU_H);
    for (i = 0; i < GPU_H; i++) {
        int x;
        for (x = 0; x < GPU_W; x++) {
            uint16_t p = fb[i * GPU_W + x];
            unsigned r5 = p >> 11, g6 = (p >> 5) & 63, b5 = p & 31;
            row[3 * x] = (unsigned char)((r5 << 3) | (r5 >> 2));
            row[3 * x + 1] = (unsigned char)((g6 << 2) | (g6 >> 4));
            row[3 * x + 2] = (unsigned char)((b5 << 3) | (b5 >> 2));
        }
        fwrite(row, 1, GPU_W * 3, f);
    }
    fclose(f);
    free(row);
}

static int count_px(const uint16_t *fb, uint16_t c)
{
    int i, n = 0;
    for (i = 0; i < GPU_W * GPU_H; i++)
        n += fb[i] == c;
    return n;
}

/* pixels whose colour is not in the allowed set */
static int count_foreign(const uint16_t *fb, const uint16_t *allowed, int na)
{
    int i, k, n = 0;
    for (i = 0; i < GPU_W * GPU_H; i++) {
        for (k = 0; k < na; k++)
            if (fb[i] == allowed[k]) break;
        if (k == na) n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------------------- */
/* independent reference transcription of SPEC 6 (per triangle, no cache, all planes run)    */
/* ---------------------------------------------------------------------------------------- */
#define MAXMESH 64
static struct { int used; uint16_t id; mesh m; } g_ref[MAXMESH];

static void ref_store(uint16_t id, const mesh *m)
{
    int i, fr = -1;
    for (i = 0; i < MAXMESH; i++) {
        if (g_ref[i].used && g_ref[i].id == id) { fr = i; break; }
        if (!g_ref[i].used && fr < 0) fr = i;
    }
    g_ref[fr].used = 1;
    g_ref[fr].id = id;
    g_ref[fr].m = *m;          /* shallow: test meshes live until the end */
}
static const mesh *ref_find(uint16_t id)
{
    int i;
    for (i = 0; i < MAXMESH; i++)
        if (g_ref[i].used && g_ref[i].id == id) return &g_ref[i].m;
    return NULL;
}

typedef struct { float a[7]; } rvtx;   /* x y z w r g b */

static float rdist(const rvtx *v, int k)
{
    float x = v->a[0], y = v->a[1], z = v->a[2], w = v->a[3];
    if (k == 0) return z + w;
    if (k == 1) return w - z;
    if (k == 2) return x + 1.35f * w;
    if (k == 3) return 1.35f * w - x;
    if (k == 4) return y + 1.65f * w;
    return 1.65f * w - y;
}

static float rclamp(float c) { return !(c > 0.0f) ? 0.0f : (c > 255.0f ? 255.0f : c); }

static int ref_frame(const geom_frame_hdr *h, const geom_draw *dr, reclist *out, geom_stats *st)
{
    uint32_t di;
    uint32_t rec[GPU_REC_WORDS];
    memset(st, 0, sizeof *st);
    for (di = 0; di < h->ndraws; di++)
        if (!ref_find(dr[di].mesh_id))
            return GPU_ERR_ARG;
    for (di = 0; di < h->ndraws; di++) {
        const geom_draw *d = &dr[di];
        const mesh *m = ref_find(d->mesh_id);
        float P[16];
        int c, r, t;
        uint32_t fl = ((d->flags & GEOM_DRAW_ZTEST) ? GPU_F_ZTEST : 0) | ((d->flags & GEOM_DRAW_ZWRITE) ? GPU_F_ZWRITE : 0);
        int cull = (d->flags & GEOM_DRAW_CULL_BACK) ? GPU_CULL_CW : GPU_CULL_NONE;
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++)
                P[c * 4 + r] = ((h->viewproj[r] * d->model[c * 4] + h->viewproj[4 + r] * d->model[c * 4 + 1])
                                + h->viewproj[8 + r] * d->model[c * 4 + 2]) + h->viewproj[12 + r] * d->model[c * 4 + 3];
        for (t = 0; t < m->ni / 3; t++) {
            rvtx poly[16], q[16];
            float dist[3][6];
            int n = 3, j, k, reject = 0, allin = 1;
            gpu_vtx pv[16];
            st->tris_in++;
            for (j = 0; j < 3; j++) {
                const geom_vertex *v = &m->v[m->ix[3 * t + j]];
                float kk = 1.0f;
                for (r = 0; r < 4; r++)
                    poly[j].a[r] = ((P[r] * v->px + P[4 + r] * v->py) + P[8 + r] * v->pz) + P[12 + r];
                if (d->flags & GEOM_DRAW_LIGHTING) {
                    float nn[3], len, dd;
                    for (r = 0; r < 3; r++)
                        nn[r] = (d->model[r] * v->nx + d->model[4 + r] * v->ny) + d->model[8 + r] * v->nz;
                    len = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                    if (len > 0) { nn[0] /= len; nn[1] /= len; nn[2] /= len; }
                    dd = -(nn[0] * h->light_dir[0] + nn[1] * h->light_dir[1] + nn[2] * h->light_dir[2]);
                    if (dd < 0) dd = 0;
                    kk = h->ambient + (1.0f - h->ambient) * dd;
                }
                poly[j].a[4] = rclamp((float)v->r * ((float)d->color_mul[0] / 255.0f) * kk);
                poly[j].a[5] = rclamp((float)v->g * ((float)d->color_mul[1] / 255.0f) * kk);
                poly[j].a[6] = rclamp((float)v->b * ((float)d->color_mul[2] / 255.0f) * kk);
                for (k = 0; k < 6; k++) {
                    dist[j][k] = rdist(&poly[j], k);
                    if (!(dist[j][k] >= 0)) allin = 0;
                }
            }
            for (k = 0; k < 6; k++)
                if (dist[0][k] < 0 && dist[1][k] < 0 && dist[2][k] < 0) reject = 1;
            if (reject) { st->tris_culled++; continue; }
            if (!allin) {
                int bad = 0;
                st->tris_clipped++;
                for (k = 0; k < 6 && n >= 3; k++) {
                    int m2 = 0, i;
                    for (i = 0; i < n; i++) {
                        const rvtx *A = &poly[i], *B = &poly[(i + 1) % n];
                        float da = rdist(A, k), db = rdist(B, k);
                        int ia = da >= 0, ib = db >= 0;
                        if (ia && m2 < 16) q[m2++] = *A;
                        if (ia != ib && m2 < 16) {
                            const rvtx *I = ia ? A : B, *O = ia ? B : A;
                            float dI = ia ? da : db, dO = ia ? db : da, tt = dI / (dI - dO);
                            int a;
                            for (a = 0; a < 7; a++) q[m2].a[a] = I->a[a] + tt * (O->a[a] - I->a[a]);
                            m2++;
                        }
                    }
                    memcpy(poly, q, sizeof(rvtx) * (size_t)m2);
                    n = m2;
                }
                if (n < 3) { st->tris_culled++; continue; }
                for (j = 0; j < n; j++) if (poly[j].a[3] <= 1e-6f) bad = 1;
                if (bad) { st->tris_culled++; continue; }
            }
            for (j = 0; j < n; j++) {
                float iw = 1.0f / poly[j].a[3];
                pv[j].x = (poly[j].a[0] * iw * 0.5f + 0.5f) * 1280.0f;
                pv[j].y = (0.5f - poly[j].a[1] * iw * 0.5f) * 720.0f;
                pv[j].z = poly[j].a[2] * iw * 0.5f + 0.5f;
                pv[j].r = (uint8_t)(rclamp(poly[j].a[4]) + 0.5f);
                pv[j].g = (uint8_t)(rclamp(poly[j].a[5]) + 0.5f);
                pv[j].b = (uint8_t)(rclamp(poly[j].a[6]) + 0.5f);
                pv[j].a = 0;
            }
            for (j = 1; j + 1 < n; j++) {
                gpu_vtx tv[3];
                tv[0] = pv[0]; tv[1] = pv[j]; tv[2] = pv[j + 1];
                if (gpu_setup_tri_noclip(tv, fl, cull, rec) == 1) { collect(out, rec); st->tris_out++; }
                else st->tris_culled++;
            }
        }
    }
    gpu_make_end(h->frame_no, rec);
    collect(out, rec);
    return 0;
}

/* screen-space reference for objects entirely in front of the camera: same projection, but the
 * guard-band clipping is done by gpu_setup_tri in screen space */
static int screen_ref(const geom_frame_hdr *h, const geom_draw *dr, reclist *out)
{
    uint32_t di;
    uint32_t recs[GPU_SETUP_MAX_OUT][GPU_REC_WORDS];
    for (di = 0; di < h->ndraws; di++) {
        const geom_draw *d = &dr[di];
        const mesh *m = ref_find(d->mesh_id);
        float P[16];
        int c, r, t, j;
        uint32_t fl = ((d->flags & GEOM_DRAW_ZTEST) ? GPU_F_ZTEST : 0) | ((d->flags & GEOM_DRAW_ZWRITE) ? GPU_F_ZWRITE : 0);
        int cull = (d->flags & GEOM_DRAW_CULL_BACK) ? GPU_CULL_CW : GPU_CULL_NONE;
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++)
                P[c * 4 + r] = ((h->viewproj[r] * d->model[c * 4] + h->viewproj[4 + r] * d->model[c * 4 + 1])
                                + h->viewproj[8 + r] * d->model[c * 4 + 2]) + h->viewproj[12 + r] * d->model[c * 4 + 3];
        for (t = 0; t < m->ni / 3; t++) {
            gpu_vtx tv[3];
            int nout;
            for (j = 0; j < 3; j++) {
                const geom_vertex *v = &m->v[m->ix[3 * t + j]];
                float cl[4], iw;
                for (r = 0; r < 4; r++)
                    cl[r] = ((P[r] * v->px + P[4 + r] * v->py) + P[8 + r] * v->pz) + P[12 + r];
                if (!(cl[3] > 0.01f)) return -1;
                iw = 1.0f / cl[3];
                tv[j].x = (cl[0] * iw * 0.5f + 0.5f) * 1280.0f;
                tv[j].y = (0.5f - cl[1] * iw * 0.5f) * 720.0f;
                tv[j].z = cl[2] * iw * 0.5f + 0.5f;
                tv[j].r = v->r; tv[j].g = v->g; tv[j].b = v->b; tv[j].a = 0;
            }
            nout = gpu_setup_tri(tv, fl, cull, recs, GPU_SETUP_MAX_OUT);
            for (j = 0; j < nout; j++) collect(out, recs[j]);
        }
    }
    {
        uint32_t e[GPU_REC_WORDS];
        gpu_make_end(h->frame_no, e);
        collect(out, e);
    }
    return 0;
}

/* deterministic PRNG (every call in its own statement: C leaves argument evaluation order open) */
static uint32_t g_seed = 1u;
static uint32_t rnd_u32(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static double rnd_d(double lo, double hi) { uint32_t r = rnd_u32() & 0xFFFFFFu; return lo + (hi - lo) * (double)r / 16777216.0; }
static v3 rnd_v3(double lo, double hi)
{
    double x, y, z;
    x = rnd_d(lo, hi);
    y = rnd_d(lo, hi);
    z = rnd_d(lo, hi);
    return V3(x, y, z);
}

/* ---------------------------------------------------------------------------------------- */
/* scene helpers                                                                             */
/* ---------------------------------------------------------------------------------------- */
static reclist g_rl, g_rl2;
static double g_geom_sec;
static uint32_t g_tris_total;

static void set_draw(geom_draw *d, uint16_t id, uint16_t flags, const double model[16], int r, int g, int b)
{
    memset(d, 0, sizeof *d);
    d->mesh_id = id;
    d->flags = flags;
    {
        float mf[16];
        m_tof(model, mf);
        memcpy(d->model, mf, sizeof mf);
    }
    d->color_mul[0] = (uint8_t)r; d->color_mul[1] = (uint8_t)g; d->color_mul[2] = (uint8_t)b; d->color_mul[3] = 255;
}
static void set_hdr(geom_frame_hdr *h, uint32_t frame_no, const double vp[16], v3 light, double ambient, int ndraws)
{
    v3 l = vnorm(light);
    memset(h, 0, sizeof *h);
    h->frame_no = frame_no;
    {
        float vf[16];
        m_tof(vp, vf);
        memcpy(h->viewproj, vf, sizeof vf);
    }
    h->light_dir[0] = (float)l.x; h->light_dir[1] = (float)l.y; h->light_dir[2] = (float)l.z;
    h->ambient = (float)ambient;
    h->ndraws = (uint16_t)ndraws;
}
static void make_vp(v3 eye, v3 at, double fovy, double n, double f, double vp[16])
{
    double P[16], V[16];
    m_persp(fovy, 1280.0 / 720.0, n, f, P);
    m_lookat(eye, at, V3(0, 1, 0), V);
    m_mul(P, V, vp);
}

/* run geom_frame (twice: once counting only), the reference, validate, hash, render */
static int run_scene(const geom_frame_hdr *h, const geom_draw *d, geom_stats *st, const char *name,
                     uint16_t clear, uint16_t *fb)
{
    geom_stats st0, rst;
    clock_t c0;
    int r, ntri;

    rl_clear(&g_rl);
    c0 = clock();
    r = geom_frame(h, d, collect, &g_rl, st);
    g_geom_sec += (double)(clock() - c0) / CLOCKS_PER_SEC;
    g_tris_total += st->tris_in;
    CHECK(r == 0, "%s: geom_frame returned %d", name, r);
    ntri = validate(&g_rl, st, h->frame_no, name);
    hash_recs(&g_rl);

    r = geom_frame(h, d, NULL, NULL, &st0);
    CHECK(r == 0 && memcmp(&st0, st, sizeof st0) == 0, "%s: stats differ without emit callback", name);

    rl_clear(&g_rl2);
    ref_frame(h, d, &g_rl2, &rst);
    CHECK(g_rl2.n == g_rl.n && memcmp(g_rl2.r, g_rl.r, (size_t)g_rl.n * GPU_REC_BYTES) == 0,
          "%s: records differ from the SPEC 6 reference transcription (%d vs %d records)", name, g_rl.n, g_rl2.n);
    CHECK(memcmp(&rst, st, sizeof rst) == 0, "%s: stats differ from reference (in %u/%u out %u/%u culled %u/%u clipped %u/%u)",
          name, st->tris_in, rst.tris_in, st->tris_out, rst.tris_out, st->tris_culled, rst.tris_culled,
          st->tris_clipped, rst.tris_clipped);
    if (fb)
        render(&g_rl, clear, fb);
    return ntri;
}

static void upload(uint16_t id, const mesh *m)
{
    int r = geom_mesh_upload(id, (uint32_t)m->nv, (uint32_t)m->ni, m->v, m->ix);
    CHECK(r == 0, "upload of mesh %u failed: %d", id, r);
    ref_store(id, m);
}

#define RGB565(r, g, b) gpu_rgb565(r, g, b)
#define CLEAR 0x0010u   /* dark blue, like the daemon */

/* ---------------------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    mesh cube, cubew, sphere, sphere_u, torus, torus_u, ground, wall1, wall12, sph_small;
    geom_frame_hdr h;
    geom_draw d[8];
    geom_stats st;
    double vp[16], M[16];
    int i, k;
    static const int red[3] = {255, 60, 40}, gold[3] = {255, 200, 40}, teal[3] = {40, 220, 200};
    uint16_t face565[6];

    if (argc > 1)
        g_outdir = argv[1];
    g_fb = (uint16_t *)malloc(GPU_W * GPU_H * 2);
    g_fb2 = (uint16_t *)malloc(GPU_W * GPU_H * 2);
    if (!g_fb || !g_fb2) return 2;
    for (k = 0; k < 6; k++)
        face565[k] = RGB565(face_rgb[k][0], face_rgb[k][1], face_rgb[k][2]);

    /* ---- meshes ---- */
    printf("[meshes]\n");
    build_cube(&cube, 1.0, 0);
    build_cube(&cubew, 1.0, 1);
    build_sphere(&sphere, 1.0, 24, 40, 1, red);
    build_sphere(&sphere_u, 1.0, 20, 32, 0, red);
    build_sphere(&sph_small, 1.0, 10, 16, 0, teal);
    build_torus(&torus, 1.0, 0.38, 48, 24, 1, gold);
    build_torus(&torus_u, 1.0, 0.38, 40, 20, 0, gold);
    build_ground(&ground, -400, 400, -100, 100, -1.0, 16, 8, 90, 200, 60);
    build_wall(&wall1, 100.0, 0.0, 1, 250, 250, 250);
    build_wall(&wall12, 100.0, 0.0, 12, 200, 120, 250);
    check_orientation(&cube, "cube");
    check_orientation(&sphere, "sphere");
    check_orientation(&torus, "torus");
    check_orientation(&ground, "ground");
    check_orientation(&wall12, "wall");
    printf("  cube %d/%d  sphere %d/%d  torus %d/%d verts/indices\n", cube.nv, cube.ni, sphere.nv, sphere.ni,
           torus.nv, torus.ni);

    geom_reset();
    upload(1, &cube);
    upload(2, &cubew);
    upload(3, &sphere);
    upload(4, &torus);
    upload(5, &sphere_u);
    upload(6, &torus_u);
    upload(7, &ground);
    upload(8, &wall1);
    upload(9, &wall12);
    upload(10, &sph_small);

    /* ---- 1. back-face culling: cube from all 8 corners ---- */
    printf("[cull] cube from its 8 corners\n");
    for (i = 0; i < 8; i++) {
        v3 eye = V3((i & 1) ? 3.0 : -3.0, (i & 2) ? 3.0 : -3.0, (i & 4) ? 3.0 : -3.0);
        int f, nvis = 0;
        char name[64];
        uint16_t allowed[4];
        make_vp(eye, V3(0, 0, 0), 60, 0.1, 100, vp);
        m_ident(M);
        set_hdr(&h, 100 + (uint32_t)i, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[0], 1, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, M, 255, 255, 255);
        snprintf(name, sizeof name, "cube_corner%d", i);
        run_scene(&h, d, &st, name, CLEAR, g_fb);
        CHECK(st.tris_in == 12 && st.tris_out == 6 && st.tris_culled == 6 && st.tris_clipped == 0,
              "%s: in %u out %u culled %u clipped %u (expected 12/6/6/0)", name, st.tris_in, st.tris_out,
              st.tris_culled, st.tris_clipped);
        allowed[0] = CLEAR;
        for (f = 0; f < 6; f++) {
            int expect = (f == ((i & 1) ? 0 : 1)) || (f == ((i & 2) ? 2 : 3)) || (f == ((i & 4) ? 4 : 5));
            int cnt = count_px(g_fb, face565[f]);
            if (expect) {
                CHECK(cnt > 20000, "%s: visible face %d has only %d pixels", name, f, cnt);
                allowed[1 + nvis++] = face565[f];
            } else {
                CHECK(cnt == 0, "%s: back face %d visible (%d pixels)", name, f, cnt);
            }
        }
        CHECK(nvis == 3, "%s: %d faces expected visible", name, nvis);
        CHECK(count_foreign(g_fb, allowed, 4) == 0, "%s: pixels of unexpected colours", name);
        if (i == 7)
            write_ppm(g_fb, "cube_corner");
    }
    /* same view without culling but with Z: the three front colours still dominate */
    {
        v3 eye = V3(3, 3, 3);
        make_vp(eye, V3(0, 0, 0), 60, 0.1, 100, vp);
        m_ident(M);
        set_hdr(&h, 120, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[0], 1, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE, M, 255, 255, 255);
        run_scene(&h, d, &st, "cube_nocull", CLEAR, g_fb2);
        CHECK(st.tris_out == 12, "cube_nocull: %u records", st.tris_out);
        render(&g_rl, CLEAR, g_fb2);
        {
            int back = count_px(g_fb2, face565[1]) + count_px(g_fb2, face565[3]) + count_px(g_fb2, face565[5]);
            int front = count_px(g_fb2, face565[0]) + count_px(g_fb2, face565[2]) + count_px(g_fb2, face565[4]);
            CHECK(back < 200 && front > 100000, "cube_nocull: back-face pixels %d front %d", back, front);
            printf("  no culling + Z test: %d back-face pixels (silhouette edge only), %d front\n", back, front);
        }
    }

    /* ---- 2. lighting ---- */
    printf("[lighting]\n");
    {
        uint16_t c[3];
        int lum[3];
        make_vp(V3(3, 3, 3), V3(0, 0, 0), 60, 0.1, 100, vp);
        m_ident(M);
        set_hdr(&h, 130, vp, V3(-1, -2, -3), 0.2, 1);   /* light travels towards -x,-y,-z */
        set_draw(&d[0], 2, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING, M, 255, 255, 255);
        run_scene(&h, d, &st, "cube_lit", CLEAR, g_fb);
        write_ppm(g_fb, "cube_lit");
        /* sample the centre of each visible face: project (1,0,0), (0,1,0), (0,0,1) */
        for (k = 0; k < 3; k++) {
            double p[4] = {k == 0, k == 1, k == 2, 1}, cl[4];
            int rr, sx, sy;
            for (rr = 0; rr < 4; rr++)
                cl[rr] = vp[rr] * p[0] + vp[4 + rr] * p[1] + vp[8 + rr] * p[2] + vp[12 + rr];
            sx = (int)((cl[0] / cl[3] * 0.5 + 0.5) * 1280);
            sy = (int)((0.5 - cl[1] / cl[3] * 0.5) * 720);
            c[k] = g_fb[sy * GPU_W + sx];
            lum[k] = (c[k] >> 11) + ((c[k] >> 5) & 63) / 2 + (c[k] & 31);
        }
        CHECK(c[0] != c[1] && c[1] != c[2] && lum[2] > lum[1] && lum[1] > lum[0],
              "cube_lit: face brightness order wrong (+X %04x +Y %04x +Z %04x)", c[0], c[1], c[2]);
        printf("  +X %04x  +Y %04x  +Z %04x (brightness increases with -L.n)\n", c[0], c[1], c[2]);

        /* sphere lit from the right: right half brighter, far left = ambient */
        make_vp(V3(0, 0, 4), V3(0, 0, 0), 50, 0.1, 100, vp);
        set_hdr(&h, 131, vp, V3(-1, 0, 0), 0.25, 1);
        set_draw(&d[0], 5, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING, M, 255, 255, 255);
        run_scene(&h, d, &st, "sphere_side_lit", CLEAR, g_fb);
        {
            uint16_t cr = g_fb[360 * GPU_W + 640 + 150], cl = g_fb[360 * GPU_W + 640 - 150];
            CHECK((cr >> 11) > (cl >> 11) + 10, "sphere_side_lit: right %04x not brighter than left %04x", cr, cl);
            /* ambient-only side: 255*0.25 = 63.75 -> 64 red channel -> r5 = 8 */
            CHECK((cl >> 11) == (64 >> 3), "sphere_side_lit: left (ambient) pixel %04x", cl);
        }
    }

    /* ---- 3. showcase scene ---- */
    printf("[scene] cube + sphere + torus, lit\n");
    {
        double T[16], R[16];
        make_vp(V3(0.5, 2.2, 6.5), V3(0, 0, 0), 55, 0.1, 100, vp);
        set_hdr(&h, 140, vp, V3(-0.4, -1, -0.6), 0.22, 3);
        m_trs(V3(-2.3, 0, 0), V3(1, 1, 0), 0.6, 0.9, T);
        set_draw(&d[0], 1, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING, T, 255, 255, 255);
        m_trs(V3(0.2, 0.1, -0.5), V3(0, 1, 0), 0.3, 1.1, R);
        set_draw(&d[1], 3, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING, R, 255, 255, 255);
        m_trs(V3(2.4, 0.2, 0.3), V3(1, 0, 0.3), 1.1, 1.0, T);
        set_draw(&d[2], 4, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING, T, 255, 255, 255);
        run_scene(&h, d, &st, "scene", CLEAR, g_fb);
        write_ppm(g_fb, "scene");
        printf("  in %u out %u culled %u clipped %u\n", st.tris_in, st.tris_out, st.tris_culled, st.tris_clipped);
        CHECK(st.tris_clipped == 0 && st.tris_out > 1000, "scene: unexpected stats");

        /* determinism: other frames in between, then the same frame again */
        {
            reclist keep = g_rl;
            geom_stats st2;
            int r;
            g_rl.r = NULL; g_rl.cap = 0; rl_clear(&g_rl);
            set_hdr(&h, 999, vp, V3(0, -1, 0), 0.5, 1);
            set_draw(&d[3], 6, GEOM_DRAW_LIGHTING, M, 10, 20, 30);
            geom_frame(&h, &d[3], collect, &g_rl, &st2);
            rl_clear(&g_rl);
            set_hdr(&h, 140, vp, V3(-0.4, -1, -0.6), 0.22, 3);
            r = geom_frame(&h, d, collect, &g_rl, &st2);
            CHECK(r == 0 && g_rl.n == keep.n && memcmp(g_rl.r, keep.r, (size_t)keep.n * GPU_REC_BYTES) == 0 &&
                  memcmp(&st2, &st, sizeof st) == 0, "scene: second run differs (determinism)");
            free(keep.r);
        }
    }

    /* ---- 4. camera inside objects: near-plane clipping, full coverage ---- */
    printf("[near] camera inside cube / sphere\n");
    {
        static const double dirs[4][3] = { {1, 0.3, 0.2}, {-0.2, -1, 0.1}, {0.7, 0.7, -0.7}, {0, 0, -1} };
        for (i = 0; i < 4; i++) {
            char name[64];
            uint16_t allowed[6];
            int nf;
            v3 at = V3(dirs[i][0], dirs[i][1], dirs[i][2]);
            make_vp(V3(0.1, -0.05, 0.02), at, 75, 0.05, 50, vp);
            m_ident(M);
            set_hdr(&h, 200 + (uint32_t)i, vp, V3(0, -1, 0), 0.2, 1);
            set_draw(&d[0], 1, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE, M, 255, 255, 255);
            snprintf(name, sizeof name, "inside_cube%d", i);
            run_scene(&h, d, &st, name, CLEAR, g_fb);
            nf = count_foreign(g_fb, face565, 6);
            CHECK(nf == 0, "%s: %d pixels not covered by a face colour", name, nf);
            CHECK(st.tris_clipped > 0, "%s: expected near-plane clipping", name);
            if (i == 0)
                write_ppm(g_fb, "inside_cube");
            set_draw(&d[0], 1, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, M, 255, 255, 255);
            run_scene(&h, d, &st, name, CLEAR, NULL);
            CHECK(st.tris_out == 0 && g_rl.n == 1, "%s: CULL_BACK from inside left %u triangles", name, st.tris_out);
            /* sphere */
            set_draw(&d[0], 5, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE, M, 255, 255, 255);
            snprintf(name, sizeof name, "inside_sphere%d", i);
            run_scene(&h, d, &st, name, CLEAR, g_fb);
            allowed[0] = RGB565(red[0], red[1], red[2]);
            nf = count_foreign(g_fb, allowed, 1);
            CHECK(nf == 0, "%s: %d pixels not sphere-coloured", name, nf);
        }
    }

    /* ---- 5. ground plane extending behind the camera ---- */
    printf("[near] ground plane behind the camera, horizon\n");
    {
        uint16_t gc = RGB565(90, 200, 60);
        int y, x, badtop = 0, badbot = 0;
        make_vp(V3(0, 0, 0), V3(0, 0, -1), 60, 0.1, 1000, vp);
        m_ident(M);
        set_hdr(&h, 300, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[0], 7, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, M, 255, 255, 255);
        run_scene(&h, d, &st, "ground", CLEAR, g_fb);
        write_ppm(g_fb, "ground");
        /* far edge z = -100 projects to sy = 366.24; the camera is 1 above the plane */
        for (y = 0; y < GPU_H; y++)
            for (x = 0; x < GPU_W; x++) {
                uint16_t p = g_fb[y * GPU_W + x];
                if (y <= 365 && p != CLEAR) badtop++;
                if (y >= 367 && p != gc) badbot++;
            }
        CHECK(badtop == 0 && badbot == 0, "ground: %d plane pixels above the horizon, %d holes below", badtop, badbot);
        CHECK(st.tris_clipped > 0 && st.tris_culled > 0, "ground: expected clipped and rejected tiles (clipped %u culled %u)",
              st.tris_clipped, st.tris_culled);
        printf("  in %u out %u culled %u clipped %u\n", st.tris_in, st.tris_out, st.tris_culled, st.tris_clipped);
    }

    /* ---- 5b. the near plane removes geometry closer than `near` (not just behind the eye) ---- */
    printf("[near] floor closer than the near plane is cut away\n");
    {
        uint16_t gc = RGB565(90, 200, 60);
        int y, x, bad = 0;
        double S[16];
        make_vp(V3(0, 0, 0), V3(0, 0, -1), 60, 0.5, 100, vp);
        m_trs(V3(0, 0, 0), V3(0, 1, 0), 0.0, 0.05, S);     /* floor at y = -0.05, z in [-5, 5] */
        set_hdr(&h, 310, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[0], 7, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, S, 255, 255, 255);
        run_scene(&h, d, &st, "near_floor", CLEAR, g_fb);
        write_ppm(g_fb, "near_floor");
        /* far edge (z=-5) at sy = 366.24, near-plane cut (z=-0.5) at sy = 422.35 */
        for (y = 0; y < GPU_H; y++)
            for (x = 0; x < GPU_W; x++) {
                uint16_t p = g_fb[y * GPU_W + x];
                if ((y <= 365 || y >= 423) && p != CLEAR) bad++;
                if (y >= 367 && y <= 421 && p != gc) bad++;
            }
        CHECK(bad == 0, "near_floor: %d pixels wrong (geometry in front of the near plane drawn, or holes)", bad);
    }

    /* ---- 6. screen-covering quads (guard-band clipping + fan watertightness) ---- */
    printf("[clip] screen-covering quad and 12x12 grid\n");
    for (k = 0; k < 2; k++) {
        uint16_t allowed[1];
        int nf;
        make_vp(V3(0.3, -0.2, 2), V3(0.35, -0.1, 0), 70, 0.1, 100, vp);
        m_trs(V3(0, 0, 0), V3(0.3, 1, 0.2), 0.35, 1.0, M);
        set_hdr(&h, 400 + (uint32_t)k, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[0], k ? 9 : 8, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, M, 255, 255, 255);
        run_scene(&h, d, &st, k ? "wall12" : "wall1", CLEAR, g_fb);
        allowed[0] = k ? RGB565(200, 120, 250) : RGB565(250, 250, 250);
        nf = count_foreign(g_fb, allowed, 1);
        CHECK(nf == 0, "%s: %d pixels not covered", k ? "wall12" : "wall1", nf);
        CHECK(st.tris_clipped > 0, "wall: expected clipping");
    }

    /* ---- 7. torus fly-through (near plane cuts the tube, camera inside the tube) ---- */
    printf("[near] 240-frame fly-through a torus\n");
    {
        uint16_t allowed[2];
        int f, worst = 0, clipped_frames = 0, inside_frames = 0;
        allowed[0] = CLEAR;
        allowed[1] = RGB565(gold[0], gold[1], gold[2]);
        for (f = 0; f < 240; f++) {
            double s = f / 239.0;
            /* straight line through the tube at x = 1 (tube centre), from z = -3 to z = +3 */
            v3 eye = V3(1.0 + 0.05 * sin(7 * s), 0.02 * cos(5 * s), -3.0 + 6.0 * s);
            v3 at = vadd(eye, V3(0.3 * sin(3 * s), 0.2 * cos(2 * s), 1.0));
            double nearp = (f % 3 == 0) ? 0.001 : 0.05;
            char name[64];
            int nf;
            make_vp(eye, at, 80, nearp, 100, vp);
            m_ident(M);
            set_hdr(&h, 1000 + (uint32_t)f, vp, V3(0, -1, 0), 0.2, 1);
            set_draw(&d[0], 6, (f & 1) ? (GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE) : (GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK),
                     M, 255, 255, 255);
            snprintf(name, sizeof name, "flythrough%d", f);
            run_scene(&h, d, &st, name, CLEAR, g_fb);
            nf = count_foreign(g_fb, allowed, 2);
            if (nf > worst) worst = nf;
            if (st.tris_clipped) clipped_frames++;
            /* camera well inside the tube and no culling: the tube wall must cover every pixel */
            {
                double ring = sqrt(eye.x * eye.x + eye.z * eye.z) - 1.0;
                if ((f & 1) && sqrt(ring * ring + eye.y * eye.y) < 0.25) {
                    int holes = count_px(g_fb, CLEAR);
                    inside_frames++;
                    CHECK(holes == 0, "%s: camera inside the tube but %d pixels uncovered", name, holes);
                }
            }
            if (f == 121)
                write_ppm(g_fb, "torus_inside_tube");
            if (f == 60)
                write_ppm(g_fb, "torus_near");
        }
        CHECK(worst == 0, "flythrough: up to %d pixels of foreign colour", worst);
        CHECK(clipped_frames > 100, "flythrough: only %d frames needed clipping", clipped_frames);
        CHECK(inside_frames >= 5, "flythrough: only %d frames with the camera inside the tube", inside_frames);
        printf("  %d/240 frames needed clipping, no foreign pixels; %d frames inside the tube fully covered\n",
               clipped_frames, inside_frames);
    }

    /* ---- 8. partly off-screen objects vs screen-space reference ---- */
    printf("[clip] objects partly off screen vs screen-space reference\n");
    {
        double T[16];
        int x, y, mism = 0, cov = 0, colour_mismatch = 0;
        make_vp(V3(0, 0, 0), V3(0, 0, -1), 60, 0.1, 100, vp);
        set_hdr(&h, 500, vp, V3(0, -1, 0), 0.2, 3);
        m_trs(V3(-5.13, 0.4, -5), V3(0, 1, 0), 0.2, 2.0, T);          /* crosses left plane 1.35w */
        set_draw(&d[0], 5, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, T, 255, 255, 255);
        m_trs(V3(5.3, -2.9, -5), V3(1, 0.2, 0), 1.2, 1.6, T);          /* bottom-right corner */
        set_draw(&d[1], 6, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, T, 255, 255, 255);
        m_trs(V3(0.5, 3.9, -5.5), V3(1, 1, 1), 0.5, 1.6, T);           /* crosses the top plane 1.65w */
        set_draw(&d[2], 10, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, T, 255, 255, 255);
        run_scene(&h, d, &st, "offscreen", CLEAR, g_fb);
        write_ppm(g_fb, "offscreen");
        CHECK(st.tris_clipped > 0, "offscreen: expected guard-band clipping in clip space (clipped=%u)", st.tris_clipped);
        rl_clear(&g_rl2);
        CHECK(screen_ref(&h, d, &g_rl2) == 0, "offscreen: screen reference failed");
        render(&g_rl2, CLEAR, g_fb2);
        for (y = 0; y < GPU_H; y++)
            for (x = 0; x < GPU_W; x++) {
                uint16_t a = g_fb[y * GPU_W + x], b = g_fb2[y * GPU_W + x];
                if ((a != CLEAR) != (b != CLEAR)) mism++;
                else if (a != b) colour_mismatch++;
                if (a != CLEAR) cov++;
            }
        CHECK(mism <= 40 && colour_mismatch <= 40, "offscreen: %d coverage / %d colour mismatches vs screen-space reference",
              mism, colour_mismatch);
        printf("  covered %d px; %d coverage and %d colour differences vs screen-space clipping (edge rounding)\n",
               cov, mism, colour_mismatch);
    }

    /* ---- 8b. random fuzz vs the reference: arbitrary matrices, lights, flags, NaN/Inf/huge ---- */
    printf("[fuzz] 400 random frames vs the SPEC 6 reference\n");
    {
        mesh soup;
        int f, j;
        static const uint16_t ids[5] = {1, 3, 4, 11, 6};
        g_seed = 12345u;
        mesh_alloc(&soup, 300, 900);
        for (j = 0; j < 300; j++) {
            v3 p = rnd_v3(-3, 3), n = rnd_v3(-1, 1);
            int r = (int)(rnd_u32() & 255), g = (int)(rnd_u32() & 255), b = (int)(rnd_u32() & 255);
            addv(&soup, p, n, r, g, b);
        }
        soup.v[7].nx = soup.v[7].ny = soup.v[7].nz = 0.0f;     /* zero-length normal */
        for (j = 0; j < 900; j++)
            soup.ix[soup.ni++] = (uint16_t)(rnd_u32() % 300);
        upload(11, &soup);
        for (f = 0; f < 400; f++) {
            char name[64];
            int nd = 1 + (int)(rnd_u32() % 4), kind = (int)(rnd_u32() % 4);
            v3 light;
            double amb;
            if (kind == 0) {             /* fully random 4x4 */
                for (j = 0; j < 16; j++) vp[j] = rnd_d(-2, 2);
            } else {                     /* random camera, sometimes inside objects */
                v3 eye = rnd_v3(-4, 4), at = rnd_v3(-1, 1);
                double fov = rnd_d(20, 120), nr = rnd_d(0.001, 1.0), fr = rnd_d(2, 1000);
                make_vp(eye, at, fov, nr, fr, vp);
            }
            light = rnd_v3(-1, 1);
            amb = rnd_d(0, 1);
            set_hdr(&h, 7000 + (uint32_t)f, vp, light, amb, nd);
            for (j = 0; j < nd; j++) {
                v3 tr = rnd_v3(-3, 3), ax = rnd_v3(-1, 1);
                double ang = rnd_d(-4, 4), sc = rnd_d(0.2, 3);
                uint16_t id, fl;
                int cr, cg, cb;
                m_trs(tr, ax, ang, sc, M);
                if (kind == 1) M[3] = rnd_d(-0.5, 0.5);          /* projective model matrix */
                id = ids[rnd_u32() % 5];
                fl = (uint16_t)(rnd_u32() & 15);
                cr = (int)(rnd_u32() & 255); cg = (int)(rnd_u32() & 255); cb = (int)(rnd_u32() & 255);
                set_draw(&d[j], id, fl, M, cr, cg, cb);
            }
            if (f % 50 == 7) h.viewproj[rnd_u32() % 16] = NAN;
            if (f % 50 == 17) h.viewproj[rnd_u32() % 16] = INFINITY;
            if (f % 50 == 27) d[0].model[rnd_u32() % 16] = 1e30f;
            if (f % 50 == 37) h.light_dir[0] = NAN;
            if (f % 50 == 47) h.ambient = -3.0f;
            snprintf(name, sizeof name, "fuzz%d", f);
            run_scene(&h, d, &st, name, CLEAR, (f % 40 == 0) ? g_fb : NULL);
        }
        mesh_free(&soup);
    }

    /* ---- 9. mesh store ---- */
    printf("[store] limits, replacement, compaction, bad data, streaming\n");
    {
        mesh t1, big;
        reclist a, b;
        int r, id;
        uint32_t nm, nv, ni;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        geom_reset();
        geom_x_usage(&nm, &nv, &ni);
        CHECK(nm == 0 && nv == 0 && ni == 0, "reset: store not empty");
        for (id = 0; id < GEOM_MAX_MESHES; id++) {
            build_sphere(&t1, 0.3 + 0.01 * id, 4 + id % 3, 6 + id % 5, 0, teal);
            r = geom_mesh_upload((uint16_t)(1000 + id), (uint32_t)t1.nv, (uint32_t)t1.ni, t1.v, t1.ix);
            CHECK(r == 0, "store: upload %d failed %d", id, r);
            ref_store((uint16_t)(1000 + id), &t1);   /* kept (leaked on purpose) for the reference */
        }
        r = geom_mesh_upload(2000, (uint32_t)cube.nv, (uint32_t)cube.ni, cube.v, cube.ix);
        CHECK(r == GPU_ERR_NOMEM, "store: 17th mesh id should give NOMEM, got %d", r);
        /* frame of meshes 1000 and 1015, before and after replacing / compacting the ones between */
        make_vp(V3(0, 0, 3), V3(0, 0, 0), 60, 0.1, 100, vp);
        m_ident(M);
        set_hdr(&h, 600, vp, V3(0, -1, 0), 0.2, 2);
        set_draw(&d[0], 1000, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_LIGHTING, M, 255, 255, 255);
        m_trs(V3(0.5, 0.2, 0), V3(0, 1, 0), 0.1, 1.0, M);
        set_draw(&d[1], 1015, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_LIGHTING, M, 255, 255, 255);
        geom_frame(&h, d, collect, &a, &st);
        CHECK(a.n > 10, "store: frame too small");
        build_torus(&t1, 1.0, 0.3, 30, 12, 1, gold);         /* larger replacement for 1005 */
        r = geom_mesh_upload(1005, (uint32_t)t1.nv, (uint32_t)t1.ni, t1.v, t1.ix);
        CHECK(r == 0, "store: replacement upload failed %d", r);
        build_sphere(&t1, 0.2, 3, 4, 0, teal);               /* smaller replacement for 1002 */
        r = geom_mesh_upload(1002, (uint32_t)t1.nv, (uint32_t)t1.ni, t1.v, t1.ix);
        CHECK(r == 0, "store: replacement upload failed %d", r);
        geom_frame(&h, d, collect, &b, &st);
        CHECK(a.n == b.n && memcmp(a.r, b.r, (size_t)a.n * GPU_REC_BYTES) == 0,
              "store: meshes 1000/1015 changed after replacing others");
        /* invalid uploads */
        {
            uint16_t badix[3] = {0, 1, 60000};
            r = geom_mesh_upload(1003, 3, 2, cube.v, cube.ix);
            CHECK(r == GPU_ERR_ARG && geom_x_mesh_exists(1003), "store: nidx%%3 upload: %d (old mesh must stay)", r);
            r = geom_mesh_upload(1003, 3, 3, cube.v, badix);
            CHECK(r == GPU_ERR_ARG && !geom_x_mesh_exists(1003), "store: bad index upload: %d (id must be gone)", r);
            rl_clear(&b);
            set_hdr(&h, 601, vp, V3(0, -1, 0), 0.2, 1);
            set_draw(&d[2], 1003, 0, M, 255, 255, 255);
            r = geom_frame(&h, &d[2], collect, &b, &st);
            CHECK(r == GPU_ERR_ARG && b.n == 0, "store: drawing a missing mesh: ret %d, %d records", r, b.n);
            set_hdr(&h, 602, vp, V3(0, -1, 0), 0.2, 3);
            d[2] = d[1];
            d[1].mesh_id = 4242;
            r = geom_frame(&h, d, collect, &b, &st);
            CHECK(r == GPU_ERR_ARG && b.n == 0, "store: missing mesh in 2nd draw must emit nothing (ret %d, %d recs)", r, b.n);
            r = geom_mesh_upload(1003, 0, 3, NULL, badix);
            CHECK(r == GPU_ERR_ARG, "store: indices without vertices: %d", r);
            r = geom_mesh_upload(1003, 0, 0, NULL, NULL);
            CHECK(r == 0 && geom_x_mesh_exists(1003), "store: empty mesh: %d", r);
            rl_clear(&b);
            set_hdr(&h, 603, vp, V3(0, -1, 0), 0.2, 1);
            set_draw(&d[2], 1003, 0, M, 255, 255, 255);
            r = geom_frame(&h, &d[2], collect, &b, &st);
            CHECK(r == 0 && b.n == 1 && st.tris_in == 0, "store: empty mesh frame: ret %d recs %d", r, b.n);
        }
        /* emit abort propagates, NULL header */
        rl_clear(&b);
        b.abort_at = 3;
        set_hdr(&h, 604, vp, V3(0, -1, 0), 0.2, 1);
        set_draw(&d[2], 1015, 0, M, 255, 255, 255);
        r = geom_frame(&h, &d[2], collect, &b, &st);
        CHECK(r == -7 && b.calls == 3 && b.n == 2, "store: emit abort: ret %d calls %d", r, b.calls);
        CHECK(geom_frame(NULL, d, collect, &b, &st) == GPU_ERR_ARG, "NULL header");
        /* capacity limits */
        geom_reset();
        mesh_alloc(&big, GEOM_MAX_VERTS, GEOM_MAX_INDICES);
        for (i = 0; i < GEOM_MAX_VERTS; i++)
            addv(&big, V3(i % 97, i / 97, 0), V3(0, 0, 1), i & 255, 0, 0);
        for (i = 0; i < GEOM_MAX_INDICES; i++)
            big.ix[big.ni++] = (uint16_t)((i * 7) % GEOM_MAX_VERTS);
        r = geom_mesh_upload(1, (uint32_t)big.nv, (uint32_t)big.ni, big.v, big.ix);
        CHECK(r == 0, "store: full-size mesh: %d", r);
        r = geom_mesh_upload(2, 1, 0, big.v, NULL);
        CHECK(r == GPU_ERR_NOMEM, "store: vertex pool full should give NOMEM: %d", r);
        r = geom_mesh_upload(1, (uint32_t)big.nv, (uint32_t)big.ni, big.v, big.ix);
        CHECK(r == 0, "store: same-size replacement of a full store: %d", r);
        r = geom_mesh_upload(3, GEOM_MAX_VERTS + 1, 0, big.v, NULL);
        CHECK(r == GPU_ERR_NOMEM && geom_x_mesh_exists(1), "store: oversize: %d", r);
        r = geom_mesh_upload(3, 3, GEOM_MAX_INDICES + 3, big.v, big.ix);
        CHECK(r == GPU_ERR_NOMEM, "store: too many indices: %d", r);
        /* streaming API: begin/abort leaves the id deleted, begin/commit stores */
        {
            void *vd, *idp;
            r = geom_x_mesh_begin(1, 4, 6, &vd, &idp);
            CHECK(r == 0 && !geom_x_mesh_exists(1), "stream: begin must delete the old mesh once the new one fits");
            geom_x_mesh_abort();
            CHECK(geom_x_mesh_commit() == GPU_ERR_ARG, "stream: commit after abort");
            r = geom_x_mesh_begin(5, (uint32_t)cube.nv, (uint32_t)cube.ni, &vd, &idp);
            memcpy(vd, cube.v, (size_t)cube.nv * sizeof(geom_vertex));
            memcpy(idp, cube.ix, (size_t)cube.ni * 2);
            CHECK(r == 0 && geom_x_mesh_commit() == 0 && geom_x_mesh_exists(5), "stream: begin/commit");
            geom_x_usage(&nm, &nv, &ni);
            CHECK(nm == 1 && nv == 24 && ni == 36, "stream: usage %u/%u/%u", nm, nv, ni);
        }
        mesh_free(&big);
        free(a.r);
        free(b.r);
    }

    printf("\ngeom record hash: %016llx\n", (unsigned long long)g_hash);
    printf("geom_frame time: %.3f s for %u input triangles (x86, incl. setup)\n", g_geom_sec, g_tris_total);
    printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    mesh_free(&cube); mesh_free(&cubew); mesh_free(&sphere); mesh_free(&sphere_u); mesh_free(&torus);
    mesh_free(&torus_u); mesh_free(&ground); mesh_free(&wall1); mesh_free(&wall12); mesh_free(&sph_small);
    free(g_rl.r); free(g_rl2.r); free(g_fb); free(g_fb2);
    return g_fail ? 1 : 0;
}

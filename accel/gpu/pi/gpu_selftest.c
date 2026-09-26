/*
 * gpu_selftest -- end-to-end, bit-exact self-test of the FPGA-GPU system (SPEC 11, 13.4, 14).
 *
 *   gpu_selftest [--fpga HOST[:PORT]] [--teensy auto|DEV|tcp:H:P|none] [--quick] [--seed N]
 *                [--dump DIR] [--frames N] [--return-seconds S] [--offline]
 *
 * Every rendered frame is compared pixel for pixel with the golden model (common/gpu_refrast.c)
 * fed with the records this program predicts locally: common/gpu_setup.c for what the Zynq daemon
 * sets up, common/geom.c for what the Teensy computes (same C code, fp-contract off everywhere).
 * Tests (PASS / FAIL / SKIP each; exit status 0 only if no enabled test failed):
 *   local        meshes CCW-front + outward normals, font, local geometry sanity (no hardware)
 *   hello        daemon HELLO, PL ID/version, STATUS, MMCM lock, vsync rate
 *   ps_*         PS path (Pi -> daemon -> PS FIFO): NET_TRIS (with guard-band clipping, culling,
 *                Z modes), NET_RECT, NET_SPRITE_UPLOAD/DRAW (colour key, negative/edge clipping),
 *                NET_RECORDS -- READBACK == prediction
 *   frame_get_*  NET_FRAME_GET (observer connection): returned frame == prediction at scale 1 and 2
 *   teensy_*     T_RECORDS through the parallel bus (+ PS part of the same list); T_MESH + T_FRAME:
 *                Teensy geometry == local geom.c (stats and rendered frame)
 *   geom_return  GEOM_FRAME_RETURN records == local prediction; with GEOM_FRAME_NO_BUS nothing
 *                reaches the FPGA
 *   auto_*       autonomous mode: static scene frame bit-exact (incl. overlay records), frames
 *                produced, T_FRAME refused while running, animation changes frames, T_AUTO 0 stops
 *   tput_*       frames/s and triangles/s of the PS and Teensy paths, Teensy us/frame, return-path
 *                frames/s at scale 1 and 2
 *   health       AXI_ERRORS / BAD_RECORDS / LIST_OVERFLOW / daemon errors / Teensy bus timeouts
 *                unchanged over the whole run
 * On a pixel mismatch DIR/selftest_<test>_{expected,got,diff}.ppm are written.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x8.h"
#include "gpu_math.h"
#include "gpu_refrast.h"
#include "img.h"
#include "mesh.h"
#include "tool_common.h"

#define CLEAR_A 0x2104u
#define CLEAR_B 0x0010u
#define KEY     0xF81Fu

/* ============================================================================================ */
/* state, results                                                                                 */
/* ============================================================================================ */
static struct {
    tool_opts    o;
    gpu_conn    *g, *obs;
    teensy_conn *t;
    uint32_t     fno, rng;
    const char  *dump;
    int          quick, frames;
    double       return_secs;
    int          npass, nfail, nskip;
    uint32_t     exp_overflow;      /* LIST_OVERFLOW increments the tests cause on purpose */
    uint32_t     exp_daemon_errors; /* commands the tests make the daemon reject on purpose */
    uint16_t    *exp, *got, *tmp;
    net_status_reply st0;
    t_stats_reply    ts0;
    int          have_ts0;
    mesh_t       mesh[4];
} S;

static void report(const char *name, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void report(const char *name, int ok, const char *fmt, ...)
{
    char d[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    printf("%s  %-20s %s\n", ok ? "PASS" : "FAIL", name, d);
    fflush(stdout);
    if (ok)
        S.npass++;
    else
        S.nfail++;
}

static void skip(const char *name, const char *why)
{
    printf("SKIP  %-20s %s\n", name, why);
    fflush(stdout);
    S.nskip++;
}

static const char *gerr(int st)
{
    static char b[400];
    snprintf(b, sizeof b, "%s%s%s", fgpu_strerror(st), st <= -100 ? ": " : "", st <= -100 ? gpu_errmsg(S.g) : "");
    return b;
}

static const char *terr(int st)
{
    static char b[400];
    snprintf(b, sizeof b, "%s%s%s", fgpu_strerror(st), st <= -100 ? ": " : "", st <= -100 ? teensy_errmsg(S.t) : "");
    return b;
}

static const char *oerr(int st)
{
    static char b[400];
    snprintf(b, sizeof b, "%s%s%s", fgpu_strerror(st), st <= -100 ? ": " : "", st <= -100 ? gpu_errmsg(S.obs) : "");
    return b;
}

/* deterministic PRNG (xorshift32) */
static uint32_t rnd(void)
{
    uint32_t x = S.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    S.rng = x;
    return x;
}

static float frand(float a, float b)
{
    uint32_t r = rnd();
    return a + (b - a) * (float)(r >> 8) * (1.0f / 16777216.0f);
}

/* ============================================================================================ */
/* record lists, local DDR model, golden rendering, comparison                                    */
/* ============================================================================================ */
typedef struct {
    uint32_t (*r)[GPU_REC_WORDS];
    uint32_t n, cap;
} reclist;

static int rl_add(reclist *l, const uint32_t rec[GPU_REC_WORDS])
{
    if (l->n == l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 256;
        uint32_t (*p)[GPU_REC_WORDS] = realloc(l->r, (size_t)nc * GPU_REC_BYTES);
        if (!p) {
            fprintf(stderr, "gpu_selftest: out of memory\n");
            exit(3);
        }
        l->r = p;
        l->cap = nc;
    }
    memcpy(l->r[l->n++], rec, GPU_REC_BYTES);
    return 0;
}

static void rl_free(reclist *l)
{
    free(l->r);
    memset(l, 0, sizeof *l);
}

static int emit_to_list(void *user, const uint32_t rec[GPU_REC_WORDS])
{
    return rl_add((reclist *)user, rec);
}

/* sprite pixels as the daemon stored them in the pool (address -> pixels) */
typedef struct {
    uint32_t addr, bytes;
    uint16_t *px;
} region;
static region R[64];
static int nR;

static void ddr_forget(void)
{
    int i;
    for (i = 0; i < nR; i++)
        free(R[i].px);
    nR = 0;
}

static void ddr_put(uint32_t addr, const uint16_t *px, uint32_t npx)
{
    int i;
    for (i = 0; i < nR; i++)
        if (R[i].addr == addr)
            break;
    if (i == nR) {
        if (nR == 64)
            return;
        nR++;
    } else {
        free(R[i].px);
    }
    R[i].addr = addr;
    R[i].bytes = npx * 2u;
    R[i].px = malloc((size_t)npx * 2u);
    if (R[i].px)
        memcpy(R[i].px, px, (size_t)npx * 2u);
}

static uint16_t ddr_rd(void *user, uint32_t a)
{
    int i;
    (void)user;
    for (i = 0; i < nR; i++)
        if (a >= R[i].addr && a - R[i].addr < R[i].bytes && R[i].px)
            return R[i].px[(a - R[i].addr) / 2u];
    return 0;
}

/* The PL keeps at most GPU_LIST_SLOTS TRI + SPRITE records per list (SPEC 7: later ones are
 * dropped, LIST_OVERFLOW += 1 each); NOP / END / unknown records take no slot. Returns the number
 * of records the PL drops. */
static uint32_t predict(const reclist *l, uint16_t clear, uint16_t *fb)
{
    reclist k;
    uint32_t i, slots = 0, dropped = 0;
    memset(&k, 0, sizeof k);
    for (i = 0; i < l->n; i++) {
        uint32_t ty = GPU_W0_TYPE(l->r[i][0]);
        if (ty == GPU_REC_TRI || ty == GPU_REC_SPRITE) {
            if (slots == GPU_LIST_SLOTS) {
                dropped++;
                continue;
            }
            slots++;
            rl_add(&k, l->r[i]);
        }
    }
    gpu_refrast_frame(k.n ? &k.r[0][0] : NULL, (int)k.n, clear, ddr_rd, NULL, fb);
    rl_free(&k);
    return dropped;
}

static void downscale2(const uint16_t *full, uint16_t *half)
{
    int x, y;
    for (y = 0; y < GPU_H / 2; y++)
        for (x = 0; x < GPU_W / 2; x++)
            half[y * (GPU_W / 2) + x] = full[(size_t)(2 * y) * GPU_W + (size_t)(2 * x)];
}

/* 0 = identical; else number of differing pixels (+ PPM dumps) */
static long frame_diff(const char *name, const uint16_t *exp, const uint16_t *got, int w, int h, char *detail,
                       size_t dn)
{
    long bad = 0, i, n = (long)w * h;
    int fx = -1, fy = -1;
    for (i = 0; i < n; i++)
        if (exp[i] != got[i]) {
            if (!bad) {
                fx = (int)(i % w);
                fy = (int)(i / w);
            }
            bad++;
        }
    if (!bad) {
        snprintf(detail, dn, "%dx%d bit-exact", w, h);
        return 0;
    }
    {
        char p[700];
        uint16_t *diff = malloc((size_t)n * 2u);
        snprintf(p, sizeof p, "%s/selftest_%s_expected.ppm", S.dump, name);
        img_write_ppm(p, exp, w, h);
        snprintf(p, sizeof p, "%s/selftest_%s_got.ppm", S.dump, name);
        img_write_ppm(p, got, w, h);
        if (diff) {
            for (i = 0; i < n; i++)
                diff[i] = exp[i] == got[i] ? (uint16_t)((exp[i] >> 2) & 0x39E7u) : 0xF800u;
            snprintf(p, sizeof p, "%s/selftest_%s_diff.ppm", S.dump, name);
            img_write_ppm(p, diff, w, h);
            free(diff);
        }
    }
    snprintf(detail, dn, "%ld of %d pixels differ, first at (%d,%d): expected 0x%04x got 0x%04x "
             "(dumped %s/selftest_%s_*.ppm)", bad, w * h, fx, fy, exp[fy * w + fx], got[fy * w + fx], S.dump, name);
    return bad;
}

/* ============================================================================================ */
/* daemon helpers                                                                                  */
/* ============================================================================================ */
static uint32_t fno_next(void) { return ++S.fno; }

/* frame numbers must only grow (gpu_wait_frame_no waits for LAST_FRAME_NO >= n) */
static int rebase_fno(void)
{
    net_status_reply s;
    int st = gpu_status(S.g, &s);
    if (st)
        return st;
    if ((int32_t)(gpu_reg(&s, GPU_R_LAST_FRAME_NO) - S.fno) >= 0)
        S.fno = gpu_reg(&s, GPU_R_LAST_FRAME_NO) + 1000u;
    return 0;
}

/* new CONTROL, then soft reset (collector restarts with the new sources; sprite pool reset) */
static int set_mode(uint32_t control, uint16_t clear)
{
    int st = gpu_set_config(S.g, control, clear);
    if (!st)
        st = gpu_reset(S.g);
    ddr_forget();
    return st;
}

static int upload(uint16_t id, int w, int h, const uint16_t *px, net_sprite_upload_reply *r)
{
    int st = gpu_sprite_upload(S.g, id, (uint16_t)w, (uint16_t)h, px, r);
    if (!st)
        ddr_put(r->ddr_addr, px, (uint32_t)(w * h));
    return st;
}

static int sync_errors(uint32_t *errs)
{
    return gpu_sync(S.g, errs);
}

/* the observer's outstanding FRAME_GET (if a test failed half-way) */
static void obs_drain(void)
{
    if (S.obs && gpu_frame_get_pending(S.obs)) {
        net_frame_get_reply info;
        gpu_frame_get_recv(S.obs, 5000, S.tmp, &info);
    }
}

/* ============================================================================================ */
/* scenes                                                                                          */
/* ============================================================================================ */
static void rand_tri(net_tri *t, int big)
{
    float cx = frand(-60.0f, 1340.0f), cy = frand(-60.0f, 780.0f);
    float sz = big ? frand(300.0f, 2600.0f) : frand(3.0f, 160.0f);
    uint32_t f = rnd() % 5u;
    int j;
    for (j = 0; j < 3; j++) {
        t->v[j].x = cx + frand(-sz, sz);
        t->v[j].y = cy + frand(-sz, sz);
        t->v[j].z = (rnd() % 8u) == 0 ? frand(-0.3f, 1.3f) : frand(0.0f, 1.0f);
        t->v[j].r = (uint8_t)rnd();
        t->v[j].g = (uint8_t)rnd();
        t->v[j].b = (uint8_t)rnd();
        t->v[j].a = 255;
    }
    t->flags = f == 0 ? 0u : f == 1 ? GPU_F_ZTEST : f == 2 ? GPU_F_ZWRITE : GPU_F_ZTEST | GPU_F_ZWRITE;
    t->cull = rnd() % 3u;
}

static void predict_tris(reclist *l, const net_tri *t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t out[GPU_SETUP_MAX_OUT][GPU_REC_WORDS];
        gpu_vtx v[3];
        int k, c;
        memcpy(v, t[i].v, sizeof v);
        c = gpu_setup_tri(v, t[i].flags & (GPU_F_ZTEST | GPU_F_ZWRITE), (int)t[i].cull, out, GPU_SETUP_MAX_OUT);
        for (k = 0; k < c; k++)
            rl_add(l, out[k]);
    }
}

/* rect on both sides */
static int do_rect(reclist *l, int x0, int y0, int x1, int y1, uint16_t c, float z, uint32_t flags)
{
    uint32_t rec[GPU_REC_WORDS];
    if (gpu_setup_rect(x0, y0, x1, y1, c, z, flags & (GPU_F_ZTEST | GPU_F_ZWRITE), rec) == 1)
        rl_add(l, rec);
    return gpu_rect(S.g, x0, y0, x1, y1, c, z, flags);
}

/* sprite draw on both sides (sprite geometry from the upload reply) */
static int do_sprite(reclist *l, uint16_t id, const net_sprite_upload_reply *u, int w, int h, int x, int y, int key_en)
{
    uint32_t rec[GPU_REC_WORDS];
    if (gpu_setup_sprite(x, y, w, h, u->ddr_addr, u->stride, key_en, KEY, rec) == 1)
        rl_add(l, rec);
    return gpu_sprite_draw(S.g, id, x, y, key_en, KEY);
}

static void make_sprite(uint16_t *px, int w, int h, int kind)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint16_t c;
            if (kind == 0) {
                c = gpu_rgb565((unsigned)(x * 255 / (w > 1 ? w - 1 : 1)), (unsigned)(y * 255 / (h > 1 ? h - 1 : 1)), 96);
                if ((x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < (h / 3) * (h / 3))
                    c = KEY;                                    /* transparent hole with the key */
            } else if (kind == 1) {
                c = ((x / 16 + y / 2) & 1) ? 0xFFE0u : 0x001Fu;
            } else {
                c = (uint16_t)(rnd() & 0xFFFFu);
            }
            px[y * w + x] = c;
        }
}

/* ============================================================================================ */
/* tests                                                                                           */
/* ============================================================================================ */
static void test_local(void)
{
    mesh_t m;
    char why[200];
    int ok = 1, w, h;
    uint16_t buf[64];
    char detail[300] = "";

    if (mesh_cube(&m, 1.0f, NULL) || mesh_check(&m, 0, 0, why, sizeof why)) {
        ok = 0;
        snprintf(detail, sizeof detail, "cube: %s", why);
    }
    mesh_free(&m);
    if (ok && (mesh_sphere(&m, 1.3f, 16, 24, NULL) || mesh_check(&m, 0, 0, why, sizeof why))) {
        ok = 0;
        snprintf(detail, sizeof detail, "sphere: %s", why);
    }
    mesh_free(&m);
    if (ok && (mesh_torus(&m, 1.0f, 0.35f, 32, 16, NULL) || mesh_check(&m, 1, 1.0f, why, sizeof why))) {
        ok = 0;
        snprintf(detail, sizeof detail, "torus: %s", why);
    }
    mesh_free(&m);
    report("local_meshes", ok, "%s", ok ? "cube, UV sphere, torus: CCW front faces, outward unit normals" : detail);

    w = font_text_w("AB\nC", 2);
    h = font_text_h("AB\nC", 2);
    memset(buf, 0, sizeof buf);
    font_draw(buf, 8, 8, 0, 0, "A", 1, 1, 0, 1);
    ok = w == 32 && h == 32 && buf[0 * 8 + 2] == 1 && buf[0 * 8 + 3] == 1 && buf[0 * 8 + 1] == 0 &&
         buf[4 * 8 + 0] == 1 && buf[4 * 8 + 5] == 1 && buf[7 * 8 + 0] == 0;
    report("local_font", ok, "text box %dx%d, glyph 'A' rows %s", w, h, ok ? "as in font8x8_basic" : "WRONG");

    /* local geometry: the three meshes seen from the front must produce triangles */
    {
        geom_frame_hdr hdr;
        geom_draw d;
        geom_stats st;
        reclist l;
        float view[16], proj[16];
        memset(&l, 0, sizeof l);
        geom_reset();
        mesh_cube(&m, 1.0f, NULL);
        geom_mesh_upload(1, m.nverts, m.nidx, m.v, m.idx);
        mesh_free(&m);
        memset(&hdr, 0, sizeof hdr);
        m4_look_at(view, v3_make(2.0f, 2.5f, 4.0f), v3_make(0, 0, 0), v3_make(0, 1, 0));
        m4_perspective(proj, 1.0f, 16.0f / 9.0f, 0.5f, 50.0f);
        {
            float vp[16];
            m4_mul(vp, proj, view);
            memcpy(hdr.viewproj, vp, sizeof vp);
        }
        hdr.light_dir[1] = -1.0f;
        hdr.ambient = 0.2f;
        hdr.ndraws = 1;
        memset(&d, 0, sizeof d);
        d.mesh_id = 1;
        d.flags = GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING;
        {
            float id[16];
            m4_identity(id);
            memcpy(d.model, id, sizeof id);
        }
        d.color_mul[0] = d.color_mul[1] = d.color_mul[2] = 255;
        geom_frame(&hdr, &d, emit_to_list, &l, &st);
        /* a cube seen from a corner: exactly 3 faces = 6 front triangles, 6 back triangles culled */
        ok = st.tris_in == 12 && st.tris_out == 6 && st.tris_culled == 6 && l.n == 7;
        report("local_geom", ok, "cube from a corner: %u in, %u out, %u culled, %u records", st.tris_in, st.tris_out,
               st.tris_culled, l.n);
        rl_free(&l);
        geom_reset();
    }
}

static int test_hello(void)
{
    const net_hello_reply *h = gpu_hello_info(S.g);
    net_status_reply a, b;
    double t0, t1, hz;
    int st, ok;
    uint32_t stat;

    ok = h->status == 0 && h->pl_id == GPU_ID_VALUE && h->pl_version == GPU_VERSION_VALUE && h->width == GPU_W &&
         h->height == GPU_H && h->proto_version == GPU_NET_PROTO_VER;
    report("hello", ok, "status %d, protocol %u, PL id 0x%08x version 0x%08x, %ux%u, pool %u KB, %u sprites",
           h->status, h->proto_version, h->pl_id, h->pl_version, h->width, h->height, h->pool_size / 1024u,
           h->max_sprites);
    if (!ok)
        return -1;
    st = gpu_status(S.g, &a);
    t0 = fgpu_now();
    if (st) {
        report("status", 0, "STATUS: %s", gerr(st));
        return -1;
    }
    fgpu_sleep_ms(500);
    st = gpu_status(S.g, &b);
    t1 = fgpu_now();
    if (st) {
        report("status", 0, "STATUS: %s", gerr(st));
        return -1;
    }
    stat = gpu_reg(&b, GPU_R_STATUS);
    hz = (uint32_t)(gpu_reg(&b, GPU_R_VSYNC_COUNT) - gpu_reg(&a, GPU_R_VSYNC_COUNT)) / (t1 - t0);
    ok = gpu_reg(&b, GPU_R_ID) == GPU_ID_VALUE && (stat & GPU_ST_MMCM_LOCKED) && hz > 50.0 && hz < 70.0;
    report("status", ok, "ID ok=%d, MMCM %s, monitor %s, vsync %.1f Hz, CONTROL 0x%x, FB0 0x%08x FB1 0x%08x, "
           "RET_ADDR 0x%08x", gpu_reg(&b, GPU_R_ID) == GPU_ID_VALUE, (stat & GPU_ST_MMCM_LOCKED) ? "locked" : "NOT LOCKED",
           (stat & GPU_ST_HPD) ? "present (HPD)" : "absent", hz, gpu_reg(&b, GPU_R_CONTROL), gpu_reg(&b, GPU_R_FB0),
           gpu_reg(&b, GPU_R_FB1), gpu_reg(&b, GPU_R_RET_ADDR));
    return 0;
}

/* error replies: the daemon rejects what it must reject and the library parses the error layouts */
static void test_protocol(void)
{
    uint32_t e1 = 99, e2 = 99;
    int a, b, c, d, s1, s2;
    net_sprite_upload_reply u;
    net_frame_get_reply fi;
    uint16_t px[8];

    memset(px, 0, sizeof px);
    gpu_sync(S.g, NULL);                                        /* zero the per-client counters */
    gpu_sync(S.obs, NULL);
    a = gpu_set_config(S.obs, GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, 0);  /* observer: not allowed */
    b = gpu_sprite_upload(S.g, 300, 4, 2, px, &u);              /* id >= 256 */
    c = gpu_frame_get(S.obs, 0, 3, 10, S.got, &fi);             /* scale 3: refused locally */
    d = gpu_rect(S.obs, 0, 0, 10, 10, 0xFFFFu, 0.0f, 0);        /* observer drawing: dropped, counted */
    s1 = gpu_sync(S.g, &e1);
    s2 = gpu_sync(S.obs, &e2);
    S.exp_daemon_errors += 3;
    report("protocol", a == GPU_ERR_PROTO && b == GPU_ERR_ARG && c == FGPU_ERR_ARG && d == 0 && s1 == 0 && s2 == 0 &&
           e1 == 1 && e2 == 2, "observer SET_CONFIG -> %d, SPRITE_UPLOAD id 300 -> %d, FRAME_GET scale 3 -> %d "
           "(local), SYNC error counts controller %u / observer %u (expect -1, -3, %d, 1 / 2)", a, b, c, e1, e2,
           FGPU_ERR_ARG);
}

static void test_bus_mode(void)
{
    t_hello_reply h1, h2;
    int a, b, c, d;
    memset(&h1, 0, sizeof h1);
    memset(&h2, 0, sizeof h2);
    a = teensy_bus_mode(S.t, 1);                /* force the pins high-Z */
    if (!a)
        a = teensy_hello(S.t, &h1);
    b = teensy_bus_mode(S.t, 7);                /* invalid */
    c = teensy_bus_mode(S.t, 0);                /* auto: driven again (FPGA still qualified) */
    d = c ? c : teensy_wait_ready(S.t, 3000, &h2);
    report("teensy_bus_mode", a == 0 && h1.bus_enabled == 0 && h1.fpga_ready == 1 && b == GPU_ERR_ARG && c == 0 &&
           d == 0 && h2.bus_enabled == 1, "T_BUS_MODE 1 -> bus_enabled %u (fpga_ready %u); mode 7 -> %d; mode 0 -> "
           "bus_enabled %u", h1.bus_enabled, h1.fpga_ready, b, h2.bus_enabled);
}

/* submit END(fno), wait until shown, READBACK, compare */
static void finish_ps_frame(const char *name, const reclist *l, uint16_t clear, const char *what)
{
    uint32_t f = fno_next(), errs = 0;
    char d[600];
    int st = gpu_end_frame(S.g, f);
    if (!st)
        st = gpu_wait_frame_no(S.g, f, 5000);
    if (!st)
        st = gpu_readback(S.g, S.got);
    if (!st)
        st = sync_errors(&errs);
    if (st) {
        report(name, 0, "%s", gerr(st));
        return;
    }
    S.exp_overflow += predict(l, clear, S.exp);
    if (frame_diff(name, S.exp, S.got, GPU_W, GPU_H, d, sizeof d) == 0 && errs == 0)
        report(name, 1, "%s: %u records, %s", what, l->n, d);
    else
        report(name, 0, "%s: %s%s", what, d, errs ? " + daemon rejected commands" : "");
}

static void test_ps(void)
{
    reclist l;
    net_tri *tr;
    uint32_t n = 400, i;
    int st;
    uint16_t *sp0, *sp1, *sp2;
    net_sprite_upload_reply u0, u1, u2;

    memset(&l, 0, sizeof l);
    st = set_mode(GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_A);
    if (st) {
        report("ps_tris", 0, "SET_CONFIG/RESET: %s", gerr(st));
        return;
    }
    /* 1: triangles (10% huge: guard-band clipping), rects with Z */
    tr = calloc(n, sizeof *tr);
    for (i = 0; i < n; i++)
        rand_tri(&tr[i], i % 10 == 0);
    st = gpu_tris(S.g, tr, n);
    predict_tris(&l, tr, n);
    if (!st)
        st = do_rect(&l, 100, 600, 500, 700, 0x07E0u, 0.25f, GPU_F_ZTEST | GPU_F_ZWRITE);
    if (!st)
        st = do_rect(&l, -50, -50, 60, 40, 0xFFFFu, 0.9f, GPU_F_ZTEST);
    if (!st)
        st = do_rect(&l, 1200, 650, 1400, 800, 0xF800u, 0.0f, 0);
    if (st) {
        report("ps_tris", 0, "%s", gerr(st));
        free(tr);
        rl_free(&l);
        return;
    }
    finish_ps_frame("ps_tris", &l, CLEAR_A, "400 NET_TRIS (40 guard-band clipped) + 3 NET_RECT");
    free(tr);
    l.n = 0;

    /* 2: sprites */
    sp0 = malloc(64 * 48 * 2);
    sp1 = malloc(1280 * 8 * 2);
    sp2 = malloc(4 * 1 * 2);
    make_sprite(sp0, 64, 48, 0);
    make_sprite(sp1, 1280, 8, 1);
    make_sprite(sp2, 4, 1, 2);
    st = upload(10, 64, 48, sp0, &u0);
    if (!st)
        st = upload(11, 1280, 8, sp1, &u1);
    if (!st)
        st = upload(12, 4, 1, sp2, &u2);
    if (!st)
        st = do_rect(&l, 0, 0, 1280, 720, 0x3186u, 0.5f, GPU_F_ZWRITE);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, 100, 100, 0);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, 200, 120, 1);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, -20, 300, 1);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, 400, -17, 0);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, 1256, 700, 1);
    if (!st)
        st = do_sprite(&l, 11, &u1, 1280, 8, 0, 356, 0);
    if (!st)
        st = do_sprite(&l, 12, &u2, 4, 1, 640, 360, 0);
    if (!st)
        st = do_sprite(&l, 10, &u0, 64, 48, -64, 10, 0);                 /* fully off-screen */
    if (!st)
        st = do_rect(&l, 180, 90, 300, 190, 0x001Fu, 0.4f, GPU_F_ZTEST);  /* over the sprites (Z untouched) */
    if (st)
        report("ps_sprites", 0, "%s", gerr(st));
    else
        finish_ps_frame("ps_sprites", &l, CLEAR_A, "3 uploads, 8 NET_SPRITE_DRAW (colour key, clipping)");
    l.n = 0;

    /* 3: raw records: TRI (noclip setup), SPRITE, rect, NOP */
    if (!st) {
        reclist raw;
        uint32_t rec[GPU_REC_WORDS];
        memset(&raw, 0, sizeof raw);
        for (i = 0; i < 150; i++) {
            net_tri t;
            gpu_vtx v[3];
            rand_tri(&t, 0);
            memcpy(v, t.v, sizeof v);
            if (gpu_setup_tri_noclip(v, t.flags, (int)t.cull, rec) == 1)
                rl_add(&raw, rec);
        }
        if (gpu_setup_sprite(600, 200, 64, 48, u0.ddr_addr, u0.stride, 1, KEY, rec) == 1)
            rl_add(&raw, rec);
        memset(rec, 0, sizeof rec);                 /* NOP */
        rl_add(&raw, rec);
        if (gpu_setup_rect(20, 20, 220, 60, 0xFD20u, 0.1f, GPU_F_ZTEST | GPU_F_ZWRITE, rec) == 1)
            rl_add(&raw, rec);
        st = gpu_records(S.g, (const uint32_t (*)[GPU_REC_WORDS])raw.r, raw.n);
        if (st)
            report("ps_records", 0, "%s", gerr(st));
        else
            finish_ps_frame("ps_records", &raw, CLEAR_A, "NET_RECORDS: TRI + SPRITE + NOP + rect");
        rl_free(&raw);
    }
    /* 4: list overflow (SPEC 7): 1600 one-record triangles -> the first 1536 drawn, 64 dropped */
    if (!st) {
        net_status_reply a, b;
        net_tri *t2 = calloc(1600, sizeof *t2);
        uint32_t f, errs = 0, drop;
        char d[600];
        l.n = 0;
        for (i = 0; i < 1600; i++) {
            float x = (float)((i % 50) * 25 + 8), y = (float)((i / 50) * 22 + 6);
            int j;
            for (j = 0; j < 3; j++) {
                t2[i].v[j].x = x + (j == 1 ? 18.0f : 0.0f) + 0.25f;
                t2[i].v[j].y = y + (j == 2 ? 15.0f : 0.0f) + 0.25f;
                t2[i].v[j].z = 0.5f;
                t2[i].v[j].r = (uint8_t)(i * 7u);
                t2[i].v[j].g = (uint8_t)(i * 3u + (unsigned)j * 90u);
                t2[i].v[j].b = (uint8_t)(255u - i);
            }
            t2[i].flags = GPU_F_ZTEST | GPU_F_ZWRITE;
            t2[i].cull = GPU_CULL_NONE;
        }
        predict_tris(&l, t2, 1600);
        f = fno_next();
        st = gpu_status(S.g, &a);
        if (!st)
            st = gpu_tris(S.g, t2, 1600);
        if (!st)
            st = gpu_end_frame(S.g, f);
        if (!st)
            st = gpu_wait_frame_no(S.g, f, 5000);
        if (!st)
            st = gpu_readback(S.g, S.got);
        if (!st)
            st = gpu_status(S.g, &b);
        if (!st)
            st = sync_errors(&errs);
        free(t2);
        if (st) {
            report("ps_overflow", 0, "%s", gerr(st));
        } else {
            uint32_t dov = gpu_reg(&b, GPU_R_LIST_OVERFLOW) - gpu_reg(&a, GPU_R_LIST_OVERFLOW);
            drop = predict(&l, CLEAR_A, S.exp);
            S.exp_overflow += drop;
            if (frame_diff("ps_overflow", S.exp, S.got, GPU_W, GPU_H, d, sizeof d) == 0 && dov == drop && drop == 64 &&
                !errs)
                report("ps_overflow", 1, "%u records -> %u drawn, LIST_OVERFLOW +%u: %s", l.n, l.n - drop, dov, d);
            else
                report("ps_overflow", 0, "%u records, LIST_OVERFLOW +%u (expected +%u): %s", l.n, dov, drop, d);
        }
    }
    free(sp0);
    free(sp1);
    free(sp2);
    rl_free(&l);
}

/* A PS scene that takes a few records; used by the FRAME_GET tests. */
static int submit_scene(reclist *l, int variant, uint32_t f)
{
    int st = 0, i;
    if (l->n == 0) {
        net_tri tr[120];
        for (i = 0; i < 120; i++)
            rand_tri(&tr[i], i % 12 == 0);
        predict_tris(l, tr, 120);
        if (variant) {
            uint32_t rec[GPU_REC_WORDS];
            if (gpu_setup_rect(0, 700, 1280, 720, 0xFFFFu, 0.0f, 0, rec) == 1)
                rl_add(l, rec);
        }
    }
    st = gpu_records(S.g, (const uint32_t (*)[GPU_REC_WORDS])l->r, l->n);
    if (!st)
        st = gpu_end_frame(S.g, f);
    return st;
}

static int wait_ret_enabled(void)
{
    int i;
    for (i = 0; i < 200; i++) {
        net_status_reply s;
        int st = gpu_status(S.g, &s);
        if (st)
            return st;
        if (gpu_reg(&s, GPU_R_RET_CTRL) & GPU_RET_ENABLE)
            return 0;
        fgpu_sleep_ms(5);
    }
    return GPU_ERR_TIMEOUT;
}

static void test_frame_get_one(const char *name, uint32_t scale, uint16_t clear, int variant)
{
    reclist l;
    uint32_t first = S.fno + 1, last = 0, k;
    int st, got = 0;
    net_frame_get_reply info;
    char d[600];

    memset(&l, 0, sizeof l);
    memset(&info, 0, sizeof info);
    st = gpu_set_config(S.g, GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, clear);
    if (!st)
        st = gpu_frame_get_send(S.obs, first, scale, 4000);
    if (st) {
        report(name, 0, "%s", st <= -100 && gpu_frame_get_pending(S.obs) == 0 ? oerr(st) : gerr(st));
        return;
    }
    st = wait_ret_enabled();        /* the daemon has parked the request and enabled capture */
    /* the same content under growing frame numbers until the observer gets one */
    for (k = 0; !st && k < 40 && !got; k++) {
        last = fno_next();
        st = submit_scene(&l, variant, last);
        if (!st)
            st = gpu_wait_frame_no(S.g, last, 3000);
        if (!st) {
            int r = gpu_frame_get_recv(S.obs, k < 2 ? 20 : 150, S.got, &info);
            if (r < 0) {
                report(name, 0, "observer: %s", oerr(r));
                rl_free(&l);
                return;
            }
            got = r;
        }
    }
    if (st) {
        report(name, 0, "%s", gerr(st));
        obs_drain();
        rl_free(&l);
        return;
    }
    if (!got) {
        int r = gpu_frame_get_recv(S.obs, 5000, S.got, &info);
        got = r == 1;
    }
    if (!got || info.status != 0) {
        report(name, 0, "no frame returned (%s)", got ? fgpu_strerror(info.status) : "no reply");
        rl_free(&l);
        return;
    }
    predict(&l, clear, S.exp);
    if (scale == 2)
        downscale2(S.exp, S.tmp);
    if ((int32_t)(info.frame_no - first) < 0 || (int32_t)(info.frame_no - last) > 0)
        report(name, 0, "returned frame_no %u outside the submitted %u..%u", info.frame_no, first, last);
    else if (frame_diff(name, scale == 2 ? S.tmp : S.exp, S.got, (int)info.w, (int)info.h, d, sizeof d) == 0)
        report(name, 1, "frame %u (submitted %u..%u) %ux%u: %s", info.frame_no, first, last, info.w, info.h, d);
    else
        report(name, 0, "frame %u: %s", info.frame_no, d);
    rl_free(&l);
}

static void test_frame_get(void)
{
    int st = set_mode(GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_A);
    if (st) {
        report("frame_get_s1", 0, "%s", gerr(st));
        return;
    }
    test_frame_get_one("frame_get_s1", 1, CLEAR_A, 0);
    test_frame_get_one("frame_get_s2", 2, CLEAR_B, 1);
}

/* ---- Teensy ---------------------------------------------------------------------------------- */
static void test_teensy_records(void)
{
    reclist tl, pl, all;
    uint32_t rec[GPU_REC_WORDS], f = fno_next(), sent = 0, i, errs = 0;
    int st;
    char d[600];

    memset(&tl, 0, sizeof tl);
    memset(&pl, 0, sizeof pl);
    memset(&all, 0, sizeof all);
    st = set_mode(GPU_CTL_SRC_TEENSY | GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_B);
    if (st) {
        report("teensy_records", 0, "%s", gerr(st));
        return;
    }
    for (i = 0; i < 300; i++) {
        net_tri t;
        rand_tri(&t, i % 15 == 0);
        predict_tris(&tl, &t, 1);
    }
    gpu_make_end(f, rec);
    rl_add(&tl, rec);
    /* PS part of the same list: drawn after the Teensy part */
    st = do_rect(&pl, 400, 300, 880, 420, 0xFFE0u, 0.5f, GPU_F_ZTEST);
    if (!st)
        st = gpu_end_frame(S.g, f);
    if (st) {
        report("teensy_records", 0, "%s", gerr(st));
        goto out;
    }
    st = teensy_records(S.t, (const uint32_t (*)[GPU_REC_WORDS])tl.r, tl.n, &sent);
    if (st || sent != tl.n) {
        report("teensy_records", 0, "T_RECORDS: %s, %u of %u records sent", terr(st), sent, tl.n);
        goto out;
    }
    st = gpu_wait_frame_no(S.g, f, 5000);
    if (!st)
        st = gpu_readback(S.g, S.got);
    if (!st)
        st = sync_errors(&errs);
    if (st) {
        report("teensy_records", 0, "%s", gerr(st));
        goto out;
    }
    for (i = 0; i + 1 < tl.n; i++)
        rl_add(&all, tl.r[i]);
    for (i = 0; i < pl.n; i++)
        rl_add(&all, pl.r[i]);
    S.exp_overflow += predict(&all, CLEAR_B, S.exp);
    if (frame_diff("teensy_records", S.exp, S.got, GPU_W, GPU_H, d, sizeof d) == 0 && !errs)
        report("teensy_records", 1, "%u records over the parallel bus + PS rect in one list: %s", tl.n, d);
    else
        report("teensy_records", 0, "%s", d);
out:
    rl_free(&tl);
    rl_free(&pl);
    rl_free(&all);
}

enum { M_CUBE = 1, M_SPHERE = 2, M_TORUS = 3 };

static int upload_meshes(void)
{
    int k, st = teensy_reset(S.t);
    if (st)
        return st;
    geom_reset();
    for (k = 1; k <= 3; k++) {
        st = teensy_mesh(S.t, (uint16_t)k, S.mesh[k].v, S.mesh[k].nverts, S.mesh[k].idx, S.mesh[k].nidx);
        if (st)
            return st;
        if (geom_mesh_upload((uint16_t)k, S.mesh[k].nverts, S.mesh[k].nidx, S.mesh[k].v, S.mesh[k].idx) != 0)
            return FGPU_ERR_ARG;
    }
    return 0;
}

static void model_at(float m[16], float x, float y, float z, float s, const float axis[3], double ang)
{
    float t[16], r[16], sc[16], tmp[16];
    m4_translate(t, x, y, z);
    m4_rotate(r, axis, ang);
    m4_scale(sc, s, s, s);
    m4_mul(tmp, r, sc);
    m4_mul(m, t, tmp);
}

static void set_draw(geom_draw *d, uint16_t mesh, uint16_t flags, const float model[16], int r, int g, int b)
{
    memset(d, 0, sizeof *d);
    d->mesh_id = mesh;
    d->flags = flags;
    memcpy(d->model, model, sizeof d->model);
    d->color_mul[0] = (uint8_t)r;
    d->color_mul[1] = (uint8_t)g;
    d->color_mul[2] = (uint8_t)b;
    d->color_mul[3] = 255;
}

#define LZC (GEOM_DRAW_LIGHTING | GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK)

/* 6 draws: lit meshes, an unculled torus, a huge cube crossing the near plane and the guard band
 * (clipping), a cube behind the camera (trivial reject), a sphere without Z test */
static int geom_scene(geom_frame_hdr *hdr, geom_draw *d, uint32_t f, double t)
{
    static const float ax1[3] = {0.3f, 1.0f, 0.2f}, ax2[3] = {1.0f, 0.0f, 0.4f}, ax3[3] = {0.0f, 1.0f, 0.0f};
    float view[16], proj[16], m[16], vp[16];
    memset(hdr, 0, sizeof *hdr);
    m4_look_at(view, v3_make(0.3f, 1.2f, 1.8f), v3_make(0.0f, -0.2f, -5.0f), v3_make(0, 1, 0));
    m4_perspective(proj, (float)(60.0 * GM_PI / 180.0), 16.0f / 9.0f, 0.3f, 60.0f);
    m4_mul(vp, proj, view);
    memcpy(hdr->viewproj, vp, sizeof vp);
    hdr->frame_no = f;
    hdr->light_dir[0] = 0.4082483f;
    hdr->light_dir[1] = -0.8164966f;
    hdr->light_dir[2] = -0.4082483f;
    hdr->ambient = 0.2f;
    model_at(m, 0.0f, 0.0f, -5.0f, 1.0f, ax1, 0.7 + t);
    set_draw(&d[0], M_CUBE, LZC, m, 255, 255, 255);
    model_at(m, -2.2f, 0.4f, -6.0f, 1.0f, ax3, 0.3 + 0.5 * t);
    set_draw(&d[1], M_SPHERE, LZC, m, 230, 255, 200);
    model_at(m, 2.3f, -0.2f, -5.5f, 1.0f, ax2, 1.1 + 0.8 * t);
    set_draw(&d[2], M_TORUS, LZC & ~GEOM_DRAW_CULL_BACK, m, 255, 220, 160);
    model_at(m, 0.0f, -6.6f, -3.0f, 6.0f, ax3, 0.2);
    set_draw(&d[3], M_CUBE, GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK, m, 120, 140, 160);
    model_at(m, 0.0f, 0.0f, 6.0f, 1.0f, ax3, 0.0);
    set_draw(&d[4], M_CUBE, LZC, m, 255, 255, 255);
    model_at(m, 1.0f, 1.5f, -8.0f, 0.5f, ax3, 0.0);
    set_draw(&d[5], M_SPHERE, GEOM_DRAW_LIGHTING, m, 255, 255, 255);
    hdr->ndraws = 6;
    return 6;
}

static void test_teensy_geom(void)
{
    geom_frame_hdr hdr;
    geom_draw d[8];
    geom_stats gs;
    reclist l;
    t_frame_reply fr;
    uint32_t f = fno_next(), errs = 0;
    int st, r;
    char dd[600];

    memset(&l, 0, sizeof l);
    st = set_mode(GPU_CTL_SRC_TEENSY | GPU_CTL_SCANOUT_EN, CLEAR_A);
    if (st) {
        report("teensy_geom", 0, "%s", gerr(st));
        return;
    }
    st = upload_meshes();
    if (st) {
        report("teensy_geom", 0, "T_RESET/T_MESH: %s", terr(st));
        return;
    }
    geom_scene(&hdr, d, f, 0.0);
    r = geom_frame(&hdr, d, emit_to_list, &l, &gs);
    st = teensy_frame(S.t, &hdr, d, &fr, NULL);
    if (st || r) {
        report("teensy_geom", 0, "T_FRAME: %s (local geom_frame %d)", terr(st), r);
        rl_free(&l);
        return;
    }
    if (fr.tris_in != gs.tris_in || fr.tris_out != gs.tris_out || fr.tris_culled != gs.tris_culled ||
        fr.tris_clipped != gs.tris_clipped || fr.frame_no != f) {
        report("teensy_geom", 0, "Teensy stats in/out/culled/clipped %u/%u/%u/%u, local %u/%u/%u/%u", fr.tris_in,
               fr.tris_out, fr.tris_culled, fr.tris_clipped, gs.tris_in, gs.tris_out, gs.tris_culled, gs.tris_clipped);
        rl_free(&l);
        return;
    }
    st = gpu_wait_frame_no(S.g, f, 5000);
    if (!st)
        st = gpu_readback(S.g, S.got);
    if (!st)
        st = sync_errors(&errs);
    if (st) {
        report("teensy_geom", 0, "%s", gerr(st));
        rl_free(&l);
        return;
    }
    S.exp_overflow += predict(&l, CLEAR_A, S.exp);
    if (frame_diff("teensy_geom", S.exp, S.got, GPU_W, GPU_H, dd, sizeof dd) == 0)
        report("teensy_geom", 1, "T_FRAME %u draws: %u tris in, %u out, %u culled, %u clipped (= local geom.c), "
               "%u us on the Teensy; frame %s", hdr.ndraws, fr.tris_in, fr.tris_out, fr.tris_culled, fr.tris_clipped,
               fr.us_total, dd);
    else
        report("teensy_geom", 0, "%s", dd);
    rl_free(&l);
}

static int same_records(const reclist *a, const uint32_t (*b)[GPU_REC_WORDS], uint32_t nb, char *why, size_t n)
{
    uint32_t i;
    if (a->n != nb) {
        snprintf(why, n, "%u records returned, %u predicted", nb, a->n);
        return 0;
    }
    for (i = 0; i < nb; i++)
        if (memcmp(a->r[i], b[i], GPU_REC_BYTES)) {
            int w;
            for (w = 0; w < GPU_REC_WORDS && a->r[i][w] == b[i][w]; w++)
                ;
            snprintf(why, n, "record %u word %d: returned 0x%08x, predicted 0x%08x", i, w, b[i][w], a->r[i][w]);
            return 0;
        }
    snprintf(why, n, "%u records identical", nb);
    return 1;
}

static void test_geom_return(void)
{
    geom_frame_hdr hdr;
    geom_draw d[8];
    geom_stats gs;
    reclist l;
    t_frame_reply fr;
    uint32_t (*recs)[GPU_REC_WORDS] = NULL;
    uint32_t f = fno_next();
    net_status_reply a, b;
    int st;
    char why[300];

    memset(&l, 0, sizeof l);
    /* 1: RETURN + bus */
    geom_scene(&hdr, d, f, 0.37);
    hdr.flags = GEOM_FRAME_RETURN;
    geom_frame(&hdr, d, emit_to_list, &l, &gs);
    st = teensy_frame(S.t, &hdr, d, &fr, &recs);
    if (st) {
        report("geom_return", 0, "T_FRAME: %s", terr(st));
        free(recs);
        rl_free(&l);
        return;
    }
    if (same_records(&l, (const uint32_t (*)[GPU_REC_WORDS])recs, fr.nrecs_returned, why, sizeof why)) {
        st = gpu_wait_frame_no(S.g, f, 5000);
        report("geom_return", st == 0, "GEOM_FRAME_RETURN: %s (TRI + END), frame also rendered: %s", why,
               st ? gerr(st) : "yes");
    } else {
        report("geom_return", 0, "GEOM_FRAME_RETURN: %s", why);
    }
    free(recs);
    recs = NULL;

    /* 2: RETURN + NO_BUS: records back, nothing on the bus */
    l.n = 0;
    f = fno_next();
    geom_scene(&hdr, d, f, 1.9);
    hdr.flags = GEOM_FRAME_RETURN | GEOM_FRAME_NO_BUS;
    geom_frame(&hdr, d, emit_to_list, &l, &gs);
    st = gpu_status(S.g, &a);
    if (!st)
        st = teensy_frame(S.t, &hdr, d, &fr, &recs);
    if (!st) {
        fgpu_sleep_ms(150);
        st = gpu_status(S.g, &b);
    }
    if (st) {
        report("geom_return_nobus", 0, "%s", st <= -100 ? "connection error" : fgpu_strerror(st));
    } else {
        int ok = same_records(&l, (const uint32_t (*)[GPU_REC_WORDS])recs, fr.nrecs_returned, why, sizeof why);
        uint32_t dw = gpu_reg(&b, GPU_R_T_WORDS) - gpu_reg(&a, GPU_R_T_WORDS);
        uint32_t dfc = gpu_reg(&b, GPU_R_FRAME_COUNT) - gpu_reg(&a, GPU_R_FRAME_COUNT);
        report("geom_return_nobus", ok && dw == 0 && dfc == 0 && fr.status == 0,
               "GEOM_FRAME_RETURN|NO_BUS: %s; bus words during the call %u, frames %u", why, dw, dfc);
    }
    free(recs);
    rl_free(&l);
}

/* ---- autonomous mode ------------------------------------------------------------------------ */
static void auto_scene(t_scene_hdr *sh, t_scene_obj *ob, int animated)
{
    static const float ax[3][3] = {{0.3f, 1.0f, 0.2f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.4f}};
    static const float pos[3][3] = {{0.0f, 0.0f, -5.0f}, {-2.2f, 0.4f, -6.0f}, {2.3f, -0.2f, -5.5f}};
    static const uint16_t mesh[3] = {M_CUBE, M_SPHERE, M_TORUS};
    float m[16], view[16], proj[16];
    int k;
    memset(sh, 0, sizeof *sh);
    sh->nobjs = 3;
    m4_look_at(view, v3_make(0.3f, 1.2f, 1.8f), v3_make(0.0f, -0.2f, -5.0f), v3_make(0, 1, 0));
    m4_perspective(proj, (float)(60.0 * GM_PI / 180.0), 16.0f / 9.0f, 0.3f, 60.0f);
    memcpy(sh->view, view, sizeof view);
    memcpy(sh->proj, proj, sizeof proj);
    sh->light_dir[0] = 0.4082483f;
    sh->light_dir[1] = -0.8164966f;
    sh->light_dir[2] = -0.4082483f;
    sh->ambient = 0.25f;
    sh->cam_orbit_rate = animated ? 0.4f : 0.0f;
    for (k = 0; k < 3; k++) {
        memset(&ob[k], 0, sizeof ob[k]);
        ob[k].mesh_id = mesh[k];
        ob[k].flags = LZC;
        model_at(m, pos[k][0], pos[k][1], pos[k][2], 1.0f, ax[k], 0.5 * k + 0.3);
        memcpy(ob[k].model, m, sizeof m);
        memcpy(ob[k].spin_axis, ax[k], sizeof ob[k].spin_axis);
        ob[k].spin_rate = animated ? 1.0f + 0.4f * (float)k : 0.0f;
        ob[k].color_mul[0] = ob[k].color_mul[1] = ob[k].color_mul[2] = ob[k].color_mul[3] = 255;
    }
}

/* the frame the firmware renders for a static scene (spin 0, orbit 0): SPEC 14 with t irrelevant */
static void auto_predict(const t_scene_hdr *sh, const t_scene_obj *ob, const uint32_t (*ovl)[GPU_REC_WORDS],
                         reclist *l)
{
    geom_frame_hdr hdr;
    geom_draw d[T_MAX_SCENE_OBJS];
    geom_stats gs;
    reclist tmp;
    uint32_t i;
    memset(&hdr, 0, sizeof hdr);
    memset(&tmp, 0, sizeof tmp);
    {
        float view[16], proj[16], vp[16];
        memcpy(view, sh->view, sizeof view);
        memcpy(proj, sh->proj, sizeof proj);
        m4_mul(vp, proj, view);                        /* viewproj = proj * view (firmware mat_mul) */
        memcpy(hdr.viewproj, vp, sizeof vp);
    }
    memcpy(hdr.light_dir, sh->light_dir, sizeof hdr.light_dir);
    hdr.ambient = sh->ambient;
    hdr.ndraws = (uint16_t)sh->nobjs;
    for (i = 0; i < sh->nobjs; i++) {
        memset(&d[i], 0, sizeof d[i]);
        d[i].mesh_id = ob[i].mesh_id;
        d[i].flags = ob[i].flags;
        memcpy(d[i].model, ob[i].model, sizeof d[i].model);
        memcpy(d[i].color_mul, ob[i].color_mul, 4);
    }
    geom_frame(&hdr, d, emit_to_list, &tmp, &gs);
    for (i = 0; i + 1 < tmp.n; i++)                    /* TRIs, then the overlays, (then END) */
        rl_add(l, tmp.r[i]);
    for (i = 0; i < sh->noverlay; i++)
        rl_add(l, ovl[i]);
    rl_free(&tmp);
}

static void test_auto(void)
{
    t_scene_hdr sh;
    t_scene_obj ob[3];
    uint32_t ovl[2][GPU_REC_WORDS];
    uint16_t *hud = malloc(128 * 32 * 2);
    net_sprite_upload_reply u;
    t_stats_reply ts0, ts1;
    net_status_reply a, b;
    net_frame_get_reply info;
    reclist l;
    int st, x, y;
    char d[600];
    double t0, t1;

    memset(&l, 0, sizeof l);
    st = set_mode(GPU_CTL_SRC_TEENSY | GPU_CTL_SCANOUT_EN, CLEAR_B);
    if (st) {
        report("auto_static", 0, "%s", gerr(st));
        free(hud);
        return;
    }
    for (y = 0; y < 32; y++)
        for (x = 0; x < 128; x++)
            hud[y * 128 + x] = (x < 2 || y < 2 || x > 125 || y > 29) ? 0xFFFFu : KEY;
    font_draw(hud, 128, 32, 8, 8, "AUTO", 2, 0xFFE0u, 0, 1);
    st = upload(20, 128, 32, hud, &u);
    free(hud);
    if (st) {
        report("auto_static", 0, "HUD upload: %s", gerr(st));
        return;
    }
    gpu_setup_sprite(24, 24, 128, 32, u.ddr_addr, u.stride, 1, KEY, ovl[0]);
    gpu_setup_rect(1100, 620, 1260, 700, 0x07FFu, 0.0f, 0, ovl[1]);
    auto_scene(&sh, ob, 0);
    sh.noverlay = 2;
    auto_predict(&sh, ob, (const uint32_t (*)[GPU_REC_WORDS])ovl, &l);
    predict(&l, CLEAR_B, S.exp);

    st = teensy_scene(S.t, &sh, ob, (const uint32_t (*)[GPU_REC_WORDS])ovl);
    if (!st)
        st = teensy_stats(S.t, &ts0);
    if (!st)
        st = teensy_auto(S.t, 1, 0);
    if (st) {
        report("auto_static", 0, "T_SCENE/T_STATS/T_AUTO: %s", terr(st));
        rl_free(&l);
        return;
    }
    /* a frame produced after the start (the Teensy numbers them 1, 2, ... since power-up) */
    st = gpu_frame_get(S.obs, ts0.auto_frames + 1u, 1, 4000, S.got, &info);
    if (st)
        report("auto_static", 0, "FRAME_GET: %s", oerr(st));
    else if (frame_diff("auto_static", S.exp, S.got, GPU_W, GPU_H, d, sizeof d) == 0)
        report("auto_static", 1, "autonomous frame %u (static scene, %u TRI + 2 overlay records) returned via "
               "FRAME_GET: %s", info.frame_no, l.n - 2, d);
    else
        report("auto_static", 0, "frame %u: %s", info.frame_no, d);
    rl_free(&l);

    /* frames keep coming */
    st = gpu_status(S.g, &a);
    t0 = fgpu_now();
    fgpu_sleep_ms(2100);            /* the T_STATS rates cover the firmware's last full 1 s window */
    if (!st)
        st = gpu_status(S.g, &b);
    t1 = fgpu_now();
    if (!st)
        st = teensy_stats(S.t, &ts1);
    if (st) {
        report("auto_frames", 0, "%s", st <= -100 ? "connection error" : fgpu_strerror(st));
    } else {
        uint32_t dfc = gpu_reg(&b, GPU_R_FRAME_COUNT) - gpu_reg(&a, GPU_R_FRAME_COUNT);
        report("auto_frames", ts1.auto_running == 1 && dfc >= 5 && ts1.auto_frames - ts0.auto_frames >= 5,
               "%.1f frames/s on screen, Teensy: running %u, %u frames since start, %.1f fps, %u tris/s, cpu %u%% "
               "at %u MHz", dfc / (t1 - t0), ts1.auto_running, ts1.auto_frames - ts0.auto_frames,
               ts1.auto_fps_x100 / 100.0, ts1.tris_per_sec, ts1.cpu_busy_pct, ts1.cpu_mhz);
    }

    /* T_FRAME is refused while autonomous mode runs */
    {
        geom_frame_hdr hdr;
        geom_draw dr[8];
        t_frame_reply fr;
        geom_scene(&hdr, dr, fno_next(), 0.0);
        st = teensy_frame(S.t, &hdr, dr, &fr, NULL);
        report("auto_busy_arg", st == GPU_ERR_ARG, "T_FRAME while running -> %s (expect GPU_ERR_ARG)", terr(st));
    }

    /* animated scene (T_SCENE while running): frames change */
    auto_scene(&sh, ob, 1);
    sh.noverlay = 2;
    st = teensy_scene(S.t, &sh, ob, (const uint32_t (*)[GPU_REC_WORDS])ovl);
    if (st) {
        report("auto_animates", 0, "T_SCENE while running: %s", terr(st));
    } else {
        net_frame_get_reply i1, i2;
        uint16_t *f1 = malloc((GPU_W / 2) * (GPU_H / 2) * 2);
        uint32_t now_frames;
        st = teensy_stats(S.t, &ts1);
        now_frames = ts1.auto_frames;
        if (!st)
            st = gpu_frame_get(S.obs, now_frames + 2u, 2, 3000, f1, &i1);
        if (!st)                    /* >= 10 frames later (a frame held in the capture buffer since
                                       just after i1 is too old and gets discarded) */
            st = gpu_frame_get(S.obs, i1.frame_no + 10u, 2, 3000, S.tmp, &i2);
        if (st || !f1) {
            report("auto_animates", 0, "FRAME_GET: %s", oerr(st));
        } else {
            long diff = 0, i;
            for (i = 0; i < (GPU_W / 2) * (GPU_H / 2); i++)
                diff += f1[i] != S.tmp[i];
            report("auto_animates", diff > 500, "frames %u and %u (640x360) differ in %ld pixels", i1.frame_no,
                   i2.frame_no, diff);
        }
        free(f1);
    }

    /* stop */
    st = teensy_auto(S.t, 0, 0);
    if (!st)
        st = teensy_stats(S.t, &ts1);
    if (!st) {
        fgpu_sleep_ms(100);
        st = gpu_status(S.g, &a);
    }
    if (!st) {
        fgpu_sleep_ms(300);
        st = gpu_status(S.g, &b);
    }
    if (st)
        report("auto_stop", 0, "%s", st <= -100 ? "connection error" : fgpu_strerror(st));
    else
        report("auto_stop", ts1.auto_running == 0 && gpu_reg(&a, GPU_R_FRAME_COUNT) == gpu_reg(&b, GPU_R_FRAME_COUNT),
               "T_AUTO 0: running %u, frames in 300 ms after stopping: %u", ts1.auto_running,
               gpu_reg(&b, GPU_R_FRAME_COUNT) - gpu_reg(&a, GPU_R_FRAME_COUNT));
    rebase_fno();
}

/* ---- throughput --------------------------------------------------------------------------- */
static void tput_ps(void)
{
    uint32_t n = 1400, i, k, K = (uint32_t)S.frames, last = 0;     /* < 1536 records per list */
    net_tri *tr = calloc(n, sizeof *tr);
    double t0, dt;
    int st;
    for (i = 0; i < n; i++) {
        rand_tri(&tr[i], 0);
        tr[i].cull = GPU_CULL_NONE;
    }
    st = set_mode(GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_A);
    t0 = fgpu_now();
    for (k = 0; !st && k < K; k++) {
        last = fno_next();
        st = gpu_tris(S.g, tr, n);
        if (!st)
            st = gpu_end_frame(S.g, last);
    }
    if (!st)
        st = gpu_wait_frame_no(S.g, last, 30000);
    dt = fgpu_now() - t0;
    free(tr);
    if (st)
        report("tput_ps", 0, "%s", gerr(st));
    else
        report("tput_ps", 1, "PS path: %u frames x %u NET_TRIS in %.3f s = %.1f frames/s, %.0f tris/s "
               "(Zynq setup + render + swap)", K, n, dt, K / dt, (double)K * n / dt);
}

static void tput_teensy(void)
{
    geom_frame_hdr hdr;
    geom_draw d[8];
    t_frame_reply fr;
    uint32_t k, K = (uint32_t)S.frames, last = 0;
    unsigned long long tris = 0, us = 0, usw = 0;
    double t0, dt;
    int st = set_mode(GPU_CTL_SRC_TEENSY | GPU_CTL_SCANOUT_EN, CLEAR_A);
    t0 = fgpu_now();
    for (k = 0; !st && k < K; k++) {
        last = fno_next();
        geom_scene(&hdr, d, last, k * 0.05);
        st = teensy_frame(S.t, &hdr, d, &fr, NULL);
        tris += fr.tris_out;
        us += fr.us_total;
        usw += fr.us_bus_wait;
    }
    if (!st)
        st = gpu_wait_frame_no(S.g, last, 30000);
    dt = fgpu_now() - t0;
    if (st)
        report("tput_teensy", 0, "%s", st <= -100 ? "connection error" : fgpu_strerror(st));
    else
        report("tput_teensy", 1, "Teensy path: %u frames in %.3f s = %.1f frames/s, %.0f tris/s drawn "
               "(%llu TRI records/frame); Teensy %.0f us/frame (of which %.0f us waiting for BUSY)", K, dt, K / dt,
               tris / dt, tris / K, (double)us / K, (double)usw / K);
}

/* return-path frame rate with an observer looping FRAME_GET(min = last + 1) */
static void tput_return(uint32_t scale, const char *name)
{
    double t0, dt, secs = S.return_secs;
    unsigned long frames = 0, timeouts = 0;
    uint32_t last = 0;
    int st = 0, use_auto = S.t != NULL, need_sync = 1;
    net_frame_get_reply info;
    t_stats_reply ts;

    if (use_auto) {                 /* producer: the Teensy in autonomous mode (animated) */
        t_scene_hdr sh;
        t_scene_obj ob[3];
        st = set_mode(GPU_CTL_SRC_TEENSY | GPU_CTL_SCANOUT_EN, CLEAR_B);
        auto_scene(&sh, ob, 1);
        if (!st)
            st = teensy_scene(S.t, &sh, ob, NULL);
        if (!st)
            st = teensy_stats(S.t, &ts);
        if (!st)
            st = teensy_auto(S.t, 1, 0);
        if (!st)
            last = ts.auto_frames;
    } else {                        /* producer: this program over the PS path */
        st = set_mode(GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_B);
        last = S.fno;
    }
    if (st) {
        report(name, 0, "setup: %s", st <= -100 ? "connection error" : fgpu_strerror(st));
        return;
    }
    t0 = fgpu_now();
    while ((dt = fgpu_now() - t0) < secs) {
        if (!use_auto) {
            /* one frame per request: park the FRAME_GET first (a frame is only captured if RET_ENABLE
             * is set when it starts rendering; after a served frame the daemon keeps it set for a
             * grace period, so the handshake is needed only at the start and after a timeout) */
            uint32_t f = fno_next();
            int r;
            st = gpu_frame_get_send(S.obs, f, scale, 1000);
            if (!st && need_sync)
                st = wait_ret_enabled();
            if (!st)
                st = gpu_rect(S.g, (int)(frames % 1200), 100, (int)(frames % 1200) + 80, 400, 0xFFFFu, 0.0f, 0);
            if (!st)
                st = gpu_end_frame(S.g, f);
            r = gpu_frame_get_recv(S.obs, -1, S.got, &info);
            if (st)
                break;
            if (r < 0) {
                st = r;
                break;
            }
            if (info.status == GPU_ERR_TIMEOUT) {
                timeouts++;
                need_sync = 1;
                continue;
            }
            if (info.status) {
                st = info.status;
                break;
            }
            need_sync = 0;
            last = info.frame_no;
            frames++;
            continue;
        }
        st = gpu_frame_get(S.obs, last + 1u, scale, 1000, S.got, &info);
        if (st == GPU_ERR_TIMEOUT) {
            timeouts++;
            st = 0;
            continue;
        }
        if (st)
            break;
        last = info.frame_no;
        frames++;
    }
    dt = fgpu_now() - t0;
    if (use_auto) {
        int s2 = teensy_auto(S.t, 0, 0);
        if (!st)
            st = s2;
        rebase_fno();
    }
    if (st)
        report(name, 0, "%s", st <= -100 ? oerr(st) : fgpu_strerror(st));
    else
        report(name, frames > 0, "return path scale %u (%ux%u): %lu frames in %.2f s = %.1f frames/s, %.1f MB/s "
               "(producer: %s; %lu idle timeouts)", scale, GPU_W / scale, GPU_H / scale, frames, dt, frames / dt,
               frames * (double)(GPU_W / scale) * (GPU_H / scale) * 2.0 / dt / 1e6,
               use_auto ? "Teensy autonomous mode" : "PS path", timeouts);
}

static void test_health(void)
{
    net_status_reply s;
    uint32_t errs = 0;
    int st = gpu_status(S.g, &s), ok;
    if (!st)
        st = sync_errors(&errs);
    if (st) {
        report("health", 0, "%s", gerr(st));
        return;
    }
    ok = gpu_reg(&s, GPU_R_AXI_ERRORS) == gpu_reg(&S.st0, GPU_R_AXI_ERRORS) &&
         gpu_reg(&s, GPU_R_BAD_RECORDS) == gpu_reg(&S.st0, GPU_R_BAD_RECORDS) &&
         gpu_reg(&s, GPU_R_LIST_OVERFLOW) - gpu_reg(&S.st0, GPU_R_LIST_OVERFLOW) == S.exp_overflow &&
         s.daemon_errors - S.st0.daemon_errors == S.exp_daemon_errors && errs == 0;
    {
        char tb[200] = "";
        if (S.t && S.have_ts0) {
            t_stats_reply ts;
            int r = teensy_stats(S.t, &ts);
            if (r) {
                ok = 0;
                snprintf(tb, sizeof tb, "; T_STATS: %s", terr(r));
            } else {
                ok = ok && ts.bus_timeouts == S.ts0.bus_timeouts && ts.usb_bad_msgs == S.ts0.usb_bad_msgs;
                snprintf(tb, sizeof tb, "; Teensy bus_timeouts +%u, usb_bad_msgs +%u", ts.bus_timeouts - S.ts0.bus_timeouts,
                         ts.usb_bad_msgs - S.ts0.usb_bad_msgs);
            }
        }
        report("health", ok, "during the run: AXI_ERRORS +%u, BAD_RECORDS +%u, LIST_OVERFLOW +%u (expected +%u), "
               "daemon errors +%u (expected +%u), DROPPED +%u%s", gpu_reg(&s, GPU_R_AXI_ERRORS) - gpu_reg(&S.st0, GPU_R_AXI_ERRORS),
               gpu_reg(&s, GPU_R_BAD_RECORDS) - gpu_reg(&S.st0, GPU_R_BAD_RECORDS),
               gpu_reg(&s, GPU_R_LIST_OVERFLOW) - gpu_reg(&S.st0, GPU_R_LIST_OVERFLOW), S.exp_overflow,
               s.daemon_errors - S.st0.daemon_errors, S.exp_daemon_errors, gpu_reg(&s, GPU_R_DROPPED) - gpu_reg(&S.st0, GPU_R_DROPPED), tb);
    }
}

/* ============================================================================================ */
static void usage(void)
{
    fprintf(stderr, "usage: gpu_selftest [options] [--quick] [--seed N] [--dump DIR] [--frames N] "
                    "[--return-seconds S] [--offline]\n%s", tool_common_help());
}

int main(int argc, char **argv)
{
    int i, offline = 0, tfail = 0, st;
    long v;
    double tstart = fgpu_now();

    tool_opts_init(&S.o, "auto");
    S.dump = ".";
    S.rng = 0x1234567u;
    S.frames = 60;
    S.return_secs = 2.0;
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&S.o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "--quick")) {
            S.quick = 1;
        } else if (!strcmp(argv[i], "--offline")) {
            offline = 1;
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 0xFFFFFFFFL, &v) < 0) {
                usage();
                return 2;
            }
            S.rng = (uint32_t)v;
        } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            S.dump = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 100000, &v) < 0) {
                usage();
                return 2;
            }
            S.frames = (int)v;
        } else if (!strcmp(argv[i], "--return-seconds") && i + 1 < argc) {
            if (tool_parse_double(argv[++i], 0.2, 3600, &S.return_secs) < 0) {
                usage();
                return 2;
            }
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    if (S.quick) {
        if (S.frames == 60)
            S.frames = 20;
        if (S.return_secs == 2.0)
            S.return_secs = 1.0;
    }
    printf("gpu_selftest: seed 0x%08x\n", S.rng);
    S.exp = malloc(GPU_FB_BYTES);
    S.got = malloc(GPU_FB_BYTES);
    S.tmp = malloc(GPU_FB_BYTES);
    if (!S.exp || !S.got || !S.tmp || mesh_cube(&S.mesh[M_CUBE], 1.0f, NULL) ||
        mesh_sphere(&S.mesh[M_SPHERE], 1.0f, 12, 18, NULL) || mesh_torus(&S.mesh[M_TORUS], 1.0f, 0.4f, 24, 12, NULL)) {
        fprintf(stderr, "gpu_selftest: out of memory\n");
        return 3;
    }
    test_local();
    if (offline)
        goto done;

    tool_catch_signals();
    S.t = tool_teensy(&S.o, 0, "gpu_selftest", &tfail);
    if (tfail) {
        report("teensy_open", 0, "cannot use the Teensy '%s' that was asked for", S.o.teensy);
        goto done;
    }
    if (S.t) {
        t_hello_reply th;
        memset(&th, 0, sizeof th);
        st = teensy_auto(S.t, 0, 0);    /* a demo may have left autonomous mode running */
        if (!st)
            st = teensy_wait_ready(S.t, 3000, &th);
        report("teensy_hello", st == 0, "%s: fw %u, meshes %u / verts %u / indices %u, fpga_ready %u, bus_enabled %u%s",
               teensy_device(S.t), th.fw_version, th.max_meshes, th.max_verts, th.max_indices, th.fpga_ready,
               th.bus_enabled, st ? " -- the FPGA does not accept the parallel bus (BUSY never low)" : "");
        if (st) {
            teensy_close(S.t);
            S.t = NULL;
        } else if (teensy_stats(S.t, &S.ts0) == 0) {
            S.have_ts0 = 1;
        }
    }
    S.g = tool_connect(&S.o, 0, "gpu_selftest");
    if (!S.g) {
        report("connect", 0, "no daemon at %s", tool_host_desc(&S.o));
        goto done;
    }
    S.obs = tool_connect(&S.o, 1, "gpu_selftest (observer)");
    if (!S.obs) {
        report("connect_observer", 0, "observer connection failed");
        goto done;
    }
    if (test_hello() < 0)
        goto done;
    st = gpu_status(S.g, &S.st0);
    if (!st)
        st = rebase_fno();
    if (st) {
        report("status", 0, "%s", gerr(st));
        goto done;
    }

    test_protocol();
    test_ps();
    test_frame_get();
    if (S.t) {
        test_bus_mode();
        test_teensy_records();
        test_teensy_geom();
        test_geom_return();
        test_auto();
    } else {
        skip("teensy_*", "no Teensy (--teensy none or none found): Teensy path, geometry, return and "
                         "autonomous tests not run");
    }
    tput_ps();
    if (S.t)
        tput_teensy();
    else
        skip("tput_teensy", "no Teensy");
    tput_return(1, "tput_return_s1");
    tput_return(2, "tput_return_s2");
    set_mode(GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, CLEAR_B);
    test_health();

done:
    obs_drain();
    printf("gpu_selftest: %d passed, %d failed, %d skipped in %.1f s -> %s\n", S.npass, S.nfail, S.nskip,
           fgpu_now() - tstart, S.nfail ? "FAIL" : "PASS");
    gpu_close(S.obs);
    gpu_close(S.g);
    teensy_close(S.t);
    ddr_forget();
    for (i = 1; i <= 3; i++)
        mesh_free(&S.mesh[i]);
    free(S.exp);
    free(S.got);
    free(S.tmp);
    return S.nfail ? 1 : 0;
}

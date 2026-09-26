/*
 * gpu_demo -- lit, rotating torus + cube + sphere with a HUD, on the FPGA-GPU.
 *
 *   gpu_demo [options] [--frames N] [--seconds S] [--view [--scale 1|2]] [--auto [--max-fps N]]
 *
 * Modes
 *   default           the Pi animates; the Teensy transforms, lights, clips, projects and sets up
 *                     the meshes (T_FRAME -> parallel bus); the Pi draws the HUD sprite over
 *                     Ethernet (NET_SPRITE_DRAW + NET_END_FRAME). CONTROL = SRC_TEENSY | SRC_PS.
 *   --teensy none     fallback without a Teensy: the Pi transforms and lights the meshes itself and
 *                     sends screen-space triangles (NET_TRIS; the Zynq ARM does the setup).
 *   --auto            T_SCENE + T_AUTO: the Teensy animates and renders on its own, flat out (or at
 *                     --max-fps); the Pi only re-uploads the HUD sprite (same id = same pool slot,
 *                     referenced by the scene's overlay SPRITE record). CONTROL = SRC_TEENSY.
 *   --view            also show the returned frames (NET_FRAME_GET, observer connection) in an X11
 *                     window on the Pi's desktop (viewer/x11view.c); closing it ends the demo.
 * Ctrl-C ends the demo (autonomous mode is stopped; the last frame stays on screen).
 */
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x8.h"
#include "gpu_math.h"
#include "mesh.h"
#include "tool_common.h"
#include "x11view.h"

#define HUD_W      512
#define HUD_H      64
#define HUD_X      16
#define HUD_Y      16
#define CLEAR_565  0x0886u               /* dark navy */
#define NOBJ       3

enum { MESH_TORUS = 1, MESH_CUBE = 2, MESH_SPHERE = 3 };

static mesh_t g_mesh[4];
static float g_view[16], g_proj[16];
static const float g_light[3] = {0.3638034f, -0.7795787f, -0.5093248f};   /* normalised (0.35,-0.75,-0.49) */
static const float g_ambient = 0.22f;

/* ---- scene --------------------------------------------------------------------------------- */
static void object_model(int k, double t, float m[16])
{
    float tr[16], r1[16], r2[16], tmp[16];
    static const float ax_t1[3] = {1, 0, 0}, ax_t2[3] = {0, 1, 0}, ax_c[3] = {0.57735f, 0.57735f, 0.57735f},
                       ax_s[3] = {0.2f, 1, 0};
    switch (k) {
    case 0:                             /* torus: tumbling */
        m4_translate(tr, -2.7f, 0.1f, 0.0f);
        m4_rotate(r1, ax_t2, 0.7 * t);
        m4_rotate(r2, ax_t1, 1.1 * t + 0.6);
        m4_mul(tmp, r1, r2);
        m4_mul(m, tr, tmp);
        break;
    case 1:                             /* cube: spinning about the diagonal */
        m4_translate(tr, 0.0f, 0.0f, 0.0f);
        m4_rotate(r1, ax_c, 0.9 * t + 0.4);
        m4_mul(m, tr, r1);
        break;
    default:                            /* sphere: bobbing, slowly turning */
        m4_translate(tr, 2.7f, (float)(0.35 * sin(1.3 * t)), 0.0f);
        m4_rotate(r1, ax_s, 0.5 * t);
        m4_mul(m, tr, r1);
        break;
    }
}

static const uint16_t obj_mesh[NOBJ] = {MESH_TORUS, MESH_CUBE, MESH_SPHERE};

static void make_draws(double t, geom_draw d[NOBJ])
{
    int k;
    for (k = 0; k < NOBJ; k++) {
        memset(&d[k], 0, sizeof d[k]);
        d[k].mesh_id = obj_mesh[k];
        d[k].flags = GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING;
        {
            float m[16];
            object_model(k, t, m);
            memcpy(d[k].model, m, sizeof m);
        }
        d[k].color_mul[0] = d[k].color_mul[1] = d[k].color_mul[2] = 255;
        d[k].color_mul[3] = 255;
    }
}

/* Detail is chosen so that the visible (front-facing) triangles plus the HUD stay well below the
 * PL's 1536 records per frame (SPEC 7): torus 1296 + sphere 1020 + cube 12 triangles, about half
 * of them back faces. */
static int build_meshes(void)
{
    if (mesh_torus(&g_mesh[MESH_TORUS], 1.0f, 0.42f, 36, 18, NULL) < 0 || mesh_cube(&g_mesh[MESH_CUBE], 0.85f, NULL) < 0 ||
        mesh_sphere(&g_mesh[MESH_SPHERE], 1.05f, 18, 30, NULL) < 0)
        return -1;
    return 0;
}

/* ---- Pi-side fallback geometry (no Teensy): transform + light + project -> NET_TRIS ---------- */
static uint32_t pi_geometry(double t, net_tri **buf, uint32_t *cap)
{
    float vp[16];
    uint32_t n = 0;
    int k;
    m4_mul(vp, g_proj, g_view);
    for (k = 0; k < NOBJ; k++) {
        const mesh_t *m = &g_mesh[obj_mesh[k]];
        float model[16], mvp[16];
        uint32_t i;
        object_model(k, t, model);
        m4_mul(mvp, vp, model);
        if (n + m->nidx / 3 > *cap) {
            uint32_t nc = (n + m->nidx / 3) * 2;
            net_tri *nb = realloc(*buf, nc * sizeof *nb);
            if (!nb)
                return n;
            *buf = nb;
            *cap = nc;
        }
        for (i = 0; i < m->nidx; i += 3) {
            net_tri *tr = &(*buf)[n];
            float clip[3][4];
            int j, behind = 0, out_l = 0, out_r = 0, out_b = 0, out_t = 0;
            for (j = 0; j < 3; j++) {
                const geom_vertex *v = &m->v[m->idx[i + j]];
                float nx, ny, nz, len, d, kk, c[3];
                int ch;
                m4_xform(mvp, v->px, v->py, v->pz, 1.0f, clip[j]);
                nx = (model[0] * v->nx + model[4] * v->ny) + model[8] * v->nz;
                ny = (model[1] * v->nx + model[5] * v->ny) + model[9] * v->nz;
                nz = (model[2] * v->nx + model[6] * v->ny) + model[10] * v->nz;
                len = sqrtf(nx * nx + ny * ny + nz * nz);
                if (len > 0) {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                }
                d = -((nx * g_light[0] + ny * g_light[1]) + nz * g_light[2]);
                if (d < 0)
                    d = 0;
                kk = g_ambient + (1.0f - g_ambient) * d;
                c[0] = v->r * kk;
                c[1] = v->g * kk;
                c[2] = v->b * kk;
                for (ch = 0; ch < 3; ch++)
                    c[ch] = c[ch] < 0 ? 0 : c[ch] > 255 ? 255 : c[ch];
                if (clip[j][3] < 0.05f)
                    behind = 1;
                out_l += clip[j][0] < -clip[j][3];
                out_r += clip[j][0] > clip[j][3];
                out_b += clip[j][1] < -clip[j][3];
                out_t += clip[j][1] > clip[j][3];
                if (!behind) {
                    float iw = 1.0f / clip[j][3];
                    tr->v[j].x = (clip[j][0] * iw * 0.5f + 0.5f) * (float)GPU_W;
                    tr->v[j].y = (0.5f - clip[j][1] * iw * 0.5f) * (float)GPU_H;
                    tr->v[j].z = clip[j][2] * iw * 0.5f + 0.5f;
                }
                tr->v[j].r = (uint8_t)(c[0] + 0.5f);
                tr->v[j].g = (uint8_t)(c[1] + 0.5f);
                tr->v[j].b = (uint8_t)(c[2] + 0.5f);
                tr->v[j].a = 255;
            }
            if (behind || out_l == 3 || out_r == 3 || out_b == 3 || out_t == 3)
                continue;               /* simple near-plane / frustum rejection (demo scene only) */
            tr->flags = GPU_F_ZTEST | GPU_F_ZWRITE;
            tr->cull = GPU_CULL_CW;
            n++;
        }
    }
    return n;
}

/* ---- HUD ------------------------------------------------------------------------------------ */
static void hud_render(uint16_t *px, const char *l1, const char *l2, const char *l3)
{
    int x, y;
    const uint16_t panel = 0x10A4u, border = 0x7BEFu, accent = 0xFE60u;
    for (y = 0; y < HUD_H; y++)
        for (x = 0; x < HUD_W; x++)
            px[y * HUD_W + x] = (x < 2 || y < 2 || x >= HUD_W - 2 || y >= HUD_H - 2) ? border : panel;
    for (y = 2; y < HUD_H - 2; y++)
        for (x = 2; x < 6; x++)
            px[y * HUD_W + x] = accent;
    font_draw(px, HUD_W, HUD_H, 14, 6, l1, 2, accent, panel, 1);
    font_draw(px, HUD_W, HUD_H, 14, 25, l2, 2, 0xFFFFu, panel, 1);
    font_draw(px, HUD_W, HUD_H, 14, 44, l3, 2, 0xB7FFu, panel, 1);
}

/* ---- in-process viewer (returned frames) ------------------------------------------------- */
typedef struct {
    gpu_conn *o;
    x11v     *v;
    int       scale, w, h, pending;
    uint16_t *px;
    uint32_t  next_min;
    unsigned long frames;
} viewer;

static int view_open(viewer *vw, tool_opts *opt, int scale)
{
    memset(vw, 0, sizeof *vw);
    vw->scale = scale;
    vw->w = GPU_W / scale;
    vw->h = GPU_H / scale;
    vw->px = malloc((size_t)vw->w * vw->h * 2u);
    if (!vw->px)
        return -1;
    vw->o = tool_connect(opt, 1, "gpu_demo (view)");
    if (!vw->o)
        return -1;
    vw->v = x11v_open(vw->w, vw->h, "gpu_demo - frames returned by the FPGA");
    if (!vw->v) {
        fprintf(stderr, "gpu_demo: cannot open an X11 window (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        return -1;
    }
    return 0;
}

/* 0 = ok, 1 = window closed, <0 = error */
static int view_service(viewer *vw, int wait_ms)
{
    int r;
    if (!vw->v)
        return 0;
    if (!vw->pending) {
        r = gpu_frame_get_send(vw->o, vw->next_min, (uint32_t)vw->scale, 500);
        if (r < 0)
            return r;
        vw->pending = 1;
    }
    if (x11v_poll(vw->v) != 0)
        return 1;
    for (;;) {
        net_frame_get_reply info;
        r = gpu_frame_get_recv(vw->o, wait_ms, vw->px, &info);
        if (r <= 0)
            return r;
        vw->pending = 0;
        if (info.status == 0) {
            if (x11v_put_rgb565(vw->v, vw->px, vw->w, vw->h) < 0)
                return -1;
            vw->next_min = info.frame_no + 1u;
            vw->frames++;
        } else if (info.status != GPU_ERR_TIMEOUT) {
            fprintf(stderr, "gpu_demo: FRAME_GET: %s\n", fgpu_strerror(info.status));
            return -1;
        }
        r = gpu_frame_get_send(vw->o, vw->next_min, (uint32_t)vw->scale, 500);
        if (r < 0)
            return r;
        vw->pending = 1;
        wait_ms = 0;
    }
}

static void view_close(viewer *vw)
{
    if (vw->v)
        x11v_close(vw->v);
    gpu_close(vw->o);
    free(vw->px);
}

/* ---- main ----------------------------------------------------------------------------------- */
static void usage(void)
{
    fprintf(stderr, "usage: gpu_demo [options] [--frames N] [--seconds S] [--view [--scale 1|2]] "
                    "[--auto [--max-fps N]]\n%s", tool_common_help());
}

#define CHECK(expr, what)                                                                             \
    do {                                                                                              \
        int r_ = (expr);                                                                              \
        if (r_ != 0) {                                                                                \
            fprintf(stderr, "gpu_demo: %s: %s%s%s\n", what, fgpu_strerror(r_), r_ <= -100 ? ": " : "", \
                    r_ <= -100 ? errsrc : "");                                                        \
            goto out;                                                                                 \
        }                                                                                             \
    } while (0)

int main(int argc, char **argv)
{
    tool_opts o;
    long max_frames = 0, scale = 1, max_fps = 0;
    double seconds = 0, t0, tstat, now;
    int i, view = 0, autonomous = 0, rc = 1, tfail = 0, hud_id = 1, k;
    gpu_conn *g = NULL;
    teensy_conn *t = NULL;
    viewer vw;
    uint16_t *hud = malloc(HUD_W * HUD_H * 2);
    net_tri *tris = NULL;
    uint32_t tris_cap = 0, fc0 = 0, fc_stat = 0, n = 0, auto_frames0 = 0;
    unsigned long long tris_in = 0, tris_out = 0, us_teensy = 0, us_wait = 0, tstat_frames = 0;
    double fps = 0;
    unsigned nupd = 0;
    char l1[64], l2[64], l3[64];
    const char *errsrc = "";
    net_status_reply s;
    net_sprite_upload_reply up;

    memset(&vw, 0, sizeof vw);
    tool_opts_init(&o, "auto");
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 2000000000L, &max_frames) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            if (tool_parse_double(argv[++i], 0.1, 1e7, &seconds) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--view")) {
            view = 1;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 2, &scale) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--auto")) {
            autonomous = 1;
        } else if (!strcmp(argv[i], "--max-fps") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 0, 100000, &max_fps) < 0) {
                usage();
                return 2;
            }
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    if (!hud || build_meshes() < 0) {
        fprintf(stderr, "gpu_demo: out of memory\n");
        return 1;
    }
    tool_catch_signals();
    m4_look_at(g_view, v3_make(0.0f, 1.7f, 6.8f), v3_make(0.0f, 0.0f, 0.0f), v3_make(0.0f, 1.0f, 0.0f));
    m4_perspective(g_proj, (float)(52.0 * GM_PI / 180.0), (float)GPU_W / (float)GPU_H, 0.5f, 40.0f);

    t = tool_teensy(&o, autonomous, "gpu_demo", &tfail);
    if (tfail || (autonomous && !t)) {
        if (autonomous && !tfail)
            fprintf(stderr, "gpu_demo: --auto needs a Teensy\n");
        return 1;
    }
    g = tool_connect(&o, 0, "gpu_demo");
    if (!g)
        goto out;
    errsrc = gpu_errmsg(g);
    if (t) {
        t_hello_reply th;
        errsrc = teensy_errmsg(t);
        CHECK(teensy_auto(t, 0, 0), "T_AUTO stop");
        for (k = 1; k <= 3; k++)
            CHECK(teensy_mesh(t, (uint16_t)k, g_mesh[k].v, g_mesh[k].nverts, g_mesh[k].idx, g_mesh[k].nidx), "T_MESH");
        memset(&th, 0, sizeof th);
        if (teensy_wait_ready(t, 3000, &th) != 0) {
            if (autonomous) {
                fprintf(stderr, "gpu_demo: warning: Teensy bus not ready (fpga_ready %u, bus_enabled %u): autonomous "
                                "mode idles until the FPGA drives BUSY low\n", th.fpga_ready, th.bus_enabled);
            } else {
                /* with SRC_TEENSY enabled the collector would wait forever for a Teensy END */
                fprintf(stderr, "gpu_demo: warning: Teensy bus not ready (fpga_ready %u, bus_enabled %u): the FPGA "
                                "does not accept the parallel bus -- using the Pi fallback (NET_TRIS)\n",
                        th.fpga_ready, th.bus_enabled);
                teensy_close(t);
                t = NULL;
            }
        }
    }
    if (!t) {
        printf("gpu_demo: no Teensy: the Pi transforms the meshes and sends NET_TRIS\n");
    }
    errsrc = gpu_errmsg(g);
    CHECK(gpu_set_config(g, (t ? GPU_CTL_SRC_TEENSY : 0u) | (autonomous ? 0u : GPU_CTL_SRC_PS) | GPU_CTL_SCANOUT_EN,
                         CLEAR_565), "SET_CONFIG");
    CHECK(gpu_reset(g), "RESET");           /* collector restarts with the new sources */
    CHECK(gpu_status(g, &s), "STATUS");
    fc0 = fc_stat = gpu_reg(&s, GPU_R_FRAME_COUNT);
    n = gpu_reg(&s, GPU_R_LAST_FRAME_NO);   /* frame numbers continue from what is on screen */

    snprintf(l1, sizeof l1, "%s", autonomous ? "FPGA-GPU  TEENSY AUTO" : t ? "FPGA-GPU  TEENSY 3D" : "FPGA-GPU  PI NET_TRIS");
    snprintf(l2, sizeof l2, "starting...");
    snprintf(l3, sizeof l3, " ");
    hud_render(hud, l1, l2, l3);
    CHECK(gpu_sprite_upload(g, 1, HUD_W, HUD_H, hud, &up), "SPRITE_UPLOAD");
    CHECK(gpu_sprite_upload(g, 2, HUD_W, HUD_H, hud, NULL), "SPRITE_UPLOAD");

    if (autonomous) {
        t_scene_hdr sh;
        t_scene_obj ob[NOBJ];
        uint32_t ovl[1][GPU_REC_WORDS];
        t_stats_reply ts;
        static const float axes[NOBJ][3] = {{0.8f, 0.6f, 0.0f}, {0.57735f, 0.57735f, 0.57735f}, {0.2f, 1.0f, 0.0f}};
        static const float rates[NOBJ] = {1.1f, 0.9f, 0.5f};
        memset(&sh, 0, sizeof sh);
        sh.nobjs = NOBJ;
        sh.noverlay = 1;
        memcpy(sh.view, g_view, sizeof sh.view);
        memcpy(sh.proj, g_proj, sizeof sh.proj);
        memcpy(sh.light_dir, g_light, sizeof sh.light_dir);
        sh.ambient = g_ambient;
        sh.cam_orbit_rate = 0.25f;
        for (k = 0; k < NOBJ; k++) {
            memset(&ob[k], 0, sizeof ob[k]);
            ob[k].mesh_id = obj_mesh[k];
            ob[k].flags = GEOM_DRAW_ZTEST | GEOM_DRAW_ZWRITE | GEOM_DRAW_CULL_BACK | GEOM_DRAW_LIGHTING;
            {
                float m[16];
                object_model(k, 0.0, m);
                memcpy(ob[k].model, m, sizeof m);
            }
            memcpy(ob[k].spin_axis, axes[k], sizeof ob[k].spin_axis);
            ob[k].spin_rate = rates[k];
            ob[k].color_mul[0] = ob[k].color_mul[1] = ob[k].color_mul[2] = ob[k].color_mul[3] = 255;
        }
        if (gpu_setup_sprite(HUD_X, HUD_Y, HUD_W, HUD_H, up.ddr_addr, up.stride, 0, 0, ovl[0]) != 1) {
            fprintf(stderr, "gpu_demo: HUD overlay record setup failed\n");
            goto out;
        }
        errsrc = teensy_errmsg(t);
        CHECK(teensy_scene(t, &sh, ob, (const uint32_t (*)[GPU_REC_WORDS])ovl), "T_SCENE");
        CHECK(teensy_stats(t, &ts), "T_STATS");
        auto_frames0 = ts.auto_frames;
        CHECK(teensy_auto(t, 1, (uint32_t)max_fps), "T_AUTO start");
        printf("gpu_demo: autonomous mode running (max_fps %ld); the Pi only updates the HUD\n", max_fps);
        n = auto_frames0;           /* the Teensy's frame numbers continue from its own counter */
    }
    if (view) {
        if (view_open(&vw, &o, (int)scale) < 0)
            goto out;
        vw.next_min = n + 1u;
    }

    t0 = tstat = fgpu_now();
    for (;;) {
        double tt;
        int vr;
        now = fgpu_now();
        tt = now - t0;
        if (tool_stop || (seconds > 0 && tt >= seconds))
            break;
        if (!autonomous && max_frames && (long)(n - gpu_reg(&s, GPU_R_LAST_FRAME_NO)) >= max_frames)
            break;

        if (now - tstat >= 0.25) {  /* statistics + HUD text, 4 times a second */
            net_status_reply s2;
            int print_now = (nupd++ & 3) == 0;     /* stdout once a second */
            errsrc = gpu_errmsg(g);
            CHECK(gpu_status(g, &s2), "STATUS");
            fps = (uint32_t)(gpu_reg(&s2, GPU_R_FRAME_COUNT) - fc_stat) / (now - tstat);
            fc_stat = gpu_reg(&s2, GPU_R_FRAME_COUNT);
            if (autonomous) {
                t_stats_reply ts;
                errsrc = teensy_errmsg(t);
                CHECK(teensy_stats(t, &ts), "T_STATS");
                snprintf(l2, sizeof l2, "%5.1f FPS  FRAME %u", fps, gpu_reg(&s2, GPU_R_LAST_FRAME_NO));
                snprintf(l3, sizeof l3, "%u TRIS/S  CPU %u%%", ts.tris_per_sec, ts.cpu_busy_pct);
                if (print_now)
                    printf("gpu_demo: %.1f fps on screen, Teensy %.1f fps, %u tris/s, cpu %u%%, frame %u\n", fps,
                           ts.auto_fps_x100 / 100.0, ts.tris_per_sec, ts.cpu_busy_pct,
                           gpu_reg(&s2, GPU_R_LAST_FRAME_NO));
                if (max_frames && (long)(ts.auto_frames - auto_frames0) >= max_frames)
                    break;
            } else {
                double nf = tstat_frames ? (double)tstat_frames : 1.0;
                snprintf(l2, sizeof l2, "%5.1f FPS  FRAME %u", fps, n);
                if (t)
                    snprintf(l3, sizeof l3, "%llu TRIS  %llu US/F", tris_out / (tstat_frames ? tstat_frames : 1),
                             us_teensy / (tstat_frames ? tstat_frames : 1));
                else
                    snprintf(l3, sizeof l3, "%llu TRIS SENT", tris_out / (tstat_frames ? tstat_frames : 1));
                if (print_now) {
                    printf("gpu_demo: %.1f fps, frame %u, %.0f tris in / %.0f out per frame%s", fps, n,
                           tris_in / nf, tris_out / nf, t ? "" : "\n");
                    if (t)
                        printf(", Teensy %.0f us/frame (bus wait %.0f us)\n", us_teensy / nf, us_wait / nf);
                }
                tris_in = tris_out = us_teensy = us_wait = tstat_frames = 0;
            }
            fflush(stdout);
            hud_render(hud, l1, l2, l3);
            errsrc = gpu_errmsg(g);
            if (autonomous) {
                CHECK(gpu_sprite_upload(g, 1, HUD_W, HUD_H, hud, NULL), "SPRITE_UPLOAD");
            } else {
                hud_id = 3 - hud_id;    /* alternate: never rewrite a sprite a queued frame reads */
                CHECK(gpu_sprite_upload(g, (uint16_t)hud_id, HUD_W, HUD_H, hud, NULL), "SPRITE_UPLOAD");
            }
            tstat = now;
        }

        if (autonomous) {
            vr = 0;
            if (vw.v)
                vr = view_service(&vw, 20);
            else
                fgpu_sleep_ms(20);
            if (vr != 0)
                break;
            continue;
        }

        /* one frame */
        n++;
        if (t) {
            geom_frame_hdr hdr;
            geom_draw d[NOBJ];
            t_frame_reply fr;
            float vp[16];
            memset(&hdr, 0, sizeof hdr);
            m4_mul(vp, g_proj, g_view);
            hdr.frame_no = n;
            memcpy(hdr.viewproj, vp, sizeof vp);
            memcpy(hdr.light_dir, g_light, sizeof hdr.light_dir);
            hdr.ambient = g_ambient;
            hdr.ndraws = NOBJ;
            make_draws(tt, d);
            errsrc = teensy_errmsg(t);
            CHECK(teensy_frame_send(t, &hdr, d), "T_FRAME");
            errsrc = gpu_errmsg(g);
            CHECK(gpu_sprite_draw(g, (uint16_t)hud_id, HUD_X, HUD_Y, 0, 0), "SPRITE_DRAW");
            CHECK(gpu_end_frame(g, n), "END_FRAME");
            errsrc = teensy_errmsg(t);
            {
                int r = teensy_frame_recv(t, &fr, NULL);
                if (r == GPU_ERR_BUS) {
                    /* the Teensy gave up mid-frame (BUSY high > 500 ms): its part of this list has no
                     * END, so the Teensy and PS list parts are out of step from here on */
                    fprintf(stderr, "gpu_demo: frame %u: the Teensy lost the FPGA bus (GPU_ERR_BUS: BUSY stayed high "
                                    "> 500 ms); stopping -- check the JM1 cable / bitstream, then restart\n", n);
                    goto out;
                }
                CHECK(r, "T_FRAME");
            }
            if (fr.tris_out + 1u > GPU_LIST_SLOTS)
                fprintf(stderr, "gpu_demo: frame %u has %u records: more than the %u the PL keeps per frame\n",
                        n, fr.tris_out + 1u, GPU_LIST_SLOTS);
            tris_in += fr.tris_in;
            tris_out += fr.tris_out;
            us_teensy += fr.us_total;
            us_wait += fr.us_bus_wait;
        } else {
            uint32_t nt = pi_geometry(tt, &tris, &tris_cap);
            errsrc = gpu_errmsg(g);
            CHECK(gpu_tris(g, tris, nt), "TRIS");
            CHECK(gpu_sprite_draw(g, (uint16_t)hud_id, HUD_X, HUD_Y, 0, 0), "SPRITE_DRAW");
            CHECK(gpu_end_frame(g, n), "END_FRAME");
            for (k = 0; k < NOBJ; k++)
                tris_in += g_mesh[obj_mesh[k]].nidx / 3;
            tris_out += nt;
        }
        tstat_frames++;
        /* pacing: at most one frame queued behind the one being shown */
        errsrc = gpu_errmsg(g);
        {
            uint32_t sent = n - gpu_reg(&s, GPU_R_LAST_FRAME_NO);
            if (sent >= 2)
                CHECK(gpu_wait_frame(g, fc0 + sent - 1u, 3000, NULL), "WAIT_FRAME");
        }
        if (vw.v) {
            vr = view_service(&vw, 0);
            if (vr != 0)
                break;
        }
    }
    rc = 0;
    {
        net_status_reply s2;
        uint32_t errs = 0;
        double el = fgpu_now() - t0;
        if (gpu_status(g, &s2) == 0)
            printf("gpu_demo: %u frames shown in %.2f s = %.1f fps%s", gpu_reg(&s2, GPU_R_FRAME_COUNT) - fc0, el,
                   (gpu_reg(&s2, GPU_R_FRAME_COUNT) - fc0) / el, vw.v ? "" : "\n");
        if (vw.v)
            printf(", %lu returned frames shown in the window (%.1f fps)\n", vw.frames, vw.frames / el);
        if (gpu_sync(g, &errs) == 0 && errs) {
            fprintf(stderr, "gpu_demo: the daemon rejected %u commands\n", errs);
            rc = 1;
        }
    }
out:
    if (t && autonomous)
        teensy_auto(t, 0, 0);
    if (vw.o || vw.v)
        view_close(&vw);
    gpu_close(g);
    teensy_close(t);
    for (k = 1; k <= 3; k++)
        mesh_free(&g_mesh[k]);
    free(tris);
    free(hud);
    return rc;
}

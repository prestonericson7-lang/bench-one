/*
 * gpu_image -- show a PPM image full screen on the FPGA-GPU's HDMI output.
 *
 *   gpu_image [options] IMAGE.ppm [--mode fit|fill|center] [--bg RRGGBB] [--no-dither]
 *
 * The image is scaled to 1280x720 (fit: whole image, letterboxed on --bg; fill: covers the
 * screen, cropped; center: 1:1, cropped), converted to RGB565 (4x4 ordered dither unless
 * --no-dither), uploaded as one 1280x720 sprite (NET_SPRITE_UPLOAD) and drawn as a frame
 * (NET_SPRITE_DRAW + NET_END_FRAME). The picture stays on screen after the tool exits, until the
 * next application draws. Takes control of the daemon (controller HELLO).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"
#include "tool_common.h"

static const uint8_t bayer4[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };

static uint16_t to565(const double c[3], int x, int y, int dither)
{
    double d = dither ? (bayer4[y & 3][x & 3] + 0.5) / 16.0 - 0.5 : 0.0;
    int r = (int)floor(c[0] * 31.0 / 255.0 + 0.5 + d), g = (int)floor(c[1] * 63.0 / 255.0 + 0.5 + d),
        b = (int)floor(c[2] * 31.0 / 255.0 + 0.5 + d);
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 63 ? 63 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void usage(void)
{
    fprintf(stderr, "usage: gpu_image [options] IMAGE.ppm [--mode fit|fill|center] [--bg RRGGBB] [--no-dither]\n%s",
            tool_common_help());
}

int main(int argc, char **argv)
{
    tool_opts o;
    const char *path = NULL, *mode = "fit";
    unsigned long bg = 0;
    int i, dither = 1, w, h, x, y, st, tfail = 0;
    uint8_t *rgb;
    uint16_t *canvas;
    char err[300];
    double s, dw, dh, ox, oy;
    gpu_conn *g;
    teensy_conn *t;
    net_sprite_upload_reply up;
    net_status_reply stat;
    uint32_t fno;
    double t0;

    tool_opts_init(&o, "none");
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            mode = argv[++i];
            if (strcmp(mode, "fit") && strcmp(mode, "fill") && strcmp(mode, "center")) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--bg") && i + 1 < argc) {
            char *end;
            bg = strtoul(argv[++i], &end, 16);
            if (*end || bg > 0xFFFFFFul) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--no-dither")) {
            dither = 0;
        } else if (argv[i][0] != '-' && !path) {
            path = argv[i];
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    if (!path) {
        usage();
        return 2;
    }
    if (img_read_ppm(path, &rgb, &w, &h, err, sizeof err) < 0) {
        fprintf(stderr, "gpu_image: %s\n", err);
        return 1;
    }
    canvas = malloc(GPU_FB_BYTES);
    if (!canvas) {
        fprintf(stderr, "gpu_image: out of memory\n");
        free(rgb);
        return 1;
    }
    if (!strcmp(mode, "center"))
        s = 1.0;
    else if (!strcmp(mode, "fit"))
        s = fmin((double)GPU_W / w, (double)GPU_H / h);
    else
        s = fmax((double)GPU_W / w, (double)GPU_H / h);
    dw = w * s;
    dh = h * s;
    ox = (GPU_W - dw) / 2.0;
    oy = (GPU_H - dh) / 2.0;
    for (y = 0; y < GPU_H; y++)
        for (x = 0; x < GPU_W; x++) {
            double c[3] = {(double)((bg >> 16) & 255u), (double)((bg >> 8) & 255u), (double)(bg & 255u)};
            /* destination pixel footprint in source coordinates */
            double sx0 = (x - ox) / s, sx1 = (x + 1 - ox) / s, sy0 = (y - oy) / s, sy1 = (y + 1 - oy) / s;
            if (sx1 > 0 && sy1 > 0 && sx0 < w && sy0 < h) {
                if (s >= 1.0) {                     /* magnify: nearest */
                    int ix = (int)floor((sx0 + sx1) * 0.5), iy = (int)floor((sy0 + sy1) * 0.5);
                    ix = ix < 0 ? 0 : ix >= w ? w - 1 : ix;
                    iy = iy < 0 ? 0 : iy >= h ? h - 1 : iy;
                    c[0] = rgb[((size_t)iy * w + ix) * 3];
                    c[1] = rgb[((size_t)iy * w + ix) * 3 + 1];
                    c[2] = rgb[((size_t)iy * w + ix) * 3 + 2];
                } else {                            /* minify: box average over the footprint */
                    int ax = (int)floor(sx0 < 0 ? 0 : sx0), bx = (int)ceil(sx1 > w ? w : sx1);
                    int ay = (int)floor(sy0 < 0 ? 0 : sy0), by = (int)ceil(sy1 > h ? h : sy1);
                    double acc[3] = {0, 0, 0};
                    int xx, yy, cnt = 0;
                    for (yy = ay; yy < by; yy++)
                        for (xx = ax; xx < bx; xx++) {
                            const uint8_t *p = rgb + ((size_t)yy * w + xx) * 3;
                            acc[0] += p[0];
                            acc[1] += p[1];
                            acc[2] += p[2];
                            cnt++;
                        }
                    if (cnt) {
                        c[0] = acc[0] / cnt;
                        c[1] = acc[1] / cnt;
                        c[2] = acc[2] / cnt;
                    }
                }
            }
            canvas[(size_t)y * GPU_W + x] = to565(c, x, y, dither);
        }
    free(rgb);

    g = tool_connect(&o, 0, "gpu_image");
    if (!g) {
        free(canvas);
        return 1;
    }
    t = tool_teensy(&o, 0, "gpu_image", &tfail);
    if (t) {                            /* a Teensy in autonomous mode would keep drawing: stop it */
        st = teensy_auto(t, 0, 0);
        if (st != 0)
            fprintf(stderr, "gpu_image: T_AUTO stop: %s\n", fgpu_strerror(st));
        teensy_close(t);
    }
    t0 = fgpu_now();
    st = gpu_set_config(g, GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN, 0);
    if (st == 0)
        st = gpu_sprite_upload(g, 0, GPU_W, GPU_H, canvas, &up);
    if (st == 0)
        st = gpu_status(g, &stat);
    fno = st == 0 ? gpu_reg(&stat, GPU_R_LAST_FRAME_NO) + 1u : 0;
    if (st == 0)
        st = gpu_sprite_draw(g, 0, 0, 0, 0, 0);
    if (st == 0)
        st = gpu_end_frame(g, fno);
    if (st == 0)
        st = gpu_wait_frame_no(g, fno, 5000);
    if (st != 0) {
        fprintf(stderr, "gpu_image: %s%s%s\n", fgpu_strerror(st), st <= -100 ? ": " : "", st <= -100 ? gpu_errmsg(g) : "");
        free(canvas);
        gpu_close(g);
        return 1;
    }
    {
        uint32_t errs = 0;
        gpu_sync(g, &errs);
        if (errs)
            fprintf(stderr, "gpu_image: the daemon reported %u rejected commands\n", errs);
    }
    printf("gpu_image: %s (%dx%d, %s) on screen as frame %u (sprite at 0x%08x), %.1f ms\n", path, w, h, mode, fno,
           up.ddr_addr, (fgpu_now() - t0) * 1e3);
    free(canvas);
    gpu_close(g);
    return 0;
}

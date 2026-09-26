/*
 * gpu_snap -- save what the FPGA-GPU shows (or renders next) as PNG or PPM. Observer connection.
 *
 *   gpu_snap [options] [-o FILE]          NET_READBACK of the front buffer (what is on screen now)
 *   gpu_snap [options] --frame-get [--scale 1|2] [-o FILE]
 *                                         NET_FRAME_GET: the next frame the PL finishes (two-way
 *                                         return path, SPEC 13), 1280x720 or 640x360
 * FILE ending in .png -> PNG, else PPM (default gpu_snap.png). --timeout-frame MS (3000) bounds
 * the wait for a new frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"
#include "tool_common.h"

static void usage(void)
{
    fprintf(stderr, "usage: gpu_snap [options] [-o FILE.png|FILE.ppm] [--frame-get] [--scale 1|2] "
                    "[--timeout-frame MS]\n%s", tool_common_help());
}

int main(int argc, char **argv)
{
    tool_opts o;
    gpu_conn *g;
    teensy_conn *t;
    const char *out = "gpu_snap.png";
    long scale = 1, ftimeout = 3000;
    int i, frame_get = 0, st, w, h, tfail = 0;
    uint16_t *px;
    uint32_t fno = 0;
    double t0;

    tool_opts_init(&o, "none");
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strcmp(argv[i], "--frame-get")) {
            frame_get = 1;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 2, &scale) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--timeout-frame") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 600000, &ftimeout) < 0) {
                usage();
                return 2;
            }
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    if (scale != 1 && !frame_get) {
        fprintf(stderr, "gpu_snap: --scale needs --frame-get (READBACK is always 1280x720)\n");
        return 2;
    }
    g = tool_connect(&o, 1, "gpu_snap");
    if (!g)
        return 1;
    t = tool_teensy(&o, 0, "gpu_snap", &tfail);     /* only reported, not needed */
    if (t)
        printf("teensy %s: fw %u fpga_ready %u bus_enabled %u\n", teensy_device(t), teensy_hello_info(t)->fw_version,
               teensy_hello_info(t)->fpga_ready, teensy_hello_info(t)->bus_enabled);
    teensy_close(t);
    px = malloc(GPU_FB_BYTES);
    if (!px) {
        fprintf(stderr, "gpu_snap: out of memory\n");
        gpu_close(g);
        return 1;
    }
    t0 = fgpu_now();
    if (!frame_get) {
        w = GPU_W;
        h = GPU_H;
        st = gpu_readback(g, px);
    } else {
        net_status_reply s;
        net_frame_get_reply info;
        st = gpu_status(g, &s);
        if (st == 0) {
            /* the next frame that finishes rendering after now */
            st = gpu_frame_get(g, gpu_reg(&s, GPU_R_LAST_FRAME_NO) + 1u, (uint32_t)scale, (uint32_t)ftimeout, px, &info);
            fno = info.frame_no;
        }
        w = GPU_W / (int)scale;
        h = GPU_H / (int)scale;
    }
    if (st != 0) {
        fprintf(stderr, "gpu_snap: %s failed: %s%s%s\n", frame_get ? "FRAME_GET" : "READBACK", fgpu_strerror(st),
                st <= -100 ? ": " : "", st <= -100 ? gpu_errmsg(g) : "");
        if (st == GPU_ERR_TIMEOUT)
            fprintf(stderr, "gpu_snap: no new frame was rendered within %ld ms (static screen? use READBACK)\n",
                    ftimeout);
        free(px);
        gpu_close(g);
        return 1;
    }
    if (img_write(out, px, w, h) < 0) {
        perror(out);
        free(px);
        gpu_close(g);
        return 1;
    }
    if (frame_get)
        printf("%s: frame %u, %dx%d, fetched in %.1f ms\n", out, fno, w, h, (fgpu_now() - t0) * 1e3);
    else
        printf("%s: front buffer %dx%d, fetched in %.1f ms\n", out, w, h, (fgpu_now() - t0) * 1e3);
    free(px);
    gpu_close(g);
    return 0;
}

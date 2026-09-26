/*
 * gpu_view -- show the frames the FPGA-GPU renders in a window on the Pi's own desktop
 * (SPEC 13.4). Observer connection: it never disturbs the application that draws.
 *
 *   gpu_view [options] [--scale 1|2] [--seconds S] [--frames N] [--quiet]
 *
 * Loop: NET_FRAME_GET(min_frame_no = last + 1) -> window (viewer/x11view.c: raw X11 protocol,
 * no libX11). The window first shows the current front buffer (NET_READBACK). The title shows
 * the frame rate; the window closes with its close button, Escape or q.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_common.h"
#include "x11view.h"

#define FG_TIMEOUT_MS 500

static void usage(void)
{
    fprintf(stderr, "usage: gpu_view [options] [--scale 1|2] [--seconds S] [--frames N] [--quiet]\n%s",
            tool_common_help());
}

int main(int argc, char **argv)
{
    tool_opts o;
    long scale = 1, max_frames = 0;
    double seconds = 0, t0, tlast;
    int i, w, h, quiet = 0, st, rc = 0, tfail = 0;
    gpu_conn *g;
    teensy_conn *t;
    x11v *v;
    uint16_t *px, *full;
    uint32_t last;
    unsigned long frames = 0, frames_win = 0, timeouts = 0;
    net_status_reply s;
    char title[160];

    tool_opts_init(&o, "none");
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 2, &scale) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            if (tool_parse_double(argv[++i], 0.1, 1e7, &seconds) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 2000000000L, &max_frames) < 0) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    tool_catch_signals();
    w = GPU_W / (int)scale;
    h = GPU_H / (int)scale;
    px = malloc((size_t)w * h * 2u);
    full = malloc(GPU_FB_BYTES);
    if (!px || !full) {
        fprintf(stderr, "gpu_view: out of memory\n");
        return 1;
    }
    g = tool_connect(&o, 1, "gpu_view");
    if (!g)
        return 1;
    t = tool_teensy(&o, 0, "gpu_view", &tfail);     /* accepted for uniformity; only reported */
    if (t)
        printf("gpu_view: teensy %s fw %u fpga_ready %u\n", teensy_device(t), teensy_hello_info(t)->fw_version,
               teensy_hello_info(t)->fpga_ready);
    teensy_close(t);
    snprintf(title, sizeof title, "gpu_view %.100s", o.host);
    v = x11v_open(w, h, title);
    if (!v) {
        fprintf(stderr, "gpu_view: cannot open an X11 window (DISPLAY=%s)\n", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        gpu_close(g);
        return 1;
    }
    /* what is on screen right now */
    st = gpu_readback(g, full);
    if (st == 0) {
        int x, y;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                px[y * w + x] = full[(size_t)(y * scale) * GPU_W + (size_t)(x * scale)];
        x11v_put_rgb565(v, px, w, h);
    }
    st = gpu_status(g, &s);
    if (st != 0) {
        fprintf(stderr, "gpu_view: STATUS: %s %s\n", fgpu_strerror(st), st <= -100 ? gpu_errmsg(g) : "");
        x11v_close(v);
        gpu_close(g);
        return 1;
    }
    last = gpu_reg(&s, GPU_R_LAST_FRAME_NO);
    t0 = tlast = fgpu_now();
    st = gpu_frame_get_send(g, last + 1u, (uint32_t)scale, FG_TIMEOUT_MS);
    while (st == 0 && !tool_stop) {
        struct pollfd p[2];
        double now;
        p[0].fd = x11v_fd(v);
        p[0].events = POLLIN;
        p[1].fd = gpu_fd(g);
        p[1].events = POLLIN;
        p[0].revents = p[1].revents = 0;
        poll(p, 2, 100);
        if (x11v_poll(v) != 0)
            break;                      /* window closed or X connection lost */
        if (p[1].revents) {
            net_frame_get_reply info;
            int r = gpu_frame_get_recv(g, 0, px, &info);
            if (r < 0) {
                fprintf(stderr, "gpu_view: FRAME_GET: %s: %s\n", fgpu_strerror(r), gpu_errmsg(g));
                rc = 1;
                break;
            }
            if (r == 1) {
                if (info.status == 0) {
                    if (x11v_put_rgb565(v, px, w, h) < 0) {
                        rc = 1;
                        break;
                    }
                    last = info.frame_no;
                    frames++;
                    frames_win++;
                } else if (info.status == GPU_ERR_TIMEOUT) {
                    timeouts++;         /* nothing new rendered: ask again */
                } else {
                    fprintf(stderr, "gpu_view: FRAME_GET status %s\n", fgpu_strerror(info.status));
                    rc = 1;
                    break;
                }
                if (max_frames && (long)frames >= max_frames)
                    break;
                st = gpu_frame_get_send(g, last + 1u, (uint32_t)scale, FG_TIMEOUT_MS);
            }
        }
        now = fgpu_now();
        if (now - tlast >= 1.0) {
            double fps = frames_win / (now - tlast);
            snprintf(title, sizeof title, "gpu_view %.100s  %.1f fps  frame %u  (%ldx scale)", o.host, fps, last, scale);
            x11v_set_title(v, title);
            if (!quiet) {
                printf("gpu_view: %.1f fps, frame %u, %lu frames total\n", fps, last, frames);
                fflush(stdout);
            }
            frames_win = 0;
            tlast = now;
        }
        if (seconds > 0 && now - t0 >= seconds)
            break;
    }
    if (st != 0) {
        fprintf(stderr, "gpu_view: FRAME_GET request failed: %s\n", gpu_errmsg(g));
        rc = 1;
    }
    {
        double el = fgpu_now() - t0;
        printf("gpu_view: %lu frames in %.2f s = %.1f fps (scale %ld, %lu idle timeouts)\n", frames, el,
               el > 0 ? frames / el : 0.0, scale, timeouts);
    }
    x11v_close(v);
    gpu_close(g);
    free(px);
    free(full);
    return rc;
}

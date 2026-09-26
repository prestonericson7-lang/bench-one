/*
 * test_view.c -- exercises x11view against the real X server in $DISPLAY.
 *
 *   test_view [seconds] [--partial SECS] [--size WxH] [--no-shm] [--no-bigreq] [--no-delta]
 *             [--no-verify] [--selftest] [--debug]
 *
 * 1. offline self-test of the pixel conversion / DISPLAY / .Xauthority code
 * 2. x11v_open (prints what the server offered and what was negotiated)
 * 3. bit-exact check: upload a frame, read the window back with GetImage, compare
 * 4. X error path: provoke one BadWindow and check it is reported with the right opcode/code
 * 5. phase "full": every pixel changes every frame (moving gradient + scrolling colour bars +
 *    bouncing square + binary frame counter) for `seconds` (default 5), fps printed every second
 *    and shown in the window title
 * 6. phase "partial" (default 2 s): static background, only the square and counter move
 *    (exercises the delta upload), then a bit-exact readback of the last frame
 * Exit status 0 = everything that could be checked passed.
 */
#define _POSIX_C_SOURCE 200809L

#include "x11view.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int tri(int t, int range)          /* triangle wave 0..range..0 */
{
    int p;
    if (range <= 0) return 0;
    p = t % (2 * range);
    return p < range ? p : 2 * range - p;
}

/* fbg animates the background (gradient + bars), fobj the square and the frame counter */
static void gen_pattern(uint16_t *px, int w, int h, unsigned fbg, unsigned fobj)
{
    static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
    static uint8_t *rcol;
    static int rcol_w;
    int bar_y = h * 3 / 4, x, y, sq = h / 8 < 16 ? 16 : h / 8, sx, sy, bit;
    unsigned shift = (fbg * 4u) % (unsigned)w, r_off = fbg / 2u;

    if (rcol_w != w) {
        free(rcol);
        rcol = (uint8_t *)malloc((size_t)w);
        if (!rcol) return;
        for (x = 0; x < w; x++) rcol[x] = (uint8_t)((x * 32) / w);
        rcol_w = w;
    }
    for (y = 0; y < bar_y; y++) {                          /* moving gradient */
        uint16_t *row = px + (size_t)y * (size_t)w;
        unsigned g6 = ((unsigned)(y * 64 / (bar_y ? bar_y : 1)) + fbg) & 63u;
        unsigned bb = (unsigned)y + fbg * 4u;
        for (x = 0; x < w; x++) {
            unsigned r5 = (rcol[x] + r_off) & 31u;
            unsigned b5 = ((unsigned)x + bb) >> 5 & 31u;
            row[x] = (uint16_t)(r5 << 11 | g6 << 5 | b5);
        }
    }
    if (bar_y < h) {                                       /* scrolling colour bars (rows identical) */
        uint16_t *row = px + (size_t)bar_y * (size_t)w;
        for (x = 0; x < w; x++) row[x] = bars[(((unsigned)x + shift) * 8u / (unsigned)w) & 7u];
        for (y = bar_y + 1; y < h; y++) memcpy(px + (size_t)y * (size_t)w, row, (size_t)w * 2u);
    }
    sx = tri((int)fobj * 7, w - sq);                       /* bouncing square */
    sy = tri((int)fobj * 5, h - sq);
    for (y = sy; y < sy + sq && y < h; y++)
        for (x = sx; x < sx + sq && x < w; x++)
            px[(size_t)y * (size_t)w + (size_t)x] =
                (y - sy < 3 || sy + sq - 1 - y < 3 || x - sx < 3 || sx + sq - 1 - x < 3) ? 0x0000 : 0xFFFF;
    for (bit = 0; bit < 16; bit++) {                       /* binary frame counter, top left */
        uint16_t c = (fobj >> (15 - bit)) & 1u ? 0xFFFF : 0x0000;
        for (y = 8; y < 24 && y < h; y++)
            for (x = 8 + bit * 20; x < 24 + bit * 20 && x < w; x++) px[(size_t)y * (size_t)w + (size_t)x] = c;
    }
}

static int mask_bits(uint32_t m)
{
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

/* allowed per-channel difference when the server has fewer bits than RGB565 (e.g. depth 15) */
static int chan_tol(int bits, int n) { return bits >= n ? 0 : 1 << (n - bits); }

/* sync, read back, compare. Returns 0 match, 1 mismatch, 2 readback unavailable, -1 error */
static int verify_frame(x11v *v, const uint16_t *px, uint16_t *back, int w, int h, const char *label)
{
    const x11v_info *in = x11v_get_info(v);
    int tr = chan_tol(mask_bits(in->red_mask), 5), tg = chan_tol(mask_bits(in->green_mask), 6),
        tb = chan_tol(mask_bits(in->blue_mask), 5);
    long bad = 0, first = -1, i;
    int r;
    if (x11v_sync(v) < 0) { printf("%s: sync failed\n", label); return -1; }
    r = x11v_read_rgb565(v, back, w, h);
    if (r > 0) { printf("%s: readback unavailable (GetImage X error %d; window off screen?) -- NOT verified\n", label, r); return 2; }
    if (r < 0) { printf("%s: readback failed (connection)\n", label); return -1; }
    for (i = 0; i < (long)w * h; i++) {
        int a = px[i], b = back[i], ok;
        if (!(tr | tg | tb)) ok = a == b;
        else {
            int dr = (a >> 11) - (b >> 11), dg = ((a >> 5) & 63) - ((b >> 5) & 63), db = (a & 31) - (b & 31);
            ok = dr <= tr && -dr <= tr && dg <= tg && -dg <= tg && db <= tb && -db <= tb;
        }
        if (!ok) { bad++; if (first < 0) first = i; }
    }
    if (bad) {
        printf("%s: MISMATCH %ld of %ld pixels (first at x=%ld y=%ld: sent 0x%04x, window has 0x%04x)\n", label, bad,
               (long)w * h, first % w, first / w, px[first], back[first]);
        return 1;
    }
    if (tr | tg | tb)
        printf("%s: window contents read back, all %ld pixels within the server's precision (%d/%d/%d bits)\n", label,
               (long)w * h, mask_bits(in->red_mask), mask_bits(in->green_mask), mask_bits(in->blue_mask));
    else
        printf("%s: window contents read back bit-exact (%dx%d, %ld pixels)\n", label, w, h, (long)w * h);
    return 0;
}

/* animate one phase; returns 0 ok, 1 window closed, -1 connection error */
static int run_phase(x11v *v, uint16_t *px, int W, int H, double secs, int partial, unsigned *fctr, const char *name)
{
    const x11v_info *in = x11v_get_info(v);
    x11v_info s0 = *in;
    double t_start, t_last, t_end, t_gen = 0.0, t_put = 0.0, el;
    unsigned frames = 0, last_frames = 0, bgf = *fctr;
    int ret = 0;
    unsigned long long npx;

    printf("phase \"%s\": %dx%d for %.1f s (%s)\n", name, W, H, secs,
           partial ? "static background, only the square and the counter move" : "every pixel changes every frame");
    t_start = t_last = now_s();
    while (now_s() - t_start < secs) {
        double a = now_s(), b, c;
        int pr;
        (*fctr)++;
        gen_pattern(px, W, H, partial ? bgf : *fctr, *fctr);
        b = now_s();
        if (x11v_put_rgb565(v, px, W, H) < 0) { printf("x11v_put_rgb565 FAILED at frame %u\n", frames); return -1; }
        c = now_s();
        t_gen += b - a; t_put += c - b;
        frames++;
        pr = x11v_poll(v);
        if (pr < 0) { printf("x11v_poll: connection error\n"); return -1; }
        if (pr == 1) { printf("window closed by the user\n"); ret = 1; break; }
        if (c - t_last >= 1.0) {
            char title[96];
            double fps = (double)(frames - last_frames) / (c - t_last);
            printf("  t=%4.1fs  frames=%5u  fps=%7.1f\n", c - t_start, frames, fps);
            snprintf(title, sizeof title, "x11view test - %s - %dx%d - %.1f fps", name, W, H, fps);
            x11v_set_title(v, title);
            last_frames = frames;
            t_last = c;
        }
    }
    if (x11v_sync(v) < 0) { printf("sync FAILED\n"); return -1; }   /* everything processed by the server */
    t_end = now_s();
    el = t_end - t_start;
    npx = in->pixels_sent - s0.pixels_sent;
    printf("  RESULT %s: %u frames in %.2f s = %.1f fps (server finished the last frame)\n", name, frames, el, frames / el);
    if (frames) {
        printf("    per frame: generate %.2f ms | put %.2f ms = convert %.2f + wait-for-server %.2f + socket write %.2f\n",
               t_gen * 1e3 / frames, t_put * 1e3 / frames, (in->ms_convert - s0.ms_convert) / frames,
               (in->ms_wait - s0.ms_wait) / frames, (in->ms_send - s0.ms_send) / frames);
        printf("    uploaded %.1f%% of the pixels (%.1f MB image data, %.1f MB/s), %llu image requests, %llu delta frames, %s\n",
               100.0 * (double)npx / ((double)frames * W * H), (double)npx * (in->bpp / 8) / 1e6,
               (double)npx * (in->bpp / 8) / 1e6 / el, in->requests - s0.requests, in->delta_frames - s0.delta_frames,
               in->shm ? "MIT-SHM" : "PutImage");
    }
    printf("    X errors: %u\n", in->x_errors - s0.x_errors);
    if (in->x_errors != s0.x_errors) return -1;
    return ret;
}

int main(int argc, char **argv)
{
    double secs = 5.0, psecs = 2.0, t0, t_open;
    int W = 1280, H = 720, do_verify = 1, selftest_only = 0, i, rc = 0, st, vr1 = -3, vr2 = -3, pr;
    unsigned fctr = 0;
    const char *scratch = ".x11v_selftest.xauth";
    const x11v_info *in;
    uint16_t *px, *back;
    x11v *v;
    unsigned err_before;

    setvbuf(stdout, NULL, _IOLBF, 0);            /* keep stdout and the library's stderr in order */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &W, &H) != 2 || W <= 0 || H <= 0) { fprintf(stderr, "bad --size\n"); return 2; }
        } else if (!strcmp(argv[i], "--partial") && i + 1 < argc) psecs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-shm")) setenv("X11V_NO_SHM", "1", 1);
        else if (!strcmp(argv[i], "--no-bigreq")) setenv("X11V_NO_BIGREQ", "1", 1);
        else if (!strcmp(argv[i], "--no-delta")) setenv("X11V_NO_DELTA", "1", 1);
        else if (!strcmp(argv[i], "--no-verify")) do_verify = 0;
        else if (!strcmp(argv[i], "--selftest")) selftest_only = 1;
        else if (!strcmp(argv[i], "--debug")) setenv("X11V_DEBUG", "1", 1);
        else if (!strcmp(argv[i], "--scratch") && i + 1 < argc) scratch = argv[++i];
        else if (argv[i][0] != '-' && atof(argv[i]) > 0) secs = atof(argv[i]);
        else {
            fprintf(stderr, "usage: %s [seconds] [--partial SECS] [--size WxH] [--no-shm] [--no-bigreq] [--no-delta]"
                            " [--no-verify] [--selftest] [--debug]\n", argv[0]);
            return 2;
        }
    }

    st = x11v_selftest(scratch);
    printf("selftest (11 pixel formats x all 65536 RGB565 values, byte layouts, DISPLAY parsing, .Xauthority): %s\n",
           st == 0 ? "PASS" : "FAIL");
    if (selftest_only) return st == 0 ? 0 : 1;
    if (st != 0) rc = 1;

    printf("DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    t0 = now_s();
    v = x11v_open(W, H, "x11view test");
    t_open = now_s() - t0;
    if (!v) { printf("x11v_open FAILED\n"); return 1; }
    in = x11v_get_info(v);
    printf("connected + window mapped in %.1f ms: \"%s\" release %u, X%d.%d, %s, auth %s\n", t_open * 1e3, in->vendor,
           (unsigned)in->release, in->protocol_major, in->protocol_minor,
           in->unix_socket ? "AF_UNIX socket" : "TCP", in->auth ? "MIT-MAGIC-COOKIE-1" : "none");
    printf("screen %d: %dx%d, root 0x%x, visual 0x%x class %d depth %d, %d bpp, pad %d, %s, masks r=%08x g=%08x b=%08x\n",
           in->screen, in->screen_w, in->screen_h, (unsigned)in->root, (unsigned)in->visual, in->visual_class,
           in->depth, in->bpp, in->scanline_pad, in->msb_first ? "MSBFirst" : "LSBFirst",
           (unsigned)in->red_mask, (unsigned)in->green_mask, (unsigned)in->blue_mask);
    printf("window 0x%x %dx%d mapped=%s, max request %u bytes (BIG-REQUESTS %s)\n", (unsigned)in->window, W, H,
           in->mapped ? "yes" : "NO", (unsigned)in->max_request_bytes, in->big_requests ? "on" : "off");

    px = (uint16_t *)malloc((size_t)W * (size_t)H * 2u);
    back = (uint16_t *)malloc((size_t)W * (size_t)H * 2u);
    if (!px || !back) { printf("out of memory\n"); x11v_close(v); return 1; }

    /* first frame + upload path + bit-exact readback */
    gen_pattern(px, W, H, 0, 0);
    if (x11v_put_rgb565(v, px, W, H) < 0) { printf("first x11v_put_rgb565 FAILED\n"); x11v_close(v); return 1; }
    printf("upload path: %s\n", in->shm ? "MIT-SHM (ShmPutImage, verified by a pixmap round trip)"
                                        : "PutImage over the socket (bands within the max request length)");
    if (do_verify) {
        vr1 = verify_frame(v, px, back, W, H, "verify frame 0");
        if (vr1 == 1 || vr1 < 0) rc = 1;
    }

    /* error path */
    err_before = in->x_errors;
    printf("provoking one X error (MapWindow on window None); expect code 3 BadWindow, opcode 8:\n");
    x11v_debug_provoke_error(v);
    if (x11v_sync(v) < 0) { printf("sync after provoked error FAILED\n"); rc = 1; }
    if (in->x_errors == err_before + 1 && in->last_error_code == 3 && in->last_error_major == 8)
        printf("error path: reported correctly, connection still usable\n");
    else { printf("error path: FAILED (errors %u -> %u, code %d opcode %d)\n", err_before, in->x_errors, in->last_error_code, in->last_error_major); rc = 1; }

    pr = run_phase(v, px, W, H, secs, 0, &fctr, "full");
    if (pr == 0 && psecs > 0) pr = run_phase(v, px, W, H, psecs, 1, &fctr, "partial");
    if (pr < 0) rc = 1;
    if (pr == 0 && do_verify) {
        vr2 = verify_frame(v, px, back, W, H, "verify last frame");
        if (vr2 == 1 || vr2 < 0) rc = 1;
    }
    printf("totals: %llu frames, %llu Expose redraws, %.1f MB written to the socket, %llu partial-write stalls, "
           "X errors %u (1 provoked)\n", in->frames, in->expose_redraws, (double)in->bytes_sent / 1e6,
           in->write_stalls, in->x_errors);
    x11v_close(v);
    free(px); free(back);
    printf("OVERALL: %s%s\n", rc == 0 ? "PASS" : "FAIL",
           (do_verify && (vr1 == 2 || vr2 == 2)) ? " (pixel readback could not be checked)" : "");
    return rc;
}

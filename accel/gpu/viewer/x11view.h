/*
 * x11view.h -- dependency-free X11 client for showing RGB565 frames in a window (SPEC section 13.4).
 *
 * Speaks the raw X11 protocol over the UNIX socket /tmp/.X11-unix/X<n> (or TCP 6000+n for
 * "host:n" displays). No libX11 / xcb: C99 + POSIX sockets only, static-linkable.
 *
 *   x11v *v = x11v_open(1280, 720, "gpu_view");
 *   while (running) {
 *       x11v_put_rgb565(v, pixels, 1280, 720);     // converts to the server's pixel format + uploads
 *       if (x11v_poll(v) != 0) break;              // 1 = window closed, <0 = connection lost
 *   }
 *   x11v_close(v);
 *
 * Environment:
 *   DISPLAY        [proto/][host]:display[.screen]  (":0", "unix:0", "localhost:10.0", "10.0.0.5:0")
 *   XAUTHORITY     cookie file (default ~/.Xauthority); MIT-MAGIC-COOKIE-1 entries only; no file = no auth
 *   X11V_NO_SHM=1      do not use the MIT-SHM fast path (local connections only anyway)
 *   X11V_NO_BIGREQ=1   do not enable BIG-REQUESTS (images then go in bands <= 256 KB)
 *   X11V_NO_DELTA=1    always upload whole frames (default: only the rectangles that changed since
 *                      the previous frame are sent over the socket; the result is identical)
 *   X11V_DEBUG=1       print connection/negotiation details to stderr
 *
 * All functions are for one thread at a time. Errors are printed to stderr prefixed "x11view:".
 */
#ifndef X11VIEW_H
#define X11VIEW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x11v x11v;

/* Connect via $DISPLAY, create + map a w x h window titled `title`. NULL on failure (reason on stderr). */
x11v *x11v_open(int w, int h, const char *title);

/* Show a w x h RGB565 image (row-major, no padding, native uint16) at the window's top-left.
 * Converts to the server's ZPixmap format (16/24/32 bpp, either image byte order) and uploads it
 * (MIT-SHM if usable, else PutImage split to the maximum request length; over the socket only
 * the rectangles that differ from the previous frame are sent). The frame is kept and redrawn on
 * Expose. Returns 0 on success, <0 on error (bad args, connection lost). Blocks while the server
 * is still busy with earlier frames, so the client never runs more than one frame ahead of what
 * the server has drawn (low latency; frame rates measured by the caller are real). */
int x11v_put_rgb565(x11v *v, const uint16_t *px, int w, int h);

/* Set the window title (WM_NAME + _NET_WM_NAME). 0 ok, <0 error. */
int x11v_set_title(x11v *v, const char *title);

/* Non-blocking: read and handle all pending events/errors/replies, redraw exposed areas.
 * Returns 1 once the window was closed (WM close button = WM_DELETE_WINDOW, window destroyed,
 * or Escape / 'q' pressed while close-on-keys is enabled), 0 otherwise, <0 on connection error. */
int x11v_poll(x11v *v);

/* Destroy the window and disconnect. NULL is ignored. */
void x11v_close(x11v *v);

/* ---- extras (diagnostics, tests, event-loop integration) ---------------------------------- */

typedef struct {
    char     vendor[64];
    uint32_t release;
    int      protocol_major, protocol_minor;
    int      unix_socket;           /* 1 = AF_UNIX, 0 = TCP */
    int      auth;                  /* 1 = connected with an MIT-MAGIC-COOKIE-1 cookie */
    int      display, screen;
    int      screen_w, screen_h;    /* root window size */
    uint32_t root, window, visual;
    int      visual_class;          /* 4 = TrueColor */
    int      depth, bpp, scanline_pad;
    int      msb_first;             /* image-byte-order: 0 LSBFirst, 1 MSBFirst */
    uint32_t red_mask, green_mask, blue_mask;
    uint32_t max_request_bytes;     /* after BIG-REQUESTS if enabled */
    int      big_requests;          /* BIG-REQUESTS enabled */
    int      shm;                   /* MIT-SHM in use for image uploads */
    int      mapped;                /* MapNotify seen (and no UnmapNotify since) */
    int      win_w, win_h;          /* last known window size */
    unsigned long long frames;      /* frames uploaded by x11v_put_rgb565 */
    unsigned long long requests;    /* image requests (bands) sent */
    unsigned long long bytes_sent;  /* bytes written to the socket (all requests) */
    unsigned long long write_stalls;/* times the socket was full mid-request (partial write, then poll) */
    unsigned long long expose_redraws;
    unsigned long long pixels_sent; /* image pixels uploaded (delta upload skips unchanged areas) */
    unsigned long long delta_frames;/* frames uploaded as changed-area rectangles only */
    double   ms_convert;            /* cumulative time converting RGB565 -> server format */
    double   ms_wait;               /* cumulative time waiting for the server to finish the previous frame */
    double   ms_send;               /* cumulative time writing image requests to the socket */
    unsigned x_errors;              /* X protocol errors received (excluding internal probes) */
    unsigned probe_errors;          /* expected errors from capability probes (e.g. MIT-SHM) */
    int      last_error_code, last_error_major, last_error_minor;
} x11v_info;

const x11v_info *x11v_get_info(x11v *v);

/* Round trip (GetInputFocus): all requests so far processed, events handled.
 * Returns the number of X errors seen so far (x11v_info.x_errors), or <0 on connection error. */
int x11v_sync(x11v *v);

/* Read the window contents back (GetImage, ZPixmap) into w x h RGB565. 0 ok, >0 X error code,
 * <0 connection/argument error. Intended for tests (the window must be mapped and on screen). */
int x11v_read_rgb565(x11v *v, uint16_t *out, int w, int h);

/* Socket descriptor, e.g. to poll() it together with other inputs (then call x11v_poll). */
int x11v_fd(x11v *v);

/* Enable/disable closing on Escape / 'q' (default enabled). */
void x11v_close_on_keys(x11v *v, int enable);

/* Test aid: sends a request that the server must reject (MapWindow on an invalid window id) so
 * the error path can be exercised. Returns 0 if queued. */
int x11v_debug_provoke_error(x11v *v);

/* Offline self-test of the pixel conversion (all 65536 RGB565 values through every supported
 * format/byte order and back), DISPLAY parsing and .Xauthority parsing (writes a temporary file
 * named `scratch_path`, NULL = skip). Returns 0 if everything passed; prints failures. */
int x11v_selftest(const char *scratch_path);

#ifdef __cplusplus
}
#endif

#endif /* X11VIEW_H */

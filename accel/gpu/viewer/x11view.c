/*
 * x11view.c -- dependency-free X11 client (raw protocol, C99 + POSIX). See x11view.h.
 *
 * Protocol notes (X Window System Protocol, X11R7):
 *  - We connect with byte order 'l', so every request/reply/event field is little-endian and is
 *    encoded/decoded explicitly (host endianness does not matter). Image data is different: it
 *    follows the server's image-byte-order from the setup reply, which we honour.
 *  - Socket is non-blocking. Writes loop over partial sends; while a write is blocked we keep
 *    reading (events/errors/replies are buffered), so neither side can deadlock.
 *  - Input is parsed in 32-byte units; replies and GenericEvents carry 4*length extra bytes.
 *  - Sequence numbers: every request increments v->seq; replies/errors carry the low 16 bits.
 *  - Image upload: MIT-SHM (verified with a pixmap round trip before use) or PutImage split
 *    into bands that fit the maximum request length (BIG-REQUESTS extended length if enabled;
 *    vertical strips if even one row is too long). After each frame a GetInputFocus marker is
 *    queued; the next frame waits for its reply, so at most one frame is in flight (low latency,
 *    and fps numbers are what the server showed).
 *  - Delta upload: the previous source frame is kept; only rectangles around changed rows/columns
 *    are sent (pixel-identical result, far less server work for mostly static frames).
 *  - Pixel conversion: 64K-entry LUT for any TrueColor format; SSE2/NEON fast path for the common
 *    x8r8g8b8 host-order case, plain copy for r5g6b5 host-order servers.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "x11view.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifndef X11V_NO_GETADDRINFO
#include <netdb.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* SIMD fast path for the common pixel format (compiler intrinsics only, little-endian hosts) */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(X11V_NO_SIMD)
#if defined(__SSE2__)
#include <emmintrin.h>
#define X11V_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define X11V_NEON 1
#endif
#endif

#define IO_TIMEOUT_MS      15000          /* no write progress for this long -> connection dead */
#define REPLY_TIMEOUT_MS   10000          /* max wait for a reply / frame marker */
#define CONNECT_TIMEOUT_MS 5000
#define MAX_IMAGE_REQ      (4u << 20)     /* cap on one image request, even with BIG-REQUESTS */
#define MAX_REPLY_BYTES    (256u << 20)   /* sanity limit on a reply's length field */
#define MAX_PRINTED_ERRORS 50
#define NWANT              16

/* core protocol opcodes used */
enum {
    OP_CreateWindow = 1, OP_DestroyWindow = 4, OP_MapWindow = 8, OP_InternAtom = 16,
    OP_ChangeProperty = 18, OP_GetInputFocus = 43, OP_CreatePixmap = 53, OP_FreePixmap = 54,
    OP_CreateGC = 55, OP_FreeGC = 60, OP_PutImage = 72, OP_GetImage = 73, OP_CreateColormap = 78,
    OP_FreeColormap = 79, OP_QueryExtension = 98, OP_GetKeyboardMapping = 101
};
/* predefined atoms */
enum { XA_ATOM = 4, XA_STRING = 31, XA_WM_NAME = 39, XA_WM_NORMAL_HINTS = 40, XA_WM_SIZE_HINTS = 41,
       XA_WM_CLASS = 67 };
/* events */
enum { EV_KeyPress = 2, EV_Expose = 12, EV_DestroyNotify = 17, EV_UnmapNotify = 18, EV_MapNotify = 19,
       EV_ConfigureNotify = 22, EV_ClientMessage = 33, EV_MappingNotify = 34, EV_GenericEvent = 35 };
#define MASK_KeyPress        0x00000001u
#define MASK_Exposure        0x00008000u
#define MASK_StructureNotify 0x00020000u

typedef struct {
    int      depth, bpp, pad;          /* pad = scanline pad in bits */
    int      msb;                      /* image byte order MSBFirst */
    uint32_t rmask, gmask, bmask;
    int      rshift, gshift, bshift, rbits, gbits, bbits;
    int      fast;                     /* 1 = x8r8g8b8 in host order (SIMD), 2 = r5g6b5 in host order (copy) */
    uint32_t *lut32;                   /* bpp 24/32: pixel bytes in server order, packed in memory order */
    uint16_t *lut16;                   /* bpp 16 */
} pixfmt;

typedef struct {
    int      active, done, err, quiet;
    uint16_t seq;
    uint8_t *buf;
    size_t   len;
} want_t;

struct x11v {
    int fd, dead, closed, destroyed, close_keys, debug, in_setup;
    uint8_t *out; size_t out_len, out_cap;
    uint8_t *in;  size_t in_len,  in_cap;
    uint32_t seq;
    uint32_t rid_base, rid_mask, rid_next; int rid_shift;
    uint32_t root, window, gc, colormap, visual, black_pixel;
    uint32_t max_req_words;
    pixfmt   fmt;
    int      min_kc, max_kc, keymap_stale;
    uint8_t  closekey[32];
    uint32_t a_wm_protocols, a_wm_delete, a_net_wm_name, a_utf8;
    int      bigreq_major, shm_major, shm_event, shm_error;
    /* last frame (socket path) */
    uint8_t *fb; size_t fb_size, fb_stride; int fb_w, fb_h;
    int      have_frame, frame_src;          /* frame_src 0 = fb, 1 = shm half shm_last */
    /* delta upload: previous source frame, per-row changed spans, packing buffer */
    int      delta;
    uint16_t *prev; size_t prev_cap;         /* pixels */
    int     *rx; int rx_cap;                 /* 2 ints per row: x0 (-1 = unchanged), x1 */
    uint8_t *stage; size_t stage_cap;
    unsigned err_at_put;
    int      delta_busy, delta_skip;         /* adaptive: skip the scan for full-motion content */
    /* pending Expose rows */
    int      dirty, dirty_y0, dirty_y1;
    /* one-frame-in-flight marker */
    int      sync_pending; uint16_t sync_seq;
    want_t   want[NWANT];
    /* quiet (expected) error range for capability probes */
    int      quiet_active; uint16_t quiet_lo, quiet_hi; unsigned quiet_hits; int quiet_code;
    /* MIT-SHM */
    int      shm_state;                      /* -1 disabled, 0 not set up, 1 active */
    int      shm_verified;
    uint8_t *shm_addr; size_t shm_half, shm_stride; uint32_t shm_seg; int shm_w, shm_h;
    int      shm_pending[2], shm_last;
    uint16_t shm_ring_seq[256];              /* outstanding ShmPutImage (send-event) requests, so an */
    uint8_t  shm_ring_half[256];             /* X error on one releases its half (no completion comes) */
    uint8_t  shm_ring_live[256];
    unsigned shm_ring_i;
    x11v_info info;
};

/* ------------------------------------------------------------------------------------------ */
/* small helpers                                                                               */
/* ------------------------------------------------------------------------------------------ */
static void put16(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static size_t pad4(size_t n) { return (n + 3u) & ~(size_t)3u; }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void xlog(const char *fmt, ...)
{
    va_list ap;
    fputs("x11view: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define DBG(v, ...) do { if ((v)->debug) xlog(__VA_ARGS__); } while (0)

static int env_true(const char *name)
{
    const char *s = getenv(name);
    return s && *s && strcmp(s, "0") != 0;
}

static int conn_fail(x11v *v, const char *what, int err)
{
    if (!v->dead) {
        v->dead = 1;
        if (err) xlog("connection to X server lost: %s: %s", what, strerror(err));
        else     xlog("connection to X server lost: %s", what);
    }
    return -1;
}

static const char *error_name(const x11v *v, unsigned code)
{
    static const char *names[] = { "Success", "BadRequest", "BadValue", "BadWindow", "BadPixmap",
        "BadAtom", "BadCursor", "BadFont", "BadMatch", "BadDrawable", "BadAccess", "BadAlloc",
        "BadColor", "BadGC", "BadIDChoice", "BadName", "BadLength", "BadImplementation" };
    if (code < sizeof names / sizeof names[0]) return names[code];
    if (v->shm_major && code == (unsigned)v->shm_error) return "BadShmSeg";
    return "extension error";
}

static const char *request_name(const x11v *v, unsigned major, unsigned minor)
{
    switch (major) {
    case OP_CreateWindow: return "CreateWindow";   case OP_DestroyWindow: return "DestroyWindow";
    case OP_MapWindow: return "MapWindow";         case OP_InternAtom: return "InternAtom";
    case OP_ChangeProperty: return "ChangeProperty"; case OP_GetInputFocus: return "GetInputFocus";
    case OP_CreatePixmap: return "CreatePixmap";   case OP_FreePixmap: return "FreePixmap";
    case OP_CreateGC: return "CreateGC";           case OP_FreeGC: return "FreeGC";
    case OP_PutImage: return "PutImage";           case OP_GetImage: return "GetImage";
    case OP_CreateColormap: return "CreateColormap"; case OP_FreeColormap: return "FreeColormap";
    case OP_QueryExtension: return "QueryExtension"; case OP_GetKeyboardMapping: return "GetKeyboardMapping";
    default: break;
    }
    if (v->bigreq_major && major == (unsigned)v->bigreq_major) return "BIG-REQUESTS:Enable";
    if (v->shm_major && major == (unsigned)v->shm_major) {
        switch (minor) {
        case 0: return "MIT-SHM:QueryVersion"; case 1: return "MIT-SHM:Attach";
        case 2: return "MIT-SHM:Detach";       case 3: return "MIT-SHM:PutImage";
        default: return "MIT-SHM:?";
        }
    }
    return major >= 128 ? "extension request" : "core request";
}

/* ------------------------------------------------------------------------------------------ */
/* pixel formats                                                                               */
/* ------------------------------------------------------------------------------------------ */
static void mask_info(uint32_t m, int *shift, int *bits)
{
    int s = 0, b = 0;
    if (m) {
        while (!(m & 1u)) { m >>= 1; s++; }
        while (m & 1u) { m >>= 1; b++; }
    }
    *shift = s; *bits = b;
}

/* 8-bit channel -> `bits`-bit channel, rounded */
static uint32_t scale8(uint32_t c8, int bits)
{
    uint64_t maxv;
    if (bits <= 0) return 0;
    maxv = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1u);
    return (uint32_t)(((uint64_t)c8 * maxv + 127u) / 255u);
}

/* `bits`-bit channel -> n-bit channel (n = 5 or 6), rounded */
static uint32_t scale_to(uint32_t c, int bits, int n)
{
    uint64_t maxv, maxn = (1ull << n) - 1u;
    if (bits <= 0) return 0;
    maxv = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1u);
    return (uint32_t)(((uint64_t)c * maxn + maxv / 2u) / maxv);
}

static void fmt_free(pixfmt *f)
{
    free(f->lut32); free(f->lut16);
    f->lut32 = NULL; f->lut16 = NULL;
}

static uint32_t rgb565_to_pixel(const pixfmt *f, uint32_t p)
{
    uint32_t r5 = (p >> 11) & 31u, g6 = (p >> 5) & 63u, b5 = p & 31u;
    uint32_t r8 = (r5 << 3) | (r5 >> 2), g8 = (g6 << 2) | (g6 >> 4), b8 = (b5 << 3) | (b5 >> 2);
    return ((scale8(r8, f->rbits) << f->rshift) & f->rmask) |
           ((scale8(g8, f->gbits) << f->gshift) & f->gmask) |
           ((scale8(b8, f->bbits) << f->bshift) & f->bmask);
}

static int fmt_init(pixfmt *f, int depth, int bpp, int pad, int msb, uint32_t rm, uint32_t gm, uint32_t bm)
{
    uint32_t p;
    memset(f, 0, sizeof *f);
    if (bpp != 16 && bpp != 24 && bpp != 32) return -1;
    if (pad != 8 && pad != 16 && pad != 32 && pad != 64) return -1;
    if (!rm || !gm || !bm) return -1;
    f->depth = depth; f->bpp = bpp; f->pad = pad; f->msb = msb;
    f->rmask = rm; f->gmask = gm; f->bmask = bm;
    mask_info(rm, &f->rshift, &f->rbits);
    mask_info(gm, &f->gshift, &f->gbits);
    mask_info(bm, &f->bshift, &f->bbits);
    if (bpp == 16) {
        f->lut16 = (uint16_t *)malloc(65536u * sizeof(uint16_t));
        if (!f->lut16) return -1;
    } else {
        f->lut32 = (uint32_t *)malloc(65536u * sizeof(uint32_t));
        if (!f->lut32) return -1;
    }
    for (p = 0; p < 65536u; p++) {
        uint32_t val = rgb565_to_pixel(f, p);
        uint8_t b[4] = { 0, 0, 0, 0 };
        if (bpp == 32) {
            if (msb) { b[0] = (uint8_t)(val >> 24); b[1] = (uint8_t)(val >> 16); b[2] = (uint8_t)(val >> 8); b[3] = (uint8_t)val; }
            else     { b[0] = (uint8_t)val; b[1] = (uint8_t)(val >> 8); b[2] = (uint8_t)(val >> 16); b[3] = (uint8_t)(val >> 24); }
            memcpy(&f->lut32[p], b, 4);
        } else if (bpp == 24) {
            if (msb) { b[0] = (uint8_t)(val >> 16); b[1] = (uint8_t)(val >> 8); b[2] = (uint8_t)val; }
            else     { b[0] = (uint8_t)val; b[1] = (uint8_t)(val >> 8); b[2] = (uint8_t)(val >> 16); }
            memcpy(&f->lut32[p], b, 4);
        } else {
            if (msb) { b[0] = (uint8_t)(val >> 8); b[1] = (uint8_t)val; }
            else     { b[0] = (uint8_t)val; b[1] = (uint8_t)(val >> 8); }
            memcpy(&f->lut16[p], b, 2);
        }
    }
    {
        const uint16_t one = 1;
        int host_msb = *(const uint8_t *)&one == 0;
        if (msb == host_msb && bpp == 32 && rm == 0xFF0000u && gm == 0xFF00u && bm == 0xFFu) f->fast = 1;
        if (msb == host_msb && bpp == 16 && rm == 0xF800u && gm == 0x07E0u && bm == 0x001Fu) f->fast = 2;
    }
    return 0;
}

/* RGB565 -> 0x00RRGGBB with 5/6-bit expansion by bit replication (same values as the LUT) */
static void conv_x888(const uint16_t *s, uint32_t *d, int n)
{
    int i = 0;
#if defined(X11V_SSE2)
    const __m128i mF8 = _mm_set1_epi16(0xF8), mFC = _mm_set1_epi16(0xFC), m3 = _mm_set1_epi16(3), m7 = _mm_set1_epi16(7);
    for (; i + 8 <= n; i += 8) {
        __m128i p = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
        __m128i r = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(p, 8), mF8), _mm_srli_epi16(p, 13));
        __m128i g = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(p, 3), mFC), _mm_and_si128(_mm_srli_epi16(p, 9), m3));
        __m128i b = _mm_or_si128(_mm_and_si128(_mm_slli_epi16(p, 3), mF8), _mm_and_si128(_mm_srli_epi16(p, 2), m7));
        __m128i lo = _mm_or_si128(_mm_slli_epi16(g, 8), b);              /* g8:b8 */
        _mm_storeu_si128((__m128i *)(void *)(d + i), _mm_unpacklo_epi16(lo, r));
        _mm_storeu_si128((__m128i *)(void *)(d + i + 4), _mm_unpackhi_epi16(lo, r));
    }
#elif defined(X11V_NEON)
    const uint16x8_t mF8 = vdupq_n_u16(0xF8), mFC = vdupq_n_u16(0xFC), m3 = vdupq_n_u16(3), m7 = vdupq_n_u16(7);
    for (; i + 8 <= n; i += 8) {
        uint16x8_t p = vld1q_u16(s + i);
        uint16x8_t r = vorrq_u16(vandq_u16(vshrq_n_u16(p, 8), mF8), vshrq_n_u16(p, 13));
        uint16x8_t g = vorrq_u16(vandq_u16(vshrq_n_u16(p, 3), mFC), vandq_u16(vshrq_n_u16(p, 9), m3));
        uint16x8_t b = vorrq_u16(vandq_u16(vshlq_n_u16(p, 3), mF8), vandq_u16(vshrq_n_u16(p, 2), m7));
        uint16x8_t lo = vorrq_u16(vshlq_n_u16(g, 8), b);
        uint16x8x2_t z = vzipq_u16(lo, r);
        vst1q_u32(d + i, vreinterpretq_u32_u16(z.val[0]));
        vst1q_u32(d + i + 4, vreinterpretq_u32_u16(z.val[1]));
    }
#endif
    for (; i < n; i++) {
        uint32_t p = s[i];
        uint32_t r = (p >> 8 & 0xF8u) | (p >> 13), g = (p >> 3 & 0xFCu) | (p >> 9 & 3u), b = (p << 3 & 0xF8u) | (p >> 2 & 7u);
        d[i] = r << 16 | g << 8 | b;
    }
}

static size_t row_stride(const pixfmt *f, int w)
{
    size_t bits = (size_t)w * (size_t)f->bpp, pad = (size_t)f->pad;
    return ((bits + pad - 1u) / pad) * pad / 8u;
}

/* n RGB565 pixels -> server pixels at d (d aligned to the pixel size for 16/32 bpp) */
static void convert_span(const pixfmt *f, const uint16_t *s, int n, uint8_t *d)
{
    int x;
    if (f->fast == 1) { conv_x888(s, (uint32_t *)(void *)d, n); return; }
    if (f->fast == 2) { memcpy(d, s, (size_t)n * 2u); return; }
    if (f->bpp == 32) {
        uint32_t *d32 = (uint32_t *)(void *)d;
        const uint32_t *lut = f->lut32;
        for (x = 0; x < n; x++) d32[x] = lut[s[x]];
    } else if (f->bpp == 16) {
        uint16_t *d16 = (uint16_t *)(void *)d;
        const uint16_t *lut = f->lut16;
        for (x = 0; x < n; x++) d16[x] = lut[s[x]];
    } else {
        for (x = 0; x < n; x++, d += 3) {
            const uint8_t *b = (const uint8_t *)&f->lut32[s[x]];
            d[0] = b[0]; d[1] = b[1]; d[2] = b[2];
        }
    }
}

/* RGB565 rows -> server ZPixmap rows (dst rows are stride-aligned; row padding zeroed) */
static void convert_rows(const pixfmt *f, const uint16_t *src, size_t src_stride_px, int w, int rows,
                         uint8_t *dst, size_t dst_stride)
{
    size_t used = (size_t)w * (size_t)f->bpp / 8u;
    int y;
    for (y = 0; y < rows; y++) {
        uint8_t *d = dst + (size_t)y * dst_stride;
        convert_span(f, src + (size_t)y * src_stride_px, w, d);
        if (dst_stride > used) memset(d + used, 0, dst_stride - used);
    }
}

/* one server pixel (at p) -> RGB565 */
static uint16_t decode_pixel(const pixfmt *f, const uint8_t *p)
{
    uint32_t val, r, g, b;
    if (f->bpp == 32) val = f->msb ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
                                   : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
    else if (f->bpp == 24) val = f->msb ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
                                        : ((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
    else val = f->msb ? ((uint32_t)p[0] << 8 | p[1]) : ((uint32_t)p[1] << 8 | p[0]);
    r = scale_to((val & f->rmask) >> f->rshift, f->rbits, 5);
    g = scale_to((val & f->gmask) >> f->gshift, f->gbits, 6);
    b = scale_to((val & f->bmask) >> f->bshift, f->bbits, 5);
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void decode_rows(const pixfmt *f, const uint8_t *src, size_t src_stride, int w, int rows,
                        uint16_t *dst, size_t dst_stride_px)
{
    int x, y, bytes = f->bpp / 8;
    for (y = 0; y < rows; y++) {
        const uint8_t *s = src + (size_t)y * src_stride;
        uint16_t *d = dst + (size_t)y * dst_stride_px;
        for (x = 0; x < w; x++) d[x] = decode_pixel(f, s + (size_t)x * (size_t)bytes);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* I/O                                                                                         */
/* ------------------------------------------------------------------------------------------ */
static void process_input(x11v *v);

static int read_input(x11v *v)
{
    if (v->dead) return -1;
    for (;;) {
        ssize_t r;
        if (v->in_cap - v->in_len < 65536u) {
            size_t nc = v->in_cap ? v->in_cap * 2u : 262144u;
            uint8_t *n;
            while (nc - v->in_len < 65536u) nc *= 2u;
            n = (uint8_t *)realloc(v->in, nc);
            if (!n) return conn_fail(v, "out of memory (input buffer)", 0);
            v->in = n; v->in_cap = nc;
        }
        r = recv(v->fd, v->in + v->in_len, v->in_cap - v->in_len, MSG_DONTWAIT);
        if (r > 0) { v->in_len += (size_t)r; continue; }
        if (r == 0) {
            if (v->in_setup) { v->dead = 1; return -1; }   /* a refusal is followed by close: caller decides */
            return conn_fail(v, "server closed the connection", 0);
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return conn_fail(v, "recv", errno);
    }
}

/* Write all of iov (modified). Handles partial writes; keeps reading input while blocked. */
static int send_iov(x11v *v, struct iovec *iov, int n)
{
    double last = now_ms();
    if (v->dead) return -1;
    while (n > 0 && iov[0].iov_len == 0) { iov++; n--; }
    while (n > 0) {
        struct msghdr mh;
        ssize_t r;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = iov;
        mh.msg_iovlen = n;
        r = sendmsg(v->fd, &mh, MSG_NOSIGNAL);
        if (r > 0) {
            size_t k = (size_t)r;
            v->info.bytes_sent += k;
            while (n > 0 && k >= iov[0].iov_len) { k -= iov[0].iov_len; iov++; n--; }
            if (n > 0) { iov[0].iov_base = (uint8_t *)iov[0].iov_base + k; iov[0].iov_len -= k; }
            while (n > 0 && iov[0].iov_len == 0) { iov++; n--; }
            last = now_ms();
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            int pr;
            v->info.write_stalls++;
            pfd.fd = v->fd; pfd.events = POLLOUT | POLLIN; pfd.revents = 0;
            pr = poll(&pfd, 1, 500);
            if (pr < 0 && errno != EINTR) return conn_fail(v, "poll", errno);
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                if (read_input(v) < 0) return -1;
                process_input(v);
            }
            if (now_ms() - last > IO_TIMEOUT_MS) return conn_fail(v, "server stopped reading (write timeout)", 0);
            continue;
        }
        return conn_fail(v, "send", r < 0 ? errno : EPIPE);
    }
    return 0;
}

static int flush_out(x11v *v)
{
    struct iovec iov;
    int r;
    if (v->dead) return -1;
    if (!v->out_len) return 0;
    iov.iov_base = v->out; iov.iov_len = v->out_len;
    r = send_iov(v, &iov, 1);
    v->out_len = 0;
    return r;
}

/* Reserve a zeroed request of `bytes` (multiple of 4) in the output queue; counts a sequence number. */
static uint8_t *req(x11v *v, size_t bytes)
{
    uint8_t *p;
    if (v->dead) return NULL;
    if (v->out_len + bytes > v->out_cap) {
        size_t nc = v->out_cap ? v->out_cap : 4096u;
        uint8_t *n;
        while (nc < v->out_len + bytes) nc *= 2u;
        n = (uint8_t *)realloc(v->out, nc);
        if (!n) { xlog("out of memory (request buffer)"); return NULL; }
        v->out = n; v->out_cap = nc;
    }
    p = v->out + v->out_len;
    memset(p, 0, bytes);
    v->out_len += bytes;
    v->seq++;
    return p;
}

static int pump(x11v *v, int timeout_ms)
{
    struct pollfd pfd;
    int r;
    if (flush_out(v) < 0) return -1;
    pfd.fd = v->fd; pfd.events = POLLIN; pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : conn_fail(v, "poll", errno);
    if (r > 0) {
        if (read_input(v) < 0) return -1;
        process_input(v);
    }
    return 0;
}

/* ---- reply bookkeeping ---- */
static int want_add(x11v *v, int quiet)   /* for the request just queued */
{
    int i;
    for (i = 0; i < NWANT; i++) {
        want_t *w = &v->want[i];
        if (!w->active) {
            memset(w, 0, sizeof *w);
            w->active = 1; w->quiet = quiet; w->seq = (uint16_t)v->seq;
            return i;
        }
    }
    xlog("internal: too many outstanding replies");
    return -1;
}

/* Wait for a wanted reply. Returns malloc'd reply (caller frees) or NULL with *err = X error code
 * (>0) or -1 (connection/timeout). */
static uint8_t *want_wait(x11v *v, int i, size_t *len, int *err)
{
    want_t *w;
    uint8_t *b;
    double end = now_ms() + REPLY_TIMEOUT_MS;
    *err = -1;
    if (len) *len = 0;
    if (i < 0) return NULL;
    w = &v->want[i];
    if (flush_out(v) == 0) process_input(v);
    while (!w->done && !v->dead) {
        int rem = (int)(end - now_ms());
        if (rem <= 0) { conn_fail(v, "timeout waiting for a reply", 0); break; }
        if (pump(v, rem) < 0) break;
    }
    b = NULL;
    if (w->done) {
        *err = w->err;
        b = w->buf;
        if (len) *len = w->len;
    } else {
        free(w->buf);
    }
    memset(w, 0, sizeof *w);
    return b;
}

static int round_trip(x11v *v)
{
    int slot, err;
    uint8_t *p = req(v, 4), *b;
    if (!p) return -1;
    p[0] = OP_GetInputFocus; put16(p + 2, 1);
    slot = want_add(v, 0);
    b = want_wait(v, slot, NULL, &err);
    free(b);
    return err == 0 ? 0 : -1;
}

static void quiet_begin(x11v *v, uint16_t lo)
{
    v->quiet_active = 1; v->quiet_lo = lo; v->quiet_hi = (uint16_t)v->seq;
    v->quiet_hits = 0; v->quiet_code = 0;
}
/* call after a round trip; returns number of errors in the range */
static unsigned quiet_end(x11v *v)
{
    v->quiet_active = 0;
    return v->quiet_hits;
}

/* ---- incoming messages ---- */
static void handle_error(x11v *v, const uint8_t *p)
{
    unsigned code = p[1], minor = get16(p + 8), major = p[10];
    uint16_t seq = (uint16_t)get16(p + 2);
    uint32_t val = get32(p + 4);
    int quiet = 0, i;
    for (i = 0; i < NWANT; i++) {
        want_t *w = &v->want[i];
        if (w->active && !w->done && w->seq == seq) {
            w->done = 1; w->err = (int)code; w->buf = NULL; w->len = 0;
            quiet = w->quiet;
            break;
        }
    }
    if (v->quiet_active && (uint16_t)(seq - v->quiet_lo) <= (uint16_t)(v->quiet_hi - v->quiet_lo)) {
        quiet = 1; v->quiet_hits++; v->quiet_code = (int)code;
    }
    /* requests that were already in flight when someone else destroyed our window */
    if (v->destroyed && val == v->window && (code == 3 || code == 8 || code == 9)) quiet = 1;
    /* a failed ShmPutImage sends no completion event: release its buffer half */
    if (v->shm_major && major == (unsigned)v->shm_major && minor == 3) {
        unsigned k;
        for (k = 0; k < 256u; k++) {
            if (v->shm_ring_live[k] && v->shm_ring_seq[k] == seq) {
                int hh = v->shm_ring_half[k];
                v->shm_ring_live[k] = 0;
                if (v->shm_pending[hh] > 0) v->shm_pending[hh]--;
                break;
            }
        }
    }
    if (quiet) {
        v->info.probe_errors++;
        DBG(v, "(expected) X error %u (%s) on %s, seq %u", code, error_name(v, code), request_name(v, major, minor), seq);
        return;
    }
    v->info.x_errors++;
    v->info.last_error_code = (int)code;
    v->info.last_error_major = (int)major;
    v->info.last_error_minor = (int)minor;
    if (v->info.x_errors <= MAX_PRINTED_ERRORS)
        xlog("X error %u (%s): request opcode %u (%s) minor %u, bad value 0x%08x, sequence %u",
             code, error_name(v, code), major, request_name(v, major, minor), minor, (unsigned)val, seq);
    else if (v->info.x_errors == MAX_PRINTED_ERRORS + 1)
        xlog("further X errors are counted but not printed");
}

static void mark_dirty(x11v *v, int y0, int y1)
{
    if (y1 <= y0) return;
    if (!v->dirty) { v->dirty = 1; v->dirty_y0 = y0; v->dirty_y1 = y1; return; }
    if (y0 < v->dirty_y0) v->dirty_y0 = y0;
    if (y1 > v->dirty_y1) v->dirty_y1 = y1;
}

static void handle_event(x11v *v, const uint8_t *p)
{
    unsigned code = p[0] & 0x7Fu;
    if (v->shm_major && code == (unsigned)v->shm_event) {          /* ShmCompletion */
        uint32_t off = get32(p + 16);
        uint16_t seq = (uint16_t)get16(p + 2);
        int i = (v->shm_half && off >= v->shm_half) ? 1 : 0;
        unsigned k;
        if (v->shm_pending[i] > 0) v->shm_pending[i]--;
        for (k = 0; k < 256u; k++)
            if (v->shm_ring_live[k] && v->shm_ring_seq[k] == seq) { v->shm_ring_live[k] = 0; break; }
        return;
    }
    switch (code) {
    case EV_KeyPress:
        if (v->close_keys && get32(p + 12) == v->window && (v->closekey[p[1] >> 3] & (1u << (p[1] & 7))))
            v->closed = 1;
        break;
    case EV_Expose:
        if (get32(p + 4) == v->window) mark_dirty(v, (int)get16(p + 10), (int)(get16(p + 10) + get16(p + 14)));
        break;
    case EV_DestroyNotify:
        if (get32(p + 8) == v->window) { v->destroyed = 1; v->closed = 1; v->info.mapped = 0; }
        break;
    case EV_UnmapNotify:
        if (get32(p + 8) == v->window) v->info.mapped = 0;
        break;
    case EV_MapNotify:
        if (get32(p + 8) == v->window) v->info.mapped = 1;
        break;
    case EV_ConfigureNotify:
        if (get32(p + 8) == v->window) { v->info.win_w = (int)get16(p + 20); v->info.win_h = (int)get16(p + 22); }
        break;
    case EV_ClientMessage:
        if (p[1] == 32 && get32(p + 4) == v->window && v->a_wm_protocols &&
            get32(p + 8) == v->a_wm_protocols && get32(p + 12) == v->a_wm_delete)
            v->closed = 1;
        break;
    case EV_MappingNotify:
        if (p[4] == 1) v->keymap_stale = 1;
        break;
    default:
        break;
    }
}

static void handle_msg(x11v *v, const uint8_t *p, size_t len)
{
    uint16_t seq = (uint16_t)get16(p + 2);
    int i;
    if (p[0] == 0) { handle_error(v, p); return; }
    if (p[0] == 1) {
        for (i = 0; i < NWANT; i++) {
            want_t *w = &v->want[i];
            if (w->active && !w->done && w->seq == seq) {
                w->buf = (uint8_t *)malloc(len);
                if (w->buf) { memcpy(w->buf, p, len); w->len = len; w->err = 0; }
                else { w->len = 0; w->err = -1; }
                w->done = 1;
                return;
            }
        }
        if (v->sync_pending && seq == v->sync_seq) { v->sync_pending = 0; return; }
        DBG(v, "ignored unexpected reply, sequence %u", seq);
        return;
    }
    handle_event(v, p);
}

static void process_input(x11v *v)
{
    size_t off = 0;
    if (v->in_setup) return;
    while (!v->dead && v->in_len - off >= 32u) {
        const uint8_t *p = v->in + off;
        size_t need = 32u;
        if (p[0] == 1 || (p[0] & 0x7Fu) == EV_GenericEvent) {
            uint64_t extra = (uint64_t)get32(p + 4) * 4u;
            if (extra > MAX_REPLY_BYTES) { conn_fail(v, "protocol error (absurd reply length)", 0); v->in_len = 0; return; }
            need += (size_t)extra;
        }
        if (v->in_len - off < need) break;
        handle_msg(v, p, need);
        off += need;
    }
    if (off) {
        memmove(v->in, v->in + off, v->in_len - off);
        v->in_len -= off;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* DISPLAY parsing                                                                             */
/* ------------------------------------------------------------------------------------------ */
typedef struct {
    int  unix_sock;        /* 1 = local socket */
    char host[256];        /* TCP host */
    char path[108];        /* explicit socket path (DISPLAY started with '/') */
    int  display, screen;
} dpy_addr;

static int parse_display(const char *d, dpy_addr *a)
{
    int force_tcp = 0, force_unix = 0;
    const char *colon, *q, *slash;
    char *end;
    long num, scr = 0;
    size_t hl;

    memset(a, 0, sizeof *a);
    if (!d || !*d) return -1;
    if (d[0] == '/') {                                   /* explicit socket path */
        const char *c = strrchr(d, ':');
        size_t pl = strlen(d);
        a->unix_sock = 1;
        if (c && c[1] && isdigit((unsigned char)c[1])) {
            num = strtol(c + 1, &end, 10);
            if (*end == '.') scr = strtol(end + 1, &end, 10);
            if (*end == 0) { pl = (size_t)(c - d); a->display = (int)num; a->screen = (int)scr; }
        }
        if (pl >= sizeof a->path) return -1;
        memcpy(a->path, d, pl);
        a->path[pl] = 0;
        return 0;
    }
    colon = strrchr(d, ':');
    if (!colon) return -1;
    slash = strchr(d, '/');
    if (slash && slash < colon) {                        /* protocol/host:display */
        size_t n = (size_t)(slash - d);
        if ((n == 3 && !strncmp(d, "tcp", 3)) || (n == 4 && !strncmp(d, "inet", 4)) ||
            (n == 5 && !strncmp(d, "inet6", 5))) force_tcp = 1;
        else if ((n == 4 && !strncmp(d, "unix", 4)) || (n == 5 && !strncmp(d, "local", 5))) force_unix = 1;
        else return -1;
        d = slash + 1;
    }
    q = colon + 1;
    if (!isdigit((unsigned char)*q)) return -1;
    errno = 0;
    num = strtol(q, &end, 10);
    if (errno || num < 0 || num > 59535) return -1;
    if (*end == '.') {
        const char *s = end + 1;
        if (!isdigit((unsigned char)*s)) return -1;
        scr = strtol(s, &end, 10);
        if (scr < 0 || scr > 255) return -1;
    }
    if (*end != 0) return -1;
    a->display = (int)num;
    a->screen = (int)scr;
    hl = (size_t)(colon - d);
    if (hl >= 2 && d[0] == '[' && d[hl - 1] == ']') { d++; hl -= 2; }
    if (hl > 0 && d[hl - 1] == ':') return -1;           /* DECnet "host::n" not supported */
    if (hl == 0 || (hl == 4 && !strncmp(d, "unix", 4)) || force_unix) {
        if (force_tcp) { strcpy(a->host, "localhost"); a->unix_sock = 0; }
        else a->unix_sock = 1;
        return 0;
    }
    if (hl >= sizeof a->host) return -1;
    memcpy(a->host, d, hl);
    a->host[hl] = 0;
    a->unix_sock = 0;
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* .Xauthority (big-endian records: family, address, number, name, data)                      */
/* ------------------------------------------------------------------------------------------ */
#define FAM_INET   0
#define FAM_INET6  6
#define FAM_LOCAL  256
#define FAM_WILD   65535

static int rd_field(const uint8_t *b, size_t n, size_t *off, const uint8_t **p, size_t *len)
{
    size_t l;
    if (*off + 2u > n) return -1;
    l = be16(b + *off);
    *off += 2u;
    if (*off + l > n) return -1;
    *p = b + *off; *len = l;
    *off += l;
    return 0;
}

/* Returns 1 and the cookie if a MIT-MAGIC-COOKIE-1 entry matches, else 0.
 * local: match FamilyLocal entries by hostname; otherwise match `fam`/`addr` (TCP peer).
 * Score: exact address match 3 > FamilyWild 2 > FamilyLocal with another hostname 1 (local only). */
static int xauth_find(const char *path, int local, const char *hostname, int fam,
                      const uint8_t *addr, size_t alen, int dpy, uint8_t *cookie, size_t *clen)
{
    FILE *f;
    uint8_t *buf;
    size_t n, off = 0;
    char num[16];
    int best = 0;

    if (!path || !*path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    buf = (uint8_t *)malloc(1u << 20);
    if (!buf) { fclose(f); return 0; }
    n = fread(buf, 1, 1u << 20, f);
    fclose(f);
    snprintf(num, sizeof num, "%d", dpy);
    while (off + 2u <= n) {
        const uint8_t *ea, *en, *ename, *edata;
        size_t eal, enl, enamel, edatal;
        uint32_t family = be16(buf + off);
        int score = 0;
        off += 2u;
        if (rd_field(buf, n, &off, &ea, &eal) || rd_field(buf, n, &off, &en, &enl) ||
            rd_field(buf, n, &off, &ename, &enamel) || rd_field(buf, n, &off, &edata, &edatal))
            break;
        if (enamel != 18 || memcmp(ename, "MIT-MAGIC-COOKIE-1", 18) != 0) continue;
        if (edatal == 0 || edatal > 64) continue;
        if (enl != 0 && (enl != strlen(num) || memcmp(en, num, enl) != 0)) continue;
        if (family == FAM_WILD) score = 2;
        else if (local && family == FAM_LOCAL)
            score = (eal == strlen(hostname) && memcmp(ea, hostname, eal) == 0) ? 3 : 1;
        else if (!local && (int)family == fam && eal == alen && memcmp(ea, addr, alen) == 0) score = 3;
        if (score > best) {
            best = score;
            memcpy(cookie, edata, edatal);
            *clen = edatal;
        }
    }
    free(buf);
    return best > 0;
}

static const char *xauth_path(char *buf, size_t sz)
{
    const char *x = getenv("XAUTHORITY"), *h;
    if (x && *x) return x;
    h = getenv("HOME");
    if (!h || !*h) return NULL;
    snprintf(buf, sz, "%s/.Xauthority", h);
    return buf;
}

/* ------------------------------------------------------------------------------------------ */
/* connecting                                                                                  */
/* ------------------------------------------------------------------------------------------ */
static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}

static int connect_unix(const dpy_addr *a)
{
    struct sockaddr_un sa;
    char path[108];
    int fd, attempt, saved = ENOENT;
    if (a->path[0]) snprintf(path, sizeof path, "%s", a->path);
    else snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", a->display);
    for (attempt = 0; attempt < 2; attempt++) {         /* 0: filesystem, 1: Linux abstract namespace */
        socklen_t sl;
        size_t pl = strlen(path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (attempt == 0) {
            memcpy(sa.sun_path, path, pl);
            sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + pl + 1u);
        } else {
            if (pl + 1u > sizeof sa.sun_path) { close(fd); break; }
            memcpy(sa.sun_path + 1, path, pl);
            sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + pl + 1u);
        }
        if (connect(fd, (struct sockaddr *)&sa, sl) == 0) return fd;
        if (attempt == 0) saved = errno;           /* report the filesystem-socket error */
        close(fd);
        if (a->path[0]) break;
    }
    errno = saved;
    return -1;
}

#ifdef X11V_NO_GETADDRINFO
/* minimal /etc/hosts lookup (keeps fully static binaries free of NSS) */
static int hosts_lookup(const char *name, struct sockaddr_storage *ss, socklen_t *sl)
{
    FILE *f = fopen("/etc/hosts", "r");
    char line[512];
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *save = NULL, *tok, *ip;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        ip = strtok_r(line, " \t\r\n", &save);
        if (!ip) continue;
        while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            if (strcasecmp(tok, name) == 0) {
                struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
                memset(ss, 0, sizeof *ss);
                if (inet_pton(AF_INET, ip, &s4->sin_addr) == 1) { s4->sin_family = AF_INET; *sl = sizeof *s4; fclose(f); return 0; }
                if (inet_pton(AF_INET6, ip, &s6->sin6_addr) == 1) { s6->sin6_family = AF_INET6; *sl = sizeof *s6; fclose(f); return 0; }
            }
        }
    }
    fclose(f);
    return -1;
}
#endif

static int resolve_host(const char *host, struct sockaddr_storage *ss, socklen_t *sl)
{
    struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
    memset(ss, 0, sizeof *ss);
    if (strcasecmp(host, "localhost") == 0) host = "127.0.0.1";
    if (inet_pton(AF_INET, host, &s4->sin_addr) == 1) { s4->sin_family = AF_INET; *sl = sizeof *s4; return 0; }
    if (inet_pton(AF_INET6, host, &s6->sin6_addr) == 1) { s6->sin6_family = AF_INET6; *sl = sizeof *s6; return 0; }
#ifdef X11V_NO_GETADDRINFO
    return hosts_lookup(host, ss, sl);
#else
    {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
        if (res->ai_addrlen > sizeof *ss) { freeaddrinfo(res); return -1; }
        memcpy(ss, res->ai_addr, res->ai_addrlen);
        *sl = (socklen_t)res->ai_addrlen;
        freeaddrinfo(res);
        return 0;
    }
#endif
}

static int connect_tcp(const dpy_addr *a, int *fam, uint8_t addr[16], size_t *alen, int *is_local)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    int fd, r, one = 1;
    unsigned port = 6000u + (unsigned)a->display;
    if (resolve_host(a->host, &ss, &sl) < 0) { xlog("cannot resolve X server host \"%s\"", a->host); return -1; }
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&ss;
        s4->sin_port = htons((uint16_t)port);
        memcpy(addr, &s4->sin_addr, 4); *alen = 4; *fam = FAM_INET;
        *is_local = addr[0] == 127;
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&ss;
        static const uint8_t v4mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
        static const uint8_t loop6[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        s6->sin6_port = htons((uint16_t)port);
        if (memcmp(&s6->sin6_addr, v4mapped, 12) == 0) {
            memcpy(addr, (const uint8_t *)&s6->sin6_addr + 12, 4); *alen = 4; *fam = FAM_INET;
            *is_local = addr[0] == 127;
        } else {
            memcpy(addr, &s6->sin6_addr, 16); *alen = 16; *fam = FAM_INET6;
            *is_local = memcmp(addr, loop6, 16) == 0;
        }
    }
    fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_nonblock(fd);
    r = connect(fd, (struct sockaddr *)&ss, sl);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd pfd;
        int soerr = 0;
        socklen_t el = sizeof soerr;
        pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
        r = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
        if (r <= 0) { errno = r == 0 ? ETIMEDOUT : errno; close(fd); return -1; }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &el) < 0 || soerr) { errno = soerr; close(fd); return -1; }
    } else if (r < 0) {
        close(fd);
        return -1;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* Send the connection setup and read the server's answer (raw, before normal message parsing).
 * Returns status byte (0 Failed, 1 Success, 2 Authenticate) with *rep = whole answer, or -1. */
static int xsetup(x11v *v, const uint8_t *cookie, size_t clen, uint8_t **rep, size_t *replen)
{
    static const char name[] = "MIT-MAGIC-COOKIE-1";
    uint8_t buf[12 + 20 + 64];
    size_t nlen = cookie ? 18u : 0u, dlen = cookie ? clen : 0u, total, need = 8;
    struct iovec iov;
    double end = now_ms() + REPLY_TIMEOUT_MS;

    memset(buf, 0, sizeof buf);
    buf[0] = 'l';                                   /* LSB first */
    put16(buf + 2, 11); put16(buf + 4, 0);
    put16(buf + 6, (uint32_t)nlen); put16(buf + 8, (uint32_t)dlen);
    if (cookie) {
        memcpy(buf + 12, name, nlen);
        memcpy(buf + 12 + pad4(nlen), cookie, dlen);
    }
    total = 12u + pad4(nlen) + pad4(dlen);
    iov.iov_base = buf; iov.iov_len = total;
    v->in_setup = 1;
    if (send_iov(v, &iov, 1) < 0) return -1;
    for (;;) {
        int eof = 0;
        if (v->in_len >= 8u) {
            need = 8u + 4u * (size_t)get16(v->in + 6);
            if (v->in_len >= need) break;
        }
        if (v->dead) eof = 1;
        else {
            struct pollfd pfd;
            int rem = (int)(end - now_ms()), r;
            if (rem <= 0) { xlog("timeout waiting for the X server's connection setup reply"); return -1; }
            pfd.fd = v->fd; pfd.events = POLLIN; pfd.revents = 0;
            r = poll(&pfd, 1, rem);
            if (r < 0 && errno != EINTR) return -1;
            if (r > 0 && read_input(v) < 0) eof = 1;       /* data read before EOF is kept */
        }
        if (eof && !(v->in_len >= 8u && v->in_len >= 8u + 4u * (size_t)get16(v->in + 6))) {
            xlog("X server closed the connection during setup (%u bytes received)", (unsigned)v->in_len);
            return -1;
        }
    }
    *rep = (uint8_t *)malloc(need);
    if (!*rep) return -1;
    memcpy(*rep, v->in, need);
    *replen = need;
    memmove(v->in, v->in + need, v->in_len - need);
    v->in_len -= need;
    v->in_setup = 0;
    return (*rep)[0];
}

static uint32_t new_id(x11v *v)
{
    uint32_t id = v->rid_base | ((v->rid_next++ << v->rid_shift) & v->rid_mask);
    return id;
}

/* Parse the Success setup reply; choose screen + visual + pixel format. */
static int parse_setup(x11v *v, const uint8_t *s, size_t n, int want_screen)
{
    size_t off, vlen, fmt_off, scr_off = 0, dep_lo = 0, dep_hi = 0;
    unsigned nroots, nfmt, i, maxreq;
    int chosen, root_depth = 0, depth = 0, bpp = 0, spad = 0, vclass = -1, sc;
    uint32_t root_visual = 0, visual = 0, rm = 0, gm = 0, bm = 0;

    if (n < 40) goto bad;
    v->info.protocol_major = (int)get16(s + 2);
    v->info.protocol_minor = (int)get16(s + 4);
    v->info.release = get32(s + 8);
    v->rid_base = get32(s + 12);
    v->rid_mask = get32(s + 16);
    vlen = get16(s + 24);
    maxreq = get16(s + 26);
    nroots = s[28];
    nfmt = s[29];
    v->fmt.msb = s[30] ? 1 : 0;
    v->min_kc = s[34];
    v->max_kc = s[35];
    if (!v->rid_mask) { xlog("server gave an empty resource id mask"); return -1; }
    {
        uint32_t m = v->rid_mask;
        v->rid_shift = 0;
        while (!(m & 1u)) { m >>= 1; v->rid_shift++; }
        v->rid_next = 1;
    }
    off = 40;
    if (off + vlen > n) goto bad;
    {
        size_t c = vlen < sizeof v->info.vendor - 1 ? vlen : sizeof v->info.vendor - 1;
        memcpy(v->info.vendor, s + off, c);
        v->info.vendor[c] = 0;
    }
    off += pad4(vlen);
    fmt_off = off;
    off += (size_t)nfmt * 8u;
    if (off > n || nroots == 0) goto bad;
    chosen = (want_screen >= 0 && (unsigned)want_screen < nroots) ? want_screen : 0;
    if (want_screen != chosen) xlog("screen %d not present, using screen 0", want_screen);
    for (sc = 0; sc < (int)nroots; sc++) {
        const uint8_t *S;
        unsigned nd, d;
        if (off + 40u > n) goto bad;
        S = s + off;
        if (sc == chosen) {
            scr_off = off;
            v->root = get32(S);
            v->black_pixel = get32(S + 12);
            v->info.screen_w = (int)get16(S + 20);
            v->info.screen_h = (int)get16(S + 22);
            root_visual = get32(S + 32);
            root_depth = S[38];
        }
        nd = S[39];
        off += 40u;
        if (sc == chosen) dep_lo = off;
        for (d = 0; d < nd; d++) {
            size_t nv;
            if (off + 8u > n) goto bad;
            nv = get16(s + off + 2);
            off += 8u;
            if (off + nv * 24u > n) goto bad;
            off += nv * 24u;
        }
        if (sc == chosen) dep_hi = off;
    }
    (void)scr_off;
    v->info.screen = chosen;
    v->info.root = v->root;

    /* visual: root visual if TrueColor, else best TrueColor visual (depth 24 > 30 > 16 > 15) */
    {
        static const int pref[] = { 24, 30, 16, 15 };
        int pass;
        for (pass = 0; pass < 5 && !visual; pass++) {
            size_t o = dep_lo;
            while (o < dep_hi) {
                int dd = s[o];
                size_t nv = get16(s + o + 2), k;
                o += 8u;
                for (k = 0; k < nv; k++, o += 24u) {
                    const uint8_t *V = s + o;
                    uint32_t vid = get32(V);
                    int cls = V[4];
                    if (visual) continue;
                    if (pass == 0 ? (vid == root_visual && cls == 4) : (cls == 4 && dd == pref[pass - 1])) {
                        visual = vid; vclass = cls; depth = dd;
                        rm = get32(V + 8); gm = get32(V + 12); bm = get32(V + 16);
                    }
                }
            }
        }
        if (!visual) { xlog("no TrueColor visual on this screen (root depth %d) -- unsupported", root_depth); return -1; }
    }
    for (i = 0; i < nfmt; i++) {
        const uint8_t *F = s + fmt_off + (size_t)i * 8u;
        if (F[0] == depth) { bpp = F[1]; spad = F[2]; break; }
    }
    if (!bpp) { xlog("no pixmap format for depth %d", depth); return -1; }
    if (fmt_init(&v->fmt, depth, bpp, spad, s[30] ? 1 : 0, rm, gm, bm) < 0) {
        xlog("unsupported pixel format: depth %d, %d bpp, scanline pad %d", depth, bpp, spad);
        return -1;
    }
    v->visual = visual;
    v->max_req_words = maxreq;
    v->info.visual = visual;
    v->info.visual_class = vclass;
    v->info.depth = depth;
    v->info.bpp = bpp;
    v->info.scanline_pad = spad;
    v->info.msb_first = v->fmt.msb;
    v->info.red_mask = rm; v->info.green_mask = gm; v->info.blue_mask = bm;
    v->info.max_request_bytes = maxreq * 4u;
    if (visual != root_visual) v->colormap = 1;   /* marker: need our own colormap */
    DBG(v, "vendor \"%s\" release %u, screen %d %dx%d, visual 0x%x depth %d bpp %d pad %d %s, masks %06x/%06x/%06x, max request %u bytes",
        v->info.vendor, (unsigned)v->info.release, chosen, v->info.screen_w, v->info.screen_h, (unsigned)visual,
        depth, bpp, spad, v->fmt.msb ? "MSBFirst" : "LSBFirst", (unsigned)rm, (unsigned)gm, (unsigned)bm, maxreq * 4u);
    return 0;
bad:
    xlog("malformed connection setup reply");
    return -1;
}

/* ------------------------------------------------------------------------------------------ */
/* requests                                                                                    */
/* ------------------------------------------------------------------------------------------ */
static int req_named(x11v *v, int opcode, int b1, const char *name)   /* InternAtom / QueryExtension */
{
    size_t n = strlen(name), len = 8u + pad4(n);
    uint8_t *p = req(v, len);
    if (!p) return -1;
    p[0] = (uint8_t)opcode; p[1] = (uint8_t)b1;
    put16(p + 2, (uint32_t)(len / 4u));
    put16(p + 4, (uint32_t)n);
    memcpy(p + 8, name, n);
    return want_add(v, 0);
}

static int change_prop(x11v *v, uint32_t prop, uint32_t type, int format, const void *data, uint32_t nelem)
{
    size_t bytes = (size_t)nelem * (size_t)(format / 8), len = 24u + pad4(bytes);
    uint8_t *p;
    if (len / 4u > 65535u) return -1;
    p = req(v, len);
    if (!p) return -1;
    p[0] = OP_ChangeProperty; p[1] = 0;                  /* Replace */
    put16(p + 2, (uint32_t)(len / 4u));
    put32(p + 4, v->window); put32(p + 8, prop); put32(p + 12, type);
    p[16] = (uint8_t)format;
    put32(p + 20, nelem);
    if (format == 32) {
        const uint32_t *d = (const uint32_t *)data;
        uint32_t i;
        for (i = 0; i < nelem; i++) put32(p + 24 + 4u * i, d[i]);
    } else if (bytes) {
        memcpy(p + 24, data, bytes);
    }
    return 0;
}

static int keymap_request(x11v *v)
{
    uint8_t *p;
    if (v->max_kc < v->min_kc || v->min_kc < 8) return -1;
    p = req(v, 8);
    if (!p) return -1;
    p[0] = OP_GetKeyboardMapping; put16(p + 2, 2);
    p[4] = (uint8_t)v->min_kc;
    p[5] = (uint8_t)(v->max_kc - v->min_kc + 1);
    return want_add(v, 0);
}

static void keymap_parse(x11v *v, const uint8_t *r, size_t len)
{
    unsigned kpk = r[1], count = (unsigned)(v->max_kc - v->min_kc + 1), k, j;
    memset(v->closekey, 0, sizeof v->closekey);
    if (!kpk || 32u + (size_t)count * kpk * 4u > len) return;
    for (k = 0; k < count; k++) {
        for (j = 0; j < kpk && j < 2; j++) {
            uint32_t ks = get32(r + 32 + ((size_t)k * kpk + j) * 4u);
            if (ks == 0xFF1Bu || ks == 'q' || ks == 'Q') {
                unsigned kc = (unsigned)v->min_kc + k;
                v->closekey[kc >> 3] |= (uint8_t)(1u << (kc & 7u));
            }
        }
    }
}

static int keymap_refresh(x11v *v)
{
    int err, slot = keymap_request(v);
    size_t len;
    uint8_t *r;
    if (slot < 0) return -1;
    r = want_wait(v, slot, &len, &err);
    if (r) keymap_parse(v, r, len);
    free(r);
    return r ? 0 : -1;
}

static int wait_frame_sync(x11v *v)
{
    double end = now_ms() + REPLY_TIMEOUT_MS;
    process_input(v);
    while (v->sync_pending && !v->dead) {
        int rem = (int)(end - now_ms());
        if (rem <= 0) return conn_fail(v, "timeout waiting for the X server to process a frame", 0);
        if (pump(v, rem) < 0) return -1;
    }
    return v->dead ? -1 : 0;
}

static int queue_frame_sync(x11v *v)
{
    uint8_t *p = req(v, 4);
    if (!p) return -1;
    p[0] = OP_GetInputFocus; put16(p + 2, 1);
    v->sync_seq = (uint16_t)v->seq;
    v->sync_pending = 1;
    return 0;
}

/* PutImage the rectangle (x0, y0, rw x rows) of the socket-path frame buffer, in bands that fit
 * the maximum request length. Full-width rectangles go straight from the frame buffer (no copy);
 * narrower ones are packed into the staging buffer band by band. */
static int send_rect(x11v *v, int x0, int y0, int rw, int rows)
{
    const size_t fstride = v->fb_stride, bpp8 = (size_t)v->fmt.bpp / 8u;
    const int packed = !(x0 == 0 && rw == v->fb_w);
    const size_t stride = packed ? row_stride(&v->fmt, rw) : fstride, used = (size_t)rw * bpp8;
    size_t maxb = (size_t)v->max_req_words * 4u, hdr = v->info.big_requests ? 28u : 24u;
    static const uint8_t zeros[4] = { 0, 0, 0, 0 };
    int per;
    if (maxb > MAX_IMAGE_REQ) maxb = MAX_IMAGE_REQ;
    if (maxb < hdr + stride) {
        /* one row does not fit in a request (very wide image on a server without BIG-REQUESTS):
           send vertical strips that do */
        int cw = (int)((maxb - hdr) * 8u / (size_t)v->fmt.bpp), x;
        while (cw > 0 && row_stride(&v->fmt, cw) > maxb - hdr) cw--;
        if (cw <= 0 || cw >= rw) { xlog("image row (%u bytes) exceeds the server's maximum request length", (unsigned)stride); return -1; }
        for (x = x0; x < x0 + rw; x += cw)
            if (send_rect(v, x, y0, x0 + rw - x < cw ? x0 + rw - x : cw, rows) < 0) return -1;
        return 0;
    }
    per = (int)((maxb - hdr) / stride);
    if (packed) {
        size_t need = stride * (size_t)(rows < per ? rows : per);
        if (need > v->stage_cap) {
            uint8_t *n = (uint8_t *)realloc(v->stage, need);
            if (!n) { xlog("out of memory (staging buffer)"); return -1; }
            v->stage = n; v->stage_cap = need;
        }
    }
    if (flush_out(v) < 0) return -1;
    while (rows > 0) {
        int nrows = rows < per ? rows : per, r;
        size_t data = stride * (size_t)nrows, padb = pad4(data) - data;
        size_t words = (24u + data + padb) / 4u, hl, o;
        uint8_t hb[28];
        struct iovec iov[3];
        const uint8_t *src;
        if (packed) {
            for (r = 0; r < nrows; r++) {
                uint8_t *d = v->stage + (size_t)r * stride;
                memcpy(d, v->fb + (size_t)(y0 + r) * fstride + (size_t)x0 * bpp8, used);
                if (stride > used) memset(d + used, 0, stride - used);
            }
            src = v->stage;
        } else {
            src = v->fb + (size_t)y0 * fstride;
        }
        memset(hb, 0, sizeof hb);
        hb[0] = OP_PutImage; hb[1] = 2;                  /* ZPixmap */
        if (words <= 65535u) { put16(hb + 2, (uint32_t)words); hl = 24u; o = 4u; }
        else { put16(hb + 2, 0); put32(hb + 4, (uint32_t)(words + 1u)); hl = 28u; o = 8u; }  /* BIG-REQUESTS */
        put32(hb + o, v->window);
        put32(hb + o + 4, v->gc);
        put16(hb + o + 8, (uint32_t)rw);
        put16(hb + o + 10, (uint32_t)nrows);
        put16(hb + o + 12, (uint32_t)x0);                /* dst-x */
        put16(hb + o + 14, (uint32_t)y0);                /* dst-y */
        hb[o + 16] = 0;                                  /* left-pad */
        hb[o + 17] = (uint8_t)v->fmt.depth;
        iov[0].iov_base = hb; iov[0].iov_len = hl;
        iov[1].iov_base = (void *)src; iov[1].iov_len = data;
        iov[2].iov_base = (void *)zeros; iov[2].iov_len = padb;
        v->seq++;
        v->info.requests++;
        v->info.pixels_sent += (unsigned long long)rw * (unsigned long long)nrows;
        if (send_iov(v, iov, 3) < 0) return -1;
        y0 += nrows; rows -= nrows;
    }
    return 0;
}

static int send_rows(x11v *v, int y0, int rows) { return send_rect(v, 0, y0, v->fb_w, rows); }

/* ---- MIT-SHM ---- */
/* ShmPutImage the rectangle (x0, y0, rw x rows) of shm half `half` to the same place in the window */
static int shm_put_rect(x11v *v, int half, int x0, int y0, int rw, int rows, int send_event)
{
    uint8_t *p = req(v, 40);
    if (!p) return -1;
    p[0] = (uint8_t)v->shm_major; p[1] = 3;             /* ShmPutImage */
    put16(p + 2, 10);
    put32(p + 4, v->window); put32(p + 8, v->gc);
    put16(p + 12, (uint32_t)v->shm_w); put16(p + 14, (uint32_t)v->shm_h);   /* total image */
    put16(p + 16, (uint32_t)x0); put16(p + 18, (uint32_t)y0);              /* src x,y */
    put16(p + 20, (uint32_t)rw); put16(p + 22, (uint32_t)rows);            /* src w,h */
    put16(p + 24, (uint32_t)x0); put16(p + 26, (uint32_t)y0);              /* dst x,y */
    p[28] = (uint8_t)v->fmt.depth; p[29] = 2; p[30] = (uint8_t)(send_event ? 1 : 0);
    put32(p + 32, v->shm_seg);
    put32(p + 36, (uint32_t)((size_t)half * v->shm_half));
    if (send_event) {
        unsigned k = v->shm_ring_i++ & 255u;
        v->shm_pending[half]++;
        v->shm_ring_seq[k] = (uint16_t)v->seq; v->shm_ring_half[k] = (uint8_t)half; v->shm_ring_live[k] = 1;
    }
    v->info.requests++;
    v->info.pixels_sent += (unsigned long long)rw * (unsigned long long)rows;
    return 0;
}

static int shm_wait_idle(x11v *v, int half, int timeout_ms)
{
    double end = now_ms() + timeout_ms;
    process_input(v);
    while (v->shm_pending[half] > 0 && !v->dead) {
        int rem = (int)(end - now_ms());
        if (rem <= 0) return -1;
        if (pump(v, rem) < 0) return -1;
    }
    return v->dead ? -1 : 0;
}

static void shm_release(x11v *v)
{
    uint8_t *p;
    if (!v->shm_addr) return;
    if (!v->dead) {
        if (!v->destroyed) {
            shm_wait_idle(v, 0, 2000);
            shm_wait_idle(v, 1, 2000);
        }
        p = req(v, 8);
        if (p) { p[0] = (uint8_t)v->shm_major; p[1] = 2; put16(p + 2, 2); put32(p + 4, v->shm_seg); }
        flush_out(v);
    }
    shmdt(v->shm_addr);
    v->shm_addr = NULL;
    v->shm_pending[0] = v->shm_pending[1] = 0;
    if (v->frame_src == 1) v->have_frame = 0;
    if (v->shm_state > 0) v->shm_state = 0;
    v->info.shm = 0;
}

/* Verify the server really sees our segment, exactly the way frames use it: a sub-rectangle of
 * an image at a non-zero offset (half 1), ShmPutImage with send-event into a pixmap, the
 * ShmCompletion event recognised, then GetImage the rectangle back and compare. */
static int shm_probe(x11v *v)
{
    enum { PW = 16, PH = 3, SX = 4, SY = 1, RW = PW - SX, RH = PH - SY };
    uint16_t pat[PW * PH], back[RW * RH], want[RW * RH];
    size_t st = row_stride(&v->fmt, PW), rst = row_stride(&v->fmt, RW), len = 0;
    uint32_t pix = new_id(v);
    uint16_t lo;
    uint8_t *p, *r;
    int i, slot, err, ok = 0, x, y;

    for (i = 0; i < PW * PH; i++) pat[i] = (uint16_t)(0x1234u + (unsigned)i * 0x9E37u);
    pat[SY * PW + SX] = 0x0000; pat[SY * PW + SX + 1] = 0xFFFF; pat[SY * PW + SX + 2] = 0xF800;
    pat[SY * PW + SX + 3] = 0x07E0; pat[SY * PW + SX + 4] = 0x001F;
    for (y = 0; y < RH; y++) for (x = 0; x < RW; x++) want[y * RW + x] = pat[(y + SY) * PW + x + SX];
    convert_rows(&v->fmt, pat, PW, PW, PH, v->shm_addr + v->shm_half, st);
    v->shm_pending[1] = 0;
    p = req(v, 16);
    if (!p) return -1;
    lo = (uint16_t)v->seq;
    p[0] = OP_CreatePixmap; p[1] = (uint8_t)v->fmt.depth; put16(p + 2, 4);
    put32(p + 4, pix); put32(p + 8, v->window); put16(p + 12, PW); put16(p + 14, PH);
    p = req(v, 40);
    if (!p) return -1;
    p[0] = (uint8_t)v->shm_major; p[1] = 3; put16(p + 2, 10);
    put32(p + 4, pix); put32(p + 8, v->gc);
    put16(p + 12, PW); put16(p + 14, PH); put16(p + 16, SX); put16(p + 18, SY);
    put16(p + 20, RW); put16(p + 22, RH); put16(p + 24, SX); put16(p + 26, SY);
    p[28] = (uint8_t)v->fmt.depth; p[29] = 2; p[30] = 1;                    /* send ShmCompletion */
    put32(p + 32, v->shm_seg); put32(p + 36, (uint32_t)v->shm_half);
    v->shm_pending[1] = 1;
    p = req(v, 20);
    if (!p) return -1;
    p[0] = OP_GetImage; p[1] = 2; put16(p + 2, 5);
    put32(p + 4, pix); put16(p + 8, SX); put16(p + 10, SY); put16(p + 12, RW); put16(p + 14, RH);
    put32(p + 16, 0xFFFFFFFFu);
    slot = want_add(v, 1);
    p = req(v, 8);
    if (!p) return -1;
    p[0] = OP_FreePixmap; put16(p + 2, 2); put32(p + 4, pix);
    quiet_begin(v, lo);
    r = want_wait(v, slot, &len, &err);
    if (round_trip(v) < 0) { free(r); quiet_end(v); return -1; }
    if (quiet_end(v) == 0 && r && len >= 32u + rst * RH) {
        decode_rows(&v->fmt, r + 32, rst, RW, RH, back, RW);
        ok = memcmp(want, back, sizeof want) == 0;
    }
    free(r);
    if (ok && v->shm_pending[1] != 0) {       /* completion comes before the GetImage reply */
        DBG(v, "MIT-SHM probe: no ShmCompletion event seen");
        ok = 0;
    }
    v->shm_pending[1] = 0;
    DBG(v, "MIT-SHM probe: %s", ok ? "pixels match, completion event seen" : "FAILED (server does not see our segment)");
    return ok ? 0 : -1;
}

static int shm_setup(x11v *v, int w, int h)
{
    size_t stride = row_stride(&v->fmt, w), half = (stride * (size_t)h + 4095u) & ~(size_t)4095u;
    int id;
    void *a;
    uint8_t *p;
    uint16_t lo;
    unsigned bad;

    if (v->shm_state < 0) return -1;
    if (v->shm_state > 0 && v->shm_addr && v->shm_w == w && v->shm_h == h) return 0;
    shm_release(v);
    if (half * 2u > (512u << 20)) goto disable;
    id = shmget(IPC_PRIVATE, half * 2u, IPC_CREAT | 0600);
    if (id < 0) { DBG(v, "shmget: %s", strerror(errno)); goto disable; }
    a = shmat(id, NULL, 0);
    if (a == (void *)-1) { DBG(v, "shmat: %s", strerror(errno)); shmctl(id, IPC_RMID, NULL); goto disable; }
    v->shm_seg = new_id(v);
    p = req(v, 16);
    if (!p) { shmdt(a); shmctl(id, IPC_RMID, NULL); return -1; }
    lo = (uint16_t)v->seq;
    p[0] = (uint8_t)v->shm_major; p[1] = 1; put16(p + 2, 4);     /* ShmAttach */
    put32(p + 4, v->shm_seg); put32(p + 8, (uint32_t)id); p[12] = 1; /* read-only */
    quiet_begin(v, lo);
    if (round_trip(v) < 0) { quiet_end(v); shmdt(a); shmctl(id, IPC_RMID, NULL); return -1; }
    bad = quiet_end(v);
    shmctl(id, IPC_RMID, NULL);           /* freed automatically once both sides detach */
    if (bad) {
        DBG(v, "MIT-SHM attach refused (X error %d) -- using PutImage", v->quiet_code);
        shmdt(a);
        goto disable;
    }
    v->shm_addr = (uint8_t *)a;
    v->shm_half = half;
    v->shm_stride = stride;
    v->shm_w = w; v->shm_h = h;
    v->shm_pending[0] = v->shm_pending[1] = 0;
    v->shm_last = 1;
    if (!v->shm_verified) {
        if (shm_probe(v) != 0) { shm_release(v); goto disable; }
        v->shm_verified = 1;
    }
    v->shm_state = 1;
    v->info.shm = 1;
    return 0;
disable:
    if (!v->dead) DBG(v, "MIT-SHM not usable, uploading with PutImage over the socket");
    v->shm_state = -1;
    v->info.shm = 0;
    return -1;
}

/* ---- delta upload (both paths): only rectangles that changed since the previous frame ---- */
#define DELTA_GAP_ROWS 8     /* unchanged rows tolerated inside one rectangle (fewer requests) */

/* Make room for the previous-frame copy and the per-row spans; on failure delta is switched off. */
static void delta_reserve(x11v *v, int w, int h)
{
    size_t npx = (size_t)w * (size_t)h;
    if (!v->delta || (npx <= v->prev_cap && h <= v->rx_cap)) return;
    if (npx > v->prev_cap) {
        uint16_t *np = (uint16_t *)realloc(v->prev, npx * 2u);
        if (np) { v->prev = np; v->prev_cap = npx; }
    }
    if (h > v->rx_cap) {
        int *nr = (int *)realloc(v->rx, (size_t)h * 2u * sizeof(int));
        if (nr) { v->rx = nr; v->rx_cap = h; }
    }
    if (npx > v->prev_cap || h > v->rx_cap) v->delta = 0;       /* no memory: plain full frames */
}

/* Compare px with the previous frame: v->rx[2y] = first changed x (-1 = row unchanged),
 * v->rx[2y+1] = last changed x + 1; the copy is updated. If conv is set, the changed spans are
 * also converted into conv (rows conv_stride apart). Also adapts: after DELTA_BUSY_FRAMES frames
 * in a row with >= 90% of the pixels changed (full-motion content), the next DELTA_SKIP_FRAMES
 * frames skip the comparison and go up whole (the result is the same, the scan is saved). */
#define DELTA_BUSY_FRAMES 3
#define DELTA_SKIP_FRAMES 15
static void delta_scan(x11v *v, const uint16_t *px, int w, int h, uint8_t *conv, size_t conv_stride)
{
    size_t bpp8 = (size_t)v->fmt.bpp / 8u, span = 0;
    int y;
    for (y = 0; y < h; y++) {
        const uint16_t *a = px + (size_t)y * (size_t)w;
        uint16_t *b = v->prev + (size_t)y * (size_t)w;
        int x0 = 0, x1 = w;
        if (memcmp(a, b, (size_t)w * 2u) == 0) { v->rx[2 * y] = -1; continue; }
        while (a[x0] == b[x0]) x0++;                  /* terminates: the rows differ */
        while (a[x1 - 1] == b[x1 - 1]) x1--;
        v->rx[2 * y] = x0; v->rx[2 * y + 1] = x1;
        if (conv) convert_span(&v->fmt, a + x0, x1 - x0, conv + (size_t)y * conv_stride + (size_t)x0 * bpp8);
        memcpy(b + x0, a + x0, (size_t)(x1 - x0) * 2u);
        span += (size_t)(x1 - x0);
    }
    if (span * 10u >= (size_t)w * (size_t)h * 9u) {
        if (++v->delta_busy >= DELTA_BUSY_FRAMES) v->delta_skip = DELTA_SKIP_FRAMES;
    } else {
        v->delta_busy = 0;
    }
}

/* delta can be used for this frame (caller has checked size/source continuity) */
static int delta_try(x11v *v)
{
    if (v->delta_skip > 0) { v->delta_skip--; return 0; }
    return 1;
}

typedef int (*rect_fn)(x11v *v, int x0, int y0, int rw, int rows, int arg);

/* Merge the changed rows into rectangles (rows closer than DELTA_GAP_ROWS join one rectangle that
 * spans the union of their changed columns -- the source image already holds the right pixels
 * for everything inside it) and hand each to fn. Returns rectangles emitted, or -1. */
static int delta_emit(x11v *v, int w, int h, rect_fn fn, int arg)
{
    int y, ry0 = -1, last = -1, gx0 = 0, gx1 = 0, n = 0;
    for (y = 0; y <= h; y++) {
        if (y < h && v->rx[2 * y] >= 0) {
            if (ry0 < 0) { ry0 = y; gx0 = v->rx[2 * y]; gx1 = v->rx[2 * y + 1]; }
            else {
                if (v->rx[2 * y] < gx0) gx0 = v->rx[2 * y];
                if (v->rx[2 * y + 1] > gx1) gx1 = v->rx[2 * y + 1];
            }
            last = y;
            continue;
        }
        if (ry0 >= 0 && (y == h || y - last > DELTA_GAP_ROWS)) {
            if ((gx1 - gx0) * 4 >= w * 3) { gx0 = 0; gx1 = w; }  /* nearly full width: whole rows, no packing */
            if (fn(v, gx0, ry0, gx1 - gx0, last + 1 - ry0, arg) < 0) return -1;
            n++;
            ry0 = -1;
        }
    }
    return n;
}

static int rect_socket(x11v *v, int x0, int y0, int rw, int rows, int arg)
{
    (void)arg;
    return send_rect(v, x0, y0, rw, rows);
}

static int rect_shm(x11v *v, int x0, int y0, int rw, int rows, int half)
{
    return shm_put_rect(v, half, x0, y0, rw, rows, 1);
}

static int put_shm(x11v *v, const uint16_t *px, int w, int h)
{
    int i = v->shm_last ^ 1, r, full;
    double t0 = now_ms(), t1, t2;
    delta_reserve(v, w, h);
    full = !v->delta || !v->have_frame || v->frame_src != 1 || v->info.x_errors != v->err_at_put || !delta_try(v);
    if (shm_wait_idle(v, i, REPLY_TIMEOUT_MS) < 0)       /* half i was used two frames ago */
        return v->dead ? -1 : conn_fail(v, "timeout waiting for MIT-SHM completion", 0);
    if (v->destroyed) return 0;                         /* learned while waiting */
    t1 = now_ms();
    /* half i holds an older frame, so it always gets the whole new frame (cheap); only the
       changed rectangles are then drawn from it */
    convert_rows(&v->fmt, px, (size_t)w, w, h, v->shm_addr + (size_t)i * v->shm_half, v->shm_stride);
    if (full) { if (v->delta) memcpy(v->prev, px, (size_t)w * (size_t)h * 2u); }
    else delta_scan(v, px, w, h, NULL, 0);
    t2 = now_ms();
    v->shm_last = i;
    v->have_frame = 1;
    v->frame_src = 1;
    if (full) r = shm_put_rect(v, i, 0, 0, w, h, 1);
    else { r = delta_emit(v, w, h, rect_shm, i); v->info.delta_frames++; }
    if (r < 0) return -1;
    r = flush_out(v);
    v->info.ms_wait += t1 - t0;
    v->info.ms_convert += t2 - t1;
    v->info.ms_send += now_ms() - t2;
    v->err_at_put = v->info.x_errors;
    return r;
}

static int put_socket(x11v *v, const uint16_t *px, int w, int h)
{
    size_t stride = row_stride(&v->fmt, w), need = stride * (size_t)h;
    int full, sent;
    double t0, t1, t2;

    if (need > v->fb_size) {
        uint8_t *n = (uint8_t *)realloc(v->fb, need);
        if (!n) { xlog("out of memory (frame buffer %u bytes)", (unsigned)need); return -1; }
        v->fb = n; v->fb_size = need;
    }
    delta_reserve(v, w, h);
    full = !v->delta || !v->have_frame || v->frame_src != 0 || v->fb_w != w || v->fb_h != h ||
           v->info.x_errors != v->err_at_put || !delta_try(v);
    v->fb_w = w; v->fb_h = h; v->fb_stride = stride;
    v->have_frame = 1;
    v->frame_src = 0;

    /* The previous frame's bytes are already in the socket, so the buffer can be reused now;
       converting before waiting overlaps our work with the server's. */
    t0 = now_ms();
    if (full) {
        convert_rows(&v->fmt, px, (size_t)w, w, h, v->fb, stride);
        if (v->delta) memcpy(v->prev, px, (size_t)w * (size_t)h * 2u);
    } else {
        delta_scan(v, px, w, h, v->fb, stride);        /* converts only the changed spans */
    }
    t1 = now_ms();
    v->info.ms_convert += t1 - t0;
    if (wait_frame_sync(v) < 0) return -1;
    t2 = now_ms();
    v->info.ms_wait += t2 - t1;
    if (v->destroyed) return 0;                         /* learned while waiting */

    if (full) {
        if (send_rect(v, 0, 0, w, h) < 0) return -1;
        sent = 1;
    } else {
        sent = delta_emit(v, w, h, rect_socket, 0);
        if (sent < 0) return -1;
        v->info.delta_frames++;
    }
    if (sent && queue_frame_sync(v) < 0) return -1;     /* nothing changed: nothing to wait for */
    if (flush_out(v) < 0) return -1;
    v->info.ms_send += now_ms() - t2;
    v->err_at_put = v->info.x_errors;
    return 0;
}

static int redraw_dirty(x11v *v)
{
    int y0 = v->dirty_y0, y1 = v->dirty_y1, fh;
    v->dirty = 0;
    if (!v->have_frame || v->destroyed) return 0;
    fh = v->frame_src == 1 ? v->shm_h : v->fb_h;
    if (y0 < 0) y0 = 0;
    if (y1 > fh) y1 = fh;
    if (y1 <= y0) return 0;
    v->info.expose_redraws++;
    if (v->frame_src == 1) return shm_put_rect(v, v->shm_last, 0, y0, v->shm_w, y1 - y0, 1);
    return send_rows(v, y0, y1 - y0);
}

/* ------------------------------------------------------------------------------------------ */
/* public API                                                                                  */
/* ------------------------------------------------------------------------------------------ */
x11v *x11v_open(int w, int h, const char *title)
{
    x11v *v;
    dpy_addr a;
    const char *dstr = getenv("DISPLAY");
    struct utsname un;
    char hostname[256] = "", apath_buf[1024];
    const char *apath;
    uint8_t cookie[64], peer[16], *rep = NULL;
    size_t clen = 0, plen = 0, replen = 0;
    int have_cookie = 0, fam = 0, is_local = 1, attempt, st = -1;
    int s_atom[4], s_big, s_shm, s_km, i, err;
    static const char *atom_names[4] = { "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING" };
    uint32_t *atoms[4];
    uint8_t *p;

    if (w <= 0 || h <= 0 || w > 32767 || h > 32767) { xlog("bad window size %dx%d", w, h); return NULL; }
    v = (x11v *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->fd = -1;
    v->close_keys = 1;
    v->debug = env_true("X11V_DEBUG");
    v->shm_state = env_true("X11V_NO_SHM") ? -1 : 0;
    v->delta = !env_true("X11V_NO_DELTA");
    atoms[0] = &v->a_wm_protocols; atoms[1] = &v->a_wm_delete; atoms[2] = &v->a_net_wm_name; atoms[3] = &v->a_utf8;

    if (parse_display(dstr, &a) < 0) {
        xlog("cannot use DISPLAY=\"%s\" (expected [host]:n[.screen])", dstr ? dstr : "(unset)");
        goto fail;
    }
    v->info.display = a.display;
    if (uname(&un) == 0) snprintf(hostname, sizeof hostname, "%s", un.nodename);
    apath = xauth_path(apath_buf, sizeof apath_buf);

    for (attempt = 0; attempt < 2; attempt++) {
        if (a.unix_sock) {
            v->fd = connect_unix(&a);
            if (v->fd < 0) {
                if (a.path[0]) xlog("cannot connect to X server socket %s: %s", a.path, strerror(errno));
                else xlog("cannot connect to X server socket /tmp/.X11-unix/X%d: %s", a.display, strerror(errno));
                goto fail;
            }
            set_nonblock(v->fd);
            is_local = 1;
        } else {
            v->fd = connect_tcp(&a, &fam, peer, &plen, &is_local);
            if (v->fd < 0) { xlog("cannot connect to X server %s:%d (TCP port %d): %s", a.host, a.display, 6000 + a.display, strerror(errno)); goto fail; }
            if (!is_local && strcmp(a.host, hostname) == 0) is_local = 1;
        }
        {
            int sb = 4 << 20;                   /* a whole frame fits in the send buffer where allowed */
            setsockopt(v->fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
        }
        v->info.unix_socket = a.unix_sock;
        if (attempt == 0)
            have_cookie = xauth_find(apath, is_local, hostname, fam, peer, plen, a.display, cookie, &clen);
        if (attempt == 1 && !have_cookie) break;
        DBG(v, "connecting to %s%s:%d (%s), %s", a.unix_sock ? "" : a.host, a.unix_sock ? "unix" : "",
            a.display, a.unix_sock ? "AF_UNIX" : "TCP",
            (attempt == 0 && have_cookie) ? "MIT-MAGIC-COOKIE-1" : "no authorization");
        st = xsetup(v, (attempt == 0 && have_cookie) ? cookie : NULL, clen, &rep, &replen);
        if (st == 1) { v->info.auth = attempt == 0 && have_cookie; break; }
        if (st < 0) { xlog("X connection setup failed (I/O)"); goto fail; }
        {
            size_t rl = st == 0 ? rep[1] : replen - 8u, rs = rl;
            if (8u + rl > replen) rs = replen - 8u;
            xlog("X server refused the connection%s: %.*s", st == 2 ? " (needs further authentication)" : "",
                 (int)rs, (const char *)rep + 8);
        }
        free(rep); rep = NULL;
        close(v->fd); v->fd = -1;
        v->in_len = 0; v->out_len = 0; v->seq = 0; v->in_setup = 0; v->dead = 0;
        if (!(attempt == 0 && have_cookie)) goto fail;
        xlog("retrying without authorization");
    }
    if (st != 1) goto fail;
    if (parse_setup(v, rep, replen, a.screen) < 0) goto fail;
    free(rep); rep = NULL;

    /* atoms, extensions, keyboard map: one pipelined round trip */
    for (i = 0; i < 4; i++) s_atom[i] = req_named(v, OP_InternAtom, 0, atom_names[i]);
    s_big = env_true("X11V_NO_BIGREQ") ? -1 : req_named(v, OP_QueryExtension, 0, "BIG-REQUESTS");
    s_shm = (a.unix_sock && v->shm_state == 0) ? req_named(v, OP_QueryExtension, 0, "MIT-SHM") : -1;
    s_km = keymap_request(v);
    for (i = 0; i < 4; i++) {
        size_t len;
        uint8_t *r = want_wait(v, s_atom[i], &len, &err);
        if (r && len >= 12) *atoms[i] = get32(r + 8);
        free(r);
        if (v->dead) goto fail;
    }
    if (s_big >= 0) {
        uint8_t *r = want_wait(v, s_big, NULL, &err);
        if (r && r[8]) v->bigreq_major = r[9];
        free(r);
    }
    if (s_shm >= 0) {
        uint8_t *r = want_wait(v, s_shm, NULL, &err);
        if (r && r[8]) { v->shm_major = r[9]; v->shm_event = r[10]; v->shm_error = r[11]; }
        free(r);
    }
    if (s_km >= 0) {
        size_t len;
        uint8_t *r = want_wait(v, s_km, &len, &err);
        if (r) keymap_parse(v, r, len);
        free(r);
    }
    if (v->dead) goto fail;
    if (!v->shm_major) v->shm_state = -1;
    if (v->bigreq_major) {                                      /* BigReqEnable */
        int slot;
        uint8_t *r;
        size_t len;
        p = req(v, 4);
        if (!p) goto fail;
        p[0] = (uint8_t)v->bigreq_major; p[1] = 0; put16(p + 2, 1);
        slot = want_add(v, 0);
        r = want_wait(v, slot, &len, &err);
        if (r && len >= 12 && get32(r + 8) > v->max_req_words) {
            v->max_req_words = get32(r + 8);
            v->info.big_requests = 1;
            v->info.max_request_bytes = v->max_req_words > 0x3FFFFFFFu ? 0xFFFFFFFCu : v->max_req_words * 4u;
        }
        free(r);
        if (v->dead) goto fail;
    }

    /* window */
    v->window = new_id(v);
    if (v->colormap) {
        v->colormap = new_id(v);
        p = req(v, 16);
        if (!p) goto fail;
        p[0] = OP_CreateColormap; p[1] = 0; put16(p + 2, 4);
        put32(p + 4, v->colormap); put32(p + 8, v->root); put32(p + 12, v->visual);
    }
    {
        uint32_t mask = 0x0002u | 0x0008u | 0x0010u | 0x0800u;  /* back pixel, border pixel, bit gravity, event mask */
        int nv = 4;
        if (v->colormap) { mask |= 0x2000u; nv++; }
        p = req(v, 32u + 4u * (size_t)nv);
        if (!p) goto fail;
        p[0] = OP_CreateWindow; p[1] = (uint8_t)v->fmt.depth; put16(p + 2, (uint32_t)(8 + nv));
        put32(p + 4, v->window); put32(p + 8, v->root);
        put16(p + 12, 0); put16(p + 14, 0); put16(p + 16, (uint32_t)w); put16(p + 18, (uint32_t)h);
        put16(p + 20, 0); put16(p + 22, 1);                     /* border 0, InputOutput */
        put32(p + 24, v->visual); put32(p + 28, mask);
        put32(p + 32, v->colormap ? 0u : v->black_pixel);        /* background: black */
        put32(p + 36, 0);                                        /* border pixel */
        put32(p + 40, 1);                                        /* bit gravity NorthWest */
        put32(p + 44, MASK_KeyPress | MASK_Exposure | MASK_StructureNotify);
        if (v->colormap) put32(p + 48, v->colormap);
    }
    v->info.window = v->window;
    v->info.win_w = w; v->info.win_h = h;
    {
        static const char wm_class[] = "x11view\0X11view";      /* instance\0class\0 */
        uint32_t hints[18];
        memset(hints, 0, sizeof hints);
        hints[0] = 8u | 16u | 32u;                               /* PSize | PMinSize | PMaxSize */
        hints[3] = (uint32_t)w; hints[4] = (uint32_t)h;
        hints[5] = (uint32_t)w; hints[6] = (uint32_t)h;
        hints[7] = (uint32_t)w; hints[8] = (uint32_t)h;
        change_prop(v, XA_WM_CLASS, XA_STRING, 8, wm_class, (uint32_t)sizeof wm_class);
        change_prop(v, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32, hints, 18);
        if (v->a_wm_protocols && v->a_wm_delete)
            change_prop(v, v->a_wm_protocols, XA_ATOM, 32, &v->a_wm_delete, 1);
    }
    if (x11v_set_title(v, title ? title : "x11view") < 0) goto fail;
    v->gc = new_id(v);
    p = req(v, 20);
    if (!p) goto fail;
    p[0] = OP_CreateGC; put16(p + 2, 5);
    put32(p + 4, v->gc); put32(p + 8, v->window);
    put32(p + 12, 0x00010000u);                                  /* value mask: graphics-exposures */
    put32(p + 16, 0);                                            /* = False */
    p = req(v, 8);
    if (!p) goto fail;
    p[0] = OP_MapWindow; put16(p + 2, 2); put32(p + 4, v->window);
    if (round_trip(v) < 0) goto fail;
    if (v->info.x_errors) { xlog("window creation failed (see X errors above)"); goto fail; }
    {   /* wait (bounded) until the window manager has actually mapped the window */
        double end = now_ms() + 2000.0;
        while (!v->info.mapped && !v->dead) {
            int rem = (int)(end - now_ms());
            if (rem <= 0) { DBG(v, "no MapNotify within 2 s (continuing)"); break; }
            if (pump(v, rem) < 0) goto fail;
        }
    }
    if (v->dead) goto fail;
    DBG(v, "window 0x%x %dx%d mapped=%d, big-requests=%d (max %u bytes), MIT-SHM %s",
        (unsigned)v->window, w, h, v->info.mapped, v->info.big_requests, (unsigned)v->info.max_request_bytes,
        v->shm_major ? "available" : "not used");
    return v;
fail:
    free(rep);
    x11v_close(v);
    return NULL;
}

int x11v_put_rgb565(x11v *v, const uint16_t *px, int w, int h)
{
    int r;
    if (!v || !px || w <= 0 || h <= 0 || w > 32767 || h > 32767) return -1;
    if (v->dead) return -1;
    if (v->destroyed) return 0;
    if (v->shm_state >= 0 && v->shm_major && shm_setup(v, w, h) == 0) r = put_shm(v, px, w, h);
    else if (v->dead) r = -1;
    else r = put_socket(v, px, w, h);
    if (r == 0) v->info.frames++;
    return r;
}

int x11v_set_title(x11v *v, const char *title)
{
    size_t n, i, o = 0;
    char *latin;
    const unsigned char *s;
    if (!v || v->dead) return -1;
    if (!title) title = "";
    n = strlen(title);
    if (n > 4096) {
        n = 4096;
        while (n > 0 && ((unsigned char)title[n] & 0xC0u) == 0x80u) n--;   /* don't split a UTF-8 char */
    }
    latin = (char *)malloc(n + 1);
    if (!latin) return -1;
    s = (const unsigned char *)title;
    for (i = 0; i < n; ) {                                      /* UTF-8 -> Latin-1 for WM_NAME */
        unsigned c = s[i], cp, extra = 0;
        if (c < 0x80u) { cp = c; }
        else if ((c & 0xE0u) == 0xC0u) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0u) == 0xE0u) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8u) == 0xF0u) { cp = c & 0x07u; extra = 3; }
        else { cp = '?'; }
        i++;
        while (extra && i < n && (s[i] & 0xC0u) == 0x80u) { cp = (cp << 6) | (s[i] & 0x3Fu); i++; extra--; }
        latin[o++] = (char)(cp < 256u ? cp : '?');
    }
    change_prop(v, XA_WM_NAME, XA_STRING, 8, latin, (uint32_t)o);
    free(latin);
    if (v->a_net_wm_name && v->a_utf8) change_prop(v, v->a_net_wm_name, v->a_utf8, 8, title, (uint32_t)n);
    return flush_out(v);
}

int x11v_poll(x11v *v)
{
    if (!v || v->dead) return -1;
    if (read_input(v) < 0) return -1;
    process_input(v);
    if (v->keymap_stale && !v->dead) { v->keymap_stale = 0; keymap_refresh(v); }
    if (v->dirty && !v->dead && redraw_dirty(v) < 0) return -1;
    if (flush_out(v) < 0) return -1;
    return v->closed ? 1 : 0;
}

void x11v_close(x11v *v)
{
    uint8_t *p;
    int i;
    if (!v) return;
    if (v->fd >= 0 && !v->dead && v->window) {
        shm_release(v);
        if (v->gc && (p = req(v, 8)) != NULL) { p[0] = OP_FreeGC; put16(p + 2, 2); put32(p + 4, v->gc); }
        if (!v->destroyed && (p = req(v, 8)) != NULL) { p[0] = OP_DestroyWindow; put16(p + 2, 2); put32(p + 4, v->window); }
        if (v->colormap && (p = req(v, 8)) != NULL) { p[0] = OP_FreeColormap; put16(p + 2, 2); put32(p + 4, v->colormap); }
        flush_out(v);
    } else if (v->shm_addr) {
        shmdt(v->shm_addr);
    }
    if (v->fd >= 0) close(v->fd);
    for (i = 0; i < NWANT; i++) free(v->want[i].buf);
    fmt_free(&v->fmt);
    free(v->out); free(v->in); free(v->fb);
    free(v->prev); free(v->rx); free(v->stage);
    free(v);
}

const x11v_info *x11v_get_info(x11v *v) { return v ? &v->info : NULL; }

int x11v_sync(x11v *v)
{
    if (!v || v->dead) return -1;
    if (round_trip(v) < 0) return -1;
    if (v->dirty && redraw_dirty(v) < 0) return -1;
    return (int)v->info.x_errors;
}

int x11v_read_rgb565(x11v *v, uint16_t *out, int w, int h)
{
    size_t stride;
    int y = 0, per;
    if (!v || !out || w <= 0 || h <= 0 || w > 32767 || h > 32767) return -1;
    if (v->dead) return -1;
    stride = row_stride(&v->fmt, w);
    per = (int)((4u << 20) / stride);
    if (per < 1) per = 1;
    while (y < h) {
        int n = h - y < per ? h - y : per, slot, err;
        size_t len;
        uint8_t *p = req(v, 20), *r;
        if (!p) return -1;
        p[0] = OP_GetImage; p[1] = 2; put16(p + 2, 5);
        put32(p + 4, v->window); put16(p + 8, 0); put16(p + 10, (uint32_t)y);
        put16(p + 12, (uint32_t)w); put16(p + 14, (uint32_t)n); put32(p + 16, 0xFFFFFFFFu);
        slot = want_add(v, 1);
        r = want_wait(v, slot, &len, &err);
        if (!r) return err > 0 ? err : -1;
        if (len < 32u + stride * (size_t)n) { free(r); xlog("short GetImage reply"); return -1; }
        decode_rows(&v->fmt, r + 32, stride, w, n, out + (size_t)y * (size_t)w, (size_t)w);
        free(r);
        y += n;
    }
    return 0;
}

int x11v_fd(x11v *v) { return v ? v->fd : -1; }

void x11v_close_on_keys(x11v *v, int enable) { if (v) v->close_keys = enable ? 1 : 0; }

int x11v_debug_provoke_error(x11v *v)
{
    uint8_t *p;
    if (!v || v->dead) return -1;
    p = req(v, 8);
    if (!p) return -1;
    p[0] = OP_MapWindow; put16(p + 2, 2); put32(p + 4, 0);   /* window None -> BadWindow */
    return flush_out(v);
}

/* ------------------------------------------------------------------------------------------ */
/* offline self-test                                                                           */
/* ------------------------------------------------------------------------------------------ */
static int st_fail;
#define ST_CHECK(cond, ...) do { if (!(cond)) { st_fail++; xlog("selftest FAIL: " __VA_ARGS__); } } while (0)

static void st_bytes(int depth, int bpp, int msb, uint32_t rm, uint32_t gm, uint32_t bm, uint16_t px,
                     const uint8_t *expect, const char *what)
{
    pixfmt f;
    uint8_t out[8];
    int nb = bpp / 8;
    if (fmt_init(&f, depth, bpp, 32, msb, rm, gm, bm) < 0) { st_fail++; xlog("selftest FAIL: fmt_init %s", what); return; }
    memset(out, 0xEE, sizeof out);
    convert_rows(&f, &px, 1, 1, 1, out, 4);
    ST_CHECK(memcmp(out, expect, (size_t)nb) == 0, "%s: 0x%04x -> %02x %02x %02x %02x", what, px, out[0], out[1], out[2], out[3]);
    fmt_free(&f);
}

static int st_display(const char *d, int ok, int unix_sock, const char *host, int dpy, int scr)
{
    dpy_addr a;
    int r = parse_display(d, &a);
    if (!ok) { ST_CHECK(r < 0, "DISPLAY \"%s\" should be rejected", d); return 0; }
    ST_CHECK(r == 0, "DISPLAY \"%s\" rejected", d);
    if (r) return 0;
    ST_CHECK(a.unix_sock == unix_sock && a.display == dpy && a.screen == scr &&
             (unix_sock || strcmp(a.host, host) == 0),
             "DISPLAY \"%s\" -> unix=%d host=\"%s\" dpy=%d screen=%d", d, a.unix_sock, a.host, a.display, a.screen);
    return 0;
}

static void st_xa_entry(FILE *f, unsigned fam, const void *addr, size_t alen, const char *num,
                        const char *name, const uint8_t *data, size_t dlen)
{
    uint8_t b[2];
    size_t nl = strlen(num), nm = strlen(name);
    b[0] = (uint8_t)(fam >> 8); b[1] = (uint8_t)fam; fwrite(b, 1, 2, f);
    b[0] = (uint8_t)(alen >> 8); b[1] = (uint8_t)alen; fwrite(b, 1, 2, f); fwrite(addr, 1, alen, f);
    b[0] = (uint8_t)(nl >> 8); b[1] = (uint8_t)nl; fwrite(b, 1, 2, f); fwrite(num, 1, nl, f);
    b[0] = (uint8_t)(nm >> 8); b[1] = (uint8_t)nm; fwrite(b, 1, 2, f); fwrite(name, 1, nm, f);
    b[0] = (uint8_t)(dlen >> 8); b[1] = (uint8_t)dlen; fwrite(b, 1, 2, f); fwrite(data, 1, dlen, f);
}

int x11v_selftest(const char *scratch_path)
{
    static const struct { int depth, bpp, pad, msb; uint32_t rm, gm, bm; int lossy_g; const char *name; } cfg[] = {
        { 24, 32, 32, 0, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0, "32bpp x8r8g8b8 LSBFirst" },
        { 24, 32, 32, 1, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0, "32bpp x8r8g8b8 MSBFirst" },
        { 24, 32, 32, 0, 0x0000FFu, 0x00FF00u, 0xFF0000u, 0, "32bpp x8b8g8r8 LSBFirst" },
        { 30, 32, 32, 0, 0x3FF00000u, 0x000FFC00u, 0x000003FFu, 0, "32bpp x2r10g10b10 LSBFirst" },
        { 24, 24, 32, 0, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0, "24bpp packed LSBFirst pad32" },
        { 24, 24, 32, 1, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0, "24bpp packed MSBFirst pad32" },
        { 24, 24,  8, 0, 0xFF0000u, 0x00FF00u, 0x0000FFu, 0, "24bpp packed LSBFirst pad8" },
        { 16, 16, 32, 0, 0xF800u, 0x07E0u, 0x001Fu, 0, "16bpp r5g6b5 LSBFirst" },
        { 16, 16, 32, 1, 0xF800u, 0x07E0u, 0x001Fu, 0, "16bpp r5g6b5 MSBFirst" },
        { 16, 16, 16, 0, 0xF800u, 0x07E0u, 0x001Fu, 0, "16bpp r5g6b5 LSBFirst pad16" },
        { 15, 16, 32, 0, 0x7C00u, 0x03E0u, 0x001Fu, 1, "16bpp x1r5g5b5 LSBFirst (lossy green)" },
    };
    enum { TW = 257, TH = 256 };                   /* odd width exercises row padding; covers all 65536 */
    uint16_t *src = (uint16_t *)malloc((size_t)TW * TH * 2u), *back = (uint16_t *)malloc((size_t)TW * TH * 2u);
    uint8_t *img = (uint8_t *)malloc((size_t)TW * 4u * TH + 64u);
    size_t c;
    int x, y;

    st_fail = 0;
    if (!src || !back || !img) { free(src); free(back); free(img); xlog("selftest: out of memory"); return -1; }
    for (y = 0; y < TH; y++) for (x = 0; x < TW; x++) src[y * TW + x] = (uint16_t)((unsigned)(y * TW + x) & 0xFFFFu);

    /* 1. round trip every RGB565 value through every format */
    for (c = 0; c < sizeof cfg / sizeof cfg[0]; c++) {
        pixfmt f;
        size_t st, used, bad = 0;
        int first = -1;
        if (fmt_init(&f, cfg[c].depth, cfg[c].bpp, cfg[c].pad, cfg[c].msb, cfg[c].rm, cfg[c].gm, cfg[c].bm) < 0) {
            st_fail++; xlog("selftest FAIL: fmt_init %s", cfg[c].name); continue;
        }
        st = row_stride(&f, TW);
        used = (size_t)TW * (size_t)cfg[c].bpp / 8u;
        ST_CHECK(st % (size_t)(cfg[c].pad / 8) == 0 && st >= used && st - used < (size_t)(cfg[c].pad / 8),
                 "%s: stride %u", cfg[c].name, (unsigned)st);
        memset(img, 0xAB, (size_t)TW * 4u * TH + 64u);
        convert_rows(&f, src, TW, TW, TH, img, st);
        for (y = 0; y < TH; y++) {
            size_t k;
            for (k = used; k < st; k++) if (img[(size_t)y * st + k] != 0) bad++;
        }
        ST_CHECK(bad == 0, "%s: row padding not zeroed", cfg[c].name);
        decode_rows(&f, img, st, TW, TH, back, TW);
        bad = 0;
        for (x = 0; x < TW * TH; x++) {
            uint16_t a = src[x], b = back[x];
            int okp;
            if (cfg[c].lossy_g) {
                int ga = (a >> 5) & 63, gb = (b >> 5) & 63;
                okp = (a & 0xF81Fu) == (b & 0xF81Fu) && (ga - gb <= 1 && gb - ga <= 1);
            } else okp = a == b;
            if (!okp) { bad++; if (first < 0) first = x; }
        }
        ST_CHECK(bad == 0, "%s: %u of %d pixels differ after round trip (first 0x%04x -> 0x%04x)", cfg[c].name,
                 (unsigned)bad, TW * TH, first >= 0 ? src[first] : 0, first >= 0 ? back[first] : 0);
        /* fast path (SIMD / copy) must produce exactly the LUT bytes, also for unaligned sub-spans */
        if (f.fast) {
            int fast = f.fast, k;
            uint8_t *ref = (uint8_t *)malloc((size_t)TW * 4u * TH + 64u);
            if (ref) {
                f.fast = 0;
                convert_rows(&f, src, TW, TW, TH, ref, st);
                f.fast = fast;
                ST_CHECK(memcmp(img, ref, st * TH) == 0, "%s: fast path differs from LUT", cfg[c].name);
                for (k = 0; k < 64; k++) {                   /* spans at odd offsets/lengths (delta path) */
                    int x0 = (k * 37) % 200, n = 1 + (k * 53) % (TW - x0 - 1);
                    size_t bo = (size_t)x0 * (size_t)(cfg[c].bpp / 8);
                    memset(img, 0x5A, (size_t)TW * 4u);
                    convert_span(&f, src + (size_t)k * TW + x0, n, img + bo);
                    f.fast = 0;
                    memset(ref, 0x5A, (size_t)TW * 4u);
                    convert_span(&f, src + (size_t)k * TW + x0, n, ref + bo);
                    f.fast = fast;
                    ST_CHECK(memcmp(img, ref, (size_t)TW * 4u) == 0, "%s: fast span x0=%d n=%d differs", cfg[c].name, x0, n);
                }
                free(ref);
            }
        }
        fmt_free(&f);
    }
#if defined(X11V_SSE2) || defined(X11V_NEON)
    {   /* the SIMD path must have been exercised by the configurations above on this host */
        pixfmt f;
        if (fmt_init(&f, 24, 32, 32, 0, 0xFF0000u, 0xFF00u, 0xFFu) == 0) {
            ST_CHECK(f.fast == 1, "SIMD fast path not selected for x8r8g8b8 LSBFirst");
            fmt_free(&f);
        }
    }
#endif

    /* 2. explicit byte layouts (independent of the decoder) */
    {
        static const uint8_t r32l[4] = { 0x00, 0x00, 0xFF, 0x00 }, b32l[4] = { 0xFF, 0x00, 0x00, 0x00 };
        static const uint8_t r32m[4] = { 0x00, 0xFF, 0x00, 0x00 }, b32m[4] = { 0x00, 0x00, 0x00, 0xFF };
        static const uint8_t g32l[4] = { 0x00, 0xFF, 0x00, 0x00 };
        static const uint8_t r24l[3] = { 0x00, 0x00, 0xFF }, r24m[3] = { 0xFF, 0x00, 0x00 };
        static const uint8_t v16l[2] = { 0x34, 0x12 }, v16m[2] = { 0x12, 0x34 };
        static const uint8_t r30l[4] = { 0x00, 0x00, 0xF0, 0x3F };
        static const uint8_t mid32l[4] = { 0x84, 0x82, 0x84, 0x00 };   /* 0x8410: r5=16 g6=32 b5=16 -> 0x84,0x82,0x84 */
        st_bytes(24, 32, 0, 0xFF0000u, 0xFF00u, 0xFFu, 0xF800, r32l, "32 LSB red");
        st_bytes(24, 32, 0, 0xFF0000u, 0xFF00u, 0xFFu, 0x001F, b32l, "32 LSB blue");
        st_bytes(24, 32, 0, 0xFF0000u, 0xFF00u, 0xFFu, 0x07E0, g32l, "32 LSB green");
        st_bytes(24, 32, 1, 0xFF0000u, 0xFF00u, 0xFFu, 0xF800, r32m, "32 MSB red");
        st_bytes(24, 32, 1, 0xFF0000u, 0xFF00u, 0xFFu, 0x001F, b32m, "32 MSB blue");
        st_bytes(24, 32, 0, 0xFF0000u, 0xFF00u, 0xFFu, 0x8410, mid32l, "32 LSB grey");
        st_bytes(24, 24, 0, 0xFF0000u, 0xFF00u, 0xFFu, 0xF800, r24l, "24 LSB red");
        st_bytes(24, 24, 1, 0xFF0000u, 0xFF00u, 0xFFu, 0xF800, r24m, "24 MSB red");
        st_bytes(16, 16, 0, 0xF800u, 0x7E0u, 0x1Fu, 0x1234, v16l, "16 LSB 565");
        st_bytes(16, 16, 1, 0xF800u, 0x7E0u, 0x1Fu, 0x1234, v16m, "16 MSB 565");
        st_bytes(30, 32, 0, 0x3FF00000u, 0xFFC00u, 0x3FFu, 0xF800, r30l, "32 LSB 10-bit red");
    }

    /* 3. DISPLAY parsing */
    st_display(":0", 1, 1, "", 0, 0);
    st_display(":1.2", 1, 1, "", 1, 2);
    st_display("unix:3", 1, 1, "", 3, 0);
    st_display("unix/anything:4.1", 1, 1, "", 4, 1);
    st_display("localhost:10.0", 1, 0, "localhost", 10, 0);
    st_display("tcp/localhost:11", 1, 0, "localhost", 11, 0);
    st_display("tcp/:5", 1, 0, "localhost", 5, 0);
    st_display("192.168.1.5:0", 1, 0, "192.168.1.5", 0, 0);
    st_display("[::1]:2", 1, 0, "::1", 2, 0);
    st_display("::1:3", 1, 0, "::1", 3, 0);
    st_display("orangepi.local:1", 1, 0, "orangepi.local", 1, 0);
    st_display("/tmp/.X11-unix/X0", 1, 1, "", 0, 0);
    st_display("", 0, 0, "", 0, 0);
    st_display("foo", 0, 0, "", 0, 0);
    st_display(":x", 0, 0, "", 0, 0);
    st_display(":0.x", 0, 0, "", 0, 0);
    st_display(":0junk", 0, 0, "", 0, 0);
    st_display("host::0", 0, 0, "", 0, 0);
    st_display("bogus/host:0", 0, 0, "", 0, 0);
    st_display(":99999", 0, 0, "", 0, 0);

    /* 4. .Xauthority matching */
    if (scratch_path) {
        FILE *f = fopen(scratch_path, "wb");
        ST_CHECK(f != NULL, "cannot create %s", scratch_path);
        if (f) {
            static const uint8_t A[16] = "AAAAAAAAAAAAAAA", B[16] = "BBBBBBBBBBBBBBB", C[16] = "CCCCCCCCCCCCCCC",
                                 D[16] = "DDDDDDDDDDDDDDD", E[16] = "EEEEEEEEEEEEEEE", F[16] = "FFFFFFFFFFFFFFF";
            static const uint8_t ip[4] = { 10, 0, 0, 5 }, ip2[4] = { 10, 0, 0, 6 };
            uint8_t ck[64];
            size_t cl = 0;
            int r;
            st_xa_entry(f, FAM_LOCAL, "otherhost", 9, "0", "MIT-MAGIC-COOKIE-1", A, 16);
            st_xa_entry(f, FAM_LOCAL, "myhost", 6, "1", "MIT-MAGIC-COOKIE-1", B, 16);
            st_xa_entry(f, FAM_LOCAL, "myhost", 6, "0", "XDM-AUTHORIZATION-1", C, 16);
            st_xa_entry(f, FAM_LOCAL, "myhost", 6, "0", "MIT-MAGIC-COOKIE-1", D, 16);
            st_xa_entry(f, FAM_INET, ip, 4, "0", "MIT-MAGIC-COOKIE-1", F, 16);
            st_xa_entry(f, FAM_WILD, "", 0, "", "MIT-MAGIC-COOKIE-1", E, 16);
            fclose(f);
            r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 0, ck, &cl);
            ST_CHECK(r == 1 && cl == 16 && memcmp(ck, D, 16) == 0, "xauth unix :0 should pick D");
            r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 1, ck, &cl);
            ST_CHECK(r == 1 && memcmp(ck, B, 16) == 0, "xauth unix :1 should pick B");
            r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 7, ck, &cl);
            ST_CHECK(r == 1 && memcmp(ck, E, 16) == 0, "xauth unix :7 should pick the wildcard E");
            r = xauth_find(scratch_path, 1, "renamed", 0, NULL, 0, 0, ck, &cl);
            ST_CHECK(r == 1 && memcmp(ck, E, 16) == 0, "xauth unix :0 unknown hostname should pick wildcard E");
            r = xauth_find(scratch_path, 0, "myhost", FAM_INET, ip, 4, 0, ck, &cl);
            ST_CHECK(r == 1 && memcmp(ck, F, 16) == 0, "xauth tcp 10.0.0.5:0 should pick F");
            r = xauth_find(scratch_path, 0, "myhost", FAM_INET, ip2, 4, 0, ck, &cl);
            ST_CHECK(r == 1 && memcmp(ck, E, 16) == 0, "xauth tcp 10.0.0.6:0 should pick wildcard E");
            /* without the wildcard: unknown hostname falls back to a Local entry for the display */
            f = fopen(scratch_path, "wb");
            if (f) {
                st_xa_entry(f, FAM_LOCAL, "otherhost", 9, "0", "MIT-MAGIC-COOKIE-1", A, 16);
                fclose(f);
                r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 0, ck, &cl);
                ST_CHECK(r == 1 && memcmp(ck, A, 16) == 0, "xauth local fallback should pick A");
                r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 3, ck, &cl);
                ST_CHECK(r == 0, "xauth :3 should find nothing");
                r = xauth_find(scratch_path, 0, "myhost", FAM_INET, ip, 4, 0, ck, &cl);
                ST_CHECK(r == 0, "xauth tcp should not use Local entries of other hosts");
            }
            /* truncated file must not crash or match */
            f = fopen(scratch_path, "wb");
            if (f) {
                static const uint8_t junk[5] = { 0x01, 0x00, 0x00, 0x09, 'o' };
                fwrite(junk, 1, sizeof junk, f);
                fclose(f);
                r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 0, ck, &cl);
                ST_CHECK(r == 0, "truncated xauth file should find nothing");
            }
            remove(scratch_path);
            r = xauth_find(scratch_path, 1, "myhost", 0, NULL, 0, 0, ck, &cl);
            ST_CHECK(r == 0, "missing xauth file should find nothing");
        }
    }
    free(src); free(back); free(img);
    return st_fail ? -1 : 0;
}

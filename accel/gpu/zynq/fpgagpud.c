/*
 * fpgagpud.c -- the Zynq PS daemon of the FPGA-GPU (SPEC sections 10, 12, 13.2; wire formats in
 * common/gpu_proto.h). Runs as root under systemd (fpgagpud.service); logs to stderr (journald).
 *
 *   fpgagpud                 real PL through /dev/mem (backend_hw.c)
 *   fpgagpud --sim           software PL model + simulated Teensy bus on TCP 7778 (backend_sim.c)
 *
 * TCP 7777, up to 4 clients, single-threaded poll loop that never blocks on one client:
 *  - a client becomes the controller with NET_HELLO (empty payload, or flags without
 *    GPU_HELLO_OBSERVER): the previous controller is demoted to observer (its unfinished message
 *    is dropped), then soft reset, CONTROL = SRC_PS|SCANOUT_EN, sprite pool reset.
 *    Observers -- and clients that have not sent HELLO yet -- may only use HELLO, STATUS,
 *    WAIT_FRAME, READBACK, FRAME_GET, SYNC; anything else is rejected (error counted, reported
 *    by SYNC, connection kept; requests that have a reply get GPU_ERR_PROTO).
 *  - messages of one client are executed strictly in order. Drawing messages are turned into
 *    records (gpu_setup_*) and pushed as whole records while PS_FIFO_FREE allows; when the FIFO
 *    is full (e.g. the collector waits for the Teensy END) that client's message is resumed
 *    later (200 us retry) while every other client keeps being served.
 *  - WAIT_FRAME and FRAME_GET are parked and completed from the loop (1 ms register polling).
 *  - the PL is probed at start and every 2 s while absent (HELLO/STATUS also re-probe, at most
 *    every 200 ms); without it HELLO/STATUS/... answer GPU_ERR_NOPL and drawing messages are
 *    dropped and counted as errors. GP0 is never touched unless the probe succeeded.
 *  - when the PL becomes ready: RET disabled, soft reset, CONTROL = SRC_PS|SCANOUT_EN,
 *    CLEAR_COLOR = 0x0010 and one END record: the screen turns dark blue = daemon running.
 * NET_RECORDS passes records through unchanged, except SPRITE records whose pixel reads would
 * leave the GPU's reserved DDR window (dropped, error counted): the PL reads DDR at w3.
 * Malformed input: wrong length for a known type -> that message is dropped (error counted,
 * GPU_ERR_PROTO reply if it has one); unknown type -> GPU_ERR_PROTO reply; bad magic or a length
 * above GPU_MSG_MAX_PAYLOAD -> the stream cannot be resynchronised: the connection is closed.
 * A client that closes its socket still gets its already-received complete messages executed.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "os.h"
#include "backend.h"
#include "gpu_proto.h"
#include "gpu_setup.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "fpgagpud assumes a little-endian host (the wire format is little-endian)"
#endif

#define FPGAGPUD_VERSION   "1.0"
#define MAX_CLIENTS        4
#define HDR_BYTES          ((uint32_t)sizeof(gpu_msg_hdr))
#define IN_MIN_CAP         (256u * 1024u)
#define OUT_HIGH_WATER     (4u * 1024u * 1024u)
#define BUF_SHRINK_ABOVE   (1u * 1024u * 1024u)
#define SOCK_BUF_BYTES     (2 * 1024 * 1024)
#define MAX_SPRITES        256
#define STARTUP_CLEAR      0x0010u
#define NS_MS              1000000ull
#define PROBE_INTERVAL_NS  (2000ull * NS_MS)
#define PROBE_MIN_GAP_NS   (200ull * NS_MS)
#define PARK_POLL_NS       (1ull * NS_MS)
#define FIFO_RETRY_NS      200000ll
#define RET_GRACE_NS       (200ull * NS_MS)
#define STALL_WARN_NS      (2000ull * NS_MS)
#define READBACK_TRIES     3
#define LOG_ERR_LIMIT      20
#define CTL_ALLOWED        (GPU_CTL_SRC_TEENSY | GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN)
#define CTL_DEFAULT        (GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN)

enum { ROLE_NONE = 0, ROLE_OBSERVER, ROLE_CONTROLLER };
enum { PARK_NONE = 0, PARK_WAIT, PARK_FGET };

typedef struct client {
    int       fd, id, role, dead, eof;
    char      peer[48];
    uint8_t  *in;
    size_t    in_cap, in_len, in_off;
    uint8_t  *out;
    size_t    out_cap, out_len, out_off;
    /* the message at in + in_off being executed (drawing messages can be resumed) */
    int       exec;
    gpu_msg_hdr hdr;
    uint32_t  item, nitems;
    uint32_t  stage[GPU_SETUP_MAX_OUT][GPU_REC_WORDS];
    int       stage_n, stage_i, fifo_stalled;
    /* parked request */
    int       park;
    uint64_t  deadline;
    uint32_t  wait_target;
    net_frame_get fg;
    /* accounting */
    uint32_t  errs_since_sync, nerr_logged;
    uint64_t  last_rx, msgs;
} client;

typedef struct {
    uint32_t addr, cap, w, h, stride;
    int      valid;
} sprite_slot;

static struct {
    backend    *be;
    int         verbose;
    int         pl_ok;
    char        pl_why[256];
    uint64_t    next_probe, last_probe;
    int         lfd;
    client     *cl[MAX_CLIENTS];
    int         ncl, next_id;
    client     *ctrl;
    uint32_t    daemon_errors, daemon_records;
    uint32_t    credit;                 /* PS FIFO entries known to be free */
    uint64_t    stall_since;
    int         stall_warned;
    sprite_slot spr[MAX_SPRITES];
    uint32_t    pool_next;
    /* frame return */
    int         ret_enabled;
    uint64_t    ret_grace_until;
    uint16_t   *cache;
    int         cache_valid, cache_mode;   /* mode 1 = all rows, 2 = even rows only */
    uint32_t    cache_frame;
    uint64_t    next_park_poll;
} D;

/* ------------------------------------------------------------------------------------------ */
/* small helpers                                                                                */
/* ------------------------------------------------------------------------------------------ */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *msg_name(unsigned t)
{
    switch (t) {
    case NET_HELLO:         return "HELLO";
    case NET_SET_CONFIG:    return "SET_CONFIG";
    case NET_TRIS:          return "TRIS";
    case NET_RECT:          return "RECT";
    case NET_SPRITE_UPLOAD: return "SPRITE_UPLOAD";
    case NET_SPRITE_DRAW:   return "SPRITE_DRAW";
    case NET_RECORDS:       return "RECORDS";
    case NET_END_FRAME:     return "END_FRAME";
    case NET_WAIT_FRAME:    return "WAIT_FRAME";
    case NET_STATUS:        return "STATUS";
    case NET_READBACK:      return "READBACK";
    case NET_RESET:         return "RESET";
    case NET_SYNC:          return "SYNC";
    case NET_FRAME_GET:     return "FRAME_GET";
    default:                return "unknown";
    }
}

static int known_type(unsigned t) { return t >= NET_HELLO && t <= NET_FRAME_GET; }

static int has_reply(unsigned t)
{
    return t == NET_HELLO || t == NET_SET_CONFIG || t == NET_SPRITE_UPLOAD || t == NET_WAIT_FRAME ||
           t == NET_STATUS || t == NET_READBACK || t == NET_RESET || t == NET_SYNC || t == NET_FRAME_GET;
}

static int observer_allowed(unsigned t)
{
    return t == NET_HELLO || t == NET_STATUS || t == NET_WAIT_FRAME || t == NET_READBACK ||
           t == NET_FRAME_GET || t == NET_SYNC;
}

static const char *role_name(int r)
{
    return r == ROLE_CONTROLLER ? "controller" : r == ROLE_OBSERVER ? "observer" : "no-HELLO-yet";
}

static void client_error(client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void client_error(client *c, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    D.daemon_errors++;
    c->errs_since_sync++;
    if (c->nerr_logged >= LOG_ERR_LIMIT)
        return;
    va_start(ap, fmt);
    os_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    os_log("client %d (%s): %s", c->id, c->peer, buf);
    if (++c->nerr_logged == LOG_ERR_LIMIT)
        os_log("client %d: further errors are counted but not logged", c->id);
}

static void kill_client(client *c, const char *why)
{
    if (!c->dead)
        os_log("client %d (%s): closing: %s", c->id, c->peer, why);
    c->dead = 1;
}

/* ------------------------------------------------------------------------------------------ */
/* replies                                                                                       */
/* ------------------------------------------------------------------------------------------ */
static int out_reserve(client *c, size_t n)
{
    size_t used;
    if (c->out_off == c->out_len)
        c->out_off = c->out_len = 0;
    if (c->out_len + n <= c->out_cap)
        return 0;
    used = c->out_len - c->out_off;
    if (c->out_off) {
        memmove(c->out, c->out + c->out_off, used);
        c->out_len = used;
        c->out_off = 0;
    }
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 65536u;
        uint8_t *p;
        while (cap < c->out_len + n)
            cap *= 2;
        p = os_realloc(c->out, cap);
        if (!p) {
            kill_client(c, "out of memory for a reply");
            return -1;
        }
        c->out = p;
        c->out_cap = cap;
    }
    return 0;
}

/* Append a reply header + len payload bytes; returns the payload pointer (NULL on failure). */
static uint8_t *reply_begin(client *c, unsigned type, uint32_t len)
{
    gpu_msg_hdr h;
    uint8_t *p;
    if (c->dead || out_reserve(c, HDR_BYTES + (size_t)len) < 0)
        return NULL;
    h.magic = GPU_NET_MAGIC;
    h.type = (uint16_t)(type | GPU_MSG_REPLY);
    h.flags = 0;
    h.len = len;
    p = c->out + c->out_len;
    memcpy(p, &h, HDR_BYTES);
    c->out_len += HDR_BYTES + (size_t)len;
    return p + HDR_BYTES;
}

static void reply_bytes(client *c, unsigned type, const void *payload, uint32_t len)
{
    uint8_t *p = reply_begin(c, type, len);
    if (p && len)
        memcpy(p, payload, len);
}

static void reply_status(client *c, unsigned type, int32_t st)
{
    reply_bytes(c, type, &st, 4);
}

/* Error reply with the full fixed-size layout the request type expects (zeroed fields). */
static void reply_error(client *c, unsigned type, int32_t st)
{
    union {
        net_hello_reply hello;
        net_sprite_upload_reply up;
        net_wait_frame_reply wf;
        net_status_reply status;
        net_sync_reply sync;
        net_frame_get_reply fg;
        struct GPU_PACKED { int32_t status; uint32_t w, h; } rb;
    } u;
    uint32_t len = 4;
    memset(&u, 0, sizeof u);
    switch (type) {
    case NET_HELLO:
        u.hello.proto_version = GPU_NET_PROTO_VER;
        u.hello.width = GPU_W;
        u.hello.height = GPU_H;
        u.hello.pool_size = GPU_POOL_SIZE;
        u.hello.max_sprites = MAX_SPRITES;
        len = sizeof u.hello;
        break;
    case NET_SPRITE_UPLOAD: len = sizeof u.up; break;
    case NET_WAIT_FRAME:    len = sizeof u.wf; break;
    case NET_STATUS:        len = sizeof u.status; break;
    case NET_SYNC:          len = sizeof u.sync; u.sync.errors_since_last_sync = c->errs_since_sync; break;
    case NET_FRAME_GET:     len = sizeof u.fg; break;
    case NET_READBACK:      len = sizeof u.rb; break;
    default:                len = 4; break;
    }
    u.hello.status = st;            /* status is the first field of every layout */
    reply_bytes(c, type, &u, len);
}

static void client_flush(client *c)
{
    while (!c->dead && c->out_off < c->out_len) {
        long r = os_send(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (r > 0) {
            c->out_off += (size_t)r;
        } else if (r == 0 || r == -OS_EAGAIN || r == -OS_EINTR) {
            break;
        } else {
            char why[96];
            os_snprintf(why, sizeof why, "send failed: %s", os_strerror((int)r));
            kill_client(c, why);
            return;
        }
    }
    if (c->out_off == c->out_len) {
        c->out_off = c->out_len = 0;
        if (c->out_cap > BUF_SHRINK_ABOVE) {
            os_free(c->out);
            c->out = NULL;
            c->out_cap = 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* PL access                                                                                     */
/* ------------------------------------------------------------------------------------------ */
static uint32_t RD(uint32_t off) { return D.be->ops->rd(D.be, off); }
static void WR(uint32_t off, uint32_t v) { D.be->ops->wr(D.be, off, v); }

static void pl_soft_reset(uint32_t ctl)
{
    WR(GPU_R_CONTROL, (ctl & CTL_ALLOWED) | GPU_CTL_SOFT_RESET);
    D.credit = 0;
}

static void pool_reset(void)
{
    memset(D.spr, 0, sizeof D.spr);
    D.pool_next = 0;
}

/* Push n whole records now if the PS FIFO has room for all of them (used at PL init only). */
static int push_now(const uint32_t *recs, int n)
{
    uint32_t f;
    os_io_barrier();
    f = RD(GPU_R_PS_FIFO_FREE);
    if (f > GPU_PS_FIFO_DEPTH)
        f = GPU_PS_FIFO_DEPTH;
    if (f < (uint32_t)n * GPU_REC_WORDS)
        return -1;
    D.be->ops->push(D.be, recs, n);
    D.daemon_records += (uint32_t)n;
    D.credit = 0;
    return 0;
}

static void pl_init(void)
{
    uint32_t rec[GPU_REC_WORDS];
    uint32_t fb0 = RD(GPU_R_FB0), fb1 = RD(GPU_R_FB1), ret = RD(GPU_R_RET_ADDR);

    WR(GPU_R_RET_CTRL, GPU_RET_ACK);                 /* disable capture, release a held frame */
    D.ret_enabled = 0;
    D.cache_valid = 0;
    if (fb0 != GPU_FB0_ADDR || fb1 != GPU_FB1_ADDR || ret != GPU_RET_ADDR) {
        os_log("FB0/FB1/RET_ADDR were 0x%08x/0x%08x/0x%08x; restoring 0x%08x/0x%08x/0x%08x",
               fb0, fb1, ret, GPU_FB0_ADDR, GPU_FB1_ADDR, GPU_RET_ADDR);
        WR(GPU_R_FB0, GPU_FB0_ADDR);
        WR(GPU_R_FB1, GPU_FB1_ADDR);
        WR(GPU_R_RET_ADDR, GPU_RET_ADDR);
    }
    WR(GPU_R_CLEAR_COLOR, STARTUP_CLEAR);
    pl_soft_reset(CTL_DEFAULT);
    gpu_make_end(0, rec);
    if (push_now(rec, 1) == 0)
        os_log("start-up frame queued (clear colour 0x%04x = dark blue: daemon running)", STARTUP_CLEAR);
    else
        os_log("warning: PS FIFO has no room right after a soft reset; start-up frame skipped");
}

static void finish_msg(client *c)
{
    c->exec = 0;
    c->fifo_stalled = 0;
    c->stage_n = c->stage_i = 0;
    c->in_off += HDR_BYTES + (size_t)c->hdr.len;
    if (c->in_off >= c->in_len)
        c->in_off = c->in_len = 0;
}

static void unpark(client *c)
{
    c->park = PARK_NONE;
}

static void pl_lost(uint64_t now, const char *why)
{
    int i;
    os_log("PL lost (%s): GP0 no longer touched; re-probing every 2 s", why);
    D.pl_ok = 0;
    D.credit = 0;
    D.ret_enabled = 0;
    D.cache_valid = 0;
    D.pl_why[0] = 0;
    D.next_probe = now + PROBE_INTERVAL_NS;
    for (i = 0; i < D.ncl; i++) {
        client *c = D.cl[i];
        if (c->exec) {
            client_error(c, "%s aborted: PL lost", msg_name(c->hdr.type));
            finish_msg(c);
        }
        if (c->park) {
            reply_error(c, c->park == PARK_WAIT ? NET_WAIT_FRAME : NET_FRAME_GET, GPU_ERR_NOPL);
            unpark(c);
        }
    }
}

static void probe_now(uint64_t now)
{
    char why[256];
    D.last_probe = now;
    D.next_probe = now + PROBE_INTERVAL_NS;
    why[0] = 0;
    if (D.be->ops->probe(D.be, why, sizeof why)) {
        D.pl_ok = 1;
        D.pl_why[0] = 0;
        os_log("PL ready: %s", why);
        pl_init();
    } else if (strcmp(why, D.pl_why) != 0) {
        os_log("PL not ready: %s -- re-checking every 2 s (HELLO/STATUS answer GPU_ERR_NOPL)", why);
        memcpy(D.pl_why, why, sizeof why);
    }
}

static void maybe_probe_now(void)
{
    uint64_t now;
    if (D.pl_ok)
        return;
    now = os_now_ns();
    if (now - D.last_probe >= PROBE_MIN_GAP_NS)
        probe_now(now);
}

static void pl_maintain(uint64_t now)
{
    if (D.pl_ok) {
        if (!D.be->ops->alive(D.be))
            pl_lost(now, "devcfg PCFG_DONE cleared");
    } else if (now >= D.next_probe) {
        probe_now(now);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* record pushing with PS_FIFO_FREE flow control                                                */
/* ------------------------------------------------------------------------------------------ */
/* 1 = all staged records pushed, 0 = FIFO full (retry later) */
static int flush_stage(client *c)
{
    while (c->stage_i < c->stage_n) {
        int k;
        if (D.credit < GPU_REC_WORDS) {
            uint32_t f;
            os_io_barrier();
            f = RD(GPU_R_PS_FIFO_FREE);
            D.credit = f > GPU_PS_FIFO_DEPTH ? GPU_PS_FIFO_DEPTH : f;
            if (D.credit < GPU_REC_WORDS) {
                if (!c->fifo_stalled) {
                    c->fifo_stalled = 1;
                    D.stall_since = os_now_ns();
                    D.stall_warned = 0;
                }
                return 0;
            }
        }
        k = (int)(D.credit / GPU_REC_WORDS);
        if (k > c->stage_n - c->stage_i)
            k = c->stage_n - c->stage_i;
        D.be->ops->push(D.be, c->stage[c->stage_i], k);
        D.credit -= (uint32_t)k * GPU_REC_WORDS;
        D.daemon_records += (uint32_t)k;
        c->stage_i += k;
    }
    c->fifo_stalled = 0;
    return 1;
}

/* A raw SPRITE record (NET_RECORDS) makes the PL read DDR at the address in w3. Only the GPU's
 * reserved window may be read that way; any other address would let a network client read kernel
 * or user memory back through READBACK / FRAME_GET. The range is exactly what core_sprite.v reads:
 * x and w used as multiples of 4, src and stride as multiples of 8, rows clipped at the bottom
 * screen edge, columns at the right edge; a record that draws nothing reads nothing. */
static int raw_sprite_ok(const uint32_t *r, uint64_t *src_out, uint64_t *end_out)
{
    uint32_t x = r[1] & 0x7FCu, y = (r[1] >> 11) & 0x7FFu;
    uint32_t w = r[2] & 0x7FCu, h = (r[2] >> 11) & 0x7FFu;
    uint64_t src = r[3] & ~7u, stride = r[4] & ~7u, rows, cols, end;
    *src_out = *end_out = src;
    if (x >= GPU_W || y >= GPU_H || w == 0 || h == 0)
        return 1;
    rows = h < GPU_H - y ? h : GPU_H - y;
    cols = w < GPU_W - x ? w : GPU_W - x;
    end = src + (rows - 1) * stride + cols * 2u;
    *end_out = end;
    return src >= GPU_DDR_BASE && end <= (uint64_t)GPU_DDR_BASE + GPU_DDR_SIZE;
}

/* Turn item c->item of the current drawing message into records in c->stage. */
static void gen_item(client *c, const uint8_t *p)
{
    int n = 0;
    uint32_t i = c->item++;

    switch (c->hdr.type) {
    case NET_TRIS: {
        net_tri t;
        gpu_vtx v[3];
        memcpy(&t, p + 4 + (size_t)i * sizeof(net_tri), sizeof t);
        if (t.cull > GPU_CULL_CCW) {
            client_error(c, "TRIS: triangle %u has cull %u (0..2)", i, t.cull);
            break;
        }
        memcpy(v, t.v, sizeof v);
        n = gpu_setup_tri(v, t.flags & (GPU_F_ZTEST | GPU_F_ZWRITE), (int)t.cull, c->stage,
                          GPU_SETUP_MAX_OUT);
        break;
    }
    case NET_RECT: {
        net_rect r;
        memcpy(&r, p, sizeof r);
        if (r.rgb565 > 0xFFFFu) {
            client_error(c, "RECT: colour 0x%x is not RGB565", r.rgb565);
            break;
        }
        n = gpu_setup_rect(r.x0, r.y0, r.x1, r.y1, (uint16_t)r.rgb565, r.z,
                           r.flags & (GPU_F_ZTEST | GPU_F_ZWRITE), c->stage[0]);
        break;
    }
    case NET_SPRITE_DRAW: {
        net_sprite_draw d;
        const sprite_slot *s;
        memcpy(&d, p, sizeof d);
        if (d.id >= MAX_SPRITES || !D.spr[d.id].valid) {
            client_error(c, "SPRITE_DRAW: sprite %u was not uploaded", d.id);
            break;
        }
        s = &D.spr[d.id];
        n = gpu_setup_sprite(d.x, d.y, (int)s->w, (int)s->h, s->addr, s->stride, (d.flags & 1u) != 0,
                             (uint16_t)d.colorkey, c->stage[0]);
        if (n < 0) {
            client_error(c, "SPRITE_DRAW: sprite %u at x=%d y=%d rejected (x must be a multiple of 4)",
                         d.id, d.x, d.y);
            n = 0;
        }
        break;
    }
    case NET_RECORDS: {
        uint64_t a0, a1;
        memcpy(c->stage[0], p + 4 + (size_t)i * GPU_REC_BYTES, GPU_REC_BYTES);
        if (GPU_W0_TYPE(c->stage[0][0]) == GPU_REC_SPRITE && !raw_sprite_ok(c->stage[0], &a0, &a1)) {
            client_error(c, "RECORDS: record %u is a SPRITE reading 0x%llx..0x%llx, outside the GPU "
                         "DDR window 0x%08x..0x%08x -- dropped", i, (unsigned long long)a0,
                         (unsigned long long)a1, GPU_DDR_BASE, GPU_DDR_BASE + GPU_DDR_SIZE);
            break;
        }
        n = 1;
        break;
    }
    case NET_END_FRAME:
        gpu_make_end(le32(p), c->stage[0]);
        n = 1;
        break;
    default:
        break;
    }
    c->stage_n = n > 0 ? n : 0;
    c->stage_i = 0;
}

/* Drawing messages (no reply). 1 = done, 0 = stalled on the PS FIFO. */
static int exec_draw(client *c, const uint8_t *p, uint32_t len)
{
    unsigned t = c->hdr.type;
    if (c->nitems == UINT32_MAX) {
        uint32_t count = 0;
        int ok;
        switch (t) {
        case NET_TRIS:
            ok = len >= 4 && (count = le32(p)) <= (len - 4) / sizeof(net_tri) &&
                 len == 4 + count * (uint32_t)sizeof(net_tri);
            break;
        case NET_RECORDS:
            ok = len >= 4 && (count = le32(p)) <= (len - 4) / GPU_REC_BYTES &&
                 len == 4 + count * (uint32_t)GPU_REC_BYTES;
            break;
        case NET_RECT:        ok = len == sizeof(net_rect); count = 1; break;
        case NET_SPRITE_DRAW: ok = len == sizeof(net_sprite_draw); count = 1; break;
        case NET_END_FRAME:   ok = len == 4; count = 1; break;
        default:              ok = 0; break;
        }
        if (!ok) {
            client_error(c, "%s: malformed (payload %u bytes) -- dropped", msg_name(t), len);
            return 1;
        }
        if (!D.pl_ok) {
            client_error(c, "%s dropped: PL not ready (%s)", msg_name(t), D.pl_why);
            return 1;
        }
        c->nitems = count;
    }
    for (;;) {
        if (c->stage_i < c->stage_n && !flush_stage(c))
            return 0;
        if (c->item >= c->nitems)
            return 1;
        gen_item(c, p);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* frame return (FRAME_GET) and parked requests                                                 */
/* ------------------------------------------------------------------------------------------ */
static int ret_region_ok(uint32_t a)
{
    return a >= GPU_DDR_BASE && a - GPU_DDR_BASE <= GPU_DDR_SIZE - GPU_FB_BYTES && (a & 7u) == 0;
}

static int fg_cache_serves(const net_frame_get *g)
{
    return D.cache_valid && (int32_t)(D.cache_frame - g->min_frame_no) >= 0 &&
           (g->scale == 2 || D.cache_mode == 1);
}

static void fg_reply_from_cache(client *c, const net_frame_get *g)
{
    uint32_t w = GPU_W / g->scale, h = GPU_H / g->scale, x, y;
    uint8_t *p = reply_begin(c, NET_FRAME_GET, (uint32_t)sizeof(net_frame_get_reply) + w * h * 2u);
    net_frame_get_reply r;
    if (!p)
        return;
    r.status = GPU_ERR_OK;
    r.frame_no = D.cache_frame;
    r.w = w;
    r.h = h;
    memcpy(p, &r, sizeof r);
    p += sizeof r;
    if (g->scale == 1) {
        memcpy(p, D.cache, GPU_FB_BYTES);
    } else {
        for (y = 0; y < h; y++) {
            const uint16_t *src = D.cache + (size_t)(2 * y) * GPU_W;
            for (x = 0; x < w; x++) {
                uint16_t px = src[2 * x];
                memcpy(p + ((size_t)y * w + x) * 2u, &px, 2);
            }
        }
    }
}

/* RET_ENABLE is on while a FRAME_GET is parked and for RET_GRACE_NS after the last frame was
 * served (so a viewer looping FRAME_GET does not miss every other frame). Switching it off also
 * releases a held frame and drops the cache, so a later FRAME_GET never gets a stale frame. */
static void ret_set_enable(int want)
{
    if (want == D.ret_enabled)
        return;
    if (want) {
        /* ACK too: a frame captured before capture was last switched off may still be held
         * (RET_ENABLE = 0 lets a capture in progress complete) and must not satisfy a new
         * request. A capture in progress now is of a frame being rendered now and completes. */
        WR(GPU_R_RET_CTRL, GPU_RET_ENABLE | GPU_RET_ACK);
    } else {
        WR(GPU_R_RET_CTRL, GPU_RET_ACK);
        D.cache_valid = 0;
    }
    D.ret_enabled = want;
}

static void fget_service(uint64_t now, int any_fget)
{
    uint32_t st, fno, addr;
    int i, any_sat = 0, need_full = 0;

    ret_set_enable(any_fget || now < D.ret_grace_until);
    if (!any_fget)
        return;
    st = RD(GPU_R_RET_STATUS);
    if (!(st & GPU_RET_FULL))
        return;
    fno = RD(GPU_R_RET_FRAME);
    for (i = 0; i < D.ncl; i++) {
        client *c = D.cl[i];
        if (c->park == PARK_FGET && (int32_t)(fno - c->fg.min_frame_no) >= 0) {
            any_sat = 1;
            if (c->fg.scale == 1)
                need_full = 1;
        }
    }
    if (!any_sat) {
        WR(GPU_R_RET_CTRL, (D.ret_enabled ? GPU_RET_ENABLE : 0u) | GPU_RET_ACK);   /* too old */
        return;
    }
    addr = RD(GPU_R_RET_ADDR);
    if (!ret_region_ok(addr)) {
        os_log("RET_ADDR 0x%08x is outside the reserved window; FRAME_GET cannot be served", addr);
        WR(GPU_R_RET_CTRL, (D.ret_enabled ? GPU_RET_ENABLE : 0u) | GPU_RET_ACK);
        return;
    }
    if (need_full) {
        D.be->ops->ddr_read(D.be, addr, D.cache, GPU_FB_BYTES);
    } else {
        uint32_t y;
        for (y = 0; y < GPU_H; y += 2)
            D.be->ops->ddr_read(D.be, addr + y * GPU_W * 2u, D.cache + (size_t)y * GPU_W, GPU_W * 2u);
    }
    D.cache_valid = 1;
    D.cache_mode = need_full ? 1 : 2;
    D.cache_frame = fno;
    WR(GPU_R_RET_CTRL, (D.ret_enabled ? GPU_RET_ENABLE : 0u) | GPU_RET_ACK);  /* PL captures on */
    D.ret_grace_until = now + RET_GRACE_NS;
    for (i = 0; i < D.ncl; i++) {
        client *c = D.cl[i];
        if (c->park == PARK_FGET && fg_cache_serves(&c->fg)) {
            fg_reply_from_cache(c, &c->fg);
            unpark(c);
        }
    }
}

static void park_poll(uint64_t now)
{
    int i, any_wait = 0, any_fget = 0;
    uint32_t fc = 0;

    for (i = 0; i < D.ncl; i++) {
        if (D.cl[i]->park == PARK_WAIT)
            any_wait = 1;
        else if (D.cl[i]->park == PARK_FGET)
            any_fget = 1;
    }
    if (!D.pl_ok)
        return;                         /* parked requests were answered by pl_lost() */
    if (!any_wait && !any_fget && !D.ret_enabled && !D.cache_valid)
        return;
    if (now < D.next_park_poll)
        return;
    D.next_park_poll = now + PARK_POLL_NS;

    if (any_wait) {
        fc = RD(GPU_R_FRAME_COUNT);
        for (i = 0; i < D.ncl; i++) {
            client *c = D.cl[i];
            net_wait_frame_reply r;
            if (c->park != PARK_WAIT)
                continue;
            if ((int32_t)(fc - c->wait_target) >= 0)
                r.status = GPU_ERR_OK;
            else if (now >= c->deadline)
                r.status = GPU_ERR_TIMEOUT;
            else
                continue;
            r.frame_count = fc;
            reply_bytes(c, NET_WAIT_FRAME, &r, sizeof r);
            unpark(c);
        }
    }
    fget_service(now, any_fget);
    for (i = 0; i < D.ncl; i++) {
        client *c = D.cl[i];
        if (c->park == PARK_FGET && now >= c->deadline) {
            reply_error(c, NET_FRAME_GET, GPU_ERR_TIMEOUT);
            unpark(c);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* message handlers                                                                              */
/* ------------------------------------------------------------------------------------------ */
static void demote(client *o, const client *by)
{
    o->role = ROLE_OBSERVER;
    os_log("client %d (%s) demoted to observer: client %d took control", o->id, o->peer, by->id);
    if (o->exec) {
        client_error(o, "control taken over by client %d: rest of %s dropped", by->id,
                     msg_name(o->hdr.type));
        finish_msg(o);
    }
}

static void do_hello(client *c, const uint8_t *p, uint32_t len)
{
    net_hello_reply r;
    uint32_t flags;
    if (len != 0 && len != 4) {
        client_error(c, "HELLO: payload %u bytes (0 or 4)", len);
        reply_error(c, NET_HELLO, GPU_ERR_PROTO);
        return;
    }
    flags = len == 4 ? le32(p) : 0;
    maybe_probe_now();
    if (flags & GPU_HELLO_OBSERVER) {
        if (D.ctrl == c) {
            D.ctrl = NULL;
            os_log("client %d (%s) gave up control", c->id, c->peer);
        }
        c->role = ROLE_OBSERVER;
        os_log("client %d (%s) is an observer", c->id, c->peer);
    } else {
        if (D.ctrl && D.ctrl != c)
            demote(D.ctrl, c);
        D.ctrl = c;
        c->role = ROLE_CONTROLLER;
        pool_reset();
        if (D.pl_ok)
            pl_soft_reset(CTL_DEFAULT);
        os_log("client %d (%s) is the controller (soft reset, CONTROL=0x%x, sprite pool reset)",
               c->id, c->peer, CTL_DEFAULT);
    }
    memset(&r, 0, sizeof r);
    r.status = D.pl_ok ? GPU_ERR_OK : GPU_ERR_NOPL;
    r.proto_version = GPU_NET_PROTO_VER;
    r.pl_id = D.pl_ok ? RD(GPU_R_ID) : 0;
    r.pl_version = D.pl_ok ? RD(GPU_R_VERSION) : 0;
    r.width = GPU_W;
    r.height = GPU_H;
    r.pool_size = GPU_POOL_SIZE;
    r.max_sprites = MAX_SPRITES;
    reply_bytes(c, NET_HELLO, &r, sizeof r);
}

static void do_set_config(client *c, const uint8_t *p, uint32_t len)
{
    net_set_config s;
    if (len != sizeof s) {
        client_error(c, "SET_CONFIG: payload %u bytes (%u)", len, (unsigned)sizeof s);
        reply_status(c, NET_SET_CONFIG, GPU_ERR_PROTO);
        return;
    }
    memcpy(&s, p, sizeof s);
    if ((s.control & ~(uint32_t)CTL_ALLOWED) || s.clear_color > 0xFFFFu) {
        client_error(c, "SET_CONFIG: control 0x%x / clear 0x%x out of range", s.control, s.clear_color);
        reply_status(c, NET_SET_CONFIG, GPU_ERR_ARG);
        return;
    }
    if (!D.pl_ok) {
        reply_status(c, NET_SET_CONFIG, GPU_ERR_NOPL);
        return;
    }
    WR(GPU_R_CLEAR_COLOR, s.clear_color);
    WR(GPU_R_CONTROL, s.control);
    if (D.verbose)
        os_log("client %d: CONTROL=0x%x CLEAR_COLOR=0x%04x", c->id, s.control, s.clear_color);
    reply_status(c, NET_SET_CONFIG, GPU_ERR_OK);
}

static void do_sprite_upload(client *c, const uint8_t *p, uint32_t len)
{
    net_sprite_upload u;
    net_sprite_upload_reply r;
    sprite_slot *s;
    uint64_t size64;
    uint32_t stride, size, addr, off;
    int e;

    memset(&r, 0, sizeof r);
    if (len < sizeof u) {
        client_error(c, "SPRITE_UPLOAD: payload %u bytes", len);
        reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_PROTO);
        return;
    }
    memcpy(&u, p, sizeof u);
    if ((uint64_t)len != sizeof u + (uint64_t)u.w * u.h * 2u) {
        client_error(c, "SPRITE_UPLOAD: %ux%u needs %llu payload bytes, got %u", u.w, u.h,
                     (unsigned long long)(sizeof u + (uint64_t)u.w * u.h * 2u), len);
        reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_PROTO);
        return;
    }
    if (u.id >= MAX_SPRITES || u.w < 4 || (u.w % 4) != 0 || u.h < 1) {
        client_error(c, "SPRITE_UPLOAD: id %u w %u h %u invalid (id < 256, w multiple of 4 >= 4, h >= 1)",
                     u.id, u.w, u.h);
        reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_ARG);
        return;
    }
    if (!D.pl_ok) {
        reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_NOPL);
        return;
    }
    stride = ((uint32_t)u.w * 2u + 7u) & ~7u;
    size64 = (uint64_t)stride * u.h;
    s = &D.spr[u.id];
    if (s->valid && size64 <= s->cap) {
        addr = s->addr;                                   /* re-upload that fits: same slot */
    } else {
        off = (D.pool_next + 7u) & ~7u;
        if (size64 > GPU_POOL_SIZE || off > GPU_POOL_SIZE - (uint32_t)size64) {
            client_error(c, "SPRITE_UPLOAD: sprite %u (%llu bytes) does not fit in the pool (%u of %u used)",
                         u.id, (unsigned long long)size64, D.pool_next, GPU_POOL_SIZE);
            reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_NOMEM);
            return;
        }
        addr = GPU_POOL_ADDR + off;
        D.pool_next = off + (uint32_t)size64;
        s->cap = (uint32_t)size64;
    }
    size = (uint32_t)size64;
    if (stride == (uint32_t)u.w * 2u) {
        e = D.be->ops->ddr_write(D.be, addr, p + sizeof u, size);
    } else {                                              /* cannot happen for w % 4 == 0 */
        uint32_t row;
        e = 0;
        for (row = 0; row < u.h && !e; row++)
            e = D.be->ops->ddr_write(D.be, addr + row * stride, p + sizeof u + (size_t)row * u.w * 2u,
                                     (size_t)u.w * 2u);
    }
    if (e < 0) {
        client_error(c, "SPRITE_UPLOAD: DDR write at 0x%08x failed: %s", addr, os_strerror(e));
        reply_error(c, NET_SPRITE_UPLOAD, GPU_ERR_ARG);
        return;
    }
    s->addr = addr;
    s->w = u.w;
    s->h = u.h;
    s->stride = stride;
    s->valid = 1;
    r.status = GPU_ERR_OK;
    r.ddr_addr = addr;
    r.stride = stride;
    if (D.verbose)
        os_log("client %d: sprite %u %ux%u at 0x%08x stride %u", c->id, u.id, u.w, u.h, addr, stride);
    reply_bytes(c, NET_SPRITE_UPLOAD, &r, sizeof r);
}

static void do_wait_frame(client *c, const uint8_t *p, uint32_t len)
{
    net_wait_frame w;
    net_wait_frame_reply r;
    if (len != sizeof w) {
        client_error(c, "WAIT_FRAME: payload %u bytes (%u)", len, (unsigned)sizeof w);
        reply_error(c, NET_WAIT_FRAME, GPU_ERR_PROTO);
        return;
    }
    if (!D.pl_ok) {
        reply_error(c, NET_WAIT_FRAME, GPU_ERR_NOPL);
        return;
    }
    memcpy(&w, p, sizeof w);
    r.frame_count = RD(GPU_R_FRAME_COUNT);
    if ((int32_t)(r.frame_count - w.target_frame_count) >= 0) {
        r.status = GPU_ERR_OK;
        reply_bytes(c, NET_WAIT_FRAME, &r, sizeof r);
        return;
    }
    c->park = PARK_WAIT;
    c->wait_target = w.target_frame_count;
    c->deadline = os_now_ns() + (uint64_t)w.timeout_ms * NS_MS;
}

static void do_status(client *c, uint32_t len)
{
    net_status_reply r;
    int i;
    if (len != 0) {
        client_error(c, "STATUS: payload %u bytes (0)", len);
        reply_error(c, NET_STATUS, GPU_ERR_PROTO);
        return;
    }
    maybe_probe_now();
    memset(&r, 0, sizeof r);
    r.status = D.pl_ok ? GPU_ERR_OK : GPU_ERR_NOPL;
    if (D.pl_ok)
        for (i = 0; i < NET_STATUS_NREGS; i++)
            r.regs[i] = RD((uint32_t)i * 4u);
    r.daemon_errors = D.daemon_errors;
    r.daemon_records = D.daemon_records;
    r.pool_used = D.pool_next;
    reply_bytes(c, NET_STATUS, &r, sizeof r);
}

static void do_readback(client *c, uint32_t len)
{
    uint32_t hdr[3], addr = 0, fc0, fc1 = 0;
    uint8_t *p;
    int tries;
    if (len != 0) {
        client_error(c, "READBACK: payload %u bytes (0)", len);
        reply_error(c, NET_READBACK, GPU_ERR_PROTO);
        return;
    }
    if (!D.pl_ok) {
        reply_error(c, NET_READBACK, GPU_ERR_NOPL);
        return;
    }
    p = reply_begin(c, NET_READBACK, 12u + GPU_FB_BYTES);
    if (!p)
        return;
    for (tries = 0; tries < READBACK_TRIES; tries++) {
        fc0 = RD(GPU_R_FRAME_COUNT);
        addr = RD((RD(GPU_R_FRONT) & 1u) ? GPU_R_FB1 : GPU_R_FB0);
        if (!ret_region_ok(addr)) {
            /* undo the reservation, answer with an error instead */
            c->out_len -= HDR_BYTES + 12u + GPU_FB_BYTES;
            client_error(c, "READBACK: front buffer address 0x%08x outside the reserved window", addr);
            reply_error(c, NET_READBACK, GPU_ERR_ARG);
            return;
        }
        D.be->ops->ddr_read(D.be, addr, p + 12, GPU_FB_BYTES);
        fc1 = RD(GPU_R_FRAME_COUNT);
        if (fc1 == fc0)
            break;                      /* no swap during the copy: a consistent frame */
    }
    if (fc1 != fc0 && D.verbose)
        os_log("client %d: READBACK raced %d buffer swaps; returning the last copy", c->id, tries);
    hdr[0] = (uint32_t)GPU_ERR_OK;
    hdr[1] = GPU_W;
    hdr[2] = GPU_H;
    memcpy(p, hdr, 12);
}

static void do_reset(client *c, uint32_t len)
{
    if (len != 0) {
        client_error(c, "RESET: payload %u bytes (0)", len);
        reply_status(c, NET_RESET, GPU_ERR_PROTO);
        return;
    }
    pool_reset();
    if (!D.pl_ok) {
        reply_status(c, NET_RESET, GPU_ERR_NOPL);
        return;
    }
    pl_soft_reset(RD(GPU_R_CONTROL));
    os_log("client %d: RESET (soft reset, sprite pool reset)", c->id);
    reply_status(c, NET_RESET, GPU_ERR_OK);
}

static void do_sync(client *c, uint32_t len)
{
    net_sync_reply r;
    if (len != 0)
        client_error(c, "SYNC: payload %u bytes (0)", len);
    r.status = D.pl_ok ? GPU_ERR_OK : GPU_ERR_NOPL;
    r.errors_since_last_sync = c->errs_since_sync;
    c->errs_since_sync = 0;
    reply_bytes(c, NET_SYNC, &r, sizeof r);
}

static void do_frame_get(client *c, const uint8_t *p, uint32_t len)
{
    net_frame_get g;
    if (len != sizeof g) {
        client_error(c, "FRAME_GET: payload %u bytes (%u)", len, (unsigned)sizeof g);
        reply_error(c, NET_FRAME_GET, GPU_ERR_PROTO);
        return;
    }
    memcpy(&g, p, sizeof g);
    if (g.scale != 1 && g.scale != 2) {
        client_error(c, "FRAME_GET: scale %u (1 or 2)", g.scale);
        reply_error(c, NET_FRAME_GET, GPU_ERR_ARG);
        return;
    }
    if (!D.pl_ok) {
        reply_error(c, NET_FRAME_GET, GPU_ERR_NOPL);
        return;
    }
    if (fg_cache_serves(&g)) {
        fg_reply_from_cache(c, &g);
        return;
    }
    c->fg = g;
    c->park = PARK_FGET;
    c->deadline = os_now_ns() + (uint64_t)g.timeout_ms * NS_MS;
    ret_set_enable(1);
    D.next_park_poll = 0;              /* look at RET_STATUS right away */
}

/* Execute (or resume) the message at c->in + c->in_off. 1 = finished, 0 = stalled. */
static int exec_msg(client *c)
{
    const uint8_t *p = c->in + c->in_off + HDR_BYTES;
    uint32_t len = c->hdr.len;
    unsigned t = c->hdr.type;

    if (c->nitems == UINT32_MAX && known_type(t) && !observer_allowed(t) && c->role != ROLE_CONTROLLER) {
        client_error(c, "%s rejected: this client is %s (send NET_HELLO without the observer flag "
                     "to become the controller)", msg_name(t), role_name(c->role));
        if (has_reply(t))
            reply_error(c, t, GPU_ERR_PROTO);
        return 1;
    }
    switch (t) {
    case NET_HELLO:         do_hello(c, p, len); return 1;
    case NET_SET_CONFIG:    do_set_config(c, p, len); return 1;
    case NET_SPRITE_UPLOAD: do_sprite_upload(c, p, len); return 1;
    case NET_WAIT_FRAME:    do_wait_frame(c, p, len); return 1;
    case NET_STATUS:        do_status(c, len); return 1;
    case NET_READBACK:      do_readback(c, len); return 1;
    case NET_RESET:         do_reset(c, len); return 1;
    case NET_SYNC:          do_sync(c, len); return 1;
    case NET_FRAME_GET:     do_frame_get(c, p, len); return 1;
    case NET_TRIS:
    case NET_RECT:
    case NET_SPRITE_DRAW:
    case NET_RECORDS:
    case NET_END_FRAME:
        return exec_draw(c, p, len);
    default:
        client_error(c, "unknown message type 0x%04x (%u payload bytes)", t, len);
        reply_status(c, t & ~GPU_MSG_REPLY, GPU_ERR_PROTO);
        return 1;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* clients                                                                                       */
/* ------------------------------------------------------------------------------------------ */
static int ensure_in_room(client *c, size_t need_total)
{
    size_t have = c->in_len - c->in_off;
    if (c->in_off) {
        memmove(c->in, c->in + c->in_off, have);
        c->in_len = have;
        c->in_off = 0;
    }
    if (need_total > c->in_cap) {
        size_t cap = need_total < IN_MIN_CAP ? IN_MIN_CAP : need_total;
        uint8_t *p = os_realloc(c->in, cap);
        if (!p) {
            kill_client(c, "out of memory for a message");
            return -1;
        }
        c->in = p;
        c->in_cap = cap;
    }
    return 0;
}

/* Execute as many complete messages as possible. Returns 1 if anything happened. */
static int client_run(client *c)
{
    int did = 0;
    if (c->dead)
        return 0;
    if (c->exec) {
        if (!exec_msg(c)) {
            client_flush(c);
            return 0;
        }
        finish_msg(c);
        did = 1;
    }
    while (!c->dead && !c->park && !c->exec) {
        size_t avail = c->in_len - c->in_off;
        if (c->out_len - c->out_off >= OUT_HIGH_WATER)
            break;                      /* the client is not reading its replies: wait */
        if (avail < HDR_BYTES)
            break;
        memcpy(&c->hdr, c->in + c->in_off, HDR_BYTES);
        if (c->hdr.magic != GPU_NET_MAGIC) {
            char why[80];
            D.daemon_errors++;
            os_snprintf(why, sizeof why, "bad magic 0x%08x (protocol error)", c->hdr.magic);
            kill_client(c, why);
            return 1;
        }
        if (c->hdr.len > GPU_MSG_MAX_PAYLOAD) {
            char why[96];
            D.daemon_errors++;
            os_snprintf(why, sizeof why, "%s with %u payload bytes (max %u)", msg_name(c->hdr.type),
                        c->hdr.len, GPU_MSG_MAX_PAYLOAD);
            kill_client(c, why);
            return 1;
        }
        if (avail < HDR_BYTES + (size_t)c->hdr.len) {
            if (HDR_BYTES + (size_t)c->hdr.len > c->in_cap - c->in_off)
                ensure_in_room(c, HDR_BYTES + (size_t)c->hdr.len);
            break;
        }
        c->exec = 1;
        c->item = 0;
        c->nitems = UINT32_MAX;
        c->stage_n = c->stage_i = 0;
        c->msgs++;
        did = 1;
        if (!exec_msg(c))
            break;                      /* stalled on the PS FIFO; resumed later */
        finish_msg(c);
    }
    client_flush(c);
    if (!c->dead && !c->exec && !c->park && c->in_off == c->in_len && c->in_cap > BUF_SHRINK_ABOVE) {
        os_free(c->in);                 /* release the buffer of a big message */
        c->in = NULL;
        c->in_cap = c->in_len = c->in_off = 0;
        ensure_in_room(c, IN_MIN_CAP);
    }
    if (c->eof && !c->dead && !c->exec && !c->park && c->out_off == c->out_len) {
        size_t avail = c->in_len - c->in_off;
        int complete = 0;
        if (avail >= HDR_BYTES) {
            gpu_msg_hdr h;
            memcpy(&h, c->in + c->in_off, HDR_BYTES);
            complete = avail >= HDR_BYTES + (size_t)h.len;
        }
        if (!complete) {
            if (avail)
                os_log("client %d (%s): disconnected with %zu bytes of an incomplete message",
                       c->id, c->peer, avail);
            kill_client(c, "disconnected");
        }
    }
    return did;
}

static void client_read(client *c, uint64_t now)
{
    for (;;) {
        long r;
        if (c->dead || c->eof)
            return;
        if (c->in_len == c->in_cap) {
            if (c->in_off == 0)
                return;                 /* full of unprocessed data: back-pressure */
            ensure_in_room(c, c->in_cap);
        }
        r = os_recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (r > 0) {
            c->in_len += (size_t)r;
            c->last_rx = now;
            if (c->in_len < c->in_cap)
                return;
        } else if (r == 0) {
            c->eof = 1;
            return;
        } else if (r == -OS_EAGAIN || r == -OS_EINTR) {
            return;
        } else {
            char why[96];
            os_snprintf(why, sizeof why, "receive failed: %s", os_strerror((int)r));
            kill_client(c, why);
            return;
        }
    }
}

static void client_free(client *c)
{
    if (D.ctrl == c) {
        D.ctrl = NULL;
        os_log("client %d: the controller left (PL keeps its state; the next HELLO takes over)", c->id);
    }
    os_close(c->fd);
    os_free(c->in);
    os_free(c->out);
    os_log("client %d (%s) closed after %llu messages", c->id, c->peer, (unsigned long long)c->msgs);
    os_free(c);
}

static void reap_clients(void)
{
    int i, j = 0;
    for (i = 0; i < D.ncl; i++) {
        if (D.cl[i]->dead)
            client_free(D.cl[i]);
        else
            D.cl[j++] = D.cl[i];
    }
    D.ncl = j;
}

static void accept_clients(uint64_t now)
{
    for (;;) {
        char peer[48];
        client *c;
        int fd = os_accept(D.lfd, peer, sizeof peer);
        if (fd < 0) {
            if (fd != -OS_EAGAIN && fd != -OS_EINTR)
                os_log("accept failed: %s", os_strerror(fd));
            return;
        }
        if (D.ncl >= MAX_CLIENTS) {
            /* evict the non-controller that has been silent longest (e.g. a half-open connection) */
            int i, victim = -1;
            for (i = 0; i < D.ncl; i++)
                if (D.cl[i] != D.ctrl && (victim < 0 || D.cl[i]->last_rx < D.cl[victim]->last_rx))
                    victim = i;
            if (victim < 0)
                victim = 0;
            os_log("%d clients connected; closing client %d to admit %s", MAX_CLIENTS,
                   D.cl[victim]->id, peer);
            kill_client(D.cl[victim], "evicted for a new connection");
            reap_clients();
        }
        c = os_calloc(sizeof *c);
        if (!c) {
            os_log("out of memory for a client; dropping %s", peer);
            os_close(fd);
            continue;
        }
        c->fd = fd;
        c->id = ++D.next_id;
        c->last_rx = now;
        memcpy(c->peer, peer, sizeof peer);
        if (ensure_in_room(c, IN_MIN_CAP) < 0) {
            os_close(fd);
            os_free(c);
            continue;
        }
        os_tune_socket(fd, SOCK_BUF_BYTES);
        D.cl[D.ncl++] = c;
        os_log("client %d connected from %s (%d/%d)", c->id, peer, D.ncl, MAX_CLIENTS);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* main                                                                                          */
/* ------------------------------------------------------------------------------------------ */
static int parse_int(const char *s, int lo, int hi, int *out)
{
    long v = 0;
    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > hi)
            return -1;
    }
    if (v < lo)
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(void)
{
    os_log("usage: fpgagpud [--sim] [--port N] [--bind IP] [--bus-port N] [--sim-nopl]\n"
           "                [--sim-pl-delay-ms N] [--fake-devmem FILE] [--cmdline FILE] [--iomem FILE] [-v]\n"
           "  (no option)        real PL through /dev/mem (run as root)\n"
           "  --sim              software PL model; simulated Teensy bus on --bus-port (7778)\n"
           "  --port N           Pi protocol TCP port (7777)\n"
           "  --bind IP          listen address (0.0.0.0 = every address: 10.20.0.2, DHCP, 10.77.0.2)\n"
           "  --sim-nopl         sim: behave as if no bitstream were loaded\n"
           "  --sim-pl-delay-ms  sim: the PL becomes ready N ms after start\n"
           "  --fake-devmem FILE hardware backend on a file instead of /dev/mem (tests)\n"
           "  --cmdline FILE     where to look for fpgagpu.pl_loaded=1 (/proc/cmdline; tests)\n"
           "  --iomem FILE       where to check that the DDR window is not System RAM (/proc/iomem; tests)\n"
           "  -v                 log every configuration change / sprite upload");
}

int main(int argc, char **argv)
{
    int port = GPU_NET_PORT, sim = 0, i;
    const char *bind_ip = "0.0.0.0", *fake = NULL, *cmdline = NULL, *iomem = NULL;
    sim_opts so;

    memset(&so, 0, sizeof so);
    so.bus_port = 7778;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--sim")) {
            sim = 1;
        } else if (!strcmp(a, "--sim-nopl")) {
            so.nopl = 1;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            D.verbose = 1;
        } else if (!strcmp(a, "--port") && v && parse_int(v, 1, 65535, &port) == 0) {
            i++;
        } else if (!strcmp(a, "--bus-port") && v && parse_int(v, 0, 65535, &so.bus_port) == 0) {
            i++;
        } else if (!strcmp(a, "--sim-pl-delay-ms") && v && parse_int(v, 0, 3600000, &so.pl_delay_ms) == 0) {
            i++;
        } else if (!strcmp(a, "--bind") && v) {
            bind_ip = v;
            i++;
        } else if (!strcmp(a, "--fake-devmem") && v) {
            fake = v;
            i++;
        } else if (!strcmp(a, "--cmdline") && v) {
            cmdline = v;
            i++;
        } else if (!strcmp(a, "--iomem") && v) {
            iomem = v;
            i++;
        } else if (!strcmp(a, "--version")) {
            os_log("version " FPGAGPUD_VERSION " (protocol %u)", GPU_NET_PROTO_VER);
            return 0;
        } else {
            usage();
            return !strcmp(a, "-h") || !strcmp(a, "--help") ? 0 : 2;
        }
    }
    os_ignore_sigpipe();
    so.bind_ip = bind_ip;
    D.be = sim ? backend_sim_create(&so) : backend_hw_create(fake, cmdline, iomem);
    if (!D.be)
        return 1;
    D.cache = os_malloc(GPU_FB_BYTES);
    if (!D.cache) {
        os_log("out of memory");
        return 1;
    }
    D.lfd = os_tcp_listen(bind_ip, port);
    if (D.lfd < 0) {
        os_log("cannot listen on %s:%d: %s", bind_ip, port, os_strerror(D.lfd));
        return 1;
    }
    os_log("fpgagpud " FPGAGPUD_VERSION " listening on %s:%d (protocol %u, %s, max %d clients)",
           bind_ip, port, GPU_NET_PROTO_VER, D.be->name, MAX_CLIENTS);
    probe_now(os_now_ns());

    for (;;) {
        struct os_pollfd pfd[1 + MAX_CLIENTS + 4];
        int n = 0, nbe, r, any_stall = 0, any_park = 0;
        int64_t timeout;
        uint64_t now = os_now_ns();

        pl_maintain(now);
        park_poll(now);
        for (i = 0; i < D.ncl; i++)
            client_run(D.cl[i]);
        reap_clients();

        now = os_now_ns();
        for (i = 0; i < D.ncl; i++) {
            if (D.cl[i]->fifo_stalled)
                any_stall = 1;
            if (D.cl[i]->park)
                any_park = 1;
        }
        if (any_stall && !D.stall_warned && now - D.stall_since > STALL_WARN_NS) {
            uint32_t st = D.pl_ok ? RD(GPU_R_STATUS) : 0;
            os_log("PS FIFO full for %llu ms (STATUS=0x%02x%s); the controller's messages wait",
                   (unsigned long long)((now - D.stall_since) / NS_MS), st,
                   (st & GPU_ST_WAIT_TEENSY) ? ": collector waits for the Teensy END" : "");
            D.stall_warned = 1;
        }

        pfd[n].fd = D.lfd;
        pfd[n].events = OS_POLLIN;
        pfd[n].revents = 0;
        n++;
        for (i = 0; i < D.ncl; i++) {
            client *c = D.cl[i];
            pfd[n].events = 0;
            pfd[n].revents = 0;
            if (!c->eof && (c->in_len < c->in_cap || c->in_off > 0))
                pfd[n].events |= OS_POLLIN;
            if (c->out_off < c->out_len)
                pfd[n].events |= OS_POLLOUT;
            /* nothing wanted (input buffer full, or EOF seen): leave it out of the poll set so
             * a hung-up socket's POLLHUP cannot make the loop spin */
            pfd[n].fd = pfd[n].events ? c->fd : -1;
            n++;
        }
        nbe = D.be->ops->pollfds ? D.be->ops->pollfds(D.be, pfd + n, 4) : 0;

        timeout = 1000 * (int64_t)NS_MS;
        if (!D.pl_ok)
            timeout = D.next_probe > now ? (int64_t)(D.next_probe - now) : 0;
        if (any_park || D.ret_enabled || D.cache_valid)
            timeout = timeout < (int64_t)PARK_POLL_NS ? timeout : (int64_t)PARK_POLL_NS;
        if (any_stall)
            timeout = timeout < FIFO_RETRY_NS ? timeout : FIFO_RETRY_NS;

        r = os_poll(pfd, n + nbe, timeout);
        if (r < 0 && r != -OS_EINTR) {
            os_log("poll failed: %s", os_strerror(r));
            return 1;
        }
        if (r <= 0)
            continue;
        now = os_now_ns();
        for (i = 0; i < n - 1; i++) {
            client *c;
            int j;
            short ev = pfd[1 + i].revents;
            if (!ev)
                continue;
            c = NULL;
            for (j = 0; j < D.ncl; j++)
                if (pfd[1 + i].fd >= 0 && D.cl[j]->fd == pfd[1 + i].fd && !D.cl[j]->dead)
                    c = D.cl[j];
            if (!c)
                continue;
            if (ev & (OS_POLLIN | OS_POLLHUP | OS_POLLERR))
                client_read(c, now);
            if (ev & OS_POLLOUT)
                client_flush(c);
            if ((ev & OS_POLLERR) && !(ev & OS_POLLIN))
                kill_client(c, "socket error");
        }
        if (nbe > 0)
            D.be->ops->service(D.be, pfd + n, nbe);
        /* last: accepting may evict a client and a new connection may get its fd number, which
         * the loop above must not confuse */
        if (pfd[0].revents & OS_POLLIN)
            accept_clients(now);
    }
}

#pragma GCC optimize("fp-contract=off")
/*
 * tg_core.c -- portable firmware core of the Teensy 4.1 geometry engine (see tg_core.h).
 * (fp-contract off: the autonomous-mode matrix products must be the same on the Teensy, where the
 * Arduino build contracts to FMA by default, and in the x86 simulator.)
 *
 * Host protocol (gpu_proto.h, "Pi <-> Teensy"): 12-byte header {magic 'TGS1', type, flags, len}
 * + len payload bytes; every request gets exactly one reply {magic, type|0x8000, 0, len} whose
 * payload starts with int32 status.
 *  - Framing: the parser hunts for the magic byte by byte (resync after garbage). A header with an
 *    unknown type, or a length that is wrong for its type, is answered with GPU_ERR_PROTO and its
 *    payload is skipped when len <= GPU_MSG_MAX_PAYLOAD (so the stream stays in sync); a larger len
 *    makes the parser resync on the next magic instead. A message that stalls for more than
 *    TG_RX_TIMEOUT_US mid-way is dropped (no reply). Every such event counts in usb_bad_msgs.
 *  - T_MESH is streamed straight into the mesh store (geom_x_mesh_begin/commit), T_RECORDS is
 *    streamed record by record onto the bus; everything else is buffered (<= 18.5 KB) first.
 *  - Error replies always have the full reply size of their type (status set, rest 0 or valid).
 *
 * Parallel bus safety (SPEC 3): the platform keeps D/SOR/STROBE as inputs until this core has seen
 * BUSY = 0 continuously for TG_BUS_ARM_US (10 ms) ("fpga_ready"); then (bus mode auto) the pins are
 * driven ("bus_enabled"). Before every record the core waits for BUSY = 0; if that takes longer
 * than TG_BUS_TIMEOUT_US (500 ms) the pins are released, bus_timeouts++, the frame reports
 * GPU_ERR_BUS and sends nothing more, and arming starts again. While idle, BUSY = 1 for longer
 * than 500 ms also releases the pins (FPGA reset / reconfigured / powered down).
 *
 * T_FRAME: geom_frame() emits into the bus. With GEOM_FRAME_RETURN the reply carries every emitted
 * record: the frame is computed once (bus pass, counts the records and gives the status), then the
 * reply header is sent and the frame is computed a second time straight into USB (geom_frame is
 * deterministic and nothing can change the mesh store in between), so no record buffer is needed.
 *
 * Autonomous mode (SPEC 14): see auto_frame(). While it runs, USB is serviced between frames and,
 * inside a frame, every TG_AUTO_POLL_US and while waiting for BUSY -- but inside a frame only
 * T_HELLO and T_STATS are executed; any other message waits (unread) until the frame is done.
 * A frame is only started while the bus pins are driven (FPGA ready, bus mode auto): with no FPGA
 * the loop idles (animation time keeps running) instead of timing out every 500 ms. frame_no
 * counts frames that were sent completely (1, 2, 3, ... since power-up; not reset by T_AUTO or
 * T_RESET).
 */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "tg_common.h"

typedef char tg_chk_sizes[(sizeof(gpu_msg_hdr) == 12 && sizeof(geom_frame_hdr) == 88 &&
                           sizeof(geom_draw) == 72 && sizeof(geom_vertex) == 28 &&
                           sizeof(t_mesh_hdr) == 12 && sizeof(t_scene_hdr) == 156 &&
                           sizeof(t_scene_obj) == 88 && sizeof(t_auto) == 8 &&
                           sizeof(t_frame_reply) == 36 && sizeof(t_hello_reply) == 28) ? 1 : -1];

#define FRAME_HDR_BYTES  88u
#define DRAW_BYTES       72u
#define SCENE_HDR_BYTES  156u
#define SCENE_OBJ_BYTES  88u
#define SMALL_MSG_MAX    256u
#define MSGBUF_BYTES     (FRAME_HDR_BYTES + TG_MAX_DRAWS * DRAW_BYTES)
#define SCENE_MSG_MAX    (SCENE_HDR_BYTES + T_MAX_SCENE_OBJS * SCENE_OBJ_BYTES + T_MAX_OVERLAY_RECS * GPU_REC_BYTES)
#define MESH_MSG_MAX     (12u + GEOM_MAX_VERTS * 28u + GEOM_MAX_INDICES * 2u)

typedef char tg_chk_msgbuf[(MSGBUF_BYTES >= SCENE_MSG_MAX && MSGBUF_BYTES >= SMALL_MSG_MAX) ? 1 : -1];

/* ---------------------------------------------------------------------------------------- */
/* state                                                                                     */
/* ---------------------------------------------------------------------------------------- */

static union {
    uint32_t align;
    uint8_t  b[MSGBUF_BYTES];
} g_msg;

/* receive parser */
enum { RX_HUNT, RX_HDR, RX_PAUSED, RX_BUF, RX_SKIP, RX_MESH_HDR, RX_MESH_DATA, RX_REC_COUNT, RX_REC_DATA };

static struct {
    int         state;
    uint32_t    sh;             /* magic shift register */
    uint32_t    hunt_n;         /* bytes consumed while hunting */
    uint8_t     hdrb[12];
    uint32_t    hdr_have;
    gpu_msg_hdr h;
    uint32_t    left;           /* payload bytes not yet consumed */
    uint32_t    have;           /* bytes collected in the current buffer */
    int32_t     skip_status;    /* reply after RX_SKIP */
    uint8_t    *vdst, *idst;    /* T_MESH streaming destinations */
    uint32_t    vleft, ileft;
    uint32_t    rec_sent;       /* T_RECORDS */
    int32_t     rec_status;
    union { uint32_t w[GPU_REC_WORDS]; uint8_t b[GPU_REC_BYTES]; } rec;
    uint32_t    last_us;        /* last time bytes arrived / work was done mid-message */
    uint8_t     q[512];         /* staging buffer for USB reads */
    uint32_t    qpos, qlen;
} rx;

/* bus */
static struct {
    uint32_t mode;              /* 0 auto, 1 forced off (T_BUS_MODE) */
    int      qualified;         /* BUSY seen 0 for >= 10 ms and no timeout since = fpga_ready */
    int      want;              /* last state requested from the platform */
    int      driving;           /* pins are outputs = bus_enabled (what the platform reported) */
    int      low_run, high_run;
    uint32_t low_since, high_since;
    uint32_t wait_total_us;     /* cumulative time spent waiting for BUSY */
} bus;

/* statistics */
static struct {
    uint32_t frames, records_sent, bus_timeouts, usb_bad_msgs, auto_frames;
    uint32_t win_start, win_auto_frames, win_tris, win_busy_us;
    uint32_t auto_fps_x100, cpu_busy_pct, tris_per_sec;
} stt;

/* autonomous mode */
static struct {
    int         have_scene, running, started, in_frame;
    t_scene_hdr hdr;
    t_scene_obj obj[T_MAX_SCENE_OBJS];
    uint32_t    ovl[T_MAX_OVERLAY_RECS][GPU_REC_WORDS];
    uint32_t    max_fps;
    uint64_t    t_us;           /* time since enable */
    uint32_t    t_last, last_start, backoff_until;
    int         backoff;
    uint32_t    frame_no;
    uint32_t    last_poll;
    geom_draw   draws[T_MAX_SCENE_OBJS];
} au;

static void rx_run(int restricted);

/* ---------------------------------------------------------------------------------------- */
/* replies                                                                                   */
/* ---------------------------------------------------------------------------------------- */

static void send_hdr(uint16_t type, uint32_t len)
{
    gpu_msg_hdr h;
    h.magic = GPU_TUSB_MAGIC;
    h.type = (uint16_t)(type | GPU_MSG_REPLY);
    h.flags = 0;
    h.len = len;
    tgp_usb_write(&h, sizeof h);
}

static void reply(uint16_t type, const void *p, uint32_t n)
{
    send_hdr(type, n);
    tgp_usb_write(p, n);
    tgp_usb_flush();
}

static void reply_status(uint16_t type, int32_t status)
{
    reply(type, &status, sizeof status);
}

static void fill_hello(t_hello_reply *r, int32_t status)
{
    memset(r, 0, sizeof *r);
    r->status = status;
    r->fw_version = GPU_TUSB_FW_VER;
    r->max_meshes = GEOM_MAX_MESHES;
    r->max_verts = GEOM_MAX_VERTS;
    r->max_indices = GEOM_MAX_INDICES;
    r->fpga_ready = (uint32_t)bus.qualified;
    r->bus_enabled = (uint32_t)bus.driving;
}

static void fill_stats(t_stats_reply *r, int32_t status)
{
    memset(r, 0, sizeof *r);
    r->status = status;
    r->frames = stt.frames;
    r->records_sent = stt.records_sent;
    r->words_sent = stt.records_sent * GPU_REC_WORDS;
    r->bus_timeouts = stt.bus_timeouts;
    r->fpga_ready = (uint32_t)bus.qualified;
    r->bus_enabled = (uint32_t)bus.driving;
    r->usb_bad_msgs = stt.usb_bad_msgs;
    r->auto_running = (uint32_t)au.running;
    r->auto_frames = stt.auto_frames;
    r->auto_fps_x100 = stt.auto_fps_x100;
    r->cpu_busy_pct = stt.cpu_busy_pct;
    r->tris_per_sec = stt.tris_per_sec;
    r->cpu_mhz = tgp_cpu_mhz();
}

/* A reply of the full size its type normally has, carrying an error status. */
static void reply_error(uint16_t type, int32_t status)
{
    switch (type) {
    case T_HELLO: {
        t_hello_reply r;
        fill_hello(&r, status);
        reply(type, &r, sizeof r);
        break;
    }
    case T_STATS: {
        t_stats_reply r;
        fill_stats(&r, status);
        reply(type, &r, sizeof r);
        break;
    }
    case T_FRAME: {
        t_frame_reply r;
        memset(&r, 0, sizeof r);
        r.status = status;
        reply(type, &r, sizeof r);
        break;
    }
    case T_RECORDS: {
        int32_t r[2];
        r[0] = status;
        r[1] = 0;
        reply(type, r, sizeof r);
        break;
    }
    default:
        reply_status(type, status);
        break;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* parallel bus state machine                                                                */
/* ---------------------------------------------------------------------------------------- */

/* Sample BUSY once, advance the arming / release state machine, return the sample. */
static int bus_update(void)
{
    uint32_t now = tgp_micros();
    int want, busy = tgp_bus_busy();
    if (!busy) {
        bus.high_run = 0;
        if (!bus.low_run) {
            bus.low_run = 1;
            bus.low_since = now;
        }
        if (!bus.qualified && now - bus.low_since >= TG_BUS_ARM_US)
            bus.qualified = 1;
    } else {
        bus.low_run = 0;
        if (!bus.high_run) {
            bus.high_run = 1;
            bus.high_since = now;
        }
        if (bus.qualified && now - bus.high_since > TG_BUS_TIMEOUT_US)
            bus.qualified = 0;              /* FPGA gone / in reset for > 500 ms: release the pins */
    }
    want = bus.qualified && bus.mode == 0;
    if (want != bus.want) {             /* ask the platform once per change (it may refuse) */
        bus.want = want;
        bus.driving = tgp_bus_drive(want) && want;
    }
    return busy;
}

static void bus_disarm(void)
{
    bus.qualified = 0;
    bus.low_run = 0;
    bus.want = 0;
    bus.driving = 0;
    tgp_bus_drive(0);
}

static void auto_service(void);

/* Wait until a record may be started. 0 = go, GPU_ERR_BUS = bus forced off or timeout. */
static int bus_wait_ready(void)
{
    uint32_t t0 = tgp_micros(), now;
    int r = 0;
    for (;;) {
        int busy = bus_update();       /* one sample: the record starts on the same BUSY = 0 */
        if (bus.mode != 0) {           /* that reset the "BUSY high for 500 ms" timer */
            r = GPU_ERR_BUS;
            break;
        }
        if (bus.driving && !busy)
            break;
        now = tgp_micros();
        if (now - t0 > TG_BUS_TIMEOUT_US) {
            bus_disarm();                   /* pins -> inputs; arming starts again */
            stt.bus_timeouts++;
            r = GPU_ERR_BUS;
            break;
        }
        if (au.in_frame)
            auto_service();
        tgp_idle();
    }
    bus.wait_total_us += tgp_micros() - t0;
    return r;
}

static int bus_send(const uint32_t rec[GPU_REC_WORDS])
{
    int r = bus_wait_ready();
    if (r < 0)
        return r;
    if (tgp_bus_send(rec) < 0)
        return GPU_ERR_BUS;                 /* link lost (simulator) */
    stt.records_sent++;
    if (GPU_W0_TYPE(rec[0]) == GPU_REC_TRI)
        stt.win_tris++;
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* geom_frame emit callback                                                                  */
/* ---------------------------------------------------------------------------------------- */

typedef struct {
    int      to_bus;            /* send each record on the bus */
    int      to_usb;            /* write each record to USB (GEOM_FRAME_RETURN second pass) */
    int      autonomous;        /* insert the scene overlay before END, abort on bus error, poll USB */
    int      bus_err;           /* first bus error of this frame (then nothing more is sent) */
    uint32_t nrecs;             /* records geom_frame emitted (overlays not counted) */
    uint32_t limit;             /* to_usb: never write more than this many */
} emit_ctx;

static void emit_bus(emit_ctx *e, const uint32_t rec[GPU_REC_WORDS])
{
    int r;
    if (!e->to_bus || e->bus_err)
        return;
    r = bus_send(rec);
    if (r < 0)
        e->bus_err = r;
}

static int emit_cb(void *user, const uint32_t rec[GPU_REC_WORDS])
{
    emit_ctx *e = (emit_ctx *)user;
    if (e->to_usb) {
        if (e->nrecs < e->limit)
            tgp_usb_write(rec, GPU_REC_BYTES);
        e->nrecs++;
        return 0;
    }
    if (e->autonomous && GPU_W0_TYPE(rec[0]) == GPU_REC_END) {
        uint32_t i;
        for (i = 0; i < au.hdr.noverlay; i++)
            emit_bus(e, au.ovl[i]);
    }
    emit_bus(e, rec);
    e->nrecs++;
    if (e->autonomous) {
        if (e->bus_err)
            return e->bus_err;              /* stop computing a frame nobody receives */
        if (tgp_micros() - au.last_poll >= TG_AUTO_POLL_US)
            auto_service();
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* message handlers                                                                          */
/* ---------------------------------------------------------------------------------------- */

static void do_frame(const uint8_t *p, uint32_t len)
{
    t_frame_reply fr;
    geom_frame_hdr hdr;
    geom_stats gs;
    emit_ctx ec;
    const geom_draw *draws = (const geom_draw *)(const void *)(p + FRAME_HDR_BYTES);
    uint32_t t0, w0;
    int r;

    memset(&fr, 0, sizeof fr);
    memcpy(&hdr, p, sizeof hdr);
    fr.frame_no = hdr.frame_no;
    if ((uint32_t)hdr.ndraws != (len - FRAME_HDR_BYTES) / DRAW_BYTES) {
        fr.status = GPU_ERR_PROTO;
        reply(T_FRAME, &fr, sizeof fr);
        return;
    }
    if (au.running) {
        fr.status = GPU_ERR_ARG;
        reply(T_FRAME, &fr, sizeof fr);
        return;
    }
    memset(&ec, 0, sizeof ec);
    ec.to_bus = (hdr.flags & GEOM_FRAME_NO_BUS) == 0;
    t0 = tgp_micros();
    w0 = bus.wait_total_us;
    r = geom_frame(&hdr, draws, emit_cb, &ec, &gs);
    fr.us_total = tgp_micros() - t0;
    fr.us_bus_wait = bus.wait_total_us - w0;
    fr.status = r < 0 ? r : ec.bus_err;
    fr.tris_in = gs.tris_in;
    fr.tris_out = gs.tris_out;
    fr.tris_culled = gs.tris_culled;
    fr.tris_clipped = gs.tris_clipped;
    if (fr.status == 0)
        stt.frames++;
    if (r == 0 && (hdr.flags & GEOM_FRAME_RETURN)) {
        uint64_t bytes = (uint64_t)sizeof fr + (uint64_t)ec.nrecs * GPU_REC_BYTES;
        if (bytes > GPU_MSG_MAX_PAYLOAD) {
            if (fr.status == 0)
                fr.status = GPU_ERR_NOMEM;  /* reply would exceed the protocol's payload limit */
        } else {
            emit_ctx e2;
            geom_stats gs2;
            fr.nrecs_returned = ec.nrecs;
            send_hdr(T_FRAME, (uint32_t)bytes);
            tgp_usb_write(&fr, sizeof fr);
            memset(&e2, 0, sizeof e2);
            e2.to_usb = 1;
            e2.limit = ec.nrecs;
            geom_frame(&hdr, draws, emit_cb, &e2, &gs2);
            while (e2.nrecs < ec.nrecs) {   /* cannot happen (deterministic); keeps the framing */
                static const uint32_t zero[GPU_REC_WORDS];
                tgp_usb_write(zero, GPU_REC_BYTES);
                e2.nrecs++;
            }
            tgp_usb_flush();
            return;
        }
    }
    reply(T_FRAME, &fr, sizeof fr);
}

static int32_t do_scene(const uint8_t *p, uint32_t len)
{
    t_scene_hdr sh;
    uint32_t i;
    const uint8_t *ov;
    memcpy(&sh, p, sizeof sh);
    if (sh.nobjs > T_MAX_SCENE_OBJS || sh.noverlay > T_MAX_OVERLAY_RECS)
        return GPU_ERR_ARG;
    if (len != SCENE_HDR_BYTES + sh.nobjs * SCENE_OBJ_BYTES + sh.noverlay * GPU_REC_BYTES)
        return GPU_ERR_PROTO;
    ov = p + SCENE_HDR_BYTES + sh.nobjs * SCENE_OBJ_BYTES;
    for (i = 0; i < sh.noverlay; i++) {
        uint32_t w0;
        memcpy(&w0, ov + i * GPU_REC_BYTES, 4);
        if (GPU_W0_TYPE(w0) != GPU_REC_TRI && GPU_W0_TYPE(w0) != GPU_REC_SPRITE && GPU_W0_TYPE(w0) != GPU_REC_NOP)
            return GPU_ERR_ARG;             /* an END (or junk) would split the frame */
    }
    au.hdr = sh;
    memcpy(au.obj, p + SCENE_HDR_BYTES, sh.nobjs * SCENE_OBJ_BYTES);
    memcpy(au.ovl, ov, sh.noverlay * GPU_REC_BYTES);
    au.have_scene = 1;
    return GPU_ERR_OK;
}

static int32_t do_auto(const uint8_t *p)
{
    t_auto a;
    memcpy(&a, p, sizeof a);
    if (a.enable == 0) {
        au.running = 0;
        au.backoff = 0;
        return GPU_ERR_OK;
    }
    if (a.enable != 1 || !au.have_scene)
        return GPU_ERR_ARG;
    if (!au.running) {                      /* (re)start: t = 0; frame_no keeps counting up so a
                                               viewer waiting for "frame_no > last" never stalls */
        au.running = 1;
        au.started = 0;
        au.backoff = 0;
        au.t_us = 0;
        au.t_last = tgp_micros();
    }
    au.max_fps = a.max_fps;
    return GPU_ERR_OK;
}

static void do_reset(void)
{
    au.running = 0;
    au.have_scene = 0;
    geom_reset();
}

/* A complete buffered message. */
static void execute(uint16_t type, const uint8_t *p, uint32_t len)
{
    switch (type) {
    case T_HELLO: {
        t_hello_reply r;
        fill_hello(&r, GPU_ERR_OK);
        reply(type, &r, sizeof r);
        break;
    }
    case T_STATS: {
        t_stats_reply r;
        fill_stats(&r, GPU_ERR_OK);
        reply(type, &r, sizeof r);
        break;
    }
    case T_BUS_MODE: {
        uint32_t m;
        memcpy(&m, p, 4);
        if (m > 1) {
            reply_status(type, GPU_ERR_ARG);
            break;
        }
        bus.mode = m;
        bus_update();
        reply_status(type, GPU_ERR_OK);
        break;
    }
    case T_RESET:
        do_reset();
        reply_status(type, GPU_ERR_OK);
        break;
    case T_FRAME:
        do_frame(p, len);
        break;
    case T_SCENE:
        reply_status(type, do_scene(p, len));
        break;
    case T_AUTO:
        reply_status(type, do_auto(p));
        break;
    default:
        reply_status(type, GPU_ERR_PROTO);  /* unreachable: filtered in on_header */
        break;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* receive state machine                                                                     */
/* ---------------------------------------------------------------------------------------- */

static void rx_resync(void)
{
    rx.state = RX_HUNT;
    rx.sh = 0;
    rx.hunt_n = 0;
}

static void begin_skip(uint32_t len, int32_t status)
{
    rx.left = len;
    rx.skip_status = status;
    rx.state = RX_SKIP;
    if (len == 0) {
        rx_resync();
        reply_error(rx.h.type, status);
    }
}

static void finish_buf(void)
{
    rx_resync();
    execute(rx.h.type, g_msg.b, rx.h.len);
    rx.last_us = tgp_micros();
}

static void begin_payload(void)
{
    rx.have = 0;
    rx.left = rx.h.len;
    switch (rx.h.type) {
    case T_MESH:
        rx.state = RX_MESH_HDR;
        break;
    case T_RECORDS:
        rx.state = RX_REC_COUNT;
        break;
    default:
        rx.state = RX_BUF;
        if (rx.left == 0)
            finish_buf();
        break;
    }
}

/* 0 if `len` is acceptable for `type`, GPU_ERR_PROTO if not; -99 for an unknown type */
static int check_len(uint16_t type, uint32_t len)
{
    switch (type) {
    case T_HELLO:
    case T_STATS:
    case T_RESET:
        return len <= SMALL_MSG_MAX ? 0 : GPU_ERR_PROTO;
    case T_BUS_MODE:
        return len == 4 ? 0 : GPU_ERR_PROTO;
    case T_AUTO:
        return len == sizeof(t_auto) ? 0 : GPU_ERR_PROTO;
    case T_FRAME:
        return (len >= FRAME_HDR_BYTES && (len - FRAME_HDR_BYTES) % DRAW_BYTES == 0 &&
                (len - FRAME_HDR_BYTES) / DRAW_BYTES <= TG_MAX_DRAWS) ? 0 : GPU_ERR_PROTO;
    case T_SCENE:
        return (len >= SCENE_HDR_BYTES && len <= SCENE_MSG_MAX) ? 0 : GPU_ERR_PROTO;
    case T_MESH:
        return (len >= sizeof(t_mesh_hdr) && len <= MESH_MSG_MAX) ? 0 : GPU_ERR_PROTO;
    case T_RECORDS:
        return (len >= 4 && len <= GPU_MSG_MAX_PAYLOAD) ? 0 : GPU_ERR_PROTO;
    default:
        return -99;
    }
}

static void on_header(int restricted)
{
    int c;
    memcpy(&rx.h, rx.hdrb, sizeof rx.h);
    c = check_len(rx.h.type, rx.h.len);
    if (c != 0) {
        stt.usb_bad_msgs++;
        if (rx.h.len <= GPU_MSG_MAX_PAYLOAD) {
            begin_skip(rx.h.len, GPU_ERR_PROTO);        /* keep the stream in sync */
        } else {
            rx_resync();
            if (c != -99)
                reply_error(rx.h.type, GPU_ERR_PROTO);
        }
        return;
    }
    if (restricted && rx.h.type != T_HELLO && rx.h.type != T_STATS) {
        rx.state = RX_PAUSED;                           /* executed after the current frame */
        return;
    }
    begin_payload();
}

/* In restricted mode (inside an autonomous frame) only states that cannot touch the mesh store,
 * the bus or the scene may advance. */
static int restricted_ok(void)
{
    switch (rx.state) {
    case RX_HUNT:
    case RX_HDR:
    case RX_SKIP:
        return 1;
    case RX_BUF:
        return rx.h.type == T_HELLO || rx.h.type == T_STATS;
    default:
        return 0;
    }
}

static void rx_check_timeout(void)
{
    if (rx.state == RX_HUNT || rx.state == RX_PAUSED)
        return;
    if (tgp_micros() - rx.last_us > TG_RX_TIMEOUT_US) {
        if (rx.state == RX_MESH_DATA)
            geom_x_mesh_abort();
        stt.usb_bad_msgs++;
        rx_resync();
    }
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void rx_step_mesh_hdr(const uint8_t *src, uint32_t n)
{
    t_mesh_hdr mh;
    uint64_t expect;
    void *vd = NULL, *id = NULL;
    int r;
    memcpy(g_msg.b + rx.have, src, n);
    rx.have += n;
    rx.left -= n;
    if (rx.have < sizeof mh)
        return;
    memcpy(&mh, g_msg.b, sizeof mh);
    expect = (uint64_t)sizeof mh + (uint64_t)mh.nverts * 28u + (uint64_t)mh.nidx * 2u;
    if (expect != rx.h.len) {
        stt.usb_bad_msgs++;
        begin_skip(rx.left, GPU_ERR_PROTO);
        return;
    }
    r = geom_x_mesh_begin(mh.mesh_id, mh.nverts, mh.nidx, &vd, &id);
    if (r != GPU_ERR_OK) {
        begin_skip(rx.left, r);
        return;
    }
    rx.vdst = (uint8_t *)vd;
    rx.idst = (uint8_t *)id;
    rx.vleft = mh.nverts * 28u;
    rx.ileft = mh.nidx * 2u;
    rx.state = RX_MESH_DATA;
    if (rx.left == 0) {
        rx_resync();
        reply_status(T_MESH, geom_x_mesh_commit());
    }
}

static void rx_step_mesh_data(const uint8_t *src, uint32_t n)
{
    rx.left -= n;
    while (n) {
        uint32_t m;
        if (rx.vleft) {
            m = min_u32(n, rx.vleft);
            memcpy(rx.vdst, src, m);
            rx.vdst += m;
            rx.vleft -= m;
        } else {
            m = min_u32(n, rx.ileft);
            memcpy(rx.idst, src, m);
            rx.idst += m;
            rx.ileft -= m;
        }
        src += m;
        n -= m;
    }
    if (rx.left == 0) {
        rx_resync();
        reply_status(T_MESH, geom_x_mesh_commit());
    }
}

static void finish_records(void)
{
    int32_t r[2];
    rx_resync();
    r[0] = rx.rec_status;
    r[1] = (int32_t)rx.rec_sent;
    reply(T_RECORDS, r, sizeof r);
}

static void rx_step_rec_count(const uint8_t *src, uint32_t n)
{
    uint32_t count;
    memcpy(g_msg.b + rx.have, src, n);
    rx.have += n;
    rx.left -= n;
    if (rx.have < 4)
        return;
    memcpy(&count, g_msg.b, 4);
    if ((uint64_t)4u + (uint64_t)count * GPU_REC_BYTES != rx.h.len) {
        stt.usb_bad_msgs++;
        begin_skip(rx.left, GPU_ERR_PROTO);
        return;
    }
    if (au.running) {
        begin_skip(rx.left, GPU_ERR_ARG);
        return;
    }
    rx.rec_sent = 0;
    rx.rec_status = 0;
    rx.have = 0;
    rx.state = RX_REC_DATA;
    if (rx.left == 0)
        finish_records();
}

static void rx_step_rec_data(const uint8_t *src, uint32_t n)
{
    memcpy(rx.rec.b + rx.have, src, n);
    rx.have += n;
    rx.left -= n;
    if (rx.have == GPU_REC_BYTES) {
        rx.have = 0;
        if (rx.rec_status == 0) {
            int r = bus_send(rx.rec.w);
            if (r < 0)
                rx.rec_status = r;          /* the rest of the message is consumed, not sent */
            else
                rx.rec_sent++;
            rx.last_us = tgp_micros();
        }
    }
    if (rx.left == 0)
        finish_records();
}

/* Process received bytes. restricted = inside an autonomous frame. */
static void rx_run(int restricted)
{
    for (;;) {
        uint32_t avail, n;
        const uint8_t *src;

        if (rx.state == RX_PAUSED) {
            if (restricted)
                return;
            begin_payload();
            continue;
        }
        if (restricted && !restricted_ok())
            return;
        if (rx.qpos >= rx.qlen) {
            int got = tgp_usb_read(rx.q, sizeof rx.q);
            if (got <= 0) {
                rx_check_timeout();
                return;
            }
            rx.qpos = 0;
            rx.qlen = (uint32_t)got;
            rx.last_us = tgp_micros();
        }
        avail = rx.qlen - rx.qpos;
        src = rx.q + rx.qpos;

        switch (rx.state) {
        case RX_HUNT:
            rx.qpos++;
            rx.hunt_n++;
            rx.sh = (rx.sh >> 8) | ((uint32_t)src[0] << 24);
            if (rx.sh == GPU_TUSB_MAGIC) {
                if (rx.hunt_n > 4)
                    stt.usb_bad_msgs++;     /* junk before this message */
                memcpy(rx.hdrb, &rx.sh, 4);
                rx.hdr_have = 4;
                rx.sh = 0;
                rx.hunt_n = 0;
                rx.state = RX_HDR;
            }
            break;
        case RX_HDR:
            n = min_u32(avail, 12u - rx.hdr_have);
            memcpy(rx.hdrb + rx.hdr_have, src, n);
            rx.hdr_have += n;
            rx.qpos += n;
            if (rx.hdr_have == 12)
                on_header(restricted);
            break;
        case RX_BUF:
            n = min_u32(avail, rx.left);
            memcpy(g_msg.b + rx.have, src, n);
            rx.have += n;
            rx.left -= n;
            rx.qpos += n;
            if (rx.left == 0)
                finish_buf();
            break;
        case RX_SKIP:
            n = min_u32(avail, rx.left);
            rx.left -= n;
            rx.qpos += n;
            if (rx.left == 0) {
                rx_resync();
                reply_error(rx.h.type, rx.skip_status);
            }
            break;
        case RX_MESH_HDR:
            n = min_u32(avail, min_u32(rx.left, (uint32_t)sizeof(t_mesh_hdr) - rx.have));
            rx.qpos += n;
            rx_step_mesh_hdr(src, n);
            break;
        case RX_MESH_DATA:
            n = min_u32(avail, rx.left);
            rx.qpos += n;
            rx_step_mesh_data(src, n);
            break;
        case RX_REC_COUNT:
            n = min_u32(avail, min_u32(rx.left, 4u - rx.have));
            rx.qpos += n;
            rx_step_rec_count(src, n);
            break;
        case RX_REC_DATA:
            n = min_u32(avail, min_u32(rx.left, GPU_REC_BYTES - rx.have));
            rx.qpos += n;
            rx_step_rec_data(src, n);
            break;
        default:
            rx_resync();
            break;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* autonomous mode (SPEC 14)                                                                 */
/* ---------------------------------------------------------------------------------------- */

static void auto_service(void)
{
    au.last_poll = tgp_micros();
    rx_run(1);
}

/* o = a * b, column-major 4x4 (o may not alias a or b) */
static void mat_mul(const float a[16], const float b[16], float o[16])
{
    int c, r;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            o[c * 4 + r] = ((a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1])
                            + a[2 * 4 + r] * b[c * 4 + 2]) + a[3 * 4 + r] * b[c * 4 + 3];
}

/* Rodrigues rotation matrix about `axis` (normalised here; zero axis -> identity), column-major */
static void mat_rot(const float axis[3], double angle, float m[16])
{
    float x = axis[0], y = axis[1], z = axis[2];
    float len = sqrtf(x * x + y * y + z * z);
    float c, s, C;
    double a = fmod(angle, 6.283185307179586);
    memset(m, 0, 16 * sizeof(float));
    m[15] = 1.0f;
    if (!(len > 0.0f)) {
        m[0] = m[5] = m[10] = 1.0f;
        return;
    }
    x /= len;
    y /= len;
    z /= len;
    c = (float)cos(a);
    s = (float)sin(a);
    C = 1.0f - c;
    m[0] = x * x * C + c;     m[4] = x * y * C - z * s; m[8]  = x * z * C + y * s;
    m[1] = y * x * C + z * s; m[5] = y * y * C + c;     m[9]  = y * z * C - x * s;
    m[2] = z * x * C - y * s; m[6] = z * y * C + x * s; m[10] = z * z * C + c;
}

static void auto_frame(void)
{
    static const float yaxis[3] = {0.0f, 1.0f, 0.0f};
    geom_frame_hdr hdr;
    float view[16], proj[16], vt[16], vp[16], ry[16], model[16], rot[16], mt[16];
    emit_ctx ec;
    geom_stats gs;
    double t;
    uint32_t i, now = tgp_micros(), nd = 0;
    int r;

    au.t_us += now - au.t_last;
    au.t_last = now;
    t = (double)au.t_us * 1e-6;

    memcpy(view, au.hdr.view, sizeof view);
    memcpy(proj, au.hdr.proj, sizeof proj);
    if (au.hdr.cam_orbit_rate != 0.0f) {    /* view(t) = view * RY(-rate*t) */
        mat_rot(yaxis, -(double)au.hdr.cam_orbit_rate * t, ry);
        mat_mul(view, ry, vt);
    } else {
        memcpy(vt, view, sizeof vt);        /* static camera: exactly the given view */
    }
    mat_mul(proj, vt, vp);                  /* viewproj = proj * view(t) */

    memset(&hdr, 0, sizeof hdr);
    hdr.frame_no = au.frame_no + 1u;        /* counted only once the frame was sent */
    memcpy(hdr.viewproj, vp, sizeof vp);
    memcpy(hdr.light_dir, au.hdr.light_dir, sizeof hdr.light_dir);
    hdr.ambient = au.hdr.ambient;
    for (i = 0; i < au.hdr.nobjs; i++) {
        const t_scene_obj *o = &au.obj[i];
        geom_draw *d = &au.draws[nd];
        float axis[3];
        if (!geom_x_mesh_exists(o->mesh_id))
            continue;                       /* not (yet) uploaded: skipped, never an error */
        memcpy(model, o->model, sizeof model);
        if (o->spin_rate != 0.0f) {         /* model(t) = model * R(axis, rate*t) */
            memcpy(axis, o->spin_axis, sizeof axis);
            mat_rot(axis, (double)o->spin_rate * t, rot);
            mat_mul(model, rot, mt);
        } else {
            memcpy(mt, model, sizeof mt);
        }
        memset(d, 0, sizeof *d);
        d->mesh_id = o->mesh_id;
        d->flags = o->flags;
        memcpy(d->model, mt, sizeof mt);
        memcpy(d->color_mul, o->color_mul, 4);
        nd++;
    }
    hdr.ndraws = (uint16_t)nd;

    memset(&ec, 0, sizeof ec);
    ec.to_bus = 1;
    ec.autonomous = 1;
    au.in_frame = 1;
    au.last_poll = tgp_micros();
    r = geom_frame(&hdr, au.draws, emit_cb, &ec, &gs);
    au.in_frame = 0;
    if (r == 0 && ec.bus_err == 0) {
        au.frame_no = hdr.frame_no;
        stt.frames++;
        stt.auto_frames++;
        stt.win_auto_frames++;
    } else {
        au.backoff = 1;                     /* bus lost mid-frame: retry in 20 ms */
        au.backoff_until = tgp_micros() + 20000u;
    }
}

/* 1 if an autonomous frame should be started now */
static int auto_due(void)
{
    uint32_t now = tgp_micros();
    if (!au.running || !bus.driving)        /* no FPGA (or bus forced off): idle, no timeouts */
        return 0;
    if (au.backoff && (int32_t)(now - au.backoff_until) < 0)
        return 0;
    if (au.max_fps && au.started && now - au.last_start < 1000000u / au.max_fps)
        return 0;
    return 1;
}

/* returns 1 if a frame was attempted */
static int auto_step(void)
{
    if (!auto_due())
        return 0;
    au.backoff = 0;
    au.started = 1;
    au.last_start = tgp_micros();
    auto_frame();
    return 1;
}

int tg_wants_cpu(void)
{
    return auto_due() || rx.state == RX_PAUSED || rx.qpos < rx.qlen;
}

/* ---------------------------------------------------------------------------------------- */
/* top level                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static void stats_tick(void)
{
    uint32_t now = tgp_micros(), el = now - stt.win_start;
    if (el < 1000000u)
        return;
    stt.auto_fps_x100 = (uint32_t)((uint64_t)stt.win_auto_frames * 100000000u / el);
    stt.tris_per_sec = (uint32_t)((uint64_t)stt.win_tris * 1000000u / el);
    stt.cpu_busy_pct = (uint32_t)((uint64_t)stt.win_busy_us * 100u / el);
    if (stt.cpu_busy_pct > 100)
        stt.cpu_busy_pct = 100;
    stt.win_start = now;
    stt.win_auto_frames = stt.win_tris = stt.win_busy_us = 0;
}

static void led_tick(void)
{
    static int last = -1;
    int on = bus.driving ? 1 : (int)((tgp_micros() / 500000u) & 1u);
    if (on != last) {
        tgp_led(on);
        last = on;
    }
}

void tg_host_reset(void)
{
    if (rx.state == RX_MESH_DATA)
        geom_x_mesh_abort();
    rx_resync();
    rx.qpos = rx.qlen = 0;
}

void tg_init(void)
{
    memset(&rx, 0, sizeof rx);
    memset(&bus, 0, sizeof bus);
    memset(&stt, 0, sizeof stt);
    memset(&au, 0, sizeof au);
    rx_resync();
    geom_reset();
    tgp_bus_drive(0);
    stt.win_start = tgp_micros();
}

void tg_poll(void)
{
    uint32_t t0 = tgp_micros(), w0 = bus.wait_total_us;
    uint32_t q0 = rx.qpos, l0 = rx.qlen;
    int worked;

    bus_update();
    rx_run(0);
    worked = (rx.qpos != q0 || rx.qlen != l0);
    if (auto_step()) {
        worked = 1;
        if (rx.state != RX_HUNT)
            rx.last_us = tgp_micros();      /* we were not reading during the frame */
    }
    if (worked)
        stt.win_busy_us += (tgp_micros() - t0) - (bus.wait_total_us - w0);
    stats_tick();
    led_tick();
}

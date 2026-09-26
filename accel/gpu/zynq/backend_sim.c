/*
 * backend_sim.c -- software model of the PL for fpgagpud --sim (SPEC sections 7, 12, 13.2).
 *
 * Same register semantics as rtl/axi_gp_regs.v + rtl/core_collector.v:
 *  - two FIFOs of 33-bit entries {sor, word}: Teensy 1024, PS 512; pushes to a full FIFO are dropped;
 *  - collector: one open record at a time; a list part is taken from each enabled source,
 *    Teensy first, then PS, each until its END; sor=1 mid-record -> partial record discarded
 *    (BAD_RECORDS+1); sor=0 with no open record -> discarded, BAD_RECORDS+1 once per run;
 *    unknown type -> BAD_RECORDS+1; NOP ignored; TRI/SPRITE beyond 1536 -> LIST_OVERFLOW+1;
 *    active source changing under an open record -> discarded (BAD+1); disabled sources are
 *    drained (DROPPED += words); no source enabled -> nothing completes;
 *  - a complete list is rendered at once with gpu_refrast_frame into the back buffer of a 32 MB
 *    virtual DDR (FB0/FB1 registers), then swapped: FRONT ^= 1, FRAME_COUNT += 1 (no vsync wait);
 *  - frame return (13.1): if RET_ENABLE and !RET_FULL when a list is rendered, the frame is also
 *    copied to RET_ADDR, RET_FULL = 1, RET_FRAME = its frame_no; RET_ACK clears RET_FULL;
 *  - SOFT_RESET flushes both FIFOs and the collector (open record, list being filled); counters
 *    are kept (as the RTL);
 *  - Teensy bus: TCP listener (default 7778) accepting one simulated Teensy at a time; its byte
 *    stream is a sequence of 96-byte records, word 0 of each carrying SOR. BUSY is modelled: a new
 *    record enters the FIFO only while >= 64 entries are free, so a stalled collector
 *    back-pressures the Teensy through TCP. A new connection replaces the old one and restarts
 *    record framing; bytes of an incomplete trailing word are dropped when the Teensy disconnects.
 * Sim-only choices: RENDER_CYCLES = wall-clock render time converted to 148.75 MHz cycles;
 * VSYNC_COUNT = time since start at 60.1 Hz; STATUS reports MMCM_LOCKED and HPD; a framebuffer or
 * RET address outside the virtual DDR counts an AXI error instead of writing.
 */
#include <string.h>
#include "backend.h"
#include "gpu_proto.h"
#include "gpu_refrast.h"

#define T_DEPTH   GPU_T_FIFO_DEPTH
#define P_DEPTH   GPU_PS_FIFO_DEPTH
#define BUSBUF    65536u
#define FRAME_NS  16638655ull          /* 1650*750 / 74.375 MHz */

typedef struct {
    uint64_t e[T_DEPTH];
    unsigned head, count, depth;
} fifo;

typedef struct {
    backend  base;
    sim_opts o;
    uint64_t t0;
    uint8_t  *ddr;
    uint16_t *fb;
    uint32_t *list;
    /* registers */
    uint32_t ctrl, clear, fb0, fb1, front, frame_count, list_overflow, bad_records, t_words;
    uint32_t render_cycles, prim_count, axi_errors, dropped, ret_addr, ret_en, ret_full, ret_frame;
    uint32_t last_frame_no;
    /* FIFOs */
    fifo tf, pf;
    /* collector */
    int      rec_open, wcnt, rsrc, bad_run, bad_run_src, got_t, got_p, cnt;
    uint32_t rtype, w1q, t_fno, p_fno;
    uint32_t rec[GPU_REC_WORDS];
    /* Teensy bus */
    int      bus_lfd, bus_fd;
    char     bus_peer[48];
    uint8_t  busbuf[BUSBUF];
    size_t   bus_len, bus_off;
    uint64_t stream_words, last_bus_ns;
    int      in_run;
} sim;

/* ---- FIFOs ------------------------------------------------------------------------------ */
static void fifo_init(fifo *f, unsigned depth) { f->head = f->count = 0; f->depth = depth; }
static void fifo_clear(fifo *f) { f->head = f->count = 0; }
static int  fifo_push(fifo *f, uint64_t v)
{
    if (f->count >= f->depth)
        return 0;
    f->e[(f->head + f->count) % f->depth] = v;
    f->count++;
    return 1;
}
static uint64_t fifo_pop(fifo *f)
{
    uint64_t v = f->e[f->head];
    f->head = (f->head + 1) % f->depth;
    f->count--;
    return v;
}

static int configured(sim *s)
{
    if (s->o.nopl)
        return 0;
    if (s->o.pl_delay_ms > 0 && os_now_ns() - s->t0 < (uint64_t)s->o.pl_delay_ms * 1000000ull)
        return 0;
    return 1;
}

/* ---- DDR --------------------------------------------------------------------------------- */
static int in_ddr(uint32_t addr, size_t len)
{
    return addr >= GPU_DDR_BASE && len <= GPU_DDR_SIZE && addr - GPU_DDR_BASE <= GPU_DDR_SIZE - len;
}

static uint16_t sim_rd16(void *user, uint32_t addr)
{
    sim *s = user;
    const uint8_t *p;
    if (!in_ddr(addr, 2))
        return 0;
    p = s->ddr + (addr - GPU_DDR_BASE);
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---- rendering + swap -------------------------------------------------------------------- */
static void finish_list(sim *s, uint32_t frame_no)
{
    uint64_t t0 = os_now_ns();
    uint32_t back = s->front ^ 1u;
    uint32_t fbaddr = back ? s->fb1 : s->fb0;

    gpu_refrast_frame(s->list, s->cnt, (uint16_t)s->clear, sim_rd16, s, s->fb);
    if (in_ddr(fbaddr, GPU_FB_BYTES))
        memcpy(s->ddr + (fbaddr - GPU_DDR_BASE), s->fb, GPU_FB_BYTES);   /* little-endian host */
    else
        s->axi_errors++;
    if (s->ret_en && !s->ret_full) {
        if (in_ddr(s->ret_addr, GPU_FB_BYTES))
            memcpy(s->ddr + (s->ret_addr - GPU_DDR_BASE), s->fb, GPU_FB_BYTES);
        else
            s->axi_errors++;
        s->ret_full = 1;
        s->ret_frame = frame_no;
    }
    s->front = back;
    s->frame_count++;
    s->last_frame_no = frame_no;
    s->prim_count = (uint32_t)s->cnt;
    s->render_cycles = (uint32_t)((os_now_ns() - t0) * 14875u / 100000u);
    s->cnt = 0;
}

/* ---- collector (mirrors core_collector.v; the sim's lists never stall) -------------------- */
static void take_word(sim *s, int act_src, uint64_t e)
{
    uint32_t w = (uint32_t)e;
    int sor = (int)((e >> 32) & 1u);

    if (sor) {
        if (s->rec_open)
            s->bad_records++;
        s->rec_open = 1;
        s->wcnt = 1;
        s->rtype = w >> 28;
        s->rsrc = act_src;
        s->bad_run = 0;
        s->rec[0] = w;
    } else if (s->rec_open) {
        s->rec[s->wcnt] = w;
        if (s->wcnt == 1)
            s->w1q = w;
        if (s->wcnt == GPU_REC_WORDS - 1) {
            s->rec_open = 0;
            switch (s->rtype) {
            case GPU_REC_TRI:
            case GPU_REC_SPRITE:
                if (s->cnt < GPU_LIST_SLOTS) {
                    memcpy(s->list + (size_t)s->cnt * GPU_REC_WORDS, s->rec, GPU_REC_BYTES);
                    s->cnt++;
                } else {
                    s->list_overflow++;
                }
                break;
            case GPU_REC_NOP:
                break;
            case GPU_REC_END:
                if (s->rsrc) {
                    s->got_p = 1;
                    s->p_fno = s->w1q;
                } else {
                    s->got_t = 1;
                    s->t_fno = s->w1q;
                }
                break;
            default:
                s->bad_records++;
                break;
            }
        } else {
            s->wcnt++;
        }
    } else {
        int eff = s->bad_run && s->bad_run_src == act_src;
        if (!eff)
            s->bad_records++;
        s->bad_run = 1;
        s->bad_run_src = act_src;
    }
}

static void cur_sources(const sim *s, int *t_cur, int *p_cur)
{
    int src_t = (s->ctrl & GPU_CTL_SRC_TEENSY) != 0, src_p = (s->ctrl & GPU_CTL_SRC_PS) != 0;
    *t_cur = src_t && !s->got_t;
    *p_cur = src_p && !s->got_p && !*t_cur;
}

static int collect(sim *s)
{
    int moved = 0;
    for (;;) {
        int src_t = (s->ctrl & GPU_CTL_SRC_TEENSY) != 0, src_p = (s->ctrl & GPU_CTL_SRC_PS) != 0;
        int t_cur, p_cur, act_valid, act_src, progress = 0;
        fifo *f;

        if (!src_t && s->tf.count) {
            s->dropped += s->tf.count;
            fifo_clear(&s->tf);
            progress = 1;
        }
        if (!src_p && s->pf.count) {
            s->dropped += s->pf.count;
            fifo_clear(&s->pf);
            progress = 1;
        }
        cur_sources(s, &t_cur, &p_cur);
        act_valid = t_cur || p_cur;
        act_src = p_cur;
        if ((src_t || src_p) && (!src_t || s->got_t) && (!src_p || s->got_p)) {
            if (s->rec_open) {
                s->rec_open = 0;
                s->bad_records++;
            }
            finish_list(s, src_p ? s->p_fno : s->t_fno);
            s->got_t = s->got_p = 0;
            s->bad_run = 0;
            moved = 1;
            continue;
        }
        if (s->rec_open && act_valid && act_src != s->rsrc) {
            s->rec_open = 0;
            s->bad_records++;
            moved = 1;
            continue;
        }
        f = act_src ? &s->pf : &s->tf;
        if (act_valid && f->count) {
            take_word(s, act_src, fifo_pop(f));
            progress = 1;
        }
        if (!progress)
            break;
        moved = 1;
    }
    return moved;
}

/* Teensy bus bytes -> Teensy FIFO, honouring BUSY (record start needs >= 64 free entries). */
static int bus_feed(sim *s)
{
    int moved = 0;
    uint64_t now = 0;
    if (!configured(s))
        return 0;                                   /* unconfigured FPGA: BUSY reads 1 */
    while (s->bus_len - s->bus_off >= 4) {
        unsigned idx = (unsigned)(s->stream_words % GPU_REC_WORDS);
        unsigned need = idx == 0 ? GPU_T_BUSY_FREE : 1u;
        const uint8_t *p = s->busbuf + s->bus_off;
        uint32_t w;
        if (T_DEPTH - s->tf.count < need)
            break;
        w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        fifo_push(&s->tf, (uint64_t)w | ((uint64_t)(idx == 0) << 32));
        s->bus_off += 4;
        s->stream_words++;
        s->t_words++;
        moved = 1;
    }
    if (moved) {
        now = os_now_ns();
        s->last_bus_ns = now ? now : 1;
    }
    if (s->bus_off == s->bus_len)
        s->bus_off = s->bus_len = 0;
    return moved;
}

static void sim_run(sim *s)
{
    if (s->in_run)
        return;
    s->in_run = 1;
    for (;;) {
        int moved = bus_feed(s);
        moved |= collect(s);
        if (!moved)
            break;
    }
    s->in_run = 0;
}

static void soft_reset(sim *s)
{
    fifo_clear(&s->tf);
    fifo_clear(&s->pf);
    s->rec_open = 0;
    s->wcnt = 0;
    s->bad_run = 0;
    s->got_t = s->got_p = 0;
    s->cnt = 0;
}

/* ---- backend ops ----------------------------------------------------------------------------- */
static int sim_probe(backend *b, char *why, size_t whyn)
{
    sim *s = (sim *)b;
    if (s->o.nopl) {
        os_snprintf(why, whyn, "simulated PL not configured (--sim-nopl)");
        return 0;
    }
    if (!configured(s)) {
        os_snprintf(why, whyn, "simulated PL not configured yet (--sim-pl-delay-ms %d)", s->o.pl_delay_ms);
        return 0;
    }
    os_snprintf(why, whyn, "simulated PL: ID 0x%08x VERSION 0x%08x, Teensy bus on TCP %d",
                GPU_ID_VALUE, GPU_VERSION_VALUE, s->o.bus_port);
    return 1;
}

static int sim_alive(backend *b)
{
    return configured((sim *)b);
}

static uint32_t sim_rd(backend *b, uint32_t off)
{
    sim *s = (sim *)b;
    uint64_t now;
    int t_cur, p_cur;
    uint32_t st;
    switch (off & 0xFFCu) {
    case GPU_R_ID:            return GPU_ID_VALUE;
    case GPU_R_VERSION:       return GPU_VERSION_VALUE;
    case GPU_R_CONTROL:       return s->ctrl;
    case GPU_R_STATUS:
        now = os_now_ns();
        cur_sources(s, &t_cur, &p_cur);
        st = GPU_ST_MMCM_LOCKED | GPU_ST_HPD;
        if (s->last_bus_ns && now - s->last_bus_ns < 100000000ull)
            st |= GPU_ST_TEENSY_ACTIVE;
        if (t_cur)
            st |= GPU_ST_WAIT_TEENSY;
        if (p_cur)
            st |= GPU_ST_WAIT_PS;
        return st;
    case GPU_R_FRAME_COUNT:   return s->frame_count;
    case GPU_R_FB0:           return s->fb0;
    case GPU_R_FB1:           return s->fb1;
    case GPU_R_FRONT:         return s->front;
    case GPU_R_CLEAR_COLOR:   return s->clear;
    case GPU_R_PS_FIFO_FREE:  return P_DEPTH - s->pf.count;
    case GPU_R_T_FIFO_LEVEL:  return s->tf.count;
    case GPU_R_LIST_OVERFLOW: return s->list_overflow;
    case GPU_R_BAD_RECORDS:   return s->bad_records;
    case GPU_R_T_WORDS:       return s->t_words;
    case GPU_R_RENDER_CYCLES: return s->render_cycles;
    case GPU_R_PRIM_COUNT:    return s->prim_count;
    case GPU_R_VSYNC_COUNT:   return (uint32_t)((os_now_ns() - s->t0) / FRAME_NS);
    case GPU_R_AXI_ERRORS:    return s->axi_errors;
    case GPU_R_DROPPED:       return s->dropped;
    case GPU_R_RET_ADDR:      return s->ret_addr;
    case GPU_R_RET_CTRL:      return s->ret_en;
    case GPU_R_RET_STATUS:    return s->ret_full ? GPU_RET_FULL : 0u;
    case GPU_R_RET_FRAME:     return s->ret_frame;
    case GPU_R_LAST_FRAME_NO: return s->last_frame_no;
    default:                  return 0;
    }
}

static void sim_wr(backend *b, uint32_t off, uint32_t v)
{
    sim *s = (sim *)b;
    switch (off & 0xFFCu) {
    case GPU_R_CONTROL:
        s->ctrl = v & (GPU_CTL_SRC_TEENSY | GPU_CTL_SRC_PS | GPU_CTL_SCANOUT_EN);
        if (v & GPU_CTL_SOFT_RESET)
            soft_reset(s);
        break;
    case GPU_R_FB0:         s->fb0 = v & 0xFFFFF000u; break;
    case GPU_R_FB1:         s->fb1 = v & 0xFFFFF000u; break;
    case GPU_R_CLEAR_COLOR: s->clear = v & 0xFFFFu; break;
    case GPU_R_RET_ADDR:    s->ret_addr = v & 0xFFFFF000u; break;
    case GPU_R_RET_CTRL:
        s->ret_en = v & GPU_RET_ENABLE;
        if ((v & GPU_RET_ACK) && s->ret_full)
            s->ret_full = 0;
        break;
    case GPU_R_PS_FIFO_DATA:
        fifo_push(&s->pf, v);
        break;
    case GPU_R_PS_FIFO_SOR:
        fifo_push(&s->pf, (uint64_t)v | (1ull << 32));
        break;
    default:
        return;                                         /* read-only / unmapped: ignored */
    }
    sim_run(s);
}

static void sim_push(backend *b, const uint32_t *recs, int n)
{
    int i, k;
    for (i = 0; i < n; i++) {
        const uint32_t *r = recs + (size_t)i * GPU_REC_WORDS;
        sim_wr(b, GPU_R_PS_FIFO_SOR, r[0]);
        for (k = 1; k < GPU_REC_WORDS; k++)
            sim_wr(b, GPU_R_PS_FIFO_DATA, r[k]);
    }
}

static int sim_ddr_write(backend *b, uint32_t phys, const void *src, size_t len)
{
    sim *s = (sim *)b;
    if (!in_ddr(phys, len) || (phys & 7u) || (len & 7u))
        return -OS_EINVAL;
    memcpy(s->ddr + (phys - GPU_DDR_BASE), src, len);
    return 0;
}

static int sim_ddr_read(backend *b, uint32_t phys, void *dst, size_t len)
{
    sim *s = (sim *)b;
    if (!in_ddr(phys, len) || (phys & 7u) || (len & 7u))
        return -OS_EINVAL;
    memcpy(dst, s->ddr + (phys - GPU_DDR_BASE), len);
    return 0;
}

static int sim_pollfds(backend *b, struct os_pollfd *p, int max)
{
    sim *s = (sim *)b;
    int n = 0;
    if (s->bus_lfd >= 0 && n < max) {
        p[n].fd = s->bus_lfd;
        p[n].events = OS_POLLIN;
        p[n].revents = 0;
        n++;
    }
    if (s->bus_fd >= 0 && n < max) {
        if (s->bus_off && s->bus_len - s->bus_off < 4096) {
            memmove(s->busbuf, s->busbuf + s->bus_off, s->bus_len - s->bus_off);
            s->bus_len -= s->bus_off;
            s->bus_off = 0;
        }
        p[n].fd = s->bus_fd;
        p[n].events = s->bus_len < BUSBUF ? OS_POLLIN : 0;
        p[n].revents = 0;
        n++;
    }
    return n;
}

static void bus_close(sim *s, const char *why)
{
    os_log("sim: Teensy bus client %s %s", s->bus_peer, why);
    os_close(s->bus_fd);
    s->bus_fd = -1;
    s->bus_len -= (s->bus_len - s->bus_off) % 4u;      /* drop an incomplete trailing word */
}

static void sim_service(backend *b, const struct os_pollfd *p, int n)
{
    sim *s = (sim *)b;
    int i;
    for (i = 0; i < n; i++) {
        if (!p[i].revents)
            continue;
        if (p[i].fd == s->bus_lfd && s->bus_lfd >= 0) {
            char peer[48];
            int fd = os_accept(s->bus_lfd, peer, sizeof peer);
            if (fd >= 0) {
                if (s->bus_fd >= 0)
                    bus_close(s, "replaced by a new connection");
                os_tune_socket(fd, 0);
                s->bus_fd = fd;
                memcpy(s->bus_peer, peer, sizeof peer);
                s->bus_len = s->bus_off = 0;
                s->stream_words = 0;                    /* a new Teensy starts at a record boundary */
                os_log("sim: Teensy bus client %s connected", peer);
            }
        } else if (p[i].fd == s->bus_fd && s->bus_fd >= 0) {
            if (s->bus_len < BUSBUF) {
                long r = os_recv(s->bus_fd, s->busbuf + s->bus_len, BUSBUF - s->bus_len);
                if (r > 0)
                    s->bus_len += (size_t)r;
                else if (r == 0)
                    bus_close(s, "disconnected");
                else if (r != -OS_EAGAIN && r != -OS_EINTR)
                    bus_close(s, os_strerror((int)r));
            } else if (p[i].revents & (OS_POLLERR | OS_POLLHUP)) {
                bus_close(s, "hung up");
            }
        }
    }
    sim_run(s);
}

static void sim_destroy(backend *b)
{
    sim *s = (sim *)b;
    if (s->bus_fd >= 0)
        os_close(s->bus_fd);
    if (s->bus_lfd >= 0)
        os_close(s->bus_lfd);
    os_free(s->ddr);
    os_free(s->fb);
    os_free(s->list);
    os_free(s);
}

static const backend_ops sim_ops = {
    sim_probe, sim_alive, sim_rd, sim_wr, sim_push, sim_ddr_write, sim_ddr_read,
    sim_pollfds, sim_service, sim_destroy
};

backend *backend_sim_create(const sim_opts *o)
{
    const uint16_t one = 1;
    sim *s;
    if (*(const uint8_t *)&one != 1) {
        os_log("sim: needs a little-endian host");
        return NULL;
    }
    s = os_calloc(sizeof *s);
    if (!s)
        return NULL;
    s->base.ops = &sim_ops;
    s->base.name = "simulated PL";
    s->o = *o;
    s->t0 = os_now_ns();
    s->ddr = os_calloc(GPU_DDR_SIZE);
    s->fb = os_calloc(GPU_FB_BYTES);
    s->list = os_calloc((size_t)GPU_LIST_SLOTS * GPU_REC_BYTES);
    s->bus_fd = -1;
    s->bus_lfd = -1;
    if (!s->ddr || !s->fb || !s->list) {
        os_log("sim: out of memory");
        sim_destroy(&s->base);
        return NULL;
    }
    fifo_init(&s->tf, T_DEPTH);
    fifo_init(&s->pf, P_DEPTH);
    s->ctrl = GPU_CTL_SRC_PS;                           /* CONTROL reset value 0x4 */
    s->fb0 = GPU_FB0_ADDR;
    s->fb1 = GPU_FB1_ADDR;
    s->ret_addr = GPU_RET_ADDR;
    if (o->bus_port > 0) {
        s->bus_lfd = os_tcp_listen(o->bind_ip, o->bus_port);
        if (s->bus_lfd < 0) {
            os_log("sim: cannot listen on %s:%d for the Teensy bus: %s", o->bind_ip, o->bus_port,
                   os_strerror(s->bus_lfd));
            s->bus_lfd = -1;
            sim_destroy(&s->base);
            return NULL;
        }
        os_log("sim: Teensy bus listener on %s:%d (raw 96-byte records)", o->bind_ip, o->bus_port);
    }
    return &s->base;
}

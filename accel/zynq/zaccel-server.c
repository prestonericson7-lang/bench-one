/*
 * zaccel-server -- the Zynq side of zaccel (accel/SPEC.md sections 2-4).
 *
 * TCP 8093 (--port), little-endian framing, ops INFO / LOAD / FREE / GEMV / PING.
 * One thread per connection; one mutex around the engine, the tensor table and the allocator.
 *
 * Engines, chosen at start behind one interface (engine_t.gemv):
 *   pl    - AXI DMA (PG021 simple mode) feeding zaccel_gemv in the PL; tensors live in the
 *           reserved DDR3 exposed as UIO "zaccel-mem", DMA registers as UIO "zaccel-dma".
 *           Only used when DEVCFG INT_STS.PCFG_DONE says the PL is configured.
 *   cpu   - exact int32 reference in C (fallback whenever the PL or DMA is absent).
 *   model - tests only (--model): the pl code path, unchanged, but the two UIO windows are
 *           ordinary memory and a software model of SPEC section 1 + the DMA reacts to the
 *           register writes.
 *
 * C11 + pthreads, no other dependencies. Build: build.sh.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64 /* 32-bit ARM: /dev/mem offset 0xF8007000 must not go negative */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "zaccel-server assumes a little-endian host (x86, ARM)"
#endif

/* ---- wire protocol (SPEC section 4) ---------------------------------------------------- */
#define REQ_MAGIC 0x3151415Au /* "ZAQ1" */
#define REP_MAGIC 0x3152415Au /* "ZAR1" */
enum { OP_INFO = 1, OP_LOAD = 2, OP_FREE = 3, OP_GEMV = 4, OP_PING = 5 };
enum { ST_OK = 0, ST_BAD = 1, ST_NOMEM = 2, ST_ENGINE = 3, ST_UNKNOWN = 4 };

#define MAX_COLS 4096u
#define MAX_BATCH 8u
#define MAX_ROWS 65535u
#define PING_MAX (16u << 20)  /* PING payloads are buffered whole, so they are bounded */
#define DRAIN_MAX (1u << 20)  /* a bad request up to this size is drained and the link kept */

/* ---- PL engine stream format (SPEC section 1) ------------------------------------------ */
#define HDR_MAGIC 0x5A41u
#define TRL_MAGIC 0x5A45u

/* ---- AXI DMA registers, PG021 (SPEC section 2) ----------------------------------------- */
#define MM2S_DMACR 0x00u
#define MM2S_DMASR 0x04u
#define MM2S_SA 0x18u
#define MM2S_LENGTH 0x28u
#define S2MM_DMACR 0x30u
#define S2MM_DMASR 0x34u
#define S2MM_DA 0x48u
#define S2MM_LENGTH 0x58u
#define CR_RS 0x1u
#define CR_RESET 0x4u
#define SR_HALTED 0x1u
#define SR_IDLE 0x2u
#define SR_INTERR 0x10u
#define SR_SLVERR 0x20u
#define SR_DECERR 0x40u
#define SR_IOC 0x1000u
#define SR_ERRIRQ 0x4000u
#define SR_ERRMASK (0x10u | 0x20u | 0x40u | 0x100u | 0x200u | 0x400u | 0x4000u)
#define DMA_LEN_MASK 0x3FFFFFFu /* 26-bit length register */
#define DMA_CHUNK (32u << 20)   /* weight transfers are split below the 26-bit limit */
#define DMA_REG_SPAN 0x60u

/* ---- memory ---------------------------------------------------------------------------- */
#define POOL_ALIGN 64u
#define STAGE_IN_BYTES (64u << 10)                 /* header + 8 x 4096 activations = 32776 */
#define STAGE_OUT_BYTES ((2u << 20) + (64u << 10)) /* 65535 x 8 results + trailer = 2097128 */
#define CPU_POOL_MB 256u                           /* heap arena for the cpu engine */
#define MODEL_MEM_PHYS 0x20000000u                 /* model: same layout as the reserved DDR3 */
#define MODEL_MEM_SIZE (384u << 20)
#define MODEL_DMA_WINDOW 0x10000u

/* ---- Zynq DEVCFG ----------------------------------------------------------------------- */
#define DEVCFG_BASE 0xF8007000u
#define DEVCFG_INT_STS 0x0Cu
#define PCFG_DONE (1u << 2)

#define RBUF 65536u

static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "zaccel-server: %s\n", b);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* row geometry of SPEC section 1: each row starts on a fresh 8-byte beat */
static uint32_t beats_per_row(uint32_t mode, uint32_t cols) { return mode == 0 ? (cols + 15) / 16 : (cols + 7) / 8; }
static uint32_t wire_row_bytes(uint32_t mode, uint32_t cols) { return mode == 0 ? (cols + 1) / 2 : cols; }

/* ---- first-fit allocator over the tensor memory (offsets, 64-byte aligned) -------------- */
typedef struct ext {
    uint64_t off, len;
    struct ext *next;
} ext_t;

typedef struct {
    ext_t *head; /* free extents, sorted by offset, never adjacent */
    uint64_t size, free;
} pool_t;

#define POOL_FAIL UINT64_MAX

static int pool_init(pool_t *p, uint64_t size)
{
    p->head = malloc(sizeof *p->head);
    if (!p->head)
        return -1;
    p->head->off = 0;
    p->head->len = size;
    p->head->next = NULL;
    p->size = p->free = size;
    return 0;
}

static void pool_destroy(pool_t *p)
{
    while (p->head) {
        ext_t *n = p->head->next;
        free(p->head);
        p->head = n;
    }
    p->size = p->free = 0;
}

static uint64_t pool_alloc(pool_t *p, uint64_t n)
{
    n = align_up(n ? n : 1, POOL_ALIGN);
    for (ext_t **pp = &p->head; *pp; pp = &(*pp)->next) {
        ext_t *e = *pp;
        if (e->len < n)
            continue;
        uint64_t off = e->off;
        e->off += n;
        e->len -= n;
        if (!e->len) {
            *pp = e->next;
            free(e);
        }
        p->free -= n;
        return off;
    }
    return POOL_FAIL;
}

static void pool_release(pool_t *p, uint64_t off, uint64_t n)
{
    n = align_up(n ? n : 1, POOL_ALIGN);
    ext_t *prev = NULL, *next = p->head;
    while (next && next->off < off) {
        prev = next;
        next = next->next;
    }
    p->free += n;
    if (prev && prev->off + prev->len == off) {
        prev->len += n;
        if (next && prev->off + prev->len == next->off) {
            prev->len += next->len;
            prev->next = next->next;
            free(next);
        }
        return;
    }
    if (next && off + n == next->off) {
        next->off = off;
        next->len += n;
        return;
    }
    ext_t *e = malloc(sizeof *e);
    if (!e) {
        p->free -= n;
        say("allocator: host malloc failed, %llu bytes of tensor memory lost", (unsigned long long)n);
        return;
    }
    e->off = off;
    e->len = n;
    e->next = next;
    if (prev)
        prev->next = e;
    else
        p->head = e;
}

/* ---- tensors --------------------------------------------------------------------------- */
typedef struct {
    uint32_t id, mode, rows, cols, bpr;
    uint64_t off, bytes; /* packed rows at mem + off, rows * bpr * 8 bytes */
} tensor_t;

static tensor_t *g_tens;
static size_t g_ntens, g_captens;
static uint32_t g_next_id = 1;

static tensor_t *tens_find(uint32_t id)
{
    for (size_t i = 0; i < g_ntens; i++)
        if (g_tens[i].id == id)
            return &g_tens[i];
    return NULL;
}

static int tens_add(tensor_t *t)
{
    if (g_ntens == g_captens) {
        size_t cap = g_captens ? g_captens * 2 : 64;
        tensor_t *n = realloc(g_tens, cap * sizeof *n);
        if (!n)
            return -1;
        g_tens = n;
        g_captens = cap;
    }
    do { /* ids are never 0 and never reused while live */
        t->id = g_next_id++;
    } while (!t->id || tens_find(t->id));
    g_tens[g_ntens++] = *t;
    return 0;
}

static void tens_del(tensor_t *t) { *t = g_tens[--g_ntens]; }

/* ---- engine ---------------------------------------------------------------------------- */
enum { ENG_CPU = 0, ENG_PL = 1, ENG_MODEL = 2 };

typedef struct {
    int fd, num;
    void *map;
    size_t maplen;
    uint8_t *ptr;
    uint64_t phys, size;
} uio_t;

typedef struct model model_t;
typedef struct engine engine_t;

struct engine {
    int kind;
    const char *name;
    uint32_t info_id; /* INFO engine and GEMV engine_used: 0 cpu, 1 pl */
    uint8_t *mem;     /* tensor memory (pl: uncached UIO map; cpu: heap arena) */
    uint64_t mem_phys, mem_size;
    volatile uint32_t *dma; /* DMA register window (pl, model) */
    pool_t pool;
    uint64_t stage_in, stage_out; /* staging offsets inside mem (pl, model) */
    uint8_t *stagebuf;            /* host buffer the staging input is built in */
    uio_t u_dma, u_mem;           /* pl */
    void *anon_mem, *anon_dma;    /* cpu arena / model windows */
    size_t anon_mem_len, anon_dma_len;
    model_t *model; /* model only */
    int (*gemv)(engine_t *e, const tensor_t *t, uint32_t nb, const int8_t *A, int32_t *Y, uint32_t *cycles);
};

static engine_t g_eng;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_selftest;
static volatile int g_selftesting;

/* Writes into tensor memory. The pl map is an uncached UIO mapping (strongly ordered on ARM),
 * which faults on unaligned or wide accesses, so it is only ever touched with aligned 32-bit
 * volatile accesses. The model uses the same accessors (same code path). */
static void mem_put(engine_t *e, uint64_t off, const uint8_t *src, uint64_t n)
{
    if (e->kind == ENG_CPU) {
        memcpy(e->mem + off, src, (size_t)n);
        return;
    }
    volatile uint32_t *d = (volatile uint32_t *)(void *)(e->mem + off);
    for (uint64_t i = 0; i < n / 4; i++) {
        uint32_t w;
        memcpy(&w, src + 4 * i, 4);
        d[i] = w;
    }
}

static void mem_get(engine_t *e, uint64_t off, uint8_t *dst, uint64_t n)
{
    const volatile uint32_t *s = (const volatile uint32_t *)(const void *)(e->mem + off);
    for (uint64_t i = 0; i < n / 4; i++) {
        uint32_t w = s[i];
        memcpy(dst + 4 * i, &w, 4);
    }
}

/* store one row, packed per SPEC section 1, on fresh beats. `wire` holds at least the row's
 * packed bytes (a LOAD row arrives already padded); the unused tail is forced to 0. */
static void store_row(engine_t *e, const tensor_t *t, uint32_t r, const uint8_t *wire, uint8_t *beats)
{
    uint32_t n = wire_row_bytes(t->mode, t->cols), pb = t->bpr * 8;
    memcpy(beats, wire, n);
    memset(beats + n, 0, pb - n);
    if (t->mode == 0 && (t->cols & 1))
        beats[n - 1] &= 0x0F; /* unused tail nibble is 0 */
    mem_put(e, t->off + (uint64_t)r * pb, beats, pb);
}

/* caller holds g_lock */
static int tensor_alloc(engine_t *e, tensor_t *t, uint32_t mode, uint32_t rows, uint32_t cols)
{
    memset(t, 0, sizeof *t);
    t->mode = mode;
    t->rows = rows;
    t->cols = cols;
    t->bpr = beats_per_row(mode, cols);
    t->bytes = (uint64_t)rows * t->bpr * 8;
    t->off = pool_alloc(&e->pool, t->bytes);
    return t->off == POOL_FAIL ? -1 : 0;
}

/* ---- cpu engine: exact int32 arithmetic on the packed rows ----------------------------- */
static int cpu_gemv(engine_t *e, const tensor_t *t, uint32_t nb, const int8_t *A, int32_t *Y, uint32_t *cycles)
{
    const uint32_t K = t->cols;
    int8_t w[MAX_COLS];
    for (uint32_t r = 0; r < t->rows; r++) {
        const uint8_t *row = e->mem + t->off + (uint64_t)r * t->bpr * 8;
        if (t->mode == 0) {
            for (uint32_t k = 0; k < K; k++) {
                int x = (row[k >> 1] >> ((k & 1) * 4)) & 0xF;
                w[k] = (int8_t)(x >= 8 ? x - 16 : x);
            }
        } else {
            memcpy(w, row, K);
        }
        for (uint32_t v = 0; v < nb; v++) {
            const int8_t *a = A + (size_t)v * K;
            int32_t acc = 0;
            for (uint32_t k = 0; k < K; k++)
                acc += (int32_t)w[k] * (int32_t)a[k];
            Y[(size_t)r * nb + v] = acc;
        }
    }
    *cycles = 0;
    return 0;
}

/* ---- DMA register access (pl and model share every line of this) ----------------------- */
static void model_reg_write(engine_t *e, uint32_t off, uint32_t v);

static void dma_wr(engine_t *e, uint32_t off, uint32_t v)
{
    __sync_synchronize();
    e->dma[off / 4] = v;
    __sync_synchronize();
    if (e->model)
        model_reg_write(e, off, v);
}

static uint32_t dma_rd(engine_t *e, uint32_t off)
{
    __sync_synchronize();
    uint32_t v = e->dma[off / 4];
    __sync_synchronize();
    return v;
}

static void poll_pause(unsigned *spins)
{
    if (++*spins > 2000) {
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
}

static int dma_wait_bits(engine_t *e, uint32_t off, uint32_t mask, uint32_t want, uint64_t limit_us, const char *what)
{
    uint64_t t0 = now_us();
    unsigned spins = 0;
    for (;;) {
        uint32_t v = dma_rd(e, off);
        if ((v & mask) == want)
            return 0;
        if (now_us() - t0 > limit_us) {
            say("pl: timeout waiting for %s (reg 0x%02x = 0x%08x)", what, off, v);
            return -1;
        }
        poll_pause(&spins);
    }
}

/* wait for a channel's Idle bit after a transfer; any error bit or a halt is fatal */
static int dma_wait_idle(engine_t *e, uint32_t sr_off, uint64_t bytes, const char *what)
{
    uint64_t t0 = now_us(), limit = 1000000u + bytes / 16u; /* >= 16 MB/s, 800 MB/s nominal */
    unsigned spins = 0;
    for (;;) {
        uint32_t sr = dma_rd(e, sr_off);
        if (sr & SR_ERRMASK) {
            say("pl: %s: DMA error, DMASR=0x%08x", what, sr);
            return -1;
        }
        if (sr & SR_IDLE)
            return 0;
        if (sr & SR_HALTED) {
            say("pl: %s: channel halted, DMASR=0x%08x", what, sr);
            return -1;
        }
        if (now_us() - t0 > limit) {
            say("pl: %s: timeout after %llu us, DMASR=0x%08x", what, (unsigned long long)limit, sr);
            return -1;
        }
        poll_pause(&spins);
    }
}

static int dma_reset(engine_t *e)
{
    dma_wr(e, MM2S_DMACR, CR_RESET);
    if (dma_wait_bits(e, MM2S_DMACR, CR_RESET, 0, 100000, "MM2S reset"))
        return -1;
    dma_wr(e, S2MM_DMACR, CR_RESET);
    if (dma_wait_bits(e, S2MM_DMACR, CR_RESET, 0, 100000, "S2MM reset"))
        return -1;
    return 0;
}

static int dma_run(engine_t *e)
{
    dma_wr(e, MM2S_DMACR, CR_RS);
    dma_wr(e, S2MM_DMACR, CR_RS);
    if (dma_wait_bits(e, MM2S_DMASR, SR_HALTED, 0, 100000, "MM2S run"))
        return -1;
    if (dma_wait_bits(e, S2MM_DMASR, SR_HALTED, 0, 100000, "S2MM run"))
        return -1;
    return 0;
}

/* ---- pl engine: one GEMV through the DMA and zaccel_gemv ------------------------------- */
static int pl_gemv(engine_t *e, const tensor_t *t, uint32_t nb, const int8_t *A, int32_t *Y, uint32_t *cycles)
{
    const uint32_t K = t->cols, N = t->rows, abeats = (K + 7) / 8;
    const uint64_t in_bytes = 8 + (uint64_t)nb * abeats * 8;
    const uint64_t nres = (uint64_t)N * nb;
    const uint64_t out_bytes = (nres + 1) / 2 * 8 + 8; /* results + trailer */
    const uint64_t w_bytes = (uint64_t)N * t->bpr * 8;
    const uint64_t in_phys = e->mem_phys + e->stage_in, out_phys = e->mem_phys + e->stage_out;
    static const uint8_t zero8[8];
    uint8_t *s = e->stagebuf;

    /* staging input: header beat, then nb activation vectors of ceil(K/8) zero-padded beats */
    uint64_t hdr = (uint64_t)HDR_MAGIC | (uint64_t)t->mode << 16 | (uint64_t)(nb - 1) << 20 |
                   (uint64_t)K << 32 | (uint64_t)N << 48;
    memset(s, 0, (size_t)in_bytes);
    memcpy(s, &hdr, 8);
    for (uint32_t v = 0; v < nb; v++)
        memcpy(s + 8 + (size_t)v * abeats * 8, A + (size_t)v * K, K);
    mem_put(e, e->stage_in, s, in_bytes);
    mem_put(e, e->stage_out + out_bytes - 8, zero8, 8); /* a stale trailer can never validate */

    if (dma_reset(e) || dma_run(e))
        goto fail;
    dma_wr(e, S2MM_DA, (uint32_t)out_phys);
    dma_wr(e, S2MM_LENGTH, (uint32_t)out_bytes);
    dma_wr(e, MM2S_SA, (uint32_t)in_phys);
    dma_wr(e, MM2S_LENGTH, (uint32_t)in_bytes);
    if (dma_wait_idle(e, MM2S_DMASR, in_bytes, "MM2S header+activations"))
        goto fail;
    for (uint64_t done = 0; done < w_bytes;) {
        uint64_t n = w_bytes - done;
        if (n > DMA_CHUNK)
            n = DMA_CHUNK;
        dma_wr(e, MM2S_SA, (uint32_t)(e->mem_phys + t->off + done));
        dma_wr(e, MM2S_LENGTH, (uint32_t)n);
        if (dma_wait_idle(e, MM2S_DMASR, n, "MM2S weights"))
            goto fail;
        done += n;
    }
    if (dma_wait_idle(e, S2MM_DMASR, out_bytes, "S2MM results"))
        goto fail;
    uint32_t got = dma_rd(e, S2MM_LENGTH) & DMA_LEN_MASK;
    if (got != out_bytes) {
        say("pl: S2MM received %u bytes, expected %llu", got, (unsigned long long)out_bytes);
        goto fail;
    }

    mem_get(e, e->stage_out, (uint8_t *)Y, nres * 4);
    if (nres & 1) {
        uint32_t pad;
        mem_get(e, e->stage_out + nres * 4, (uint8_t *)&pad, 4);
        if (pad) {
            say("pl: odd result count but pad word is 0x%08x", pad);
            goto fail;
        }
    }
    uint32_t tr[2];
    mem_get(e, e->stage_out + out_bytes - 8, (uint8_t *)tr, 8);
    if ((tr[1] >> 16) != TRL_MAGIC || (tr[1] & 0xFFFFu) != (N & 0xFFFFu)) {
        say("pl: bad trailer 0x%08x%08x (want magic 0x%04x rows %u)", tr[1], tr[0], TRL_MAGIC, N & 0xFFFFu);
        goto fail;
    }
    *cycles = tr[0];
    return 0;
fail:
    dma_reset(e);
    return -1;
}

/* ---- model: SPEC section 1 engine + PG021 simple-mode DMA, in software ----------------- */
#define MODEL_FIFO 16u /* engine output FIFO depth: input stalls when S2MM is not taking data */
#define MODEL_QCAP 64u

struct model {
    /* zaccel_gemv */
    int st; /* 0 wait header, 1 activations, 2 weights */
    uint32_t mode, nb, K, N, abeats, bpr, v, j, row, rb, cycles;
    int8_t A[MAX_BATCH][MAX_COLS + 16];
    int32_t acc[MAX_BATCH];
    int have_half;
    uint32_t half;
    uint64_t q[MODEL_QCAP];
    uint8_t qlast[MODEL_QCAP];
    unsigned qh, qn;
    long fault, jobs; /* --model-fault */
    /* AXI DMA: channel 0 = MM2S, 1 = S2MM */
    uint32_t cr[2], sr[2];
    int busy[2];
    uint64_t addr[2], len[2], done[2];
};

static void model_emit(model_t *m, uint64_t beat, int last)
{
    unsigned i = (m->qh + m->qn) % MODEL_QCAP;
    m->q[i] = beat;
    m->qlast[i] = (uint8_t)last;
    m->qn++;
}

static void model_out32(model_t *m, uint32_t v)
{
    if (!m->have_half) {
        m->half = v;
        m->have_half = 1;
    } else {
        model_emit(m, (uint64_t)m->half | (uint64_t)v << 32, 0);
        m->have_half = 0;
    }
}

static void model_beat(model_t *m, uint64_t b)
{
    if (m->st == 0) {
        uint32_t mode = (uint32_t)(b >> 16) & 0xF, nb = ((uint32_t)(b >> 20) & 0xF) + 1;
        uint32_t K = (uint32_t)(b >> 32) & 0xFFFF, N = (uint32_t)(b >> 48);
        if ((b & 0xFFFF) != HDR_MAGIC || mode > 1 || nb > MAX_BATCH || K < 1 || K > MAX_COLS || N < 1)
            return; /* not a header: dropped */
        m->mode = mode;
        m->nb = nb;
        m->K = K;
        m->N = N;
        m->abeats = (K + 7) / 8;
        m->bpr = beats_per_row(mode, K);
        m->v = m->j = m->row = m->rb = m->cycles = 0;
        m->have_half = 0;
        memset(m->A, 0, sizeof m->A);
        memset(m->acc, 0, sizeof m->acc);
        m->st = 1;
        return;
    }
    m->cycles++; /* one beat per clock after the header */
    if (m->st == 1) {
        int8_t *a = &m->A[m->v][m->j * 8];
        for (int i = 0; i < 8; i++)
            a[i] = (int8_t)(uint8_t)(b >> (8 * i));
        if (++m->j == m->abeats) {
            m->j = 0;
            if (++m->v == m->nb)
                m->st = 2;
        }
        return;
    }
    const uint32_t nb = m->nb;
    if (m->mode == 0) {
        const uint32_t base = m->rb * 16;
        for (uint32_t i = 0; i < 16; i++) {
            int w = (int)((b >> (4 * i)) & 0xF);
            if (w >= 8)
                w -= 16;
            for (uint32_t v = 0; v < nb; v++)
                m->acc[v] += w * m->A[v][base + i];
        }
    } else {
        const uint32_t base = m->rb * 8;
        for (uint32_t i = 0; i < 8; i++) {
            int w = (int8_t)(uint8_t)(b >> (8 * i));
            for (uint32_t v = 0; v < nb; v++)
                m->acc[v] += w * m->A[v][base + i];
        }
    }
    if (++m->rb < m->bpr)
        return;
    m->rb = 0;
    for (uint32_t v = 0; v < nb; v++) {
        model_out32(m, (uint32_t)m->acc[v]);
        m->acc[v] = 0;
    }
    if (++m->row < m->N)
        return;
    if (m->have_half) {
        model_emit(m, m->half, 0); /* odd count: bits 63:32 are 0 */
        m->have_half = 0;
    }
    uint32_t magic = TRL_MAGIC;
    if (m->fault < 0 && g_selftesting)
        magic ^= 0xFFFF;
    if (m->fault > 0 && !g_selftesting && ++m->jobs == m->fault)
        magic ^= 0xFFFF;
    model_emit(m, (uint64_t)m->cycles | (uint64_t)(m->N & 0xFFFF) << 32 | (uint64_t)magic << 48, 1);
    m->st = 0;
}

static uint8_t *model_xlat(engine_t *e, uint64_t pa, uint64_t n)
{
    if (pa < e->mem_phys || pa + n > e->mem_phys + e->mem_size)
        return NULL;
    return e->mem + (pa - e->mem_phys);
}

static void model_err(model_t *m, int ch, uint32_t bit)
{
    m->sr[ch] |= bit | SR_ERRIRQ | SR_HALTED;
    m->cr[ch] &= ~CR_RS;
    m->busy[ch] = 0;
}

static void model_pump(engine_t *e)
{
    model_t *m = e->model;
    for (;;) {
        int progress = 0;
        while (m->busy[1] && m->qn) { /* S2MM takes the engine's output */
            if (m->done[1] + 8 > m->len[1]) {
                model_err(m, 1, SR_INTERR); /* packet longer than the programmed buffer */
                break;
            }
            uint8_t *d = model_xlat(e, m->addr[1] + m->done[1], 8);
            if (!d) {
                model_err(m, 1, SR_DECERR);
                break;
            }
            uint64_t b = m->q[m->qh];
            int last = m->qlast[m->qh];
            m->qh = (m->qh + 1) % MODEL_QCAP;
            m->qn--;
            memcpy(d, &b, 8);
            m->done[1] += 8;
            progress = 1;
            if (last) { /* TLAST ends the transfer; LENGTH reports the bytes received */
                m->busy[1] = 0;
                m->sr[1] |= SR_IDLE | SR_IOC;
                e->dma[S2MM_LENGTH / 4] = (uint32_t)m->done[1];
            }
        }
        while (m->busy[0] && m->qn < MODEL_FIFO) { /* MM2S feeds the engine */
            uint64_t n = m->len[0] - m->done[0];
            if (n > 8)
                n = 8;
            const uint8_t *src = model_xlat(e, m->addr[0] + m->done[0], n);
            if (!src) {
                model_err(m, 0, SR_DECERR);
                break;
            }
            uint64_t b = 0;
            memcpy(&b, src, (size_t)n);
            m->done[0] += n;
            progress = 1;
            model_beat(m, b);
            if (m->done[0] == m->len[0]) {
                m->busy[0] = 0;
                m->sr[0] |= SR_IDLE | SR_IOC;
            }
            if (m->qn && m->busy[1])
                break;
        }
        if (!progress)
            return;
    }
}

static void model_publish(engine_t *e)
{
    model_t *m = e->model;
    e->dma[MM2S_DMACR / 4] = m->cr[0];
    e->dma[MM2S_DMASR / 4] = m->sr[0];
    e->dma[S2MM_DMACR / 4] = m->cr[1];
    e->dma[S2MM_DMASR / 4] = m->sr[1];
}

/* called after the driver's store has landed in the (ordinary memory) register window */
static void model_reg_write(engine_t *e, uint32_t off, uint32_t v)
{
    model_t *m = e->model;
    int ch = off >= S2MM_DMACR;
    switch (off - (ch ? S2MM_DMACR : 0)) {
    case 0x00: /* DMACR */
        if (v & CR_RESET) { /* soft reset resets the whole core; completes at once here */
            for (int c = 0; c < 2; c++) {
                m->cr[c] = 0;
                m->sr[c] = SR_HALTED;
                m->busy[c] = 0;
            }
            break;
        }
        m->cr[ch] = v;
        if (v & CR_RS) {
            m->sr[ch] &= ~SR_HALTED;
        } else {
            m->sr[ch] |= SR_HALTED;
            m->busy[ch] = 0;
        }
        break;
    case 0x04: /* DMASR: interrupt bits write-1-to-clear, the rest read-only */
        m->sr[ch] &= ~(v & (SR_IOC | 0x2000u | SR_ERRIRQ));
        break;
    case 0x28: { /* LENGTH: 26 bits wide; writing it starts the transfer */
        uint32_t len = v & DMA_LEN_MASK;
        e->dma[off / 4] = len;
        if (!(m->cr[ch] & CR_RS) || (m->sr[ch] & SR_HALTED) || !len)
            break;
        m->busy[ch] = 1;
        m->addr[ch] = e->dma[(off - 0x10) / 4]; /* SA 0x18 / DA 0x48 */
        m->len[ch] = len;
        m->done[ch] = 0;
        m->sr[ch] &= ~SR_IDLE;
        break;
    }
    default: /* SA / DA stay in the window as written */
        break;
    }
    model_pump(e);
    model_publish(e);
}

/* ---- engine setup ---------------------------------------------------------------------- */
static void *anon_map(size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void uio_close(uio_t *u)
{
    if (u->map && u->map != MAP_FAILED)
        munmap(u->map, u->maplen);
    if (u->fd >= 0)
        close(u->fd);
    u->map = NULL;
    u->fd = -1;
}

static void engine_teardown(engine_t *e)
{
    pool_destroy(&e->pool);
    uio_close(&e->u_dma);
    uio_close(&e->u_mem);
    if (e->anon_mem)
        munmap(e->anon_mem, e->anon_mem_len);
    if (e->anon_dma)
        munmap(e->anon_dma, e->anon_dma_len);
    free(e->stagebuf);
    free(e->model);
    memset(e, 0, sizeof *e);
    e->u_dma.fd = e->u_mem.fd = -1;
}

/* pl and model: pool over the tensor memory with the staging areas carved out first */
static int pl_common_init(engine_t *e, char *why, size_t wl)
{
    if (e->mem_size < STAGE_IN_BYTES + STAGE_OUT_BYTES + (1u << 20)) {
        snprintf(why, wl, "zaccel-mem too small (%llu bytes)", (unsigned long long)e->mem_size);
        return -1;
    }
    if (e->mem_phys + e->mem_size > 0x100000000ull || (e->mem_phys & (POOL_ALIGN - 1))) {
        snprintf(why, wl, "zaccel-mem at 0x%llx is not 32-bit DMA addressable/aligned", (unsigned long long)e->mem_phys);
        return -1;
    }
    e->stagebuf = malloc(STAGE_IN_BYTES);
    if (!e->stagebuf || pool_init(&e->pool, e->mem_size)) {
        snprintf(why, wl, "out of host memory");
        return -1;
    }
    e->stage_in = pool_alloc(&e->pool, STAGE_IN_BYTES);
    e->stage_out = pool_alloc(&e->pool, STAGE_OUT_BYTES);
    e->gemv = pl_gemv;
    e->info_id = 1;
    return 0;
}

static int cpu_init(engine_t *e)
{
    engine_teardown(e);
    e->kind = ENG_CPU;
    e->name = "cpu";
    e->info_id = 0;
    e->anon_mem_len = (size_t)CPU_POOL_MB << 20;
    e->anon_mem = anon_map(e->anon_mem_len);
    if (!e->anon_mem || pool_init(&e->pool, e->anon_mem_len))
        return -1;
    e->mem = e->anon_mem;
    e->mem_size = e->anon_mem_len;
    e->gemv = cpu_gemv;
    return 0;
}

static int model_init(engine_t *e, long fault, char *why, size_t wl)
{
    engine_teardown(e);
    e->kind = ENG_MODEL;
    e->name = "model";
    e->anon_mem_len = MODEL_MEM_SIZE;
    e->anon_dma_len = MODEL_DMA_WINDOW;
    e->anon_mem = anon_map(e->anon_mem_len);
    e->anon_dma = anon_map(e->anon_dma_len);
    e->model = calloc(1, sizeof *e->model);
    if (!e->anon_mem || !e->anon_dma || !e->model) {
        snprintf(why, wl, "out of host memory");
        return -1;
    }
    e->mem = e->anon_mem;
    e->mem_phys = MODEL_MEM_PHYS;
    e->mem_size = MODEL_MEM_SIZE;
    e->dma = e->anon_dma;
    e->model->fault = fault;
    e->model->sr[0] = e->model->sr[1] = SR_HALTED; /* state after power-on reset */
    model_publish(e);
    return pl_common_init(e, why, wl);
}

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char *ok = fgets(buf, (int)n, f);
    fclose(f);
    if (!ok)
        return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int read_u64(const char *path, uint64_t *v)
{
    char b[64], *end;
    if (read_line(path, b, sizeof b))
        return -1;
    errno = 0;
    unsigned long long x = strtoull(b, &end, 0);
    if (errno || end == b)
        return -1;
    *v = x;
    return 0;
}

static int uio_open(const char *name, uio_t *u, char *why, size_t wl)
{
    DIR *d = opendir("/sys/class/uio");
    if (!d) {
        snprintf(why, wl, "no /sys/class/uio (UIO not in the kernel?)");
        return -1;
    }
    int num = -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        char p[300], nm[128] = "";
        if (strncmp(de->d_name, "uio", 3))
            continue;
        snprintf(p, sizeof p, "/sys/class/uio/%s/name", de->d_name);
        /* uio_pdrv_genirq names the device after the full DT node, unit address included
         * ("zaccel-dma@40400000" -- seen on the 6.12 kernel in QEMU), so compare up to the '@' */
        if (!read_line(p, nm, sizeof nm)) {
            char *at = strchr(nm, '@');
            if (at)
                *at = 0;
        }
        if (!strcmp(nm, name)) {
            num = atoi(de->d_name + 3);
            break;
        }
    }
    closedir(d);
    if (num < 0) {
        snprintf(why, wl, "no UIO device named %s", name);
        return -1;
    }
    char p[128];
    uint64_t addr, size, offs = 0;
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map0/addr", num);
    int bad = read_u64(p, &addr);
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map0/size", num);
    bad |= read_u64(p, &size);
    snprintf(p, sizeof p, "/sys/class/uio/uio%d/maps/map0/offset", num);
    read_u64(p, &offs); /* optional: sub-page offset of the region */
    if (bad || size <= offs) {
        snprintf(why, wl, "cannot read map0 of uio%d (%s)", num, name);
        return -1;
    }
    snprintf(p, sizeof p, "/dev/uio%d", num);
    u->fd = open(p, O_RDWR | O_SYNC);
    if (u->fd < 0) {
        snprintf(why, wl, "open %s: %s", p, strerror(errno));
        return -1;
    }
    u->maplen = (size_t)size;
    u->map = mmap(NULL, u->maplen, PROT_READ | PROT_WRITE, MAP_SHARED, u->fd, 0); /* map0 */
    if (u->map == MAP_FAILED) {
        snprintf(why, wl, "mmap %s (%llu bytes): %s", p, (unsigned long long)size, strerror(errno));
        return -1;
    }
    u->num = num;
    u->ptr = (uint8_t *)u->map + offs;
    u->phys = addr + offs;
    u->size = size - offs;
    return 0;
}

static sigjmp_buf g_probe_jb;
static void probe_fault(int sig)
{
    (void)sig;
    siglongjmp(g_probe_jb, 1);
}

/* first read of a PL address, guarded: a bitstream without the DMA answers with a bus error */
static int guarded_read32(volatile uint32_t *p, uint32_t *out)
{
    struct sigaction sa, obus, osegv;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &obus);
    sigaction(SIGSEGV, &sa, &osegv);
    volatile int bad = 0;
    if (sigsetjmp(g_probe_jb, 1))
        bad = 1;
    else
        *out = *p;
    sigaction(SIGBUS, &obus, NULL);
    sigaction(SIGSEGV, &osegv, NULL);
    return bad;
}

static int pl_init(engine_t *e, char *why, size_t wl)
{
    engine_teardown(e);
    e->kind = ENG_PL;
    e->name = "pl";

    /* 1. is the PL configured? Touching a PL address while it is not hangs the bus. */
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        snprintf(why, wl, "cannot open /dev/mem to check PCFG_DONE: %s", strerror(errno));
        return -1;
    }
    void *p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, DEVCFG_BASE);
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(why, wl, "cannot map DEVCFG at 0x%08x: %s", DEVCFG_BASE, strerror(errno));
        return -1;
    }
    uint32_t sts = ((volatile uint32_t *)p)[DEVCFG_INT_STS / 4];
    munmap(p, 4096);
    if (!(sts & PCFG_DONE)) {
        snprintf(why, wl, "PL not configured (DEVCFG INT_STS=0x%08x, PCFG_DONE clear)", sts);
        return -1;
    }

    /* 2. the two UIO devices */
    if (uio_open("zaccel-dma", &e->u_dma, why, wl) || uio_open("zaccel-mem", &e->u_mem, why, wl))
        return -1;
    if (e->u_dma.size < DMA_REG_SPAN) {
        snprintf(why, wl, "zaccel-dma window too small (%llu bytes)", (unsigned long long)e->u_dma.size);
        return -1;
    }
    e->dma = (volatile uint32_t *)(void *)e->u_dma.ptr;
    e->mem = e->u_mem.ptr;
    e->mem_phys = e->u_mem.phys;
    e->mem_size = e->u_mem.size;

    /* 3. the DMA answers */
    uint32_t sr;
    if (guarded_read32(&e->dma[MM2S_DMASR / 4], &sr)) {
        snprintf(why, wl, "bus error reading the DMA at 0x%llx (bitstream without the DMA?)",
                 (unsigned long long)e->u_dma.phys);
        return -1;
    }
    if (pl_common_init(e, why, wl))
        return -1;
    say("pl: zaccel-dma uio%d @0x%08llx, zaccel-mem uio%d @0x%08llx %llu MB, MM2S_DMASR=0x%08x",
        e->u_dma.num, (unsigned long long)e->u_dma.phys, e->u_mem.num, (unsigned long long)e->mem_phys,
        (unsigned long long)(e->mem_size >> 20), sr);
    return 0;
}

/* ---- selftest: random small jobs on the active engine against a plain C reference ------ */
static uint32_t g_rng = 0x2545F491u;
static uint32_t rng(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng = x;
}

static void ref_gemv(uint32_t N, uint32_t K, uint32_t nb, const int8_t *W, const int8_t *A, int32_t *Y)
{
    for (uint32_t r = 0; r < N; r++)
        for (uint32_t v = 0; v < nb; v++) {
            int64_t s = 0;
            for (uint32_t k = 0; k < K; k++)
                s += (int64_t)W[(size_t)r * K + k] * A[(size_t)v * K + k];
            Y[(size_t)r * nb + v] = (int32_t)s;
        }
}

static const struct {
    uint8_t mode, nb, fill; /* fill 0 random, 1 min x min, 2 max x min */
    uint16_t cols, rows;
} st_cases[] = {
    {0, 1, 0, 1, 1},    {1, 1, 0, 1, 1},    {0, 3, 0, 17, 5},    {1, 3, 0, 17, 5},
    {0, 8, 0, 33, 9},   {1, 8, 0, 33, 9},   {0, 2, 0, 255, 13},  {1, 5, 0, 100, 7},
    {0, 7, 0, 4096, 3}, {1, 4, 0, 4095, 3}, {0, 8, 1, 4096, 2},  {1, 8, 1, 4096, 2},
    {0, 8, 2, 4096, 2}, {1, 8, 2, 4096, 2},
};

static uint32_t selftest(engine_t *e)
{
    uint8_t *beats = malloc(MAX_COLS);
    for (unsigned i = 0; i < sizeof st_cases / sizeof st_cases[0]; i++) {
        const uint32_t mode = st_cases[i].mode, nb = st_cases[i].nb, K = st_cases[i].cols, N = st_cases[i].rows;
        const uint32_t wl = wire_row_bytes(mode, K);
        int8_t *W = malloc((size_t)N * K), *A = malloc((size_t)nb * K);
        uint8_t *wire = calloc((size_t)N, wl);
        int32_t *Y = malloc((size_t)N * nb * 4), *R = malloc((size_t)N * nb * 4);
        uint32_t code = 0, cyc = 0;
        tensor_t t;
        if (!beats || !W || !A || !wire || !Y || !R) {
            code = (i + 1) << 8 | 4;
            goto next;
        }
        for (size_t k = 0; k < (size_t)N * K; k++) {
            int lo = mode ? -128 : -8, hi = mode ? 127 : 7;
            W[k] = (int8_t)(st_cases[i].fill == 1 ? lo : st_cases[i].fill == 2 ? hi : lo + (int)(rng() % (uint32_t)(hi - lo + 1)));
        }
        for (size_t k = 0; k < (size_t)nb * K; k++)
            A[k] = (int8_t)(st_cases[i].fill ? -128 : (int)(rng() & 0xFF) - 128);
        for (uint32_t r = 0; r < N; r++)
            for (uint32_t k = 0; k < K; k++) {
                uint8_t *b = wire + (size_t)r * wl;
                if (mode)
                    b[k] = (uint8_t)W[(size_t)r * K + k];
                else
                    b[k >> 1] |= (uint8_t)(((uint8_t)W[(size_t)r * K + k] & 0xF) << ((k & 1) * 4));
            }
        if (tensor_alloc(e, &t, mode, N, K)) {
            code = (i + 1) << 8 | 1;
            goto next;
        }
        for (uint32_t r = 0; r < N; r++)
            store_row(e, &t, r, wire + (size_t)r * wl, beats);
        if (e->gemv(e, &t, nb, A, Y, &cyc))
            code = (i + 1) << 8 | 2;
        else {
            ref_gemv(N, K, nb, W, A, R);
            if (memcmp(Y, R, (size_t)N * nb * 4))
                code = (i + 1) << 8 | 3;
        }
        pool_release(&e->pool, t.off, t.bytes);
    next:
        free(W);
        free(A);
        free(wire);
        free(Y);
        free(R);
        if (code) {
            free(beats);
            return code;
        }
    }
    free(beats);
    return 0;
}

/* ---- connections ----------------------------------------------------------------------- */
typedef struct {
    int fd;
    uint8_t *rb; /* receive buffer */
    size_t rpos, rlen;
    uint8_t *buf; /* scratch: GEMV payload, LOAD rows, drain */
} conn_t;

enum { R_CLOSE = 0, R_KEEP = 1, R_LINGER = 2 };

static int rd(conn_t *c, void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n) {
        if (c->rpos < c->rlen) {
            size_t m = c->rlen - c->rpos;
            if (m > n)
                m = n;
            memcpy(d, c->rb + c->rpos, m);
            c->rpos += m;
            d += m;
            n -= m;
            continue;
        }
        int direct = n >= RBUF;
        ssize_t k = recv(c->fd, direct ? d : c->rb, direct ? n : RBUF, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        if (direct) {
            d += k;
            n -= (size_t)k;
        } else {
            c->rpos = 0;
            c->rlen = (size_t)k;
        }
    }
    return 0;
}

static int drain(conn_t *c, uint64_t n)
{
    while (n) {
        size_t m = n > RBUF ? RBUF : (size_t)n;
        if (rd(c, c->buf, m))
            return -1;
        n -= m;
    }
    return 0;
}

static int send_all(int fd, const void *p, size_t n, int more)
{
    const uint8_t *s = p;
    while (n) {
        ssize_t k = send(fd, s, n, MSG_NOSIGNAL | (more ? MSG_MORE : 0));
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

static int reply(conn_t *c, uint32_t status, uint32_t seq, const void *payload, uint32_t len)
{
    uint8_t h[16];
    put32(h, REP_MAGIC);
    put32(h + 4, status);
    put32(h + 8, seq);
    put32(h + 12, len);
    if (send_all(c->fd, h, 16, len != 0))
        return -1;
    if (len && send_all(c->fd, payload, len, 0))
        return -1;
    return 0;
}

static int reply_st(conn_t *c, uint32_t status, uint32_t seq) { return reply(c, status, seq, NULL, 0) ? R_CLOSE : R_KEEP; }

/* status 1 for a request whose remaining payload is `rest` bytes: drain it when small and keep
 * the connection, otherwise answer and close */
static int bad_request(conn_t *c, uint32_t seq, uint64_t rest)
{
    if (rest <= DRAIN_MAX) {
        if (drain(c, rest))
            return R_CLOSE;
        return reply_st(c, ST_BAD, seq);
    }
    reply(c, ST_BAD, seq, NULL, 0);
    return R_LINGER;
}

/* close so that the peer still gets our last reply: stop sending, swallow what it is still
 * sending (bounded in time), then close -- closing with unread input would reset the link */
static void linger_close(conn_t *c)
{
    shutdown(c->fd, SHUT_WR);
    struct timeval tv = {1, 0};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint64_t t0 = now_us();
    while (now_us() - t0 < 10000000u && recv(c->fd, c->buf, RBUF, 0) > 0)
        ;
}

static int op_info(conn_t *c, uint32_t seq, uint32_t len)
{
    if (len)
        return bad_request(c, seq, len);
    uint8_t p[28];
    pthread_mutex_lock(&g_lock);
    put32(p, 1);
    put32(p + 4, g_eng.info_id);
    put32(p + 8, (uint32_t)(g_eng.mem_size >> 20));
    put32(p + 12, (uint32_t)(g_eng.pool.free >> 20));
    put32(p + 16, MAX_COLS);
    put32(p + 20, MAX_BATCH);
    put32(p + 24, g_selftest);
    pthread_mutex_unlock(&g_lock);
    return reply(c, ST_OK, seq, p, sizeof p) ? R_CLOSE : R_KEEP;
}

static int op_load(conn_t *c, uint32_t seq, uint32_t len)
{
    if (len < 12)
        return bad_request(c, seq, len);
    if (rd(c, c->buf, 12))
        return R_CLOSE;
    const uint32_t mode = get32(c->buf), rows = get32(c->buf + 4), cols = get32(c->buf + 8);
    const uint64_t rest = (uint64_t)len - 12;
    if (mode > 1 || rows < 1 || rows > MAX_ROWS || cols < 1 || cols > MAX_COLS)
        return bad_request(c, seq, rest);
    /* exactly rows x rowbytes: each row packed as in SPEC section 1 and padded to whole beats */
    const uint32_t rowlen = beats_per_row(mode, cols) * 8;
    if (rest != (uint64_t)rows * rowlen)
        return bad_request(c, seq, rest);

    tensor_t t;
    pthread_mutex_lock(&g_lock);
    int ok = tensor_alloc(&g_eng, &t, mode, rows, cols) == 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        if (drain(c, rest))
            return R_CLOSE;
        return reply_st(c, ST_NOMEM, seq);
    }
    /* the block is ours until it is published, so it is filled without the lock */
    for (uint32_t r = 0; r < rows; r++) {
        if (rd(c, c->buf, rowlen)) {
            pthread_mutex_lock(&g_lock);
            pool_release(&g_eng.pool, t.off, t.bytes);
            pthread_mutex_unlock(&g_lock);
            return R_CLOSE;
        }
        store_row(&g_eng, &t, r, c->buf, c->buf + 8192);
    }
    pthread_mutex_lock(&g_lock);
    ok = tens_add(&t) == 0;
    if (!ok)
        pool_release(&g_eng.pool, t.off, t.bytes);
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        return reply_st(c, ST_NOMEM, seq);
    uint8_t p[4];
    put32(p, t.id);
    return reply(c, ST_OK, seq, p, 4) ? R_CLOSE : R_KEEP;
}

static int op_free(conn_t *c, uint32_t seq, uint32_t len)
{
    if (len != 4)
        return bad_request(c, seq, len);
    if (rd(c, c->buf, 4))
        return R_CLOSE;
    uint32_t id = get32(c->buf);
    pthread_mutex_lock(&g_lock);
    tensor_t *t = tens_find(id);
    if (t) {
        pool_release(&g_eng.pool, t->off, t->bytes);
        tens_del(t);
    }
    pthread_mutex_unlock(&g_lock);
    return reply_st(c, t ? ST_OK : ST_UNKNOWN, seq);
}

static int op_gemv(conn_t *c, uint32_t seq, uint32_t len)
{
    if (len < 8 || len > 8 + MAX_BATCH * MAX_COLS)
        return bad_request(c, seq, len);
    if (rd(c, c->buf, len))
        return R_CLOSE;
    const uint32_t id = get32(c->buf), nb = get32(c->buf + 4);
    pthread_mutex_lock(&g_lock);
    tensor_t *tp = tens_find(id);
    if (!tp) {
        pthread_mutex_unlock(&g_lock);
        return reply_st(c, ST_UNKNOWN, seq);
    }
    if (nb < 1 || nb > MAX_BATCH || len != 8 + nb * tp->cols) {
        pthread_mutex_unlock(&g_lock);
        return reply_st(c, ST_BAD, seq);
    }
    const tensor_t t = *tp;
    const size_t plen = 8 + (size_t)t.rows * nb * 4;
    uint8_t *out = malloc(plen);
    if (!out) {
        pthread_mutex_unlock(&g_lock);
        return reply_st(c, ST_NOMEM, seq);
    }
    uint32_t cycles = 0;
    int rc = g_eng.gemv(&g_eng, &t, nb, (const int8_t *)(c->buf + 8), (int32_t *)(void *)(out + 8), &cycles);
    uint32_t used = g_eng.info_id;
    pthread_mutex_unlock(&g_lock);
    int r;
    if (rc) {
        r = reply_st(c, ST_ENGINE, seq);
    } else {
        put32(out, cycles);
        put32(out + 4, used);
        r = reply(c, ST_OK, seq, out, (uint32_t)plen) ? R_CLOSE : R_KEEP;
    }
    free(out);
    return r;
}

static int op_ping(conn_t *c, uint32_t seq, uint32_t len)
{
    if (len > PING_MAX)
        return bad_request(c, seq, len);
    uint8_t *p = len ? malloc(len) : NULL;
    if (len && !p) {
        if (drain(c, len))
            return R_CLOSE;
        return reply_st(c, ST_NOMEM, seq);
    }
    int r = R_CLOSE;
    if (!rd(c, p, len))
        r = reply(c, ST_OK, seq, p, len) ? R_CLOSE : R_KEEP;
    free(p);
    return r;
}

static void *conn_main(void *arg)
{
    conn_t *c = arg;
    uint8_t h[16];
    int r = R_CLOSE;
    while (!rd(c, h, 16)) {
        const uint32_t magic = get32(h), op = get32(h + 4), seq = get32(h + 8), len = get32(h + 12);
        if (magic != REQ_MAGIC) {
            reply(c, ST_BAD, seq, NULL, 0);
            r = R_LINGER;
            break;
        }
        switch (op) {
        case OP_INFO: r = op_info(c, seq, len); break;
        case OP_LOAD: r = op_load(c, seq, len); break;
        case OP_FREE: r = op_free(c, seq, len); break;
        case OP_GEMV: r = op_gemv(c, seq, len); break;
        case OP_PING: r = op_ping(c, seq, len); break;
        default: r = bad_request(c, seq, len); break;
        }
        if (r != R_KEEP)
            break;
        r = R_CLOSE;
    }
    if (r == R_LINGER)
        linger_close(c);
    close(c->fd);
    free(c->rb);
    free(c->buf);
    free(c);
    return NULL;
}

/* ---- main ------------------------------------------------------------------------------ */
static void usage(void)
{
    fprintf(stderr,
            "usage: zaccel-server [--port N] [--cpu | --model [--model-fault N]]\n"
            "  default engine: pl when the PL is configured and zaccel-dma/zaccel-mem exist, else cpu\n"
            "  --cpu          force the cpu engine\n"
            "  --model        tests only: the pl code path against a software model of the PL\n"
            "  --model-fault  tests only: corrupt the trailer of the Nth client job (-1: every selftest job)\n");
}

int main(int argc, char **argv)
{
    int port = 8093, want = -1; /* -1 auto */
    long fault = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cpu")) {
            want = ENG_CPU;
        } else if (!strcmp(argv[i], "--model")) {
            want = ENG_MODEL;
        } else if (!strcmp(argv[i], "--model-fault") && i + 1 < argc) {
            fault = strtol(argv[++i], NULL, 0);
        } else {
            usage();
            return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
        }
    }
    if (port < 1 || port > 65535) {
        usage();
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g_eng.u_dma.fd = g_eng.u_mem.fd = -1;

    char why[256] = "";
    int ok;
    if (want == ENG_CPU) {
        ok = cpu_init(&g_eng) == 0;
    } else if (want == ENG_MODEL) {
        ok = model_init(&g_eng, fault, why, sizeof why) == 0;
        if (!ok)
            say("model engine failed: %s", why);
    } else {
        ok = pl_init(&g_eng, why, sizeof why) == 0;
        if (!ok) {
            say("pl engine unavailable: %s -> cpu engine", why);
            ok = cpu_init(&g_eng) == 0;
        }
    }
    if (!ok) {
        say("cannot set up an engine");
        return 1;
    }

    g_selftesting = 1;
    uint32_t st = selftest(&g_eng);
    g_selftesting = 0;
    if (st && g_eng.kind != ENG_CPU) {
        say("selftest FAILED on the %s engine (code 0x%x) -> cpu engine", g_eng.name, st);
        if (cpu_init(&g_eng)) {
            say("cannot set up the cpu engine");
            return 1;
        }
        g_selftesting = 1;
        uint32_t st2 = selftest(&g_eng);
        g_selftesting = 0;
        if (st2)
            say("selftest FAILED on the cpu engine too (code 0x%x)", st2);
    } else if (st) {
        say("selftest FAILED on the cpu engine (code 0x%x)", st);
    }
    g_selftest = st;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 64)) {
        say("cannot listen on TCP %d: %s", port, strerror(errno));
        return 1;
    }
    say("engine %s, %u MB tensor memory, selftest %s (0x%x), listening on TCP %d", g_eng.name,
        (unsigned)(g_eng.mem_size >> 20), st ? "FAIL" : "pass", st, port);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u << 10);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                struct timespec ts = {0, 100000000};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_t *c = calloc(1, sizeof *c);
        if (c) {
            c->fd = fd;
            c->rb = malloc(RBUF);
            c->buf = malloc(RBUF);
        }
        pthread_t th;
        if (!c || !c->rb || !c->buf || pthread_create(&th, &attr, conn_main, c)) {
            if (c) {
                free(c->rb);
                free(c->buf);
                free(c);
            }
            close(fd);
        }
    }
}

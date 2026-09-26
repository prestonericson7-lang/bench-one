/* zaccel_cpu -- see zaccel_cpu.h.  Must match zaccel_ref_gemv() bit for bit.
 *
 * NEON SDOT path (built with -march=armv8.2-a+dotprod):
 *   A weight row is read 16 bytes at a time.  For int4 those 16 bytes are 32 weights: the low
 *   nibbles (even k) and the high nibbles (odd k) are sign-extended with two shifts each, and the
 *   activations were permuted once per call to match -- block c of vector v holds
 *   A[32c+0], A[32c+2], ..., A[32c+30], A[32c+1], A[32c+3], ..., A[32c+31]  (zero past cols).
 *   Blocks are stored chunk-major ([chunk][vector][32]) so one pass over a row walks the prepared
 *   activations sequentially.  Each 16-byte weight load feeds nb vectors (up to 8 accumulators),
 *   so a batch reads the weights once.  A row whose packed length is an odd number of 8-byte beats
 *   ends in a half chunk, loaded as 8 bytes and zero-extended.  Weights past cols meet zero
 *   activations, so tail padding never reaches the answer.
 *   Sums are exact: |sum| <= 4096*128*128 = 2^26 in every int32 lane.
 *
 * Portable path: unpack the row to int8, then plain int32 dot products (auto-vectorised).
 */
#define _POSIX_C_SOURCE 200809L

#include "zaccel_cpu.h"
#include "libzaccel.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_DOTPROD)
#include <arm_neon.h>
#define ZC_SDOT 1
#else
#define ZC_SDOT 0
#endif

#define ACT_BYTES (ZACCEL_MAX_BATCH * ZACCEL_MAX_COLS + 64)   /* prepared activations */
#define SCRATCH   (ZACCEL_MAX_COLS + 64)                      /* per-thread unpacked row */

typedef struct {
    uint32_t mode, rows, cols, nb, chunk;
    size_t rb;                /* packed bytes per row                                   */
    const uint8_t *w;
    const int8_t *a;          /* prepared activations (SDOT) or the caller's A (portable) */
    int32_t *y;
} job_t;

struct worker { zaccel_cpu_t *p; int idx; };

struct zaccel_cpu {
    int n;                    /* threads, the caller included */
    pthread_t *tid;
    struct worker *wk;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    unsigned long gen;
    int busy, quit;
    job_t job;
    atomic_size_t next;       /* next row to hand out */
    int8_t *act;
    int8_t *scratch;
};

const char *zaccel_cpu_kernel(void) { return ZC_SDOT ? "neon-sdot" : "portable-c"; }
int zaccel_cpu_threads(const zaccel_cpu_t *p) { return p ? p->n : 0; }

/* ---------------------------------------------------------------- kernels */
#if ZC_SDOT
static inline __attribute__((always_inline))
void rows_i4(const job_t *j, uint32_t r0, uint32_t r1, const int NB)
{
    const size_t rb = j->rb, full = rb / 16;
    const int half = (rb & 8) != 0;
    for (uint32_t r = r0; r < r1; r++) {
        const uint8_t *wr = j->w + (size_t)r * rb;
        const int8_t *ap = j->a;
        int32x4_t acc[8];
#pragma GCC unroll 8
        for (int v = 0; v < NB; v++) acc[v] = vdupq_n_s32(0);
        for (size_t c = 0; c < full; c++) {
            int8x16_t w  = vreinterpretq_s8_u8(vld1q_u8(wr + 16 * c));
            int8x16_t lo = vshrq_n_s8(vshlq_n_s8(w, 4), 4);
            int8x16_t hi = vshrq_n_s8(w, 4);
#pragma GCC unroll 8
            for (int v = 0; v < NB; v++) {
                acc[v] = vdotq_s32(acc[v], lo, vld1q_s8(ap));
                acc[v] = vdotq_s32(acc[v], hi, vld1q_s8(ap + 16));
                ap += 32;
            }
        }
        if (half) {
            int8x16_t w  = vreinterpretq_s8_u8(vcombine_u8(vld1_u8(wr + 16 * full), vdup_n_u8(0)));
            int8x16_t lo = vshrq_n_s8(vshlq_n_s8(w, 4), 4);
            int8x16_t hi = vshrq_n_s8(w, 4);
#pragma GCC unroll 8
            for (int v = 0; v < NB; v++) {
                acc[v] = vdotq_s32(acc[v], lo, vld1q_s8(ap));
                acc[v] = vdotq_s32(acc[v], hi, vld1q_s8(ap + 16));
                ap += 32;
            }
        }
        int32_t *y = j->y + (size_t)r * NB;
#pragma GCC unroll 8
        for (int v = 0; v < NB; v++) y[v] = vaddvq_s32(acc[v]);
    }
}

static inline __attribute__((always_inline))
void rows_i8(const job_t *j, uint32_t r0, uint32_t r1, const int NB)
{
    const size_t rb = j->rb, full = rb / 16;
    const int half = (rb & 8) != 0;
    for (uint32_t r = r0; r < r1; r++) {
        const int8_t *wr = (const int8_t *)(j->w + (size_t)r * rb);
        const int8_t *ap = j->a;
        int32x4_t acc[8];
#pragma GCC unroll 8
        for (int v = 0; v < NB; v++) acc[v] = vdupq_n_s32(0);
        for (size_t c = 0; c < full; c++) {
            int8x16_t w = vld1q_s8(wr + 16 * c);
#pragma GCC unroll 8
            for (int v = 0; v < NB; v++) { acc[v] = vdotq_s32(acc[v], w, vld1q_s8(ap)); ap += 16; }
        }
        if (half) {
            int8x16_t w = vcombine_s8(vld1_s8(wr + 16 * full), vdup_n_s8(0));
#pragma GCC unroll 8
            for (int v = 0; v < NB; v++) { acc[v] = vdotq_s32(acc[v], w, vld1q_s8(ap)); ap += 16; }
        }
        int32_t *y = j->y + (size_t)r * NB;
#pragma GCC unroll 8
        for (int v = 0; v < NB; v++) y[v] = vaddvq_s32(acc[v]);
    }
}

#define CASES(fn) \
    case 1: fn(j, r0, r1, 1); break; case 2: fn(j, r0, r1, 2); break; \
    case 3: fn(j, r0, r1, 3); break; case 4: fn(j, r0, r1, 4); break; \
    case 5: fn(j, r0, r1, 5); break; case 6: fn(j, r0, r1, 6); break; \
    case 7: fn(j, r0, r1, 7); break; case 8: fn(j, r0, r1, 8); break;

static void do_rows(const job_t *j, int8_t *scratch, uint32_t r0, uint32_t r1)
{
    (void)scratch;
    if (j->mode == ZACCEL_MODE_INT4) { switch (j->nb) { CASES(rows_i4) default: break; } }
    else                             { switch (j->nb) { CASES(rows_i8) default: break; } }
}

/* chunk-major, permuted for int4 (see the file comment) */
static void prep_act(int8_t *d, uint32_t mode, uint32_t cols, uint32_t nb, size_t rb,
                     const int8_t *A)
{
    size_t nch = (rb + 15) / 16;
    for (size_t c = 0; c < nch; c++)
        for (uint32_t v = 0; v < nb; v++) {
            const int8_t *a = A + (size_t)v * cols;
            if (mode == ZACCEL_MODE_INT4) {
                for (uint32_t i = 0; i < 16; i++) {
                    size_t k0 = 32 * c + 2 * i, k1 = k0 + 1;
                    d[i]      = k0 < cols ? a[k0] : 0;
                    d[16 + i] = k1 < cols ? a[k1] : 0;
                }
                d += 32;
            } else {
                for (uint32_t i = 0; i < 16; i++) {
                    size_t k = 16 * c + i;
                    d[i] = k < cols ? a[k] : 0;
                }
                d += 16;
            }
        }
}

#else  /* portable C */

static void do_rows(const job_t *j, int8_t *s, uint32_t r0, uint32_t r1)
{
    const uint32_t cols = j->cols, nb = j->nb;
    for (uint32_t r = r0; r < r1; r++) {
        const uint8_t *wr = j->w + (size_t)r * j->rb;
        const int8_t *w;
        if (j->mode == ZACCEL_MODE_INT4) {
            uint32_t nbytes = (cols + 1) / 2;
            for (uint32_t i = 0; i < nbytes; i++) {
                int lo = wr[i] & 0xF, hi = wr[i] >> 4;
                s[2 * i]     = (int8_t)(lo - ((lo & 8) << 1));
                s[2 * i + 1] = (int8_t)(hi - ((hi & 8) << 1));
            }
            w = s;
        } else {
            w = (const int8_t *)wr;
        }
        int32_t *y = j->y + (size_t)r * nb;
        for (uint32_t v = 0; v < nb; v++) {
            const int8_t *a = j->a + (size_t)v * cols;
            int32_t acc = 0;
            for (uint32_t k = 0; k < cols; k++) acc += (int32_t)w[k] * (int32_t)a[k];
            y[v] = acc;
        }
    }
}
#endif

/* ---------------------------------------------------------------- the pool */
static void run(zaccel_cpu_t *p, int idx)
{
    const job_t *j = &p->job;
    int8_t *s = p->scratch + (size_t)idx * SCRATCH;
    for (;;) {
        size_t r0 = atomic_fetch_add_explicit(&p->next, j->chunk, memory_order_relaxed);
        if (r0 >= j->rows) return;
        size_t r1 = r0 + j->chunk < j->rows ? r0 + j->chunk : j->rows;
        do_rows(j, s, (uint32_t)r0, (uint32_t)r1);
    }
}

static void *thread_main(void *arg)
{
    struct worker *w = arg;
    zaccel_cpu_t *p = w->p;
    unsigned long seen = 0;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (!p->quit && p->gen == seen) pthread_cond_wait(&p->go, &p->mu);
        if (p->quit) break;
        seen = p->gen;
        pthread_mutex_unlock(&p->mu);
        run(p, w->idx);
        pthread_mutex_lock(&p->mu);
        if (--p->busy == 0) pthread_cond_signal(&p->done);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

zaccel_cpu_t *zaccel_cpu_create(int nthreads)
{
    if (nthreads <= 0) {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = c > 0 ? (int)c : 1;
    }
    if (nthreads > 256) nthreads = 256;
    zaccel_cpu_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->act = aligned_alloc(64, ACT_BYTES);
    p->scratch = aligned_alloc(64, (size_t)nthreads * SCRATCH);
    p->tid = calloc((size_t)nthreads, sizeof *p->tid);
    p->wk = calloc((size_t)nthreads, sizeof *p->wk);
    if (!p->act || !p->scratch || !p->tid || !p->wk) {
        free(p->act); free(p->scratch); free(p->tid); free(p->wk); free(p);
        return NULL;
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->go, NULL);
    pthread_cond_init(&p->done, NULL);
    atomic_init(&p->next, 0);
    p->n = 1;
    for (int i = 0; i < nthreads; i++) { p->wk[i].p = p; p->wk[i].idx = i; }
    for (int i = 1; i < nthreads; i++) {
        if (pthread_create(&p->tid[i], NULL, thread_main, &p->wk[i]) != 0) break;
        p->n = i + 1;
    }
    return p;
}

void zaccel_cpu_destroy(zaccel_cpu_t *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->mu);
    for (int i = 1; i < p->n; i++) pthread_join(p->tid[i], NULL);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->go);
    pthread_cond_destroy(&p->done);
    free(p->act); free(p->scratch); free(p->tid); free(p->wk); free(p);
}

int zaccel_cpu_gemv(zaccel_cpu_t *p, uint32_t mode, uint32_t rows, uint32_t cols,
                    const uint8_t *packed, uint32_t nb, const int8_t *A, int32_t *Y)
{
    if (!p || mode > ZACCEL_MODE_INT8 || rows == 0 || cols == 0 || cols > ZACCEL_MAX_COLS ||
        nb == 0 || nb > ZACCEL_MAX_BATCH || !packed || !A || !Y)
        return ZACCEL_E_ARG;
    job_t j;
    j.mode = mode; j.rows = rows; j.cols = cols; j.nb = nb;
    j.rb = zaccel_row_bytes(mode, cols);
    j.w = packed; j.y = Y;
#if ZC_SDOT
    prep_act(p->act, mode, cols, nb, j.rb, A);
    j.a = p->act;
#else
    j.a = A;
#endif
    uint32_t chunk = rows / ((uint32_t)p->n * 16u);
    j.chunk = chunk < 1 ? 1 : chunk > 64 ? 64 : chunk;
    p->job = j;
    atomic_store_explicit(&p->next, 0, memory_order_relaxed);
    if (p->n > 1) {
        pthread_mutex_lock(&p->mu);
        p->gen++;
        p->busy = p->n - 1;
        pthread_cond_broadcast(&p->go);
        pthread_mutex_unlock(&p->mu);
    }
    run(p, 0);
    if (p->n > 1) {
        pthread_mutex_lock(&p->mu);
        while (p->busy) pthread_cond_wait(&p->done, &p->mu);
        pthread_mutex_unlock(&p->mu);
    }
    return ZACCEL_OK;
}

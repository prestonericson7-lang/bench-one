/* ===========================================================================================
 *  dot_verify.c -- prove the vector kernel is bit-identical to the scalar one, then time it
 * ===========================================================================================
 *
 *  WHY BIT-IDENTICAL AND NOT "CLOSE"
 *  ----------------------------------
 *  The fused kernel multiplies quantized weights by a quantized activation in INTEGER arithmetic and
 *  only converts to floating point once per 32 weights, at the scale. Integer addition is
 *  associative, so reducing the same 32 products in a different order cannot change the result. The
 *  vector version therefore has to produce the same bits as the scalar one, and if it produces
 *  something merely close then something is wrong: a saturating instruction where it should not be,
 *  a sign confusion between the unsigned nibble and the signed activation, a misread scale.
 *
 *  "Close enough" is exactly how a kernel bug hides inside quantization noise. This refuses to let
 *  it. Both kernels are in the same binary and gguf_dot_force_scalar picks between them, so the
 *  comparison is of the code actually shipping rather than of two builds.
 *
 *  WHAT IT MEASURES
 *  ----------------
 *  The speedup, which is the reason the vector path was written, and which decides a conclusion this
 *  project has been leaning on: decode_limit reported decode as unpacking bound by 3.0x, but that
 *  ceiling was measured against the scalar kernel. If vectorizing lifts it above the memory ceiling
 *  the verdict flips, and the argument for putting the unpacking in FPGA fabric weakens.
 *
 *  RUN
 *      dot_verify [rows] [cols]
 * ======================================================================================== */

#include "gguf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <windows.h>
  static double now_s(void)
  {
      LARGE_INTEGER f, t;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t);
      return (double)t.QuadPart / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double now_s(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
#endif

static uint64_t rs = 0x853c49e6748fea9bULL;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (uint32_t)(rs >> 32);
}

/* Random bytes are a valid Q4_K block: every field is either a nibble pair, a packed 6-bit scale, or
 * an fp16. Random fp16 can be inf or nan, though, and a nan would make the comparison meaningless
 * for the wrong reason, so the two fp16 fields are given sane exponents. */
static void fill_q4k(uint8_t *raw, uint64_t nblocks)
{
    for (uint64_t b = 0; b < nblocks; b++) {
        uint8_t *p = raw + b * 144u;
        for (int i = 0; i < 144; i++) p[i] = (uint8_t)(rnd() >> 7);
        /* fp16 with exponent in a normal range: sign random, exponent 0x0C..0x11, mantissa random */
        for (int k = 0; k < 2; k++) {
            const uint16_t mant = (uint16_t)(rnd() & 0x03FF);
            const uint16_t expo = (uint16_t)(0x0C + (rnd() % 6));
            const uint16_t sign = (uint16_t)((rnd() & 1) << 15);
            const uint16_t v = (uint16_t)(sign | (expo << 10) | mant);
            memcpy(p + k * 2, &v, 2);
        }
    }
}

/* Q6_K blocks are 210 bytes: 128 low nibbles, 32 high-bit bytes, 16 signed scales, one fp16.
 * Random bytes are valid for all of it except the fp16, same as Q4_K. */
static void fill_q6k(uint8_t *raw, uint64_t nblocks)
{
    for (uint64_t b = 0; b < nblocks; b++) {
        uint8_t *p = raw + b * 210u;
        for (int i = 0; i < 210; i++) p[i] = (uint8_t)(rnd() >> 7);
        const uint16_t mant = (uint16_t)(rnd() & 0x03FF);
        const uint16_t expo = (uint16_t)(0x0C + (rnd() % 6));
        const uint16_t sign = (uint16_t)((rnd() & 1) << 15);
        const uint16_t v = (uint16_t)(sign | (expo << 10) | mant);
        memcpy(p + 208, &v, 2);
    }
}

/* One type, end to end: build weights, run both kernels, compare bits, report the rate.
 *
 * Repeated until at least a second of work has happened in each. The first version ran for ten
 * milliseconds and reported a 1.23x speedup, which was mostly the scalar pass paying to fill the
 * cache that the vector pass then read warm. A timing that short measures the cache, not the
 * kernel. */
static int run_type(uint32_t type, const char *name, int rows, int cols, uint64_t blkbytes,
                    void (*fill)(uint8_t *, uint64_t))
{
    const uint64_t nblk = (uint64_t)cols / 256;
    const uint64_t rowb = nblk * blkbytes;

    uint8_t *w  = (uint8_t *)malloc((size_t)rowb * rows);
    float   *xf = (float *)malloc((size_t)cols * sizeof(float));
    int8_t  *xq = (int8_t *)malloc((size_t)cols);
    float   *xs = (float *)malloc((size_t)(cols / 32 + 1) * sizeof(float));
    float   *a  = (float *)malloc((size_t)rows * sizeof(float));
    float   *b  = (float *)malloc((size_t)rows * sizeof(float));
    if (!w || !xf || !xq || !xs || !a || !b) { printf("  out of memory\n"); return 1; }

    fill(w, nblk * (uint64_t)rows);
    for (int i = 0; i < cols; i++)
        xf[i] = ((float)(rnd() % 20001) - 10000.0f) / 3000.0f;
    gguf_quantize_act(xf, (uint64_t)cols, xq, xs);

    /* Touch everything once so neither pass pays the first-touch cost for the other. */
    for (int r = 0; r < rows; r++)
        a[r] = gguf_dot_q(type, w + (size_t)r * rowb, xq, xs, (uint64_t)cols);

    int reps = 0;
    double t0 = now_s(), t_ref, t_vec;
    gguf_dot_force_scalar(1);
    do {
        for (int r = 0; r < rows; r++)
            a[r] = gguf_dot_q(type, w + (size_t)r * rowb, xq, xs, (uint64_t)cols);
        reps++;
    } while (now_s() - t0 < 1.0);
    t_ref = (now_s() - t0) / reps;

    gguf_dot_force_scalar(0);
    t0 = now_s();
    for (int i = 0; i < reps; i++)
        for (int r = 0; r < rows; r++)
            b[r] = gguf_dot_q(type, w + (size_t)r * rowb, xq, xs, (uint64_t)cols);
    t_vec = (now_s() - t0) / reps;

    int differ = 0;
    float worst = 0.0f;
    int worst_row = -1;
    for (int r = 0; r < rows; r++) {
        uint32_t ua, ub;
        memcpy(&ua, &a[r], 4);
        memcpy(&ub, &b[r], 4);
        if (ua != ub) {
            differ++;
            const float rel = (a[r] != 0.0f) ? (b[r] - a[r]) / a[r] : (b[r] - a[r]);
            const float ar = rel < 0 ? -rel : rel;
            if (ar > worst) { worst = ar; worst_row = r; }
        }
    }

    /* Q4_K's presum kernel -- what the Teensy runs for every Q4_K matrix but q/k/v -- against the same
     * scalar results: the activation sums hoisted out must leave every bit where it was. */
    int presum_differ = 0;
    if (type == GGML_Q4_K) {
        int32_t *xsum = (int32_t *)malloc((size_t)(cols / 32 + 1) * sizeof(int32_t));
        if (!xsum) { printf("  out of memory\n"); return 1; }
        gguf_act_sums(xq, (uint64_t)cols, xsum);
        for (int r = 0; r < rows; r++) {
            const float c = gguf_dot_q4k_presum(w + (size_t)r * rowb, xq, xs, xsum, (uint64_t)cols);
            uint32_t ua, uc;
            memcpy(&ua, &a[r], 4);
            memcpy(&uc, &c, 4);
            presum_differ += (ua != uc);
        }
        free(xsum);
    }

    /* The batched kernels -- one row against GGUF_NPOS_MAX different vectors -- against the single-vector
     * kernel run on each vector: every output must carry the same bits. */
    int batch_differ = 0;
    {
        const int NP = GGUF_NPOS_MAX;
        int8_t  *bq[GGUF_NPOS_MAX];
        float   *bs[GGUF_NPOS_MAX];
        int32_t *bm[GGUF_NPOS_MAX];
        for (int p = 0; p < NP; p++) {
            bq[p] = (int8_t *)malloc((size_t)cols);
            bs[p] = (float *)malloc((size_t)(cols / 32 + 1) * sizeof(float));
            bm[p] = (int32_t *)malloc((size_t)(cols / 32 + 1) * sizeof(int32_t));
            if (!bq[p] || !bs[p] || !bm[p]) { printf("  out of memory\n"); return 1; }
            for (int i = 0; i < cols; i++) xf[i] = ((float)(rnd() % 20001) - 10000.0f) / (1000.0f + 700.0f * p);
            gguf_quantize_act(xf, (uint64_t)cols, bq[p], bs[p]);
            gguf_act_sums(bq[p], (uint64_t)cols, bm[p]);
        }
        for (int r = 0; r < rows; r++) {
            const uint8_t *row = w + (size_t)r * rowb;
            for (int np = 1; np <= NP; np += (np < 2 ? 1 : 3)) {       /* 1, 2, 5, 8 vectors */
                float o[GGUF_NPOS_MAX];
                if (type == GGML_Q4_K)
                    gguf_dot_q4k_presum_n(row, np, (const int8_t *const *)bq, (const float *const *)bs,
                                          (const int32_t *const *)bm, (uint64_t)cols, o);
                else
                    gguf_dot_q_n(type, row, np, (const int8_t *const *)bq, (const float *const *)bs, (uint64_t)cols, o);
                for (int p = 0; p < np; p++) {
                    const float s = gguf_dot_q(type, row, bq[p], bs[p], (uint64_t)cols);
                    uint32_t u1, u2;
                    memcpy(&u1, &o[p], 4);
                    memcpy(&u2, &s, 4);
                    batch_differ += (u1 != u2);
                }
            }
        }
        for (int p = 0; p < NP; p++) { free(bq[p]); free(bs[p]); free(bm[p]); }
    }

    const double bytes = (double)rowb * rows;
    printf("  %s, %d rows of %d, %.1f MB, %d passes each\n", name, rows, cols,
           bytes / 1048576.0, reps);
    if (type == GGML_Q4_K)
        printf("    presum kernel: %s\n", presum_differ ? "DIFFERS from the scalar reference" : "IDENTICAL to the scalar reference on every row");
    printf("    batched kernel, 1/2/5/8 vectors per row: %s\n",
           batch_differ ? "DIFFERS from the single-vector kernel" : "IDENTICAL to the single-vector kernel on every row and vector");
    differ += presum_differ + batch_differ;
    if (differ) {
        printf("    DIFFER on %d of %d rows, worst %.3e at row %d (%.9g against %.9g)\n",
               differ, rows, (double)worst, worst_row,
               (double)a[worst_row], (double)b[worst_row]);
        printf("    Integer reduction is associative, so this is a bug and not rounding.\n");
    } else {
        printf("    IDENTICAL on all %d rows, to the bit\n", rows);
    }
    printf("    scalar %7.2f GB/s   vector %7.2f GB/s   speedup %.2fx\n\n",
           bytes / t_ref / 1073741824.0, bytes / t_vec / 1073741824.0, t_ref / t_vec);

    free(w); free(xf); free(xq); free(xs); free(a); free(b);
    return differ;
}

int main(int argc, char **argv)
{
    const int rows = (argc > 1) ? atoi(argv[1]) : 4096;
    const int cols = (argc > 2) ? atoi(argv[2]) : 11008;
    if (cols % 256) { printf("cols must be a multiple of 256\n"); return 1; }

    printf("\n========================================================================\n");
    printf("  the fused kernels: scalar against vector, on one core\n");
    printf("========================================================================\n");
    printf("  this build uses: %s\n\n", gguf_dot_kernel());

    int bad = 0;
    bad += run_type(GGML_Q4_K, "Q4_K", rows, cols, 144u, fill_q4k);
    bad += run_type(GGML_Q6_K, "Q6_K", rows, cols, 210u, fill_q6k);

    printf("  Single threaded on purpose. decode_limit runs these across eight cores, and a\n");
    printf("  per-core figure is the one that compares against a board.\n\n");
    return bad ? 1 : 0;
}

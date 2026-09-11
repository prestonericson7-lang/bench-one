/* ===========================================================================================
 *  fast_path.c -- is the integer dot product correct, and does it break the 5.47 GB/s ceiling?
 * ===========================================================================================
 *
 *  docs/23 concluded that 4-bit decode on a processor is limited by unpacking at 5.47 GB/s, six times
 *  below this host's 32.49 GB/s of memory bandwidth, and that the FPGA fabric is therefore worth 31x.
 *  That conclusion was drawn against an implementation that writes 256 floats to memory per 144-byte
 *  block so a dot product can read them back once. Blaming the format for a bad loop would be
 *  flattering the FPGA with someone else's slowness.
 *
 *  gguf_dot.c removes the round trip. This file asks the two questions that decide whether the
 *  conclusion survives, in the order that matters:
 *
 *    1. IS IT RIGHT? Rows of real tensors, fused integer dot against the float reference. A fast
 *       kernel that is subtly wrong is worse than a slow one, and quantizing the activation to int8
 *       is a real approximation whose size has to be measured rather than assumed.
 *
 *    2. IS IT FASTER? Same bytes, same threads, same machine.
 *
 *  If the fused path is several times quicker, the 31x shrinks and the architecture has to know it.
 *  If it is not, the ceiling stands against a genuine attempt instead of a straw man.
 *
 *  RUN
 *      fast_path <model.gguf>
 * ===========================================================================================
 */

#include "gguf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_OPENMP)
  #include <omp.h>
#endif

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

/* A plausible activation: zero-mean with a few large outliers, which is what comes out of a residual
 * stream and is exactly the shape that punishes one quantization scale for a whole vector. */
static void make_activation(float *x, uint64_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 1u;
    for (uint64_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        const float u = (float)((s >> 8) & 0xFFFF) / 65536.0f - 0.5f;
        x[i] = u * 2.0f;
        if (((s >> 24) & 0x3F) == 0) x[i] *= 20.0f;   /* about one element in 64 is an outlier */
    }
}

/* An activation that survives int8 quantization EXACTLY, so the kernel can be tested without the
 * approximation in the way.
 *
 * Every value is a whole number in -127..127, and the first element of each 32-block is forced to 127
 * so that block's scale comes out as exactly 1.0. Quantizing is then the identity, and any error left
 * over belongs to the kernel rather than to the format.
 *
 * This separation is the whole point. The first version of this test used a realistic activation and
 * reported 6% mean error, which is unusable as evidence: it cannot distinguish a wrong kernel from a
 * correct kernel paying the expected cost of 8-bit activations. Two tests answer two questions. */
static void make_activation_exact(float *x, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        x[i] = (i % 32 == 0) ? 127.0f : (float)((int)((i * 61u) % 255u) - 127);
}

/* Double accumulation, so the reference is not itself a source of error being blamed on the kernel. */
static double ref_dot(const float *w, const float *x, uint64_t n)
{
    double s = 0.0;
    for (uint64_t i = 0; i < n; i++) s += (double)w[i] * (double)x[i];
    return s;
}

typedef struct { double max_rel, mean_rel; uint64_t rows; } acc_t;

/* One row's raw bytes. Row by row rather than whole-tensor, because token_embd is 243 MB and this
 * only ever needs 2 KB of it at a time. */
static int read_row_raw(gguf_t *g, const gguf_tensor *t, uint64_t r, uint8_t *buf, uint64_t rb)
{
    const long long off = (long long)(g->data_start + t->offset + r * rb);
#if defined(_WIN32)
    if (_fseeki64(g->f, off, SEEK_SET) != 0) return -1;
#else
    if (fseeko(g->f, (off_t)off, SEEK_SET) != 0) return -1;
#endif
    return fread(buf, 1, (size_t)rb, g->f) == (size_t)rb ? 0 : -1;
}

static void check_tensor(gguf_t *g, const char *name, const float *x,
                         const int8_t *xq, const float *xs, acc_t *a)
{
    const gguf_tensor *t = gguf_tensor_find(g, name);
    if (!t) { printf("  %-26s MISSING\n", name); return; }

    const uint64_t cols = t->dims[0];
    const uint64_t rows = t->dims[1] ? t->dims[1] : 1;
    const uint64_t rb   = gguf_row_bytes(t);
    /* 256 rows finds a systematic error and is cheap enough to run on every tensor. A kernel that
     * unpacks one sub-block wrongly is wrong in every row, not in a rare one. */
    const uint64_t check_rows = rows < 256 ? rows : 256;

    uint8_t *raw = (uint8_t *)malloc((size_t)rb);
    float   *deq = (float *)malloc((size_t)cols * sizeof(float));
    if (!raw || !deq) { printf("  out of memory\n"); free(raw); free(deq); return; }

    double max_rel = 0.0, sum_rel = 0.0;
    uint64_t n_ok = 0;

    for (uint64_t r = 0; r < check_rows; r++) {
        if (gguf_read_rows_f32(g, t, r, 1, deq)) break;
        if (read_row_raw(g, t, r, raw, rb)) break;

        const double ref = ref_dot(deq, x, cols);
        const double got = (double)gguf_dot_q(t->type, raw, xq, xs, cols);
        /* Relative to the row's own magnitude, with a floor: a dot product that happens to land near
         * zero would otherwise report an enormous relative error from a tiny absolute one. */
        const double scale = fabs(ref) > 1e-6 ? fabs(ref) : 1e-6;
        const double rel = fabs(got - ref) / scale;
        if (rel > max_rel) max_rel = rel;
        sum_rel += rel;
        n_ok++;
    }

    if (n_ok) {
        printf("  %-26s %-5s %5llu rows   worst %7.4f   mean %7.5f\n",
               name, gguf_type_name(t->type), (unsigned long long)n_ok, max_rel, sum_rel / n_ok);
        a->rows += n_ok;
        if (max_rel > a->max_rel) a->max_rel = max_rel;
        a->mean_rel += sum_rel;
    } else {
        printf("  %-26s could not be read: %s\n", name, g->err);
    }
    free(raw);
    free(deq);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("fast_path <model.gguf>\n"); return 1; }

    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("open: %s\n", g.err); return 1; }

    int threads = 1;
#ifdef _OPENMP
#pragma omp parallel
    {
#pragma omp single
        threads = omp_get_num_threads();
    }
#endif

    const uint64_t dim  = (uint64_t)gguf_arch_int(&g, "embedding_length", 2048);
    const uint64_t hid  = (uint64_t)gguf_arch_int(&g, "feed_forward_length", 11008);
    const uint64_t wide = hid > dim ? hid : dim;

    float  *x  = (float *)malloc((size_t)wide * sizeof(float));
    int8_t *xq = (int8_t *)malloc((size_t)wide);
    float  *xs = (float *)malloc((size_t)(wide / 32 + 1) * sizeof(float));
    if (!x || !xq || !xs) { printf("out of memory\n"); return 1; }

    printf("\n========================================================================\n");
    printf("  the fused integer dot product: correct, and how fast\n");
    printf("========================================================================\n");

    /* ---- 1. IS THE KERNEL RIGHT? An activation with no quantization error at all ------------- */
    printf("\n  1. KERNEL CORRECTNESS -- activation chosen so int8 quantization is exact\n");
    printf("  Any error here is the kernel's. Expect float rounding, about 1e-6.\n");
    make_activation_exact(x, dim);
    gguf_quantize_act(x, dim, xq, xs);

    acc_t k;
    memset(&k, 0, sizeof(k));
    check_tensor(&g, "blk.0.attn_q.weight", x, xq, xs, &k);
    check_tensor(&g, "blk.0.attn_v.weight", x, xq, xs, &k);
    check_tensor(&g, "blk.0.attn_output.weight", x, xq, xs, &k);
    check_tensor(&g, "token_embd.weight", x, xq, xs, &k);
    make_activation_exact(x, hid);
    gguf_quantize_act(x, hid, xq, xs);
    check_tensor(&g, "blk.0.ffn_down.weight", x, xq, xs, &k);

    printf("\n  worst %.2e over %llu rows -- ", k.max_rel, (unsigned long long)k.rows);
    if (k.max_rel < 1e-4)
        printf("BOTH KERNELS AGREE. Q4_K and Q6_K unpacking is correct.\n");
    else
        printf("THE KERNEL IS WRONG. Fix it before reading anything below.\n");

    /* ---- 2. WHAT DOES THE INT8 ACTIVATION COST? Now with a realistic, hostile activation ----- */
    printf("\n  2. COST OF 8-BIT ACTIVATIONS -- same kernel, realistic activation\n");
    printf("  Zero-mean with one element in 64 at 20x, which is harsher than a real residual\n");
    printf("  stream and puts an outlier in about half of all 32-element scale blocks.\n");
    make_activation(x, dim, 0xBEEF);
    gguf_quantize_act(x, dim, xq, xs);

    acc_t a;
    memset(&a, 0, sizeof(a));
    check_tensor(&g, "blk.0.attn_q.weight", x, xq, xs, &a);
    check_tensor(&g, "blk.0.attn_k.weight", x, xq, xs, &a);
    check_tensor(&g, "blk.0.attn_v.weight", x, xq, xs, &a);
    check_tensor(&g, "blk.0.attn_output.weight", x, xq, xs, &a);
    check_tensor(&g, "blk.0.ffn_gate.weight", x, xq, xs, &a);
    check_tensor(&g, "blk.17.attn_q.weight", x, xq, xs, &a);
    check_tensor(&g, "token_embd.weight", x, xq, xs, &a);

    printf("  now a %llu-wide activation, for the one tensor that takes it:\n",
           (unsigned long long)hid);
    make_activation(x, hid, 0xF00D);
    gguf_quantize_act(x, hid, xq, xs);
    check_tensor(&g, "blk.0.ffn_down.weight", x, xq, xs, &a);

    printf("\n  across %llu rows: worst relative error %.4f, mean %.5f\n",
           (unsigned long long)a.rows, a.max_rel, a.rows ? a.mean_rel / a.rows : 0.0);
    printf("\n  Do not read that as the kernel being broken -- test 1 settled that. It is what a\n");
    printf("  dot product of 2048 random signed terms does: the sum is about sqrt(2048) times a\n");
    printf("  single term, so the terms mostly CANCEL, and the quantization error does not cancel\n");
    printf("  with them. A relative error on a heavily cancelled sum is a misleading measure.\n");
    printf("\n  What matters is whether the MODEL changes, not whether one dot product shifts a few\n");
    printf("  percent. The only honest test of that is running it: generate text with the float\n");
    printf("  path and with this one and compare token for token. Outliers also sit in a few fixed\n");
    printf("  channels in a real network rather than scattered at random as they are here, which is\n");
    printf("  why the reference implementations get away with an even coarser activation scale.\n");

    /* ---- 2. throughput, same bytes and threads as decode_limit ------------------------------ */
    printf("\n  THROUGHPUT -- one tensor held in memory, read repeatedly, %d threads\n", threads);

    static const char *bench[] = { "blk.0.attn_q.weight", "blk.0.ffn_down.weight" };
    double best_q4 = 0.0, best_q6 = 0.0;

    for (int bi = 0; bi < 2; bi++) {
        const gguf_tensor *t = gguf_tensor_find(&g, bench[bi]);
        if (!t) continue;
        const uint64_t cols = t->dims[0];
        const uint64_t rows = t->dims[1] ? t->dims[1] : 1;
        const uint64_t rb   = gguf_row_bytes(t);
        const uint64_t nb   = gguf_nbytes(t);

        uint8_t *raw = (uint8_t *)malloc((size_t)nb);
        float   *out = (float *)malloc((size_t)rows * sizeof(float));
        if (!raw || !out) { printf("  cannot hold %s\n", bench[bi]); free(raw); free(out); continue; }
        if (gguf_read_raw(&g, t, raw)) {
            printf("  %s: %s\n", bench[bi], g.err);
            free(raw); free(out);
            continue;
        }

        make_activation(x, cols, 0x1234);
        gguf_quantize_act(x, cols, xq, xs);

        const int reps = 12;

        /* The old path: dequantize a row into memory, then read it back for the dot product. */
        double t0 = now_s();
        for (int r = 0; r < reps; r++) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
            for (int64_t i = 0; i < (int64_t)rows; i++) {
                float row[16384];
                gguf_dequant(t->type, raw + (size_t)i * rb, cols, row);
                float s = 0.0f;
                for (uint64_t c = 0; c < cols; c++) s += row[c] * x[c];
                out[i] = s;
            }
        }
        const double t_deq = now_s() - t0;

        /* The fused path: integers straight out of the nibbles, no intermediate array. */
        t0 = now_s();
        for (int r = 0; r < reps; r++) {
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
            for (int64_t i = 0; i < (int64_t)rows; i++)
                out[i] = gguf_dot_q(t->type, raw + (size_t)i * rb, xq, xs, cols);
        }
        const double t_fus = now_s() - t0;

        const double gb = (double)nb * reps / 1073741824.0;
        const double r_deq = gb / t_deq, r_fus = gb / t_fus;
        printf("  %-24s %-5s  old %6.2f GB/s   FUSED %6.2f GB/s   %.2fx\n",
               bench[bi], gguf_type_name(t->type), r_deq, r_fus, r_deq > 0 ? r_fus / r_deq : 0.0);

        if (t->type == GGML_Q4_K && r_fus > best_q4) best_q4 = r_fus;
        if (t->type == GGML_Q6_K && r_fus > best_q6) best_q6 = r_fus;

        free(raw);
        free(out);
    }

    /* The real file is 69% Q4_K and 31% Q6_K by bytes. Time adds, rates do not, so the ceiling for
     * the mix is the harmonic mean rather than the average. */
    if (best_q4 > 0.0 && best_q6 > 0.0) {
        const double mix = 1.0 / (0.69 / best_q4 + 0.31 / best_q6);
        const double mb_tok = 1833.9;
        printf("\n  ceiling for the real 69/31 mix: %.2f GB/s, against 5.47 GB/s before\n", mix);
        printf("  that predicts %.2f tok/s for this model, against 1.30 measured with the old path\n",
               mix * 1024.0 / mb_tok);
        printf("  memory is still 32.49 GB/s, so the remaining gap to it is %.1fx\n", 32.49 / mix);
        printf("\n  The FPGA's advantage is now that gap, not the old one. plan.py and docs/23 both\n");
        printf("  carry the unpack ceiling as a number and both have to move with it.\n");
    }
    printf("\n");

    free(x);
    free(xq);
    free(xs);
    gguf_close(&g);
    return 0;
}

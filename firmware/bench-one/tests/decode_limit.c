/* ===========================================================================================
 *  decode_limit.c -- what actually limits decode: the memory, or the unpacking?
 * ===========================================================================================
 *
 *  run_model measured 1.30 tokens a second reading 1833.9 MB per token, which is an effective
 *  2.32 GB/s. The whole thesis of the project rests on decode being MEMORY BOUND, so that number has
 *  to be compared against something, and "it feels slow" is not a comparison.
 *
 *  There are exactly three candidates for the limit, and this separates them by measuring each one
 *  alone on the same machine, in the same process, with the same thread count:
 *
 *    1. MEMORY. Read a buffer larger than cache and do nothing but add it up. This is the ceiling
 *       no arrangement of software can beat.
 *    2. UNPACKING. Dequantize Q4_K and Q6_K blocks and throw the floats away. Pure nibble
 *       extraction, scale assembly and multiply, touching the same bytes per second as real decode.
 *    3. ARITHMETIC. The dot product itself, on floats already in cache.
 *
 *  Whichever of the three comes closest to 2.32 GB/s is the limit. If it is memory, distributing
 *  memory across many nodes is the correct architecture and the project is right. If it is unpacking,
 *  then a CPU node is compute bound on INT4 -- which is the one cost FPGA fabric removes for free,
 *  and the reason there are FPGAs in the design rather than just more microcontrollers.
 *
 *  Either answer is useful. Not measuring it and assuming the first is how a project spends a year
 *  building the wrong machine.
 * ===========================================================================================
 */

#include "gguf.h"

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

/* Working set, in MB. Overridable on the command line because the targets are three orders of
 * magnitude apart in memory: this desktop has 48 GB and a Luckfox has 33 MB TOTAL. A fixed 512 MB
 * buffer measures the desktop and refuses to run on the board the numbers are actually for. */
static unsigned BUF_MB = 512u;
#define QK_K     256

/* ---- 1. memory: read and sum, nothing else -------------------------------------------------- */

static double bw_memory(const uint8_t *buf, size_t bytes, int reps)
{
    const double t0 = now_s();
    uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t local = 0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+:local) schedule(static)
#endif
        for (int64_t i = 0; i < (int64_t)(bytes / 8); i++) {
            uint64_t w;
            memcpy(&w, buf + (size_t)i * 8, 8);
            local += w;
        }
        sink += local;
    }
    const double dt = now_s() - t0;
    /* The sink is printed so nothing here can be optimised away, which is the mistake that made an
     * earlier version of bench_stream report 381 MB/s for reads against 1144 for writes. */
    if (sink == 0x5555555555555555ull) printf(" ");
    return (double)bytes * reps / dt / 1073741824.0;
}

/* ---- 2. unpacking: dequantize and discard ---------------------------------------------------- */

static double bw_dequant(const uint8_t *buf, size_t bytes, uint32_t type, uint32_t blk_bytes, int reps)
{
    const size_t nblocks = bytes / blk_bytes;
    double sink = 0.0;
    const double t0 = now_s();
    for (int r = 0; r < reps; r++) {
        double local = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+:local) schedule(static)
#endif
        for (int64_t b = 0; b < (int64_t)nblocks; b++) {
            float out[QK_K];
            gguf_dequant(type, buf + (size_t)b * blk_bytes, QK_K, out);
            /* One element is consumed so the call cannot be elided. Summing all 256 would measure
             * the addition instead of the unpacking. */
            local += out[0];
        }
        sink += local;
    }
    const double dt = now_s() - t0;
    if (sink == 12345.6789) printf(" ");
    return (double)nblocks * blk_bytes * reps / dt / 1073741824.0;
}

/* ---- 2b. the FUSED path: unpack and multiply in one pass, integers, no intermediate array ----
 *
 * The measurement above is what this project's first implementation did, and it is not what the
 * format costs. Writing 256 floats to memory so a dot product can read them back once is a bad loop,
 * and charging it to 4-bit weights flatters the FPGA with someone else's slowness.
 *
 * gguf_dot_q does the same arithmetic reassociated: integer multiply-accumulate straight out of the
 * nibbles, with the block scales applied once per 32 weights. Measured at 4.26x on Q4_K, and that
 * moved the verdict below from "6x clear of memory" to under 4x. Verified exact against the float
 * path in tests/fast_path.c before being believed. */
static double bw_fused(const uint8_t *buf, size_t bytes, uint32_t type, uint32_t blk_bytes,
                       const int8_t *xq, const float *xs, int reps)
{
    const size_t nblocks = bytes / blk_bytes;
    double sink = 0.0;
    const double t0 = now_s();
    for (int r = 0; r < reps; r++) {
        double local = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+:local) schedule(static)
#endif
        for (int64_t b = 0; b < (int64_t)nblocks; b++)
            local += gguf_dot_q(type, buf + (size_t)b * blk_bytes, xq, xs, QK_K);
        sink += local;
    }
    const double dt = now_s() - t0;
    if (sink == 12345.6789) printf(" ");
    return (double)nblocks * blk_bytes * reps / dt / 1073741824.0;
}

/* ---- 3. arithmetic: float dot product, data already unpacked --------------------------------- */

static double gmacs_dot(int reps)
{
    enum { N = 2048 };
    static float a[N], b[N];
    for (int i = 0; i < N; i++) { a[i] = (float)(i % 13) * 0.01f; b[i] = (float)(i % 7) * 0.02f; }

    double sink = 0.0;
    const double t0 = now_s();
    const int64_t iters = 200000;
    for (int r = 0; r < reps; r++) {
        double local = 0.0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+:local) schedule(static)
#endif
        for (int64_t k = 0; k < iters; k++) {
            float s = 0.0f;
            for (int i = 0; i < N; i++) s += a[i] * b[i];
            local += s;
        }
        sink += local;
    }
    const double dt = now_s() - t0;
    if (sink == 1.0) printf(" ");
    return (double)iters * N * reps / dt / 1e9;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        const int mb = atoi(argv[1]);
        if (mb > 0) BUF_MB = (unsigned)mb;
    }
    int threads = 1;
#ifdef _OPENMP
#pragma omp parallel
    {
#pragma omp master
        threads = omp_get_num_threads();
    }
#endif

    const size_t bytes = (size_t)BUF_MB * 1024u * 1024u;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) { printf("cannot allocate %u MB\n", BUF_MB); return 1; }

    /* Filled with a pattern rather than zeroes: a page of zeroes can be served by the allocator
     * without ever touching memory, and Q4_K scales of zero make the dequantizer's inner multiply
     * trivially cheap. Both would flatter the result. */
    uint32_t s = 12345u;
    for (size_t i = 0; i < bytes; i++) { s = s * 1103515245u + 12345u; buf[i] = (uint8_t)(s >> 16); }

    printf("\n========================================================================\n");
    printf("  what limits decode on this host\n");
    printf("========================================================================\n");
    printf("  %u MB working set, %d threads\n\n", BUF_MB, threads);

    /* An activation for the fused kernel. Content is irrelevant to throughput; only the shape is. */
    int8_t *xq = (int8_t *)malloc(QK_K);
    float  *xs = (float *)malloc((QK_K / 32) * sizeof(float));
    if (!xq || !xs) { printf("out of memory\n"); return 1; }
    for (int i = 0; i < QK_K; i++) xq[i] = (int8_t)((i * 37) % 255 - 127);
    for (int i = 0; i < QK_K / 32; i++) xs[i] = 0.0123f;

    const double mem = bw_memory(buf, bytes, 4);
    const double q4  = bw_dequant(buf, bytes, GGML_Q4_K, 144, 4);
    const double q6  = bw_dequant(buf, bytes, GGML_Q6_K, 210, 4);
    const double f4  = bw_fused(buf, bytes, GGML_Q4_K, 144, xq, xs, 4);
    const double f6  = bw_fused(buf, bytes, GGML_Q6_K, 210, xq, xs, 4);
    const double gm  = gmacs_dot(2);

    printf("  %-34s %9.2f GB/s\n", "1. memory read, sum only", mem);
    printf("  %-34s %9.2f GB/s\n", "2. Q4_K dequantize to memory", q4);
    printf("  %-34s %9.2f GB/s\n", "2. Q6_K dequantize to memory", q6);
    printf("  %-34s %9.2f GB/s\n", "2b. Q4_K FUSED integer dot", f4);
    printf("  %-34s %9.2f GB/s\n", "2b. Q6_K FUSED integer dot", f6);
    printf("  %-34s %9.2f G MAC/s\n", "3. float dot, data in cache", gm);

    /* Decode reads a fixed mix, measured from the file: 69% Q4_K, 31% Q6_K by bytes. The ceiling
     * unpacking imposes is the harmonic mean over that mix, not the average -- time adds, rates do
     * not. */
    const double fq4 = 0.69, fq6 = 0.31;
    const double old_ceiling = 1.0 / (fq4 / q4 + fq6 / q6);
    const double unpack_ceiling = 1.0 / (fq4 / f4 + fq6 / f6);
    const double per_token_mb = 1833.9;

    printf("\n  the mix in the real file is 69%% Q4_K and 31%% Q6_K by bytes\n");
    printf("  %-34s %9.2f GB/s   (optimistic: never reads the f32 back)\n",
           "ceiling, dequantize-to-memory", old_ceiling);
    printf("  %-34s %9.2f GB/s\n", "ceiling, FUSED", unpack_ceiling);
    printf("  %-34s %9.2f GB/s\n", "memory ceiling", mem);

    printf("\n  predicted decode at the fused unpacking ceiling: %.2f tok/s\n",
           unpack_ceiling * 1024.0 / per_token_mb);
    printf("  predicted decode at the memory ceiling:          %.2f tok/s\n",
           mem * 1024.0 / per_token_mb);
    printf("  measured by run_model, float path:               1.28 tok/s\n");
    printf("  measured by run_model, fused path:               4.25 tok/s\n");

    printf("\n  VERDICT: ");
    if (unpack_ceiling < mem * 0.7)
        printf("STILL UNPACKING, by %.1fx.\n"
               "  The fused kernel took back most of the gap a careless loop had created, so what\n"
               "  remains is the real cost of the format on this instruction set. A processor node\n"
               "  is compute bound on 4-bit weights; FPGA fabric is not, because splitting a byte\n"
               "  into nibbles is wiring rather than arithmetic. The FPGA argument survives at the\n"
               "  smaller number, which is the number that belongs in the design.\n",
               mem / unpack_ceiling);
    else
        printf("MEMORY, once the unpacking is written properly. Decode is\n"
               "  bandwidth bound after all, distributing memory is the right architecture, and the\n"
               "  FPGA's value is capacity and power rather than unpacking throughput.\n");
    printf("\n");
    free(xq);
    free(xs);

    free(buf);
    return 0;
}

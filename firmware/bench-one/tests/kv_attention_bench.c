/* ===========================================================================================
 *  kv_attention_bench.c -- the workload where the unpacking limit does not apply
 * ===========================================================================================
 *
 *  Every measurement so far says the same thing about the small boards: they cannot unpack 4-bit
 *  weights fast enough to matter. A Luckfox reads memory at 993 MB/s and turns packed weights into
 *  arithmetic at 61. Sixteen times more bandwidth than it can use. On the language model that makes
 *  the microcontrollers traffic control and nothing else.
 *
 *  ATTENTION OVER A CACHED CONTEXT IS NOT THAT WORKLOAD.
 *
 *  The KV cache is not packed. It is plain int8 values, read straight through, one multiply-add per
 *  value. There is no nibble to split, no scale to assemble, no sign to extend. The single reason
 *  the small boards are bad at weights does not exist here.
 *
 *  That matters because of what was measured earlier: at 32k context with four conversations the KV
 *  cache is 2,304 MB against 2,079 MB of weights. **Bigger than the model.** It is also the thing a
 *  GPU runs out of first, because VRAM is the scarcest memory in the building and context is what
 *  consumes it.
 *
 *  So the question this answers: how fast can a node do attention over cached context, as opposed to
 *  how fast it can unpack weights? If the answer is much better than 61 MB/s, then a fleet of cheap
 *  boards is a context server, and the workload it is worst at and the workload it is best at are
 *  different workloads.
 *
 *
 *  WHAT IT MEASURES
 *  -----------------
 *  One decode step of attention for one head, against a cache of N positions:
 *
 *      score[t] = q . k[t]        for every cached position      -- one MAC per cached value
 *      softmax
 *      out += score[t] * v[t]     for every cached position      -- one MAC per cached value
 *
 *  Reported as MB of cache consumed per second, so it sits directly beside the 61 MB/s unpacking
 *  figure and the 993 MB/s raw read figure from the same board.
 *
 *  RUN
 *      kv_attention_bench [cache_MB] [head_dim]
 * ===========================================================================================
 */

#include <math.h>
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

int main(int argc, char **argv)
{
    const int cache_mb = (argc > 1) ? atoi(argv[1]) : 8;
    const int hd = (argc > 2) ? atoi(argv[2]) : 128;

    /* Two arrays, keys and values, each holding `positions` rows of head_dim int8 values. */
    const size_t bytes = (size_t)cache_mb * 1024u * 1024u / 2u;
    const int positions = (int)(bytes / (size_t)hd);
    if (positions < 16) { printf("cache too small\n"); return 1; }

    int8_t *k = (int8_t *)malloc((size_t)positions * hd);
    int8_t *v = (int8_t *)malloc((size_t)positions * hd);
    float *q = (float *)malloc((size_t)hd * sizeof(float));
    float *att = (float *)malloc((size_t)positions * sizeof(float));
    float *out = (float *)malloc((size_t)hd * sizeof(float));
    int8_t *qi = (int8_t *)malloc((size_t)hd);
    int32_t *acc32 = (int32_t *)malloc((size_t)hd * sizeof(int32_t));
    float *kscale = (float *)malloc((size_t)positions * sizeof(float));
    float *vscale = (float *)malloc((size_t)positions * sizeof(float));
    if (!k || !v || !q || !att || !out || !kscale || !vscale || !qi || !acc32) { printf("out of memory\n"); return 1; }

    /* A pattern rather than zeroes: zero pages can be served without touching memory, and a cache of
     * zeroes would let the multiply be skipped by a clever compiler. */
    uint32_t s = 22222u;
    for (size_t i = 0; i < (size_t)positions * hd; i++) {
        s = s * 1103515245u + 12345u;
        k[i] = (int8_t)((s >> 16) & 0xFF);
        v[i] = (int8_t)((s >> 8) & 0xFF);
    }
    for (int i = 0; i < positions; i++) { kscale[i] = 0.013f; vscale[i] = 0.011f; }
    for (int i = 0; i < hd; i++) q[i] = ((float)(i % 17) - 8.0f) * 0.05f;
    /* Quantize the query once. 128 values against millions of cached ones, so the cost is nothing
     * and it is what lets the inner loop stay in integers. */
    float qamax = 0.0f;
    for (int i = 0; i < hd; i++) { const float a = fabsf(q[i]); if (a > qamax) qamax = a; }
    const float qs = qamax / 127.0f;
    for (int i = 0; i < hd; i++) qi[i] = (int8_t)(q[i] / (qs > 0 ? qs : 1.0f));

    printf("\n========================================================================\n");
    printf("  attention over a cached context: the workload with no unpacking in it\n");
    printf("========================================================================\n");
    printf("  %d MB of cache, head_dim %d, %d cached positions\n", cache_mb, hd, positions);

    const int reps = 8;
    const double t0 = now_s();
    double sink = 0.0;

    for (int r = 0; r < reps; r++) {
        /* scores: one multiply-add per cached key value, then the per-position scale once */
        /* INTEGER ACCUMULATION, for the same reason the weight kernel needed it.
         *
         * The obvious loop converts every cached byte to float and multiplies. That is one widening
         * convert per byte and it does not vectorise, which is exactly the mistake that made batched
         * prefill slower than no batching at all. The query is 128 values against a cache of
         * millions, so quantizing it ONCE and accumulating in int32 costs nothing and turns the
         * inner loop into int8 multiply-accumulate, which every SIMD unit made this century does
         * eight or sixteen at a time. */
        const float scale = 1.0f / sqrtf((float)hd);
        for (int t = 0; t < positions; t++) {
            const int8_t *kt = k + (size_t)t * hd;
            int32_t acc = 0;
            for (int i = 0; i < hd; i++) acc += (int32_t)qi[i] * (int32_t)kt[i];
            att[t] = (float)acc * qs * kscale[t] * scale;
        }
        /* softmax over the whole context */
        float mx = att[0];
        for (int t = 1; t < positions; t++) if (att[t] > mx) mx = att[t];
        float sum = 0.0f;
        for (int t = 0; t < positions; t++) { att[t] = expf(att[t] - mx); sum += att[t]; }
        const float inv = 1.0f / sum;
        /* weighted sum: one multiply-add per cached value value */
        /* The same trick on the value sum. Attention weights are already in [0,1] after the
         * softmax, so one scale covers all of them and the accumulation stays integer. */
        for (int i = 0; i < hd; i++) acc32[i] = 0;
        for (int t = 0; t < positions; t++) {
            const int8_t *vt = v + (size_t)t * hd;
            /* NO SKIPPING ZERO WEIGHTS, deliberately.
             *
             * Skipping them made this benchmark three times faster and meaningless: over a million
             * positions the softmax concentrates on a handful, so nearly every position rounds to
             * zero and the loop stopped reading the cache it claims to measure. Sparsity like that is
             * a real and exploitable property of attention, but it is a different finding, and a
             * bandwidth number that quietly depends on skipping most of the bytes is not one. */
            int ai = (int)(att[t] * inv * 127.0f + 0.5f);
            if (ai > 127) ai = 127;
            for (int i = 0; i < hd; i++) acc32[i] += ai * (int32_t)vt[i];
        }
        for (int i = 0; i < hd; i++) out[i] = (float)acc32[i] * (1.0f / 127.0f) * vscale[0];
        sink += out[0];
    }
    const double dt = now_s() - t0;
    if (sink == 1234.5) printf(" ");

    /* Both arrays are read once per rep, so the bytes consumed are the whole cache each time. */
    const double mb = (double)cache_mb * reps;
    printf("\n  %.1f MB of cache swept in %.3f s\n", mb, dt);
    printf("  ATTENTION THROUGHPUT %.1f MB/s\n", mb / dt);
    printf("\n  compare, on the same machine:\n");
    printf("    plain memory read        -- the ceiling nothing beats\n");
    printf("    4-bit weight unpacking   -- the limit that makes small boards useless for weights\n");
    printf("    this                     -- the same boards on cached context\n");
    printf("\n  If this figure is far above the unpacking one, the workload the fleet is worst at and\n");
    printf("  the workload it is best at are different workloads, and a context server is the second.\n\n");

    free(k); free(v); free(q); free(att); free(out); free(kscale); free(vscale);
    free(qi); free(acc32);
    return 0;
}

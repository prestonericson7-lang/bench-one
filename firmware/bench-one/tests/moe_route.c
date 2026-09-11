/* ===========================================================================================
 *  moe_route.c -- how concentrated is expert use, and does a cache of the hot ones work
 * ===========================================================================================
 *
 *  THE QUESTION THIS ANSWERS
 *  --------------------------
 *  A dense 14B cannot run on this machine and cannot be streamed onto it either: measured, an
 *  uncached SSD gives 629 MB/s, and a dense model reads every weight every token, so 4.23 GB of
 *  non-resident weights is 6.75 seconds a token. That is not slow, it is dead.
 *
 *  A mixture-of-experts model is the one shape where the arithmetic changes. Qwen3-30B reads
 *  1.79 GB a token out of 17.5 GB of weights, because only 8 experts of 128 fire in each layer.
 *  If those 8 were a different 8 every time, nothing is gained -- the reads are just scattered
 *  instead of sequential, which is worse. If the same experts keep firing, a cache of the hot ones
 *  turns a storage-bound problem into a memory-bound one, and the machine can hold a 30B.
 *
 *  Nothing in the architecture says which. The router does, on real text, and only on real text.
 *
 *  WHAT IT DOES
 *  ------------
 *  Loads the model ONCE -- 17 GB, 36 seconds off a cold disk -- and runs several prompts through
 *  it on different subjects, resetting the counters between them. Then it reports:
 *
 *    * the hit rate a cache of the hottest N experts per layer would have achieved, per prompt
 *    * the same figure pooled across every prompt, which is the number that matters for a machine
 *      that will see arbitrary work
 *    * how much the hot set MOVES between prompts, because a cache sized for one subject and
 *      thrashed by the next is not a cache
 *
 *  That last one is the trap. One long generation on one topic will show high concentration
 *  whatever the router does, simply because the text stays in one mode. Measuring several subjects
 *  and reporting the overlap between their hot sets is what separates a real result from an
 *  artifact of the prompt.
 *
 *  RUN
 *      moe_route <model.gguf> [tokens-per-prompt]
 * ======================================================================================== */

#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

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

/* Deliberately unrelated subjects. Expert routing that concentrates within a subject but not
 * across subjects is a cache that works in a demo and thrashes in use. */
static const char *PROMPTS[] = {
    "Write a Python function that reverses a linked list in place.",
    "Explain why the sky appears blue at midday and red at sunset.",
    "SELECT customer_id, SUM(total) FROM orders GROUP BY",
    "The treaty was signed in 1648, ending a war that had lasted",
    "def quicksort(arr):\n    if len(arr) <= 1:",
    "Translate to French: the weather tomorrow will be cold and wet.",
};
#define NPROMPT (int)(sizeof(PROMPTS) / sizeof(PROMPTS[0]))

static const int CACHE_N[] = { 4, 8, 12, 16, 24, 32, 48, 64 };
#define NCACHE (int)(sizeof(CACHE_N) / sizeof(CACHE_N[0]))

/* Hit rate of a cache holding the hottest `n` experts of every layer, against `hits`. */
static double hit_rate(const uint32_t *hits, int NL, int NE, int n, uint32_t *scratch)
{
    uint64_t total = 0, served = 0;
    for (int l = 0; l < NL; l++) {
        memcpy(scratch, hits + (size_t)l * NE, (size_t)NE * sizeof(uint32_t));
        for (int i = 1; i < NE; i++) {          /* insertion sort, descending */
            const uint32_t v = scratch[i];
            int j = i - 1;
            while (j >= 0 && scratch[j] < v) { scratch[j + 1] = scratch[j]; j--; }
            scratch[j + 1] = v;
        }
        for (int i = 0; i < NE; i++) {
            total += scratch[i];
            if (i < n) served += scratch[i];
        }
    }
    return total ? 100.0 * (double)served / (double)total : 0.0;
}

/* Which experts are in the hot set of every layer, as a bitmap of NL*NE bits packed one per byte. */
static void hot_set(const uint32_t *hits, int NL, int NE, int n, uint8_t *out)
{
    memset(out, 0, (size_t)NL * NE);
    for (int l = 0; l < NL; l++) {
        const uint32_t *row = hits + (size_t)l * NE;
        for (int k = 0; k < n; k++) {
            int best = -1;
            uint32_t bv = 0;
            for (int e = 0; e < NE; e++)
                if (!out[(size_t)l * NE + e] && row[e] >= bv) { bv = row[e]; best = e; }
            if (best < 0) break;
            out[(size_t)l * NE + best] = 1;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: moe_route <model.gguf> [tokens-per-prompt]\n"); return 1; }
    const int NGEN = (argc > 2) ? atoi(argv[2]) : 40;

    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("  %s\n", g.err); return 1; }

    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { printf("  tokenizer: %s\n", g.err); return 1; }

    model_t m;
    const double t0 = now_s();
    if (model_load(&m, &g, 512)) { printf("  %s\n", g.err); return 1; }
    const double t_load = now_s() - t0;
    model_set_fast(&m, 1);

    printf("\n========================================================================\n");
    printf("  mixture-of-experts routing, measured on real text\n");
    printf("========================================================================\n");
    printf("  %.2f GB of weights loaded in %.1f s\n", m.weight_bytes / 1073741824.0, t_load);
    if (m.n_expert <= 0) { printf("  this model has no experts, nothing to measure\n"); return 1; }
    printf("  %d layers, %d experts each, %d fire per token\n", m.n_layer, m.n_expert, m.n_expert_used);
    printf("  %d prompts on unrelated subjects, %d tokens each\n\n", NPROMPT, NGEN);

    const int NL = m.n_layer, NE = m.n_expert;
    const size_t TBL = (size_t)NL * NE;
    uint32_t *pooled = (uint32_t *)calloc(TBL, sizeof(uint32_t));
    uint32_t *snap   = (uint32_t *)calloc(TBL, sizeof(uint32_t));
    uint32_t *scr    = (uint32_t *)calloc((size_t)NE, sizeof(uint32_t));
    uint8_t  *hotA   = (uint8_t *)calloc(TBL, 1);
    uint8_t  *hotB   = (uint8_t *)calloc(TBL, 1);
    double   *rates  = (double *)calloc((size_t)NPROMPT * NCACHE, sizeof(double));
    if (!pooled || !snap || !scr || !hotA || !hotB || !rates) { printf("  out of memory\n"); return 1; }

    const double expert_mb =
        ((double)m.expert_ff * m.L[0].e_gate.row_bytes +
         (double)m.expert_ff * m.L[0].e_up.row_bytes +
         (double)m.dim       * m.L[0].e_down.row_bytes) / 1048576.0;

    uint32_t *hits = (uint32_t *)model_expert_hits(&m);
    int32_t tok[1024];

    for (int pi = 0; pi < NPROMPT; pi++) {
        memset(hits, 0, TBL * sizeof(uint32_t));

        int n = tokenizer_encode(&tk, PROMPTS[pi], tok, 1024);
        if (n <= 0) { printf("  prompt %d did not tokenize\n", pi); continue; }
        model_prefill(&m, tok, n, 0, 0);
        /* prefill leaves the activation, not the logits. Without this the first pick is whatever
         * was in the logits buffer from the previous prompt. */
        if (!model_head(&m)) { printf("  no output head\n"); return 1; }

        const double p0 = now_s();
        int32_t next = model_argmax(&m);
        for (int i = 0; i < NGEN; i++) {
            model_forward(&m, next, n + i);
            next = model_argmax(&m);
        }
        const double dt = now_s() - p0;

        /* Snapshot before pooling, so a per-prompt hot set can be compared with the next one. */
        memcpy(snap, hits, TBL * sizeof(uint32_t));
        for (size_t i = 0; i < TBL; i++) pooled[i] += hits[i];

        printf("  prompt %d: %d prompt tokens, %d generated, %.2f tok/s\n",
               pi + 1, n, NGEN, NGEN / dt);
        printf("    cache of the hottest N per layer: ");
        for (int c = 0; c < NCACHE; c++) {
            rates[pi * NCACHE + c] = hit_rate(snap, NL, NE, CACHE_N[c], scr);
            printf("N=%d %.0f%%  ", CACHE_N[c], rates[pi * NCACHE + c]);
        }
        printf("\n");

        /* How much of prompt 1's hot set is still hot here. A cache is only worth building if this
         * stays high, because the machine cannot re-choose its resident experts per request. */
        if (pi == 0) {
            hot_set(snap, NL, NE, 16, hotA);
        } else {
            hot_set(snap, NL, NE, 16, hotB);
            int both = 0, any = 0;
            for (size_t i = 0; i < TBL; i++) { if (hotA[i] && hotB[i]) both++; if (hotA[i]) any++; }
            printf("    of prompt 1's hot 16 per layer, %d of %d are hot here too (%.0f%%)\n",
                   both, any, any ? 100.0 * both / any : 0.0);
        }
    }

    printf("\n  ----------------------------------------------------------------------\n");
    printf("  POOLED ACROSS EVERY PROMPT -- the figure a real machine would see\n\n");
    printf("    N per layer   experts   cache size    hit rate   uniform would be\n");
    for (int c = 0; c < NCACHE; c++) {
        const double r = hit_rate(pooled, NL, NE, CACHE_N[c], scr);
        printf("      %3d          %5d     %6.2f GB     %6.2f%%      %6.2f%%\n",
               CACHE_N[c], CACHE_N[c] * NL, CACHE_N[c] * NL * expert_mb / 1024.0,
               r, 100.0 * CACHE_N[c] / NE);
    }

    /* What that does to the machine, in the only unit that decides anything.
     *
     * 3.10 GB total, less what has to be resident whatever happens: attention, the routers, the
     * embedding and the output head. Everything left caches experts. Misses come off the SSD at
     * the measured large-block random rate, because an expert is 2.92 MB contiguous but which one
     * is wanted is not known until the router has run. */
    /* Measured from the tensors themselves, not from expert_mb times a count.
     *
     * The first version subtracted an estimate of the expert bytes from weight_bytes and got a
     * NEGATIVE resident figure, which is how it announced that the estimate was wrong. Adding up
     * what was actually allocated cannot be wrong in that way. */
    double expert_bytes = 0.0;
    for (int l = 0; l < NL; l++) {
        expert_bytes += (double)m.L[l].e_gate.rows * m.L[l].e_gate.row_bytes;
        expert_bytes += (double)m.L[l].e_up.rows   * m.L[l].e_up.row_bytes;
        expert_bytes += (double)m.L[l].e_down.rows * m.L[l].e_down.row_bytes;
    }
    const double resident_gb = (m.weight_bytes - expert_bytes) / 1073741824.0;
    const double cache_gb = 3.10 - resident_gb;
    const double per_token_mb = expert_mb * m.n_expert_used * NL;
    const double ssd_random = 661.0;      /* MB/s, measured by disk_stream, 4 MB random uncached */

    printf("\n  ON THIS MACHINE (3.10 GB of memory, %.0f MB/s of random SSD reads)\n", ssd_random);
    printf("    experts, all of them            %6.2f GB\n", expert_bytes / 1073741824.0);
    printf("    must be resident regardless   %6.2f GB   attention, routers, embedding, head\n",
           resident_gb);
    printf("    left for an expert cache      %6.2f GB\n", cache_gb);
    if (cache_gb > 0.0) {
        const int fits = (int)(cache_gb * 1024.0 / expert_mb / NL);
        const double r = hit_rate(pooled, NL, NE, fits > 0 ? fits : 1, scr);
        const double miss_mb = per_token_mb * (1.0 - r / 100.0);
        const double t_ssd = miss_mb / ssd_random;
        printf("    which holds                    %5d experts a layer of %d\n", fits, NE);
        printf("    measured hit rate at that size %6.2f%%\n", r);
        printf("    expert bytes per token         %6.0f MB, of which %.0f MB misses\n",
               per_token_mb, miss_mb);
        printf("    storage time per token         %6.2f s   ->  %.2f tokens/s from storage alone\n",
               t_ssd, t_ssd > 0 ? 1.0 / t_ssd : 0.0);
        printf("\n    Add boards and the cache grows. At each size:\n");
        for (int c = 0; c < NCACHE; c++) {
            const double rr = hit_rate(pooled, NL, NE, CACHE_N[c], scr);
            const double mm = per_token_mb * (1.0 - rr / 100.0);
            const double tt = mm / ssd_random;
            printf("      %2d per layer  %6.2f GB of cache  %6.2f%% hit  %6.2f s/token  %5.2f tok/s\n",
                   CACHE_N[c], CACHE_N[c] * NL * expert_mb / 1024.0 + resident_gb, rr, tt,
                   tt > 0 ? 1.0 / tt : 0.0);
        }
    } else {
        printf("    the resident part alone does not fit, by %.2f GB\n", -cache_gb);
    }
    printf("\n");

    free(pooled); free(snap); free(scr); free(hotA); free(hotB); free(rates);
    model_free(&m);
    tokenizer_free(&tk);
    gguf_close(&g);
    return 0;
}

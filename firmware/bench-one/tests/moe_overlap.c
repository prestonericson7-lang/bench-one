/* ===========================================================================================
 *  moe_overlap.c -- how many experts a pass over k+1 consecutive positions must read (2026-10-16)
 * ===========================================================================================
 *
 *  On the Teensy a mixture-of-experts token is card-bound: 1.92 GB a token, 1.08 GB of it the eight experts each
 *  layer picks. Verifying k guessed tokens in one pass (speculative decoding) reads the attention, routers and head
 *  once for k+1 positions -- but every expert ANY of them picked. Whether that pays depends on how much consecutive
 *  positions share experts, a property of the model's router, not of any computer. This measures it.
 *
 *  For each prompt it feeds the prompt and greedy answer one token at a time through model_q.c's fused path, takes
 *  each position's picks from the expert-hit counters (the difference across that position's forward), and reports
 *  per k = 0..7 the mean number of distinct experts a layer must read for k+1 consecutive positions of the ANSWER,
 *  and what a pass then costs in card bytes relative to one position.
 *
 *  BUILD (tests/, vendored gcc on PATH):
 *    gcc -O3 -fopenmp -std=c11 -Wall -Wextra -I../shared -o moe_overlap.exe moe_overlap.c \
 *        ../shared/model_q.c ../shared/gguf.c ../shared/gguf_bits.c ../shared/gguf_dot.c ../shared/tokenizer.c -lm
 *  RUN
 *    moe_overlap.exe <moe.gguf> <prompts file, one escaped prompt per line> [n_gen]
 * ======================================================================================== */
#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KMAX 8

static void unesc(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        } else if (*p != '\r' && *p != '\n') *o++ = *p;
    }
    *o = 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: moe_overlap <moe.gguf> <prompts.txt> [n_gen]\n"); return 1; }
    const int n_gen = argc > 3 ? atoi(argv[3]) : 32;
    gguf_t g;
    if (gguf_open(&g, argv[1])) { fprintf(stderr, "open: %s\n", g.err); return 1; }
    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { fprintf(stderr, "tokenizer: %s\n", g.err); return 1; }
    model_t m;
    if (model_load(&m, &g, 1024)) { fprintf(stderr, "load: %s\n", g.err); return 1; }
    model_set_fast(&m, 1);
    if (m.n_expert <= 0) { fprintf(stderr, "not a mixture of experts\n"); return 1; }
    const int L = m.n_layer, E = m.n_expert, K = m.n_expert_used;
    const size_t HN = (size_t)L * E;
    uint32_t *before = (uint32_t *)malloc(HN * sizeof(uint32_t));
    /* picks[pos][layer][expert] as a bitmap row per (pos, layer) */
    unsigned char *pk = (unsigned char *)calloc((size_t)1024 * HN, 1);
    double sum_u[KMAX] = { 0 };
    long cnt_u[KMAX] = { 0 };

    FILE *pf = fopen(argv[2], "rb");
    if (!pf) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    static char line[65536];
    int np = 0;
    while (fgets(line, sizeof line, pf)) {
        unesc(line);
        if (!line[0]) continue;
        int32_t ids[1024];
        const int n = tokenizer_encode(&tk, line, ids, 512);
        if (n <= 0) continue;
        /* no reset: positions restart at 0, and attention at p reads only rows 0..p, rewritten as it goes */
        int32_t next = 0;
        int pos = 0, ans0 = n - 1, last = -1;
        for (int step = 0; step < n + n_gen && step < 1000; step++) {
            const int32_t fed = step < n ? ids[step] : next;
            memcpy(before, model_expert_hits(&m), HN * sizeof(uint32_t));
            const float *lg = model_forward(&m, fed, step);
            if (!lg) { fprintf(stderr, "forward failed\n"); return 1; }
            const uint32_t *after = model_expert_hits(&m);
            for (size_t i = 0; i < HN; i++) pk[(size_t)step * HN + i] = (unsigned char)(after[i] != before[i]);
            int best = 0;
            for (int i = 1; i < m.v_hi - m.v_lo; i++) if (lg[i] > lg[best]) best = i;
            next = best;
            pos = step;
            if (step >= n - 1 && best == tk.eos) { last = step; break; }
        }
        if (last < 0) last = pos;
        /* windows of k+1 consecutive positions inside the answer (the positions a lookup pass would verify) */
        for (int k = 0; k < KMAX; k++)
            for (int t = ans0; t + k <= last; t++)
                for (int l = 0; l < L; l++) {
                    int u = 0;
                    for (int e = 0; e < E; e++) {
                        int any = 0;
                        for (int j = 0; j <= k && !any; j++) any = pk[(size_t)(t + j) * HN + (size_t)l * E + e];
                        u += any;
                    }
                    sum_u[k] += u;
                    cnt_u[k]++;
                }
        np++;
        fprintf(stderr, "prompt %d: %d prompt tokens, answer positions %d..%d\n", np, n, ans0, last);
    }
    fclose(pf);
    /* card bytes of a pass, relative to one position: the parts read once (attention, routers, head: 0.84 of 1.92 GB
     * for Qwen3-Coder-30B-A3B) plus the experts, in proportion to how many distinct ones the pass needs */
    printf("prompts %d, %d layers, %d experts, %d picked per position\n", np, L, E, K);
    printf("k+1 positions | distinct experts a layer (mean) | of k+1 x %d | pass bytes / one position's (0.84 + 1.08 x u/8, of 1.92)\n", K);
    for (int k = 0; k < KMAX; k++) {
        if (!cnt_u[k]) continue;
        const double u = sum_u[k] / cnt_u[k];
        printf("%d | %.2f | %.0f%% | %.2f\n", k + 1, u, 100.0 * u / ((k + 1) * K), (0.84 + 1.08 * u / K) / 1.92);
    }
    return 0;
}

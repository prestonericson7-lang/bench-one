/* ===========================================================================================
 *  run_model.c -- real weights, real layers, real tokens
 * ===========================================================================================
 *
 *  Everything before this measured a piece of the machine. This runs the workload the machine exists
 *  for: a 3B parameter model out of the file on this disk, tokenized with the vocabulary from that
 *  same file, through 36 real transformer layers, producing real text.
 *
 *  WHAT IT REPORTS AND WHY EACH NUMBER MATTERS
 *  --------------------------------------------
 *    load time          how long 1.8 GB takes off this disk. Sets cold start on every target.
 *    prefill tok/s      the prompt, which is compute bound. Matrix-matrix work, GPUs win here.
 *    decode tok/s       generation, which is MEMORY BANDWIDTH BOUND. This is the one that matters.
 *    bytes per token    every weight read once. Decode speed is this divided by bandwidth, and
 *                       nothing else, which is the entire thesis of the project in one ratio.
 *    effective GB/s     bytes per token times decode rate. Compare against the machine's measured
 *                       memory bandwidth: if they match, decode is bandwidth bound and distributing
 *                       memory is the right answer. If effective is far below, something else is
 *                       the limit and the architecture needs rethinking.
 *
 *  That last comparison is the experiment. It is the difference between a thesis and a hope.
 *
 *  RUN
 *      run_model <model.gguf> "prompt" [n_tokens]
 * ===========================================================================================
 */

#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  static double now_s(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
#endif

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("run_model <model.gguf> \"prompt\" [n_tokens]\n");
#ifdef ZACCEL_OFFLOAD
        printf("   --zaccel HOST[:PORT] [--share S]  the Zynq's matrix engine takes a share of the rows (default: measured)\n");
#endif
        return 1;
    }
    const char *path = argv[1];
    const char *prompt = argv[2];
    const int n_gen = (argc > 3) ? atoi(argv[3]) : 24;
    /* --fast selects the fused integer path. Both are built so the same binary can produce the
     * reference text and the fast text, which is the only check that matters on an approximation. */
    int want_fast = 0;
    int want_kv_f32 = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) want_fast = 1;
        /* Read the KV cache as float instead of int8, to judge what int8 costs. */
        if (!strcmp(argv[i], "--kv-f32")) want_kv_f32 = 1;
    }

    gguf_t g;
    if (gguf_open(&g, path)) { printf("open: %s\n", g.err); return 1; }

    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { printf("tokenizer: %s\n", g.err); return 1; }

    printf("\n========================================================================\n");
    printf("  a real model, running\n");
    printf("========================================================================\n");
    printf("  %s %lld layers, dim %lld, %llu token vocabulary\n",
           gguf_str(&g, "general.architecture", "?"),
           (long long)gguf_arch_int(&g, "block_count", 0),
           (long long)gguf_arch_int(&g, "embedding_length", 0),
           (unsigned long long)tk.n_vocab);

    /* The tokenizer is checked before anything expensive happens. A wrong tokenizer produces fluent
     * nonsense at the right speed, which is indistinguishable from success unless it is tested. */
    static const char *rt[] = {
        "The capital of France is",
        "def fib(n):\n    if n < 2:\n        return n\n",
        "  double  spaces\tand\ttabs  ",
        "x = 3.14159; y = x*2 // comment",
        "unicode: cafe\xc3\xa9 \xe2\x86\x92 done"
    };
    int rt_fail = 0;
    for (size_t i = 0; i < sizeof(rt) / sizeof(rt[0]); i++)
        if (tokenizer_roundtrip(&tk, rt[i]) != 0) { rt_fail++; printf("  ROUNDTRIP FAILED: %s\n", rt[i]); }
    printf("  tokenizer roundtrip: %d of %d exact\n",
           (int)(sizeof(rt) / sizeof(rt[0])) - rt_fail, (int)(sizeof(rt) / sizeof(rt[0])));

    int32_t ids[4096];
    const int n_prompt = tokenizer_encode(&tk, prompt, ids, 4096);
    if (n_prompt <= 0) { printf("  prompt did not encode\n"); return 1; }
    printf("  prompt: %d tokens ->", n_prompt);
    for (int i = 0; i < n_prompt && i < 16; i++) printf(" %d", ids[i]);
    printf("%s\n", n_prompt > 16 ? " ..." : "");

    model_t m;
    const double t_load0 = now_s();
    if (model_load(&m, &g, n_prompt + n_gen + 8)) { printf("load: %s\n", g.err); return 1; }
    const double t_load = now_s() - t_load0;
    model_set_fast(&m, want_fast);
#ifdef ZACCEL_OFFLOAD
    {   /* --zaccel HOST [--share S]: the Zynq's matrix engine takes a share of every dense matrix */
        const char *zh = NULL; double zshare = -1.0;
        for (int a = 1; a < argc; a++) {
            if (!strcmp(argv[a], "--zaccel") && a + 1 < argc) zh = argv[++a];
            else if (!strcmp(argv[a], "--share") && a + 1 < argc) zshare = atof(argv[++a]);
        }
        if (zh) {
            char zmsg[256];
            long long nw = model_zaccel_attach(&m, zh, zshare, zmsg, sizeof zmsg);
            printf("  zaccel: %s\n", zmsg);
            if (nw < 0) return 1;
        }
    }
#endif
    if (want_kv_f32 && model_set_kv_f32(&m, 1)) { printf("  cannot allocate a float KV cache\n"); return 1; }

    printf("\n  loaded %.2f GB of quantized weights in %.1f s (%.0f MB/s off this disk)\n",
           m.weight_bytes / 1073741824.0, t_load, m.weight_bytes / 1048576.0 / t_load);
    printf("  output head is %s\n", m.tied_output
           ? "TIED to the embedding table -- the same 243 MB is read in full every token"
           : "its own tensor");
    printf("  read per token: %.1f MB\n", m.bytes_per_token / 1048576.0);
    printf("  KV cache for %d positions: %.1f MB\n", m.max_seq,
           2.0 * m.n_layer * m.max_seq * m.n_kv * m.head_dim * sizeof(float) / 1048576.0);

    /* ---- prefill: every prompt token, which is the compute-bound half ---------------------- */
    printf("\n  prefill");
    fflush(stdout);
    const double t_pre0 = now_s();
    /* THE WHOLE PROMPT AT ONCE. Every position needs the same weights, so a row is unpacked once and
     * used n_prompt times. One token at a time reads the entire 1.8 GB model once per prompt token,
     * which for a 500-token prompt is 900 GB and minutes of waiting for the first word. */
    int pos = n_prompt;
    model_prefill(&m, ids, n_prompt, 0, 0);
    if (!model_head(&m)) { printf("\n  no output head\n"); return 1; }
    const double t_pre = now_s() - t_pre0;
    const double ttft = t_load + t_pre;
    printf(" %d tokens in %.2f s = %.2f tok/s\n", n_prompt, t_pre, n_prompt / t_pre);

    /* ---- decode: the memory-bound half, which is the thesis -------------------------------- */
    printf("\n  ----------------------------------------------------------------------\n  %s", prompt);
    fflush(stdout);

    char piece[64];
    double t_dec = 0.0;
    int produced = 0;
    int32_t tok = model_argmax(&m);

    for (int i = 0; i < n_gen; i++) {
        const int n = tokenizer_decode(&tk, tok, piece, (int)sizeof(piece) - 1);
        piece[n] = 0;
        printf("%s", piece);
        fflush(stdout);
        produced++;
        if (tok == tk.eos) break;

        const double t0 = now_s();
        if (!model_forward(&m, tok, pos)) { printf("\n  forward failed\n"); return 1; }
        t_dec += now_s() - t0;
        pos++;
        tok = model_argmax(&m);
    }
    printf("\n  ----------------------------------------------------------------------\n");

    const double per_tok = t_dec / (produced > 1 ? (produced - 1) : 1);
    const double tok_s = per_tok > 0 ? 1.0 / per_tok : 0.0;
    const double eff_gbs = m.bytes_per_token * tok_s / 1073741824.0;

    printf("\n  ENGINEERING NUMBERS\n");
    printf("  %-26s %10.2f s\n", "model load", t_load);
    printf("  %-26s %10.2f s\n", "time to first token", ttft);
    printf("  %-26s %10.2f tok/s\n", "prefill (compute bound)", n_prompt / t_pre);
    printf("  %-26s %10.2f tok/s\n", "decode (memory bound)", tok_s);
    printf("  %-26s %10.1f ms\n", "per decoded token", per_tok * 1000.0);
    printf("  %-26s %10.1f MB\n", "weights read per token", m.bytes_per_token / 1048576.0);
    printf("  %-26s %10.2f GB/s\n", "EFFECTIVE BANDWIDTH", eff_gbs);
    printf("\n  Decode reads every weight exactly once per token, so tokens per second is\n");
    printf("  bandwidth divided by %.1f MB and almost nothing else. That ratio is why this\n",
           m.bytes_per_token / 1048576.0);
    printf("  project distributes MEMORY rather than adding compute: at batch size one there\n");
    printf("  is no arithmetic to speak of, only reading.\n");
    printf("\n  Compare %.2f GB/s against this host's measured memory bandwidth. If they are\n", eff_gbs);
    printf("  close, decode is bandwidth bound and the thesis holds. If effective is far\n");
    printf("  below, the INT4 unpacking is the limit instead -- which is exactly the cost the\n");
    printf("  FPGA fabric removes, and it would be measured here rather than assumed.\n\n");

    /* -----------------------------------------------------------------------------------------
     *  Mixture-of-experts routing, measured rather than assumed.
     *
     *  A 30B model reads 1.79 GB a token, of which the experts are most of it. This machine has
     *  3.10 GB, so whether it can hold the model comes down to one question: how concentrated is
     *  expert use? If a handful of experts take most of the traffic, a cache of them turns a
     *  storage-bound problem into a memory-bound one. If use is flat, no cache helps and the
     *  answer is more boards.
     *
     *  Nothing in the architecture answers that. The router does, on real text.
     * -------------------------------------------------------------------------------------- */
    /* Where the time inside a token actually went. Printed always, because the one thing that
     * reliably wastes a day is optimizing the loop that was never the problem. */
    {
        const char *const *pn = model_prof_names();
        const double *ps = model_prof_seconds();
        double tot = 0.0;
        for (int i = 0; i < MODEL_PROF_N; i++) tot += ps[i];
        if (tot > 0.0) {
            printf("\n  WHERE THE TIME WENT, across every token of this run\n");
            for (int i = 0; i < MODEL_PROF_N; i++)
                printf("    %-32s %7.2f s  %5.1f%%\n", pn[i], ps[i], 100.0 * ps[i] / tot);
            printf("    %-32s %7.2f s\n", "accounted for", tot);
        }
    }

    const uint32_t *hits = model_expert_hits(&m);
    if (hits && m.n_expert > 0) {
        const int NE = m.n_expert, NL = m.n_layer;
        uint64_t total = 0;
        for (int i = 0; i < NL * NE; i++) total += hits[i];

        /* Sorted per layer, because an expert is only interchangeable with others in its own
         * layer -- caching "the top 500 experts" means the top few per layer, not globally. */
        printf("\n  MIXTURE-OF-EXPERTS ROUTING, over %llu expert selections\n",
               (unsigned long long)total);
        printf("  %d experts a layer, %d fire per token, %d layers resident\n",
               NE, m.n_expert_used, NL);

        uint32_t *sorted = (uint32_t *)malloc((size_t)NL * NE * sizeof(uint32_t));
        if (sorted) {
            for (int l = 0; l < NL; l++) {
                uint32_t *row = sorted + (size_t)l * NE;
                memcpy(row, hits + (size_t)l * NE, (size_t)NE * sizeof(uint32_t));
                /* Insertion sort, descending. NE is 128 and this runs once. */
                for (int i = 1; i < NE; i++) {
                    const uint32_t v = row[i];
                    int j = i - 1;
                    while (j >= 0 && row[j] < v) { row[j + 1] = row[j]; j--; }
                    row[j + 1] = v;
                }
            }
            printf("\n  if the cache holds the hottest N experts of every layer:\n");
            printf("    N   experts cached   cache size   hit rate\n");
            const int NS[] = { 4, 8, 16, 24, 32, 48, 64, 96, 128 };
            const double expert_mb =
                ((double)m.expert_ff * m.L[0].e_gate.row_bytes +
                 (double)m.expert_ff * m.L[0].e_up.row_bytes +
                 (double)m.dim       * m.L[0].e_down.row_bytes) / 1048576.0;
            for (int k = 0; k < (int)(sizeof(NS) / sizeof(NS[0])); k++) {
                const int n = NS[k];
                if (n > NE) break;
                uint64_t served = 0;
                for (int l = 0; l < NL; l++) {
                    const uint32_t *row = sorted + (size_t)l * NE;
                    for (int i = 0; i < n; i++) served += row[i];
                }
                printf("  %3d   %6d          %7.2f GB   %6.2f%%\n",
                       n, n * NL, n * NL * expert_mb / 1024.0,
                       total ? 100.0 * (double)served / (double)total : 0.0);
            }
            printf("\n  one expert is %.2f MB, all %d of a layer %.1f MB, the whole set %.2f GB\n",
                   expert_mb, NE, expert_mb * NE, expert_mb * NE * NL / 1024.0);
            free(sorted);
        }

        /* A flat distribution is the bad case and it has to be visible, not inferred from a table.
         * With 8 of 128 firing uniformly, the hottest 8 would serve 6.25%. */
        printf("\n  For comparison, if routing were uniform the hottest N of %d would serve\n", NE);
        printf("  exactly N/%d of the traffic. Anything above that line is concentration a\n", NE);
        printf("  cache can exploit; anything at it means the cache buys nothing.\n");
    }

    model_free(&m);
    tokenizer_free(&tk);
    gguf_close(&g);
    return 0;
}

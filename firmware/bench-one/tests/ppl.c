/* ===========================================================================================
 *  ppl.c -- a quality yardstick, because "the text still reads fine" is not one
 * ===========================================================================================
 *
 *  Every check in this project so far has been an EXACTNESS check: run it two ways, diff the output,
 *  demand they match. That works perfectly for changes that should not alter arithmetic -- splitting a
 *  model across four nodes, reordering a loop, moving the embedding lookup to disk -- and it caught
 *  real bugs.
 *
 *  It stops working the moment a change is an approximation. Storing the KV cache at int8 instead of
 *  float changed the output on three prompts out of five. Both versions were fluent and both were
 *  plausible; greedy decoding flips whenever two logits are close, so divergence proves only that
 *  something changed, not that anything got worse.
 *
 *  Perplexity answers the question that matters: given a real passage, how surprised is the model by
 *  the word that actually came next? It is one number, it is comparable between runs, and lower is
 *  better. An approximation that leaves it alone is free; one that raises it is buying memory with
 *  quality, and then the trade can be judged instead of hoped about.
 *
 *
 *  WHAT IT COMPUTES
 *  -----------------
 *  For each position, the log-probability the model assigned to the token that genuinely followed:
 *
 *      ppl = exp( -(1/N) * sum log P(actual next token) )
 *
 *  A perfect model scores 1. A model choosing uniformly at random from 151,936 tokens scores 151,936.
 *  Real models on ordinary English land in single or low double figures.
 *
 *  The log-sum-exp is computed by subtracting the maximum logit first. Exponentiating raw logits
 *  overflows to infinity and returns a perplexity of nan, which looks like a broken model rather than
 *  a broken metric.
 *
 *
 *  ONE TOKEN AT A TIME, ON PURPOSE
 *  --------------------------------
 *  Batched prefill is far quicker but only produces logits for the last position of a batch, and this
 *  needs them at every position. Slower and complete beats faster and blind.
 *
 *  RUN
 *      ppl <model.gguf> [--fast] [--kv-f32]
 * ===========================================================================================
 */

#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ordinary prose and ordinary code, because this machine is aimed at a coding model and a metric taken
 * on only one of those would miss a regression in the other. Fixed text, so runs are comparable. */
static const char *PASSAGE =
    "The quick brown fox jumps over the lazy dog. Machine learning models are trained on large "
    "datasets to recognise patterns in text. A transformer processes every token in parallel during "
    "training, but generates them one at a time when producing output. This asymmetry is why "
    "inference is bound by memory bandwidth while training is bound by arithmetic.\n"
    "\n"
    "def binary_search(arr, target):\n"
    "    low, high = 0, len(arr) - 1\n"
    "    while low <= high:\n"
    "        mid = (low + high) // 2\n"
    "        if arr[mid] == target:\n"
    "            return mid\n"
    "        elif arr[mid] < target:\n"
    "            low = mid + 1\n"
    "        else:\n"
    "            high = mid - 1\n"
    "    return -1\n"
    "\n"
    "The function above runs in logarithmic time because each comparison halves the range that remains "
    "to be searched. It requires the input to be sorted, which is the cost paid up front in exchange "
    "for every later lookup being cheap.";

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("ppl <model.gguf> [--fast] [--kv-f32] [--kv-int4] [--text file] [--limit n]\n");
#ifdef ZACCEL_OFFLOAD
        printf("   --zaccel auto|HOST[:PORT] [--share S]  the Zynq's matrix engine takes a share of the rows (default: measured)\n");
#endif
        return 1;
    }

    int want_fast = 0, want_kv_f32 = 0, want_kv_int4 = 0, limit = 0;
    const char *textfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) want_fast = 1;
        if (!strcmp(argv[i], "--kv-f32")) want_kv_f32 = 1;
        if (!strcmp(argv[i], "--kv-int4")) want_kv_int4 = 1;
        if (!strcmp(argv[i], "--text") && i + 1 < argc) textfile = argv[++i];
        if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
    }

    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("open: %s\n", g.err); return 1; }
    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { printf("tokenizer: %s\n", g.err); return 1; }

    /* Scoring arbitrary text, not just the built-in passage.
     *
     * One 204-token passage of English prose is enough to catch a broken kernel and nowhere near
     * enough to decide whether one model is better than another. A 30B that loses to a 3B on prose
     * may well win on code or at length, and the only way to find out is to point this at the
     * text that matters. */
    char *body = NULL;
    const char *text = PASSAGE;
    if (textfile) {
        FILE *tf = fopen(textfile, "rb");
        if (!tf) { printf("cannot open %s\n", textfile); return 1; }
        fseek(tf, 0, SEEK_END);
        const long sz = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        body = (char *)malloc((size_t)sz + 1);
        if (!body || fread(body, 1, (size_t)sz, tf) != (size_t)sz) {
            printf("cannot read %s\n", textfile);
            return 1;
        }
        body[sz] = 0;
        fclose(tf);
        text = body;
    }

    static int32_t ids[65536];
    int n = tokenizer_encode(&tk, text, ids, 65536);
    if (n < 2) { printf("passage did not encode\n"); return 1; }
    if (limit > 1 && n > limit) n = limit;

    model_t m;
    if (model_load(&m, &g, n + 8)) { printf("load: %s\n", g.err); return 1; }
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
    if (want_kv_f32 && model_set_kv_f32(&m, 1)) { printf("cannot allocate a float KV cache\n"); return 1; }
    /* This line was missing once, and the 4-bit run silently reported the 8-bit result instead. A flag
     * that is parsed but never applied is worse than one that is absent: the test appears to pass. The
     * compiler did say so -- "variable set but not used" -- which is why that warning is worth reading
     * rather than filtering out of the build log. */
    model_set_kv_int4(&m, want_kv_int4);

    printf("\n  passage: %d tokens%s%s\n", n, textfile ? " from " : "", textfile ? textfile : "");
    printf("  weights: %s\n", want_fast ? "fused integer path" : "float reference path");
    printf("  KV cache: %s\n", want_kv_f32 ? "float32"
           : (want_kv_int4 ? "4-bit with a scale per head" : "int8 with a scale per head"));
    fflush(stdout);

    double nll = 0.0;
    int counted = 0;
    int top1 = 0;

    for (int i = 0; i < n - 1; i++) {
        const float *logits = model_forward(&m, ids[i], i);
        if (!logits) { printf("  forward failed\n"); return 1; }

        /* Subtract the maximum before exponentiating. Without it exp() overflows and the metric
         * reports nan, which reads as a broken model rather than a broken measurement. */
        float mx = logits[0];
        for (int v = 1; v < m.vocab; v++) if (logits[v] > mx) mx = logits[v];
        double sum = 0.0;
        for (int v = 0; v < m.vocab; v++) sum += exp((double)(logits[v] - mx));

        const int32_t actual = ids[i + 1];
        nll += -((double)(logits[actual] - mx) - log(sum));
        counted++;

        if (model_argmax(&m) == actual) top1++;

        if ((i % 25) == 0) { printf("."); fflush(stdout); }
    }

    const double avg = nll / counted;
    printf("\n\n  ================================================\n");
    printf("  tokens scored          %10d\n", counted);
    printf("  average negative log   %10.4f\n", avg);
    printf("  PERPLEXITY             %10.3f   <- lower is better\n", exp(avg));
    printf("  top-1 agreement        %9.1f%%   <- how often greedy picks the real next token\n",
           100.0 * top1 / counted);
    printf("  ================================================\n");
    printf("\n  Run this again with the setting changed and compare. A change that leaves perplexity\n");
    printf("  alone is free; one that raises it is buying memory or speed with quality, which is a\n");
    printf("  trade worth making sometimes and worth knowing about always.\n\n");

    model_free(&m);
    tokenizer_free(&tk);
    gguf_close(&g);
    return 0;
}

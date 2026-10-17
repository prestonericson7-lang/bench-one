/* ===========================================================================================
 *  ppl_state.c -- what storing Qwen3-Next's recurrent state at 16 bits costs, measured
 *
 *  The 80B's 36 linear-attention layers carry a 32 x 128 x 128 state each: 72 MB at fp32, which the
 *  bench's PSRAM cannot hold; 36 MB at 16 bits, which it can. The question is whether rounding the state
 *  after every token hurts the model, and greedy text cannot answer it (two runs part wherever two logits
 *  are close, better or worse). So the same model runs several times IN LOCKSTEP over one passage -- each
 *  variant a way of computing it (below) -- and for every variant this reports:
 *
 *      perplexity                   the yardstick (lower is better)
 *      KL(first || variant) mean    how far its next-token distribution sits from the first variant's, per token
 *      KL(second || variant) mean   the same against the second (with f first: against the int8 path itself)
 *      top-1 agreement              how often the greedy pick is the actual next token, and the first's pick
 *      the KL in the last quarter   whether the error grows as the state ages
 *
 *  All variants share the file's weight map; a token's weights are read once and hit the page cache after.
 *
 *  BUILD (from firmware/bench-one/tests, vendored gcc on PATH):
 *    gcc -O3 -fopenmp -std=c11 -Wall -Wextra -I../shared -o ppl_state.exe ppl_state.c \
 *        ../shared/model_q.c ../shared/gguf.c ../shared/gguf_bits.c ../shared/gguf_dot.c ../shared/tokenizer.c -lm
 *  RUN
 *    ppl_state <qwen3next.gguf> --text file [--limit n] [--modes a,b,...]
 *  modes (2..5 of them, default 0,1,2): 0 1 2 3 the fused path with an fp32 / half / bfloat16 / row-scaled int16
 *  state; f the float path with an fp32 state and a float cache -- how far the int8 activations the Teensy uses
 *  already sit from float, the approximation accepted since doc 33. The first is the yardstick.
 * ======================================================================================== */
#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVMAX 5
/* a variant's state mode (-1 = the float path) and its router (0 float32, 1 bfloat16 'b', 2 int8 per row 'q') */
static int g_rt[NVMAX];
static const char *mode_name(int md, int rt)
{
    static const char *const N[] = { "int8+fp32", "int8+fp16", "int8+bf16", "int8+int16r" };
    static const char *const R[] = { "", "+rbf16", "+rint8" };
    static char buf[NVMAX][32];
    static int k;
    char *b = buf[k++ % NVMAX];
    snprintf(b, 32, "%s%s", md < 0 ? "float+fp32" : N[md], R[rt]);
    return b;
}

/* log-softmax of logits into lp (double): lp[v] = logit[v] - max - log sum exp */
static void log_softmax(const float *lg, int n, double *lp)
{
    float mx = lg[0];
    for (int v = 1; v < n; v++) if (lg[v] > mx) mx = lg[v];
    double s = 0.0;
    for (int v = 0; v < n; v++) s += exp((double)(lg[v] - mx));
    const double ls = log(s);
    for (int v = 0; v < n; v++) lp[v] = (double)(lg[v] - mx) - ls;
}

/* KL(a || b) over the vocabulary, a and b log-probabilities */
static double kl_div(const double *a, const double *b, int n)
{
    double d = 0.0;
    for (int t = 0; t < n; t++) d += exp(a[t]) * (a[t] - b[t]);
    return d;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("ppl_state <model.gguf> --text file [--limit n] [--modes a,b,...]\n"); return 1; }
    const char *textfile = NULL;
    int limit = 0, nv = 3, md[NVMAX] = { 0, 1, 2, 0, 0 };
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--text") && i + 1 < argc) textfile = argv[++i];
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--modes") && i + 1 < argc) {
            const char *q = argv[++i];
            nv = 0;
            while (*q && nv < NVMAX) {
                if (*q == 'f') md[nv] = -1;
                else if (*q >= '0' && *q <= '3') md[nv] = *q - '0';
                else { printf("--modes takes f and 0..3, each optionally followed by b (bf16 router) or q (int8 router)\n"); return 1; }
                q++;
                g_rt[nv] = *q == 'b' ? 1 : *q == 'q' ? 2 : 0;
                if (g_rt[nv]) q++;
                nv++;
                if (*q == ',') q++;
            }
            if (nv < 2) { printf("--modes needs at least two\n"); return 1; }
        }
    }
    if (!textfile) { printf("--text is required\n"); return 1; }

    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("open: %s\n", g.err); return 1; }
    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { printf("tokenizer: %s\n", g.err); return 1; }

    FILE *tf = fopen(textfile, "rb");
    if (!tf) { printf("cannot open %s\n", textfile); return 1; }
    fseek(tf, 0, SEEK_END);
    const long sz = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    char *body = (char *)malloc((size_t)sz + 1);
    if (!body || fread(body, 1, (size_t)sz, tf) != (size_t)sz) { printf("cannot read %s\n", textfile); return 1; }
    body[sz] = 0;
    fclose(tf);

    static int32_t ids[65536];
    int n = tokenizer_encode(&tk, body, ids, 65536);
    if (n < 2) { printf("passage did not encode\n"); return 1; }
    if (limit > 1 && n > limit) n = limit;

    static model_t m[NVMAX];
    for (int v = 0; v < nv; v++) {
        if (model_load(&m[v], &g, n + 8)) { printf("load: %s\n", g.err); return 1; }
        if (!m[v].hybrid) { printf("not a hybrid model: no recurrent state to round\n"); return 1; }
        model_set_fast(&m[v], md[v] >= 0);
        model_set_state16(&m[v], md[v] > 0 ? md[v] : 0);
        model_round_router(&m[v], g_rt[v]);
        if (md[v] < 0 && model_set_kv_f32(&m[v], 1)) { printf("no float cache\n"); return 1; }
    }
    const int vocab = m[0].v_hi - m[0].v_lo;
    printf("\n  passage: %d tokens from %s\n  variants:", n, textfile);
    for (int v = 0; v < nv; v++) printf(" %s%s", mode_name(md[v], g_rt[v]), v ? "" : " (the yardstick)");
    printf("\n  %d linear-attention layers, state %d x %d x %d each\n", m[0].n_linear, m[0].ssm_vheads, m[0].ssm_state, m[0].ssm_state);
    fflush(stdout);

    double *lp[NVMAX];
    for (int v = 0; v < nv; v++) lp[v] = (double *)malloc((size_t)vocab * sizeof(double));
    double nll[NVMAX] = { 0 }, kl[NVMAX] = { 0 }, kl2[NVMAX] = { 0 }, kl_late[NVMAX] = { 0 }, klmax[NVMAX] = { 0 };
    int agree[NVMAX] = { 0 }, top1[NVMAX] = { 0 }, counted = 0, late = 0;
    const int late0 = (n - 1) * 3 / 4;

    for (int i = 0; i < n - 1; i++) {
        int32_t best[NVMAX];
        for (int v = 0; v < nv; v++) {
            const float *lg = model_forward(&m[v], ids[i], i);
            if (!lg) { printf("forward failed\n"); return 1; }
            log_softmax(lg, vocab, lp[v]);
            best[v] = model_argmax(&m[v]);
        }
        const int32_t actual = ids[i + 1];
        for (int v = 0; v < nv; v++) {
            nll[v] += -lp[v][actual];
            if (best[v] == actual) top1[v]++;
            if (best[v] == best[0]) agree[v]++;
            const double d = kl_div(lp[0], lp[v], vocab);
            kl[v] += d;
            if (d > klmax[v]) klmax[v] = d;
            if (i >= late0) kl_late[v] += d;
            kl2[v] += kl_div(lp[1], lp[v], vocab);
        }
        counted++;
        if (i >= late0) late++;
        if ((i % 32) == 0) {
            printf("  %5d/%d  ppl", i + 1, n - 1);
            for (int v = 0; v < nv; v++) printf(" %.4f", exp(nll[v] / counted));
            printf("  KL vs first");
            for (int v = 1; v < nv; v++) printf(" %.2e", kl[v] / counted);
            printf("\n");
            fflush(stdout);
        }
    }

    printf("\n  ===========================================================================================\n");
    printf("  %d tokens scored (last quarter: %d); KL in nats a token\n", counted, late);
    printf("  %-17s %10s %11s %11s %11s %11s %8s %8s\n", "variant", "perplexity", "KL v first", "KL max", "KL late", "KL v 2nd",
           "top-1", "=1st");
    for (int v = 0; v < nv; v++)
        printf("  %-17s %10.4f %11.3e %11.3e %11.3e %11.3e %7.1f%% %7.1f%%\n", mode_name(md[v], g_rt[v]), exp(nll[v] / counted),
               kl[v] / counted, klmax[v], late ? kl_late[v] / late : 0.0, kl2[v] / counted,
               100.0 * top1[v] / counted, 100.0 * agree[v] / counted);
    printf("  ===========================================================================================\n\n");

    for (int v = 0; v < nv; v++) { model_free(&m[v]); free(lp[v]); }
    tokenizer_free(&tk);
    gguf_close(&g);
    free(body);
    return 0;
}

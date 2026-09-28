/* ===========================================================================================
 *  tl_ref.c -- the reference the Teensy core is checked against: shared/model_q.c on its fused
 *  integer path (model_set_fast 1), fed one token per position exactly as the Teensy feeds it, printing
 *  the same line per position as tests/tl_host.c. The two outputs must be byte-identical.
 *
 *  BUILD (from firmware/bench-one/tests, vendored gcc on PATH):
 *    gcc -O3 -fopenmp -std=c11 -Wall -Wextra -I../shared -o tl_ref.exe tl_ref.c \
 *        ../shared/model_q.c ../shared/gguf.c ../shared/gguf_bits.c ../shared/gguf_dot.c ../shared/tokenizer.c -lm
 *  RUN
 *    tl_ref.exe <model.gguf> "prompt" [n_gen] > ref.txt
 * ======================================================================================== */
#include "gguf.h"
#include "model_q.h"
#include "tokenizer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void esc(const char *s, int n, char *o, int omax)
{
    int k = 0;
    for (int i = 0; i < n && k < omax - 5; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c >= 0x20 && c < 0x7F) o[k++] = (char)c;
        else k += snprintf(o + k, (size_t)(omax - k), "\\x%02X", c);
    }
    o[k] = 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: tl_ref <model.gguf> \"prompt\" [n_gen]\n"); return 1; }
    const int n_gen = argc > 3 ? atoi(argv[3]) : 8;
    gguf_t g;
    if (gguf_open(&g, argv[1])) { fprintf(stderr, "open: %s\n", g.err); return 1; }
    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { fprintf(stderr, "tokenizer: %s\n", g.err); return 1; }

    if (getenv("TL_TOKREC")) {                   /* records separated by NUL bytes, newlines and all */
        FILE *tf = fopen(getenv("TL_TOKREC"), "rb");
        if (!tf) { fprintf(stderr, "cannot open %s\n", getenv("TL_TOKREC")); return 1; }
        static char rb[1 << 16];
        static int32_t tids[1 << 16];
        int ln = 0, c, k = 0;
        do {
            c = fgetc(tf);
            if (c == 0 || (c == EOF && k)) {
                rb[k] = 0;
                ln++;
                const int m = tokenizer_encode(&tk, rb, tids, 1 << 16);
                printf("L%d %d:", ln, m);
                for (int i = 0; i < m; i++) printf(" %d", (int)tids[i]);
                printf("\n");
                k = 0;
            } else if (c != EOF && k < (int)sizeof rb - 1) rb[k++] = (char)c;
        } while (c != EOF);
        fclose(tf);
        return 0;
    }
    if (getenv("TL_TOKFILE")) {                  /* the same tokenizer-parity output tl_host.c prints */
        FILE *tf = fopen(getenv("TL_TOKFILE"), "rb");
        if (!tf) { fprintf(stderr, "cannot open %s\n", getenv("TL_TOKFILE")); return 1; }
        static char lb[65536];
        static int32_t tids[65536];
        int ln = 0;
        while (fgets(lb, sizeof lb, tf)) {
            ln++;
            const int k = tokenizer_encode(&tk, lb, tids, 65536);
            printf("L%d %d:", ln, k);
            for (int i = 0; i < k; i++) printf(" %d", (int)tids[i]);
            printf("\n");
        }
        fclose(tf);
        return 0;
    }

    int32_t ids[512];
    const int n = tokenizer_encode(&tk, argv[2], ids, 512);
    if (n <= 0) { fprintf(stderr, "prompt did not encode\n"); return 1; }
    printf("prompt ids:");
    for (int i = 0; i < n; i++) printf(" %d", (int)ids[i]);
    printf("\n");

    model_t m;
    if (model_load(&m, &g, n + n_gen + 8)) { fprintf(stderr, "load: %s\n", g.err); return 1; }
    model_set_fast(&m, 1);
    /* TL_REF_FLOAT=1: model_q's float path instead -- weights dequantized to float, float attention cache.
     * Not what the Teensy computes; it is the yardstick for how far the fused int8 path sits from float. */
    if (getenv("TL_REF_FLOAT") && atoi(getenv("TL_REF_FLOAT"))) {
        model_set_fast(&m, 0);
        if (model_set_kv_f32(&m, 1)) { fprintf(stderr, "no float cache\n"); return 1; }
    }
    /* TL_REF_KVF32=1: the fused path with a float cache -- separates what the int8 cache changes from what
     * the int8 activations change. */
    if (getenv("TL_REF_KVF32") && atoi(getenv("TL_REF_KVF32")) && model_set_kv_f32(&m, 1)) {
        fprintf(stderr, "no float cache\n");
        return 1;
    }
    const int vocab = m.v_hi - m.v_lo;

    char txt[256], e[1024];
    int32_t next = 0;
    for (int step = 0; step < n + n_gen; step++) {
        const int32_t fed = step < n ? ids[step] : next;
        const float *lg = model_forward(&m, fed, step);
        if (!lg) { fprintf(stderr, "forward failed\n"); return 1; }
        /* the same two-best scan tl_core.c runs as the rows stream past */
        float b1 = -INFINITY, b2 = -INFINITY;
        int32_t i1 = 0, i2 = -1;
        for (int i = 0; i < vocab; i++) {
            const float v = lg[i];
            if (v > b1) { b2 = b1; i2 = i1; b1 = v; i1 = i; }
            else if (v > b2) { b2 = v; i2 = i; }
        }
        const int k = tokenizer_decode(&tk, i1, txt, (int)sizeof txt - 1);
        esc(txt, k, e, (int)sizeof e);
        printf("step %d pos %d fed %d -> top1 %d %.9g top2 %d %.9g text \"%s\"\n",
               step, step, (int)fed, (int)i1, b1, (int)i2, b2, e);
        fflush(stdout);
        next = i1;
        if (step >= n - 1 && i1 == tk.eos) break;
    }
    return 0;
}

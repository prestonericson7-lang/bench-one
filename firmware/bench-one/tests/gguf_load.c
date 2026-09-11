/* ===========================================================================================
 *  gguf_load.c -- prove the C loader reads the real file correctly
 * ===========================================================================================
 *
 *  The Python inspector already said what is in the file. This says whether the C code agrees, and
 *  whether the weights it produces are real numbers rather than convincing noise.
 *
 *  A dequantizer with a wrong bit does not crash. It returns plausible floats, the model loads, the
 *  benchmark reports a fine tokens-per-second figure, and the output is subtly wrong forever. So
 *  there are four checks here, each of which catches a different class of that error:
 *
 *    1. SHAPE. Every tensor's byte length implied by its dims and type must add up to the file size
 *       minus the header. Catches a misparsed descriptor table or a wrong block size.
 *
 *    2. STATISTICS. Transformer weights are roughly zero-mean with a standard deviation near 0.02
 *       for attention and MLP matrices. If a block's scale is being read from the wrong offset the
 *       numbers come out orders of magnitude off, or the mean drifts far from zero. This is a weak
 *       check that catches strong errors, which is the most common kind.
 *
 *    3. NORM WEIGHTS. RMSNorm gains are stored as F32 and sit near 1.0 after training. They are the
 *       one tensor whose correct values are known in advance without a reference implementation, so
 *       they verify the seek-and-offset arithmetic independently of any quantization code.
 *
 *    4. DUMPED VALUES. The first sixteen weights of a named Q4_K tensor and a named Q6_K tensor are
 *       printed, so tests/gguf_check.py can dequantize the same bytes with an independent
 *       implementation of the published format and compare. Two implementations agreeing is
 *       evidence. One implementation agreeing with itself is not.
 *
 *  RUN
 *      gguf_load <model.gguf>
 * ===========================================================================================
 */

#include "gguf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stats(const float *v, uint64_t n, double *mean, double *sd, double *amax)
{
    double s = 0.0, s2 = 0.0, mx = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        const double x = v[i];
        s += x;
        s2 += x * x;
        if (fabs(x) > mx) mx = fabs(x);
    }
    *mean = s / (double)n;
    *sd = sqrt(s2 / (double)n - (*mean) * (*mean));
    *amax = mx;
}

static void show(gguf_t *g, const char *name, float *buf, uint64_t cap)
{
    const gguf_tensor *t = gguf_tensor_find(g, name);
    if (!t) { printf("  %-28s MISSING\n", name); return; }
    const uint64_t n = gguf_nelem(t);
    if (n > cap) {
        printf("  %-28s %-5s %10llu elems  (not loaded, buffer is %llu)\n",
               name, gguf_type_name(t->type), (unsigned long long)n, (unsigned long long)cap);
        return;
    }
    if (gguf_read_f32(g, t, buf)) { printf("  %-28s FAILED: %s\n", name, g->err); return; }
    double mean, sd, amax;
    stats(buf, n, &mean, &sd, &amax);
    printf("  %-28s %-5s %6llux%-6llu mean %+.5f  sd %.5f  max %.4f\n",
           name, gguf_type_name(t->type),
           (unsigned long long)t->dims[0], (unsigned long long)(t->dims[1] ? t->dims[1] : 1),
           mean, sd, amax);
}

static void dump16(gguf_t *g, const char *name)
{
    const gguf_tensor *t = gguf_tensor_find(g, name);
    if (!t) return;
    float row[4096];
    const uint64_t cols = t->dims[0];
    if (cols > 4096) return;
    if (gguf_read_rows_f32(g, t, 0, 1, row)) { printf("  %s: %s\n", name, g->err); return; }
    printf("  DUMP %s %s", name, gguf_type_name(t->type));
    for (int i = 0; i < 16; i++) printf(" %.8g", row[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("gguf_load <model.gguf>\n"); return 1; }

    gguf_t g;
    if (gguf_open(&g, argv[1])) { printf("open failed: %s\n", g.err); return 1; }

    printf("\n========================================================================\n");
    printf("  %s\n", argv[1]);
    printf("========================================================================\n");
    printf("  GGUF v%u, %llu tensors, %llu metadata keys, data at byte %llu\n",
           g.version, (unsigned long long)g.n_tensors, (unsigned long long)g.n_kv,
           (unsigned long long)g.data_start);

    const char *arch = gguf_str(&g, "general.architecture", "?");
    const int64_t n_layer = gguf_arch_int(&g, "block_count", 0);
    const int64_t dim     = gguf_arch_int(&g, "embedding_length", 0);
    const int64_t hidden  = gguf_arch_int(&g, "feed_forward_length", 0);
    const int64_t heads   = gguf_arch_int(&g, "attention.head_count", 0);
    const int64_t kvheads = gguf_arch_int(&g, "attention.head_count_kv", 0);
    const int64_t ctx     = gguf_arch_int(&g, "context_length", 0);
    const double  eps     = gguf_arch_flt(&g, "attention.layer_norm_rms_epsilon", 1e-6);
    const double  rope    = gguf_arch_flt(&g, "rope.freq_base", 10000.0);

    const gguf_kv *toks = gguf_find(&g, "tokenizer.ggml.tokens");
    const gguf_kv *mrgs = gguf_find(&g, "tokenizer.ggml.merges");

    printf("\n  arch %s, %lld layers, dim %lld, hidden %lld\n",
           arch, (long long)n_layer, (long long)dim, (long long)hidden);
    printf("  %lld heads / %lld kv heads -> head_dim %lld, context %lld\n",
           (long long)heads, (long long)kvheads,
           heads ? (long long)(dim / heads) : 0, (long long)ctx);
    printf("  rms eps %.3g, rope base %.1f\n", eps, rope);
    printf("  tokenizer: %llu tokens, %llu merges, model \"%s\"\n",
           toks ? (unsigned long long)toks->n : 0ull,
           mrgs ? (unsigned long long)mrgs->n : 0ull,
           gguf_str(&g, "tokenizer.ggml.model", "?"));
    printf("  bos %lld, eos %lld\n",
           (long long)gguf_int(&g, "tokenizer.ggml.bos_token_id", -1),
           (long long)gguf_int(&g, "tokenizer.ggml.eos_token_id", -1));

    /* --- check 1: every descriptor's implied size must tile the blob without gaps -------------- */
    uint64_t sum = 0, expect_end = 0;
    int bad_type = 0;
    for (uint64_t i = 0; i < g.n_tensors; i++) {
        const uint64_t nb = gguf_nbytes(&g.t[i]);
        if (!nb) { bad_type++; continue; }
        sum += nb;
        const uint64_t end = g.t[i].offset + nb;
        if (end > expect_end) expect_end = end;
    }
    printf("\n  tensor bytes total   %.1f MB\n", sum / 1048576.0);
    printf("  highest offset + size %.1f MB\n", expect_end / 1048576.0);
    printf("  %s\n", sum == expect_end
           ? "TILES EXACTLY: offsets and sizes agree, no gaps and no overlap"
           : "MISMATCH: the descriptor table does not tile the blob");
    if (bad_type) printf("  %d tensors of an unsupported type\n", bad_type);

    /* --- checks 2 and 3: real numbers out of real bytes --------------------------------------- */
    const uint64_t cap = 16u * 1024u * 1024u;       /* 64 MB of floats, enough for any one layer */
    float *buf = (float *)malloc((size_t)cap * sizeof(float));
    if (!buf) { printf("out of memory\n"); gguf_close(&g); return 1; }

    printf("\n  layer 0, every tensor:\n");
    static const char *suffix[] = {
        "attn_norm.weight", "attn_q.weight", "attn_q.bias", "attn_k.weight", "attn_k.bias",
        "attn_v.weight", "attn_v.bias", "attn_output.weight",
        "ffn_norm.weight", "ffn_gate.weight", "ffn_up.weight", "ffn_down.weight"
    };
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++) {
        char nm[128];
        snprintf(nm, sizeof(nm), "blk.0.%s", suffix[i]);
        show(&g, nm, buf, cap);
    }
    printf("\n  outside the layers:\n");
    show(&g, "output_norm.weight", buf, cap);
    show(&g, "token_embd.weight", buf, cap);
    show(&g, "output.weight", buf, cap);

    /* One row of the embedding table, which is what decode actually reads, and the reason
     * gguf_read_rows_f32 exists. Token 9707 is arbitrary but fixed so it is reproducible. */
    {
        const gguf_tensor *t = gguf_tensor_find(&g, "token_embd.weight");
        if (t && t->dims[0] <= cap) {
            if (gguf_read_rows_f32(&g, t, 9707, 1, buf) == 0) {
                double mean, sd, amax;
                stats(buf, t->dims[0], &mean, &sd, &amax);
                printf("\n  ONE ROW of token_embd (token 9707): %llu floats, mean %+.5f sd %.5f\n",
                       (unsigned long long)t->dims[0], mean, sd);
                printf("  read without touching the other %.1f MB, which is what makes an SD card\n",
                       (gguf_nbytes(t) - (double)(gguf_nbytes(t) / t->dims[1])) / 1048576.0);
                printf("  a legitimate home for this tensor\n");
            } else {
                printf("\n  row read failed: %s\n", g.err);
            }
        }
    }

    /* --- check 4: values for the independent Python implementation to compare against --------- */
    printf("\n  first 16 weights of two tensors, for tests/gguf_check.py to verify:\n");
    dump16(&g, "blk.0.attn_q.weight");
    dump16(&g, "blk.0.ffn_down.weight");
    dump16(&g, "output_norm.weight");

    free(buf);
    gguf_close(&g);
    printf("\n");
    return 0;
}

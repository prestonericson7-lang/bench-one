/* model_q.c -- see model_q.h. The Llama-shaped decoder, with real weights. */

#include "model_q.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLS 16384     /* widest row in any model this is pointed at; 11008 here */

/* ---------------------------------------------------------------------------------------------
 *  loading
 * ------------------------------------------------------------------------------------------ */

static int load_q(model_t *m, gguf_t *g, qten_t *q, const char *fmt, ...)
{
    char name[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);

    const gguf_tensor *t = gguf_tensor_find(g, name);
    if (!t) { snprintf(g->err, sizeof(g->err), "missing tensor %s", name); return -1; }

    q->type = t->type;
    q->cols = (uint32_t)t->dims[0];
    q->rows = (uint32_t)(t->dims[1] ? t->dims[1] : 1);
    /* A mixture-of-experts tensor is three-dimensional: [cols][rows][expert]. Treating it as two
     * loads the whole allocation but describes only the first expert, so every expert after the
     * first is invisible and the matvec walks off the end of what it thinks it has. */
    if (t->n_dims > 2 && t->dims[2] > 1) q->rows *= (uint32_t)t->dims[2];
    q->row_bytes = gguf_row_bytes(t);
    if (!q->row_bytes) { snprintf(g->err, sizeof(g->err), "%s: unsupported type", name); return -1; }
    if (q->cols > MAX_COLS) {
        snprintf(g->err, sizeof(g->err), "%s: %u columns exceeds MAX_COLS %d", name, q->cols, MAX_COLS);
        return -1;
    }

    const uint64_t nb = gguf_nbytes(t);
    q->raw = (uint8_t *)malloc((size_t)nb);
    if (!q->raw) {
        snprintf(g->err, sizeof(g->err), "%s: cannot allocate %.1f MB", name, nb / 1048576.0);
        return -1;
    }
    if (gguf_read_raw(g, t, q->raw)) return -1;
    m->weight_bytes += nb;
    return 0;
}

/* Norm gains and biases are F32 and tiny, so these genuinely do get dequantized once. */
static float *load_f32(model_t *m, gguf_t *g, int required, const char *fmt, ...)
{
    char name[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);

    const gguf_tensor *t = gguf_tensor_find(g, name);
    if (!t) {
        if (required) snprintf(g->err, sizeof(g->err), "missing tensor %s", name);
        return NULL;
    }
    const uint64_t n = gguf_nelem(t);
    float *v = (float *)malloc((size_t)n * sizeof(float));
    if (!v) { snprintf(g->err, sizeof(g->err), "%s: out of memory", name); return NULL; }
    if (gguf_read_f32(g, t, v)) { free(v); return NULL; }
    m->weight_bytes += gguf_nbytes(t);
    return v;
}

int model_load(model_t *m, gguf_t *g, int max_seq)
{
    const int total = (int)gguf_arch_int(g, "block_count", 0);
    return model_load_range(m, g, max_seq, 0, total, 1, 1);
}

int model_layer_lo(const model_t *m) { return m->layer_lo; }

int model_load_range(model_t *m, gguf_t *g, int max_seq, int layer0, int layer1,
                     int want_embd, int want_head)
{
    /* The undivided case: if this node has the head at all, it has the whole vocabulary. */
    const gguf_tensor *e = gguf_tensor_find(g, "token_embd.weight");
    const int vocab = e ? (int)(e->dims[1] ? e->dims[1] : 1) : 0;
    return model_load_slice(m, g, max_seq, layer0, layer1,
                            want_head ? 0 : 0, want_head ? vocab : 0, want_embd);
}

int model_load_slice(model_t *m, gguf_t *g, int max_seq, int layer0, int layer1,
                     int v_lo, int v_hi, int embed_from_disk)
{
    memset(m, 0, sizeof(*m));

    const int total = (int)gguf_arch_int(g, "block_count", 0);
    m->dim       = (int)gguf_arch_int(g, "embedding_length", 0);
    m->hidden    = (int)gguf_arch_int(g, "feed_forward_length", 0);
    m->n_heads   = (int)gguf_arch_int(g, "attention.head_count", 0);
    m->n_kv      = (int)gguf_arch_int(g, "attention.head_count_kv", m->n_heads);
    m->eps       = (float)gguf_arch_flt(g, "attention.layer_norm_rms_epsilon", 1e-6);
    m->rope_base = (float)gguf_arch_flt(g, "rope.freq_base", 10000.0);
    if (!total || !m->dim || !m->n_heads) {
        snprintf(g->err, sizeof(g->err), "incomplete architecture metadata");
        return -1;
    }
    if (layer0 < 0 || layer1 > total || layer0 >= layer1) {
        snprintf(g->err, sizeof(g->err), "layer range %d..%d outside 0..%d", layer0, layer1, total);
        return -1;
    }
    /* head_dim comes from the file when the file says so. dim / n_heads is only a fallback, and on
     * Qwen3-30B it is wrong by a factor of two: dim 2048, 32 heads, head_dim 128. */
    {
        const int klen = (int)gguf_arch_int(g, "attention.key_length", 0);
        m->head_dim = klen ? klen : m->dim / m->n_heads;
    }
    m->q_dim = m->n_heads * m->head_dim;

    m->n_expert      = (int)gguf_arch_int(g, "expert_count", 0);
    m->n_expert_used = (int)gguf_arch_int(g, "expert_used_count", 0);
    m->expert_ff     = (int)gguf_arch_int(g, "expert_feed_forward_length", 0);
    if (m->n_expert > 0 && (m->n_expert_used <= 0 || m->expert_ff <= 0)) {
        snprintf(g->err, sizeof(g->err),
                 "%d experts but expert_used_count=%d expert_feed_forward_length=%d",
                 m->n_expert, m->n_expert_used, m->expert_ff);
        return -1;
    }
    m->max_seq  = max_seq;
    m->layer_lo = layer0;
    m->n_layer  = layer1 - layer0;        /* layers RESIDENT here, not in the model */
    m->has_embd = embed_from_disk ? 1 : 0;
    m->has_head = (v_hi > v_lo) ? 1 : 0;
    m->v_lo = v_lo;
    m->v_hi = v_hi;

    /* The vocabulary size is needed even by a node that holds no embedding table, because it sizes
     * the logits buffer and appears in reporting. Read it from the descriptor without loading the
     * 243 MB of weights behind it. */
    {
        const gguf_tensor *t = gguf_tensor_find(g, "token_embd.weight");
        if (!t) { snprintf(g->err, sizeof(g->err), "no token_embd.weight"); return -1; }
        m->vocab = (int)(t->dims[1] ? t->dims[1] : 1);
    }
    m->tied_output = gguf_tensor_find(g, "output.weight") ? 0 : 1;

    /* The input lookup is served from the FILE, not from memory. One 4 KB row per token against a
     * 243 MB table: holding it in RAM to read a thousandth of it would be the single most wasteful
     * megabyte in the machine. */
    if (embed_from_disk) {
        m->src = g;
        m->embd_t = gguf_tensor_find(g, "token_embd.weight");
        if (!m->embd_t) { snprintf(g->err, sizeof(g->err), "no token_embd.weight"); return -1; }
        m->rowbuf = (uint8_t *)malloc((size_t)gguf_row_bytes(m->embd_t));
        if (!m->rowbuf) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }
    }

    /* The projection slice this node owns, and only that slice. */
    if (v_hi > v_lo) {
        const char *nm = m->tied_output ? "token_embd.weight" : "output.weight";
        const gguf_tensor *t = gguf_tensor_find(g, nm);
        if (!t) { snprintf(g->err, sizeof(g->err), "no %s", nm); return -1; }
        const uint64_t rb = gguf_row_bytes(t);
        const uint64_t n = (uint64_t)(v_hi - v_lo);
        m->out_head.type = t->type;
        m->out_head.cols = (uint32_t)t->dims[0];
        m->out_head.rows = (uint32_t)n;
        m->out_head.row_bytes = rb;
        m->out_head.raw = (uint8_t *)malloc((size_t)(rb * n));
        if (!m->out_head.raw) {
            snprintf(g->err, sizeof(g->err), "%s: cannot allocate %.1f MB of slice",
                     nm, rb * n / 1048576.0);
            return -1;
        }
        if (gguf_read_raw_rows(g, t, (uint64_t)v_lo, n, m->out_head.raw)) return -1;
        m->weight_bytes += rb * n;

        m->out_norm = load_f32(m, g, 1, "output_norm.weight");
        if (!m->out_norm) return -1;
    }

    m->L = (mlayer_t *)calloc((size_t)m->n_layer, sizeof(mlayer_t));
    if (!m->L) { snprintf(g->err, sizeof(g->err), "out of memory"); return -1; }

    for (int i = 0; i < m->n_layer; i++) {
        const int l = layer0 + i;          /* GLOBAL layer index, which is what the file is keyed by */
        mlayer_t *L = &m->L[i];
        L->attn_norm = load_f32(m, g, 1, "blk.%d.attn_norm.weight", l);
        L->ffn_norm  = load_f32(m, g, 1, "blk.%d.ffn_norm.weight", l);
        if (!L->attn_norm || !L->ffn_norm) return -1;
        /* Not required: Llama has no attention biases, Qwen2 does. */
        L->bq = load_f32(m, g, 0, "blk.%d.attn_q.bias", l);
        L->bk = load_f32(m, g, 0, "blk.%d.attn_k.bias", l);
        L->bv = load_f32(m, g, 0, "blk.%d.attn_v.bias", l);
        /* Qwen3 only. Absent on Qwen2 and Llama, and absence is not an error. */
        L->q_norm = load_f32(m, g, 0, "blk.%d.attn_q_norm.weight", l);
        L->k_norm = load_f32(m, g, 0, "blk.%d.attn_k_norm.weight", l);
        if (load_q(m, g, &L->wq,     "blk.%d.attn_q.weight", l)      ||
            load_q(m, g, &L->wk,     "blk.%d.attn_k.weight", l)      ||
            load_q(m, g, &L->wv,     "blk.%d.attn_v.weight", l)      ||
            load_q(m, g, &L->wo,     "blk.%d.attn_output.weight", l)) return -1;

        if (m->n_expert > 0) {
            L->router = load_f32(m, g, 1, "blk.%d.ffn_gate_inp.weight", l);
            if (!L->router) return -1;
            if (load_q(m, g, &L->e_gate, "blk.%d.ffn_gate_exps.weight", l) ||
                load_q(m, g, &L->e_up,   "blk.%d.ffn_up_exps.weight", l)   ||
                load_q(m, g, &L->e_down, "blk.%d.ffn_down_exps.weight", l)) return -1;
            /* The stacking has to be exactly what the forward pass assumes, and getting it wrong
             * gives plausible nonsense rather than a crash. Check it here instead. */
            if (L->e_gate.rows != (uint32_t)(m->expert_ff * m->n_expert) ||
                L->e_down.rows != (uint32_t)(m->dim * m->n_expert)) {
                snprintf(g->err, sizeof(g->err),
                         "layer %d: expert tensors are %u and %u rows, expected %d and %d",
                         l, L->e_gate.rows, L->e_down.rows,
                         m->expert_ff * m->n_expert, m->dim * m->n_expert);
                return -1;
            }
        } else {
            if (load_q(m, g, &L->w_gate, "blk.%d.ffn_gate.weight", l)    ||
                load_q(m, g, &L->w_up,   "blk.%d.ffn_up.weight", l)      ||
                load_q(m, g, &L->w_down, "blk.%d.ffn_down.weight", l)) return -1;
        }
    }

    if (m->n_expert > 0) {
        m->expert_hits = (uint32_t *)calloc((size_t)m->n_layer * m->n_expert, sizeof(uint32_t));
        m->router_p    = (float *)calloc((size_t)m->n_expert, sizeof(float));
        const size_t ke = (size_t)m->n_expert_used;
        m->e_out = (float *)calloc(ke * (size_t)m->dim, sizeof(float));
        m->e_xq  = (int8_t *)calloc(ke * (size_t)m->expert_ff, 1);
        m->e_xs  = (float *)calloc(ke * (size_t)m->expert_ff / 32 + ke, sizeof(float));
        if (!m->expert_hits || !m->router_p || !m->e_out || !m->e_xq || !m->e_xs) {
            snprintf(g->err, sizeof(g->err), "out of memory");
            return -1;
        }
    }

    m->n_streams = 1;
    const size_t kv = (size_t)m->n_layer * (size_t)max_seq * (size_t)m->n_kv * (size_t)m->head_dim;
    const size_t ks = (size_t)m->n_layer * (size_t)max_seq * (size_t)m->n_kv;
    m->k_cache = (int8_t *)calloc(kv, 1);
    m->v_cache = (int8_t *)calloc(kv, 1);
    m->k_scale = (float *)calloc(ks, sizeof(float));
    m->v_scale = (float *)calloc(ks, sizeof(float));
    /* Every scratch buffer is sized to the WIDEST thing that lands in it. q and the attention
     * output are q_dim wide, not dim, and the feed-forward buffer is expert_ff on a mixture model
     * and hidden on a dense one. Sizing any of them to dim is a silent overrun on Qwen3. */
    const int wide_x  = m->q_dim > m->dim ? m->q_dim : m->dim;
    /* With the experts fused into one region, hb holds all of them at once. */
    const int all_ff = m->n_expert > 0 ? m->expert_ff * m->n_expert_used : 0;
    const int wide_ff = all_ff > m->hidden ? all_ff : m->hidden;
    m->x       = (float *)calloc((size_t)wide_x, sizeof(float));
    m->xb      = (float *)calloc((size_t)wide_x, sizeof(float));
    m->xb2     = (float *)calloc((size_t)wide_x, sizeof(float));
    m->q       = (float *)calloc((size_t)wide_x, sizeof(float));
    m->att     = (float *)calloc((size_t)max_seq, sizeof(float));
    m->hb      = (float *)calloc((size_t)(wide_ff ? wide_ff : 1), sizeof(float));
    m->hb2     = (float *)calloc((size_t)(wide_ff ? wide_ff : 1), sizeof(float));
    /* Logits are sized to the SLICE, not to the vocabulary. A node holding a quarter of the words
     * produces a quarter of the numbers. */
    m->logits  = (float *)calloc((size_t)(m->v_hi > m->v_lo ? m->v_hi - m->v_lo : 1), sizeof(float));
    {
        /* The quantized activation is reused for every matrix in the layer, so it must fit the
         * widest INPUT any of them takes: dim for q/k/v and the experts, q_dim for the attention
         * output, expert_ff for an expert's down projection. */
        size_t wide = (size_t)wide_x;
        if ((size_t)wide_ff > wide) wide = (size_t)wide_ff;
        if ((size_t)m->hidden > wide) wide = (size_t)m->hidden;
        m->xq = (int8_t *)calloc(wide, 1);
        m->xs = (float *)calloc(wide / 32 + 1, sizeof(float));
        m->xsum = (int32_t *)calloc(wide / 32 + 1, sizeof(int32_t));
    }
    if (!m->k_cache || !m->v_cache || !m->k_scale || !m->v_scale ||
        !m->x || !m->xb || !m->xb2 || !m->q ||
        !m->att || !m->hb || !m->hb2 || !m->logits || !m->xq || !m->xs) {
        snprintf(g->err, sizeof(g->err), "cannot allocate the KV cache and scratch");
        return -1;
    }

    /* Bytes READ to produce one token, which is the number that sets decode speed on every target.
     * Every layer in full, plus the output head in full. The input embedding lookup is one row and
     * rounds to nothing against 1.8 GB, but it is counted because being exact is free here. */
    uint64_t per_tok = 0;
    for (int l = 0; l < m->n_layer; l++) {
        const mlayer_t *L = &m->L[l];
        per_tok += (uint64_t)L->wq.rows * L->wq.row_bytes;
        per_tok += (uint64_t)L->wk.rows * L->wk.row_bytes;
        per_tok += (uint64_t)L->wv.rows * L->wv.row_bytes;
        per_tok += (uint64_t)L->wo.rows * L->wo.row_bytes;
        if (m->n_expert > 0) {
            /* Only the experts that fire are read, which is the entire reason a 30B is even worth
             * discussing on this machine. Counting all 128 would say 18 GB a token; the truth is
             * 8 of them. */
            per_tok += (uint64_t)m->n_expert_used * m->expert_ff * L->e_gate.row_bytes;
            per_tok += (uint64_t)m->n_expert_used * m->expert_ff * L->e_up.row_bytes;
            per_tok += (uint64_t)m->n_expert_used * m->dim       * L->e_down.row_bytes;
            per_tok += (uint64_t)m->n_expert * m->dim * sizeof(float);   /* the router, in full */
        } else {
            per_tok += (uint64_t)L->w_gate.rows * L->w_gate.row_bytes;
            per_tok += (uint64_t)L->w_up.rows * L->w_up.row_bytes;
            per_tok += (uint64_t)L->w_down.rows * L->w_down.row_bytes;
        }
    }
    if (m->has_head) per_tok += (uint64_t)m->out_head.rows * m->out_head.row_bytes;
    if (m->has_embd) per_tok += gguf_row_bytes(m->embd_t);
    m->bytes_per_token = per_tok;
    return 0;
}

void model_free(model_t *m)
{
#ifdef ZACCEL_OFFLOAD
    model_zaccel_detach(m);
#endif
    if (m->L) {
        for (int l = 0; l < m->n_layer; l++) {
            mlayer_t *L = &m->L[l];
            free(L->attn_norm); free(L->ffn_norm);
            free(L->bq); free(L->bk); free(L->bv);
            free(L->wq.raw); free(L->wk.raw); free(L->wv.raw); free(L->wo.raw);
            free(L->w_gate.raw); free(L->w_up.raw); free(L->w_down.raw);
            free(L->q_norm); free(L->k_norm); free(L->router);
            free(L->e_gate.raw); free(L->e_up.raw); free(L->e_down.raw);
        }
        free(m->L);
    }
    free(m->expert_hits);
    free(m->router_p);
    free(m->e_out); free(m->e_xq); free(m->e_xs);
    /* out_head now owns its own slice buffer even on a tied model, so both are freed. */
    free(m->embd.raw);
    free(m->out_head.raw);
    free(m->rowbuf);
    free(m->out_norm);
    free(m->k_cache); free(m->v_cache); free(m->k_scale); free(m->v_scale);
    free(m->k_cache_f); free(m->v_cache_f);
    free(m->x); free(m->xb); free(m->xb2); free(m->q);
    free(m->att); free(m->hb); free(m->hb2); free(m->logits);
    free(m->xq); free(m->xs);
    free(m->xsum);
    memset(m, 0, sizeof(*m));
}

#if defined(__AVX2__)
  #include <immintrin.h>
  /* Dot of two float arrays, eight lanes at a time.
   *
   * The router was 9.0% of a token on Qwen3-30B: 48 MB of float weights read per token, at 3.3 GB/s,
   * in a scalar loop. It is the one float matrix left in the decode path.
   *
   * This sums in a DIFFERENT ORDER from the scalar version, because eight partial sums are combined
   * at the end. Float addition is not associative, so the two can differ in the last bits. That
   * matters here and nowhere else in this file: the router's output is only used to pick the top
   * eight of 128, and two experts whose probabilities agree to seven digits could swap places. The
   * check is the reference comparison in scratchpad/firsttok, which still agrees on seven of eight
   * prompts after this change. */
  static float fdot_avx2(const float *a, const float *b, int n)
  {
      __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
      int i = 0;
      for (; i + 16 <= n; i += 16) {
          s0 = _mm256_add_ps(s0, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
          s1 = _mm256_add_ps(s1, _mm256_mul_ps(_mm256_loadu_ps(a + i + 8),
                                               _mm256_loadu_ps(b + i + 8)));
      }
      __m256 s = _mm256_add_ps(s0, s1);
      __m128 v = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
      v = _mm_add_ps(v, _mm_movehl_ps(v, v));
      v = _mm_add_ss(v, _mm_shuffle_ps(v, v, 1));
      float acc = _mm_cvtss_f32(v);
      for (; i < n; i++) acc += a[i] * b[i];
      return acc;
  }
#endif

/* ---------------------------------------------------------------------------------------------
 *  coarse per-section timers
 * ------------------------------------------------------------------------------------------ */

static const char *const PROF_NAMES[MODEL_PROF_N] = {
    "norm", "q k v projections", "attention over the cache", "attention output",
    "router", "experts or dense feed-forward", "output head",
};
static double g_prof[MODEL_PROF_N];

const char *const *model_prof_names(void) { return PROF_NAMES; }
const double      *model_prof_seconds(void) { return g_prof; }
void               model_prof_reset(void) { memset(g_prof, 0, sizeof(g_prof)); }

#if defined(_WIN32)
  #include <windows.h>
  static double prof_now(void)
  {
      LARGE_INTEGER f, t;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t);
      return (double)t.QuadPart / (double)f.QuadPart;
  }
#else
  #include <time.h>
  static double prof_now(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
#endif

/* Deliberately a macro pair rather than a scoped timer: the sections do not nest and a single
 * accumulator variable per call site is cheaper than anything cleverer. */
#define PROF_T0()      const double _t0 = prof_now()
#define PROF_ADD(slot) g_prof[slot] += prof_now() - _t0

/* ---------------------------------------------------------------------------------------------
 *  kernels
 * ------------------------------------------------------------------------------------------ */

/* out[rows] = W[rows][cols] . x[cols], unpacking one row at a time.
 *
 * The row is dequantized into stack, used once and discarded. That is deliberately the same traffic
 * pattern the FPGA core implements and the same one bench_stream measures: read each weight exactly
 * once, never revisit it, keep the vector in fast storage. It is also why this is memory bound rather
 * than compute bound, by construction rather than by accident. */
void model_matvec_rows(model_t *m, const qten_t *w, const float *x, float *out, int r0, int n)
{
    if (r0 >= n) return;
    if (m->fast) {
        /* Quantize the activation ONCE for the whole matrix, then every row is an integer dot. The
         * activation is 2048 values against millions of weights, so this cost disappears; it is the
         * same asymmetry that puts activations in fabric and streams weights past them. */
        gguf_quantize_act(x, w->cols, m->xq, m->xs);

        /* and the per-32 sums, once for the whole matrix rather than once per row. Only Q4_K uses
         * them -- it is the format with a per-block minimum -- and the result is bit-identical, so
         * this is a scheduling change exactly like matvec_group below and not a numerics one. */
        if (w->type == GGML_Q4_K && m->xsum) {
            gguf_act_sums(m->xq, w->cols, m->xsum);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
            for (int r = r0; r < n; r++)
                out[r] = gguf_dot_q4k_presum(w->raw + (size_t)r * w->row_bytes,
                                             m->xq, m->xs, m->xsum, w->cols);
            return;
        }
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (int r = r0; r < n; r++)
            out[r] = gguf_dot_q(w->type, w->raw + (size_t)r * w->row_bytes, m->xq, m->xs, w->cols);
        return;
    }

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int r = r0; r < n; r++) {
        float row[MAX_COLS];
        gguf_dequant(w->type, w->raw + (size_t)r * w->row_bytes, w->cols, row);
        float s = 0.0f;
        for (uint32_t c = 0; c < w->cols; c++) s += row[c] * x[c];
        out[r] = s;
    }
}

static void matvec(model_t *m, const qten_t *w, const float *x, float *out, int rows)
{
    const int n = rows ? rows : (int)w->rows;
#ifdef ZACCEL_OFFLOAD
    if (m->zo && model_zaccel_matvec(m, w, x, out, n)) return;
#endif
    model_matvec_rows(m, w, x, out, 0, n);
}

/* SEVERAL MATRICES THAT SHARE ONE INPUT, IN ONE PARALLEL REGION.
 *
 * q, k and v all multiply the same normalised activation. Run as three matvecs that is three thread
 * fork-and-joins and three quantizations of the same vector, and two of the three regions are tiny:
 * on Qwen3-30B k and v are 512 rows each against q's 4096. Measured, q/k/v was 24.5% of a token.
 *
 * Every output element is an independent row dot, so flattening the three into one loop changes no
 * arithmetic at all. It is purely a scheduling change, and the result is bit-identical. */
static void matvec_group(model_t *m, const qten_t *const *w, float *const *out, const int *rows,
                         int nmat, const float *x)
{
    if (!m->fast || m->zo) {    /* with the Zynq taking rows, each matrix goes through matvec */
        for (int i = 0; i < nmat; i++) matvec(m, w[i], x, out[i], rows[i]);
        return;
    }

    /* One quantization of the shared input, not nmat of them. */
    gguf_quantize_act(x, w[0]->cols, m->xq, m->xs);

    int total = 0;
    for (int i = 0; i < nmat; i++) total += rows[i];

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int t = 0; t < total; t++) {
        int i = 0, r = t;
        while (r >= rows[i]) { r -= rows[i]; i++; }
        out[i][r] = gguf_dot_q(w[i]->type, w[i]->raw + (size_t)r * w[i]->row_bytes,
                               m->xq, m->xs, w[i]->cols);
    }
}

void model_set_fast(model_t *m, int on) { m->fast = on ? 1 : 0; }

int model_set_streams(model_t *m, int n)
{
    if (n < 1) n = 1;
    const size_t per = (size_t)m->n_layer * (size_t)m->max_seq * (size_t)m->n_kv * (size_t)m->head_dim;
    const size_t sper = (size_t)m->n_layer * (size_t)m->max_seq * (size_t)m->n_kv;
    int8_t *k = (int8_t *)calloc(per * (size_t)n, 1);
    int8_t *v = (int8_t *)calloc(per * (size_t)n, 1);
    float  *ks = (float *)calloc(sper * (size_t)n, sizeof(float));
    float  *vs = (float *)calloc(sper * (size_t)n, sizeof(float));
    if (!k || !v || !ks || !vs) { free(k); free(v); free(ks); free(vs); return -1; }
    free(m->k_cache); free(m->v_cache); free(m->k_scale); free(m->v_scale);
    m->k_cache = k;
    m->v_cache = v;
    m->k_scale = ks;
    m->v_scale = vs;
    m->n_streams = n;
    return 0;
}

int model_set_kv_f32(model_t *m, int on)
{
    m->kv_f32 = on ? 1 : 0;
    free(m->k_cache_f);
    free(m->v_cache_f);
    m->k_cache_f = NULL;
    m->v_cache_f = NULL;
    if (!on) return 0;
    const size_t per = (size_t)m->n_streams * (size_t)m->n_layer * (size_t)m->max_seq
                       * (size_t)m->n_kv * (size_t)m->head_dim;
    m->k_cache_f = (float *)calloc(per, sizeof(float));
    m->v_cache_f = (float *)calloc(per, sizeof(float));
    if (!m->k_cache_f || !m->v_cache_f) { m->kv_f32 = 0; return -1; }
    return 0;
}

/* Store one head's worth of key or value at int8, keeping the scale that reconstructs it.
 *
 * Symmetric around zero and scaled by the largest magnitude in the head, so no value clips and the
 * quantization step is as small as the head allows. A head of exact zeroes gets scale zero, which
 * reconstructs to zero and needs no special case. */
void model_set_kv_int4(model_t *m, int on) { m->kv_int4 = on ? 1 : 0; }

/* Levels available either side of zero. 127 for 8-bit, 7 for 4-bit. The scale is set from the head's
 * largest magnitude either way, so nothing clips; 4 bits simply steps more coarsely. */
static int g_kv_levels = 127;

static void kv_store(const float *src, int8_t *dst, float *scale, int n)
{
    const float lv = (float)g_kv_levels;
    float amax = 0.0f;
    for (int i = 0; i < n; i++) {
        const float a = fabsf(src[i]);
        if (a > amax) amax = a;
    }
    const float s = amax / lv;
    *scale = s;
    const float inv = (s > 0.0f) ? 1.0f / s : 0.0f;
    for (int i = 0; i < n; i++) {
        float v = src[i] * inv;
        /* Round away from zero rather than truncating: truncation biases every cached value toward
         * zero, and across 36 layers and thousands of positions that is a systematic shrink of the
         * attention scores rather than noise that cancels. */
        v = (v < 0.0f) ? v - 0.5f : v + 0.5f;
        int q = (int)v;
        if (q > g_kv_levels) q = g_kv_levels;
        if (q < -g_kv_levels) q = -g_kv_levels;
        dst[i] = (int8_t)q;
    }
}

static void rmsnorm(float *out, const float *x, const float *g, int n, float eps)
{
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    /* Epsilon goes INSIDE the square root, after the mean. Putting it outside, or before the divide,
     * changes the result by more than it looks like it should on the first layer and compounds. */
    const float inv = 1.0f / sqrtf(ss / (float)n + eps);
    for (int i = 0; i < n; i++) out[i] = x[i] * inv * g[i];
}

static void softmax(float *v, int n)
{
    float mx = v[0];
    for (int i = 1; i < n; i++) if (v[i] > mx) mx = v[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { v[i] = expf(v[i] - mx); sum += v[i]; }
    const float inv = 1.0f / sum;
    for (int i = 0; i < n; i++) v[i] *= inv;
}

/* Rotary position embedding, NeoX pairing: element i pairs with element i + head_dim/2, not with
 * i+1.
 *
 * Both conventions exist and both produce plausible-looking output, so this is one of the few places
 * where a wrong choice is silent. Qwen2 and Llama as converted to GGUF use this one. The symptom of
 * the other is text that starts coherently and loses the thread after a dozen tokens, because the
 * positional information is present but scrambled. */
static void rope(float *v, int n_heads, int head_dim, int pos, float base)
{
    const int half = head_dim / 2;
    for (int h = 0; h < n_heads; h++) {
        float *p = v + (size_t)h * head_dim;
        for (int i = 0; i < half; i++) {
            const float freq = powf(base, -2.0f * (float)i / (float)head_dim);
            const float th = (float)pos * freq;
            const float c = cosf(th), s = sinf(th);
            const float x0 = p[i], x1 = p[i + half];
            p[i]        = x0 * c - x1 * s;
            p[i + half] = x0 * s + x1 * c;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 *  forward
 * ------------------------------------------------------------------------------------------ */

/* Input embedding: ONE row of the table, which is the lookup the planner cared about. 4 KB read
 * against 243 MB held, and the reason this would have been fine on an SD card if the same tensor were
 * not also the output head. */
void model_embed(model_t *m, int32_t token)
{
    if (m->embd.raw) {                     /* whole table in RAM, the single-process case */
        gguf_dequant(m->embd.type, m->embd.raw + (size_t)token * m->embd.row_bytes,
                     m->embd.cols, m->x);
        return;
    }
    if (m->src && m->embd_t) {             /* one row off the file, which is what a head does */
        if (gguf_read_raw_rows(m->src, m->embd_t, (uint64_t)token, 1, m->rowbuf) == 0)
            gguf_dequant(m->embd_t->type, m->rowbuf, m->embd_t->dims[0], m->x);
        return;
    }
    /* A node with neither cannot embed, and silently leaving stale data in x would look like the
     * model drifting. Zero is wrong too, but it is obviously wrong. */
    memset(m->x, 0, (size_t)m->dim * sizeof(float));
}

void model_norm_final(model_t *m, float *out)
{
    rmsnorm(out, m->x, m->out_norm, m->dim, m->eps);
}

int32_t model_project_slice(model_t *m, const float *xn, float *best_logit)
{
    if (m->v_hi <= m->v_lo) return -1;
    const int n = m->v_hi - m->v_lo;
    matvec(m, &m->out_head, xn, m->logits, n);
    int best = 0;
    for (int i = 1; i < n; i++) if (m->logits[i] > m->logits[best]) best = i;
    if (best_logit) *best_logit = m->logits[best];
    return (int32_t)(m->v_lo + best);
}

/* Final norm and the output projection. Separated from the layers because on a pipeline this runs on
 * the LAST node only, and because the projection is the one tensor that splits by vocabulary. */
const float *model_head(model_t *m)
{
    PROF_T0();
    if (!m->has_head) return NULL;
    rmsnorm(m->xb, m->x, m->out_norm, m->dim, m->eps);
    matvec(m, &m->out_head, m->xb, m->logits, m->v_hi - m->v_lo);
    PROF_ADD(6);
    return m->logits;
}

/* Every layer this node holds, in place on m->x.
 *
 * `pos` is the position in the SEQUENCE, not in this node's slice, because RoPE and the KV cache are
 * both indexed by it and both must agree with every other node. The layer loop is local: index i here
 * is global layer layer_lo + i, and nothing in the arithmetic needs to know that. */
const uint32_t *model_expert_hits(const model_t *m) { return m->expert_hits; }



/* Per-head RMSNorm on q or k, which Qwen3 applies before the rotation.
 *
 * Note that it normalises WITHIN a head, not across the whole vector: 32 independent norms of 128
 * values, not one norm of 4096. Doing it across the whole vector runs, produces text, and is a
 * different model. */
static void head_norm(float *v, int heads, int hd, const float *gain, float eps)
{
    if (!gain) return;
    for (int h = 0; h < heads; h++) {
        float *p = v + (size_t)h * hd;
        float ss = 0.0f;
        for (int i = 0; i < hd; i++) ss += p[i] * p[i];
        const float inv = 1.0f / sqrtf(ss / (float)hd + eps);
        for (int i = 0; i < hd; i++) p[i] = p[i] * inv * gain[i];
    }
}

/* One expert's feed-forward, accumulated into `acc` with weight `w`.
 *
 * The reference path. Correct, three parallel regions per expert, and slow for that reason. Kept
 * because the fused version below has to be checked against something. */
static void expert_ffn(model_t *m, const mlayer_t *L, int e, const float *xin, float w, float *acc)
{
    const int ff = m->expert_ff, dim = m->dim;

    qten_t g = L->e_gate, u = L->e_up, d = L->e_down;
    g.rows = (uint32_t)ff;  g.raw = L->e_gate.raw + (size_t)e * ff  * L->e_gate.row_bytes;
    u.rows = (uint32_t)ff;  u.raw = L->e_up.raw   + (size_t)e * ff  * L->e_up.row_bytes;
    d.rows = (uint32_t)dim; d.raw = L->e_down.raw + (size_t)e * dim * L->e_down.row_bytes;

    matvec(m, &g, xin, m->hb,  ff);
    matvec(m, &u, xin, m->hb2, ff);
    for (int i = 0; i < ff; i++) {
        const float gg = m->hb[i];
        m->hb[i] = (gg / (1.0f + expf(-gg))) * m->hb2[i];
    }
    matvec(m, &d, m->hb, m->xb2, dim);
    for (int i = 0; i < dim; i++) acc[i] += w * m->xb2[i];
    m->expert_reads++;
}

/* EVERY FIRING EXPERT, IN THREE PARALLEL REGIONS INSTEAD OF TWENTY-FOUR.
 *
 * The arithmetic is identical to calling expert_ffn k times; only the thread scheduling changes.
 * All k experts read the same input for gate and up, so that activation is quantized once rather
 * than k times, and the k*ff rows of gate and up are one flat loop the threads divide between
 * them. The down pass is a second flat loop over k*dim rows.
 *
 * Only for the fused integer path. The float reference path stays one expert at a time, because it
 * exists to be obviously right rather than quick. */
static void experts_fused(model_t *m, const mlayer_t *L, const int *pick, const float *wgt,
                          int k, const float *xin, float *acc)
{
    const int ff = m->expert_ff, dim = m->dim;
    const uint64_t gb = L->e_gate.row_bytes, ub = L->e_up.row_bytes, db = L->e_down.row_bytes;
    const int nsb = ff / 32;                  /* activation scale blocks in one expert's hidden */

    gguf_quantize_act(xin, (uint64_t)dim, m->xq, m->xs);

    /* gate and up together: 2*k*ff rows, one region. */
    const int nrow = 2 * k * ff;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int t = 0; t < nrow; t++) {
        const int up = t >= k * ff;
        const int idx = up ? t - k * ff : t;
        const int j = idx / ff, r = idx % ff;
        const uint8_t *base = up ? L->e_up.raw : L->e_gate.raw;
        const uint64_t rb = up ? ub : gb;
        const uint32_t ty = up ? L->e_up.type : L->e_gate.type;
        const uint32_t cols = up ? L->e_up.cols : L->e_gate.cols;
        const float v = gguf_dot_q(ty, base + ((size_t)pick[j] * ff + r) * rb,
                                   m->xq, m->xs, cols);
        (up ? m->hb2 : m->hb)[(size_t)j * ff + r] = v;
    }

    /* SwiGLU and the per-expert quantization of what comes out of it. Serial and cheap: k*ff is
     * 6144 elements against the millions of weight bytes on either side of it. */
    for (int j = 0; j < k; j++) {
        float *hh = m->hb + (size_t)j * ff;
        const float *uu = m->hb2 + (size_t)j * ff;
        for (int i = 0; i < ff; i++) {
            const float gg = hh[i];
            hh[i] = (gg / (1.0f + expf(-gg))) * uu[i];
        }
        gguf_quantize_act(hh, (uint64_t)ff, m->e_xq + (size_t)j * ff, m->e_xs + (size_t)j * nsb);
    }

    /* down: k*dim rows, one region. */
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int t = 0; t < k * dim; t++) {
        const int j = t / dim, r = t % dim;
        m->e_out[(size_t)j * dim + r] =
            gguf_dot_q(L->e_down.type,
                       L->e_down.raw + ((size_t)pick[j] * dim + r) * db,
                       m->e_xq + (size_t)j * ff, m->e_xs + (size_t)j * nsb, L->e_down.cols);
    }

    for (int j = 0; j < k; j++) {
        const float *o = m->e_out + (size_t)j * dim;
        const float w = wgt[j];
        for (int i = 0; i < dim; i++) acc[i] += w * o[i];
        m->expert_reads++;
    }
}

/* Route one token: softmax over every expert, keep the top n_expert_used, renormalise those to sum
 * to one. That last step is what Qwen3 calls norm_topk_prob and it is not optional -- without it
 * the residual is scaled by however much probability mass the chosen experts happened to hold. */
static void route(model_t *m, const mlayer_t *L, const float *xin, int *pick, float *wgt)
{
    const int ne = m->n_expert, k = m->n_expert_used, dim = m->dim;
    float *p = m->router_p;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int e = 0; e < ne; e++) {
        const float *row = L->router + (size_t)e * dim;
#if defined(__AVX2__)
        p[e] = fdot_avx2(row, xin, dim);
#else
        float acc = 0.0f;
        for (int i = 0; i < dim; i++) acc += row[i] * xin[i];
        p[e] = acc;
#endif
    }
    softmax(p, ne);

    for (int j = 0; j < k; j++) {
        int best = -1;
        float bv = -1e30f;
        for (int e = 0; e < ne; e++) {
            int taken = 0;
            for (int q = 0; q < j; q++) if (pick[q] == e) { taken = 1; break; }
            if (!taken && p[e] > bv) { bv = p[e]; best = e; }
        }
        pick[j] = best;
        wgt[j] = bv;
    }
    float sum = 0.0f;
    for (int j = 0; j < k; j++) sum += wgt[j];
    if (sum > 0.0f) for (int j = 0; j < k; j++) wgt[j] /= sum;
}

void model_layers(model_t *m, int pos, int stream)
{
    g_kv_levels = m->kv_int4 ? 7 : 127;
    const int dim = m->dim, hd = m->head_dim, nkv = m->n_kv, nh = m->n_heads;
    const int kv_dim = nkv * hd;
    if (stream < 0 || stream >= m->n_streams) stream = 0;
    /* Base offset of this stream's whole cache. Everything below indexes from here, so a stream is a
     * contiguous block and switching streams costs an add. */
    const size_t sbase = (size_t)stream * (size_t)m->n_layer * (size_t)m->max_seq * (size_t)kv_dim;
    const size_t sscale = (size_t)stream * (size_t)m->n_layer * (size_t)m->max_seq * (size_t)nkv;
    const int group = nh / nkv;               /* query heads sharing one kv head */

    float *kbuf = (float *)malloc((size_t)kv_dim * sizeof(float));
    float *vbuf = (float *)malloc((size_t)kv_dim * sizeof(float));
    /* Two more only when there are experts: the shared input every expert reads, and the weighted
     * sum of what they produce. Both are dim floats and both are zero-cost on a dense model. */
    float *ffn_in  = m->n_expert > 0 ? (float *)malloc((size_t)dim * sizeof(float)) : NULL;
    float *ffn_acc = m->n_expert > 0 ? (float *)malloc((size_t)dim * sizeof(float)) : NULL;
    if (!kbuf || !vbuf || (m->n_expert > 0 && (!ffn_in || !ffn_acc))) {
        free(kbuf); free(vbuf); free(ffn_in); free(ffn_acc); return;
    }

    for (int l = 0; l < m->n_layer; l++) {
        const mlayer_t *L = &m->L[l];

        { PROF_T0(); rmsnorm(m->xb, m->x, L->attn_norm, dim, m->eps); PROF_ADD(0); }

        { PROF_T0();
        const qten_t *qkvw[3] = { &L->wq, &L->wk, &L->wv };
        float *qkvo[3] = { m->q, kbuf, vbuf };
        const int qkvr[3] = { m->q_dim, kv_dim, kv_dim };
        matvec_group(m, qkvw, qkvo, qkvr, 3, m->xb);
        PROF_ADD(1); }
        { PROF_T0();
        if (L->bq) for (int i = 0; i < m->q_dim; i++) m->q[i] += L->bq[i];
        if (L->bk) for (int i = 0; i < kv_dim; i++)   kbuf[i] += L->bk[i];
        if (L->bv) for (int i = 0; i < kv_dim; i++)   vbuf[i] += L->bv[i];

        /* Qwen3 normalises each head before the rotation. On a model without these it is a no-op. */
        head_norm(m->q, nh,  hd, L->q_norm, m->eps);
        head_norm(kbuf, nkv, hd, L->k_norm, m->eps);

        /* Position goes on q and k only. v carries no position, which is why the cache can be
         * written once and reread unchanged for the rest of the sequence. */
        rope(m->q, nh, hd, pos, m->rope_base);
        rope(kbuf, nkv, hd, pos, m->rope_base);

        const size_t base = sbase + ((size_t)l * m->max_seq + (size_t)pos) * kv_dim;
        const size_t sb = sscale + ((size_t)l * m->max_seq + (size_t)pos) * nkv;
        for (int h = 0; h < nkv; h++) {
            kv_store(kbuf + (size_t)h * hd, m->k_cache + base + (size_t)h * hd, &m->k_scale[sb + h], hd);
            kv_store(vbuf + (size_t)h * hd, m->v_cache + base + (size_t)h * hd, &m->v_scale[sb + h], hd);
        }
        if (m->kv_f32) {
            memcpy(m->k_cache_f + base, kbuf, (size_t)kv_dim * sizeof(float));
            memcpy(m->v_cache_f + base, vbuf, (size_t)kv_dim * sizeof(float));
        }

        PROF_ADD(1); }

        PROF_T0();
        const float scale = 1.0f / sqrtf((float)hd);
        for (int h = 0; h < nh; h++) {
            const float *qh = m->q + (size_t)h * hd;
            const int kvh = h / group;
            for (int t = 0; t <= pos; t++) {
                const size_t off = ((size_t)l * m->max_seq + (size_t)t);
                if (m->kv_f32) {
                    const float *kf = m->k_cache_f + sbase + off * kv_dim + (size_t)kvh * hd;
                    float s = 0.0f;
                    for (int i = 0; i < hd; i++) s += qh[i] * kf[i];
                    m->att[t] = s * scale;
                    continue;
                }
                const int8_t *kt = m->k_cache + sbase + off * kv_dim + (size_t)kvh * hd;
                float s = 0.0f;
                for (int i = 0; i < hd; i++) s += qh[i] * (float)kt[i];
                /* One float multiply per head instead of one per element: the scale factors out of
                 * the whole dot product. */
                m->att[t] = s * m->k_scale[sscale + off * nkv + kvh] * scale;
            }
            softmax(m->att, pos + 1);
            float *oh = m->xb + (size_t)h * hd;
            for (int i = 0; i < hd; i++) oh[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                const size_t off = ((size_t)l * m->max_seq + (size_t)t);
                if (m->kv_f32) {
                    const float *vf = m->v_cache_f + sbase + off * kv_dim + (size_t)kvh * hd;
                    const float a = m->att[t];
                    for (int i = 0; i < hd; i++) oh[i] += a * vf[i];
                    continue;
                }
                const int8_t *vt = m->v_cache + sbase + off * kv_dim + (size_t)kvh * hd;
                const float a = m->att[t] * m->v_scale[sscale + off * nkv + kvh];
                for (int i = 0; i < hd; i++) oh[i] += a * (float)vt[i];
            }
        }

        PROF_ADD(2);

        { PROF_T0();
        matvec(m, &L->wo, m->xb, m->xb2, dim);
        for (int i = 0; i < dim; i++) m->x[i] += m->xb2[i];
        PROF_ADD(3); }

        { PROF_T0(); rmsnorm(m->xb, m->x, L->ffn_norm, dim, m->eps); PROF_ADD(0); }

        if (m->n_expert > 0) {
            /* THE MIXTURE-OF-EXPERTS FEED-FORWARD.
             *
             * This is the only part of the forward pass that reads a data-dependent slice of the
             * weights. Everything else -- attention, the norms, the head -- touches the same bytes
             * on every token. Eight experts of 128 fire here, which is why a 30B model reads 2.03 GB
             * a token instead of 18.4, and why it is worth arguing about at all on this machine. */
            int pick[64];
            float wgt[64];
            const int k = m->n_expert_used < 64 ? m->n_expert_used : 64;
            { PROF_T0(); route(m, L, m->xb, pick, wgt); PROF_ADD(4); }

            /* xin has to be a copy: expert_ffn writes m->xb2 and m->hb, and route left the
             * probabilities in m->att, but m->xb itself is the input to every one of the eight. */
            float *xin = ffn_in;
            memcpy(xin, m->xb, (size_t)dim * sizeof(float));

            PROF_T0();
            for (int i = 0; i < dim; i++) ffn_acc[i] = 0.0f;
            for (int j = 0; j < k; j++)
                if (pick[j] >= 0 && m->expert_hits)
                    m->expert_hits[(size_t)l * m->n_expert + pick[j]]++;
            if (m->fast) {
                experts_fused(m, L, pick, wgt, k, xin, ffn_acc);
            } else {
                for (int j = 0; j < k; j++) {
                    if (pick[j] < 0) continue;
                    expert_ffn(m, L, pick[j], xin, wgt[j], ffn_acc);
                }
            }
            for (int i = 0; i < dim; i++) m->x[i] += ffn_acc[i];
            PROF_ADD(5);
        } else {
            PROF_T0();
            matvec(m, &L->w_gate, m->xb, m->hb, m->hidden);
            matvec(m, &L->w_up,   m->xb, m->hb2, m->hidden);
            /* SwiGLU: silu on the gate, elementwise product with up. */
            for (int i = 0; i < m->hidden; i++) {
                const float g = m->hb[i];
                m->hb[i] = (g / (1.0f + expf(-g))) * m->hb2[i];
            }
            matvec(m, &L->w_down, m->hb, m->xb2, dim);
            for (int i = 0; i < dim; i++) m->x[i] += m->xb2[i];
            PROF_ADD(5);
        }
    }

    free(kbuf);
    free(vbuf);
    free(ffn_in);
    free(ffn_acc);
}


/* ---------------------------------------------------------------------------------------------
 *  prefill: the whole prompt at once
 * ------------------------------------------------------------------------------------------ */

/* How many positions are processed together. Every extra position costs dim floats of scratch in
 * several buffers, so this is the knob that trades memory for weight reuse. 64 positions on this model
 * is about 3 MB of scratch and already gets 64x the reuse, which is most of what is available: past
 * that the arithmetic dominates and more batching buys little. A Teensy would set this to 4. */
#define PREFILL_CHUNK 64

/* out[j][rows] = W[rows][cols] . X[j][cols], for j in 0..n-1.
 *
 * THE ROW IS UNPACKED ONCE AND USED n TIMES. That single fact is the whole difference between prefill
 * and decode. At n = 1 this is matvec and is bound by unpacking; at n = 64 the unpacking is amortised
 * 64-fold and what is left is arithmetic. */
void model_matmul_rows(const qten_t *w, const float *X, int n, float *out, int r0, int r1)
{
    const int rows = (int)w->rows;
    const uint32_t cols = w->cols;
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int r = r0; r < r1; r++) {
        float row[MAX_COLS];
        gguf_dequant(w->type, w->raw + (size_t)r * w->row_bytes, cols, row);

        /* FOUR POSITIONS AT A TIME, and the reason is loads, not multiplies.
         *
         * The obvious loop does one position per pass, which reloads row[c] on every pass: one load
         * of the weight for one multiply. Measured that way, prefill managed 9 G MAC/s against the
         * 35 G MAC/s this host reaches on a float dot with data in cache, and a batched prefill came
         * out SLOWER than decoding one token at a time -- which is absurd, since it reads the weights
         * 45 times less often.
         *
         * Holding four accumulators means row[c] is loaded once and used four times, and four
         * independent sums also give the pipeline something to overlap instead of one serial
         * dependency chain. Eight-wide first, then four, then one, so any n is handled and the widest
         * block that fits is always used. Eight accumulators plus the broadcast weight is nine vector
         * registers of the sixteen AVX2 has, which leaves the compiler room to unroll. */
        int j = 0;
        for (; j + 8 <= n; j += 8) {
            const float *x0 = X + (size_t)(j + 0) * cols, *x1 = X + (size_t)(j + 1) * cols;
            const float *x2 = X + (size_t)(j + 2) * cols, *x3 = X + (size_t)(j + 3) * cols;
            const float *x4 = X + (size_t)(j + 4) * cols, *x5 = X + (size_t)(j + 5) * cols;
            const float *x6 = X + (size_t)(j + 6) * cols, *x7 = X + (size_t)(j + 7) * cols;
            float s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;
            for (uint32_t c = 0; c < cols; c++) {
                const float wv = row[c];
                s0 += wv * x0[c]; s1 += wv * x1[c]; s2 += wv * x2[c]; s3 += wv * x3[c];
                s4 += wv * x4[c]; s5 += wv * x5[c]; s6 += wv * x6[c]; s7 += wv * x7[c];
            }
            out[(size_t)(j + 0) * rows + r] = s0;  out[(size_t)(j + 1) * rows + r] = s1;
            out[(size_t)(j + 2) * rows + r] = s2;  out[(size_t)(j + 3) * rows + r] = s3;
            out[(size_t)(j + 4) * rows + r] = s4;  out[(size_t)(j + 5) * rows + r] = s5;
            out[(size_t)(j + 6) * rows + r] = s6;  out[(size_t)(j + 7) * rows + r] = s7;
        }
        for (; j + 4 <= n; j += 4) {
            const float *x0 = X + (size_t)(j + 0) * cols;
            const float *x1 = X + (size_t)(j + 1) * cols;
            const float *x2 = X + (size_t)(j + 2) * cols;
            const float *x3 = X + (size_t)(j + 3) * cols;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            for (uint32_t c = 0; c < cols; c++) {
                const float wv = row[c];
                s0 += wv * x0[c];
                s1 += wv * x1[c];
                s2 += wv * x2[c];
                s3 += wv * x3[c];
            }
            out[(size_t)(j + 0) * rows + r] = s0;
            out[(size_t)(j + 1) * rows + r] = s1;
            out[(size_t)(j + 2) * rows + r] = s2;
            out[(size_t)(j + 3) * rows + r] = s3;
        }
        for (; j < n; j++) {
            const float *x = X + (size_t)j * cols;
            float s = 0.0f;
            for (uint32_t c = 0; c < cols; c++) s += row[c] * x[c];
            out[(size_t)j * rows + r] = s;
        }
    }
}

static void matmul_rows(model_t *m, const qten_t *w, const float *X, int n, float *out)
{
#ifdef ZACCEL_OFFLOAD
    if (m->zo && model_zaccel_matmul(m, w, X, n, out)) return;
#else
    (void)m;
#endif
    model_matmul_rows(w, X, n, out, 0, (int)w->rows);
}

/* One chunk of positions through every layer this node holds. Scratch is allocated per call: against
 * the arithmetic in here, a handful of mallocs is free, and it keeps a stage that never prefills from
 * carrying the buffers. */
void model_layers_batch(model_t *m, float *X, int n, int pos0, int stream)
{
    g_kv_levels = m->kv_int4 ? 7 : 127;
    const int dim = m->dim, hd = m->head_dim, nkv = m->n_kv, nh = m->n_heads;
    const int kv_dim = nkv * hd, group = nh / nkv;
    if (stream < 0 || stream >= m->n_streams) stream = 0;
    const size_t sbase = (size_t)stream * (size_t)m->n_layer * (size_t)m->max_seq * (size_t)kv_dim;
    const size_t sscale = (size_t)stream * (size_t)m->n_layer * (size_t)m->max_seq * (size_t)nkv;

    float *XB = (float *)malloc((size_t)n * dim * sizeof(float));
    float *Q  = (float *)malloc((size_t)n * dim * sizeof(float));
    float *KB = (float *)malloc((size_t)n * kv_dim * sizeof(float));
    float *VB = (float *)malloc((size_t)n * kv_dim * sizeof(float));
    float *H1 = (float *)malloc((size_t)n * m->hidden * sizeof(float));
    float *H2 = (float *)malloc((size_t)n * m->hidden * sizeof(float));
    float *AT = (float *)malloc((size_t)m->max_seq * sizeof(float));
    if (!XB || !Q || !KB || !VB || !H1 || !H2 || !AT) {
        free(XB); free(Q); free(KB); free(VB); free(H1); free(H2); free(AT);
        /* One position at a time is slow but correct, which beats refusing to run. A node that cannot
         * hold the batch scratch can still do its job. */
        for (int j = 0; j < n; j++) {
            memcpy(m->x, X + (size_t)j * dim, (size_t)dim * sizeof(float));
            model_layers(m, pos0 + j, stream);
            memcpy(X + (size_t)j * dim, m->x, (size_t)dim * sizeof(float));
        }
        return;
    }

    for (int l = 0; l < m->n_layer; l++) {
        const mlayer_t *L = &m->L[l];

        for (int j = 0; j < n; j++)
            rmsnorm(XB + (size_t)j * dim, X + (size_t)j * dim, L->attn_norm, dim, m->eps);

        matmul_rows(m, &L->wq, XB, n, Q);
        matmul_rows(m, &L->wk, XB, n, KB);
        matmul_rows(m, &L->wv, XB, n, VB);

        for (int j = 0; j < n; j++) {
            float *qj = Q + (size_t)j * dim;
            float *kj = KB + (size_t)j * kv_dim;
            float *vj = VB + (size_t)j * kv_dim;
            if (L->bq) for (int i = 0; i < dim; i++)    qj[i] += L->bq[i];
            if (L->bk) for (int i = 0; i < kv_dim; i++) kj[i] += L->bk[i];
            if (L->bv) for (int i = 0; i < kv_dim; i++) vj[i] += L->bv[i];
            rope(qj, nh, hd, pos0 + j, m->rope_base);
            rope(kj, nkv, hd, pos0 + j, m->rope_base);

            /* Every key and value for the batch is written BEFORE any attention runs, so a position
             * later in the batch can attend to an earlier one. */
            const size_t off = sbase + ((size_t)l * m->max_seq + (size_t)(pos0 + j)) * kv_dim;
            const size_t sb = sscale + ((size_t)l * m->max_seq + (size_t)(pos0 + j)) * nkv;
            for (int h = 0; h < nkv; h++) {
                kv_store(kj + (size_t)h * hd, m->k_cache + off + (size_t)h * hd, &m->k_scale[sb + h], hd);
                kv_store(vj + (size_t)h * hd, m->v_cache + off + (size_t)h * hd, &m->v_scale[sb + h], hd);
            }
            if (m->kv_f32) {
                memcpy(m->k_cache_f + off, kj, (size_t)kv_dim * sizeof(float));
                memcpy(m->v_cache_f + off, vj, (size_t)kv_dim * sizeof(float));
            }
        }

        /* Attention per position, over everything at or before it. The causal mask is the loop bound:
         * position p attends to 0..p and never forward, which is what lets every cached key and value
         * be reused unchanged by every later token. */
        const float scale = 1.0f / sqrtf((float)hd);
        for (int j = 0; j < n; j++) {
            const int p = pos0 + j;
            for (int h = 0; h < nh; h++) {
                const float *qh = Q + (size_t)j * dim + (size_t)h * hd;
                const int kvh = h / group;
                for (int t = 0; t <= p; t++) {
                    const size_t o = ((size_t)l * m->max_seq + (size_t)t);
                    if (m->kv_f32) {
                        const float *kf = m->k_cache_f + sbase + o * kv_dim + (size_t)kvh * hd;
                        float sd = 0.0f;
                        for (int i = 0; i < hd; i++) sd += qh[i] * kf[i];
                        AT[t] = sd * scale;
                        continue;
                    }
                    const int8_t *kt = m->k_cache + sbase + o * kv_dim + (size_t)kvh * hd;
                    float sdot = 0.0f;
                    for (int i = 0; i < hd; i++) sdot += qh[i] * (float)kt[i];
                    AT[t] = sdot * m->k_scale[sscale + o * nkv + kvh] * scale;
                }
                softmax(AT, p + 1);
                float *oh = XB + (size_t)j * dim + (size_t)h * hd;
                for (int i = 0; i < hd; i++) oh[i] = 0.0f;
                for (int t = 0; t <= p; t++) {
                    const size_t o = ((size_t)l * m->max_seq + (size_t)t);
                    if (m->kv_f32) {
                        const float *vf = m->v_cache_f + sbase + o * kv_dim + (size_t)kvh * hd;
                        const float a = AT[t];
                        for (int i = 0; i < hd; i++) oh[i] += a * vf[i];
                        continue;
                    }
                    const int8_t *vt = m->v_cache + sbase + o * kv_dim + (size_t)kvh * hd;
                    const float a = AT[t] * m->v_scale[sscale + o * nkv + kvh];
                    for (int i = 0; i < hd; i++) oh[i] += a * (float)vt[i];
                }
            }
        }

        matmul_rows(m, &L->wo, XB, n, Q);            /* Q reused as scratch for the projection */
        for (int j = 0; j < n; j++)
            for (int i = 0; i < dim; i++)
                X[(size_t)j * dim + i] += Q[(size_t)j * dim + i];

        for (int j = 0; j < n; j++)
            rmsnorm(XB + (size_t)j * dim, X + (size_t)j * dim, L->ffn_norm, dim, m->eps);

        matmul_rows(m, &L->w_gate, XB, n, H1);
        matmul_rows(m, &L->w_up,   XB, n, H2);
        for (int j = 0; j < n; j++) {
            float *h1 = H1 + (size_t)j * m->hidden;
            const float *h2 = H2 + (size_t)j * m->hidden;
            for (int i = 0; i < m->hidden; i++) {
                const float gv = h1[i];
                h1[i] = (gv / (1.0f + expf(-gv))) * h2[i];
            }
        }
        matmul_rows(m, &L->w_down, H1, n, Q);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < dim; i++)
                X[(size_t)j * dim + i] += Q[(size_t)j * dim + i];
    }

    free(XB); free(Q); free(KB); free(VB); free(H1); free(H2); free(AT);
}

void model_prefill(model_t *m, const int32_t *tok, int n, int pos0, int stream)
{
    const int dim = m->dim;
    const int chunk = n < PREFILL_CHUNK ? n : PREFILL_CHUNK;

    /* A mixture-of-experts layer routes every position to a different eight experts, so there is no
     * single weight matrix to unpack once and reuse across the batch. Batching would mean sorting
     * positions by expert and running a ragged matmul per expert, which is worth writing later and
     * is not what makes the model correct now. One position at a time is right and slow. */
    if (m->n_expert > 0) {
        for (int j = 0; j < n; j++) { model_embed(m, tok[j]); model_layers(m, pos0 + j, stream); }
        return;
    }

    float *X = (float *)malloc((size_t)chunk * dim * sizeof(float));
    if (!X) {
        for (int j = 0; j < n; j++) { model_embed(m, tok[j]); model_layers(m, pos0 + j, stream); }
        return;
    }

    for (int base = 0; base < n; base += chunk) {
        const int nb = (n - base) < chunk ? (n - base) : chunk;
        for (int j = 0; j < nb; j++) {
            model_embed(m, tok[base + j]);
            memcpy(X + (size_t)j * dim, m->x, (size_t)dim * sizeof(float));
        }
        model_layers_batch(m, X, nb, pos0 + base, stream);
        /* The last position is what the output head needs. */
        memcpy(m->x, X + (size_t)(nb - 1) * dim, (size_t)dim * sizeof(float));
    }
    free(X);
}

/* The single-process case: all three pieces, same behaviour as before the split. */
const float *model_forward(model_t *m, int32_t token, int pos)
{
    model_embed(m, token);
    model_layers(m, pos, 0);
    return model_head(m);
}

int32_t model_argmax(const model_t *m)
{
    const int n = m->v_hi - m->v_lo;
    if (n <= 0) return -1;
    int32_t best = 0;
    float bv = m->logits[0];
    for (int i = 1; i < n; i++) if (m->logits[i] > bv) { bv = m->logits[i]; best = i; }
    /* The global id, not the offset in this slice. Returning the offset would work perfectly on a
     * single node and be wrong on every distributed one. */
    return (int32_t)(m->v_lo + best);
}

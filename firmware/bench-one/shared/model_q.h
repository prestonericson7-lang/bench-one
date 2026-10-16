/* ===========================================================================================
 *  model_q.h -- a whole transformer, weights kept quantized, arithmetic in float
 * ===========================================================================================
 *
 *  This is the reference runtime: real weights out of a real GGUF file, real tokens in and out, and
 *  no framework. It exists to be CORRECT, and to be the thing every faster version is checked
 *  against. Build the fast path first and every mistake looks like quantization noise.
 *
 *
 *  WEIGHTS STAY QUANTIZED IN MEMORY. THIS IS THE WHOLE POINT.
 *  ----------------------------------------------------------
 *  Dequantizing the model once at load time would be simpler and faster per token. It would also
 *  turn 1.8 GB into 12 GB, which is more memory than the entire machine being built has, and it
 *  would destroy the only property that makes the machine possible.
 *
 *  So a row is unpacked at the moment it is multiplied, into 44 KB of stack, and thrown away. The
 *  cost is real, and it has only ever been measured on a PC, which is not a node in this machine
 *  and whose vector units none of the boards have. The figure that decides the design is this
 *  kernel on the Zynq's own cores against the same fabric, and it is UNMEASURED. Every number
 *  previously quoted here -- 31x, 7x, 3.0x, 1.6x -- came off a desktop and none of them count.
 *
 *
 *  THE TIED EMBEDDING TABLE, WHICH CHANGED THE PLAN
 *  -------------------------------------------------
 *  This model has no output.weight. 434 tensors = 36 layers x 12 + token_embd + output_norm, and
 *  that is the whole file. The embedding table IS the output projection, transposed.
 *
 *  That one fact inverts an architectural decision. Input embedding is a lookup: one row, 4 KB, and
 *  an SD card serves it in a tenth of a millisecond, so the planner was putting the table on storage
 *  and buying back 243 MB of DDR3. But the same tensor used as the output head is a full matrix-vector
 *  product over all 151,936 rows -- 243 MB READ EVERY TOKEN. On an SSD at 35 MB/s that is seven
 *  seconds a token.
 *
 *  So token_embd has to live in fast memory after all, and the model no longer fits two Zynqs by
 *  granularity. The output head is splittable by vocabulary in a way a layer is not, which is the way
 *  out, and none of that was visible until the file was opened.
 * ===========================================================================================
 */

#ifndef MODEL_Q_H
#define MODEL_Q_H

#include "gguf.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A quantized matrix held in memory exactly as it is on disk. */
typedef struct {
    uint32_t type;
    uint32_t rows;        /* outputs */
    uint32_t cols;        /* inputs  */
    uint64_t row_bytes;
    uint8_t *raw;
    uint8_t  mapped;      /* raw points into the GGUF file's read-only map: never written, never freed */
} qten_t;

typedef struct {
    float *attn_norm, *ffn_norm;
    float *bq, *bk, *bv;            /* qwen2 carries biases on q, k and v; NULL if absent */

    /* Qwen3 normalises q and k per head before the rotation, with a gain of head_dim floats each.
     * Qwen2 does not. Leaving these out does not crash and does not look obviously wrong -- it
     * produces fluent text that is subtly the wrong model, which is the worst kind of bug to have. */
    float *q_norm, *k_norm;

    qten_t wq, wk, wv, wo;
    qten_t w_gate, w_up, w_down;    /* the dense feed-forward; unused on a mixture-of-experts layer */

    /* MIXTURE OF EXPERTS.
     *
     * The router is F32 and tiny -- [n_expert][dim], 1 MB a layer -- and it decides which experts
     * see this token. It has to be resident: it is read on every token of every layer, and nothing
     * can be prefetched until it has run.
     *
     * The three expert tensors are one allocation each holding all n_expert matrices stacked, in
     * the order the file has them. Expert e's gate is rows [e*expert_ff, (e+1)*expert_ff) and its
     * down is rows [e*dim, (e+1)*dim). Kept stacked rather than split into n_expert qten_t so that
     * an expert is a contiguous run of bytes, which is what makes caching one of them a single
     * read rather than three. */
    float *router;
    qten_t e_gate, e_up, e_down;
} mlayer_t;

typedef struct {
    int   n_layer, dim, hidden, n_heads, n_kv, head_dim, vocab, max_seq;
    float eps, rope_base;

    /* q_dim is n_heads * head_dim and is NOT always dim.
     *
     * On every model here until now they were equal, so dim was used for both and nothing showed.
     * Qwen3-30B has dim 2048 and head_dim 128 across 32 heads, so the query projection is 2048 in
     * and 4096 out, and the attention output tensor is 4096 in and 2048 out. Using dim for the
     * query buffer reads half the heads and writes past the end of the other half. */
    int   q_dim;

    /* Zero on a dense model. n_expert_used of n_expert experts fire per token per layer, each with
     * its own feed-forward of width expert_ff. */
    int   n_expert, n_expert_used, expert_ff;

    /* Counts, filled in as the model runs, so the cache question can be answered with measurements
     * instead of an assumption about how skewed routing is. expert_hits is [layer][expert] over
     * this node's resident layers; expert_reads is how many expert matrices were fetched. */
    uint32_t *expert_hits;
    uint64_t  expert_reads;

    /* Router probabilities, n_expert wide.
     *
     * These were being written into m->att, which is max_seq long. With 128 experts and a run
     * whose context is shorter than 128 positions -- which is every short generation -- that
     * overruns the attention buffer by whatever the difference is, silently. */
    float    *router_p;

    /* Scratch for running all the firing experts in ONE parallel region instead of one per matrix.
     *
     * Measured: the experts took 63.6% of a token and moved 1120 MB in 128.8 ms, which is 8.7 GB/s
     * against a kernel ceiling of 20.75. The gap was not the kernel. Eight experts times three
     * matrices is twenty-four thread fork-and-joins per layer, and each region only had 110 KB of
     * work in it, so roughly half the time was spent starting and stopping threads. Three regions
     * per layer instead of twenty-four needs the intermediate results of every expert to exist at
     * once, which is what these hold. */
    float    *e_out;        /* [n_expert_used][dim]     each expert's contribution */
    int8_t   *e_xq;         /* [n_expert_used][expert_ff] quantized activations for the down pass */
    float    *e_xs;

    qten_t   embd;                  /* token_embd.weight, also the output head when tied */
    int      layer_lo;              /* global index of the first layer this node holds     */
    int      has_embd, has_head;    /* whether this node is the first or last stage        */

    /* THE VOCABULARY SLICE THIS NODE OWNS, [v_lo, v_hi).
     *
     * The output projection is the one weight matrix that splits freely. Every logit needs the whole
     * activation but only its own row, so node A can compute logits 0..37983 and node B 37984..75967
     * with no communication at all -- then each sends back its local best, tens of bytes, instead of
     * 594 KB of logits.
     *
     * A layer cannot do that. Splitting a layer means exchanging activations inside attention several
     * times per token, and the exchange costs more than the weights saved.
     *
     * This is not an optimisation. Qwen2.5-Coder 3B does NOT fit two 1 GB boards with the projection
     * whole: 243.4 MB plus 15 layers on one and 20 on the other leaves one layer homeless. Split two
     * ways it fits with room. */
    int      v_lo, v_hi;

    /* Where an input embedding row comes from when this node does not hold the table in RAM.
     *
     * The tied tensor is read two completely different ways. As the output projection it is a full
     * matmul over 151,936 rows and must be in fast memory. As the input embedding it is a LOOKUP of one
     * 4 KB row per token, which a disk serves in a tenth of a millisecond. So the projection is split
     * across RAM and the lookup is served from the file, and the head pays no memory for it at all. */
    gguf_t             *src;
    const gguf_tensor  *embd_t;
    uint8_t            *rowbuf;
    int      tied_output;
    qten_t   out_head;              /* output.weight when the file has its own             */
    float   *out_norm;
    mlayer_t *L;

    /* [stream][layer][pos][kv_head][head_dim].
     *
     * One cache per in-flight sequence. n_streams is 1 unless model_set_streams says otherwise, and
     * the memory is real: nine layers at 4096 positions is about 75 MB per stream on this model. That
     * is the price of keeping every node busy, and it is worth paying, because the alternative is
     * n-1 nodes idling on every token. */
    /* THE CACHE IS INT8, WITH A SCALE PER HEAD PER POSITION.
     *
     * It was float, which is four times what it needs and the largest easy saving left. On this model
     * one position of one layer costs 512 int8 values plus four scales -- 528 bytes against 2048 --
     * and the cache is not a rounding error: at 32k context with four conversations it is 2.3 GB
     * against 2.0 GB of weights. Bigger than the model.
     *
     * That matters twice over, because the two things this machine needs pull against each other. It
     * wants several requests in flight or n-1 nodes sit idle, and every extra request costs a whole
     * cache. Context and concurrency spend the same megabytes.
     *
     * The scale is per (layer, position, kv_head), which is 128 values on this model. Finer would cost
     * more in scales than it saves in error; coarser would let one outlier flatten a head. */
    int     n_streams;
    int8_t *k_cache, *v_cache;      /* [stream][layer][pos][kv_head][head_dim] */
    float  *k_scale, *v_scale;      /* [stream][layer][pos][kv_head]           */

    /* An optional float cache, used only to judge what the int8 one costs.
     *
     * int8 is standard practice and it was identical to float on the one matched test available, but
     * one test is an anecdote: greedy decoding flips whenever two logits are close, so "identical on a
     * prompt" and "harmless in general" are different claims. With both caches in one binary the
     * question can be asked properly on as many prompts as wanted, and the answer stops depending on
     * which build happened to be on disk. */
    int     kv_f32;
    float  *k_cache_f, *v_cache_f;

    /* Quantize the cache to 4 bits instead of 8, while still storing one value per byte.
     *
     * Deliberately measures the QUALITY of the approximation before anyone does the work of packing
     * two values into a byte. If 4-bit costs nothing, the packing is worth writing and conversation
     * capacity doubles again; if it costs something, the packing was never worth starting. Measuring
     * the cheap half of a change first is how not to waste a day on one that was never going to pay. */
    int     kv_int4;

    /* scratch */
    float *x, *xb, *xb2, *q, *att, *hb, *hb2, *logits;

    /* The fused integer path. Set model_set_fast(m, 1) to use gguf_dot_q instead of dequantizing
     * each row to floats. Measured 4.26x quicker on Q4_K and 2.11x on Q6_K, at the cost of holding
     * the activation at int8 with one scale per 32 elements. The float path stays in the build as the
     * reference the fast one is checked against. */
    int     fast;
    int8_t *xq;          /* quantized activation, widest of dim and hidden */
    float  *xs;          /* its per-32 scales                             */
    int32_t *xsum;       /* and its per-32 SUMS.
                          *
                          * Q4_K's per-block minimum needs the sum of the activations in each 32-element
                          * sub-block, and gguf_dot_q recomputes them inside every row. They do not
                          * depend on the weights, and a matvec dots one activation vector against
                          * thousands of rows, so computing them once here and passing them in saves
                          * two SMLAD per four weights on every row but the first.
                          *
                          * Measured on a Teensy 4.1: 60.68 -> 66.97 MB/s, and bit-identical, because the
                          * sums are the same integers either way. */

    uint64_t weight_bytes;          /* everything held in memory               */
    uint64_t bytes_per_token;       /* everything READ to produce one token    */

    /* The Zynq's matrix engine taking a share of every dense matrix's rows (accel/llm, ZACCEL_OFFLOAD
     * builds only). NULL = everything on this CPU, which is every other build. */
    void    *zo;
} model_t;

/* out[r0..r1) = W[r0..r1) . x on this CPU -- the same kernels matvec uses, over a row range, so an
 * accelerator can take the other rows. */
void model_matvec_rows(model_t *m, const qten_t *w, const float *x, float *out, int r0, int r1);
/* out[j][r] = W[r] . X[j] for rows [r0, r1) and positions j < n, the prefill kernel over a row range. */
void model_matmul_rows(const qten_t *w, const float *X, int n, float *out, int r0, int r1);

#ifdef ZACCEL_OFFLOAD
/* accel/llm/zaccel_offload.c: move a share of every resident dense matrix's rows (requantized to int8
 * with a scale per row) onto the Zynq's matrix engine and run them concurrently with the CPU rows.
 * share < 0 measures both sides and picks the split; the Zynq's free memory caps it. Returns the
 * number of weights offloaded, or -1 with the reason in msg. */
long long model_zaccel_attach(model_t *m, const char *host, double share, char *msg, int msglen);
void      model_zaccel_detach(model_t *m);
int       model_zaccel_matvec(model_t *m, const qten_t *w, const float *x, float *out, int n);
int       model_zaccel_matmul(model_t *m, const qten_t *w, const float *X, int n, float *out);
#endif

/* Load every tensor into memory. Returns 0, or -1 with the reason in g->err. */
int  model_load(model_t *m, gguf_t *g, int max_seq);
void model_free(model_t *m);

/* ---- the distributed form -------------------------------------------------------------------
 *
 * Load ONLY layers [layer0, layer1), and only the embedding table or output head if this node is
 * the one that needs them. Everything else in the file is never read, so a node holds what it
 * carries and nothing more.
 *
 * This is the whole architecture in one function signature. A node with 1 GB cannot hold a 2 GB
 * model, and does not have to: it holds eighteen layers of it. The activation that crosses between
 * nodes is one vector, measured at 8 KB for this model against 49.2 MB per layer of weights, which is
 * the asymmetry the machine is built on.
 *
 * want_embd  this node turns a token id into an activation, so it is the first stage
 * want_head  this node turns the final activation into logits, so it is the last stage
 *
 * On a tied-output model the embedding table IS the head, so asking for either loads the same
 * 243 MB. Asking for neither skips it entirely, which is what makes a middle stage cheap.
 */
int model_load_range(model_t *m, gguf_t *g, int max_seq, int layer0, int layer1,
                     int want_embd, int want_head);

/* The distributed form with the output projection split by vocabulary.
 *
 * v_lo/v_hi is the slice of the projection this node holds in RAM; pass v_lo == v_hi for a node that
 * holds none. embed_from_disk makes this node able to look up any input embedding row by reading the
 * file, which costs no memory and is what the head does.
 *
 * The gguf_t must outlive the model when embed_from_disk is set, because rows are read on demand. */
int model_load_slice(model_t *m, gguf_t *g, int max_seq, int layer0, int layer1,
                     int v_lo, int v_hi, int embed_from_disk);

/* Final RMSNorm only, written to `out` (dim floats). The head does this once and sends the result
 * round the ring, so every node projects exactly the same vector. */
void model_norm_final(model_t *m, float *out);

/* Project an already-normed activation against this node's vocabulary slice. Returns the GLOBAL token
 * id of the local best and writes its logit. Returns -1 if this node holds no slice. */
int32_t model_project_slice(model_t *m, const float *xn, float *best_logit);

/* Allocate KV caches for `n` independent sequences. Must be called before the first model_layers if
 * more than one stream is wanted. Returns 0, or -1 if the memory is not there.
 *
 * WHY THIS EXISTS. Measured: four nodes holding nine layers each produced 1.55 tok/s where one
 * process produced 4.25, and the network was 0.05% of it. The loss is not overhead, it is that a
 * single token in flight leaves three of four nodes waiting. With k independent sequences moving,
 * throughput goes from one-over-the-sum-of-stages to one-over-the-slowest-stage. */
int model_set_streams(model_t *m, int n);

/* ---- prefill, which is a different problem from decode ---------------------------------------
 *
 * Decode produces one token and must read every weight to do it, so it is bound by memory and
 * unpacking. Prefill processes a whole prompt, and every position needs the SAME weights, so a row can
 * be unpacked once and used n times. That turns matrix-vector into matrix-matrix and moves the
 * bottleneck from memory to arithmetic, which is the one place a GPU is the right tool.
 *
 * Running prefill one token at a time, as model_forward does, reads the entire 1.8 GB model once per
 * prompt token. A 500-token prompt would read 900 GB and take minutes. This reads it once.
 *
 * It deliberately uses the FLOAT path even when the fused integer path is enabled, because unpacking a
 * row once and reusing it n times beats unpacking it n times inside a fused kernel. The fast path wins
 * at n = 1 and loses here, which is worth knowing rather than assuming.
 *
 * pos0 is the sequence position of tok[0]. The KV cache is left filled for all n positions and m->x
 * holds the last one, so model_head can be called straight after.
 */
void model_prefill(model_t *m, const int32_t *tok, int n, int pos0, int stream);

/* The layer loop over a BATCH of positions, on a buffer the caller owns: X is [n][dim] and is updated
 * in place. This is what a pipeline stage runs during prefill -- it receives n activations at once,
 * pushes all of them through its layers, and passes them on, so every weight it holds is unpacked once
 * for the whole prompt instead of once per prompt token.
 *
 * pos0 is the sequence position of X[0]. The KV cache is filled for all n positions. */
void model_layers_batch(model_t *m, float *X, int n, int pos0, int stream);

/* The three pieces model_forward is made of, exposed so a pipeline stage can run just its part.
 * They operate on m->x, which is the activation this node received and will pass on. */
void         model_embed (model_t *m, int32_t token);   /* token id -> m->x                       */
void         model_layers(model_t *m, int pos, int stream);  /* resident layers, in place on m->x  */
const float *model_head  (model_t *m);                  /* m->x -> logits, final norm included     */

/* WHERE THE TIME GOES INSIDE ONE TOKEN.
 *
 * Vectorizing the kernels took the 3B from 4.25 to 8.23 tokens/s and the 30B only from 3.37 to 4.85,
 * and the 30B is 2.3x off its own unpacking ceiling where the 3B is 1.4x off. Guessing why is how a
 * week gets spent optimizing the wrong loop. These are coarse timers around the seven things a layer
 * does, accumulated across every token, and they cost one clock read each.
 *
 * model_prof_names returns the labels, model_prof_seconds the accumulated seconds, both of length
 * MODEL_PROF_N. model_prof_reset zeroes them. */
#define MODEL_PROF_N 7
const char *const *model_prof_names(void);
const double      *model_prof_seconds(void);
void               model_prof_reset(void);

/* Mixture-of-experts routing counters, for measuring how skewed expert use actually is.
 *
 * The whole 30B plan turns on one number nobody has measured: what fraction of expert fetches a
 * cache of the hottest experts would serve. That is not answerable from the architecture -- it
 * depends on what the router does with real text -- so the runtime counts it.
 *
 * Returns the [n_layer][n_expert] hit table, or NULL on a dense model. */
const uint32_t *model_expert_hits(const model_t *m);

/* Where this node sits in the model, for reporting. layer_lo is the global index of its first
 * layer; n_layer is how many it holds. */
int model_layer_lo(const model_t *m);

/* One token through the whole stack. `pos` must advance by one per call and is the position in the
 * sequence, which is what RoPE and the KV cache are indexed by. Returns the logits, which belong to
 * the model and are overwritten by the next call. */
const float *model_forward(model_t *m, int32_t token, int pos);

/* Keep a float KV cache alongside the int8 one and read from it instead. Costs four times the memory
 * and exists only so the int8 cache can be judged against something. Call before model_set_streams. */
int model_set_kv_f32(model_t *m, int on);

/* Quantize the KV cache to 4 bits (stored one per byte, so this measures quality, not memory). */
void model_set_kv_int4(model_t *m, int on);

/* Switch between the float reference and the fused integer path at runtime, so the same process can
 * run both and compare. */
void model_set_fast(model_t *m, int on);

/* Greedy pick, which is temperature zero. Deliberately the only sampler here: a reference runtime
 * has to be reproducible, and argmax is the one choice with no parameters to get wrong. */
int32_t model_argmax(const model_t *m);

#ifdef __cplusplus
}
#endif
#endif /* MODEL_Q_H */

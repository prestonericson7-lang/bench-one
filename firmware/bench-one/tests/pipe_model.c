/* ===========================================================================================
 *  pipe_model.c -- a real model split across machines, with enough work in flight to fill it
 * ===========================================================================================
 *
 *  The first version of this file proved the hard part: Qwen2.5-Coder 3B running across four
 *  processes, nine layers each, none of them holding the model, producing text identical to the
 *  single-process runtime. The network cost 0.3 ms of a 645 ms token, which is 0.05%.
 *
 *  It also produced 1.55 tokens a second where one process produces 4.25, and that is the interesting
 *  part. The loss is not overhead. With a single token in flight only ONE node is ever working, and
 *  the other three wait, because token t+1 cannot begin until token t exists. Utilisation is 1/n and
 *  no amount of hardware fixes it.
 *
 *  So this version keeps k independent sequences moving at once. Standard pipeline arithmetic:
 *
 *      k = 1     throughput = 1 / (sum of every stage)     -> 1.55 tok/s measured
 *      k >= n    throughput = 1 / (slowest stage alone)    -> the whole point
 *
 *  A single conversation cannot supply that, since it is autoregressive by construction. Several
 *  conversations can, and so can one user with several prompts, and so can any batch job. That makes
 *  this a multi-request server rather than a single-chat box, which is a design decision the
 *  measurement forced rather than one that was chosen.
 *
 *
 *  THE STREAMS ARE ALSO THE CORRECTNESS TEST
 *  ------------------------------------------
 *  Every stream is given the SAME prompt, so every stream must produce the SAME text. Each node keeps
 *  one KV cache per stream, and if the indexing is wrong the caches blend: two conversations mixing
 *  reads as the model losing the thread, not as a crash. Identical output across k streams is what
 *  proves they stayed separate, and it costs nothing to check.
 *
 *
 *  THE RING, AND WHY THE HEAD HOLDS BOTH ENDS
 *  -------------------------------------------
 *      head  ->  stage 1  ->  ...  ->  stage n-1  ->  back to head
 *
 *  The head owns the embedding table, the final norm and the output projection; everyone else owns
 *  layers and nothing else. This model has a TIED output head, so the embedding table is also the
 *  output projection. Splitting them across first and last node makes the same 243 MB resident twice.
 *  Closing the ring makes it resident once, and 243 MB is five transformer layers on these boards.
 *
 *
 *  RUN -- four processes on one machine, four sequences in flight
 *      pipe_model stage <model.gguf> 4 1 127.0.0.1 --fast --streams 4
 *      pipe_model stage <model.gguf> 4 2 127.0.0.1 --fast --streams 4
 *      pipe_model stage <model.gguf> 4 3 127.0.0.1 --fast --streams 4
 *      pipe_model head  <model.gguf> 4 127.0.0.1 "The capital of France is" 16 --fast --streams 4
 *
 *  Start the stages first; the head closes the ring. Nothing in the code knows whether the next node
 *  is a loopback socket or a Luckfox on the end of an ethernet cable.
 * ===========================================================================================
 */

#include "gguf.h"
#include "model_q.h"
#include "stage_link.h"
#include "tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Positions carried in one prefill message. 24 x 2048 x 4 bytes is 196 KB, inside the 256 KB payload
 * cap with room to spare. The cap is the reason this is not simply the whole prompt: a 500-token prompt
 * would be 4 MB on the wire, and chunking costs one extra hop per chunk at 30 us each. */
#define PIPE_BATCH_MAX 24
/* Settable with --batch so the batching effect can be measured rather than assumed, and so a
 * suspected batching bug can be isolated by setting it to 1. */
static int g_batch = PIPE_BATCH_MAX;

/* One candidate per stage, appended after the activation on the projection lap. */
typedef struct { int32_t id; float logit; } cand_t;

/* The best candidate across every node's slice. Each node scored a disjoint range of the vocabulary,
 * so the global argmax is simply the largest of the n local ones -- no logits ever leave the node that
 * computed them. For this model that is n entries of 8 bytes instead of 594 KB. */
static int32_t pick_winner(const uint8_t *buf, uint32_t act_bytes, int n_stages)
{
    const cand_t *c = (const cand_t *)(buf + act_bytes);
    int32_t id = -1;
    float lg = -1e30f;
    for (int i = 0; i < n_stages; i++)
        if (c[i].id >= 0 && c[i].logit > lg) { lg = c[i].logit; id = c[i].id; }
    return id;
}

/* Vocabulary split, computed identically on every node from the same file. Even by row count: unlike
 * layers, every row of the projection costs exactly the same, so there is nothing to weight. */
static void split_vocab(int vocab, int n_stages, int idx, int *lo, int *hi)
{
    const int base = vocab / n_stages, extra = vocab % n_stages;
    *lo = idx * base + (idx < extra ? idx : extra);
    *hi = *lo + base + (idx < extra ? 1 : 0);
}

#define PORT0       9200
#define MAX_STREAMS 16
#define TEXT_CAP    4096

static int g_fast = 0;
static int g_streams = 1;

/* ---------------------------------------------------------------------------------------------
 *  where to cut the model
 *
 *  AN EVEN SPLIT IS THE WRONG SPLIT, and the first run of this file proved it.
 *
 *  Four nodes, nine layers each, four sequences in flight: 3.63 tok/s measured. The prediction from
 *  the stage times was 1 / 275 ms = 3.64, so the pipeline was already perfect and the problem was the
 *  cut. The head was doing nine layers (133 ms) AND the output projection (142 ms) while every other
 *  node did nine layers and nothing else. One node carrying 2.1x the work sets the rate for all four.
 *
 *  So layers are assigned by COST, not by count, and the head's share is reduced by what the output
 *  projection already costs it.
 *
 *  Cost is not bytes either. Q4_K unpacks at 15.69 GB/s on this host and Q6_K at 6.11, measured in
 *  fast_path.c, so a Q6_K tensor costs 2.6x what the same number of Q4_K bytes would. The output
 *  projection on this model is Q6_K, which is exactly why it hurt so much more than its size
 *  suggested.
 *
 *  Every node runs this same function on the same file and reaches the same answer, so the split needs
 *  no negotiation and no coordinator.
 * ------------------------------------------------------------------------------------------ */

#define MAX_LAYERS 256

/* Time, in arbitrary but consistent units: bytes divided by the measured rate for that format. */
static double tensor_cost(const gguf_tensor *t)
{
    const double b = (double)gguf_nbytes(t);
    switch (t->type) {
    case GGML_Q4_K: return b / 15.69;    /* measured, tests/fast_path.c */
    case GGML_Q6_K: return b / 6.11;     /* measured, and 2.6x dearer per byte */
    case GGML_Q8_0: return b / 20.00;    /* estimate; no Q8_0 in this file */
    default:        return b / 29.40;    /* F32 and F16 move at memory speed */
    }
}

static double layer_cost(gguf_t *g, int l)
{
    static const char *suffix[] = {
        "attn_norm.weight", "attn_q.weight", "attn_q.bias", "attn_k.weight", "attn_k.bias",
        "attn_v.weight", "attn_v.bias", "attn_output.weight",
        "ffn_norm.weight", "ffn_gate.weight", "ffn_up.weight", "ffn_down.weight"
    };
    double c = 0.0;
    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++) {
        const gguf_tensor *t = gguf_tensor_findf(g, "blk.%d.%s", l, suffix[i]);
        if (t) c += tensor_cost(t);
    }
    return c;
}

static double head_cost(gguf_t *g)
{
    const gguf_tensor *o = gguf_tensor_find(g, "output.weight");
    if (!o) o = gguf_tensor_find(g, "token_embd.weight");   /* tied: the same tensor does both */
    double c = o ? tensor_cost(o) : 0.0;
    const gguf_tensor *n = gguf_tensor_find(g, "output_norm.weight");
    if (n) c += tensor_cost(n);
    return c;
}

/* Contiguous ranges, balanced by cost, with the head charged for the output projection up front. */
static void split_layers(gguf_t *g, int total, int n_stages, int idx, int *lo, int *hi)
{
    if (total > MAX_LAYERS) total = MAX_LAYERS;

    double lc[MAX_LAYERS];
    double all = head_cost(g);
    for (int l = 0; l < total; l++) { lc[l] = layer_cost(g, l); all += lc[l]; }
    const double target = all / (double)n_stages;

    /* Every node now carries 1/n of the projection, not one node carrying all of it, so each stage
     * starts with the same handicap and the layer split comes out even again. Before the vocabulary
     * split this line charged the whole projection to stage 0, which left it holding one layer of
     * thirty-six. */
    const double share = head_cost(g) / (double)n_stages;
    int    start = 0;
    double acc = share;
    int    stage = 0;

    for (int l = 0; l < total; l++) {
        acc += lc[l];
        const int stages_left = n_stages - stage;
        const int layers_left = total - l - 1;

        /* Close this stage when it has reached its share, unless doing so would leave a later stage
         * with no layers at all. A stage holding nothing would still cost a hop. */
        const int must_close = (layers_left == stages_left - 1);
        const int may_close  = (acc >= target) && (stages_left > 1) && (layers_left >= stages_left - 1);

        if (must_close || may_close) {
            if (stage == idx) { *lo = start; *hi = l + 1; return; }
            stage++;
            start = l + 1;
            acc = share;
            if (stage == n_stages - 1) break;       /* the last stage takes everything left */
        }
    }
    *lo = start;
    *hi = total;
}

/* ---------------------------------------------------------------------------------------------
 *  a middle or last stage
 * ------------------------------------------------------------------------------------------ */

static int run_stage(const char *path, int n_stages, int idx, const char *peer)
{
    gguf_t g;
    if (gguf_open(&g, path)) { printf("open: %s\n", g.err); return 1; }
    const int total = (int)gguf_arch_int(&g, "block_count", 0);

    int lo, hi;
    split_layers(&g, total, n_stages, idx, &lo, &hi);

    const uint16_t listen_port = (uint16_t)(PORT0 + idx);
    const int      last        = (idx == n_stages - 1);
    const uint16_t next_port   = (uint16_t)(last ? PORT0 : PORT0 + idx + 1);

    printf("\n  stage %d of %d: layers %d..%d, listening on %u, forwarding to %s:%u\n",
           idx, n_stages, lo, hi - 1, listen_port, peer, next_port);
    fflush(stdout);

    model_t m;
    const uint64_t t0 = stage_now_us();
    /* This node holds its layers AND its slice of the output projection. The slice is what makes the
     * model fit: with the projection whole on one node, 243.4 MB plus 15 layers on one 1 GB board and
     * 20 on the other leaves one layer of thirty-six with nowhere to go. */
    int vlo = 0, vhi = 0;
    {
        const gguf_tensor *e = gguf_tensor_find(&g, "token_embd.weight");
        const int vocab = e ? (int)(e->dims[1] ? e->dims[1] : 1) : 0;
        split_vocab(vocab, n_stages, idx, &vlo, &vhi);
    }
    if (model_load_slice(&m, &g, 2048, lo, hi, vlo, vhi, 0)) { printf("load: %s\n", g.err); return 1; }
    model_set_fast(&m, g_fast);
    if (model_set_streams(&m, g_streams)) { printf("  cannot allocate %d KV caches\n", g_streams); return 1; }

    const double kv_mb = 2.0 * m.n_layer * m.max_seq * m.n_kv * m.head_dim * sizeof(float)
                         * g_streams / 1048576.0;
    printf("  %.1f MB of weights, of which vocabulary %d..%d of the output projection\n",
           m.weight_bytes / 1048576.0, vlo, vhi - 1);
    printf("  %.1f MB of KV cache for %d streams, loaded in %.1f s\n",
           kv_mb, g_streams, (stage_now_us() - t0) / 1e6);
    fflush(stdout);

    stage_fd_t srv = stage_listen(listen_port);
    if (srv == STAGE_BAD_FD) { printf("  cannot listen on %u\n", listen_port); return 1; }
    stage_fd_t up = stage_accept(srv);
    if (up == STAGE_BAD_FD) { printf("  accept failed\n"); return 1; }

    /* Connect downstream only after upstream arrives, so the ring comes up in order and a stage
     * cannot sit retrying against a peer that has not started. */
    stage_fd_t dn = STAGE_BAD_FD;
    for (int t = 0; t < 400 && dn == STAGE_BAD_FD; t++) dn = stage_connect(peer, next_port);
    if (dn == STAGE_BAD_FD) { printf("  cannot reach %s:%u\n", peer, next_port); return 1; }
    printf("  ring connected, running\n\n");
    fflush(stdout);

    const uint32_t act_bytes = (uint32_t)m.dim * sizeof(float);
    /* One buffer big enough for the largest prefill batch. A decode activation is one row of it. */
    float *X = (float *)malloc((size_t)PIPE_BATCH_MAX * m.dim * sizeof(float));
    /* The projection lap carries a normed activation plus one candidate slot per stage. */
    const uint32_t proj_bytes = act_bytes + (uint32_t)(n_stages * sizeof(cand_t));
    uint8_t *PROJ = (uint8_t *)malloc(proj_bytes);
    if (!X || !PROJ) { printf("  cannot allocate the buffers\n"); return 1; }

    uint64_t compute_us = 0, n = 0, n_pos = 0;
    const uint64_t wall0 = stage_now_us();

    for (;;) {
        stage_hdr_t h;
        if (stage_recv(up, &h, X, (uint32_t)PIPE_BATCH_MAX * act_bytes) != 0) break;
        if (h.type == STAGE_BYE) { stage_send(dn, &h, NULL); break; }

        /* The projection lap: fill in this node's candidate and pass the message on. The activation
         * is already normed by the head, so every node projects exactly the same vector and there is
         * no chance of two nodes disagreeing about what they scored. */
        if (h.type == STAGE_LOGITS) {
            memcpy(PROJ, X, h.len);
            float best = 0.0f;
            const int32_t id = model_project_slice(&m, (const float *)PROJ, &best);
            cand_t *c = (cand_t *)(PROJ + act_bytes);
            c[idx].id = id;
            c[idx].logit = best;
            h.stage = (uint16_t)(h.stage + 1);
            if (stage_send(dn, &h, PROJ) != 0) break;
            continue;
        }

        /* How many positions arrived is told by the length, not by a separate field: the payload is
         * always a whole number of activations and deriving it cannot disagree with itself. */
        const int np = (int)(h.len / act_bytes);
        if (np < 1 || np > PIPE_BATCH_MAX) { printf("  payload of %u bytes is not a batch\n", h.len); break; }

        const uint64_t c0 = stage_now_us();
        /* seq is the position of the FIRST activation in the batch, stream says which sequence. Both
         * come off the wire rather than being counted locally: a node that counted its own would be
         * correct until one activation was dropped and then quietly wrong. */
        if (np == 1) {
            /* model_layers works on m.x, not on the receive buffer. Forgetting that cost an hour:
             * the batch path was correct, so the token produced by prefill came out right and every
             * token after it was computed from stale memory. Output that starts correct and then
             * degenerates is the signature of reading the wrong buffer, not of a broken kernel. */
            memcpy(m.x, X, act_bytes);
            model_layers(&m, (int)h.seq, (int)h.stream);
            memcpy(X, m.x, act_bytes);
        } else {
            model_layers_batch(&m, X, np, (int)h.seq, (int)h.stream);
        }
        compute_us += stage_now_us() - c0;
        n++;
        n_pos += (uint64_t)np;

        h.stage = (uint16_t)(h.stage + 1);
        h.layer = (uint16_t)(h.layer + m.n_layer);
        if (stage_send(dn, &h, X) != 0) break;
    }

    if (n) {
        const double wall_s = (stage_now_us() - wall0) / 1e6;
        const double busy = 100.0 * ((double)compute_us / 1e6) / wall_s;
        printf("  %llu messages carrying %llu positions through %d layers, %.1f ms per position\n",
               (unsigned long long)n, (unsigned long long)n_pos, m.n_layer,
               (double)compute_us / n_pos / 1000.0);
        printf("  %.1f MB of weights, read once per MESSAGE not per position -> %.2f GB/s\n",
               m.bytes_per_token / 1048576.0,
               (double)m.bytes_per_token * n / ((double)compute_us / 1e6) / 1073741824.0);
        printf("  BUSY %.0f%% of the run. That is the number streams exist to raise.\n", busy);
    }
    free(X); free(PROJ);
    stage_close(up); stage_close(dn); stage_close(srv);
    model_free(&m);
    gguf_close(&g);
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 *  the head
 * ------------------------------------------------------------------------------------------ */

static int run_head(const char *path, int n_stages, const char *peer,
                    const char *prompt, int n_gen)
{
    gguf_t g;
    if (gguf_open(&g, path)) { printf("open: %s\n", g.err); return 1; }
    const int total = (int)gguf_arch_int(&g, "block_count", 0);

    tokenizer_t tk;
    if (tokenizer_init(&tk, &g)) { printf("tokenizer: %s\n", g.err); return 1; }

    int lo, hi;
    split_layers(&g, total, n_stages, 0, &lo, &hi);

    int32_t ids[1024];
    const int n_prompt = tokenizer_encode(&tk, prompt, ids, 1024);
    if (n_prompt <= 0) { printf("  prompt did not encode\n"); return 1; }

    const int K = g_streams;
    printf("\n======================================================================\n");
    printf("  %d nodes, none holding the model, %d sequences in flight\n", n_stages, K);
    printf("======================================================================\n");
    printf("  %d layers total, this node holds %d..%d plus the embedding table and head\n",
           total, lo, hi - 1);
    fflush(stdout);

    model_t m;
    const uint64_t t_load0 = stage_now_us();
    int vlo = 0, vhi = 0;
    {
        const gguf_tensor *e = gguf_tensor_find(&g, "token_embd.weight");
        const int vocab = e ? (int)(e->dims[1] ? e->dims[1] : 1) : 0;
        split_vocab(vocab, n_stages, 0, &vlo, &vhi);
    }
    /* The head reads input embedding rows off the FILE. One 4 KB row per token against a 243 MB
     * table: holding the whole thing in memory to read a thousandth of it would be the most wasteful
     * megabyte in the machine, and it is the megabyte that decides whether the model fits. */
    if (model_load_slice(&m, &g, n_prompt + n_gen + 8, lo, hi, vlo, vhi, 1)) {
        printf("load: %s\n", g.err);
        return 1;
    }
    model_set_fast(&m, g_fast);
    if (model_set_streams(&m, K)) { printf("  cannot allocate %d KV caches\n", K); return 1; }

    const uint32_t act_bytes = (uint32_t)m.dim * sizeof(float);
    printf("  %.1f MB here: layers %d..%d plus vocabulary %d..%d of the projection\n",
           m.weight_bytes / 1048576.0, lo, hi - 1, vlo, vhi - 1);
    printf("  input embeddings come off the file, not memory. Loaded in %.1f s, %u bytes a hop\n",
           (stage_now_us() - t_load0) / 1e6, act_bytes);
    fflush(stdout);

    stage_fd_t back = stage_listen(PORT0);
    if (back == STAGE_BAD_FD) { printf("  cannot listen on %u\n", PORT0); return 1; }
    stage_fd_t dn = STAGE_BAD_FD;
    for (int t = 0; t < 400 && dn == STAGE_BAD_FD; t++) dn = stage_connect(peer, (uint16_t)(PORT0 + 1));
    if (dn == STAGE_BAD_FD) { printf("  cannot reach the next stage\n"); return 1; }
    stage_fd_t up = stage_accept(back);
    if (up == STAGE_BAD_FD) { printf("  ring never closed\n"); return 1; }
    printf("  ring closed across %d nodes\n\n", n_stages);
    fflush(stdout);

    const uint32_t proj_bytes = act_bytes + (uint32_t)(n_stages * sizeof(cand_t));
    uint8_t *PROJ = (uint8_t *)malloc(proj_bytes);
    if (!PROJ) { printf("  cannot allocate the projection buffer\n"); return 1; }

    /* Per-stream state. Every stream gets the same prompt, so every stream must end up with the same
     * text; that is the check that the KV caches stayed apart. */
    int32_t tok[MAX_STREAMS];
    int     pos[MAX_STREAMS];
    int     done[MAX_STREAMS];
    char   *text[MAX_STREAMS];
    int     tlen[MAX_STREAMS];
    for (int s = 0; s < K; s++) {
        tok[s] = ids[0];
        pos[s] = 0;
        done[s] = 0;
        text[s] = (char *)calloc(TEXT_CAP, 1);
        tlen[s] = 0;
        if (!text[s]) { printf("  out of memory\n"); return 1; }
    }

    uint64_t produced = 0;
    const int steps = n_prompt + n_gen;
    const uint64_t wall0 = stage_now_us();
    uint64_t t_mine = 0, t_head = 0;

    /* NO ROUND BARRIER.
     *
     * The obvious loop is "send all K, then receive all K, repeat", and it cost 40% of the machine.
     * Measured with the balanced split: stages 51%, 60% and 57% busy, aggregate 3.39 tok/s against a
     * predicted 5.68 from the stage times. The gap was entirely the barrier -- at the end of every
     * round the pipe drains, every node falls idle waiting for the slowest, then refills.
     *
     * So the pipe is PRIMED with K activations and then held full: every time one result comes back,
     * that stream's next token is computed and sent immediately. Exactly K activations are in flight
     * at all times and no node ever waits for a sibling.
     *
     * Each stream keeps its own step counter. A shared one was what made the barrier feel natural. */
    int step[MAX_STREAMS];
    for (int s = 0; s < K; s++) step[s] = 0;

    /* Compute this node's share for one stream and put it on the wire. */
    #define PUMP(s)                                                                     \
        do {                                                                            \
            const uint64_t a0 = stage_now_us();                                         \
            model_embed(&m, tok[s]);                                                    \
            model_layers(&m, pos[s], (s));                                              \
            t_mine += stage_now_us() - a0;                                              \
            stage_hdr_t h;                                                              \
            memset(&h, 0, sizeof(h));                                                   \
            h.type = STAGE_ACT;                                                         \
            h.token_id = (uint32_t)tok[s];                                              \
            h.len = act_bytes;                                                          \
            h.stage = 1;                                                                \
            h.layer = (uint16_t)m.n_layer;                                              \
            h.seq = (uint32_t)pos[s];                                                   \
            h.stream = (uint32_t)(s);                                                   \
            h.t_emit_us = stage_now_us();                                               \
            if (stage_send(dn, &h, m.x) != 0) { printf("\n  send failed\n"); goto finish; } \
        } while (0)

    /* ---- PREFILL, batched through the whole ring ------------------------------------------
     *
     * One position at a time would make every node read all of its weights once per prompt token.
     * Measured single-process, batching took prefill from 4.98 to 10.51 tok/s and moved it from
     * memory bound to 93% of this host's float arithmetic peak. The same applies to every node in the
     * ring, so the batch travels as one message.
     *
     * Each stream is prefilled in turn. They are independent, so there is nothing to interleave yet,
     * and the decode loop below is where keeping the pipe full matters. */
    const uint64_t t_pre0 = stage_now_us();
    float *XB = (float *)malloc((size_t)PIPE_BATCH_MAX * m.dim * sizeof(float));
    if (!XB) { printf("  cannot allocate the prefill batch\n"); return 1; }

    /* Prefill chunks from DIFFERENT streams are independent, so they can be in flight together even
     * though chunks within one stream cannot. Sending one chunk and waiting for it left three of four
     * nodes idle and prefill ran at 1.59 tok/s across the ring; this is the same prime-and-refill shape
     * the decode loop uses, for the same reason. */
    int pf_base[MAX_STREAMS];
    for (int s = 0; s < K; s++) pf_base[s] = 0;

    /* Declared here rather than after the loops, because `goto finish` on a broken ring jumps past
     * any declaration below and would leave these holding whatever was on the stack. */
    double   t_prefill = 0.0;
    uint64_t t_dec0 = 0, pre_produced = 0;

    /* Embed and run this node's layers for one chunk of one stream, then put it on the wire. XB is
     * safe to reuse because the send completes before this returns. */
    #define PF_SEND(s)                                                                        \
        do {                                                                                  \
            const int b0 = pf_base[s];                                                        \
            const int nb = (n_prompt - b0) < g_batch ? (n_prompt - b0) : g_batch;             \
            for (int j = 0; j < nb; j++) {                                                    \
                model_embed(&m, ids[b0 + j]);                                                 \
                memcpy(XB + (size_t)j * m.dim, m.x, (size_t)m.dim * sizeof(float));           \
            }                                                                                 \
            model_layers_batch(&m, XB, nb, b0, (s));                                          \
            stage_hdr_t h;                                                                    \
            memset(&h, 0, sizeof(h));                                                         \
            h.type = STAGE_ACT;                                                               \
            h.len = (uint32_t)nb * act_bytes;                                                 \
            h.stage = 1;                                                                      \
            h.layer = (uint16_t)m.n_layer;                                                    \
            h.seq = (uint32_t)b0;                                                             \
            h.stream = (uint32_t)(s);                                                         \
            h.t_emit_us = stage_now_us();                                                     \
            if (stage_send(dn, &h, XB) != 0) { printf("\n  prefill send failed\n"); goto finish; } \
        } while (0)

    int pf_flight = 0;
    for (int s = 0; s < K; s++) { PF_SEND(s); pf_flight++; }

    /* Start the projection lap for one stream. m.x must hold that stream's final activation. */
    #define PROJ_SEND(s)                                                                        \
        do {                                                                                    \
            model_norm_final(&m, (float *)PROJ);                                                \
            cand_t *cc = (cand_t *)(PROJ + act_bytes);                                          \
            for (int _i = 0; _i < n_stages; _i++) { cc[_i].id = -1; cc[_i].logit = -1e30f; }    \
            float _b = 0.0f;                                                                    \
            cc[0].id = model_project_slice(&m, (const float *)PROJ, &_b);                       \
            cc[0].logit = _b;                                                                   \
            stage_hdr_t hp;                                                                     \
            memset(&hp, 0, sizeof(hp));                                                         \
            hp.type = STAGE_LOGITS;                                                             \
            hp.len = proj_bytes;                                                                \
            hp.stream = (uint32_t)(s);                                                          \
            hp.t_emit_us = stage_now_us();                                                      \
            if (stage_send(dn, &hp, PROJ) != 0) { printf("\n  projection send failed\n"); goto finish; } \
        } while (0)

    /* BOTH LOOPS BELOW DISPATCH ON MESSAGE TYPE, never on arrival order.
     *
     * Each token now takes two laps of the ring: one for the layers, one for the projection. With k
     * streams moving, stream 2's layer reply can arrive while stream 0's projection is still out.
     * Assuming what comes next would work perfectly at k = 1 and silently mix streams at k = 4, which
     * is the class of bug that reads as the model losing the thread rather than as a crash.
     *
     * Every stream has exactly one message outstanding at any moment, so the in-flight count only
     * changes when a stream finishes. */
    while (pf_flight > 0) {
        stage_hdr_t r;
        if (stage_recv(up, &r, XB, (uint32_t)PIPE_BATCH_MAX * act_bytes) != 0) {
            printf("\n  ring broke during prefill\n");
            goto finish;
        }
        const int s = (int)r.stream;
        if (s < 0 || s >= K) { printf("\n  bad stream id %d in prefill\n", s); goto finish; }

        if (r.type == STAGE_LOGITS) {
            memcpy(PROJ, XB, proj_bytes);
            tok[s] = pick_winner(PROJ, act_bytes, n_stages);
            pos[s] = n_prompt;
            step[s] = n_prompt;
            tlen[s] += tokenizer_decode(&tk, tok[s], text[s] + tlen[s], TEXT_CAP - tlen[s] - 1);
            produced++;
            pf_flight--;
            continue;
        }

        /* A batch of activations came back, so this chunk's layers are done everywhere. */
        const int nb = (int)(r.len / act_bytes);
        pf_base[s] += nb;
        if (pf_base[s] >= n_prompt) {
            /* Only the last position of the last chunk produces a token; every other position in the
             * prompt exists to fill the KV caches on every node. */
            memcpy(m.x, XB + (size_t)(nb - 1) * m.dim, (size_t)m.dim * sizeof(float));
            PROJ_SEND(s);
        } else {
            PF_SEND(s);
        }
    }
    #undef PF_SEND

    t_prefill = (stage_now_us() - t_pre0) / 1e6;
    printf("  prefill: %d prompt tokens x %d streams = %d positions in %.2f s = %.1f tok/s\n",
           n_prompt, K, n_prompt * K, t_prefill, (double)n_prompt * K / t_prefill);
    fflush(stdout);

    /* Decode is timed from here. Folding prefill into the same average would report neither: the two
     * phases are limited by different things, which is the whole point of docs/26. */
    t_dec0 = stage_now_us();
    pre_produced = produced;

    int in_flight = 0;
    for (int s = 0; s < K; s++) { if (!done[s]) { PUMP(s); in_flight++; } }

    while (in_flight > 0) {
        stage_hdr_t r;
        if (stage_recv(up, &r, XB, (uint32_t)PIPE_BATCH_MAX * act_bytes) != 0) {
            printf("\n  ring broke\n");
            goto finish;
        }
        const int s = (int)r.stream;
        if (s < 0 || s >= K) { printf("\n  bad stream id %d off the wire\n", s); goto finish; }

        if (r.type == STAGE_ACT) {
            /* Layers finished. Norm here, then send the vector round for everyone to project their
             * own slice of the vocabulary against. */
            memcpy(m.x, XB, act_bytes);
            const uint64_t h0 = stage_now_us();
            PROJ_SEND(s);
            t_head += stage_now_us() - h0;
            continue;
        }

        memcpy(PROJ, XB, proj_bytes);
        const int32_t next = pick_winner(PROJ, act_bytes, n_stages);
        if (next < 0) { printf("\n  no node claimed a candidate\n"); goto finish; }

        pos[s]++;
        step[s]++;
        produced++;

        /* The prompt was consumed by the batched prefill above, so every token from here is generated
         * and every one of them is kept. */
        tok[s] = next;
        if (next == tk.eos) done[s] = 1;
        else {
            const int k = tokenizer_decode(&tk, next, text[s] + tlen[s], TEXT_CAP - tlen[s] - 1);
            tlen[s] += k;
        }
        if (step[s] >= steps) done[s] = 1;

        if (!done[s]) PUMP(s);
        else          in_flight--;
    }
    #undef PUMP
    #undef PROJ_SEND

finish:
    {
        stage_hdr_t b;
        memset(&b, 0, sizeof(b));
        b.type = STAGE_BYE;
        stage_send(dn, &b, NULL);
    }

    const double wall = (stage_now_us() - wall0) / 1e6;

    printf("  ----------------------------------------------------------------------\n");
    printf("  %s%s\n", prompt, text[0]);
    printf("  ----------------------------------------------------------------------\n");

    int agree = 1;
    for (int s = 1; s < K; s++) if (strcmp(text[s], text[0]) != 0) agree = 0;
    printf("\n  %d streams, all given the same prompt: %s\n", K,
           agree ? "IDENTICAL output, so the KV caches stayed separate"
                 : "OUTPUT DIVERGED -- the per-stream caches are blending");

    if (produced) {
        /* Prefill and decode are limited by different things -- arithmetic and memory -- so averaging
         * them together reports neither. They are timed and printed apart. */
        const double t_dec = (stage_now_us() - t_dec0) / 1e6;
        const unsigned long long dec_tokens = (unsigned long long)(produced - pre_produced);
        printf("\n  THROUGHPUT with %d sequences in flight\n", K);
        printf("  %-32s %9d\n", "prompt positions prefilled", n_prompt * K);
        printf("  %-32s %9.2f s  -> %6.2f tok/s\n", "PREFILL, compute bound",
               t_prefill, (double)n_prompt * K / t_prefill);
        printf("  %-32s %9llu\n", "tokens decoded, all streams", dec_tokens);
        printf("  %-32s %9.2f s  -> %6.2f tok/s\n", "DECODE, memory bound",
               t_dec, dec_tokens > 0 ? dec_tokens / t_dec : 0.0);
        printf("  %-32s %9.2f tok/s\n", "decode, per stream",
               dec_tokens > 0 ? dec_tokens / t_dec / K : 0.0);
        printf("  %-32s %9.2f s\n", "wall clock, both phases", wall);
        printf("  %-32s %9.1f ms\n", "this node's layers, per token",
               (double)t_mine / produced / 1000.0);
        printf("  %-32s %9.1f ms\n", "output projection, per token",
               (double)t_head / produced / 1000.0);
        printf("\n  Compare against the same four nodes with one sequence in flight: 1.55 tok/s.\n");
        printf("  Nothing about the hardware changed. The nodes simply stopped waiting on each\n");
        printf("  other, which is the difference between a capacity machine and a throughput one.\n");
    }

    free(XB);
    for (int s = 0; s < K; s++) free(text[s]);
    stage_close(dn); stage_close(up); stage_close(back);
    model_free(&m);
    tokenizer_free(&tk);
    gguf_close(&g);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fast")) g_fast = 1;
        if (!strcmp(argv[i], "--batch") && i + 1 < argc) {
            g_batch = atoi(argv[i + 1]);
            if (g_batch < 1) g_batch = 1;
            if (g_batch > PIPE_BATCH_MAX) g_batch = PIPE_BATCH_MAX;
        }
        if (!strcmp(argv[i], "--streams") && i + 1 < argc) {
            g_streams = atoi(argv[i + 1]);
            if (g_streams < 1) g_streams = 1;
            if (g_streams > MAX_STREAMS) g_streams = MAX_STREAMS;
        }
    }

    if (stage_init() != 0) { printf("socket startup failed\n"); return 1; }
    int rc = 1;

    if (argc >= 6 && !strcmp(argv[1], "head")) {
        rc = run_head(argv[2], atoi(argv[3]), argv[4], argv[5], argc > 6 ? atoi(argv[6]) : 16);
    } else if (argc >= 6 && !strcmp(argv[1], "stage")) {
        rc = run_stage(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    } else {
        printf("\n  pipe_model head  <model.gguf> <n_stages> <peer> \"<prompt>\" [tokens]"
               " [--fast] [--streams k]\n");
        printf("  pipe_model stage <model.gguf> <n_stages> <index 1..n-1> <peer>"
               " [--fast] [--streams k]\n\n");
        printf("  Start every stage first, then the head. The head closes the ring.\n\n");
    }
    stage_shutdown();
    return rc;
}

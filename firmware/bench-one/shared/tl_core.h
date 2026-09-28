/* ===========================================================================================
 *  tl_core.h -- a real GGUF transformer on a microcontroller: weights streamed, memory external
 * ===========================================================================================
 *
 *  Runs Qwen2-family dense models (tested: Qwen2.5-Coder-3B Q4_K_M, 3.09 B parameters, 1.93 GB) on
 *  a board with no memory that can hold them:
 *
 *    weights         read from storage through plat_sd_read, a block of rows at a time, every token
 *    working memory  an external store reached only through plat_ps_read / plat_ps_write (the Teensy's
 *                    bit-banged PSRAM): the tokenizer, every norm and bias, and the attention cache
 *    on-chip RAM     activations, one block of weight rows, attention scores
 *
 *  The arithmetic is shared/model_q.c's fused integer path, operation for operation and in the same
 *  order, so on one machine the two produce the same logits to the last bit. tests/tl_host.c and
 *  tests/tl_ref.c check exactly that on the PC.
 * ======================================================================================== */
#ifndef TL_CORE_H
#define TL_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      n_layer, dim, hidden, n_heads, n_kv, head_dim, q_dim, vocab, max_seq;
    int32_t  bos, eos;
    uint64_t file_bytes, sd_bytes_per_token;
    uint32_t ps_used;                 /* PSRAM bytes allocated: the attention cache, nothing else */
    uint32_t n_merges;
    uint32_t ps_kv;                   /* = ps_used                                    */
    uint32_t tok_store_bytes;         /* the tokenizer's tables in the card store (tl_plat.h) */
    int      tok_built;               /* 1: built from the model file on this open; 0: the store matched */
    double   params;                  /* weights in the file, counted from tensor shapes */
} tl_info_t;

typedef struct {
    double   t_total, t_sd, t_ps, t_compute;
    /* where t_total went, by stage; each includes that stage's own storage reads */
    double   t_embed, t_qkv, t_att, t_wo, t_ffn, t_head;
    double   t_hidden;          /* arithmetic done while the card was reading (plat_sd_read_overlap);
                                   t_sd counts only the card time the pass waited for */
    uint64_t sd_bytes, ps_read, ps_written;
} tl_stats_t;

/* Parse the model through plat_sd_read; find the tokenizer's tables in the card store, or build them there
 * (PSRAM as scratch, erased after); lay out the attention cache, the only thing kept in PSRAM. Norms and
 * biases stay in the model file and are read where used. Returns 0, or -1 with the reason in err. */
int  tl_open(tl_info_t *info, char *err, int errlen);

/* Text -> token ids, identical to shared/tokenizer.c. Returns the count, or -1 if it does not fit. */
int  tl_encode(const char *text, int32_t *ids, int max);

/* One token id -> its raw bytes, identical to shared/tokenizer.c. Returns the byte count. */
int  tl_decode(int32_t id, char *out, int max);

/* Feed one token at sequence position pos (0-based, positions fed in order) through the whole model and
 * return the two most likely next tokens. Returns 0, or -1 on a storage/PSRAM error or pos out of range. */
int  tl_forward(int32_t token, int pos, int32_t *top1, float *l1, int32_t *top2, float *l2);

void tl_stats(tl_stats_t *s);
void tl_stats_reset(void);

/* Where one weight matrix lives in the file, for benchmarks that want real rows. which: 0 attn_q,
 * 1 attn_k, 2 attn_v, 3 attn_output, 4 ffn_gate, 5 ffn_up, 6 ffn_down, 7 token_embd (layer ignored).
 * Returns 0, or -1 when there is no such matrix. Reads nothing; the model must be open. */
int tl_matrix(int layer, int which, uint64_t *off, uint32_t *type, uint32_t *rows, uint32_t *cols,
              uint32_t *row_bytes);

/* Feed n prompt tokens (1..tl_batch()) at positions pos0..pos0+n-1 in ONE pass over the weights, and
 * return, for every one of those positions, the two most likely next tokens: the same values tl_forward
 * gives one position at a time (tests/tl_host.c with TL_PREFILL=1 checks it against model_q.c). The
 * output arrays hold n entries; any may be NULL. Positions must follow those already fed. */
int  tl_prefill(const int32_t *tokens, int n, int pos0, int32_t *top1, float *l1, int32_t *top2, float *l2);
int  tl_batch(void);

/* The pass underneath tl_prefill, for positions of different sequences: slot p feeds tokens[p] at position
 * pos[p] of the sequence whose attention cache starts at row base[p]. Sequences must not share cache rows;
 * the slots of one sequence must be consecutive positions following those already fed. */
int  tl_step(const int32_t *tokens, const int *pos, const int *base, int n,
             int32_t *top1, float *l1, int32_t *top2, float *l2);

/* Several prompts answered together (up to tl_batch() of them), one pass over the weights per round.
 * The caller fills ids (the prompt, with room for n_prompt + n_gen + 1 ids), n_prompt and n_gen; the core
 * fills the rest. A sequence is fed positions 0 .. n_prompt+n_gen-1, stopping early after it produces the
 * end token, the same positions tests/tl_ref.c feeds; the tokens it produced are ids[n_prompt .. fed]. */
typedef struct {
    int32_t *ids;
    int      n_prompt, n_gen;
    int      base, fed, done;
    int      donor, share;   /* core-filled: an earlier sequence whose first `share` prompt tokens are this
                                one's too; those cache rows are copied from it, not computed (-1, 0: none) */
} tl_seq_t;
/* Shared prompt openings (default on): a sequence whose prompt starts with the same tokens as an earlier
 * one's -- every chat prompt's system turn -- copies those positions' cache rows instead of recomputing them.
 * Exact: a cache row depends only on the tokens up to its own position. Returns the previous setting. */
int  tl_multi_share(int on);
typedef void (*tl_multi_emit_fn)(void *ctx, int seq, int pos, int32_t fed, int32_t top1, float l1,
                                 int32_t top2, float l2);
/* Lays the sequences out in the attention cache. Returns 0, or -1 when they do not fit (tl_last_error). */
int  tl_multi_begin(tl_seq_t *seqs, int n_seqs);
/* One pass over the weights; emit is called for every position fed, in slot order. Returns the number of
 * positions fed, 0 once every sequence is done, or -1 on an error. */
int  tl_multi_pass(tl_seq_t *seqs, int n_seqs, tl_multi_emit_fn emit, void *ctx);

/* The core's working arena, for a board to borrow BETWEEN passes (benchmarks, card tests). Its contents
 * do not survive a pass and a pass does not need them to survive. */
void *tl_scratch(uint32_t *bytes);

/* The last error tl_forward hit, for the board to print. */
const char *tl_last_error(void);

#ifdef __cplusplus
}
#endif
#endif /* TL_CORE_H */

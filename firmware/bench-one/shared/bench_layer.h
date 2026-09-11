/* ===========================================================================================
 *  bench_layer.h -- an actual transformer decoder layer, INT4 weights, no framework
 * ===========================================================================================
 *
 *  Everything else in this project measures a piece: how fast a board reads memory, what a nibble
 *  multiply costs, what a hop between nodes costs. This is the thing those pieces exist to run.
 *
 *  One layer, the Llama shape, which is what nearly every open model uses:
 *
 *      x += attention( rmsnorm(x) )
 *      x += mlp( rmsnorm(x) )
 *
 *  with attention being multi-head with rotary position embedding and a KV cache, and the MLP being
 *  SwiGLU. No framework, no dependencies, integer arithmetic, and it runs on a microcontroller.
 *
 *
 *  WHY INT4 WEIGHTS AND INT8 ACTIVATIONS
 *  --------------------------------------
 *  Capacity is the whole architecture. INT4 halves the weights against INT8 and quarters them
 *  against FP16, and weights are gigabytes while activations are kilobytes -- so the weights get the
 *  aggressive format and the activations get the safe one.
 *
 *  Measured, that choice costs 7x in software: 2.18 G MAC/s for INT4 against 15.66 for INT8 on a
 *  host with AVX2, because every byte needs a shift and two sign extensions before any multiply.
 *  It costs NOTHING in FPGA fabric, where splitting a byte into nibbles is wiring. That gap is the
 *  entire reason there are FPGAs in this machine.
 *
 *
 *  PER-ROW SCALES, NOT PER-TENSOR
 *  -------------------------------
 *  A 4-bit weight holds sixteen values, so a whole matrix sharing one scale factor would waste most
 *  of that range on the rows whose values happen to be small. Each row therefore carries its own
 *  scale, costing two bytes per row against 2048 bytes of weights for a 4096-wide row -- a tenth of
 *  a percent for a large accuracy gain. This is what every serious 4-bit quantizer does and skipping
 *  it produces a model that runs at exactly the right speed and answers badly.
 *
 *
 *  THE KV CACHE IS THE PART THAT GROWS
 *  ------------------------------------
 *  Weights are fixed. The KV cache grows with every token generated, and at long context it can
 *  exceed the weights of the layer it belongs to. Held at INT8 here, per layer it is
 *
 *      2 * n_kv_heads * head_dim * max_seq  bytes
 *
 *  which for 8 KV heads, 128-dim heads and 4096 tokens is 8 MB PER LAYER. On a node with 13 MB of
 *  usable memory that is the difference between holding four layers and holding one, so it is
 *  budgeted explicitly rather than allocated hopefully.
 *
 *
 *  GROUPED-QUERY ATTENTION
 *  ------------------------
 *  n_kv_heads may be fewer than n_heads, with several query heads sharing one key/value head. Every
 *  recent model does this because it shrinks the KV cache by exactly that ratio, and on this machine
 *  the KV cache is a first-order memory cost rather than a detail.
 * ===========================================================================================
 */

#ifndef BENCH_LAYER_H
#define BENCH_LAYER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A quantized weight matrix: INT4, packed two per byte, row-major, with a scale per row. */
typedef struct {
    uint8_t  *w;        /* rows * (cols/2) bytes                                        */
    int16_t  *scale;    /* rows entries, Q12: real value = (int4 * scale) / 4096         */
    uint16_t  rows;
    uint16_t  cols;
} qmat_t;

typedef struct {
    uint16_t dim;           /* model width                                              */
    uint16_t hidden;        /* MLP inner width                                          */
    uint16_t n_heads;
    uint16_t n_kv_heads;    /* fewer than n_heads means grouped-query attention          */
    uint16_t head_dim;      /* dim / n_heads                                            */
    uint16_t max_seq;
} layer_cfg_t;

typedef struct {
    layer_cfg_t cfg;

    int16_t *attn_norm;     /* dim, Q12                                                 */
    int16_t *ffn_norm;      /* dim, Q12                                                 */

    qmat_t wq, wk, wv, wo;
    qmat_t w_gate, w_up, w_down;

    /* KV cache, INT8, laid out [pos][kv_head][head_dim] so one position is contiguous --
     * appending a token then writes one run rather than scattering across the cache. */
    int8_t  *k_cache;
    int8_t  *v_cache;
    uint16_t pos;           /* how many tokens are cached                               */
} layer_t;

/* Bytes a layer needs, split so a planner can see what grows and what does not. */
size_t layer_weight_bytes(const layer_cfg_t *c);
size_t layer_kv_bytes(const layer_cfg_t *c);

/* Allocate and fill with deterministic pseudo-random weights.
 *
 * Real weights are not the point of a bandwidth and latency experiment, and needing a model file on
 * every board would stop the experiment happening at all. The ARITHMETIC is real: the same INT4
 * kernel, the same shapes, the same memory traffic. Only the numbers are synthetic. */
int  layer_init_random(layer_t *L, const layer_cfg_t *c, uint32_t seed);
void layer_free(layer_t *L);

/* One decoder layer, in place. `x` is dim INT8 activations and comes back as the layer's output.
 * `scratch` must hold at least layer_scratch_bytes(). Caller-owned, because a node holding sixteen
 * layers should allocate this once rather than sixteen times. */
size_t layer_scratch_bytes(const layer_cfg_t *c);
void   layer_forward(layer_t *L, int8_t *x, void *scratch);

/* Reset the KV cache, i.e. start a new sequence. */
void   layer_reset(layer_t *L);

#ifdef __cplusplus
}
#endif
#endif /* BENCH_LAYER_H */

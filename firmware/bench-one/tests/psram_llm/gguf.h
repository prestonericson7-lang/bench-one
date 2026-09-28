/* ===========================================================================================
 *  gguf.h -- read a real model file in C, on the hardware that will run it
 * ===========================================================================================
 *
 *  There is already a Python inspector (tests/gguf_inspect.py) and it answered the planning
 *  questions: 36 layers, 49.2 MB each, Q4_K and Q6_K, data starting at byte 5956576. That is where
 *  Python's usefulness ends. A Teensy does not have Python, a Zynq running bare metal does not have
 *  Python, and the thing being built has to open the file itself.
 *
 *  So this is the same job in C, with no dependencies beyond the standard library, reading tensors
 *  one at a time by seek and dequantizing them on the spot. Nothing is memory-mapped and nothing
 *  assumes the file fits in RAM, because on every target here it does not.
 *
 *
 *  WHAT IS ACTUALLY HARD ABOUT THIS
 *  ---------------------------------
 *  Not the container. The container is a header, a list of key-value pairs, a list of tensor
 *  descriptors, and then a blob. An afternoon.
 *
 *  The quantization formats are the work, and getting one bit wrong produces a model that loads
 *  cleanly, runs at exactly the right speed, and talks nonsense. Q4_K is not "four-bit weights": it
 *  is 256 weights in 144 bytes, split into eight sub-blocks of 32, with a 16-bit scale and a 16-bit
 *  minimum for the whole block, and then a SIX-BIT scale and a SIX-BIT minimum per sub-block packed
 *  three-to-two-bytes across twelve bytes. Q6_K splits each weight's six bits across two separate
 *  arrays, low nibbles in one and high pairs in another, with a signed 8-bit scale every sixteen
 *  weights.
 *
 *  Both are implemented here to match llama.cpp exactly, because the weights in the file were
 *  produced by llama.cpp's quantizer and any disagreement is a bug by definition. The unpacking is
 *  verified against an independent Python implementation of the same spec rather than against
 *  itself -- see tests/gguf_check.py. Two implementations of a format agreeing is evidence; one
 *  implementation agreeing with itself is not.
 *
 *
 *  WHY DEQUANTIZE TO FLOAT AT ALL, GIVEN THE WHOLE PROJECT IS INT4
 *  ----------------------------------------------------------------
 *  Because correctness has to come before speed or there is no way to tell speed from wrongness.
 *  Float is the reference path: slow, obviously right, and able to say what the model's real output
 *  is. The INT4 path is then measured AGAINST it. Build the fast path first and every error looks
 *  like quantization noise, which is how people ship models that are quietly 10% worse.
 * ===========================================================================================
 */

#ifndef GGUF_H
#define GGUF_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Metadata value types, as written in the file. */
enum {
    GGUF_U8 = 0, GGUF_I8, GGUF_U16, GGUF_I16, GGUF_U32, GGUF_I32,
    GGUF_F32, GGUF_BOOL, GGUF_STR, GGUF_ARR, GGUF_U64, GGUF_I64, GGUF_F64
};

/* ggml tensor types. Only the ones this file can dequantize are listed; anything else loads as a
 * descriptor and fails loudly when read, which is better than returning plausible zeroes. */
enum {
    GGML_F32 = 0, GGML_F16 = 1, GGML_Q4_0 = 2, GGML_Q4_1 = 3,
    GGML_Q5_0 = 6, GGML_Q5_1 = 7, GGML_Q8_0 = 8, GGML_Q8_1 = 9,
    GGML_Q2_K = 10, GGML_Q3_K = 11, GGML_Q4_K = 12, GGML_Q5_K = 13,
    GGML_Q6_K = 14, GGML_Q8_K = 15
};

typedef struct {
    char    *key;
    uint32_t type;       /* one of the GGUF_* types; GGUF_ARR means see arr_type */
    uint32_t arr_type;
    uint64_t n;          /* element count for arrays, 1 otherwise */
    int64_t  i;          /* integer and bool scalars                */
    double   f;          /* float scalars                           */
    char    *s;          /* string scalars                          */
    char   **sv;         /* string arrays: n entries                 */
    void    *av;         /* numeric arrays: n elements, arr_type wide */
} gguf_kv;

typedef struct {
    char    *name;
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;     /* bytes from the start of the tensor data blob */
} gguf_tensor;

typedef struct {
    FILE        *f;
    uint32_t     version;
    uint64_t     n_tensors;
    uint64_t     n_kv;
    gguf_kv     *kv;
    gguf_tensor *t;
    uint64_t     data_start;   /* absolute byte offset of the tensor blob */
    char         err[256];
} gguf_t;

/* Open and parse the header. Returns 0 on success; on failure g->err says why. The file stays open
 * for tensor reads, so call gguf_close when done. */
int  gguf_open(gguf_t *g, const char *path);
void gguf_close(gguf_t *g);

/* Metadata lookup. The `arch` helpers prepend the architecture name, since nearly every key in a
 * GGUF file is namespaced by it -- "qwen2.block_count", not "block_count". */
const gguf_kv *gguf_find(const gguf_t *g, const char *key);
int64_t     gguf_int(const gguf_t *g, const char *key, int64_t dflt);
double      gguf_flt(const gguf_t *g, const char *key, double dflt);
const char *gguf_str(const gguf_t *g, const char *key, const char *dflt);
int64_t     gguf_arch_int(const gguf_t *g, const char *suffix, int64_t dflt);
double      gguf_arch_flt(const gguf_t *g, const char *suffix, double dflt);

/* Tensors. Names follow llama.cpp: "token_embd.weight", "blk.7.attn_q.weight", "output_norm.weight". */
const gguf_tensor *gguf_tensor_find(const gguf_t *g, const char *name);
const gguf_tensor *gguf_tensor_findf(const gguf_t *g, const char *fmt, ...);
uint64_t gguf_nelem(const gguf_tensor *t);
uint64_t gguf_nbytes(const gguf_tensor *t);
const char *gguf_type_name(uint32_t type);

/* Read one tensor and dequantize it into `out`, which must hold gguf_nelem() floats.
 * Returns 0 on success. This is the slow, correct path. */
int gguf_read_f32(gguf_t *g, const gguf_tensor *t, float *out);

/* Read only part of a tensor, in whole rows. `row0` and `n_rows` are along the LAST dimension,
 * which is how ggml stores a matrix: dims[0] is the row length.
 *
 * This exists because an embedding table is 255 MB and decode needs one 2 KB row of it. Reading the
 * whole thing to get one row is the difference between a machine that works and one that does not,
 * and it is the reason the planner can put embeddings on an SD card. */
int gguf_read_rows_f32(gguf_t *g, const gguf_tensor *t, uint64_t row0, uint64_t n_rows, float *out);

/* Read a tensor's raw bytes, no dequantization. `out` must hold gguf_nbytes().
 *
 * This is how the runtime actually loads weights. Dequantizing the whole model to float would turn
 * 1.8 GB into 12 GB, which no target here has, and would throw away the only property that makes
 * the machine possible. Weights stay quantized in memory and a row is unpacked at the moment it is
 * multiplied. */
int gguf_read_raw(gguf_t *g, const gguf_tensor *t, void *out);

/* Raw bytes for a range of rows. This is how a node loads only its slice of a tensor: the output
 * projection is split by vocabulary across nodes, so each one reads the rows it owns and never sees
 * the rest. `out` must hold n_rows * gguf_row_bytes(). */
int gguf_read_raw_rows(gguf_t *g, const gguf_tensor *t, uint64_t row0, uint64_t n_rows, void *out);

/* Bytes in one row, i.e. one output of a matrix-vector product. */
uint64_t gguf_row_bytes(const gguf_tensor *t);

/* Dequantize a raw block-quantized buffer. `n` is the element count, which must be a multiple of
 * the type's block size. Exposed separately so a node that streams weights off an SD card can
 * dequantize in place without a tensor descriptor. */
int gguf_dequant(uint32_t type, const void *raw, uint64_t n, float *out);

/* ---- the fast path: integer dot products, no float materialization ----------------------------
 *
 * gguf_dequant writes floats to memory and something else reads them back. For a matrix-vector
 * product that round trip is the whole cost: 256 floats stored and reloaded per 144 bytes of weights,
 * to be used exactly once each.
 *
 * These skip it. The ACTIVATION is quantized to int8 once per matrix, and then each row's dot product
 * runs in integers straight out of the packed nibbles, with the block scales applied once per 32
 * weights instead of once per weight. It is the same arithmetic, reassociated:
 *
 *     sum w_i x_i  =  sum (d*sc*q_i - dmin*m) * x_i  =  d*sc * sum(q_i x_i) - dmin*m * sum(x_i)
 *
 * so the inner loop is an integer multiply-accumulate over nibbles and the float work drops from
 * 256 operations to 2 per sub-block.
 *
 * Activations get a scale every 32 elements rather than one for the whole vector. Activations have
 * outliers -- one attention bias in this model reaches 106 against a standard deviation of 18 -- and a
 * single scale would crush everything else to a couple of levels. */

/* Quantize an activation vector to int8 in blocks of 32. `xq` holds n bytes, `xs` holds n/32 floats.
 * n must be a multiple of 32. */
void gguf_quantize_act(const float *x, uint64_t n, int8_t *xq, float *xs);

/* Fused dot product of one quantized row against a quantized activation vector.
 * Returns the same value gguf_dequant + a float dot would, within rounding. */
float gguf_dot_q(uint32_t type, const void *raw, const int8_t *xq, const float *xs, uint64_t n);

/* Force the portable scalar kernels even on a host that has a vector path.
 *
 * Exists so one binary can run both and prove they agree. The vector version reduces the same
 * integer products in a different order, and integer addition is associative, so the two must be
 * BIT IDENTICAL rather than merely close. A vector kernel that is only close has a bug in it. */
void gguf_dot_force_scalar(int on);

/* Which kernel this build will actually use, for reporting. */
const char *gguf_dot_kernel(void);

/* Per-ABLK sums of a quantized activation vector. `xsum` holds n/ABLK int32 values.
 *
 * Q4_K's per-block minimum needs the sum of the activations in each sub-block, and its inner loop has
 * been recomputing them for every row of every matrix. They do not depend on the weights, so in a
 * matrix-vector product they are the same for every row: compute them once here and pass them to
 * gguf_dot_q4k_presum. Measured at 3.64 of 5.57 cycles per weight in the nibble loop, most of it this. */
void gguf_act_sums(const int8_t *xq, uint64_t n, int32_t *xsum);

/* Q4_K with those sums supplied. BIT-IDENTICAL to gguf_dot_q, not merely close: the sums are the same
 * integers either way, so the float arithmetic downstream is unchanged. */
float gguf_dot_q4k_presum(const void *raw, const int8_t *xq, const float *xs,
                          const int32_t *xsum, uint64_t n);

/* One row against np activation vectors (1..GGUF_NPOS_MAX): out[p] is exactly what the single-vector call
 * returns for vector p, bit for bit. On the Cortex-M7 each group of weights is unpacked once for all np
 * vectors instead of once per vector; elsewhere these simply loop over the single-vector kernels.
 * gguf_dot_q_n is batched for Q6_K on the M7 and loops over gguf_dot_q for every other type; the batched
 * Q4_K path is gguf_dot_q4k_presum_n (its result equals gguf_dot_q's, the sums being the same integers). */
#define GGUF_NPOS_MAX 8
void gguf_dot_q4k_presum_n(const void *raw, int np, const int8_t *const *xq, const float *const *xs,
                           const int32_t *const *xsum, uint64_t n, float *out);
void gguf_dot_q_n(uint32_t type, const void *raw, int np, const int8_t *const *xq, const float *const *xs,
                  uint64_t n, float *out);

/* Q4_K with stages removed, for attributing cost. NOT a kernel to call for a result.
 *
 *   stage 0   the real thing, identical to gguf_dot_q
 *   stage 1   integer dots and the 6-bit scale unpack, no float or double arithmetic
 *   stage 2   integer dots only
 *
 * Stages 1 and 2 return a number that is not the dot product. They exist so that subtracting two
 * timings attributes time to exactly one stage, which is the only way to tell a slow scale unpack
 * (fixable while staying bit-identical) from slow double arithmetic (a numerics change). */
float gguf_dot_q4k_stage(const void *raw, const int8_t *xq, const float *xs, uint64_t n, int stage);

/* Q4_K packs eight 6-bit scales and eight 6-bit minimums into twelve bytes, asymmetrically. Exposed
 * because both the dequantizer and the fused dot need it and a second copy of this bit arithmetic
 * would drift from the first. */
void gguf_q4k_scale_min(int j, const uint8_t *q, uint8_t *d, uint8_t *m);

/* Half-precision, which GGUF uses for every block scale. */
float gguf_fp16(uint16_t h);

#ifdef __cplusplus
}
#endif
#endif /* GGUF_H */

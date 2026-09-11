/* ===========================================================================================
 *  bench_gemv.h -- the operation a transformer actually spends its life doing
 * ===========================================================================================
 *
 *  WHY THIS IS THE MEASUREMENT THAT DECIDES THE ARCHITECTURE
 *  ----------------------------------------------------------
 *  The whole thesis is that single-stream inference is memory-bandwidth bound, so a machine built
 *  from many nodes with private memory beats one with a big shared bus. That is true on a GPU,
 *  which has dedicated hardware for INT4 and INT8 dot products and can consume weights faster than
 *  any memory can supply them.
 *
 *  It is NOT automatically true here. A Cortex-A7 or an Xtensa LX7 has no quantized math unit. If
 *  a node can read 930 MB/s of weights but only multiply-accumulate through 100 MB/s of them, then
 *  it is compute bound, every bandwidth figure measured so far is irrelevant, and the argument for
 *  the architecture collapses. That is a real possibility and it has to be measured, not assumed.
 *
 *  So this measures the same quantity in the same units as bench_stream: MEGABYTES OF WEIGHTS
 *  CONSUMED PER SECOND. Put the two side by side and the answer is immediate.
 *
 *      gemv throughput ~= memory bandwidth   ->  memory bound, the thesis holds
 *      gemv throughput << memory bandwidth   ->  compute bound, the thesis needs rework
 *
 *
 *  WHAT THE OPERATION IS
 *  ----------------------
 *  y = W . x, with W a rows x cols matrix of quantized weights and x a vector of INT8 activations.
 *  That is a transformer's linear layer at batch size one, which is where nearly all the time goes
 *  during decode. Attention adds work but the linear layers dominate the weight traffic, and weight
 *  traffic is what this architecture is built around.
 *
 *  Every weight is touched exactly once per call. That is the property that makes the comparison
 *  against streaming bandwidth honest: both walk the same bytes the same number of times.
 *
 *
 *  INT4 IS PACKED TWO PER BYTE, WHICH IS THE POINT AND ALSO THE COST
 *  ------------------------------------------------------------------
 *  A 1T-parameter model at INT4 is 500 GB; at INT8 it is a terabyte. The packing is what makes the
 *  capacity arithmetic work at all. It is not free: every byte needs a shift, two sign extensions
 *  and two multiply-accumulates, where INT8 needs one of each. If INT4 comes out at less than half
 *  the byte rate of INT8, the unpacking is costing more than the halved traffic saves, and on a
 *  part with no SIMD that is a real risk rather than a theoretical one.
 *
 *  Both are measured so the tradeoff is a number instead of an opinion.
 *
 *
 *  SIGN CONVENTION
 *  ----------------
 *  INT4 nibbles are two's complement in the range -8..7. The low nibble is the even-indexed weight.
 *  Getting this wrong produces a kernel that runs at exactly the right speed and computes garbage,
 *  which is why gemv_check exists and why the host test compares INT4 against INT8 on identical
 *  values rather than trusting either alone.
 * ===========================================================================================
 */

#ifndef BENCH_GEMV_H
#define BENCH_GEMV_H

#include <stdint.h>

/* Sign-extend a 4-bit two's complement value held in the low nibble. */
static inline int32_t gemv_nib(uint8_t n)
{
    return (int32_t)((n & 0x8u) ? ((int32_t)(n & 0x0Fu) - 16) : (int32_t)(n & 0x0Fu));
}

/* y[rows] = W[rows][cols] . x[cols], weights INT8. */
static inline void gemv_int8(const int8_t *W, const int8_t *x, int32_t *y,
                      uint32_t rows, uint32_t cols)
{
    for (uint32_t r = 0; r < rows; r++) {
        const int8_t *w = W + (size_t)r * cols;
        int32_t acc = 0;
        uint32_t c = 0;
        /* Unrolled by four. Not a micro-optimisation for its own sake: without it the loop
         * overhead is comparable to the arithmetic on an in-order core, and the measurement ends
         * up reporting the branch predictor rather than the multiplier. */
        for (; c + 4u <= cols; c += 4u) {
            acc += (int32_t)w[c]     * (int32_t)x[c];
            acc += (int32_t)w[c + 1] * (int32_t)x[c + 1];
            acc += (int32_t)w[c + 2] * (int32_t)x[c + 2];
            acc += (int32_t)w[c + 3] * (int32_t)x[c + 3];
        }
        for (; c < cols; c++) acc += (int32_t)w[c] * (int32_t)x[c];
        y[r] = acc;
    }
}

/* y[rows] = W[rows][cols] . x[cols], weights INT4 packed two per byte, low nibble first.
 * `cols` must be even. */
static inline void gemv_int4(const uint8_t *W, const int8_t *x, int32_t *y,
                      uint32_t rows, uint32_t cols)
{
    const uint32_t stride = cols / 2u;
    for (uint32_t r = 0; r < rows; r++) {
        const uint8_t *w = W + (size_t)r * stride;
        int32_t acc = 0;
        for (uint32_t b = 0; b < stride; b++) {
            const uint8_t byte = w[b];
            acc += gemv_nib(byte & 0x0Fu)        * (int32_t)x[b * 2u];
            acc += gemv_nib((uint8_t)(byte >> 4)) * (int32_t)x[b * 2u + 1u];
        }
        y[r] = acc;
    }
}

/* y[rows] = W[rows][cols] . x[cols], weights INT16.
 *
 * INT16 exists here because it is what the hardware actually has. A Cortex-M7's DSP extension
 * provides SMLAD, a dual INT16 multiply-accumulate that retires two MACs per cycle, and a
 * Cortex-A7's NEON does eight. Neither part, and nothing else in this price class, has any INT4
 * hardware at all -- which is why INT4 costs a shift and two sign extensions per byte before any
 * arithmetic starts, and why it measured seven times slower per MAC than INT8 on a host with AVX2.
 *
 * The tradeoff is therefore not obvious and has to be measured per part: INT4 halves the bytes and
 * multiplies the work, INT16 doubles the bytes and matches the silicon. */
static inline void gemv_int16(const int16_t *W, const int16_t *x, int32_t *y,
                       uint32_t rows, uint32_t cols)
{
#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1)
    /* SMLAD: two 16x16 multiplies and both accumulated, in one instruction. This is the path the
     * Teensy's M7 was given a DSP extension for, and skipping it would measure the wrong machine.
     * Requires both operands 32-bit aligned, which the callers below guarantee. */
    for (uint32_t r = 0; r < rows; r++) {
        const uint32_t *w = (const uint32_t *)(const void *)(W + (size_t)r * cols);
        const uint32_t *v = (const uint32_t *)(const void *)x;
        int32_t acc = 0;
        const uint32_t pairs = cols / 2u;
        for (uint32_t p = 0; p < pairs; p++) {
            __asm__("smlad %0, %1, %2, %0" : "+r"(acc) : "r"(w[p]), "r"(v[p]));
        }
        y[r] = acc;
    }
#else
    for (uint32_t r = 0; r < rows; r++) {
        const int16_t *w = W + (size_t)r * cols;
        int32_t acc = 0;
        uint32_t c = 0;
        for (; c + 4u <= cols; c += 4u) {
            acc += (int32_t)w[c]     * (int32_t)x[c];
            acc += (int32_t)w[c + 1] * (int32_t)x[c + 1];
            acc += (int32_t)w[c + 2] * (int32_t)x[c + 2];
            acc += (int32_t)w[c + 3] * (int32_t)x[c + 3];
        }
        for (; c < cols; c++) acc += (int32_t)w[c] * (int32_t)x[c];
        y[r] = acc;
    }
#endif
}

static inline double gemv_bytes_int16(uint32_t rows, uint32_t cols)
{
    return (double)rows * (double)cols * 2.0;
}

/* Pack INT8 values into INT4 nibbles, clamping to -8..7. Used to build a matrix whose INT4 and
 * INT8 forms hold identical values, so the two kernels can be checked against each other. */
static inline void gemv_pack4(const int8_t *src, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i + 1u < n; i += 2u) {
        int32_t lo = src[i];
        int32_t hi = src[i + 1];
        if (lo > 7)  lo = 7;
        if (lo < -8) lo = -8;
        if (hi > 7)  hi = 7;
        if (hi < -8) hi = -8;
        dst[i / 2u] = (uint8_t)(((uint32_t)(lo & 0x0F)) | ((uint32_t)(hi & 0x0F) << 4));
    }
}

/* Bytes of WEIGHT touched per call, which is what makes this comparable to a streaming figure. */
static inline double gemv_bytes_int8(uint32_t rows, uint32_t cols)
{
    return (double)rows * (double)cols;
}
static inline double gemv_bytes_int4(uint32_t rows, uint32_t cols)
{
    return (double)rows * (double)cols / 2.0;
}

#endif /* BENCH_GEMV_H */

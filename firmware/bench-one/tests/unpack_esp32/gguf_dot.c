/* ===========================================================================================
 *  gguf_dot.c -- the fast path, and an honest attempt to beat this project's own headline number
 * ===========================================================================================
 *
 *  tests/decode_limit.c measured Q4_K unpacking at 5.53 GB/s against 32.49 GB/s of memory bandwidth
 *  on this host, and docs/23 concluded from that gap that 4-bit decode on a general-purpose processor
 *  is unpack bound, and that the FPGA fabric is worth 31x because nibble splitting is free in wiring.
 *
 *  That conclusion is only honest if the unpacking was implemented as well as it reasonably can be.
 *  The version it was measured with writes 256 floats to memory per 144-byte block so that a dot
 *  product can read them back once. That is not a 4-bit cost, it is a bad loop, and attributing it to
 *  the format would be talking up the FPGA with someone else's slowness.
 *
 *  So this is the same arithmetic without the round trip, and decode_limit measures both. Whatever it
 *  says, it says: if the gap closes, the FPGA argument shrinks and the design has to know that. If it
 *  holds, it holds against a real attempt instead of a straw man.
 *
 *
 *  THE REASSOCIATION
 *  ------------------
 *  A Q4_K weight is  w = d*sc*q - dmin*m,  where d and dmin are per 256 weights, sc and m are per 32,
 *  and q is the 4-bit integer. So over one sub-block of 32:
 *
 *      sum w_i x_i  =  d*sc * sum(q_i x_i)  -  dmin*m * sum(x_i)
 *
 *  Two floating-point multiplies per 32 weights instead of one per weight, and the inner loop is an
 *  integer multiply-accumulate over nibbles. Nothing is approximated; the operations are regrouped.
 *
 *
 *  WHY THE ACTIVATION IS QUANTIZED PER 32 AND NOT PER VECTOR
 *  ----------------------------------------------------------
 *  One scale for a whole 2048-wide activation would be set by its largest element. This model's
 *  attention biases reach 106 against a standard deviation of 18, and after the first residual the
 *  activations inherit that spread -- so a single scale would leave most of the vector using three or
 *  four of the 255 available levels. A scale every 32 elements costs 4 bytes per 32 and keeps the
 *  resolution where the values are. The error this introduces is measured against the float reference
 *  in tests/fast_path.c rather than assumed to be small.
 * ===========================================================================================
 */

#include "gguf.h"

#include <math.h>
#include <string.h>

#define QK_K 256
#define ABLK 32          /* activation scale block */

/* ---------------------------------------------------------------------------------------------
 *  activation quantization
 * ------------------------------------------------------------------------------------------ */

void gguf_quantize_act(const float *x, uint64_t n, int8_t *xq, float *xs)
{
    const uint64_t nb = n / ABLK;
    for (uint64_t b = 0; b < nb; b++) {
        const float *p = x + b * ABLK;
        float amax = 0.0f;
        for (int i = 0; i < ABLK; i++) {
            const float a = fabsf(p[i]);
            if (a > amax) amax = a;
        }
        /* 127 rather than 128, so the positive and negative ranges are symmetric and the scale can be
         * applied identically to both signs. A block of exact zeroes gets scale zero and quantizes to
         * zero, which is correct and avoids a division. */
        const float s = amax / 127.0f;
        xs[b] = s;
        const float inv = (s > 0.0f) ? 1.0f / s : 0.0f;
        int8_t *o = xq + b * ABLK;
        for (int i = 0; i < ABLK; i++) {
            float v = p[i] * inv;
            /* Round half away from zero, then clamp. Truncation here biases every value toward zero,
             * which across 2048 elements and 36 layers is a systematic shrink of the activation rather
             * than noise that cancels. */
            v = (v < 0.0f) ? v - 0.5f : v + 0.5f;
            int q = (int)v;
            if (q > 127) q = 127;
            if (q < -127) q = -127;
            o[i] = (int8_t)q;
        }
    }
}

/* ---------------------------------------------------------------------------------------------
 *  Q4_K
 * ------------------------------------------------------------------------------------------ */

static float dot_q4_k(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    /* DOUBLE accumulator, deliberately.
     *
     * A dot product over 11008 random signed terms is a sum about sqrt(11008) times the size of one
     * term, so the terms very nearly cancel and what survives is small relative to what went in. A
     * float running total over the 344 sub-block contributions that makes it lost 3.5e-4 of relative
     * accuracy on ffn_down, which read as a kernel bug until the accumulator was ruled out.
     *
     * One double add per 32 weights is free against the integer multiply-accumulate underneath it,
     * and it takes the agreement with the float reference to 1e-7. Paying nothing to remove a whole
     * class of doubt is the right trade. */
    double total = 0.0;

    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 144u;
        uint16_t hd, hm;
        memcpy(&hd, blk + 0, 2);
        memcpy(&hm, blk + 2, 2);
        const float d    = gguf_fp16(hd);
        const float dmin = gguf_fp16(hm);
        const uint8_t *sc_raw = blk + 4;
        const uint8_t *q      = blk + 16;
        const int8_t  *x  = xq + b * QK_K;
        const float   *sx = xs + b * (QK_K / ABLK);

        int is = 0;
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;
            int32_t dot, sum;

            /* Low nibbles of 32 bytes are elements j..j+31. Contiguous in both operands, which is
             * what lets the compiler vectorize this; the float version could not, because it had a
             * store in the middle. */
            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            dot = 0; sum = 0;
            for (int l = 0; l < 32; l++) {
                const int32_t xv = x[j + l];
                dot += (int32_t)(q[l] & 0x0F) * xv;
                sum += xv;
            }
            total += (double)sx[is + 0] * ((double)d * sc * dot - (double)dmin * m * sum);

            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            dot = 0; sum = 0;
            for (int l = 0; l < 32; l++) {
                const int32_t xv = x[j + 32 + l];
                dot += (int32_t)(q[l] >> 4) * xv;
                sum += xv;
            }
            total += (double)sx[is + 1] * ((double)d * sc * dot - (double)dmin * m * sum);

            q += 32;
            is += 2;
        }
    }
    return (float)total;
}

/* ---------------------------------------------------------------------------------------------
 *  Q6_K
 * ------------------------------------------------------------------------------------------ */

/* Q6_K's elements are not laid out in order. Within each 128-element half, byte l of the low-nibble
 * array serves elements l, l+32, l+64 and l+96, taking two more bits each from one byte of the
 * high-bit array. So four accumulators run at once, each over a different 32-element stride, and each
 * of those 32 spans two 16-weight scale groups. The indexing is transcribed from the dequantizer
 * deliberately: this is the part where being clever produces a wrong answer that looks right. */
static float dot_q6_k(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    double total = 0.0;   /* see the note in dot_q4_k */

    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 210u;
        const uint8_t *ql  = blk;
        const uint8_t *qh  = blk + 128;
        const int8_t  *sc  = (const int8_t *)(blk + 192);
        uint16_t hd;
        memcpy(&hd, blk + 208, 2);
        const float d = gguf_fp16(hd);
        const int8_t *x  = xq + b * QK_K;
        const float  *sx = xs + b * (QK_K / ABLK);

        for (int n128 = 0; n128 < QK_K; n128 += 128) {
            /* [offset][scale group] : offsets 0,32,64,96 and the two 16-element halves of each. */
            /* No running sum of activations here, unlike Q4_K: Q6_K has no per-block minimum, so
             * the -32 bias folds into q itself and there is nothing left to correct for. */
            int32_t dot[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };

            for (int l = 0; l < 32; l++) {
                const int h = qh[l];
                const int g = l >> 4;                      /* which 16-weight scale group */
                const int q1 = (int)((ql[l]      & 0x0F) | (((h >> 0) & 3) << 4)) - 32;
                const int q2 = (int)((ql[l + 32] & 0x0F) | (((h >> 2) & 3) << 4)) - 32;
                const int q3 = (int)((ql[l]      >>   4) | (((h >> 4) & 3) << 4)) - 32;
                const int q4 = (int)((ql[l + 32] >>   4) | (((h >> 6) & 3) << 4)) - 32;
                const int32_t x1 = x[n128 + l];
                const int32_t x2 = x[n128 + l + 32];
                const int32_t x3 = x[n128 + l + 64];
                const int32_t x4 = x[n128 + l + 96];
                dot[0][g] += q1 * x1;
                dot[1][g] += q2 * x2;
                dot[2][g] += q3 * x3;
                dot[3][g] += q4 * x4;
            }

            for (int o = 0; o < 4; o++) {
                /* Activation scale block: 32 contiguous elements starting at n128 + 32*o. */
                const float s = sx[(n128 + 32 * o) / ABLK];
                for (int g = 0; g < 2; g++)
                    total += (double)s * d * sc[2 * o + g] * dot[o][g];
            }
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
    return (float)total;
}

/* ---------------------------------------------------------------------------------------------
 *  Q8_0 and the dispatcher
 * ------------------------------------------------------------------------------------------ */

static float dot_q8_0(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / 32;
    double total = 0.0;   /* see the note in dot_q4_k */
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 34u;
        uint16_t hd;
        memcpy(&hd, blk, 2);
        const float d = gguf_fp16(hd);
        const int8_t *q = (const int8_t *)(blk + 2);
        const int8_t *x = xq + b * 32;
        int32_t dot = 0;
        for (int l = 0; l < 32; l++) dot += (int32_t)q[l] * (int32_t)x[l];
        total += (double)d * xs[b] * dot;
    }
    return (float)total;
}

float gguf_dot_q(uint32_t type, const void *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    switch (type) {
    case GGML_Q4_K: return dot_q4_k((const uint8_t *)raw, xq, xs, n);
    case GGML_Q6_K: return dot_q6_k((const uint8_t *)raw, xq, xs, n);
    case GGML_Q8_0: return dot_q8_0((const uint8_t *)raw, xq, xs, n);
    default: {
        /* No fused kernel for this format. Falling back silently would make a benchmark of the fast
         * path secretly measure the slow one, so this returns a NaN the caller cannot mistake for a
         * result. Formats that matter here are covered. */
        float z = 0.0f;
        return z / z;
    }
    }
}

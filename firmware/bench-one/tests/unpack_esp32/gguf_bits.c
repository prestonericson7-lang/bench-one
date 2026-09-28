/* ===========================================================================================
 *  gguf_bits.c -- the format's bit arithmetic, with no file code attached
 * ===========================================================================================
 *
 *  Half-precision conversion and Q4_K's packed sub-block scales are needed by two very different
 *  callers: the loader, which reads files, and the dot kernels, which run on a microcontroller that
 *  has no filesystem and no business linking one in.
 *
 *  They used to live in gguf.c beside fopen and fseek, which meant a Teensy sketch that only wanted to
 *  multiply nibbles had to drag stdio in behind it. Splitting them out costs nothing and keeps the one
 *  copy of the bit arithmetic that everything else agrees with -- a second copy for the embedded side
 *  would drift, and drift in this particular arithmetic produces plausible wrong weights rather than
 *  an error.
 * ===========================================================================================
 */

/* Round like every other machine does. GCC on ARM fuses a*b+c into one multiply-add unless told not to,
 * which rounds once where the PC rounds twice; the Teensy's v5 image has 23 fused operations in this file
 * and gguf_bits.c (gguf_dot_q 13, gguf_dot_q4k_presum 4, gguf_dot_q4k_stage 4, gguf_dequant 2 --
 * bench-archive/20260927-060317 psram_llm.ino.lst). Off, so the double arithmetic below gives the
 * same bits on the M7 as on the PC. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("fp-contract=off")
#endif
#include "gguf.h"
#include <string.h>

/* Elements per K-quant block. Every Q*_K layout in this file is built on it. */
#define QK_K 256

/* Done in integer arithmetic rather than with a _Float16 cast, because the targets are a Cortex-M7
 * without half-precision hardware, a Cortex-A7, and a host, and three different compilers agreeing
 * is worth more than one of them being fast. Block scales are read once per 256 weights. */
float gguf_fp16(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t man  = h & 0x3FFu;
    uint32_t bits;

    if (exp == 0) {
        if (man == 0) {
            bits = sign;                     /* +-0 */
        } else {
            /* Subnormal half: renormalise into a normal float by shifting the mantissa up until
             * its leading one falls off, and charging each shift to the exponent. */
            uint32_t m = man, e = 0;
            while (!(m & 0x400u)) { m <<= 1; e++; }
            m &= 0x3FFu;
            bits = sign | ((127u - 15u - e + 1u) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);   /* inf and NaN keep their payload */
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }

    float out;
    memcpy(&out, &bits, 4);
    return out;
}

/* Q4_K sub-block scales and minimums: eight 6-bit pairs packed into twelve bytes.
 *
 * The first four pairs are plain: six bits of scale in bytes 0..3, six bits of minimum in bytes
 * 4..7. The last four are split -- low four bits in bytes 8..11, high two bits stolen from the top
 * of the first eight bytes. It is not symmetric and it is not guessable, which is exactly why this
 * is a named function with the bit positions written out rather than inlined arithmetic. */
void gguf_q4k_scale_min(int j, const uint8_t *q, uint8_t *d, uint8_t *m)
{
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (uint8_t)((q[j + 4] & 0x0F) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4)   | ((q[j]     >> 6) << 4));
    }
}

/* ---------------------------------------------------------------------------------------------
 *  dequantization
 *
 *  Pure format arithmetic: bytes in, floats out, no file and no descriptor. It lived next to fopen
 *  because that is where the loader needed it, which meant a Teensy sketch wanting to unpack a block
 *  had to link stdio to get here. Nothing about turning a nibble into a number needs a filesystem.
 * ------------------------------------------------------------------------------------------ */

static void deq_q4_k(const uint8_t *raw, uint64_t n, float *y)
{
    const uint64_t nb = n / QK_K;
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 144u;
        uint16_t hd, hm;
        memcpy(&hd, blk + 0, 2);
        memcpy(&hm, blk + 2, 2);
        const float d    = gguf_fp16(hd);
        const float dmin = gguf_fp16(hm);
        const uint8_t *sc_raw = blk + 4;     /* 12 bytes of packed 6-bit scales and minimums */
        const uint8_t *q      = blk + 16;    /* 128 bytes = 256 nibbles                      */

        int is = 0;
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;
            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            const float d1 = d * sc, m1 = dmin * m;
            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            const float d2 = d * sc, m2 = dmin * m;
            /* Low nibbles of 32 bytes are the first 32 weights, high nibbles the next 32 --
             * interleaved by nibble, not by byte. */
            for (int l = 0; l < 32; l++) *y++ = d1 * (float)(q[l] & 0x0F) - m1;
            for (int l = 0; l < 32; l++) *y++ = d2 * (float)(q[l] >> 4)   - m2;
            q += 32;
            is += 2;
        }
    }
}

static void deq_q6_k(const uint8_t *raw, uint64_t n, float *y)
{
    const uint64_t nb = n / QK_K;
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 210u;
        const uint8_t *ql  = blk;             /* 128 bytes: low 4 bits            */
        const uint8_t *qh  = blk + 128;       /*  64 bytes: high 2 bits           */
        const int8_t  *sc  = (const int8_t *)(blk + 192); /* 16 signed scales     */
        uint16_t hd;
        memcpy(&hd, blk + 208, 2);
        const float d = gguf_fp16(hd);

        /* Six bits per weight, assembled from two arrays, then biased by -32 to make it signed.
         * Four weights 32 apart share one qh byte, taking two bits each. */
        for (int n128 = 0; n128 < QK_K; n128 += 128) {
            float *yy = y + n128;
            for (int l = 0; l < 32; l++) {
                const int is = l / 16;
                const int q1 = (int)((ql[l]      & 0x0F) | (((qh[l] >> 0) & 3) << 4)) - 32;
                const int q2 = (int)((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) - 32;
                const int q3 = (int)((ql[l]      >>   4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                const int q4 = (int)((ql[l + 32] >>   4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                yy[l]      = d * (float)sc[is + 0] * (float)q1;
                yy[l + 32] = d * (float)sc[is + 2] * (float)q2;
                yy[l + 64] = d * (float)sc[is + 4] * (float)q3;
                yy[l + 96] = d * (float)sc[is + 6] * (float)q4;
            }
            ql += 64;
            qh += 32;
            sc += 8;
        }
        y += QK_K;
    }
}

static void deq_q8_0(const uint8_t *raw, uint64_t n, float *y)
{
    const uint64_t nb = n / 32;
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 34u;
        uint16_t hd;
        memcpy(&hd, blk, 2);
        const float d = gguf_fp16(hd);
        const int8_t *q = (const int8_t *)(blk + 2);
        for (int l = 0; l < 32; l++) *y++ = d * (float)q[l];
    }
}

static void deq_q4_0(const uint8_t *raw, uint64_t n, float *y)
{
    const uint64_t nb = n / 32;
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 18u;
        uint16_t hd;
        memcpy(&hd, blk, 2);
        const float d = gguf_fp16(hd);
        const uint8_t *q = blk + 2;
        /* Same nibble interleave as Q4_K: element l and element l+16 share byte l. */
        for (int l = 0; l < 16; l++) {
            y[l]      = d * (float)((int)(q[l] & 0x0F) - 8);
            y[l + 16] = d * (float)((int)(q[l] >> 4)   - 8);
        }
        y += 32;
    }
}

int gguf_dequant(uint32_t type, const void *raw, uint64_t n, float *out)
{
    const uint8_t *p = (const uint8_t *)raw;
    switch (type) {
    case GGML_F32:
        memcpy(out, p, (size_t)n * 4);
        return 0;
    case GGML_F16: {
        for (uint64_t i = 0; i < n; i++) {
            uint16_t h;
            memcpy(&h, p + i * 2, 2);
            out[i] = gguf_fp16(h);
        }
        return 0;
    }
    case GGML_Q4_0: if (n % 32)   return -1; deq_q4_0(p, n, out); return 0;
    case GGML_Q8_0: if (n % 32)   return -1; deq_q8_0(p, n, out); return 0;
    case GGML_Q4_K: if (n % QK_K) return -1; deq_q4_k(p, n, out); return 0;
    case GGML_Q6_K: if (n % QK_K) return -1; deq_q6_k(p, n, out); return 0;
    default:
        return -1;
    }
}

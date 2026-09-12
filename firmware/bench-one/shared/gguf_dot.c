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

/* AVX2, where the host has it.
 *
 * This was written because the runtime was measured at 3.37 tokens/s on Qwen3-30B where llama.cpp
 * on the same eight cores managed 13.99. A 4.2x gap on identical weights is not an algorithm
 * difference, it is a kernel that never left scalar code. And the gap mattered beyond speed:
 * decode_limit reported decode as unpacking bound by 3.0x against a SCALAR unpacking ceiling, which
 * is a conclusion about this kernel rather than about the format. */
#if defined(__AVX2__)
  #include <immintrin.h>
  #define GGUF_HAVE_AVX2 1
#else
  #define GGUF_HAVE_AVX2 0
#endif

/* NEON, for the processors that are actually in the machine.
 *
 * Every weight-carrying node in BENCH ONE is ARM or FPGA fabric: the Zynq's Cortex-A9, the Luckfox's
 * Cortex-A7, the Lyra. None of them has AVX2, so the x86 path above does nothing for the unit. This
 * is the one that decides the per-node figure, and through that the unit's capacity per watt.
 *
 * Cortex-A7 and A9 are ARMv7-A with NEON and no sdot, so the widening multiply is vmull_s8 into
 * int16 and vpadalq_s16 to accumulate into int32. Both operands are kept inside int8: the Q4_K
 * nibble is 0..15 and the Q6_K value is biased to 0..63, so the largest product is 63 * 127 = 8001
 * and the int16 intermediate cannot overflow. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  #include <arm_neon.h>
  #define GGUF_HAVE_NEON 1

  static inline int32_t hsum_i32_4n(int32x4_t v)
  {
      int32x2_t s = vadd_s32(vget_low_s32(v), vget_high_s32(v));
      s = vpadd_s32(s, s);
      return vget_lane_s32(s, 0);
  }

  /* 16 weights against 16 signed activations: the dot, and the plain sum of the activations. */
  static inline void dot16_neon(int8x16_t w, int8x16_t x, int32_t *dot, int32_t *sum)
  {
      int32x4_t d = vdupq_n_s32(0);
      d = vpadalq_s16(d, vmull_s8(vget_low_s8(w),  vget_low_s8(x)));
      d = vpadalq_s16(d, vmull_s8(vget_high_s8(w), vget_high_s8(x)));
      *dot = hsum_i32_4n(d);
      *sum = hsum_i32_4n(vpadalq_s16(vdupq_n_s32(0), vpaddlq_s8(x)));
  }
#else
  #define GGUF_HAVE_NEON 0
#endif

/* Cortex-M7, which is every Teensy in the machine and has no NEON.
 *
 * The block above covers the Cortex-A parts. The Teensy 4.1 is a Cortex-M7: ARMv7E-M, no NEON, and
 * therefore it has been running the scalar reference this whole time. That is where this project's
 * headline 39.3 MB/s comes from, and it is the figure the FPGA argument is built on, so it is worth
 * knowing what the part can actually do.
 *
 * It has the DSP extensions, and they fit this kernel unusually well. SXTB16 takes a word and spreads
 * two of its four bytes into two sign-extended 16-bit lanes -- bytes 0 and 2 plainly, bytes 1 and 3
 * with a rotate folded into the same instruction. SMLAD then multiplies two such lanes pairwise and
 * adds both products to an accumulator in one instruction.
 *
 * The reason it fits so well is that BOTH operands get the same treatment. A word of four packed
 * nibbles and a word of four int8 activations, each split by SXTB16, come out paired correctly with no
 * shuffling and no precomputed activation table: lanes {0,2} of the weights meet lanes {0,2} of the
 * activations. Four weights, four multiply-accumulates, two instructions.
 *
 * The high nibbles need no separate extraction path either. Shifting the whole word right by four
 * moves each byte's high nibble into its own low-nibble position -- the bits that cross the byte
 * boundary land in the high nibble of the byte below and are removed by the same 0x0F0F0F0F mask that
 * the low-nibble case uses. One shift serves all four.
 *
 * BIT-IDENTICAL, and not by luck. dot and sum are sums of int32 products, integer addition is exact
 * and associative, so reordering them changes nothing. Everything outside the two inner loops --
 * including the double accumulator and the order of the per-sub-block float arithmetic -- is the
 * reference's, untouched. gguf_dot_force_scalar(1) switches the reference back on at runtime so the
 * two can be compared on the board rather than assumed equal. */
#if !GGUF_HAVE_AVX2 && !GGUF_HAVE_NEON && defined(__ARM_FEATURE_DSP)
  #define GGUF_HAVE_M7DSP 1

  /* alignment-safe: q is blk+16 and blk is a multiple of 144, so both are 4-aligned when the caller's
   * buffer is, but a kernel in shared code should not depend on a caller's luck. GCC turns this into
   * a single LDR, which on a Cortex-M7 handles an unaligned address by itself. */
  static inline uint32_t gd_ld32(const void *p)
  {
      uint32_t v; __builtin_memcpy(&v, p, 4); return v;
  }
  static inline uint32_t gd_sxtb16(uint32_t x)
  {
      uint32_t r; __asm__("sxtb16 %0, %1" : "=r"(r) : "r"(x)); return r;
  }
  static inline uint32_t gd_sxtb16r8(uint32_t x)
  {
      uint32_t r; __asm__("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x)); return r;
  }
  static inline int32_t gd_smlad(uint32_t a, uint32_t b, int32_t acc)
  {
      int32_t r; __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc)); return r;
  }

  static inline uint32_t gd_ssub8(uint32_t a, uint32_t b)
  {
      uint32_t r; __asm__("ssub8 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); return r;
  }

  /* Q6_K, one 16-weight scale group.
   *
   * A Q6_K value is six bits split across two arrays: four bits in ql and two in qh, then biased by
   * -32. Assembling it in packed bytes needs one extra step over Q4_K and one instruction that Q4_K
   * did not: the bias cannot be applied with an ordinary subtract, because a byte lane whose value is
   * below 32 would borrow into the lane above it. SSUB8 subtracts all four lanes independently, which
   * is exactly the operation the format wants and it is a single instruction.
   *
   * The two shifts compose the value without any per-byte work. Shifting the ql word right by 0 or 4
   * and masking with 0x0F0F0F0F selects the low or high nibble of every byte at once, for the reason
   * set out above gd_dot32_m7. Shifting the qh word right by 0, 2, 4 or 6 and masking with 0x03030303
   * selects one of the four bit-pairs of every byte at once, by the same argument: the bits that cross
   * a byte boundary land above the mask.
   *
   * The -32 does NOT factor out the way Q4_K's zero point did, and deliberately so. It could -- the
   * sum of (u-32)*x is the sum of u*x minus 32 times the sum of x -- but that trades one SSUB8 per four
   * weights for a running sum of activations, which costs two more SMLAD per four weights. The
   * identity is a win when the correction is already needed, as it is in Q4_K where the block minimum
   * demands a sum of activations anyway, and a loss when it is not. Q6_K has no per-block minimum, so
   * nothing else needs that sum and the bias stays in place. */
  static inline int32_t gd_q6_dot16(const uint8_t *ql, const uint8_t *qh, const int8_t *x,
                                    uint32_t qlsh, uint32_t hsh)
  {
      int32_t a0 = 0, a1 = 0;
      for (int l = 0; l < 16; l += 4) {
          const uint32_t lo = (gd_ld32(ql + l) >> qlsh) & 0x0F0F0F0Fu;
          const uint32_t hi = ((gd_ld32(qh + l) >> hsh) & 0x03030303u) << 4;
          const uint32_t q  = gd_ssub8(lo | hi, 0x20202020u);    /* six-bit value, biased by -32 */
          const uint32_t xv = gd_ld32(x + l);
          a0 = gd_smlad(gd_sxtb16(q),   gd_sxtb16(xv),   a0);
          a1 = gd_smlad(gd_sxtb16r8(q), gd_sxtb16r8(xv), a1);
      }
      return a0 + a1;
  }

  /* One 32-weight sub-block. shift is 0 for the low nibbles and 4 for the high ones.
   *
   * Two accumulators per quantity rather than one, so the SMLAD chain does not serialise on a single
   * register: the M7 issues these back to back only if consecutive instructions do not depend on each
   * other. The sums are combined at the end, which is free and still exact. */
  static inline void gd_dot32_m7(const uint8_t *q, const int8_t *x, uint32_t shift,
                                 int32_t *dot, int32_t *sum)
  {
      int32_t d0 = 0, d1 = 0, s0 = 0, s1 = 0;
      for (int l = 0; l < 32; l += 4) {
          const uint32_t qv = (gd_ld32(q + l) >> shift) & 0x0F0F0F0Fu;
          const uint32_t xv = gd_ld32(x + l);
          const uint32_t qa = gd_sxtb16(qv), qb = gd_sxtb16r8(qv);
          const uint32_t xa = gd_sxtb16(xv), xb = gd_sxtb16r8(xv);
          d0 = gd_smlad(qa, xa, d0);
          d1 = gd_smlad(qb, xb, d1);
          s0 = gd_smlad(xa, 0x00010001u, s0);   /* sum of activations, same split, times one */
          s1 = gd_smlad(xb, 0x00010001u, s1);
      }
      *dot = d0 + d1;
      *sum = s0 + s1;
  }
#else
  #define GGUF_HAVE_M7DSP 0
#endif

static int g_force_scalar = 0;
void gguf_dot_force_scalar(int on) { g_force_scalar = on ? 1 : 0; }

const char *gguf_dot_kernel(void)
{
#if GGUF_HAVE_AVX2
    return g_force_scalar ? "scalar (AVX2 available, forced off)" : "AVX2";
#elif GGUF_HAVE_NEON
    return g_force_scalar ? "scalar (NEON available, forced off)" : "NEON";
#elif GGUF_HAVE_M7DSP
    return g_force_scalar ? "scalar (Cortex-M7 DSP available, forced off)" : "Cortex-M7 DSP";
#else
    return "scalar";
#endif
}

#if GGUF_HAVE_AVX2
/* Sum of eight int32 lanes. Exact, so the order it adds them in cannot change the answer. */
static inline int32_t hsum_i32_8(__m256i v)
{
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(1, 0, 3, 2)));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(s);
}

static inline int32_t hsum_i32_4(__m128i v)
{
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(v);
}

/* dot of 32 unsigned nibbles against 32 signed int8, and the plain sum of those int8.
 *
 * maddubs multiplies unsigned by signed and adds ADJACENT PAIRS into int16. The largest pair here
 * is 2 * 15 * 127 = 3810, so it cannot saturate -- which is the one thing to check before using
 * that instruction, because saturation is silent. madd_epi16 then widens pairs to int32, and from
 * there nothing can overflow either. */
static inline void dot32_u4(__m256i nib, __m256i xv, int32_t *dot, int32_t *sum)
{
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i ones8  = _mm256_set1_epi8(1);
    *dot = hsum_i32_8(_mm256_madd_epi16(_mm256_maddubs_epi16(nib, xv), ones16));
    *sum = hsum_i32_8(_mm256_madd_epi16(_mm256_maddubs_epi16(ones8, xv), ones16));
}
#endif

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

static float dot_q4_k_ref(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
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

#if GGUF_HAVE_AVX2
/* The same kernel with the two inner 32-element loops replaced by four instructions each.
 *
 * Everything outside those loops is untouched, including the double accumulator, so this produces
 * bit-identical output to dot_q4_k_ref. There is no gguf_dot_check -- that name was in this
 * comment and never in the code. The real mechanism is gguf_dot_force_scalar(1), which
 * switches the reference back on at runtime so a caller can run both over the same bytes and
 * compare. tests/fast_path.c does that on the host; unpack_teensy does it on the board. */
static float dot_q4_k_avx2(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    const __m256i lowmask = _mm256_set1_epi8(0x0F);
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
            const __m256i qb = _mm256_loadu_si256((const __m256i *)q);
            uint8_t sc, m;
            int32_t dot, sum;

            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            dot32_u4(_mm256_and_si256(qb, lowmask),
                     _mm256_loadu_si256((const __m256i *)(x + j)), &dot, &sum);
            total += (double)sx[is + 0] * ((double)d * sc * dot - (double)dmin * m * sum);

            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            dot32_u4(_mm256_and_si256(_mm256_srli_epi16(qb, 4), lowmask),
                     _mm256_loadu_si256((const __m256i *)(x + j + 32)), &dot, &sum);
            total += (double)sx[is + 1] * ((double)d * sc * dot - (double)dmin * m * sum);

            q += 32;
            is += 2;
        }
    }
    return (float)total;
}
#endif

#if GGUF_HAVE_NEON
/* Bit-identical to dot_q4_k_ref: same integer products, same double accumulation order. */
static float dot_q4_k_neon(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    const uint8x16_t lowmask = vdupq_n_u8(0x0F);
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
            const uint8x16_t q0 = vld1q_u8(q), q1 = vld1q_u8(q + 16);
            uint8_t sc, m;
            int32_t d0, d1, s0, s1;

            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            dot16_neon(vreinterpretq_s8_u8(vandq_u8(q0, lowmask)), vld1q_s8(x + j), &d0, &s0);
            dot16_neon(vreinterpretq_s8_u8(vandq_u8(q1, lowmask)), vld1q_s8(x + j + 16), &d1, &s1);
            total += (double)sx[is + 0] *
                     ((double)d * sc * (d0 + d1) - (double)dmin * m * (s0 + s1));

            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            dot16_neon(vreinterpretq_s8_u8(vshrq_n_u8(q0, 4)), vld1q_s8(x + j + 32), &d0, &s0);
            dot16_neon(vreinterpretq_s8_u8(vshrq_n_u8(q1, 4)), vld1q_s8(x + j + 48), &d1, &s1);
            total += (double)sx[is + 1] *
                     ((double)d * sc * (d0 + d1) - (double)dmin * m * (s0 + s1));

            q += 32;
            is += 2;
        }
    }
    return (float)total;
}
#endif

#if GGUF_HAVE_M7DSP
/* The reference with its two inner 32-element loops replaced by eight instructions per four weights.
 * Everything else is copied from it unchanged, deliberately, including the double accumulator. */
static float dot_q4_k_m7(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
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

            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            gd_dot32_m7(q, x + j, 0, &dot, &sum);
            total += (double)sx[is + 0] * ((double)d * sc * dot - (double)dmin * m * sum);

            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            gd_dot32_m7(q, x + j + 32, 4, &dot, &sum);
            total += (double)sx[is + 1] * ((double)d * sc * dot - (double)dmin * m * sum);

            q += 32;
            is += 2;
        }
    }
    return (float)total;
}
#endif

static float dot_q4_k(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
#if GGUF_HAVE_AVX2
    if (!g_force_scalar) return dot_q4_k_avx2(raw, xq, xs, n);
#elif GGUF_HAVE_NEON
    if (!g_force_scalar) return dot_q4_k_neon(raw, xq, xs, n);
#elif GGUF_HAVE_M7DSP
    if (!g_force_scalar) return dot_q4_k_m7(raw, xq, xs, n);
#endif
    return dot_q4_k_ref(raw, xq, xs, n);
}

/* ---------------------------------------------------------------------------------------------
 *  Q6_K
 * ------------------------------------------------------------------------------------------ */

/* Q6_K's elements are not laid out in order. Within each 128-element half, byte l of the low-nibble
 * array serves elements l, l+32, l+64 and l+96, taking two more bits each from one byte of the
 * high-bit array. So four accumulators run at once, each over a different 32-element stride, and each
 * of those 32 spans two 16-weight scale groups. The indexing is transcribed from the dequantizer
 * deliberately: this is the part where being clever produces a wrong answer that looks right. */
static float dot_q6_k_ref(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
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

#if GGUF_HAVE_AVX2
/* 32 weights against 32 activations, reduced into the TWO 16-element scale groups separately.
 *
 * maddubs works inside each 128-bit half, so int16 lanes 0..7 come from bytes 0..15 and lanes 8..15
 * from bytes 16..31. That lane split is exactly the scale-group split, which is why the two groups
 * fall out of one instruction instead of needing a shuffle. */
static inline void dot32_u6_split(__m256i qu, __m256i xv, int32_t d[2], int32_t sm[2])
{
    const __m256i ones8  = _mm256_set1_epi8(1);
    const __m128i ones16 = _mm_set1_epi16(1);
    const __m256i p = _mm256_maddubs_epi16(qu, xv);
    const __m256i t = _mm256_maddubs_epi16(ones8, xv);
    d[0]  = hsum_i32_4(_mm_madd_epi16(_mm256_castsi256_si128(p), ones16));
    d[1]  = hsum_i32_4(_mm_madd_epi16(_mm256_extracti128_si256(p, 1), ones16));
    sm[0] = hsum_i32_4(_mm_madd_epi16(_mm256_castsi256_si128(t), ones16));
    sm[1] = hsum_i32_4(_mm_madd_epi16(_mm256_extracti128_si256(t, 1), ones16));
}

/* Q6_K, vectorized.
 *
 * Two things make this harder than Q4_K and both are solved rather than worked around.
 *
 * The weights are strided: byte l of the low array serves elements l, l+32, l+64 and l+96. But the
 * ACTIVATIONS those four groups multiply are each 32 contiguous values, so the stride lives only on
 * the weight side and is undone with shifts and masks on whole 32-byte vectors.
 *
 * The weights are signed, -32..31, and maddubs needs its first operand unsigned. So the -32 bias is
 * left folded in, the product is taken against the unsigned 0..63 value, and 32 times the sum of the
 * activations is subtracted afterwards. That is the same identity Q4_K already uses for its block
 * minimum, and it is exact: no rounding enters anywhere.
 *
 * Bit-identical to dot_q6_k_ref. Every term is an integer and the double accumulations happen in the
 * same order, so there is nothing left for the two to disagree about. */
static float dot_q6_k_avx2(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const __m256i m3 = _mm256_set1_epi8(0x03);
    double total = 0.0;

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
            const __m256i l0 = _mm256_loadu_si256((const __m256i *)ql);
            const __m256i l1 = _mm256_loadu_si256((const __m256i *)(ql + 32));
            const __m256i h  = _mm256_loadu_si256((const __m256i *)qh);

            /* srli_epi16 then mask is how a per-byte shift is done without a per-byte shift
             * instruction: bits that cross the byte boundary are removed by the mask. */
            __m256i qu[4];
            qu[0] = _mm256_or_si256(_mm256_and_si256(l0, m4),
                    _mm256_slli_epi16(_mm256_and_si256(h, m3), 4));
            qu[1] = _mm256_or_si256(_mm256_and_si256(l1, m4),
                    _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 2), m3), 4));
            qu[2] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l0, 4), m4),
                    _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 4), m3), 4));
            qu[3] = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(l1, 4), m4),
                    _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(h, 6), m3), 4));

            for (int o = 0; o < 4; o++) {
                int32_t dd[2], sm[2];
                dot32_u6_split(qu[o],
                               _mm256_loadu_si256((const __m256i *)(x + n128 + 32 * o)), dd, sm);
                const float s = sx[(n128 + 32 * o) / ABLK];
                for (int g = 0; g < 2; g++)
                    total += (double)s * d * sc[2 * o + g] * (dd[g] - 32 * sm[g]);
            }
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
    return (float)total;
}
#endif

#if GGUF_HAVE_NEON
/* Q6_K on NEON, and it lands more neatly here than on AVX2: vshrq_n_u8 is a genuine per-byte shift,
 * so no mask is needed to clear bits that crossed a byte boundary, and a 16-byte vector IS one
 * 16-weight scale group, so the two groups need no lane shuffling at all.
 *
 * The -32 bias stays folded into the weight and 32 times the sum of the activations is subtracted
 * afterwards, exactly as the x86 version does, which keeps both operands inside int8. */
static float dot_q6_k_neon(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    const uint8x16_t m4 = vdupq_n_u8(0x0F), m3 = vdupq_n_u8(0x03);
    double total = 0.0;

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
            const uint8x16_t la = vld1q_u8(ql),      lb = vld1q_u8(ql + 16);
            const uint8x16_t lc = vld1q_u8(ql + 32), ld = vld1q_u8(ql + 48);
            const uint8x16_t ha = vld1q_u8(qh),      hb = vld1q_u8(qh + 16);

            uint8x16_t w[4][2];
            w[0][0] = vorrq_u8(vandq_u8(la, m4), vshlq_n_u8(vandq_u8(ha, m3), 4));
            w[0][1] = vorrq_u8(vandq_u8(lb, m4), vshlq_n_u8(vandq_u8(hb, m3), 4));
            w[1][0] = vorrq_u8(vandq_u8(lc, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(ha, 2), m3), 4));
            w[1][1] = vorrq_u8(vandq_u8(ld, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 2), m3), 4));
            w[2][0] = vorrq_u8(vshrq_n_u8(la, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(ha, 4), m3), 4));
            w[2][1] = vorrq_u8(vshrq_n_u8(lb, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 4), m3), 4));
            w[3][0] = vorrq_u8(vshrq_n_u8(lc, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(ha, 6), m3), 4));
            w[3][1] = vorrq_u8(vshrq_n_u8(ld, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 6), m3), 4));

            for (int o = 0; o < 4; o++) {
                const float s = sx[(n128 + 32 * o) / ABLK];
                for (int g = 0; g < 2; g++) {
                    int32_t dd, sm;
                    dot16_neon(vreinterpretq_s8_u8(w[o][g]),
                               vld1q_s8(x + n128 + 32 * o + 16 * g), &dd, &sm);
                    total += (double)s * d * sc[2 * o + g] * (dd - 32 * sm);
                }
            }
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
    return (float)total;
}
#endif

#if GGUF_HAVE_M7DSP
/* Bit-identical to dot_q6_k_ref. All eight integer dots are computed before any float arithmetic runs,
 * so the double accumulation happens in exactly the reference's order -- offset 0 to 3, and within each
 * offset scale group 0 then 1. Reordering the integer products inside a dot is free because integer
 * addition is exact; reordering the double sum would not be. */
static float dot_q6_k_m7(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
    const uint64_t nb = n / QK_K;
    double total = 0.0;

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
            int32_t dot[4][2];
            /* the four offsets are {ql base, ql nibble, qh bit-pair, activation offset} */
            static const uint8_t QLOFF[4] = {  0, 32,  0, 32 };
            static const uint8_t QLSH[4]  = {  0,  0,  4,  4 };
            static const uint8_t HSH[4]   = {  0,  2,  4,  6 };

            for (int o = 0; o < 4; o++) {
                const uint8_t *qlb = ql + QLOFF[o];
                const int8_t  *xb  = x + n128 + 32 * o;
                /* scale group 0 is l = 0..15, group 1 is l = 16..31 */
                dot[o][0] = gd_q6_dot16(qlb,      qh,      xb,      QLSH[o], HSH[o]);
                dot[o][1] = gd_q6_dot16(qlb + 16, qh + 16, xb + 16, QLSH[o], HSH[o]);
            }

            for (int o = 0; o < 4; o++) {
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
#endif

static float dot_q6_k(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n)
{
#if GGUF_HAVE_AVX2
    if (!g_force_scalar) return dot_q6_k_avx2(raw, xq, xs, n);
#elif GGUF_HAVE_NEON
    if (!g_force_scalar) return dot_q6_k_neon(raw, xq, xs, n);
#elif GGUF_HAVE_M7DSP
    if (!g_force_scalar) return dot_q6_k_m7(raw, xq, xs, n);
#endif
    return dot_q6_k_ref(raw, xq, xs, n);
}

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

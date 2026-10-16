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

/* Round like every other machine does. GCC on ARM fuses a*b+c into one multiply-add unless told not to,
 * which rounds once where the PC rounds twice; the Teensy's v5 image has 23 fused operations in this file
 * and gguf_bits.c (gguf_dot_q 13, gguf_dot_q4k_presum 4, gguf_dot_q4k_stage 4, gguf_dequant 2 --
 * bench-archive/20260927-060317 psram_llm.ino.lst). Off, so the double arithmetic below gives the
 * same bits on the M7 as on the PC. */
#if defined(__arm__) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("fp-contract=off")
#endif
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
#if !GGUF_HAVE_AVX2 && !GGUF_HAVE_NEON && (defined(__ARM_FEATURE_DSP) || defined(GD_EMULATE_M7))
  #define GGUF_HAVE_M7DSP 1

  /* alignment-safe: q is blk+16 and blk is a multiple of 144, so both are 4-aligned when the caller's
   * buffer is, but a kernel in shared code should not depend on a caller's luck. GCC turns this into
   * a single LDR, which on a Cortex-M7 handles an unaligned address by itself. */
  static inline uint32_t gd_ld32(const void *p)
  {
      uint32_t v; __builtin_memcpy(&v, p, 4); return v;
  }
#if defined(GD_EMULATE_M7)
  /* THE SAME FOUR INSTRUCTIONS IN C, for proving the M7 kernels on the PC (tests/dot_verify.c built with
   * -DGD_EMULATE_M7). Until 2026-09-27 these kernels could only be checked on the board itself; with this
   * the exact code the Teensy runs -- every shift, mask and lane pairing -- runs against the scalar
   * reference on a PC. Straight from the ARMv7-M reference: SXTB16 sign-extends bytes 0 and 2 into two
   * 16-bit lanes (ROR #8 first rotates bytes 1 and 3 into those places), SMLAD adds both 16x16 lane
   * products to the accumulator (wrapping), SSUB8 subtracts each byte lane on its own. */
  static inline uint32_t gd_sxtb16(uint32_t x)
  {
      return (uint32_t)(uint16_t)(int16_t)(int8_t)(x & 0xFFu) |
             ((uint32_t)(uint16_t)(int16_t)(int8_t)((x >> 16) & 0xFFu) << 16);
  }
  static inline uint32_t gd_sxtb16r8(uint32_t x) { return gd_sxtb16((x >> 8) | (x << 24)); }
  static inline int32_t gd_smlad(uint32_t a, uint32_t b, int32_t acc)
  {
      const int32_t p0 = (int32_t)(int16_t)(a & 0xFFFFu) * (int32_t)(int16_t)(b & 0xFFFFu);
      const int32_t p1 = (int32_t)(int16_t)(a >> 16) * (int32_t)(int16_t)(b >> 16);
      return (int32_t)((uint32_t)acc + (uint32_t)p0 + (uint32_t)p1);
  }
  static inline uint32_t gd_ssub8(uint32_t a, uint32_t b)
  {
      uint32_t r = 0;
      for (int i = 0; i < 32; i += 8)
          r |= (uint32_t)(uint8_t)((int8_t)(a >> i) - (int8_t)(b >> i)) << i;
      return r;
  }
#else
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
#endif

  /* The same 32-weight sub-block WITHOUT the running sum of activations.
   *
   * Q4_K needs that sum because of its per-block minimum, and computing it costs two more SMLAD for
   * every four weights -- which the staged measurement showed is most of why this loop runs at 3.64
   * cycles a weight where a kernel without it does 2.07.
   *
   * In a matrix-vector product the sum does not belong in here at all: the activation vector is
   * quantized once and every row of the weight matrix is dotted against the same one, so the per-32
   * sums are identical for every row and can be computed once per vector. They are integers, so
   * hoisting them out changes nothing about the result.
   *
   * Eight bytes an iteration rather than four, with two accumulators, because the loop overhead is
   * then paid half as often and the SMLAD chains do not serialise. Unrolling is safe here in a way it
   * was not on the PSRAM bus: this is compute with no timing specification attached, so a different
   * instruction sequence changes the speed and nothing else. */
  static inline int32_t gd_dot32_nosum(const uint8_t *q, const int8_t *x, uint32_t shift)
  {
      int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
      for (int l = 0; l < 32; l += 8) {
          const uint32_t q0 = (gd_ld32(q + l)     >> shift) & 0x0F0F0F0Fu;
          const uint32_t q1 = (gd_ld32(q + l + 4) >> shift) & 0x0F0F0F0Fu;
          const uint32_t x0 = gd_ld32(x + l);
          const uint32_t x1 = gd_ld32(x + l + 4);
          a0 = gd_smlad(gd_sxtb16(q0),   gd_sxtb16(x0),   a0);
          a1 = gd_smlad(gd_sxtb16r8(q0), gd_sxtb16r8(x0), a1);
          a2 = gd_smlad(gd_sxtb16(q1),   gd_sxtb16(x1),   a2);
          a3 = gd_smlad(gd_sxtb16r8(q1), gd_sxtb16r8(x1), a3);
      }
      return (a0 + a1) + (a2 + a3);
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

static float dot_q4_k(const uint8_t *raw, const int8_t *xq, const float *xs, uint64_t n);

/* ---------------------------------------------------------------------------------------------
 *  Q4_K with the activation sums supplied rather than recomputed
 *
 *  The staged measurement attributes 3.64 of Q4_K's 5.57 cycles per weight to the nibble loop, against
 *  2.07 for a kernel that does the same multiply-accumulates without also summing the activations. Q4_K
 *  sums them because of its per-block minimum: the weight is d*sc*q - dmin*m, so the minimum's
 *  contribution over a sub-block is dmin*m times the sum of the activations in it.
 *
 *  That sum does not depend on the weights. In a matrix-vector product -- which is every use of this
 *  kernel -- one activation vector is dotted against every row of a matrix, so the per-32 sums are
 *  computed once per vector instead of once per row. There are n/32 of them and they are int32, so
 *  hoisting them out is exact: the double arithmetic downstream sees the same integers it saw before,
 *  and the result is bit-identical to gguf_dot_q rather than merely close.
 *
 *  Q6_K is not here because it has no per-block minimum and never needed the sum.
 * ------------------------------------------------------------------------------------------ */
void gguf_act_sums(const int8_t *xq, uint64_t n, int32_t *xsum)
{
    const uint64_t ng = n / ABLK;
    for (uint64_t g = 0; g < ng; g++) {
        int32_t t = 0;
        for (int l = 0; l < ABLK; l++) t += xq[g * ABLK + l];
        xsum[g] = t;
    }
}

float gguf_dot_q4k_presum(const void *raw_, const int8_t *xq, const float *xs,
                          const int32_t *xsum, uint64_t n)
{
#if !GGUF_HAVE_M7DSP
    /* NEVER SLOWER THAN gguf_dot_q, wherever it is called.
     *
     * Hoisting the sums only wins where this file's fastest Q4_K kernel is the one below. On a host
     * with AVX2, or a Cortex-A with NEON, gguf_dot_q dispatches to a vector kernel that the scalar
     * loop below would lose to badly -- so a runtime that switched to this function unconditionally
     * would fall off the vector path entirely on x86 and on every Cortex-A node in the machine.
     *
     * That regression was written and caught before it shipped, by asking which kernel each target
     * would actually run rather than assuming the faster-looking call is faster. It is the same
     * question document 45 is about. Here the answer is to forward: the sums go unused, the result is
     * identical, and the caller can use one entry point everywhere without knowing which node it is on.
     */
    (void)xsum;
    return dot_q4_k((const uint8_t *)raw_, xq, xs, n);
#else
    const uint8_t *raw = (const uint8_t *)raw_;
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
        const int8_t  *x  = xq   + b * QK_K;
        const float   *sx = xs   + b * (QK_K / ABLK);
        const int32_t *sm = xsum + b * (QK_K / ABLK);

        int is = 0;
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;
            int32_t dot;

            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
#if GGUF_HAVE_M7DSP
            dot = gd_dot32_nosum(q, x + j, 0);
#else
            dot = 0;
            for (int l = 0; l < 32; l++) dot += (int32_t)(q[l] & 0x0F) * (int32_t)x[j + l];
#endif
            total += (double)sx[is + 0] *
                     ((double)d * sc * dot - (double)dmin * m * sm[is + 0]);

            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
#if GGUF_HAVE_M7DSP
            dot = gd_dot32_nosum(q, x + j + 32, 4);
#else
            dot = 0;
            for (int l = 0; l < 32; l++) dot += (int32_t)(q[l] >> 4) * (int32_t)x[j + 32 + l];
#endif
            total += (double)sx[is + 1] *
                     ((double)d * sc * dot - (double)dmin * m * sm[is + 1]);

            q += 32;
            is += 2;
        }
    }
    return (float)total;
#endif
}

/* ---------------------------------------------------------------------------------------------
 *  INSTRUMENTATION, not a kernel anybody should call for a result
 *
 *  With the DSP path in, Q4_K runs at 5.4 cycles per weight while the bare 4-bit kernel in
 *  psram_matrix does 2.07. So roughly 3.3 cycles a weight is no longer the nibbles, and it is worth
 *  knowing which of two things it is before deciding what to do about it:
 *
 *      gguf_q4k_scale_min       eight 6-bit scales and eight 6-bit minimums out of twelve
 *                               asymmetrically packed bytes, per 256 weights
 *      the float combination    three double multiplies, a double subtract and a double add per 32
 *                               weights, plus two fp16 conversions per block
 *
 *  The distinction decides the cost of the repair. A faster scale unpack produces the same integers
 *  and stays bit-identical, so it can simply be adopted. Moving the combination out of double changes
 *  the result bits, so it has to be argued against the float reference and may not be adoptable at
 *  all -- the double is there because a float running total over 344 sub-block contributions lost
 *  3.5e-4 of relative accuracy on ffn_down and read as a kernel bug.
 *
 *  So this runs the same kernel with stages removed and returns a value nobody should use, purely so
 *  the difference in TIME can be measured. Stage 2 in particular returns arithmetic nonsense and says
 *  so in its name. Subtracting timings of variants that differ by exactly one stage is the only way to
 *  attribute cost without guessing, and guessing is what produced the 61% estimate this replaces.
 * ------------------------------------------------------------------------------------------ */
float gguf_dot_q4k_stage(const void *raw_, const int8_t *xq, const float *xs, uint64_t n, int stage)
{
    const uint8_t *raw = (const uint8_t *)raw_;
    const uint64_t nb = n / QK_K;
    double total = 0.0;
    int64_t itotal = 0;

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
            uint8_t sc = 1, m = 0;
            int32_t dot, sum;

#if GGUF_HAVE_M7DSP
            gd_dot32_m7(q, x + j, 0, &dot, &sum);
#else
            dot = 0; sum = 0;
            for (int l = 0; l < 32; l++) { const int32_t xv = x[j + l];
                dot += (int32_t)(q[l] & 0x0F) * xv; sum += xv; }
#endif
            if (stage < 2) gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            if (stage < 1) total += (double)sx[is + 0] *
                                    ((double)d * sc * dot - (double)dmin * m * sum);
            else           itotal += (int64_t)dot * sc - (int64_t)sum * m;

#if GGUF_HAVE_M7DSP
            gd_dot32_m7(q, x + j + 32, 4, &dot, &sum);
#else
            dot = 0; sum = 0;
            for (int l = 0; l < 32; l++) { const int32_t xv = x[j + 32 + l];
                dot += (int32_t)(q[l] >> 4) * xv; sum += xv; }
#endif
            if (stage < 2) gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            if (stage < 1) total += (double)sx[is + 1] *
                                    ((double)d * sc * dot - (double)dmin * m * sum);
            else           itotal += (int64_t)dot * sc - (int64_t)sum * m;

            q += 32;
            is += 2;
        }
    }
    /* stage 0 is the real answer; 1 and 2 return something the compiler cannot discard, so the loops
     * survive, and nothing else. The caller is timing, not computing. */
    return stage == 0 ? (float)total : (float)(double)itotal;
}

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

/* ---------------------------------------------------------------------------------------------
 *  one row against one activation vector, the vector pre-widened (2026-09-28)
 *
 *  Every M7 kernel above widens the activation bytes into 16-bit lane pairs (SXTB16, SXTB16 ROR #8) for
 *  every weight word of every row: two instructions per four weights that do not depend on the row. In a
 *  matrix-vector product the vector is the same for every row, so its lane pairs are made once here
 *  (gguf_widen_act) and loaded: two loads per four weights instead of two SXTB16, and the M7 can issue a
 *  load beside an ALU instruction where it cannot issue two ALU instructions. The lane words are the same
 *  words, the SMLADs the same, the sums the same integers in the same order; the double expressions are
 *  gguf_dot_q4k_presum's and dot_q6_k_m7's, unchanged. tests/dot_verify.c checks both to the bit.
 * ------------------------------------------------------------------------------------------ */
#if GGUF_HAVE_M7DSP
void gguf_widen_act(const int8_t *xq, uint64_t n, uint32_t *xw)
{
    for (uint64_t i = 0; i + 4 <= n; i += 4) {
        const uint32_t w = gd_ld32(xq + i);
        xw[i / 2]     = gd_sxtb16(w);
        xw[i / 2 + 1] = gd_sxtb16r8(w);
    }
}

static inline int32_t gd_dot32_w(const uint8_t *q, const uint32_t *xw, uint32_t shift)
{
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    for (int l = 0; l < 32; l += 8) {
        const uint32_t q0 = (gd_ld32(q + l)     >> shift) & 0x0F0F0F0Fu;
        const uint32_t q1 = (gd_ld32(q + l + 4) >> shift) & 0x0F0F0F0Fu;
        const uint32_t *xl = xw + l / 2;
        a0 = gd_smlad(gd_sxtb16(q0),   xl[0], a0);
        a1 = gd_smlad(gd_sxtb16r8(q0), xl[1], a1);
        a2 = gd_smlad(gd_sxtb16(q1),   xl[2], a2);
        a3 = gd_smlad(gd_sxtb16r8(q1), xl[3], a3);
    }
    return (a0 + a1) + (a2 + a3);
}

float gguf_dot_q4k_presum_w(const void *raw_, const int8_t *xq, const uint32_t *xw, const float *xs,
                            const int32_t *xsum, uint64_t n)
{
    (void)xq;
    const uint8_t *raw = (const uint8_t *)raw_;
    const uint64_t nb = n / QK_K;
    double total = 0.0;
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 144u;
        uint16_t hd, hm;
        memcpy(&hd, blk + 0, 2);
        memcpy(&hm, blk + 2, 2);
        const float d    = gguf_fp16(hd);
        const float dmin = gguf_fp16(hm);
        const uint8_t  *sc_raw = blk + 4;
        const uint8_t  *q      = blk + 16;
        const uint32_t *xb = xw + b * (QK_K / 2);
        const float    *sx = xs + b * (QK_K / ABLK);
        const int32_t  *sm = xsum + b * (QK_K / ABLK);
        int is = 0;
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;
            int32_t dot;
            gguf_q4k_scale_min(is + 0, sc_raw, &sc, &m);
            dot = gd_dot32_w(q, xb + j / 2, 0);
            total += (double)sx[is + 0] * ((double)d * sc * dot - (double)dmin * m * sm[is + 0]);
            gguf_q4k_scale_min(is + 1, sc_raw, &sc, &m);
            dot = gd_dot32_w(q, xb + (j + 32) / 2, 4);
            total += (double)sx[is + 1] * ((double)d * sc * dot - (double)dmin * m * sm[is + 1]);
            q += 32;
            is += 2;
        }
    }
    return (float)total;
}

static inline int32_t gd_q6_dot16_w(const uint8_t *ql, const uint8_t *qh, const uint32_t *xw,
                                    uint32_t qlsh, uint32_t hsh)
{
    int32_t a0 = 0, a1 = 0;
    for (int l = 0; l < 16; l += 4) {
        const uint32_t lo = (gd_ld32(ql + l) >> qlsh) & 0x0F0F0F0Fu;
        const uint32_t hi = ((gd_ld32(qh + l) >> hsh) & 0x03030303u) << 4;
        const uint32_t q  = gd_ssub8(lo | hi, 0x20202020u);
        a0 = gd_smlad(gd_sxtb16(q),   xw[l / 2],     a0);
        a1 = gd_smlad(gd_sxtb16r8(q), xw[l / 2 + 1], a1);
    }
    return a0 + a1;
}

float gguf_dot_q6k_w(const void *raw_, const int8_t *xq, const uint32_t *xw, const float *xs, uint64_t n)
{
    (void)xq;
    const uint8_t *raw = (const uint8_t *)raw_;
    const uint64_t nb = n / QK_K;
    double total = 0.0;
    static const uint8_t QLOFF[4] = {  0, 32,  0, 32 };
    static const uint8_t QLSH[4]  = {  0,  0,  4,  4 };
    static const uint8_t HSH[4]   = {  0,  2,  4,  6 };
    for (uint64_t b = 0; b < nb; b++) {
        const uint8_t *blk = raw + b * 210u;
        const uint8_t *ql  = blk;
        const uint8_t *qh  = blk + 128;
        const int8_t  *sc  = (const int8_t *)(blk + 192);
        uint16_t hd;
        memcpy(&hd, blk + 208, 2);
        const float d = gguf_fp16(hd);
        const uint32_t *xb = xw + b * (QK_K / 2);
        const float    *sx = xs + b * (QK_K / ABLK);
        for (int n128 = 0; n128 < QK_K; n128 += 128) {
            int32_t dot[4][2];
            for (int o = 0; o < 4; o++) {
                const uint8_t  *qlb = ql + QLOFF[o];
                const uint32_t *xo  = xb + (n128 + 32 * o) / 2;
                dot[o][0] = gd_q6_dot16_w(qlb,      qh,      xo,     QLSH[o], HSH[o]);
                dot[o][1] = gd_q6_dot16_w(qlb + 16, qh + 16, xo + 8, QLSH[o], HSH[o]);
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
#else
void gguf_widen_act(const int8_t *xq, uint64_t n, uint32_t *xw) { (void)xq; (void)n; (void)xw; }
float gguf_dot_q4k_presum_w(const void *raw_, const int8_t *xq, const uint32_t *xw, const float *xs,
                            const int32_t *xsum, uint64_t n)
{
    (void)xw;
    return gguf_dot_q4k_presum(raw_, xq, xs, xsum, n);
}
float gguf_dot_q6k_w(const void *raw_, const int8_t *xq, const uint32_t *xw, const float *xs, uint64_t n)
{
    (void)xw;
    return gguf_dot_q(GGML_Q6_K, raw_, xq, xs, n);
}
#endif

/* ---------------------------------------------------------------------------------------------
 *  one row against several activation vectors
 *
 *  A pass that feeds several positions (a batched prompt, several prompts at once) dots every row with up
 *  to eight vectors. The single-vector kernels unpack the row's nibbles again for each one. Here each group
 *  of 32 weights is unpacked into 16-bit lanes ONCE, and every vector is multiplied against those lanes.
 *
 *  BIT-IDENTICAL to calling the single-vector kernel per vector: the integer dots are the same integers
 *  (their order of summation is free), and each vector's double accumulation is the single kernel's
 *  expression, written the same way and run in the same sub-block order. tests/dot_verify.c checks it
 *  against the scalar reference; built with -DGD_EMULATE_M7 it checks the M7 code itself on a PC.
 * ------------------------------------------------------------------------------------------ */
void gguf_dot_q4k_presum_n(const void *raw_, int np, const int8_t *const *xq, const float *const *xs,
                           const int32_t *const *xsum, uint64_t n, float *out)
{
#if GGUF_HAVE_M7DSP
    if (!g_force_scalar && np > 1 && np <= GGUF_NPOS_MAX) {
        const uint8_t *raw = (const uint8_t *)raw_;
        const uint64_t nb = n / QK_K;
        double total[GGUF_NPOS_MAX];
        for (int p = 0; p < np; p++) total[p] = 0.0;
        for (uint64_t b = 0; b < nb; b++) {
            const uint8_t *blk = raw + b * 144u;
            uint16_t hd, hm;
            memcpy(&hd, blk + 0, 2);
            memcpy(&hm, blk + 2, 2);
            const float d    = gguf_fp16(hd);
            const float dmin = gguf_fp16(hm);
            const uint8_t *sc_raw = blk + 4;
            const uint8_t *q      = blk + 16;
            int is = 0;
            for (int j = 0; j < QK_K; j += 64) {
                uint8_t sc0, m0, sc1, m1;
                gguf_q4k_scale_min(is + 0, sc_raw, &sc0, &m0);
                gguf_q4k_scale_min(is + 1, sc_raw, &sc1, &m1);
                /* The products every vector shares, once. (double)d * sc0 is exactly the first product the
                 * single kernel's (double)d * sc0 * dot0 evaluates, so A0 * dot0 is the same double. */
                const double A0 = (double)d * sc0, B0 = (double)dmin * m0;
                const double A1 = (double)d * sc1, B1 = (double)dmin * m1;
                int32_t dot0[GGUF_NPOS_MAX], dot1[GGUF_NPOS_MAX];
                /* TWO VECTORS AT A TIME (2026-09-28). The first version kept this group's 32 lane words and
                 * every vector's four accumulators alive at once; the compiler put them on the stack, and the
                 * inner loop became 24 instructions for four SMLADs -- 4.3 cycles a multiply-add measured,
                 * against the instruction's 0.5. Here each word's four lane words are made once and used for
                 * two vectors whose four accumulators stay in registers with them: loads, SXTB16 and SMLAD
                 * and nothing else. The integer sums are the same integers in another order (exact), and each
                 * vector's double accumulation is the single kernel's expression, unchanged. */
                int p = 0;
                for (; p + 1 < np; p += 2) {
                    const int8_t *x0 = xq[p] + b * QK_K + j, *x1 = xq[p + 1] + b * QK_K + j;
                    int32_t a0 = 0, c0 = 0, a1 = 0, c1 = 0;        /* low-nibble dot, high-nibble dot, x2 */
                    for (int k = 0; k < 32; k += 4) {
                        const uint32_t w  = gd_ld32(q + k);
                        const uint32_t lo = w & 0x0F0F0F0Fu, hi = (w >> 4) & 0x0F0F0F0Fu;
                        const uint32_t la = gd_sxtb16(lo), lb = gd_sxtb16r8(lo);
                        const uint32_t ha = gd_sxtb16(hi), hb = gd_sxtb16r8(hi);
                        const uint32_t v0 = gd_ld32(x0 + k), u0 = gd_ld32(x0 + 32 + k);
                        const uint32_t v1 = gd_ld32(x1 + k), u1 = gd_ld32(x1 + 32 + k);
                        a0 = gd_smlad(la, gd_sxtb16(v0), a0);
                        a0 = gd_smlad(lb, gd_sxtb16r8(v0), a0);
                        c0 = gd_smlad(ha, gd_sxtb16(u0), c0);
                        c0 = gd_smlad(hb, gd_sxtb16r8(u0), c0);
                        a1 = gd_smlad(la, gd_sxtb16(v1), a1);
                        a1 = gd_smlad(lb, gd_sxtb16r8(v1), a1);
                        c1 = gd_smlad(ha, gd_sxtb16(u1), c1);
                        c1 = gd_smlad(hb, gd_sxtb16r8(u1), c1);
                    }
                    dot0[p] = a0; dot1[p] = c0; dot0[p + 1] = a1; dot1[p + 1] = c1;
                }
                if (p < np) {                                       /* an odd count: the last vector alone */
                    const int8_t *x0 = xq[p] + b * QK_K + j;
                    int32_t a0 = 0, c0 = 0;
                    for (int k = 0; k < 32; k += 4) {
                        const uint32_t w  = gd_ld32(q + k);
                        const uint32_t lo = w & 0x0F0F0F0Fu, hi = (w >> 4) & 0x0F0F0F0Fu;
                        const uint32_t v0 = gd_ld32(x0 + k), u0 = gd_ld32(x0 + 32 + k);
                        a0 = gd_smlad(gd_sxtb16(lo), gd_sxtb16(v0), a0);
                        a0 = gd_smlad(gd_sxtb16r8(lo), gd_sxtb16r8(v0), a0);
                        c0 = gd_smlad(gd_sxtb16(hi), gd_sxtb16(u0), c0);
                        c0 = gd_smlad(gd_sxtb16r8(hi), gd_sxtb16r8(u0), c0);
                    }
                    dot0[p] = a0; dot1[p] = c0;
                }
                for (int p2 = 0; p2 < np; p2++) {
                    const float   *sx = xs[p2] + b * (QK_K / ABLK);
                    const int32_t *sm = xsum[p2] + b * (QK_K / ABLK);
                    total[p2] += (double)sx[is + 0] * (A0 * dot0[p2] - B0 * sm[is + 0]);
                    total[p2] += (double)sx[is + 1] * (A1 * dot1[p2] - B1 * sm[is + 1]);
                }
                q += 32;
                is += 2;
            }
        }
        for (int p = 0; p < np; p++) out[p] = (float)total[p];
        return;
    }
#endif
    for (int p = 0; p < np; p++) out[p] = gguf_dot_q4k_presum(raw_, xq[p], xs[p], xsum[p], n);
}

void gguf_dot_q_n(uint32_t type, const void *raw_, int np, const int8_t *const *xq, const float *const *xs,
                  uint64_t n, float *out)
{
#if GGUF_HAVE_M7DSP
    if (type == GGML_Q6_K && !g_force_scalar && np > 1 && np <= GGUF_NPOS_MAX) {
        const uint8_t *raw = (const uint8_t *)raw_;
        const uint64_t nb = n / QK_K;
        static const uint8_t QLOFF[4] = {  0, 32,  0, 32 };
        static const uint8_t QLSH[4]  = {  0,  0,  4,  4 };
        static const uint8_t HSH[4]   = {  0,  2,  4,  6 };
        double total[GGUF_NPOS_MAX];
        for (int p = 0; p < np; p++) total[p] = 0.0;
        for (uint64_t b = 0; b < nb; b++) {
            const uint8_t *blk = raw + b * 210u;
            const uint8_t *ql  = blk;
            const uint8_t *qh  = blk + 128;
            const int8_t  *sc  = (const int8_t *)(blk + 192);
            uint16_t hd;
            memcpy(&hd, blk + 208, 2);
            const float d = gguf_fp16(hd);
            for (int n128 = 0; n128 < QK_K; n128 += 128) {
                /* TWO VECTORS AT A TIME (2026-09-28, as gguf_dot_q4k_presum_n): the first version built all 64
                 * lane words of the half-block and then ran every vector's eight accumulators over them, all on
                 * the stack. Here each word's two lane words are made where they are used and feed two vectors'
                 * accumulators in registers. The same integers summed in another order (exact); each vector's
                 * double expression is the single kernel's, unchanged -- its first factor is the vector's own
                 * scale, so nothing of it can be shared across vectors. dot[p][o][g]: 16 weights each. */
                int32_t dot[GGUF_NPOS_MAX][4][2];
                for (int o = 0; o < 4; o++) {
                    /* this offset's 32 six-bit weights as 16 lane words, built ONCE for every vector: a Q6_K
                     * lane word costs two source words, a shift, a mask, an OR and an SSUB8, so building it per
                     * pair (tried first, 2026-09-28) cost more than the shared build it replaced */
                    uint32_t wa[8], wb[8];
                    for (int k = 0; k < 8; k++) {
                        const uint32_t lo = (gd_ld32(ql + QLOFF[o] + 4 * k) >> QLSH[o]) & 0x0F0F0F0Fu;
                        const uint32_t hi = ((gd_ld32(qh + 4 * k) >> HSH[o]) & 0x03030303u) << 4;
                        const uint32_t qv = gd_ssub8(lo | hi, 0x20202020u);
                        wa[k] = gd_sxtb16(qv);
                        wb[k] = gd_sxtb16r8(qv);
                    }
                    int p = 0;
                    for (; p + 1 < np; p += 2) {
                        const int8_t *x0 = xq[p] + b * QK_K + n128 + 32 * o, *x1 = xq[p + 1] + b * QK_K + n128 + 32 * o;
                        for (int g = 0; g < 2; g++) {
                            int32_t a0 = 0, a1 = 0;
                            for (int k = 4 * g; k < 4 * g + 4; k++) {
                                const uint32_t la = wa[k], lb = wb[k];
                                const uint32_t v0 = gd_ld32(x0 + 4 * k), v1 = gd_ld32(x1 + 4 * k);
                                a0 = gd_smlad(la, gd_sxtb16(v0), a0);
                                a0 = gd_smlad(lb, gd_sxtb16r8(v0), a0);
                                a1 = gd_smlad(la, gd_sxtb16(v1), a1);
                                a1 = gd_smlad(lb, gd_sxtb16r8(v1), a1);
                            }
                            dot[p][o][g] = a0; dot[p + 1][o][g] = a1;
                        }
                    }
                    if (p < np) {                                   /* an odd count: the last vector alone */
                        const int8_t *x0 = xq[p] + b * QK_K + n128 + 32 * o;
                        for (int g = 0; g < 2; g++) {
                            int32_t a0 = 0;
                            for (int k = 4 * g; k < 4 * g + 4; k++) {
                                const uint32_t v0 = gd_ld32(x0 + 4 * k);
                                a0 = gd_smlad(wa[k], gd_sxtb16(v0), a0);
                                a0 = gd_smlad(wb[k], gd_sxtb16r8(v0), a0);
                            }
                            dot[p][o][g] = a0;
                        }
                    }
                }
                for (int p = 0; p < np; p++) {
                    const float *sx = xs[p] + b * (QK_K / ABLK);
                    for (int o = 0; o < 4; o++) {
                        const float s = sx[(n128 + 32 * o) / ABLK];
                        for (int g = 0; g < 2; g++)
                            total[p] += (double)s * d * sc[2 * o + g] * dot[p][o][g];
                    }
                }
                ql += 64;
                qh += 32;
                sc += 8;
            }
        }
        for (int p = 0; p < np; p++) out[p] = (float)total[p];
        return;
    }
#endif
    for (int p = 0; p < np; p++) out[p] = gguf_dot_q(type, raw_, xq[p], xs[p], n);
}

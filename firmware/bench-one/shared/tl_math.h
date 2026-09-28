/* ===========================================================================================
 *  tl_math.h -- exp, sin/cos and the RoPE frequency, computed the same way on every processor
 *
 *  The model's forward pass needs four transcendental functions: expf (softmax, SiLU), and powf, sinf,
 *  cosf (the rotary position encoding). Taken from each machine's C library they are four different
 *  implementations -- the PC's and newlib's round the last bit differently -- and int8 activation
 *  quantization amplifies a last-bit difference into logit drift (0.111 on the Teensy's first run).
 *
 *  Here each is built from IEEE double additions, multiplications and divisions only: a range reduction
 *  and a Taylor polynomial long enough that the double result is accurate to about 1e-16, then one
 *  rounding to float. With fp-contract off (every file that includes this sets it on ARM) those operations
 *  give the same bits on a Cortex-M7 and on x86-64, so the Teensy and tests/tl_ref.exe compute the same
 *  logits to the last bit instead of to 0.1.
 *
 *  Used by shared/model_q.c (the PC reference) and shared/tl_core.c (the Teensy). Accuracy against the
 *  C library is checked by tests/tl_math_check.c.
 * ======================================================================================== */
#ifndef TL_MATH_H
#define TL_MATH_H

#include <stdint.h>
#include <string.h>

/* 2^k as a double, exactly, by building its bits; k in [-1022, 1023] */
static inline double tlm_pow2i(int k)
{
    const uint64_t b = (uint64_t)(k + 1023) << 52;
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* round to the nearest integer, halves away from zero, in plain arithmetic */
static inline int tlm_rint(double x) { return (int)(x < 0.0 ? x - 0.5 : x + 0.5); }

/* e^x in double for |x| < 700. x = k ln2 + r, |r| <= ln2/2; e^r by Taylor to r^14 (error < 2e-17);
 * the ln2 split (fdlibm's) keeps k ln2 exact for |k| < 2^20. */
static inline double tlm_exp_d(double x)
{
    const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
    const double inv_ln2 = 1.44269504088896338700e+00;
    if (x > 700.0) x = 700.0;
    if (x < -740.0) return 0.0;
    const int k = tlm_rint(x * inv_ln2);
    const double r = (x - (double)k * ln2_hi) - (double)k * ln2_lo;
    double p = 1.0 / 87178291200.0;                     /* 1/14! */
    p = p * r + 1.0 / 6227020800.0;                     /* 1/13! */
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    p = p * r + 1.0;
    p = p * r + 1.0;
    if (k < -1021) return p * tlm_pow2i(k + 600) * tlm_pow2i(-600);   /* results below the normal range */
    return p * tlm_pow2i(k);
}

static inline float tlm_expf(float x) { return (float)tlm_exp_d((double)x); }

/* natural log in double for a positive normal x: x = m 2^e with m in [sqrt(1/2), sqrt(2)),
 * log m = 2 atanh(z), z = (m-1)/(m+1), |z| <= 0.172, series to z^25 (error < 1e-19) */
static inline double tlm_log_d(double x)
{
    const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10;
    uint64_t b;
    memcpy(&b, &x, 8);
    int e = (int)((b >> 52) & 0x7FF) - 1023;
    b = (b & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;   /* m in [1, 2) */
    double m;
    memcpy(&m, &b, 8);
    if (m > 1.41421356237309504880) { m = m * 0.5; e++; }
    const double z = (m - 1.0) / (m + 1.0), z2 = z * z;
    double s = 1.0 / 25.0;
    for (int k = 23; k >= 1; k -= 2) s = s * z2 + 1.0 / (double)k;
    return (double)e * ln2_hi + ((double)e * ln2_lo + 2.0 * z * s);
}

/* RoPE's frequency for dimension pair i of head_dim: base^(-2i/head_dim), in double, then float */
static inline float tlm_rope_freq(float base, int i, int head_dim)
{
    return (float)tlm_exp_d(-(2.0 * (double)i / (double)head_dim) * tlm_log_d((double)base));
}

/* sin and cos of a float angle, |x| < 2^20: x = k pi/2 + r, |r| <= pi/4 (fdlibm's 33+53-bit split of
 * pi/2 keeps k pi/2 exact); sin r to r^17 and cos r to r^18 (errors < 1e-18), then the quadrant. */
static inline void tlm_sincosf(float xf, float *sn, float *cs)
{
    const double pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11;
    const double invpio2 = 6.36619772367581382433e-01;
    const double x = (double)xf;
    const int k = tlm_rint(x * invpio2);
    const double r = (x - (double)k * pio2_1) - (double)k * pio2_1t, r2 = r * r;
    /* sin r = r (1 - r^2/3! + r^4/5! - r^6/7! + r^8/9! - r^10/11! + r^12/13! - r^14/15! + r^16/17!) */
    double s = 1.0 / 355687428096000.0;                 /* + 1/17! */
    s = s * r2 - 1.0 / 1307674368000.0;                 /* - 1/15! */
    s = s * r2 + 1.0 / 6227020800.0;                    /* + 1/13! */
    s = s * r2 - 1.0 / 39916800.0;                      /* - 1/11! */
    s = s * r2 + 1.0 / 362880.0;                        /* + 1/9!  */
    s = s * r2 - 1.0 / 5040.0;                          /* - 1/7!  */
    s = s * r2 + 1.0 / 120.0;                           /* + 1/5!  */
    s = s * r2 - 1.0 / 6.0;                             /* - 1/3!  */
    s = s * r2 + 1.0;
    s = s * r;
    /* cos r = 1 - r^2/2! + r^4/4! - r^6/6! + r^8/8! - r^10/10! + r^12/12! - r^14/14! + r^16/16! - r^18/18! */
    double c = -1.0 / 6402373705728000.0;               /* - 1/18! */
    c = c * r2 + 1.0 / 20922789888000.0;                /* + 1/16! */
    c = c * r2 - 1.0 / 87178291200.0;                   /* - 1/14! */
    c = c * r2 + 1.0 / 479001600.0;                     /* + 1/12! */
    c = c * r2 - 1.0 / 3628800.0;                       /* - 1/10! */
    c = c * r2 + 1.0 / 40320.0;                         /* + 1/8!  */
    c = c * r2 - 1.0 / 720.0;                           /* - 1/6!  */
    c = c * r2 + 1.0 / 24.0;                            /* + 1/4!  */
    c = c * r2 - 0.5;                                   /* - 1/2!  */
    c = c * r2 + 1.0;
    switch (k & 3) {
    case 0:  *sn = (float)s;  *cs = (float)c;  break;
    case 1:  *sn = (float)c;  *cs = (float)-s; break;
    case 2:  *sn = (float)-s; *cs = (float)-c; break;
    default: *sn = (float)-c; *cs = (float)s;  break;
    }
}

#endif

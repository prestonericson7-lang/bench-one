/* tl_math_check.c -- shared/tl_math.h against the C library, over the inputs the model actually gives it.
 *
 *   gcc -O2 -std=c11 -Wall -Wextra -I../shared -o tl_math_check.exe tl_math_check.c -lm
 *
 * For each function: how many results equal the double-precision libm value rounded to float (the
 * correctly rounded answer, near enough), and the largest difference in float ulps. expf over the ranges
 * softmax (v - max <= 0) and SiLU (-g) produce; sin/cos over RoPE angles pos * freq for pos < 4096;
 * the RoPE frequencies for base 1e6 and head_dim 128 (Qwen2.5). */
#include "tl_math.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int32_t ulp_diff(float a, float b)
{
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    if (ia < 0) ia = (int32_t)0x80000000 - ia;
    if (ib < 0) ib = (int32_t)0x80000000 - ib;
    const int32_t d = ia - ib;
    return d < 0 ? -d : d;
}

static uint32_t rng = 12345u;
static float frand(float lo, float hi)
{
    rng = rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(rng >> 8) / 16777216.0f;
}

int main(void)
{
    long n = 0, exact = 0, libm_exact = 0;
    int32_t worst = 0;
    float wx = 0;
    for (int i = 0; i < 2000000; i++) {
        const float x = frand(-88.0f, 20.0f);
        const float ref = (float)exp((double)x);
        const float got = tlm_expf(x);
        const int32_t u = ulp_diff(got, ref);
        n++;
        exact += (u == 0);
        libm_exact += (ulp_diff(expf(x), ref) == 0);
        if (u > worst) { worst = u; wx = x; }
    }
    printf("expf   %ld inputs in [-88, 20]: %.5f%% equal to exp() rounded to float (libm expf: %.5f%%); worst %d ulp at %.9g\n",
           n, 100.0 * exact / n, 100.0 * libm_exact / n, (int)worst, wx);

    n = exact = libm_exact = 0;
    worst = 0;
    long nc = 0, cexact = 0, clibm = 0;
    int32_t cworst = 0;
    for (int pos = 0; pos < 4096; pos++)
        for (int i = 0; i < 64; i++) {
            const float th = (float)pos * tlm_rope_freq(1000000.0f, i, 128);
            float s, c;
            tlm_sincosf(th, &s, &c);
            const float rs = (float)sin((double)th), rc = (float)cos((double)th);
            const int32_t us = ulp_diff(s, rs), uc = ulp_diff(c, rc);
            n++; nc++;
            exact += (us == 0); cexact += (uc == 0);
            libm_exact += (ulp_diff(sinf(th), rs) == 0);
            clibm += (ulp_diff(cosf(th), rc) == 0);
            if (us > worst) worst = us;
            if (uc > cworst) cworst = uc;
        }
    printf("sinf   %ld RoPE angles: %.5f%% equal to sin() rounded (libm sinf: %.5f%%); worst %d ulp\n",
           n, 100.0 * exact / n, 100.0 * libm_exact / n, (int)worst);
    printf("cosf   %ld RoPE angles: %.5f%% equal to cos() rounded (libm cosf: %.5f%%); worst %d ulp\n",
           nc, 100.0 * cexact / nc, 100.0 * clibm / nc, (int)cworst);

    n = exact = libm_exact = 0;
    worst = 0;
    for (int i = 0; i < 64; i++) {
        const float ref = (float)pow(1000000.0, -2.0 * i / 128.0);
        const float got = tlm_rope_freq(1000000.0f, i, 128);
        const float lib = powf(1000000.0f, -2.0f * (float)i / 128.0f);
        const int32_t u = ulp_diff(got, ref);
        n++;
        exact += (u == 0);
        libm_exact += (ulp_diff(lib, ref) == 0);
        if (u > worst) worst = u;
    }
    printf("freq   %ld RoPE frequencies: %ld equal to pow() rounded (libm powf: %ld); worst %d ulp\n",
           n, exact, libm_exact, (int)worst);
    return 0;
}

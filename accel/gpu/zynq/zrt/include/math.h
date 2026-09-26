/* math.h -- zrt (freestanding ARM runtime for fpgagpud): what common/gpu_setup.c needs. */
#ifndef ZRT_MATH_H
#define ZRT_MATH_H

/* round to nearest, ties to even (the default IEEE rounding mode) -- identical to glibc llrint
 * in the default floating-point environment for every finite input with |x| < 2^63 */
long long llrint(double x);

#define isfinite(x) __builtin_isfinite(x)
#define isnan(x)    __builtin_isnan(x)

#endif

/* ===========================================================================================
 *  gemv_host.c -- quantized matrix-vector throughput, host reference
 * ===========================================================================================
 *  Correctness first. All three formats hold identical values, so if they disagree the speed
 *  number is the speed of computing garbage. Then throughput, in the same units as bench_stream
 *  so the two can be compared directly: megabytes of WEIGHT consumed per second.
 * ===========================================================================================
 */
#include "bench_gemv.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

#define ROWS 256u
#define COLS 1024u
#define REPS 200u

int main(void)
{
    int8_t  *w8  = malloc((size_t)ROWS * COLS);
    uint8_t *w4  = malloc((size_t)ROWS * COLS / 2);
    int16_t *w16 = malloc((size_t)ROWS * COLS * 2);
    int8_t  *x8  = malloc(COLS);
    int16_t *x16 = malloc(COLS * 2);
    int32_t *y8  = malloc(ROWS * sizeof(int32_t));
    int32_t *y4  = malloc(ROWS * sizeof(int32_t));
    int32_t *y16 = malloc(ROWS * sizeof(int32_t));
    if (!w8 || !w4 || !w16 || !x8 || !x16 || !y8 || !y4 || !y16) { puts("oom"); return 1; }

    /* Weights inside -7..7 so the INT4 packing is lossless and all three hold the same values. */
    unsigned s = 12345u;
    for (size_t i = 0; i < (size_t)ROWS * COLS; i++) {
        s = s * 1103515245u + 12345u;
        w8[i] = (int8_t)((int)((s >> 16) % 15u) - 7);
        w16[i] = (int16_t)w8[i];
    }
    for (unsigned i = 0; i < COLS; i++) {
        s = s * 1103515245u + 12345u;
        x8[i] = (int8_t)((int)((s >> 16) % 255u) - 127);
        x16[i] = (int16_t)x8[i];
    }
    for (unsigned r = 0; r < ROWS; r++)
        gemv_pack4(w8 + (size_t)r * COLS, w4 + (size_t)r * (COLS / 2), COLS);

    gemv_int8(w8, x8, y8, ROWS, COLS);
    gemv_int4(w4, x8, y4, ROWS, COLS);
    gemv_int16(w16, x16, y16, ROWS, COLS);

    unsigned bad4 = 0, bad16 = 0;
    for (unsigned r = 0; r < ROWS; r++) {
        if (y4[r] != y8[r]) bad4++;
        if (y16[r] != y8[r]) bad16++;
    }
    printf("\n  correctness   INT4 vs INT8: %s     INT16 vs INT8: %s\n",
           bad4 ? "FAIL" : "exact", bad16 ? "FAIL" : "exact");
    if (bad4 || bad16) return 1;

    double t, dt;
    t = now_s(); for (unsigned k = 0; k < REPS; k++) gemv_int8(w8, x8, y8, ROWS, COLS);
    dt = now_s() - t;
    const double m8 = gemv_bytes_int8(ROWS, COLS) * REPS / dt / 1048576.0;

    t = now_s(); for (unsigned k = 0; k < REPS; k++) gemv_int4(w4, x8, y4, ROWS, COLS);
    dt = now_s() - t;
    const double m4 = gemv_bytes_int4(ROWS, COLS) * REPS / dt / 1048576.0;

    t = now_s(); for (unsigned k = 0; k < REPS; k++) gemv_int16(w16, x16, y16, ROWS, COLS);
    dt = now_s() - t;
    const double m16 = gemv_bytes_int16(ROWS, COLS) * REPS / dt / 1048576.0;

    printf("  matrix %ux%u\n\n", ROWS, COLS);
    printf("  format   weight bytes   consumed        per MAC\n");
    printf("  ------   ------------   ------------    -----------\n");
    printf("  INT16    %8.0f KB   %8.1f MB/s   %6.2f G MAC/s\n",
           gemv_bytes_int16(ROWS, COLS) / 1024.0, m16, m16 / 2.0 * 1048576.0 / 1e9);
    printf("  INT8     %8.0f KB   %8.1f MB/s   %6.2f G MAC/s\n",
           gemv_bytes_int8(ROWS, COLS) / 1024.0, m8, m8 * 1048576.0 / 1e9);
    printf("  INT4     %8.0f KB   %8.1f MB/s   %6.2f G MAC/s\n",
           gemv_bytes_int4(ROWS, COLS) / 1024.0, m4, m4 * 2.0 * 1048576.0 / 1e9);

    printf("\n  INT4 halves the bytes but runs at %.2fx INT8's MAC rate.\n",
           (m4 * 2.0) / (m8));
    printf("  It only pays where memory is the limit, never where compute is.\n\n");
    return 0;
}

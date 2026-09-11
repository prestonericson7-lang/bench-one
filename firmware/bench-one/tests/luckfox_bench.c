/* ===========================================================================================
 *  luckfox_bench.c -- the last big measurement gap: is a Luckfox memory bound or compute bound?
 * ===========================================================================================
 *
 *  Its bandwidth is measured at about 930 MB/s. That figure only matters if the CPU can consume
 *  weights that fast. If a Cortex-A7 chews through 100 MB/s of INT4 then the memory is irrelevant,
 *  the node's role in the architecture changes, and every planner number that leans on 930 is wrong.
 *
 *  Three things, all reported in the SAME units so they compare directly:
 *
 *      STREAM      bytes per second the memory can deliver.
 *      hd_hamming  the HDC kernel, kept for continuity with the Teensy and ESP32 figures.
 *      GEMV        INT4, INT8 and INT16 weights consumed per second -- the number that decides it.
 *
 *  Put the STREAM read next to the GEMV rate and the answer is immediate. Whichever is smaller is
 *  the bottleneck, and this is the only board in the project where nobody knows which.
 *
 *  BUILD -- statically linked, because the board runs uClibc and this is built against glibc.
 *  A fully static binary carries its own libc and only needs the kernel's syscall ABI to match.
 *
 *      arm-none-linux-gnueabihf-gcc -O2 -static -std=gnu11 \
 *          -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
 *          -I../shared -o luckfox_bench luckfox_bench.c ../shared/bench_hdc.c
 *
 *  The -march and -mfpu matter: the RV1103's Cortex-A7 reports neon, vfpv4 and edsp, so both the
 *  USAD8 path in bench_hdc and the SMLAD path in bench_gemv should compile in. Building for a
 *  generic ARM target would silently take the slow path and measure the wrong machine.
 * ===========================================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "bench_hdc.h"
#include "bench_gemv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Best of N rather than a mean. This is a shared single-core Linux box with a shell and an adb
 * daemon on it, so the fastest pass is the one least disturbed by the scheduler. */
#define REPS 12

#define STREAM_MB 4u
#define ROWS 128u
#define COLS 1024u

int main(void)
{
    printf("\n===============================================================\n");
    printf("Luckfox: memory bound or compute bound?\n");

    {
        FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
        if (f) { int k = 0; if (fscanf(f, "%d", &k) == 1) printf("  clock %d kHz\n", k); fclose(f); }
        f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
        if (f) { char g[32] = {0}; if (fscanf(f, "%31s", g) == 1) printf("  governor %s\n", g); fclose(f); }
    }
#if defined(__ARM_NEON)
    printf("  NEON: compiled in\n");
#else
    printf("  NEON: NOT compiled in -- rebuild with -mfpu=neon-vfpv4\n");
#endif
#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1)
    printf("  DSP (USAD8, SMLAD): compiled in\n");
#else
    printf("  DSP: NOT compiled in -- the slow paths are being measured\n");
#endif
    printf("===============================================================\n\n");

    /* ---- 1. STREAM ------------------------------------------------------------------- */
    printf("[1] memory bandwidth\n");
    const size_t words = (size_t)STREAM_MB * 1048576u / 4u;
    uint32_t *a = malloc(words * 4), *b = malloc(words * 4);
    if (!a || !b) { printf("  out of memory\n"); return 1; }
    for (size_t i = 0; i < words; i++) a[i] = (uint32_t)i;

    double best;
    best = 1e9;
    for (int r = 0; r < REPS; r++) {
        uint32_t sum = 0;
        const double t = now_s();
        for (size_t i = 0; i < words; i++) sum += a[i];
        const double dt = now_s() - t;
        static volatile uint32_t keep; keep = sum; (void)keep;
        if (dt < best) best = dt;
    }
    const double rd = (double)STREAM_MB / best;
    printf("    read  %8.1f MB/s\n", rd);

    best = 1e9;
    for (int r = 0; r < REPS; r++) {
        const double t = now_s();
        for (size_t i = 0; i < words; i++) a[i] = (uint32_t)(i + r);
        const double dt = now_s() - t;
        if (dt < best) best = dt;
    }
    printf("    write %8.1f MB/s\n", (double)STREAM_MB / best);

    best = 1e9;
    for (int r = 0; r < REPS; r++) {
        const double t = now_s();
        memcpy(b, a, words * 4);
        const double dt = now_s() - t;
        if (dt < best) best = dt;
    }
    printf("    copy  %8.1f MB/s of bus traffic (memcpy, NEON)\n",
           (double)STREAM_MB * 2.0 / best);
    printf("\n");

    /* ---- 2. the HDC kernel, for continuity with the other boards ---------------------- */
    printf("[2] hd_hamming, same source the Teensy and ESP32 ran\n");
    {
        static hd_t x, y;
        uint32_t rng = 0x1234u;
        hd_random(x, &rng);
        hd_random(y, &rng);
        volatile uint32_t sink = 0;
        best = 1e9;
        for (int r = 0; r < REPS; r++) {
            const double t = now_s();
            for (int i = 0; i < 20000; i++) sink += hd_hamming(x, y);
            const double dt = now_s() - t;
            if (dt < best) best = dt;
        }
        (void)sink;
        const double us = best / 20000.0 * 1e6;
        printf("    %.2f us per %d-bit compare, %.2f cycles per 32-bit word at 1104 MHz\n",
               us, HD_BITS, us * 1104.0 / (double)HD_WORDS);
        printf("      Teensy 4.1 measured 3.66 us, ESP32-S3 49.14 us\n");
    }
    printf("\n");

    /* ---- 3. THE ONE THAT DECIDES IT --------------------------------------------------- */
    printf("[3] quantized matrix-vector: weights consumed per second\n");
    {
        int8_t  *w8  = malloc((size_t)ROWS * COLS);
        uint8_t *w4  = malloc((size_t)ROWS * COLS / 2);
        int16_t *w16 = malloc((size_t)ROWS * COLS * 2);
        int8_t  *x8  = malloc(COLS);
        int16_t *x16 = malloc(COLS * 2);
        int32_t *y   = malloc(ROWS * sizeof(int32_t));
        if (!w8 || !w4 || !w16 || !x8 || !x16 || !y) { printf("  out of memory\n"); return 1; }

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

        /* Correctness before speed: a fast kernel computing nonsense is worse than no kernel. */
        int32_t *y4 = malloc(ROWS * sizeof(int32_t));
        gemv_int8(w8, x8, y, ROWS, COLS);
        gemv_int4(w4, x8, y4, ROWS, COLS);
        unsigned bad = 0;
        for (unsigned r = 0; r < ROWS; r++) if (y[r] != y4[r]) bad++;
        printf("    INT4 agrees with INT8: %s\n", bad ? "NO -- results below are meaningless" : "yes");

        double m8, m4, m16;
        best = 1e9;
        for (int r = 0; r < REPS; r++) {
            const double t = now_s();
            for (int k = 0; k < 20; k++) gemv_int8(w8, x8, y, ROWS, COLS);
            const double dt = now_s() - t;
            if (dt < best) best = dt;
        }
        m8 = gemv_bytes_int8(ROWS, COLS) * 20.0 / best / 1048576.0;

        best = 1e9;
        for (int r = 0; r < REPS; r++) {
            const double t = now_s();
            for (int k = 0; k < 20; k++) gemv_int4(w4, x8, y, ROWS, COLS);
            const double dt = now_s() - t;
            if (dt < best) best = dt;
        }
        m4 = gemv_bytes_int4(ROWS, COLS) * 20.0 / best / 1048576.0;

        best = 1e9;
        for (int r = 0; r < REPS; r++) {
            const double t = now_s();
            for (int k = 0; k < 20; k++) gemv_int16(w16, x16, y, ROWS, COLS);
            const double dt = now_s() - t;
            if (dt < best) best = dt;
        }
        m16 = gemv_bytes_int16(ROWS, COLS) * 20.0 / best / 1048576.0;

        printf("    INT16 %8.1f MB/s   %5.2f G MAC/s\n", m16, m16 / 2.0 * 1048576.0 / 1e9);
        printf("    INT8  %8.1f MB/s   %5.2f G MAC/s\n", m8, m8 * 1048576.0 / 1e9);
        printf("    INT4  %8.1f MB/s   %5.2f G MAC/s\n", m4, m4 * 2.0 * 1048576.0 / 1e9);

        printf("\n===============================================================\n");
        printf("  memory can deliver     %8.1f MB/s\n", rd);
        printf("  INT4 can consume       %8.1f MB/s\n", m4);
        if (m4 < rd) {
            printf("  COMPUTE BOUND by %.1fx. The 930 MB/s is unreachable and every planner\n",
                   rd / m4);
            printf("  figure using it is optimistic. A Luckfox is worth %.0f MB/s, not %.0f.\n",
                   m4, rd);
        } else {
            printf("  MEMORY BOUND. The CPU keeps up, so bandwidth is the real limit and the\n");
            printf("  planner figures stand.\n");
        }
        printf("===============================================================\n\n");
    }
    return 0;
}

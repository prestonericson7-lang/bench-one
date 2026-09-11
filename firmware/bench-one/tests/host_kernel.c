/* ===========================================================================================
 *  host_kernel.c -- the same compare kernel, on the development PC, for an honest comparison
 * ===========================================================================================
 *
 *  Every claim about how this machine stacks up against an ordinary computer has so far been made
 *  against an ESTIMATE of that computer. After a day in which estimates were wrong by 1.65x in one
 *  direction and 2.3x in the other, that is not good enough. This runs bench_hdc.c's hd_hamming --
 *  the identical source file the Teensy and the ESP32 run -- on the host, and reports the same
 *  numbers in the same units.
 *
 *  THREE WORKING SET SIZES, BECAUSE ONE NUMBER WOULD FLATTER THE PC
 *  ----------------------------------------------------------------
 *  A benchmark that fits in L1 measures the cache, not the machine. The real workload streams
 *  through memory it has never seen and will not revisit, so the number that matters is the one
 *  taken from a working set far larger than last-level cache. All three are printed, because the
 *  gap between them is itself the finding: on a PC it is large, and on a microcontroller with no
 *  cache hierarchy worth the name it is small.
 *
 *  TWO BUILDS, BECAUSE "FAIR" CUTS BOTH WAYS
 *  ------------------------------------------
 *  Built at -O2 with no architecture flags, this is the same portable C the microcontrollers run,
 *  and the comparison is like for like. Built with -O3 -march=native, the compiler is free to use
 *  AVX2 and the popcount instruction, and that is the PC at its best. A microcontroller has no
 *  equivalent second gear, so quoting only the first would understate the PC and quoting only the
 *  second would overstate it. Run both.
 *
 *  BUILD
 *      gcc -O2            -I../shared -o host_kernel.exe host_kernel.c ../shared/bench_hdc.c
 *      gcc -O3 -march=native -I../shared -o host_native.exe host_kernel.c ../shared/bench_hdc.c
 * ===========================================================================================
 */

#include "bench_hdc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* QueryPerformanceCounter rather than timespec_get: this mingw build does not expose the C11
 * function, and the Windows counter is the higher resolution clock here anyway. */
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
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

/* Time a scan over `n` vectors, repeated until at least `min_s` has elapsed so a short run cannot
 * be dominated by clock granularity. Returns microseconds per compare. */
static double bench(hd_t *mem, uint32_t n, const hd_t q, double min_s, uint64_t *out_compares)
{
    volatile uint32_t sink = 0;
    uint64_t compares = 0;
    const double t0 = now_s();
    double dt;
    do {
        for (uint32_t i = 0; i < n; i++) sink += hd_hamming(mem[i], q);
        compares += n;
        dt = now_s() - t0;
    } while (dt < min_s);
    (void)sink;
    if (out_compares) *out_compares = compares;
    return dt / (double)compares * 1e6;
}

int main(void)
{
    /* 8192 vectors is 8 MB, comfortably past any consumer last-level cache. 64 is 64 KB and sits
     * in L2. 8 is 8 KB and lives in L1. */
    const uint32_t sizes[3] = { 8, 64, 8192 };
    const char *names[3] = { "L1-resident  (8 KB)", "L2-resident  (64 KB)", "beyond cache (8 MB)" };

    hd_t *mem = (hd_t *)malloc(sizeof(hd_t) * 8192);
    if (!mem) { printf("out of memory\n"); return 1; }

    hd_t q;
    uint32_t rng = 0x2BAD1DEAu;
    hd_random(q, &rng);
    for (uint32_t i = 0; i < 8192; i++) hd_random(mem[i], &rng);

    printf("\n");
    printf("===============================================================\n");
    printf("host compare kernel -- the same hd_hamming the boards run\n");
    printf("  %d-bit vectors, %u bytes each\n", HD_BITS, (unsigned)HD_BYTES);
#if defined(__AVX2__)
    printf("  built WITH AVX2 -- this is the PC at its best\n");
#else
    printf("  built portable, no architecture flags -- like for like with the boards\n");
#endif
    printf("===============================================================\n\n");

    double beyond = 0.0;
    for (int s = 0; s < 3; s++) {
        uint64_t c = 0;
        const double us = bench(mem, sizes[s], q, 0.5, &c);
        printf("  %-22s %8.3f us per compare   %10.0f compares/s\n",
               names[s], us, 1e6 / us);
        if (s == 2) beyond = us;
    }

    printf("\n");
    printf("  the beyond-cache figure is the one to compare, because the real workload\n");
    printf("  streams through memory it will not revisit.\n\n");

    /* Put it next to what was measured on the boards, so the comparison is in one place and
     * nobody has to go looking for the other half of it. */
    printf("  measured on hardware, same source file:\n");
    printf("    Teensy 4.1  600 MHz, internal RAM     3.660 us    273224 compares/s\n");
    printf("    Teensy 4.1  600 MHz, PSRAM           31.340 us     31908 compares/s\n");
    printf("    ESP32-S3    240 MHz, internal SRAM   49.140 us     20350 compares/s\n");
    printf("    ESP32-S3    240 MHz, PSRAM           60.070 us     16647 compares/s\n");
    printf("    this host,           beyond cache   %7.3f us  %10.0f compares/s\n",
           beyond, 1e6 / beyond);
    printf("\n");
    printf("  one host core is worth %.1f Teensys or %.1f ESP32-S3s on this kernel.\n",
           3.660 / beyond, 49.140 / beyond);
    printf("  multiply by the core count for the whole machine, remembering that they all\n");
    printf("  share one memory bus and the boards do not.\n");
    printf("===============================================================\n\n");

    free(mem);
    return 0;
}

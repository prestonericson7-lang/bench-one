/* ===========================================================================================
 *  system_test_teensy.ino -- run the BENCH ONE system test on the silicon it is for
 * ===========================================================================================
 *
 *  There is no C compiler on the development PC, and that turns out not to matter, because a PC
 *  was always the wrong place to run this. A desktop answers every query out of cache in
 *  microseconds. The behaviour this architecture is built around -- a scan that runs out of
 *  deadline halfway through, a node that goes quiet, the cost of a vector that is not in tightly
 *  coupled memory -- does not occur there. Every claim would pass and nothing would be tested.
 *
 *  On an M7 they occur. 340 KB of hypervectors does not fit in the 32 KB data cache or in DTCM,
 *  so the scan is memory bound exactly as it is on a real node, and the cycle counter reports
 *  what it really costs.
 *
 *  BOARD    Teensy 4.1
 *  OPTIMISE Faster  (or Fastest; both are fine, and the numbers change, which is the point)
 *  THEN     Tools -> Serial Monitor at 115200. It starts on its own and takes about a minute.
 *
 *  READING THE OUTPUT
 *  -------------------
 *  Four claims, each with a stated pass condition. The two columns that matter most are
 *  "confidently wrong", which must stay at zero as nodes are stalled, and cycles per word, which
 *  is what decides how many vectors a node can hold and still answer inside a deadline.
 * ===========================================================================================
 */

#include <Arduino.h>
#include <stdarg.h>

extern "C" {
    int  tprintf(const char *fmt, ...);
    int  system_test_run(int argc, char **argv);
}

/* The test code is plain C and prints with printf. Route that at Serial rather than rewrite the
 * test for the Arduino API -- the same source then runs unchanged wherever a libc exists. */
extern "C" int tprintf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.write(buf, (n > 0 && n < (int)sizeof(buf)) ? (size_t)n : strlen(buf));
    return n;
}

/* ------------------------------------------------------------------------------------------- */
/*  The measurement a PC cannot give: cycles per 32-bit word of Hamming distance.               */
/* ------------------------------------------------------------------------------------------- */

#include "bench_hdc.h"

static void report_kernel_cost()
{
    /* DWT's cycle counter is the only honest clock here. micros() has a 1 us granularity, and at
     * 600 MHz a whole 8192-bit comparison takes about 6 us, so micros() would be measuring its
     * own resolution. */
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;

    static hd_t a, b;
    uint32_t rng = 0x13579BDFu;
    hd_random(a, &rng);
    hd_random(b, &rng);

    /* Warm the cache deliberately, then measure. Reporting a cold-cache figure as if it were the
     * steady state is how a benchmark flatters itself. */
    volatile uint32_t sink = 0;
    for (int i = 0; i < 64; i++) sink += hd_hamming(a, b);

    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (int i = 0; i < 1024; i++) sink += hd_hamming(a, b);
    const uint32_t dt = ARM_DWT_CYCCNT - t0;
    (void)sink;

    const double per_call = (double)dt / 1024.0;
    tprintf("\n");
    tprintf("kernel cost on this chip, cache warm\n");
    tprintf("  %d-bit compare : %.0f cycles\n", HD_BITS, per_call);
    tprintf("  per 32-bit word: %.2f cycles\n", per_call / (double)HD_WORDS);
    tprintf("  at %u MHz that is %.2f us per compare\n",
            (unsigned)(F_CPU_ACTUAL / 1000000u),
            per_call / (double)F_CPU_ACTUAL * 1e6);
#ifdef __ARM_FEATURE_DSP
    tprintf("  USAD8 path: COMPILED IN\n");
#else
    tprintf("  USAD8 path: NOT compiled -- expect roughly 1.7x this\n");
#endif
    tprintf("  a node holding N vectors answers in N x that, so a 50 ms deadline covers\n");
    tprintf("  about %.0f vectors before it has to reply partially.\n",
            0.050 * (double)F_CPU_ACTUAL / per_call);
}

/* ------------------------------------------------------------------------------------------- */

void setup()
{
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) { }
    delay(300);

    report_kernel_cost();

    const uint32_t c0 = ARM_DWT_CYCCNT;
    const int rc = system_test_run(0, NULL);
    const uint32_t c1 = ARM_DWT_CYCCNT;

    tprintf("whole suite: %.2f s of silicon time\n",
            (double)(c1 - c0) / (double)F_CPU_ACTUAL);
    tprintf(rc ? "EXIT: a claim failed\n" : "EXIT: clean\n");
}

void loop()
{
    delay(1000);
}

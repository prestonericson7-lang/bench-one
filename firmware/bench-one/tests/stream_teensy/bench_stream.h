/* ===========================================================================================
 *  bench_stream.h -- actual memory bandwidth, which is the only number the architecture needs
 * ===========================================================================================
 *
 *  WHY EVERY EARLIER MEASUREMENT WAS THE WRONG ONE
 *  ------------------------------------------------
 *  Everything measured on these boards so far was a compare kernel: read a vector, XOR it, count
 *  the bits. That number mixes memory and arithmetic and cannot be separated afterwards. On a
 *  Teensy the arithmetic dominates from internal RAM (3.66 us) and the memory dominates from PSRAM
 *  (31.34 us), and there is no way to tell from those two figures alone how many bytes per second
 *  either path can actually deliver.
 *
 *  Single-stream model inference is memory-bandwidth bound. Tokens per second is bytes per second
 *  divided by bytes per token, and compute barely enters it. So the byte rate IS the architecture's
 *  headline number, and until it is measured every scaling estimate is built on a proxy.
 *
 *  THREE OPERATIONS, BECAUSE THEY DO NOT COST THE SAME
 *  ----------------------------------------------------
 *      READ   sum the words. One direction, no write traffic. This is the one inference needs,
 *             because weights are read and never written.
 *      WRITE  fill the words. Often faster than read on a cached machine because a write can be
 *             posted and forgotten, and often slower on a bus with no write buffer.
 *      COPY   both at once. The classic STREAM figure and the pessimistic one.
 *
 *  Quoting COPY when the workload only reads understates a machine, and quoting READ when it does
 *  both overstates it. Inference reads. Watch the READ column.
 *
 *  HOW IT AVOIDS MEASURING THE CACHE INSTEAD OF THE MEMORY
 *  --------------------------------------------------------
 *  A buffer that fits in cache measures the cache. The caller passes the size, and it must be
 *  several times the last-level cache for the number to mean anything -- on a Teensy 4.1 that means
 *  well past 32 KB, and PSRAM is the interesting case anyway. Each pass walks forward once and
 *  never revisits, which is exactly what streaming weights does.
 *
 *  The read loop keeps its accumulator in a plain local and publishes it to a volatile once at the
 *  end. Accumulating straight into a volatile is the obvious way to stop the compiler deleting a
 *  loop whose result is unused, and it quietly turns a read test into a read-modify-write. See the
 *  note in the READ block.
 * ===========================================================================================
 */

#ifndef BENCH_STREAM_H
#define BENCH_STREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    double read_mbs;
    double write_mbs;
    double copy_mbs;
    uint32_t bytes;
} stream_result_t;

/* `cycles()` must return a free-running cycle counter and `hz` the rate it advances at. Passing
 * the counter in keeps this file free of any platform header: the Teensy uses ARM_DWT_CYCCNT, the
 * ESP32 uses ESP.getCycleCount(), and a host uses whatever it has. */
typedef uint32_t (*stream_clock_fn)(void);

/* `a` and `b` must be distinct buffers of `bytes` each, word aligned. `b` may be NULL, in which
 * case COPY is skipped and reported as zero. */
static stream_result_t bench_stream(volatile uint32_t *a, volatile uint32_t *b,
                                    uint32_t bytes, uint32_t reps,
                                    stream_clock_fn cycles, double hz)
{
    stream_result_t r;
    r.bytes = bytes;
    r.read_mbs = r.write_mbs = r.copy_mbs = 0.0;

    const uint32_t words = bytes / 4u;
    if (!a || words == 0u || reps == 0u) return r;

    /* Prime it once so the first timed pass is not paying for page faults or a cold TLB. */
    for (uint32_t i = 0; i < words; i++) a[i] = i;

    /* ---- READ ------------------------------------------------------------------------- */
    {
        /* The accumulator is a PLAIN local, published to a volatile once at the end.
         *
         * The first version accumulated straight into a volatile, which is the obvious way to stop
         * the compiler deleting a loop whose result is unused -- and it silently turned the read
         * test into a read-modify-WRITE with a dependency chain through memory on every iteration.
         * It reported 381 MB/s against 1144 for the pure write loop, and a read being three times
         * slower than a write is the tell that the measurement is not measuring reads.
         *
         * Publishing once at the end keeps the loop alive, because the compiler cannot prove the
         * sum is unused, while leaving the inner loop as nothing but loads. */
        uint32_t sum = 0;
        const uint32_t t0 = cycles();
        for (uint32_t k = 0; k < reps; k++)
            for (uint32_t i = 0; i < words; i++) sum += a[i];
        const uint32_t dt = cycles() - t0;
        static volatile uint32_t keep;
        keep = sum;
        (void)keep;
        if (dt) r.read_mbs = ((double)bytes * reps) / ((double)dt / hz) / 1048576.0;
    }

    /* ---- WRITE ------------------------------------------------------------------------ */
    {
        const uint32_t t0 = cycles();
        for (uint32_t k = 0; k < reps; k++)
            for (uint32_t i = 0; i < words; i++) a[i] = k + i;
        const uint32_t dt = cycles() - t0;
        if (dt) r.write_mbs = ((double)bytes * reps) / ((double)dt / hz) / 1048576.0;
    }

    /* ---- COPY ------------------------------------------------------------------------- */
    if (b) {
        const uint32_t t0 = cycles();
        for (uint32_t k = 0; k < reps; k++)
            for (uint32_t i = 0; i < words; i++) b[i] = a[i];
        const uint32_t dt = cycles() - t0;
        /* Both directions count: a copy moves `bytes` in and `bytes` out. */
        if (dt) r.copy_mbs = ((double)bytes * 2.0 * reps) / ((double)dt / hz) / 1048576.0;
    }

    return r;
}

/* What a byte rate means for inference. `active_gb` is the weight volume touched per token: the
 * whole model for a dense one, or roughly 5% of it for a mixture of experts. */
static double stream_tokens_per_sec(double mbs, double active_gb)
{
    if (mbs <= 0.0 || active_gb <= 0.0) return 0.0;
    return (mbs / 1024.0) / active_gb;
}

#endif /* BENCH_STREAM_H */

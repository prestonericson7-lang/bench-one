/* ===========================================================================================
 *  neon1bit.c -- what the Luckfox can do on weights, instead of waiting for the Teensy
 *
 *  THE QUESTION THIS ANSWERS
 *
 *  This board has DDR2 at roughly 930 MB/s and a Cortex-A7 with NEON, and in the machine as built it
 *  spends fourteen seconds of every pass idle, holding a UART open while a microcontroller reads weights
 *  at 3.88 MB/s. That is the largest single waste in the design: the fast memory in the box is the
 *  memory nothing computes out of.
 *
 *  So: how fast can this core multiply-accumulate one-bit weights out of its own DDR2? That one number
 *  decides the architecture. If it is large, the model's hot weights belong here and the Teensy becomes
 *  a capacity extender that runs CONCURRENTLY rather than a bottleneck everything waits on.
 *
 *  THE ARITHMETIC, matched bit-for-bit to the Teensy kernel so the two ends agree
 *
 *  A bit stores 0 or 1 and means -1 or +1, so
 *
 *      sum = SUM (2b - 1) x  =  2 * SUM_{b=1} x  -  SUM x
 *
 *  which is one masked accumulation plus a constant. In NEON: broadcast the weight byte across eight
 *  lanes, AND with {1,2,4,...,128}, compare-equal against the same constant to get 0xFF where the bit is
 *  set, AND that mask into the activations, and widen-accumulate. Six operations for eight weights.
 *
 *  Activations are int8 and weights are one bit, so the products fit in int8 and the widening pair-add
 *  chain into int16 then int32 never overflows for the block sizes used here.
 *
 *  Build with the cross compiler in tools/, run on the board, and compare against the scalar reference
 *  in the same file -- a wrong kernel that is fast is worth nothing.
 * ======================================================================================== */

#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NACT 2048          /* the activation vector, same length the Teensy uses */

static int8_t xvec[NACT];

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* The truth, written the obvious way. Slow on purpose. */
static int64_t mac1_scalar(const uint8_t *w, size_t nbytes)
{
    int64_t total = 0;
    for (size_t j = 0; j < nbytes; j++) {
        const uint8_t b = w[j];
        for (int f = 0; f < 8; f++)
            total += (2 * ((b >> f) & 1) - 1) * (int)xvec[(8 * j + f) & (NACT - 1)];
    }
    return total;
}

/* Eight weights a byte, sixteen bytes a NEON load, and the activation vector wraps every 256 bytes --
 * which divides every block size used here, so no chunk boundary lands mid-wrap. */
static int64_t mac1_neon(const uint8_t *w, size_t nbytes, int64_t xsum)
{
    const uint8x8_t bitsel = { 1, 2, 4, 8, 16, 32, 64, 128 };
    int32x4_t acc = vdupq_n_s32(0);

    for (size_t j = 0; j < nbytes; j += 8) {
        /* eight weight bytes = 64 weights, against 64 activations starting at (8j & 2047) */
        const int8_t *x = &xvec[(8 * j) & (NACT - 1)];
        int16x8_t part = vdupq_n_s16(0);

        for (int k = 0; k < 8; k++) {
            const uint8x8_t bcast = vdup_n_u8(w[j + k]);
            const uint8x8_t sel   = vand_u8(bcast, bitsel);
            const uint8x8_t mask  = vceq_u8(sel, bitsel);          /* 0xFF where the bit is set */
            const int8x8_t  a     = vld1_s8(x + 8 * k);
            const int8x8_t  kept  = vand_s8(a, vreinterpret_s8_u8(mask));
            part = vaddw_s8(part, kept);
        }
        acc = vpadalq_s16(acc, part);
    }

    int64_t set_sum = (int64_t)vgetq_lane_s32(acc, 0) + vgetq_lane_s32(acc, 1)
                    + vgetq_lane_s32(acc, 2) + vgetq_lane_s32(acc, 3);
    /* every 256 bytes is one full wrap of the activation vector */
    return 2 * set_sum - (int64_t)(nbytes / 256) * xsum;
}

int main(int argc, char **argv)
{
    size_t mb = (argc > 1) ? (size_t)atoi(argv[1]) : 8;
    int reps  = (argc > 2) ? atoi(argv[2]) : 3;
    size_t n  = mb * 1024u * 1024u;

    for (int i = 0; i < NACT; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    int64_t xsum = 0;
    for (int i = 0; i < NACT; i++) xsum += xvec[i];

    uint8_t *w = (uint8_t *)malloc(n);
    if (!w) {
        printf("  cannot allocate %zu MB -- free memory is the constraint on this board\n", mb);
        return 1;
    }
    /* the same rule the Teensy fills its banks with, so both ends can be compared */
    for (size_t i = 0; i < n; i++) w[i] = (uint8_t)(i * 0x9Du + 0x3Bu);

    printf("  luckfox 1-bit NEON, %zu MB of weights = %zu million parameters\n",
           mb, (size_t)(mb * 8u));

    /* correctness first, over a block small enough for the scalar reference to finish */
    const size_t chk = 65536;
    const int64_t want = mac1_scalar(w, chk);
    const int64_t got  = mac1_neon(w, chk, xsum);
    printf("  kernel check over %zu kB: neon %lld  scalar %lld  %s\n",
           chk / 1024, (long long)got, (long long)want,
           (got == want) ? "MATCH" : "MISMATCH -- the rate below means nothing");
    if (got != want) return 2;

    double best = 0.0;
    int64_t sink = 0;
    for (int r = 0; r < reps; r++) {
        const double t0 = now_s();
        sink += mac1_neon(w, n, xsum);
        const double dt = now_s() - t0;
        const double mbps = (double)n / dt / 1e6;
        if (mbps > best) best = mbps;
        printf("    pass %d: %6.2f s   %8.2f MB/s   %9.2f MMAC/s\n",
               r, dt, mbps, mbps * 8.0);
    }
    printf("  best %.2f MB/s = %.2f MMAC/s   (checksum %lld)\n",
           best, best * 8.0, (long long)sink);

    printf("\n  for scale, the Teensy over its 7 verified banks does 3.88 MB/s of weights,\n");
    printf("  so this core is %.0fx the whole PSRAM array on the bytes it can hold.\n", best / 3.88);
    free(w);
    return 0;
}

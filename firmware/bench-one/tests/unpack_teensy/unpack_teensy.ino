/* ===========================================================================================
 *  unpack_teensy.ino -- how fast can a Teensy turn 4-bit weights into arithmetic?
 * ===========================================================================================
 *
 *  This is the last CPU estimate in the project that can be measured with hardware on the desk. The
 *  planner has been carrying 58 MB/s for a Teensy, scaled from this desktop by clock and issue width,
 *  and scaled guesses have already been wrong once today: the Luckfox estimate was 127 MB/s and the
 *  truth was 61.
 *
 *  It runs the SAME kernel the desktop and the Luckfox ran -- shared/gguf_dot.c, unmodified. Not a
 *  reimplementation, not an approximation of it. A benchmark of a different piece of code would
 *  measure a different machine.
 *
 *
 *  TWO NUMBERS, AND THE GAP BETWEEN THEM IS THE POINT
 *  --------------------------------------------------
 *  FROM INTERNAL RAM   pure unpacking throughput, with the memory system out of the way. This is what
 *                      the planner wants, because it is the property of the processor.
 *  FROM PSRAM          what actually happens when the weights live where weights would live. PSRAM
 *                      reads at a measured 32.8 MB/s, so if unpacking is faster than that, a Teensy is
 *                      memory bound and its arithmetic is not the limit.
 *
 *  Whichever is smaller decides what a Teensy can be in this machine. The verdict so far, argued from
 *  estimates, is that microcontrollers orchestrate and FPGAs compute. This either confirms it with a
 *  number or overturns it.
 *
 *
 *  WHY THE DATA IS RANDOM RATHER THAN A REAL MODEL
 *  ------------------------------------------------
 *  Unpacking cost does not depend on the values, only on the format: the same shifts, masks and
 *  multiplies happen whatever the nibbles hold. A real model file would need 1.8 GB of storage on a
 *  board with 16 MB, to measure something that cannot vary. What must NOT be random is the SHAPE --
 *  real Q4_K block layout, real 256-element blocks, real per-sub-block scales -- and that is exactly
 *  what the shared kernel walks.
 *
 *  BOARD    Teensy 4.1
 *  OPTIMISE Faster
 *  THEN     Serial Monitor at 115200. It starts by itself.
 * ===========================================================================================
 */

#include <Arduino.h>

/* Declared here rather than by including shared/gguf.h, because the Arduino build copies the .ino into
 * a temporary directory before compiling it, so a relative include from the sketch itself cannot
 * resolve. The shared sources are still the real ones -- shared_kernels.cpp pulls them in by relative
 * path, and a .cpp is not copied. Only these four declarations are restated, and if any of them ever
 * disagrees with the header the linker says so. */
extern "C" {
float gguf_dot_q(uint32_t type, const void *raw, const int8_t *xq, const float *xs, uint64_t n);
/* Added when the Cortex-M7 DSP path went in: this sketch produced the project's headline unpacking
 * figure and could not say which of the kernels it had measured. These two make it say so, and let
 * one binary run both and compare. */
const char *gguf_dot_kernel(void);
void  gguf_dot_force_scalar(int on);
int   gguf_dequant(uint32_t type, const void *raw, uint64_t n, float *out);
void  gguf_quantize_act(const float *x, uint64_t n, int8_t *xq, float *xs);
}

enum { GGML_Q4_K = 12, GGML_Q6_K = 14 };

#define QK_K        256
#define Q4K_BYTES   144
#define Q6K_BYTES   210

/* Internal buffer: big enough to be well past the 32 KB data cache so the measurement is of work and
 * not of cache, small enough to leave the Teensy room to breathe. */
#define INT_BLOCKS  512
static uint8_t  int_buf[INT_BLOCKS * Q6K_BYTES];

/* The same thing in PSRAM, larger, because that is where weights would actually sit. */
#define PS_BLOCKS   8192
EXTMEM uint8_t ps_buf[PS_BLOCKS * Q6K_BYTES];

static int8_t xq[QK_K];
static float  xs[QK_K / 32];

extern "C" uint8_t external_psram_size;

/* Results are kept and reprinted, because setup() runs once and a USB serial monitor attached a second
 * later finds an empty port and an idle loop. Measuring once and reporting forever is also the honest
 * arrangement: rerunning the benchmark on every print would quietly report a warm cache. */
static float r_int_q4, r_int_q6, r_int_deq, r_int_read;
static float r_ps_read, r_ps_q4, r_ps_q6, r_mix;
static float r_fmac, r_imac;
static bool  r_ready = false;

static void cyccnt_begin()
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (uint8_t)(s >> 16);
    }
}

/* MB/s of PACKED bytes consumed. Packed, not unpacked: the whole point of 4 bits is how little has to
 * be read, so measuring the floats that come out would flatter every format equally and compare
 * nothing. */
static float bench_fused(const uint8_t *buf, uint32_t blocks, uint32_t type, uint32_t blk_bytes)
{
    volatile float sink = 0.0f;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (uint32_t b = 0; b < blocks; b++)
        sink += gguf_dot_q(type, buf + (size_t)b * blk_bytes, xq, xs, QK_K);
    const uint32_t c1 = ARM_DWT_CYCCNT;
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    if (sink == 12345.6789f) Serial.print(' ');
    return (float)(blocks * blk_bytes) / secs / 1e6f;
}

static float bench_dequant(const uint8_t *buf, uint32_t blocks, uint32_t type, uint32_t blk_bytes)
{
    static float out[QK_K];
    volatile float sink = 0.0f;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (uint32_t b = 0; b < blocks; b++) {
        gguf_dequant(type, buf + (size_t)b * blk_bytes, QK_K, out);
        sink += out[0];
    }
    const uint32_t c1 = ARM_DWT_CYCCNT;
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    if (sink == 12345.6789f) Serial.print(' ');
    return (float)(blocks * blk_bytes) / secs / 1e6f;
}

/* ---------------------------------------------------------------------------------------------
 *  THE ARITHMETIC ITSELF, which nothing in this project had measured
 *
 *  Every Teensy figure quoted so far -- 18.2 MB/s, and the verdict built on it -- was 4-bit weights
 *  streamed from PSRAM. That is one workload, and it happens to be the single worst thing this chip
 *  does: the slow memory and the shift-and-mask arithmetic, both at once.
 *
 *  The Cortex-M7 has a single-precision FPU that issues a fused multiply-add every cycle. That is the
 *  same operation a GPU shader core performs, and the difference is how many run at once, not what
 *  they are. TGX renders a full perspective-correct, Phong-lit, z-buffered 3D pipeline on this exact
 *  board, which is not a claim about the FPU, it is a working proof of it.
 *
 *  So: how many multiply-accumulates a second, from internal memory, where the chip is not starved?
 *  That number belongs beside the desktop's measured 35 G MAC/s and the Luckfox Pico's 0.26, and
 *  until now it did not exist.
 * ------------------------------------------------------------------------------------------ */

#define MAC_N 1024
static float fa[MAC_N], fb[MAC_N];
static int8_t ia[MAC_N], ib[MAC_N];

/* Float multiply-accumulate, data in internal RAM. Four accumulators, because one long dependency
 * chain measures the FPU's latency rather than its throughput -- the same register-blocking lesson
 * that took batched prefill from 9 to 32 G MAC/s on the desktop. */
static float bench_fmac(void)
{
    volatile float sink = 0.0f;
    const int reps = 400;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (int r = 0; r < reps; r++) {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < MAC_N; i += 4) {
            s0 += fa[i + 0] * fb[i + 0];
            s1 += fa[i + 1] * fb[i + 1];
            s2 += fa[i + 2] * fb[i + 2];
            s3 += fa[i + 3] * fb[i + 3];
        }
        sink += s0 + s1 + s2 + s3;
    }
    const uint32_t c1 = ARM_DWT_CYCCNT;
    if (sink == 12345.6789f) Serial.print(' ');
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    return (float)MAC_N * reps / secs / 1e9f;
}

/* The same in 8-bit integers, which is what a GPU's tensor path and this project's weight kernel both
 * use. The M7 has no SIMD for this, so expect it to lose to the float figure rather than beat it. */
static float bench_imac(void)
{
    volatile int32_t sink = 0;
    const int reps = 400;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (int r = 0; r < reps; r++) {
        int32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < MAC_N; i += 4) {
            s0 += (int32_t)ia[i + 0] * ib[i + 0];
            s1 += (int32_t)ia[i + 1] * ib[i + 1];
            s2 += (int32_t)ia[i + 2] * ib[i + 2];
            s3 += (int32_t)ia[i + 3] * ib[i + 3];
        }
        sink += s0 + s1 + s2 + s3;
    }
    const uint32_t c1 = ARM_DWT_CYCCNT;
    if (sink == 123456789) Serial.print(' ');
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    return (float)MAC_N * reps / secs / 1e9f;
}

/* Straight read, nothing else, so the unpack figures have something on the same board to sit against. */
static float bench_read(const volatile uint8_t *buf, size_t bytes)
{
    const uint32_t *p = (const uint32_t *)buf;
    const size_t words = bytes / 4;
    uint32_t sum = 0;
    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (size_t i = 0; i < words; i++) sum += p[i];
    const uint32_t c1 = ARM_DWT_CYCCNT;
    /* Consumed, not accumulated into a volatile: an earlier version of bench_stream added to a
     * volatile and paid for a store every iteration, reporting reads at a third of their real speed. */
    if (sum == 0xDEADBEEFu) Serial.print(' ');
    const float secs = (float)(c1 - c0) / (float)F_CPU_ACTUAL;
    return (float)bytes / secs / 1e6f;
}

static void report()
{
    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  Teensy 4.1: turning 4-bit weights into arithmetic"));
    Serial.println(F("=============================================================="));
    Serial.print(F("  CPU "));
    Serial.print(F_CPU_ACTUAL / 1000000);
    Serial.print(F(" MHz, PSRAM "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB"));

    /* A plausible activation. Its values do not change the cost, but quantizing it properly means the
     * kernel walks the same path it walks in the real runtime. */
    static float act[QK_K];
    for (int i = 0; i < QK_K; i++) act[i] = ((float)(i % 37) - 18.0f) * 0.05f;
    gguf_quantize_act(act, QK_K, xq, xs);

    fill(int_buf, sizeof(int_buf), 0xC0FFEE);

    Serial.println(F("\n  RAW ARITHMETIC, internal RAM -- the same operation a GPU core runs"));
    Serial.print(F("    float multiply-add  ")); Serial.print(r_fmac, 3);
    Serial.println(F(" G MAC/s"));
    Serial.print(F("    int8 multiply-add   ")); Serial.print(r_imac, 3);
    Serial.println(F(" G MAC/s"));
    Serial.println(F("    for scale: this desktop 35.0, a Luckfox Pico 0.26, a 2080 Super ~5500"));
    Serial.print(F("    nine of these boards together: "));
    Serial.print(r_fmac * 9.0f, 2);
    Serial.println(F(" G MAC/s in float"));

    /* ---- which kernel is actually running, and is it the same arithmetic ------------------- *
     * This sketch produced the 39.3 MB/s that the whole FPGA argument is built on, and it never
     * said which code path it measured. A Teensy 4.1 is a Cortex-M7: no NEON, so the NEON path
     * written for the Cortex-A parts never applied, and it has been measuring the scalar reference
     * all along. The DSP path added to shared/gguf_dot.c changes that, and the first thing to
     * establish is not that it is faster but that it computes the same number. */
    Serial.print(F("\n  kernel path: "));
    Serial.println(gguf_dot_kernel());

    gguf_dot_force_scalar(0);
    const float fast_q4 = bench_fused(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    const float fast_val = gguf_dot_q(GGML_Q4_K, int_buf, xq, xs, QK_K);
    gguf_dot_force_scalar(1);
    const float ref_q4 = bench_fused(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    const float ref_val = gguf_dot_q(GGML_Q4_K, int_buf, xq, xs, QK_K);
    gguf_dot_force_scalar(0);

    uint32_t fb, rb;
    memcpy(&fb, &fast_val, 4);
    memcpy(&rb, &ref_val, 4);
    Serial.print(F("    Q4_K one block: fast 0x")); Serial.print(fb, HEX);
    Serial.print(F("  reference 0x"));              Serial.print(rb, HEX);
    Serial.println(fb == rb ? F("   bit-identical") : F("   DIFFERENT -- the fast path is wrong"));
    Serial.print(F("    Q4_K fused dot   scalar ")); Serial.print(ref_q4, 2);
    Serial.print(F(" MB/s   fast ")); Serial.print(fast_q4, 2);
    Serial.print(F(" MB/s   "));      Serial.print(fast_q4 / ref_q4, 2);
    Serial.println(F("x"));

    /* Q6_K the same way. It is 31% of the real file by bytes and time adds while rates do not, so the
     * mix ceiling is the harmonic mean -- which makes the slower format matter more than its share. */
    gguf_dot_force_scalar(0);
    const float fast_q6 = bench_fused(int_buf, INT_BLOCKS, GGML_Q6_K, Q6K_BYTES);
    const float fast6v  = gguf_dot_q(GGML_Q6_K, int_buf, xq, xs, QK_K);
    gguf_dot_force_scalar(1);
    const float ref_q6  = bench_fused(int_buf, INT_BLOCKS, GGML_Q6_K, Q6K_BYTES);
    const float ref6v   = gguf_dot_q(GGML_Q6_K, int_buf, xq, xs, QK_K);
    gguf_dot_force_scalar(0);

    uint32_t f6, r6;
    memcpy(&f6, &fast6v, 4);
    memcpy(&r6, &ref6v, 4);
    Serial.print(F("    Q6_K one block: fast 0x")); Serial.print(f6, HEX);
    Serial.print(F("  reference 0x"));              Serial.print(r6, HEX);
    Serial.println(f6 == r6 ? F("   bit-identical") : F("   DIFFERENT -- the fast path is wrong"));
    Serial.print(F("    Q6_K fused dot   scalar ")); Serial.print(ref_q6, 2);
    Serial.print(F(" MB/s   fast ")); Serial.print(fast_q6, 2);
    Serial.print(F(" MB/s   "));      Serial.print(fast_q6 / ref_q6, 2);
    Serial.println(F("x"));

    const float mix_ref  = 1.0f / (0.69f / ref_q4  + 0.31f / ref_q6);
    const float mix_fast = 1.0f / (0.69f / fast_q4 + 0.31f / fast_q6);
    Serial.print(F("    the real 69/31 mix: scalar ")); Serial.print(mix_ref, 2);
    Serial.print(F(" MB/s   fast ")); Serial.print(mix_fast, 2);
    Serial.print(F(" MB/s   ")); Serial.print(mix_fast / mix_ref, 2);
    Serial.println(F("x"));

    Serial.println(F("\n  FROM INTERNAL RAM -- the processor's own unpacking rate"));
    Serial.print(F("    Q4_K fused dot   "));
    Serial.print(bench_fused(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES), 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    Q6_K fused dot   "));
    Serial.print(bench_fused(int_buf, INT_BLOCKS, GGML_Q6_K, Q6K_BYTES), 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    Q4_K dequantize  "));
    Serial.print(bench_dequant(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES), 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    plain read       "));
    Serial.print(bench_read(int_buf, sizeof(int_buf)), 2);
    Serial.println(F(" MB/s"));

    if (external_psram_size == 0) {
        Serial.println(F("\n  No PSRAM found, so the second half cannot run."));
        return;
    }

    Serial.println(F("\n  filling PSRAM, this takes a moment"));
    fill(ps_buf, sizeof(ps_buf), 0xBEEF);

    Serial.println(F("  FROM PSRAM -- where weights would actually live"));
    Serial.print(F("    plain read       "));
    Serial.print(bench_read(ps_buf, sizeof(ps_buf)), 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    Q4_K fused dot   "));
    const float ps_q4 = bench_fused(ps_buf, PS_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    Serial.print(ps_q4, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    Q6_K fused dot   "));
    const float ps_q6 = bench_fused(ps_buf, PS_BLOCKS, GGML_Q6_K, Q6K_BYTES);
    Serial.print(ps_q6, 2);
    Serial.println(F(" MB/s"));

    /* The real file is 69% Q4_K and 31% Q6_K by bytes. Time adds, rates do not, so the ceiling for
     * that mix is the harmonic mean rather than the average. */
    const float mix = 1.0f / (0.69f / ps_q4 + 0.31f / ps_q6);
    Serial.print(F("\n  the real 69/31 mix from PSRAM: "));
    Serial.print(mix, 2);
    Serial.println(F(" MB/s"));

    Serial.println(F("\n  WHAT IT MEANS"));
    Serial.print(F("  A 49.2 MB transformer layer would take "));
    Serial.print(49.2f / mix, 1);
    Serial.println(F(" s to read once at this rate."));
    Serial.println(F("  The planner was carrying 58 MB/s for this board, scaled from a desktop."));
    Serial.println(F("  Whatever is printed above replaces it."));
    Serial.println();
}

static void measure()
{
    /* A plausible activation. Its values do not change the cost, but quantizing it properly means the
     * kernel walks the same path it walks in the real runtime. */
    static float act[QK_K];
    for (int i = 0; i < QK_K; i++) act[i] = ((float)(i % 37) - 18.0f) * 0.05f;
    gguf_quantize_act(act, QK_K, xq, xs);

    for (int i = 0; i < MAC_N; i++) {
        fa[i] = ((float)(i % 23) - 11.0f) * 0.031f;
        fb[i] = ((float)(i % 17) - 8.0f) * 0.047f;
        ia[i] = (int8_t)((i % 200) - 100);
        ib[i] = (int8_t)((i % 150) - 75);
    }
    r_fmac = bench_fmac();
    r_imac = bench_imac();

    fill(int_buf, sizeof(int_buf), 0xC0FFEE);
    r_int_q4   = bench_fused(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    r_int_q6   = bench_fused(int_buf, INT_BLOCKS, GGML_Q6_K, Q6K_BYTES);
    r_int_deq  = bench_dequant(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    r_int_read = bench_read(int_buf, sizeof(int_buf));

    if (external_psram_size) {
        fill(ps_buf, sizeof(ps_buf), 0xBEEF);
        r_ps_read = bench_read(ps_buf, sizeof(ps_buf));
        r_ps_q4   = bench_fused(ps_buf, PS_BLOCKS, GGML_Q4_K, Q4K_BYTES);
        r_ps_q6   = bench_fused(ps_buf, PS_BLOCKS, GGML_Q6_K, Q6K_BYTES);
        /* The real file is 69% Q4_K and 31% Q6_K by bytes. Time adds, rates do not, so the ceiling for
         * that mix is the harmonic mean rather than the average. */
        r_mix = 1.0f / (0.69f / r_ps_q4 + 0.31f / r_ps_q6);
    }
    r_ready = true;
}

void setup()
{
    cyccnt_begin();
    Serial.begin(115200);
    /* Measure first, without waiting for a monitor. Waiting and then measuring would let a host that
     * attaches late change when the benchmark runs, and the PSRAM fill alone takes seconds. */
    measure();
}

void loop()
{
    if (r_ready) report();
    delay(6000);
}

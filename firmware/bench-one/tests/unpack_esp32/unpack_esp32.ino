/* ===========================================================================================
 *  unpack_esp32.ino -- how fast can an ESP32-S3 turn 4-bit weights into arithmetic?
 * ===========================================================================================
 *
 *  The companion to unpack_teensy. Same kernel, same question, different processor. Between them they
 *  close the last two CPU estimates in the project that can be settled with hardware on the desk; only
 *  the Zynq's memory speed is left, and those boards are still in transit.
 *
 *  The estimates have not held up well. The Luckfox was guessed at 127 MB/s and measured 61. The Teensy
 *  was guessed at 58 and measured 39.3 from internal memory, 21.4 from PSRAM. This board is currently
 *  carrying 11 MB/s, scaled from the Teensy's measured figure by clock, and there is no reason to trust
 *  that any more than the other two.
 *
 *  It runs shared/gguf_dot.c unmodified -- the same code the desktop, the Luckfox and the Teensy ran.
 *  A benchmark of a reimplementation measures a different machine.
 *
 *
 *  WHAT THIS BOARD IS FOR, AND WHAT THE NUMBER DECIDES
 *  ----------------------------------------------------
 *  An ESP32-S3 has 8 MB of PSRAM, which cannot hold a 49.2 MB transformer layer, so it was never going
 *  to carry weights for this model. What it can do is hold a small model's layer, or one expert of a
 *  mixture, or act as the thing that moves activations and keeps time while the FPGAs compute.
 *
 *  There is also a known reason to keep it away from anything timed: measured earlier in this project,
 *  its radio stack steals about 0.55 ms whenever it feels like it. That is nothing against a 50 ms
 *  budget and fatal against an 8 us one.
 *
 *
 *  TWO NUMBERS, AND THE GAP IS THE POINT
 *  --------------------------------------
 *  FROM INTERNAL RAM  the processor's own unpacking rate, memory system out of the way.
 *  FROM PSRAM         what happens when the weights live where weights would live.
 *
 *  On a processor those two costs ADD rather than one dominating -- measured on the Teensy, where
 *  33.9 MB/s of reading and 39.3 of unpacking came to 21.4 together. Expect the same shape here.
 *
 *  BOARD    ESP32-S3 with PSRAM enabled (Tools > PSRAM > OPI or QSPI, matching your module)
 *  THEN     Serial Monitor at 115200. It measures once and reprints every six seconds.
 * ===========================================================================================
 */

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

extern "C" {
float gguf_dot_q(uint32_t type, const void *raw, const int8_t *xq, const float *xs, uint64_t n);
int   gguf_dequant(uint32_t type, const void *raw, uint64_t n, float *out);
void  gguf_quantize_act(const float *x, uint64_t n, int8_t *xq, float *xs);
}

enum { GGML_Q4_K = 12, GGML_Q6_K = 14 };

#define QK_K       256
#define Q4K_BYTES  144
#define Q6K_BYTES  210

/* Internal: past the cache, small enough to leave the radio stack its heap. */
#define INT_BLOCKS  256
static uint8_t int_buf[INT_BLOCKS * Q6K_BYTES];

/* PSRAM: allocated rather than declared, because on this chip external memory comes from the heap with
 * a capability flag and not from a linker section. */
#define PS_BLOCKS   4096
static uint8_t *ps_buf = nullptr;

static int8_t xq[QK_K];
static float  xs[QK_K / 32];

static float r_int_q4, r_int_q6, r_int_deq, r_int_read;
static float r_ps_read, r_ps_q4, r_ps_q6, r_mix;
static bool  r_ready = false;
static bool  r_psram = false;

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        p[i] = (uint8_t)(s >> 16);
    }
}

/* MB/s of PACKED bytes consumed. Packed, not unpacked: the point of 4 bits is how little has to be
 * read, so counting the floats that come out would flatter every format equally and compare nothing. */
static float bench_fused(const uint8_t *buf, uint32_t blocks, uint32_t type, uint32_t blk_bytes)
{
    volatile float sink = 0.0f;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t b = 0; b < blocks; b++)
        sink += gguf_dot_q(type, buf + (size_t)b * blk_bytes, xq, xs, QK_K);
    const int64_t t1 = esp_timer_get_time();
    if (sink == 12345.6789f) Serial.print(' ');
    return (float)(blocks * blk_bytes) / ((float)(t1 - t0) * 1e-6f) / 1e6f;
}

static float bench_dequant(const uint8_t *buf, uint32_t blocks, uint32_t type, uint32_t blk_bytes)
{
    static float out[QK_K];
    volatile float sink = 0.0f;
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t b = 0; b < blocks; b++) {
        gguf_dequant(type, buf + (size_t)b * blk_bytes, QK_K, out);
        sink += out[0];
    }
    const int64_t t1 = esp_timer_get_time();
    if (sink == 12345.6789f) Serial.print(' ');
    return (float)(blocks * blk_bytes) / ((float)(t1 - t0) * 1e-6f) / 1e6f;
}

static float bench_read(const uint8_t *buf, size_t bytes)
{
    const uint32_t *p = (const uint32_t *)buf;
    const size_t words = bytes / 4;
    uint32_t sum = 0;
    const int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < words; i++) sum += p[i];
    const int64_t t1 = esp_timer_get_time();
    /* Consumed, not accumulated into a volatile: accumulating into one costs a store per iteration and
     * reported reads at a third of their real speed the first time this project measured them. */
    if (sum == 0xDEADBEEFu) Serial.print(' ');
    return (float)bytes / ((float)(t1 - t0) * 1e-6f) / 1e6f;
}

static void measure()
{
    static float act[QK_K];
    for (int i = 0; i < QK_K; i++) act[i] = ((float)(i % 37) - 18.0f) * 0.05f;
    gguf_quantize_act(act, QK_K, xq, xs);

    fill(int_buf, sizeof(int_buf), 0xC0FFEE);
    r_int_q4   = bench_fused(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    r_int_q6   = bench_fused(int_buf, INT_BLOCKS, GGML_Q6_K, Q6K_BYTES);
    r_int_deq  = bench_dequant(int_buf, INT_BLOCKS, GGML_Q4_K, Q4K_BYTES);
    r_int_read = bench_read(int_buf, sizeof(int_buf));

    ps_buf = (uint8_t *)heap_caps_malloc((size_t)PS_BLOCKS * Q6K_BYTES, MALLOC_CAP_SPIRAM);
    if (ps_buf) {
        r_psram = true;
        fill(ps_buf, (size_t)PS_BLOCKS * Q6K_BYTES, 0xBEEF);
        r_ps_read = bench_read(ps_buf, (size_t)PS_BLOCKS * Q6K_BYTES);
        r_ps_q4   = bench_fused(ps_buf, PS_BLOCKS, GGML_Q4_K, Q4K_BYTES);
        r_ps_q6   = bench_fused(ps_buf, PS_BLOCKS, GGML_Q6_K, Q6K_BYTES);
        /* The real file is 69% Q4_K and 31% Q6_K by bytes. Time adds, rates do not, so the ceiling for
         * the mix is the harmonic mean rather than the average. */
        r_mix = 1.0f / (0.69f / r_ps_q4 + 0.31f / r_ps_q6);
    }
    r_ready = true;
}

static void report()
{
    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  ESP32-S3: turning 4-bit weights into arithmetic"));
    Serial.println(F("=============================================================="));
    Serial.print(F("  CPU "));
    Serial.print(getCpuFrequencyMhz());
    Serial.print(F(" MHz, PSRAM "));
    Serial.print(ESP.getPsramSize() / 1048576);
    Serial.println(F(" MB"));

    Serial.println(F("\n  FROM INTERNAL RAM -- the processor's own unpacking rate"));
    Serial.print(F("    Q4_K fused dot   ")); Serial.print(r_int_q4, 2);   Serial.println(F(" MB/s"));
    Serial.print(F("    Q6_K fused dot   ")); Serial.print(r_int_q6, 2);   Serial.println(F(" MB/s"));
    Serial.print(F("    Q4_K dequantize  ")); Serial.print(r_int_deq, 2);  Serial.println(F(" MB/s"));
    Serial.print(F("    plain read       ")); Serial.print(r_int_read, 2); Serial.println(F(" MB/s"));

    if (!r_psram) {
        Serial.println(F("\n  No PSRAM available. Either the module has none or it is not enabled"));
        Serial.println(F("  in Tools > PSRAM, and without it the second half cannot run."));
        return;
    }

    Serial.println(F("\n  FROM PSRAM -- where weights would actually live"));
    Serial.print(F("    plain read       ")); Serial.print(r_ps_read, 2); Serial.println(F(" MB/s"));
    Serial.print(F("    Q4_K fused dot   ")); Serial.print(r_ps_q4, 2);   Serial.println(F(" MB/s"));
    Serial.print(F("    Q6_K fused dot   ")); Serial.print(r_ps_q6, 2);   Serial.println(F(" MB/s"));
    Serial.print(F("\n  the real 69/31 mix from PSRAM: "));
    Serial.print(r_mix, 2);
    Serial.println(F(" MB/s"));

    Serial.println(F("\n  FOR COMPARISON, all measured with this same kernel"));
    Serial.println(F("    Teensy 4.1 from PSRAM      21.40 MB/s"));
    Serial.println(F("    Luckfox Pico               57.00 MB/s"));
    Serial.println(F("    one Zynq, fabric         2290.00 MB/s"));
    Serial.print(F("  A 49.2 MB layer would take "));
    Serial.print(49.2f / r_mix, 1);
    Serial.println(F(" s to read once on this board."));
    Serial.println(F("  The planner carries 11 MB/s for it. Whatever is above replaces that."));
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    /* Measured before waiting for a monitor, so a host attaching late cannot change when the
     * benchmark ran. Reprinted from loop() because setup() happens once and an empty port tells
     * nobody anything. */
    measure();
}

void loop()
{
    if (r_ready) report();
    delay(6000);
}

/* ===========================================================================================
 *  stream_teensy.ino -- real memory bandwidth on a Teensy 4.1, internal and PSRAM
 * ===========================================================================================
 *  Every earlier figure from this board was a compare kernel, which mixes memory with arithmetic.
 *  Inference is bandwidth bound, so the byte rate is the architecture's headline number and it has
 *  to be measured on its own.
 *
 *  BOARD  Teensy 4.1, PSRAM fitted     THEN  Serial Monitor at 115200
 * ===========================================================================================
 */
#include <Arduino.h>
#include "bench_stream.h"

#define INT_BYTES   (64u * 1024u)          /* past the 32 KB data cache          */
#define PS_BYTES    (4u * 1024u * 1024u)   /* far past any cache                 */

extern "C" uint8_t external_psram_size;

static uint32_t internal_a[INT_BYTES / 4];
static uint32_t internal_b[INT_BYTES / 4];
EXTMEM uint32_t psram_a[PS_BYTES / 4];
EXTMEM uint32_t psram_b[PS_BYTES / 4];

static uint32_t cyc() { return ARM_DWT_CYCCNT; }

static void show(const char *what, stream_result_t r)
{
    Serial.printf("  %-16s %7u KB   read %8.1f   write %8.1f   copy %8.1f  MB/s\n",
                  what, r.bytes / 1024u, r.read_mbs, r.write_mbs, r.copy_mbs);
}

void setup()
{
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) { }
    delay(300);
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;

    const double hz = (double)F_CPU_ACTUAL;
    Serial.println();
    Serial.println("=================================================================");
    Serial.printf("Teensy 4.1 memory bandwidth  --  %lu MHz, %u MB PSRAM\n",
                  (unsigned long)(F_CPU_ACTUAL / 1000000u), external_psram_size);
    Serial.println("=================================================================");

    show("internal RAM", bench_stream(internal_a, internal_b, INT_BYTES, 64, cyc, hz));
    if (external_psram_size >= 8) {
        show("PSRAM", bench_stream(psram_a, psram_b, PS_BYTES, 2, cyc, hz));
    } else {
        Serial.println("  PSRAM        not fitted");
    }

    Serial.println();
    Serial.println("  READ is the column that matters: weights are read, never written.");
    Serial.println("  what one board contributes to a distributed model, per token:");
    stream_result_t ps = bench_stream(psram_a, NULL, PS_BYTES, 2, cyc, hz);
    Serial.printf("    dense, 1 GB of weights on this node : %8.4f tok/s\n",
                  stream_tokens_per_sec(ps.read_mbs, 1.0));
    Serial.printf("    MoE, 50 MB of experts active here   : %8.3f tok/s\n",
                  stream_tokens_per_sec(ps.read_mbs, 0.05));
    Serial.println("=================================================================");
}

void loop() { delay(1000); }

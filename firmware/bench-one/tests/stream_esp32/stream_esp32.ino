/* ===========================================================================================
 *  stream_esp32.ino -- real memory bandwidth on an ESP32-S3, internal and octal PSRAM
 * ===========================================================================================
 *  BOARD  ESP32-S3, PSRAM=opi, USB CDC on boot    THEN  Serial Monitor at 115200
 * ===========================================================================================
 */
#include <Arduino.h>
#include "bench_stream.h"

#define INT_BYTES  (32u * 1024u)
#define PS_BYTES   (2u * 1024u * 1024u)

static uint32_t internal_a[INT_BYTES / 4];
static uint32_t internal_b[INT_BYTES / 4];

static uint32_t cyc() { return ESP.getCycleCount(); }

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
    delay(1500);

    const double hz = (double)getCpuFrequencyMhz() * 1e6;
    Serial.println();
    Serial.println("=================================================================");
    Serial.printf("ESP32-S3 memory bandwidth  --  %u MHz, %u MB PSRAM\n",
                  (unsigned)getCpuFrequencyMhz(),
                  (unsigned)(ESP.getPsramSize() / (1024u * 1024u)));
    Serial.println("=================================================================");

    show("internal SRAM", bench_stream(internal_a, internal_b, INT_BYTES, 64, cyc, hz));

    uint32_t *pa = (uint32_t *)ps_malloc(PS_BYTES);
    uint32_t *pb = (uint32_t *)ps_malloc(PS_BYTES);
    if (pa && pb) {
        show("octal PSRAM", bench_stream(pa, pb, PS_BYTES, 4, cyc, hz));
        Serial.println();
        Serial.println("  READ is the column that matters: weights are read, never written.");
        stream_result_t ps = bench_stream(pa, NULL, PS_BYTES, 4, cyc, hz);
        Serial.printf("    MoE, 50 MB of experts active here : %8.3f tok/s\n",
                      stream_tokens_per_sec(ps.read_mbs, 0.05));
    } else {
        Serial.println("  PSRAM        could not allocate 2 MB");
    }
    Serial.println("=================================================================");
}

void loop() { delay(1000); }

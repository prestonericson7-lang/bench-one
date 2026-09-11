/* ===========================================================================================
 *  kernel_esp32s3.ino -- what an ESP32-S3 is actually worth as a node, measured
 * ===========================================================================================
 *
 *  Three numbers, and the third is the one nobody else measures.
 *
 *      1. COMPARE COST FROM INTERNAL SRAM.  The Xtensa LX7 has no population count instruction,
 *         so bench_hdc falls back to the SWAR bit trick with no USAD8 to finish it. This says what
 *         that costs at 240 MHz. My estimate in the stack model was 21.3 us and my last estimate
 *         on a Teensy was wrong by 1.65x, so it gets measured.
 *
 *      2. COMPARE COST FROM PSRAM.  Internal SRAM holds a few hundred hypervectors at most. PSRAM
 *         holds thousands. On a Teensy that swap cost 8.6x. Here it goes over a different bus and
 *         has to be measured separately.
 *
 *      3. WHAT THE RADIO COSTS.  You flagged this: the cores stop to service wifi and BLE. That is
 *         the reason this whole architecture is built on a monoid, so a node can answer partially,
 *         late, or not at all without corrupting the result. But the SIZE of the stall has never
 *         been measured, only assumed -- the stack model guesses a 20% chance of being unavailable.
 *         So this runs the same benchmark with the radio idle and again with it scanning, and
 *         reports the worst case and the distribution. That turns the guess into a number.
 *
 *  BOARD    ESP32-S3, USB CDC on boot enabled, PSRAM enabled
 *  THEN     Serial Monitor at 115200
 *
 *  Nothing here connects to a network or needs credentials. The wifi test runs an asynchronous
 *  scan, which exercises the radio and the driver's interrupt load without joining anything.
 * ===========================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include "bench_hdc.h"
#include "spread.h"

#define LOCAL_VECTORS   48          /* 48 KB, comfortably inside internal SRAM */
#define PSRAM_VECTORS   2048        /* 2 MB, far larger than any cache          */
#define STALL_SAMPLES   3000        /* individual compares timed one by one     */

static hd_t  local_vec[LOCAL_VECTORS];
static hd_t  query;
static hd_t *ps_vec = nullptr;

static inline uint32_t cycles() { return ESP.getCycleCount(); }

/* One compare, timed on its own. Timing them individually rather than in a batch is the whole
 * point of the stall test: an average hides a 10 ms interruption, and the interruption is what
 * decides whether a node makes its deadline. */
static uint32_t time_one(const hd_t *v)
{
    const uint32_t t0 = cycles();
    volatile uint32_t d = hd_hamming(*v, query);
    const uint32_t dt = cycles() - t0;
    (void)d;
    return dt;
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static Spread measure(const hd_t *base, uint32_t nvec, uint32_t samples, uint32_t *scratch)
{
    const double f = (double)getCpuFrequencyMhz();
    for (uint32_t i = 0; i < samples; i++) scratch[i] = time_one(&base[i % nvec]);

    qsort(scratch, samples, sizeof(uint32_t), cmp_u32);

    double sum = 0;
    for (uint32_t i = 0; i < samples; i++) sum += scratch[i];

    Spread s;
    s.mean_us  = sum / samples / f;
    s.p50_us   = scratch[samples / 2] / f;
    s.p99_us   = scratch[(uint32_t)(samples * 0.99)] / f;
    s.worst_us = scratch[samples - 1] / f;

    /* How often a compare took more than twice the median. That is the tail the deadline cares
     * about, and an average would never show it. */
    const uint32_t limit = scratch[samples / 2] * 2u;
    s.over_2x = 0;
    for (uint32_t i = 0; i < samples; i++) if (scratch[i] > limit) s.over_2x++;
    return s;
}

static void report(const char *what, const Spread &s, uint32_t samples)
{
    Serial.printf("    %-18s mean %7.2f  median %7.2f  p99 %8.2f  worst %9.2f us\n",
                  what, s.mean_us, s.p50_us, s.p99_us, s.worst_us);
    Serial.printf("    %-18s %u of %u compares took over twice the median (%.2f%%)\n",
                  "", s.over_2x, samples, 100.0 * s.over_2x / samples);
}

/* ------------------------------------------------------------------------------------------- */

void setup()
{
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) { }
    delay(1500);

    Serial.println();
    Serial.println("=========================================================================");
    Serial.println("ESP32-S3 as a BENCH ONE node");
    Serial.println("=========================================================================");
    Serial.println();

    Serial.printf("  chip      : %s rev %d, %d core(s) at %u MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned)getCpuFrequencyMhz());
    Serial.printf("  flash     : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024u * 1024u)));
    Serial.printf("  PSRAM     : %u MB (%u bytes free)\n",
                  (unsigned)(ESP.getPsramSize() / (1024u * 1024u)), (unsigned)ESP.getFreePsram());
    Serial.printf("  free heap : %u bytes internal\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("  die temp  : %.1f C\n", temperatureRead());
    Serial.println();

    if (ESP.getPsramSize() == 0) {
        Serial.println("  NOTE: no PSRAM found. If the board has octal PSRAM, rebuild with");
        Serial.println("        PSRAM=opi instead of PSRAM=enabled. Internal results still valid.");
        Serial.println();
    }

    uint32_t rng = 0x51F0A3C7u;
    hd_random(query, &rng);
    for (uint32_t i = 0; i < LOCAL_VECTORS; i++) hd_random(local_vec[i], &rng);

    uint32_t *scratch = (uint32_t *)malloc(STALL_SAMPLES * sizeof(uint32_t));
    if (!scratch) { Serial.println("out of memory for the sample buffer"); return; }

    /* ---- 1 & 2: the kernel, radio off ---------------------------------------------------- */
    Serial.println("[1] compare cost with the radio OFF");
    WiFi.mode(WIFI_OFF);
    delay(300);

    const Spread local_off = measure(local_vec, LOCAL_VECTORS, STALL_SAMPLES, scratch);
    report("internal SRAM", local_off, STALL_SAMPLES);

    const double f = (double)getCpuFrequencyMhz();
    Serial.printf("    %.2f cycles per 32-bit word, %d-bit compare\n",
                  local_off.p50_us * f / (double)HD_WORDS, HD_BITS);

    Spread ps_off;
    bool have_ps = false;
    if (ESP.getPsramSize() >= 4u * 1024u * 1024u) {
        ps_vec = (hd_t *)ps_malloc((size_t)PSRAM_VECTORS * sizeof(hd_t));
        if (ps_vec) {
            for (uint32_t i = 0; i < PSRAM_VECTORS; i++) hd_random(ps_vec[i], &rng);
            ps_off = measure(ps_vec, PSRAM_VECTORS, STALL_SAMPLES, scratch);
            report("PSRAM", ps_off, STALL_SAMPLES);
            Serial.printf("    PSRAM is %.2fx the cost of internal SRAM\n",
                          ps_off.p50_us / local_off.p50_us);
            have_ps = true;
        } else {
            Serial.println("    could not allocate 2 MB of PSRAM to test");
        }
    }
    Serial.printf("    die temp now %.1f C\n", temperatureRead());
    Serial.println();

    /* ---- 3: the radio, which is the point ------------------------------------------------ */
    Serial.println("[2] the same compare with the radio SCANNING -- the stall you flagged");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(200);
    WiFi.scanNetworks(true, true);          /* async, include hidden; no credentials needed */
    delay(50);

    const Spread local_on = measure(local_vec, LOCAL_VECTORS, STALL_SAMPLES, scratch);
    report("internal SRAM", local_on, STALL_SAMPLES);
    if (have_ps) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true);
        const Spread ps_on = measure(ps_vec, PSRAM_VECTORS, STALL_SAMPLES, scratch);
        report("PSRAM", ps_on, STALL_SAMPLES);
    }
    Serial.printf("    die temp now %.1f C\n", temperatureRead());
    Serial.println();

    /* ---- 4: an ACTIVE radio, which is the case that actually bites ---------------------- */
    Serial.println("[3] the radio doing real work -- association attempts, not a passive scan");
    Serial.println("    This is the case a passive scan does not cover. Watching for the longest");
    Serial.println("    GAP between consecutive compares over 15 seconds, because a node that");
    Serial.println("    disappears for a second misses about twenty queries outright rather than");
    Serial.println("    answering any of them slowly.");

    WiFi.scanDelete();
    WiFi.mode(WIFI_STA);
    WiFi.begin("bench-one-no-such-network", "not-a-real-password");

    uint32_t worst_gap = 0;
    uint32_t gaps_over_1ms = 0, gaps_over_10ms = 0, gaps_over_100ms = 0;
    uint32_t compares = 0;
    const uint32_t window_start = millis();
    uint32_t last = cycles();

    while (millis() - window_start < 15000u) {
        (void)time_one(&local_vec[compares % LOCAL_VECTORS]);
        const uint32_t now = cycles();
        const uint32_t gap = now - last;
        last = now;
        compares++;
        if (gap > worst_gap) worst_gap = gap;
        const double gap_us = gap / f;
        if (gap_us > 1000.0)   gaps_over_1ms++;
        if (gap_us > 10000.0)  gaps_over_10ms++;
        if (gap_us > 100000.0) gaps_over_100ms++;
        /* Keep nudging the radio: a failed association retries, which is real transmit work. */
        if ((compares & 0x3FFFu) == 0 && WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    }

    Serial.printf("    %u compares in 15 s, longest gap %.2f ms\n",
                  compares, worst_gap / f / 1000.0);
    Serial.printf("    gaps over   1 ms: %u\n", gaps_over_1ms);
    Serial.printf("    gaps over  10 ms: %u\n", gaps_over_10ms);
    Serial.printf("    gaps over 100 ms: %u\n", gaps_over_100ms);
    if (gaps_over_10ms) {
        Serial.printf("    a 50 ms deadline is missed outright whenever a gap exceeds it;\n");
        Serial.printf("    %u of %u compare intervals did, roughly %.3f%% of the time.\n",
                      gaps_over_10ms, compares, 100.0 * gaps_over_10ms / compares);
    } else {
        Serial.println("    nothing over 10 ms. The stall is real but not in this workload;");
        Serial.println("    it likely needs an actual association plus traffic to reproduce.");
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println();

    /* ---- what it means -------------------------------------------------------------------- */
    Serial.println("=========================================================================");
    Serial.println("WHAT THIS MAKES AN ESP32-S3 NODE");
    Serial.println("=========================================================================");

    const double per_us = local_on.p50_us;
    const uint32_t in_50ms = (uint32_t)(50000.0 / per_us);
    Serial.printf("  a 50 ms deadline covers %u vectors at the median rate\n", in_50ms);
    Serial.printf("  internal SRAM realistically holds a few hundred, so the deadline is not\n");
    Serial.printf("  the limit here either -- capacity is.\n");

    const double infl = local_on.p50_us / local_off.p50_us;
    Serial.printf("  the radio costs %.2fx on the median compare and %.2fx on the worst\n",
                  infl, local_on.worst_us / local_off.worst_us);
    Serial.printf("  worst single compare with the radio busy: %.2f us, against %.2f us idle\n",
                  local_on.worst_us, local_off.worst_us);

    /* A node that loses this fraction of its scan to the radio is one that must be allowed to
     * answer partially. That is exactly what hd_scan_chunk and the merge monoid are for. */
    Serial.printf("  %.2f%% of compares ran long enough to matter, so a scan loses roughly\n",
                  100.0 * local_on.over_2x / STALL_SAMPLES);
    Serial.printf("  that share of its budget. It must be allowed to reply partially.\n");
    Serial.println("=========================================================================");

    free(scratch);
}

void loop()
{
    delay(1000);
}

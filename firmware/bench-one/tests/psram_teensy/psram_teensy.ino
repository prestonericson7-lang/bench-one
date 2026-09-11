/* ===========================================================================================
 *  psram_teensy.ino -- prove the PSRAM you just soldered, then measure what it costs
 * ===========================================================================================
 *
 *  Run this before trusting a single byte to a freshly soldered chip, and run it again after the
 *  second one goes on. It answers three questions in order, and the order matters:
 *
 *      1. IS IT THERE?        Teensyduino probes QSPI at boot. If it reports 0 MB the chip is not
 *                             talking, and no amount of testing further down will tell you why.
 *      2. IS IT SOUND?        A hand-soldered SOIC-8 joint can look perfect and be intermittent.
 *                             Only writing and reading back the whole chip finds that.
 *      3. WHAT DOES IT COST?  The number that decides the architecture: microseconds to compare
 *                             the query against one hypervector living in PSRAM, against the same
 *                             comparison in internal RAM.
 *
 *  SOLDER ONE CHIP FIRST. Populate the second footprint only after this passes on the first. If
 *  both go on at once and the result is 8 MB instead of 16, you cannot tell which chip is at fault
 *  without desoldering, and desoldering is where boards die.
 *
 *  BOARD    Teensy 4.1
 *  OPTIMISE Faster
 *  THEN     Serial Monitor at 115200. It starts by itself.
 *
 *  WHY THE SCAN SPEED IS THE POINT
 *  --------------------------------
 *  Internal RAM makes this chip compute bound: the measured cost is 8.61 cycles per 32-bit word,
 *  which at 600 MHz is 3.68 us for a full 8192-bit compare, and a 50 ms deadline covers about
 *  13,600 vectors. But only about a thousand vectors fit in internal RAM, so the deadline was never
 *  the limit -- capacity was.
 *
 *  PSRAM inverts that. 16 MB holds 16,384 vectors, far more than the deadline can sweep, because
 *  every byte now crosses FlexSPI2 instead of sitting in tightly coupled memory. Whatever this
 *  prints for the PSRAM figure is what decides whether a Teensy with PSRAM scans linearly or needs
 *  the prefix screening that bench_index.c does for the Luckfox sticks.
 *
 *  Both a cold and a warm figure are printed. The cold one is the honest number for a full sweep,
 *  because a real scan walks memory once and never revisits it, so the cache cannot help.
 * ===========================================================================================
 */

#include <Arduino.h>
#include "bench_hdc.h"

/* Teensyduino sets this at boot from what it found on the QSPI bus. */
extern "C" uint8_t external_psram_size;

/* EXTMEM puts this in PSRAM. It is deliberately large: a scan that fits in the 32 KB data cache
 * measures the cache, not the memory. */
#define VECTORS 4096
EXTMEM hd_t psram_vec[VECTORS];

/* The same data in internal RAM, small enough to fit, for the side-by-side comparison. */
#define LOCAL_VECTORS 64
static hd_t local_vec[LOCAL_VECTORS];
static hd_t query;

static void cyccnt_begin()
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

/* ------------------------------------------------------------------------------------------- */

/* The whole external RAM region, not the EXTMEM array.
 *
 * The array is only 4 MB, and the point of a memtest is to touch every byte of the chip. Walking
 * off the end of psram_vec would happen to work, because it is the only EXTMEM object and therefore
 * sits at the base of the region, but that is an accident waiting to break the moment a second
 * EXTMEM variable is declared. The base address is architectural on the i.MX RT1062 and does not
 * move. */
#define PSRAM_BASE ((volatile uint32_t *)0x70000000u)

static bool memtest(uint32_t mb)
{
    const uint32_t words = (mb * 1024u * 1024u) / 4u;
    volatile uint32_t *p = PSRAM_BASE;

    Serial.printf("    writing %u MB ", mb);
    uint32_t seed = 0x13579BDFu;
    for (uint32_t i = 0; i < words; i++) {
        seed = seed * 1664525u + 1013904223u;
        p[i] = seed;
        if ((i & 0x3FFFFu) == 0) Serial.print(".");
    }
    Serial.println();

    Serial.printf("    reading back ");
    seed = 0x13579BDFu;
    uint32_t bad = 0;
    uint32_t first_bad = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < words; i++) {
        seed = seed * 1664525u + 1013904223u;
        if (p[i] != seed) {
            if (bad == 0) first_bad = i * 4u;
            bad++;
        }
        if ((i & 0x3FFFFu) == 0) Serial.print(".");
    }
    Serial.println();

    if (bad == 0) {
        Serial.printf("    PASS: %u MB verified, %u words'\n", mb, words);
        return true;
    }
    Serial.printf("    FAIL: %u words differ, first at byte %u\n", bad, first_bad);
    Serial.println("    A joint that passes detection and fails here is cold. Reflow it.");
    Serial.println("    If the whole chip fails, check pin 1 orientation before anything else.");
    return false;
}

static double time_scan(const hd_t *base, uint32_t n)
{
    volatile uint32_t sink = 0;
    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t i = 0; i < n; i++) sink += hd_hamming(base[i], query);
    const uint32_t dt = ARM_DWT_CYCCNT - t0;
    (void)sink;
    return (double)dt / (double)n;
}

/* Walk far enough to evict everything, so the next pass over the vectors starts cold. A real scan
 * never revisits a vector, so the cold figure is the one that matters. */
static void flush_cache()
{
    volatile uint32_t sink = 0;
    volatile uint32_t *p = PSRAM_BASE;
    const uint32_t stride = 32 / 4;                     /* one cache line */
    for (uint32_t i = 0; i < (VECTORS * (HD_BYTES / 4)); i += stride) sink += p[i];
    (void)sink;
}

/* ------------------------------------------------------------------------------------------- */

void setup()
{
    Serial.begin(115200);
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) { }
    delay(300);
    cyccnt_begin();

    Serial.println();
    Serial.println("=====================================================");
    Serial.println("Teensy 4.1 PSRAM  --  BENCH ONE");
    Serial.println("=====================================================");
    Serial.println();

    /* ---- 1. is it there ---------------------------------------------------------------- */
    Serial.printf("[1] Teensyduino detected %u MB of PSRAM\n", external_psram_size);
    if (external_psram_size == 0) {
        Serial.println("    Nothing on the QSPI bus. That is wiring, not memory:");
        Serial.println("      - pin 1 orientation. the dot goes to the marked corner");
        Serial.println("      - all eight joints reflowed, none bridged to a neighbour");
        Serial.println("      - the chip must be QSPI PSRAM. ESP-PSRAM64H and APS6404L both work;");
        Serial.println("        a plain SPI flash chip in the same package will not");
        return;
    }
    const uint32_t chips = (external_psram_size + 7u) / 8u;
    Serial.printf("    that is %u chip%s, %u hypervectors of %u bytes\n",
                  chips, chips == 1 ? "" : "s",
                  (external_psram_size * 1024u * 1024u) / HD_BYTES, (unsigned)HD_BYTES);
    if (external_psram_size == 8) {
        Serial.println("    one populated. do NOT add the second until this passes.");
    }
    Serial.println();

    /* ---- 2. is it sound --------------------------------------------------------------- */
    Serial.println("[2] memtest -- the only thing that finds a cold joint");
    if (!memtest(external_psram_size)) return;
    Serial.println();

    /* ---- 3. what does it cost --------------------------------------------------------- */
    Serial.println("[3] the number that decides the architecture");

    uint32_t rng = 0xA5A5F00Du;
    hd_random(query, &rng);
    for (uint32_t i = 0; i < LOCAL_VECTORS; i++) hd_random(local_vec[i], &rng);
    for (uint32_t i = 0; i < VECTORS; i++)       hd_random(psram_vec[i], &rng);

    const double local_cyc = time_scan(local_vec, LOCAL_VECTORS);
    flush_cache();
    const double psram_cold = time_scan(psram_vec, VECTORS);
    /* A genuinely cache-resident scan has to FIT in the 32 KB data cache. The first version of
     * this used 64 vectors, which is 64 KB, so it missed exactly as often as the cold pass and the
     * two figures came back bit-identical -- the tell that the measurement was measuring nothing.
     * 16 vectors is 16 KB and fits with room to spare. */
    for (int w = 0; w < 2; w++) (void)time_scan(psram_vec, 16);   /* pull it in, then measure */
    const double psram_warm = time_scan(psram_vec, 16);

    const double f = (double)F_CPU_ACTUAL;
    Serial.printf("    internal RAM : %7.0f cycles = %6.2f us per compare\n",
                  local_cyc, local_cyc / f * 1e6);
    Serial.printf("    PSRAM  cold  : %7.0f cycles = %6.2f us per compare  <-- use this one\n",
                  psram_cold, psram_cold / f * 1e6);
    Serial.printf("    PSRAM  warm  : %7.0f cycles = %6.2f us per compare  (cache, not memory)\n",
                  psram_warm, psram_warm / f * 1e6);
    Serial.printf("    PSRAM is %.1fx the cost of internal RAM\n", psram_cold / local_cyc);
    Serial.println();

    const double per_us = psram_cold / f * 1e6;
    const uint32_t in_deadline = (uint32_t)(50000.0 / per_us);
    const uint32_t held = (external_psram_size * 1024u * 1024u) / HD_BYTES;
    Serial.printf("    a 50 ms deadline sweeps %u vectors; this board now holds %u\n",
                  in_deadline, held);
    if (in_deadline >= held) {
        Serial.println("    SCAN LINEARLY. The deadline covers the whole chip, so this node needs");
        Serial.println("    no index and answers with full coverage every time.");
    } else {
        Serial.printf("    NEEDS PREFIX SCREENING. Only %u%% of memory fits the deadline, so a\n",
                      (unsigned)(100u * in_deadline / held));
        Serial.println("    linear scan would report partial coverage on every single query.");
        Serial.printf("    An index of %u entries at 20 B (128-bit prefix) is %u KB of internal\n",
                      held, (held * 20u) / 1024u);
        Serial.println("    RAM, which is what has to fit alongside everything else.");
    }

    Serial.println();
    Serial.println("=====================================================");
    Serial.println("done. press reset to run again.");
    Serial.println("=====================================================");
}

void loop()
{
    delay(1000);
}

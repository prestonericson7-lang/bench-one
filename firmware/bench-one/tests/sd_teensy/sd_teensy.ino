/* ===========================================================================================
 *  sd_teensy.ino -- what a microSD card is worth as a body store, measured not guessed
 * ===========================================================================================
 *
 *  WHY A CARD CHANGES THE SHAPE OF A NODE
 *  ---------------------------------------
 *  Measured on this hardware already: a Teensy holds 16,384 hypervectors in 16 MB of PSRAM and
 *  fetches one in 31.34 us. A 4 GB card holds 4,194,304 of them. That is 256 times the capacity
 *  for the price of a card, and it turns a Teensy from a fast shallow node into something with
 *  FPGA-class depth.
 *
 *  It only works if two numbers come out right, and neither can be reasoned about from a
 *  datasheet:
 *
 *      RANDOM 1 KB READ LATENCY.  A body fetch is a random access, not a stream. Cards are built
 *                                 for sequential video writes and their random read latency is
 *                                 nothing like their headline throughput. This is THE number.
 *
 *      PACKED PREFIX SCAN RATE.   The index has to live in PSRAM, because 4 million entries will
 *                                 not fit in 512 KB of internal RAM. Screening walks it linearly,
 *                                 so what matters is streaming 16-byte prefixes out of PSRAM, not
 *                                 the 31.34 us it costs to pull a scattered 1 KB body.
 *
 *  AND THE HEATSINK
 *  -----------------
 *  Every timing figure taken from this board so far assumes 600 MHz. The i.MX RT1062 throttles
 *  when it gets hot, and a node in a finished machine runs flat out indefinitely, not for the
 *  twenty seconds a bench test takes. So this reports die temperature and the ACTUAL clock before
 *  and after a sustained load. If the clock moves, every number measured so far was optimistic and
 *  wants taking again with the heatsink fitted.
 *
 *  BOARD    Teensy 4.1, PSRAM fitted, card in the socket
 *  THEN     Serial Monitor at 115200
 *
 *  The card does not need formatting. A new 4 GB card ships FAT32 and this reads FAT16, FAT32 and
 *  exFAT. Reformat only if mounting fails, and use the SD Association's formatter rather than
 *  Windows, which can misalign the partition and cost real throughput.
 * ===========================================================================================
 */

#include <Arduino.h>
/* SdFat directly, NOT <SD.h>.
 *
 * The Arduino standard SD library is installed globally on this machine and its bundled copy of
 * SdFat shadows the modified one Teensy ships, which stops the build with a deliberate #error.
 * Teensy's SD is only a thin wrapper over its own SdFat anyway, so going straight to SdFat skips
 * the collision without touching a library other sketches may depend on. */
#include <SdFat.h>
#include "bench_hdc.h"

extern "C" uint8_t external_psram_size;
extern "C" float tempmonGetTemp(void);

#define TEST_MB        32           /* the file written and then read back            */
#define RANDOM_READS   400          /* body fetches timed                             */
#define PREFIX_BYTES   16           /* 128 bits, the width that fits in an index      */
#define PREFIX_COUNT   200000       /* prefixes streamed out of PSRAM                 */

static const char *TESTFILE = "benchone.bin";

static SdFs  sd;
static FsFile f;

EXTMEM uint8_t prefixes[PREFIX_COUNT * PREFIX_BYTES];

static void cyccnt_begin()
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

static void thermals(const char *when)
{
    Serial.printf("    %-6s  %.1f C   clock %lu MHz\n",
                  when, tempmonGetTemp(), (unsigned long)(F_CPU_ACTUAL / 1000000u));
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
    Serial.println("Teensy 4.1 microSD as a body store  --  BENCH ONE");
    Serial.println("=====================================================");
    Serial.println();

    Serial.println("[0] thermals before any load");
    thermals("start");
    Serial.printf("    PSRAM fitted: %u MB\n", external_psram_size);
    if (external_psram_size < 16) {
        Serial.println("    WARNING: the index test below needs 16 MB. Results will be short.");
    }
    Serial.println();

    /* ---- 1. mount ---------------------------------------------------------------------- */
    Serial.println("[1] mounting the card");
    if (!sd.begin(SdioConfig(FIFO_SDIO))) {
        Serial.println("    FAILED. In order of likelihood:");
        Serial.println("      - the card is not fully seated. it clicks");
        Serial.println("      - the card is exFAT from a camera and the library was built");
        Serial.println("        without exFAT. reformat FAT32 with the SD Association tool");
        Serial.println("      - a genuinely dead card. try another before blaming the Teensy");
        return;
    }
    const uint64_t total = (uint64_t)sd.card()->sectorCount() * 512ull;
    Serial.printf("    mounted: %.2f GB, FAT type %d\n", total / 1e9, (int)sd.fatType());
    Serial.printf("    that is %llu hypervectors of %u bytes\n",
                  (unsigned long long)(total / HD_BYTES), (unsigned)HD_BYTES);
    Serial.println();

    /* ---- 2. write the working file ----------------------------------------------------- */
    Serial.printf("[2] writing a %d MB file to read back\n", TEST_MB);
    if (sd.exists(TESTFILE)) sd.remove(TESTFILE);
    if (!f.open(TESTFILE, O_RDWR | O_CREAT | O_TRUNC)) {
        Serial.println("    cannot create a file. the card mounted but is not writable.");
        return;
    }
    static uint8_t block[32768];
    for (uint32_t i = 0; i < sizeof(block); i++) block[i] = (uint8_t)(i * 31u + 7u);

    uint32_t t = millis();
    for (int i = 0; i < (TEST_MB * 1024 * 1024) / (int)sizeof(block); i++) {
        if (f.write(block, sizeof(block)) != sizeof(block)) {
            Serial.println("    write failed part way. the card may be full or failing.");
            f.close();
            return;
        }
        if ((i & 63) == 0) Serial.print(".");
    }
    f.flush();
    const uint32_t wms = millis() - t;
    f.close();
    Serial.printf("\n    wrote %d MB in %.2f s = %.1f MB/s\n",
                  TEST_MB, wms / 1000.0, TEST_MB / (wms / 1000.0));
    Serial.println();

    /* ---- 3. sequential read ------------------------------------------------------------ */
    Serial.println("[3] sequential read -- the headline number, and the least useful one");
    if (!f.open(TESTFILE, O_RDONLY)) {
        Serial.println("    cannot reopen the file for reading.");
        return;
    }
    t = millis();
    uint32_t got = 0;
    while (f.available()) {
        const int n = f.read(block, sizeof(block));
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    const uint32_t rms = millis() - t;
    const double seq_mbs = (got / 1048576.0) / (rms / 1000.0);
    Serial.printf("    read %u MB in %.2f s = %.1f MB/s\n",
                  got / 1048576u, rms / 1000.0, seq_mbs);
    Serial.println();

    /* ---- 4. random 1 KB reads -- THE number -------------------------------------------- */
    Serial.println("[4] random 1 KB reads -- a body fetch is a seek, not a stream");
    static uint8_t body[HD_BYTES];
    const uint32_t slots = (TEST_MB * 1024u * 1024u) / HD_BYTES;
    uint32_t rng = 0x2468ACE0u;
    uint32_t worst = 0;

    const uint32_t c0 = ARM_DWT_CYCCNT;
    for (int i = 0; i < RANDOM_READS; i++) {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t slot = rng % slots;
        const uint32_t a0 = ARM_DWT_CYCCNT;
        f.seek((uint64_t)slot * HD_BYTES);
        f.read(body, HD_BYTES);
        const uint32_t dt = ARM_DWT_CYCCNT - a0;
        if (dt > worst) worst = dt;
    }
    const uint32_t c1 = ARM_DWT_CYCCNT;
    f.close();

    const double fcpu = (double)F_CPU_ACTUAL;
    const double body_us = (double)(c1 - c0) / RANDOM_READS / fcpu * 1e6;
    Serial.printf("    mean %.1f us per 1 KB body, worst %.1f us\n",
                  body_us, worst / fcpu * 1e6);
    Serial.printf("    that is %.2f MB/s at random, against %.1f MB/s sequential\n",
                  1024.0 / body_us, seq_mbs);
    Serial.println();
    thermals("after");
    Serial.println();

    /* ---- 5. the index has to live in PSRAM --------------------------------------------- */
    Serial.println("[5] streaming 128-bit prefixes out of PSRAM -- the screening pass");
    for (uint32_t i = 0; i < sizeof(prefixes); i++) prefixes[i] = (uint8_t)(i * 17u + 3u);

    uint32_t qp[4] = { 0x01234567u, 0x89ABCDEFu, 0xFEDCBA98u, 0x76543210u };
    volatile uint32_t sink = 0;
    const uint32_t p0 = ARM_DWT_CYCCNT;
    for (uint32_t i = 0; i < PREFIX_COUNT; i++) {
        const uint32_t *e = (const uint32_t *)(prefixes + (size_t)i * PREFIX_BYTES);
        sink += hd_popcount(e[0] ^ qp[0]) + hd_popcount(e[1] ^ qp[1])
              + hd_popcount(e[2] ^ qp[2]) + hd_popcount(e[3] ^ qp[3]);
    }
    const uint32_t p1 = ARM_DWT_CYCCNT;
    (void)sink;
    const double pre_us = (double)(p1 - p0) / PREFIX_COUNT / fcpu * 1e6;
    Serial.printf("    %.3f us per prefix, %.1f MB/s streamed\n",
                  pre_us, (PREFIX_BYTES / pre_us));
    Serial.println();
    thermals("end");
    Serial.println();

    /* ---- 6. what this makes the node --------------------------------------------------- */
    Serial.println("=====================================================");
    Serial.println("WHAT THIS MAKES A TEENSY NODE");
    Serial.println("=====================================================");

    const uint32_t idx_entries = (external_psram_size * 1024u * 1024u) / (PREFIX_BYTES + 4u);
    const uint64_t card_slots  = total / HD_BYTES;
    const uint32_t addressable = (idx_entries < card_slots) ? idx_entries : (uint32_t)card_slots;

    Serial.printf("  index of %u-byte entries in %u MB PSRAM addresses %u bodies\n",
                  PREFIX_BYTES + 4u, external_psram_size, idx_entries);
    Serial.printf("  the card holds %llu, so this node carries %u vectors\n",
                  (unsigned long long)card_slots, addressable);

    const double screen_ms = addressable * pre_us * 2.0 / 1000.0;
    const double fetch_ms  = 64 * body_us / 1000.0;
    Serial.printf("  a query costs %.1f ms screening + %.1f ms for 64 bodies = %.1f ms\n",
                  screen_ms, fetch_ms, screen_ms + fetch_ms);

    if (screen_ms + fetch_ms <= 50.0) {
        Serial.println("  FAST TIER. It answers inside 50 ms with this much memory.");
    } else if (screen_ms + fetch_ms <= 500.0) {
        Serial.println("  DEEP TIER. Too slow for 50 ms, comfortable inside 500 ms.");
        Serial.printf("  For the fast tier it would have to hold about %u vectors instead.\n",
                      (uint32_t)(addressable * 50.0 / (screen_ms + fetch_ms)));
    } else {
        Serial.println("  TOO SLOW even for the deep tier at this capacity. Shrink the index");
        Serial.printf("  to about %u entries to answer inside 500 ms.\n",
                      (uint32_t)(addressable * 500.0 / (screen_ms + fetch_ms)));
    }
    Serial.println("=====================================================");
}

void loop()
{
    delay(1000);
}

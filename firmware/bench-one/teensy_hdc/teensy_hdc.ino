/* ===========================================================================================
 *  teensy_hdc.ino -- BENCH ONE: the Teensy as the machine's senses
 * ===========================================================================================
 *
 *  THE ROLE THIS NODE CLASS ACTUALLY HAS
 *  --------------------------------------
 *  A Teensy holds 256 concepts. A Luckfox holds twenty thousand. So the Teensy is not where
 *  the memory lives, and pretending otherwise wastes the one thing it is genuinely unmatched
 *  at: 512 KB of SINGLE-CYCLE memory and hard real-time determinism.
 *
 *  So it does two jobs nothing else in the machine can do:
 *
 *    1. IT IS THE SENSE ORGAN. The 74HC fabric captures 152 channels on one clock edge --
 *       simultaneously, with no scan skew. This sketch turns that instant into a hypervector.
 *       That is the bridge from the physical world into the being, and it is the first job in
 *       this whole project that the discrete logic is genuinely the right tool for.
 *
 *    2. IT IS THE HOT CACHE. 256 concepts in DTCM, searched with zero cache misses and zero
 *       scheduler, so the most recently seen things are recalled in microseconds without a
 *       single packet leaving the board.
 *
 *  Anything it does not recognise goes out over the UART to its Luckfox, which is its network
 *  stack (docs/11: Teensys have no IP on purpose -- a Teensy should never spend a cycle on
 *  TCP). Miss locally, ask the cluster. That is a cache hierarchy, and it is the same shape as
 *  L1 -> RAM, for the same reason.
 *
 *
 *  WHY THE ENCODING MATTERS MORE THAN THE SEARCH
 *  ----------------------------------------------
 *  Everything downstream is only as good as this vector. Two rules, both load-bearing:
 *
 *    * A CHANNEL'S IDENTITY IS A RANDOM VECTOR, its VALUE is a level vector. Bind them, bundle
 *      the results. Random identities cannot collide; level values make 61 and 62 similar so
 *      the machine generalises across a measurement instead of memorising each reading.
 *
 *    * AN UNREAD CHANNEL IS OMITTED, never encoded as zero. A disconnected sensor reading as a
 *      hard zero is a lie the whole system would then reason from.
 * ===========================================================================================
 */

#include <Arduino.h>
#include "bench_hdc.h"
#include "bench_hdc_shard.h"

extern "C" uint8_t external_psram_size;

/* --- identity: Teensys are nodes 31..39 (docs/11 gives 20..30 to the Luckfoxes) ----------- */
#ifndef BENCH_NODE_ID
#define BENCH_NODE_ID 31
#endif

/* --- the local hot memory ----------------------------------------------------------------
 * DMAMEM puts these in OCRAM (RAM2, 512 KB) instead of DTCM. Deliberate: the vectors are
 * streamed once per search and are perfectly sequential, so the cache handles them well, and
 * it leaves DTCM free for the query, the accumulator and the stack -- the things that are
 * touched randomly and repeatedly. Putting a 256 KB array in DTCM would evict exactly the
 * data that benefits from being there. */
#define LOCAL_N 256
DMAMEM static hd_t     g_store[LOCAL_N];
DMAMEM static uint16_t g_label[LOCAL_N];
static hd_mem_t g_mem;

/* Hot, random-access, small -> DTCM. hd_acc_t is 16 KB and must never be a local: as a
 * stack frame it is an instant overflow, which is how it was found on the ESP32 side. */
static hd_acc_t g_acc;

/* --- the fabric's 152 channels ----------------------------------------------------------- */
#define FABRIC_BITS 152
DMAMEM static hd_t g_chan[FABRIC_BITS];       /* identity vector per channel, 152 KB          */
static hd_t g_analog_base[8];                 /* level base per analog group                  */

static uint32_t g_rng = 0x7E5E0000u ^ (BENCH_NODE_ID * 2654435761u);

static inline void cyc_begin(void)
{
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    ARM_DWT_CYCCNT = 0;
}
static inline uint32_t cyc(void) { return ARM_DWT_CYCCNT; }

/* ===========================================================================================
 * ENCODE ONE INSTANT OF THE WORLD
 * -------------------------------------------------------------------------------------------
 * `bits`   152 digital channels, one bit each, captured on a single /PL edge
 * `adc`    8 analog channels, 0..255, or 0xFFFF for "not read"
 * =========================================================================================*/
static void encode_instant(const uint8_t *bits, const uint16_t *adc, hd_t out)
{
    hd_t lvl, bound;
    hd_acc_clear(&g_acc);

    /* Digital: a channel that is HIGH contributes its identity. A channel that is LOW
     * contributes nothing -- it is the ABSENCE of evidence, and bundling a "low" vector would
     * make two unrelated quiet channels look like a shared feature. */
    for (uint16_t i = 0; i < FABRIC_BITS; i++)
        if ((bits[i >> 3] >> (i & 7u)) & 1u)
            hd_acc_add(&g_acc, g_chan[i]);

    /* Analog: bind the channel's identity to a LEVEL vector for its value, so 61 and 62 are
     * similar and 61 and 200 are not. */
    for (uint8_t a = 0; a < 8; a++) {
        if (adc[a] == 0xFFFFu) continue;                 /* not read: omit, never zero */
        hd_encode_level(lvl, g_analog_base[a], adc[a] > 255u ? 255u : adc[a], 255u);
        hd_bind(bound, g_chan[(FABRIC_BITS - 8) + a], lvl);
        hd_acc_add(&g_acc, bound);
    }

    if (g_acc.n == 0u) { hd_zero(out); return; }
    hd_acc_result(&g_acc, out);
}

/* ===========================================================================================*/
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }
    cyc_begin();

    Serial.println(F("\n=========================================================="));
    Serial.printf ("BENCH ONE -- Teensy sense node %d\n", BENCH_NODE_ID);
    Serial.println(F("=========================================================="));
    Serial.printf("F_CPU        : %lu Hz\n", (unsigned long)F_CPU_ACTUAL);
    Serial.printf("PSRAM        : %u MB\n", (unsigned)external_psram_size);
    Serial.printf("die temp     : %.1f C\n", tempmonGetTemp());
    Serial.printf("hypervector  : %u bits = %u bytes\n", (unsigned)HD_BITS, (unsigned)HD_BYTES);
    Serial.printf("local memory : %d concepts (%d KB in OCRAM)\n",
                  LOCAL_N, (LOCAL_N * HD_BYTES) / 1024);
    Serial.printf("channel ids  : %d (%d KB)\n", FABRIC_BITS, (FABRIC_BITS * HD_BYTES) / 1024);

    /* Vocabulary is DERIVED from a constant seed, so every node in the machine builds the
     * identical channel identities without one hypervector ever crossing a wire. */
    uint32_t s = 0xFAB21C00u;
    for (int i = 0; i < FABRIC_BITS; i++) hd_random(g_chan[i], &s);
    for (int i = 0; i < 8; i++)           hd_random(g_analog_base[i], &s);
    hd_mem_init(&g_mem, g_store, g_label, LOCAL_N);

    /* --- substrate check: never trust an unrun kernel ------------------------------------ */
    {
        hd_t a, b, c, back;
        uint32_t t = 11;
        hd_random(a, &t); hd_random(b, &t);
        hd_bind(c, a, b); hd_bind(back, c, b);
        bool inv = true;
        for (unsigned i = 0; i < HD_WORDS; i++) if (back[i] != a[i]) inv = false;
        Serial.printf("\nbind inverse : %s\n", inv ? "OK" : "FAIL");
        Serial.printf("unrelated    : %lu bits apart (orthogonal = %u)\n",
                      (unsigned long)hd_hamming(a, b), (unsigned)(HD_BITS / 2));
        hd_t p1, p2;
        hd_permute(p1, a, 1); hd_permute(p2, p1, -1);
        bool pinv = true;
        for (unsigned i = 0; i < HD_WORDS; i++) if (p2[i] != a[i]) pinv = false;
        Serial.printf("permute inv  : %s\n", pinv ? "OK" : "FAIL");
    }

    /* --- teach it some instants ---------------------------------------------------------- */
    Serial.println(F("\n--- learning 64 fabric patterns, one shot each -----------"));
    uint8_t  bits[(FABRIC_BITS + 7) / 8];
    uint16_t adc[8];
    for (int pat = 0; pat < 64; pat++) {
        uint32_t r = 0x1000u + pat;
        for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = (uint8_t)hd_rand(&r);
        for (int a = 0; a < 8; a++) adc[a] = (uint16_t)(hd_rand(&r) & 0xFFu);
        hd_t v;
        encode_instant(bits, adc, v);
        hd_mem_add(&g_mem, v, (uint16_t)pat);
    }
    Serial.printf("stored %u instants\n", g_mem.n);

    /* --- recall a noisy repeat of one --------------------------------------------------- */
    Serial.println(F("\n--- recall with sensor noise ------------------------------"));
    for (int pat = 0; pat < 3; pat++) {
        uint32_t r = 0x1000u + pat;
        for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = (uint8_t)hd_rand(&r);
        for (int a = 0; a < 8; a++) adc[a] = (uint16_t)(hd_rand(&r) & 0xFFu);

        /* Corrupt it the way real hardware does: flip 8 digital channels and drift the
         * analog readings by a few counts. */
        uint32_t n = 0xC0FFEEu + pat;
        for (int k = 0; k < 8; k++) {
            const uint16_t ch = (uint16_t)(hd_rand(&n) % FABRIC_BITS);
            bits[ch >> 3] ^= (uint8_t)(1u << (ch & 7u));
        }
        for (int a = 0; a < 8; a++) {
            const int drift = (int)(hd_rand(&n) % 9u) - 4;
            int v = (int)adc[a] + drift;
            adc[a] = (uint16_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }

        hd_t q;
        encode_instant(bits, adc, q);
        uint32_t d = 0;
        const uint32_t t0 = cyc();
        const int32_t slot = hd_mem_best(&g_mem, q, &d);
        const uint32_t dt = cyc() - t0;
        Serial.printf("  pattern %d + 8 flipped channels + analog drift -> slot %ld %s"
                      "  dist %lu  %lu cycles (%.1f us)\n",
                      pat, (long)slot, (slot == pat ? "CORRECT" : "wrong"),
                      (unsigned long)d, (unsigned long)dt,
                      (double)dt / (F_CPU_ACTUAL / 1000000.0));
    }

    /* --- an instant it has never seen ---------------------------------------------------- */
    {
        uint32_t r = 0xDEAD00u;
        for (unsigned i = 0; i < sizeof(bits); i++) bits[i] = (uint8_t)hd_rand(&r);
        for (int a = 0; a < 8; a++) adc[a] = (uint16_t)(hd_rand(&r) & 0xFFu);
        hd_t q;
        encode_instant(bits, adc, q);
        uint32_t d = 0;
        hd_mem_best(&g_mem, q, &d);
        Serial.printf("  never-seen instant -> nearest %lu (orthogonal %u) : %s\n",
                      (unsigned long)d, (unsigned)(HD_BITS / 2),
                      d > (HD_BITS * 2u) / 5u ? "correctly unrecognised"
                                              : "WARNING false match");
    }

    /* --- MEASURED throughput. Every rate in the docs is derived; this one is not. -------- */
    Serial.println(F("\n--- measured search rate ----------------------------------"));
    {
        hd_t q;
        hd_random(q, &g_rng);
        uint32_t d;
        const int reps = 50;
        const uint32_t t0 = cyc();
        for (int i = 0; i < reps; i++) hd_mem_best(&g_mem, q, &d);
        const uint32_t dt = cyc() - t0;

        const double per_cmp = (double)dt / ((double)reps * g_mem.n);
        const double cmps    = (double)F_CPU_ACTUAL / per_cmp;
        Serial.printf("  %.1f cycles per 8192-bit comparison\n", per_cmp);
        Serial.printf("  %.2f bits/cycle\n", HD_BITS / per_cmp);
        Serial.printf("  %.0f comparisons/second on ONE Teensy\n", cmps);
        Serial.printf("  9 Teensys  -> %.1f million comparisons/second\n", 9.0 * cmps / 1e6);
    }
    Serial.printf("\ndie temp after: %.1f C (panic at 90)\n", tempmonGetTemp());
    Serial.println(F("\nlocal miss -> ask the Luckfox. That link is docs/11's job.\n"));
}

void loop()
{
    digitalWriteFast(LED_BUILTIN, (millis() / 500) & 1);
    delay(10);
}

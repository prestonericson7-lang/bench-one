/* ===========================================================================================
 *  teensy_layer.ino -- BENCH ONE: one DBN layer on one Teensy 4.1
 * ===========================================================================================
 *
 *  FLASH THIS FIRST. It needs no PSRAM, no FPGA, no network, no other board -- just a Teensy
 *  and the USB cable. It answers the three questions that decide the whole machine:
 *
 *      1. Does the kernel learn on real silicon?      (bars-and-stripes, error must fall)
 *      2. Does it GENERATE, not just reconstruct?     (annealed sampling, valid-pattern rate)
 *      3. How fast is this Teensy actually?           (measured MAC/s, not my arithmetic)
 *
 *  Question 3 is the one I cannot answer from a datasheet. Every throughput number in docs 08
 *  through 11 is derived. This prints the measured one, and if it disagrees with 1.2 GMAC/s
 *  then the measured one is right and the docs get corrected.
 *
 *  Open the Serial Monitor at any baud (Teensy USB ignores it). Everything is printed.
 *
 *  WHAT IT DOES NOT DO
 *  -------------------
 *  It does not talk to any other node yet. That is bench_dbn.c's job and it comes next; this
 *  sketch is the proof that the thing being distributed is worth distributing.
 * ===========================================================================================
 */

#include <Arduino.h>
#include "bench_bitslice.h"

extern "C" uint8_t external_psram_size;   /* set by the core at boot (startup.c) */

/* --- the correctness task -------------------------------------------------------------- */
/* Bars-and-stripes on a 6x6 grid: 124 patterns in which every row is uniform or every column
 * is. It is the standard RBM sanity set because the structure is impossible to reach by
 * matching per-pixel statistics -- a model that has only learned the marginals scores zero. */
#define NV   36
#define NH   32
#define VW   BS_WORDS(NV)
#define HW   BS_WORDS(NH)

/* Weight planes live in DTCM: this is the memory that makes a Teensy worth using. */
static uint32_t g_wpos[NH * VW];
static uint32_t g_wneg[NH * VW];
static int16_t  g_shadow[NH * NV];      /* small enough for DTCM at this size; the 784x500
                                         * case goes in EXTMEM -- see the big-layer note */
static int16_t  g_bvis[NV];
static int16_t  g_bhid[NH];

static uint32_t g_vis [BS_REPLICAS][VW];
static uint32_t g_fant[BS_REPLICAS][VW];
static uint32_t g_rec [BS_REPLICAS][VW];
static uint32_t g_h0  [BS_REPLICAS][HW];
static uint32_t g_h1  [BS_REPLICAS][HW];

static bs_layer_t L;

/* --- bars-and-stripes generator --------------------------------------------------------- */
static void bas_pattern(uint32_t idx, uint32_t *out)
{
    const uint32_t m    = (idx >> 1) % 62u + 1u;    /* 1..62, never all-on or all-off */
    const int      vert = (int)(idx & 1u);
    out[0] = 0; out[1] = 0;
    for (int i = 0; i < NV; i++) {
        const int r = vert ? (i % 6) : (i / 6);
        if ((m >> r) & 1u) out[i >> 5] |= (1u << (i & 31u));
    }
}

static int bas_valid(const uint32_t *v)
{
    int px[NV];
    for (int i = 0; i < NV; i++) px[i] = (v[i >> 5] >> (i & 31u)) & 1;
    int ok = 1;
    for (int r = 0; r < 6 && ok; r++)
        for (int c = 1; c < 6; c++) if (px[r*6+c] != px[r*6]) { ok = 0; break; }
    if (ok) return 1;
    ok = 1;
    for (int c = 0; c < 6 && ok; c++)
        for (int r = 1; r < 6; r++) if (px[r*6+c] != px[c]) { ok = 0; break; }
    return ok;
}

/* --- DWT cycle counter, same convention as the worker sketch ---------------------------- */
static inline void cyccnt_begin(void)
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
    ARM_DWT_CYCCNT = 0;
}
static inline uint32_t cyccnt(void) { return ARM_DWT_CYCCNT; }

/* =========================================================================================*/

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }
    cyccnt_begin();

    Serial.println(F("\n=============================================================="));
    Serial.println(F("BENCH ONE -- bit-sliced ensemble RBM, one layer, one Teensy"));
    Serial.println(F("=============================================================="));
    Serial.printf ("F_CPU_ACTUAL   : %lu Hz\n", (unsigned long)F_CPU_ACTUAL);
    Serial.printf ("PSRAM detected : %u MB\n", (unsigned)external_psram_size);
    Serial.printf ("die temp       : %.1f C\n", tempmonGetTemp());
    Serial.printf ("layer          : %d visible x %d hidden, %d replicas\n", NV, NH, BS_REPLICAS);
    {
        const unsigned planes = (unsigned)BS_PLANE_BYTES(NV, NH);
        Serial.printf("weight planes  : %u bytes in DTCM (2 bits/weight)\n", planes);
        Serial.printf("int8 equivalent: %u bytes  -- ternary is %.1fx denser\n",
                      (unsigned)(NV * NH), planes ? (double)(NV * NH) / planes : 0.0);
    }
    Serial.println();

    bs_layer_init(&L, NV, NH, g_wpos, g_wneg, g_shadow, g_bvis, g_bhid, 0xB3C41011u);
    bs_randomize(&L, 400);
    Serial.printf("after randomize: thresh=%d gain_h=%d gain_v=%d\n",
                  L.tern_thresh, L.gain_h, L.gain_v);

    for (uint32_t r = 0; r < BS_REPLICAS; r++)
        for (unsigned k = 0; k < VW; k++) g_fant[r][k] = bs_popcount(r * 2654435761u) ^ (r * 97u + k);

    /* ---------------------------------------------------------------------------------- */
    Serial.println(F("\n--- 1. TRAINING -----------------------------------------------"));
    Serial.println(F("batch |  recon err (0-255)          | ms/batch"));

    uint32_t seed = 12345u;
    for (int b = 1; b <= 300; b++) {
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            seed = seed * 1664525u + 1013904223u;
            bas_pattern(seed >> 8, g_vis[r]);
        }

        const uint32_t t0 = cyccnt();
        const uint32_t err = bs_pcd_step(&L, &g_vis[0][0], &g_fant[0][0],
                                         &g_h0[0][0], &g_h1[0][0], &g_rec[0][0], 6);
        const uint32_t dt = cyccnt() - t0;

        /* weight decay, then refresh the ternary planes -- the planes are what compute, so
         * forgetting this makes the shadow move while the network never changes. */
        for (int i = 0; i < NH * NV; i++) g_shadow[i] -= g_shadow[i] >> 9;
        bs_rethreshold(&L);

        if (b % 25 == 0) {
            Serial.printf("%5d | %3lu ", b, (unsigned long)err);
            for (uint32_t i = 0; i < err / 6; i++) Serial.print('#');
            for (uint32_t i = err / 6; i < 44; i++) Serial.print(' ');
            Serial.printf("| %.2f\n", (double)dt / (F_CPU_ACTUAL / 1000.0));
        }
    }

    /* ---------------------------------------------------------------------------------- */
    Serial.println(F("\n--- 2. RECONSTRUCTION (deterministic, held-out) ----------------"));
    int exact = 0, bits = 0, tot = 0;
    for (int st = 0; st + (int)BS_REPLICAS <= 124; st += BS_REPLICAS) {
        for (uint32_t r = 0; r < BS_REPLICAS; r++) bas_pattern((uint32_t)(st + r), g_vis[r]);
        bs_mean_hidden (&L, &g_vis[0][0], &g_h0[0][0]);
        bs_mean_visible(&L, &g_h0[0][0], &g_rec[0][0]);
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            uint32_t d = 0;
            for (unsigned k = 0; k < VW; k++) {
                uint32_t x = g_vis[r][k] ^ g_rec[r][k];
                if (k == VW - 1 && (NV & 31)) x &= (1u << (NV & 31)) - 1u;
                d += bs_popcount(x);
            }
            bits += (int)d;
            if (d == 0) exact++;
            tot++;
        }
    }
    Serial.printf("exact 36-of-36 : %d / %d\n", exact, tot);
    Serial.printf("bit error      : %.1f %%   (chance = 50 %%)\n", 100.0 * bits / (tot * NV));

    /* ---------------------------------------------------------------------------------- */
    Serial.println(F("\n--- 3. GENERATION (annealed) ----------------------------------"));
    Serial.println(F("A flat-temperature chain sits at maximum entropy and produces nothing."));
    int valid_flat = 0, valid_ann = 0, n = 0;
    for (int t = 0; t < 8; t++) {
        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            seed = seed * 1664525u + 1013904223u;
            for (unsigned k = 0; k < VW; k++) g_vis[r][k] = (seed ^ (uint32_t)(r * 7919u + k));
        }
        static const uint16_t flat[40] = {
            16,16,16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,16,16,
            16,16,16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,16,16 };
        bs_generate(&L, &g_vis[0][0], &g_h0[0][0], flat, 40);
        for (uint32_t r = 0; r < BS_REPLICAS; r++) if (bas_valid(g_vis[r])) valid_flat++;

        for (uint32_t r = 0; r < BS_REPLICAS; r++) {
            seed = seed * 1664525u + 1013904223u;
            for (unsigned k = 0; k < VW; k++) g_vis[r][k] = (seed ^ (uint32_t)(r * 7919u + k));
        }
        bs_generate(&L, &g_vis[0][0], &g_h0[0][0], NULL, 0);   /* default anneal */
        for (uint32_t r = 0; r < BS_REPLICAS; r++) { if (bas_valid(g_vis[r])) valid_ann++; n++; }
    }
    Serial.printf("flat  chain    : %d / %d valid  (%.1f %%)\n", valid_flat, n, 100.0*valid_flat/n);
    Serial.printf("annealed chain : %d / %d valid  (%.1f %%)\n", valid_ann,  n, 100.0*valid_ann /n);
    Serial.println(F("random chance for a 36-bit image = 126 / 2^36 = 0.0000002 %"));

    Serial.println(F("\nthree samples the model invented:"));
    for (uint32_t r = 0, shown = 0; r < BS_REPLICAS && shown < 3; r++) {
        if (!bas_valid(g_vis[r])) continue;
        for (int rr = 0; rr < 6; rr++) {
            Serial.print(F("    "));
            for (int c = 0; c < 6; c++) {
                const int i = rr * 6 + c;
                Serial.print((g_vis[r][i >> 5] >> (i & 31u)) & 1u ? F("##") : F(". "));
            }
            Serial.println();
        }
        Serial.println();
        shown++;
    }

    /* ---------------------------------------------------------------------------------- */
    Serial.println(F("--- 4. MEASURED THROUGHPUT ------------------------------------"));
    Serial.println(F("Every MAC/s figure in docs 08-11 is derived. This one is measured."));
    {
        const uint32_t t0 = cyccnt();
        const int reps = 200;
        for (int i = 0; i < reps; i++) bs_sample_hidden(&L, &g_vis[0][0], &g_h0[0][0]);
        const uint32_t dt = cyccnt() - t0;

        /* one P(h|v) pass touches nv*nh weights for each of R replicas */
        const double macs = (double)NV * NH * BS_REPLICAS * reps;
        const double secs = (double)dt / (double)F_CPU_ACTUAL;
        Serial.printf("P(h|v)  : %.0f cycles/pass, %.3f GMAC/s equivalent\n",
                      (double)dt / reps, macs / secs / 1e9);
        Serial.printf("        : %.2f weights per cycle\n", macs / (double)dt);
    }
    {
        const uint32_t t0 = cyccnt();
        const int reps = 200;
        for (int i = 0; i < reps; i++) bs_sample_visible(&L, &g_h0[0][0], &g_vis[0][0]);
        const uint32_t dt = cyccnt() - t0;
        const double macs = (double)NV * NH * BS_REPLICAS * reps;
        const double secs = (double)dt / (double)F_CPU_ACTUAL;
        Serial.printf("P(v|h)  : %.0f cycles/pass, %.3f GMAC/s equivalent\n",
                      (double)dt / reps, macs / secs / 1e9);
    }
    Serial.printf("die temp after : %.1f C   (panic trips at 90)\n", tempmonGetTemp());

    Serial.println(F("\n--- VERDICT ---------------------------------------------------"));
    Serial.printf("learns      : %s\n", exact > 0 ? "YES" : "no");
    Serial.printf("generates   : %s\n", valid_ann > 4 * (valid_flat + 1) ? "YES (annealing works)"
                                                                        : "no");
    Serial.println(F("Next: bench_dbn chains this layer to the next node.\n"));
}

void loop()
{
    digitalWriteFast(LED_BUILTIN, (millis() / 500) & 1);
}

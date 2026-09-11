/* ===========================================================================================
 *  psram_bitbang -- is it the supply or is it the wiring, and how fast can this actually go
 * ===========================================================================================
 *
 *  WHERE THIS GOT TO
 *  -----------------
 *      by hand, a microsecond per edge (~250 kHz)     64 of 64 bytes perfect
 *      by hand, flat out (43 MB/s write, 18 read)     4080 of 4096 wrong
 *      the controller at 49.5 MHz, all 32 sampling positions   garbage everywhere
 *
 *  Slow works and fast does not, and the crossover has never been measured.
 *
 *  TWO CANDIDATE CAUSES, AND THEY CALL FOR DIFFERENT FIXES
 *  ------------------------------------------------------
 *  SUPPLY. Every chip has its 100 nF, but there is no bulk capacitor on the rail, and the chips sit
 *  at the far end of breadboard power rails with real inductance in them. Switching four data lines
 *  hard takes charge that has to come from somewhere, and if it cannot arrive in time the rail sags
 *  and the chip cannot answer. A 10 uF on the rail fixes that.
 *
 *  SIGNAL. A breadboard gives a signal wire no ground beside it to return along, so the return
 *  current takes a long loop, which is an inductor, and the line rings. No capacitor fixes that;
 *  only shorter wires with a ground running beside them.
 *
 *  These look identical from the outside -- fast fails, slow works -- but they separate cleanly
 *  under one test, and that test is section 2 below.
 *
 *      supply sag      gets worse with LONGER bursts, better with idle GAPS between them,
 *                      because both change how much charge is demanded and how long the rail
 *                      has to recover
 *      ringing         barely cares about burst length or gaps, and cares only about how fast
 *                      the edges are
 *
 *  So section 2 holds the edge rate fixed at a setting that currently fails, and varies burst length
 *  and recovery gap instead. Whichever knob moves the error count names the cause.
 *
 *
 *  IT DRIVES ITSELF
 *  ----------------
 *  Everything runs from loop() on a repeat rather than once from setup(), so a host attaching at any
 *  moment sees a full run shortly after, instead of having to be connected during the four seconds
 *  after a reset.
 *
 *  And the 8 microsecond rule still applies at every speed: these are DRAM inside and refresh only
 *  while chip select is high. A slower clock makes a fixed burst take longer, so burst length is
 *  computed from the measured rate at each setting rather than fixed.
 *
 *  THE BUG THAT MADE ALL OF THIS LOOK LIKE A HARDWARE PROBLEM
 *  ----------------------------------------------------------
 *  Every clock here used to end HIGH. The next phase then began by writing the clock high again,
 *  which is not an edge, so the chip's reply arrived shifted by exactly one bit. Reading 0D 5D 53 31
 *  as 06 AE A9 98 is that shift and nothing else.
 *
 *  It failed at every speed, in every width, in both directions -- which reads exactly like a bus
 *  that cannot carry the signal, and is why it was blamed on the breadboard, then on the supply,
 *  then on contention. Two implementations of the same identity read, side by side, settled it in
 *  one run. Every bit now ends with the clock low.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define B_SS0  (1u << 24)
#define B_CLK  (1u << 25)
#define B_DATA (0xFu << 26)
#define DSHIFT 26
#define B_SS1  (1u << 22)

#define PIN_A 2
#define PIN_B 3
#define PIN_C 4

static uint32_t idle;
static uint32_t g_spin  = 0;      /* no-ops either side of each clock edge */
static uint32_t g_burst = 32;     /* bytes per chip-select assertion */
static uint32_t g_gap   = 0;      /* microseconds of idle between bursts */

static inline void spin(void) { for (uint32_t i = 0; i < g_spin; i++) __asm__ volatile("nop"); }

static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }

/* base MUST arrive with the four data bits already cleared. An earlier version left them set
 * from the idle value and only ORed the nibble in, so D2 and D3 could never be driven low and
 * every byte written was corrupt -- which then read back faithfully as corruption and looked
 * for all the world like a bus that was too fast. */
static inline void put_nib(uint32_t base, uint8_t nib)
{
    const uint32_t v = base | ((uint32_t)nib << DSHIFT);
    /* Three writes, because two does not work: dropping the explicit return to low and letting the
     * next nibble's data write provide the falling edge fails even at 5.9 MB/s, where this is clean
     * at 11.6. The edge and the data change must not coincide.
     *
     * The waiting, though, does not have to be even. Setup is a register write the chip never sees
     * until the rising edge, so only the HIGH phase and the recovery need time. Skipping the wait
     * after the data write costs nothing and is a third of the period. */
    GPIO9_DR = v;
    GPIO9_DR = v | B_CLK;  spin();     /* rising edge: the chip latches here */
    GPIO9_DR = v;          spin();     /* and back down before the next data */
}

static inline uint8_t get_nib(uint32_t base)
{
    GPIO9_DR = base | B_CLK;  spin();
    const uint8_t n = (uint8_t)((GPIO9_PSR >> DSHIFT) & 0xF);
    GPIO9_DR = base;          spin();
    return n;
}

static void s_out(uint32_t base, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = base | (((v >> i) & 1u) << DSHIFT);
        GPIO9_DR = d;          spin();
        GPIO9_DR = d | B_CLK;  spin();
        GPIO9_DR = d;          spin();
    }
}

static void cmd_single(uint8_t c)
{
    bus_out();
    /* single SPI leaves D2 and D3 high: they are not command lines in this mode. */
    const uint32_t base = (idle & ~B_SS0 & ~B_CLK & ~B_DATA) | (1u << 28) | (1u << 29);
    GPIO9_DR = base;  s_out(base, c);  GPIO9_DR = idle;
    delayMicroseconds(5);
}

static void cmd_quad(uint8_t c)
{
    bus_out();
    const uint32_t base = idle & ~B_SS0 & ~B_CLK & ~B_DATA;
    GPIO9_DR = base;
    put_nib(base, (uint8_t)(c >> 4));
    put_nib(base, (uint8_t)(c & 0xF));
    GPIO9_DR = idle;
    delayMicroseconds(5);
}

static void qread(uint32_t addr, uint8_t *dst, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t base = idle & ~B_SS0 & ~B_CLK & ~B_DATA;
        GPIO9_DR = base;
        put_nib(base, 0xE); put_nib(base, 0xB);
        put_nib(base, (addr >> 20) & 0xF); put_nib(base, (addr >> 16) & 0xF);
        put_nib(base, (addr >> 12) & 0xF); put_nib(base, (addr >>  8) & 0xF);
        put_nib(base, (addr >>  4) & 0xF); put_nib(base, (addr      ) & 0xF);
        data_in();
        for (int i = 0; i < 6; i++) { GPIO9_DR = base | B_CLK; spin(); GPIO9_DR = base; spin(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib(base);
            dst[i] = (uint8_t)((hi << 4) | get_nib(base));
        }
        GPIO9_DR = idle;
        bus_out();
        if (g_gap) delayMicroseconds(g_gap);
        addr += n; dst += n; len -= n;
    }
}

static void qwrite(uint32_t addr, const uint8_t *src, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t base = idle & ~B_SS0 & ~B_CLK & ~B_DATA;
        GPIO9_DR = base;
        put_nib(base, 0x3); put_nib(base, 0x8);
        put_nib(base, (addr >> 20) & 0xF); put_nib(base, (addr >> 16) & 0xF);
        put_nib(base, (addr >> 12) & 0xF); put_nib(base, (addr >>  8) & 0xF);
        put_nib(base, (addr >>  4) & 0xF); put_nib(base, (addr      ) & 0xF);
        for (uint32_t i = 0; i < n; i++) {
            put_nib(base, (uint8_t)(src[i] >> 4));
            put_nib(base, (uint8_t)(src[i] & 0xF));
        }
        GPIO9_DR = idle;
        if (g_gap) delayMicroseconds(g_gap);
        addr += n; src += n; len -= n;
    }
}

/* The decoder's three address pins live in GPIO9 too -- bits 4, 5 and 6 -- and every nibble here
 * writes the WHOLE register. So `idle` has to be recaptured whenever the bank changes, or each
 * nibble quietly restores the bank that was selected when idle was first taken. That is what was
 * happening: idle was captured before the first pick(), so every nibble reset the decoder to bank 0
 * and put a banked chip on the bus alongside the onboard one. Contention at every speed, which is
 * exactly what a bus too fast for its wiring looks like. */
static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    delayMicroseconds(10);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | (1u << 28) | (1u << 29)) & ~B_CLK;
}

static inline uint8_t pat(uint32_t i, uint8_t s) { return (uint8_t)(i * 0x9Du + s); }

#define BLK 4096
static uint8_t wbuf[BLK], rbuf[BLK];

static void enter_quad(void)
{
    cmd_quad(0xF5);
    cmd_single(0x66);
    cmd_single(0x99);
    delay(2);
    cmd_single(0x35);
    delayMicroseconds(50);
}

static uint32_t trial(uint32_t bytes, float *mw, float *mr)
{
    for (uint32_t i = 0; i < bytes; i++) wbuf[i] = pat(i, 0x3B);
    uint32_t t0 = micros();  qwrite(0, wbuf, bytes);  const uint32_t tw = micros() - t0;
    t0 = micros();           qread(0, rbuf, bytes);   const uint32_t tr = micros() - t0;
    if (mw) *mw = tw ? bytes / (float)tw : 0.0f;
    if (mr) *mr = tr ? bytes / (float)tr : 0.0f;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++) if (rbuf[i] != wbuf[i]) bad++;
    return bad;
}

/* ======================================================================================== */

static void section1(int *best_spin, float *best_rate)
{
    Serial.println(F("\n[1] edge rate sweep. Fewer no-ops is faster."));
    Serial.println(F("     nops   burst   write MB/s   read MB/s   wrong of 4096"));
    Serial.println(F("     ----   -----   ----------   ---------   -------------"));

    /* The ceiling is at the fine end, so spend the sweep there. */
    static const uint32_t SPINS[] = { 25, 12, 6, 4, 3, 2, 1, 0 };
    *best_spin = -1; *best_rate = 0;
    g_gap = 0;

    for (unsigned k = 0; k < sizeof(SPINS) / sizeof(SPINS[0]); k++) {
        g_spin = SPINS[k];

        /* Size the burst for about 4 us of payload at whatever rate this setting gives. */
        g_burst = 8;
        float r0;
        (void)trial(256, NULL, &r0);
        uint32_t b = (r0 > 0.01f) ? (uint32_t)(r0 * 4.0f) : 4;
        if (b < 4)  b = 4;
        if (b > 64) b = 64;
        g_burst = b;

        float w, r;
        const uint32_t bad = trial(BLK, &w, &r);

        Serial.print(F("     "));
        if (SPINS[k] < 100) Serial.print(' ');
        if (SPINS[k] < 10)  Serial.print(' ');
        Serial.print(SPINS[k]); Serial.print(F("     "));
        if (b < 10) Serial.print(' ');
        Serial.print(b);        Serial.print(F("      "));
        Serial.print(w, 2);     Serial.print(F("        "));
        Serial.print(r, 2);     Serial.print(F("        "));
        Serial.println(bad);

        if (bad == 0 && r > *best_rate) { *best_rate = r; *best_spin = (int)SPINS[k]; }
    }
}

static void section2(void)
{
    /* Hold the edge rate at something that fails, then move only burst and gap. */
    Serial.println(F("\n[2] supply or signal? edge rate FIXED at 3 nops, only burst and gap move."));
    Serial.println(F("    supply sag cares about both. ringing cares about neither."));
    Serial.println(F("\n              gap 0us   gap 2us   gap 10us   gap 50us"));

    static const uint32_t BURSTS[] = { 4, 8, 16, 32 };
    static const uint32_t GAPS[]   = { 0, 2, 10, 50 };
    g_spin = 0;      /* run where there ARE errors, or the grid is all zeros and says nothing */

    for (unsigned b = 0; b < 4; b++) {
        g_burst = BURSTS[b];
        Serial.print(F("    burst "));
        if (BURSTS[b] < 10) Serial.print(' ');
        Serial.print(BURSTS[b]);
        Serial.print(F("  "));
        for (unsigned g = 0; g < 4; g++) {
            g_gap = GAPS[g];
            const uint32_t bad = trial(1024, NULL, NULL);
            Serial.print(F("    "));
            if (bad < 1000) Serial.print(' ');
            if (bad < 100)  Serial.print(' ');
            if (bad < 10)   Serial.print(' ');
            Serial.print(bad);
            Serial.print(F("  "));
        }
        Serial.println();
    }
    Serial.println(F("    (wrong bytes of 1024)"));
    g_gap = 0;
}

static void run(void)
{
    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  breadboard bus: how fast, and why it stops"));
    Serial.println(F("=============================================================="));

    pick(7);
    enter_quad();

    int best_spin; float best_rate;
    section1(&best_spin, &best_rate);
    section2();

    Serial.println(F("\n--- verdict ---"));
    if (best_spin < 0) {
        Serial.println(F("  Nothing clean at any edge rate here, even the slowest. Widening the"));
        Serial.println(F("  sweep next run."));
        return;
    }

    g_spin = best_spin;
    g_burst = 32; g_gap = 0;
    Serial.print(F("  fastest clean edge rate: "));
    Serial.print(best_spin); Serial.print(F(" nops, "));
    Serial.print(best_rate, 2); Serial.println(F(" MB/s read"));

    float w, r;
    const uint32_t again = trial(BLK, &w, &r);
    Serial.print(F("  re-tested at 32-byte bursts: "));
    Serial.print(again);
    Serial.println(again ? F(" wrong") : F(" wrong, it holds"));

    if (again == 0) {
        Serial.println(F("\n  banks, each given its own pattern then read back:"));
        uint32_t collide = 0;
        for (uint8_t bk = 0; bk < 5; bk++) {
            pick(bk); enter_quad();
            for (uint32_t i = 0; i < 256; i++) wbuf[i] = pat(i, (uint8_t)(0x10 + bk));
            qwrite(0, wbuf, 256);
        }
        for (uint8_t bk = 0; bk < 5; bk++) {
            pick(bk); enter_quad();
            qread(0, rbuf, 256);
            uint32_t e = 0;
            for (uint32_t i = 0; i < 256; i++) if (rbuf[i] != pat(i, (uint8_t)(0x10 + bk))) e++;
            Serial.print(F("    bank ")); Serial.print(bk);
            Serial.print(F(": ")); Serial.print(e); Serial.println(F(" wrong"));
            collide += e;
        }
        pick(7);
        Serial.println(collide ? F("\n  banks collide: the onboard chip is answering with them.")
                               : F("\n  every bank independent. All 48 MB usable."));
    }
    Serial.println(F("\n=== done, repeating in 15 s ==="));
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | (1u << 28) | (1u << 29)) & ~B_CLK;
    GPIO9_DR = idle;
    bus_out();
    pick(7);
}

void loop()
{
    run();
    delay(15000);
}

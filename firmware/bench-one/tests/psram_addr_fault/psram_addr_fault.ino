/* ===========================================================================================
 *  psram_addr_fault -- is it timing, or is it addressing
 * ===========================================================================================
 *
 *  THE CONTRADICTION THIS RESOLVES
 *  -------------------------------
 *  Over 128 kB, three rounds, the bus is clean at 8 read no-ops: zero wrong bytes in 393,216.
 *  Over 8 MB, the same settings give about 6,900 wrong in 8,388,608.
 *
 *  Those two numbers cannot both describe a uniform error rate. One says below 2.5 per million, the
 *  other says 820 per million -- a factor of three hundred. Errors that appear only when the test
 *  grows are not randomly scattered; they live at addresses the small test never visits. The small
 *  test stops at 0x020000, so every address line above A16 is held at zero for the whole of it.
 *
 *  A timing fault does not care which address it corrupts. An addressing fault only shows up when
 *  the line in question is actually driven high.
 *
 *  HOW TO TELL THEM APART IN ONE PASS
 *  ----------------------------------
 *  Store, at every four-byte word, that word's own byte address. Then a wrong word is not just
 *  "wrong" -- it says where it thinks it came from, and the XOR of where it was found with where it
 *  claims to be NAMES THE BROKEN ADDRESS BIT. A single power of two dominating that histogram is an
 *  address line. Scattered values with low bit counts are data corruption.
 *
 *  This is the same tactic that found the one-bit clock shift earlier: make the data carry enough
 *  information to identify its own fault, then look at the bytes instead of a pass/fail count.
 *
 *  AND THE CONTROL THAT SETTLES IT
 *  -------------------------------
 *  The whole sweep runs at three read speeds: the fast edge, the middle of the clean window, and
 *  far past it. If the error count barely moves between 8 no-ops and 40, slowing down is not the
 *  fix and never was, because the fault is not in the timing.
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
#define BURST 96

static uint32_t idle;

static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }
static inline uint32_t qbase(void) { return idle & ~B_SS0 & ~B_CLK & ~B_DATA; }
static inline uint32_t sbase(void) { return qbase() | (1u << 28) | (1u << 29); }

template <int S> struct Nop { static inline void go() { __asm__ volatile("nop"); Nop<S - 1>::go(); } };
template <>      struct Nop<0> { static inline void go() { } };
template <int S> static inline void spin(void) { Nop<S>::go(); }

template <int S> static inline void put_nib(uint32_t base, uint8_t nib)
{
    const uint32_t v = base | ((uint32_t)nib << DSHIFT);
    GPIO9_DR = v;
    GPIO9_DR = v | B_CLK;  spin<S>();
    GPIO9_DR = v;          spin<S>();
}

template <int S> static inline uint8_t get_nib(uint32_t base)
{
    GPIO9_DR = base | B_CLK;  spin<S>();
    const uint8_t n = (uint8_t)((GPIO9_PSR >> DSHIFT) & 0xF);
    GPIO9_DR = base;          spin<S>();
    return n;
}

template <int S> static void s_byte(uint32_t base, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = base | (((v >> i) & 1u) << DSHIFT);
        GPIO9_DR = d;
        GPIO9_DR = d | B_CLK;  spin<S>();
        GPIO9_DR = d;          spin<S>();
    }
}

static void cmd_single(uint8_t c)
{
    bus_out(); const uint32_t b = sbase();
    GPIO9_DR = b; s_byte<8>(b, c); GPIO9_DR = idle; delayMicroseconds(5);
}
static void cmd_quad(uint8_t c)
{
    bus_out(); const uint32_t b = qbase();
    GPIO9_DR = b; put_nib<8>(b, c >> 4); put_nib<8>(b, c & 0xF);
    GPIO9_DR = idle; delayMicroseconds(5);
}

template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

template <int S> static void wr(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<S>(b, 0x3); put_nib<S>(b, 0x8); addr_out<S>(b, a);
        for (uint32_t i = 0; i < n; i++) { put_nib<S>(b, s[i] >> 4); put_nib<S>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

template <int S> static void rd(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = qbase(); GPIO9_DR = b;
        put_nib<S>(b, 0xE); put_nib<S>(b, 0xB); addr_out<S>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<S>(); GPIO9_DR = b; spin<S>(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib<S>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<S>(b));
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    delayMicroseconds(10);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | (1u << 28) | (1u << 29)) & ~B_CLK;
}

static void enter_quad(void)
{
    cmd_quad(0xF5); cmd_single(0x66); cmd_single(0x99);
    delay(2); cmd_single(0x35); delayMicroseconds(50);
}

/* ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint32_t blk[BLK / 4];

/* the tally. Every counter here exists to answer one specific question about the fault. */
static uint32_t n_bad;              /* wrong words */
static uint32_t n_alias;            /* wrong words that read as a DIFFERENT VALID address */
static uint32_t n_garbage;          /* wrong words that are not any address at all */
static uint32_t per_mb[8];          /* which megabyte */
static uint32_t bit_hist[24];       /* which address bit the alias names */
static uint32_t lane_hist[4];       /* which byte lane of the word disagrees */
static uint32_t nib_lo, nib_hi;     /* low or high nibble of the differing byte */
static uint32_t first_a, first_e, first_g;

static void reset_tally(void)
{
    n_bad = n_alias = n_garbage = nib_lo = nib_hi = 0;
    first_a = first_e = first_g = 0;
    for (int i = 0; i < 8;  i++) per_mb[i]    = 0;
    for (int i = 0; i < 24; i++) bit_hist[i]  = 0;
    for (int i = 0; i < 4;  i++) lane_hist[i] = 0;
}

static void judge(uint32_t addr, uint32_t expect, uint32_t got)
{
    if (n_bad == 0) { first_a = addr; first_e = expect; first_g = got; }
    n_bad++;
    per_mb[(addr >> 20) & 7]++;

    /* which byte lanes of the 32-bit word differ, and within the first such byte, which nibble */
    const uint32_t x = expect ^ got;
    for (int L = 0; L < 4; L++) {
        const uint8_t d = (uint8_t)((x >> (L * 8)) & 0xFF);
        if (d) {
            lane_hist[L]++;
            if (d & 0x0F) nib_lo++;
            if (d & 0xF0) nib_hi++;
        }
    }

    /* does the value it returned name a real, word-aligned address inside the part? If so this is
     * an aliasing fault and the XOR tells us which address line failed to reach the chip. */
    if ((got & 3u) == 0 && got < SIZE) {
        n_alias++;
        const uint32_t a = addr ^ got;
        for (int b = 0; b < 24; b++) if (a & (1u << b)) bit_hist[b]++;
    } else {
        n_garbage++;
    }
}

template <int S> static uint32_t verify(void)
{
    reset_tally();
    const uint32_t t0 = ARM_DWT_CYCCNT;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<S>(off, (uint8_t *)blk, BLK);
        for (uint32_t i = 0; i < BLK / 4; i++) {
            const uint32_t expect = off + i * 4;
            if (blk[i] != expect) judge(expect, expect, blk[i]);
        }
    }
    return ARM_DWT_CYCCNT - t0;
}

static void report(int nops, uint32_t cyc)
{
    const float s = (float)cyc / (float)F_CPU_ACTUAL;
    Serial.print(F("\n  read at ")); Serial.print(nops);
    Serial.print(F(" no-ops, ")); Serial.print((float)SIZE / s / 1e6f, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    wrong words: ")); Serial.print(n_bad);
    Serial.print(F(" of ")); Serial.println(SIZE / 4);
    if (n_bad == 0) { Serial.println(F("    clean.")); return; }

    Serial.print(F("    of those, ")); Serial.print(n_alias);
    Serial.print(F(" read as another valid address, ")); Serial.print(n_garbage);
    Serial.println(F(" were not an address at all"));

    Serial.print(F("    first: at 0x")); Serial.print(first_a, HEX);
    Serial.print(F(" expected 0x")); Serial.print(first_e, HEX);
    Serial.print(F(" got 0x")); Serial.println(first_g, HEX);

    Serial.print(F("    by megabyte: "));
    for (int i = 0; i < 8; i++) { Serial.print(per_mb[i]); Serial.print(' '); }
    Serial.println();

    Serial.print(F("    by byte lane: "));
    for (int i = 0; i < 4; i++) { Serial.print(lane_hist[i]); Serial.print(' '); }
    Serial.print(F("   low nibble ")); Serial.print(nib_lo);
    Serial.print(F(", high nibble ")); Serial.println(nib_hi);

    /* the answer, if there is one */
    int top = -1; uint32_t topn = 0, total = 0;
    for (int b = 0; b < 24; b++) { total += bit_hist[b]; if (bit_hist[b] > topn) { topn = bit_hist[b]; top = b; } }
    if (total) {
        Serial.print(F("    address bits implicated: "));
        for (int b = 23; b >= 0; b--) if (bit_hist[b]) {
            Serial.print('A'); Serial.print(b);
            Serial.print('='); Serial.print(bit_hist[b]); Serial.print(' ');
        }
        Serial.println();
        if (topn * 2 > total) {
            Serial.print(F("    --> A")); Serial.print(top);
            Serial.print(F(" accounts for ")); Serial.print(100.0f * topn / total, 0);
            Serial.println(F("% of the aliases. That is one address line, not timing."));
        }
    }
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1);
    bus_out();
    pick(7);
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  every word holds its own address, so a wrong word names its fault"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    /* written slowly and once. If writes were the problem the three reads below would all agree
     * with each other and disagree with the pattern -- which is itself a distinguishable answer. */
    Serial.println(F("\n  writing 8 MB of self-identifying words at 16 no-ops"));
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        for (uint32_t i = 0; i < BLK / 4; i++) blk[i] = off + i * 4;
        wr<16>(off, (const uint8_t *)blk, BLK);
    }

    /* Two verifies back to back at every setting. The first run showed errors packed into the
     * first megabyte read after the write pass and almost none afterwards, which is the signature
     * of something settling rather than of an address or a data line. A second immediate pass over
     * the same bytes at the same speed either repeats that or it does not, and that one comparison
     * separates "this setting is marginal" from "the bus needs a moment after a long run of
     * writing". */
    static const int SET[] = { 8, 10, 12, 14, 16, 20, 24 };
    Serial.println(F("  full 8 MB at each setting, twice in a row"));
    Serial.println(F("    nops   MB/s    wrong pass1   wrong pass2   first fault"));
    int clean_from = -1;
    for (unsigned k = 0; k < sizeof(SET) / sizeof(SET[0]); k++) {
        uint32_t c1 = 0, b1 = 0, fa = 0, al = 0, b2 = 0;
        switch (SET[k]) {
            case  8: c1 = verify< 8>(); break;
            case 10: c1 = verify<10>(); break;
            case 12: c1 = verify<12>(); break;
            case 14: c1 = verify<14>(); break;
            case 16: c1 = verify<16>(); break;
            case 20: c1 = verify<20>(); break;
            default: c1 = verify<24>(); break;
        }
        b1 = n_bad; fa = first_a; al = n_alias;
        switch (SET[k]) {
            case  8: verify< 8>(); break;
            case 10: verify<10>(); break;
            case 12: verify<12>(); break;
            case 14: verify<14>(); break;
            case 16: verify<16>(); break;
            case 20: verify<20>(); break;
            default: verify<24>(); break;
        }
        b2 = n_bad;

        const float s = (float)c1 / (float)F_CPU_ACTUAL;
        Serial.print(F("     "));
        if (SET[k] < 10) Serial.print(' ');
        Serial.print(SET[k]);                    Serial.print(F("    "));
        Serial.print((float)SIZE / s / 1e6f, 2); Serial.print(F("   "));
        Serial.print(b1);                        Serial.print(F("            "));
        Serial.print(b2);                        Serial.print(F("            "));
        if (b1 || b2) {
            Serial.print(F("0x")); Serial.print(fa, HEX);
            Serial.print(F("  (")); Serial.print(al); Serial.print(F(" aliases)"));
        } else {
            Serial.print('-');
        }
        Serial.println();
        if (b1 == 0 && b2 == 0 && clean_from < 0) clean_from = SET[k];
    }

    Serial.println(F("--- verdict ---"));
    if (clean_from < 0) {
        Serial.println(F("  nothing up to 24 no-ops is clean over the full range."));
    } else {
        Serial.print(F("  clean over all 8 MB, twice, from "));
        Serial.print(clean_from);
        Serial.println(F(" no-ops upward."));
        Serial.print(F("  The driver shipped at 8. "));
        Serial.println(clean_from > 8
            ? F("That was the cliff edge, and it has fallen off it.")
            : F("Eight still holds over the full range today."));
    }
    Serial.println(F("  A count that drops steeply with no-ops is timing. A count that barely"));
    Serial.println(F("  moved would have been a wire, and the alias tally would have named it."));
    Serial.println(F("  Pass 2 much cleaner than pass 1 at the same speed means the bus wants a"));
    Serial.println(F("  moment after a long run of writes, which is a supply story, not an edge one."));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

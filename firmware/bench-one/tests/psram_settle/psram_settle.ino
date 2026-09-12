/* ===========================================================================================
 *  psram_settle -- the first read after a long write, and whether a wait fixes it
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

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
/* ===========================================================================================
 *  the question
 *
 *  At 8 read no-ops over the full 8 MB: pass one has about 18,000 wrong words and pass two, over
 *  the same bytes at the same speed with nothing changed, has zero. Every single run. At 10 no-ops
 *  and above both passes are clean.
 *
 *  Two things follow immediately. The bytes in the chip are correct, because the second read finds
 *  them. And the read path itself works at 8 no-ops, because the second read uses it. What fails is
 *  only the first read after 8 MB of solid writing.
 *
 *  That is a recovery effect, and the earlier dismissal of the supply was narrower than it looked.
 *  The test that cleared the supply varied burst length and idle gaps at an edge rate that was
 *  already failing, and found the error count barely moved. It never asked what happens to the
 *  first read after half a second of driving four data lines and a clock flat out. This does.
 *
 *  If a delay before reading brings pass one to zero, the fast setting is usable with one wait and
 *  the bulk capacitor is worth fitting. If no delay helps, the timing has to give instead.
 *
 *  It also measures where in the pass the errors stop, in bytes, so "it settles" becomes a number
 *  rather than an impression.
 * ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint32_t blk[BLK / 4];

static uint32_t n_bad, first_a, last_a;

/* verify at 8 no-ops, the setting that fails only on a first pass, and record not just how many
 * words were wrong but the address of the LAST one -- that is where the bus came good, and it turns
 * a settling story into a measurement. */
static uint32_t verify8(void)
{
    n_bad = 0; first_a = 0; last_a = 0;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<8>(off, (uint8_t *)blk, BLK);
        for (uint32_t i = 0; i < BLK / 4; i++) {
            const uint32_t expect = off + i * 4;
            if (blk[i] != expect) {
                if (n_bad == 0) first_a = expect;
                last_a = expect;
                n_bad++;
            }
        }
    }
    return n_bad;
}

static void fill(void)
{
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        for (uint32_t i = 0; i < BLK / 4; i++) blk[i] = off + i * 4;
        wr<16>(off, (const uint8_t *)blk, BLK);
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
    Serial.println(F("  does the first read after 8 MB of writing just need a moment"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    static const uint32_t MS[] = { 0, 1, 5, 20, 100, 500 };
    Serial.println(F("  write all 8 MB, wait, then read all 8 MB at 8 no-ops"));
    Serial.println(F("    wait ms   wrong words   first fault   last fault"));
    int fixed_at = -1;
    for (unsigned k = 0; k < sizeof(MS) / sizeof(MS[0]); k++) {
        fill();
        if (MS[k]) delay(MS[k]);
        const uint32_t bad = verify8();
        Serial.print(F("       "));
        if (MS[k] < 10)  Serial.print(' ');
        if (MS[k] < 100) Serial.print(' ');
        Serial.print(MS[k]);   Serial.print(F("        "));
        Serial.print(bad);     Serial.print(F("         "));
        if (bad) {
            Serial.print(F("0x")); Serial.print(first_a, HEX);
            Serial.print(F("      0x")); Serial.print(last_a, HEX);
            Serial.print(F("  (settled after ")); Serial.print(last_a / 1024u);
            Serial.print(F(" kB of reading)"));
        } else {
            Serial.print(F("-            -"));
        }
        Serial.println();
        if (bad == 0 && fixed_at < 0) fixed_at = (int)MS[k];
    }

    /* The control that keeps this honest. If the chip is simply better at being read a second time
     * regardless of any wait, then a delay proving nothing would still look like a fix when the
     * loop repeats -- so read twice at the end with no wait at all and show both counts. */
    fill();
    const uint32_t p1 = verify8();
    const uint32_t p2 = verify8();
    Serial.print(F("\n  control, no wait: first pass ")); Serial.print(p1);
    Serial.print(F(" wrong, second pass ")); Serial.print(p2);
    Serial.println(F(" wrong"));

    Serial.println(F("--- verdict ---"));
    if (fixed_at == 0) {
        Serial.println(F("  pass one was clean with no wait at all this time. The effect is not"));
        Serial.println(F("  reproducing right now, so do not tune on it."));
    } else if (fixed_at > 0) {
        Serial.print(F("  a "));
        Serial.print(fixed_at);
        Serial.println(F(" ms wait before reading makes the fast setting clean over all 8 MB."));
        Serial.println(F("  So the chip and the read path are both fine at 8 no-ops, and what"));
        Serial.println(F("  fails is recovery from sustained switching. That is the bulk"));
        Serial.println(F("  capacitor's job, and it is worth fitting after all -- not to raise"));
        Serial.println(F("  the clock, which it never would, but to remove this wait."));
    } else {
        Serial.println(F("  no wait up to half a second helps. Recovery is not the mechanism,"));
        Serial.println(F("  so the timing has to give: run reads at 10 no-ops, which measured"));
        Serial.println(F("  clean over the full range twice at 13.49 MB/s."));
    }

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

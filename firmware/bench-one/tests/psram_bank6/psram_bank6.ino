/* ===========================================================================================
 *  psram_bank6 -- 8 MB or 48? one wire decides, and this detects which is wired
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
 *  the capacity problem, and the one wire that fixes it
 *
 *  Six chips are wired and all six answer an identity read, but only 8 MB of the 48 is usable. The
 *  74LVC138A decoder's enable shares a wire with the onboard chip's chip select, so selecting any
 *  breadboard bank also selects the onboard chip, and both drive the data lines at once. Proven, not
 *  assumed: writing a different pattern to each bank in turn and reading them back gives every bank
 *  wrong except the one written last, because the onboard chip holds whatever was written last and
 *  agrees only with that.
 *
 *  The accepted repair was to desolder the onboard chip or lift its pin 1. Neither is necessary.
 *
 *  WHAT THE DECODER ENABLE ACTUALLY NEEDS TO BE
 *  -------------------------------------------
 *  A chip select of its own. Then:
 *
 *      decoder enabled, CS0 held high   ->  exactly one of the five breadboard banks
 *      decoder disabled, CS0 driven     ->  the onboard chip, alone
 *
 *  Nothing is desoldered, six banks become independent, and the usable capacity goes from 8 MB to
 *  48 MB. It is one wire moved from the CS0 pad to a free pin.
 *
 *  WHICH PIN, AND WHY IT MATTERS THAT IT IS THIS ONE
 *  ------------------------------------------------
 *  Pin 5. It is free, and more importantly `teensy_pinmap` asked the core and found it is GPIO9
 *  bit 8 -- the same register as the clock and the four data lines. The bus driver writes that
 *  register whole, once per edge, so a chip select living anywhere else would cost a second store on
 *  every burst boundary and would be a second thing to get wrong in exactly the way the decoder's
 *  address pins were got wrong once already.
 *
 *  The enable must be active low, and that is deduced rather than assumed: it is currently driven by
 *  a chip select, which is active low, and all six chips do answer. Had it been wired to the
 *  active-high G1 input, asserting CS0 would have disabled the decoder and no breadboard bank could
 *  ever have responded.
 *
 *  WHAT THIS SKETCH DOES TODAY, BEFORE THE WIRE MOVES
 *  -------------------------------------------------
 *  It tries the split wiring first, and if that fails it falls back and confirms the collision is
 *  still there. So it is useful on both sides of the repair: run it now and it should report 8 MB and
 *  name the collision; run it after moving one wire and it should report 48 MB without any edit.
 * ======================================================================================== */

#define B_DEC  (1u << 8)               /* pin 5, GPIO9 bit 8 -- see teensy_pinmap */
#define BLK    (8u * 1024u)
#define BANKSZ (8u * 1024u * 1024u)
#define PROBE  (64u * 1024u)           /* per-bank check: enough to catch a collision instantly */

static uint8_t buf[BLK];
static uint32_t g_cs = B_SS0;          /* which bit is the chip select for the current target */
static bool     g_split = false;

static inline uint8_t pat(uint8_t bank, uint32_t i)
{
    return (uint8_t)(i * 0x9Du + 0x3Bu + (uint32_t)bank * 0x57u);
}

/* the bus layer above clears B_SS0 to assert. With a separate decoder enable the asserted bit
 * depends on the target, so the base used for a burst is computed from g_cs instead. */
static inline uint32_t cbase(void) { return (idle | B_SS0 | B_DEC) & ~g_cs & ~B_CLK & ~B_DATA; }

static void target(uint8_t bank)
{
    if (g_split && bank < 5) {
        /* a breadboard bank: the decoder picks it, and CS0 stays high so the onboard chip sleeps */
        digitalWriteFast(PIN_A, (bank >> 0) & 1);
        digitalWriteFast(PIN_B, (bank >> 1) & 1);
        digitalWriteFast(PIN_C, (bank >> 2) & 1);
        g_cs = B_DEC;
    } else if (g_split) {
        /* the onboard chip: park the decoder on its unconnected output AND leave its enable high */
        digitalWriteFast(PIN_A, 1); digitalWriteFast(PIN_B, 1); digitalWriteFast(PIN_C, 1);
        g_cs = B_SS0;
    } else {
        /* legacy wiring: one select for everything, which is the whole problem */
        digitalWriteFast(PIN_A, (bank >> 0) & 1);
        digitalWriteFast(PIN_B, (bank >> 1) & 1);
        digitalWriteFast(PIN_C, (bank >> 2) & 1);
        g_cs = B_SS0;
    }
    delayMicroseconds(10);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC | (1u << 28) | (1u << 29)) & ~B_CLK;
}

/* write and read using the selected chip select rather than B_SS0 unconditionally */
static void bwrite(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > 96u) ? 96u : len;
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b;
        put_nib<6>(b, 0x3); put_nib<6>(b, 0x8); addr_out<6>(b, a);
        for (uint32_t i = 0; i < n; i++) { put_nib<6>(b, s[i] >> 4); put_nib<6>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

static void bread(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > 96u) ? 96u : len;
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b;
        put_nib<10>(b, 0xE); put_nib<10>(b, 0xB); addr_out<10>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<10>(); GPIO9_DR = b; spin<10>(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib<10>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<10>(b));
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

/* quad mode has to be entered per chip, because each chip has its own mode latch */
static void quad_here(void)
{
    const uint32_t b4 = cbase() | (1u << 28) | (1u << 29);
    bus_out();
    { const uint32_t b = cbase();
      GPIO9_DR = b; put_nib<10>(b, 0xF); put_nib<10>(b, 0x5); GPIO9_DR = idle;
      delayMicroseconds(5); }
    GPIO9_DR = b4; s_byte<10>(b4, 0x66); GPIO9_DR = idle; delayMicroseconds(5);
    GPIO9_DR = b4; s_byte<10>(b4, 0x99); GPIO9_DR = idle; delay(2);
    GPIO9_DR = b4; s_byte<10>(b4, 0x35); GPIO9_DR = idle; delayMicroseconds(50);
}

/* The test that settles it: a DIFFERENT pattern per bank, all written before any is read. If the
 * banks are independent every one reads back its own. If they share a select, the last chip written
 * holds the last pattern and only that bank agrees -- which is exactly the signature recorded in
 * docs/40, so this both detects the repair and re-proves the fault. */
static uint32_t cross_check(uint32_t *per_bank)
{
    for (uint8_t k = 0; k < 6; k++) {
        target(k); quad_here();
        for (uint32_t off = 0; off < PROBE; off += BLK) {
            for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(k, off + i);
            bwrite(off, buf, BLK);
        }
    }
    uint32_t good = 0;
    for (uint8_t k = 0; k < 6; k++) {
        target(k);
        uint32_t bad = 0;
        for (uint32_t off = 0; off < PROBE; off += BLK) {
            bread(off, buf, BLK);
            for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat(k, off + i)) bad++;
        }
        per_bank[k] = bad;
        if (bad == 0) good++;
    }
    return good;
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    pinMode(5, OUTPUT); digitalWriteFast(5, HIGH);        /* the new enable, idle high */
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    bus_out();
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

void loop()
{
    uint32_t per[6];

    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  six banks, or one? the decoder enable decides"));
    Serial.println(F("=================================================================="));

    /* try the repaired wiring first. It costs one pass and tells the truth either way. */
    g_split = true;
    Serial.println(F("\n  [split] decoder enable on pin 5, CS0 reserved for the onboard chip"));
    uint32_t good = cross_check(per);
    Serial.print(F("    wrong bytes per bank, 0 to 5:  "));
    for (int k = 0; k < 6; k++) { Serial.print(per[k]); Serial.print(' '); }
    Serial.println();
    Serial.print(F("    banks holding their own data: ")); Serial.print(good); Serial.println(F(" of 6"));

    if (good < 6) {
        g_split = false;
        Serial.println(F("\n  [legacy] one select for everything, as currently wired"));
        const uint32_t g2 = cross_check(per);
        Serial.print(F("    wrong bytes per bank, 0 to 5:  "));
        for (int k = 0; k < 6; k++) { Serial.print(per[k]); Serial.print(' '); }
        Serial.println();
        Serial.print(F("    banks holding their own data: ")); Serial.print(g2); Serial.println(F(" of 6"));
    }

    Serial.println(F("\n--- verdict ---"));
    if (good == 6) {
        Serial.println(F("  Six independent banks. The wire has been moved and the capacity is"));
        Serial.print(F("  "));
        Serial.print(6u * (BANKSZ / (1024u * 1024u)));
        Serial.println(F(" MB, not 8."));
    } else if (good == 1) {
        Serial.println(F("  One bank. The decoder enable is still sharing CS0, so every breadboard"));
        Serial.println(F("  bank brings the onboard chip onto the data lines with it. Exactly one"));
        Serial.println(F("  bank reads back correctly, and it is the one written last."));
        Serial.println(F("  THE REPAIR: move the decoder's enable wire -- the one now on the CS0"));
        Serial.println(F("  pad, pin 48 -- to pin 5. Nothing else changes, no chip comes off, and"));
        Serial.println(F("  this sketch will report 48 MB without being edited."));
    } else {
        Serial.print(F("  ")); Serial.print(good);
        Serial.println(F(" banks verified, which is neither one nor six. Something is wired"));
        Serial.println(F("  differently from both hypotheses -- check the decoder address lines"));
        Serial.println(F("  before trusting any capacity figure."));
    }

    Serial.println(F("\n=== repeating in 20 s ==="));
    delay(20000);
}

/* ===========================================================================================
 *  psram_pattern -- does the error rate depend on how hard the data makes the lines switch
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
 *  the variable every one of my own diagnostics controlled by accident: the DATA
 *
 *  The pair test says write 6 with read 10 is clean over 96 MB of traffic. The driver, at those
 *  exact settings, reports one to five wrong bytes in every 8 MB. Both cannot be right about the
 *  same bus, so they are not testing the same thing.
 *
 *  They differ in one place. Every diagnostic written this afternoon stores each word's own address,
 *  because that lets a wrong word name its own fault -- and an ascending address is the quietest
 *  pattern there is. Three of its four bytes barely change from one word to the next. The driver
 *  writes a pseudorandom byte stream, where consecutive nibbles differ in about half their bits.
 *
 *  Switching activity is what makes a bus noisy. Every bit that changes on a data line pulls current
 *  through whatever inductance stands between the chip and its supply, and five jumper wires plus a
 *  breadboard rail is a lot of inductance. A quiet pattern is an easier test, and nothing in the
 *  sweeps said so, because none of them varied it.
 *
 *  So this holds the settings fixed and varies only the data:
 *
 *      address     the quiet one every diagnostic used. Three bytes in four hardly move.
 *      random      the driver's own pattern, about half the bits changing per byte.
 *      00/FF       every line swinging the full rail on every single nibble. The worst case.
 *      55/AA       full swing too, but with neighbouring lines always in opposition, which is
 *                  the shape that couples between adjacent wires rather than loading the supply.
 *
 *  If the counts climb with activity then the limit is current, the fix is the bulk capacitor the
 *  board is missing, and every clean result above was flattered by its own test data.
 * ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint8_t  blk[BLK];
static uint32_t n_bad, first_a;

/* each pattern is a pure function of byte address, so verifying needs no second buffer and cannot
 * accidentally compare a block against itself */
static inline uint8_t pat(uint32_t P, uint32_t x)
{
    switch (P) {
        case 0:  return (uint8_t)(x >> ((x & 3u) * 8));        /* the word holds its own address */
        case 1:  return (uint8_t)(x * 0x9Du + 0x3Bu);          /* the driver's own stream */
        case 2:  return (x & 1u) ? 0xFFu : 0x00u;              /* full swing, all lines together */
        default: return (x & 1u) ? 0xAAu : 0x55u;              /* full swing, lines in opposition */
    }
}

template <int S> static uint32_t check(uint32_t P)
{
    n_bad = 0; first_a = 0;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<S>(off, blk, BLK);
        for (uint32_t i = 0; i < BLK; i++)
            if (blk[i] != pat(P, off + i)) { if (!n_bad) first_a = off + i; n_bad++; }
    }
    return n_bad;
}

template <int S> static void fill(uint32_t P)
{
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) blk[i] = pat(P, off + i);
        wr<S>(off, blk, BLK);
    }
}

template <int SWR, int SRD> static void cell(int sw, int sr, uint32_t P, const char *name)
{
    fill<SWR>(P);
    const uint32_t b1 = check<SRD>(P);
    const uint32_t f1 = first_a;
    const uint32_t b2 = check<SRD>(P);

    Serial.print(F("      "));
    if (sw < 10) Serial.print(' ');
    Serial.print(sw);  Serial.print('/');
    if (sr < 10) Serial.print(' ');
    Serial.print(sr);  Serial.print(F("   "));
    Serial.print(name);
    for (unsigned k = strlen(name); k < 9; k++) Serial.print(' ');
    if (b1 < 10)     Serial.print(' ');
    if (b1 < 100)    Serial.print(' ');
    if (b1 < 1000)   Serial.print(' ');
    if (b1 < 10000)  Serial.print(' ');
    Serial.print(b1);  Serial.print(F("     "));
    if (b2 < 10)     Serial.print(' ');
    if (b2 < 100)    Serial.print(' ');
    if (b2 < 1000)   Serial.print(' ');
    if (b2 < 10000)  Serial.print(' ');
    Serial.print(b2);  Serial.print(F("   "));
    if (b1 || b2) { Serial.print(F("from 0x")); Serial.print(f1, HEX); }
    else          { Serial.print(F("clean")); }
    Serial.println();
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
    Serial.println(F("  settings fixed, data varied. 8 MB written, read twice, per row."));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    Serial.println(F("      W/R   pattern   pass1     pass2"));
    cell< 6,10>( 6, 10, 0, "address");
    cell< 6,10>( 6, 10, 1, "random");
    cell< 6,10>( 6, 10, 2, "00/FF");
    cell< 6,10>( 6, 10, 3, "55/AA");

    Serial.println(F("      -- and with more margin on the read --"));
    cell< 6,14>( 6, 14, 1, "random");
    cell< 6,14>( 6, 14, 2, "00/FF");
    cell< 6,20>( 6, 20, 2, "00/FF");
    cell< 8,20>( 8, 20, 2, "00/FF");

    Serial.println(F("--- reading this ---"));
    Serial.println(F("  Address clean and the rest dirty: activity is the limit, the missing bulk"));
    Serial.println(F("  capacitor is the fix, and every earlier clean result was flattered by a"));
    Serial.println(F("  quiet pattern. Qualify on 00/FF from here on, never on addresses."));
    Serial.println(F("  00/FF and 55/AA alike, both worse than random: supply current."));
    Serial.println(F("  55/AA much worse than 00/FF: coupling between neighbouring wires instead,"));
    Serial.println(F("  which separating the data lines would fix and a capacitor would not."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

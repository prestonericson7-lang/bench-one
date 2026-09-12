/* ===========================================================================================
 *  psram_pair -- qualify the write and read settings together, the way the driver uses them
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
 *  the cell nothing had tested: the two settings TOGETHER
 *
 *  Everything so far varied one side and held the other generous, which is the right way to isolate
 *  a path and the wrong way to qualify a configuration. The results were:
 *
 *      reads at 10, writes held at 16      8 MB clean, twice
 *      writes at 6, reads held at 16      8 MB clean, twice, and so is every setting to 20
 *      writes at 6 AND reads at 10        one to five wrong bytes in 8 MB, every run
 *
 *  No single-variable sweep can see that, because the failing combination is never visited by
 *  either of them. A faster write pass leaves the bus in a worse state for the read that follows,
 *  and the read setting that survives a gentle write pass does not survive a hard one.
 *
 *  So this measures pairs. Nothing is held generous. Each cell is the real thing: write the whole
 *  part at SW, read the whole part twice at SR, count. That is what the driver actually does, and
 *  it is the only shape of test whose answer the driver can use.
 * ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint32_t blk[BLK / 4];
static uint32_t n_bad, first_a;

template <int S> static uint32_t check(void)
{
    n_bad = 0; first_a = 0;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<S>(off, (uint8_t *)blk, BLK);
        for (uint32_t i = 0; i < BLK / 4; i++) {
            const uint32_t expect = off + i * 4;
            if (blk[i] != expect) { if (n_bad == 0) first_a = expect; n_bad++; }
        }
    }
    return n_bad;
}

template <int S> static void fill(void)
{
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        for (uint32_t i = 0; i < BLK / 4; i++) blk[i] = off + i * 4;
        wr<S>(off, (const uint8_t *)blk, BLK);
    }
}

/* one real configuration, reported as the driver would experience it: the first read after the
 * write is the one that matters, and the second says whether the first was a transient. */
template <int SWR, int SRD> static void cell(int sw, int sr)
{
    uint32_t t0 = ARM_DWT_CYCCNT;
    fill<SWR>();
    const float ws = (float)(ARM_DWT_CYCCNT - t0) / (float)F_CPU_ACTUAL;

    t0 = ARM_DWT_CYCCNT;
    const uint32_t b1 = check<SRD>();
    const float rs = (float)(ARM_DWT_CYCCNT - t0) / (float)F_CPU_ACTUAL;
    const uint32_t f1 = first_a;
    const uint32_t b2 = check<SRD>();

    Serial.print(F("      "));
    if (sw < 10) Serial.print(' ');
    Serial.print(sw);  Serial.print(F("    "));
    if (sr < 10) Serial.print(' ');
    Serial.print(sr);  Serial.print(F("    "));
    Serial.print((float)SIZE / ws / 1e6f, 2);  Serial.print(F("   "));
    Serial.print((float)SIZE / rs / 1e6f, 2);  Serial.print(F("   "));
    if (b1 < 10)   Serial.print(' ');
    if (b1 < 100)  Serial.print(' ');
    if (b1 < 1000) Serial.print(' ');
    Serial.print(b1);  Serial.print(F("    "));
    if (b2 < 10)   Serial.print(' ');
    if (b2 < 100)  Serial.print(' ');
    if (b2 < 1000) Serial.print(' ');
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
    Serial.println(F("  real configurations: both settings at once, over the whole part"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    Serial.println(F("    write  read   W MB/s  R MB/s  pass1  pass2"));
    cell< 6,10>( 6, 10);
    cell< 6,12>( 6, 12);
    cell< 6,14>( 6, 14);
    cell< 8,12>( 8, 12);
    cell<10,12>(10, 12);
    cell<12,12>(12, 12);

    Serial.println(F("--- choosing from this ---"));
    Serial.println(F("  Take the fastest row that is clean in BOTH passes, then step once more"));
    Serial.println(F("  away from the cliff. The whole reason this test exists is that the last"));
    Serial.println(F("  configuration was chosen as the fastest row that passed once."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

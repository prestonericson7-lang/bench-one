/* ===========================================================================================
 *  psram_write_margin -- the write side, measured across the whole part instead of 128 kB
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
 *  the last gap, and why it stayed hidden
 *
 *  With reads at 10 no-ops the full 8 MB verified clean twice over, with writes held at 16. With
 *  writes dropped to 6 the same range gives one to five wrong bytes, every run. So what is left is
 *  the write path.
 *
 *  The write sweep that blessed 4 no-ops only ever covered 128 kB, three rounds. At the rate being
 *  seen now -- two bad bytes in 8,388,608 -- a 128 kB round expects 0.03 errors. Thirty rounds would
 *  still most likely come back clean. The sweep was not wrong; it was blind, and its resolution was
 *  never stated alongside its verdict.
 *
 *  That is the general lesson of the whole afternoon and it is worth writing down: a clean result
 *  bounds the error rate at roughly one over the number of bytes tested, and nothing better. 128 kB
 *  of silence means "below about ten per million", which is a long way from "zero".
 *
 *  So this sweeps writes across the whole part, with reads pinned at 16 where they are known clean,
 *  and reports counts rather than verdicts. Twice at each setting, because a single pass cannot
 *  distinguish two errors from zero when two is the number in question.
 * ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint32_t blk[BLK / 4];
static uint32_t n_bad, first_a, last_a, n_alias;

/* verified at 16 no-ops throughout: that read setting was measured clean over the full 8 MB, so a
 * disagreement here belongs to the writing. The alias count stays in because if a write ever lands
 * at the wrong address this is where it would show, and a wrong address is a different repair from
 * a wrong byte. */
static uint32_t check16(void)
{
    n_bad = 0; first_a = 0; last_a = 0; n_alias = 0;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<16>(off, (uint8_t *)blk, BLK);
        for (uint32_t i = 0; i < BLK / 4; i++) {
            const uint32_t expect = off + i * 4;
            const uint32_t got = blk[i];
            if (got != expect) {
                if (n_bad == 0) first_a = expect;
                last_a = expect;
                n_bad++;
                if ((got & 3u) == 0 && got < SIZE) n_alias++;
            }
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

template <int S> static void one(int nops)
{
    uint32_t t0 = ARM_DWT_CYCCNT;
    fill<S>();
    const float ws = (float)(ARM_DWT_CYCCNT - t0) / (float)F_CPU_ACTUAL;
    const uint32_t b1 = check16();
    const uint32_t f1 = first_a, l1 = last_a, a1 = n_alias;

    fill<S>();
    const uint32_t b2 = check16();

    Serial.print(F("      "));
    if (nops < 10) Serial.print(' ');
    Serial.print(nops);                         Serial.print(F("     "));
    Serial.print((float)SIZE / ws / 1e6f, 2);   Serial.print(F("    "));
    if (b1 < 10)   Serial.print(' ');
    if (b1 < 100)  Serial.print(' ');
    if (b1 < 1000) Serial.print(' ');
    Serial.print(b1);                           Serial.print(F("      "));
    if (b2 < 10)   Serial.print(' ');
    if (b2 < 100)  Serial.print(' ');
    if (b2 < 1000) Serial.print(' ');
    Serial.print(b2);                           Serial.print(F("     "));
    if (b1) {
        Serial.print(F("0x")); Serial.print(f1, HEX);
        Serial.print(F(" .. 0x")); Serial.print(l1, HEX);
        Serial.print(F("  ")); Serial.print(a1); Serial.print(F(" aliases"));
    } else {
        Serial.print('-');
    }
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
    Serial.println(F("  writes across the whole 8 MB, read back where reads are clean"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    Serial.println(F("    nops   MB/s    pass1   pass2   where"));
    one< 6>( 6);
    one< 8>( 8);
    one<10>(10);
    one<12>(12);
    one<16>(16);
    one<20>(20);

    /* reversed, for the same reason the cold-start test ran both ways: a setting that only fails
     * when it goes first is not failing because of its setting */
    Serial.println(F("    reversed"));
    one<20>(20);
    one<12>(12);
    one< 6>( 6);

    Serial.println(F("--- what a clean row is worth ---"));
    Serial.println(F("  Two passes of 8 MB is 16,777,216 bytes. Silence there bounds the write"));
    Serial.println(F("  error rate below about 60 per billion, and no lower. It is not zero, it"));
    Serial.println(F("  is a number, and quoting it that way is the whole difference between"));
    Serial.println(F("  this sweep and the 128 kB one that blessed 4 no-ops."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

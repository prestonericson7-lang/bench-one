/* ===========================================================================================
 *  psram_soak -- run every candidate configuration for as long as it takes to resolve a rate
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
 *  why a soak and not another sweep
 *
 *  The corrected driver reports 0, 1, 2 or 3 wrong bytes per 8 MB, run after run. That is an error
 *  rate somewhere near two in ten million, and it is the hardest kind of number to work with: far
 *  too low for any single-pass test to resolve, and far too high to ignore in a machine whose whole
 *  purpose is holding model weights.
 *
 *  A sweep cannot settle it. Every sweep this afternoon declared a setting clean on one or two
 *  passes, and a clean pass over 8 MB only bounds the rate below about one in eight million -- which
 *  is the same order as the rate being measured. The sweeps were not lying; they had no resolution
 *  left at the scale that matters.
 *
 *  The only instrument that works here is time. Accumulate bytes and errors across every pass, never
 *  reset, and print the running rate in parts per billion. After a minute each configuration has a
 *  rough number; after twenty it has a real one, and the differences between settings become
 *  visible instead of inferred.
 *
 *  Written to run unattended, so it states its own confidence: the rate is printed beside the
 *  number of bytes it rests on, and a configuration with no errors yet is reported as an upper
 *  bound rather than as zero. There is no such thing as a measured zero, only a bound that has not
 *  been beaten yet.
 *
 *  Chunked at 16 kB and written with the driver's own pseudorandom pattern, so this is the driver's
 *  workload and not a friendlier one. The pattern test already showed that a quiet ascending-address
 *  pattern is not easier on this bus, but there is no reason to hand the soak an advantage.
 * ======================================================================================== */

#define CH   (16u * 1024u)
#define SIZE (8u * 1024u * 1024u)
static uint8_t buf[CH];

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

#define NCFG 6
static const int CW[NCFG] = {  6,  6,  6,  6,  8,  8 };
static const int CR[NCFG] = { 10, 12, 14, 16, 16, 20 };

static uint64_t acc_bytes[NCFG];
static uint64_t acc_errs[NCFG];
static uint32_t acc_pass[NCFG];
static uint32_t worst[NCFG];
static uint32_t passes_clean[NCFG];

template <int SWR, int SRD> static uint32_t one_pass(void)
{
    for (uint32_t off = 0; off < SIZE; off += CH) {
        for (uint32_t i = 0; i < CH; i++) buf[i] = pat(off + i);
        wr<SWR>(off, buf, CH);
    }
    uint32_t bad = 0;
    for (uint32_t off = 0; off < SIZE; off += CH) {
        rd<SRD>(off, buf, CH);
        for (uint32_t i = 0; i < CH; i++) if (buf[i] != pat(off + i)) bad++;
    }
    return bad;
}

static void record(int c, uint32_t bad)
{
    acc_bytes[c] += SIZE;
    acc_errs[c]  += bad;
    acc_pass[c]++;
    if (bad > worst[c]) worst[c] = bad;
    if (bad == 0) passes_clean[c]++;
}

static void pad(uint32_t v, uint32_t upto)
{
    for (uint32_t d = upto; d > 1; d /= 10) if (v < d) Serial.print(' ');
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
    for (int c = 0; c < NCFG; c++) {
        acc_bytes[c] = 0; acc_errs[c] = 0;
        acc_pass[c] = 0; worst[c] = 0; passes_clean[c] = 0;
    }
}

void loop()
{
    pick(7);
    enter_quad();

    /* one pass of every configuration per round, so they all see the same conditions as the board
     * warms, cools, or gets nudged. Running one config to exhaustion and then the next would make
     * the later ones a test of the later hour. */
    record(0, one_pass< 6,10>());
    record(1, one_pass< 6,12>());
    record(2, one_pass< 6,14>());
    record(3, one_pass< 6,16>());
    record(4, one_pass< 8,16>());
    record(5, one_pass< 8,20>());

    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.print(F("  soak, "));
    Serial.print(millis() / 60000u);
    Serial.print(F(" min "));
    Serial.print((millis() / 1000u) % 60u);
    Serial.print(F(" s, "));
    Serial.print(acc_pass[0]);
    Serial.println(F(" passes of each configuration"));
    Serial.println(F("=================================================================="));
    Serial.println(F("    W/R    MB tested   errors   worst pass   clean   rate"));

    int best = -1;
    for (int c = 0; c < NCFG; c++) {
        Serial.print(F("    "));
        if (CW[c] < 10) Serial.print(' ');
        Serial.print(CW[c]); Serial.print('/');
        if (CR[c] < 10) Serial.print(' ');
        Serial.print(CR[c]); Serial.print(F("    "));

        const uint32_t mb = (uint32_t)(acc_bytes[c] >> 20);
        pad(mb, 10000); Serial.print(mb); Serial.print(F("      "));

        const uint32_t e = (uint32_t)acc_errs[c];
        pad(e, 10000); Serial.print(e); Serial.print(F("      "));

        pad(worst[c], 1000); Serial.print(worst[c]); Serial.print(F("       "));
        pad(passes_clean[c], 100); Serial.print(passes_clean[c]);
        Serial.print('/'); Serial.print(acc_pass[c]); Serial.print(F("   "));

        /* a rate, or the bound that stands in for one when nothing has gone wrong yet */
        if (acc_bytes[c] == 0) { Serial.println(); continue; }
        const float ppb = 1e9f * (float)(double)acc_errs[c] / (float)(double)acc_bytes[c];
        if (e) {
            Serial.print(ppb, 1); Serial.println(F(" per billion"));
        } else {
            Serial.print(F("under "));
            Serial.print(1e9f / (float)(double)acc_bytes[c], 1);
            Serial.println(F(" per billion so far"));
            if (best < 0) best = c;
        }
    }

    Serial.println(F("  The right column is the point. A configuration with no errors yet is not"));
    Serial.println(F("  clean, it is bounded, and the bound falls as the soak runs. Compare the"));
    Serial.println(F("  bounds against each other, not against zero."));
    if (best >= 0) {
        Serial.print(F("  Fastest setting still unbeaten: "));
        Serial.print(CW[best]); Serial.print('/'); Serial.println(CR[best]);
    } else {
        Serial.println(F("  Every configuration has produced at least one error. None of them is"));
        Serial.println(F("  sound enough to hold weights unchecked, so the data needs a check code"));
        Serial.println(F("  rather than a faster setting."));
    }
}

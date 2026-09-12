/* ===========================================================================================
 *  psram_tcem -- chip-select-low time measured, not inferred, against the 8 us refresh limit
 *
 *  BUS-VARIANT: times a single burst in isolation, so the transfer loop is inlined here with the
 *  cycle counter inside the chip-select-low window instead of calling the shared read and write.
 *  The nibble sequence is identical; only the timing instrumentation differs.
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
 *  the safety check the corrected settings made necessary
 *
 *  The 96-byte burst was chosen when reads ran at 8 no-ops. At that speed a burst held chip select
 *  low for 6.46 us against a limit of 8, which was comfortable. Document 41 moved reads to 10 no-ops
 *  for margin, and every nibble is now about 7.5% longer -- so the same 96-byte burst holds chip
 *  select low for longer than it did, and the margin that justified it has shrunk without anyone
 *  deciding to spend it.
 *
 *  WHY THIS IS NOT A SPEED QUESTION
 *  These parts are DRAM behind an SPI interface. They refresh themselves, but only while chip select
 *  is high, and the datasheet limit on a single chip-select-low period is 8 us. Go past it and the
 *  chip does not complain, does not fail the transfer, and returns exactly the right bytes -- and
 *  then loses rows that have been sitting. The earlier burst sweep found 128 bytes measurably the
 *  fastest option and rejected it for this reason alone.
 *
 *  So a configuration can be fast, verified over 8 MB, clean in a soak, and still be quietly
 *  destroying data that is not being read often enough to notice. That failure mode is invisible to
 *  every test in this project except one that waits.
 *
 *  WHAT IS MEASURED, AND WHY DIRECTLY
 *  Earlier the chip-select-low time was INFERRED from throughput: bytes divided by MB/s. That is the
 *  payload time and it leaves out the command, the address and the six dummy clocks, which are
 *  fourteen nibbles of chip-select-low that carry no payload at all. So the real figure was always
 *  larger than the one quoted, by roughly 7%, and nobody had measured it.
 *
 *  This times a single burst from the cycle counter -- the actual interval between asserting chip
 *  select and releasing it -- and then adds a retention test long enough to catch the failure the
 *  limit exists to prevent.
 * ======================================================================================== */

#define SPAN (256u * 1024u)
#define BLK  (8u * 1024u)
static uint8_t buf[BLK];
static uint32_t g_burst_len = 96;

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

/* one burst, timed across exactly the chip-select-low interval and nothing else. The bus layer's
 * own read is not used here because it loops over bursts; this has to see one. */
static uint32_t one_burst_cycles(uint32_t a, uint8_t *d, uint32_t n)
{
    bus_out();
    const uint32_t b = qbase();
    const uint32_t t0 = ARM_DWT_CYCCNT;       /* chip select goes low on the next store */
    GPIO9_DR = b;
    put_nib<10>(b, 0xE); put_nib<10>(b, 0xB); addr_out<10>(b, a);
    data_in();
    for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<10>(); GPIO9_DR = b; spin<10>(); }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t hi = get_nib<10>(b);
        d[i] = (uint8_t)((hi << 4) | get_nib<10>(b));
    }
    const uint32_t c = ARM_DWT_CYCCNT - t0;   /* still low; the next store releases it */
    GPIO9_DR = idle;
    bus_out();
    return c;
}

static uint32_t wr_burst_cycles(uint32_t a, const uint8_t *s, uint32_t n)
{
    bus_out();
    const uint32_t b = qbase();
    const uint32_t t0 = ARM_DWT_CYCCNT;
    GPIO9_DR = b;
    put_nib<6>(b, 0x3); put_nib<6>(b, 0x8); addr_out<6>(b, a);
    for (uint32_t i = 0; i < n; i++) { put_nib<6>(b, s[i] >> 4); put_nib<6>(b, s[i] & 0xF); }
    const uint32_t c = ARM_DWT_CYCCNT - t0;
    GPIO9_DR = idle;
    return c;
}

/* full-span traffic at a given burst length, so the error count is not based on one burst */
static uint32_t span_at(uint32_t n, float *rmb)
{
    /* The last burst of a block must be clamped. Without this, a burst length that does not divide
     * the block -- 48, 80, 96, 112 -- reads past the end of buf, and the first version of this test
     * did exactly that and corrupted its own state, producing an error column of 130,000 and
     * 261,000 that said nothing about the memory. The timing figures were unaffected and correct;
     * the pass/fail column was entirely my bug. */
    uint32_t bad = 0, rc = 0;
    for (uint32_t off = 0; off < SPAN; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(off + i);
        for (uint32_t k = 0; k < BLK; k += n) {
            const uint32_t m = (BLK - k < n) ? (BLK - k) : n;
            wr_burst_cycles(off + k, buf + k, m);
        }
    }
    for (uint32_t off = 0; off < SPAN; off += BLK) {
        const uint32_t t = ARM_DWT_CYCCNT;
        for (uint32_t k = 0; k < BLK; k += n) {
            const uint32_t m = (BLK - k < n) ? (BLK - k) : n;
            one_burst_cycles(off + k, buf + k, m);
        }
        rc += ARM_DWT_CYCCNT - t;
        for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat(off + i)) bad++;
    }
    *rmb = (float)SPAN / ((float)rc / (float)F_CPU_ACTUAL) / 1e6f;
    return bad;
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
    Serial.println(F("  chip select low time, measured rather than inferred. Limit 8 us."));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    static const uint32_t N[] = { 32, 48, 64, 80, 96, 112, 128 };
    Serial.println(F("    bytes   read CS-low   write CS-low   MB/s     wrong   verdict"));
    uint32_t best = 32; float best_mb = 0;
    for (unsigned k = 0; k < sizeof(N) / sizeof(N[0]); k++) {
        const uint32_t n = N[k];
        /* one representative burst of each direction, timed exactly */
        for (uint32_t i = 0; i < n; i++) buf[i] = pat(i);
        const float wus = 1e6f * (float)wr_burst_cycles(0, buf, n) / (float)F_CPU_ACTUAL;
        const float rus = 1e6f * (float)one_burst_cycles(0, buf, n) / (float)F_CPU_ACTUAL;

        float mb = 0;
        const uint32_t bad = span_at(n, &mb);

        Serial.print(F("     "));
        if (n < 100) Serial.print(' ');
        Serial.print(n);             Serial.print(F("      "));
        Serial.print(rus, 2);        Serial.print(F(" us       "));
        Serial.print(wus, 2);        Serial.print(F(" us     "));
        Serial.print(mb, 2);        Serial.print(F("    "));
        if (bad < 1000) Serial.print(' ');
        if (bad < 100)  Serial.print(' ');
        if (bad < 10)   Serial.print(' ');
        Serial.print(bad);           Serial.print(F("    "));
        const float worst = rus > wus ? rus : wus;
        if (worst > 8.0f)      Serial.println(F("OVER THE LIMIT"));
        else if (worst > 7.0f) Serial.println(F("thin margin"));
        else                   Serial.println(F("safe"));

        if (bad == 0 && worst < 7.0f && mb > best_mb) { best_mb = mb; best = n; }
    }

    /* and the test the limit exists for. A burst over tCEM reads back perfectly and loses rows that
     * have been sitting, so the only way to see it is to make some bytes sit. */
    Serial.println(F("\n  retention at the driver's 96-byte burst: write, wait, re-read"));
    for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(i);
    for (uint32_t k = 0; k < BLK; k += 96) wr_burst_cycles(k, buf + k, (BLK - k < 96) ? BLK - k : 96);
    uint32_t b0 = 0;
    for (uint32_t k = 0; k < BLK; k += 96) one_burst_cycles(k, buf + k, (BLK - k < 96) ? BLK - k : 96);
    for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat(i)) b0++;
    Serial.print(F("    immediately: ")); Serial.print(b0); Serial.println(F(" wrong"));

    for (int sec = 1; sec <= 3; sec++) {
        delay(10000);
        uint32_t bn = 0;
        for (uint32_t k = 0; k < BLK; k += 96) one_burst_cycles(k, buf + k, (BLK - k < 96) ? BLK - k : 96);
        for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat(i)) bn++;
        Serial.print(F("    after ")); Serial.print(sec * 10);
        Serial.print(F(" s idle: ")); Serial.print(bn); Serial.println(F(" wrong"));
    }

    Serial.println(F("\n--- verdict ---"));
    Serial.print(F("  fastest burst that is clean AND keeps chip select low under 7 us: "));
    Serial.print(best); Serial.print(F(" bytes at "));
    Serial.print(best_mb, 2); Serial.println(F(" MB/s"));
    Serial.println(F("  The driver ships 96. If the measured read CS-low at 96 is past 8 us then"));
    Serial.println(F("  the burst has to shrink, and the throughput table in docs/40 was computed"));
    Serial.println(F("  from payload time only -- which left out fourteen nibbles of command,"));
    Serial.println(F("  address and dummy clocks that are chip-select-low too."));
    Serial.println(F("  A zero in the retention rows is the only evidence that matters here: a"));
    Serial.println(F("  burst past the limit returns correct bytes and rots the ones left alone."));

    Serial.println(F("\n=== repeating ==="));
}

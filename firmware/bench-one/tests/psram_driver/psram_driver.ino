/* ===========================================================================================
 *  psram_driver -- the tuned driver, and a full 8 MB verification
 * ===========================================================================================
 *
 *  THE THREE SETTINGS, EACH MEASURED RATHER THAN CHOSEN
 *  ---------------------------------------------------
 *  WRITE AND READ WANT DIFFERENT TIMING. Holding writes at 8 no-ops and sweeping reads, reads break
 *  below 8. Holding reads at 8 and sweeping writes, writes stayed clean all the way down to 5, the
 *  bottom of the sweep. The write path only drives; the read path also has to get a value back out
 *  of GPIO9_PSR between the edges, and that costs bus latency the write never pays. One shared
 *  number throttles writes to the read's limit and gives away a quarter of the write rate.
 *
 *      writes   5 no-ops   21.85 MB/s
 *      reads    8 no-ops   13.43 MB/s
 *
 *  BURSTS PAY UNTIL REFRESH STOPS THEM. Every burst re-sends a command and a 24-bit address, and a
 *  read adds six dummy clocks: fourteen nibbles of overhead whatever the payload. At 32 bytes that
 *  is 22% of a read thrown away.
 *
 *      16 bytes   11.70 MB/s   1.37 us
 *      32         13.43        2.38
 *      64         14.50        4.41
 *      96         14.87        6.46
 *     128         15.09        8.48   <-- over the limit
 *
 *  Chip select is low for the whole burst and these chips only refresh while it is high, for at most
 *  8 microseconds. 128 bytes is measurably faster and is not usable: it would read correctly today
 *  and lose data that has been sitting a while. 96 is the last size with real margin.
 *
 *
 *  WHAT THIS SAYS ABOUT THE PERFBOARD BUILD
 *  ----------------------------------------
 *  The breadboard needs 37.2 ns per nibble, which is 26.9 MHz. FlexSPI cannot be clocked below
 *  49.5 MHz, or 20.2 ns. So the hardware controller is 1.85x too fast for this wiring and no setting
 *  of it can ever work -- which is why every DLL and clock sweep came back empty.
 *
 *  That makes the target for the perfboard a number rather than a hope: get under 20.2 ns per nibble
 *  and the controller becomes usable, with memory mapping and roughly 33 MB/s, instead of a hand
 *  driven bus at 15. The perfboard has to be about twice as good as the breadboard, and that is
 *  mostly about giving the four data lines a ground to return along.
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

/* measured, not chosen. See the header. */
#define SW     5
#define SR     8
#define BURST  96

static uint32_t idle;

static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }
static inline uint32_t qbase(void) { return idle & ~B_SS0 & ~B_CLK & ~B_DATA; }
static inline uint32_t sbase(void) { return qbase() | (1u << 28) | (1u << 29); }

template <int S> struct Nop { static inline void go() { __asm__ volatile("nop"); Nop<S - 1>::go(); } };
template <>      struct Nop<0> { static inline void go() { } };
template <int S> static inline void spin(void) { Nop<S>::go(); }

/* Three writes per nibble. Two fails even when slow: the falling edge and the data change must not
 * land together. The setup write needs no wait, since nothing sees it until the clock rises.
 * And every phase must END with the clock low -- leaving it high makes the next phase's first
 * "rise" no edge at all, and the whole reply arrives shifted by one bit. */
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
    bus_out();
    const uint32_t b = sbase();
    GPIO9_DR = b;  s_byte<SR>(b, c);  GPIO9_DR = idle;
    delayMicroseconds(5);
}

static void cmd_quad(uint8_t c)
{
    bus_out();
    const uint32_t b = qbase();
    GPIO9_DR = b;
    put_nib<SR>(b, (uint8_t)(c >> 4));
    put_nib<SR>(b, (uint8_t)(c & 0xF));
    GPIO9_DR = idle;
    delayMicroseconds(5);
}

template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

void psram_write(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out();
        const uint32_t b = qbase();
        GPIO9_DR = b;
        put_nib<SW>(b, 0x3); put_nib<SW>(b, 0x8);
        addr_out<SW>(b, a);
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {
            put_nib<SW>(b, s[i+0] >> 4); put_nib<SW>(b, s[i+0] & 0xF);
            put_nib<SW>(b, s[i+1] >> 4); put_nib<SW>(b, s[i+1] & 0xF);
            put_nib<SW>(b, s[i+2] >> 4); put_nib<SW>(b, s[i+2] & 0xF);
            put_nib<SW>(b, s[i+3] >> 4); put_nib<SW>(b, s[i+3] & 0xF);
            put_nib<SW>(b, s[i+4] >> 4); put_nib<SW>(b, s[i+4] & 0xF);
            put_nib<SW>(b, s[i+5] >> 4); put_nib<SW>(b, s[i+5] & 0xF);
            put_nib<SW>(b, s[i+6] >> 4); put_nib<SW>(b, s[i+6] & 0xF);
            put_nib<SW>(b, s[i+7] >> 4); put_nib<SW>(b, s[i+7] & 0xF);
        }
        for (; i < n; i++) { put_nib<SW>(b, s[i] >> 4); put_nib<SW>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

void psram_read(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out();
        const uint32_t b = qbase();
        GPIO9_DR = b;
        put_nib<SR>(b, 0xE); put_nib<SR>(b, 0xB);
        addr_out<SR>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<SR>(); GPIO9_DR = b; spin<SR>(); }
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {
            for (int k = 0; k < 8; k++) {
                const uint8_t hi = get_nib<SR>(b);
                d[i + k] = (uint8_t)((hi << 4) | get_nib<SR>(b));
            }
        }
        for (; i < n; i++) {
            const uint8_t hi = get_nib<SR>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<SR>(b));
        }
        GPIO9_DR = idle;
        bus_out();
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
    cmd_quad(0xF5);
    cmd_single(0x66);
    cmd_single(0x99);
    delay(2);
    cmd_single(0x35);
    delayMicroseconds(50);
}

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

#define CH (16u * 1024u)
static uint8_t buf[CH];
#define TOTAL (8u * 1024u * 1024u)

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1);
    bus_out();
    pick(7);
}

void loop()
{
    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.print(F("  tuned driver: write "));  Serial.print(SW);
    Serial.print(F(" nops, read "));            Serial.print(SR);
    Serial.print(F(" nops, "));                 Serial.print(BURST);
    Serial.println(F("-byte bursts"));
    Serial.println(F("=============================================================="));

    pick(7);
    enter_quad();

    /* ---- the whole 8 MB, not a sample ------------------------------------------------------ */
    Serial.println(F("\n[1] writing and verifying all 8 MB"));
    uint64_t tw = 0, tr = 0;
    uint32_t bad = 0;

    for (uint32_t off = 0; off < TOTAL; off += CH) {
        for (uint32_t i = 0; i < CH; i++) buf[i] = pat(off + i);
        const uint32_t t0 = micros();
        psram_write(off, buf, CH);
        tw += micros() - t0;
    }
    for (uint32_t off = 0; off < TOTAL; off += CH) {
        const uint32_t t0 = micros();
        psram_read(off, buf, CH);
        tr += micros() - t0;
        for (uint32_t i = 0; i < CH; i++) if (buf[i] != pat(off + i)) bad++;
    }

    Serial.print(F("    wrong bytes: "));
    Serial.print(bad);
    Serial.print(F(" of "));
    Serial.println(TOTAL);
    Serial.print(F("    write "));
    Serial.print(TOTAL / (float)tw, 2);
    Serial.print(F(" MB/s   read "));
    Serial.print(TOTAL / (float)tr, 2);
    Serial.println(F(" MB/s"));

    if (bad) {
        Serial.println(F("    Not clean over the full range. A short block passing and a long one"));
        Serial.println(F("    failing is the signature of refresh, not of signalling: check the"));
        Serial.println(F("    burst against 8 us."));
    }

    /* ---- does it still hold after sitting? -------------------------------------------------- *
     * Every test so far wrote and read back immediately, which cannot tell a working memory from
     * one whose refresh has been suspended by an over-long burst. Data that is correct now and gone
     * in a second is the failure this design is most exposed to, so it is worth the wait.          */
    Serial.println(F("\n[2] retention: the same bytes after five seconds of idle"));
    psram_read(0, buf, CH);
    uint32_t bad_before = 0;
    for (uint32_t i = 0; i < CH; i++) if (buf[i] != pat(i)) bad_before++;

    delay(5000);

    psram_read(0, buf, CH);
    uint32_t bad_after = 0;
    for (uint32_t i = 0; i < CH; i++) if (buf[i] != pat(i)) bad_after++;

    Serial.print(F("    before the wait: ")); Serial.print(bad_before);
    Serial.print(F("   after: "));            Serial.println(bad_after);
    Serial.println(bad_after > bad_before
        ? F("    IT ROTS. The burst is holding chip select low past the refresh limit.")
        : F("    it holds. Refresh is surviving the bursts."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

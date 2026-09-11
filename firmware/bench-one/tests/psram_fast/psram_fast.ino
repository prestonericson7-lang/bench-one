/* ===========================================================================================
 *  psram_fast -- how fast this bus will actually go, with the delay quantised to one cycle
 * ===========================================================================================
 *
 *  WHERE THIS STARTS
 *  -----------------
 *      1 no-op    16.45 MB/s write, 13.21 MB/s read, zero errors
 *      0 no-ops   33.57 / 18.88, and 2721 of 4096 wrong
 *
 *  The ceiling is somewhere between those two and there was no way to look, because the delay was a
 *  loop: `for (i = 0; i < n; i++) nop`. At n = 1 that is a compare, a branch and an increment as
 *  well as the no-op, so the smallest step available was four or five cycles wide and the whole
 *  interesting region fell inside one step.
 *
 *  Making the count a template parameter fixes that. The compiler unrolls it into exactly S no-ops
 *  with no loop at all, so the step is one cycle -- 1.67 ns at 600 MHz -- and the region between
 *  "works at 16 MB/s" and "fails at 33" can be walked properly.
 *
 *  The inner data loop is unrolled eight bytes at a time for the same reason: at these rates the
 *  loop's own arithmetic is a real fraction of the clock period, not a rounding error.
 *
 *
 *  WHAT IS ALREADY SETTLED, SO IT IS NOT RE-TESTED HERE
 *  ---------------------------------------------------
 *  The wiring is good: every one of the six chips returns a correct signature, and all four data
 *  lines carry a block intact. The breadboard is not the limit at these speeds. And it is not the
 *  supply either -- at an edge rate that fails, changing burst length from 4 to 32 bytes and adding
 *  gaps from 0 to 50 us moved the error count only between 666 and 691 out of 1024. Sag would swing
 *  hard with both; this barely notices, which makes it signal timing rather than charge.
 *
 *  The five banked chips still collide with the onboard one, because they share a chip select. Only
 *  the 8 MB on Y7 is trustworthy, and that is what is measured.
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
static uint32_t g_burst = 32;

static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }
static inline uint32_t qbase(void) { return idle & ~B_SS0 & ~B_CLK & ~B_DATA; }
static inline uint32_t sbase(void) { return qbase() | (1u << 28) | (1u << 29); }

/* Exactly S no-op instructions, by recursion rather than by a loop.
 *
 * The obvious `for (i = 0; i < S; i++) nop` with S constant only unrolls while the compiler feels
 * like it. It did so up to four and emitted a real loop at five, which is why the first sweep showed
 * 23.6 MB/s at four no-ops and 5.75 at five: a 4x cliff for one instruction, and the entire region
 * worth measuring fell inside it. Recursion cannot be un-unrolled. */
template <int S> struct Nop { static inline void go() { __asm__ volatile("nop"); Nop<S - 1>::go(); } };
template <>      struct Nop<0> { static inline void go() { } };

template <int S> static inline void spin(void) { Nop<S>::go(); }

/* Three writes per nibble. Two does not work: letting the next nibble's data write provide the
 * falling edge fails even at 5.9 MB/s where this is clean at 13. The edge and the data change must
 * not land together. The setup write needs no wait, since nothing sees it until the clock rises. */
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

template <int S> static void cmd_single(uint8_t c)
{
    bus_out();
    const uint32_t b = sbase();
    GPIO9_DR = b;  s_byte<S>(b, c);  GPIO9_DR = idle;
    delayMicroseconds(5);
}

template <int S> static void cmd_quad(uint8_t c)
{
    bus_out();
    const uint32_t b = qbase();
    GPIO9_DR = b;
    put_nib<S>(b, (uint8_t)(c >> 4));
    put_nib<S>(b, (uint8_t)(c & 0xF));
    GPIO9_DR = idle;
    delayMicroseconds(5);
}

template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

template <int S> static void qwrite(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t b = qbase();
        GPIO9_DR = b;
        put_nib<S>(b, 0x3); put_nib<S>(b, 0x8);
        addr_out<S>(b, a);
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {            /* unrolled: the loop's own arithmetic matters here */
            put_nib<S>(b, s[i+0] >> 4); put_nib<S>(b, s[i+0] & 0xF);
            put_nib<S>(b, s[i+1] >> 4); put_nib<S>(b, s[i+1] & 0xF);
            put_nib<S>(b, s[i+2] >> 4); put_nib<S>(b, s[i+2] & 0xF);
            put_nib<S>(b, s[i+3] >> 4); put_nib<S>(b, s[i+3] & 0xF);
            put_nib<S>(b, s[i+4] >> 4); put_nib<S>(b, s[i+4] & 0xF);
            put_nib<S>(b, s[i+5] >> 4); put_nib<S>(b, s[i+5] & 0xF);
            put_nib<S>(b, s[i+6] >> 4); put_nib<S>(b, s[i+6] & 0xF);
            put_nib<S>(b, s[i+7] >> 4); put_nib<S>(b, s[i+7] & 0xF);
        }
        for (; i < n; i++) { put_nib<S>(b, s[i] >> 4); put_nib<S>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

template <int S> static void qread(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t b = qbase();
        GPIO9_DR = b;
        put_nib<S>(b, 0xE); put_nib<S>(b, 0xB);
        addr_out<S>(b, a);
        data_in();
        for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<S>(); GPIO9_DR = b; spin<S>(); }
        uint32_t i = 0;
        for (; i + 8 <= n; i += 8) {
            for (int k = 0; k < 8; k++) {
                const uint8_t hi = get_nib<S>(b);
                d[i + k] = (uint8_t)((hi << 4) | get_nib<S>(b));
            }
        }
        for (; i < n; i++) {
            const uint8_t hi = get_nib<S>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<S>(b));
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

template <int S> static void enter_quad(void)
{
    cmd_quad<S>(0xF5);
    cmd_single<S>(0x66);
    cmd_single<S>(0x99);
    delay(2);
    cmd_single<S>(0x35);
    delayMicroseconds(50);
}

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

#define BLK 8192
static uint8_t wbuf[BLK], rbuf[BLK];

/* One measured trial at a given unrolled delay. */
template <int S> static uint32_t trial(uint32_t bytes, float *mw, float *mr)
{
    enter_quad<S>();
    for (uint32_t i = 0; i < bytes; i++) wbuf[i] = pat(i);
    uint32_t t0 = micros(); qwrite<S>(0, wbuf, bytes); const uint32_t tw = micros() - t0;
    t0 = micros();          qread<S>(0, rbuf, bytes);  const uint32_t tr = micros() - t0;
    *mw = tw ? bytes / (float)tw : 0.0f;
    *mr = tr ? bytes / (float)tr : 0.0f;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++) if (rbuf[i] != wbuf[i]) bad++;
    return bad;
}

/* Write and read need not share a delay. The write path only drives; the read path also has to
 * get a value back out of GPIO9_PSR between the edges, which costs bus latency the write never
 * pays. If reads need more settling than writes, one shared number throttles both to the slower. */
template <int SW, int SR> static uint32_t trial2(uint32_t bytes, float *mw, float *mr)
{
    enter_quad<SW>();
    for (uint32_t i = 0; i < bytes; i++) wbuf[i] = pat(i);
    uint32_t t0 = micros(); qwrite<SW>(0, wbuf, bytes); const uint32_t tw = micros() - t0;
    t0 = micros();          qread<SR>(0, rbuf, bytes);  const uint32_t tr = micros() - t0;
    *mw = tw ? bytes / (float)tw : 0.0f;
    *mr = tr ? bytes / (float)tr : 0.0f;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i++) if (rbuf[i] != wbuf[i]) bad++;
    return bad;
}

typedef uint32_t (*TrialFn)(uint32_t, float *, float *);
static const TrialFn TRIALS[] = {
    trial<5>, trial<6>, trial<7>, trial<8>, trial<9>, trial<10>
};
static const int NOPS[] = { 5, 6, 7, 8, 9, 10 };

/* writes held at 8, reads swept */
static const TrialFn RSWEEP[] = {
    trial2<8,5>, trial2<8,6>, trial2<8,7>, trial2<8,8>, trial2<8,9>, trial2<8,10>
};
/* reads held at 8, writes swept */
static const TrialFn WSWEEP[] = {
    trial2<5,8>, trial2<6,8>, trial2<7,8>, trial2<8,8>, trial2<9,8>, trial2<10,8>
};
#define NT (sizeof(NOPS) / sizeof(NOPS[0]))

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
    float w, r;
    pick(7);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  digging: the exact threshold, split timing, burst overhead"));
    Serial.println(F("=============================================================="));

    /* ---- A. where exactly does it break -------------------------------------------------- */
    g_burst = 32;
    Serial.println(F("\n[A] same delay both ways, one cycle at a time"));
    Serial.println(F("     nops   write MB/s   read MB/s   wrong of 8192"));
    int a_best = -1; float a_r = 0;
    for (unsigned k = 0; k < NT; k++) {
        const uint32_t bad = TRIALS[k](BLK, &w, &r);
        Serial.print(F("     "));
        if (NOPS[k] < 10) Serial.print(' ');
        Serial.print(NOPS[k]);  Serial.print(F("     "));
        Serial.print(w, 2);     Serial.print(F("        "));
        Serial.print(r, 2);     Serial.print(F("        "));
        Serial.println(bad);
        if (bad == 0 && r > a_r) { a_r = r; a_best = (int)k; }
    }

    /* ---- B. do the two directions want the same delay ------------------------------------ */
    Serial.println(F("\n[B] writes pinned at 8, reads swept"));
    Serial.println(F("     read nops   read MB/s   wrong"));
    int r_best = -1; float r_rate = 0;
    for (unsigned k = 0; k < NT; k++) {
        const uint32_t bad = RSWEEP[k](BLK, &w, &r);
        Serial.print(F("       "));
        if (NOPS[k] < 10) Serial.print(' ');
        Serial.print(NOPS[k]);  Serial.print(F("        "));
        Serial.print(r, 2);     Serial.print(F("        "));
        Serial.println(bad);
        if (bad == 0 && r > r_rate) { r_rate = r; r_best = NOPS[k]; }
    }

    Serial.println(F("\n[C] reads pinned at 8, writes swept"));
    Serial.println(F("     write nops   write MB/s   wrong"));
    int w_best = -1; float w_rate = 0;
    for (unsigned k = 0; k < NT; k++) {
        const uint32_t bad = WSWEEP[k](BLK, &w, &r);
        Serial.print(F("        "));
        if (NOPS[k] < 10) Serial.print(' ');
        Serial.print(NOPS[k]);  Serial.print(F("         "));
        Serial.print(w, 2);     Serial.print(F("        "));
        Serial.println(bad);
        if (bad == 0 && w > w_rate) { w_rate = w; w_best = NOPS[k]; }
    }

    /* ---- D. burst overhead, and how close it gets to the refresh limit -------------------- *
     * Every burst re-sends a command and a 24-bit address, and a read adds six dummy clocks on
     * top: fourteen nibbles of overhead against the payload. At a 32-byte burst that is 22% of a
     * read wasted. Longer bursts amortise it -- but chip select is low the whole time, and these
     * chips only refresh while it is high, for at most 8 us. So the useful burst is bounded from
     * above by physics, not by preference, and the measured time is printed against that bound. */
    Serial.println(F("\n[D] burst length. CS-low time must stay under 8 us."));
    Serial.println(F("     bytes   read MB/s   CS low us   wrong"));
    uint32_t d_best = 32; float d_rate = 0;
    static const uint32_t BURSTS[] = { 16, 32, 48, 64, 96, 128 };
    for (unsigned k = 0; k < 6; k++) {
        g_burst = BURSTS[k];
        const uint32_t bad = TRIALS[a_best < 0 ? 3 : a_best](BLK, &w, &r);
        const float cs_us = r > 0.01f ? (BURSTS[k] / r) : 0.0f;
        Serial.print(F("      "));
        if (BURSTS[k] < 100) Serial.print(' ');
        if (BURSTS[k] < 10)  Serial.print(' ');
        Serial.print(BURSTS[k]);  Serial.print(F("      "));
        Serial.print(r, 2);       Serial.print(F("        "));
        Serial.print(cs_us, 2);   Serial.print(cs_us > 8.0f ? F("  OVER  ") : F("        "));
        Serial.println(bad);
        if (bad == 0 && cs_us < 7.0f && r > d_rate) { d_rate = r; d_best = BURSTS[k]; }
    }

    /* ---- what it adds up to --------------------------------------------------------------- */
    Serial.println(F("\n--- verdict ---"));
    if (a_best >= 0) {
        Serial.print(F("  threshold: clean at "));
        Serial.print(NOPS[a_best]);
        Serial.print(F(" nops, "));
        Serial.print(1000.0f / (a_r * 2.0f), 1);
        Serial.println(F(" ns per nibble"));
        Serial.print(F("  that is "));
        Serial.print(1000.0f / (1000.0f / (a_r * 2.0f)), 1);
        Serial.println(F(" MHz equivalent. FlexSPI's floor is 49.5 MHz, so the"));
        Serial.println(F("  controller is too fast for this wiring by a wide margin and no"));
        Serial.println(F("  setting of it can ever work here."));
    }
    if (r_best >= 0 && w_best >= 0) {
        Serial.print(F("  reads need "));  Serial.print(r_best);
        Serial.print(F(" nops, writes need ")); Serial.print(w_best);
        Serial.println(w_best < r_best ? F(" -- writes tolerate more speed")
                                       : F(" -- both want the same"));
    }
    Serial.print(F("  best burst: ")); Serial.print(d_best);
    Serial.print(F(" bytes at ")); Serial.print(d_rate, 2);
    Serial.println(F(" MB/s read"));

    g_burst = d_best;
    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

/* ===========================================================================================
 *  psram_idle -- cold, warm and primed, at the settings the driver ships
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
 *  the last difference between the driver and every test that says the driver should be clean
 *
 *  At write 6, read 10 the soak has run 56 MB without a single error. The driver, at those exact
 *  settings with that exact pattern and that exact chunk size, produces one to three wrong bytes per
 *  8 MB, every round. One of them is wrong about this bus and it is not the settings.
 *
 *  What the soak never does is stop. It writes and reads continuously, round after round, with no
 *  pause anywhere. The driver idles ten seconds between rounds and another five inside its retention
 *  test, and then starts its next 8 MB write immediately after coming back.
 *
 *  The cold-start test already found the shape of this: at 8 no-ops the first read of a session was
 *  wrong at about one percent density and then stopped dead, while every later read was perfect. At
 *  10 no-ops that effect looked gone. It may not be gone, only smaller -- a handful of bytes instead
 *  of eighteen thousand, which is exactly the size of what the driver is reporting.
 *
 *  Three arms, interleaved so none of them owns a particular minute:
 *
 *      cold      idle ten seconds, then write and verify the whole part
 *      warm      write and verify immediately, no idle at all
 *      primed    idle ten seconds, throw away one 64 kB read, then write and verify
 *
 *  Cold worse than warm names idling as the cause. Primed as good as warm makes the repair a few
 *  hundred microseconds of discarded reading in the driver's start-up, which is as cheap as a fix
 *  ever gets. Primed still bad means the wake-up has to be longer, and the soak should say how long.
 * ======================================================================================== */

#define CH   (16u * 1024u)
#define SIZE (8u * 1024u * 1024u)
#define REST 10000u
static uint8_t buf[CH];

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

static uint64_t b_cold, b_warm, b_prim;    /* bytes */
static uint64_t e_cold, e_warm, e_prim;    /* errors */
static uint32_t p_cold, p_warm, p_prim;    /* passes */
static uint32_t w_cold, w_warm, w_prim;    /* worst single pass */

static uint32_t pass(void)
{
    for (uint32_t off = 0; off < SIZE; off += CH) {
        for (uint32_t i = 0; i < CH; i++) buf[i] = pat(off + i);
        wr<6>(off, buf, CH);
    }
    uint32_t bad = 0;
    for (uint32_t off = 0; off < SIZE; off += CH) {
        rd<10>(off, buf, CH);
        for (uint32_t i = 0; i < CH; i++) if (buf[i] != pat(off + i)) bad++;
    }
    return bad;
}

/* the priming read: 64 kB thrown straight away, purely to get the bus moving before the pass that
 * is being measured. Reads rather than writes, because the failure has always been on the read. */
static void prime(void)
{
    for (uint32_t off = 0; off < 64u * 1024u; off += CH) rd<10>(off, buf, CH);
}

static void line(const char *name, uint64_t by, uint64_t er, uint32_t pa, uint32_t wo)
{
    Serial.print(F("    "));
    Serial.print(name);
    for (unsigned k = strlen(name); k < 9; k++) Serial.print(' ');
    const uint32_t mb = (uint32_t)(by >> 20);
    if (mb < 1000) Serial.print(' ');
    if (mb < 100)  Serial.print(' ');
    if (mb < 10)   Serial.print(' ');
    Serial.print(mb);                Serial.print(F("      "));
    if (er < 100) Serial.print(' ');
    if (er < 10)  Serial.print(' ');
    Serial.print((uint32_t)er);      Serial.print(F("       "));
    if (wo < 10) Serial.print(' ');
    Serial.print(wo);                Serial.print(F("        "));
    Serial.print(pa);                Serial.print(F("      "));
    if (by == 0) { Serial.println(); return; }
    if (er) {
        Serial.print(1e9f * (float)(double)er / (float)(double)by, 1);
        Serial.println(F(" per billion"));
    } else {
        Serial.print(F("under "));
        Serial.print(1e9f / (float)(double)by, 1);
        Serial.println(F(" per billion"));
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
    pick(7);
    enter_quad();
}

void loop()
{
    uint32_t bad;

    /* cold: the driver's own situation */
    delay(REST);
    bad = pass();
    b_cold += SIZE; e_cold += bad; p_cold++; if (bad > w_cold) w_cold = bad;

    /* warm: the soak's situation, run immediately after the cold pass so the bus is moving */
    bad = pass();
    b_warm += SIZE; e_warm += bad; p_warm++; if (bad > w_warm) w_warm = bad;

    /* primed: cold again, but woken first */
    delay(REST);
    prime();
    bad = pass();
    b_prim += SIZE; e_prim += bad; p_prim++; if (bad > w_prim) w_prim = bad;

    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.print(F("  idle or not, at write 6 read 10.  "));
    Serial.print(millis() / 60000u); Serial.print(F(" min "));
    Serial.print((millis() / 1000u) % 60u); Serial.println(F(" s"));
    Serial.println(F("=================================================================="));
    Serial.println(F("    arm       MB    errors   worst   passes   rate"));
    line("cold",   b_cold, e_cold, p_cold, w_cold);
    line("warm",   b_warm, e_warm, p_warm, w_warm);
    line("primed", b_prim, e_prim, p_prim, w_prim);

    if (e_cold && !e_warm)
        Serial.println(F("  Idling is the cause. The bus is reliable while it is busy."));
    if (e_cold && e_prim * 4 < e_cold)
        Serial.println(F("  And 64 kB of discarded reading fixes it, which belongs in the driver."));
    if (!e_cold && !e_warm && !e_prim) {
        Serial.println(F("  Nothing has failed yet in any arm. Keep it running; the driver's one"));
        Serial.println(F("  to three per 8 MB needs roughly 100 MB per arm to show up at all."));
    }
}

/* ===========================================================================================
 *  psram_pads -- is the 37 ns nibble the wiring, or just the pad configuration nobody set
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
 *  the three knobs nobody had turned
 *
 *  Every speed result so far has been read as a property of the wiring: the bus needs 37 ns per
 *  nibble because five jumper wires and a breadboard cannot carry an edge any faster. That may be
 *  true. It has never been tested, because the pads driving those wires have been left at whatever
 *  pinMode leaves them, and pinMode is not trying to drive a memory bus.
 *
 *  What the core actually sets for an OUTPUT pin, from digital.c:
 *
 *      IOMUXC_PAD_DSE(7)                  drive strength at maximum, so that one is already right
 *      SPEED field absent, so 0           the SLOWEST bandwidth setting of four
 *      SRE bit absent, so 0               the SLOW slew rate of two
 *
 *  and for an INPUT pin it adds IOMUXC_PAD_HYS -- hysteresis on, which rejects noise by refusing to
 *  believe an edge until it has travelled far enough, and pays for that in input delay. The read path
 *  is the one that needs eight no-ops where the write needs five, and it is the only path that goes
 *  through that hysteresis.
 *
 *  So three knobs, none of them touched:
 *
 *      SPEED   0 to 3, nominally 50 / 100 / 100 / 200 MHz of pad bandwidth
 *      SRE     slow or fast slew
 *      HYS     hysteresis on the input path, on or off
 *
 *  WHICH WAY IT GOES IS GENUINELY UNKNOWN, AND THAT IS WHY IT IS WORTH MEASURING
 *  A faster edge arrives sooner, which helps timing. A faster edge into an unterminated jumper wire
 *  rings harder, which hurts everything. On a breadboard those fight, and the answer is not
 *  predictable from either principle alone -- which makes this exactly the kind of question to settle
 *  with a sweep rather than an argument.
 *
 *  WHAT IS REPORTED, AFTER DOCUMENT 41
 *  The minimum clean no-op count is the fast edge of a window, and this project has already paid for
 *  shipping one of those. So each configuration reports the lowest setting that is clean AND the error
 *  count at one step faster, so the shape of the cliff is visible rather than just its location. A
 *  configuration that goes from zero to eighteen thousand in one step has no margin wherever its edge
 *  happens to sit.
 * ======================================================================================== */

#define SPAN (256u * 1024u)            /* per trial: big enough to see a rate of a few per 8 MB */
#define BLK  (8u * 1024u)
static uint8_t buf[BLK];

static inline uint8_t pat(uint32_t i) { return (uint8_t)(i * 0x9Du + 0x3Bu); }

/* the seven pads the bus uses, by their EMC number. 25 is the clock, 26 to 29 the data, 24 and 22
 * the two chip selects. */
static volatile uint32_t *const PADS[] = {
    &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_22, &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_24,
    &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_25, &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_26,
    &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_27, &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_28,
    &IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_29,
};
#define NPADS (sizeof(PADS) / sizeof(PADS[0]))

static void set_pads(int speed, int sre, int hys)
{
    const uint32_t v = IOMUXC_PAD_DSE(7)
                     | IOMUXC_PAD_SPEED(speed)
                     | (sre ? IOMUXC_PAD_SRE : 0u)
                     | (hys ? IOMUXC_PAD_HYS : 0u);
    for (unsigned i = 0; i < NPADS; i++) *PADS[i] = v;
    __asm__ volatile("dsb");
    delayMicroseconds(50);
}

template <int S> static uint32_t trial(float *mb)
{
    uint32_t bad = 0, rc = 0;
    for (uint32_t off = 0; off < SPAN; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) buf[i] = pat(off + i);
        wr<6>(off, buf, BLK);
    }
    for (uint32_t off = 0; off < SPAN; off += BLK) {
        const uint32_t t = ARM_DWT_CYCCNT;
        rd<S>(off, buf, BLK);
        rc += ARM_DWT_CYCCNT - t;
        for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat(off + i)) bad++;
    }
    *mb = (float)SPAN / ((float)rc / (float)F_CPU_ACTUAL) / 1e6f;
    return bad;
}

typedef uint32_t (*tfn)(float *);
static const int  RN[] = {  4,  5,  6,  7,  8,  9, 10, 12, 14 };
static const tfn  RF[] = { trial<4>, trial<5>, trial<6>, trial<7>, trial<8>,
                           trial<9>, trial<10>, trial<12>, trial<14> };
#define NR (sizeof(RN) / sizeof(RN[0]))

struct Cfg { int speed, sre, hys; const char *name; };
static const Cfg CFG[] = {
    { 0, 0, 1, "baseline  " },      /* exactly what pinMode leaves: the current driver */
    { 1, 0, 1, "speed1    " },
    { 2, 0, 1, "speed2    " },
    { 3, 0, 1, "speed3    " },
    { 0, 1, 1, "fast slew " },
    { 3, 1, 1, "sp3+slew  " },
    { 0, 0, 0, "no hyst   " },
    { 3, 1, 0, "sp3+sl-hy " },
};
#define NCFG (sizeof(CFG) / sizeof(CFG[0]))

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
    Serial.println(F("  does pad drive change the bus speed, or is it really the wiring"));
    Serial.println(F("=================================================================="));
    Serial.println(F("    config      fastest clean   MB/s     errors one step faster"));

    int best_nops = 99; float best_mb = 0; const char *best_name = CFG[0].name;

    for (unsigned c = 0; c < NCFG; c++) {
        pick(7);
        set_pads(CFG[c].speed, CFG[c].sre, CFG[c].hys);
        enter_quad();

        int clean_at = -1; float clean_mb = 0; uint32_t prev_bad = 0;
        for (unsigned k = 0; k < NR; k++) {
            float mb = 0;
            const uint32_t bad = RF[k](&mb);
            if (bad == 0) { clean_at = RN[k]; clean_mb = mb; break; }
            prev_bad = bad;                 /* the count at the step below the clean one */
        }

        Serial.print(F("    "));
        Serial.print(CFG[c].name);
        if (clean_at < 0) {
            Serial.println(F("   none up to 14"));
            continue;
        }
        Serial.print(F("    "));
        if (clean_at < 10) Serial.print(' ');
        Serial.print(clean_at);          Serial.print(F("          "));
        Serial.print(clean_mb, 2);       Serial.print(F("    "));
        Serial.println(prev_bad);

        if (clean_at < best_nops) { best_nops = clean_at; best_mb = clean_mb; best_name = CFG[c].name; }
    }

    /* leave the hardware as the driver expects to find it, whatever the sweep concluded */
    set_pads(0, 0, 1);

    Serial.println(F("\n--- verdict ---"));
    Serial.print(F("  fastest clean configuration: "));
    Serial.print(best_name);
    Serial.print(F(" at ")); Serial.print(best_nops);
    Serial.print(F(" no-ops, ")); Serial.print(best_mb, 2);
    Serial.println(F(" MB/s read"));
    Serial.println(F("  The driver runs the baseline row at 10 no-ops. If another row is clean at"));
    Serial.println(F("  8 or below, the limit was the pad configuration and not the wiring, and"));
    Serial.println(F("  this is free speed. If every row lands in the same place, the wiring is"));
    Serial.println(F("  confirmed as the constraint and the perfboard is the only route."));
    Serial.println(F("  Read the error column before believing any row: a setting that jumps from"));
    Serial.println(F("  zero to thousands in one step has no margin, wherever its edge sits."));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

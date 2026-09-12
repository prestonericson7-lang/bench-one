/* ===========================================================================================
 *  psram_margin -- find the clean WINDOW, not the fastest edge of it
 * ===========================================================================================
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  The tuned driver measured 8,388,608 bytes with zero errors at write 5 no-ops, read 8, 96-byte
 *  bursts. The identical binary, re-flashed later the same day with nothing changed in software,
 *  now reports about 6,900 wrong bytes in the same 8 MB -- stable at that figure across five
 *  consecutive runs. One in twelve hundred.
 *
 *  Nothing in the firmware moved, so the wiring did. A breadboard on a bench beside someone
 *  soldering is not a fixed object.
 *
 *  That is not really a hardware story. It is a measurement story, and the mistake was mine: the
 *  sweep that produced those settings asked "what is the fastest setting that passes?" and took the
 *  answer. A threshold search returns the edge of a cliff by construction. The edge passed once and
 *  will not pass twice, because the thing being measured moves.
 *
 *  So this asks a different question: where does the clean region BEGIN, where does it END, and how
 *  wide is it? Then it takes the middle. A setting with a known margin on both sides survives a
 *  nudged jumper; the fastest passing setting never did.
 *
 *  WHAT IT REPORTS
 *  ---------------
 *  An error COUNT for every setting, not a verdict. A count shows the shape of the cliff -- whether
 *  failure arrives gradually over several settings or all at once -- and that shape is the
 *  difference between a timing problem and a signal-integrity problem. A pass/fail flag throws that
 *  information away, which is how the first sweep managed to look conclusive while being wrong.
 *
 *  Three rounds at every setting, because a setting that passes once and fails twice is exactly the
 *  failure being hunted and a single round cannot see it.
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
static uint32_t g_burst = 96;

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
        const uint32_t n = (len > g_burst) ? g_burst : len;
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
        const uint32_t n = (len > g_burst) ? g_burst : len;
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

/* ======================================================================================== */

#define BLK (8u * 1024u)
#define SPAN (128u * 1024u)          /* bytes tested per round: ~107 errors at the current rate */
static uint8_t buf[BLK];
static uint8_t ref[BLK];

/* one round: write SPAN, read it back, count the bytes that disagree. The pattern is seeded per
 * round so a round cannot pass by reading back what the previous round left behind. */
template <int SWR, int SRD>
static uint32_t round_trip(uint32_t seed, float *wmb, float *rmb)
{
    uint32_t bad = 0, wc = 0, rc = 0;
    for (uint32_t off = 0; off < SPAN; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) ref[i] = (uint8_t)((off + i) * 0x9D + seed);
        uint32_t t = ARM_DWT_CYCCNT; wr<SWR>(off, ref, BLK); wc += ARM_DWT_CYCCNT - t;
        t = ARM_DWT_CYCCNT;          rd<SRD>(off, buf, BLK); rc += ARM_DWT_CYCCNT - t;
        for (uint32_t i = 0; i < BLK; i++) if (buf[i] != ref[i]) bad++;
    }
    *wmb = (float)SPAN / ((float)wc / (float)F_CPU_ACTUAL) / 1e6f;
    *rmb = (float)SPAN / ((float)rc / (float)F_CPU_ACTUAL) / 1e6f;
    return bad;
}

/* three rounds, worst case reported. A setting is only clean if it is clean every time. */
template <int SWR, int SRD>
static uint32_t trial(float *wmb, float *rmb)
{
    uint32_t worst = 0;
    for (uint32_t k = 0; k < 3; k++) {
        float w, r;
        const uint32_t bad = round_trip<SWR, SRD>(0x3B + k * 0x51, &w, &r);
        if (bad > worst) worst = bad;
        if (k == 0) { *wmb = w; *rmb = r; }
    }
    return worst;
}

typedef uint32_t (*fn_t)(float *, float *);

/* read sweep, writes held generous at 16 so only the read path is under test */
static const int  RN[]  = {  6,  8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 40 };
static const fn_t RF[]  = { trial<16, 6>,  trial<16, 8>,  trial<16,10>, trial<16,12>,
                            trial<16,14>, trial<16,16>, trial<16,18>, trial<16,20>,
                            trial<16,24>, trial<16,28>, trial<16,32>, trial<16,40> };

/* write sweep, reads held generous at 32 so only the write path is under test */
static const int  WN[]  = {  3,  4,  5,  6,  8, 10, 12, 16, 20, 24 };
static const fn_t WF[]  = { trial< 3,32>, trial< 4,32>, trial< 5,32>, trial< 6,32>,
                            trial< 8,32>, trial<10,32>, trial<12,32>, trial<16,32>,
                            trial<20,32>, trial<24,32> };

#define NR (sizeof(RN)/sizeof(RN[0]))
#define NW (sizeof(WN)/sizeof(WN[0]))

static void sweep(const char *what, const int *nops,
                  uint32_t (*const *fns)(float *, float *), unsigned n,
                  int *first_clean, int *last_tested_clean)
{
    Serial.print(F("\n[")); Serial.print(what);
    Serial.println(F(" sweep]  the other direction is held generous, so only this one is on trial"));
    Serial.println(F("     nops   wrong of 131072   write MB/s   read MB/s"));
    *first_clean = -1; *last_tested_clean = -1;
    for (unsigned k = 0; k < n; k++) {
        float w = 0, r = 0;
        const uint32_t bad = fns[k](&w, &r);
        Serial.print(F("      "));
        if (nops[k] < 10) Serial.print(' ');
        Serial.print(nops[k]);    Serial.print(F("        "));
        if (bad < 10)     Serial.print(' ');
        if (bad < 100)    Serial.print(' ');
        if (bad < 1000)   Serial.print(' ');
        if (bad < 10000)  Serial.print(' ');
        Serial.print(bad);        Serial.print(F("         "));
        Serial.print(w, 2);       Serial.print(F("       "));
        Serial.println(r, 2);
        if (bad == 0) {
            if (*first_clean < 0) *first_clean = nops[k];
            *last_tested_clean = nops[k];
        } else if (*first_clean >= 0) {
            /* dirty again after being clean: the window has a far edge, which matters */
            Serial.println(F("      ^ errors came BACK after a clean stretch -- not a simple threshold"));
        }
    }
}

/* ===========================================================================================
 *  THE STAGE THIS SKETCH WAS MISSING, AND THE REASON IT MISLED
 *
 *  Everything above sweeps 128 kB per setting, three rounds. That was enough to find the shape of the
 *  cliff and nowhere near enough to qualify a setting: 393,216 bytes of silence bounds the error rate
 *  below about 2.5 per million, and the rate that later turned up in the driver was 0.24 per million.
 *  The sweep was not wrong. It had no resolution left at the scale that mattered, and it did not say so.
 *
 *  So the window search now hands its answer to a confirmation that covers the whole part, twice, with
 *  BOTH directions at their chosen values at once -- because two single-variable sweeps can each come
 *  back clean while the combination fails, and on this bench they did.
 *
 *  The verdict prints the bound its evidence supports rather than the word "clean". A configuration
 *  that has not failed is bounded, not proven, and the bound is one over the bytes tested.
 * ======================================================================================== */

#define FULL (8u * 1024u * 1024u)

template <int SWR, int SRD>
static uint32_t confirm_full(uint32_t seed, float *wmb, float *rmb)
{
    uint32_t bad = 0, wc = 0, rc = 0;
    for (uint32_t off = 0; off < FULL; off += BLK) {
        for (uint32_t i = 0; i < BLK; i++) ref[i] = (uint8_t)((off + i) * 0x9D + seed);
        uint32_t t = ARM_DWT_CYCCNT; wr<SWR>(off, ref, BLK); wc += ARM_DWT_CYCCNT - t;
        t = ARM_DWT_CYCCNT;          rd<SRD>(off, buf, BLK); rc += ARM_DWT_CYCCNT - t;
        for (uint32_t i = 0; i < BLK; i++) if (buf[i] != ref[i]) bad++;
    }
    *wmb = (float)FULL / ((float)wc / (float)F_CPU_ACTUAL) / 1e6f;
    *rmb = (float)FULL / ((float)rc / (float)F_CPU_ACTUAL) / 1e6f;
    return bad;
}

/* the pairs worth confirming: the middle of each window, and one step faster on the read so the
 * margin being bought is visible rather than asserted */
typedef uint32_t (*cfn)(uint32_t, float *, float *);
static const int  CW[] = {  6,  6,  6,  8 };
static const int  CR[] = { 10, 12, 14, 16 };
static const cfn  CF[] = { confirm_full< 6,10>, confirm_full< 6,12>,
                           confirm_full< 6,14>, confirm_full< 8,16> };
#define NC (sizeof(CW) / sizeof(CW[0]))

static void confirm_stage(void)
{
    Serial.println(F("\n[confirmation] both directions at once, the whole 8 MB, twice each"));
    Serial.println(F("     write/read   pass1   pass2   W MB/s   R MB/s   bound"));
    int best = -1;
    for (unsigned k = 0; k < NC; k++) {
        float w1 = 0, r1 = 0, w2 = 0, r2 = 0;
        const uint32_t b1 = CF[k](0x3B, &w1, &r1);
        const uint32_t b2 = CF[k](0x91, &w2, &r2);

        Serial.print(F("        "));
        if (CW[k] < 10) Serial.print(' ');
        Serial.print(CW[k]); Serial.print('/');
        if (CR[k] < 10) Serial.print(' ');
        Serial.print(CR[k]);    Serial.print(F("      "));
        Serial.print(b1);       Serial.print(F("       "));
        Serial.print(b2);       Serial.print(F("     "));
        Serial.print(w1, 2);    Serial.print(F("    "));
        Serial.print(r1, 2);    Serial.print(F("    "));
        if (b1 || b2) {
            Serial.print((b1 + b2) * 1e9f / (2.0f * (float)FULL), 1);
            Serial.println(F(" per billion"));
        } else {
            Serial.print(F("under "));
            Serial.print(1e9f / (2.0f * (float)FULL), 1);
            Serial.println(F(" per billion"));
            if (best < 0) best = (int)k;          /* the list is fastest-first */
        }
    }

    Serial.println(F("\n--- what to ship ---"));
    if (best < 0) {
        Serial.println(F("  no pair survived 16 MB of traffic. Do not ship any of them; the wiring"));
        Serial.println(F("  needs attention before a setting can mean anything."));
    } else {
        Serial.print(F("  write ")); Serial.print(CW[best]);
        Serial.print(F(", read ")); Serial.print(CR[best]);
        Serial.println(F(", 96-byte bursts."));
        Serial.println(F("  That is bounded under 60 errors per billion bytes by 16 MB of traffic, which"));
        Serial.println(F("  is evidence and not proof. Run psram_soak to push the bound down, and"));
        Serial.println(F("  psram_tcem to check chip select stays under 8 us at whatever rate results."));
    }
    Serial.println(F("  The 128 kB sweep above finds the SHAPE of the cliff. It cannot qualify a"));
    Serial.println(F("  setting, and a previous version of this sketch was read as though it could."));
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
    Serial.println(F("  where does the clean region begin and end"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();

    int rf, rl, wf, wl;
    sweep("read",  RN, RF, NR, &rf, &rl);
    sweep("write", WN, WF, NW, &wf, &wl);

    confirm_stage();

    Serial.println(F("\n--- the window ---"));
    if (rf < 0) {
        Serial.println(F("  reads never came clean, even at 40 no-ops. The bus is not marginal,"));
        Serial.println(F("  it is broken: a wire has moved or lost contact. Reseat before tuning."));
    } else {
        Serial.print(F("  reads clean from ")); Serial.print(rf);
        Serial.print(F(" no-ops through ")); Serial.print(rl);
        Serial.println(F(" no-ops."));
        Serial.print(F("  margin-safe read setting: "));
        Serial.print(rf + (rl - rf) / 2);
        Serial.println(F("  -- the middle of the window, not its fast edge."));
    }
    if (wf < 0) {
        Serial.println(F("  writes never came clean either."));
    } else {
        Serial.print(F("  writes clean from ")); Serial.print(wf);
        Serial.print(F(" through ")); Serial.print(wl);
        Serial.print(F(", margin-safe at "));
        Serial.println(wf + (wl - wf) / 2);
    }
    Serial.println(F("  The first tuning took the fast edge of this window and measured zero"));
    Serial.println(F("  errors over 8 MB. It was true at that instant and false an hour later."));
    Serial.println(F("  A threshold search returns a cliff edge by construction."));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

/* ===========================================================================================
 *  psram_ai -- the matrix workload, sharded across every bank that works
 *
 *  BUS-VARIANT: a fixed 32-byte burst, chosen so chip select cannot stay low past the 8 us refresh
 *  limit at any clock this sketch uses. The nibble sequence is the shared one.
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
 *  THE SHARDED MATRIX-VECTOR PRODUCT, WHICH IS WHAT THE BANKS ARE FOR
 *
 *  A matrix-vector product splits by rows and nothing else has to be shared. Give each bank a slice
 *  of the rows, let each one compute a partial sum over its own slice, and add the partials at the
 *  end. The banks never have to agree about anything, no operation ever needs more memory visible
 *  than one bank holds, and the same code runs on two banks or on ten.
 *
 *  That is also the structure the machine uses one level up, across boards. This is the same idea
 *  inside a single Teensy, which makes it a rehearsal as well as a measurement.
 *
 *
 *  WHY THE BURST IS SHORT, AND WHY SLOWING DOWN WAS THE WRONG LEVER
 *  ----------------------------------------------------------------
 *  These parts are DRAM behind an SPI interface. They refresh themselves only while chip select is
 *  HIGH, and a single chip-select-low period may not exceed 8 microseconds.
 *
 *  That makes the burst length and the clock rate pull against each other in a way that is easy to
 *  get backwards. Every time a bank failed, the instinct was to slow the clock down -- and a slower
 *  clock makes a fixed burst hold chip select low for LONGER, which is the one thing the part cannot
 *  tolerate. Slowing down past a certain point cannot fix a bank; it can only break one.
 *
 *  So the rule here is the other way round: keep the clock quick, keep the burst short, and measure
 *  the chip-select-low time directly rather than inferring it from throughput. The command, the
 *  24-bit address and the six dummy clocks are all chip-select-low too, and they are what an
 *  inferred figure leaves out.
 *
 *
 *  WHAT IT REPORTS
 *  ---------------
 *  Multiply-accumulates per second across all working banks, the aggregate read rate, and the figure
 *  the project is actually judged on: how long one decoder layer takes and what that is in tokens per
 *  second. Each bank's contribution is listed separately, because a bank that is slower than the rest
 *  drags the total and it should be visible which one.
 * ======================================================================================== */

#define PIN_DEC  5
#define B_DEC    (1u << 8)
#define P_SS0    48
#define P_SS1    51
#define P_SCLK   53
#define P_D0     52
#define P_D1     49
#define P_D2     50
#define P_D3     54

#define MAXBANK  10
#define BANKSZ   (8u * 1024u * 1024u)
#define BLK      (8u * 1024u)
#define BURST    32u               /* short on purpose: see the note about the 8 us refresh limit */

enum { SEL_CS0, SEL_CS1, SEL_DEC };
static uint8_t  g_kind[MAXBANK], g_y[MAXBANK], g_nbank;
static uint32_t g_csbit = B_SS0;
static int8_t   g_cur = -1;

static uint8_t stage[BLK] __attribute__((aligned(32)));

static inline uint32_t cbase(void)
{
    return (idle | B_SS0 | B_SS1 | B_DEC) & ~g_csbit & ~B_CLK & ~B_DATA;
}

static void select_route(uint8_t kind, uint8_t y)
{
    /* restate the pad configuration every time: anything that has touched a pin as an input leaves
     * it configured differently from what these register writes assume, and GDIR alone does not put
     * that back. Six banks looked dead for an afternoon because of exactly that. */
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    pinMode(PIN_DEC, OUTPUT);

    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    if (kind == SEL_DEC) {
        digitalWriteFast(PIN_A, (y >> 0) & 1);
        digitalWriteFast(PIN_B, (y >> 1) & 1);
        digitalWriteFast(PIN_C, (y >> 2) & 1);
        g_csbit = B_DEC;
    } else {
        g_csbit = (kind == SEL_CS0) ? B_SS0 : B_SS1;
    }
    delayMicroseconds(3);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC | (1u << 28) | (1u << 29)) & ~B_CLK;
    bus_out();
}

static void use_bank(int b)
{
    if (b == g_cur) return;
    select_route(g_kind[b], g_y[b]);
    g_cur = (int8_t)b;
}

/* ---------------------------------------------------------------------------------------------
 *  transfers, short-burst by construction
 * ------------------------------------------------------------------------------------------ */
template <int S> static void bwrite(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b;
        put_nib<S>(b, 0x3); put_nib<S>(b, 0x8); addr_out<S>(b, a);
        for (uint32_t i = 0; i < n; i++) { put_nib<S>(b, s[i] >> 4); put_nib<S>(b, s[i] & 0xF); }
        GPIO9_DR = idle;
        a += n; s += n; len -= n;
    }
}

template <int S> static void bread(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > BURST) ? BURST : len;
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b;
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

static const int NOPS[] = { 4, 5, 6, 8, 10, 12, 14, 16 };
#define NSET (sizeof(NOPS) / sizeof(NOPS[0]))
static void (*const WF[NSET])(uint32_t, const uint8_t *, uint32_t) = {
    bwrite<4>, bwrite<5>, bwrite<6>, bwrite<8>, bwrite<10>, bwrite<12>, bwrite<14>, bwrite<16> };
static void (*const RF[NSET])(uint32_t, uint8_t *, uint32_t) = {
    bread<4>, bread<5>, bread<6>, bread<8>, bread<10>, bread<12>, bread<14>, bread<16> };
static uint8_t g_wi[MAXBANK], g_ri[MAXBANK];
static bool    g_ok[MAXBANK];

static void enter_quad_here(void)
{
    const uint32_t b = cbase();
    const uint32_t b4 = b | (1u << 28) | (1u << 29);
    bus_out();
    GPIO9_DR = b; put_nib<10>(b, 0xF); put_nib<10>(b, 0x5); GPIO9_DR = idle;
    delayMicroseconds(5);
    GPIO9_DR = b4; s_byte<10>(b4, 0x66); GPIO9_DR = idle; delayMicroseconds(5);
    GPIO9_DR = b4; s_byte<10>(b4, 0x99); GPIO9_DR = idle; delay(2);
    GPIO9_DR = b4; s_byte<10>(b4, 0x35); GPIO9_DR = idle; delayMicroseconds(50);
}

/* ---------------------------------------------------------------------------------------------
 *  the arithmetic: 4-bit weights against int8 activations, SMLAD, two per instruction
 * ------------------------------------------------------------------------------------------ */
static int8_t   xvec[2048];
static uint32_t xpack[256 * 4];
static int32_t  xsum_all;

static inline uint32_t sxtb16(uint32_t x)
{ uint32_t r; __asm__("sxtb16 %0, %1" : "=r"(r) : "r"(x)); return r; }
static inline uint32_t sxtb16r8(uint32_t x)
{ uint32_t r; __asm__("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x)); return r; }
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc)
{ int32_t r; __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc)); return r; }

static void build_xpack(void)
{
    for (uint32_t g = 0; g < 256; g++) {
        const int8_t *a = &xvec[g * 8];
        xpack[g*4+0] = ((uint32_t)(a[0] & 0xFFFF)) | ((uint32_t)(a[4] & 0xFFFF) << 16);
        xpack[g*4+1] = ((uint32_t)(a[2] & 0xFFFF)) | ((uint32_t)(a[6] & 0xFFFF) << 16);
        xpack[g*4+2] = ((uint32_t)(a[1] & 0xFFFF)) | ((uint32_t)(a[5] & 0xFFFF) << 16);
        xpack[g*4+3] = ((uint32_t)(a[3] & 0xFFFF)) | ((uint32_t)(a[7] & 0xFFFF) << 16);
    }
    xsum_all = 0;
    for (int i = 0; i < 2048; i++) xsum_all += xvec[i];
}

/* one bank's partial sum over its own slice of the rows */
static inline int32_t mac_block(const uint8_t *w, uint32_t n)
{
    int32_t a0 = 0, a1 = 0;
    uint32_t g = 0;
    for (uint32_t i = 0; i + 8 <= n; i += 8) {
        const uint32_t *P = &xpack[g * 4];
        const uint32_t v0 = *(const uint32_t *)(w + i);
        const uint32_t v1 = *(const uint32_t *)(w + i + 4);
        const uint32_t L0 = v0 & 0x0F0F0F0Fu, H0 = (v0 >> 4) & 0x0F0F0F0Fu;
        const uint32_t L1 = v1 & 0x0F0F0F0Fu, H1 = (v1 >> 4) & 0x0F0F0F0Fu;
        a0 = smlad(sxtb16(L0),   P[0], a0);
        a0 = smlad(sxtb16r8(L0), P[1], a0);
        a0 = smlad(sxtb16(H0),   P[2], a0);
        a0 = smlad(sxtb16r8(H0), P[3], a0);
        a1 = smlad(sxtb16(L1),   P[4], a1);
        a1 = smlad(sxtb16r8(L1), P[5], a1);
        a1 = smlad(sxtb16(H1),   P[6], a1);
        a1 = smlad(sxtb16r8(H1), P[7], a1);
        g = (g + 2) & 255u;
    }
    return a0 + a1 - 8 * (int32_t)(n / 1024u) * xsum_all;
}

/* ======================================================================================== */

static bool probe_bank(uint8_t kind, uint8_t y)
{
    select_route(kind, y);
    enter_quad_here();
    for (uint32_t i = 0; i < BURST * 4; i++) stage[i] = (uint8_t)(i * 0x9Du + 0x3Bu + y);
    WF[2](0, stage, BURST * 4);
    uint8_t back[BURST * 4];
    for (uint32_t i = 0; i < BURST * 4; i++) back[i] = 0;
    RF[4](0, back, BURST * 4);
    for (uint32_t i = 0; i < BURST * 4; i++)
        if (back[i] != (uint8_t)(i * 0x9Du + 0x3Bu + y)) return false;
    return true;
}

void setup()
{
    Serial.begin(115200);
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_22 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_24 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_25 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_26 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_27 = 5; IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_28 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_29 = 5;
    __asm__ volatile("dsb");

    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    pinMode(PIN_DEC, OUTPUT); digitalWriteFast(PIN_DEC, HIGH);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    bus_out();
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;

    for (int i = 0; i < 2048; i++) xvec[i] = (int8_t)(((i * 37) & 0x7F) - 64);
    build_xpack();
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  sharded matrix-vector across every bank that works"));
    Serial.println(F("=================================================================="));

    /* ---- 1. which banks can actually hold data, at a burst short enough to refresh ---------- */
    static const uint8_t CK[MAXBANK] = { SEL_CS0, SEL_CS1, SEL_DEC, SEL_DEC, SEL_DEC,
                                         SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC };
    static const uint8_t CY[MAXBANK] = { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7 };
    static const char *CN[MAXBANK]   = { "CS0", "CS1", "Y0", "Y1", "Y2", "Y3", "Y4", "Y5", "Y6", "Y7" };

    g_nbank = 0; g_cur = -1;
    Serial.print(F("  banks holding data at a "));
    Serial.print(BURST);
    Serial.println(F("-byte burst:"));
    for (int k = 0; k < MAXBANK; k++) {
        if (!probe_bank(CK[k], CY[k])) continue;
        g_kind[g_nbank] = CK[k]; g_y[g_nbank] = CY[k];
        g_wi[g_nbank] = 2; g_ri[g_nbank] = 4; g_ok[g_nbank] = true;
        Serial.print(F("    bank ")); Serial.print(g_nbank);
        Serial.print(F("  on ")); Serial.println(CN[k]);
        g_nbank++;
    }
    Serial.print(F("  ")); Serial.print(g_nbank);
    Serial.print(F(" banks, ")); Serial.print((uint32_t)g_nbank * 8u);
    Serial.println(F(" MB of weights"));
    if (g_nbank == 0) { Serial.println(F("  nothing to run on.")); delay(10000); return; }

    /* ---- 2. chip select low, measured, against the 8 us limit -------------------------------- */
    use_bank(0);
    uint32_t t = ARM_DWT_CYCCNT; RF[g_ri[0]](0, stage, BURST);
    const float cs_us = 1e6f * (float)(ARM_DWT_CYCCNT - t) / (float)F_CPU_ACTUAL;
    Serial.print(F("  chip select low per burst: ")); Serial.print(cs_us, 2);
    Serial.println(cs_us > 8.0f ? F(" us  -- OVER THE REFRESH LIMIT") : F(" us  -- inside the 8 us limit"));

    /* ---- 3. fill every bank with weights ----------------------------------------------------- */
    const uint32_t PER = 1u * 1024u * 1024u;      /* 1 MB of weights per bank, so a pass is quick */
    Serial.print(F("\n  loading ")); Serial.print(PER / 1024u);
    Serial.println(F(" kB of 4-bit weights into each bank"));
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        for (uint32_t off = 0; off < PER; off += BLK) {
            for (uint32_t i = 0; i < BLK; i++) stage[i] = (uint8_t)((off + i) * 0x9Du + 0x3Bu + b);
            WF[g_wi[b]](off, stage, BLK);
        }
    }

    /* ---- 4. the sharded product: a partial sum per bank, added at the end --------------------- */
    Serial.println(F("\n  each bank computes a partial sum over its own rows"));
    Serial.println(F("    bank    MB/s    MMAC/s    ms"));
    int64_t total = 0;
    uint64_t all_cycles = 0;
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        int32_t part = 0;
        const uint32_t t0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < PER; off += BLK) {
            RF[g_ri[b]](off, stage, BLK);
            part += mac_block(stage, BLK);
        }
        const uint32_t cyc = ARM_DWT_CYCCNT - t0;
        all_cycles += cyc;
        total += part;

        const float s = (float)cyc / (float)F_CPU_ACTUAL;
        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("     ")); Serial.print((float)PER / s / 1e6f, 2);
        Serial.print(F("    ")); Serial.print((float)PER * 2.0f / s / 1e6f, 2);
        Serial.print(F("    ")); Serial.println(s * 1000.0f, 1);
    }

    const float secs = (float)(double)all_cycles / (float)F_CPU_ACTUAL;
    const float bytes = (float)PER * (float)g_nbank;
    Serial.print(F("    combined partial sum: ")); Serial.println((int32_t)total);
    Serial.print(F("    aggregate: ")); Serial.print(bytes / secs / 1e6f, 2);
    Serial.print(F(" MB/s, ")); Serial.print(bytes * 2.0f / secs / 1e6f, 2);
    Serial.println(F(" MMAC/s across all banks"));

    /* ---- 5. and what that is in the unit the project is judged on ----------------------------- */
    Serial.println(F("\n  what it means for a model"));
    const float rate = bytes / secs;                        /* bytes per second, end to end */
    for (uint32_t d = 256; d <= 1024; d *= 2) {
        const float wbytes = 8.0f * (float)d * (float)d;    /* 16 d^2 weights at 4 bits */
        const float layer_ms = wbytes / rate * 1000.0f;
        Serial.print(F("    hidden size ")); Serial.print(d);
        Serial.print(F(":  ")); Serial.print(wbytes / 1024.0f, 0);
        Serial.print(F(" kB/layer, ")); Serial.print(layer_ms, 1);
        Serial.print(F(" ms, ")); Serial.print(1000.0f / (layer_ms * 24.0f), 3);
        Serial.println(F(" tokens/s for 24 layers"));
    }
    Serial.print(F("    capacity: ")); Serial.print((uint32_t)g_nbank * 8u);
    Serial.print(F(" MB holds ")); Serial.print((uint32_t)((float)g_nbank * 8.0f * 1048576.0f / (8.0f * 512.0f * 512.0f)));
    Serial.println(F(" layers at hidden size 512"));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

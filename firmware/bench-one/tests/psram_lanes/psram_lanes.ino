/* ===========================================================================================
 *  psram_lanes -- eight chips answer, two transfer. This finds which wire the other six are missing.
 *
 *  BUS-VARIANT: deliberately runs every transfer at 16 no-ops, far slower than any qualified
 *  setting, so that a failure here cannot be a timing margin. It also adds single-bit SPI paths that
 *  the shared layer has no reason to carry.
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
 *  WHICH DATA LINE IS BROKEN, PER BANK
 *
 *  All eight chips answer an identity command. Two of them then transfer data correctly and six fail
 *  at every speed offered, down to the slowest setting in the table. Speed is therefore not the
 *  problem, and neither is the chip select, the clock, or the chip itself -- all of those had to work
 *  for the identity to come back.
 *
 *  What separates the two cases is how many wires each operation needs:
 *
 *      identity, command 0x9F   single-bit SPI    command out on SIO0, reply in on SIO1
 *      real transfers           quad              all four of SIO0..SIO3, every clock
 *
 *  So a chip whose SIO2 or SIO3 never arrives answers the probe perfectly and fails every transfer.
 *  That is exactly the pattern on the bench, and the two banks that work are the two wired by the
 *  Teensy's own board rather than by hand.
 *
 *  This proves it rather than inferring it, in two steps per bank:
 *
 *  1. SINGLE-BIT ROUND TRIP. Write with 0x02 and read back with 0x03, which touch only SIO0 and SIO1.
 *     Clean here means the chip, its select, the clock and those two lines are all sound, and narrows
 *     everything that follows to the other two wires.
 *
 *  2. QUAD ROUND TRIP, COUNTED PER LANE. Enter quad, write with 0x38, read with 0xEB, and tally the
 *     wrong bits separately for SIO0, SIO1, SIO2 and SIO3. A line that is not connected reads the same
 *     value every clock, so it is wrong about half the time and its neighbours are not. The tally
 *     names the wire.
 *
 *  The pattern in the tally says which fault it is. One lane wrong on its own is that wire. Two lanes
 *  wrong together, at about half, is usually a swap. All four wrong is the chip never entered quad
 *  mode, which points back at SIO0 rather than at the pair.
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

#define NB   64u                        /* bytes per round trip: plenty for a per-lane tally */
#define MAXBANK 10

static uint8_t ref[NB], got[NB];

enum { SEL_CS0, SEL_CS1, SEL_DEC };
static uint8_t g_kind[MAXBANK], g_y[MAXBANK], g_nbank;
static uint32_t g_csbit = B_SS0;

static inline uint32_t cbase(void)
{
    return (idle | B_SS0 | B_SS1 | B_DEC) & ~g_csbit & ~B_CLK & ~B_DATA;
}

static void select_route(uint8_t kind, uint8_t y)
{
    /* Re-assert the pad configuration every time, not once in setup().
     *
     * Reads from six of the eight banks were coming back empty until this was added, and the same
     * banks answered perfectly through a path that happened to call pinMode on its way past. Anything
     * that touches a pin as an input -- a reply line being released, a probe, a previous test -- can
     * leave the pad configured differently from what the register writes assume, and GDIR alone does
     * not put that back. Restating it costs microseconds once per bank and removes a whole class of
     * "the hardware is broken" that was not. */
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
    delayMicroseconds(5);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC | (1u << 28) | (1u << 29)) & ~B_CLK;
}

/* ---------------------------------------------------------------------------------------------
 *  single-bit SPI, slow and deliberate. Only SIO0 out and SIO1 in.
 * ------------------------------------------------------------------------------------------ */
/* g_us is the half-period in microseconds. Zero means as fast as the register allows, which is about
 * 8 MHz; one microsecond is 500 kHz; eight is 60 kHz. One implementation covers the whole span so a
 * ceiling is measured rather than bracketed by two different pieces of code. */
static uint16_t g_us = 0;

static inline void s1_hold(void)
{
    if (g_us) delayMicroseconds(g_us);
    else      spin<16>();
}

static void s1_byte(uint32_t b, uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        const uint32_t d = b | (((v >> i) & 1u) << DSHIFT);
        GPIO9_DR = d;
        GPIO9_DR = d | B_CLK;  s1_hold();
        GPIO9_DR = d;          s1_hold();
    }
}

static uint8_t s1_recv(uint32_t b)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        GPIO9_DR = b | B_CLK;  s1_hold();
        v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));   /* SIO1 is GPIO9 bit 27 */
        GPIO9_DR = b;          s1_hold();
    }
    return v;
}

/* leave quad if we are in it, then reset, so the chip is certainly listening in single-bit mode */
static void to_single(void)
{
    bus_out();
    uint32_t b = cbase();
    GPIO9_DR = b; put_nib<16>(b, 0xF); put_nib<16>(b, 0x5); GPIO9_DR = idle;
    delayMicroseconds(10);
    const uint32_t b4 = cbase() | (1u << 28) | (1u << 29);
    GPIO9_DR = b4; s1_byte(b4, 0x66); GPIO9_DR = idle; delayMicroseconds(10);
    GPIO9_DR = b4; s1_byte(b4, 0x99); GPIO9_DR = idle; delay(2);
}

static void s1_write(uint32_t a, const uint8_t *s, uint32_t n)
{
    bus_out();
    const uint32_t b = cbase() | (1u << 28) | (1u << 29);   /* SIO2/3 idle high, unused here */
    GPIO9_DR = b;
    s1_byte(b, 0x02);
    s1_byte(b, (a >> 16) & 0xFF); s1_byte(b, (a >> 8) & 0xFF); s1_byte(b, a & 0xFF);
    for (uint32_t i = 0; i < n; i++) s1_byte(b, s[i]);
    GPIO9_DR = idle;
}

static void s1_read(uint32_t a, uint8_t *d, uint32_t n)
{
    bus_out();
    const uint32_t b = cbase() | (1u << 28) | (1u << 29);
    GPIO9_DR = b;
    s1_byte(b, 0x03);
    s1_byte(b, (a >> 16) & 0xFF); s1_byte(b, (a >> 8) & 0xFF); s1_byte(b, a & 0xFF);
    GPIO9_GDIR &= ~(1u << 27);                              /* release SIO1 to listen */
    for (uint32_t i = 0; i < n; i++) d[i] = s1_recv(b);
    GPIO9_DR = idle;
    bus_out();
}

/* ---------------------------------------------------------------------------------------------
 *  quad, at a deliberately slow edge rate so that anything wrong is a wire and not a timing margin
 * ------------------------------------------------------------------------------------------ */
static void q_enter(void)
{
    const uint32_t b4 = cbase() | (1u << 28) | (1u << 29);
    bus_out();
    GPIO9_DR = b4; s1_byte(b4, 0x35); GPIO9_DR = idle; delayMicroseconds(50);
}

static void q_write(uint32_t a, const uint8_t *s, uint32_t n)
{
    bus_out();
    const uint32_t b = cbase(); GPIO9_DR = b;
    put_nib<16>(b, 0x3); put_nib<16>(b, 0x8); addr_out<16>(b, a);
    for (uint32_t i = 0; i < n; i++) { put_nib<16>(b, s[i] >> 4); put_nib<16>(b, s[i] & 0xF); }
    GPIO9_DR = idle;
}

static void q_read(uint32_t a, uint8_t *d, uint32_t n)
{
    bus_out();
    const uint32_t b = cbase(); GPIO9_DR = b;
    put_nib<16>(b, 0xE); put_nib<16>(b, 0xB); addr_out<16>(b, a);
    data_in();
    for (int k = 0; k < 6; k++) { GPIO9_DR = b | B_CLK; spin<16>(); GPIO9_DR = b; spin<16>(); }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t hi = get_nib<16>(b);
        d[i] = (uint8_t)((hi << 4) | get_nib<16>(b));
    }
    GPIO9_DR = idle; bus_out();
}

/* ======================================================================================== */

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

    /* every place a chip can be, in the order psram_perfboard numbers them */
    static const uint8_t K[8] = { SEL_CS0, SEL_CS1, SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC };
    static const uint8_t Y[8] = { 0, 0, 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 8; i++) { g_kind[i] = K[i]; g_y[i] = Y[i]; }
    g_nbank = 8;
}

/* The slow path, written out the long way. Nothing here shares a line with the register path, so if
 * the two disagree the difference is in one of them and not in a constant they both read. */
static void slow_select(uint8_t kind, uint8_t y)
{
    digitalWriteFast(P_SS0, HIGH);
    digitalWriteFast(P_SS1, HIGH);
    digitalWriteFast(PIN_DEC, HIGH);
    if (kind == SEL_DEC) {
        digitalWriteFast(PIN_A, (y >> 0) & 1);
        digitalWriteFast(PIN_B, (y >> 1) & 1);
        digitalWriteFast(PIN_C, (y >> 2) & 1);
    }
    delayMicroseconds(5);
}

static uint8_t slow_cs(uint8_t kind)
{
    if (kind == SEL_CS0) return P_SS0;
    if (kind == SEL_CS1) return P_SS1;
    return PIN_DEC;
}

static void slow_tick(void)
{
    digitalWriteFast(P_SCLK, HIGH); delayMicroseconds(2);
    digitalWriteFast(P_SCLK, LOW);  delayMicroseconds(2);
}

static void slow_send(uint8_t v)
{
    pinMode(P_D0, OUTPUT);
    for (int i = 7; i >= 0; i--) { digitalWriteFast(P_D0, (v >> i) & 1); slow_tick(); }
}

static uint8_t slow_recv(void)
{
    uint8_t v = 0;
    pinMode(P_D1, INPUT);
    for (int i = 0; i < 8; i++) {
        digitalWriteFast(P_SCLK, HIGH);
        v = (uint8_t)((v << 1) | (digitalReadFast(P_D1) ? 1 : 0));
        delayMicroseconds(2);
        digitalWriteFast(P_SCLK, LOW);
        delayMicroseconds(2);
    }
    return v;
}

/* identity through the slow path, including the wake-up the fast path also does */
static void slow_identity(uint8_t kind, uint8_t y, uint8_t *id)
{
    const uint8_t cs = slow_cs(kind);
    slow_select(kind, y);

    pinMode(P_SCLK, OUTPUT); digitalWriteFast(P_SCLK, LOW);
    pinMode(P_D2, OUTPUT);   digitalWriteFast(P_D2, HIGH);
    pinMode(P_D3, OUTPUT);   digitalWriteFast(P_D3, HIGH);

    /* leave quad, in quad */
    pinMode(P_D0, OUTPUT); pinMode(P_D1, OUTPUT);
    digitalWriteFast(cs, LOW); delayMicroseconds(2);
    for (int k = 0; k < 2; k++) {
        const uint8_t nib = (k == 0) ? 0xF : 0x5;
        digitalWriteFast(P_D0, (nib >> 0) & 1);
        digitalWriteFast(P_D1, (nib >> 1) & 1);
        digitalWriteFast(P_D2, (nib >> 2) & 1);
        digitalWriteFast(P_D3, (nib >> 3) & 1);
        slow_tick();
    }
    digitalWriteFast(cs, HIGH);
    digitalWriteFast(P_D2, HIGH); digitalWriteFast(P_D3, HIGH);
    delayMicroseconds(10);

    digitalWriteFast(cs, LOW); delayMicroseconds(2); slow_send(0x66);
    digitalWriteFast(cs, HIGH); delayMicroseconds(10);
    digitalWriteFast(cs, LOW); delayMicroseconds(2); slow_send(0x99);
    digitalWriteFast(cs, HIGH); delay(2);

    digitalWriteFast(cs, LOW); delayMicroseconds(2);
    slow_send(0x9F); slow_send(0); slow_send(0); slow_send(0);
    id[0] = slow_recv(); id[1] = slow_recv();
    digitalWriteFast(cs, HIGH); delayMicroseconds(10);

    digitalWriteFast(P_SS0, HIGH); digitalWriteFast(P_SS1, HIGH); digitalWriteFast(PIN_DEC, HIGH);
}

/* identity through the register path, at the same 250 kHz so speed cannot be the difference */
static void fast_identity(uint8_t kind, uint8_t y, uint8_t *id)
{
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    bus_out();

    g_us = 2;
    select_route(kind, y);
    to_single();

    const uint32_t b = cbase() | (1u << 28) | (1u << 29);
    GPIO9_DR = b;
    s1_byte(b, 0x9F); s1_byte(b, 0); s1_byte(b, 0); s1_byte(b, 0);
    GPIO9_GDIR &= ~(1u << 27);
    id[0] = s1_recv(b); id[1] = s1_recv(b);
    GPIO9_DR = idle;
    bus_out();
    g_us = 0;
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  the same identity, two ways, same bank, same 250 kHz"));
    Serial.println(F("=================================================================="));
    Serial.println(F("    bank   digitalWriteFast   GPIO9 register   agree?"));
    int disagree = 0;
    for (int b = 0; b < g_nbank; b++) {
        uint8_t a[2] = { 0, 0 }, c[2] = { 0, 0 };
        slow_identity(g_kind[b], g_y[b], a);
        fast_identity(g_kind[b], g_y[b], c);

        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("        ")); Serial.print(a[0], HEX); Serial.print(' '); Serial.print(a[1], HEX);
        Serial.print(F("              ")); Serial.print(c[0], HEX); Serial.print(' '); Serial.print(c[1], HEX);
        Serial.print(F("          "));
        if (a[0] == c[0] && a[1] == c[1]) Serial.println(F("yes"));
        else { Serial.println(F("NO -- one of my two paths is wrong")); disagree++; }
    }
    Serial.print(F("    "));
    if (disagree)
        Serial.println(F("The paths disagree, so the fault is in my code, not on the board."));
    else
        Serial.println(F("Both paths agree, so whatever they report is the board talking."));

    /* Reading an identity register proves a chip is alive and almost nothing else. Storing a byte is
     * the thing the machine is actually for, and it is the operation that needs real supply current
     * rather than the trickle an output driver can push through an input clamp. */
    Serial.println(F("\n  write then read back, 64 bytes, single-bit, 250 kHz"));
    Serial.println(F("    bank   wrong of 64"));
    for (int b = 0; b < g_nbank; b++) {
        g_us = 2;
        select_route(g_kind[b], g_y[b]);
        to_single();
        for (uint32_t i = 0; i < NB; i++) ref[i] = (uint8_t)(i * 0x9Du + 0x3Bu + b * 0x11u);
        s1_write(0, ref, NB);
        for (uint32_t i = 0; i < NB; i++) got[i] = 0;
        s1_read(0, got, NB);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < NB; i++) if (got[i] != ref[i]) bad++;
        g_us = 0;
        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("       ")); Serial.print(bad);
        Serial.print(bad ? F("   <- cannot store.  wrote ") : F("   stores fine. wrote "));
        /* What came back matters as much as how much was wrong. All 00 or all FF means the chip is
         * not driving the reply line at all during an array read, even though it drove it happily
         * for the identity -- which is a chip that answers hardwired logic and cannot reach its
         * array. Stable but different bytes would mean the read works and the write did not. */
        for (int k = 0; k < 4; k++) { Serial.print(ref[k], HEX); Serial.print(' '); }
        Serial.print(F(" got "));
        for (int k = 0; k < 4; k++) { Serial.print(got[k], HEX); Serial.print(' '); }
        Serial.println();
    }

    Serial.println(F("\n=== repeating in 12 s ==="));
    delay(12000);
}

static void unused_old_loop(void)
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  which data line is broken, per bank. Both tests run SLOWLY."));
    Serial.println(F("=================================================================="));
    Serial.println(F("    how slow does each bank have to be, on two wires only"));
    Serial.println(F("    bank    8MHz   500kHz  250kHz  125kHz   60kHz    ceiling"));

    static const uint16_t US[5] = { 0, 1, 2, 4, 8 };
    static const char *UN[5]    = { "8MHz", "500kHz", "250kHz", "125kHz", "60kHz" };

    for (int b = 0; b < g_nbank; b++) {
        Serial.print(F("      ")); Serial.print(b); Serial.print(F("     "));
        int best = -1;
        for (int k = 0; k < 5; k++) {
            g_us = US[k];
            select_route(g_kind[b], g_y[b]);
            to_single();
            for (uint32_t i = 0; i < NB; i++) ref[i] = (uint8_t)(i * 0x9Du + 0x3Bu + b * 0x11u);
            s1_write(0, ref, NB);
            for (uint32_t i = 0; i < NB; i++) got[i] = 0;
            s1_read(0, got, NB);
            uint32_t bad = 0;
            for (uint32_t i = 0; i < NB; i++) if (got[i] != ref[i]) bad++;

            if (bad < 10) Serial.print(' ');
            Serial.print(bad);
            Serial.print(F("      "));
            if (bad == 0 && best < 0) best = k;
        }
        if (best < 0) Serial.println(F("  none -- fails even at 60 kHz"));
        else { Serial.print(F("  ")); Serial.println(UN[best]); }
    }
    g_us = 0;

    /* ---- is the supply real, or borrowed from the bus ---------------------------------------- */
    Serial.println(F("\n[2] real supply, or charge stolen from the bus"));
    Serial.println(F("    identity, then every bus line held low 150 ms, then identity again"));
    Serial.println(F("    bank   before   after    verdict"));

    for (int b = 0; b < g_nbank; b++) {
        select_route(g_kind[b], g_y[b]);
        to_single();

        /* 1. identity with the bus idling high */
        uint8_t id1[2];
        {
            bus_out();
            const uint32_t bb = cbase() | (1u << 28) | (1u << 29);
            GPIO9_DR = bb;
            s1_byte(bb, 0x9F); s1_byte(bb, 0); s1_byte(bb, 0); s1_byte(bb, 0);
            GPIO9_GDIR &= ~(1u << 27);
            id1[0] = s1_recv(bb); id1[1] = s1_recv(bb);
            GPIO9_DR = idle; bus_out();
        }

        /* 2. deselect everything and hold every bus line low, so there is nothing to trickle from */
        GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
        bus_out();
        GPIO9_DR = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC) & ~B_DATA & ~B_CLK;
        delay(150);

        /* 3. and ask again straight away */
        uint8_t id2[2];
        {
            select_route(g_kind[b], g_y[b]);
            bus_out();
            const uint32_t bb = cbase() | (1u << 28) | (1u << 29);
            GPIO9_DR = bb;
            s1_byte(bb, 0x9F); s1_byte(bb, 0); s1_byte(bb, 0); s1_byte(bb, 0);
            GPIO9_GDIR &= ~(1u << 27);
            id2[0] = s1_recv(bb); id2[1] = s1_recv(bb);
            GPIO9_DR = idle; bus_out();
        }

        const bool ok1 = (id1[0] == 0x0D && id1[1] == 0x5D);
        const bool ok2 = (id2[0] == 0x0D && id2[1] == 0x5D);

        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("     ")); Serial.print(id1[0], HEX); Serial.print(' '); Serial.print(id1[1], HEX);
        Serial.print(F("    ")); Serial.print(id2[0], HEX); Serial.print(' '); Serial.print(id2[1], HEX);
        Serial.print(F("    "));
        if (ok1 && ok2)       Serial.println(F("has its own supply"));
        else if (ok1 && !ok2) Serial.println(F("LIVING OFF THE BUS -- no supply at this chip"));
        else if (!ok1)        Serial.println(F("did not answer at all this time"));
        Serial.println();
    }

    Serial.println(F("\n--- what the ceiling means ---"));
    Serial.println(F("  The numbers are wrong bytes out of 64, on a round trip that uses only SIO0"));
    Serial.println(F("  and SIO1. Zero is a clean pass at that speed."));
    Serial.println(F("  A bank clean at 8 MHz is as good as the two on the Teensy's own board."));
    Serial.println(F("  A bank that needs 250 kHz or slower is not a timing-margin problem and no"));
    Serial.println(F("  no-op count reaches it. Something costs nothing at 250 kHz and everything"));
    Serial.println(F("  higher, which is the supply at the far chips or the ground return:"));
    Serial.println(F("    a 100 nF across pins 8 and 4 AT EACH CHIP, shortest loop possible"));
    Serial.println(F("    a ground wire running beside the signals the whole length of the run"));
    Serial.println(F("    bulk capacitance where 3.3 V enters the group"));
    Serial.println(F("  A bank failing even at 60 kHz is a wire, not a speed. Check its CE# first,"));
    Serial.println(F("  then its power, because everything else is shared with banks that work."));

    Serial.println(F("\n=== repeating in 15 s ==="));
    delay(15000);
}

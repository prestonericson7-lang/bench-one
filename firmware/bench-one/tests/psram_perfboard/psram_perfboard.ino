/* ===========================================================================================
 *  psram_perfboard -- flash this first on the perfboard build. It finds what you wired.
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
 *  THE TOPOLOGY THIS EXPECTS, AND WHY IT IS THIS ONE
 *
 *  The breadboard build is capped at 8 MB of 48 because the decoder's enable shares a wire with the
 *  onboard chip's chip select, so selecting any bank also selects the onboard chip and both drive the
 *  data lines. On the perfboard the enable gets a pin of its own and the cap goes away.
 *
 *      pin 48   CS0, the Teensy's own QSPI select   -> onboard chip A's CE#, already wired, untouched
 *      pin 5    GPIO9 bit 8, free                   -> decoder G2A, the enable, active low
 *      pins 2/3/4                                   -> decoder A, B, C, already wired
 *      Y0                                           -> onboard chip B's CE#
 *      Y1 .. Y7                                     -> the seven new chips
 *
 *  Nine chips, 72 MB, and the decoder fully used. Parking does not need a spare output any more --
 *  with a dedicated enable, "no chip selected" is just the enable deasserted, so all eight outputs
 *  carry a chip instead of seven plus an unconnected one.
 *
 *  Pin 5 is not an arbitrary choice. teensy_pinmap asked the core rather than the pinout card and
 *  found it free and in GPIO9, the same register as the clock and the four data lines, which the bus
 *  driver writes whole once per edge. A select in another register would cost a second store on every
 *  burst boundary. Pin 33 is the alternative if pin 5 is awkward to reach; it is GPIO9 bit 7.
 *
 *  The enable must be ACTIVE LOW, so it goes to G2A or G2B and not to G1. That is deduced, not
 *  assumed: on the breadboard the enable is driven by a chip select, which is active low, and all six
 *  chips do answer -- had it been on the active-high G1 input, asserting CS0 would have DISABLED the
 *  decoder and nothing behind it could ever have replied.
 *
 *
 *  WHAT THIS SKETCH IS FOR
 *  -----------------------
 *  Flash it first, before anything else. It assumes nothing about what got soldered: it finds every
 *  chip that answers, proves the banks are independent rather than sharing a select, qualifies each
 *  one's timing separately, checks chip select against the refresh limit, and then writes and verifies
 *  the whole flat address space across every bank found.
 *
 *  It assumes nothing because the last time something was assumed about this wiring it cost a day.
 *  Four hardware hypotheses were chased before the actual fault turned out to be in my own test code,
 *  and the detail that would have settled it was printed in every run from the first.
 *
 *
 *  THE NUMBER TO WATCH
 *  -------------------
 *  Nanoseconds per nibble, in stage 3. The breadboard needs 37.2 ns and FlexSPI2 cannot be clocked
 *  slower than 20.2 ns, which is why the hardware controller has never been usable: it is 1.85x too
 *  fast for that wiring and no setting of it can work. Get any bank under 20.2 ns and the controller
 *  becomes reachable for it, which brings memory mapping and roughly 33 MB/s instead of 14.
 *
 *  That is what the perfboard is for, and this prints the number directly against the threshold.
 * ======================================================================================== */

#define PIN_DEC  5                      /* decoder enable, active low. GPIO9 bit 8. */
#define B_DEC    (1u << 8)
#define P_SS0    48
#define P_SS1    51
#define P_SCLK   53
#define P_D0     52                     /* SIO0, data IN in single-SPI mode  */
#define P_D1     49                     /* SIO1, data OUT in single-SPI mode */
#define P_D2     50
#define P_D3     54

#define BANKSZ   (8u * 1024u * 1024u)
#define BLK      (8u * 1024u)
#define MAXBANK  10                     /* CS0, CS1, and eight decoder outputs */

static uint8_t buf[BLK];
static uint8_t ref[BLK];
static uint32_t g_burst = 96;            /* per bank, set from the measured chip-select-low line */

/* ---------------------------------------------------------------------------------------------
 *  a select route, and the current one
 *
 *  Three kinds exist: the Teensy's own two chip selects, and the eight decoder outputs. They differ
 *  only in which GPIO9 bit has to go low and whether the decoder address needs setting first.
 * ------------------------------------------------------------------------------------------ */
enum { SEL_CS0, SEL_CS1, SEL_DEC };

/* Two parallel arrays rather than an array of structs, deliberately. Arduino inserts its own
 * prototypes for every function at the top of the file, ahead of any typedef written in it, so a
 * struct in a signature fails to compile for a reason that has nothing to do with the code. */
static uint8_t g_kind[MAXBANK];          /* the banks that actually answered, in order */
static uint8_t g_y[MAXBANK];
static uint8_t g_nbank;
static uint32_t g_csbit = B_SS0;         /* the bit the transfer routines deassert to select */
static int8_t  g_cur = -1;               /* which discovered bank is currently addressed */

/* every bank gets its own qualified timing, because a chip on short onboard traces and one at the
 * far end of a perfboard are not the same electrical problem */
static uint8_t g_wi[MAXBANK];
static uint8_t g_ri[MAXBANK];
static float   g_ns[MAXBANK];            /* measured ns per nibble: the figure 20.2 is compared to */
static uint16_t g_bn[MAXBANK];           /* the burst length that keeps chip select inside 7 us */

static inline uint32_t cbase(void)
{
    return (idle | B_SS0 | B_SS1 | B_DEC) & ~g_csbit & ~B_CLK & ~B_DATA;
}

/* BREAK BEFORE MAKE. The address lines must never change while a chip is selected, or the decoder
 * walks a low pulse across other outputs on the way -- which momentarily selects chips that were not
 * asked for, on a shared data bus. So the enable is raised, the address is set, and only then is the
 * enable lowered, which is what cbase() does on the next burst. */
static void select_route(uint8_t kind, uint8_t y)
{
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);          /* everything deasserted first */
    if (kind == SEL_DEC) {
        digitalWriteFast(PIN_A, (y >> 0) & 1);
        digitalWriteFast(PIN_B, (y >> 1) & 1);
        digitalWriteFast(PIN_C, (y >> 2) & 1);
        g_csbit = B_DEC;
    } else {
        g_csbit = (kind == SEL_CS0) ? B_SS0 : B_SS1;
    }
    delayMicroseconds(2);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC | (1u << 28) | (1u << 29)) & ~B_CLK;
}

static void use_bank(int b)
{
    if (b == g_cur) return;                        /* re-selecting costs microseconds; skip it */
    select_route(g_kind[b], g_y[b]);
    g_cur = (int8_t)b;
}

/* ---------------------------------------------------------------------------------------------
 *  the slow, safe probe
 *
 *  digitalWriteFast at roughly 250 kHz, which is the implementation that was already proven to work
 *  when a register-level rewrite of the same thing was failing in every configuration. Speed is
 *  irrelevant here and being believed is not, so this is the one that asks "is a chip there".
 * ------------------------------------------------------------------------------------------ */
static void s_tick(void)
{
    digitalWriteFast(P_SCLK, HIGH); delayMicroseconds(1);
    digitalWriteFast(P_SCLK, LOW);  delayMicroseconds(1);
}

static void s_send(uint8_t v)
{
    pinMode(P_D0, OUTPUT);
    for (int i = 7; i >= 0; i--) { digitalWriteFast(P_D0, (v >> i) & 1); s_tick(); }
}

static uint8_t s_recv(void)
{
    uint8_t v = 0;
    pinMode(P_D1, INPUT);
    for (int i = 0; i < 8; i++) {
        digitalWriteFast(P_SCLK, HIGH);
        v = (uint8_t)((v << 1) | (digitalReadFast(P_D1) ? 1 : 0));
        delayMicroseconds(1);
        digitalWriteFast(P_SCLK, LOW);
        delayMicroseconds(1);
    }
    return v;
}

static void all_high(void)
{
    digitalWriteFast(P_SS0, HIGH);
    digitalWriteFast(P_SS1, HIGH);
    digitalWriteFast(PIN_DEC, HIGH);
}

static uint8_t sel_pin(uint8_t kind)
{
    if (kind == SEL_CS0) return P_SS0;
    if (kind == SEL_CS1) return P_SS1;
    return PIN_DEC;
}

/* Ask one route for an identity. 0x9F, a 24-bit don't-care address, then eight bytes; a real
 * ESP-PSRAM64H answers 0x0D then 0x5D and an open bus cannot.
 *
 * The chip may be in quad mode from a previous sketch, in which case a single-bit command means
 * nothing to it -- so the quad-mode exit and a reset go first, sent in the mode the chip might
 * actually be in. Skipping that step makes a working chip look absent. */
static bool probe(uint8_t kind, uint8_t y, uint8_t *id)
{
    const uint8_t cs = sel_pin(kind);

    all_high();
    if (kind == SEL_DEC) {
        digitalWriteFast(PIN_A, (y >> 0) & 1);
        digitalWriteFast(PIN_B, (y >> 1) & 1);
        digitalWriteFast(PIN_C, (y >> 2) & 1);
    }
    delayMicroseconds(5);

    pinMode(P_SCLK, OUTPUT); digitalWriteFast(P_SCLK, LOW);
    pinMode(P_D2, OUTPUT);   digitalWriteFast(P_D2, HIGH);   /* idle high so a quad chip is not */
    pinMode(P_D3, OUTPUT);   digitalWriteFast(P_D3, HIGH);   /* reading them as command bits     */

    /* quad exit, in quad: 0xF5 as two nibbles across all four lines */
    pinMode(P_D0, OUTPUT); pinMode(P_D1, OUTPUT);
    digitalWriteFast(cs, LOW); delayMicroseconds(1);
    for (int k = 0; k < 2; k++) {
        const uint8_t n = (k == 0) ? 0xF : 0x5;
        digitalWriteFast(P_D0, (n >> 0) & 1);
        digitalWriteFast(P_D1, (n >> 1) & 1);
        digitalWriteFast(P_D2, (n >> 2) & 1);
        digitalWriteFast(P_D3, (n >> 3) & 1);
        s_tick();
    }
    digitalWriteFast(cs, HIGH);
    digitalWriteFast(P_D2, HIGH); digitalWriteFast(P_D3, HIGH);
    delayMicroseconds(5);

    /* reset enable, reset */
    digitalWriteFast(cs, LOW); delayMicroseconds(1); s_send(0x66);
    digitalWriteFast(cs, HIGH); delayMicroseconds(5);
    digitalWriteFast(cs, LOW); delayMicroseconds(1); s_send(0x99);
    digitalWriteFast(cs, HIGH); delay(2);

    /* the identity */
    digitalWriteFast(cs, LOW); delayMicroseconds(1);
    s_send(0x9F); s_send(0x00); s_send(0x00); s_send(0x00);
    for (int i = 0; i < 8; i++) id[i] = s_recv();
    digitalWriteFast(cs, HIGH); delayMicroseconds(5);

    all_high();
    return id[0] == 0x0D && id[1] == 0x5D;
}

/* ---------------------------------------------------------------------------------------------
 *  the fast transfers, one per timing setting
 *
 *  Identical nibble sequences to every other psram sketch in the tree -- .claude/verify-psram-bus.py
 *  checks that mechanically, because a driver running a different instruction sequence from the tests
 *  that qualified it is exactly the fault that cost a day. The only difference here is cbase(), which
 *  deasserts whichever bit selects the current bank instead of always CS0.
 * ------------------------------------------------------------------------------------------ */
template <int S> static void bwrite(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
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
        const uint32_t n = (len > g_burst) ? g_burst : len;
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

static const int NOPS[] = { 4, 5, 6, 8, 10, 12, 14, 16, 20, 24 };
#define NSET (sizeof(NOPS) / sizeof(NOPS[0]))

static void (*const WF[NSET])(uint32_t, const uint8_t *, uint32_t) = {
    bwrite<4>, bwrite<5>, bwrite<6>, bwrite<8>, bwrite<10>,
    bwrite<12>, bwrite<14>, bwrite<16>, bwrite<20>, bwrite<24>
};
static void (*const RF[NSET])(uint32_t, uint8_t *, uint32_t) = {
    bread<4>, bread<5>, bread<6>, bread<8>, bread<10>,
    bread<12>, bread<14>, bread<16>, bread<20>, bread<24>
};

/* quad mode has to be entered per chip: each one has its own mode latch */
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

static inline uint8_t pat(uint8_t bank, uint32_t i)
{
    return (uint8_t)(i * 0x9Du + 0x3Bu + (uint32_t)bank * 0x57u);
}

/* ======================================================================================== */

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    pinMode(PIN_DEC, OUTPUT); digitalWriteFast(PIN_DEC, HIGH);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    GPIO9_DR |= (B_SS0 | B_SS1 | B_DEC);
    bus_out();
    ARM_DEMCR |= ARM_DEMCR_TRCENA;
    ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

static void stage1_discover(void)
{
    static const uint8_t CKIND[MAXBANK] = { SEL_CS0, SEL_CS1, SEL_DEC, SEL_DEC, SEL_DEC,
                                            SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC, SEL_DEC };
    static const uint8_t CY[MAXBANK]    = { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7 };
    static const char *NAME[MAXBANK] = {
        "CS0  pin 48", "CS1  pin 51",
        "dec Y0", "dec Y1", "dec Y2", "dec Y3", "dec Y4", "dec Y5", "dec Y6", "dec Y7"
    };

    Serial.println(F("[1] what is actually wired"));

    digitalWriteFast(PIN_A, 1); digitalWriteFast(PIN_B, 1); digitalWriteFast(PIN_C, 1);
    delayMicroseconds(5);

    Serial.println(F("    select        identity     "));
    g_nbank = 0;
    uint8_t id[8];
    int ndec = 0;
    for (int k = 0; k < MAXBANK; k++) {
        const bool ok = probe(CKIND[k], CY[k], id);
        Serial.print(F("    "));
        Serial.print(NAME[k]);
        for (unsigned p = strlen(NAME[k]); p < 13; p++) Serial.print(' ');
        if (ok) {
            Serial.print(F("0D 5D        bank "));
            Serial.println(g_nbank);
            g_kind[g_nbank] = CKIND[k]; g_y[g_nbank] = CY[k]; g_nbank++;
            if (CKIND[k] == SEL_DEC) ndec++;
        } else {
            Serial.print(F("--  (got "));
            Serial.print(id[0], HEX); Serial.print(' '); Serial.print(id[1], HEX);
            Serial.println(F(")"));
        }
    }

    Serial.print(F("    found ")); Serial.print(g_nbank);
    Serial.print(F(" chips = ")); Serial.print((uint32_t)g_nbank * 8u);
    Serial.println(F(" MB"));

    if (ndec == 0) {
        Serial.println(F("    NO decoder bank answered. Either the enable wire is not on pin 5 yet,"));
        Serial.println(F("    or it went to G1 instead of G2A -- G1 is active HIGH and would hold the"));
        Serial.println(F("    decoder off. Check that one wire before anything else here means much."));
    }

    /* The old breadboard wiring ties the decoder enable to pad 48. If it is still like that, then
     * asserting CS0 also enables the decoder, and what answers on "CS0" depends on where A/B/C happen
     * to point. Probing CS0 twice with different addresses detects exactly that, and it is worth
     * detecting because every other result would be quietly wrong. */
    /* No separate "is the enable shared" verdict here, deliberately. It cannot be measured from this
     * side: when the onboard chip and a bank are selected together the onboard chip wins the read, so
     * a CS0 read returns it whatever the decoder is doing. The decoder rows above already answer the
     * question -- they are probed from pin 5, so a reply means the enable is on pin 5. */
    if (ndec > 0) {
        Serial.println(F("    The decoder replies to pin 5, so the enable has its own pin and the"));
        Serial.println(F("    8 MB cap is gone. Stage 2 checks the banks really are separate chips."));
    }
}

/* A different pattern in every bank, all written before any is read. Independent banks each return
 * their own; banks sharing a select return whatever the last chip written holds. */
static int stage2_independent(void)
{
    Serial.println(F("\n[2] are the banks independent"));
    g_burst = 64;        /* before stage 3 has measured anything, pick a burst short enough that the
                          * refresh limit cannot be the reason a bank looks broken here */
    const uint32_t span = 64u * 1024u;
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b); enter_quad_here();
        for (uint32_t off = 0; off < span; off += BLK) {
            for (uint32_t i = 0; i < BLK; i++) ref[i] = pat((uint8_t)b, off + i);
            WF[2](off, ref, BLK);                      /* 6 no-ops, generous for a write */
        }
    }
    int good = 0;
    Serial.println(F("    bank   wrong of 65536"));
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        uint32_t bad = 0;
        for (uint32_t off = 0; off < span; off += BLK) {
            RF[4](off, buf, BLK);                      /* 10 no-ops, generous for a read */
            for (uint32_t i = 0; i < BLK; i++) if (buf[i] != pat((uint8_t)b, off + i)) bad++;
        }
        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("      ")); Serial.println(bad);
        if (bad == 0) good++;
    }
    Serial.print(F("    ")); Serial.print(good);
    Serial.print(F(" of ")); Serial.print(g_nbank);
    Serial.println(F(" banks hold their own data"));
    if (good < g_nbank)
        Serial.println(F("    A bank that does not is sharing a chip select with another one."));
    return good;
}

/* Per bank: the fastest setting that is clean over a real span, then one step of margin. The window
 * is found per direction and the chosen pair is confirmed together, because two single-variable
 * sweeps can each pass while the combination fails -- on the breadboard they did. */
static void stage3_timing(void)
{
    const uint32_t span = 256u * 1024u;
    Serial.println(F("\n[3] timing, per bank. 20.2 ns/nibble is the FlexSPI threshold."));
    Serial.println(F("    bank  write       read        W MB/s   R MB/s   ns/nib  controller?"));
    Serial.println(F("          (edge->used) (edge->used)"));

    for (int b = 0; b < g_nbank; b++) {
        use_bank(b); enter_quad_here();

        /* reads first, with writes held generous */
        int ri = -1;
        for (unsigned k = 0; k < NSET; k++) {
            uint32_t bad = 0;
            for (uint32_t off = 0; off < span; off += BLK) {
                for (uint32_t i = 0; i < BLK; i++) ref[i] = pat((uint8_t)b, off + i);
                WF[7](off, ref, BLK);
                RF[k](off, buf, BLK);
                for (uint32_t i = 0; i < BLK; i++) if (buf[i] != ref[i]) bad++;
            }
            if (bad == 0) { ri = (int)k; break; }
        }
        /* then writes, with reads held at the value just found plus margin */
        const int rsafe = (ri < 0) ? (int)NSET - 1 : ((ri + 1 < (int)NSET) ? ri + 1 : ri);
        int wi = -1;
        for (unsigned k = 0; k < NSET; k++) {
            uint32_t bad = 0;
            for (uint32_t off = 0; off < span; off += BLK) {
                for (uint32_t i = 0; i < BLK; i++) ref[i] = pat((uint8_t)b, off + i);
                WF[k](off, ref, BLK);
                RF[rsafe](off, buf, BLK);
                for (uint32_t i = 0; i < BLK; i++) if (buf[i] != ref[i]) bad++;
            }
            if (bad == 0) { wi = (int)k; break; }
        }

        /* one step of margin on each, which is the whole lesson of docs/41 */
        const int wsafe = (wi < 0) ? (int)NSET - 1 : ((wi + 1 < (int)NSET) ? wi + 1 : wi);
        g_wi[b] = (uint8_t)wsafe;
        g_ri[b] = (uint8_t)rsafe;

        /* Now fix the burst for this bank. The timing was chosen for margin and that lengthens a
         * burst, so the refresh limit has to be re-checked against the setting actually chosen
         * rather than against the one the burst was picked for. */
        g_burst = 96;
        for (uint32_t i = 0; i < 96; i++) ref[i] = pat((uint8_t)b, i);
        uint32_t t0 = ARM_DWT_CYCCNT; RF[rsafe](0, buf, 96); const float u96 =
            1e6f * (float)(ARM_DWT_CYCCNT - t0) / (float)F_CPU_ACTUAL;
        g_burst = 32;
        t0 = ARM_DWT_CYCCNT;          RF[rsafe](0, buf, 32); const float u32 =
            1e6f * (float)(ARM_DWT_CYCCNT - t0) / (float)F_CPU_ACTUAL;
        const float per = (u96 - u32) / 64.0f;                 /* us per payload byte */
        const float fix = u96 - per * 96.0f;                   /* us of command and dummy clocks */
        int nmax = (per > 0.0f) ? (int)((7.0f - fix) / per) : 96;
        nmax = (nmax / 16) * 16;                               /* a tidy multiple */
        if (nmax > 96) nmax = 96;
        if (nmax < 16) nmax = 16;
        g_bn[b] = (uint16_t)nmax;
        g_burst = (uint32_t)nmax;

        /* measure the chosen pair */
        uint32_t wc = 0, rc = 0, bad = 0;
        for (uint32_t off = 0; off < span; off += BLK) {
            for (uint32_t i = 0; i < BLK; i++) ref[i] = pat((uint8_t)b, off + i);
            uint32_t t = ARM_DWT_CYCCNT; WF[wsafe](off, ref, BLK); wc += ARM_DWT_CYCCNT - t;
            t = ARM_DWT_CYCCNT;          RF[rsafe](off, buf, BLK); rc += ARM_DWT_CYCCNT - t;
            for (uint32_t i = 0; i < BLK; i++) if (buf[i] != ref[i]) bad++;
        }
        const float wmb = (float)span / ((float)wc / (float)F_CPU_ACTUAL) / 1e6f;
        const float rmb = (float)span / ((float)rc / (float)F_CPU_ACTUAL) / 1e6f;
        const float ns  = 1000.0f / (rmb * 2.0f);      /* two nibbles to a byte */

        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("      "));
        if (wi < 0) Serial.print(F("--")); else Serial.print(NOPS[wi]);
        Serial.print(F("->"));  Serial.print(NOPS[wsafe]);
        Serial.print(F("        "));
        if (ri < 0) Serial.print(F("--")); else Serial.print(NOPS[ri]);
        Serial.print(F("->"));  Serial.print(NOPS[rsafe]);
        Serial.print(F("      "));
        Serial.print(wmb, 2);       Serial.print(F("    "));
        Serial.print(rmb, 2);       Serial.print(F("    "));
        g_ns[b] = bad ? 0.0f : ns;
        Serial.print(ns, 1);        Serial.print(F("       "));
        if (bad) Serial.println(F("-- never came clean"));
        else     Serial.println(ns < 20.2f ? F("IN REACH") : F("too slow"));
    }
}

/* A burst holds chip select low for its whole length and these parts only refresh while it is high,
 * for at most 8 us. Past that they read back correctly today and lose rows that have been sitting,
 * which is the one failure mode every other test here is blind to. Measured, not inferred from
 * throughput: the command, address and six dummy clocks are chip-select-low too. */
static void stage4_tcem(void)
{
    Serial.println(F("\n[4] chip select low per burst, against the 8 us refresh limit"));
    Serial.println(F("    bank   burst   read      write"));
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        g_burst = g_bn[b];
        for (uint32_t i = 0; i < g_burst; i++) ref[i] = pat((uint8_t)b, i);
        uint32_t t = ARM_DWT_CYCCNT; WF[g_wi[b]](0, ref, g_burst); const uint32_t wcy = ARM_DWT_CYCCNT - t;
        t = ARM_DWT_CYCCNT;          RF[g_ri[b]](0, buf, g_burst); const uint32_t rcy = ARM_DWT_CYCCNT - t;
        const float rus = 1e6f * (float)rcy / (float)F_CPU_ACTUAL;
        const float wus = 1e6f * (float)wcy / (float)F_CPU_ACTUAL;
        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("      ")); Serial.print(g_bn[b]);
        Serial.print(F(" B    ")); Serial.print(rus, 2);
        Serial.print(F(" us   ")); Serial.print(wus, 2);
        Serial.print(F(" us   "));
        const float worst = rus > wus ? rus : wus;
        Serial.print(worst > 8.0f ? F("OVER") : (worst > 7.0f ? F("thin") : F("safe")));

        Serial.println();
    }
}

/* The whole flat space, across every bank, with each bank at its own qualified timing. This is what
 * proves the address mapping and the bank switching, not just the chips. */
static void stage5_whole(void)
{
    const uint32_t total = (uint32_t)g_nbank * BANKSZ;
    Serial.print(F("\n[5] the whole address space: "));
    Serial.print(total / (1024u * 1024u));
    Serial.println(F(" MB written then verified"));

    uint64_t wc = 0, rc = 0;
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        g_burst = g_bn[b];
        for (uint32_t off = 0; off < BANKSZ; off += BLK) {
            const uint32_t flat = (uint32_t)b * BANKSZ + off;
            for (uint32_t i = 0; i < BLK; i++) ref[i] = (uint8_t)((flat + i) * 0x9Du + 0x3Bu);
            const uint32_t t = ARM_DWT_CYCCNT;
            WF[g_wi[b]](off, ref, BLK);
            wc += ARM_DWT_CYCCNT - t;
        }
    }
    uint32_t bad = 0, badbank = 0;
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        g_burst = g_bn[b];
        uint32_t bb = 0;
        for (uint32_t off = 0; off < BANKSZ; off += BLK) {
            const uint32_t flat = (uint32_t)b * BANKSZ + off;
            const uint32_t t = ARM_DWT_CYCCNT;
            RF[g_ri[b]](off, buf, BLK);
            rc += ARM_DWT_CYCCNT - t;
            for (uint32_t i = 0; i < BLK; i++)
                if (buf[i] != (uint8_t)((flat + i) * 0x9Du + 0x3Bu)) bb++;
        }
        if (bb) { badbank++; Serial.print(F("    bank ")); Serial.print(b);
                  Serial.print(F(": ")); Serial.print(bb); Serial.println(F(" wrong")); }
        bad += bb;
    }
    const float ws = (float)wc / (float)F_CPU_ACTUAL;
    const float rs = (float)rc / (float)F_CPU_ACTUAL;
    Serial.print(F("    wrong bytes: ")); Serial.print(bad);
    Serial.print(F(" of ")); Serial.println(total);
    Serial.print(F("    aggregate write ")); Serial.print((float)total / ws / 1e6f, 2);
    Serial.print(F(" MB/s, read ")); Serial.print((float)total / rs / 1e6f, 2);
    Serial.println(F(" MB/s"));
    Serial.print(F("    bound on the error rate: under "));
    Serial.print(1e9f / (float)total, 2);
    Serial.println(F(" per billion bytes from this pass alone"));
    if (badbank == 0)
        Serial.println(F("    Every bank holds every byte. Run psram_soak next to push that bound down."));
}

static uint64_t sk_bytes[MAXBANK];
static uint64_t sk_errs[MAXBANK];
static uint32_t sk_pass;
static uint32_t sk_worst[MAXBANK];

static void stage6_soak(void)
{
    sk_pass++;
    for (int b = 0; b < g_nbank; b++) {
        use_bank(b);
        g_burst = g_bn[b];
        const uint32_t seed = 0x3Bu + sk_pass * 0x51u;       /* a new pattern every round, so a pass
                                                              * cannot succeed by reading back what the
                                                              * previous one left behind */
        for (uint32_t off = 0; off < BANKSZ; off += BLK) {
            for (uint32_t i = 0; i < BLK; i++) ref[i] = (uint8_t)((off + i) * 0x9Du + seed);
            WF[g_wi[b]](off, ref, BLK);
        }
        uint32_t bad = 0;
        for (uint32_t off = 0; off < BANKSZ; off += BLK) {
            RF[g_ri[b]](off, buf, BLK);
            for (uint32_t i = 0; i < BLK; i++)
                if (buf[i] != (uint8_t)((off + i) * 0x9Du + seed)) bad++;
        }
        sk_bytes[b] += BANKSZ;
        sk_errs[b]  += bad;
        if (bad > sk_worst[b]) sk_worst[b] = bad;
    }

    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.print(F("  soak, ")); Serial.print(millis() / 60000u);
    Serial.print(F(" min, ")); Serial.print(sk_pass);
    Serial.print(F(" passes of each of ")); Serial.print(g_nbank);
    Serial.println(F(" banks"));
    Serial.println(F("=================================================================="));
    Serial.println(F("    bank   MB tested   errors   worst pass   rate"));
    for (int b = 0; b < g_nbank; b++) {
        const uint32_t mb = (uint32_t)(sk_bytes[b] >> 20);
        Serial.print(F("      ")); Serial.print(b);
        Serial.print(F("      "));
        if (mb < 10000) Serial.print(' ');
        if (mb < 1000)  Serial.print(' ');
        if (mb < 100)   Serial.print(' ');
        if (mb < 10)    Serial.print(' ');
        Serial.print(mb);                      Serial.print(F("      "));
        Serial.print((uint32_t)sk_errs[b]);     Serial.print(F("        "));
        Serial.print(sk_worst[b]);              Serial.print(F("        "));
        if (sk_errs[b]) {
            Serial.print(1e9f * (float)(double)sk_errs[b] / (float)(double)sk_bytes[b], 2);
            Serial.println(F(" per billion"));
        } else {
            Serial.print(F("under "));
            Serial.print(1e9f / (float)(double)sk_bytes[b], 2);
            Serial.println(F(" per billion so far"));
        }
    }
    Serial.println(F("  A bank with no errors yet is bounded, not proven, and the bound falls as"));
    Serial.println(F("  this runs. Leave it going; the rate that matters is well below what one"));
    Serial.println(F("  pass can resolve."));
}

static bool done_once = false;

void loop()
{
    if (done_once) { stage6_soak(); return; }

    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  perfboard bring-up. Flash this first; it assumes nothing."));
    Serial.println(F("=================================================================="));

    stage1_discover();
    if (g_nbank == 0) {
        Serial.println(F("\n  Nothing answered at all, at 250 kHz, which no timing setting can explain."));
        Serial.println(F("  In order of likelihood:"));
        Serial.println(F("    1. power or ground missing at the chips, or VCC and VSS swapped"));
        Serial.println(F("    2. SCLK not reaching them -- it is Teensy pin 53, NOT pin 52"));
        Serial.println(F("    3. SIO0 and SIO1 swapped, so commands go out on the reply line"));
        Serial.println(F("    4. every chip select stuck high, or the decoder enable on G1 rather"));
        Serial.println(F("       than G2A -- G1 is active high and holds the decoder off"));
        Serial.println(F("  Pin 53 is the one to check first. It is the pad a pinout card is most"));
        Serial.println(F("  often misread on, and it is verified twice in docs/44 from the core."));
        Serial.println(F("\n=== retrying in 20 s, in case something is being rewired ==="));
        delay(20000);
        return;
    }

    const int indep = stage2_independent();
    stage3_timing();
    stage4_tcem();
    if (indep == g_nbank) {
        stage5_whole();
    } else {
        Serial.println(F("\n[5] skipped: banks are not independent, so a flat address space across"));
        Serial.println(F("    them would be measuring the same chip several times over."));
    }

    Serial.println(F("\n--- what you built ---"));
    Serial.print(F("  ")); Serial.print(g_nbank);
    Serial.print(F(" chips, ")); Serial.print((uint32_t)g_nbank * 8u);
    Serial.println(F(" MB usable."));
    int inreach = 0;
    float best = 0.0f;
    for (int b = 0; b < g_nbank; b++) {
        if (g_ns[b] <= 0.0f) continue;
        if (g_ns[b] < 20.2f) inreach++;
        if (best == 0.0f || g_ns[b] < best) best = g_ns[b];
    }
    if (best > 0.0f) {
        Serial.print(F("  fastest bank: ")); Serial.print(best, 1);
        Serial.println(F(" ns per nibble. The FlexSPI threshold is 20.2."));
    }
    Serial.print(F("  banks under that threshold: ")); Serial.print(inreach);
    Serial.print('/'); Serial.println(g_nbank);
    if (inreach) {
        Serial.println(F("  Run psram_clock_sweep next. The controller brings memory mapping and"));
        Serial.println(F("  about 33 MB/s, and it reaches only a bank whose select it drives itself"));
        Serial.println(F("  -- which on this wiring is the one on pin 48, not the decoder banks."));
    } else {
        Serial.println(F("  None under 20.2 yet, so the bit-banged driver is the path for now. The"));
        Serial.println(F("  lever is a ground return beside the data lines, not the pad drive:"));
        Serial.println(F("  psram_pads measured all eight drive and slew settings as identical."));
    }

    Serial.println(F("\n  Bring-up complete. Soaking every bank from here on, indefinitely."));
    Serial.println(F("  Stop it whenever; the bound printed is the evidence so far."));
    done_once = true;
}

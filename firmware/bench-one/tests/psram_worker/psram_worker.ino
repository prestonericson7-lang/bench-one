/* HOW MANY INSTANTIATIONS THIS FILE CAN AFFORD, AND WHY THAT IS A TIMING QUESTION.
 *
 * Adding the 0x0B read path cost the two quad banks 21% on a setting that had not changed: at write 8,
 * read 8, burst 64 the same chip went from 9.70 MB/s to 7.71, with a bit-identical partial sum. It was
 * not the wiring, the clock, the burst or the code placement -- both builds put these loops in ITCM and
 * both keep the buffers in DTCM. It was the compiler.
 *
 * The symbol table says it outright. bread<8> is 0x29C bytes in the fast build and 0x244 in the slow
 * one, from identical source. GCC unrolls the payload loop when the translation unit is small enough to
 * justify it and stops when it is not, and the unrolled form is 21% faster. So every template
 * instantiation added anywhere in this file is a change to the timing of every bus loop in it.
 *
 * That is docs/41 again at a larger scale: a no-op count is a timing specification for one exact
 * instruction sequence, and here the compiler rewrote the sequence in response to code that has nothing
 * to do with it. It is also why FASTRUN did not help -- these were already in ITCM, so there was never a
 * fetch penalty to remove.
 *
 * The practical rule: keep the number of instantiations down, and treat adding one as a timing change
 * that requires requalification. The diagnostic tail of 32 to 128 no-ops has done its job -- it proved
 * that slowing down does not rescue a bank that will not read -- and it costs 25 instantiations, so it
 * is gone.
 */

/* BUS-VARIANT: the burst asserts chip select and then waits g_csu no-ops before the first clock
 * edge, which no other sketch in the tree does. The six banks behind the 74LVC138A answer the slow
 * identity probe and return all zeroes to every fast transfer at every timing setting, and the reason
 * is arithmetic: "GPIO9_DR = b" is followed immediately by put_nib, whose first two stores raise the
 * clock about three to seven nanoseconds later. The decoder needs up to six nanoseconds to propagate
 * its enable to the selected output, so the first command bit is clocked into a chip that is not yet
 * selected, every bit after it is shifted, the command is never recognised and the chip never drives
 * SIO1. The two onboard banks have no decoder in the path and pass on the same margin. The delay sits
 * OUTSIDE the nibble loop, so the no-op counts still describe the same instruction sequence they
 * always did -- but the function bodies differ, so it is declared here rather than hidden. */

/* ===========================================================================================
 *  psram_worker -- the Teensy as a math engine, and nothing else.
 *
 *  BOARD    Teensy 4.1, 8x ESP-PSRAM64H = 64 MB. 2 onboard (CS0/CS1), 6 behind a 74LVC138A.
 *  LINK     Serial2 (pins 7/8) at 1 Mbaud to the Luckfox's /dev/ttyS3.
 *
 *  The bus layer below is copied from psram_perfboard unchanged, because a no-op count is a
 *  timing specification for one exact instruction sequence and not for a number. See
 *  .claude/verify-psram-bus.py, which checks that mechanically.
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
static inline void bus_out(void) { GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1); }
static inline void data_in(void) { GPIO9_GDIR &= ~B_DATA; }
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
template <int S> static inline void addr_out(uint32_t b, uint32_t a)
{
    put_nib<S>(b, (a >> 20) & 0xF); put_nib<S>(b, (a >> 16) & 0xF);
    put_nib<S>(b, (a >> 12) & 0xF); put_nib<S>(b, (a >>  8) & 0xF);
    put_nib<S>(b, (a >>  4) & 0xF); put_nib<S>(b, (a      ) & 0xF);
}

/* ===========================================================================================
 *  THE TEENSY AS A MATH ENGINE AND NOTHING ELSE
 *
 *  This end holds 64 MB of weights on a bus it drives itself, and a multiply-accumulate kernel at
 *  2.07 cycles a weight. That is all it is for. It does not discover its own banks, does not decide
 *  its own timing, does not run its own benchmark, does not keep a log and does not print a report.
 *  Every one of those is a decision, and decisions belong to the board with an operating system and
 *  33 MB of DDR2 to think in.
 *
 *  The split is not tidiness. A worker that decides things has state, and state is what makes a
 *  measurement drift between runs -- the same reason a daemon quietly polling hardware ruins a bench.
 *  This one remembers only which bank is selected and how fast to clock it, and both were told to it.
 *
 *  THE COMMANDS, one line in and one line out, at 1 Mbaud on Serial2 (pins 7 and 8):
 *
 *    I                       identity: name, bank count, bank size, bit widths, clock rate
 *    P <kind> <y>            probe one select. kind 0=CS0 1=CS1 2=decoder. -> "P <ok> <id0> <id1>"
 *    B <kind> <y>            select a bank and put it in quad mode
 *    T <wi> <ri> <burst>     set write index, read index and burst for the selected bank
 *    U <nops>                chip-select setup: no-ops between asserting select and the first clock
 *    D <n>                   wait cycles after the read address, before the first data nibble
 *    Q <mode>                bus mode: 0 quad on four lines, 1 single-bit with command 0x03,
 *                            2 single-bit with the fast-read command 0x0B. Takes effect at the next
 *                            B, because it decides whether the chip is put into quad mode.
 *    J <n>                   wait cycles for the 0x0B fast read
 *    K <mode>                pad configuration for the four data lines: 0 the core default,
 *                            1 keeper off, 2 keeper off and hysteresis on, 3 as 2 plus a pull-up
 *    N <tag>                 tag the pattern, so each bank can be filled with data only it should
 *                            hold and a read that returns somebody else is visible
 *    F <addr> <len>          fill with the pattern. -> "F <cycles>". The host never sends weights
 *                            over the UART: it sends the rule that generates them.
 *    M <addr> <len> <bits>   read and multiply-accumulate. bits is 4 or 2. -> "M <sum> <cycles>"
 *    R <nbytes>              time ONE read burst of this length. -> "R <nbytes> <cycles>"
 *    W <nbytes>              time ONE write burst of this length. -> "W <nbytes> <cycles>"
 *    X <addr> <n>            read n bytes and hand them back as hex, n <= 24. -> "X <hex...>"
 *    S <addr>                write four bytes and read them back in SINGLE-bit SPI, same chip
 *                            select path as the fast read. -> "S <hex...>"
 *    G <addr>                read four bytes in SINGLE-bit SPI without writing anything first, so
 *                            what a QUAD write left behind can be read over a path that works.
 *                            -> "G <hex...>"
 *    V <addr> <len>          verify the pattern read back. -> "V <wrong> <cycles>"
 *    L <addr> <len>          verify, and count the wrong bits per DATA LINE over the whole span.
 *                            -> "L <sio0> <sio1> <sio2> <sio3> <nibbles>"
 *
 *  Nothing here loops forever, nothing runs unasked, and every reply carries the microseconds it took
 *  so the host times the work rather than the round trip.
 *
 *  QUANTISATION lives here because it is arithmetic, not policy. The host picks 4 or 2 per call. At a
 *  bus-limited rate, two bits a weight is exactly twice the multiply-accumulates for the same bytes,
 *  because every byte costs the same to move whatever is packed into it.
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

enum { SEL_CS0, SEL_CS1, SEL_DEC };

static uint8_t  buf[BLK] __attribute__((aligned(32)));
static uint32_t g_csbit = B_SS0;
static uint32_t g_burst = 32;
static uint8_t  g_wi = 2, g_ri = 4;
static uint8_t  g_kind = 0, g_y = 0;
static uint32_t g_csu = 0;          /* no-ops between chip select falling and the first clock edge */

static inline void cs_setup(void)
{
    for (uint32_t k = 0; k < g_csu; k++) __asm__ volatile("nop");
}

/* WHERE THE READ IS SAMPLED, AND HOW LONG IT WAITS FIRST.
 *
 * The six external chips take a quad write correctly -- read back over a single-bit path that works,
 * every one of them returns the right bytes with its own tag from four separate addresses -- and then
 * return wrong nibbles to the quad read. The errors are spread evenly over all four data lines, about
 * a quarter of the nibbles each, which is not what a broken wire looks like: one bad line puts all its
 * errors in one bit position. Evenly spread errors across all four lines means the whole nibble is
 * being sampled at the wrong moment.
 *
 * Two things decide that moment. One was the instant within the clock high phase at which GPIO9_PSR is
 * read, and it was swept across seven positions on every bank and changed nothing: the window is wide.
 * That knob has been removed, because the loop implementing it sat inside the nibble loop and added a
 * compare and a branch to every clock phase even at zero -- a different timing specification wearing
 * the same no-op count, which is precisely the fault docs/41 exists to prevent. It showed: the two good
 * banks qualified at read 12 over 32 kB and then returned a different sum on every pass over 1 MB.
 *
 * The other is the number of wait cycles after the read address, and it stays, because it sits outside
 * the payload loop. The sweep settled it at six: the onboard banks are clean at six and totally wrong
 * at four, five, seven, eight and ten, and no other value helps any bank. */
static uint32_t g_dummy = 6;
static uint32_t g_dummy1 = 8;           /* wait cycles for the 0x0B fast read */

/* WHAT THE PAD DOES WHILE IT IS LISTENING.
 *
 * A read turns the four data pads into inputs, and pinMode leaves the bus keeper switched on. A keeper
 * holds whatever the pad last drove -- here, the final nibble of the address -- against anything trying
 * to change it. The two chips soldered to the board overcome it without noticing. Six chips at the far
 * end of ribbon, through 22 ohm resistors, driving into the capacitance of eight chip pins, may not:
 * the keeper is pulling the line back towards the last thing the TEENSY said while the chip is trying
 * to say something else. That would break reads and leave writes untouched, which is exactly the shape
 * of this fault.
 *
 * Hysteresis is the other half. Without it the input threshold sits at one point and a slow, ringing
 * edge crosses it several times; with it the threshold moves away after the first crossing.
 *
 * Neither is assumed to help. Both are switchable and the host sweeps them against a read-back. */
#define PADC_DEFAULT  0x10B0u
#define PADC_NOKEEP   0x0039u             /* speed 3, drive 7, fast slew, no keeper, no pull */
#define PADC_HYS      0x10039u            /* the same with hysteresis on the input */
#define PADC_PULLUP   0x1B039u            /* hysteresis, and a 22k pull-up instead of a keeper */

static uint8_t g_padmode;

static void set_pads(uint8_t mode)
{
    g_padmode = mode;
    uint32_t v = PADC_DEFAULT;
    if (mode == 1) v = PADC_NOKEEP;
    else if (mode == 2) v = PADC_HYS;
    else if (mode == 3) v = PADC_PULLUP;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_26 = v;   /* pin 52, SIO0 */
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_27 = v;   /* pin 49, SIO1 */
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_28 = v;   /* pin 50, SIO2 */
    IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_29 = v;   /* pin 54, SIO3 */
    __asm__ volatile("dsb");
}

static inline uint32_t cbase(void)
{
    return (idle | B_SS0 | B_SS1 | B_DEC) & ~g_csbit & ~B_CLK & ~B_DATA;
}

static void select_route(uint8_t kind, uint8_t y)
{
    /* restate the pad configuration every time: anything that has touched a pin as an input leaves it
     * configured differently from what these register writes assume, and GDIR alone does not put that
     * back. Six banks looked dead for an afternoon over exactly this. */
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
    g_kind = kind; g_y = y;
    set_pads(g_padmode);                 /* pinMode above rewrites pad control; put it back */
    delayMicroseconds(3);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | B_DEC | (1u << 28) | (1u << 29)) & ~B_CLK;
    bus_out();
}

template <int S> static void bwrite(uint32_t a, const uint8_t *s, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b; cs_setup();
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
        bus_out(); const uint32_t b = cbase(); GPIO9_DR = b; cs_setup();
        put_nib<S>(b, 0xE); put_nib<S>(b, 0xB); addr_out<S>(b, a);
        data_in();
        for (uint32_t k = 0; k < g_dummy; k++)
            { GPIO9_DR = b | B_CLK; spin<S>(); GPIO9_DR = b; spin<S>(); }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t hi = get_nib<S>(b);
            d[i] = (uint8_t)((hi << 4) | get_nib<S>(b));
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

/* The table runs far past anything usable on purpose. 24 no-ops is about 10.7 MHz and is the slowest
 * setting any bank has ever needed; 128 is about 2.3 MHz and exists only to answer one question. The
 * six external banks take a quad write correctly and return wrong nibbles to a quad read, spread over
 * all four data lines, at every setting up to 24. If the far end of that wiring simply cannot be
 * clocked that fast, one of these will come back clean. If none does, clock speed is not the fault and
 * the answer is physical. Either way the sweep decides it rather than an argument. */
/* THE SINGLE-BIT READ, BECAUSE HALF SPEED BEATS NO CAPACITY.
 *
 * Six of the eight chips take a quad write perfectly and return wrong nibbles to a quad read, and
 * nothing in the firmware changes that: not the clock from 10.7 MHz down to 2.3, not the burst from 8
 * bytes to 64, not the wait cycles, not the sample instant, not the bus keeper, the hysteresis or the
 * drive strength. But every one of them answers a SINGLE-bit read correctly, which is the same clock,
 * the same ground, the same supply and the same command phase with one difference: the chip drives one
 * line instead of four.
 *
 * So there is a path to those 48 MB. It is a quarter of the bits per clock, which at the clock a quad
 * read needs is about half the bytes a second -- and half the bytes a second out of 48 MB is worth
 * more than all of the bytes a second out of nothing. Capacity is what this machine is for.
 *
 * The write cannot stay quad. A chip in quad mode does not understand a single-bit command, so the two
 * cannot be mixed on one chip: choosing the single-bit read means the whole bank runs single-bit, and
 * the chip is never given the 0x35 that puts it into quad mode. That was worth finding out the
 * expensive way -- a single-bit read issued to a chip still in quad mode returns garbage from every
 * bank, including the two that are perfect, which looked for a moment like the new path simply did not
 * work. It was the mode, not the path.
 *
 * Writes at one bit a clock cost four times the clocks. That is paid once, when the weights are loaded,
 * and never again during inference. */
template <int S> static void bwrite1(uint32_t a, const uint8_t *src, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t b = cbase() | (1u << 28) | (1u << 29);
        GPIO9_DR = b; cs_setup();
        s_byte<S>(b, 0x02);
        s_byte<S>(b, (uint8_t)(a >> 16)); s_byte<S>(b, (uint8_t)(a >> 8)); s_byte<S>(b, (uint8_t)a);
        for (uint32_t i = 0; i < n; i++) s_byte<S>(b, src[i]);
        GPIO9_DR = idle;
        a += n; src += n; len -= n;
    }
}
/* 0x0B RATHER THAN 0x03, BECAUSE 0x03 IS THE SLOW ONE.
 *
 * Five of the seven usable banks read one bit to the clock, and they are the whole critical path: the
 * two quad banks finish a megabyte in 103 ms and the five single-bit ones take 386 ms each. Anything
 * that moves that number moves the machine.
 *
 * Command 0x03 is specified to 33 MHz on this part and has no wait cycles. 0x0B is the same read with
 * wait cycles after the address, and exists precisely so the clock can go faster than 0x03 allows --
 * the wait gives the array time to deliver the first byte instead of requiring a slow clock throughout.
 * It costs eight clocks once per burst and may buy a faster clock on every one of the 128 payload
 * clocks after it.
 *
 * Whether it actually does is a measurement, not an argument: the wait count is swept and so is the
 * clock, and the only thing accepted is a full-span read-back with zero wrong bytes. The quad read
 * already uses a wait count from the same datasheet table and the sweep confirmed its value exactly,
 * which is the reason to trust the table enough to try this at all. */
template <int S> static void bread1f(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t b = cbase() | (1u << 28) | (1u << 29);
        GPIO9_DR = b; cs_setup();
        s_byte<S>(b, 0x0B);
        s_byte<S>(b, (uint8_t)(a >> 16)); s_byte<S>(b, (uint8_t)(a >> 8)); s_byte<S>(b, (uint8_t)a);
        GPIO9_GDIR &= ~(1u << 27);
        for (uint32_t k = 0; k < g_dummy1; k++)
            { GPIO9_DR = b | B_CLK; spin<S>(); GPIO9_DR = b; spin<S>(); }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                GPIO9_DR = b | B_CLK; spin<S>();
                v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));
                GPIO9_DR = b;         spin<S>();
            }
            d[i] = v;
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

template <int S> static void bread1(uint32_t a, uint8_t *d, uint32_t len)
{
    while (len) {
        const uint32_t n = (len > g_burst) ? g_burst : len;
        bus_out();
        const uint32_t b = cbase() | (1u << 28) | (1u << 29);   /* SIO2/3 idle high in single mode */
        GPIO9_DR = b; cs_setup();
        s_byte<S>(b, 0x03);
        s_byte<S>(b, (uint8_t)(a >> 16)); s_byte<S>(b, (uint8_t)(a >> 8)); s_byte<S>(b, (uint8_t)a);
        GPIO9_GDIR &= ~(1u << 27);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                GPIO9_DR = b | B_CLK; spin<S>();
                v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));
                GPIO9_DR = b;         spin<S>();
            }
            d[i] = v;
        }
        GPIO9_DR = idle; bus_out();
        a += n; d += n; len -= n;
    }
}

static const int NOPS[] = { 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 24 };
#define NSET (sizeof(NOPS) / sizeof(NOPS[0]))
static void (*const WF[NSET])(uint32_t, const uint8_t *, uint32_t) = {
    bwrite<4>, bwrite<5>, bwrite<6>, bwrite<7>, bwrite<8>, bwrite<9>, bwrite<10>,
    bwrite<11>, bwrite<12>, bwrite<13>, bwrite<14>, bwrite<16>, bwrite<18>,
    bwrite<20>, bwrite<24> };
static void (*const RQ[NSET])(uint32_t, uint8_t *, uint32_t) = {
    bread<4>, bread<5>, bread<6>, bread<7>, bread<8>, bread<9>, bread<10>,
    bread<11>, bread<12>, bread<13>, bread<14>, bread<16>, bread<18>,
    bread<20>, bread<24> };
static void (*const R1[NSET])(uint32_t, uint8_t *, uint32_t) = {
    bread1<4>, bread1<5>, bread1<6>, bread1<7>, bread1<8>, bread1<9>, bread1<10>,
    bread1<11>, bread1<12>, bread1<13>, bread1<14>, bread1<16>, bread1<18>,
    bread1<20>, bread1<24> };

static void (*const W1[NSET])(uint32_t, const uint8_t *, uint32_t) = {
    bwrite1<4>, bwrite1<5>, bwrite1<6>, bwrite1<7>, bwrite1<8>, bwrite1<9>, bwrite1<10>,
    bwrite1<11>, bwrite1<12>, bwrite1<13>, bwrite1<14>, bwrite1<16>, bwrite1<18>,
    bwrite1<20>, bwrite1<24> };

static void (*const RZ[NSET])(uint32_t, uint8_t *, uint32_t) = {
    bread1f<4>, bread1f<5>, bread1f<6>, bread1f<7>, bread1f<8>, bread1f<9>, bread1f<10>,
    bread1f<11>, bread1f<12>, bread1f<13>, bread1f<14>, bread1f<16>, bread1f<18>,
    bread1f<20>, bread1f<24> };

static uint8_t g_rmode;                 /* 0 quad, 1 single-bit 0x03, 2 single-bit 0x0B */

/* One place decides how a bank is read, so what the sweep timed is what the work runs. */
static inline void do_read(uint32_t a, uint8_t *d, uint32_t len)
{
    if (g_rmode == 2)      RZ[g_ri](a, d, len);
    else if (g_rmode == 1) R1[g_ri](a, d, len);
    else                   RQ[g_ri](a, d, len);
}

static inline void do_write(uint32_t a, const uint8_t *src, uint32_t len)
{
    if (g_rmode) W1[g_wi](a, src, len);      /* both single-bit modes write with 0x02 */
    else         WF[g_wi](a, src, len);
}

static void enter_quad_here(void)
{
    const uint32_t b = cbase();
    const uint32_t b4 = b | (1u << 28) | (1u << 29);
    bus_out();
    GPIO9_DR = b;  cs_setup(); put_nib<10>(b, 0xF); put_nib<10>(b, 0x5); GPIO9_DR = idle;
    delayMicroseconds(5);
    GPIO9_DR = b4; cs_setup(); s_byte<10>(b4, 0x66); GPIO9_DR = idle; delayMicroseconds(5);
    GPIO9_DR = b4; cs_setup(); s_byte<10>(b4, 0x99); GPIO9_DR = idle; delay(2);
    /* 0x35 is what puts the chip into quad mode. In single-bit mode it is simply not sent, and the
     * chip stays where the reset left it: one bit a clock, command 0x02 out and 0x03 back. */
    if (!g_rmode) {
        GPIO9_DR = b4; cs_setup(); s_byte<10>(b4, 0x35); GPIO9_DR = idle;
    }
    delayMicroseconds(50);
}

/* ---------------------------------------------------------------------------------------------
 *  the slow, safe probe -- copied verbatim from psram_perfboard
 *
 *  digitalWriteFast at roughly 250 kHz. A register-level rewrite of this same thing was failing in
 *  every configuration while this one found all eight chips, and a once-per-boot identity read has no
 *  reason to be fast. The chip may still be in quad mode from a previous sketch, so the quad exit and
 *  reset are sent in the mode the chip might actually be in; skipping that makes a working chip look
 *  absent.
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

static bool probe_id(uint8_t kind, uint8_t y, uint8_t *id)
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

    /* leave the bus the way the fast path expects to find it */
    for (int q = 48; q <= 54; q++) pinMode(q, OUTPUT);
    bus_out();
    return id[0] == 0x0D && id[1] == 0x5D;
}

/* ---------------------------------------------------------------------------------------------
 *  the arithmetic: the only thing this board decides
 * ------------------------------------------------------------------------------------------ */
static int8_t   xvec[2048];
static uint32_t xpack4[256 * 4];
static uint32_t xpack2[128 * 8];
static int32_t  xsum_all;

static inline uint32_t sxtb16(uint32_t x)
{ uint32_t r; __asm__("sxtb16 %0, %1" : "=r"(r) : "r"(x)); return r; }
static inline uint32_t sxtb16r8(uint32_t x)
{ uint32_t r; __asm__("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x)); return r; }
static inline int32_t smlad_(uint32_t a, uint32_t b, int32_t acc)
{ int32_t r; __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc)); return r; }

static void build_packs(void)
{
    for (uint32_t g = 0; g < 256; g++) {
        const int8_t *a = &xvec[g * 8];
        xpack4[g*4+0] = ((uint32_t)(a[0] & 0xFFFF)) | ((uint32_t)(a[4] & 0xFFFF) << 16);
        xpack4[g*4+1] = ((uint32_t)(a[2] & 0xFFFF)) | ((uint32_t)(a[6] & 0xFFFF) << 16);
        xpack4[g*4+2] = ((uint32_t)(a[1] & 0xFFFF)) | ((uint32_t)(a[5] & 0xFFFF) << 16);
        xpack4[g*4+3] = ((uint32_t)(a[3] & 0xFFFF)) | ((uint32_t)(a[7] & 0xFFFF) << 16);
    }
    for (uint32_t g = 0; g < 128; g++) {
        const int8_t *a = &xvec[g * 16];
        for (int j = 0; j < 4; j++) {
            xpack2[g*8 + 2*j + 0] = ((uint32_t)(a[0 + j] & 0xFFFF))
                                  | ((uint32_t)(a[8 + j] & 0xFFFF) << 16);
            xpack2[g*8 + 2*j + 1] = ((uint32_t)(a[4 + j] & 0xFFFF))
                                  | ((uint32_t)(a[12 + j] & 0xFFFF) << 16);
        }
    }
    xsum_all = 0;
    for (int i = 0; i < 2048; i++) xsum_all += xvec[i];
}

/* four bits: two weights a byte, eight per 32-bit load */
static inline int32_t mac4(const uint8_t *w, uint32_t nbytes)
{
    int32_t a0 = 0, a1 = 0;
    uint32_t g = 0;
    for (uint32_t i = 0; i + 8 <= nbytes; i += 8) {
        const uint32_t *P = &xpack4[g * 4];
        const uint32_t v0 = *(const uint32_t *)(w + i);
        const uint32_t v1 = *(const uint32_t *)(w + i + 4);
        const uint32_t L0 = v0 & 0x0F0F0F0Fu, H0 = (v0 >> 4) & 0x0F0F0F0Fu;
        const uint32_t L1 = v1 & 0x0F0F0F0Fu, H1 = (v1 >> 4) & 0x0F0F0F0Fu;
        a0 = smlad_(sxtb16(L0),   P[0], a0);
        a0 = smlad_(sxtb16r8(L0), P[1], a0);
        a0 = smlad_(sxtb16(H0),   P[2], a0);
        a0 = smlad_(sxtb16r8(H0), P[3], a0);
        a1 = smlad_(sxtb16(L1),   P[4], a1);
        a1 = smlad_(sxtb16r8(L1), P[5], a1);
        a1 = smlad_(sxtb16(H1),   P[6], a1);
        a1 = smlad_(sxtb16r8(H1), P[7], a1);
        g = (g + 2) & 255u;
    }
    return a0 + a1 - 8 * (int32_t)(nbytes / 1024u) * xsum_all;
}

/* two bits: FOUR weights a byte. Same instruction count per weight, half the bytes across the bus,
 * so at a bus-limited rate this is twice the multiply-accumulates per second. */
static inline int32_t mac2(const uint8_t *w, uint32_t nbytes)
{
    int32_t a0 = 0, a1 = 0;
    uint32_t g = 0;
    for (uint32_t i = 0; i + 4 <= nbytes; i += 4) {
        const uint32_t *P = &xpack2[g * 8];
        const uint32_t v = *(const uint32_t *)(w + i);
        const uint32_t l0 = v & 0x03030303u;
        const uint32_t l1 = (v >> 2) & 0x03030303u;
        const uint32_t l2 = (v >> 4) & 0x03030303u;
        const uint32_t l3 = (v >> 6) & 0x03030303u;
        a0 = smlad_(sxtb16(l0),   P[0], a0);
        a1 = smlad_(sxtb16r8(l0), P[1], a1);
        a0 = smlad_(sxtb16(l1),   P[2], a0);
        a1 = smlad_(sxtb16r8(l1), P[3], a1);
        a0 = smlad_(sxtb16(l2),   P[4], a0);
        a1 = smlad_(sxtb16r8(l2), P[5], a1);
        a0 = smlad_(sxtb16(l3),   P[6], a0);
        a1 = smlad_(sxtb16r8(l3), P[7], a1);
        g = (g + 1) & 127u;
    }
    return a0 + a1 - 2 * (int32_t)(nbytes / 512u) * xsum_all;
}

/* THE TAG IS HOW YOU FIND OUT WHO ANSWERED.
 *
 * The quad write demonstrably lands on the external chips -- a single-bit read, over a path that
 * works, returns the right bytes from four different addresses. Only the quad read comes back wrong.
 * A wire does not know which direction data is travelling, so a path that works one way and not the
 * other is not a broken wire: the likeliest remaining cause is two chips driving the bus at once.
 * Writing to two chips is invisible, because both get the same bytes; reading from two is garbage.
 *
 * With a per-bank tag, every chip holds data only it should hold, and a read that returns another
 * bank's pattern names the chip that is wrongly selected. Without it every bank holds identical bytes
 * and a collision is unobservable -- which is why it went unseen this long. */
static uint8_t g_tag;

static inline uint8_t pat(uint32_t i)
{
    return (uint8_t)(i * 0x9Du + 0x3Bu + (uint32_t)g_tag * 0x51u);
}

/* ======================================================================================== */

void setup()
{
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
    build_packs();

    Serial2.begin(1000000);          /* UART3 to the Luckfox */
    Serial.begin(115200);            /* USB, for a human watching. Never used for control. */
    delay(300);
    Serial.println(F("psram_worker up. math only; the Luckfox decides everything else."));
    Serial.println(F("every line in and out of the link is echoed here, > in and < out."));
}

static char line[64];
static uint8_t ln;

static long arg(const char *s, int which)
{
    int seen = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ' ') { seen++; if (seen == which) return atol(p + 1); }
    }
    return 0;
}

static char out[96];

static void say(const char *s)
{
    Serial2.println(s);
    Serial.print(F("< ")); Serial.println(s);
}

static void handle(const char *c)
{
    Serial.print(F("> ")); Serial.println(c);
    const uint32_t t0 = ARM_DWT_CYCCNT;
    switch (c[0]) {
    case 'I': case 'i':
        snprintf(out, sizeof(out),
                 "I bench-one worker 1 nsel 10 banksz %lu blk %lu bits 4,2 fcpu %lu",
                 (unsigned long)BANKSZ, (unsigned long)BLK, (unsigned long)F_CPU_ACTUAL);
        say(out);
        break;

    case 'P': case 'p': {
        uint8_t id[8];
        for (int i = 0; i < 8; i++) id[i] = 0;
        const bool ok = probe_id((uint8_t)arg(c, 1), (uint8_t)arg(c, 2), id);
        snprintf(out, sizeof(out), "P %d %u %u %u %u", ok ? 1 : 0,
                 id[0], id[1], id[2], id[3]);
        say(out);
        break;
    }

    case 'B': case 'b':
        select_route((uint8_t)arg(c, 1), (uint8_t)arg(c, 2));
        enter_quad_here();
        say("B ok");
        break;

    case 'T': case 't': {
        long wi = arg(c, 1), ri = arg(c, 2), bu = arg(c, 3);
        if (wi < 0) wi = 0; if (wi >= (long)NSET) wi = NSET - 1;
        if (ri < 0) ri = 0; if (ri >= (long)NSET) ri = NSET - 1;
        if (bu < 8)  bu = 8;
        if (bu > 96) bu = 96;
        g_wi = (uint8_t)wi; g_ri = (uint8_t)ri; g_burst = (uint32_t)bu;
        snprintf(out, sizeof(out), "T %d %d %lu",
                 NOPS[g_wi], NOPS[g_ri], (unsigned long)g_burst);
        say(out);
        break;
    }

    case 'U': case 'u': {
        long u = arg(c, 1);
        if (u < 0) u = 0;
        if (u > 400) u = 400;
        g_csu = (uint32_t)u;
        snprintf(out, sizeof(out), "U %lu", (unsigned long)g_csu);
        say(out);
        break;
    }

    case 'N': case 'n': {
        g_tag = (uint8_t)arg(c, 1);
        snprintf(out, sizeof(out), "N %u", g_tag);
        say(out);
        break;
    }

    case 'D': case 'd': {
        long d = arg(c, 1);
        if (d < 0)  d = 0;
        if (d > 24) d = 24;
        g_dummy = (uint32_t)d;
        snprintf(out, sizeof(out), "D %lu", (unsigned long)g_dummy);
        say(out);
        break;
    }

    case 'Q': case 'q': {
        {
            const long m = arg(c, 1);
            g_rmode = (uint8_t)((m < 0 || m > 2) ? 0 : m);
        }
        snprintf(out, sizeof(out), "Q %u", g_rmode);
        say(out);
        break;
    }

    case 'J': case 'j': {
        long d = arg(c, 1);
        if (d < 0)  d = 0;
        if (d > 32) d = 32;
        g_dummy1 = (uint32_t)d;
        snprintf(out, sizeof(out), "J %lu", (unsigned long)g_dummy1);
        say(out);
        break;
    }

    case 'K': case 'k': {
        const uint8_t m = (uint8_t)arg(c, 1);
        set_pads(m);
        snprintf(out, sizeof(out), "K %u %08lX", m,
                 (unsigned long)IOMUXC_SW_PAD_CTL_PAD_GPIO_EMC_26);
        say(out);
        break;
    }

    case 'F': case 'f': {
        const uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        for (uint32_t off = 0; off < len; off += BLK) {
            const uint32_t n = (len - off > BLK) ? BLK : (len - off);
            for (uint32_t i = 0; i < n; i++) buf[i] = pat(a + off + i);
            do_write(a + off, buf, n);
        }
        snprintf(out, sizeof(out), "F %lu", (unsigned long)(ARM_DWT_CYCCNT - t0));
        say(out);
        break;
    }

    case 'M': case 'm': {
        const uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        const long bits = arg(c, 3);
        int32_t sum = 0;
        const uint32_t s0 = ARM_DWT_CYCCNT;
        for (uint32_t off = 0; off < len; off += BLK) {
            const uint32_t n = (len - off > BLK) ? BLK : (len - off);
            do_read(a + off, buf, n);
            sum += (bits == 2) ? mac2(buf, n) : mac4(buf, n);
        }
        const uint32_t cyc = ARM_DWT_CYCCNT - s0;
        snprintf(out, sizeof(out), "M %ld %lu", (long)sum, (unsigned long)cyc);
        say(out);
        break;
    }

    case 'R': case 'r': {
        /* One burst at the current read setting, timed in cycles. The host turns two of these into
         * the fixed overhead and the per-byte cost, and from those picks the longest burst that keeps
         * chip select low inside the refresh window. That 8 us window is a policy number and it lives
         * on the host, where changing it does not mean reflashing. */
        long nb = arg(c, 1);
        if (nb < 4) nb = 4;
        if (nb > (long)BLK) nb = BLK;
        const uint32_t save = g_burst;
        g_burst = (uint32_t)nb;
        const uint32_t r0 = ARM_DWT_CYCCNT;
        do_read(0, buf, (uint32_t)nb);
        const uint32_t rc = ARM_DWT_CYCCNT - r0;
        g_burst = save;
        snprintf(out, sizeof(out), "R %lu %lu", (unsigned long)nb, (unsigned long)rc);
        say(out);
        break;
    }

    case 'W': case 'w': {
        /* The write direction, timed the same way. Both bounds matter: a burst short enough for the
         * read can still hold chip select low past the refresh window on the write, and a write that
         * never landed looks exactly like a read that cannot be trusted. */
        long nb = arg(c, 1);
        if (nb < 4) nb = 4;
        if (nb > (long)BLK) nb = BLK;
        for (uint32_t i = 0; i < (uint32_t)nb; i++) buf[i] = pat(i);
        const uint32_t wsave = g_burst;
        g_burst = (uint32_t)nb;
        const uint32_t w0 = ARM_DWT_CYCCNT;
        do_write(0, buf, (uint32_t)nb);
        const uint32_t wc = ARM_DWT_CYCCNT - w0;
        g_burst = wsave;
        snprintf(out, sizeof(out), "W %lu %lu", (unsigned long)nb, (unsigned long)wc);
        say(out);
        break;
    }

    case 'X': case 'x': {
        /* The bytes themselves. A count of wrong bytes says a bank is broken; it never says how.
         * All zeroes means nothing is driving the bus, all ones means nothing is pulling it down, a
         * one-nibble shift means the dummy cycle count is wrong, and plausible bytes from the wrong
         * offset means the address phase is. Those need different fixes. */
        const uint32_t a = (uint32_t)arg(c, 1);
        long nb = arg(c, 2);
        if (nb < 1) nb = 1;
        if (nb > 24) nb = 24;
        do_read(a, buf, (uint32_t)nb);
        int k = snprintf(out, sizeof(out), "X");
        for (long i = 0; i < nb && k < (int)sizeof(out) - 4; i++)
            k += snprintf(out + k, sizeof(out) - k, " %02X", buf[i]);
        say(out);
        break;
    }

    case 'S': case 's': {
        /* FOUR BYTES, SINGLE-BIT, THROUGH THE SAME CHIP SELECT AS THE FAST PATH.
         *
         * Six banks answer the identity probe and then read back constant junk through the quad path.
         * Exactly three things can cause that: the chip select the fast path drives is not reaching
         * the chip, the chip is not in quad mode, or the quad edges are not being met. The probe and
         * the fast read differ in ALL THREE at once, so neither of them can tell those apart.
         *
         * This varies one. It drives chip select from the same register word the fast path uses, at
         * the same kind of clock, and talks single-bit 0x02/0x03 instead of quad 0x38/0xEB. Four bytes
         * keeps chip select low for about three microseconds, inside the refresh window, so the answer
         * cannot be a tCEM artefact either.
         *
         *   right bytes back   -> selection works; quad mode or the quad edge is the fault
         *   junk back          -> the fast path never selects this chip at all
         */
        const uint32_t a = (uint32_t)arg(c, 1);
        uint8_t want[4], got[4];
        for (int i = 0; i < 4; i++) want[i] = pat(a + i);

        select_route(g_kind, g_y);                 /* fresh idle word, nothing left over */
        const uint32_t b  = cbase();
        const uint32_t b1 = b | (1u << 28) | (1u << 29);   /* SIO2/SIO3 idle high in single mode */
        bus_out();

        /* leave quad mode if it is in it, then reset, all in the mode it might be in */
        GPIO9_DR = b;  cs_setup(); put_nib<10>(b, 0xF); put_nib<10>(b, 0x5); GPIO9_DR = idle;
        delayMicroseconds(5);
        GPIO9_DR = b1; cs_setup(); s_byte<10>(b1, 0x66); GPIO9_DR = idle; delayMicroseconds(5);
        GPIO9_DR = b1; cs_setup(); s_byte<10>(b1, 0x99); GPIO9_DR = idle; delay(2);

        /* 0x02 write, 24-bit address, four bytes */
        GPIO9_DR = b1; cs_setup();
        s_byte<10>(b1, 0x02);
        s_byte<10>(b1, (uint8_t)(a >> 16)); s_byte<10>(b1, (uint8_t)(a >> 8));
        s_byte<10>(b1, (uint8_t)a);
        for (int i = 0; i < 4; i++) s_byte<10>(b1, want[i]);
        GPIO9_DR = idle;
        delayMicroseconds(5);

        /* 0x03 read, 24-bit address, four bytes back on SIO1 */
        GPIO9_DR = b1; cs_setup();
        s_byte<10>(b1, 0x03);
        s_byte<10>(b1, (uint8_t)(a >> 16)); s_byte<10>(b1, (uint8_t)(a >> 8));
        s_byte<10>(b1, (uint8_t)a);
        GPIO9_GDIR &= ~(1u << 27);
        for (int i = 0; i < 4; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                GPIO9_DR = b1 | B_CLK; spin<10>();
                v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));
                GPIO9_DR = b1;         spin<10>();
            }
            got[i] = v;
        }
        GPIO9_DR = idle;
        bus_out();

        snprintf(out, sizeof(out), "S %02X %02X %02X %02X want %02X %02X %02X %02X",
                 got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        say(out);
        break;
    }

    case 'G': case 'g': {
        /* READ-ONLY, SINGLE-BIT. This is the one that separates the two directions.
         *
         * S writes and reads over the same path, so it proves that path end to end and says nothing
         * about any other. This reads whatever is already in the chip, so after a QUAD fill it answers
         * the question that matters: did the quad WRITE land, and is it only the quad READ that is
         * broken? A bus that works outbound and fails inbound is not a broken wire -- a wire does not
         * know which way the data is going -- so the answer changes what to look for. */
        const uint32_t a = (uint32_t)arg(c, 1);
        uint8_t got[4], want[4];
        for (int i = 0; i < 4; i++) want[i] = pat(a + i);

        select_route(g_kind, g_y);
        const uint32_t b  = cbase();
        const uint32_t b1 = b | (1u << 28) | (1u << 29);
        bus_out();

        GPIO9_DR = b;  cs_setup(); put_nib<10>(b, 0xF); put_nib<10>(b, 0x5); GPIO9_DR = idle;
        delayMicroseconds(5);

        GPIO9_DR = b1; cs_setup();
        s_byte<10>(b1, 0x03);
        s_byte<10>(b1, (uint8_t)(a >> 16)); s_byte<10>(b1, (uint8_t)(a >> 8));
        s_byte<10>(b1, (uint8_t)a);
        GPIO9_GDIR &= ~(1u << 27);
        for (int i = 0; i < 4; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                GPIO9_DR = b1 | B_CLK; spin<10>();
                v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));
                GPIO9_DR = b1;         spin<10>();
            }
            got[i] = v;
        }
        GPIO9_DR = idle;
        bus_out();

        snprintf(out, sizeof(out), "G %02X %02X %02X %02X want %02X %02X %02X %02X",
                 got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
        say(out);
        break;
    }

    case 'L': case 'l': {
        /* WHICH LINE, counted over a real span instead of a handful of bytes.
         *
         * One bad wire puts all of its errors in one bit position, because a nibble travels on four
         * wires and each wire carries one bit of it. Errors spread evenly over all four mean the whole
         * nibble arrived wrong, which is a clock, a select, a supply or a sampling instant -- not a
         * wire. The two answers call for completely different work, and forty-eight nibbles is not
         * enough of a sample to tell them apart. This counts every nibble in the span. */
        const uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        uint32_t e[4] = { 0, 0, 0, 0 };
        uint32_t nib = 0;
        for (uint32_t off = 0; off < len; off += BLK) {
            const uint32_t nb = (len - off > BLK) ? BLK : (len - off);
            do_read(a + off, buf, nb);
            for (uint32_t i = 0; i < nb; i++) {
                const uint8_t w = pat(a + off + i);
                const uint8_t g = buf[i];
                const uint8_t d0 = (uint8_t)((g >> 4) ^ (w >> 4));
                const uint8_t d1 = (uint8_t)((g & 0xF) ^ (w & 0xF));
                for (int k = 0; k < 4; k++) {
                    if ((d0 >> k) & 1) e[k]++;
                    if ((d1 >> k) & 1) e[k]++;
                }
                nib += 2;
            }
        }
        snprintf(out, sizeof(out), "L %lu %lu %lu %lu %lu",
                 (unsigned long)e[0], (unsigned long)e[1], (unsigned long)e[2],
                 (unsigned long)e[3], (unsigned long)nib);
        say(out);
        break;
    }

    case 'V': case 'v': {
        const uint32_t a = (uint32_t)arg(c, 1), len = (uint32_t)arg(c, 2);
        uint32_t bad = 0;
        for (uint32_t off = 0; off < len; off += BLK) {
            const uint32_t n = (len - off > BLK) ? BLK : (len - off);
            do_read(a + off, buf, n);
            for (uint32_t i = 0; i < n; i++) if (buf[i] != pat(a + off + i)) bad++;
        }
        snprintf(out, sizeof(out), "V %lu %lu",
                 (unsigned long)bad, (unsigned long)(ARM_DWT_CYCCNT - t0));
        say(out);
        break;
    }

    default:
        say("E unknown");
        break;
    }
}

void loop()
{
    while (Serial2.available()) {
        const char ch = (char)Serial2.read();
        if (ch == '\n' || ch == '\r') {
            if (ln) { line[ln] = 0; handle(line); ln = 0; }
        } else if (ln < sizeof(line) - 1) {
            line[ln++] = ch;
        }
    }
}

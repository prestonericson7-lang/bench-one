/* ===========================================================================================
 *  psram_coldstart -- is the fast setting unreliable, or was the previous test mis-ordered
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
 *  the question, and the confound that had to be removed first
 *
 *  The previous test swept a wait before the read -- 0, 1, 5, 20, 100, 500 ms -- and reported that
 *  zero was dirty and every non-zero wait was clean. It read like a clean causal result.
 *
 *  Its own control line disproved it. At the end of the same run, with no wait at all, the first
 *  pass came back clean. The same operation that produced eighteen thousand errors in the first row
 *  produced none in the last. The wait was never the variable. The zero row was simply always the
 *  FIRST cycle after the fifteen-second idle between runs, and the other rows never were.
 *
 *  That is an ordering artefact, and sweeping a parameter in a fixed order cannot tell it apart
 *  from the parameter mattering. Every row after the first carried the previous row's warm-up.
 *
 *  So this measures the thing that actually varied. Fill once. Then, for each edge rate, go idle
 *  long enough to be cold, read the whole part, and read it again immediately. The first number is
 *  a cold read and the second is a warm one, at identical settings, with nothing else different.
 *
 *  There are no writes inside the loop at all, which also removes the other candidate: whatever
 *  happens here cannot be recovery from sustained writing, because no writing happened.
 * ======================================================================================== */

#define BLK  (8u * 1024u)
#define SIZE (8u * 1024u * 1024u)
#define IDLE 5000u                      /* long enough to be cold, short enough to run often */
static uint32_t blk[BLK / 4];
static uint32_t n_bad, first_a, last_a;

template <int S> static uint32_t check(void)
{
    n_bad = 0; first_a = 0; last_a = 0;
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        rd<S>(off, (uint8_t *)blk, BLK);
        for (uint32_t i = 0; i < BLK / 4; i++) {
            const uint32_t expect = off + i * 4;
            if (blk[i] != expect) {
                if (n_bad == 0) first_a = expect;
                last_a = expect;
                n_bad++;
            }
        }
    }
    return n_bad;
}

static void fill(void)
{
    for (uint32_t off = 0; off < SIZE; off += BLK) {
        for (uint32_t i = 0; i < BLK / 4; i++) blk[i] = off + i * 4;
        wr<16>(off, (const uint8_t *)blk, BLK);
    }
}

/* cold reading is the whole point, so the idle has to be real: chip select stays high, nothing
 * toggles, and the part refreshes itself through it. Five seconds of that was already shown to
 * preserve every byte, so anything wrong afterwards is the reading and not the remembering. */
template <int S> static void pair(int nops, int trial)
{
    delay(IDLE);
    const uint32_t cold = check<S>();
    const uint32_t cf = first_a, cl = last_a;
    const uint32_t warm = check<S>();

    Serial.print(F("      "));
    if (nops < 10) Serial.print(' ');
    Serial.print(nops);      Serial.print(F("       "));
    Serial.print(trial);     Serial.print(F("      "));
    if (cold < 10)    Serial.print(' ');
    if (cold < 100)   Serial.print(' ');
    if (cold < 1000)  Serial.print(' ');
    if (cold < 10000) Serial.print(' ');
    Serial.print(cold);      Serial.print(F("        "));
    if (warm < 10)    Serial.print(' ');
    if (warm < 100)   Serial.print(' ');
    if (warm < 1000)  Serial.print(' ');
    if (warm < 10000) Serial.print(' ');
    Serial.print(warm);      Serial.print(F("     "));
    if (cold) {
        Serial.print(F("0x")); Serial.print(cf, HEX);
        Serial.print(F(" .. 0x")); Serial.print(cl, HEX);
    } else {
        Serial.print('-');
    }
    Serial.println();
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
    Serial.println(F("  a cold read and a warm read, same setting, nothing else different"));
    Serial.println(F("=================================================================="));

    pick(7);
    enter_quad();
    Serial.println(F("  filling 8 MB once at 16 no-ops. No writes after this point."));
    fill();

    Serial.println(F("    nops   trial   cold wrong   warm wrong   where"));
    pair< 8>( 8, 1);  pair< 8>( 8, 2);
    pair<10>(10, 1);  pair<10>(10, 2);
    pair<12>(12, 1);  pair<12>(12, 2);

    /* and the order reversed, because if cold reads are dirty only at the top of a run then this
     * is still an ordering artefact and the fast setting is being blamed for being first again */
    Serial.println(F("    the same three, in the opposite order"));
    pair<12>(12, 3);  pair<10>(10, 3);  pair< 8>( 8, 3);

    Serial.println(F("--- how to read it ---"));
    Serial.println(F("  Cold dirty and warm clean at 8 but both clean at 10 and 12: the fast"));
    Serial.println(F("  setting needs the bus already moving, and 10 is the honest floor."));
    Serial.println(F("  Both columns clean everywhere: the earlier errors belonged to the"));
    Serial.println(F("  writing after all, and this test cannot see them because it never writes."));
    Serial.println(F("  8 dirty when it runs first and clean when it runs last: still ordering,"));
    Serial.println(F("  and the thing that varies is time since power-up, not edge placement."));

    Serial.println(F("\n=== repeating ==="));
}

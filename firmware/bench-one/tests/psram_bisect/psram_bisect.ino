/* ===========================================================================================
 *  psram_bisect -- two implementations of the same read, side by side
 * ===========================================================================================
 *
 *  WHERE THIS GOT TO
 *  -----------------
 *  psram_probe read a correct 0D 5D signature from all six chips. psram_quad_check wrote and read
 *  64 of 64 bytes correctly over both single SPI and quad. Both used pinMode and digitalWriteFast.
 *
 *  The fast driver rewritten with direct writes to GPIO9_DR fails at every speed, in every width,
 *  in both directions -- including single-write-single-read, which the earlier sketch proved works.
 *  Two real bugs were found in it and fixed and it still fails.
 *
 *  When a rewrite of a working thing fails in every configuration, the rewrite is wrong. So this
 *  stops testing the hardware and tests the two implementations against each other, on the smallest
 *  operation there is: read the chip's identity, where the right answer is known in advance.
 *
 *      0D 5D   a real ESP-PSRAM64H
 *      FF FF   nothing driving the line
 *      anything else   the implementation is at fault
 *
 *  One of these two will be right. Whichever is wrong is a bug in my code and nothing to do with
 *  the breadboard, the power, or the soldering.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

#define P_SS0  48
#define P_SCLK 53
#define P_D0   52
#define P_D1   49
#define P_D2   50
#define P_D3   54
#define PIN_A 2
#define PIN_B 3
#define PIN_C 4

#define B_SS0  (1u << 24)
#define B_CLK  (1u << 25)
#define B_D1   (1u << 27)
#define B_DATA (0xFu << 26)
#define DSHIFT 26
#define B_SS1  (1u << 22)

static void pick(uint8_t n)
{
    digitalWriteFast(PIN_A, (n >> 0) & 1);
    digitalWriteFast(PIN_B, (n >> 1) & 1);
    digitalWriteFast(PIN_C, (n >> 2) & 1);
    delayMicroseconds(10);
}

/* ===========================================================================================
 *  IMPLEMENTATION A -- pinMode and digitalWriteFast. This is the one that is known to work.
 * ======================================================================================== */
static inline void a_tick(void)
{
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, LOW);
}

static void a_out(uint8_t v)
{
    pinMode(P_D0, OUTPUT);
    for (int i = 7; i >= 0; i--) { digitalWriteFast(P_D0, (v >> i) & 1); a_tick(); }
}

static uint8_t a_in(void)
{
    pinMode(P_D1, INPUT);
    uint8_t v = 0;
    for (int i = 7; i >= 0; i--) {
        delayMicroseconds(1);
        digitalWriteFast(P_SCLK, HIGH);
        v = (uint8_t)((v << 1) | (digitalReadFast(P_D1) ? 1 : 0));
        delayMicroseconds(1);
        digitalWriteFast(P_SCLK, LOW);
    }
    return v;
}

static void a_id(uint8_t *out)
{
    pinMode(P_SS0, OUTPUT);  pinMode(P_SCLK, OUTPUT);
    pinMode(P_D2, OUTPUT);   digitalWriteFast(P_D2, HIGH);
    pinMode(P_D3, OUTPUT);   digitalWriteFast(P_D3, HIGH);
    digitalWriteFast(P_SCLK, LOW);
    digitalWriteFast(P_SS0, LOW);
    delayMicroseconds(2);
    a_out(0x9F); a_out(0); a_out(0); a_out(0);
    for (int i = 0; i < 4; i++) out[i] = a_in();
    digitalWriteFast(P_SS0, HIGH);
    delayMicroseconds(5);
}

/* ===========================================================================================
 *  IMPLEMENTATION B -- whole-register writes to GPIO9_DR. This is the one under suspicion.
 * ======================================================================================== */
static uint32_t idle;
#define SPIN 60
static inline void spin(void) { for (int i = 0; i < SPIN; i++) __asm__ volatile("nop"); }

static void b_id(uint8_t *out)
{
    GPIO9_GDIR |= (B_DATA | B_CLK | B_SS0 | B_SS1);
    idle = (GPIO9_DR | B_SS0 | B_SS1 | (1u << 28) | (1u << 29)) & ~B_CLK;
    GPIO9_DR = idle;
    delayMicroseconds(5);

    const uint32_t base = (idle & ~B_SS0 & ~B_CLK & ~B_DATA) | (1u << 28) | (1u << 29);
    GPIO9_DR = base;
    spin();

    /* command 0x9F then a 24-bit don't-care address, on D0 */
    const uint8_t cmd[4] = { 0x9F, 0, 0, 0 };
    for (int c = 0; c < 4; c++) {
        for (int i = 7; i >= 0; i--) {
            const uint32_t d = base | (((cmd[c] >> i) & 1u) << DSHIFT);
            GPIO9_DR = d;          spin();     /* data set up, clock low  */
            GPIO9_DR = d | B_CLK;  spin();     /* rising edge: chip latches */
            GPIO9_DR = d;          spin();     /* and BACK DOWN. Leaving it high means the read
                                                * loop's first "rise" is no edge at all, so the
                                                * whole reply arrives shifted by one bit. */
        }
    }

    GPIO9_GDIR &= ~B_D1;                  /* release D1 so the chip can drive it */
    for (int c = 0; c < 4; c++) {
        uint8_t v = 0;
        for (int i = 7; i >= 0; i--) {
            GPIO9_DR = base | B_CLK;  spin();
            v = (uint8_t)((v << 1) | ((GPIO9_PSR >> 27) & 1u));
            GPIO9_DR = base;          spin();
        }
        out[c] = v;
    }
    GPIO9_DR = idle;
    GPIO9_GDIR |= B_D1;
    delayMicroseconds(5);
}

/* ======================================================================================== */

static void show(const char *who, const uint8_t *id)
{
    Serial.print(F("    "));
    Serial.print(who);
    Serial.print(F("  "));
    for (int i = 0; i < 4; i++) {
        if (id[i] < 16) Serial.print('0');
        Serial.print(id[i], HEX);
        Serial.print(' ');
    }
    if (id[0] == 0x0D && id[1] == 0x5D)      Serial.println(F("  CORRECT"));
    else if (id[0] == 0xFF && id[1] == 0xFF) Serial.println(F("  nothing driving"));
    else                                     Serial.println(F("  WRONG"));
}

void setup()
{
    Serial.begin(115200);
    pinMode(PIN_A, OUTPUT); pinMode(PIN_B, OUTPUT); pinMode(PIN_C, OUTPUT);
    for (int p = 48; p <= 54; p++) pinMode(p, OUTPUT);
    digitalWriteFast(P_SS0, HIGH);
    digitalWriteFast(51, HIGH);
    pick(7);
}

void loop()
{
    uint8_t a[4], b[4];

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  the same identity read, written two ways"));
    Serial.println(F("  expected reply: 0D 5D ..   decoder on bare Y7"));
    Serial.println(F("=============================================================="));
    Serial.println();

    pick(7);
    a_id(a);
    show("A  pinMode + digitalWriteFast ", a);

    pick(7);
    b_id(b);
    show("B  direct GPIO9_DR writes     ", b);

    Serial.println();
    if (a[0] == 0x0D && b[0] != 0x0D) {
        Serial.println(F("  A works and B does not, so the register-write driver is the bug."));
        Serial.println(F("  Nothing about the board is at fault. Rewrite B on A's primitives."));
    } else if (a[0] == 0x0D && b[0] == 0x0D) {
        Serial.println(F("  Both work, so the primitives are fine and the fault is further up,"));
        Serial.println(F("  in the block transfer rather than in a single command."));
    } else {
        Serial.println(F("  Even A fails now, and A worked earlier today. Something on the board"));
        Serial.println(F("  has changed since: a wire moved, or a chip select is being held."));
    }

    Serial.println(F("\n=== repeating in 12 s ==="));
    delay(12000);
}

/* ===========================================================================================
 *  psram_quad_check -- which of the four data lines is actually broken
 * ===========================================================================================
 *
 *  THE CLUE
 *  --------
 *  At 49.5 MHz the chip identifies itself perfectly and then loses three quarters of the bytes
 *  written through the memory window. Those two operations do not use the same wires.
 *
 *      identity, command 0x9F   single SPI   uses D0 out and D1 back.  WORKS.
 *      memory window            quad         uses D0, D1, D2 and D3.   FAILS.
 *
 *  So the fault is most likely on D2 or D3, and every test so far has been blind to it: the
 *  bit-banged probe read identities down D1 alone and pronounced all six chips healthy, which they
 *  are, over the two lines it used.
 *
 *  THE TEST
 *  --------
 *  Everything here is bit-banged at roughly 250 kHz so that timing cannot be the explanation. Write
 *  a known block over single SPI, read it back over single SPI, then read the SAME block over quad.
 *  If single is clean and quad is not, the extra two lines are the fault.
 *
 *  Then it says WHICH one. Every wrong byte is exclusive-ored against what it should have been and
 *  the failing bit positions are counted. In quad mode a byte crosses the wires as two nibbles, so
 *  bits 0 and 4 travelled on D0, bits 1 and 5 on D1, bits 2 and 6 on D2, bits 3 and 7 on D3. A line
 *  that is open or shorted shows up as its own two bit positions failing and the others not.
 *
 *  That turns "the memory does not work" into "D3 is not connected", which is a thing you can fix.
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
#define P_A     2
#define P_B     3
#define P_C     4
#define PARK    7

#define N 64
static uint8_t expect[N], got[N];

static void pick(uint8_t n)
{
    digitalWriteFast(P_A, (n >> 0) & 1);
    digitalWriteFast(P_B, (n >> 1) & 1);
    digitalWriteFast(P_C, (n >> 2) & 1);
    delayMicroseconds(10);
}

static inline void tick(void)
{
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, LOW);
}

/* ---- single SPI: command and data on D0, replies on D1 ------------------------------------ */
static void s_out(uint8_t v)
{
    pinMode(P_D0, OUTPUT);
    for (int i = 7; i >= 0; i--) { digitalWriteFast(P_D0, (v >> i) & 1); tick(); }
}

static uint8_t s_in(void)
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

/* ---- quad: all four lines, most significant nibble first --------------------------------- */
static void q_out(uint8_t v)
{
    pinMode(P_D0, OUTPUT); pinMode(P_D1, OUTPUT);
    pinMode(P_D2, OUTPUT); pinMode(P_D3, OUTPUT);
    for (int half = 1; half >= 0; half--) {
        const uint8_t nib = (v >> (half * 4)) & 0x0F;
        digitalWriteFast(P_D0, (nib >> 0) & 1);
        digitalWriteFast(P_D1, (nib >> 1) & 1);
        digitalWriteFast(P_D2, (nib >> 2) & 1);
        digitalWriteFast(P_D3, (nib >> 3) & 1);
        tick();
    }
}

static uint8_t q_in(void)
{
    pinMode(P_D0, INPUT); pinMode(P_D1, INPUT);
    pinMode(P_D2, INPUT); pinMode(P_D3, INPUT);
    uint8_t v = 0;
    for (int half = 1; half >= 0; half--) {
        delayMicroseconds(1);
        digitalWriteFast(P_SCLK, HIGH);
        delayMicroseconds(1);
        const uint8_t nib = (uint8_t)((digitalReadFast(P_D0) ? 1 : 0)
                                    | (digitalReadFast(P_D1) ? 2 : 0)
                                    | (digitalReadFast(P_D2) ? 4 : 0)
                                    | (digitalReadFast(P_D3) ? 8 : 0));
        digitalWriteFast(P_SCLK, LOW);
        v = (uint8_t)((v << 4) | nib);
    }
    return v;
}

static void cs(bool low)
{
    digitalWriteFast(P_SCLK, LOW);
    digitalWriteFast(P_SS0, low ? LOW : HIGH);
    delayMicroseconds(2);
}

/* ======================================================================================== */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    pinMode(P_A, OUTPUT); pinMode(P_B, OUTPUT); pinMode(P_C, OUTPUT);
    pick(PARK);
    pinMode(P_SS0, OUTPUT);  digitalWriteFast(P_SS0, HIGH);
    pinMode(P_SCLK, OUTPUT); digitalWriteFast(P_SCLK, LOW);
    pinMode(P_D2, OUTPUT);   digitalWriteFast(P_D2, HIGH);
    pinMode(P_D3, OUTPUT);   digitalWriteFast(P_D3, HIGH);

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  single SPI against quad, both bit-banged, on the onboard chip"));
    Serial.println(F("=============================================================="));

    /* Leave whatever mode the last sketch left it in, then reset. 0xF5 exits QPI if it is in it;
     * harmless if it is not. */
    cs(true); q_out(0xF5); cs(false);
    cs(true); s_out(0x66); cs(false);
    cs(true); s_out(0x99); cs(false);
    delay(2);

    for (int i = 0; i < N; i++) expect[i] = (uint8_t)(i * 0x9D + 0x3B);

    /* ---- 1. write and read over single SPI: this only exercises D0 and D1 ------------------ */
    cs(true); s_out(0x02); s_out(0); s_out(0); s_out(0);
    for (int i = 0; i < N; i++) s_out(expect[i]);
    cs(false);
    delayMicroseconds(20);

    cs(true); s_out(0x03); s_out(0); s_out(0); s_out(0);
    for (int i = 0; i < N; i++) got[i] = s_in();
    cs(false);

    uint32_t bad_single = 0;
    for (int i = 0; i < N; i++) if (got[i] != expect[i]) bad_single++;
    Serial.print(F("\n[1] single SPI, D0 and D1 only:  "));
    Serial.print(bad_single); Serial.print(F(" of ")); Serial.print(N);
    Serial.println(bad_single ? F(" wrong") : F(" wrong  <-- clean"));

    if (bad_single) {
        Serial.println(F("    Even D0 and D1 cannot carry a byte, so this is not about quad."));
        Serial.println(F("    Check the chip's own joints before anything else."));
        return;
    }

    /* ---- 2. same bytes back over quad: this adds D2 and D3 -------------------------------- */
    cs(true); s_out(0x35); cs(false);       /* enter QPI */
    delayMicroseconds(20);

    cs(true);
    q_out(0xEB); q_out(0); q_out(0); q_out(0);
    /* SIX dummy clocks, not three, and every line released first. An earlier version clocked
     * three and read the data shifted by three nibbles, which comes back as exactly 50% random
     * bits on all four lines -- the signature of a framing error, not of a broken wire. */
    pinMode(P_D0, INPUT); pinMode(P_D1, INPUT);
    pinMode(P_D2, INPUT); pinMode(P_D3, INPUT);
    for (int i = 0; i < 6; i++) tick();
    for (int i = 0; i < N; i++) got[i] = q_in();
    cs(false);

    cs(true); q_out(0xF5); cs(false);       /* back out of QPI */

    uint32_t bad_quad = 0, bitfail[8] = {0};
    for (int i = 0; i < N; i++) {
        const uint8_t x = (uint8_t)(got[i] ^ expect[i]);
        if (x) bad_quad++;
        for (int b = 0; b < 8; b++) if (x & (1 << b)) bitfail[b]++;
    }
    Serial.print(F("[2] quad, all four lines:        "));
    Serial.print(bad_quad); Serial.print(F(" of ")); Serial.print(N);
    Serial.println(bad_quad ? F(" wrong") : F(" wrong  <-- clean"));

    /* ---- 3. name the line ------------------------------------------------------------------ */
    Serial.println(F("\n[3] failures by data line"));
    const char *nm[4] = { "D0 (pin 5, SIO0)", "D1 (pin 2, SIO1)",
                          "D2 (pin 3, SIO2)", "D3 (pin 7, SIO3)" };
    int worst = -1; uint32_t worstn = 0, leastn = 0xFFFFFFFF;
    for (int line = 0; line < 4; line++) {
        const uint32_t n = bitfail[line] + bitfail[line + 4];
        if (n < leastn) leastn = n;
        Serial.print(F("    "));
        Serial.print(nm[line]);
        Serial.print(F("  bad bits: "));
        Serial.println(n);
        if (n > worstn) { worstn = n; worst = line; }
    }
    /* One broken wire makes its own two bit positions fail and leaves the others alone. Four lines
     * failing within a few percent of each other is not a wire, it is framing or power. */
    const uint32_t spread = worstn - leastn;

    Serial.println(F("\n--- verdict ---"));
    if (bad_quad == 0) {
        Serial.println(F("  Quad is clean when bit-banged, so all four lines are connected and"));
        Serial.println(F("  the fault is speed, not wiring. The controller at 49.5 MHz is still"));
        Serial.println(F("  too fast for this much capacitance. Shorter stubs is the only fix."));
    } else if (worstn > 0 && spread > 3 * (worstn / 10)) {
        Serial.print(F("  "));
        Serial.print(nm[worst]);
        Serial.println(F(" is the bad line."));
        Serial.println(F("  It carries no traffic in single SPI, which is why every earlier test"));
        Serial.println(F("  said the chip was healthy. Reflow that one pin on the Teensy end and"));
        Serial.println(F("  on every chip, and check it is not bridged to its neighbour."));
    } else {
        Serial.println(F("  All four lines are failing about equally, which is not a broken wire."));
        Serial.println(F("  That is the whole bus being marginal even at bit-bang speed, and it"));
        Serial.println(F("  points at power: check 3.3 V and ground reach every chip, and that"));
        Serial.println(F("  each one has its 100 nF."));
    }
    Serial.println(F("\n=== done ==="));
}

void loop() { }

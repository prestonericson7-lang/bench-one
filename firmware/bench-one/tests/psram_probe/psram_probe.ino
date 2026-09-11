/* ===========================================================================================
 *  psram_probe -- ask every chip on the bus who it is, by hand
 * ===========================================================================================
 *
 *  WHY NOT JUST READ external_psram_size
 *  -------------------------------------
 *  Because it is one number for the whole bus and it has now said 0 twice, which rules nothing out.
 *  It cannot tell "no chip on that select" from "chip there but a data line is shorted" from "chip
 *  there but two of them are answering at once". Three different faults, one answer, and the last
 *  two guesses at it were both wrong.
 *
 *  So this stops using the controller and drives the seven bus pins as ordinary GPIO. Two things
 *  become possible that were not before.
 *
 *  FIRST, a real continuity test. Drive one line, read the others, and any that follows it is
 *  bridged to it. Pull each line up and then down on its own and see whether something external
 *  holds it. That finds a solder bridge without a meter and without unsoldering anything.
 *
 *  SECOND, ask each chip directly. Every PSRAM answers command 0x9F with a manufacturer byte and a
 *  known-good-die byte -- 0x0D then 0x5D on an ESP-PSRAM64H. Bit-banging that is slow and completely
 *  reliable, and the reply says whether a chip is present, powered and wired, with no controller
 *  configuration to get wrong.
 *
 *  Doing it on each chip select in turn, and on each decoder output in turn, locates every chip on
 *  the board by name.
 *
 *
 *  ABOUT tCEM, SO IT IS NOT A SURPRISE
 *  -----------------------------------
 *  PSRAM is DRAM inside and chip select may not stay low for more than 8 microseconds or internal
 *  refresh stops. Bit-banging an identity read takes longer than that. That is harmless here: the
 *  penalty is that stored data is not retained, and there is no stored data worth keeping yet. It
 *  cannot damage the part. It does mean this sketch is a diagnostic and never a benchmark.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

/* The QSPI bus, as ordinary pin numbers. */
#define P_SS0   48      /* the RAM footprint's select, and the decoder's enable */
#define P_SS1   51      /* the Flash footprint's select. This is the pad that tore off. */
#define P_SCLK  53
#define P_D0    52      /* SIO0, which is the data IN line in single-SPI mode */
#define P_D1    49      /* SIO1, the data OUT line */
#define P_D2    50
#define P_D3    54

/* The decoder's address lines. */
#define P_A     2
#define P_B     3
#define P_C     4

static const uint8_t BUS[]      = { P_SCLK, P_D0, P_D1, P_D2, P_D3 };
static const char *  BUS_NAME[] = { "SCLK", "D0", "D1", "D2", "D3" };
#define NBUS (sizeof(BUS) / sizeof(BUS[0]))

/* --------------------------------------------------------------------------------------- */

static void idle_bus(void)
{
    pinMode(P_SS0, OUTPUT);  digitalWriteFast(P_SS0, HIGH);   /* everything deselected */
    pinMode(P_SS1, OUTPUT);  digitalWriteFast(P_SS1, HIGH);
    pinMode(P_SCLK, OUTPUT); digitalWriteFast(P_SCLK, LOW);
    pinMode(P_D0, OUTPUT);   digitalWriteFast(P_D0, LOW);
    pinMode(P_D1, INPUT);
    pinMode(P_D2, OUTPUT);   digitalWriteFast(P_D2, HIGH);    /* not a command line in single SPI */
    pinMode(P_D3, OUTPUT);   digitalWriteFast(P_D3, HIGH);
}

static void pick(uint8_t n)
{
    digitalWriteFast(P_A, (n >> 0) & 1);
    digitalWriteFast(P_B, (n >> 1) & 1);
    digitalWriteFast(P_C, (n >> 2) & 1);
    delayMicroseconds(5);
}

static inline void clk(void)
{
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, HIGH);
    delayMicroseconds(1);
    digitalWriteFast(P_SCLK, LOW);
}

static void send(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        digitalWriteFast(P_D0, (v >> i) & 1);
        clk();
    }
}

static uint8_t recv(void)
{
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

/* Command 0x9F then a 24-bit don't-care address, then eight bytes back. */
static void read_id(uint8_t sel_pin, uint8_t *out)
{
    digitalWriteFast(P_SCLK, LOW);
    digitalWriteFast(sel_pin, LOW);
    delayMicroseconds(1);
    send(0x9F);
    send(0x00); send(0x00); send(0x00);
    for (int i = 0; i < 8; i++) out[i] = recv();
    delayMicroseconds(1);
    digitalWriteFast(sel_pin, HIGH);
    delayMicroseconds(5);
}

static bool alive(const uint8_t *id)
{
    /* A real ESP-PSRAM64H answers 0x0D then 0x5D. An open bus reads all ones or all zeros. */
    return id[0] == 0x0D && id[1] == 0x5D;
}

static void show(const char *what, const uint8_t *id)
{
    Serial.print(F("    "));
    Serial.print(what);
    Serial.print(F("  "));
    for (int i = 0; i < 8; i++) {
        if (id[i] < 16) Serial.print('0');
        Serial.print(id[i], HEX);
        Serial.print(' ');
    }
    if (alive(id))                                   Serial.println(F("  <-- A CHIP ANSWERED"));
    else if (id[0] == 0xFF && id[1] == 0xFF)         Serial.println(F("  (all ones: nothing driving)"));
    else if (id[0] == 0x00 && id[1] == 0x00)         Serial.println(F("  (all zeros: nothing driving, or held low)"));
    else                                             Serial.println(F("  (garbage: something is there but wrong)"));
}

/* ======================================================================================== */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }
    delay(150);

    pinMode(P_A, OUTPUT); pinMode(P_B, OUTPUT); pinMode(P_C, OUTPUT);
    pick(7);
    idle_bus();

    Serial.println();
    Serial.println(F("=============================================================="));
    Serial.println(F("  asking the bus directly, with the controller out of the way"));
    Serial.println(F("=============================================================="));

    /* ---- 1. is anything shorted? ------------------------------------------------------- */
    Serial.println(F("\n[1] shorts between the five bus lines"));
    {
        uint32_t found = 0;
        for (unsigned i = 0; i < NBUS; i++) {
            for (unsigned j = 0; j < NBUS; j++) pinMode(BUS[j], (i == j) ? OUTPUT : INPUT_PULLDOWN);
            digitalWriteFast(BUS[i], HIGH);
            delayMicroseconds(50);
            for (unsigned j = 0; j < NBUS; j++) {
                if (i == j) continue;
                if (digitalReadFast(BUS[j])) {
                    Serial.print(F("    BRIDGED: ")); Serial.print(BUS_NAME[i]);
                    Serial.print(F(" and ")); Serial.println(BUS_NAME[j]);
                    found++;
                }
            }
            digitalWriteFast(BUS[i], LOW);
        }
        if (!found) Serial.println(F("    none. every line is independent."));
    }

    /* ---- 2. is anything stuck to a rail? ------------------------------------------------ */
    Serial.println(F("\n[2] lines held by something external"));
    {
        uint32_t found = 0;
        for (unsigned i = 0; i < NBUS; i++) {
            pinMode(BUS[i], INPUT_PULLUP);   delayMicroseconds(200);
            const bool hi = digitalReadFast(BUS[i]);
            pinMode(BUS[i], INPUT_PULLDOWN); delayMicroseconds(200);
            const bool lo = digitalReadFast(BUS[i]);
            if (!hi) { Serial.print(F("    SHORTED TO GROUND: ")); Serial.println(BUS_NAME[i]); found++; }
            if (lo)  { Serial.print(F("    SHORTED TO 3.3 V:  ")); Serial.println(BUS_NAME[i]); found++; }
        }
        if (!found) Serial.println(F("    none. every line floats when released."));
    }

    idle_bus();

    /* ---- 3. who is on which select? ----------------------------------------------------- */
    uint8_t id[8];
    Serial.println(F("\n[3] identity read, command 0x9F. 0D 5D means a real PSRAM answered."));

    Serial.println(F("\n  the Flash footprint, SS1 (pin 51). Its pad tore off, so expect nothing:"));
    read_id(P_SS1, id);
    show("SS1     ", id);

    Serial.println(F("\n  the RAM footprint, SS0 (pin 48), with the decoder parked on bare Y7."));
    Serial.println(F("  Only a chip soldered to the RAM footprint itself can answer here:"));
    pick(7);
    read_id(P_SS0, id);
    show("SS0 + Y7", id);

    Serial.println(F("\n  now SS0 with the decoder pointed at each wired output in turn."));
    Serial.println(F("  Anything that answers is one of your five banked chips:"));
    for (uint8_t y = 0; y < 5; y++) {
        pick(y);
        read_id(P_SS0, id);
        char lbl[10];
        snprintf(lbl, sizeof(lbl), "SS0 + Y%u", (unsigned)y);
        show(lbl, id);
    }

    pick(7);
    idle_bus();
    Serial.println(F("\n=== done. Nothing above depends on the controller or on my guesses. ==="));
}

void loop() { }

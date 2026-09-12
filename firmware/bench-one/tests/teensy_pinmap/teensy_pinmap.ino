/* ===========================================================================================
 *  teensy_pinmap -- which GPIO register each pin lives in, asked of the board itself
 * ===========================================================================================
 *
 *  WHY THIS EXISTS
 *  ---------------
 *  The PSRAM bank is capped at 8 MB of 48 because the 74LVC138A decoder's enable shares a wire with
 *  the onboard chip's chip select, so both answer at once. The repair does not need the onboard chip
 *  removed: it needs the decoder's enable moved to a pin of its own. Then chip select 0 selects the
 *  onboard chip with the decoder disabled, and the spare pin selects one of five breadboard banks
 *  with chip select 0 held high. One wire, and 8 MB becomes 48.
 *
 *  But the replacement pin has to be in GPIO9, the same register as the clock and the four data
 *  lines. The driver writes that whole register in one store per edge, and a chip select in a
 *  different register would need a second store on every burst boundary and, worse, would be a
 *  second thing to get wrong in the same way the decoder address pins already were once.
 *
 *  So the question is: which pins are in GPIO9, which bit is each, and which of them are free.
 *
 *  WHY ASK THE BOARD INSTEAD OF THE PINOUT CARD
 *  --------------------------------------------
 *  Because the pinout card has already been wrong once in this project in a way that cost an evening:
 *  pins 48 and 51 exist only on the QSPI pads, every other QSPI signal is marked as appearing twice,
 *  and those two are not. A table read off a picture is a guess about the core. A table printed by
 *  the core is the core.
 *
 *  Nothing here drives a pin. It reads the compile-time port register and bit mask the core assigns
 *  to each pin and prints them, so it is safe to run with the bank wired and powered.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

/* the pins the PSRAM bank already owns, so the report can mark what is left */
static const struct { uint8_t pin; const char *use; } TAKEN[] = {
    {  2, "decoder A"     },
    {  3, "decoder B"     },
    {  4, "decoder C"     },
    { 48, "CS0 / decoder enable -- the wire to move" },
    { 49, "D2"            },
    { 50, "D3"            },
    { 51, "CS1 (pad torn off)" },
    { 52, "SCLK"          },
    { 53, "D0"            },
    { 54, "D1"            },
};
#define NTAKEN (sizeof(TAKEN) / sizeof(TAKEN[0]))

static const char *taken_for(uint8_t p)
{
    for (unsigned i = 0; i < NTAKEN; i++) if (TAKEN[i].pin == p) return TAKEN[i].use;
    return 0;
}

static const char *portname(volatile uint32_t *r)
{
    if (r == &GPIO6_DR) return "GPIO6";
    if (r == &GPIO7_DR) return "GPIO7";
    if (r == &GPIO8_DR) return "GPIO8";
    if (r == &GPIO9_DR) return "GPIO9";
    if (r == &GPIO1_DR) return "GPIO1";
    if (r == &GPIO2_DR) return "GPIO2";
    if (r == &GPIO3_DR) return "GPIO3";
    if (r == &GPIO4_DR) return "GPIO4";
    return "?";
}

static int bitnum(uint32_t mask)
{
    for (int b = 0; b < 32; b++) if (mask == (1u << b)) return b;
    return -1;
}

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.print(F("  pin map as the core defines it. "));
    Serial.print(CORE_NUM_DIGITAL);
    Serial.println(F(" digital pins."));
    Serial.println(F("=================================================================="));

    /* the whole table first, so anything else needing a register-wide trick can use it */
    Serial.println(F("\n  pin   port    bit"));
    for (uint8_t p = 0; p < CORE_NUM_DIGITAL; p++) {
        volatile uint32_t *r = portOutputRegister(p);
        const uint32_t m = digitalPinToBitMask(p);
        Serial.print(F("  "));
        if (p < 10) Serial.print(' ');
        Serial.print(p);      Serial.print(F("   "));
        Serial.print(portname(r)); Serial.print(F("   "));
        const int b = bitnum(m);
        if (b < 10) Serial.print(' ');
        Serial.print(b);
        const char *u = taken_for(p);
        if (u) { Serial.print(F("   <- ")); Serial.print(u); }
        Serial.println();
    }

    /* and then the answer to the question that prompted it */
    Serial.println(F("\n  GPIO9 pins, which is the register the bus driver writes whole:"));
    Serial.println(F("    pin   bit   status"));
    int free_pin = -1, free_bit = -1;
    for (uint8_t p = 0; p < CORE_NUM_DIGITAL; p++) {
        if (portOutputRegister(p) != &GPIO9_DR) continue;
        const int b = bitnum(digitalPinToBitMask(p));
        const char *u = taken_for(p);
        Serial.print(F("     "));
        if (p < 10) Serial.print(' ');
        Serial.print(p);   Serial.print(F("    "));
        if (b < 10) Serial.print(' ');
        Serial.print(b);   Serial.print(F("   "));
        if (u) {
            Serial.println(u);
        } else {
            Serial.println(F("FREE"));
            /* prefer the lowest free bit, purely so the choice is reproducible rather than
             * whichever one happened to be printed last */
            if (free_pin < 0) { free_pin = p; free_bit = b; }
        }
    }

    Serial.println(F("\n--- the repair this was for ---"));
    if (free_pin < 0) {
        Serial.println(F("  No free pin in GPIO9. The decoder enable would have to live in another"));
        Serial.println(F("  register, which costs a second store on every burst boundary."));
    } else {
        Serial.print(F("  Move the decoder's enable wire from pin 48 to pin "));
        Serial.print(free_pin);
        Serial.print(F(" (GPIO9 bit "));
        Serial.print(free_bit);
        Serial.println(F(")."));
        Serial.println(F("  Then: decoder enabled + CS0 high selects one of five breadboard banks;"));
        Serial.println(F("  decoder disabled + CS0 low selects the onboard chip. Nothing has to be"));
        Serial.println(F("  desoldered, and the usable capacity goes from 8 MB to 48 MB."));
        Serial.print(F("  In the driver: #define B_DEC (1u << "));
        Serial.print(free_bit);
        Serial.println(F(")"));
    }

    Serial.println(F("\n=== repeating in 20 s ==="));
    delay(20000);
}

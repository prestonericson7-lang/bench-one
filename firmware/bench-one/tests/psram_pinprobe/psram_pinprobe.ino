/* ===========================================================================================
 *  psram_pinprobe -- find a bridge between two pins, electrically, with no chip involved
 * ===========================================================================================
 *
 *  WHAT PROMPTED IT
 *  ----------------
 *  On the perfboard build every chip select came back silent, and the pattern in the silence is the
 *  clue. Nine of the ten probes read FF FF on the reply line. One -- CS1, Teensy pin 51 -- read 00 00.
 *
 *  One short explains both halves of that exactly. If pin 51 is bridged to pin 49, which is SIO1 and
 *  the line every chip answers on, then:
 *
 *      probing CS1      drives pin 51 low, which drags SIO1 low   -> 00 00
 *      probing anything else   holds pin 51 high, which pins SIO1 high  -> FF FF
 *
 *  and no chip anywhere on the bus can ever pull the reply line down against a pin driving it hard,
 *  so every chip looks absent -- including the two soldered to the Teensy's own footprints, which is
 *  why this looked like a board-wide fault rather than one joint.
 *
 *  That is a hypothesis with a number attached, and it takes one test to settle.
 *
 *
 *  HOW THE TEST WORKS, AND WHY ITS ANSWER IS NOT IN DOUBT
 *  -----------------------------------------------------
 *  Drive one pin LOW while every other pin is an input with its pull-up enabled, then read them all.
 *  Any pin that reads low is joined to the one being driven. A push-pull driver against a 22k pull-up
 *  is not a close contest, so a low reading means connected, full stop.
 *
 *  This matters because an earlier probe on this project got the opposite wrong. It reported three
 *  data lines "shorted to 3.3 V" using an internal pull-DOWN of about 100k against the input leakage
 *  of six chips, and the leakage won. Nothing here pulls against anything: every verdict comes from a
 *  real driver.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

static const uint8_t PIN[]  = { 53, 52, 49, 50, 54, 48, 51, 5, 2, 3, 4 };
static const char *NAME[]   = { "53 SCLK", "52 SIO0", "49 SIO1", "50 SIO2", "54 SIO3",
                                "48 CS0",  "51 CS1",  "5 DEC-EN", "2 A", "3 B", "4 C" };
#define NPIN (sizeof(PIN) / sizeof(PIN[0]))

extern "C" uint8_t external_psram_size;

static void all_input_pullup(void)
{
    for (unsigned i = 0; i < NPIN; i++) pinMode(PIN[i], INPUT_PULLUP);
    delayMicroseconds(300);
}

void setup()
{
    Serial.begin(115200);
    /* the hardware controller owns these pads at boot if it found anything, so take them back before
     * driving them, or half of this measures a fight rather than a wire */
    FLEXSPI2_MCR0 |= FLEXSPI_MCR0_MDIS;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_22 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_24 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_25 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_26 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_27 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_28 = 5;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_EMC_29 = 5;
    __asm__ volatile("dsb");
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  pin probe. No chip has to answer for any of this to be true."));
    Serial.println(F("=================================================================="));
    Serial.print(F("  core found "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB on the QSPI footprints at boot"));

    /* ---- 1. is any pin joined to any other pin ---------------------------------------------- */
    Serial.println(F("\n[1] bridges between pins"));
    Serial.println(F("    one pin driven LOW, all others input-pullup, then all read back"));
    int found = 0;
    for (unsigned i = 0; i < NPIN; i++) {
        all_input_pullup();
        pinMode(PIN[i], OUTPUT);
        digitalWriteFast(PIN[i], LOW);
        delayMicroseconds(500);

        bool first = true;
        for (unsigned j = 0; j < NPIN; j++) {
            if (j == i) continue;
            if (digitalReadFast(PIN[j]) == 0) {
                if (first) {
                    Serial.print(F("    BRIDGE  "));
                    Serial.print(NAME[i]);
                    Serial.print(F("  ---  "));
                    first = false;
                } else {
                    Serial.print(F(" and "));
                }
                Serial.print(NAME[j]);
                found++;
            }
        }
        if (!first) Serial.println();
        pinMode(PIN[i], INPUT_PULLUP);
    }
    if (!found)
        Serial.println(F("    none. No two of these pins are connected to each other."));

    /* ---- 2. is any pin welded to a rail ------------------------------------------------------ */
    Serial.println(F("\n[2] can each pin still be driven to both rails"));
    int stuck = 0;
    for (unsigned i = 0; i < NPIN; i++) {
        pinMode(PIN[i], OUTPUT);
        digitalWriteFast(PIN[i], HIGH); delayMicroseconds(300);
        const int hi = digitalReadFast(PIN[i]);
        digitalWriteFast(PIN[i], LOW);  delayMicroseconds(300);
        const int lo = digitalReadFast(PIN[i]);
        pinMode(PIN[i], INPUT_PULLUP);
        if (hi != 1 || lo != 0) {
            Serial.print(F("    STUCK   ")); Serial.print(NAME[i]);
            Serial.println(hi != 1 ? F("  cannot be driven HIGH -- tied to ground")
                                   : F("  cannot be driven LOW -- tied to 3.3 V"));
            stuck++;
        }
    }
    if (!stuck) Serial.println(F("    all of them. Nothing is welded to a rail."));

    /* ---- 3. what the reply line does while each select is asserted --------------------------- */
    Serial.println(F("\n[3] does SIO1 follow a chip select"));
    Serial.println(F("    SIO1 is the line every chip answers on. It must not move when a select"));
    Serial.println(F("    moves, because nothing but a chip should ever drive it."));
    for (unsigned i = 5; i <= 7; i++) {          /* CS0, CS1, DEC-EN */
        all_input_pullup();
        pinMode(PIN[2], INPUT_PULLUP);           /* SIO1 stays an input throughout */
        pinMode(PIN[i], OUTPUT);
        digitalWriteFast(PIN[i], HIGH); delayMicroseconds(300);
        const int with_high = digitalReadFast(PIN[2]);
        digitalWriteFast(PIN[i], LOW);  delayMicroseconds(300);
        const int with_low  = digitalReadFast(PIN[2]);
        pinMode(PIN[i], INPUT_PULLUP);

        Serial.print(F("    "));
        Serial.print(NAME[i]);
        Serial.print(F("  high -> SIO1 reads ")); Serial.print(with_high);
        Serial.print(F(",  low -> SIO1 reads "));  Serial.print(with_low);
        Serial.println(with_high != with_low ? F("   <-- SIO1 IS FOLLOWING IT. Bridge.")
                                             : F("   ok"));
    }

    Serial.println(F("\n--- what to do with this ---"));
    Serial.println(F("  A bridge names both ends. Reflow the two pads it names and nothing else;"));
    Serial.println(F("  everything downstream of here was silent because of it, so there is no"));
    Serial.println(F("  point reading any other result until this section is clean."));
    Serial.println(F("  Nothing found in any section means the pins are electrically fine and the"));
    Serial.println(F("  fault is somewhere a driven pin cannot see: power at the chips, or a wire"));
    Serial.println(F("  that goes to the right pad on the Teensy and the wrong pin on the PSRAM."));

    Serial.println(F("\n=== repeating in 12 s ==="));
    delay(12000);
}

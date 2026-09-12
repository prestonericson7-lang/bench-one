/* ===========================================================================================
 *  psram_pintruth -- the smallest possible question, asked with no history in the code
 * ===========================================================================================
 *
 *  Two previous probes disagreed with each other on this board. One said pin 49 and pin 51 were
 *  bridged; the other said driving pin 51 did not move pin 49. A bridge shows from both ends, so at
 *  least one of those tests was lying, and there is no point reading a third opinion from a tool that
 *  has already been caught.
 *
 *  So this asks the smallest question there is, for every pin, in four states:
 *
 *      driven HIGH      does the pad read back 1
 *      driven LOW       does the pad read back 0
 *      input, pull-up   does it float to 1
 *      input, pull-down does it float to 0
 *
 *  A pin with nothing attached gives 1, 0, 1, 0. Anything else is a fact about the board.
 *
 *
 *  THE CONTROL PINS ARE THE POINT
 *  ------------------------------
 *  Pins 14, 15 and 16 are in the list and are wired to nothing. If they come back 1, 0, 1, 0 then the
 *  method works and every other row is a real measurement. If they come back looking broken too, the
 *  method is broken and nothing else on the page means anything.
 *
 *  That check is here because the last two tools were trusted without one, and both were wrong.
 *
 *  Nothing else happens in this sketch. No FlexSPI, no decoder, no chip, no assumption about which
 *  pad is which or what was wired last time. Just pins.
 *
 *  BOARD    Teensy 4.1
 * ======================================================================================== */

#include <Arduino.h>

static const uint8_t PIN[] = { 53, 52, 49, 50, 54, 48, 51,  5,  2,  3,  4,   14, 15, 16 };
static const char *NAME[]  = { "53 SCLK", "52 SIO0", "49 SIO1", "50 SIO2", "54 SIO3",
                               "48 CS0",  "51 CS1",  "5 DEC-EN", "2 DEC-A", "3 DEC-B", "4 DEC-C",
                               "14 CONTROL", "15 CONTROL", "16 CONTROL" };
#define NPIN (sizeof(PIN) / sizeof(PIN[0]))

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    Serial.println();
    Serial.println(F("=================================================================="));
    Serial.println(F("  every pin, four states. 1 0 1 0 means nothing is attached."));
    Serial.println(F("=================================================================="));
    Serial.println(F("    pin           hi  lo  pu  pd   reading"));

    for (unsigned i = 0; i < NPIN; i++) {
        const uint8_t p = PIN[i];

        pinMode(p, OUTPUT);
        digitalWrite(p, HIGH);  delayMicroseconds(500);
        const int hi = digitalRead(p);

        digitalWrite(p, LOW);   delayMicroseconds(500);
        const int lo = digitalRead(p);

        pinMode(p, INPUT_PULLUP);   delayMicroseconds(2000);
        const int pu = digitalRead(p);

        pinMode(p, INPUT_PULLDOWN); delayMicroseconds(2000);
        const int pd = digitalRead(p);

        pinMode(p, INPUT_DISABLE);

        Serial.print(F("    "));
        Serial.print(NAME[i]);
        for (unsigned k = strlen(NAME[i]); k < 13; k++) Serial.print(' ');
        Serial.print(F(" ")); Serial.print(hi);
        Serial.print(F("   ")); Serial.print(lo);
        Serial.print(F("   ")); Serial.print(pu);
        Serial.print(F("   ")); Serial.print(pd);
        Serial.print(F("    "));

        if (hi == 1 && lo == 0 && pu == 1 && pd == 0)
            Serial.println(F("floats freely -- nothing holding it"));
        else if (hi == 0)
            Serial.println(F("CANNOT BE DRIVEN HIGH -- hard short to ground"));
        else if (lo == 1)
            Serial.println(F("CANNOT BE DRIVEN LOW -- hard short to 3.3 V"));
        else if (pu == 0 && pd == 0)
            Serial.println(F("held LOW when released -- something is pulling it down"));
        else if (pu == 1 && pd == 1)
            Serial.println(F("held HIGH when released -- something is pulling it up"));
        else
            Serial.println(F("odd"));
    }

    Serial.println(F("\n--- read the control rows first ---"));
    Serial.println(F("  Pins 14, 15 and 16 go nowhere. If they are not 1 0 1 0 then this method"));
    Serial.println(F("  is wrong and nothing above them should be believed, including by me."));
    Serial.println(F("  If they ARE 1 0 1 0, every other row is a fact about the board:"));
    Serial.println(F("    a pin that cannot be driven is shorted to that rail, full stop;"));
    Serial.println(F("    a pin held low when released has something pulling it, which on a"));
    Serial.println(F("    CMOS input should never happen -- chip inputs are high impedance."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

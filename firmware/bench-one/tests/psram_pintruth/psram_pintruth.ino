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

    /* ---- do the failing pins share a node ---------------------------------------------------- */
    Serial.println(F("\n[2] are the decoder pins connected to each other"));
    Serial.println(F("    drive one, read the others. Rising together means one shared node."));
    static const uint8_t D[3]     = { 2, 3, 4 };
    static const char *DN[3]      = { "2 DEC-A", "3 DEC-B", "4 DEC-C" };
    int coupled = 0, tested = 0;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) pinMode(D[k], INPUT_PULLDOWN);
        delayMicroseconds(1000);
        pinMode(D[i], OUTPUT);
        digitalWrite(D[i], HIGH);
        delayMicroseconds(2000);

        Serial.print(F("    driving ")); Serial.print(DN[i]); Serial.print(F(" high  ->  "));
        for (int k = 0; k < 3; k++) {
            if (k == i) continue;
            const int v = digitalRead(D[k]);
            Serial.print(DN[k]); Serial.print(F(" reads ")); Serial.print(v); Serial.print(F("   "));
            tested++;
            if (v) coupled++;
        }
        Serial.println();
        pinMode(D[i], INPUT_DISABLE);
    }
    for (int k = 0; k < 3; k++) pinMode(D[k], INPUT_DISABLE);

    Serial.print(F("    "));
    if (coupled == 0) {
        Serial.println(F("none of them moved. They are NOT joined to each other, so each pad is"));
        Serial.println(F("    separately touching ground. Look for solder, three times over."));
    } else if (coupled == tested) {
        Serial.println(F("they all move together, so they share one node -- and the only node all"));
        Serial.println(F("    three decoder inputs have in common is the chip's own supply. THE"));
        Serial.println(F("    DECODER HAS NO POWER. Check pin 16 to 3.3 V and pin 8 to ground; the"));
        Serial.println(F("    wires to A, B and C are probably fine and so is the solder on them."));
    } else {
        Serial.print(coupled); Serial.print('/'); Serial.print(tested);
        Serial.println(F(" moved. Partly joined, so it is likely both: a dead supply AND a"));
        Serial.println(F("    bridge. Fix the power first, then measure again."));
    }

    Serial.println(F("\n--- read the control rows first ---"));
    Serial.println(F("  Pins 2, 3 and 4 are the OLD decoder address pads, kept in the list as a"));
    Serial.println(F("  reference. They could not be driven high before and are expected to stay"));
    Serial.println(F("  that way; the address has moved to 14, 15 and 16."));
    Serial.println(F("  If they ARE 1 0 1 0, every other row is a fact about the board:"));
    Serial.println(F("    a pin that cannot be driven is shorted to that rail, full stop;"));
    Serial.println(F("    a pin held low when released has something pulling it, which on a"));
    Serial.println(F("    CMOS input should never happen -- chip inputs are high impedance."));

    Serial.println(F("\n=== repeating in 10 s ==="));
    delay(10000);
}

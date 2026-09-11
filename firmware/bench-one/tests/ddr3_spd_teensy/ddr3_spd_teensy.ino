/* ===========================================================================================
 *  ddr3_spd_teensy -- read a DDR3 stick's SPD EEPROM from a Teensy, four wires, no risk
 * ===========================================================================================
 *
 *  WHY THIS IS THE FIRST STEP AND NOT A DETOUR
 *  -------------------------------------------
 *  Driving DDR3 from a microcontroller needs a 1.5 V rail, two 0.75 V references, level
 *  translation on ten bidirectional lines, and a bit-banged controller. That is a weekend of work
 *  and a shopping list.
 *
 *  The SPD EEPROM on the same stick needs none of it. It is a separate I2C device with its own
 *  3.3 V supply pin, electrically isolated from the DRAM array, so a Teensy talks to it directly.
 *  Four wires. No 1.5 V anywhere. Nothing that can damage the DRAM, because the DRAM is not powered.
 *
 *  And it answers the two questions everything after it depends on:
 *
 *    1. CAN I SOLDER TO THESE CONTACTS AT ALL? The edge pads are 1.0 mm pitch gold. If 30 AWG
 *       wire-wrap will sit on them reliably, the rest of the project is mechanically possible. If it
 *       will not, you have found that out for the price of four wires instead of forty.
 *
 *    2. WHICH CHIP IS ACTUALLY ON THE STICK? Every timing number after this comes from that part's
 *       datasheet. Row and column address widths, bank count, density, tRFC, tRAS -- all of it is in
 *       the SPD, and all of it has to be right or the bit-banged controller addresses memory that
 *       is not there and reads back plausible rubbish.
 *
 *
 *  FINDING THE PINS, AND THE ONE CHECK TO DO FIRST
 *  -----------------------------------------------
 *  A 240-pin DDR3 UDIMM is numbered 1-120 along the front and 121-240 along the back, with pin 1
 *  and pin 121 back to back. The key notch is OFF CENTRE, which is what tells you which end is
 *  pin 1: the notch sits nearer pin 1's end on a DDR3 module.
 *
 *  The SPD signals, which want verifying against your own stick's datasheet before power:
 *
 *      VDDSPD   pin 236      3.3 V, the EEPROM's own supply
 *      SDA      pin 238      I2C data
 *      SCL      pin 118      I2C clock
 *      SA0      pin 117      address select, leave open for 0x50
 *      SA1      pin 237      address select, leave open
 *      SA2      pin 119      address select, leave open
 *      VSS      pin 239      ground, and dozens of others
 *
 *  THESE ARE FROM THE MICRON 240-PIN UDIMM PIN TABLE, NOT FROM MEMORY. An earlier draft of this
 *  file said SCL was 148 and SA0 was 147, which came from a search summary and was wrong. On a real
 *  DDR3 UDIMM pin 147 is DQ23, 148 is VSS and 149 is DQ28, so following that would have put 3.3 V
 *  onto a data pin of an unpowered DRAM. Check any pin number against the module's own datasheet
 *  before it sees a volt.
 *
 *  BEFORE APPLYING 3.3 V, meter it. Every VSS pin is continuous with every other VSS pin, so find
 *  two of them and confirm they beep together. Then confirm your candidate VDDSPD does NOT beep to
 *  VSS, and that SDA and SCL read open to everything. If VDDSPD beeps to ground you have miscounted
 *  and you are about to short your supply.
 *
 *
 *  WIRING
 *  ------
 *      stick pin 236  (VDDSPD) -> Teensy 3.3V
 *      stick pin 239  (VSS)    -> Teensy GND
 *      stick pin 238  (SDA)    -> Teensy pin 18, plus 4.7k to 3.3V
 *      stick pin 118  (SCL)    -> Teensy pin 19, plus 4.7k to 3.3V
 *
 *  The module carries no pullups -- a motherboard normally provides them -- so fit the two
 *  resistors. The Teensy's internal pullups are weak and will usually work over 50 mm of wire at
 *  100 kHz, so try it without first and add them if the read is flaky.
 *
 *  Nothing else on the stick gets connected. No 1.5 V, no VREF, no DQ.
 *
 *  BOARD    Teensy 4.1
 *  THEN     Serial Monitor at 115200
 * ======================================================================================== */

#include <Wire.h>

/* JEDEC puts the SPD at 0x50 through 0x57, selected by SA0..SA2. With those pins open it answers
 * at 0x50, but this scans the range because a stick pulled from a multi-slot board may have had
 * them strapped. */
#define SPD_FIRST 0x50
#define SPD_LAST  0x57
#define SPD_BYTES 256

static uint8_t spd[SPD_BYTES];
static uint8_t found_addr = 0;

/* The EEPROM is byte addressed: write the offset, then read. Reading one byte at a time is slow and
 * completely adequate for 256 bytes, and it makes a single bad byte obvious instead of letting a
 * failed block read look like a wall of zeroes. */
static int spd_read_byte(uint8_t addr, uint8_t off, uint8_t *out)
{
    Wire.beginTransmission(addr);
    Wire.write(off);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0;
    *out = (uint8_t)Wire.read();
    return 1;
}

static uint8_t scan(void)
{
    for (uint8_t a = SPD_FIRST; a <= SPD_LAST; a++) {
        uint8_t b;
        if (spd_read_byte(a, 2, &b)) {
            Serial.print(F("  answered at 0x"));
            Serial.print(a, HEX);
            Serial.print(F(", byte 2 = 0x"));
            Serial.println(b, HEX);
            return a;
        }
    }
    return 0;
}

/* ---- the handful of fields the bit-banged controller actually needs ------------------------- */

static const char *dram_type(uint8_t b)
{
    switch (b) {
        case 0x08: return "DDR2";
        case 0x0B: return "DDR3";
        case 0x0C: return "DDR4";
        default:   return "not a type this knows";
    }
}

static const char *module_type(uint8_t b)
{
    switch (b & 0x0F) {
        case 0x01: return "RDIMM, registered -- the command bus goes through a register chip";
        case 0x02: return "UDIMM, unbuffered -- what you want";
        case 0x03: return "SO-DIMM";
        case 0x04: return "Micro-DIMM";
        case 0x08: return "LRDIMM, load reduced -- buffered, do not start here";
        default:   return "unknown module type";
    }
}

static void report(void)
{
    const uint8_t b2 = spd[2], b3 = spd[3], b4 = spd[4], b5 = spd[5];
    const uint8_t b6 = spd[6], b7 = spd[7], b8 = spd[8];

    Serial.println(F("\n  WHAT THE STICK SAYS IT IS"));
    Serial.print(F("    type                 ")); Serial.println(dram_type(b2));
    Serial.print(F("    module               ")); Serial.println(module_type(b3));

    /* Byte 4: low nibble is the per-device density, bits 6:4 are bank address bits. */
    const uint8_t dens = b4 & 0x0F;
    const int bank_bits = 3 + ((b4 >> 4) & 0x07);
    const long dev_mbit = (dens <= 6) ? (256L << dens) : 0;
    Serial.print(F("    per-chip density     "));
    if (dev_mbit) { Serial.print(dev_mbit); Serial.println(F(" Mbit")); }
    else Serial.println(F("unrecognised"));
    Serial.print(F("    banks                ")); Serial.println(1 << bank_bits);

    /* Byte 5: bits 2:0 column address bits, bits 5:3 row address bits. THESE TWO NUMBERS ARE THE
     * ONES THE CONTROLLER CANNOT GUESS. Address the wrong width and every read is plausible rubbish
     * from somewhere else in the array. */
    const int col_bits = 9 + (b5 & 0x07);
    const int row_bits = 12 + ((b5 >> 3) & 0x07);
    Serial.print(F("    row address bits     ")); Serial.print(row_bits);
    Serial.println(F("   <- A0..A(n-1) on an ACTIVATE"));
    Serial.print(F("    column address bits  ")); Serial.print(col_bits);
    Serial.println(F("   <- A0..A(n-1) on a READ or WRITE"));
    Serial.print(F("    so you need           "));
    Serial.print(row_bits > col_bits ? row_bits : col_bits);
    Serial.print(F(" address pins plus "));
    Serial.print(bank_bits);
    Serial.println(F(" bank pins"));

    /* Byte 7: bits 2:0 device width, bits 5:3 ranks. Device width decides how many DQ lines one
     * chip owns, which is how many you have to wire. */
    const int dev_width = 4 << (b7 & 0x07);
    const int ranks = 1 + ((b7 >> 3) & 0x07);
    const int bus_width = 8 << (b8 & 0x07);
    Serial.print(F("    device width         x")); Serial.print(dev_width);
    Serial.println(F("   <- DQ lines to wire for ONE chip"));
    Serial.print(F("    ranks                ")); Serial.println(ranks);
    Serial.print(F("    module bus width     ")); Serial.println(bus_width);
    Serial.print(F("    chips per rank       ")); Serial.println(bus_width / dev_width);

    /* Byte 6: nominal voltage. Bit 0 clear means 1.5 V is NOT operable, bit 1 set means 1.35 V is. */
    Serial.print(F("    1.5 V operable       ")); Serial.println((b6 & 0x01) ? "no" : "yes");
    Serial.print(F("    1.35 V operable      ")); Serial.println((b6 & 0x02) ? "yes" : "no");

    /* What one chip is worth, which is the number that decides whether this is worth doing. */
    if (dev_mbit) {
        const long per_chip_mb = dev_mbit / 8;
        Serial.print(F("\n    ONE CHIP IS          "));
        Serial.print(per_chip_mb);
        Serial.println(F(" MB"));
        Serial.print(F("    nine Teensys would hold "));
        Serial.print(per_chip_mb * 9 / 1024.0f, 2);
        Serial.println(F(" GB, against 0.42 GB from the PSRAM bank"));
    }

    /* Part number, ASCII, bytes 128..145 on DDR3. This is what you search for to get the datasheet
     * the rest of the project depends on. */
    Serial.print(F("\n    module part number    "));
    for (int i = 128; i <= 145; i++) {
        const char c = (char)spd[i];
        Serial.print((c >= 32 && c < 127) ? c : ' ');
    }
    Serial.println();
    Serial.print(F("    manufacturer JEDEC ID 0x"));
    Serial.print(spd[117], HEX); Serial.print(' '); Serial.println(spd[118], HEX);

    Serial.println(F("\n  The part number is the point of this whole sketch. Every timing the"));
    Serial.println(F("  bit-banged controller needs -- tRCD, tRP, tRFC, tRAS max -- comes out of"));
    Serial.println(F("  that datasheet, and guessing them is how a controller reads back rubbish"));
    Serial.println(F("  that looks like data."));
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }

    Wire.begin();
    Wire.setClock(100000);          /* 100 kHz: slow, and the wires are long and unshielded */

    Serial.println(F("\n========================================================================"));
    Serial.println(F("  DDR3 stick, SPD EEPROM only · four wires · the DRAM is not powered"));
    Serial.println(F("========================================================================"));

    found_addr = scan();
    if (!found_addr) {
        Serial.println(F("\n  NOTHING ANSWERED on 0x50..0x57. In order of likelihood:"));
        Serial.println(F("    1. SDA and SCL swapped. Costs nothing to try the other way round."));
        Serial.println(F("    2. No pullups. Fit 4.7k from each of SDA and SCL to 3.3 V."));
        Serial.println(F("    3. VDDSPD not actually on 3.3 V. Meter it at the stick, not at the"));
        Serial.println(F("       Teensy -- a wire that looks soldered to a gold pad often is not."));
        Serial.println(F("    4. Miscounted pins. The key notch is OFF CENTRE and sits nearer the"));
        Serial.println(F("       pin 1 end; if you counted from the wrong end every pin is wrong."));
        Serial.println(F("    5. A dead EEPROM, which does happen on pulled sticks."));
        return;
    }

    int bad = 0;
    for (int i = 0; i < SPD_BYTES; i++)
        if (!spd_read_byte(found_addr, (uint8_t)i, &spd[i])) { spd[i] = 0; bad++; }

    Serial.print(F("\n  read 256 bytes, "));
    Serial.print(bad);
    Serial.println(F(" failed"));
    if (bad > 0)
        Serial.println(F("  Any failures at all mean the wiring is marginal. Fix it before trusting"
                         " anything below."));

    Serial.println(F("\n  RAW SPD"));
    for (int i = 0; i < SPD_BYTES; i += 16) {
        Serial.print(F("    "));
        if (i < 16) Serial.print('0');
        Serial.print(i, HEX);
        Serial.print(F(": "));
        for (int j = 0; j < 16; j++) {
            if (spd[i + j] < 16) Serial.print('0');
            Serial.print(spd[i + j], HEX);
            Serial.print(' ');
        }
        Serial.println();
    }

    if (spd[2] != 0x0B)
        Serial.println(F("\n  WARNING: byte 2 is not 0x0B, so this is not DDR3 and the decode below"
                         " will be wrong."));
    report();
}

void loop() { }

/* ===========================================================================================
 *  esp32_isp.ino -- program an ATtiny85 from an ESP32-S3, with no other tools
 * ===========================================================================================
 *
 *  WHY THIS EXISTS
 *  ----------------
 *  The guardian needs to get into an ATtiny85, and an ATtiny85 is programmed over ISP, which is
 *  SPI plus a reset line. The usual answer is a second Arduino running ArduinoISP with avrdude
 *  driving it from a PC: another board, a COM port, a command line, and three things that can
 *  each fail quietly.
 *
 *  None of it is necessary. The firmware is 526 bytes. It fits inside this sketch as an array,
 *  and this sketch speaks ISP itself. Upload once, open the serial monitor at 115200, and it
 *  erases, programs, verifies and fuses the chip while saying what it is doing at each step.
 *  Nothing else gets installed and nothing gets typed.
 *
 *  WIRING -- five wires, and the ATtiny takes its power from this board
 *  --------------------------------------------------------------------
 *      ESP32-S3            ATtiny85  (DIP-8, notch up: pin 1 is top-left, count down the left
 *      --------            side 1-4, then up the right side 5-8)
 *      GPIO 4   ------->   pin 7   PB2 / SCK
 *      GPIO 5   <-------   pin 6   PB1 / MISO
 *      GPIO 6   ------->   pin 5   PB0 / MOSI
 *      GPIO 7   ------->   pin 1   PB5 / RESET
 *      3V3      ------->   pin 8   VCC
 *      GND      ------->   pin 4   GND
 *
 *  One 0.1 uF ceramic capacitor (the ones marked 104) across pins 8 and 4, right at the chip.
 *  Nothing else. Leave the 10k off RESET while programming -- this sketch drives RESET both
 *  ways deliberately, and a pull-up only fights it.
 *
 *  Both ends are 3.3 V. The ATtiny85 is rated 2.7-5.5 V, so this is in spec, and programming it
 *  at the voltage it will actually run at avoids a whole class of bug where a part fused at 5 V
 *  misbehaves at 3.3.
 *
 *  BIT-BANGED ON PURPOSE
 *  ----------------------
 *  A factory ATtiny85 runs at 1 MHz, because CKDIV8 ships programmed. ISP demands a clock under
 *  a quarter of the target's, so under 250 kHz. This drives the lines by hand at roughly 100 kHz,
 *  which sits comfortably inside that limit and does not depend on which pins the S3's SPI
 *  peripheral happens to allow on your particular board.
 * ===========================================================================================
 */

#include "guardian_image.h"

#define PIN_SCK    4
#define PIN_MISO   5
#define PIN_MOSI   6
#define PIN_RESET  7

#define SCK_HALF_US   5              /* about 100 kHz, well under the 250 kHz ceiling */
#define PAGE_BYTES    64             /* ATtiny85 flash page = 32 words                */

/* ATtiny85. If the chip answers with any other signature this sketch stops, rather than write
 * to a part whose page size and fuse layout it does not know. */
static const uint8_t SIG_EXPECT[3] = { 0x1E, 0x93, 0x0B };

/* LOW  0xE2 : internal 8 MHz RC, CKDIV8 cleared. Factory is 0x62, which runs the chip at 1 MHz.
 * HIGH 0xDD : RESET stays RESET, ISP stays enabled, brown-out at 2.7 V for a 3.3 V rail.
 *             Factory is 0xDF, which has brown-out detection switched off altogether. */
static const uint8_t FUSE_LOW_WANT  = 0xE2;
static const uint8_t FUSE_HIGH_WANT = 0xDD;

/* ------------------------------------------------------------------------------------------- */

static uint8_t sbyte(uint8_t out)
{
    uint8_t in = 0;
    for (int8_t i = 7; i >= 0; i--) {
        digitalWrite(PIN_MOSI, (out >> i) & 1);
        delayMicroseconds(SCK_HALF_US);
        digitalWrite(PIN_SCK, HIGH);           /* mode 0: the target samples on the rising edge */
        in = (uint8_t)((in << 1) | (digitalRead(PIN_MISO) ? 1 : 0));
        delayMicroseconds(SCK_HALF_US);
        digitalWrite(PIN_SCK, LOW);
    }
    return in;
}

/* Every ISP instruction is a four byte exchange. The interesting reply is usually the fourth
 * byte received, but programming-enable hides its acknowledgement in the third, so return all. */
static void isp(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t *r)
{
    r[0] = sbyte(a); r[1] = sbyte(b); r[2] = sbyte(c); r[3] = sbyte(d);
}

static uint8_t isp4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t r[4];
    isp(a, b, c, d, r);
    return r[3];
}

/* The chip raises a busy flag during erase and page writes. Polling it beats a fixed delay: a
 * delay that is slightly too short corrupts the page and still reports success. */
static bool wait_ready(uint32_t ms)
{
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        if (!(isp4(0xF0, 0x00, 0x00, 0x00) & 1)) return true;
        delay(1);
    }
    return false;
}

static bool enter_prog()
{
    /* SCK MUST already be low when RESET falls. If RESET goes low while SCK is high the target
     * can come up in debugWIRE instead of serial programming, and then it ignores every
     * instruction below while looking exactly like a dead chip. */
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, LOW);
    digitalWrite(PIN_RESET, HIGH);
    delay(1);
    digitalWrite(PIN_RESET, LOW);
    delay(25);

    for (uint8_t attempt = 0; attempt < 4; attempt++) {
        uint8_t r[4];
        isp(0xAC, 0x53, 0x00, 0x00, r);
        if (r[2] == 0x53) return true;
        /* Nudge it: a positive pulse on RESET longer than two clock periods, then back down. */
        digitalWrite(PIN_RESET, HIGH);
        delayMicroseconds(100);
        digitalWrite(PIN_RESET, LOW);
        delay(25);
    }
    return false;
}

static void leave_prog()
{
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_RESET, HIGH);              /* release the target; it starts running */
}

static bool write_page(uint16_t byte_addr)
{
    const uint16_t word_base = (uint16_t)(byte_addr >> 1);

    for (uint8_t i = 0; i < PAGE_BYTES; i += 2) {
        const uint8_t w = (uint8_t)((word_base + (i >> 1)) & 0x1F);  /* word index inside page */
        isp4(0x40, 0x00, w, GUARDIAN[byte_addr + i]);                /* low  byte of the word  */
        isp4(0x48, 0x00, w, GUARDIAN[byte_addr + i + 1]);            /* high byte of the word  */
    }
    /* Commit. The page address goes in with the in-page bits masked off; setting them here is a
     * classic way to write the right bytes to the wrong page. */
    isp4(0x4C, (uint8_t)((word_base >> 8) & 0x0F), (uint8_t)(word_base & 0xE0), 0x00);
    return wait_ready(50);
}

static uint16_t verify_flash()
{
    uint16_t bad = 0;
    for (uint16_t i = 0; i < GUARDIAN_LEN; i += 2) {
        const uint16_t w  = (uint16_t)(i >> 1);
        const uint8_t  hi = (uint8_t)((w >> 8) & 0x0F);
        const uint8_t  lo = (uint8_t)(w & 0xFF);
        if (isp4(0x20, hi, lo, 0x00) != GUARDIAN[i])     bad++;
        if (isp4(0x28, hi, lo, 0x00) != GUARDIAN[i + 1]) bad++;
    }
    return bad;
}

static void show_fuses(const char *when)
{
    const uint8_t lo = isp4(0x50, 0x00, 0x00, 0x00);
    const uint8_t hi = isp4(0x58, 0x08, 0x00, 0x00);
    const uint8_t ex = isp4(0x50, 0x08, 0x00, 0x00);
    Serial.printf("  fuses %-6s  low 0x%02X   high 0x%02X   ext 0x%02X\n", when, lo, hi, ex);
}

/* ------------------------------------------------------------------------------------------- */

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("=====================================================");
    Serial.println("ATtiny85 programmer   --   BENCH ONE guardian");
    Serial.println("=====================================================");
    Serial.printf("image  : %u bytes, %u pages of %u\n",
                  GUARDIAN_LEN, GUARDIAN_LEN / PAGE_BYTES, PAGE_BYTES);
    Serial.println("wiring : GPIO4=SCK(p7)  5=MISO(p6)  6=MOSI(p5)  7=RESET(p1)");
    Serial.println("         3V3=p8  GND=p4  and a 104 cap across 8 and 4");
    Serial.println();

    pinMode(PIN_SCK, OUTPUT);
    pinMode(PIN_MOSI, OUTPUT);
    pinMode(PIN_RESET, OUTPUT);
    pinMode(PIN_MISO, INPUT);

    /* --- 1. get its attention ------------------------------------------------------------ */
    if (!enter_prog()) {
        Serial.println("FAILED: the chip never acknowledged programming mode.");
        Serial.println("  - check that GND and 3V3 really reach pins 4 and 8");
        Serial.println("  - RESET goes to pin 1. pin 1 and pin 8 are diagonal, easy to confuse");
        Serial.println("  - MISO is chip pin 6 and MOSI is chip pin 5. swapping them does this");
        return;
    }
    Serial.println("[1] programming mode entered");

    /* --- 2. confirm what it is before writing a single byte ------------------------------ */
    uint8_t sig[3];
    for (uint8_t i = 0; i < 3; i++) sig[i] = isp4(0x30, 0x00, i, 0x00);
    Serial.printf("[2] signature 0x%02X%02X%02X", sig[0], sig[1], sig[2]);

    if (sig[0] == 0x00 || sig[0] == 0xFF) {
        Serial.println("   nothing is answering. that is a wiring or power fault, not a bad chip.");
        leave_prog();
        return;
    }
    if (sig[0] != SIG_EXPECT[0] || sig[1] != SIG_EXPECT[1] || sig[2] != SIG_EXPECT[2]) {
        Serial.println("   NOT an ATtiny85. stopping instead of guessing its page layout.");
        leave_prog();
        return;
    }
    Serial.println("   ATtiny85 confirmed");
    show_fuses("before");

    /* --- 3. erase ------------------------------------------------------------------------ */
    isp4(0xAC, 0x80, 0x00, 0x00);
    if (!wait_ready(200)) {
        Serial.println("FAILED: erase never finished");
        leave_prog();
        return;
    }
    Serial.println("[3] chip erased");

    /* --- 4. program ---------------------------------------------------------------------- */
    Serial.print("[4] writing ");
    for (uint16_t a = 0; a < GUARDIAN_LEN; a += PAGE_BYTES) {
        if (!write_page(a)) {
            Serial.printf("\nFAILED: the page at 0x%04X did not commit\n", a);
            leave_prog();
            return;
        }
        Serial.print(".");
    }
    Serial.println(" done");

    /* --- 5. verify byte for byte, BEFORE touching any fuse ------------------------------- */
    const uint16_t bad = verify_flash();
    if (bad) {
        Serial.printf("[5] VERIFY FAILED: %u bytes differ. fuses left alone.\n", bad);
        Serial.println("    shorten the jumpers. ISP is happy at 100 kHz, but a loose wire on");
        Serial.println("    MISO reads back garbage that looks exactly like a faulty chip.");
        leave_prog();
        return;
    }
    Serial.printf("[5] verified %u of %u bytes\n", GUARDIAN_LEN, GUARDIAN_LEN);

    /* --- 6. fuses last, and only values that leave the chip reprogrammable -------------- */
    if ((FUSE_HIGH_WANT & 0x80) == 0) {
        Serial.println("[6] REFUSED: that high fuse clears RSTDISBL and would cost you RESET.");
        leave_prog();
        return;
    }
    if ((FUSE_HIGH_WANT & 0x20) != 0) {
        Serial.println("[6] REFUSED: that high fuse turns ISP off. recovery needs a 12 V rig.");
        leave_prog();
        return;
    }
    if ((FUSE_HIGH_WANT & 0x07) == 0x04) {
        Serial.println("[6] REFUSED: brown-out at 4.3 V on a 3.3 V rail holds the AVR in reset.");
        leave_prog();
        return;
    }
    isp4(0xAC, 0xA0, 0x00, FUSE_LOW_WANT);
    delay(10);
    isp4(0xAC, 0xA8, 0x00, FUSE_HIGH_WANT);
    delay(10);
    Serial.println("[6] fuses written");
    show_fuses("after");

    const uint8_t lo = isp4(0x50, 0x00, 0x00, 0x00);
    const uint8_t hi = isp4(0x58, 0x08, 0x00, 0x00);
    leave_prog();

    Serial.println();
    if (lo == FUSE_LOW_WANT && hi == FUSE_HIGH_WANT) {
        Serial.println("SUCCESS. the guardian is running on the ATtiny right now.");
        Serial.println("the LED on PB4 flashes while it waits for a heartbeat on PB0 and goes");
        Serial.println("dark once one arrives. unplug it and seat it beside the FPGA.");
    } else {
        Serial.printf("flash is good, but the fuses read back low 0x%02X high 0x%02X against "
                      "0x%02X / 0x%02X.\nthe code still runs, just at 1 MHz.\n",
                      lo, hi, FUSE_LOW_WANT, FUSE_HIGH_WANT);
    }
    Serial.println("press reset on the ESP32 to run the whole thing again.");
}

void loop()
{
    delay(1000);
}

/* ===========================================================================================
 *  hmi_e32r40t.ino -- BENCH ONE node 5: the 4.0in display
 * ===========================================================================================
 *
 *  Board: E32R40T. ESP32-WROOM-32E -- the CLASSIC ESP32, NOT an S3.
 *         4.0in ST7796 480x320 IPS + XPT2046 resistive touch, both on HSPI.
 *
 *  That distinction matters in three places and each one bites differently:
 *    - No native USB. The console is a CH340 on UART0, so the CDCOnBoot trap that costs an
 *      evening on an S3 does not exist here, and neither does the USB CDC port.
 *    - GPIO12 is MTDI, a strapping pin that sets the flash voltage at reset. It is the LCD's
 *      MISO on this board, which is fine because nothing pulls it at reset -- but it is why a
 *      pull-up added there later would brick the boot.
 *    - Pins 34-39 are input-only with no internal pull-ups.
 *
 *
 *  THE LINK, AND THE CONNECTOR THAT LOOKS RIGHT AND IS NOT
 *  --------------------------------------------------------
 *  This board has no 0.1in headers. Only 1.25 mm connectors on the back. The one silkscreened
 *  "UART" (P2) is the WRONG one: it is UART0, wired in parallel with the onboard CH340C through
 *  100 ohm series resistors. With USB attached the CH340 drives that net at low impedance, so a
 *  Teensy pulling low only gets this chip's RX down to about 2.2 V -- still a logic high. The
 *  link appears completely dead, but only when USB is plugged in, with nothing wrong anywhere
 *  you would think to look.
 *
 *  So the link uses P4, silkscreened "I2C": 3.3V / GPIO32 / GPIO25 / GND. Both are free,
 *  full-function pins, the connector has a real ground, and the classic ESP32's GPIO matrix
 *  routes UART2 to any pin. The USB console keeps working while the link is up.
 *
 *  The 10k pull-ups the vendor fitted on those two pins stay. For an I2C port they are required;
 *  for a UART they are useful, holding both lines idle-high before begin() and while the cable
 *  is unplugged, so a floating line is never read as a start bit.
 *
 *
 *  THE ROLE RULE
 *  -------------
 *  This node renders and reports. It decides nothing. A touch is an EVENT sent upward, not a
 *  command executed locally -- Teensy 1 decides what a touch means. Same rule the radio follows,
 *  and it is what lets the display be unplugged mid-session without anything else caring.
 * ===========================================================================================
 */

#include <Arduino.h>
#include <SPI.h>

/* ===========================================================================================
 * HMI_DISPLAY_ENABLED -- build the link half without the panel
 * ===========================================================================================
 * Set to 0 to compile and run the UART link, the touch-free status reporting and the whole
 * protocol layer with no graphics library at all.
 *
 * This is not a convenience switch, it is a bring-up tool and a blast shield. The link and the
 * panel are independent subsystems that fail for entirely different reasons, and being able to
 * prove one while the other is unavailable is what keeps a bring-up session moving.
 *
 * It is also load-bearing right now: see README-QUESTIONS.md Q5. The installed
 * "GFX Library for Arduino" 1.6.7 does not compile against the installed esp32 core 3.0.7 --
 * its QSPI backend references ESP_INTR_CPU_AFFINITY_AUTO, which arrives with arduino-esp32 3.1.
 * That is a library/core mismatch in the environment, not a fault in this sketch, and it would
 * equally break his own existing E32R40T firmware. Resolving it means moving a version, which
 * is his call because the same core builds the tested ESP32-S3 radio.
 * =========================================================================================== */
#ifndef HMI_DISPLAY_ENABLED
#define HMI_DISPLAY_ENABLED 1
#endif

/* The backend is TFT_eSPI, configured entirely by build_opt.h in this folder -- no edit to any
 * installed library, so his other display projects are untouched. See build_opt.h for why
 * Arduino_GFX, which his own firmware for this board uses, does not build here. */
#if HMI_DISPLAY_ENABLED
#include <TFT_eSPI.h>
#endif

#include "interop_protocol.h"
#include "bench_protocol.h"
#include "bench_pins.h"
#include "bench_link.h"

/* ===========================================================================================
 * DISPLAY
 * ===========================================================================================
 * A shared SPIClass in transaction mode, not the dedicated ESP32 SPI driver.
 *
 * This is the fix his own bring-up found on this exact board: the Arduino_ESP32SPI backend locks
 * the HSPI pins to the SPI peripheral, after which nothing else can talk to the XPT2046 sitting
 * on the same three wires. Arduino_HWSPI wraps each draw in beginTransaction/endTransaction, so
 * the touch controller can interleave safely.
 */
#if HMI_DISPLAY_ENABLED
/* One object drives BOTH the panel and the touch controller. That is the point: they share
 * HSPI, and his own bring-up on this board found that a separate touch library locks those pins
 * to the SPI peripheral so nothing else can reach the XPT2046. Letting one bus manager own both
 * removes the arbitration problem rather than solving it. */
static TFT_eSPI tft = TFT_eSPI();
#endif  /* HMI_DISPLAY_ENABLED */

#define SCREEN_W 480
#define SCREEN_H 320

/* Colours, RGB565. Deliberately few: this is an instrument panel, not a UI. */
#define C_BG     0x0000
#define C_INK    0xFFFF
#define C_DIM    0x8410
#define C_OK     0x07E0
#define C_WARN   0xFD20
#define C_BAD    0xF800
#define C_RULE   0x2124

/* ===========================================================================================
 * LINK
 * =========================================================================================== */
static BenchLink gMaster;
static HardwareSerial LinkSerial(HMI_LINK_UART_NUM);

/* ===========================================================================================
 * SCREEN STATE
 * ===========================================================================================
 * The state pushed from Teensy 1 is a screen id, a status byte, and a set of NUL-separated text
 * rows. Deliberately generic: this node has no model of the stack and should not acquire one.
 * Teensy 1 knows what is worth showing; this node knows how to show it.
 *
 * Rows are diffed against the previous frame and only changed rows are redrawn. That is not
 * polish -- a full 480x320 repaint is 307,200 bytes over SPI, about 61 ms at 40 MHz, and doing
 * that on every push would make the render time dominate every measurement taken through this
 * node. Redrawing one changed row is well under a millisecond.
 */
#define MAX_ROWS      12
#define MAX_ROW_CHARS 56

static char     gRows[MAX_ROWS][MAX_ROW_CHARS];
static char     gPrevRows[MAX_ROWS][MAX_ROW_CHARS];
static uint8_t  gRowCount;
static uint8_t  gScreen;
static uint8_t  gStatusFlags;
static bool     gFullRepaint = true;

static uint32_t gLastRenderUs;
static uint32_t gFrames;
static bool     gTouchOk;

/* ===========================================================================================
 * PAYLOAD HELPERS
 * =========================================================================================== */
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* ===========================================================================================
 * RENDERING
 * =========================================================================================== */

#define ROW_H      20
#define ROW_TOP    46
#define ROW_LEFT   14

#if HMI_DISPLAY_ENABLED
static void drawChrome(void)
{
    tft.fillScreen(C_BG);
    tft.setTextSize(2);
    tft.setTextColor(C_INK);
    tft.setCursor(ROW_LEFT, 12);
    tft.print("BENCH ONE");

    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(SCREEN_W - 150, 18);
    tft.print("E32R40T  HMI  0x08");

    tft.drawFastHLine(0, 38, SCREEN_W, C_RULE);
    tft.drawFastHLine(0, SCREEN_H - 26, SCREEN_W, C_RULE);
}

static void drawRow(uint8_t i)
{
    int16_t y = (int16_t)(ROW_TOP + i * ROW_H);
    tft.fillRect(ROW_LEFT, y, SCREEN_W - ROW_LEFT * 2, ROW_H, C_BG);

    if (i >= gRowCount) { return; }

    /* A leading marker character selects a colour, so Teensy 1 can flag a bad line without this
     * node needing to understand what any of it means. Keeping the semantics upstream is the
     * whole point of the role split. */
    const char *s = gRows[i];
    uint16_t col = C_INK;
    if      (s[0] == '!') { col = C_BAD;  s++; }
    else if (s[0] == '~') { col = C_WARN; s++; }
    else if (s[0] == '+') { col = C_OK;   s++; }
    else if (s[0] == '.') { col = C_DIM;  s++; }

    tft.setTextSize(2);
    tft.setTextColor(col);
    tft.setCursor(ROW_LEFT, y + 2);
    tft.print(s);
}

static void drawFooter(void)
{
    tft.fillRect(0, SCREEN_H - 24, SCREEN_W, 24, C_BG);
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(ROW_LEFT, SCREEN_H - 18);
    tft.printf("screen %u   frames %lu   render %lu us   touch %s   link %s",
                (unsigned)gScreen, (unsigned long)gFrames,
                (unsigned long)gLastRenderUs,
                gTouchOk ? "ok" : "--",
                gMaster.isUp(millis()) ? "up" : "DOWN");
}

static void render(void)
{
    uint32_t t0 = micros();

    if (gFullRepaint) {
        drawChrome();
        for (uint8_t i = 0; i < MAX_ROWS; i++) { drawRow(i); }
        memcpy(gPrevRows, gRows, sizeof(gPrevRows));
        gFullRepaint = false;
    } else {
        for (uint8_t i = 0; i < MAX_ROWS; i++) {
            if (strncmp(gRows[i], gPrevRows[i], MAX_ROW_CHARS) != 0) {
                drawRow(i);
                memcpy(gPrevRows[i], gRows[i], MAX_ROW_CHARS);
            }
        }
    }

    drawFooter();

    gLastRenderUs = micros() - t0;
    gFrames++;
}

/* ===========================================================================================
 * TOUCH
 * =========================================================================================== */

static void pollTouch(void)
{
    static bool wasDown = false;
    static uint32_t lastMs = 0;

    uint32_t now = millis();
    if ((now - lastMs) < 20u) { return; }   /* 50 Hz is plenty and keeps SPI free for drawing */
    lastMs = now;

    uint16_t tx = 0, ty = 0;
    bool isDown = tft.getTouch(&tx, &ty, 300);
    if (isDown) { gTouchOk = true; }

    if (isDown != wasDown) {
        uint8_t p[8];
        p[0] = isDown ? BENCH_HMI_INPUT_DOWN : BENCH_HMI_INPUT_UP;
        put16(&p[1], tx);
        put16(&p[3], ty);
        put16(&p[5], 0);
        p[7] = 0xFF;    /* widget id: this node does not know about widgets, and should not */

        /* An EVENT, not a command. Raw controller coordinates are sent uncalibrated and
         * unmapped, because calibration is a policy decision that belongs with whoever laid out
         * the screen -- and that is Teensy 1, which pushed the rows. */
        gMaster.event(BENCH_CHAN_HMI, BENCH_CMD_HMI_INPUT_EVENT, p, 8);
        wasDown = isDown;
    }
}

#else   /* HMI_DISPLAY_ENABLED == 0 -- link-only build */

/* Stubs, so every call site below stays identical in both builds. Rendering becomes a
 * measured no-op rather than a conditional scattered through the frame handlers: one #if
 * here beats fifteen at the call sites, and the link half is then provably the same code in
 * both configurations. */
static void drawChrome(void) {}
static void drawRow(uint8_t i) { (void)i; }
static void drawFooter(void) {}
static void render(void) { gLastRenderUs = 0; gFrames++; }
static void pollTouch(void) {}

#endif  /* HMI_DISPLAY_ENABLED */

/* ===========================================================================================
 * FRAME HANDLING
 * =========================================================================================== */

static void sendHello(void)
{
    uint8_t p[10];
    p[0] = BENCH_NODE_HMI;
    p[1] = IOP_PROTOCOL_VERSION;
    p[2] = IOP_PROTOCOL_MINOR;
    p[3] = BENCH_PROTOCOL_VERSION;
    p[4] = (uint8_t)esp_reset_reason();
    put16(&p[5], 0x0000u);
    put16(&p[7], IOP_CRC16_CHECK_VALUE);   /* header-drift check on the first frame           */
    p[9] = BENCH_FABRIC_HW_REV;
    gMaster.event(BENCH_CHAN_HMI, BENCH_CMD_HMI_HELLO, p, 10);
}

static void handleStateReq(const IopFrame *f)
{
    uint8_t resp[6];

    if (f->payload_len < 4u) {
        resp[0] = BENCH_ST_BAD_ARG; resp[1] = gScreen; put32(&resp[2], 0);
        gMaster.respond(f->seq, BENCH_CHAN_HMI, BENCH_CMD_HMI_STATE_RESP, resp, 6);
        return;
    }

    uint8_t  screen = f->payload[0];
    uint8_t  flags  = f->payload[1];
    uint16_t blobLen = get16(&f->payload[2]);
    if ((uint32_t)blobLen + 4u > f->payload_len) { blobLen = (uint16_t)(f->payload_len - 4u); }

    if (screen != gScreen || (flags & 0x01u)) { gFullRepaint = true; }
    gScreen = screen;
    gStatusFlags = flags;

    /* Split the blob on NUL into rows. Bounded on both axes: a row longer than the buffer is
     * truncated rather than overrunning, and rows beyond MAX_ROWS are dropped. Truncation is
     * visible on screen; an overrun would corrupt something unrelated and surface much later. */
    memset(gRows, 0, sizeof(gRows));
    gRowCount = 0;
    uint16_t i = 0;
    while (i < blobLen && gRowCount < MAX_ROWS) {
        uint8_t c = 0;
        while (i < blobLen && f->payload[4 + i] != 0u && c < (MAX_ROW_CHARS - 1u)) {
            gRows[gRowCount][c++] = (char)f->payload[4 + i];
            i++;
        }
        gRows[gRowCount][c] = '\0';
        gRowCount++;
        while (i < blobLen && f->payload[4 + i] != 0u) { i++; }   /* skip the overlong tail   */
        i++;                                                       /* step past the NUL       */
    }

    render();

    resp[0] = BENCH_ST_OK;
    resp[1] = gScreen;
    put32(&resp[2], gLastRenderUs);
    gMaster.respond(f->seq, BENCH_CHAN_HMI, BENCH_CMD_HMI_STATE_RESP, resp, 6);
}

static void onFrame(void *ctx, const IopFrame *f, bool solicited)
{
    (void)ctx; (void)solicited;

    if (f->chan == IOP_CHAN_TRANSPORT) {
        if (f->cmd == IOP_CMD_PING) {
            gMaster.respond(f->seq, IOP_CHAN_TRANSPORT, IOP_CMD_PONG, 0, 0);
        }
        return;
    }

    if (f->chan != BENCH_CHAN_HMI) {
        gMaster.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CHAN);
        return;
    }

    switch (f->cmd) {
    case BENCH_CMD_HMI_STATE_REQ:
        handleStateReq(f);
        break;

    case BENCH_CMD_HMI_STATUS_REQ: {
        uint8_t p[10];
        p[0] = BENCH_ST_OK;
        p[1] = gScreen;
        p[2] = (gLastRenderUs > 0u) ? (uint8_t)(1000000ul / gLastRenderUs) : 0u;
        p[3] = gTouchOk ? 1u : 0u;
        p[4] = 0;                       /* SD not mounted in v1                               */
        put32(&p[5], millis());
        p[9] = 0;
        gMaster.respond(f->seq, BENCH_CHAN_HMI, BENCH_CMD_HMI_STATUS_RESP, p, 10);
        break;
    }

    case BENCH_CMD_HMI_BL_REQ: {
        uint8_t level = (f->payload_len > 0u) ? f->payload[0] : 255u;
        ledcWrite(HMI_LCD_BL, level);
        uint8_t p[2] = { BENCH_ST_OK, level };
        gMaster.respond(f->seq, BENCH_CHAN_HMI, BENCH_CMD_HMI_BL_RESP, p, 2);
        break;
    }

    default:
        gMaster.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
        break;
    }
}

static void onTimeout(void *ctx, uint8_t chan, uint8_t cmd, uint8_t seq)
{
    (void)ctx; (void)chan; (void)cmd; (void)seq;
    /* This node initiates no requests. The hook exists so a future one has somewhere to land
     * rather than being silently ignored. */
}

/* ===========================================================================================
 * SETUP AND LOOP
 * =========================================================================================== */

void setup(void)
{
    Serial.begin(115200);   /* CH340 on UART0. Independent of the link, which is on UART2. */

    /* RGB LED is common anode: HIGH is off. Driven off first so the board does not sit lit
     * during init for no reason. */
    pinMode(HMI_LED_R, OUTPUT); digitalWrite(HMI_LED_R, HIGH);
    pinMode(HMI_LED_G, OUTPUT); digitalWrite(HMI_LED_G, HIGH);
    pinMode(HMI_LED_B, OUTPUT); digitalWrite(HMI_LED_B, HIGH);

    /* Audio amplifier enable is active LOW on this board. Held HIGH so the speaker stays
     * silent: an amplifier enabled with a floating input hisses, and this node has no reason
     * to make noise. */
    pinMode(HMI_AUDIO_EN, OUTPUT); digitalWrite(HMI_AUDIO_EN, HIGH);

    /* Backlight on a PWM channel. Arduino-ESP32 3.x binds the channel to the pin directly;
     * the older ledcSetup/ledcAttachPin pair was removed in that major version. */
    ledcAttach(HMI_LCD_BL, 5000 /* Hz */, 8 /* bits */);
    ledcWrite(HMI_LCD_BL, 0);   /* dark until the panel is initialised, so no flash of noise */

#if HMI_DISPLAY_ENABLED
    tft.init();
    tft.fillScreen(C_BG);
    ledcWrite(HMI_LCD_BL, 200);
#endif

    /* The touch controller shares the LCD's bus and is begun with the SAME SPIClass, which is
     * what makes the transaction arbitration work. Giving it its own SPIClass on the same pins
     * is the mistake that makes touch appear dead. */
#if HMI_DISPLAY_ENABLED
    /* Touch calibration is deliberately NOT applied here. Raw controller counts are what get
     * sent upward, because mapping them to screen coordinates is a policy decision that belongs
     * with whoever laid out the screen -- and that is Teensy 1, which pushed the rows. */
#endif

    /* Protocol self-test before the link is trusted. Ten milliseconds of arithmetic that turns
     * "the link does not work" into a specific answer about whether the fault is in this file or
     * in the wiring. */
    static IopParser scratch;
    uint16_t failures = iop_selftest(&scratch);

    LinkSerial.begin(BENCH_LINK_BAUD, SERIAL_8N1, HMI_LINK_RX, HMI_LINK_TX);
    gMaster.begin(&LinkSerial, "master", BENCH_NODE_TEENSY1, BENCH_LINK_BAUD);
    gMaster.setHandlers(0, onFrame, onTimeout);

    /* begin() above already called LinkSerial.begin with the default pins; calling it again with
     * the explicit pin map is harmless and is what actually binds UART2 to GPIO25/32 through the
     * GPIO matrix. Order matters only in that the pin-mapped call must be the last one. */
    LinkSerial.begin(BENCH_LINK_BAUD, SERIAL_8N1, HMI_LINK_RX, HMI_LINK_TX);

    gScreen = 0;
    gRowCount = 3;
    strncpy(gRows[0], "waiting for TEENSY1", MAX_ROW_CHARS - 1);
    strncpy(gRows[1], ".link  Serial7  pins 28/29", MAX_ROW_CHARS - 1);
    strncpy(gRows[2], ".here  UART2    P4: IO25 tx / IO32 rx", MAX_ROW_CHARS - 1);

    if (failures != 0u) {
        snprintf(gRows[0], MAX_ROW_CHARS, "!PROTOCOL SELFTEST FAILED 0x%04X", (unsigned)failures);
        strncpy(gRows[1], "!every frame built here would be malformed", MAX_ROW_CHARS - 1);
        strncpy(gRows[2], "!the wiring is innocent -- fix the header", MAX_ROW_CHARS - 1);
        gRowCount = 3;
    }

    gFullRepaint = true;
    render();

    if (failures == 0u) { sendHello(); }
}

void loop(void)
{
    gMaster.poll();
    pollTouch();

    /* The footer carries the link state, so it has to refresh even when no state is pushed --
     * otherwise a link that went down would look fine indefinitely. Once a second is enough and
     * costs a 480x24 fill, well under a millisecond. */
    static uint32_t lastFooter = 0;
    uint32_t now = millis();
    if ((now - lastFooter) >= 1000u) {
        lastFooter = now;
        drawFooter();

        /* Green while the link is up, red when it is down. Visible from across the desk, which
         * is the point: a node that has hung with its link up looks identical to an idle one
         * over the wire. */
        bool up = gMaster.isUp(now);
        digitalWrite(HMI_LED_G, up ? LOW : HIGH);
        digitalWrite(HMI_LED_R, up ? HIGH : LOW);
    }
}

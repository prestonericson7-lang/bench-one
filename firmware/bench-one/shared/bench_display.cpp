/* ===========================================================================================
 *  bench_display.cpp -- 2.4 inch SPI TFT on Teensy 1, SPI0
 * ===========================================================================================
 *
 *  FOUR THINGS THE RESEARCH CHANGED IN THIS FILE
 *  ----------------------------------------------
 *  1. THE DRIVER. Adafruit_ILI9341 1.6.3 silently defaults to 8 MHz on a Teensy 4.1 -- its
 *     version check matches none of the Teensy 3.x branches and falls through to a TEENSYDUINO
 *     catch-all. A full 240x320 repaint at 8 MHz is 153.6 ms. Worse, Adafruit_GFX's DMA path is
 *     compiled only for SAMD51/SAMD/nRF52, so on a Teensy 4 writePixels() degrades to a
 *     per-pixel polled loop and dmaWait() is a no-op: the Adafruit stack is 100% blocking here.
 *     Teensyduino bundles ILI9341_t3 and ST7789_t3, which are written for this processor. Used.
 *
 *  2. THE PROBE CLOCK. Both controllers cap READS at a 150 ns cycle -- 6.67 MHz -- while allowing
 *     100 ns or faster writes. Probing at the write clock returns plausible-looking garbage.
 *     PJRC's own driver hardcodes 6.5 MHz for reads, which corroborates it exactly.
 *
 *  3. THE PROBE COMMAND. ILI9341 register 0x04 is NOT an identity test: its datasheet Default
 *     Value column literally reads "See description", because ID1/ID2/ID3 are the MODULE maker's
 *     ids, not the IC's. Only 0xD3 has a fixed value (0x00/0x93/0x41). Plenty of code on the
 *     internet tests 0x04 and concludes wrongly. And reading 0xD3 on an Ilitek part wants the
 *     0xD9 index trick rather than a plain dummy byte.
 *
 *  4. THE LATENCY PROBLEM IS NOT A DRIVER PROBLEM. No driver makes a repaint free, so the fix is
 *     structural: measure the fabric transaction with the cycle counter wrapped tightly around
 *     the transaction itself, never around the loop iteration, and give rendering an explicit
 *     time budget it cannot exceed. Then a blocking repaint is incapable of being counted as
 *     latency, whatever driver is underneath. benchDisplayService() is that budget.
 * ===========================================================================================
 */

#include "bench_display.h"

#if BENCH_TFT_ENABLED

#include <SPI.h>
#include <stdarg.h>

/* ONE DRIVER, CHOSEN AT COMPILE TIME -- and this was forced, then turned out to be better.
 *
 * ILI9341_t3 and ST7735_t3 both define ILI9341_t3_font_t, so including both is a hard compile
 * error. Linking only the one in use is smaller anyway, and it makes the runtime probe do
 * something more useful than switching a pointer: it becomes a CHECK. If the chip disagrees with
 * the compiled driver, the firmware says so loudly instead of silently rendering wrong.
 *
 * Flip BENCH_TFT_DRIVER if the boot message tells you to. */
#if BENCH_TFT_DRIVER == BENCH_TFT_ST7789
  #include <ST7789_t3.h>
  static ST7789_t3 tft(T1_TFT_CS, T1_TFT_DC, T1_TFT_RST);
#else
  #include <ILI9341_t3.h>
  static ILI9341_t3 tft(T1_TFT_CS, T1_TFT_DC, T1_TFT_RST,
                        T1_TFT_MOSI, T1_TFT_SCK, T1_TFT_MISO);
#endif

static uint8_t  gController = BENCH_TFT_DEFAULT;
static bool     gIdentified = false;
static uint32_t gRawId      = 0;
static bool     gMismatch   = false;

static char     gRows[BENCH_TFT_ROWS][BENCH_TFT_ROW_CHARS];
static char     gPrev[BENCH_TFT_ROWS][BENCH_TFT_ROW_CHARS];
static bool     gFullRepaint = true;
static uint8_t  gDirtyCursor = 0;      /* where the budgeted service resumes                  */

#define C_BG    0x0000
#define C_INK   0xFFFF
#define C_DIM   0x8410
#define C_OK    0x07E0
#define C_WARN  0xFD20
#define C_BAD   0xF800
#define C_RULE  0x2124

#define ROW_H    20
#define ROW_TOP  30
#define ROW_X    8

/* Read clock. 6.5 MHz, matching PJRC's own ILI9341_t3, because both datasheets cap reads at a
 * 150 ns cycle. Writes run far faster; these two numbers are genuinely different and using the
 * write clock for a read is the classic way to get a confident wrong answer. */
#define TFT_READ_HZ   6500000ul
#define TFT_WRITE_HZ  30000000ul

/* ===========================================================================================
 * CONTROLLER IDENTIFICATION
 * =========================================================================================== */

static uint8_t ili9341ReadReg(uint8_t reg, uint8_t index)
{
    /* The Ilitek index trick: 0xD9 with 0x10|index selects WHICH parameter byte the next read
     * of `reg` returns. Reading 0xD3 with a plain dummy byte works on some modules and returns
     * shifted data on others, because the datasheets describe a full dummy BYTE while at least
     * one careful implementation reports a dummy BIT. This sidesteps that disagreement entirely
     * by asking for one specific byte at a time. */
    SPI.beginTransaction(SPISettings(TFT_READ_HZ, MSBFIRST, SPI_MODE0));

    digitalWriteFast(T1_TFT_DC, LOW);
    digitalWriteFast(T1_TFT_CS, LOW);
    SPI.transfer(0xD9);
    digitalWriteFast(T1_TFT_DC, HIGH);
    SPI.transfer(0x10 + index);
    digitalWriteFast(T1_TFT_CS, HIGH);

    digitalWriteFast(T1_TFT_DC, LOW);
    digitalWriteFast(T1_TFT_CS, LOW);
    SPI.transfer(reg);
    digitalWriteFast(T1_TFT_DC, HIGH);
    uint8_t v = SPI.transfer(0x00);
    digitalWriteFast(T1_TFT_CS, HIGH);

    SPI.endTransaction();
    return v;
}

static uint8_t identifyController(void)
{
    pinMode(T1_TFT_CS, OUTPUT);
    pinMode(T1_TFT_DC, OUTPUT);
    digitalWriteFast(T1_TFT_CS, HIGH);

    pinMode(T1_TFT_RST, OUTPUT);
    digitalWriteFast(T1_TFT_RST, LOW);
    delay(10);
    digitalWriteFast(T1_TFT_RST, HIGH);
    delay(150);           /* both parts want time after reset before they answer anything */

    SPI.begin();

    /* ILI9341 is the POSITIVE test, and it is the only one of the two that can be. Register 0xD3
     * has a fixed value baked into the die -- 0x00/0x93/0x41 -- across power-on and software
     * reset. The ST7789 has no equivalent: its 0x04 values are power-on defaults held in NVM
     * that the PANEL MAKER can reprogram. So ST7789 is never confirmed, only inferred from
     * ILI9341 not answering. Structuring it the other way round would produce false positives. */
    uint8_t d1 = ili9341ReadReg(0xD3, 1);
    uint8_t d2 = ili9341ReadReg(0xD3, 2);
    uint8_t d3 = ili9341ReadReg(0xD3, 3);
    gRawId = ((uint32_t)d1 << 16) | ((uint32_t)d2 << 8) | d3;

    if (d2 == 0x93u && d3 == 0x41u) {
        gIdentified = true;
        return BENCH_TFT_ILI9341;
    }

    /* All-zeros or all-ones means MISO is not connected, not that the part is an ST7789. Many
     * ST7789 breakouts are 7-pin -- GND VCC SCL SDA RES DC BLK -- with no SDO pad at all, and
     * some are strapped into a single-bidirectional-data-line mode a Teensy cannot turn around.
     * Those modules genuinely cannot be probed, and saying "assumed" is the honest output. */
    gIdentified = false;
    return BENCH_TFT_DEFAULT;
}

const char *benchDisplayControllerName(void)
{
    return (gController == BENCH_TFT_ST7789) ? "ST7789" : "ILI9341";
}
uint8_t  benchDisplayController(void) { return gController; }
bool     benchDisplayIdentified(void) { return gIdentified; }
uint32_t benchDisplayRawId(void)      { return gRawId; }
bool     benchDisplayMismatch(void)   { return gMismatch; }

/* ===========================================================================================
 * DRAWING -- one wrapper so nothing below branches on the controller
 * =========================================================================================== */

static void gfxFillScreen(uint16_t c) { tft.fillScreen(c); }
static void gfxFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{ tft.fillRect(x, y, w, h, c); }
static void gfxHLine(int16_t x, int16_t y, int16_t w, uint16_t c)
{ tft.drawFastHLine(x, y, w, c); }
static void gfxText(int16_t x, int16_t y, uint16_t colour, uint8_t size, const char *s)
{
    tft.setTextSize(size);
    tft.setTextColor(colour);
    tft.setCursor(x, y);
    tft.print(s);
}

static void drawChrome(void)
{
    gfxFillScreen(C_BG);
    gfxText(ROW_X, 6, C_INK, 2, "BENCH ONE");
    gfxText(BENCH_TFT_W - 126, 8, C_DIM, 1, benchDisplayControllerName());
    gfxText(BENCH_TFT_W - 126, 18, gMismatch ? C_BAD : (gIdentified ? C_OK : C_WARN), 1,
            gMismatch ? "MISMATCH!" : (gIdentified ? "detected" : "ASSUMED"));
    gfxHLine(0, 26, BENCH_TFT_W, C_RULE);
}

static void drawRow(uint8_t i)
{
    int16_t y = (int16_t)(ROW_TOP + i * ROW_H);
    gfxFillRect(ROW_X, y, BENCH_TFT_W - ROW_X * 2, ROW_H, C_BG);
    if (gRows[i][0] == '\0') { return; }

    const char *s = gRows[i];
    uint16_t col = C_INK;
    if      (s[0] == '!') { col = C_BAD;  s++; }
    else if (s[0] == '~') { col = C_WARN; s++; }
    else if (s[0] == '+') { col = C_OK;   s++; }
    else if (s[0] == '.') { col = C_DIM;  s++; }

    gfxText(ROW_X, y + 3, col, 2, s);
}

/* ===========================================================================================
 * PUBLIC
 * =========================================================================================== */

uint8_t benchDisplayBegin(void)
{
    memset(gRows, 0, sizeof(gRows));
    memset(gPrev, 0xFF, sizeof(gPrev));    /* nothing matches, so the first render paints all */

    /* Backlight dark until the controller is configured. An uninitialised panel shows RAM noise,
     * and lighting it early only makes the noise visible. */
    pinMode(T1_TFT_BL, OUTPUT);
    analogWrite(T1_TFT_BL, 0);

    uint8_t found = identifyController();
    gController = BENCH_TFT_DRIVER;

#if BENCH_TFT_DRIVER == BENCH_TFT_ST7789
    tft.init(240, 320);
#else
    /* PJRC's driver already defaults to 30 MHz write and 6.5 MHz read -- ILI9341_SPICLOCK and
     * ILI9341_SPICLOCK_READ. That is why this one is used instead of the Adafruit stack, which
     * silently falls back to 8 MHz on a Teensy 4.1 and has no DMA path compiled for it at all. */
    tft.begin();
#endif
    tft.setRotation(1);

    /* THE CHECK. If the chip answered and disagrees with what was compiled, say so as loudly as
     * a headless board can. Rendering will look wrong -- shifted, inverted, or blank -- and
     * without this line that symptom is indistinguishable from a wiring fault. */
    gMismatch = (gIdentified && found != BENCH_TFT_DRIVER);

    gFullRepaint = true;
    benchDisplayRender();

    analogWriteFrequency(T1_TFT_BL, 4000);   /* above audible, below anything that smears */
    analogWrite(T1_TFT_BL, 200);

    return gController;
}

void benchDisplayBacklight(uint8_t level) { analogWrite(T1_TFT_BL, level); }
void benchDisplayInvalidate(void)         { gFullRepaint = true; gDirtyCursor = 0; }

void benchDisplaySetRow(uint8_t row, const char *text)
{
    if (row >= BENCH_TFT_ROWS || !text) { return; }
    strncpy(gRows[row], text, BENCH_TFT_ROW_CHARS - 1);
    gRows[row][BENCH_TFT_ROW_CHARS - 1] = '\0';
}

void benchDisplayPrintf(uint8_t row, const char *fmt, ...)
{
    if (row >= BENCH_TFT_ROWS) { return; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gRows[row], BENCH_TFT_ROW_CHARS, fmt, ap);
    va_end(ap);
}

uint32_t benchDisplayRender(void)
{
    uint32_t t0 = micros();

    if (gFullRepaint) {
        drawChrome();
        for (uint8_t i = 0; i < BENCH_TFT_ROWS; i++) { drawRow(i); }
        memcpy(gPrev, gRows, sizeof(gPrev));
        gFullRepaint = false;
    } else {
        for (uint8_t i = 0; i < BENCH_TFT_ROWS; i++) {
            if (strncmp(gRows[i], gPrev[i], BENCH_TFT_ROW_CHARS) != 0) {
                drawRow(i);
                memcpy(gPrev[i], gRows[i], BENCH_TFT_ROW_CHARS);
            }
        }
    }

    return micros() - t0;
}

uint32_t benchDisplayService(uint32_t budget_us)
{
    /* THE STRUCTURAL FIX, and it matters more than the driver choice.
     *
     * No driver makes a repaint free. On a Teensy 4 the Adafruit stack has no DMA at all and
     * even the PJRC drivers block for the duration of a fill. So the defence cannot be "make it
     * fast"; it has to be "make it bounded, and never let it run at the wrong moment".
     *
     * At 30 MHz the panel takes 3.75 bytes per microsecond, so a 2000 us budget is roughly one
     * 240x15 strip. This function draws at most one row per call and stops the moment the budget
     * is gone, resuming where it left off next time.
     *
     * The other half of the fix lives in the caller: time a fabric transaction with the cycle
     * counter wrapped tightly around THE TRANSACTION, never around the loop iteration. Do both
     * and a blocking repaint becomes structurally incapable of being recorded as latency --
     * which is the exact failure this project's bring-up log already documents once, where a
     * display refresh landing between a request and its response was counted as round-trip time
     * and corrupted the very statistic the display existed to show. */
    uint32_t t0 = micros();

    if (gFullRepaint) {
        /* A full repaint cannot be budgeted meaningfully -- it is one indivisible 153,600-byte
         * push. It happens once at boot and after an explicit invalidate, both of which are
         * moments when nothing is being measured. */
        return benchDisplayRender();
    }

    uint8_t scanned = 0;
    while (scanned < BENCH_TFT_ROWS) {
        uint8_t i = gDirtyCursor;
        gDirtyCursor = (uint8_t)((gDirtyCursor + 1u) % BENCH_TFT_ROWS);
        scanned++;

        if (strncmp(gRows[i], gPrev[i], BENCH_TFT_ROW_CHARS) != 0) {
            drawRow(i);
            memcpy(gPrev[i], gRows[i], BENCH_TFT_ROW_CHARS);
            if ((micros() - t0) >= budget_us) { break; }
        }
    }

    return micros() - t0;
}

#else   /* BENCH_TFT_ENABLED == 0 */

uint8_t  benchDisplayBegin(void)      { return 0; }
bool     benchDisplayIdentified(void) { return false; }
uint8_t  benchDisplayController(void) { return 0; }
uint32_t benchDisplayRawId(void)      { return 0; }
const char *benchDisplayControllerName(void) { return "none"; }
void benchDisplaySetRow(uint8_t, const char *) {}
void benchDisplayPrintf(uint8_t, const char *, ...) {}
uint32_t benchDisplayRender(void)     { return 0; }
uint32_t benchDisplayService(uint32_t) { return 0; }
bool     benchDisplayMismatch(void)   { return false; }
void benchDisplayBacklight(uint8_t)   {}
void benchDisplayInvalidate(void)     {}

#endif  /* BENCH_TFT_ENABLED */

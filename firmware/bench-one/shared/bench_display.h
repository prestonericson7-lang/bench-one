/* ===========================================================================================
 *  bench_display.h -- the 2.4 inch SPI TFT on Teensy 1
 * ===========================================================================================
 *
 *  Confirmed by the builder: the 2.4in screen is a TFT, not an OLED.
 *
 *  WHY THIS DETECTS THE CONTROLLER AT RUNTIME
 *  ------------------------------------------
 *  The BOM says ST7789. Most 2.4 inch 240x320 SPI modules sold are ILI9341 -- ST7789 is far more
 *  common on 1.3 inch 240x240 and 2.0 inch panels. Both are plausible, they need different
 *  drivers, and getting it wrong does not fail cleanly: the wrong driver typically produces a
 *  shifted image, inverted colours, or a blank screen with no error anywhere.
 *
 *  Rather than make him open the box and squint at a chip, this reads the controller's own ID
 *  register and picks. If the module does not bring MISO out -- and many cheap ones do not, which
 *  makes the read impossible -- it falls back to BENCH_TFT_DEFAULT and says so on the console.
 *
 *  Detection is honest about its own limits: it reports what it found, what it assumed, and
 *  which of those two it is. A driver that silently guesses is how a shifted image becomes an
 *  afternoon of wiring checks.
 *
 *
 *  WHY IT IS ON SPI0 AND ALONE THERE
 *  ----------------------------------
 *  A full 240x320 16-bit repaint is 153,600 bytes. At 30 MHz that is roughly 41 ms of solid bus
 *  time. If the display shared the fabric's bus, every fabric read would queue behind whatever
 *  the screen was doing.
 *
 *  Worse, and this is the lesson already recorded in this project's bring-up log: a blocking
 *  refresh that lands between sending a request and reading its response gets RECORDED AS
 *  LATENCY. The measurement the display exists to show becomes the thing the display corrupts.
 *  Two defences: the display has its own bus, and it only ever repaints rows that changed.
 * ===========================================================================================
 */

#ifndef BENCH_DISPLAY_H
#define BENCH_DISPLAY_H

#include <Arduino.h>
#include <stdint.h>
#include "bench_pins.h"

/* Set to 0 to compile the panel out entirely -- no library, no RAM, no SPI traffic. Useful for
 * bringing up the links before the screen is wired, and for isolating a fault to one subsystem. */
#ifndef BENCH_TFT_ENABLED
#define BENCH_TFT_ENABLED 1
#endif

/* Used when the controller cannot be identified, which happens whenever the module does not
 * bring MISO out to a pin. */
#define BENCH_TFT_ILI9341   1
#define BENCH_TFT_ST7789    2
#ifndef BENCH_TFT_DEFAULT
#define BENCH_TFT_DEFAULT   BENCH_TFT_ILI9341
#endif

/* Which driver is COMPILED IN. Only one can be: ILI9341_t3 and ST7735_t3 both define
 * ILI9341_t3_font_t, so including both is a hard compile error.
 *
 * That constraint turned out to be an improvement. Linking one driver is smaller, and it gives
 * the runtime probe a better job than switching a pointer -- it becomes a CHECK. If the chip
 * answers and disagrees with what was compiled, the firmware says so on the console and on the
 * screen, instead of rendering wrong and looking like a wiring fault. */
#ifndef BENCH_TFT_DRIVER
#define BENCH_TFT_DRIVER    BENCH_TFT_ILI9341
#endif

#define BENCH_TFT_W         320     /* landscape */
#define BENCH_TFT_H         240

#define BENCH_TFT_ROWS      10
#define BENCH_TFT_ROW_CHARS 40

/* Bring the panel up, identify the controller, and paint the frame. Returns the detected
 * controller, or BENCH_TFT_DEFAULT if it had to assume. */
uint8_t benchDisplayBegin(void);

/* True if the controller was READ rather than assumed. Reported on the console and in status,
 * because "we assumed ILI9341 and it looks right" and "we read 0x9341 off the chip" are
 * different claims and only one of them is evidence. */
bool benchDisplayIdentified(void);

uint8_t benchDisplayController(void);
const char *benchDisplayControllerName(void);

/* The raw 24 bits read back from register 0xD3, for the console. 0x009341 is an ILI9341;
 * 0x000000 or 0xFFFFFF means MISO is not connected and nothing was learned. */
uint32_t benchDisplayRawId(void);

/* True if the chip answered and disagrees with BENCH_TFT_DRIVER. Flip the define and rebuild. */
bool benchDisplayMismatch(void);

/* Set one text row. Rows are diffed against the previous frame; only changed rows repaint.
 * A leading marker character picks a colour, so a caller can flag a bad line without this
 * module needing to understand what any of it means:
 *      '!' bad    '~' warn    '+' ok    '.' dim    otherwise normal
 */
void benchDisplaySetRow(uint8_t row, const char *text);
void benchDisplayPrintf(uint8_t row, const char *fmt, ...);

/* Repaint whatever changed. Cheap when nothing did. Returns microseconds spent drawing, so the
 * cost is measurable rather than assumed. */
uint32_t benchDisplayRender(void);

/* Draw at most one changed row and stop when the budget is spent, resuming next call.
 *
 * This is the structural defence against a repaint being recorded as latency. No driver makes a
 * repaint free -- on a Teensy 4 the Adafruit stack has no DMA at all -- so the answer is not
 * "make it fast" but "make it bounded, and never run it at the wrong moment". At 30 MHz the
 * panel takes 3.75 bytes per microsecond, so 2000 us is roughly one 240x15 strip.
 *
 * The other half belongs to the caller: wrap the cycle counter tightly around the fabric
 * TRANSACTION, never around the loop iteration. */
uint32_t benchDisplayService(uint32_t budget_us);

/* Backlight, 0-255. */
void benchDisplayBacklight(uint8_t level);

/* Force a full repaint on the next render -- after a mode change or a visual glitch. */
void benchDisplayInvalidate(void);

#endif /* BENCH_DISPLAY_H */

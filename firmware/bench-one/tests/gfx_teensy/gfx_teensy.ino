/* ===========================================================================================
 *  gfx_teensy -- a Teensy 4.1 renders its share of the machine's picture
 * ===========================================================================================
 *
 *  The Teensy was given the verdict "carries no weights": 16 MB of PSRAM against a 1.79 GB model,
 *  and 21.4 MB/s of 4-bit unpacking. During inference it has nothing to do.
 *
 *  It is not idle because it is weak. It runs the same integer rasteriser as the desktop and the
 *  Luckfox -- literally the same file, machine_scene.h, not a port of it -- so a band rendered here
 *  is byte-for-byte what the desktop would have produced for those rows. That is the whole point:
 *  nine of these are a third of the boards in the machine, and if their bands stitch exactly then
 *  the fleet is one renderer rather than nine toys.
 *
 *  Three measurements, in the order that matters:
 *
 *    1. A 64-row band out of internal RAM. This is the honest per-node figure, and it is directly
 *       comparable with the Luckfox: same rows, same scene, same code.
 *    2. The same band out of PSRAM, if fitted. Says what the extra memory costs in throughput.
 *    3. The whole 480x320 frame out of PSRAM. Only possible at all because of the PSRAM, and the
 *       number it produces is the reason bands exist.
 *
 *  BUILD
 *      build.bat            (copies machine_scene.h in, compiles, uploads)
 * ======================================================================================== */

#include "machine_scene.h"

#define FW      480
#define FH      320
#define BAND_Y0 128          /* the busiest rows of the scene, and the ones the Luckfox timed */
#define BAND_Y1 192
#define BAND_H  (BAND_Y1 - BAND_Y0)
#define FRAMES  120

/* Internal RAM. 480 x 64 x 2 bytes twice is 123 KB, which fits with room to spare. */
static uint16_t fb_int[FW * BAND_H];
static uint16_t zb_int[FW * BAND_H];

/* PSRAM. EXTMEM is empty and harmless on a board without the chips fitted. */
EXTMEM static uint16_t fb_ext[FW * FH];
EXTMEM static uint16_t zb_ext[FW * FH];

extern "C" uint8_t external_psram_size;

static void run(const char *tag, uint16_t *fb, uint16_t *zb, int y0, int y1)
{
    scene_setup(fb, zb, NULL, FW, FH, y0, y1);

    /* One frame first, untimed. The first pass through any of this pulls the scene table and the
     * code itself into cache, and timing that instead of the rasteriser is how a benchmark ends up
     * flattering whichever buffer it happened to test second. */
    scene_frame(0);

    const uint32_t t0 = micros();
    long pix = 0;
    for (int f = 0; f < FRAMES; f++) pix += scene_frame(f);
    const uint32_t dt = micros() - t0;

    const float secs = (float)dt * 1e-6f;
    Serial.print(F("  "));
    Serial.print(tag);
    Serial.print(F("  rows "));
    Serial.print(y0); Serial.print(F("..")); Serial.print(y1 - 1);
    Serial.print(F("   "));
    Serial.print((float)FRAMES / secs, 1);
    Serial.print(F(" frames/s   "));
    Serial.print((float)pix / secs * 1e-6f, 2);
    Serial.print(F(" M pixels/s   "));
    Serial.print((int)((uint32_t)FW * (y1 - y0) * 4 / 1024));
    Serial.println(F(" KB of buffers"));
}

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }

    Serial.println(F("\n========================================================================"));
    Serial.println(F("  a Teensy 4.1 draws its share of the machine"));
    Serial.println(F("========================================================================"));
    Serial.print(F("  CPU "));
    Serial.print(F_CPU / 1000000);
    Serial.print(F(" MHz, PSRAM "));
    Serial.print(external_psram_size);
    Serial.println(F(" MB"));
    Serial.println(F("  same rasteriser as the desktop and the Luckfox, same file, not a port"));
    Serial.println();

    run("band, internal RAM ", fb_int, zb_int, BAND_Y0, BAND_Y1);

    if (external_psram_size > 0) {
        run("band, PSRAM       ", fb_ext, zb_ext, BAND_Y0, BAND_Y1);
        run("whole frame, PSRAM", fb_ext, zb_ext, 0, FH);
    } else {
        Serial.println(F("  no PSRAM fitted, so the whole-frame case cannot run here"));
    }

    Serial.println();
    Serial.println(F("  For scale, the same 64-row band on a Luckfox Pico: 696.4 frames/s."));
    Serial.println(F("  Nine of these boards are 27% of the machine and hold none of the model."));
}

void loop() { }

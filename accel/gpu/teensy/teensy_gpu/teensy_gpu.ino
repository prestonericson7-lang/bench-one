/*
 * teensy_gpu.ino -- Teensy 4.1 geometry engine for the FPGA-GPU (SPEC.md sections 3, 6, 13, 14).
 *
 * Build:  teensy\build_teensy.cmd        (copies common/ sources into src/, Teensy 4.1, USB Serial,
 *                                          600 MHz -> out\teensy\600\; "build_teensy.cmd 816" =
 *                                          816 MHz overclock -> out\teensy\816\)
 * Flash:  teensy\flash_teensy.cmd [600|816] [port]
 *
 * Files:  tg_core.c     portable protocol handler, bus safety, autonomous mode (shared with the
 *                       x86 simulator teensy/sim/teensy_sim.c)
 *         tg_teensy.cpp Teensy-only glue: USB CDC, parallel bus driver, timing, LED
 *         src/          geom.c, gpu_setup.c + headers (copied from common/ by the build script)
 *
 * The USB serial port carries ONLY the binary T_* protocol: nothing may print to Serial.
 * LED (pin 13): blinking 1 Hz = running, bus pins high-Z (FPGA not seen);
 *               solid = bus enabled (FPGA drove BUSY low for >= 10 ms);
 *               fast 8 Hz blink forever = pin table check failed, bus disabled permanently.
 */
#include "tg_common.h"

void tg_teensy_begin(void);
bool tg_teensy_pins_ok(void);

static bool g_dtr;

void setup()
{
    tg_teensy_begin();
    tg_init();
}

void loop()
{
    /* The host (re)opened the port (DTR rising): drop any half-received message of an earlier
     * session so the new session's first message is parsed from its header. */
    bool dtr = Serial.dtr();
    if (dtr && !g_dtr)
        tg_host_reset();
    g_dtr = dtr;

    tg_poll();
    if (!tg_teensy_pins_ok()) {
        /* The core's pin table does not match SPEC 3 (cannot happen with the Teensy 4.1 core this
         * was built against: also checked at compile time). The bus is never driven (the platform
         * refuses, sends fail with GPU_ERR_BUS); USB still answers; the LED blinks fast. */
        digitalWriteFast(13, (millis() / 62) & 1);
    }
}

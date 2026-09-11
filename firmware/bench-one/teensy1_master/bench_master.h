/* ===========================================================================================
 *  bench_master.h -- BENCH ONE additions to the tested Teensy 1 master controller
 * ===========================================================================================
 *
 *  HOW THIS INTEGRATES, AND WHY IT IS SHAPED THIS WAY
 *  ---------------------------------------------------
 *  `teensy_master_controller.ino` is 149 KB of tested code carrying 1.4M passing assertions and
 *  a hardware baseline of 53,530 frames with zero errors of every class. The temptation is to
 *  open it and weave the new links through its existing structures.
 *
 *  That is the wrong move, and the reason is not sentiment. Its radio link, its parser, its
 *  pending-request slot and its console are correct BECAUSE they were measured. Editing them
 *  puts that evidence back in question, and the measurement that would restore it costs a bench
 *  session. Every line added to that file is a line that has to be re-proven.
 *
 *  So the integration is four lines in his sketch:
 *
 *      #include "bench_master.h"      // near his other includes
 *      benchSetup();                  // last line of setup()
 *      benchLoop();                   // in loop(), alongside his existing calls
 *      if (benchConsole(cmd)) return;  // first line of his console dispatcher (optional)
 *
 *  Everything else lives here. His radio link is not touched, not wrapped, and not re-parsed.
 *  If this module is deleted, his sketch is byte-for-byte the tested one again.
 *
 *
 *  WHAT THIS MODULE OWNS
 *  ---------------------
 *    Link A  Serial2  pins 7/8    -> Luckfox Pico Mini B   (orchestrator, channel 0x07)
 *    Link C  Serial3  pins 14/15  -> Teensy 4.1 #2         (worker, channel 0x06)
 *    Link D  Serial7  pins 28/29  -> E32R40T display node  (HMI, channel 0x08)
 *    The 74HC/MCP fabric on SPI1 and Wire                  (channel 0x05)
 *
 *  It does NOT own Serial1. That is his radio link and it stays his.
 *
 *
 *  THE ROUTING RULE
 *  ----------------
 *  The Luckfox can ask this node to forward a frame to the radio, the worker or the display
 *  (ORC_FWD_REQ). It cannot address them directly, and that is deliberate: every link here is
 *  point-to-point with no addressing, and adding addressing to the frame header would have meant
 *  touching the tested parser. Forwarding through the hub costs one hop and keeps the wire
 *  format identical on all four links -- which is also what lets one analyser decoder and one
 *  Python codec serve the whole stack.
 * ===========================================================================================
 */

#ifndef BENCH_MASTER_H
#define BENCH_MASTER_H

#include <Arduino.h>
#include "bench_protocol.h"
#include "bench_pins.h"
#include "bench_link.h"
#include "bench_fabric.h"

/* Call from the end of setup(). Brings up the three new links and the fabric, runs the port
 * table validator, and announces this node upward with an ORC_HELLO. */
void benchSetup(void);

/* Call from loop(). Non-blocking: polls three links, services the fabric interrupt, and pumps
 * the event subscription. */
void benchLoop(void);

/* Optional. Handles the BENCH ONE console commands and returns true if it consumed the line, so
 * his existing dispatcher can be left exactly as it is. Returns false for anything it does not
 * recognise. */
bool benchConsole(const char *line);

/* Exposed so his sketch can print them in its own status command if wanted. */
extern BenchLink   gLuckfox;
extern BenchLink   gWorker;
extern BenchLink   gHmi;
extern BenchFabric gFabric;

#endif /* BENCH_MASTER_H */

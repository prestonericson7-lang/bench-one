/*
 * tg_plat.h -- the platform layer under the portable firmware core (tg_core.c).
 *
 * Implemented twice:
 *   teensy_gpu/tg_teensy.cpp   the real Teensy 4.1 (USB CDC, GPIO6/GPIO9 parallel bus, DWT, LED)
 *   sim/teensy_sim.c           x86 simulator (TCP 7779 instead of USB, TCP 7778 instead of the bus)
 * Everything protocol-, geometry- and safety-related lives in tg_core.c and is identical on both.
 */
#ifndef TG_PLAT_H
#define TG_PLAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t tgp_micros(void);                         /* free-running microseconds (wraps) */
uint32_t tgp_cpu_mhz(void);                        /* F_CPU / 1e6 */

/* Host link (USB CDC on the Teensy). */
int  tgp_usb_read(void *buf, uint32_t max);        /* non-blocking: bytes read (0 = none yet) */
void tgp_usb_write(const void *buf, uint32_t n);   /* blocking (the platform may drop on timeout) */
void tgp_usb_flush(void);                          /* push out a partially filled packet */

/* Parallel bus (SPEC section 3). The core decides WHEN; the platform only does the pin work. */
int  tgp_bus_busy(void);                           /* raw BUSY input: 1 = busy / not driven low */
int  tgp_bus_drive(int enable);                    /* 1: D, SOR, STROBE -> outputs, all driven 0 first;
                                                      0: all back to high-impedance inputs.
                                                      Returns the resulting state (1 = driving); the
                                                      platform may refuse to drive (then 0). */
int  tgp_bus_send(const uint32_t rec[24]);         /* one record = 48 transfers, no BUSY check.
                                                      0 = sent, <0 = not sent (link lost / not driving) */

void tgp_led(int on);
void tgp_idle(void);                               /* called in busy-wait loops (the simulator waits
                                                      briefly for socket events; the Teensy returns) */

#ifdef __cplusplus
}
#endif
#endif

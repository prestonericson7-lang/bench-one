/*
 * tg_core.h -- portable Teensy geometry-engine firmware core: USB message handler for the T_*
 * protocol (common/gpu_proto.h), mesh store + geom_frame (common/geom.c), parallel-bus safety
 * state machine (SPEC 3), autonomous mode (SPEC 14), statistics. Shared verbatim by the Teensy
 * sketch and the x86 simulator; the platform layer is tg_plat.h.
 */
#ifndef TG_CORE_H
#define TG_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limits of this firmware (not part of the wire protocol) */
#define TG_MAX_DRAWS        256          /* draws per T_FRAME */
#define TG_BUS_ARM_US       10000u       /* BUSY must read 0 this long before the pins are driven */
#define TG_BUS_TIMEOUT_US   500000u      /* BUSY = 1 this long while sending -> pins released, GPU_ERR_BUS */
#define TG_RX_TIMEOUT_US    1000000u     /* a message stalled this long mid-payload is dropped */
#define TG_AUTO_POLL_US     1000u        /* autonomous mode: service USB at least this often */

void tg_init(void);          /* once at boot (after the platform set the pins to inputs) */
void tg_poll(void);          /* call continuously: host messages, bus state, autonomous frames, LED */
void tg_host_reset(void);    /* host link was re-established: drop any partial message */
int  tg_wants_cpu(void);     /* 1 = tg_poll has work right now (an autonomous frame is due, or
                                received bytes are waiting): the simulator must not sleep */

#ifdef __cplusplus
}
#endif
#endif

/* ===========================================================================================
 *  mesh_protocol.h  --  what ESP32 radios say to each other over ESP-NOW
 * ===========================================================================================
 *
 *  This is the SECOND protocol in this project, and it is deliberately not the first one.
 *
 *      Teensy  <--UART, interop_protocol.h-->  hub ESP32  <--ESP-NOW, THIS FILE-->  node ESP32s
 *
 *  WHY NOT REUSE THE UART FRAMING. Every reason interop_protocol.h has a start byte, a length,
 *  a CRC and a resynchronising parser is a property of a RAW SERIAL STREAM: no packet
 *  boundaries, no integrity check, no addressing. ESP-NOW has all three built in. It delivers
 *  discrete packets of known length, the 802.11 MAC already CRCs every frame and discards
 *  corrupt ones, and each packet carries the sender's MAC. Re-implementing framing on top would
 *  add bytes and complexity to solve problems that no longer exist.
 *
 *  So this protocol is a plain packed struct. That is not laziness -- it is the correct answer
 *  once the transport provides framing, and recognising when NOT to add a layer is as much of
 *  the job as knowing how to build one.
 *
 *  WHAT ESP-NOW DOES NOT GIVE YOU, and what this file therefore still has to:
 *    - No retransmission. A lost packet is simply gone; the send callback reports whether the
 *      MAC got an ack, nothing more. Hence a sequence number, so loss is COUNTED not guessed.
 *    - No ordering guarantee.
 *    - A hard 250-byte payload ceiling.
 *    - No clock relationship between boards. Nothing here assumes two boards agree on time --
 *      see the timing note below, which is the most important paragraph in this file.
 *
 *  THE TIMING DESIGN, and why there is no timestamp comparison anywhere
 *  --------------------------------------------------------------------
 *  The obvious way to measure "how long did that take" is to stamp the packet on the node and
 *  compare against the hub's clock on arrival. That number would be meaningless: two ESP32s have
 *  independent oscillators with no discipline between them, so the difference is dominated by an
 *  unknown, drifting offset. It would look plausible and be wrong -- the worst kind of
 *  measurement.
 *
 *  Instead every figure here is a DELTA MEASURED ON A SINGLE CLOCK:
 *
 *    int_to_send_us   node clock only: the IMU's data-ready interrupt to the moment
 *                     esp_now_send() is called. Pure node-side latency.
 *    ping round trip  hub clock only: hub sends, node echoes, hub stops the timer. Halving it
 *                     estimates one-way air time without either board knowing the other's clock.
 *    UART leg         Teensy clock only, via the DWT cycle counter, already built.
 *
 *  Those three sum to the end-to-end path, and no term requires two clocks to agree.
 * ===========================================================================================
 */

#ifndef MESH_PROTOCOL_H
#define MESH_PROTOCOL_H

#include <stdint.h>

/* ESP-NOW's payload ceiling is a radio limit, not ours. */
#define MESH_MAX_ESPNOW_PAYLOAD   250u

/* Message types. Hub-to-node commands are 0x1x, node-to-hub reports are 0x2x, purely so an
 * unexpected direction is obvious in a hex dump. */
#define MESH_MSG_CONFIG           0x10u  /* hub  -> node: change mode/rate                   */
#define MESH_MSG_PING             0x11u  /* hub  -> node: echo this back immediately         */
#define MESH_MSG_IDENTIFY         0x12u  /* hub  -> node: who are you                        */

#define MESH_MSG_SAMPLE           0x20u  /* node -> hub:  IMU data                           */
#define MESH_MSG_PONG             0x21u  /* node -> hub:  ping echo                          */
#define MESH_MSG_HELLO            0x22u  /* node -> hub:  identity, sent on boot and on ask   */

/* Sampling modes -- both implemented so the trade can be measured rather than argued about. */
#define MESH_MODE_ON_INTERRUPT    0x00u  /* one packet per IMU sample, at the data-ready IRQ.
                                          * Highest fidelity: every packet carries its own
                                          * node-side latency. Also the highest packet rate,
                                          * which is the point when stress-testing the mesh.   */
#define MESH_MODE_BATCHED         0x01u  /* accumulate and send periodically. Far fewer
                                          * packets, but per-sample latency is blurred into a
                                          * batch interval -- you measure the batch, not the
                                          * event.                                            */
#define MESH_MODE_IDLE            0x02u  /* sample nothing; the node stays reachable           */

/* 16 samples x 12 bytes = 192, plus a 12-byte header = 204, comfortably inside 250. Chosen as a
 * round number with headroom rather than squeezing to the limit, because a payload that exactly
 * fills the maximum leaves nowhere to add a field later without a format break. */
#define MESH_MAX_SAMPLES          16u

/* One IMU reading: raw counts, exactly as the MPU-6050 reports them.
 *
 * RAW, NOT SCALED, and deliberately: converting to g and deg/s on the node would bake in an
 * assumed full-scale range, cost float maths in an interrupt-driven path, and throw away the
 * exact bits the sensor produced. The brain has a 600 MHz FPU and knows the configured range.
 * Same principle as forwarding raw 802.11 reason codes: send the fact, interpret at the top. */
typedef struct __attribute__((packed)) {
    int16_t ax, ay, az;
    int16_t gx, gy, gz;
} MeshSample;

typedef struct __attribute__((packed)) {
    uint8_t  type;            /* MESH_MSG_SAMPLE                                             */
    uint8_t  mode;            /* which mode produced this                                    */
    uint16_t seq;             /* rolling; a gap is packet loss, and loss is expected          */
    uint32_t t_first_us;      /* node clock at the first sample -- node-relative only         */
    uint16_t int_to_send_us;  /* node-local latency: data-ready IRQ -> esp_now_send()         */
    uint8_t  count;           /* samples actually present                                    */
    uint8_t  dropped;         /* samples the node itself discarded since the last packet,
                               * saturating. A node that cannot keep up must SAY so rather
                               * than silently thin the stream.                              */
    MeshSample samples[MESH_MAX_SAMPLES];
} MeshSampleMsg;

typedef struct __attribute__((packed)) {
    uint8_t  type;            /* MESH_MSG_PING or MESH_MSG_PONG                              */
    uint32_t token;           /* echoed verbatim, so a late pong cannot be counted as a fast
                               * one -- the same stale-response problem SEQ solves on the UART */
} MeshPingMsg;

typedef struct __attribute__((packed)) {
    uint8_t  type;            /* MESH_MSG_CONFIG                                             */
    uint8_t  mode;
    uint16_t rate_hz;         /* target sample rate; 0 leaves the sensor's own rate alone     */
    uint8_t  batch;           /* samples per packet in batched mode, clamped to MESH_MAX      */
} MeshConfigMsg;

typedef struct __attribute__((packed)) {
    uint8_t  type;            /* MESH_MSG_HELLO                                              */
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  caps;            /* MESH_CAP_*                                                  */
    uint8_t  sensor_ok;       /* 1 if the IMU answered and identified itself                 */
    uint8_t  who_am_i;        /* the sensor's WHO_AM_I, so clones are visible from the brain  */
} MeshHelloMsg;

/* ---- Wire offsets into MeshSampleMsg, derived rather than counted ----
 *
 * WHY THESE EXIST. A receiver cannot simply cast a received buffer to MeshSampleMsg*: the payload
 * sits at an arbitrary offset inside a larger frame, and a packed struct at an odd address is an
 * unaligned access -- a fault or a silently wrong read on Cortex-M. So receivers decode byte by
 * byte, and byte-by-byte decoding needs offsets.
 *
 * THE FIRST SET WAS HAND-COUNTED AND WRONG. The Teensy decoder used 10 / 12 / 14 where the truth
 * is 8 / 10 / 12, because the field widths were mis-added by two. Every symptom was silent: the
 * CRC passed, the bounds checks passed, no counter moved. A live accelerometer simply reported
 * a = 0 0 0, g = 0 0 0 with a latency of 1 us. The only clue was that the numbers were not
 * plausible -- and "the data looks wrong" is a far weaker signal than a failed assertion.
 *
 * offsetof() is unavailable in some Arduino translation units without <stddef.h>, and duplicating
 * the arithmetic in three sketches is what caused the bug in the first place. So the offsets are
 * defined ONCE, here, next to the struct they describe, and the static assertion below makes the
 * compiler check them against the real layout. If a field is ever added, moved or resized, every
 * board fails to BUILD rather than quietly decoding garbage. */
#define MESH_SAMPLE_OFF_TYPE      0u
#define MESH_SAMPLE_OFF_MODE      1u
#define MESH_SAMPLE_OFF_SEQ       2u   /* uint16, little-endian (ESP32 native)  */
#define MESH_SAMPLE_OFF_TFIRST    4u   /* uint32                                */
#define MESH_SAMPLE_OFF_LATENCY   8u   /* uint16 -- int_to_send_us              */
#define MESH_SAMPLE_OFF_COUNT    10u
#define MESH_SAMPLE_OFF_DROPPED  11u
#define MESH_SAMPLE_OFF_SAMPLES  12u
#define MESH_SAMPLE_STRIDE       12u   /* bytes per MeshSample: 6 x int16       */

/* Compile-time proof that the offsets above match the struct. A negative array size is the
 * portable pre-C11 static assert, and it works in every Arduino toolchain this project targets.
 * The whole point of the constants is defeated if they can drift from the definition. */
typedef char mesh_offsets_are_correct[
    (sizeof(MeshSample) == MESH_SAMPLE_STRIDE &&
     ((char *)&((MeshSampleMsg *)0)->seq            - (char *)0) == MESH_SAMPLE_OFF_SEQ &&
     ((char *)&((MeshSampleMsg *)0)->int_to_send_us - (char *)0) == MESH_SAMPLE_OFF_LATENCY &&
     ((char *)&((MeshSampleMsg *)0)->count          - (char *)0) == MESH_SAMPLE_OFF_COUNT &&
     ((char *)&((MeshSampleMsg *)0)->dropped        - (char *)0) == MESH_SAMPLE_OFF_DROPPED &&
     ((char *)&((MeshSampleMsg *)0)->samples        - (char *)0) == MESH_SAMPLE_OFF_SAMPLES)
    ? 1 : -1];

/* The ESP-NOW broadcast address. Sending here reaches every node on the channel at once, with no
 * roster and no per-node send -- the difference between commanding a fleet and commanding a list.
 * It must still be registered as a peer before esp_now_send accepts it. */
#define MESH_BROADCAST_MAC        { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu }

#define MESH_CAP_IMU              0x01u

#define MESH_FW_MAJOR             1u
#define MESH_FW_MINOR             0u

#endif /* MESH_PROTOCOL_H */

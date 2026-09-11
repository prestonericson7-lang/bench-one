/* ===========================================================================================
 *  bench_protocol.h -- BENCH ONE stack extension to interop_protocol.h
 * ===========================================================================================
 *
 *  WHAT THIS FILE IS, AND WHAT IT DELIBERATELY IS NOT
 *  --------------------------------------------------
 *  This header ADDS channels and commands to the tested Brain-and-Radio protocol. It does not
 *  modify it. `interop_protocol.h` in this folder is a BYTE-IDENTICAL copy of the tested file
 *  (md5 f90495fd51b6ee308935efb7642636d2) and must stay that way.
 *
 *  That is a deliberate engineering decision, not laziness. The parser, the CRC table and the
 *  frame builder carry 1.4M passing assertions and a hardware baseline of 53,530 frames with
 *  zero errors of every class. The cheapest way to keep that guarantee is to not touch the file
 *  it lives in. Everything new lives here, is strictly additive, and cannot regress the parser
 *  because it never compiles into it.
 *
 *  Concretely: if you delete this header, the original two-board system still builds and runs
 *  exactly as it did. That property is the acceptance test for this file.
 *
 *
 *  CORRECTIONS TO THE PROJECT HANDOFF NOTES -- READ THIS
 *  -----------------------------------------------------
 *  The BENCH ONE handoff documents (docs/01-brain-and-radio.md) describe the protocol as:
 *
 *      "START(0xAA) | LEN_H | LEN_L | SEQ | CMD | PAYLOAD | CRC8, poly 0x31, check 0xA2"
 *
 *  That is the v1 protocol. It is WRONG for the code that is actually on the boards. The real
 *  frame, read out of interop_protocol.h, is:
 *
 *      START(0xAA) | LEN_H | LEN_L | SEQ | FLAGS | CHAN | CMD | PAYLOAD | CRC_H | CRC_L
 *
 *      - LEN is big-endian and counts FLAGS + CHAN + CMD + payload. Overhead is 6 bytes.
 *      - CRC is CRC-16/MCRF4XX: poly 0x1021 reflected as 0x8408, init 0xFFFF, refin/refout,
 *        xorout 0x0000, check("123456789") = 0x6F91. It covers LEN..payload, START excluded.
 *      - SEQ bit 7 is a CLASS bit: set = unsolicited event with its own rolling counter.
 *        It is NOT "SEQ 0x00 means unsolicited" as the handoff says -- that was v1.
 *
 *  Anyone building a decoder from the handoff summary would produce a decoder that fails on
 *  every single frame. Build from this file and from interop_protocol.h, never from the prose.
 *
 *  Second correction. The handoff proposes command range 0x30-0x3F for Teensy1<->Teensy2 worker
 *  jobs. That range is ALREADY TAKEN: IOP_CMD_MESH_INIT_REQ through IOP_CMD_MESH_NODES_RESP
 *  occupy 0x30-0x3F on channel 0x04. Using it again would have been a silent collision on any
 *  link that ever carries both.
 *
 *
 *  HOW THE ADDRESS SPACE IS ORGANISED, AND WHY
 *  -------------------------------------------
 *  Commands are namespaced PER CHANNEL, so 0x30 on channel 0x04 and 0x30 on a new channel would
 *  not actually collide on the wire. We still give every new channel a globally unique command
 *  range anyway. The reason is the one already established in this codebase for the ESP-NOW
 *  message ids: "purely so an unexpected direction is obvious in a hex dump."
 *
 *  If CHAN is ever corrupted -- and the parser's own documentation records a false-start
 *  survival rate of 1 in 17,614 over 4 MB of noise -- a globally unique CMD still identifies
 *  the frame unambiguously. Overlapping ranges would turn a corrupted channel byte into a
 *  plausible frame on the wrong subsystem. That is exactly the "articulate wrong answer" class
 *  of failure the bring-up log warns about, and it costs one byte of address space to avoid.
 *
 *      CHANNEL              CMD RANGE   LINK                            OWNER
 *      0x00 TRANSPORT       0x01-0x0F   all links                       (existing)
 *      0x01 WIFI            0x10-0x1F   Teensy1 <-> ESP32-S3            (existing, TESTED)
 *      0x02 BLE             0x20-0x2F   Teensy1 <-> ESP32-S3            (existing, reserved)
 *      0x03 DATA            --          reserved, not implemented       (existing)
 *      0x04 MESH            0x30-0x3F   Teensy1 <-> ESP32-S3            (existing, TESTED)
 *      0x05 FABRIC          0x50-0x6F   any link -> Teensy1             NEW
 *      0x06 WORKER          0x70-0x7F   Teensy1 <-> Teensy2             NEW
 *      0x07 ORCH            0x80-0x8F   Luckfox <-> Teensy1             NEW
 *      0x08 HMI             0x90-0x9F   Teensy1 <-> E32R40T display     NEW
 *      0x09 ROUTE           0xA0-0xAF   stack <-> stack                 NEW (stack 2)
 *
 *      0x40-0x4F is left FREE on purpose. The handoff earmarked it for the orchestrator; the
 *      orchestrator ended up at 0x80. Leaving the gap costs nothing and means any half-built
 *      thing written against the old plan fails loudly with UNKNOWN_CMD instead of quietly
 *      landing on a real command.
 *
 *      0xFE NACK stays global and unchanged on every channel.
 *
 *
 *  THE ONE RULE THAT MAKES "PLUG IN ANYTHING" WORK
 *  -----------------------------------------------
 *  Teensy1 is the SOLE master of the I2C and SPI fabric. No other node ever drives those wires.
 *  The Luckfox, the radio and the display node ask for IO over UART using channel 0x05, and
 *  Teensy1 performs it. This is what makes a multi-master I2C bus impossible by construction
 *  rather than by convention -- the other nodes are not wired to the bus at all.
 *
 *  The escape hatch that makes any module supportable without new firmware is the pair
 *  FABRIC_SPI_XFER and FABRIC_I2C_XFER: raw, addressed, arbitrary-length transactions against a
 *  named port. A module nobody has written a driver for is still fully controllable from Python
 *  on the Luckfox on day one. Purpose-built commands exist for the things worth doing fast.
 *
 * ===========================================================================================
 */

#ifndef BENCH_PROTOCOL_H
#define BENCH_PROTOCOL_H

#include "interop_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================================
 * SECTION 1 -- VERSIONING
 * ===========================================================================================
 *
 * Three version numbers exist because three things change independently, and conflating them
 * is how a compatibility matrix becomes a lie:
 *
 *   IOP_PROTOCOL_VERSION   the frame itself. Owned by interop_protocol.h. Do not touch.
 *   BENCH_PROTOCOL_VERSION this extension's command set. Bump on any wire-visible change.
 *   BENCH_FABRIC_HW_REV    the physical fabric build. Bump when a wire moves.
 *
 * Every node reports all three in its HELLO. The master compares them and says so in plain
 * English on a mismatch, because the existing project already learned that a half-upgraded pair
 * produces a stream of CRC failures that looks exactly like a wiring fault.
 */
#define BENCH_PROTOCOL_VERSION   1u
#define BENCH_PROTOCOL_MINOR     0u

/* Bump when the physical fabric changes in a way firmware can observe. Printed at boot and
 * carried in FABRIC_INFO_RESP so a capture can be matched to the hardware that produced it. */
#define BENCH_FABRIC_HW_REV      1u

/* ===========================================================================================
 * SECTION 2 -- NODE IDENTITY
 * ===========================================================================================
 *
 * Every link in this stack is point-to-point, so a frame needs no address to be delivered. The
 * node id is NOT an address. It exists so that a node can say who it is in its HELLO, so that a
 * log line has an unambiguous subject, and so that ROUTE frames on a two-stack build can name a
 * destination. Keeping it out of the frame header is what lets the tested parser stay untouched.
 */
#define BENCH_NODE_UNKNOWN       0x00u
#define BENCH_NODE_LUCKFOX       0x01u  /* RV1103, Linux orchestrator                          */
#define BENCH_NODE_TEENSY1       0x02u  /* i.MX RT1062 master: owns the fabric and the display  */
#define BENCH_NODE_TEENSY2       0x03u  /* i.MX RT1062 worker: compute jobs only                */
#define BENCH_NODE_ESP32S3       0x04u  /* radio: WiFi/BLE/ESP-NOW                              */
#define BENCH_NODE_HMI           0x05u  /* E32R40T, ESP32-WROOM-32E, 4.0in ST7796 + XPT2046     */

/* Stack id, for the two-stack build. Stack 1 is the one being built now. */
#define BENCH_STACK_LOCAL        0x00u  /* "this stack", whichever it is -- never routed        */
#define BENCH_STACK_1            0x01u
#define BENCH_STACK_2            0x02u

static inline const char *bench_node_name(uint8_t node)
{
    switch (node) {
        case BENCH_NODE_LUCKFOX: return "LUCKFOX";
        case BENCH_NODE_TEENSY1: return "TEENSY1";
        case BENCH_NODE_TEENSY2: return "TEENSY2";
        case BENCH_NODE_ESP32S3: return "ESP32S3";
        case BENCH_NODE_HMI:     return "HMI";
        default:                 return "UNKNOWN_NODE";
    }
}

/* ===========================================================================================
 * SECTION 3 -- NEW CHANNELS
 * =========================================================================================== */
#define BENCH_CHAN_FABRIC      0x05u
#define BENCH_CHAN_WORKER      0x06u
#define BENCH_CHAN_ORCH        0x07u
#define BENCH_CHAN_HMI         0x08u
#define BENCH_CHAN_ROUTE       0x09u

/* ===========================================================================================
 * SECTION 4 -- FABRIC COMMANDS (channel 0x05, range 0x50-0x6F)
 * ===========================================================================================
 *
 * Direction convention throughout: *_REQ travels toward Teensy1, *_RESP travels back, and
 * *_EVENT is unsolicited (SEQ class bit set) and travels away from Teensy1.
 *
 * Every RESP begins with a one-byte status (BENCH_ST_*). Every REQ that names hardware names it
 * by LOGICAL PORT ID, never by raw pin. The port table lives in bench_ports.h. That indirection
 * is the whole point: firmware, Python and the silk on the board all say "P3", and the wiring
 * behind P3 can change without any of them changing.
 */

/* -- inventory and discovery ------------------------------------------------------------- */
#define BENCH_CMD_FAB_INFO_REQ       0x50u  /* []                                              */
#define BENCH_CMD_FAB_INFO_RESP      0x51u  /* [st][hw_rev][proto_v][proto_m][n_ports]
                                             * [n_mcp][n_595][n_165][n_138][n_4051][flags]     */
#define BENCH_CMD_FAB_PORTS_REQ      0x52u  /* [] enumerate the port table                     */
#define BENCH_CMD_FAB_PORTS_RESP     0x53u  /* [st][count] then count x BenchPortDesc (packed) */
#define BENCH_CMD_FAB_SCAN_REQ       0x54u  /* [bus]  0=Wire 1=Wire1 2=Wire2                   */
#define BENCH_CMD_FAB_SCAN_RESP      0x55u  /* [st][bus][count] then count x [addr7]           */

/* -- logical GPIO, expander-agnostic ------------------------------------------------------ */
/* A "logical pin" is (port_id, index). The driver resolves it to an MCP23017 register bit, a
 * 74HC595 output bit, a 74HC165 input bit, or a native Teensy pin, and the caller never knows
 * or cares which. This is the layer that removes the guessing. */
#define BENCH_CMD_FAB_PIN_MODE_REQ   0x56u  /* [port][index][mode]  mode: BENCH_PINMODE_*      */
#define BENCH_CMD_FAB_PIN_MODE_RESP  0x57u  /* [st]                                            */
#define BENCH_CMD_FAB_PIN_WRITE_REQ  0x58u  /* [port][index][value]                            */
#define BENCH_CMD_FAB_PIN_WRITE_RESP 0x59u  /* [st]                                            */
#define BENCH_CMD_FAB_PIN_READ_REQ   0x5Au  /* [port][index]                                   */
#define BENCH_CMD_FAB_PIN_READ_RESP  0x5Bu  /* [st][value]                                     */

/* -- whole-port atomic access ------------------------------------------------------------- */
/* Reading 16 MCP23017 bits in one transaction is not an optimisation, it is a correctness
 * requirement: two separate byte reads of GPIOA and GPIOB observe two different instants, and
 * anything that samples a bus or a rotary encoder across that gap reads a state that never
 * existed. Same reason a 74HC595 bank is latched by RCLK rather than appearing bit by bit. */
#define BENCH_CMD_FAB_PORT_WRITE_REQ 0x5Cu  /* [port][len][data...]  atomic, latched           */
#define BENCH_CMD_FAB_PORT_WRITE_RESP 0x5Du /* [st]                                            */
#define BENCH_CMD_FAB_PORT_READ_REQ  0x5Eu  /* [port][len]           atomic snapshot           */
#define BENCH_CMD_FAB_PORT_READ_RESP 0x5Fu  /* [st][len][data...]                              */

/* -- interrupts ---------------------------------------------------------------------------- */
/* Unsolicited. Carries INTCAP (the value AT the interrupt) as well as the live GPIO value,
 * because they differ whenever the input bounced or changed again before the frame was built,
 * and only the first of those answers "what happened". */
#define BENCH_CMD_FAB_IRQ_EVENT      0x60u  /* [port][intf_a][intf_b][intcap_a][intcap_b]
                                             *       [gpio_a][gpio_b][us32]                    */
#define BENCH_CMD_FAB_IRQ_CFG_REQ    0x61u  /* [port][mask_a][mask_b][mode]                    */
#define BENCH_CMD_FAB_IRQ_CFG_RESP   0x62u  /* [st]                                            */

/* -- raw bus escape hatches: this is what makes "any module" true --------------------------- */
/* SPI: assert the port's chip select through the 74HC138 (break-before-make, enforced in the
 * driver, not requested by the caller), clock out tx, capture rx, deassert. One atomic unit --
 * a caller can never leave a chip selected, because there is no command that only selects. */
#define BENCH_CMD_FAB_SPI_XFER_REQ   0x63u  /* [port][mode][speed_khz16][flags][len16][tx...]  */
#define BENCH_CMD_FAB_SPI_XFER_RESP  0x64u  /* [st][len16][rx...]                              */
/* I2C: write-then-read against one address, the only shape that is safe to expose. A repeated
 * START is used when rd_len > 0, never a STOP between, so a register read cannot be interrupted
 * by another transaction on a bus that has exactly one master. */
#define BENCH_CMD_FAB_I2C_XFER_REQ   0x65u  /* [bus][addr7][wr_len][rd_len][wr...]             */
#define BENCH_CMD_FAB_I2C_XFER_RESP  0x66u  /* [st][rd_len][rd...]                             */

/* -- shift-register fabric ----------------------------------------------------------------- */
/* 74HC595 chain out and 74HC165 chain in. Byte order on the wire is CHAIN ORDER (chip 0 first,
 * where chip 0 is the one nearest the Teensy). The driver reverses it internally because the
 * first byte clocked out of a 595 chain ends up in the FARTHEST device. Exposing chain order
 * rather than wire order means the caller's mental model matches the silk on the board. */
#define BENCH_CMD_FAB_SHIFT_OUT_REQ  0x67u  /* [chain][len][data... chain order]               */
#define BENCH_CMD_FAB_SHIFT_OUT_RESP 0x68u  /* [st]                                            */
#define BENCH_CMD_FAB_SHIFT_IN_REQ   0x69u  /* [chain][len]                                    */
#define BENCH_CMD_FAB_SHIFT_IN_RESP  0x6Au  /* [st][len][data... chain order]                  */

/* -- analog: 74HC4051 tree into a Teensy ADC ----------------------------------------------- */
/* n_samples>1 averages in firmware. The first conversion after a mux switch is ALWAYS discarded
 * by the driver and is never included in the average; charge injection makes it wrong and no
 * caller should have to know that. */
#define BENCH_CMD_FAB_ADC_REQ        0x6Bu  /* [tree][channel][n_samples]                      */
#define BENCH_CMD_FAB_ADC_RESP       0x6Cu  /* [st][channel][raw16][avg16][n]                  */
#define BENCH_CMD_FAB_ADC_SWEEP_REQ  0x6Du  /* [tree][first][count][n_samples]                 */
#define BENCH_CMD_FAB_ADC_SWEEP_RESP 0x6Eu  /* [st][first][count] then count x [avg16]         */

/* -- fault ---------------------------------------------------------------------------------- */
/* Unsolicited, and it is the reason a fabric problem reaches the Linux box with enough context
 * to act on instead of being felt as "it went weird". */
#define BENCH_CMD_FAB_FAULT_EVENT    0x6Fu  /* [code][port][detail16][us32]                    */

/* ===========================================================================================
 * SECTION 5 -- WORKER COMMANDS (channel 0x06, range 0x70-0x7F)
 * ===========================================================================================
 * Teensy1 (master) <-> Teensy2 (worker). The worker never initiates work and never talks to any
 * other node; jobs arrive through the master. That is what keeps its firmware small enough to
 * reason about and its timing deterministic.
 */
#define BENCH_CMD_WRK_HELLO          0x70u  /* UNSOLICITED on boot:
                                             * [node][proto_v][proto_m][bench_v][reset_reason]
                                             * [caps16][crc_check16]                           */
#define BENCH_CMD_WRK_JOB_REQ        0x71u  /* [job_id][job_type][flags][len16][args...]       */
#define BENCH_CMD_WRK_JOB_RESP       0x72u  /* [st][job_id]  -- ACCEPTED, not COMPLETE         */
#define BENCH_CMD_WRK_JOB_EVENT      0x73u  /* UNSOLICITED: [job_id][state][pct][len16][data]  */
#define BENCH_CMD_WRK_RESULT_REQ     0x74u  /* [job_id][offset32][max_len16]                   */
#define BENCH_CMD_WRK_RESULT_RESP    0x75u  /* [st][job_id][offset32][len16][data...]          */
#define BENCH_CMD_WRK_CANCEL_REQ     0x76u  /* [job_id]                                        */
#define BENCH_CMD_WRK_CANCEL_RESP    0x77u  /* [st][job_id]                                    */
#define BENCH_CMD_WRK_STATUS_REQ     0x78u  /* []                                              */
#define BENCH_CMD_WRK_STATUS_RESP    0x79u  /* [st][running][queued][done][failed][cpu_pct]
                                             * [free_ram32][uptime_ms32][temp_c16]             */
#define BENCH_CMD_WRK_BENCH_REQ      0x7Au  /* [kind][iterations32]  -- stress/benchmark        */
#define BENCH_CMD_WRK_BENCH_RESP     0x7Bu  /* [st][kind][min_ns32][mean_ns32][max_ns32]
                                             * [iterations32][ops_per_s32]                     */

/* A job is accepted, then runs. JOB_RESP says "accepted" and nothing more. Completion arrives
 * as an unsolicited JOB_EVENT. Conflating the two -- returning a result from the request -- is
 * what turns one slow job into a stalled link, because this protocol allows exactly one
 * outstanding request per link. */
#define BENCH_JOB_STATE_QUEUED       0x00u
#define BENCH_JOB_STATE_RUNNING      0x01u
#define BENCH_JOB_STATE_DONE         0x02u
#define BENCH_JOB_STATE_FAILED       0x03u
#define BENCH_JOB_STATE_CANCELLED    0x04u

/* ===========================================================================================
 * SECTION 6 -- ORCHESTRATOR COMMANDS (channel 0x07, range 0x80-0x8F)
 * ===========================================================================================
 * Luckfox (Linux) <-> Teensy1. The Luckfox decides WHAT. Teensy1 decides WHEN and HOW.
 */
#define BENCH_CMD_ORC_HELLO          0x80u  /* UNSOLICITED from Teensy1 on boot, same shape as
                                             * WRK_HELLO -- the Linux side re-anchors on it     */
#define BENCH_CMD_ORC_SYS_REQ        0x81u  /* []  whole-stack health in one round trip        */
#define BENCH_CMD_ORC_SYS_RESP       0x82u  /* [st][links_up_mask][n_nodes] then per node:
                                             * [node][state][uptime_ms32][errors16]            */
#define BENCH_CMD_ORC_FWD_REQ        0x83u  /* [dst_node][chan][cmd][len16][payload...]
                                             * ask Teensy1 to relay a frame to a node it owns  */
#define BENCH_CMD_ORC_FWD_RESP       0x84u  /* [st][dst_node][chan][cmd][len16][payload...]    */
#define BENCH_CMD_ORC_EVENT          0x85u  /* UNSOLICITED upward: [src_node][chan][cmd]
                                             * [len16][payload...]  -- the event firehose      */
#define BENCH_CMD_ORC_SUB_REQ        0x86u  /* [event_mask32][divisor]  subscription, like mesh */
#define BENCH_CMD_ORC_SUB_RESP       0x87u  /* [st][event_mask32][divisor]                     */
#define BENCH_CMD_ORC_SYNC_REQ       0x88u  /* [t_host_us64]  clock-offset probe               */
#define BENCH_CMD_ORC_SYNC_RESP      0x89u  /* [st][t_host_us64 echoed][t_t1_us64][t1_cycles32] */
#define BENCH_CMD_ORC_LOG            0x8Au  /* UNSOLICITED: [level][len][utf8...]              */
#define BENCH_CMD_ORC_MARK_REQ       0x8Bu  /* [mark_id][pulses]  toggle the LA marker GPIO    */
#define BENCH_CMD_ORC_MARK_RESP      0x8Cu  /* [st][mark_id][t_t1_us64]                        */
#define BENCH_CMD_ORC_SELFTEST_REQ   0x8Du  /* [level]  0=quick 1=full 2=destructive           */
#define BENCH_CMD_ORC_SELFTEST_RESP  0x8Eu  /* [st][level][n] then n x [test_id][result][detail16] */

/* The event subscription defaults to OFF, for the reason Defect 1 in PLATFORM_BASELINE.md
 * established: "should this telemetry go upward, and how much of it" is a policy decision, and
 * policy does not live on the node producing the data. */
#define BENCH_EVMASK_NONE            0x00000000ul
#define BENCH_EVMASK_FABRIC_IRQ      0x00000001ul
#define BENCH_EVMASK_FABRIC_FAULT    0x00000002ul
#define BENCH_EVMASK_WORKER_JOB      0x00000004ul
#define BENCH_EVMASK_RADIO           0x00000008ul  /* WIFI_EVENT, MESH_*                       */
#define BENCH_EVMASK_HMI_INPUT       0x00000010ul
#define BENCH_EVMASK_LOG             0x00000020ul
#define BENCH_EVMASK_LINK            0x00000040ul  /* a link went up or down                   */
#define BENCH_EVMASK_ALL             0xFFFFFFFFul

#define BENCH_LOG_DEBUG              0x00u
#define BENCH_LOG_INFO               0x01u
#define BENCH_LOG_WARN               0x02u
#define BENCH_LOG_ERROR              0x03u

/* ===========================================================================================
 * SECTION 7 -- HMI COMMANDS (channel 0x08, range 0x90-0x9F)
 * ===========================================================================================
 * Teensy1 <-> the E32R40T display node (ESP32-WROOM-32E, ST7796 480x320, XPT2046 touch).
 *
 * The display node renders and reports touches. It holds no authority: a touch is an EVENT, not
 * a command. Teensy1 decides what a touch means. Same rule as the radio -- mechanism at the
 * leaf, policy at the hub -- and it is what lets the display be replaced or unplugged without
 * the stack caring.
 */
#define BENCH_CMD_HMI_HELLO          0x90u  /* UNSOLICITED on boot, same shape as WRK_HELLO    */
#define BENCH_CMD_HMI_STATE_REQ      0x91u  /* [screen][flags][len16][state blob...]           */
#define BENCH_CMD_HMI_STATE_RESP     0x92u  /* [st][screen][render_us32]                       */
#define BENCH_CMD_HMI_INPUT_EVENT    0x93u  /* UNSOLICITED: [kind][x16][y16][z16][widget_id]   */
#define BENCH_CMD_HMI_STATUS_REQ     0x94u  /* []                                              */
#define BENCH_CMD_HMI_STATUS_RESP    0x95u  /* [st][screen][fps][touch_ok][sd_ok][uptime_ms32] */
#define BENCH_CMD_HMI_BL_REQ         0x96u  /* [level]  0-255 backlight PWM                    */
#define BENCH_CMD_HMI_BL_RESP        0x97u  /* [st][level]                                     */

#define BENCH_HMI_INPUT_DOWN         0x01u
#define BENCH_HMI_INPUT_UP           0x02u
#define BENCH_HMI_INPUT_DRAG         0x03u

/* ===========================================================================================
 * SECTION 8 -- ROUTE COMMANDS (channel 0x09, range 0xA0-0xAF) -- the two-stack build
 * ===========================================================================================
 *
 * The existing protocol has no node addressing, and adding it to the header would have meant
 * touching the tested parser. Instead a routed frame is CARRIED as the payload of an ordinary
 * point-to-point frame: the envelope names the destination, the inner bytes are a complete,
 * independently-CRC'd frame.
 *
 * This is the cheapest correct option. It costs 4 bytes per hop, needs no parser change, and a
 * corrupted envelope cannot corrupt the inner frame because the inner frame carries its own CRC.
 * Routing happens at the Luckfox, which is the only node with the memory and the scheduling
 * freedom to do it without hurting anyone's real-time behaviour.
 */
#define BENCH_CMD_RTE_FRAME          0xA0u  /* [dst_stack][dst_node][src_stack][src_node]
                                             * [len16][complete inner frame...]                */
#define BENCH_CMD_RTE_ANNOUNCE       0xA1u  /* UNSOLICITED: [stack][node][proto_v][bench_v]
                                             * [caps16][uptime_ms32]                           */
#define BENCH_CMD_RTE_PING_REQ       0xA2u  /* [dst_stack][dst_node][count]                    */
#define BENCH_CMD_RTE_PING_RESP      0xA3u  /* [st][sent][recv][min_us32][mean_us32][max_us32] */

/* ===========================================================================================
 * SECTION 9 -- STATUS CODES
 * ===========================================================================================
 * One status enum for every new RESP, so a caller writes one decoder. Distinct from IOP_ERR_*,
 * which lives in the NACK path and answers "the frame was unusable"; these answer "the frame was
 * fine and the operation did not work", which is a different question with different remedies.
 */
#define BENCH_ST_OK                  0x00u
#define BENCH_ST_BUSY                0x01u  /* fabric bus in use / job queue full              */
#define BENCH_ST_BAD_PORT            0x02u  /* no such logical port                            */
#define BENCH_ST_BAD_INDEX           0x03u  /* pin index outside the port                      */
#define BENCH_ST_BAD_ARG             0x04u  /* argument out of range                           */
#define BENCH_ST_NOT_PRESENT         0x05u  /* device did not ACK / chip missing               */
#define BENCH_ST_BUS_ERROR           0x06u  /* NACK on I2C, timeout, arbitration lost          */
#define BENCH_ST_TIMEOUT             0x07u
#define BENCH_ST_WRONG_MODE          0x08u  /* e.g. writing to a port configured as input      */
#define BENCH_ST_NO_SUCH_JOB         0x09u
#define BENCH_ST_UNSUPPORTED         0x0Au  /* this build does not implement it                */
#define BENCH_ST_OVERFLOW            0x0Bu  /* payload would exceed IOP_MAX_PAYLOAD_LEN        */
#define BENCH_ST_HW_FAULT            0x0Cu  /* self-test failed / rail out of range            */
#define BENCH_ST_ERROR               0xFFu

static inline const char *bench_st_name(uint8_t st)
{
    switch (st) {
        case BENCH_ST_OK:          return "OK";
        case BENCH_ST_BUSY:        return "BUSY";
        case BENCH_ST_BAD_PORT:    return "BAD_PORT";
        case BENCH_ST_BAD_INDEX:   return "BAD_INDEX";
        case BENCH_ST_BAD_ARG:     return "BAD_ARG";
        case BENCH_ST_NOT_PRESENT: return "NOT_PRESENT";
        case BENCH_ST_BUS_ERROR:   return "BUS_ERROR";
        case BENCH_ST_TIMEOUT:     return "TIMEOUT";
        case BENCH_ST_WRONG_MODE:  return "WRONG_MODE";
        case BENCH_ST_NO_SUCH_JOB: return "NO_SUCH_JOB";
        case BENCH_ST_UNSUPPORTED: return "UNSUPPORTED";
        case BENCH_ST_OVERFLOW:    return "OVERFLOW";
        case BENCH_ST_HW_FAULT:    return "HW_FAULT";
        case BENCH_ST_ERROR:       return "ERROR";
        default:                   return "UNKNOWN_ST";
    }
}

/* ===========================================================================================
 * SECTION 10 -- FABRIC FAULT CODES
 * =========================================================================================== */
#define BENCH_FAULT_I2C_NACK         0x01u
#define BENCH_FAULT_I2C_WEDGED       0x02u  /* SDA stuck low; recovery clocks were issued      */
#define BENCH_FAULT_SPI_NO_RESPONSE  0x03u
#define BENCH_FAULT_IRQ_STORM        0x04u  /* interrupt asserted continuously -- see below    */
#define BENCH_FAULT_DEVICE_LOST      0x05u  /* was present at scan, no longer ACKs             */
#define BENCH_FAULT_DEVICE_APPEARED  0x06u  /* hot-plug: ACKed at an address that was empty    */
#define BENCH_FAULT_RAIL_LOW         0x07u  /* INA219 says a rail sagged                       */
#define BENCH_FAULT_RAIL_OVERCURRENT 0x08u
#define BENCH_FAULT_SELFTEST_FAILED  0x09u
#define BENCH_FAULT_CS_CONFLICT      0x0Au  /* two ports claim one 138 slot -- config error    */

/* IRQ_STORM is the specific, documented failure of an MCP23017 with MIRROR=1 whose ISR read
 * only one port: INTA/INTB stay asserted, the falling edge never comes again, and interrupts
 * wedge SILENTLY. The driver reads BOTH ports every time, and this fault exists so that if the
 * line is still low afterwards -- meaning something else is wrong -- it is reported rather than
 * being felt later as "the buttons stopped working". */

/* ===========================================================================================
 * SECTION 11 -- LOGICAL PIN MODES
 * =========================================================================================== */
#define BENCH_PINMODE_INPUT          0x00u
#define BENCH_PINMODE_INPUT_PULLUP   0x01u  /* MCP internal pull-up: 40-115 uA, ~100k. Weak.
                                             * Use an external 10k for anything off-board.     */
#define BENCH_PINMODE_OUTPUT         0x02u
#define BENCH_PINMODE_OUTPUT_SINK    0x03u  /* documents intent: LOW = active. MCP23017 sources
                                             * only 3 mA guaranteed but sinks 8 mA, so every
                                             * LED on this fabric is anode-to-3V3 and sunk.    */

/* ===========================================================================================
 * SECTION 12 -- SELF-TEST IDS (the L0-L7 bring-up ladder, in firmware)
 * ===========================================================================================
 * These mirror the manual bring-up checklist so the scripted regression and the bench procedure
 * cannot drift apart. A PASS here means the same thing it means on the bench.
 */
#define BENCH_TEST_I2C_IDLE_HIGH     0x01u  /* both lines high with no traffic -> pull-ups live */
#define BENCH_TEST_I2C_SCAN          0x02u  /* every expected address ACKs, no extras           */
#define BENCH_TEST_MCP_READBACK      0x03u  /* write OLAT, read GPIO, compare                   */
#define BENCH_TEST_SPI_LOOPBACK      0x04u  /* MOSI->MISO jumper proves clocking                */
#define BENCH_TEST_SHIFT_LOOPBACK    0x05u  /* 595 output -> 165 input jumper, walking 1        */
#define BENCH_TEST_CS_EXCLUSIVE      0x06u  /* exactly one 138 output low, per address          */
#define BENCH_TEST_CS_DESELECT       0x07u  /* enable off -> every output high                  */
#define BENCH_TEST_ADC_RAILS         0x08u  /* mux ch tied to GND and to 3V3 read 0 and full    */
#define BENCH_TEST_IRQ_ROUNDTRIP     0x09u  /* drive an MCP input, see the IRQ, clear it        */
#define BENCH_TEST_POWER_BUDGET      0x0Au  /* INA219 rails inside golden limits                */

#define BENCH_TESTRESULT_PASS        0x00u
#define BENCH_TESTRESULT_FAIL        0x01u
#define BENCH_TESTRESULT_SKIPPED     0x02u  /* hardware for it is not fitted                    */
#define BENCH_TESTRESULT_NOT_RUN     0x03u

/* ===========================================================================================
 * SECTION 13 -- TIMEOUTS
 * ===========================================================================================
 * Same rule as the existing table: the requester's timeout always exceeds the responder's own
 * deadline, with margin, so a race never decides who was right.
 */
#define BENCH_TIMEOUT_FABRIC_MS         500u
#define BENCH_TIMEOUT_FABRIC_SCAN_MS   3000u   /* 128 addresses x 3 buses, worst case          */
#define BENCH_TIMEOUT_FABRIC_SWEEP_MS  2000u   /* 64 channels x settle + samples               */
#define BENCH_TIMEOUT_WORKER_MS         500u   /* accept a job; the job itself is asynchronous  */
#define BENCH_TIMEOUT_WORKER_BENCH_MS 30000u
#define BENCH_TIMEOUT_ORCH_MS           500u
#define BENCH_TIMEOUT_ORCH_FWD_MS     16000u   /* must exceed the slowest thing it can wrap:
                                                * IOP_TIMEOUT_SCAN_MS is 15000               */
#define BENCH_TIMEOUT_SELFTEST_MS     20000u
#define BENCH_TIMEOUT_HMI_MS           1000u   /* a full 480x320 repaint is not instant        */

/* ===========================================================================================
 * SECTION 14 -- NAME HELPERS
 * ===========================================================================================
 * These WRAP the originals rather than replacing them. iop_chan_name() and iop_cmd_name() are
 * left exactly as they are in the tested header; these fall through to them for anything they
 * do not recognise. Console output stays identical for every pre-existing frame.
 */
static inline const char *bench_chan_name(uint8_t chan)
{
    switch (chan) {
        case BENCH_CHAN_FABRIC: return "FABRIC";
        case BENCH_CHAN_WORKER: return "WORKER";
        case BENCH_CHAN_ORCH:   return "ORCH";
        case BENCH_CHAN_HMI:    return "HMI";
        case BENCH_CHAN_ROUTE:  return "ROUTE";
        default:                return iop_chan_name(chan);
    }
}

static inline const char *bench_cmd_name(uint8_t cmd)
{
    switch (cmd) {
        case BENCH_CMD_FAB_INFO_REQ:        return "FAB_INFO_REQ";
        case BENCH_CMD_FAB_INFO_RESP:       return "FAB_INFO_RESP";
        case BENCH_CMD_FAB_PORTS_REQ:       return "FAB_PORTS_REQ";
        case BENCH_CMD_FAB_PORTS_RESP:      return "FAB_PORTS_RESP";
        case BENCH_CMD_FAB_SCAN_REQ:        return "FAB_SCAN_REQ";
        case BENCH_CMD_FAB_SCAN_RESP:       return "FAB_SCAN_RESP";
        case BENCH_CMD_FAB_PIN_MODE_REQ:    return "FAB_PIN_MODE_REQ";
        case BENCH_CMD_FAB_PIN_MODE_RESP:   return "FAB_PIN_MODE_RESP";
        case BENCH_CMD_FAB_PIN_WRITE_REQ:   return "FAB_PIN_WRITE_REQ";
        case BENCH_CMD_FAB_PIN_WRITE_RESP:  return "FAB_PIN_WRITE_RESP";
        case BENCH_CMD_FAB_PIN_READ_REQ:    return "FAB_PIN_READ_REQ";
        case BENCH_CMD_FAB_PIN_READ_RESP:   return "FAB_PIN_READ_RESP";
        case BENCH_CMD_FAB_PORT_WRITE_REQ:  return "FAB_PORT_WRITE_REQ";
        case BENCH_CMD_FAB_PORT_WRITE_RESP: return "FAB_PORT_WRITE_RESP";
        case BENCH_CMD_FAB_PORT_READ_REQ:   return "FAB_PORT_READ_REQ";
        case BENCH_CMD_FAB_PORT_READ_RESP:  return "FAB_PORT_READ_RESP";
        case BENCH_CMD_FAB_IRQ_EVENT:       return "FAB_IRQ_EVENT";
        case BENCH_CMD_FAB_IRQ_CFG_REQ:     return "FAB_IRQ_CFG_REQ";
        case BENCH_CMD_FAB_IRQ_CFG_RESP:    return "FAB_IRQ_CFG_RESP";
        case BENCH_CMD_FAB_SPI_XFER_REQ:    return "FAB_SPI_XFER_REQ";
        case BENCH_CMD_FAB_SPI_XFER_RESP:   return "FAB_SPI_XFER_RESP";
        case BENCH_CMD_FAB_I2C_XFER_REQ:    return "FAB_I2C_XFER_REQ";
        case BENCH_CMD_FAB_I2C_XFER_RESP:   return "FAB_I2C_XFER_RESP";
        case BENCH_CMD_FAB_SHIFT_OUT_REQ:   return "FAB_SHIFT_OUT_REQ";
        case BENCH_CMD_FAB_SHIFT_OUT_RESP:  return "FAB_SHIFT_OUT_RESP";
        case BENCH_CMD_FAB_SHIFT_IN_REQ:    return "FAB_SHIFT_IN_REQ";
        case BENCH_CMD_FAB_SHIFT_IN_RESP:   return "FAB_SHIFT_IN_RESP";
        case BENCH_CMD_FAB_ADC_REQ:         return "FAB_ADC_REQ";
        case BENCH_CMD_FAB_ADC_RESP:        return "FAB_ADC_RESP";
        case BENCH_CMD_FAB_ADC_SWEEP_REQ:   return "FAB_ADC_SWEEP_REQ";
        case BENCH_CMD_FAB_ADC_SWEEP_RESP:  return "FAB_ADC_SWEEP_RESP";
        case BENCH_CMD_FAB_FAULT_EVENT:     return "FAB_FAULT_EVENT";

        case BENCH_CMD_WRK_HELLO:           return "WRK_HELLO";
        case BENCH_CMD_WRK_JOB_REQ:         return "WRK_JOB_REQ";
        case BENCH_CMD_WRK_JOB_RESP:        return "WRK_JOB_RESP";
        case BENCH_CMD_WRK_JOB_EVENT:       return "WRK_JOB_EVENT";
        case BENCH_CMD_WRK_RESULT_REQ:      return "WRK_RESULT_REQ";
        case BENCH_CMD_WRK_RESULT_RESP:     return "WRK_RESULT_RESP";
        case BENCH_CMD_WRK_CANCEL_REQ:      return "WRK_CANCEL_REQ";
        case BENCH_CMD_WRK_CANCEL_RESP:     return "WRK_CANCEL_RESP";
        case BENCH_CMD_WRK_STATUS_REQ:      return "WRK_STATUS_REQ";
        case BENCH_CMD_WRK_STATUS_RESP:     return "WRK_STATUS_RESP";
        case BENCH_CMD_WRK_BENCH_REQ:       return "WRK_BENCH_REQ";
        case BENCH_CMD_WRK_BENCH_RESP:      return "WRK_BENCH_RESP";

        case BENCH_CMD_ORC_HELLO:           return "ORC_HELLO";
        case BENCH_CMD_ORC_SYS_REQ:         return "ORC_SYS_REQ";
        case BENCH_CMD_ORC_SYS_RESP:        return "ORC_SYS_RESP";
        case BENCH_CMD_ORC_FWD_REQ:         return "ORC_FWD_REQ";
        case BENCH_CMD_ORC_FWD_RESP:        return "ORC_FWD_RESP";
        case BENCH_CMD_ORC_EVENT:           return "ORC_EVENT";
        case BENCH_CMD_ORC_SUB_REQ:         return "ORC_SUB_REQ";
        case BENCH_CMD_ORC_SUB_RESP:        return "ORC_SUB_RESP";
        case BENCH_CMD_ORC_SYNC_REQ:        return "ORC_SYNC_REQ";
        case BENCH_CMD_ORC_SYNC_RESP:       return "ORC_SYNC_RESP";
        case BENCH_CMD_ORC_LOG:             return "ORC_LOG";
        case BENCH_CMD_ORC_MARK_REQ:        return "ORC_MARK_REQ";
        case BENCH_CMD_ORC_MARK_RESP:       return "ORC_MARK_RESP";
        case BENCH_CMD_ORC_SELFTEST_REQ:    return "ORC_SELFTEST_REQ";
        case BENCH_CMD_ORC_SELFTEST_RESP:   return "ORC_SELFTEST_RESP";

        case BENCH_CMD_HMI_HELLO:           return "HMI_HELLO";
        case BENCH_CMD_HMI_STATE_REQ:       return "HMI_STATE_REQ";
        case BENCH_CMD_HMI_STATE_RESP:      return "HMI_STATE_RESP";
        case BENCH_CMD_HMI_INPUT_EVENT:     return "HMI_INPUT_EVENT";
        case BENCH_CMD_HMI_STATUS_REQ:      return "HMI_STATUS_REQ";
        case BENCH_CMD_HMI_STATUS_RESP:     return "HMI_STATUS_RESP";
        case BENCH_CMD_HMI_BL_REQ:          return "HMI_BL_REQ";
        case BENCH_CMD_HMI_BL_RESP:         return "HMI_BL_RESP";

        case BENCH_CMD_RTE_FRAME:           return "RTE_FRAME";
        case BENCH_CMD_RTE_ANNOUNCE:        return "RTE_ANNOUNCE";
        case BENCH_CMD_RTE_PING_REQ:        return "RTE_PING_REQ";
        case BENCH_CMD_RTE_PING_RESP:       return "RTE_PING_RESP";

        default:                            return iop_cmd_name(cmd);
    }
}

static inline const char *bench_fault_name(uint8_t code)
{
    switch (code) {
        case BENCH_FAULT_I2C_NACK:         return "I2C_NACK";
        case BENCH_FAULT_I2C_WEDGED:       return "I2C_WEDGED";
        case BENCH_FAULT_SPI_NO_RESPONSE:  return "SPI_NO_RESPONSE";
        case BENCH_FAULT_IRQ_STORM:        return "IRQ_STORM";
        case BENCH_FAULT_DEVICE_LOST:      return "DEVICE_LOST";
        case BENCH_FAULT_DEVICE_APPEARED:  return "DEVICE_APPEARED";
        case BENCH_FAULT_RAIL_LOW:         return "RAIL_LOW";
        case BENCH_FAULT_RAIL_OVERCURRENT: return "RAIL_OVERCURRENT";
        case BENCH_FAULT_SELFTEST_FAILED:  return "SELFTEST_FAILED";
        case BENCH_FAULT_CS_CONFLICT:      return "CS_CONFLICT";
        default:                           return "UNKNOWN_FAULT";
    }
}

static inline const char *bench_test_name(uint8_t id)
{
    switch (id) {
        case BENCH_TEST_I2C_IDLE_HIGH:  return "I2C_IDLE_HIGH";
        case BENCH_TEST_I2C_SCAN:       return "I2C_SCAN";
        case BENCH_TEST_MCP_READBACK:   return "MCP_READBACK";
        case BENCH_TEST_SPI_LOOPBACK:   return "SPI_LOOPBACK";
        case BENCH_TEST_SHIFT_LOOPBACK: return "SHIFT_LOOPBACK";
        case BENCH_TEST_CS_EXCLUSIVE:   return "CS_EXCLUSIVE";
        case BENCH_TEST_CS_DESELECT:    return "CS_DESELECT";
        case BENCH_TEST_ADC_RAILS:      return "ADC_RAILS";
        case BENCH_TEST_IRQ_ROUNDTRIP:  return "IRQ_ROUNDTRIP";
        case BENCH_TEST_POWER_BUDGET:   return "POWER_BUDGET";
        default:                        return "UNKNOWN_TEST";
    }
}

/* ===========================================================================================
 * SECTION 15 -- COMPILE-TIME GUARDS
 * ===========================================================================================
 * These exist because the most expensive failures in this project so far were SILENT. A build
 * error is the cheapest possible place to find a mismatch.
 */

/* If the underlying protocol ever goes to v3, the frame changed and every offset in this file
 * is suspect. Fail the build rather than let it run. */
#if IOP_PROTOCOL_VERSION != 2u
#error "bench_protocol.h was written against interop protocol v2. Re-verify every payload layout."
#endif

/* The new channels must not collide with the existing ones. */
#if (BENCH_CHAN_FABRIC <= IOP_CHAN_MESH)
#error "BENCH_CHAN_FABRIC collides with an existing channel."
#endif

/* The largest fixed-size thing we ask the frame to carry must fit. ORC_FWD wraps another frame's
 * payload plus a 5-byte header; if IOP_MAX_PAYLOAD_LEN ever shrinks this catches it. */
#if (IOP_MAX_PAYLOAD_LEN < 64u)
#error "IOP_MAX_PAYLOAD_LEN is too small for the BENCH ONE command set."
#endif

#ifdef __cplusplus
}
#endif

#endif /* BENCH_PROTOCOL_H */

/* ===========================================================================================
 *  interop_protocol.h  --  ESP32-S3 <-> Teensy 4.1 binary serial interop protocol
 *  PROTOCOL VERSION 2
 * ===========================================================================================
 *
 *  WHAT THIS FILE IS
 *  -----------------
 *  The single source of truth for the wire format spoken between the two boards. It is
 *  byte-for-byte identical on both sides. Change the protocol here, and only here.
 *
 *
 *  WHY IT IS WRITTEN THE WAY IT IS
 *  -------------------------------
 *  This header contains ZERO Arduino dependencies. No Serial, no millis(), no String, no
 *  Arduino.h. Everything is plain C built on <stdint.h> and <string.h>.
 *
 *    1. PORTABILITY. The same parser runs on the ESP32-S3 (Xtensa LX7), the Teensy 4.1
 *       (Cortex-M7), a desktop unit test, or a Python reimplementation. Reaching for millis()
 *       or Serial would weld it to Arduino forever.
 *    2. TESTABILITY. The parser takes the current time as an ARGUMENT, so timeout behaviour is
 *       tested by passing fake timestamps. A function that calls millis() internally cannot be
 *       tested without a real clock and real waiting.
 *    3. TRANSPORT INDEPENDENCE. The builder writes into a caller-supplied buffer, not to a
 *       serial port. Today the transport is a UART; tomorrow it could be SPI, RS-485, or a
 *       recorded log replayed for debugging.
 *
 *  This header is the PROTOCOL. The two .ino sketches are ADAPTERS bolting it onto a board's
 *  UART. Keep that boundary clean and this code outlives the boards it was written for.
 *
 *
 *  THE ARCHITECTURE THIS PROTOCOL SERVES
 *  -------------------------------------
 *  The Teensy 4.1 is the BRAIN. The ESP32-S3 is a RADIO -- a peripheral giving the Teensy WiFi
 *  and BLE, nothing more. That decides where every behaviour belongs:
 *
 *      MECHANISM lives on the radio.   POLICY lives on the brain.
 *
 *  The radio drives an 802.11 MAC and a BLE controller and reports what happened. It does not
 *  decide what happens NEXT -- not retries, not which network or device to prefer, not whether a
 *  failure matters. Three consequences visible throughout:
 *    1. The radio reports RAW facts beside its summary (raw 802.11 reason codes, raw BLE
 *       advertising payloads), because the summary is made by the component with the least
 *       context and the brain must be able to overrule it.
 *    2. The radio speaks unasked. Links drop and chips brown out on the radio's schedule.
 *    3. The radio is replaceable. Nothing here mentions Espressif.
 *
 *
 *  THE FRAME FORMAT
 *  ----------------
 *
 *   +-------+-------+-------+-------+-------+-------+-------+==========+-------+-------+
 *   | START | LEN_H | LEN_L |  SEQ  | FLAGS | CHAN  |  CMD  | PAYLOAD  | CRC_H | CRC_L |
 *   +-------+-------+-------+-------+-------+-------+-------+==========+-------+-------+
 *      0xAA    1 B     1 B     1 B     1 B     1 B     1 B   0..1015 B    1 B     1 B
 *           \_________________________________________________________/
 *                     CRC-16 is computed over exactly this span
 *                                   \_____________________________/
 *                                    LEN counts these (FLAGS+CHAN+CMD+PAYLOAD), so LEN >= 3
 *
 *
 *  WHAT CHANGED FROM VERSION 1, AND WHY
 *  ------------------------------------
 *  v1 worked, shipped, and was verified on hardware. It was broken deliberately, exactly once,
 *  because three of its limits could not be lifted without changing the wire -- and a wire break
 *  deferred is one that costs triple later. All four changes landed together.
 *
 *  1. THE CHANNEL BYTE. v1 had a flat command space with ranges reserved by convention. That
 *     does not survive a second radio technology: BLE alone wants scan, advertise, connect, GATT
 *     read/write/notify, server and client. Espressif's own ESP-Hosted answers this with a 4-bit
 *     interface selector in which Bluetooth consumes ZERO command codes, because HCI frames are
 *     just a header followed by raw HCI bytes. CHAN routes; CMD means something within it.
 *     Adding a third radio later costs one channel value, not a renumbering.
 *
 *  2. CRC-16/MCRF4XX REPLACES CRC-8, and this was the change that mattered most. Poly 0x31
 *     factors as (x+1)(degree-7 cofactor of order 127), so HD=4 held only to a 119-bit dataword
 *     -- about 10 payload bytes. Every longer frame was HD=2, and that included EVERY REAL
 *     WIFI_CONNECT_REQ, since a 5-character SSID with an 8-character passphrase is already 15
 *     payload bytes. A two-bit error in a credential frame could pass, the radio would try a
 *     wrong password, and the brain would be told WRONG_PASSWORD for a password that was
 *     correct. No better 8-bit polynomial exists -- 119 bits is the maximum HD=4 length for ANY
 *     CRC-8 -- so the limit was the width. CRC-16/MCRF4XX holds HD>=4 to 32,751 bits for one
 *     extra byte, and is the MAVLink polynomial, so analyser decoders already know it.
 *
 *  3. THE FLAGS BYTE. Fragmentation and truncation had nowhere to live. v1's WiFi scan silently
 *     truncated a long list and said so only in the radio's own debug log, so a brain ranking
 *     access points by signal was ranking an arbitrary subset with no way to know.
 *
 *  4. SEQ BIT 7 MARKS UNSOLICITED. v1 reserved the single value 0x00, which worked but gave
 *     every announcement the same sequence number -- so a lost event was undetectable. Splitting
 *     the byte gives requests 128 transaction ids and gives unsolicited frames their own rolling
 *     counter, so the brain can SEE that it missed one. It also removes the 0xFF->0x00 wrap
 *     hazard structurally instead of relying on a helper to remember to skip a value.
 *
 *  Cost: header 5 -> 8 bytes, max payload 1018 -> 1015. On a full frame that is +32 us at
 *  921600 baud, or 0.29%. Espressif measure their own UART transport at ~92% of theoretical at
 *  this same baud rate, so the link is wire-limited and the overhead argument was never
 *  really available.
 *
 *
 *  FIELD BY FIELD
 *  --------------
 *
 *  START (0xAA)
 *      A serial link has no packet boundaries. When the receiver loses its place it needs a
 *      landmark. 0xAA is binary 10101010: maximally alternating, rare in structured data,
 *      unmistakable on a logic analyser.
 *
 *      Worth knowing: Espressif's own ESP-Hosted UART transport has NO sync marker, no byte
 *      stuffing and no resync -- both ends read a fixed-size header and continue on error from
 *      the same misaligned offset, so one stray byte desynchronises it permanently. That is
 *      their issues #51 and #142, and their fix was to flush the RX FIFO after reset rather than
 *      add a marker. Never remove this field.
 *
 *      0xAA is NOT escaped, so it occurs inside payloads constantly -- -86 dBm as a signed RSSI
 *      byte IS 0xAA. See FALSE STARTS below for why that is safe.
 *
 *  LEN_H, LEN_L (big-endian uint16)
 *      Bytes after SEQ up to but excluding the CRC. Never less than 3.
 *
 *  SEQ
 *      Bit 7 is the CLASS flag; bits 0-6 are a rolling counter within that class.
 *          0x00-0x7F   a request, or the response echoing it
 *          0x80-0xFF   unsolicited: radio news answering no request
 *      Echoing the request's SEQ is what lets a late reply be DISCARDED rather than mistaken for
 *      the answer to the next command -- without it, one timeout shifts every later response by
 *      one, permanently, and the symptom looks like random nonsense.
 *      The unsolicited counter makes event loss VISIBLE: the radio's queue can overflow before a
 *      frame is built, the brain's ring can overrun, and the UART can drop a byte in hardware
 *      without setting any flag the Teensy core exposes. A gap in the counter says so.
 *
 *  FLAGS   Frame-level modifiers, orthogonal to the command. See IOP_FLAG_*.
 *  CHAN    Which subsystem. A receiver MUST route on CHAN.
 *  CMD     What this frame means, within its channel.
 *  PAYLOAD Command-specific, possibly empty.
 *
 *  CRC-16
 *      CRC-16/MCRF4XX over LEN_H..end of PAYLOAD, high byte first. A frame that fails is DROPPED
 *      and NACKed, never partially acted upon. START is excluded because it is a marker, not
 *      data: if it were corrupted there would be no frame to verify.
 *
 *
 *  FALSE STARTS
 *  ------------
 *  If the parser resyncs on a payload byte that happens to be 0xAA, two filters reject it:
 *    Filter 1 -- the LEN check, free and immediate. Only 3..1018 is accepted, so roughly 98.5%
 *    of false starts die two bytes in, before a buffer is touched.
 *    Filter 2 -- the CRC. Survivors must produce a body whose trailing two bytes equal its own
 *    CRC-16: a further 1 in 65536.
 *  Combined, roughly 1 in 4.2 million -- against 1 in 16,000 for v1's CRC-8, which was measured
 *  empirically at 1 in 17,614 over 4 MB of noise. A survivor is still not obeyed: its CHAN and
 *  CMD must also be implemented, or it is NACKed.
 *
 *
 *  WHAT THIS PROTOCOL STILL DOES NOT DO
 *  ------------------------------------
 *    - No byte stuffing / COBS. Framing is LEN + CRC; the arithmetic above is why.
 *    - No ACK of success, no automatic retry. A NACK says something broke; policy lives on the
 *      brain, on purpose. Silent retries hide faults during bring-up.
 *    - No flow control by default, though both boards have the hardware.
 *    - One outstanding request at a time. SEQ supports pipelining; the endpoints do not yet.
 *
 * ===========================================================================================
 */

#ifndef INTEROP_PROTOCOL_H
#define INTEROP_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================================
 * SECTION 1 -- VERSION AND WIRE CONSTANTS
 * =========================================================================================== */

/* MAJOR is a hard compatibility boundary: v1 and v2 cannot interoperate at all, because the
 * header length and CRC width differ. Both sketches print this and compare it on the first
 * frame of a session, so a half-upgraded pair is diagnosed immediately. */
#define IOP_PROTOCOL_VERSION   2u
#define IOP_PROTOCOL_MINOR     0u

#define IOP_START_BYTE         0xAAu

/* START + LEN_H + LEN_L + SEQ + CRC_H + CRC_L. FLAGS/CHAN/CMD are counted by LEN. */
#define IOP_FRAME_OVERHEAD     6u

#define IOP_MAX_FRAME_SIZE     1024u
#define IOP_MAX_BODY_LEN       (IOP_MAX_FRAME_SIZE - IOP_FRAME_OVERHEAD)   /* 1018 */
#define IOP_BODY_HEADER_LEN    3u                                          /* FLAGS+CHAN+CMD */
#define IOP_MAX_PAYLOAD_LEN    (IOP_MAX_BODY_LEN - IOP_BODY_HEADER_LEN)    /* 1015 */
#define IOP_MIN_BODY_LEN       IOP_BODY_HEADER_LEN

/* Largest run of bytes the parser will discard for a frame it has already rejected.
 *
 * A declared length within this bound is a real frame merely too big for us -- most likely a
 * peer built with a larger IOP_MAX_FRAME_SIZE -- and is drained precisely so the stream resumes
 * cleanly. Anything larger is judged a false start, because our own builder refuses to emit more
 * than IOP_MAX_PAYLOAD_LEN, and the parser resynchronises at once.
 *
 * Learned on hardware: twelve bytes of injected noise containing a stray 0xAA produced a
 * declared length of 43,538, and the parser sat waiting for bytes that were never coming while
 * eating the perfectly good frame that followed. */
#define IOP_MAX_SKIP_BYTES     (IOP_MAX_FRAME_SIZE * 2u)

/* Inter-byte timeout. At 921600 8N1 a byte is ~10.9 us and a full frame ~11 ms, so 50 ms is
 * ~4600 byte-times of slack against a healthy sender being preempted mid-frame, while still
 * unwedging a stuck parser faster than a human notices. */
#define IOP_FRAME_TIMEOUT_MS   50u

/* ===========================================================================================
 * SECTION 2 -- SEQUENCE NUMBERS
 * =========================================================================================== */

#define IOP_SEQ_CLASS_MASK     0x80u
#define IOP_SEQ_COUNTER_MASK   0x7Fu
#define IOP_SEQ_IS_UNSOLICITED(seq)  (((seq) & IOP_SEQ_CLASS_MASK) != 0u)

/* Two counters, because they answer different questions: one matches a reply to its request,
 * the other lets the receiver detect that an announcement went missing. Keeping the class bit
 * outside the arithmetic means neither can wrap into the other -- which in v1, where a single
 * reserved value carried the meaning, needed a helper that remembered to skip it. */
static inline uint8_t iop_next_request_seq(uint8_t seq)
{
    return (uint8_t)((seq + 1u) & IOP_SEQ_COUNTER_MASK);
}

static inline uint8_t iop_next_event_seq(uint8_t seq)
{
    return (uint8_t)(IOP_SEQ_CLASS_MASK | ((seq + 1u) & IOP_SEQ_COUNTER_MASK));
}

/* ===========================================================================================
 * SECTION 3 -- FLAGS
 * =========================================================================================== */
#define IOP_FLAG_NONE          0x00u
#define IOP_FLAG_MORE          0x01u  /* more fragments follow                                */
#define IOP_FLAG_TRUNCATED     0x02u  /* the sender had more data than would fit              */
/* bits 2-7 reserved. A receiver MUST ignore flags it does not recognise rather than reject the
 * frame, so a newer sender stays usable by an older receiver. */

/* ===========================================================================================
 * SECTION 4 -- CHANNELS
 * =========================================================================================== */
#define IOP_CHAN_TRANSPORT     0x00u
#define IOP_CHAN_WIFI          0x01u
#define IOP_CHAN_BLE           0x02u
#define IOP_CHAN_DATA          0x03u  /* reserved: bulk network data, not implemented */
#define IOP_CHAN_MESH          0x04u  /* ESP-NOW fleet: the brain directing other radios    */

/* ===========================================================================================
 * SECTION 5 -- COMMANDS, BY CHANNEL
 * =========================================================================================== */

/* ---- TRANSPORT ---- */
#define IOP_CMD_PING              0x01u
#define IOP_CMD_PONG              0x02u
#define IOP_CMD_RADIO_READY       0x04u  /* UNSOLICITED */
#define IOP_CMD_NACK              0xFEu

/* ---- WIFI ---- */
#define IOP_CMD_WIFI_SCAN_REQ     0x10u
#define IOP_CMD_WIFI_SCAN_RESP    0x11u
#define IOP_CMD_WIFI_CONNECT_REQ  0x12u
#define IOP_CMD_WIFI_CONNECT_RESP 0x13u
#define IOP_CMD_WIFI_EVENT        0x14u  /* UNSOLICITED */
#define IOP_CMD_WIFI_STATUS_REQ   0x15u
#define IOP_CMD_WIFI_STATUS_RESP  0x16u

/* ---- BLE ---- */
#define IOP_CMD_BLE_SCAN_REQ      0x20u
#define IOP_CMD_BLE_SCAN_RESP     0x21u
#define IOP_CMD_BLE_ADV_REQ       0x22u
#define IOP_CMD_BLE_ADV_RESP      0x23u
#define IOP_CMD_BLE_CONNECT_REQ   0x24u
#define IOP_CMD_BLE_CONNECT_RESP  0x25u
#define IOP_CMD_BLE_EVENT         0x26u  /* UNSOLICITED */
#define IOP_CMD_BLE_STATUS_REQ    0x27u
#define IOP_CMD_BLE_STATUS_RESP   0x28u

/* ---- MESH (ESP-NOW) ----
 * The brain does not speak ESP-NOW itself -- it has no radio. It directs the hub, which relays.
 * Same mechanism/policy split as everywhere else: the hub moves packets, the brain decides which
 * nodes matter, what they should be doing, and what a missing node means. */
#define IOP_CMD_MESH_INIT_REQ     0x30u  /* [channel][mode]                                  */
#define IOP_CMD_MESH_INIT_RESP    0x31u  /* [status][channel][hub_mac 6]                     */
#define IOP_CMD_MESH_PEER_REQ     0x32u  /* [op][mac 6]  op: 0=add 1=remove                  */
#define IOP_CMD_MESH_PEER_RESP    0x33u  /* [status][peer_count]                             */
#define IOP_CMD_MESH_SEND_REQ     0x34u  /* [mac 6][node_cmd][data...]                       */
#define IOP_CMD_MESH_SEND_RESP    0x35u  /* [status]                                         */
#define IOP_CMD_MESH_DATA         0x36u  /* UNSOLICITED: a node's payload, relayed verbatim   */
#define IOP_CMD_MESH_PING_REQ     0x37u  /* [mac 6][count]  -- hub-timed round trips          */
#define IOP_CMD_MESH_PING_RESP    0x38u  /* [status][sent][recv][min_us*][mean_us*][max_us*]  */
#define IOP_CMD_MESH_STATUS_REQ   0x39u
#define IOP_CMD_MESH_STATUS_RESP  0x3Au  /* [state][channel][peers][rx_count*][drops*]        */
#define IOP_CMD_MESH_NODE_EVENT   0x3Bu  /* UNSOLICITED: [event][mac 6]                       */
#define IOP_CMD_MESH_STREAM_REQ   0x3Cu  /* [divisor]  0=off, 1=every packet, N=every Nth     */
#define IOP_CMD_MESH_STREAM_RESP  0x3Du  /* [status][divisor]                                 */
#define IOP_CMD_MESH_NODES_REQ    0x3Eu  /* enumerate the fleet                               */
#define IOP_CMD_MESH_NODES_RESP   0x3Fu  /* [count] then count x [mac 6][alive][packets 4]    */

#define IOP_CMD_NONE              0x00u  /* not a wire value */

/* ===========================================================================================
 * SECTION 6 -- PAYLOAD LAYOUTS
 * ===========================================================================================
 *
 * NACK: [ORIGINAL_CHAN][ORIGINAL_CMD][ERROR_CODE]
 *     The channel is included because a bare command byte is ambiguous once channels exist. On a
 *     CRC failure both are best-effort -- the bytes could not be trusted, which is why the frame
 *     was rejected. A diagnostic hint for a human, never a value to branch on.
 *
 * WIFI_SCAN_RESP: [COUNT] then COUNT x [RSSI int8][CHANNEL][ENC][SSID_LEN][SSID...]
 *                 then [TOTAL_FOUND]. IOP_FLAG_TRUNCATED set when TOTAL_FOUND > COUNT.
 *     RSSI IS SIGNED: -67 dBm travels as 0xBD, and reading it into a uint8_t prints 189.
 *     SSID is length-prefixed and NOT null-terminated: an 802.11 SSID is a counted octet string
 *     that may contain any byte, including 0x00 and 0xAA. SSID_LEN 0 is a hidden network.
 *
 * WIFI_CONNECT_REQ:  [SSID_LEN][SSID][PASS_LEN][PASS]
 * WIFI_CONNECT_RESP: [STATUS][IP_A..IP_D][RAW_REASON]
 * WIFI_EVENT:        [EVENT_TYPE][RAW_REASON][IP_A..IP_D]
 * WIFI_STATUS_RESP:  [STATE][RAW_REASON][LAST_EVENT][RSSI int8][IP_A..IP_D]
 *                    [EVENTS_DROPPED][RESERVED][CAPABILITIES][FLAGS]
 *
 * RADIO_READY: [MAJOR][MINOR][CRC_CHECK_H][CRC_CHECK_L][RESET_REASON][CAPABILITIES]
 *     Unsolicited, once, as soon as the radio boots. The radio can restart without the brain
 *     restarting -- brownout, watchdog, nudged connector -- and without this the brain believes
 *     it is still associated while the radio sits blank. RESET_REASON usually names the culprit;
 *     BROWNOUT means the 3.3 V rail sagged when the transmitter keyed up, which is a power
 *     problem no amount of protocol debugging will fix. The version and CRC check value catch a
 *     stale header copy in the first frame rather than as inexplicable CRC failures later.
 *
 * BLE_SCAN_REQ:  [DURATION_S][ACTIVE]   active=1 sends scan requests for scan-response data
 * BLE_SCAN_RESP: [COUNT] then COUNT x [ADDR_TYPE][ADDR 6][RSSI int8][ADV_TYPE][DATA_LEN][DATA]
 *                then [TOTAL_FOUND]
 *
 *     DATA is the RAW advertising payload, unparsed. WHY: an advertisement is a sequence of
 *     length-type-value structures whose meaning depends on the profile. Parsing it on the radio
 *     would mean choosing which fields matter, on the component with the least context, and
 *     discarding the rest -- the same mistake as collapsing an 802.11 reason code to a friendly
 *     string. The brain gets the bytes. Legacy advertising is at most 31 bytes, so this is cheap.
 *
 * BLE_ADV_REQ:     [ENABLE][NAME_LEN][NAME...]
 * BLE_ADV_RESP:    [STATUS]
 * BLE_CONNECT_REQ: [ADDR_TYPE][ADDR 6]
 * BLE_CONNECT_RESP:[STATUS][CONN_ID]
 * BLE_EVENT:       [EVENT_TYPE][REASON][CONN_ID][ADDR 6]
 * BLE_STATUS_RESP: [STATE][CONNECTED][CONN_ID][EVENTS_DROPPED][ADV_ACTIVE][RESERVED]
 */

#define IOP_ENC_OPEN           0x00u
#define IOP_ENC_WEP            0x01u
#define IOP_ENC_WPA            0x02u
#define IOP_ENC_WPA2           0x03u
#define IOP_ENC_WPA_WPA2       0x04u
#define IOP_ENC_WPA3           0x05u
#define IOP_ENC_WPA2_WPA3      0x06u
#define IOP_ENC_ENTERPRISE     0x07u
#define IOP_ENC_UNKNOWN        0xFFu

#define IOP_WIFI_STATUS_CONNECTED      0x00u
#define IOP_WIFI_STATUS_WRONG_PASSWORD 0x01u
#define IOP_WIFI_STATUS_NO_NETWORK     0x02u
#define IOP_WIFI_STATUS_TIMEOUT        0x03u
#define IOP_WIFI_STATUS_UNKNOWN_ERROR  0xFFu

#define IOP_WIFI_STATE_IDLE       0x00u
#define IOP_WIFI_STATE_SCANNING   0x01u
#define IOP_WIFI_STATE_CONNECTING 0x02u
#define IOP_WIFI_STATE_ASSOCIATED 0x03u
#define IOP_WIFI_STATE_GOT_IP     0x04u

#define IOP_WIFI_EVT_CONNECTED    0x01u
#define IOP_WIFI_EVT_GOT_IP       0x02u
#define IOP_WIFI_EVT_DISCONNECTED 0x03u
#define IOP_WIFI_EVT_SCAN_DONE    0x04u

#define IOP_BLE_STATE_IDLE        0x00u
#define IOP_BLE_STATE_SCANNING    0x01u
#define IOP_BLE_STATE_ADVERTISING 0x02u
#define IOP_BLE_STATE_CONNECTING  0x03u
#define IOP_BLE_STATE_CONNECTED   0x04u

#define IOP_BLE_EVT_SCAN_DONE     0x01u
#define IOP_BLE_EVT_CONNECTED     0x02u
#define IOP_BLE_EVT_DISCONNECTED  0x03u
#define IOP_BLE_EVT_ADV_STARTED   0x04u
#define IOP_BLE_EVT_ADV_STOPPED   0x05u

#define IOP_BLE_STATUS_OK         0x00u
#define IOP_BLE_STATUS_BUSY       0x01u
#define IOP_BLE_STATUS_NOT_FOUND  0x02u
#define IOP_BLE_STATUS_TIMEOUT    0x03u
#define IOP_BLE_STATUS_ERROR      0xFFu

/* A BLE address is always 6 bytes; the type distinguishes public from the three random kinds,
 * and getting it wrong makes a device unconnectable in a way that looks like it vanished. */
#define IOP_BLE_ADDR_LEN          6u
#define IOP_BLE_ADV_DATA_MAX      31u
#define IOP_BLE_SCAN_REC_FIXED    10u   /* addr_type + addr[6] + rssi + adv_type + data_len */
#define IOP_BLE_SCAN_REC_MAX      (IOP_BLE_SCAN_REC_FIXED + IOP_BLE_ADV_DATA_MAX)
#define IOP_BLE_MAX_DEVICES       ((IOP_MAX_PAYLOAD_LEN - 2u) / IOP_BLE_SCAN_REC_MAX)

#define IOP_ERR_UNKNOWN_CMD    0x01u
#define IOP_ERR_BAD_CRC        0x02u
#define IOP_ERR_TIMEOUT        0x03u
#define IOP_ERR_BUSY           0x04u
#define IOP_ERR_PAYLOAD_LARGE  0x05u
#define IOP_ERR_BAD_PAYLOAD    0x06u
#define IOP_ERR_UNKNOWN_CHAN   0x07u
#define IOP_ERR_NOT_SUPPORTED  0x08u

#define IOP_SSID_MAX_LEN       32u
#define IOP_PASS_MAX_LEN       64u
#define IOP_SCAN_REC_FIXED     4u
#define IOP_SCAN_REC_MAX       (IOP_SCAN_REC_FIXED + IOP_SSID_MAX_LEN)
#define IOP_SCAN_MAX_NETWORKS  ((IOP_MAX_PAYLOAD_LEN - 2u) / IOP_SCAN_REC_MAX)

/* Mesh operating modes, and the reason both exist.
 *
 * MESH_MODE_ONLY pins the hub to a fixed channel and never joins an access point. Deterministic
 * and lowest latency -- nothing else contends for the radio, and no AP can move the channel
 * underneath the fleet.
 *
 * MESH_MODE_WITH_WIFI keeps station mode running, which means the CHANNEL IS DICTATED BY THE AP
 * and every node has to follow it. That is the constraint people trip over: ESP-NOW peers must
 * share a channel, and joining an AP takes that choice away from you.
 *
 * Both are implemented so the cost of coexistence can be measured rather than guessed at. */
#define IOP_MESH_MODE_ONLY        0x00u
#define IOP_MESH_MODE_WITH_WIFI   0x01u

#define IOP_MESH_STATE_OFF        0x00u
#define IOP_MESH_STATE_READY      0x01u
#define IOP_MESH_STATE_ERROR      0xFFu

#define IOP_MESH_EVT_NODE_SEEN    0x01u  /* first packet from a MAC we had not heard from */
#define IOP_MESH_EVT_NODE_LOST    0x02u  /* nothing heard for the liveness window          */

#define IOP_MESH_PEER_ADD         0x00u
#define IOP_MESH_PEER_REMOVE      0x01u

/* ---- Relay subscription, and the design error it fixes ----
 *
 * THE BUG THIS EXISTS TO CORRECT. The first version of the hub relayed every ESP-NOW packet to
 * the brain the instant it arrived, unconditionally and with no way to stop it. The brain never
 * asked for the stream and could not decline it. Measured cost, with one node at 200 Hz:
 * worst-case UART round trip went from 299 us to 1001 us, and 97.8% of the brain's console
 * output became a single "unknown channel" line repeated 198 times a second. Full write-up in
 * docs/PLATFORM_BASELINE.md.
 *
 * WHY IT WAS WRONG, not just expensive. "Should this telemetry go to the brain, and how much of
 * it" is a statement about what the application needs. That is policy, and policy belongs on the
 * brain. The relay code already refused to rescale or summarise node payloads -- correctly, and
 * with a comment saying so -- but the decision to send them AT ALL is the same kind of decision,
 * and that half was never questioned. It is easy to get the visible half of a principle right
 * and miss the invisible half.
 *
 * THE DIVISOR. One byte covers every case worth having:
 *
 *     0    off. The hub still tracks nodes, counts packets and detects loss; it just does not
 *          forward the data. THE DEFAULT, because a radio should be quiet until spoken to.
 *     1    every packet. Full fidelity, for when the brain genuinely wants the firehose.
 *     N    every Nth packet. The interesting case: a 200 Hz sensor at divisor 20 gives the brain
 *          a 10 Hz view for a display or a control loop while the hub keeps counting all 200 --
 *          so packet-loss statistics stay exact even though the brain sees a twentieth of them.
 *
 * Decimating at the hub rather than the brain is deliberate. Sending 200 frames a second so the
 * brain can throw 190 of them away spends the wire, both parsers and the brain's loop time to
 * produce the same answer.
 *
 * NODE EVENTS ARE NOT GATED BY THIS. MESH_NODE_EVENT (a node appearing or going silent) always
 * reaches the brain. The distinction is the one this protocol draws everywhere: a rare FACT the
 * brain must not miss is an event, and events are pushed; a continuous STREAM is subscribed to.
 * A node dying while the brain had the stream turned off is exactly when it most needs telling. */
#define IOP_MESH_STREAM_OFF       0x00u
#define IOP_MESH_STREAM_ALL       0x01u
#define IOP_MESH_STREAM_DEFAULT   IOP_MESH_STREAM_OFF

/* Status byte for MESH responses.
 *
 * These exist because the hub was reusing IOP_BLE_STATUS_OK in its mesh ping reply. The value was
 * right (0x00) and it worked -- which is exactly what makes it worth fixing now rather than after
 * the two ranges diverge. A constant borrowed from another subsystem is a rename away from a
 * silent wire-level bug, and nothing warns you.
 *
 * AND THEN IT BIT US, in the same session, in the way half-finished migrations always do. The
 * constants were introduced and the ping reply was converted -- but MESH_PEER_RESP and
 * MESH_SEND_RESP were left sending IOP_MESH_STATE_READY (0x01) while the newly written brain
 * checked for IOP_MESH_STATUS_OK (0x00). Every successful peer-add and every successful send
 * reported "FAILED" to the operator. Nothing crashed, no NACK was raised, no counter moved: the
 * two boards simply disagreed about what 0x01 meant.
 *
 * TWO LESSONS WORTH THE SPACE. First, introducing a constant is not the change -- converting
 * EVERY site is the change, and a partial conversion is strictly worse than none because it
 * looks done. Second, a status byte is a shared vocabulary between two independently compiled
 * programs; the compiler checks neither side against the other, so the only defences are
 * converting all at once and testing the SUCCESS path. We tested failure paths religiously in
 * this project and still shipped a bug that only appears when things go right. */
#define IOP_MESH_STATUS_OK        0x00u
#define IOP_MESH_STATUS_BUSY      0x01u
#define IOP_MESH_STATUS_NO_NODE   0x02u
#define IOP_MESH_STATUS_ERROR     0xFFu

/* ESP-NOW carries at most 250 bytes per packet -- a hard radio limit, not a design choice. Our
 * frames hold 1015, so a relayed node payload always fits with room to spare. */
#define IOP_MESH_MAX_PAYLOAD      250u
#define IOP_MESH_MAC_LEN          6u

#define IOP_CAP_MESH           0x04u
#define IOP_CAP_WIFI           0x01u
#define IOP_CAP_BLE            0x02u

/* THE RULE: the master's timeout always exceeds the radio's own deadline, with margin. If both
 * gave up at the same instant a race would decide who was right, producing exactly the stale
 * response SEQ exists to catch but which is far better avoided. */
#define IOP_TIMEOUT_PING_MS             500u
#define IOP_TIMEOUT_STATUS_MS           500u
#define IOP_TIMEOUT_SCAN_MS           15000u
#define IOP_TIMEOUT_CONNECT_MS        12000u
#define IOP_TIMEOUT_BLE_SCAN_MS       15000u
#define IOP_TIMEOUT_BLE_CONNECT_MS    12000u
#define IOP_TIMEOUT_MESH_MS            3000u   /* init/peer/send are all local  */
#define IOP_TIMEOUT_MESH_PING_MS      10000u   /* a ping train takes real time  */
#define IOP_ESP_SCAN_DEADLINE_MS      12000u
#define IOP_ESP_CONNECT_DEADLINE_MS   10000u
#define IOP_ESP_BLE_SCAN_DEADLINE_MS  12000u

/* ===========================================================================================
 * SECTION 7 -- CRC-16/MCRF4XX
 * ===========================================================================================
 *
 *     width=16  poly=0x1021  init=0xFFFF  refin=true  refout=true  xorout=0x0000
 *     check("123456789") = 0x6F91
 *
 * The MAVLink polynomial, chosen deliberately so existing analyser decoders and third-party
 * implementations already know it. v1's CRC-8 was specified by a label ("CRC-8/MAXIM") that did
 * not match its own stated parameters, and that ambiguity cost real time.
 *
 * WHY 16 BITS: see "WHAT CHANGED FROM VERSION 1" at the top. Short version -- CRC-8 poly 0x31
 * gave HD=2 above a 119-bit dataword, which covered every credential frame, and no better 8-bit
 * polynomial exists because the width was the limit.
 *
 * REFLECTED IMPLEMENTATION. MCRF4XX is refin/refout, so the table shifts RIGHT with the
 * reflected polynomial 0x8408 and the update step is
 *     crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF]
 * which is NOT the shape of v1's MSB-first CRC-8. Mixing the two produces a table that still
 * looks like a valid permutation and still passes casual inspection while being wrong.
 *
 * The table was GENERATED and verified against an independent bitwise implementation over every
 * 1-byte input, all 65,536 2-byte inputs and 5,000 pseudo-random strings: 70,792 cases, 0
 * mismatches, catalogue check value reproduced exactly. Never typed by hand, and re-proven on
 * real hardware at every boot by iop_selftest().
 */

#define IOP_CRC16_POLY_REFLECTED  0x8408u
#define IOP_CRC16_INIT            0xFFFFu
#define IOP_CRC16_CHECK_VALUE     0x6F91u

static const uint16_t IOP_CRC16_TABLE[256] = {
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78,
};

static inline uint16_t iop_crc16_update(uint16_t crc, uint8_t b)
{
    return (uint16_t)((crc >> 8) ^ IOP_CRC16_TABLE[(crc ^ b) & 0xFFu]);
}

static inline uint16_t iop_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = IOP_CRC16_INIT;
    uint16_t i;
    for (i = 0; i < len; i++) {
        crc = iop_crc16_update(crc, data[i]);
    }
    return crc;
}

/* The slow, obvious implementation, kept as an INDEPENDENT ORACLE for the table.
 *
 * A corrupt lookup table is the nastiest bug this file can have: it fails silently, identically,
 * on exactly one of the two boards, and presents as "the link randomly does not work". A second
 * implementation sharing no data with the first lets the boot self-test prove the table intact.
 * It costs a few dozen bytes of flash and never runs in the hot path. */
static inline uint16_t iop_crc16_bitwise(const uint8_t *data, uint16_t len)
{
    uint16_t crc = IOP_CRC16_INIT;
    uint16_t i;
    uint8_t bit;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc & 1u) ? ((crc >> 1) ^ IOP_CRC16_POLY_REFLECTED) : (crc >> 1));
        }
    }
    return crc;
}

/* ===========================================================================================
 * SECTION 8 -- HUMAN-READABLE NAMES
 * ===========================================================================================
 * The project rule is "when something fails, say WHY, not just 'error'". A line reading
 *     NACK chan=0x01 cmd=0x12 err=0x04
 * needs this header open in another window. A line reading
 *     NACK WIFI/WIFI_CONNECT_REQ: busy (still processing a previous command)
 * is understood at 3am by someone who has never read this file. All of these return string
 * literals, so they are safe to print directly and never allocate.
 */

static inline const char *iop_chan_name(uint8_t chan)
{
    switch (chan) {
        case IOP_CHAN_TRANSPORT: return "TRANSPORT";
        case IOP_CHAN_WIFI:      return "WIFI";
        case IOP_CHAN_BLE:       return "BLE";
        case IOP_CHAN_DATA:      return "DATA";
        case IOP_CHAN_MESH:      return "MESH";
        default:                 return "UNKNOWN_CHAN";
    }
}

static inline const char *iop_cmd_name(uint8_t cmd)
{
    switch (cmd) {
        case IOP_CMD_PING:              return "PING";
        case IOP_CMD_PONG:              return "PONG";
        case IOP_CMD_RADIO_READY:       return "RADIO_READY";
        case IOP_CMD_NACK:              return "NACK";
        case IOP_CMD_WIFI_SCAN_REQ:     return "WIFI_SCAN_REQ";
        case IOP_CMD_WIFI_SCAN_RESP:    return "WIFI_SCAN_RESP";
        case IOP_CMD_WIFI_CONNECT_REQ:  return "WIFI_CONNECT_REQ";
        case IOP_CMD_WIFI_CONNECT_RESP: return "WIFI_CONNECT_RESP";
        case IOP_CMD_WIFI_EVENT:        return "WIFI_EVENT";
        case IOP_CMD_WIFI_STATUS_REQ:   return "WIFI_STATUS_REQ";
        case IOP_CMD_WIFI_STATUS_RESP:  return "WIFI_STATUS_RESP";
        case IOP_CMD_BLE_SCAN_REQ:      return "BLE_SCAN_REQ";
        case IOP_CMD_BLE_SCAN_RESP:     return "BLE_SCAN_RESP";
        case IOP_CMD_BLE_ADV_REQ:       return "BLE_ADV_REQ";
        case IOP_CMD_BLE_ADV_RESP:      return "BLE_ADV_RESP";
        case IOP_CMD_BLE_CONNECT_REQ:   return "BLE_CONNECT_REQ";
        case IOP_CMD_BLE_CONNECT_RESP:  return "BLE_CONNECT_RESP";
        case IOP_CMD_BLE_EVENT:         return "BLE_EVENT";
        case IOP_CMD_BLE_STATUS_REQ:    return "BLE_STATUS_REQ";
        case IOP_CMD_BLE_STATUS_RESP:   return "BLE_STATUS_RESP";
        case IOP_CMD_MESH_INIT_REQ:     return "MESH_INIT_REQ";
        case IOP_CMD_MESH_INIT_RESP:    return "MESH_INIT_RESP";
        case IOP_CMD_MESH_PEER_REQ:     return "MESH_PEER_REQ";
        case IOP_CMD_MESH_PEER_RESP:    return "MESH_PEER_RESP";
        case IOP_CMD_MESH_SEND_REQ:     return "MESH_SEND_REQ";
        case IOP_CMD_MESH_SEND_RESP:    return "MESH_SEND_RESP";
        case IOP_CMD_MESH_DATA:         return "MESH_DATA";
        case IOP_CMD_MESH_PING_REQ:     return "MESH_PING_REQ";
        case IOP_CMD_MESH_PING_RESP:    return "MESH_PING_RESP";
        case IOP_CMD_MESH_STATUS_REQ:   return "MESH_STATUS_REQ";
        case IOP_CMD_MESH_STATUS_RESP:  return "MESH_STATUS_RESP";
        case IOP_CMD_MESH_NODE_EVENT:   return "MESH_NODE_EVENT";
        case IOP_CMD_MESH_STREAM_REQ:   return "MESH_STREAM_REQ";
        case IOP_CMD_MESH_STREAM_RESP:  return "MESH_STREAM_RESP";
        case IOP_CMD_MESH_NODES_REQ:    return "MESH_NODES_REQ";
        case IOP_CMD_MESH_NODES_RESP:   return "MESH_NODES_RESP";
        default:                        return "UNKNOWN_CMD";
    }
}

static inline const char *iop_err_name(uint8_t err)
{
    switch (err) {
        case IOP_ERR_UNKNOWN_CMD:   return "unknown command";
        case IOP_ERR_BAD_CRC:       return "bad CRC (frame corrupted in transit)";
        case IOP_ERR_TIMEOUT:       return "timeout (command took too long)";
        case IOP_ERR_BUSY:          return "busy (still processing a previous command)";
        case IOP_ERR_PAYLOAD_LARGE: return "payload too large for the receive buffer";
        case IOP_ERR_BAD_PAYLOAD:   return "malformed payload for this command";
        case IOP_ERR_UNKNOWN_CHAN:  return "channel not implemented on this radio";
        case IOP_ERR_NOT_SUPPORTED: return "known command, unavailable in this build";
        default:                    return "unrecognised error code";
    }
}

static inline const char *iop_enc_name(uint8_t enc)
{
    switch (enc) {
        case IOP_ENC_OPEN:       return "OPEN";
        case IOP_ENC_WEP:        return "WEP";
        case IOP_ENC_WPA:        return "WPA";
        case IOP_ENC_WPA2:       return "WPA2";
        case IOP_ENC_WPA_WPA2:   return "WPA/WPA2";
        case IOP_ENC_WPA3:       return "WPA3";
        case IOP_ENC_WPA2_WPA3:  return "WPA2/WPA3";
        case IOP_ENC_ENTERPRISE: return "ENTERPRISE";
        default:                 return "UNKNOWN";
    }
}

static inline const char *iop_wifi_state_name(uint8_t state)
{
    switch (state) {
        case IOP_WIFI_STATE_IDLE:       return "idle";
        case IOP_WIFI_STATE_SCANNING:   return "scanning";
        case IOP_WIFI_STATE_CONNECTING: return "connecting";
        case IOP_WIFI_STATE_ASSOCIATED: return "associated (no IP yet)";
        case IOP_WIFI_STATE_GOT_IP:     return "connected with an address";
        default:                        return "unknown state";
    }
}

static inline const char *iop_wifi_status_name(uint8_t status)
{
    switch (status) {
        case IOP_WIFI_STATUS_CONNECTED:      return "connected";
        case IOP_WIFI_STATUS_WRONG_PASSWORD: return "wrong password (authentication rejected)";
        case IOP_WIFI_STATUS_NO_NETWORK:     return "no such network found";
        case IOP_WIFI_STATUS_TIMEOUT:        return "timed out while joining";
        case IOP_WIFI_STATUS_UNKNOWN_ERROR:  return "unknown error";
        default:                             return "unrecognised status code";
    }
}

static inline const char *iop_wifi_event_name(uint8_t evt)
{
    switch (evt) {
        case IOP_WIFI_EVT_CONNECTED:    return "CONNECTED";
        case IOP_WIFI_EVT_GOT_IP:       return "GOT_IP";
        case IOP_WIFI_EVT_DISCONNECTED: return "DISCONNECTED";
        case IOP_WIFI_EVT_SCAN_DONE:    return "SCAN_DONE";
        default:                        return "UNKNOWN_EVENT";
    }
}

/* Raw 802.11 reason codes. This table lives on the BRAIN's side of the conceptual line -- the
 * radio forwards a number and says nothing about it, which is exactly the mechanism/policy split
 * the whole protocol is built on. Values verified against the ESP-IDF headers in the core. */
static inline const char *iop_wifi_reason_name(uint8_t reason)
{
    switch (reason) {
        case 0:   return "none";
        case 1:   return "unspecified";
        case 2:   return "previous authentication expired";
        case 3:   return "deauthenticated: station leaving";
        case 4:   return "disassociated: inactivity";
        case 8:   return "disassociated: station leaving";
        case 15:  return "4-way handshake timeout (usually a wrong passphrase)";
        case 23:  return "802.1X authentication failed";
        case 200: return "beacon timeout (AP went out of range)";
        case 201: return "no AP with that SSID found";
        case 202: return "authentication failed (AP rejected the credentials)";
        case 203: return "association failed";
        case 204: return "handshake timeout";
        case 205: return "connection failed";
        case 206: return "AP TSF reset";
        case 207: return "roaming";
        default:  return "see the 802.11 reason code table";
    }
}

static inline const char *iop_ble_state_name(uint8_t state)
{
    switch (state) {
        case IOP_BLE_STATE_IDLE:        return "idle";
        case IOP_BLE_STATE_SCANNING:    return "scanning";
        case IOP_BLE_STATE_ADVERTISING: return "advertising";
        case IOP_BLE_STATE_CONNECTING:  return "connecting";
        case IOP_BLE_STATE_CONNECTED:   return "connected";
        default:                        return "unknown state";
    }
}

static inline const char *iop_ble_event_name(uint8_t evt)
{
    switch (evt) {
        case IOP_BLE_EVT_SCAN_DONE:    return "SCAN_DONE";
        case IOP_BLE_EVT_CONNECTED:    return "CONNECTED";
        case IOP_BLE_EVT_DISCONNECTED: return "DISCONNECTED";
        case IOP_BLE_EVT_ADV_STARTED:  return "ADV_STARTED";
        case IOP_BLE_EVT_ADV_STOPPED:  return "ADV_STOPPED";
        default:                       return "UNKNOWN_EVENT";
    }
}

/* BLE address types. Getting this wrong makes a device unconnectable in a way that looks exactly
 * like it went out of range, which is why it is carried on the wire rather than assumed. */
static inline const char *iop_ble_addr_type_name(uint8_t t)
{
    switch (t) {
        case 0:  return "public";
        case 1:  return "random";
        case 2:  return "public identity (RPA resolved)";
        case 3:  return "random identity (RPA resolved)";
        default: return "unknown";
    }
}

/* ===========================================================================================
 * SECTION 9 -- THE FRAME PARSER
 * ===========================================================================================
 *
 *   WAIT_START --0xAA--> LEN_H --> LEN_L --+--(len ok)--> SEQ --> BODY --> CRC_H --> CRC_L --+
 *       ^                                  |                                                 |
 *       |                                  +--(len bad)--> SEQ --> SKIP --------------+      |
 *       |                                                                             |      |
 *       +--- any byte that is not 0xAA, or an inter-byte timeout ---------------------+------+
 *
 * WHY A STATE MACHINE AND NOT A BLOCKING READ: a blocking readBytes() halts the whole sketch
 * until the frame completes or times out. On the radio that means the LED stops and no other
 * work happens; on the brain it means user input is ignored. A byte-at-a-time machine consumes
 * exactly what is buffered right now and returns immediately, and it is the only structure that
 * survives arbitrary garbage, because every state has a defined answer for every byte.
 *
 * WHY THE SKIP STATE EXISTS -- worth reading twice. When a declared length is impossible, the
 * naive reaction is to NACK and jump straight back to hunting. That is a mistake: the rest of
 * that frame is STILL ARRIVING, and scanning it for 0xAA finds several by chance, each spawning
 * another bogus parse. One bad length byte becomes a storm, and the storm looks like the bug.
 * SKIP consumes exactly the declared remainder so a malformed frame costs one NACK.
 *
 * But SKIP assumed the promised bytes were actually coming, which is false for a false start --
 * and on real hardware that swallowed a live PING. So a length beyond IOP_MAX_SKIP_BYTES is
 * judged garbage and resynchronises immediately instead.
 *
 * ORDERING: oversize is detectable at LEN_L, but SEQ has not arrived yet and a NACK without the
 * right SEQ is far less useful. So the parser flags the frame, reads SEQ normally to capture it,
 * and only then decides. Correctness first, then diagnostics -- but both.
 */

typedef enum {
    IOP_ST_WAIT_START = 0,
    IOP_ST_LEN_H,
    IOP_ST_LEN_L,
    IOP_ST_SEQ,
    IOP_ST_BODY,     /* FLAGS, CHAN, CMD, then PAYLOAD */
    IOP_ST_CRC_H,
    IOP_ST_CRC_L,
    IOP_ST_SKIP
} IopParseState;

typedef enum {
    IOP_EV_NONE = 0,
    IOP_EV_FRAME,
    IOP_EV_BAD_CRC,
    IOP_EV_BAD_LENGTH,
    IOP_EV_OVERSIZE,
    IOP_EV_TIMEOUT,
    IOP_EV_RESYNC_DISCARD
} IopEvent;

/* A parsed frame handed back to the application.
 *
 * LIFETIME WARNING: `payload` points DIRECTLY INTO the parser's buffer. It is valid only until
 * the next byte of the NEXT frame is pushed. The normal loop -- push a byte, handle the frame
 * immediately, push the next -- is safe. To defer handling, memcpy the payload out first. No
 * copy is made here because copying a kilobyte per frame to guard against a mistake nobody makes
 * in the normal pattern is a poor trade. */
typedef struct {
    uint8_t        seq;
    uint8_t        flags;
    uint8_t        chan;
    uint8_t        cmd;
    uint16_t       payload_len;
    const uint8_t *payload;
    uint16_t       declared_len;
    uint16_t       crc_received;
    uint16_t       crc_computed;
} IopFrame;

typedef struct {
    IopParseState state;
    uint16_t      expected_len;
    uint16_t      body_index;
    uint32_t      skip_remaining;
    uint8_t       seq;
    uint16_t      crc_running;
    uint16_t      crc_received;
    uint8_t       frame_bad;
    uint8_t       bad_reason;
    uint32_t      last_byte_ms;

    uint8_t       body[IOP_MAX_BODY_LEN];

    /* Lifetime counters. The useful question is almost never "did that one frame work" but "is
     * this link healthy" -- and that is a question about RATES. A link with 3 bad CRCs per
     * thousand frames has a wiring or grounding problem even though every command appears to
     * succeed after a retry. Without counters that is invisible. */
    uint32_t      stat_frames_ok;
    uint32_t      stat_bad_crc;
    uint32_t      stat_bad_length;
    uint32_t      stat_oversize;
    uint32_t      stat_timeouts;
    uint32_t      stat_resync_bytes;
} IopParser;

static inline void iop_parser_reset(IopParser *p)
{
    p->state          = IOP_ST_WAIT_START;
    p->expected_len   = 0;
    p->body_index     = 0;
    p->skip_remaining = 0;
    p->crc_running    = IOP_CRC16_INIT;
    p->crc_received   = 0;
    p->frame_bad      = 0;
    p->bad_reason     = (uint8_t)IOP_EV_NONE;
}

static inline void iop_parser_init(IopParser *p)
{
    memset(p, 0, sizeof(*p));
    iop_parser_reset(p);
}

/* iop_parser_tick -- age out a stalled partial frame.
 *
 * >>> CALL THIS ONLY WHEN THE RECEIVE QUEUE IS EMPTY. <<<
 *
 * That restriction is the entire subtlety, and getting it wrong destroys good frames. The
 * correct shape is:
 *
 *     while (port.available() > 0) { ev = iop_parser_push(...); ... }
 *     if (port.available() == 0) { if (iop_parser_tick(...) == IOP_EV_TIMEOUT) { ... } }
 *
 * The timeout exists to detect a sender that STOPPED mid-frame. It must not fire merely because
 * the RECEIVER was slow. Those look identical if you only measure elapsed time, and are told
 * apart by exactly one question: is there more data waiting?
 *
 * Concretely: suppose loop() is busy 60 ms printing a table while the peer transmits a complete
 * frame into the UART ring. Every byte arrived on time; only our processing was late. Evaluating
 * the timeout before draining discards a perfectly intact frame and reports a fault that never
 * happened -- and the harder you debug it with print statements, the more reliably it reproduces,
 * because printing is what made loop() slow.
 *
 * This is also why push() does NOT check the timeout: it cannot see the queue, so it cannot tell
 * a dead sender from a late reader.
 *
 * ROLLOVER: millis() wraps after ~49.7 days. The subtraction is unsigned 32-bit, which wraps
 * identically, so elapsed time stays correct across the boundary. Hence (uint32_t)(now - last)
 * and never now > last + limit -- the latter breaks exactly once, seven weeks in, at 3am. */
static inline IopEvent iop_parser_tick(IopParser *p, uint32_t now_ms, IopFrame *out)
{
    if (p->state == IOP_ST_WAIT_START) {
        return IOP_EV_NONE;
    }
    if ((uint32_t)(now_ms - p->last_byte_ms) > IOP_FRAME_TIMEOUT_MS) {

        /* A frame already condemned, whose promised bytes then stopped arriving. Report WHY it
         * was condemned rather than a bare timeout -- the peer needs "payload too large" in
         * order to send the right NACK, and "something timed out" tells it nothing. */
        if (p->state == IOP_ST_SKIP && out != 0) {
            IopEvent reason   = (IopEvent)p->bad_reason;
            uint8_t  seq      = p->seq;
            uint16_t declared = p->expected_len;

            p->stat_timeouts++;
            iop_parser_reset(p);
            if (reason == IOP_EV_OVERSIZE) { p->stat_oversize++; }
            else                           { p->stat_bad_length++; }
            memset(out, 0, sizeof(*out));
            out->seq          = seq;
            out->cmd          = IOP_CMD_NONE;
            out->declared_len = declared;
            return reason;
        }

        p->stat_timeouts++;
        iop_parser_reset(p);
        return IOP_EV_TIMEOUT;
    }
    return IOP_EV_NONE;
}

/* iop_parser_push -- feed exactly one received byte.
 *
 * Returns an event rather than invoking a callback, deliberately: callbacks invert control and
 * make reentrancy possible -- a handler that sends a response could end up called from inside
 * the parser while its buffer is live. Returning a value keeps the application in charge. */
static inline IopEvent iop_parser_push(IopParser *p, uint8_t b, uint32_t now_ms, IopFrame *out)
{
    p->last_byte_ms = now_ms;

    switch (p->state) {

    case IOP_ST_WAIT_START:
        if (b == IOP_START_BYTE) {
            /* A CANDIDATE frame. This may well be a payload byte that merely looks like a
             * marker; LEN and CRC decide, not this byte. */
            p->crc_running = IOP_CRC16_INIT;
            p->state       = IOP_ST_LEN_H;
            return IOP_EV_NONE;
        }
        /* Debris: the tail of a frame we gave up on, boot chatter from a rebooting board, or
         * line noise. A steadily climbing count with no successful frames is the signature of a
         * baud-rate mismatch -- which is why it is counted rather than merely dropped. */
        p->stat_resync_bytes++;
        return IOP_EV_RESYNC_DISCARD;

    case IOP_ST_LEN_H:
        p->expected_len = (uint16_t)((uint16_t)b << 8);
        p->crc_running  = iop_crc16_update(p->crc_running, b);
        p->state        = IOP_ST_LEN_L;
        return IOP_EV_NONE;

    case IOP_ST_LEN_L:
        p->expected_len = (uint16_t)(p->expected_len | (uint16_t)b);
        p->crc_running  = iop_crc16_update(p->crc_running, b);

        /* THE CHEAP FILTER. Rejects ~98.5% of false syncs two bytes in, before a single byte is
         * written to the body buffer. The difference between "lost one frame" and "lost the
         * rest of the stream". */
        p->frame_bad = 0;
        if (p->expected_len < IOP_MIN_BODY_LEN) {
            p->frame_bad  = 1;
            p->bad_reason = (uint8_t)IOP_EV_BAD_LENGTH;
        } else if (p->expected_len > IOP_MAX_BODY_LEN) {
            p->frame_bad  = 1;
            p->bad_reason = (uint8_t)IOP_EV_OVERSIZE;
        }
        p->state = IOP_ST_SEQ;
        return IOP_EV_NONE;

    case IOP_ST_SEQ:
        p->seq         = b;
        p->crc_running = iop_crc16_update(p->crc_running, b);
        if (p->frame_bad) {
            uint32_t remaining = (uint32_t)p->expected_len + 2u;   /* body + 2 CRC bytes */

            if (remaining <= IOP_MAX_SKIP_BYTES) {
                /* Believable frame, just too big for us -- drain it precisely so the stream
                 * resumes cleanly instead of resyncing on a byte inside its payload. */
                p->skip_remaining = remaining;
                p->state          = IOP_ST_SKIP;
                return IOP_EV_NONE;
            }
            /* Not believable. Do NOT wait for bytes that are never coming: report now, while we
             * still have the SEQ, and start hunting immediately. */
            {
                IopEvent reason   = (IopEvent)p->bad_reason;
                uint8_t  seq      = p->seq;
                uint16_t declared = p->expected_len;

                iop_parser_reset(p);
                if (reason == IOP_EV_OVERSIZE) { p->stat_oversize++; }
                else                           { p->stat_bad_length++; }
                memset(out, 0, sizeof(*out));
                out->seq          = seq;
                out->cmd          = IOP_CMD_NONE;
                out->declared_len = declared;
                return reason;
            }
        }
        p->body_index = 0;
        p->state      = IOP_ST_BODY;
        return IOP_EV_NONE;

    case IOP_ST_BODY:
        /* The bound was validated at LEN_L, so body_index cannot run past the buffer. Written as
         * >= rather than == anyway: a defensive habit costing nothing that turns a hypothetical
         * overrun into a harmless early stop. */
        p->body[p->body_index++] = b;
        p->crc_running = iop_crc16_update(p->crc_running, b);
        if (p->body_index >= p->expected_len) {
            p->state = IOP_ST_CRC_H;
        }
        return IOP_EV_NONE;

    case IOP_ST_CRC_H:
        p->crc_received = (uint16_t)((uint16_t)b << 8);
        p->state = IOP_ST_CRC_L;
        return IOP_EV_NONE;

    case IOP_ST_CRC_L: {
        uint16_t received = (uint16_t)(p->crc_received | (uint16_t)b);
        uint16_t computed = p->crc_running;
        uint8_t  seq      = p->seq;
        uint16_t body_len = p->expected_len;

        iop_parser_reset(p);   /* ready for the next frame; body[] is left intact so the payload
                                * pointer handed out below stays valid */

        memset(out, 0, sizeof(*out));
        out->seq          = seq;
        out->declared_len = body_len;
        out->crc_received = received;
        out->crc_computed = computed;

        if (received != computed) {
            /* THE CENTRAL RULE: a frame that fails its CRC is never acted upon, not even
             * partially. The fields below are reported for the log only, and the BAD_CRC event
             * itself flags them as untrustworthy. */
            p->stat_bad_crc++;
            out->flags = (body_len > 0u) ? p->body[0] : 0u;
            out->chan  = (body_len > 1u) ? p->body[1] : 0u;
            out->cmd   = (body_len > 2u) ? p->body[2] : IOP_CMD_NONE;
            return IOP_EV_BAD_CRC;
        }

        p->stat_frames_ok++;
        out->flags       = p->body[0];
        out->chan        = p->body[1];
        out->cmd         = p->body[2];
        out->payload     = &p->body[IOP_BODY_HEADER_LEN];
        out->payload_len = (uint16_t)(body_len - IOP_BODY_HEADER_LEN);
        return IOP_EV_FRAME;
    }

    case IOP_ST_SKIP:
        if (p->skip_remaining > 0) {
            p->skip_remaining--;
        }
        if (p->skip_remaining == 0) {
            IopEvent reason   = (IopEvent)p->bad_reason;
            uint8_t  seq      = p->seq;
            uint16_t declared = p->expected_len;

            iop_parser_reset(p);
            if (reason == IOP_EV_OVERSIZE) { p->stat_oversize++; }
            else                           { p->stat_bad_length++; }
            memset(out, 0, sizeof(*out));
            out->seq          = seq;
            out->cmd          = IOP_CMD_NONE;
            out->declared_len = declared;
            return reason;
        }
        return IOP_EV_NONE;

    default:
        iop_parser_reset(p);
        return IOP_EV_NONE;
    }
}

static inline const char *iop_event_name(IopEvent ev)
{
    switch (ev) {
        case IOP_EV_NONE:           return "NONE";
        case IOP_EV_FRAME:          return "FRAME";
        case IOP_EV_BAD_CRC:        return "BAD_CRC";
        case IOP_EV_BAD_LENGTH:     return "BAD_LENGTH";
        case IOP_EV_OVERSIZE:       return "OVERSIZE";
        case IOP_EV_TIMEOUT:        return "TIMEOUT";
        case IOP_EV_RESYNC_DISCARD: return "RESYNC_DISCARD";
        default:                    return "?";
    }
}

/* ===========================================================================================
 * SECTION 10 -- THE FRAME BUILDER
 * ===========================================================================================
 *
 * Writes into a caller-supplied buffer rather than to a serial port, for three practical
 * reasons: it keeps this header free of Arduino types (which is what makes it portable and
 * testable); it lets the caller send the whole frame in ONE write, handing the UART driver a
 * contiguous block that is far more likely to reach the wire as one uninterrupted burst -- which
 * matters because the receiver runs an inter-byte timeout; and it makes the frame inspectable
 * before transmission, which the hex-dump debug mode relies on.
 *
 * Returns 0 rather than truncating. A truncated frame is indistinguishable on the wire from a
 * corrupted one and would burn the receiver's timeout for nothing. Refusing is loud and local;
 * truncating is silent and remote. Always check the return value. */
static inline uint16_t iop_build_frame(uint8_t *out, uint16_t out_cap,
                                       uint8_t seq, uint8_t flags, uint8_t chan, uint8_t cmd,
                                       const uint8_t *payload, uint16_t payload_len)
{
    uint16_t body_len;
    uint16_t total;
    uint16_t crc;

    if (out == 0) {
        return 0;
    }
    if (payload_len > IOP_MAX_PAYLOAD_LEN) {
        return 0;
    }
    if (payload_len > 0 && payload == 0) {
        return 0;
    }

    body_len = (uint16_t)(payload_len + IOP_BODY_HEADER_LEN);
    total    = (uint16_t)(body_len + IOP_FRAME_OVERHEAD);
    if (total > out_cap) {
        return 0;
    }

    out[0] = (uint8_t)IOP_START_BYTE;
    out[1] = (uint8_t)((body_len >> 8) & 0xFFu);
    out[2] = (uint8_t)(body_len & 0xFFu);
    out[3] = seq;
    out[4] = flags;
    out[5] = chan;
    out[6] = cmd;
    if (payload_len > 0) {
        memcpy(&out[7], payload, payload_len);
    }

    /* CRC spans LEN_H..last payload byte: out[1] through out[6 + payload_len], i.e.
     * (body_len + 3) bytes starting at out[1]. Derived from body_len rather than written as a
     * literal so it stays correct if the header ever grows a field. */
    crc = iop_crc16(&out[1], (uint16_t)(body_len + 3u));
    out[7 + payload_len] = (uint8_t)((crc >> 8) & 0xFFu);
    out[8 + payload_len] = (uint8_t)(crc & 0xFFu);

    return total;
}

/* Convenience wrapper for the error frame, since it is built from several places on each board
 * and getting its three-byte payload order wrong would be easy. */
static inline uint16_t iop_build_nack(uint8_t *out, uint16_t out_cap, uint8_t seq,
                                      uint8_t orig_chan, uint8_t orig_cmd, uint8_t error_code)
{
    uint8_t payload[3];
    payload[0] = orig_chan;
    payload[1] = orig_cmd;
    payload[2] = error_code;
    return iop_build_frame(out, out_cap, seq, IOP_FLAG_NONE, IOP_CHAN_TRANSPORT,
                           IOP_CMD_NACK, payload, 3);
}

/* ===========================================================================================
 * SECTION 11 -- NACK RATE LIMITING
 * ===========================================================================================
 * On a badly broken link -- baud mismatch, floating ground, a board rebooting in a loop -- both
 * ends see continuous garbage and both emit an error frame per malformed frame. Those cross the
 * broken link, arrive corrupted, and provoke more. At 921600 baud that feedback loop saturates
 * the wire in both directions with complaints, and the actual fault is buried under the noise its
 * own reporting created. A persistent error is worth reporting ONCE.
 *
 * Applied only to error NACKs. A NACK answering a well-formed request -- BUSY, for instance --
 * is a real response to a real question and is never suppressed, because dropping it would leave
 * the master waiting for nothing. */
#define IOP_NACK_MIN_INTERVAL_MS  200u
#define IOP_NACK_BURST            3u

typedef struct {
    uint32_t last_ms;
    uint8_t  burst_used;
    uint32_t suppressed;   /* report this, never hide it */
} IopNackLimiter;

static inline void iop_nack_limiter_init(IopNackLimiter *l)
{
    l->last_ms    = 0;
    l->burst_used = 0;
    l->suppressed = 0;
}

static inline int iop_nack_should_send(IopNackLimiter *l, uint32_t now_ms)
{
    if ((uint32_t)(now_ms - l->last_ms) >= IOP_NACK_MIN_INTERVAL_MS) {
        l->burst_used = 0;
    }
    if (l->burst_used < IOP_NACK_BURST) {
        l->burst_used++;
        l->last_ms = now_ms;
        return 1;
    }
    l->suppressed++;
    return 0;
}

/* ===========================================================================================
 * SECTION 12 -- POWER-ON SELF-TEST
 * ===========================================================================================
 * Every failure this file can suffer -- a mistyped table entry, a header out of sync between the
 * two sketch folders, a compiler packing a struct unexpectedly -- presents identically at runtime
 * as "the link does not work". That is the least informative symptom imaginable. Ten milliseconds
 * of arithmetic at boot turns an afternoon of oscilloscope work into one line of serial output.
 *
 * If this ever fails, STOP: the problem is in this file or in how it was copied, not the wiring.
 */
#define IOP_SELFTEST_TABLE_MISMATCH   0x0001u
#define IOP_SELFTEST_CHECK_VALUE      0x0002u
#define IOP_SELFTEST_ROUNDTRIP        0x0004u
#define IOP_SELFTEST_CORRUPTION_MISS  0x0008u
#define IOP_SELFTEST_OVERSIZE         0x0010u
#define IOP_SELFTEST_FALSE_START      0x0020u
#define IOP_SELFTEST_BUILDER_BOUNDS   0x0040u
#define IOP_SELFTEST_SEQ_CLASS        0x0080u

static inline uint16_t iop_selftest(IopParser *scratch)
{
    uint16_t failures = 0;
    uint8_t  buf[80];
    uint8_t  probe[3];
    IopFrame frame;
    IopEvent ev;
    uint16_t n;
    uint16_t i;
    uint32_t t = 0;   /* a fake clock -- the parser takes time as an argument, which is exactly
                       * what makes this testable with no waiting */

    memset(&frame, 0, sizeof(frame));
    memset(buf, 0, sizeof(buf));

    /* --- 1: the table agrees with the independent bitwise implementation. --------------- */
    for (i = 0; i < 256u; i++) {
        probe[0] = (uint8_t)i;
        probe[1] = (uint8_t)(255u - i);
        probe[2] = (uint8_t)(i ^ 0x5Au);
        if (iop_crc16(probe, 1) != iop_crc16_bitwise(probe, 1) ||
            iop_crc16(probe, 3) != iop_crc16_bitwise(probe, 3)) {
            failures |= IOP_SELFTEST_TABLE_MISMATCH;
            break;
        }
    }

    /* --- 2: the catalogue check value. This is the number to quote when someone else
     * implements this protocol. If theirs prints anything else, they built a different CRC. -- */
    {
        const uint8_t check_input[9] = { '1','2','3','4','5','6','7','8','9' };
        if (iop_crc16(check_input, 9) != IOP_CRC16_CHECK_VALUE) {
            failures |= IOP_SELFTEST_CHECK_VALUE;
        }
    }

    /* --- 3: build -> parse round trip, with a payload byte equal to the START marker. That
     * 0xAA is deliberate: it proves payload data is never mistaken for framing once synced. -- */
    {
        const uint8_t payload[4] = { 0x01u, 0xAAu, 0xFFu, 0x00u };
        iop_parser_init(scratch);
        n = iop_build_frame(buf, sizeof(buf), 0x42u, IOP_FLAG_NONE,
                            IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, payload, 4);
        if (n != (4u + IOP_BODY_HEADER_LEN + IOP_FRAME_OVERHEAD)) {
            failures |= IOP_SELFTEST_ROUNDTRIP;
        } else {
            ev = IOP_EV_NONE;
            for (i = 0; i < n; i++) {
                ev = iop_parser_push(scratch, buf[i], t, &frame);
            }
            if (ev != IOP_EV_FRAME ||
                frame.seq != 0x42u ||
                frame.chan != IOP_CHAN_WIFI ||
                frame.cmd != IOP_CMD_WIFI_SCAN_REQ ||
                frame.payload_len != 4u ||
                frame.payload[1] != 0xAAu) {
                failures |= IOP_SELFTEST_ROUNDTRIP;
            }
        }
    }

    /* --- 4: a single flipped bit is caught. The whole justification for spending two bytes on
     * a CRC is this behaviour, so it is tested rather than assumed. ---------------------- */
    {
        iop_parser_init(scratch);
        n = iop_build_frame(buf, sizeof(buf), 0x07u, IOP_FLAG_NONE,
                            IOP_CHAN_TRANSPORT, IOP_CMD_PONG, 0, 0);
        buf[6] ^= 0x01u;                 /* corrupt the CMD byte, exactly one bit */
        ev = IOP_EV_NONE;
        for (i = 0; i < n; i++) {
            ev = iop_parser_push(scratch, buf[i], t, &frame);
        }
        if (ev != IOP_EV_BAD_CRC) {
            failures |= IOP_SELFTEST_CORRUPTION_MISS;
        }
    }

    /* --- 5: an absurd length is judged a false start and resyncs IMMEDIATELY, and the parser
     * is usable again with no timeout needed. That second half is what proves the fix that a
     * live PING being swallowed on real hardware forced. ------------------------------- */
    {
        uint8_t bad[5];
        bad[0] = (uint8_t)IOP_START_BYTE;
        bad[1] = 0xFFu;
        bad[2] = 0xFFu;                  /* 65535, far past IOP_MAX_SKIP_BYTES */
        bad[3] = 0x99u;                  /* SEQ, which must still be captured  */
        bad[4] = 0x00u;

        iop_parser_init(scratch);
        for (i = 0; i < 5u; i++) {
            (void)iop_parser_push(scratch, bad[i], t, &frame);
        }
        if (frame.seq != 0x99u || frame.declared_len != 0xFFFFu) {
            failures |= IOP_SELFTEST_OVERSIZE;
        }
        if (scratch->state != IOP_ST_WAIT_START) {
            failures |= IOP_SELFTEST_OVERSIZE;
        }
        n = iop_build_frame(buf, sizeof(buf), 0x11u, IOP_FLAG_NONE,
                            IOP_CHAN_TRANSPORT, IOP_CMD_PING, 0, 0);
        ev = IOP_EV_NONE;
        for (i = 0; i < n; i++) {
            ev = iop_parser_push(scratch, buf[i], t, &frame);
        }
        if (ev != IOP_EV_FRAME || frame.seq != 0x11u) {
            failures |= IOP_SELFTEST_OVERSIZE;
        }
    }

    /* --- 6: recovery from a false START byte -- the everyday resync path. --------------- */
    {
        iop_parser_init(scratch);
        (void)iop_parser_push(scratch, (uint8_t)IOP_START_BYTE, t, &frame);
        (void)iop_parser_push(scratch, 0x00u, t, &frame);
        (void)iop_parser_push(scratch, 0x00u, t, &frame);   /* LEN 0 -> impossible */
        (void)iop_parser_push(scratch, 0x55u, t, &frame);   /* SEQ captured        */
        ev = IOP_EV_NONE;
        for (i = 0; i < 2u; i++) {
            ev = iop_parser_push(scratch, 0x00u, t, &frame);  /* drain body+CRC */
        }
        if (ev != IOP_EV_BAD_LENGTH) {
            failures |= IOP_SELFTEST_FALSE_START;
        }
        n = iop_build_frame(buf, sizeof(buf), 0x21u, IOP_FLAG_NONE,
                            IOP_CHAN_TRANSPORT, IOP_CMD_PONG, 0, 0);
        ev = IOP_EV_NONE;
        for (i = 0; i < n; i++) {
            ev = iop_parser_push(scratch, buf[i], t, &frame);
        }
        if (ev != IOP_EV_FRAME || frame.seq != 0x21u || frame.cmd != IOP_CMD_PONG) {
            failures |= IOP_SELFTEST_FALSE_START;
        }
    }

    /* --- 7: the builder refuses what it cannot legally send. ---------------------------- */
    {
        static const uint8_t dummy[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        if (iop_build_frame(buf, 4u, 0x00u, IOP_FLAG_NONE,
                            IOP_CHAN_TRANSPORT, IOP_CMD_PING, dummy, 8) != 0) {
            failures |= IOP_SELFTEST_BUILDER_BOUNDS;
        }
        if (iop_build_frame(buf, sizeof(buf), 0x00u, IOP_FLAG_NONE,
                            IOP_CHAN_TRANSPORT, IOP_CMD_PING,
                            dummy, (uint16_t)(IOP_MAX_PAYLOAD_LEN + 1u)) != 0) {
            failures |= IOP_SELFTEST_BUILDER_BOUNDS;
        }
    }

    /* --- 8: the two sequence classes never collide. A request counter must never produce a
     * value the receiver would read as an unsolicited announcement, and vice versa. ------ */
    {
        uint8_t rs = 0, es = IOP_SEQ_CLASS_MASK;
        for (i = 0; i < 600u; i++) {
            rs = iop_next_request_seq(rs);
            es = iop_next_event_seq(es);
            if (IOP_SEQ_IS_UNSOLICITED(rs) || !IOP_SEQ_IS_UNSOLICITED(es)) {
                failures |= IOP_SELFTEST_SEQ_CLASS;
                break;
            }
        }
    }

    iop_parser_init(scratch);
    return failures;
}

static inline const char *iop_selftest_name(uint16_t bit)
{
    switch (bit) {
        case IOP_SELFTEST_TABLE_MISMATCH:  return "CRC table does not match the bitwise reference";
        case IOP_SELFTEST_CHECK_VALUE:     return "CRC check value is wrong (expected 0x6F91)";
        case IOP_SELFTEST_ROUNDTRIP:       return "build/parse round trip failed";
        case IOP_SELFTEST_CORRUPTION_MISS: return "a corrupted frame was NOT detected";
        case IOP_SELFTEST_OVERSIZE:        return "oversize handling or resync failed";
        case IOP_SELFTEST_FALSE_START:     return "recovery from a false START byte failed";
        case IOP_SELFTEST_BUILDER_BOUNDS:  return "frame builder accepted an illegal frame";
        case IOP_SELFTEST_SEQ_CLASS:       return "request and unsolicited SEQ classes collide";
        default:                           return "unknown self-test failure";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_PROTOCOL_H */

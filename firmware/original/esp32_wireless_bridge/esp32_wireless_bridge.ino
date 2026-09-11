/* ===========================================================================================
 *  esp32_wireless_bridge.ino  --  ESP32-S3, SLAVE side of the interop link
 * ===========================================================================================
 *
 *  ROLE
 *  ----
 *  This board is a RADIO. The Teensy 4.1 is the brain.
 *
 *  That is the whole design, and it is worth being blunt about because the ESP32 is a capable
 *  computer in its own right and the temptation to let it think is constant. Resist it. The
 *  rule this sketch is built on:
 *
 *      MECHANISM HERE. POLICY ON THE TEENSY.
 *
 *  This sketch knows how to drive an 802.11 MAC -- start a scan, attempt a join, read a reason
 *  code, report an address. It does not decide which network matters, how many times to retry,
 *  or whether a failure is worth escalating. It reports what happened, in our own wire format,
 *  and the brain decides what that means.
 *
 *  Concretely, that is why WIFI_CONNECT_RESP carries the raw 802.11 reason code alongside the
 *  friendly status byte: the status is this board's best guess, the reason code is the truth,
 *  and the brain gets both so it can overrule the guess. And it is why the radio ALSO speaks
 *  without being asked -- see "UNSOLICITED FRAMES" below.
 *
 *  Keeping this boundary clean is what makes the radio replaceable. Nothing in the protocol
 *  mentions Espressif; the auth modes and status codes are translated to our own enums right
 *  here, at the radio's edge. Swap this chip for a different module, or add BLE beside it, and
 *  only this file changes.
 *
 *
 *  UNSOLICITED FRAMES: why a radio must be able to interrupt
 *  ---------------------------------------------------------
 *  A peripheral that only answers questions is a sensor you have to poll. A radio is not that.
 *  Connections drop, access points vanish, and this chip can brown out and reboot -- all on the
 *  radio's schedule, never the brain's.
 *
 *  So this sketch sends two kinds of frame nobody asked for, both tagged SEQ 0x00:
 *
 *    RADIO_READY, once, immediately after boot. It tells the brain that the radio restarted,
 *    why it restarted, what protocol build it is running, and what it is capable of. Without
 *    it, a brownout-induced reset is invisible: the brain goes on believing it is associated
 *    with an AP while the radio sits in a blank state, and the fault surfaces minutes later as
 *    "the WiFi randomly stopped working".
 *
 *    WIFI_EVENT, whenever the radio's state changes -- associated, got an address, dropped,
 *    with the raw reason. The brain learns about a 3am disconnect at 3am, rather than the next
 *    time it happens to ask.
 *
 *  Everything else here is still strictly request/response.
 *
 *  THE CENTRAL DESIGN DECISION: NOTHING HERE BLOCKS
 *  ------------------------------------------------
 *  A WiFi scan takes seconds. Joining a network takes seconds. The obvious implementation is
 *
 *      case WIFI_SCAN_REQ:  n = WiFi.scanNetworks();  sendResults(n);   // DON'T
 *
 *  and it is wrong in a way that is not obvious until it bites. While that call is blocked:
 *
 *    - loop() does not run, so the UART receive buffer is never drained. The Teensy can send
 *      a PING, get no answer, time out, and conclude the link is dead -- while the ESP32 is
 *      perfectly healthy and merely busy.
 *    - Bytes that arrive during the block sit in a fixed-size hardware/driver buffer. Enough
 *      of them and they are silently dropped, which shows up later as a mysterious bad CRC.
 *    - The status LED freezes, so the one piece of out-of-band feedback you have goes dark
 *      exactly when you most want to know what the board is doing.
 *
 *  So both long operations are driven as EXPLICIT STATE MACHINES using the asynchronous forms
 *  of the WiFi API -- scanNetworks(async=true) polled with scanComplete(), and begin() polled
 *  with status() plus the disconnect-reason event. loop() keeps spinning the whole time, the
 *  link stays responsive, and PING keeps answering.
 *
 *  This is also precisely why the protocol has a BUSY error code. A second WiFi request that
 *  arrives mid-operation is refused with NACK 0x04 -- an honest, immediate answer instead of
 *  a queue that silently changes the meaning of "the command completed".
 *
 *  WIRING
 *  ------
 *      ESP32-S3                            Teensy 4.1
 *      --------                            ----------
 *      GPIO17 (TX)  --------------------->  pin 0 (RX1)
 *      GPIO18 (RX)  <---------------------  pin 1 (TX1)
 *      GND          <-------------------->  GND     <-- REQUIRED
 *
 *  TX goes to RX, crossed. See LINK_TX_PIN below for why these two GPIOs specifically.
 *
 *  BUILD
 *  -----
 *  Board: "ESP32S3 Dev Module" (or your specific S3 board).
 *  interop_protocol.h must be in THIS folder -- see the README.
 * ===========================================================================================
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "interop_protocol.h"
#include "mesh_protocol.h"

/* ===========================================================================================
 * CONFIGURATION
 * =========================================================================================== */

#define LINK_PORT        Serial1     /* the hardware UART to the Teensy                       */
#define LINK_BAUD        921600      /* must match the Teensy sketch exactly                  */
#define DEBUG_PORT       Serial      /* human-readable output -- BUT SEE THE NOTE BELOW       */
#define DEBUG_BAUD       115200

/* WHERE DOES "Serial" ACTUALLY GO ON AN ESP32-S3? READ THIS BEFORE YOU DEBUG A SILENT BOARD.
 *
 * On this chip Serial is not an object, it is a MACRO, and what it expands to depends on a
 * board menu setting -- Tools > USB CDC On Boot. That makes it the single most confusing thing
 * about bringing up an S3, because the same line of code sends output to two different physical
 * connectors depending on a menu you may never have opened.
 *
 *   USB CDC On Boot = Disabled   (THE DEFAULT)
 *       Serial expands to Serial0, which is UART0 on GPIO43/44. On a typical devkit those pins
 *       run to the on-board USB-to-serial chip, so your output appears on the port labelled
 *       "UART" or "COM". Plug into the port labelled "USB" (the native one) and you will see
 *       ABSOLUTELY NOTHING -- no error, no warning, just a board that appears dead.
 *
 *   USB CDC On Boot = Enabled
 *       Serial expands to HWCDCSerial, the chip's native USB. Output now appears on the "USB"
 *       port and nothing comes out of the "UART" port.
 *
 * Two ports, one macro, no diagnostic. If your boot banner never appears, this is why -- try
 * the other USB socket before you touch the wiring. The banner below prints which one this
 * build is using, so the board tells you itself.
 *
 * A related trap worth knowing: because Serial is a macro, never name a variable, struct field
 * or parameter "Serial" anywhere in shared code. It gets textually substituted and produces
 * incomprehensible errors on ESP32 while compiling perfectly on the Teensy.
 */
#if ARDUINO_USB_CDC_ON_BOOT
  #define DEBUG_PORT_NAME "native USB (USB CDC On Boot = Enabled)"
#else
  #define DEBUG_PORT_NAME "UART0 on GPIO43/44 (USB CDC On Boot = Disabled) -- use the UART port"
#endif

/* UART pins to the Teensy.
 *
 * WHY THESE TWO, specifically. On an ESP32-S3 most GPIOs are freely mappable to any
 * peripheral, but a good number are spoken for and using one produces a board that fails to
 * boot, fails to flash, or corrupts its own RAM. The ones to avoid on a WROOM-1 module:
 *
 *      GPIO0, 3, 45, 46   strapping pins, sampled at reset. A pull on these changes boot mode.
 *      GPIO19, 20         native USB D- and D+. Using them kills USB, including the CDC port
 *                         you are reading debug output from.
 *      GPIO26..32         wired to the SPI flash chip. Touching these bricks the running app.
 *      GPIO33..37         wired to octal PSRAM on -N8R8 / -N16R8 modules. Safe ONLY on modules
 *                         with quad or no PSRAM, which is not a bet worth making in a
 *                         reference design.
 *      GPIO43, 44         default UART0 TX/RX, i.e. the serial-monitor port on many boards.
 *
 * GPIO17 and GPIO18 are outside every one of those ranges on all common S3 modules, are
 * adjacent on the usual pin headers, and have no boot-time function. If your board breaks
 * out different pins, change these two lines -- nothing else in the sketch cares.
 */
#define LINK_TX_PIN      17
#define LINK_RX_PIN      18

/* Receive buffer for the UART driver.
 *
 * The core's default is 256 bytes. At 921600 baud that is about 2.8 ms of data. Any single
 * frame can be up to 1024 bytes, so a full frame plus margin removes the possibility of an
 * overrun while loop() is momentarily busy. Must be set BEFORE begin() -- afterwards the call
 * is ignored, silently, which is a fine way to spend an evening. */
#define LINK_RX_BUFFER   (IOP_MAX_FRAME_SIZE * 2)

/* How often to report the link is alive when nothing is happening. Purely a debug convenience;
 * it is the difference between "the board is idle" and "the board has crashed". */
#define HEARTBEAT_MS     10000u

/* WiFi.disconnect() -- ALWAYS pass the third argument.
 *
 * The signature in this core is
 *     bool disconnect(bool wifioff = false, bool eraseap = false, unsigned long timeout = 100)
 * so the familiar two-argument call blocks for up to 100 ms, polled internally with delay(5).
 * That is a real blocking delay hiding inside an innocuous-looking call, and this sketch has a
 * no-blocking rule. Passing 0 makes it fire-and-forget, which is all that is needed here:
 * nothing depends on the teardown having finished before the next line runs.
 *
 * This is the kind of default that only shows up when you read the header. It is exactly why
 * every API signature behind this sketch was checked against the installed core rather than
 * recalled from memory. */
#define WIFI_DISCONNECT_NONBLOCKING_TIMEOUT_MS  0

/* ===========================================================================================
 * EXPLICIT PROTOTYPES  (see the same note in the Teensy sketch -- this defeats the Arduino
 * .ino auto-prototype generator, which otherwise emits declarations mentioning IopFrame
 * before interop_protocol.h has been included.)
 * =========================================================================================== */
static void     linkPoll(void);
static void     handleFrame(const IopFrame *f);
static void     handlePing(const IopFrame *f);
static void     handleScanRequest(const IopFrame *f);
static void     handleConnectRequest(const IopFrame *f);
static void     scanPoll(void);
static void     connectPoll(void);
static void     finishConnect(uint8_t status);
static void     sendFrame(uint8_t seq, uint8_t chan, uint8_t cmd,
                          const uint8_t *payload, uint16_t len);
static void     sendEvent(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len);
static void     sendNack(uint8_t seq, uint8_t origChan, uint8_t origCmd, uint8_t errorCode);
static void     sendErrorNack(uint8_t seq, uint8_t origChan, uint8_t origCmd, uint8_t errorCode);
static void     sendScanResults(int16_t count);
static void     sendConnectResult(uint8_t status);
static uint8_t  mapEncryption(int authmode);
static uint8_t  mapDisconnectReason(uint8_t reason);
static void     setStatusLed(uint8_t r, uint8_t g, uint8_t b);
static void     onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info);
static void     hexDump(const char *prefix, const uint8_t *data, uint16_t n);
static void     handleMeshFrame(const IopFrame *f);
static void     meshPoll(void);
static bool     meshBegin(uint8_t channel, uint8_t mode);
static int      meshFindNode(const uint8_t *mac);
static void     eventPush(uint8_t type, uint8_t reason);
static void     eventPoll(void);
static void     sendRadioReady(void);
static void     sendWifiEvent(uint8_t type, uint8_t reason);
static void     handleStatusRequest(const IopFrame *f);
static uint8_t  currentWifiState(void);

/* ===========================================================================================
 * STATE
 * =========================================================================================== */

/* What long-running operation, if any, is in progress. Exactly one at a time -- see the BUSY
 * discussion in the file header. */
typedef enum {
    BRIDGE_IDLE = 0,
    BRIDGE_SCANNING,
    BRIDGE_CONNECTING
} BridgeState;

static BridgeState gState = BRIDGE_IDLE;

/* The SEQ of the request that started the current operation. It is stashed the moment the
 * request arrives, because the response may be built seconds later -- long after the frame
 * that asked for it has been overwritten in the parser buffer. Echoing the right SEQ is the
 * whole basis of request/response matching, so it gets its own variable rather than being
 * fished back out of anything. */
static uint8_t  gOperationSeq = 0;

/* The unsolicited sequence counter. Starts in the unsolicited class so the very first
 * event already carries the class bit. */
static uint8_t  gEventSeq = IOP_SEQ_CLASS_MASK;

/* When the current operation must be abandoned. */
static uint32_t gOperationDeadline = 0;

static IopParser gParser;

/* Throttles error NACKs. Errors provoked by garbage on the wire are rate limited; NACKs that
 * answer a well-formed request (BUSY, for instance) are not, because those are real responses
 * that a waiting master needs. */
static IopNackLimiter gNackLimiter;
static uint8_t   gTxBuf[IOP_MAX_FRAME_SIZE];
static uint8_t   gPayloadBuf[IOP_MAX_PAYLOAD_LEN];

/* Credentials for the join in progress. Plain char arrays, deliberately.
 *
 * WHY NOT String: an Arduino String allocates on the heap and reallocates as it grows. On a
 * board expected to run for weeks, repeated allocate/free cycles of varying sizes fragment the
 * heap until a later allocation fails even though plenty of total memory is free. The failure
 * lands somewhere unrelated -- typically inside the WiFi stack -- days after the code that
 * caused it. Fixed buffers sized to the 802.11 limits cannot do that, and the limits are known
 * constants, so there is nothing to gain from dynamic sizing. */
static char gSsid[IOP_SSID_MAX_LEN + 1];
static char gPassword[IOP_PASS_MAX_LEN + 1];

/* Set from the WiFi event callback when the station is disconnected, with the reason code.
 *
 * volatile: the callback runs from the Arduino event task, a DIFFERENT FreeRTOS task from
 * loop(). Without volatile the compiler is entitled to cache these in registers across the
 * polling loop and never observe the update. The values are single bytes/words, so torn reads
 * are not a concern on this architecture; volatile alone is the right and sufficient tool. */
static volatile uint8_t  gLastDisconnectReason = 0;
static volatile bool     gDisconnectSeen = false;
static volatile uint8_t  gNoApFoundCount = 0;
static volatile uint8_t  gLastEventType = 0;        /* most recent IOP_WIFI_EVT_* emitted  */
static volatile uint8_t  gAuthFailCount = 0;        /* reasons 2, 202: the AP said no      */
static volatile uint8_t  gHandshakeFailCount = 0;   /* reasons 15, 204: probably said no   */

/* ------------------------------------------------------------------------------------------
 * RADIO EVENT QUEUE -- crossing from the WiFi task to loop()
 *
 * The Arduino WiFi callback runs on the "arduino_events" FreeRTOS task, not in loop(). Frames
 * must NEVER be transmitted from there: two tasks writing gTxBuf and Serial1 at once is a data
 * race, and the bug it produces (occasional garbled frames, only under WiFi activity) is one of
 * the worst kinds to chase.
 *
 * So the callback does the least possible work -- push a small record into this ring -- and
 * loop() drains it and does the transmitting. Single producer, single consumer, one slot always
 * left empty so that head == tail unambiguously means "empty": that arrangement needs no mutex,
 * because each index is written by exactly one task.
 *
 * The ring is deliberately tiny. If events arrive faster than loop() drains them the OLDEST are
 * dropped and counted, because during a failing join the core's auto-reconnect can emit a burst
 * of near-identical disconnects, and the newest state is the one the brain actually needs.
 * Dropping is reported, never silent.
 */
#define RADIO_EVENT_QUEUE_SIZE  8u

typedef struct {
    uint8_t type;      /* IOP_WIFI_EVT_*                     */
    uint8_t reason;    /* raw 802.11 reason, 0 if not a drop */
} RadioEvent;

static volatile RadioEvent gEventQueue[RADIO_EVENT_QUEUE_SIZE];
static volatile uint8_t    gEventHead = 0;   /* written by the WiFi task only */
static volatile uint8_t    gEventTail = 0;   /* written by loop() only        */
static volatile uint32_t   gEventsDropped = 0;

static uint32_t gLastHeartbeat = 0;
static bool     gHexDump = false;

/* Counters, printed with the heartbeat. */
static struct {
    uint32_t framesHandled;
    uint32_t nacksSent;
    uint32_t scansDone;
} gStats = { 0, 0, 0 };

/* ===========================================================================================
 * THE MESH -- ESP-NOW, and the hub half of the fleet
 * ===========================================================================================
 *
 * The brain has no radio. It directs this board, which relays. Same mechanism/policy split as
 * everywhere else, one layer further out: this code moves packets and measures time; it does not
 * decide which nodes matter or what a silent one means.
 *
 * THE CHANNEL PROBLEM, which is the thing that actually bites.
 * ESP-NOW peers must share a WiFi channel. That is fine until the hub also wants to be a WiFi
 * station, because then THE ACCESS POINT CHOOSES THE CHANNEL and every node has to follow. The
 * failure mode is nasty: everything initialises without error, every esp_now_send() returns
 * ESP_OK, and not one packet is ever received. There is no error to read, because from the
 * radio's point of view nothing is wrong -- it transmitted, on the channel it was told to use.
 *
 * Both modes are implemented so the cost of coexistence can be MEASURED rather than argued:
 *   IOP_MESH_MODE_ONLY       fixed channel, no AP. Deterministic, nothing else contends.
 *   IOP_MESH_MODE_WITH_WIFI  station mode stays up and the mesh inherits the AP's channel.
 */
/* Bring the mesh up on our own if the brain has not done so within this window.
 *
 * WHY THIS EXISTS AND WHY IT IS EXPLICIT: normally the brain owns the decision -- it picks the
 * channel and the mode, because that is policy. But a radio that CANNOT be exercised without a
 * brain attached is untestable exactly when you most need to test it: during bring-up, when the
 * UART may not be working yet. That is not hypothetical here; this feature was added while the
 * link to the Teensy was down with a broken jumper, and without it the mesh could not have been
 * measured at all.
 *
 * The brain's later MESH_INIT_REQ overrides whatever autostart chose, so this is a fallback and
 * never a competing authority. Set to 0 to require an explicit command. */
#define MESH_AUTOSTART_MS     8000u
#define MESH_AUTOSTART_CHAN   1u

/* ---- Autostart ping train ----
 *
 * WHAT IT MEASURES. Hub sends a token, node echoes it, hub stops the timer -- start and stop on
 * ONE clock, so no agreement between the two boards' oscillators is required or assumed. Half the
 * round trip estimates one-way air time. This is the number the second board was added to
 * produce, and it is the only honest way to get it from two free-running crystals. Same reasoning
 * as the timing note at the top of mesh_protocol.h.
 *
 * WHY IT AUTOSTARTS. Same argument as the mesh autostart above: the measurement should not depend
 * on the brain being present and correctly programmed. It ran first while the brain still had no
 * MESH commands at all, and it stays because a fleet that self-characterises on boot tells you
 * the air is healthy before you have written a line of application code.
 *
 * The delay is measured from the moment the first node is DISCOVERED, not from boot. There is
 * nothing to ping until a node exists, and the settle time lets the node finish its own
 * initialisation so the first trains are not measuring its boot sequence.
 *
 * The result is published as an unsolicited EVENT, not a response -- nobody asked for it, and
 * dressing an unrequested measurement up as a reply to a request the brain never sent is exactly
 * the confusion the SEQ class bit exists to prevent. Set to 0 to require an explicit command. */
#define MESH_PING_AUTOSTART_MS     4000u   /* after the first node is seen */
#define MESH_PING_AUTOSTART_COUNT    30u
#define MESH_PING_REPEAT_MS       60000u   /* re-characterise the air every minute; 0 = once */

#define MESH_MAX_NODES        8u
#define MESH_NODE_TIMEOUT_MS  5000u   /* silence after which a node is declared lost */
#define MESH_RX_QUEUE_SIZE    12u

typedef struct {
    uint8_t  mac[6];
    bool     used;
    bool     alive;
    uint32_t lastSeenMs;
    uint32_t packets;
    uint16_t lastSeq;
    bool     seqValid;
    uint32_t lostPackets;   /* inferred from gaps in the node's own sequence numbers */
    uint8_t  relayCount;    /* counts toward the stream divisor; per-node so one chatty node
                             * cannot starve a quiet one out of its share of the stream */
} MeshNode;

static MeshNode gNodes[MESH_MAX_NODES];
static uint8_t  gMeshState   = IOP_MESH_STATE_OFF;
static uint8_t  gMeshChannel = 0;
static uint8_t  gMeshMode    = IOP_MESH_MODE_ONLY;
static uint32_t gMeshRx = 0, gMeshRxDropped = 0;

/* How much of the node stream to forward to the brain: 0 = none, 1 = all, N = every Nth.
 *
 * DEFAULTS TO OFF, and that default is the entire point -- see the long note beside
 * IOP_MESH_STREAM_OFF in interop_protocol.h. A radio that starts shouting at 200 Hz before the
 * brain has said a word is making an application decision on the brain's behalf. This board
 * keeps counting every packet either way, so turning the stream off costs no statistics. */
static uint8_t  gMeshStream = IOP_MESH_STREAM_DEFAULT;
static uint32_t gMeshRelayed = 0, gMeshDecimated = 0;

/* Received ESP-NOW payloads, queued for loop() to relay.
 *
 * The receive callback runs on the WiFi task. Building and transmitting a UART frame from there
 * would mean two tasks writing gTxBuf and Serial1 concurrently -- the same race the WiFi event
 * queue exists to avoid, and it would produce occasionally-garbled frames only under load. */
typedef struct {
    uint8_t  mac[6];
    uint8_t  len;
    uint8_t  data[MESH_MAX_ESPNOW_PAYLOAD];
} MeshRxItem;

static volatile MeshRxItem gMeshRxQ[MESH_RX_QUEUE_SIZE];
static volatile uint8_t    gMeshRxHead = 0;   /* written by the WiFi task only */
static volatile uint8_t    gMeshRxTail = 0;   /* written by loop() only        */

/* Ping measurement, timed entirely on THIS board's clock so no cross-board clock agreement is
 * needed -- see the timing note in mesh_protocol.h. */
static bool     gPingActive = false;
static uint8_t  gPingMac[6];
static uint16_t gPingRemaining = 0, gPingSent = 0, gPingRecv = 0;
static uint32_t gPingToken = 0, gPingSentUs = 0, gPingDeadline = 0;
static uint32_t gPingMinUs = 0xFFFFFFFFu, gPingMaxUs = 0, gPingSumUs = 0;
static uint8_t  gPingSeq = 0;

/* Autostart bookkeeping. gPingAuto decides whether the finished train is reported as a RESPONSE
 * (the brain asked) or as an EVENT (nobody asked) -- the two are not interchangeable. */
static bool     gPingAuto = false;
static uint32_t gFirstNodeMs = 0;      /* 0 = no node has ever been seen */
static uint32_t gNextAutoPingMs = 0;   /* 0 = not scheduled */

static int meshFindNode(const uint8_t *mac)
{
    uint8_t i;
    for (i = 0; i < MESH_MAX_NODES; i++) {
        if (gNodes[i].used && memcmp(gNodes[i].mac, mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int meshAddNode(const uint8_t *mac)
{
    uint8_t i;
    int idx = meshFindNode(mac);
    if (idx >= 0) {
        return idx;
    }
    for (i = 0; i < MESH_MAX_NODES; i++) {
        if (!gNodes[i].used) {
            memset(&gNodes[i], 0, sizeof(gNodes[i]));
            memcpy(gNodes[i].mac, mac, 6);
            gNodes[i].used  = true;
            gNodes[i].alive = true;
            gNodes[i].lastSeenMs = millis();
            return (int)i;
        }
    }
    return -1;
}

static void onMeshSent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac; (void)status;
    /* Deliberately empty. ESP-NOW's send status only reports whether the MAC layer got an ack;
     * it does not mean the application received anything, and treating it as delivery is a
     * classic overread. Real reachability is measured by the ping round trip. */
}

static void onMeshRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    uint8_t head = gMeshRxHead;
    uint8_t next = (uint8_t)((head + 1u) % MESH_RX_QUEUE_SIZE);

    gMeshRx++;
    if (len <= 0 || len > (int)MESH_MAX_ESPNOW_PAYLOAD) {
        return;
    }

    /* A ping echo is answered by the measurement code in loop(), but its ARRIVAL TIME must be
     * captured here -- queueing it first would add loop latency to a number that is supposed to
     * be air time. So the stopwatch is read at the earliest possible instant. */
    if (data[0] == MESH_MSG_PONG && gPingActive) {
        MeshPingMsg pong;
        memcpy(&pong, data, (len < (int)sizeof(pong)) ? (size_t)len : sizeof(pong));
        if (pong.token == gPingToken) {
            uint32_t rtt = (uint32_t)esp_timer_get_time() - gPingSentUs;
            gPingRecv++;
            gPingSumUs += rtt;
            if (rtt < gPingMinUs) { gPingMinUs = rtt; }
            if (rtt > gPingMaxUs) { gPingMaxUs = rtt; }
            gPingActive = false;   /* loop() fires the next one */
        }
        return;
    }

    if (next == gMeshRxTail) {
        /* Full. Drop the NEWEST here rather than the oldest: unlike radio state events, sensor
         * samples are a stream, and dropping the newest keeps the queue temporally contiguous
         * so a gap in the node's sequence numbers reads as one clean loss rather than a shuffle. */
        gMeshRxDropped++;
        return;
    }
    memcpy((void *)gMeshRxQ[head].mac, info->src_addr, 6);
    gMeshRxQ[head].len = (uint8_t)len;
    memcpy((void *)gMeshRxQ[head].data, data, (size_t)len);
    gMeshRxHead = next;
}

static bool meshBegin(uint8_t channel, uint8_t mode)
{
    uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    esp_now_peer_info_t peer;

    gMeshMode = mode;

    if (mode == IOP_MESH_MODE_ONLY) {
        /* No AP, so we own the channel outright. */
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, false, 0);
        esp_wifi_set_promiscuous(true);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);
        gMeshChannel = channel;
    } else {
        /* Station mode stays as-is. If we are associated the AP dictates the channel and the
         * requested one is IGNORED -- reporting the channel we actually ended up on is the whole
         * point, because a node told the wrong number will transmit into silence. */
        uint8_t primary = channel;
        wifi_second_chan_t second;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0u) {
            gMeshChannel = primary;
        } else {
            gMeshChannel = channel;
        }
    }

    if (gMeshState == IOP_MESH_STATE_OFF) {
        if (esp_now_init() != ESP_OK) {
            gMeshState = IOP_MESH_STATE_ERROR;
            return false;
        }
        esp_now_register_send_cb(onMeshSent);
        esp_now_register_recv_cb(onMeshRecv);
    }

    /* A broadcast peer lets a node be heard before the hub knows its MAC -- otherwise discovery
     * is a chicken-and-egg problem. */
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, bcast, 6);
    peer.channel = gMeshChannel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    gMeshState = IOP_MESH_STATE_READY;
    return true;
}

static bool meshEnsurePeer(const uint8_t *mac)
{
    esp_now_peer_info_t peer;
    if (esp_now_is_peer_exist(mac)) {
        return true;
    }
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = gMeshChannel;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

/* meshPoll -- drain received packets, relay them to the brain, run the ping train, age out
 * silent nodes. Called every loop(); never blocks. */
static void meshPoll(void)
{
    uint32_t now = millis();
    uint8_t  i;

    /* ---- autostart, if the brain has not spoken ---- */
#if MESH_AUTOSTART_MS
    if (gMeshState == IOP_MESH_STATE_OFF && now >= MESH_AUTOSTART_MS) {
        DEBUG_PORT.printf("[mesh] no MESH_INIT from the brain after %lu ms -- autostarting on "
                          "channel %u so the fleet is usable standalone\n",
                          (unsigned long)MESH_AUTOSTART_MS, (unsigned)MESH_AUTOSTART_CHAN);
        meshBegin(MESH_AUTOSTART_CHAN, IOP_MESH_MODE_ONLY);
    }
#endif

    /* ---- relay one queued packet per pass ---- */
    if (gMeshRxTail != gMeshRxHead) {
        uint8_t  tail = gMeshRxTail;
        uint8_t  mac[6];
        uint8_t  buf[MESH_MAX_ESPNOW_PAYLOAD];
        uint8_t  len = gMeshRxQ[tail].len;
        int      idx;

        memcpy(mac, (const void *)gMeshRxQ[tail].mac, 6);
        memcpy(buf, (const void *)gMeshRxQ[tail].data, len);
        gMeshRxTail = (uint8_t)((tail + 1u) % MESH_RX_QUEUE_SIZE);

        idx = meshFindNode(mac);
        if (idx < 0) {
            idx = meshAddNode(mac);
            if (idx >= 0) {
                uint8_t ev[7];
                ev[0] = IOP_MESH_EVT_NODE_SEEN;
                memcpy(&ev[1], mac, 6);
                DEBUG_PORT.printf("[mesh] new node %02X:%02X:%02X:%02X:%02X:%02X\n",
                                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                sendEvent(IOP_CHAN_MESH, IOP_CMD_MESH_NODE_EVENT, ev, sizeof(ev));
                meshEnsurePeer(mac);
                /* First node ever seen starts the autostart-ping clock. Timed from discovery
                 * rather than from boot, because before this instant there was nothing to ping. */
                if (gFirstNodeMs == 0u) {
                    gFirstNodeMs    = now;
                    gNextAutoPingMs = now + MESH_PING_AUTOSTART_MS;
                }
            }
        }
        if (idx >= 0) {
            gNodes[idx].used = true;
            gNodes[idx].alive = true;
            gNodes[idx].lastSeenMs = now;
            gNodes[idx].packets++;

            /* Loss detection from the NODE's own sequence number. The hub cannot tell a dropped
             * packet from one that was never sent; the node's counter can. */
            if (len >= (int)offsetof(MeshSampleMsg, t_first_us) && buf[0] == MESH_MSG_SAMPLE) {
                MeshSampleMsg m;
                memcpy(&m, buf, (len < (int)sizeof(m)) ? (size_t)len : sizeof(m));
                if (gNodes[idx].seqValid) {
                    uint16_t expected = (uint16_t)(gNodes[idx].lastSeq + 1u);
                    if (m.seq != expected) {
                        gNodes[idx].lostPackets += (uint16_t)(m.seq - expected);
                    }
                }
                gNodes[idx].lastSeq  = m.seq;
                gNodes[idx].seqValid = true;
            }

            /* Relay to the brain -- but ONLY as much as the brain subscribed to.
             *
             * Everything above this point still runs at full rate: the node is marked alive, its
             * packet counted, its sequence number checked for loss. Only the forwarding is
             * gated. So `mesh status` reports exact totals no matter what the divisor is, and
             * the brain can watch a 200 Hz sensor at 10 Hz without losing the ability to say how
             * many packets were actually lost. Decimating measurement along with delivery would
             * be the easy mistake here.
             *
             * The payload itself is relayed VERBATIM, prefixed with the source MAC. The hub does
             * not parse, rescale or summarise -- it is a relay, and interpreting sensor data here
             * would put policy in the wrong place. */
            if (gMeshStream != IOP_MESH_STREAM_OFF) {
                gNodes[idx].relayCount++;
                if (gNodes[idx].relayCount >= gMeshStream) {
                    uint8_t out[6 + MESH_MAX_ESPNOW_PAYLOAD];
                    gNodes[idx].relayCount = 0;
                    memcpy(out, mac, 6);
                    memcpy(&out[6], buf, len);
                    sendEvent(IOP_CHAN_MESH, IOP_CMD_MESH_DATA, out, (uint16_t)(6u + len));
                    gMeshRelayed++;
                } else {
                    gMeshDecimated++;
                }
            } else {
                gMeshDecimated++;
            }
        }
    }

    /* ---- autostart the ping train once a node exists and has had time to settle ---- */
#if MESH_PING_AUTOSTART_MS
    if (gNextAutoPingMs != 0u && gPingRemaining == 0u && !gPingActive &&
        gPingSent == 0u && (int32_t)(now - gNextAutoPingMs) >= 0) {
        int idx = -1;
        uint8_t n;
        /* Ping the first node that is currently alive. A dead node would just measure the
         * timeout path 30 times and report 0/30, which is true but useless. */
        for (n = 0; n < MESH_MAX_NODES; n++) {
            if (gNodes[n].used && gNodes[n].alive) { idx = (int)n; break; }
        }
        if (idx >= 0) {
            memcpy(gPingMac, gNodes[idx].mac, 6);
            meshEnsurePeer(gPingMac);
            gPingRemaining = MESH_PING_AUTOSTART_COUNT;
            gPingSent = 0; gPingRecv = 0; gPingSumUs = 0;
            gPingMinUs = 0xFFFFFFFFu; gPingMaxUs = 0;
            gPingActive = false;
            gPingAuto   = true;
            DEBUG_PORT.printf("[mesh] autostart ping train: %u round trips to "
                              "%02X:%02X:%02X:%02X:%02X:%02X, timed on this board's clock\n",
                              (unsigned)MESH_PING_AUTOSTART_COUNT,
                              gPingMac[0], gPingMac[1], gPingMac[2],
                              gPingMac[3], gPingMac[4], gPingMac[5]);
        }
        /* Reschedule whether or not a node was available, so a node that appears later still
         * gets characterised instead of the autostart silently giving up forever. */
        gNextAutoPingMs = (MESH_PING_REPEAT_MS != 0u) ? (now + MESH_PING_REPEAT_MS) : 0u;
    }
#endif

    /* ---- ping train ---- */
    if (gPingRemaining > 0u && !gPingActive) {
        MeshPingMsg ping;
        gPingToken  = (uint32_t)esp_random();
        ping.type   = MESH_MSG_PING;
        ping.token  = gPingToken;
        gPingSentUs = (uint32_t)esp_timer_get_time();
        gPingActive = true;
        gPingDeadline = now + 200u;
        gPingSent++;
        gPingRemaining--;
        esp_now_send(gPingMac, (const uint8_t *)&ping, sizeof(ping));
    } else if (gPingActive && (int32_t)(now - gPingDeadline) >= 0) {
        gPingActive = false;    /* lost; the train continues so loss is measured, not fatal */
    }

    if (gPingRemaining == 0u && !gPingActive && gPingSent > 0u) {
        uint8_t resp[14];
        uint32_t mean = (gPingRecv > 0u) ? (gPingSumUs / gPingRecv) : 0u;
        uint32_t mn   = (gPingRecv > 0u) ? gPingMinUs : 0u;
        resp[0]  = (gPingRecv > 0u) ? IOP_MESH_STATUS_OK : IOP_MESH_STATUS_ERROR;
        resp[1]  = (uint8_t)(gPingSent >> 8);   resp[2]  = (uint8_t)gPingSent;
        resp[3]  = (uint8_t)(gPingRecv >> 8);   resp[4]  = (uint8_t)gPingRecv;
        resp[5]  = (uint8_t)(mn >> 24); resp[6]  = (uint8_t)(mn >> 16);
        resp[7]  = (uint8_t)(mn >> 8);  resp[8]  = (uint8_t)mn;
        resp[9]  = (uint8_t)(mean >> 16); resp[10] = (uint8_t)(mean >> 8); resp[11] = (uint8_t)mean;
        resp[12] = (uint8_t)(gPingMaxUs >> 8); resp[13] = (uint8_t)gPingMaxUs;
        DEBUG_PORT.printf("[mesh] ping done%s: %u/%u, min %luus mean %luus max %luus "
                          "(one way ~%luus)\n",
                          gPingAuto ? " (autostart)" : "",
                          (unsigned)gPingRecv, (unsigned)gPingSent,
                          (unsigned long)mn, (unsigned long)mean, (unsigned long)gPingMaxUs,
                          (unsigned long)(mean / 2u));
        /* An unrequested measurement is an EVENT; a requested one is a RESPONSE to that request's
         * SEQ. Sending an autostarted result as a response would hand the brain a reply with a
         * sequence number it never issued -- precisely the stale-response case SEQ exists to
         * catch, manufactured by us. */
        if (gPingAuto) {
            sendEvent(IOP_CHAN_MESH, IOP_CMD_MESH_PING_RESP, resp, sizeof(resp));
        } else {
            sendFrame(gPingSeq, IOP_CHAN_MESH, IOP_CMD_MESH_PING_RESP, resp, sizeof(resp));
        }
        gPingSent = 0;
        gPingAuto = false;
    }

    /* ---- liveness ---- */
    for (i = 0; i < MESH_MAX_NODES; i++) {
        if (gNodes[i].used && gNodes[i].alive &&
            (uint32_t)(now - gNodes[i].lastSeenMs) > MESH_NODE_TIMEOUT_MS) {
            uint8_t ev[7];
            gNodes[i].alive = false;
            ev[0] = IOP_MESH_EVT_NODE_LOST;
            memcpy(&ev[1], gNodes[i].mac, 6);
            DEBUG_PORT.printf("[mesh] lost node %02X:%02X:%02X:%02X:%02X:%02X\n",
                              gNodes[i].mac[0], gNodes[i].mac[1], gNodes[i].mac[2],
                              gNodes[i].mac[3], gNodes[i].mac[4], gNodes[i].mac[5]);
            sendEvent(IOP_CHAN_MESH, IOP_CMD_MESH_NODE_EVENT, ev, sizeof(ev));
        }
    }
}

/* handleMeshFrame -- commands from the brain on the MESH channel. */
static void handleMeshFrame(const IopFrame *f)
{
    uint8_t resp[16];

    switch (f->cmd) {
    case IOP_CMD_MESH_INIT_REQ: {
        uint8_t ch   = (f->payload_len > 0u) ? f->payload[0] : 1u;
        uint8_t mode = (f->payload_len > 1u) ? f->payload[1] : IOP_MESH_MODE_ONLY;
        bool ok = meshBegin(ch, mode);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        resp[0] = ok ? IOP_MESH_STATE_READY : IOP_MESH_STATE_ERROR;
        resp[1] = gMeshChannel;      /* the channel we ACTUALLY got, not the one requested */
        memcpy(&resp[2], mac, 6);
        DEBUG_PORT.printf("[mesh] init mode=%u requested ch=%u, actual ch=%u -> %s\n",
                          (unsigned)mode, (unsigned)ch, (unsigned)gMeshChannel,
                          ok ? "ready" : "FAILED");
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_INIT_RESP, resp, 8);
        break;
    }

    case IOP_CMD_MESH_PEER_REQ: {
        uint8_t count = 0, i;
        if (f->payload_len < 7u) {
            sendNack(f->seq, IOP_CHAN_MESH, f->cmd, IOP_ERR_BAD_PAYLOAD);
            return;
        }
        if (f->payload[0] == IOP_MESH_PEER_ADD) {
            meshEnsurePeer(&f->payload[1]);
            meshAddNode(&f->payload[1]);
        } else {
            esp_now_del_peer(&f->payload[1]);
            int idx = meshFindNode(&f->payload[1]);
            if (idx >= 0) { gNodes[idx].used = false; }
        }
        for (i = 0; i < MESH_MAX_NODES; i++) { if (gNodes[i].used) { count++; } }
        resp[0] = IOP_MESH_STATUS_OK;
        resp[1] = count;
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_PEER_RESP, resp, 2);
        break;
    }

    case IOP_CMD_MESH_SEND_REQ: {
        if (f->payload_len < 7u) {
            sendNack(f->seq, IOP_CHAN_MESH, f->cmd, IOP_ERR_BAD_PAYLOAD);
            return;
        }
        meshEnsurePeer(f->payload);
        esp_err_t r = esp_now_send(f->payload, &f->payload[6], f->payload_len - 6u);
        resp[0] = (r == ESP_OK) ? IOP_MESH_STATUS_OK : IOP_MESH_STATUS_ERROR;
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_SEND_RESP, resp, 1);
        break;
    }

    case IOP_CMD_MESH_PING_REQ: {
        if (f->payload_len < 7u) {
            sendNack(f->seq, IOP_CHAN_MESH, f->cmd, IOP_ERR_BAD_PAYLOAD);
            return;
        }
        memcpy(gPingMac, f->payload, 6);
        meshEnsurePeer(gPingMac);
        gPingRemaining = f->payload[6];
        if (gPingRemaining == 0u) { gPingRemaining = 20u; }
        gPingSent = 0; gPingRecv = 0; gPingSumUs = 0;
        gPingMinUs = 0xFFFFFFFFu; gPingMaxUs = 0;
        gPingActive = false;
        gPingAuto   = false;   /* the brain asked, so the result is a RESPONSE to this SEQ */
        gPingSeq = f->seq;
        DEBUG_PORT.printf("[mesh] ping train of %u starting\n", (unsigned)gPingRemaining);
        break;
    }

    case IOP_CMD_MESH_STATUS_REQ: {
        uint8_t i, peers = 0, alive = 0;
        for (i = 0; i < MESH_MAX_NODES; i++) {
            if (gNodes[i].used) { peers++; if (gNodes[i].alive) { alive++; } }
        }
        resp[0] = gMeshState;
        resp[1] = gMeshChannel;
        resp[2] = gMeshMode;
        resp[3] = peers;
        resp[4] = alive;
        resp[5] = (uint8_t)(gMeshRx >> 24); resp[6] = (uint8_t)(gMeshRx >> 16);
        resp[7] = (uint8_t)(gMeshRx >> 8);  resp[8] = (uint8_t)gMeshRx;
        resp[9] = (gMeshRxDropped > 255u) ? 255u : (uint8_t)gMeshRxDropped;
        /* Byte 10 is APPENDED, not inserted: a brain built against the 10-byte version bounds its
         * walk with payload_len and simply never looks here. Growing a response at the tail is
         * the one shape of protocol change that needs no version negotiation. */
        resp[10] = gMeshStream;
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_STATUS_RESP, resp, 11);
        break;
    }

    /* MESH_NODES_REQ -- hand the brain the fleet roster.
     *
     * WHY THIS IS NEEDED even though NODE_SEEN events already announce every node. Events only
     * reach a brain that was LISTENING when they fired. A Teensy that reboots, or that is flashed
     * while the hub keeps running, has missed every announcement and has no way to learn a single
     * MAC -- which is exactly what happened here: 'mesh ping' failed with "no live node known"
     * while the hub was happily tracking a live node.
     *
     * This is the same poll-to-re-anchor pattern as WIFI_STATUS_REQ. Events keep a listening
     * brain current cheaply; a poll lets a brain that missed them recover the truth. A system
     * with only one of the two is broken for whoever arrives late. */
    case IOP_CMD_MESH_NODES_REQ: {
        /* 1 + 8 x 11 = 89 bytes, far past the 16-byte scratch buffer the other cases share. */
        uint8_t list[1u + (MESH_MAX_NODES * 11u)];
        uint16_t off = 1u;
        uint8_t i, count = 0;
        for (i = 0; i < MESH_MAX_NODES; i++) {
            if (!gNodes[i].used) { continue; }
            memcpy(&list[off], gNodes[i].mac, 6);
            list[off + 6] = gNodes[i].alive ? 1u : 0u;
            list[off + 7]  = (uint8_t)(gNodes[i].packets >> 24);
            list[off + 8]  = (uint8_t)(gNodes[i].packets >> 16);
            list[off + 9]  = (uint8_t)(gNodes[i].packets >> 8);
            list[off + 10] = (uint8_t)gNodes[i].packets;
            off = (uint16_t)(off + 11u);
            count++;
        }
        list[0] = count;
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_NODES_RESP, list, off);
        break;
    }

    /* MESH_STREAM_REQ -- the brain subscribing to (or unsubscribing from) the node firehose.
     * See IOP_MESH_STREAM_OFF in interop_protocol.h for why this command exists at all. */
    case IOP_CMD_MESH_STREAM_REQ: {
        if (f->payload_len < 1u) {
            sendNack(f->seq, IOP_CHAN_MESH, f->cmd, IOP_ERR_BAD_PAYLOAD);
            return;
        }
        gMeshStream = f->payload[0];
        /* Reset the per-node phase so a divisor change takes effect immediately and identically
         * on every node, rather than each one carrying over a partial count from the old rate. */
        {
            uint8_t i;
            for (i = 0; i < MESH_MAX_NODES; i++) { gNodes[i].relayCount = 0; }
        }
        if (gMeshStream == IOP_MESH_STREAM_OFF) {
            DEBUG_PORT.printf("[mesh] stream OFF -- still counting every packet, forwarding none\n");
        } else if (gMeshStream == IOP_MESH_STREAM_ALL) {
            DEBUG_PORT.printf("[mesh] stream ALL -- forwarding every packet\n");
        } else {
            DEBUG_PORT.printf("[mesh] stream 1-in-%u\n", (unsigned)gMeshStream);
        }
        resp[0] = IOP_MESH_STATUS_OK;
        resp[1] = gMeshStream;
        sendFrame(f->seq, IOP_CHAN_MESH, IOP_CMD_MESH_STREAM_RESP, resp, 2);
        break;
    }

    default:
        sendErrorNack(f->seq, IOP_CHAN_MESH, f->cmd, IOP_ERR_UNKNOWN_CMD);
        break;
    }
}

/* ===========================================================================================
 * SETUP
 * =========================================================================================== */
void setup(void)
{
    uint16_t selftest;

    DEBUG_PORT.begin(DEBUG_BAUD);

    /* MAKE DEBUG OUTPUT NON-BLOCKING. This is the single most important line in setup().
     *
     * With USB CDC on boot, DEBUG_PORT is HWCDC, whose write() takes a lock with a 100 ms
     * timeout and then loops on the TX ring buffer, resetting that timeout on any progress. A
     * host that is connected but not draining -- a serial monitor scrolled up, minimised, or
     * simply slow -- blocks a single printf for at least 100 ms and potentially indefinitely.
     *
     * The receive ring is 2048 bytes, which at 921600 baud is 22.2 ms of data. So one stalled
     * debug line can overrun the buffer several times over, and the debug output is densest
     * exactly when something interesting is happening. The diagnostics would destroy what they
     * are diagnosing.
     *
     * A zero timeout makes writes drop instead of stall. Losing a log line is a fair trade for
     * never losing a frame. */
#if ARDUINO_USB_CDC_ON_BOOT
    DEBUG_PORT.setTxTimeoutMs(0);
#endif

    /* A short, BOUNDED wait for the USB CDC port to enumerate.
     *
     * On an S3 with "USB CDC On Boot" enabled, DEBUG_PORT is the native USB peripheral, and
     * output written before the host enumerates it is simply lost -- which is why the boot
     * banner sometimes seems to vanish. A brief wait catches it. It is bounded because this
     * board must also work headless on a power supply, where the port never enumerates and an
     * unbounded "while (!Serial)" would hang forever. */
    {
        uint32_t t0 = millis();
        while (!DEBUG_PORT && (millis() - t0) < 1500u) {
            /* spin */
        }
    }

    DEBUG_PORT.println();
    DEBUG_PORT.println("===========================================================");
    DEBUG_PORT.println(" debug output is going to: " DEBUG_PORT_NAME);
    DEBUG_PORT.println(" ESP32-S3 Wireless Bridge  --  interop SLAVE");
    DEBUG_PORT.printf(  " protocol v%u.%u, frame max %u bytes, CRC-16 check 0x%04X\n",
                        (unsigned)IOP_PROTOCOL_VERSION, (unsigned)IOP_PROTOCOL_MINOR,
                        (unsigned)IOP_MAX_FRAME_SIZE,
                        (unsigned)IOP_CRC16_CHECK_VALUE);
    DEBUG_PORT.println("===========================================================");

    /* Validate the protocol implementation before trusting it with traffic. If the two boards
     * print different CRC check values in their banners, they are running different headers
     * and nothing else will work -- that comparison is the fastest diagnosis available. */
    selftest = iop_selftest(&gParser);
    if (selftest == 0u) {
        DEBUG_PORT.println("[selftest] protocol self-test PASSED");
    } else {
        uint16_t bit;
        DEBUG_PORT.printf("[selftest] FAILED, mask=0x%04X\n", (unsigned)selftest);
        for (bit = 1u; bit != 0u; bit = (uint16_t)(bit << 1)) {
            if (selftest & bit) {
                DEBUG_PORT.printf("           -> %s\n", iop_selftest_name(bit));
            }
        }
        DEBUG_PORT.println("           Fix interop_protocol.h before debugging anything else.");
    }

    /* Open the link. Buffer size first, then begin() with explicit pins. */
    LINK_PORT.setRxBufferSize(LINK_RX_BUFFER);
    LINK_PORT.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
    iop_parser_init(&gParser);
    iop_nack_limiter_init(&gNackLimiter);

    DEBUG_PORT.printf("[link] Serial1 at %lu baud, RX=GPIO%d, TX=GPIO%d, RX buffer %u bytes\n",
                      (unsigned long)LINK_BAUD, (int)LINK_RX_PIN, (int)LINK_TX_PIN,
                      (unsigned)LINK_RX_BUFFER);

    /* Bring the radio up in station mode but do NOT join anything yet.
     *
     * WiFi.mode(WIFI_STA) starts the WiFi task and initialises the driver, which takes a few
     * hundred milliseconds. Doing it now, at boot, means the first scan the Teensy asks for is
     * not also paying that cost -- which would otherwise look like "the first scan is always
     * slow and sometimes times out". */
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true, WIFI_DISCONNECT_NONBLOCKING_TIMEOUT_MS);   /* clear any stored credentials from a previous session,
                                     * so a connect attempt reflects what we were just told,
                                     * not what happened to be in NVS */
    WiFi.onEvent(onWiFiEvent);

    DEBUG_PORT.println("[wifi] station mode ready (not connected)");

    setStatusLed(0, 40, 0);         /* green: idle and ready */
    DEBUG_PORT.println("ESP32-S3 Wireless Bridge ready");

    /* Tell the brain the radio is alive BEFORE waiting to be spoken to. If the Teensy has been
     * running all along and this board just reset, this frame is the only thing that will tell
     * it so. */
    sendRadioReady();

    DEBUG_PORT.println("Waiting for commands from the Teensy...");
    gLastHeartbeat = millis();
}

/* ===========================================================================================
 * MAIN LOOP
 * ===========================================================================================
 * Four non-blocking pumps. Every one of them returns promptly, every time. There is no delay()
 * anywhere in this sketch.
 */
void loop(void)
{
    uint32_t now = millis();

    linkPoll();      /* bytes from the Teensy -> parser -> handlers                        */
    scanPoll();      /* has an in-flight scan finished?                                     */
    connectPoll();   /* has an in-flight join resolved?                                     */
    eventPoll();     /* radio news the brain has not asked for, but needs                   */
    meshPoll();      /* ESP-NOW: relay node packets, run ping trains, age out silent nodes   */

    /* Heartbeat, so an idle board is visibly distinguishable from a crashed one. */
    if ((uint32_t)(now - gLastHeartbeat) >= HEARTBEAT_MS) {
        gLastHeartbeat = now;
        DEBUG_PORT.printf("[idle] up %lus, frames=%lu nacks=%lu scans=%lu, "
                          "parser ok=%lu badcrc=%lu discarded=%lu\n",
                          (unsigned long)(now / 1000u),
                          (unsigned long)gStats.framesHandled,
                          (unsigned long)gStats.nacksSent,
                          (unsigned long)gStats.scansDone,
                          (unsigned long)gParser.stat_frames_ok,
                          (unsigned long)gParser.stat_bad_crc,
                          (unsigned long)gParser.stat_resync_bytes,
                          (unsigned long)gNackLimiter.suppressed);
    if (gMeshState != IOP_MESH_STATE_OFF) {
        uint8_t i, peers = 0, alive = 0;
        for (i = 0; i < MESH_MAX_NODES; i++) {
            if (gNodes[i].used) { peers++; if (gNodes[i].alive) { alive++; } }
        }
        /* rx counts what the air delivered; relayed/held show what the SUBSCRIPTION let through.
         * Printing both makes the divisor's effect visible at a glance and keeps it obvious that
         * a quiet UART is a policy choice, not lost data. */
        DEBUG_PORT.printf("[mesh] ch=%u mode=%u nodes=%u/%u rx=%lu dropped=%lu "
                          "stream=%u relayed=%lu held=%lu\n",
                          (unsigned)gMeshChannel, (unsigned)gMeshMode,
                          (unsigned)alive, (unsigned)peers,
                          (unsigned long)gMeshRx, (unsigned long)gMeshRxDropped,
                          (unsigned)gMeshStream,
                          (unsigned long)gMeshRelayed, (unsigned long)gMeshDecimated);
        for (i = 0; i < MESH_MAX_NODES; i++) {
            if (gNodes[i].used) {
                DEBUG_PORT.printf("       %02X:%02X:%02X:%02X:%02X:%02X  pkts=%lu lost=%lu %s\n",
                                  gNodes[i].mac[0], gNodes[i].mac[1], gNodes[i].mac[2],
                                  gNodes[i].mac[3], gNodes[i].mac[4], gNodes[i].mac[5],
                                  (unsigned long)gNodes[i].packets,
                                  (unsigned long)gNodes[i].lostPackets,
                                  gNodes[i].alive ? "alive" : "LOST");
            }
        }
    }
    if (gEventsDropped > 0u) {
        DEBUG_PORT.printf("[idle] WARNING: %lu radio events were dropped from the queue\n",
                          (unsigned long)gEventsDropped);
    }
    }
}

/* ===========================================================================================
 * LINK
 * =========================================================================================== */

/* linkPoll -- drain the UART and drive the parser. Identical in structure to the Teensy side;
 * both boards parse frames with exactly the same code, which is the point of the shared
 * header. */
static void linkPoll(void)
{
    IopFrame frame;

    while (LINK_PORT.available() > 0) {
        uint8_t  b  = (uint8_t)LINK_PORT.read();
        IopEvent ev = iop_parser_push(&gParser, b, millis(), &frame);

        switch (ev) {
        case IOP_EV_FRAME:
            handleFrame(&frame);
            break;

        case IOP_EV_BAD_CRC:
            /* Frame arrived complete but corrupted. Report BOTH CRC values: if they differ
             * randomly it is noise on the wire; if they differ the same way every time, the
             * two boards are running different CRC parameters. */
            DEBUG_PORT.printf("[rx] BAD CRC: received 0x%04X, computed 0x%04X, seq=%u, len=%u"
                              " -- frame DROPPED\n",
                              (unsigned)frame.crc_received, (unsigned)frame.crc_computed,
                              (unsigned)frame.seq, (unsigned)frame.declared_len);
            sendErrorNack(frame.seq, frame.chan, frame.cmd, IOP_ERR_BAD_CRC);
            break;

        case IOP_EV_OVERSIZE:
            DEBUG_PORT.printf("[rx] OVERSIZE: frame declared %u body bytes, buffer holds %u"
                              " -- discarded cleanly\n",
                              (unsigned)frame.declared_len, (unsigned)IOP_MAX_BODY_LEN);
            sendErrorNack(frame.seq, IOP_CHAN_TRANSPORT, IOP_CMD_NONE, IOP_ERR_PAYLOAD_LARGE);
            break;

        case IOP_EV_BAD_LENGTH:
            DEBUG_PORT.println("[rx] frame declared length 0 (impossible) -- discarded");
            break;

        case IOP_EV_TIMEOUT:
            DEBUG_PORT.println("[rx] partial frame timed out mid-stream");
            break;

        case IOP_EV_RESYNC_DISCARD:
            /* Silent by design. One log line per discarded byte would bury the useful output
             * under a wall of noise at exactly the wrong moment. The running total is printed
             * with the heartbeat. */
            break;

        default:
            break;
        }
    }

    /* THE TIMEOUT CHECK GOES HERE, AFTER THE DRAIN, AND ONLY WHEN THE QUEUE IS EMPTY.
     *
     * Checking it first would let a slow loop() impersonate a dead sender: bytes that arrived
     * on time and are waiting in the UART ring would be discarded because WE were late reading
     * them. On this board that is not hypothetical -- the WiFi driver and the USB CDC console
     * can both stall loop() well past 50 ms. Data still queued proves the peer is alive; only
     * silence justifies a timeout. See iop_parser_tick() in the shared header. */
    if (LINK_PORT.available() == 0) {
        /* tick() can now report the SPECIFIC reason a condemned frame died, not just that it
         * aged out -- so a header that declared an impossible length still produces the right
         * NACK even though its body never arrived. */
        switch (iop_parser_tick(&gParser, millis(), &frame)) {
        case IOP_EV_OVERSIZE:
            DEBUG_PORT.printf("[rx] OVERSIZE: frame declared %u body bytes and then stopped "
                              "sending; buffer holds %u\n",
                              (unsigned)frame.declared_len, (unsigned)IOP_MAX_BODY_LEN);
            sendErrorNack(frame.seq, IOP_CHAN_TRANSPORT, IOP_CMD_NONE, IOP_ERR_PAYLOAD_LARGE);
            break;
        case IOP_EV_BAD_LENGTH:
            DEBUG_PORT.println("[rx] frame declared length 0 and then stopped -- discarded");
            break;
        case IOP_EV_TIMEOUT:
            DEBUG_PORT.println("[rx] partial frame timed out and was discarded");
            break;
        default:
            break;
        }
    }
}

/* handleFrame -- dispatch a validated frame.
 *
 * By the time execution reaches here the frame has passed its CRC, so its contents are exactly
 * what the Teensy sent. That is the ONLY thing the CRC guarantees; whether the payload makes
 * structural sense is checked separately, inside each handler. */
static void handleFrame(const IopFrame *f)
{
    gStats.framesHandled++;

    if (gHexDump) {
        DEBUG_PORT.printf("[rx] %s/%s seq=%u payload=%u bytes\n",
                          iop_chan_name(f->chan), iop_cmd_name(f->cmd),
                          (unsigned)f->seq, (unsigned)f->payload_len);
        hexDump("     ", f->payload, f->payload_len);
    }

    /* ROUTE ON CHANNEL FIRST. This is the whole point of v2: a channel we do not implement is
     * refused as a channel, not mistaken for an unknown command, and a future radio adds a
     * channel without renumbering anything that already exists. */
    if (f->chan == IOP_CHAN_MESH) {
        handleMeshFrame(f);
        return;
    }
    if (f->chan != IOP_CHAN_TRANSPORT && f->chan != IOP_CHAN_WIFI) {
        DEBUG_PORT.printf("[rx] channel %s (0x%02X) is not implemented on this radio\n",
                          iop_chan_name(f->chan), (unsigned)f->chan);
        sendErrorNack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CHAN);
        return;
    }

    switch (f->cmd) {
    case IOP_CMD_PING:
        handlePing(f);
        break;

    case IOP_CMD_WIFI_SCAN_REQ:
        handleScanRequest(f);
        break;

    case IOP_CMD_WIFI_CONNECT_REQ:
        handleConnectRequest(f);
        break;

    case IOP_CMD_WIFI_STATUS_REQ:
        handleStatusRequest(f);
        break;

    case IOP_CMD_NACK:
        /* The Teensy is complaining about something we sent. Log it -- this is how a
         * one-directional fault (our TX is fine, our RX is not, or vice versa) becomes
         * visible from this end. */
        if (f->payload_len >= 3u) {
            DEBUG_PORT.printf("[rx] Teensy NACKed our %s/%s: %s (0x%02X)\n",
                              iop_chan_name(f->payload[0]), iop_cmd_name(f->payload[1]),
                              iop_err_name(f->payload[2]), (unsigned)f->payload[2]);
        } else {
            DEBUG_PORT.println("[rx] malformed NACK from the Teensy");
        }
        break;

    default:
        DEBUG_PORT.printf("[rx] unknown command 0x%02X (seq=%u) -- replying NACK\n",
                          (unsigned)f->cmd, (unsigned)f->seq);
        sendErrorNack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
        break;
    }
}

/* handlePing -- answer immediately with PONG, echoing the SEQ.
 *
 * NOTE THAT PING IS ANSWERED EVEN WHILE BUSY, deliberately. PING is the link health check; a
 * health check that fails whenever the device is working would report "dead" for the entire
 * duration of every scan, which is worse than useless. It is answered from the same loop that
 * is polling the scan, costs microseconds, and touches no shared state. */
static void handlePing(const IopFrame *f)
{
    DEBUG_PORT.printf("[cmd] PING seq=%u -> PONG\n", (unsigned)f->seq);
    sendFrame(f->seq, IOP_CHAN_TRANSPORT, IOP_CMD_PONG, NULL, 0);
}

/* handleScanRequest -- start an ASYNCHRONOUS scan and return immediately. */
static void handleScanRequest(const IopFrame *f)
{
    int16_t started;

    if (gState != BRIDGE_IDLE) {
        DEBUG_PORT.printf("[cmd] WIFI_SCAN_REQ seq=%u refused: busy with %s\n",
                          (unsigned)f->seq,
                          (gState == BRIDGE_SCANNING) ? "a scan" : "a connect");
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, IOP_ERR_BUSY);
        return;
    }

    /* Free the previous result set before starting another. Without this, every scan leaks the
     * memory holding the last one -- a slow leak that eventually starves the WiFi stack on a
     * board meant to run for weeks. */
    WiFi.scanDelete();

    /* async=true returns immediately; show_hidden=true so that networks not broadcasting an
     * SSID still appear (they arrive with a zero-length name, which the protocol represents
     * honestly as SSID_LEN=0 rather than inventing a placeholder). */
    started = WiFi.scanNetworks(true, true);

    if (started == WIFI_SCAN_FAILED) {
        DEBUG_PORT.println("[scan] the radio refused to start a scan");
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, IOP_ERR_TIMEOUT);
        return;
    }

    gState             = BRIDGE_SCANNING;
    gOperationSeq      = f->seq;
    gOperationDeadline = millis() + IOP_ESP_SCAN_DEADLINE_MS;
    setStatusLed(0, 0, 40);          /* blue: working */
    DEBUG_PORT.printf("[scan] started (async), seq=%u, deadline %lu ms\n",
                      (unsigned)f->seq, (unsigned long)IOP_ESP_SCAN_DEADLINE_MS);
}

/* scanPoll -- has the scan finished? Called every loop() while one is running. */
static void scanPoll(void)
{
    int16_t result;

    if (gState != BRIDGE_SCANNING) {
        return;
    }

    result = WiFi.scanComplete();

    if (result == WIFI_SCAN_RUNNING) {
        /* Still working. Enforce our own deadline: the master is waiting with a timeout that
         * is deliberately longer than this one, so answering late is still better than not
         * answering -- but only if we actually answer. */
        if ((int32_t)(millis() - gOperationDeadline) >= 0) {
            DEBUG_PORT.println("[scan] deadline exceeded -- abandoning and reporting timeout");
            WiFi.scanDelete();
            gState = BRIDGE_IDLE;
            setStatusLed(40, 0, 0);   /* red: something went wrong */
            sendNack(gOperationSeq, IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, IOP_ERR_TIMEOUT);
        }
        return;
    }

    if (result == WIFI_SCAN_FAILED) {
        DEBUG_PORT.println("[scan] the driver reported failure");
        gState = BRIDGE_IDLE;
        setStatusLed(40, 0, 0);
        sendNack(gOperationSeq, IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, IOP_ERR_TIMEOUT);
        return;
    }

    /* result >= 0: that many networks were found. Zero is a perfectly valid answer and is
     * reported as an empty list, not as an error -- "I looked and heard nothing" is different
     * from "I could not look". */
    DEBUG_PORT.printf("[scan] complete: %d network(s)\n", (int)result);
    gStats.scansDone++;
    sendScanResults(result);
    WiFi.scanDelete();
    gState = BRIDGE_IDLE;
    setStatusLed(0, 40, 0);
}

/* sendScanResults -- serialise the scan into a WIFI_SCAN_RESP payload.
 *
 * Reads the raw wifi_ap_record_t structures rather than WiFi.SSID(i).
 *
 * WHY: WiFi.SSID(i) returns an Arduino String. Calling it in a loop over 30 networks performs
 * 30 heap allocations and frees, which is exactly the fragmentation pattern this project
 * forbids. WiFi.getScanInfoByIndex(i) hands back a pointer to the driver's own record --
 * ssid[33], rssi, primary (channel) and authmode -- with no allocation whatsoever. Same data,
 * no heap. This is the String-free path, and it is public API.
 */
static void sendScanResults(int16_t count)
{
    uint16_t offset = 1;             /* payload[0] is the count, filled in at the end */
    uint8_t  written = 0;
    int16_t  i;

    if (count < 0) {
        count = 0;
    }

    for (i = 0; i < count; i++) {
        const wifi_ap_record_t *rec =
            (const wifi_ap_record_t *)WiFi.getScanInfoByIndex((int)i);
        uint8_t  ssidLen;
        uint16_t needed;

        if (rec == NULL) {
            continue;                /* index vanished; skip rather than dereference null */
        }

        /* The driver null-terminates ssid[33], but bound the scan anyway: trusting a length
         * that came from a radio is how buffer overruns happen. */
        ssidLen = (uint8_t)strnlen((const char *)rec->ssid, IOP_SSID_MAX_LEN);

        needed = (uint16_t)(IOP_SCAN_REC_FIXED + ssidLen);
        if ((uint16_t)(offset + needed) > IOP_MAX_PAYLOAD_LEN || written == 0xFFu) {
            /* TRUNCATION, ANNOUNCED. The count byte will report what we actually sent, so the
             * Teensy never parses past real data -- but silently dropping networks would make
             * a crowded band look empty. Say it out loud instead. */
            DEBUG_PORT.printf("[scan] TRUNCATED: sending %u of %d networks; the rest do not "
                              "fit in a %u-byte frame\n",
                              (unsigned)written, (int)count, (unsigned)IOP_MAX_FRAME_SIZE);
            break;
        }

        gPayloadBuf[offset++] = (uint8_t)rec->rssi;      /* int8 -> raw byte, sign preserved */
        gPayloadBuf[offset++] = rec->primary;            /* channel */
        gPayloadBuf[offset++] = mapEncryption((int)rec->authmode);
        gPayloadBuf[offset++] = ssidLen;
        if (ssidLen > 0u) {
            memcpy(&gPayloadBuf[offset], rec->ssid, ssidLen);
            offset = (uint16_t)(offset + ssidLen);
        }
        written++;
    }

    gPayloadBuf[0] = written;

    /* v1.1: append how many networks were actually SEEN, which may exceed how many were SENT.
     * Older brains stop after `written` records and ignore this trailing byte, so it is
     * backward compatible. Without it, a truncated list is indistinguishable from a complete
     * one and a brain ranking APs by signal is silently ranking a subset. */
    if (offset < IOP_MAX_PAYLOAD_LEN) {
        gPayloadBuf[offset++] = (count > 255) ? 255u : (uint8_t)count;
    }

    if (written < (uint8_t)count) {
        DEBUG_PORT.printf("[scan] reporting %u of %d networks (frame full)\n",
                          (unsigned)written, (int)count);
        /* v2: say so ON THE WIRE as well, via the frame flag. The trailing TOTAL_FOUND byte
         * carries the number; this flag means a receiver can notice the truncation without
         * having to parse to the end of the payload first. */
        {
            uint16_t n = iop_build_frame(gTxBuf, sizeof(gTxBuf), gOperationSeq,
                                         IOP_FLAG_TRUNCATED, IOP_CHAN_WIFI,
                                         IOP_CMD_WIFI_SCAN_RESP, gPayloadBuf, offset);
            if (n > 0u) {
                LINK_PORT.write(gTxBuf, n);
                LINK_PORT.flush();
            }
            return;
        }
    }
    sendFrame(gOperationSeq, IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_RESP, gPayloadBuf, offset);
}

/* currentWifiState -- collapse the radio's internal state into the single wire enum.
 *
 * Deliberately derived fresh from the driver each time rather than cached: a cached copy is one
 * more thing that can be wrong, and the whole point of this command is to be the authority the
 * brain falls back on when its event-driven model has drifted. */
static uint8_t currentWifiState(void)
{
    if (gState == BRIDGE_SCANNING) {
        return IOP_WIFI_STATE_SCANNING;
    }
    if (gState == BRIDGE_CONNECTING) {
        return IOP_WIFI_STATE_CONNECTING;
    }
    if (WiFi.status() == WL_CONNECTED) {
        /* An all-zero address means associated but DHCP has not finished. */
        return (WiFi.localIP()[0] != 0) ? IOP_WIFI_STATE_GOT_IP : IOP_WIFI_STATE_ASSOCIATED;
    }
    return IOP_WIFI_STATE_IDLE;
}

/* handleStatusRequest -- answer with a snapshot of everything the brain might have missed.
 *
 * Answered even while BUSY, like PING. A status poll performs no radio work at all -- it reads
 * variables already in RAM -- and refusing it during a scan would deny the brain information at
 * exactly the moment it is most likely to want it. */
static void handleStatusRequest(const IopFrame *f)
{
    uint8_t payload[12];
    uint8_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = 0;
    }

    payload[0] = currentWifiState();
    payload[1] = gLastDisconnectReason;
    payload[2] = gLastEventType;
    payload[3] = (WiFi.status() == WL_CONNECTED) ? (uint8_t)(int8_t)WiFi.RSSI() : 0u;

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        payload[4] = ip[0];
        payload[5] = ip[1];
        payload[6] = ip[2];
        payload[7] = ip[3];
    }

    /* Saturate rather than wrap: "255 or more were dropped" is honest, whereas a wrapped count
     * could read as zero and hide the very problem this byte exists to report. */
    payload[8]  = (gEventsDropped > 255u) ? 255u : (uint8_t)gEventsDropped;
    payload[9]  = 0;
    payload[10] = IOP_CAP_WIFI;
    payload[11] = (uint8_t)(((gState == BRIDGE_SCANNING) ? 0x01u : 0x00u) |
                            ((gState != BRIDGE_IDLE)     ? 0x02u : 0x00u));

    DEBUG_PORT.printf("[cmd] WIFI_STATUS_REQ seq=%u -> state=%s, dropped=%u\n",
                      (unsigned)f->seq, iop_wifi_state_name(payload[0]),
                      (unsigned)payload[8]);
    sendFrame(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_STATUS_RESP, payload, sizeof(payload));
}

/* mapEncryption -- translate the ESP-IDF auth mode into our wire code.
 *
 * WHY A TRANSLATION AT ALL, rather than sending the driver's number directly: the wire format
 * must not depend on the internals of one vendor's SDK. If Espressif renumbers wifi_auth_mode_t
 * in a future IDF -- and enum values HAVE moved between IDF versions -- a pass-through would
 * silently change the meaning of the protocol for every receiver in the field. An explicit
 * mapping is one switch statement, and it makes the wire format ours.
 *
 * Values verified against esp_wifi_types.h in the installed core (IDF v5.1). */
static uint8_t mapEncryption(int authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:            return IOP_ENC_OPEN;
        case WIFI_AUTH_WEP:             return IOP_ENC_WEP;
        case WIFI_AUTH_WPA_PSK:         return IOP_ENC_WPA;
        case WIFI_AUTH_WPA2_PSK:        return IOP_ENC_WPA2;
        case WIFI_AUTH_WPA_WPA2_PSK:    return IOP_ENC_WPA_WPA2;
        case WIFI_AUTH_WPA3_PSK:        return IOP_ENC_WPA3;
        case WIFI_AUTH_WPA2_WPA3_PSK:   return IOP_ENC_WPA2_WPA3;
        case WIFI_AUTH_ENTERPRISE:      return IOP_ENC_ENTERPRISE;
        default:                        return IOP_ENC_UNKNOWN;
    }
}

/* handleConnectRequest -- validate the payload, then start an asynchronous join. */
static void handleConnectRequest(const IopFrame *f)
{
    uint16_t offset = 0;
    uint8_t  ssidLen;
    uint8_t  passLen;

    if (gState != BRIDGE_IDLE) {
        DEBUG_PORT.printf("[cmd] WIFI_CONNECT_REQ seq=%u refused: busy\n", (unsigned)f->seq);
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_REQ, IOP_ERR_BUSY);
        return;
    }

    /* STRUCTURAL VALIDATION. The CRC proved these bytes are what the Teensy sent; it proved
     * nothing about whether they form a well-formed request. Every length is checked against
     * the bytes actually present BEFORE it is used to index anything. */
    if (f->payload_len < 2u) {
        DEBUG_PORT.println("[cmd] WIFI_CONNECT_REQ payload too short for even two lengths");
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_REQ, IOP_ERR_BAD_PAYLOAD);
        return;
    }
    ssidLen = f->payload[offset++];
    if (ssidLen == 0u || ssidLen > IOP_SSID_MAX_LEN ||
        (uint16_t)(offset + ssidLen + 1u) > f->payload_len) {
        DEBUG_PORT.printf("[cmd] WIFI_CONNECT_REQ has an invalid SSID length (%u)\n",
                          (unsigned)ssidLen);
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_REQ, IOP_ERR_BAD_PAYLOAD);
        return;
    }
    memcpy(gSsid, &f->payload[offset], ssidLen);
    gSsid[ssidLen] = '\0';
    offset = (uint16_t)(offset + ssidLen);

    passLen = f->payload[offset++];
    if (passLen > IOP_PASS_MAX_LEN || (uint16_t)(offset + passLen) > f->payload_len) {
        DEBUG_PORT.printf("[cmd] WIFI_CONNECT_REQ has an invalid password length (%u)\n",
                          (unsigned)passLen);
        sendNack(f->seq, IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_REQ, IOP_ERR_BAD_PAYLOAD);
        return;
    }
    memcpy(gPassword, &f->payload[offset], passLen);
    gPassword[passLen] = '\0';

    /* Reset the event-driven failure state before starting, so we cannot possibly report the
     * reason from a PREVIOUS attempt. */
    gLastDisconnectReason = 0;
    gDisconnectSeen       = false;
    gNoApFoundCount       = 0;
    gAuthFailCount        = 0;
    gHandshakeFailCount   = 0;

    WiFi.disconnect(false, true, WIFI_DISCONNECT_NONBLOCKING_TIMEOUT_MS);

    /* A NOTE ON LEGACY NETWORKS, deliberately left as a comment rather than as code.
     *
     * This core defaults setMinSecurity() to WIFI_AUTH_WPA2_PSK, so a WEP or WPA-only access
     * point is REFUSED before the radio ever tries -- and the refusal surfaces here as a plain
     * failure to connect, with no hint that a policy rather than a passphrase was responsible.
     *
     * If you genuinely need to join such a network, uncomment the line below:
     *
     *     WiFi.setMinSecurity(WIFI_AUTH_WEP);
     *
     * It is left commented on purpose. WEP is broken and WPA1 is deprecated; lowering the floor
     * silently, on everyone's behalf, to save one person a puzzling afternoon is the wrong
     * trade. Documenting exactly which line to change is the right one. Note also that the
     * minimum only applies when a passphrase is supplied -- open networks are unaffected. */

    WiFi.begin(gSsid, (passLen > 0u) ? gPassword : NULL);

    gState             = BRIDGE_CONNECTING;
    gOperationSeq      = f->seq;
    gOperationDeadline = millis() + IOP_ESP_CONNECT_DEADLINE_MS;
    setStatusLed(0, 0, 40);

    /* The SSID is logged; the password is not, only its length. Debug logs get pasted into
     * issue reports and screenshots. */
    DEBUG_PORT.printf("[connect] joining \"%s\" (%u-character password), deadline %lu ms\n",
                      gSsid, (unsigned)passLen, (unsigned long)IOP_ESP_CONNECT_DEADLINE_MS);
}

/* finishConnect -- the single place a join attempt ends.
 *
 * Every exit from BRIDGE_CONNECTING must do the same four things: leave the state, set the LED,
 * stop the radio retrying, and answer the master. Spelling that out at each exit invites the
 * classic bug where one path forgets to reply and the master waits for a frame that will never
 * arrive. One function, one correct sequence, four call sites. */
static void finishConnect(uint8_t status)
{
    bool ok = (status == IOP_WIFI_STATUS_CONNECTED);

    gState = BRIDGE_IDLE;
    setStatusLed(ok ? 0 : 40, ok ? 40 : 0, 0);
    if (!ok) {
        /* Stop the core's automatic reconnect loop. Without this it keeps retrying a network we
         * have already reported as failed, so the next command runs against a radio that is
         * still churning through attempts for the previous one. */
        WiFi.disconnect(false, true, WIFI_DISCONNECT_NONBLOCKING_TIMEOUT_MS);
    }
    sendConnectResult(status);
}

/* connectPoll -- has the join resolved, one way or the other? */
static void connectPoll(void)
{
    if (gState != BRIDGE_CONNECTING) {
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PORT.println("[connect] success");
        finishConnect(IOP_WIFI_STATUS_CONNECTED);
        return;
    }

    /* Decide early when the radio has told us something DEFINITIVE.
     *
     * WHY NOT JUST WAIT FOR THE DEADLINE: an authentication rejection is final. The AP heard
     * us, checked the passphrase, and said no. Waiting another eight seconds to report a
     * verdict we already have makes the tool feel broken. So auth-class failures answer
     * immediately.
     *
     * "No AP found" gets different treatment: a single miss can happen if the scan sweep and
     * the beacon interval line up badly, so it is only believed after several consecutive
     * attempts. That is the difference between a fast answer and a WRONG fast answer. */
    if (gDisconnectSeen) {
        gDisconnectSeen = false;     /* consumed; the class counters below hold the real state */

        /* The three failure classes are graded by CONFIDENCE, and each gets the treatment its
         * confidence deserves. Reporting fast is good; reporting fast and WRONG is not.
         *
         * AUTH_FAIL / AUTH_EXPIRE: the AP heard us, checked the passphrase and refused. That is
         * a verdict, not a symptom. Answer at once rather than making the user sit out the full
         * deadline for news we already have. */
        if (gAuthFailCount >= 1u) {
            DEBUG_PORT.printf("[connect] authentication rejected (reason %u) -- definitive\n",
                              (unsigned)gLastDisconnectReason);
            finishConnect(IOP_WIFI_STATUS_WRONG_PASSWORD);
            return;
        }

        /* Handshake timeouts (15, 204) usually ALSO mean a wrong passphrase -- the 4-way
         * handshake is exactly where a bad PSK surfaces -- but unlike an outright rejection
         * they can equally be a weak or congested link that simply lost a handshake frame. So
         * they are believed only on the second occurrence. Answering on the first would
         * confidently tell a user with a marginal signal that their correct password is wrong,
         * which is a far more damaging failure than taking a few more seconds to be sure. */
        if (gHandshakeFailCount >= 2u) {
            DEBUG_PORT.printf("[connect] handshake failed twice (reason %u) -- treating this "
                              "as a bad passphrase\n", (unsigned)gLastDisconnectReason);
            finishConnect(IOP_WIFI_STATUS_WRONG_PASSWORD);
            return;
        }

        /* NO_AP_FOUND: one miss is unremarkable, since the scan sweep and the AP beacon
         * interval can simply fail to line up. Three consecutive misses is an absence. */
        if (gNoApFoundCount >= 3u) {
            DEBUG_PORT.printf("[connect] no AP named \"%s\" found after %u attempts\n",
                              gSsid, (unsigned)gNoApFoundCount);
            finishConnect(IOP_WIFI_STATUS_NO_NETWORK);
            return;
        }
    }

    if ((int32_t)(millis() - gOperationDeadline) >= 0) {
        /* Out of time. Report the best explanation we have rather than a bare "timeout": if
         * the radio told us why along the way, that reason is far more actionable. */
        uint8_t status = IOP_WIFI_STATUS_TIMEOUT;
        if (gLastDisconnectReason != 0u) {
            uint8_t mapped = mapDisconnectReason(gLastDisconnectReason);
            if (mapped != IOP_WIFI_STATUS_UNKNOWN_ERROR) {
                status = mapped;
            }
        }
        DEBUG_PORT.printf("[connect] gave up after %lu ms (last disconnect reason %u) -> %s\n",
                          (unsigned long)IOP_ESP_CONNECT_DEADLINE_MS,
                          (unsigned)gLastDisconnectReason, iop_wifi_status_name(status));
        finishConnect(status);
    }
}

/* sendConnectResult -- build the fixed 5-byte WIFI_CONNECT_RESP payload. */
static void sendConnectResult(uint8_t status)
{
    uint8_t payload[6];

    payload[0] = status;
    /* The raw 802.11 reason, unedited. STATUS above is this radio's interpretation; this is the
     * fact it was derived from. The brain gets both and may disagree -- see the architecture
     * note at the top of interop_protocol.h. */
    payload[5] = gLastDisconnectReason;
    if (status == IOP_WIFI_STATUS_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        payload[1] = ip[0];
        payload[2] = ip[1];
        payload[3] = ip[2];
        payload[4] = ip[3];
        DEBUG_PORT.printf("[connect] IP address %u.%u.%u.%u\n",
                          (unsigned)payload[1], (unsigned)payload[2],
                          (unsigned)payload[3], (unsigned)payload[4]);
    } else {
        /* Explicitly zeroed. The four bytes are always present so the payload is a fixed size,
         * but they carry no meaning unless STATUS is CONNECTED -- and sending stale octets
         * from a previous session would be worse than sending nothing. */
        payload[1] = 0;
        payload[2] = 0;
        payload[3] = 0;
        payload[4] = 0;
    }
    sendFrame(gOperationSeq, IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_RESP, payload, sizeof(payload));
}

/* mapDisconnectReason -- turn an 802.11 reason code into a status a human can act on.
 *
 * Values verified against wifi_err_reason_t in the installed IDF headers. This mapping is the
 * whole reason the sketch registers a WiFi event handler at all: WiFi.status() alone reports
 * only WL_DISCONNECTED, which cannot distinguish "wrong password" from "no such network" --
 * the two failures a user most needs told apart. */
static uint8_t mapDisconnectReason(uint8_t reason)
{
    switch (reason) {
        case 201: /* WIFI_REASON_NO_AP_FOUND */
            return IOP_WIFI_STATUS_NO_NETWORK;

        case 2:   /* WIFI_REASON_AUTH_EXPIRE            */
        case 15:  /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT -- the classic wrong-PSK signature */
        case 202: /* WIFI_REASON_AUTH_FAIL              */
        case 204: /* WIFI_REASON_HANDSHAKE_TIMEOUT      */
            return IOP_WIFI_STATUS_WRONG_PASSWORD;

        case 200: /* WIFI_REASON_BEACON_TIMEOUT   -- was there, then went away  */
        case 203: /* WIFI_REASON_ASSOC_FAIL                                     */
        case 205: /* WIFI_REASON_CONNECTION_FAIL                                */
            return IOP_WIFI_STATUS_TIMEOUT;

        default:
            return IOP_WIFI_STATUS_UNKNOWN_ERROR;
    }
}

/* onWiFiEvent -- runs on the Arduino event task, NOT in loop().
 *
 * Keep it tiny: record what happened and return. Doing real work here -- building frames,
 * writing to Serial1 -- would mean two tasks touching the same buffers without a lock, which
 * is a race waiting to happen. All it does is set flags that connectPoll() reads. */
static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        eventPush(IOP_WIFI_EVT_CONNECTED, 0);
        return;
    }
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        /* The address is deliberately NOT read out of the event union here. Doing so would bake
         * an assumption about lwIP's struct layout into this sketch; loop() calls
         * WiFi.localIP() instead, which is the supported accessor and is equally current by the
         * time the frame is built. Less clever, less to break. */
        eventPush(IOP_WIFI_EVT_GOT_IP, 0);
        return;
    }
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = (uint8_t)info.wifi_sta_disconnected.reason;

        eventPush(IOP_WIFI_EVT_DISCONNECTED, reason);
        gLastDisconnectReason = reason;
        gDisconnectSeen       = true;

        /* Tally by CLASS of failure, because the classes carry different confidence and are
         * acted on differently by connectPoll().
         *
         * Counting rather than just recording matters because this core auto-reconnects behind
         * your back: getAutoReconnect() defaults to true, and its retry list includes
         * NO_AP_FOUND, AUTH_EXPIRE, the handshake timeouts, ASSOC_FAIL and CONNECTION_FAIL. So
         * one failed join produces a SERIES of these events, not a single one, and a decision
         * based on "did a disconnect happen" would fire on the first flap. */
        if (reason == 201u) {                                  /* NO_AP_FOUND               */
            if (gNoApFoundCount < 255u) { gNoApFoundCount++; }
        } else if (reason == 202u || reason == 2u) {           /* AUTH_FAIL, AUTH_EXPIRE    */
            if (gAuthFailCount < 255u) { gAuthFailCount++; }
        } else if (reason == 15u || reason == 204u) {          /* handshake timeouts        */
            if (gHandshakeFailCount < 255u) { gHandshakeFailCount++; }
        }
    }
}

/* eventPush -- called from the WiFi task. Must stay short and must not transmit. */
static void eventPush(uint8_t type, uint8_t reason)
{
    uint8_t head = gEventHead;
    uint8_t next = (uint8_t)((head + 1u) % RADIO_EVENT_QUEUE_SIZE);

    if (next == gEventTail) {
        /* Full. Drop the OLDEST by advancing the tail, so the freshest radio state always wins.
         * Stale news about a link that has since changed again is worse than no news. */
        gEventTail = (uint8_t)((gEventTail + 1u) % RADIO_EVENT_QUEUE_SIZE);
        gEventsDropped++;
    }
    gEventQueue[head].type   = type;
    gEventQueue[head].reason = reason;
    gEventHead = next;
}

/* eventPoll -- drain the queue from loop() and announce each event to the brain.
 *
 * One frame per loop pass, not the whole queue at once: a burst of disconnects would otherwise
 * put several kilobytes on the wire in a single pass and could collide with the master's own
 * traffic. Draining steadily keeps the link smooth, and the queue is only eight deep. */
static void eventPoll(void)
{
    RadioEvent ev;

    if (gEventTail == gEventHead) {
        return;
    }
    ev.type   = gEventQueue[gEventTail].type;
    ev.reason = gEventQueue[gEventTail].reason;
    gLastEventType = ev.type;
    gEventTail = (uint8_t)((gEventTail + 1u) % RADIO_EVENT_QUEUE_SIZE);

    DEBUG_PORT.printf("[event] %s%s%s\n", iop_wifi_event_name(ev.type),
                      (ev.type == IOP_WIFI_EVT_DISCONNECTED) ? ": " : "",
                      (ev.type == IOP_WIFI_EVT_DISCONNECTED)
                          ? iop_wifi_reason_name(ev.reason) : "");
    sendWifiEvent(ev.type, ev.reason);
}

/* sendWifiEvent -- an unsolicited WIFI_EVENT frame, SEQ 0x00. */
static void sendWifiEvent(uint8_t type, uint8_t reason)
{
    uint8_t payload[6];

    payload[0] = type;
    payload[1] = reason;
    payload[2] = 0;
    payload[3] = 0;
    payload[4] = 0;
    payload[5] = 0;
    if (type == IOP_WIFI_EVT_GOT_IP) {
        IPAddress ip = WiFi.localIP();
        payload[2] = ip[0];
        payload[3] = ip[1];
        payload[4] = ip[2];
        payload[5] = ip[3];
    }
    sendEvent(IOP_CHAN_WIFI, IOP_CMD_WIFI_EVENT, payload, sizeof(payload));
}

/* sendRadioReady -- announce, once at boot, that the radio exists and why it restarted.
 *
 * RESET_REASON is the payload byte that earns this frame its keep. esp_reset_reason() tells you
 * whether this was a clean power-up, a watchdog, a software restart, or -- the one that matters
 * most on a shared USB supply with a WiFi radio -- a BROWNOUT. A brownout means the 3.3 V rail
 * sagged when the transmitter keyed up, and no amount of protocol debugging will fix it. The
 * cure is the bulk capacitance already built into this project; this byte is how you find out
 * whether it is doing its job. */
static void sendRadioReady(void)
{
    uint8_t payload[6];
    uint8_t caps = IOP_CAP_WIFI | IOP_CAP_MESH;

    payload[0] = (uint8_t)IOP_PROTOCOL_VERSION;
    payload[1] = (uint8_t)IOP_PROTOCOL_MINOR;
    payload[2] = (uint8_t)((IOP_CRC16_CHECK_VALUE >> 8) & 0xFFu);
    payload[3] = (uint8_t)(IOP_CRC16_CHECK_VALUE & 0xFFu);
    payload[4] = (uint8_t)esp_reset_reason();
    payload[5] = caps;

    /* payload[4], not payload[2]. The v1 payload was 4 bytes with the reset reason at index 2;
     * v2 inserted the protocol minor and widened the CRC check value to two bytes, pushing it to
     * index 4. This print was missed in the migration and reported "reset reason 111" -- which is
     * 0x6F, the HIGH BYTE OF THE CRC CHECK VALUE, a plausible-looking number that is entirely
     * wrong. Indexing a payload by literal offset is exactly the kind of thing that survives a
     * format change silently; the only reason it was caught is that 111 is not a valid
     * esp_reset_reason_t value and looked wrong on sight. */
    DEBUG_PORT.printf("[radio] announcing RADIO_READY (reset reason %u, capabilities 0x%02X)\n",
                      (unsigned)payload[4], (unsigned)caps);
    sendEvent(IOP_CHAN_TRANSPORT, IOP_CMD_RADIO_READY, payload, sizeof(payload));
}

/* sendFrame -- serialise and transmit, with a single bulk write. */
static void sendFrame(uint8_t seq, uint8_t chan, uint8_t cmd,
                      const uint8_t *payload, uint16_t len)
{
    uint16_t n = iop_build_frame(gTxBuf, sizeof(gTxBuf), seq, IOP_FLAG_NONE, chan, cmd,
                                 payload, len);

    if (n == 0u) {
        /* Refusing to send is the correct behaviour, but it must never be silent -- a response
         * that was never sent looks identical, from the master, to a link that died. */
        DEBUG_PORT.printf("[tx] FAILED to build %s/%s (payload %u bytes, limit %u)\n",
                          iop_chan_name(chan), iop_cmd_name(cmd),
                          (unsigned)len, (unsigned)IOP_MAX_PAYLOAD_LEN);
        return;
    }

    LINK_PORT.write(gTxBuf, n);
    /* flush() waits for the bytes to actually leave the UART. It matters here because the very
     * next thing this sketch may do is a WiFi operation that hogs the CPU; getting the response
     * fully onto the wire first keeps the master's inter-byte timeout out of the picture. */
    LINK_PORT.flush();

    if (gHexDump) {
        DEBUG_PORT.printf("[tx] %s/%s seq=%u (%u bytes)\n",
                          iop_chan_name(chan), iop_cmd_name(cmd),
                          (unsigned)seq, (unsigned)n);
        hexDump("     ", gTxBuf, n);
    }
}

/* sendEvent -- an unsolicited frame, carrying the next value of the EVENT sequence counter.
 *
 * v1 gave every unsolicited frame the same reserved SEQ of 0x00, which distinguished them from
 * replies but made event LOSS undetectable -- and events genuinely can be lost, three ways:
 * this radio's queue can overflow before a frame is built, the brain's receive ring can overrun,
 * and the UART can drop a byte in hardware without setting any flag the Teensy core exposes.
 *
 * A rolling counter in bits 0-6, with bit 7 marking the class, makes a gap visible. The brain
 * sees "I expected 0x83 and got 0x85" and knows exactly how many announcements it missed. */
static void sendEvent(uint8_t chan, uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    gEventSeq = iop_next_event_seq(gEventSeq);
    sendFrame(gEventSeq, chan, cmd, payload, len);
}

/* sendNack -- the standard refusal, and the only place error frames are built. */
static void sendNack(uint8_t seq, uint8_t origChan, uint8_t origCmd, uint8_t errorCode)
{
    uint16_t n = iop_build_nack(gTxBuf, sizeof(gTxBuf), seq, origChan, origCmd, errorCode);

    gStats.nacksSent++;
    DEBUG_PORT.printf("[tx] NACK seq=%u %s/0x%02X: %s\n",
                      (unsigned)seq, iop_chan_name(origChan), (unsigned)origCmd,
                      iop_err_name(errorCode));
    if (n > 0u) {
        LINK_PORT.write(gTxBuf, n);
        LINK_PORT.flush();
    }
}

/* sendErrorNack -- a NACK provoked by garbage rather than by a real request, rate limited.
 *
 * On a badly broken link (baud mismatch, floating ground, a board rebooting in a loop) both
 * ends see continuous nonsense. Without a limit, each end emits an error frame per malformed
 * frame; those cross the broken link, arrive corrupted, and provoke more of the same. At
 * 921600 baud that fills the wire in both directions with complaints, and the real fault -- a
 * loose wire -- is buried under the noise its own reporting created. A persistent fault is
 * worth reporting once, not ten thousand times. Suppressed NACKs are counted and printed with
 * the heartbeat, so throttling never hides anything. */
static void sendErrorNack(uint8_t seq, uint8_t origChan, uint8_t origCmd, uint8_t errorCode)
{
    if (!iop_nack_should_send(&gNackLimiter, millis())) {
        return;
    }
    sendNack(seq, origChan, origCmd, errorCode);
}

/* setStatusLed -- one line of out-of-band status you can read across the room.
 *
 * THE TRAP THIS AVOIDS: on most ESP32-S3 development boards the "built-in LED" is a single
 * addressable WS2812 RGB device, not a plain LED on a GPIO. The variant header proves it --
 * LED_BUILTIN is defined as SOC_GPIO_PIN_COUNT + PIN_RGB_LED, an offset-encoded pin number
 * rather than a real GPIO. Calling digitalWrite(LED_BUILTIN, HIGH) on such a board does not
 * light anything, and produces no error either, so it reads as "my board is broken".
 *
 * The #ifdef below picks the right call at compile time and works unmodified on both kinds of
 * board. Colour scheme: green = idle and ready, blue = working, red = last operation failed.
 */
static void setStatusLed(uint8_t r, uint8_t g, uint8_t b)
{
#if defined(RGB_BUILTIN)
    rgbLedWrite(RGB_BUILTIN, r, g, b);
#elif defined(LED_BUILTIN)
    /* A plain LED cannot show colour, so treat any non-zero request as "on". */
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, (r || g || b) ? HIGH : LOW);
#else
    (void)r; (void)g; (void)b;   /* no LED on this board; not an error */
#endif
}

/* hexDump -- 16 bytes per line, the universal convention, so output can be compared directly
 * against a logic-analyser capture without mental arithmetic. */
static void hexDump(const char *prefix, const uint8_t *data, uint16_t n)
{
    uint16_t i;
    if (data == NULL || n == 0u) {
        return;
    }
    for (i = 0; i < n; i++) {
        if ((i % 16u) == 0u) {
            if (i > 0u) {
                DEBUG_PORT.println();
            }
            DEBUG_PORT.print(prefix);
        }
        DEBUG_PORT.printf("%02X ", (unsigned)data[i]);
    }
    DEBUG_PORT.println();
}

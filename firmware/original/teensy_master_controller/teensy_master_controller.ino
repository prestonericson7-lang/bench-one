/* ===========================================================================================
 *  teensy_master_controller.ino  --  Teensy 4.1, MASTER side of the interop link
 * ===========================================================================================
 *
 *  ROLE
 *  ----
 *  This board is the BRAIN. The ESP32-S3 is a radio -- a peripheral that gives this Teensy
 *  WiFi, and later BLE. Nothing more.
 *
 *  That division decides where behaviour belongs: MECHANISM on the radio, POLICY here. The
 *  radio knows how to drive an 802.11 MAC and reports what happened. This sketch decides what
 *  it MEANS and what to do about it -- how many times to retry, whether a failure matters,
 *  which network to prefer, what state to restore after the radio reboots.
 *
 *  In practice that means this sketch does three things a mere terminal would not:
 *
 *    It keeps its own model of the radio's state (see gRadio), updated from unsolicited
 *    events, so it always knows what the radio is doing without asking.
 *
 *    It reads the RAW 802.11 reason codes the radio forwards, not just the friendly summary,
 *    because the summary is made by the component with the least context. Reason 15 and reason
 *    202 both mean "wrong password" to the radio; only the brain knows that 15 on a marginal
 *    link is worth retrying and 202 never is.
 *
 *    It listens for the radio announcing its own reboot, and says so loudly -- because a radio
 *    that silently restarted is the single most confusing failure in a system like this.
 *
 *  The Teensy never waits on the ESP32 with a blocking call: it sends a request, notes a
 *  deadline, and carries on. Everything here is driven from loop().
 *
 *  This sketch is also the human interface: you type text commands into the Arduino Serial
 *  Monitor over USB, and it translates them into binary frames on the UART.
 *
 *  WIRING
 *  ------
 *      Teensy 4.1                          ESP32-S3
 *      ----------                          --------
 *      pin 1  (TX1)  ------------------->  RX pin (default GPIO18 in the ESP32 sketch)
 *      pin 0  (RX1)  <-------------------  TX pin (default GPIO17 in the ESP32 sketch)
 *      GND           <------------------>  GND      <-- REQUIRED, and the usual culprit
 *
 *  TX GOES TO RX. Crossed, not straight through. If you wire TX to TX you get perfect silence
 *  and no error message, because nothing is electrically wrong -- both sides are just talking
 *  into their own ears. If the link is dead, check this first, then check GND.
 *
 *  Both boards are 3.3 V logic, so they connect directly. Do NOT put a 5 V level shifter or an
 *  Arduino Uno in the middle: 5 V on an ESP32-S3 or Teensy 4.1 input damages the pin.
 *
 *  TWO SERIAL PORTS, AND WHY THAT MATTERS
 *  --------------------------------------
 *      Serial   = USB to your PC. This is the Serial Monitor. Human-readable text.
 *      Serial1  = the hardware UART to the ESP32. Binary frames. Never printed to directly.
 *
 *  Confusing these is the single most common way to break this project. If you ever write
 *  Serial.write(frame, n), the frame goes to your PC and the ESP32 hears nothing.
 *
 *  WHAT THE TEENSY 4.1 BRINGS TO THIS LINK (and why the master side looks like it does)
 *  ------------------------------------------------------------------------------------
 *  It would be easy to treat the master as a dumb terminal that forwards typed commands. That
 *  wastes the board. A Teensy 4.1 is a 600 MHz Cortex-M7 with an FPU, 1 MB of RAM, eight
 *  hardware UARTs and a cycle counter -- it is a better instrument than most of the USB logic
 *  analysers people buy to debug links like this one. This sketch uses that deliberately:
 *
 *    CYCLE-ACCURATE LATENCY. The Cortex-M7 DWT cycle counter runs free at the core clock and
 *    is already enabled by the Teensy core at startup. At 600 MHz one tick is 1.67 ns, so
 *    round-trip time is measured in NANOSECONDS rather than reported as "less than a
 *    millisecond". That precision is not a gimmick: it is how you tell a healthy 200 us
 *    turnaround from a 3 ms one caused by the ESP32's WiFi task preempting its own loop().
 *    See rttNanoseconds() and the "bench" command.
 *
 *    STATISTICAL LINK CHARACTERISATION. "bench" fires a train of PINGs and reports min, mean,
 *    max, standard deviation and packet loss. A single ping tells you the link works; a
 *    thousand of them tell you whether it is RELIABLE, which is a different and more useful
 *    question. The FPU makes the statistics free.
 *
 *    CRASH FORENSICS. The Teensy core preserves a fault report across a reset. If the board
 *    hard-faulted, setup() prints exactly where -- so a link that "randomly stops working" is
 *    diagnosed from the boot banner instead of guessed at.
 *
 *    RAM TO SPEND. With 1 MB, the receive buffer is 8 KB rather than the core's 64-byte
 *    default: 89 ms of headroom at 921600 baud, so no plausible stall can drop a byte. On a
 *    board with 2 KB of RAM this would be an agonising trade. Here it is free, and refusing to
 *    spend it would be a false economy.
 *
 *    HARDWARE FLOW CONTROL, AVAILABLE BUT UNUSED. Serial1 can drive RTS on any digital pin and
 *    CTS through the XBAR. This protocol does not need it -- request/response traffic never
 *    saturates the wire -- but the capability is real, it is wired up behind one #define, and
 *    the moment you add streaming it is the first thing to reach for. See LINK_USE_FLOW_CONTROL.
 *
 *    A SECOND USB PORT. Built with USB Type "Dual Serial", the board exposes SerialUSB1 in
 *    addition to the console, and every raw frame is mirrored there as clean binary. A PC-side
 *    decoder can read the actual wire bytes while a human reads plain English on the first
 *    port -- no interleaving, no parsing text back into hex.
 *
 *  Deliberately left for later, because each is a project rather than a feature: the built-in
 *  microSD slot (frame logging to disk), the 10/100 Ethernet PHY pads (bridging this protocol
 *  to a network), USB Host, two CAN-FD controllers, and the seven unused hardware UARTs -- the
 *  last of which means this exact code drives several ESP32s at once by changing one #define.
 *
 *  USAGE
 *  -----
 *  Open the Serial Monitor at 115200 with line ending "Newline" or "Both NL & CR", then type
 *  "help". Every command is listed there, including the deliberate error-injection ones used
 *  to verify the milestone 1 failure paths.
 *
 *  BUILD
 *  -----
 *  Board: "Teensy 4.1". interop_protocol.h must be in THIS folder -- see the README for why a
 *  copy is used rather than a relative include.
 * ===========================================================================================
 */

#include "interop_protocol.h"
/* The brain never speaks ESP-NOW, but it DECODES the node payloads the hub relays -- so it needs
 * the fleet's packet layout just as much as the boards that transmit it. */
#include "mesh_protocol.h"

/* ===========================================================================================
 * OPTIONAL STATUS DISPLAY
 * ===========================================================================================
 * A 0.96" SSD1306 128x64 OLED on I2C, showing link health without a PC attached.
 *
 * WHY IT EXISTS: every fault found while building this link was diagnosed by reading a serial
 * console. A user in the field with two boards and no laptop had no way to tell "no wire" from
 * "baud mismatch" from "the radio is rebooting". The brain already computes every number needed
 * to separate those; it just had nowhere to show them.
 *
 * Set to 0 and the entire feature compiles out -- no library, no RAM, no I2C traffic.
 */
#define DISPLAY_ENABLED   1

#if DISPLAY_ENABLED
#include <U8g2lib.h>
#include <Wire.h>
#endif

/* ===========================================================================================
 * CONFIGURATION -- every tunable in one place, no magic numbers below this block
 * =========================================================================================== */

#define LINK_PORT              Serial1     /* hardware UART to the ESP32: RX=pin 0, TX=pin 1  */
#define LINK_BAUD              921600      /* must match the ESP32 sketch exactly             */
#define CONSOLE_PORT           Serial      /* USB CDC to the PC -- the Serial Monitor         */
#define CONSOLE_BAUD           115200      /* ignored by USB CDC, but set for convention      */

/* The one and only blocking wait in this sketch, and it is in setup() by design.
 *
 * WHY IT EXISTS: the ESP32-S3 takes noticeably longer to boot than the Teensy (bootloader,
 * flash init, WiFi stack bring-up). If both boards power up together and the Teensy pings
 * immediately, the PING lands before anyone is listening and the link looks dead when it is
 * merely early. Two seconds covers it comfortably. */
#define BOOT_WAIT_MS           2000u

/* Extra receive buffer handed to Serial1.
 *
 * WHY THIS IS NOT OPTIONAL: the Teensy core defines SERIAL1_RX_BUFFER_SIZE as 64 bytes
 * (verified in the core's HardwareSerial1.cpp). At 921600 baud a byte arrives every ~10.9 us,
 * so 64 bytes is 0.7 MILLISECONDS of headroom. A WiFi scan response can be most of a kilobyte
 * and arrives as one continuous burst -- roughly 11 ms of solid data. If loop() is even
 * briefly busy (printing a long line to USB, formatting a table), the hardware FIFO overflows
 * and bytes are lost silently. The symptom is maddening: PING always works, scans "randomly"
 * fail their CRC. Adding a full frame's worth of buffer removes the failure mode entirely. */
/* WHY 8 KB AND NOT ONE FRAME: 8192 bytes is 89 ms of continuous data at 921600 baud, versus
 * 11 ms for a single max-size frame and 0.68 ms for the core default. The question is not "how
 * big is a frame" but "how long might loop() be busy". Printing a 30-network scan table over
 * USB can take tens of milliseconds, and USB itself can stall for a full 1 ms frame at a time.
 * On a board with 1 MB of RAM, 8 KB costs 0.8% of it and removes the entire failure class.
 * Sizing this to the frame rather than to the stall is the mistake to avoid. */
#define LINK_RX_EXTRA_BUFFER   8192

/* HARDWARE FLOW CONTROL -- available on this board, off by default.
 *
 * Set to 1 and wire two more signals to enable it. Teensy 4.1 drives RTS from any digital pin
 * (the core asserts and releases it from the UART ISR using the receive watermarks) and reads
 * CTS through the XBAR on pins 2,3,4,5,7,8,30,31,33,36,37,42,43,44,45. The ESP32-S3 has
 * matching hardware via setHwFlowCtrlMode().
 *
 * It is off because this protocol is strictly request/response: only one side transmits at a
 * time, the peer is always listening, and an 8 KB buffer holds eight max-size frames. There is
 * no scenario in the current command set where the receiver cannot keep up. Turn it on when you
 * add continuous streaming -- which is exactly when the 1-in-16000 false-start arithmetic and
 * an 8-bit CRC also stop being adequate. */
#define LINK_USE_FLOW_CONTROL  0
#define LINK_RTS_PIN           2           /* any digital pin                                 */
#define LINK_CTS_PIN           3           /* must be XBAR-capable; attachCts() reports back   */

#define CONSOLE_LINE_MAX       160u        /* longest typed command line we accept            */

/* ---- display configuration ---------------------------------------------------------------
 * SDA = pin 18, SCL = pin 19. Those are LPI2C1's only pins on a Teensy 4.1 (verified in the
 * core's WireIMXRT.cpp) and are clear of Serial1 on pins 0/1.
 *
 * POWER IT FROM 3.3 V, NOT 5 V. The module's I2C pull-ups tie to its own VCC, so a 5 V-powered
 * module drives SDA and SCL to 5 V straight into pins that are not 5 V tolerant -- even though
 * you never ran a 5 V wire to the Teensy directly.
 */
#define DISPLAY_I2C_ADDR       0x3Cu
#define DISPLAY_I2C_HZ         400000u   /* Wire quantizes: <400k gives 100k, and the bus
                                          * actually runs ~428 kHz off the 24 MHz LPI2C clock.
                                          * 1 MHz would exceed the SSD1306's rated 400 kHz.   */
#define DISPLAY_REFRESH_MS     250u      /* 4 Hz. Faster than a human reads, and it keeps the
                                          * duty cycle of I2C traffic negligible.             */
#define DISPLAY_ROTATE_MS      4000u     /* auto-advance through screens                      */
#define DISPLAY_TILE_ROWS      8u        /* 64 px / 8 px per tile row                         */
#define DISPLAY_TILE_COLS      16u       /* 128 px / 8 px per tile                            */

/* Default number of round trips for "bench". Large enough for the standard deviation to mean
 * something, small enough to finish while you are still looking at the screen. */
#define BENCH_DEFAULT_COUNT    200u
#define BENCH_MAX_COUNT        10000u

/* ===========================================================================================
 * EXPLICIT FUNCTION PROTOTYPES
 * ===========================================================================================
 * WHY THESE ARE HERE, written out by hand:
 *
 * The Arduino build system preprocesses .ino files and auto-generates prototypes for the
 * functions it finds, inserting them near the top of the file. That is convenient right up
 * until a function takes a user-defined type -- here, IopFrame or IopEvent. If the generated
 * prototype is placed before the #include that defines the type, the sketch fails to compile
 * with an error that points at a line you never wrote.
 *
 * Declaring the prototypes ourselves, after the include, makes the generator's contribution
 * redundant and the problem impossible. It costs a dozen lines and removes an entire class of
 * confusing build failure. Do this in every Arduino sketch that passes a struct around.
 */
static void  linkPoll(void);
static void  consolePoll(void);
static void  pendingPoll(void);
static void  handleFrame(const IopFrame *f);
static void  handlePong(const IopFrame *f);
static void  handleScanResponse(const IopFrame *f);
static void  handleConnectResponse(const IopFrame *f);
static void  handleNack(const IopFrame *f);
static bool  sendRequest(uint8_t chan, uint8_t cmd, const uint8_t *payload,
                         uint16_t len, uint32_t timeoutMs);
static void  sendRawBytes(const uint8_t *bytes, uint16_t n, const char *why);
static void  clearPending(const char *reason);
static void  sendErrorNack(uint8_t seq, uint8_t origChan, uint8_t origCmd,
                           uint8_t errorCode);
static void  executeCommand(char *line);
static void  cmdHelp(void);
static void  cmdConnect(char *args);
static void  cmdStats(void);
static void  cmdCorrupt(void);
static void  cmdBadCommand(void);
static void  cmdOversize(void);
static void  cmdNoise(void);
static void  hexDump(const char *prefix, const uint8_t *data, uint16_t n);
static void  cmdBench(char *args);
static void  cmdSys(void);
static void  benchRecord(uint32_t rttNs);
static void  benchFinish(void);
static uint32_t rttNanoseconds(uint32_t startCycles, uint32_t endCycles);
static void  mirrorRawFrame(const uint8_t *data, uint16_t n, uint8_t direction);
static void  handleRadioReady(const IopFrame *f);
static void  handleWifiEvent(const IopFrame *f);
static void  cmdRadio(void);
static void  cmdStatus(void);
static void  cmdMesh(char *args);
static void  handleStatusResponse(const IopFrame *f);
static bool  consoleCanWrite(uint16_t bytes);
static const char *espResetReasonName(uint8_t reason);
static void  displayInit(void);
static void  displayPoll(void);
static void  displayRender(void);
static uint8_t displayFaultLevel(void);
static char *skipSpaces(char *s);
static char *takeToken(char **cursor);

/* ===========================================================================================
 * STATE
 * =========================================================================================== */

/* The receive-side parser. Static/global, never on the stack: it contains a 1019-byte buffer. */
static IopParser gParser;

/* Scratch space for building outgoing frames. One buffer is enough because a frame is fully
 * written to the UART before the next one is built. */
static uint8_t gTxBuf[IOP_MAX_FRAME_SIZE];

/* Throttles error NACKs so a broken link cannot turn into a NACK storm. */
static IopNackLimiter gNackLimiter;

/* Backing store donated to Serial1's receive ring. Must stay alive forever, hence static. */
static uint8_t gLinkRxBuffer[LINK_RX_EXTRA_BUFFER];

/* The SEQ to use for the next request. Wraps 0xFF -> 0x00 naturally as a uint8_t, which is
 * exactly the intended behaviour: SEQ only needs to be locally unique among requests that
 * could plausibly still be in flight, and with one outstanding request that is trivially
 * satisfied. It deliberately starts at 1 rather than 0, so that a zeroed variable somewhere
 * never looks like a legitimate sequence number while debugging. */
static uint8_t gNextSeq = 1;

/* The single in-flight request.
 *
 * WHY ONLY ONE: the protocol supports pipelining (that is what SEQ is for), but the ESP32
 * executes one long-running WiFi operation at a time and answers a second request with a BUSY
 * NACK. Modelling that honestly here -- one slot, and a clear refusal when it is occupied --
 * is simpler and more debuggable than a queue that hides the constraint. */
static struct {
    bool     active;
    uint8_t  seq;
    uint8_t  chan;
    uint8_t  cmd;
    uint32_t sentMs;
    uint32_t sentCycles;   /* DWT snapshot, for nanosecond-resolution round-trip timing */
    uint32_t deadlineMs;
} gPending = { false, 0, 0, 0, 0, 0, 0 };

/* rttNanoseconds -- convert a DWT cycle delta into nanoseconds.
 *
 * WHY THE DWT COUNTER RATHER THAN micros(): micros() has 1 us granularity, and a PING round
 * trip over this link is on the order of 150-400 us. Measuring that with a 1 us ruler throws
 * away the very variation you are trying to see -- jitter caused by the ESP32's WiFi task, or
 * by a loop() that occasionally takes longer. ARM_DWT_CYCCNT ticks once per CPU clock, so at
 * 600 MHz the resolution is 1.67 ns: about 600 times finer.
 *
 * The Teensy core enables this counter during startup (startup.c sets ARM_DEMCR_TRCENA and
 * ARM_DWT_CTRL_CYCCNTENA), so there is nothing to initialise -- it is simply running.
 *
 * F_CPU_ACTUAL rather than F_CPU: the real clock, read at runtime. It reflects whatever the
 * Tools > CPU Speed menu selected, and the Teensy also reduces the clock if the die gets hot.
 * Hard-coding 600 MHz would silently mis-scale every measurement on an underclocked board.
 *
 * ROLLOVER: the counter is 32 bits, so it wraps every 2^32 / 600e6 = 7.16 seconds at 600 MHz.
 * The unsigned subtraction below is wrap-correct, so any interval SHORTER than that is measured
 * exactly. Longer operations -- a WiFi scan, a connect -- are timed with millis() instead, and
 * this function is deliberately never used for them. Using it there would produce a confidently
 * wrong number, which is worse than a coarse one. */
static uint32_t rttNanoseconds(uint32_t startCycles, uint32_t endCycles)
{
    uint32_t cycles = endCycles - startCycles;           /* wrap-correct */
    uint32_t hz = F_CPU_ACTUAL;

    if (hz == 0u) {
        return 0;
    }
    /* 64-bit intermediate: cycles can reach 4.29e9 and multiplying by 1e9 overflows 32 bits
     * immediately. The Cortex-M7 does 64-bit integer maths in a few cycles; there is no reason
     * to be clever here. */
    return (uint32_t)(((uint64_t)cycles * 1000000000ULL) / (uint64_t)hz);
}

/* Benchmark state. A train of PINGs, driven entirely from loop() -- each response fires the
 * next request, so the console stays responsive and the measurement is never distorted by a
 * blocking wait. */
static struct {
    bool     active;
    uint32_t remaining;
    uint32_t completed;
    uint32_t lost;
    uint32_t minNs;
    uint32_t maxNs;
    uint64_t sumNs;
    uint64_t sumSqNs;    /* for the standard deviation -- the FPU makes this free */
} gBench = { false, 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0 };

/* Print every frame in hex, both directions. Off by default because it is noisy; turn it on
 * with "hexdump on" the moment anything misbehaves. Seeing the actual bytes is almost always
 * faster than reasoning about what they should have been. */
static bool gHexDump = false;

/* THE BRAIN'S MODEL OF THE RADIO.
 *
 * Maintained from unsolicited events, so it is current without polling. This is the difference
 * between a master that has to ask "are you still connected?" and one that already knows: the
 * radio reports state changes as they happen, and this struct is where they land.
 *
 * `ready` starts false. Until a RADIO_READY frame arrives, or a PONG comes back, the brain does
 * not assume there is a radio out there at all. */
static struct {
    bool     ready;              /* a RADIO_READY has been received                         */
    bool     associated;         /* joined an AP (may not have an address yet)              */
    bool     hasIp;
    uint8_t  ip[4];
    uint8_t  lastReason;         /* raw 802.11 reason from the most recent disconnect       */
    uint8_t  capabilities;       /* IOP_CAP_* bits the radio advertised                     */
    uint8_t  protocolVersion;
    uint8_t  protocolMinor;
    uint16_t crcCheckValue;
    uint8_t  resetReason;
    uint32_t reboots;            /* how many times the radio has announced itself           */
    uint32_t disconnects;
    bool     mismatch;           /* the radio is running a different protocol build          */
    uint8_t  state;              /* last polled IOP_WIFI_STATE_*                             */
    int8_t   rssi;
    uint8_t  eventsDropped;      /* radio-side events that never reached the wire            */
    bool     polled;             /* a status snapshot has been received at least once        */
    uint8_t  lastEventSeq;       /* for detecting gaps in the unsolicited counter            */
    bool     eventSeqValid;
} gRadio = { false, false, false, { 0, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0,
             false, 0, 0, 0, false, 0, false };

/* ============================================================================================
 *  THE FLEET, as this brain models it
 * ============================================================================================
 *
 * The Teensy has no radio of any kind. Everything below is built from frames the hub relays, and
 * that separation is the whole architecture: the hub knows how to move ESP-NOW packets, and this
 * board decides which nodes matter, how much of their data it wants, and what a silent node means.
 *
 * WHY THE BRAIN KEEPS ITS OWN COUNTERS instead of just asking the hub. Two different questions:
 * the hub's rx count is what the AIR delivered; the counters here are what actually reached the
 * brain through the subscription. When the divisor is 20 those numbers legitimately differ by
 * 20x, and conflating them would make a working system look like it was losing 95% of its data.
 * Keeping both is what makes the subscription safe to reason about.
 *
 * Only IOP_MESH_STREAM_ALL gives sample-accurate node data here; at any other divisor this is a
 * sampled view and says so when printed. */
#define BRAIN_MAX_NODES  8u

typedef struct {
    uint8_t  mac[6];
    bool     used;
    bool     alive;
    uint32_t frames;         /* MESH_DATA frames this brain received (post-decimation)      */
    uint32_t lastMs;
    uint16_t lastSeq;        /* the NODE's own sequence number, from its payload            */
    bool     seqValid;
    uint32_t seqGaps;        /* jumps in that counter -- at divisor 1 this is real loss,
                              * at divisor N it is mostly decimation and is labelled so     */
    uint8_t  mode;
    uint16_t intToSendUs;    /* node-local IRQ-to-send latency, newest sample               */
    int16_t  ax, ay, az;
    int16_t  gx, gy, gz;
    /* Identity, from MESH_MSG_HELLO. A fleet you cannot interrogate is a fleet you are guessing
     * about: firmware version and WHO_AM_I are how you find the one board that is different. */
    bool     identified;
    uint8_t  fwMajor, fwMinor, caps, sensorOk, whoAmI;
} MeshNodeView;

static MeshNodeView gMeshNodes[BRAIN_MAX_NODES];

static struct {
    uint8_t  state;          /* IOP_MESH_STATE_*                                            */
    uint8_t  channel;
    uint8_t  mode;
    uint8_t  stream;         /* the divisor WE asked for; authoritative copy lives on the hub */
    bool     polled;
    uint32_t dataFrames;     /* MESH_DATA frames received, all nodes                        */
    uint32_t hubRx;          /* the hub's own air-side count, from the last status poll      */
    uint32_t pingMinUs, pingMeanUs, pingMaxUs;
    uint16_t pingSent, pingRecv;
    bool     pingValid;
} gMesh = { IOP_MESH_STATE_OFF, 0, IOP_MESH_MODE_ONLY, IOP_MESH_STREAM_OFF,
            false, 0, 0, 0, 0, 0, 0, 0, false };

/* Live sample printing. OFF by default for the same reason the hub's stream is: filling the
 * console at 200 Hz is a decision the operator makes, not one the firmware makes for them. */
static bool gMeshWatch = false;

/* Link health counters, reported by "stats". Rates matter more than individual events. */
static struct {
    uint32_t requestsSent;
    uint32_t responsesOk;
    uint32_t timeouts;
    uint32_t nacksReceived;
    uint32_t staleFrames;
    uint32_t badCrc;
    uint32_t consoleDropped;   /* log lines skipped because the USB host was not draining */
    uint32_t eventsMissed;     /* gaps detected in the radio's unsolicited sequence        */
} gStats = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* consoleCanWrite -- is there room in the USB TX buffer for n bytes right now?
 *
 * WHY THIS GUARD EXISTS. Teensy USB serial writes are NOT free. The core spins on
 * TX_TIMEOUT_MSEC, which is 120 ms, whenever the host is connected but not draining -- a serial
 * monitor scrolled up, minimised, or simply slow. Meanwhile the link's receive ring holds 8192
 * bytes, which at 921600 baud is 88.9 ms of data.
 *
 *     120 ms of possible blocking  >  88.9 ms of buffered headroom
 *
 * So one stalled console write can overrun the ring that was deliberately enlarged to prevent
 * exactly that. And the console output is densest precisely when something interesting is
 * happening -- a scan table is dozens of lines. The diagnostics can destroy what they diagnose.
 *
 * Checking availableForWrite() first turns a stall into a dropped log line. Losing a line of
 * text is a fair trade for never losing a frame, and the drop is counted so it is never silent.
 */
static bool consoleCanWrite(uint16_t bytes)
{
    if (CONSOLE_PORT.availableForWrite() >= (int)bytes) {
        return true;
    }
    gStats.consoleDropped++;
    return false;
}


/* ===========================================================================================
 * STATUS DISPLAY
 * ===========================================================================================
 *
 * HOW THIS AVOIDS BREAKING THE LINK -- the reasoning matters more than the code.
 *
 * The obvious worry is that blocking I2C drops bytes. It does not, and it is worth knowing why:
 * Serial1 receive on a Teensy 4 is entirely ISR-driven. HardwareSerialIMXRT::IRQHandler() fills
 * the ring buffer from interrupt context no matter what loop() is doing, so a slow loop() delays
 * the PARSER, not RECEPTION. With an 8 KB ring that is 89 ms of headroom. Nor can a slow loop
 * trip the 50 ms inter-byte timeout, because iop_parser_tick() is only ever called when the
 * receive queue is already empty -- see the long note on that function in the shared header.
 *
 * The real hazard is subtler: this sketch measures round-trip latency with the DWT cycle counter
 * at 1.67 ns resolution, and reports a mean of ~298 us with a sigma of ~7 us. A blocking display
 * refresh that lands between sending a request and reading its response would be COUNTED AS
 * LATENCY -- a 21 ms refresh is a 77-sigma outlier injected into the very statistic this display
 * exists to show. The display would corrupt its own measurement.
 *
 * Two rules follow, and they are the whole design:
 *
 *   1. NEVER TOUCH I2C WHILE A REQUEST IS IN FLIGHT. gPending.active gates every transfer, so a
 *      refresh can never land inside a timed exchange.
 *   2. PUSH ONE TILE ROW PER ITERATION, NOT THE WHOLE SCREEN. u8g2 renders into a 1 KB buffer in
 *      RAM with no I2C at all; updateDisplayArea() then pushes an 8-pixel-tall strip. One row is
 *      128 data bytes, and Teensy 4's Wire BUFFER_LENGTH is 136, so a row is a SINGLE I2C
 *      transaction of roughly 2.7 ms at 428 kHz rather than one 21 ms blast.
 *
 * Worst case added to any loop() iteration: about 2.7 ms, and never during a measured exchange.
 */
#if DISPLAY_ENABLED

/* Full-buffer constructor: 1024 bytes of RAM on a board with 512 KB. Full buffer is what makes
 * rule 2 possible -- drawing is free, and only the push costs I2C time. */
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C gDisplay(U8G2_R0, U8X8_PIN_NONE);

static bool     gDisplayOk = false;     /* false if nothing answered at 0x3C */
static uint8_t  gDisplayRow = 0;        /* which tile row is pushed next     */
static uint32_t gDisplayLastFull = 0;
static uint32_t gDisplayLastRotate = 0;
static uint8_t  gDisplayScreen = 0;

/* displayInit -- probe the bus first, then set up.
 *
 * The probe matters: u8g2's begin() returns void and happily "succeeds" against an empty bus, so
 * without it a missing display is indistinguishable from a working one. A direct Wire probe
 * gives a definite answer, which is then printed to the console and shown in "sys". */
static void displayInit(void)
{
    Wire.begin();
    Wire.setClock(DISPLAY_I2C_HZ);

    Wire.beginTransmission((uint8_t)DISPLAY_I2C_ADDR);
    gDisplayOk = (Wire.endTransmission() == 0);

    if (!gDisplayOk) {
        CONSOLE_PORT.printf("[oled] nothing answered at 0x%02X -- display disabled.\n",
                            (unsigned)DISPLAY_I2C_ADDR);
        CONSOLE_PORT.println(F("       Check SDA=pin18, SCL=pin19, VCC=3.3V (NOT 5V), GND shared."));
        return;
    }
    gDisplay.begin();
    gDisplay.setContrast(160);
    gDisplay.clearBuffer();
    gDisplay.sendBuffer();     /* one full blast is fine here -- the link is not running yet */
    CONSOLE_PORT.printf("[oled] SSD1306 128x64 at 0x%02X, refresh %lu ms, one tile row per pass\n",
                        (unsigned)DISPLAY_I2C_ADDR, (unsigned long)DISPLAY_REFRESH_MS);
}

/* displayFaultLevel -- 0 healthy, 1 warning, 2 fault.
 *
 * This is the display's single most valuable computation, and it encodes the diagnostic rule
 * that took an afternoon to learn on real hardware:
 *
 *     no valid frames AND nothing discarded  ->  nothing is arriving ELECTRICALLY.
 *                                                No wire, wrong pins, or TX wired to TX.
 *     no valid frames AND bytes discarded    ->  bytes ARE arriving but never parse.
 *                                                Baud mismatch, near-certainly.
 *
 * A user should not have to remember that. The screen says which one it is. */
static uint8_t displayFaultLevel(void)
{
    if (gParser.stat_frames_ok == 0u) {
        return 2;                                   /* link has never worked */
    }
    if (gRadio.reboots > 1u) {
        return 2;                                   /* the radio is restarting on its own */
    }
    if (gStats.timeouts > 0u || gParser.stat_bad_crc > 0u) {
        return 1;
    }
    return 0;
}

/* displayRender -- draw into the RAM buffer. Touches no I2C at all, so its cost is a few
 * microseconds of CPU and it can run whenever. */
static void displayRender(void)
{
    char line[26];
    uint8_t fault = displayFaultLevel();

    gDisplay.clearBuffer();
    gDisplay.setFont(u8g2_font_6x10_tf);

    /* --- header: the verdict, in words, always on screen ------------------------------- */
    if (gParser.stat_frames_ok == 0u && gParser.stat_resync_bytes == 0u) {
        gDisplay.drawStr(0, 8, "LINK DEAD - NO SIGNAL");
        gDisplay.setFont(u8g2_font_5x7_tf);
        gDisplay.drawStr(0, 18, "0 bytes arriving. Check");
        gDisplay.drawStr(0, 26, "TX->RX crossed, and GND.");
    } else if (gParser.stat_frames_ok == 0u) {
        gDisplay.drawStr(0, 8, "LINK DEAD - GARBAGE");
        gDisplay.setFont(u8g2_font_5x7_tf);
        snprintf(line, sizeof(line), "%lu bytes, 0 frames",
                 (unsigned long)gParser.stat_resync_bytes);
        gDisplay.drawStr(0, 18, line);
        gDisplay.drawStr(0, 26, "Baud mismatch likely.");
    } else {
        snprintf(line, sizeof(line), "LINK %s",
                 (fault == 0u) ? "OK" : (fault == 1u ? "OK (errors)" : "DEGRADED"));
        gDisplay.drawStr(0, 8, line);
    }

    gDisplay.drawHLine(0, 11, 128);
    gDisplay.setFont(u8g2_font_5x7_tf);

    if (gParser.stat_frames_ok == 0u) {
        /* The dead-link screens above already used the whole display. */
        gDisplay.drawStr(0, 62, "teensy=brain esp=radio");
        return;
    }

    switch (gDisplayScreen) {
    case 0:                                        /* ---- link health ---- */
        snprintf(line, sizeof(line), "frames ok   %lu", (unsigned long)gParser.stat_frames_ok);
        gDisplay.drawStr(0, 21, line);
        snprintf(line, sizeof(line), "bad CRC     %lu", (unsigned long)gParser.stat_bad_crc);
        gDisplay.drawStr(0, 30, line);
        snprintf(line, sizeof(line), "discarded   %lu", (unsigned long)gParser.stat_resync_bytes);
        gDisplay.drawStr(0, 39, line);
        snprintf(line, sizeof(line), "timeouts    %lu", (unsigned long)gStats.timeouts);
        gDisplay.drawStr(0, 48, line);
        snprintf(line, sizeof(line), "sent %lu  rx %lu",
                 (unsigned long)gStats.requestsSent, (unsigned long)gStats.responsesOk);
        gDisplay.drawStr(0, 57, line);
        break;

    case 1:                                        /* ---- latency ---- */
        gDisplay.drawStr(0, 21, "ROUND TRIP");
        if (gBench.completed > 0u) {
            snprintf(line, sizeof(line), "min  %lu.%02lu us",
                     (unsigned long)(gBench.minNs / 1000u),
                     (unsigned long)((gBench.minNs % 1000u) / 10u));
            gDisplay.drawStr(0, 32, line);
            snprintf(line, sizeof(line), "max  %lu.%02lu us",
                     (unsigned long)(gBench.maxNs / 1000u),
                     (unsigned long)((gBench.maxNs % 1000u) / 10u));
            gDisplay.drawStr(0, 41, line);
            snprintf(line, sizeof(line), "n=%lu lost=%lu",
                     (unsigned long)gBench.completed, (unsigned long)gBench.lost);
            gDisplay.drawStr(0, 50, line);
            gDisplay.drawStr(0, 59, "wire floor 130.2us");
        } else {
            gDisplay.drawStr(0, 32, "no samples yet");
            gDisplay.drawStr(0, 41, "type 'bench' on USB");
        }
        break;

    default:                                       /* ---- radio ---- */
        gDisplay.drawStr(0, 21, "RADIO");
        if (!gRadio.ready) {
            gDisplay.drawStr(0, 32, "not announced yet");
            gDisplay.drawStr(0, 41, "(only sent when the");
            gDisplay.drawStr(0, 50, " RADIO itself boots)");
        } else {
            snprintf(line, sizeof(line), "assoc %s  reboots %lu",
                     gRadio.associated ? "yes" : "no ", (unsigned long)gRadio.reboots);
            gDisplay.drawStr(0, 32, line);
            if (gRadio.hasIp) {
                snprintf(line, sizeof(line), "%u.%u.%u.%u",
                         (unsigned)gRadio.ip[0], (unsigned)gRadio.ip[1],
                         (unsigned)gRadio.ip[2], (unsigned)gRadio.ip[3]);
            } else {
                snprintf(line, sizeof(line), "no address");
            }
            gDisplay.drawStr(0, 41, line);
            snprintf(line, sizeof(line), "last reason %u",
                     (unsigned)gRadio.lastReason);
            gDisplay.drawStr(0, 50, line);
            snprintf(line, sizeof(line), "proto v%u crc %02X",
                     (unsigned)gRadio.protocolVersion, (unsigned)gRadio.crcCheckValue);
            gDisplay.drawStr(0, 59, line);
        }
        break;
    }
}

/* displayPoll -- the non-blocking pump. See the two rules in the section header. */
static void displayPoll(void)
{
    uint32_t now;

    if (!gDisplayOk) {
        return;
    }

    /* RULE 1: never touch the bus while an exchange is being timed. */
    if (gPending.active) {
        return;
    }

    now = millis();

    if (gDisplayRow == 0u) {
        if ((uint32_t)(now - gDisplayLastFull) < DISPLAY_REFRESH_MS) {
            return;
        }
        if ((uint32_t)(now - gDisplayLastRotate) >= DISPLAY_ROTATE_MS) {
            gDisplayLastRotate = now;
            gDisplayScreen = (uint8_t)((gDisplayScreen + 1u) % 3u);
        }
        displayRender();               /* RAM only -- no I2C */
    }

    /* RULE 2: one tile row per pass. 128 data bytes fits Teensy 4's 136-byte Wire buffer, so
     * this is a single transaction of roughly 2.7 ms rather than a 21 ms full-screen push. */
    gDisplay.updateDisplayArea(0, gDisplayRow, DISPLAY_TILE_COLS, 1);
    gDisplayRow++;
    if (gDisplayRow >= DISPLAY_TILE_ROWS) {
        gDisplayRow = 0;
        gDisplayLastFull = now;
    }
}

#else   /* DISPLAY_ENABLED == 0 : compile the feature out entirely */
static void displayInit(void) { }
static void displayPoll(void) { }
#endif

/* ===========================================================================================
 * SETUP
 * =========================================================================================== */
void setup(void)
{
    uint16_t selftest;
    uint32_t waitStart;

    CONSOLE_PORT.begin(CONSOLE_BAUD);

    /* Wait for the USB serial monitor, but with a BOUND.
     *
     * WHY BOUNDED: the idiomatic "while (!Serial) {}" hangs forever when the board is running
     * standalone on a power supply with no PC attached -- which is exactly how this thing will
     * eventually be deployed. A bounded wait gives you the boot banner when a monitor is open
     * and costs nothing when it is not.
     *
     * This wait doubles as the ESP32 boot delay described above, so the two-second budget is
     * spent once rather than twice. */
    waitStart = millis();
    while (!CONSOLE_PORT && (millis() - waitStart) < BOOT_WAIT_MS) {
        /* spin -- USB enumeration is handled by the core in the background */
    }
    /* If the monitor opened quickly, spend whatever is left of the boot budget so the ESP32 is
     * definitely awake before we ping it. This is the one deliberate blocking delay. */
    while ((millis() - waitStart) < BOOT_WAIT_MS) {
        /* deliberately empty */
    }

    CONSOLE_PORT.println();

    /* CRASH FORENSICS, printed before anything else.
     *
     * The Teensy core preserves a fault report in a region of RAM that survives a reset, so if
     * the last run ended in a hard fault, a bus error, or a watchdog timeout, the details are
     * still here: the faulting instruction address, the fault status registers, and how long
     * the board had been running.
     *
     * This is worth the eight lines because of the specific way link bugs present. "The link
     * randomly stops working" and "the board silently rebooted" look identical from the far
     * end -- both are just silence. Printing the report at boot separates them instantly, and
     * points at the actual faulting address rather than at the wiring. */
    if (CrashReport) {
        CONSOLE_PORT.println(F("*** The previous run ended in a crash. Report follows. ***"));
        CONSOLE_PORT.print(CrashReport);
        CONSOLE_PORT.println(F("*** End of crash report. It is cleared once printed. ***"));
        CONSOLE_PORT.println();
    }

    CONSOLE_PORT.println(F("==========================================================="));
    CONSOLE_PORT.println(F(" Teensy 4.1  --  interop MASTER"));
    CONSOLE_PORT.printf( " protocol v%u.%u, frame max %u bytes, CRC-16 check 0x%04X\n",
                         (unsigned)IOP_PROTOCOL_VERSION, (unsigned)IOP_PROTOCOL_MINOR,
                         (unsigned)IOP_MAX_FRAME_SIZE, (unsigned)IOP_CRC16_CHECK_VALUE);
    CONSOLE_PORT.println(F("==========================================================="));

    /* Prove the protocol implementation is intact BEFORE trusting it with real traffic.
     * A corrupt lookup table or a mismatched copy of the header presents as "the link does not
     * work", which is the least informative symptom imaginable. Ten milliseconds here turns it
     * into one explicit line. */
    selftest = iop_selftest(&gParser);
    if (selftest == 0u) {
        CONSOLE_PORT.println(F("[selftest] protocol self-test PASSED"));
    } else {
        uint16_t bit;
        CONSOLE_PORT.printf("[selftest] FAILED, mask=0x%04X\n", (unsigned)selftest);
        for (bit = 1u; bit != 0u; bit = (uint16_t)(bit << 1)) {
            if (selftest & bit) {
                CONSOLE_PORT.printf("           -> %s\n", iop_selftest_name(bit));
            }
        }
        CONSOLE_PORT.println(F("           The problem is in interop_protocol.h or in how it"));
        CONSOLE_PORT.println(F("           was copied. Do not debug the wiring yet."));
    }

    /* Enlarge the UART receive buffer BEFORE begin(), then open the link. */
    LINK_PORT.addMemoryForRead(gLinkRxBuffer, sizeof(gLinkRxBuffer));
    LINK_PORT.begin(LINK_BAUD);

    iop_parser_init(&gParser);
    iop_nack_limiter_init(&gNackLimiter);

    CONSOLE_PORT.printf("[link] Serial1 open at %lu baud (RX=pin 0, TX=pin 1)\n",
                        (unsigned long)LINK_BAUD);
    CONSOLE_PORT.printf("[link] RX buffer enlarged to %u bytes = %.1f ms of headroom "
                        "(core default is 64 bytes = 0.7 ms)\n",
                        (unsigned)sizeof(gLinkRxBuffer),
                        (double)sizeof(gLinkRxBuffer) * 10.0 * 1000.0 / (double)LINK_BAUD);
    CONSOLE_PORT.printf("[sys]  core clock %lu MHz, DWT timing resolution %lu ps\n",
                        (unsigned long)(F_CPU_ACTUAL / 1000000u),
                        (unsigned long)(1000000000000ULL / (uint64_t)F_CPU_ACTUAL));

#if LINK_USE_FLOW_CONTROL
    /* Hardware flow control, if the extra two wires are present. Both calls report success, so
     * a mis-chosen pin is a printed warning rather than a link that mysteriously stalls --
     * attachCts() in particular fails silently on a pin the XBAR cannot reach. */
    if (LINK_PORT.attachRts(LINK_RTS_PIN)) {
        CONSOLE_PORT.printf("[link] RTS on pin %d\n", (int)LINK_RTS_PIN);
    } else {
        CONSOLE_PORT.printf("[link] WARNING: RTS could not be attached to pin %d\n",
                            (int)LINK_RTS_PIN);
    }
    if (LINK_PORT.attachCts(LINK_CTS_PIN)) {
        CONSOLE_PORT.printf("[link] CTS on pin %d\n", (int)LINK_CTS_PIN);
    } else {
        CONSOLE_PORT.printf("[link] WARNING: CTS could not be attached to pin %d. That pin must "
                            "be XBAR-capable: 2,3,4,5,7,8,30,31,33,36,37,42,43,44,45\n",
                            (int)LINK_CTS_PIN);
    }
#endif

    /* Handshake: prove the link is alive before handing control to the user. */
    CONSOLE_PORT.println(F("[link] sending PING to establish the link..."));
    if (sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS)) {
        uint32_t deadline = millis() + IOP_TIMEOUT_PING_MS;
        while (gPending.active && (int32_t)(millis() - deadline) < 0) {
            linkPoll();   /* pump the parser; this is a bounded wait, not a blocking read */
        }
        if (!gPending.active && gStats.responsesOk > 0u) {
            CONSOLE_PORT.println(F("[link] LINK ESTABLISHED"));
        } else {
            clearPending("handshake");
            CONSOLE_PORT.println(F("[link] LINK FAILED -- no PONG received."));
            CONSOLE_PORT.println(F("       Check, in this order:"));
            CONSOLE_PORT.println(F("        1. Teensy pin 1 (TX) goes to the ESP32 RX pin"));
            CONSOLE_PORT.println(F("        2. Teensy pin 0 (RX) goes to the ESP32 TX pin"));
            CONSOLE_PORT.println(F("        3. GND is shared between the boards"));
            CONSOLE_PORT.println(F("        4. Both sketches use 921600 baud"));
            CONSOLE_PORT.println(F("        5. The ESP32 has actually finished booting"));
            CONSOLE_PORT.println(F("       Then type 'ping' to retry."));
        }
    }

    displayInit();

    CONSOLE_PORT.println();
    cmdHelp();
    CONSOLE_PORT.print(F("> "));
}

/* ===========================================================================================
 * MAIN LOOP
 * ===========================================================================================
 * Three non-blocking pumps, called unconditionally, forever. No delay(), no waiting on the
 * ESP32, no reading until a frame arrives. Each pump does whatever work is ready right now and
 * returns. That is what keeps the console responsive while a 12-second WiFi scan is running.
 */
void loop(void)
{
    linkPoll();      /* bytes from the ESP32 -> parser -> handlers                        */
    consolePoll();   /* characters from the user -> command interpreter                   */
    pendingPoll();   /* has an in-flight request run out of time?                          */
    displayPoll();   /* one tile row of the OLED, and only when nothing is in flight       */
}

/* ===========================================================================================
 * LINK: receiving
 * =========================================================================================== */

/* linkPoll -- drain whatever the UART has buffered and feed it to the parser.
 *
 * WHAT: reads every byte currently available (never waits for more), pushes each into the
 * state machine, and dispatches on the events that come back.
 *
 * WHY IT DRAINS RATHER THAN READING ONE BYTE PER LOOP: at 921600 baud, bytes arrive roughly
 * every 11 us. A loop() iteration that also services USB can easily take longer than that, so
 * reading a single byte per pass would fall behind the wire and overflow the ring buffer.
 * Draining fully each pass keeps the software ahead of the hardware.
 */
static void linkPoll(void)
{
    IopFrame frame;

    while (LINK_PORT.available() > 0) {
        uint8_t  b  = (uint8_t)LINK_PORT.read();
        IopEvent ev = iop_parser_push(&gParser, b, millis(), &frame);

        switch (ev) {
        case IOP_EV_FRAME:
            mirrorRawFrame(frame.payload, frame.payload_len, 'E');
            handleFrame(&frame);
            break;

        case IOP_EV_BAD_CRC:
            /* The frame arrived complete but corrupted. Report exactly what went wrong -- the
             * expected and received CRC together tell you whether this is noise (values differ
             * randomly) or a protocol mismatch (values differ consistently, every frame). */
            gStats.badCrc++;
            CONSOLE_PORT.printf("[rx] BAD CRC: got 0x%04X, computed 0x%04X, seq=%u, len=%u\n",
                                (unsigned)frame.crc_received, (unsigned)frame.crc_computed,
                                (unsigned)frame.seq, (unsigned)frame.declared_len);
            CONSOLE_PORT.println(F("     Frame DROPPED without being acted on."));
            /* Tell the other side, so its logs line up with ours. Best-effort and rate
             * limited: the CMD byte echoed back is itself untrusted, which is why it is
             * reported rather than relied upon. */
            sendErrorNack(frame.seq, frame.chan, frame.cmd, IOP_ERR_BAD_CRC);
            break;

        case IOP_EV_OVERSIZE:
            CONSOLE_PORT.printf("[rx] OVERSIZE frame: declared %u bytes, buffer holds %u. "
                                "Discarded cleanly.\n",
                                (unsigned)frame.declared_len, (unsigned)IOP_MAX_BODY_LEN);
            sendErrorNack(frame.seq, IOP_CHAN_TRANSPORT, IOP_CMD_NONE, IOP_ERR_PAYLOAD_LARGE);
            break;

        case IOP_EV_BAD_LENGTH:
            CONSOLE_PORT.println(F("[rx] frame declared a length of 0 (impossible) -- discarded"));
            break;

        case IOP_EV_TIMEOUT:
            CONSOLE_PORT.println(F("[rx] partial frame timed out mid-stream"));
            break;

        case IOP_EV_RESYNC_DISCARD:
            /* Deliberately silent in the normal case. A resync byte is expected after any
             * error, and printing one line per discarded byte would flood the console at
             * exactly the moment you need to read it. The count is available via "stats". */
            break;

        default:
            break;
        }
    }

    /* THE TIMEOUT CHECK GOES HERE, AFTER THE DRAIN, AND ONLY WHEN THE QUEUE IS EMPTY.
     *
     * Running it before the drain would let a slow loop() masquerade as a dead sender: bytes
     * that arrived on time and are sitting in the UART buffer would be thrown away because WE
     * were late reading them. Data still queued is proof the peer is alive; only silence
     * justifies a timeout. See the long note on iop_parser_tick() in the shared header. */
    if (LINK_PORT.available() == 0) {
        switch (iop_parser_tick(&gParser, millis(), &frame)) {
        case IOP_EV_OVERSIZE:
            CONSOLE_PORT.printf("[rx] OVERSIZE: declared %u body bytes then stopped sending; "
                                "buffer holds %u\n",
                                (unsigned)frame.declared_len, (unsigned)IOP_MAX_BODY_LEN);
            sendErrorNack(frame.seq, IOP_CHAN_TRANSPORT, IOP_CMD_NONE, IOP_ERR_PAYLOAD_LARGE);
            break;
        case IOP_EV_BAD_LENGTH:
            CONSOLE_PORT.println(F("[rx] frame declared length 0 then stopped -- discarded"));
            break;
        case IOP_EV_TIMEOUT:
            CONSOLE_PORT.println(F("[rx] partial frame timed out and was discarded"));
            break;
        default:
            break;
        }
    }
}

/* sendErrorNack -- emit an error NACK, subject to the rate limit.
 *
 * A NACK that answers a well-formed request goes out directly; this wrapper is only for
 * errors provoked by garbage on the wire. On a badly broken link both boards would otherwise
 * generate an error frame for every malformed frame they see, and those error frames -- also
 * arriving corrupted -- would provoke more of the same until the link carried nothing but
 * complaints about itself. Reporting a persistent fault once is useful; reporting it ten
 * thousand times a second buries the evidence. */
static void sendErrorNack(uint8_t seq, uint8_t origChan, uint8_t origCmd, uint8_t errorCode)
{
    uint16_t n;

    if (!iop_nack_should_send(&gNackLimiter, millis())) {
        return;
    }
    n = iop_build_nack(gTxBuf, sizeof(gTxBuf), seq, origChan, origCmd, errorCode);
    if (n > 0u) {
        LINK_PORT.write(gTxBuf, n);
    }
}

/* handleFrame -- a complete, CRC-valid frame arrived. Decide what it is and route it.
 *
 * The SEQ check happens HERE, once, rather than in each handler, because the rule is the same
 * for every response type and duplicating it invites drift. */
static void handleFrame(const IopFrame *f)
{
    if (gHexDump) {
        CONSOLE_PORT.printf("[rx] %s/%s seq=%u len=%u\n",
                            iop_chan_name(f->chan), iop_cmd_name(f->cmd),
                            (unsigned)f->seq, (unsigned)f->payload_len);
        hexDump("     ", f->payload, f->payload_len);
    }

    /* UNSOLICITED FRAMES FIRST.
     *
     * SEQ 0x00 means "this answers no request" -- radio news, not a reply. It must be handled
     * before the stale-response check below, because that check would otherwise discard every
     * event as an unexpected frame. Requests never use SEQ 0x00 (see iop_next_seq), so the two
     * categories cannot be confused. */
    if (IOP_SEQ_IS_UNSOLICITED(f->seq)) {
        /* GAP DETECTION -- new in v2 and the reason the sequence byte was split.
         *
         * Unsolicited frames now carry their own rolling counter, so a missed announcement is
         * VISIBLE rather than merely unfortunate. Events can be lost three ways: the radio's
         * queue overflowing before a frame is built, this board's receive ring overrunning, and
         * a UART hardware overrun the Teensy core discards without exposing any flag. In v1 all
         * of those were silent, and the brain's model of the radio would simply be wrong
         * forever. Now it says so, and 'status' can re-anchor it. */
        /* A RADIO_READY means the radio RESTARTED, so its event counter legitimately went back
         * to the start of the class. That is not packet loss and must not be counted as such --
         * doing so reported "127 events missed" the first time the radio was reflashed while the
         * brain kept running, which is exactly the false alarm that teaches people to ignore a
         * counter. Re-anchor on it instead. */
        if (f->cmd == IOP_CMD_RADIO_READY) {
            gRadio.lastEventSeq  = f->seq;
            gRadio.eventSeqValid = true;
            handleRadioReady(f);
            return;
        }

        if (gRadio.eventSeqValid) {
            uint8_t expected = iop_next_event_seq(gRadio.lastEventSeq);
            if (f->seq != expected) {
                uint8_t missed = (uint8_t)((f->seq - expected) & IOP_SEQ_COUNTER_MASK);
                gStats.eventsMissed += missed;
                CONSOLE_PORT.printf("\n[radio] MISSED %u unsolicited frame%s (expected seq 0x%02X, "
                                    "got 0x%02X)\n", (unsigned)missed, (missed == 1u) ? "" : "s",
                                    (unsigned)expected, (unsigned)f->seq);
                CONSOLE_PORT.println(F("        The cached radio state may be stale."));
                CONSOLE_PORT.println(F("        Type 'status' to re-anchor it from the radio."));
            }
        }
        gRadio.lastEventSeq  = f->seq;
        gRadio.eventSeqValid = true;

        switch (f->cmd) {
            case IOP_CMD_RADIO_READY:      handleRadioReady(f);            return;
            case IOP_CMD_WIFI_EVENT:       handleWifiEvent(f);             return;
            case IOP_CMD_MESH_DATA:        handleMeshData(f);              return;
            case IOP_CMD_MESH_NODE_EVENT:  handleMeshNodeEvent(f);         return;
            /* An autostarted ping train reports as an event, because nobody asked for it. */
            case IOP_CMD_MESH_PING_RESP:   handleMeshPingResult(f, false); return;
            default:
                CONSOLE_PORT.printf("[radio] unsolicited %s/%s that this brain does not "
                                    "understand -- ignored\n",
                                    iop_chan_name(f->chan), iop_cmd_name(f->cmd));
                return;
        }
    }

    /* STALE RESPONSE REJECTION -- this is what SEQ is for.
     *
     * If nothing is in flight, or this frame answers a different request than the one we are
     * waiting on, it is a leftover: the response to a request that already timed out. Acting
     * on it would attribute an old answer to a new question, and from then on every response
     * would be off by one. Drop it, count it, and say so. */
    if (!gPending.active || f->seq != gPending.seq) {
        gStats.staleFrames++;
        CONSOLE_PORT.printf("[rx] ignoring unexpected %s/%s (seq=%u); ",
                            iop_chan_name(f->chan), iop_cmd_name(f->cmd), (unsigned)f->seq);
        if (gPending.active) {
            CONSOLE_PORT.printf("waiting for seq=%u\n", (unsigned)gPending.seq);
        } else {
            CONSOLE_PORT.println(F("nothing was requested"));
        }
        return;
    }

    switch (f->cmd) {
        case IOP_CMD_PONG:              handlePong(f);            break;
        case IOP_CMD_WIFI_SCAN_RESP:    handleScanResponse(f);    break;
        case IOP_CMD_WIFI_CONNECT_RESP: handleConnectResponse(f); break;
        case IOP_CMD_WIFI_STATUS_RESP:  handleStatusResponse(f);  break;
        case IOP_CMD_NACK:              handleNack(f);            break;

        /* Every MESH_*_RESP shares one handler: they are replies from one subsystem and the
         * dispatch inside it is a single switch, so splitting them here would only duplicate it. */
        case IOP_CMD_MESH_INIT_RESP:
        case IOP_CMD_MESH_PEER_RESP:
        case IOP_CMD_MESH_SEND_RESP:
        case IOP_CMD_MESH_PING_RESP:
        case IOP_CMD_MESH_STATUS_RESP:
        case IOP_CMD_MESH_STREAM_RESP:
        case IOP_CMD_MESH_NODES_RESP:   handleMeshResponse(f);
                                        clearPending(NULL);       break;
        default:
            /* We do not understand this command. Say so on the wire as well as on the console:
             * the protocol is symmetric, and a silent drop would leave the ESP32 waiting. */
            CONSOLE_PORT.printf("[rx] unknown command 0x%02X from the ESP32\n",
                                (unsigned)f->cmd);
            sendErrorNack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
            clearPending(NULL);
            break;
    }
}

/* handlePong -- the link is alive. Report the measured round-trip time.
 *
 * WHY REPORT THE TIME: a PONG that takes 2 ms instead of the usual 0.2 ms is the first visible
 * sign that the ESP32 is busy, that the UART is retrying, or that something is blocking in the
 * other sketch's loop(). A bare "OK" would throw that information away. */
static void handlePong(const IopFrame *f)
{
    uint32_t rttNs = rttNanoseconds(gPending.sentCycles, ARM_DWT_CYCCNT);
    uint8_t  seq   = gPending.seq;

    (void)f;
    gStats.responsesOk++;
    clearPending(NULL);

    if (gBench.active) {
        /* Benchmarking: record and immediately fire the next one. Chaining from the response
         * rather than from a timer keeps exactly one request in flight, so each measurement is
         * a clean round trip with nothing queued behind it. */
        benchRecord(rttNs);
        if (gBench.remaining > 0u) {
            gBench.remaining--;
            (void)sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS);
        } else {
            benchFinish();
        }
        return;
    }

    /* Report microseconds with three decimals -- the underlying resolution at 600 MHz is
     * 1.67 ns, so the fractional digits are real measurement, not decoration. */
    CONSOLE_PORT.printf("[ping] PONG seq=%u, round trip %lu.%03lu us\n",
                        (unsigned)seq,
                        (unsigned long)(rttNs / 1000u),
                        (unsigned long)(rttNs % 1000u));
}

/* handleScanResponse -- decode and print the network list.
 *
 * Every field is bounds-checked against the payload length as it is consumed. That is not
 * paranoia: the CRC proves the bytes are what the ESP32 sent, but it does NOT prove the ESP32
 * built them correctly. A length field that walks off the end of a validated buffer is still
 * a buffer overrun. Validate structure separately from integrity, always. */
static void handleScanResponse(const IopFrame *f)
{
    uint16_t offset = 0;
    uint8_t  count;
    uint8_t  i;

    gStats.responsesOk++;

    if (f->payload_len < 1u) {
        CONSOLE_PORT.println(F("[scan] malformed response: payload is empty"));
        clearPending(NULL);
        return;
    }
    count = f->payload[offset++];

    if (count == 0u) {
        CONSOLE_PORT.println(F("[scan] complete: 0 networks found."));
        CONSOLE_PORT.println(F("       That is a valid result, not an error -- it means the"));
        CONSOLE_PORT.println(F("       radio scanned and heard nothing (shielded room, all APs"));
        CONSOLE_PORT.println(F("       on 5 GHz, or antenna not connected)."));
        clearPending(NULL);
        return;
    }

    CONSOLE_PORT.printf("[scan] complete: %u network%s\n",
                        (unsigned)count, (count == 1u) ? "" : "s");
    CONSOLE_PORT.println(F("  #  RSSI  CH  SIGNAL      SECURITY    SSID"));
    CONSOLE_PORT.println(F("  -- ----- --  ----------  ----------  --------------------------------"));

    for (i = 0; i < count; i++) {
        int8_t   rssi;
        uint8_t  channel;
        uint8_t  encryption;
        uint8_t  ssidLen;
        uint8_t  bars;
        uint8_t  b;
        uint16_t j;

        /* Enough bytes left for this record's fixed part? */
        if ((uint16_t)(offset + IOP_SCAN_REC_FIXED) > f->payload_len) {
            CONSOLE_PORT.printf("[scan] truncated: record %u runs past the payload\n",
                                (unsigned)i);
            break;
        }

        /* THE SIGNED-RSSI CAST. The wire carries a two's-complement byte, so -67 dBm arrives
         * as 0xBD. Reading it as unsigned prints 189 and every signal looks superb. */
        rssi       = (int8_t)f->payload[offset++];
        channel    = f->payload[offset++];
        encryption = f->payload[offset++];
        ssidLen    = f->payload[offset++];

        if ((uint16_t)(offset + ssidLen) > f->payload_len) {
            CONSOLE_PORT.printf("[scan] truncated: SSID of record %u runs past the payload\n",
                                (unsigned)i);
            break;
        }

        /* A crude signal meter. Cheap, and far easier to scan visually than raw dBm when you
         * are deciding which network to join. Thresholds are the conventional ones. */
        if      (rssi >= -55) { bars = 4; }
        else if (rssi >= -67) { bars = 3; }
        else if (rssi >= -78) { bars = 2; }
        else if (rssi >= -87) { bars = 1; }
        else                  { bars = 0; }

        /* Skip this row rather than stall the loop if the USB host is not draining. The
         * row count is still correct above; a dropped row is counted in 'stats'. */
        if (!consoleCanWrite(72u)) {
            offset = (uint16_t)(offset + ssidLen);
            continue;
        }
        CONSOLE_PORT.printf("  %2u  %4d  %2u  ", (unsigned)(i + 1u), (int)rssi,
                            (unsigned)channel);
        for (b = 0; b < 4u; b++) {
            CONSOLE_PORT.print((b < bars) ? F("#") : F("."));
        }
        CONSOLE_PORT.printf("      %-10s  ", iop_enc_name(encryption));

        /* Print the SSID byte by byte, substituting '.' for anything unprintable.
         *
         * WHY NOT JUST PRINT IT: an SSID is an arbitrary octet string. It can contain control
         * characters, embedded nulls, or raw bytes that a terminal will interpret as escape
         * sequences -- which at best garbles your console and at worst repositions the cursor
         * over text you were reading. Sanitising on output is the cheap, correct answer. It
         * also means a hidden network (length 0) prints as an explicit marker rather than as
         * an ambiguous blank. */
        if (ssidLen == 0u) {
            CONSOLE_PORT.print(F("<hidden>"));
        } else {
            for (j = 0; j < ssidLen; j++) {
                uint8_t c = f->payload[offset + j];
                CONSOLE_PORT.write((c >= 0x20u && c < 0x7Fu) ? (char)c : '.');
            }
        }
        CONSOLE_PORT.println();
        offset = (uint16_t)(offset + ssidLen);
    }

    /* v1.1: an optional trailing byte says how many networks the radio actually SAW. Absent on
     * an older radio, in which case what was sent is all there was. */
    if (f->flags & IOP_FLAG_TRUNCATED) {
        CONSOLE_PORT.println(F("[scan] the radio set the TRUNCATED flag on this frame."));
    }
    if (offset < f->payload_len) {
        uint8_t totalFound = f->payload[offset];
        if (totalFound > count) {
            CONSOLE_PORT.printf("[scan] TRUNCATED: %u of %u networks shown -- the rest did not "
                                "fit in one %u-byte frame.\n",
                                (unsigned)count, (unsigned)totalFound,
                                (unsigned)IOP_MAX_FRAME_SIZE);
            CONSOLE_PORT.println(F("       This list is a SUBSET. Do not rank access points"));
            CONSOLE_PORT.println(F("       from it and assume you saw the best one."));
        }
    } else if (count >= IOP_SCAN_MAX_NETWORKS) {
        CONSOLE_PORT.printf("[scan] NOTE: at the %u-network cap for a %u-byte frame; there may "
                            "be more.\n",
                            (unsigned)IOP_SCAN_MAX_NETWORKS, (unsigned)IOP_MAX_FRAME_SIZE);
    }
    clearPending(NULL);
}

/* handleConnectResponse -- report the outcome of a join attempt in plain language. */
static void handleConnectResponse(const IopFrame *f)
{
    uint8_t status;

    gStats.responsesOk++;

    /* Accept 5 bytes (status + IP) or 6 (with the raw reason appended). Being liberal about a
     * trailing field costs one comparison and lets the two boards be upgraded independently --
     * an older radio still works with a newer brain. Anything SHORTER is malformed, and saying
     * so precisely beats soldiering on with a partial record. */
    if (f->payload_len < 5u) {
        CONSOLE_PORT.printf("[connect] malformed response: expected at least 5 payload bytes, "
                            "got %u\n", (unsigned)f->payload_len);
        clearPending(NULL);
        return;
    }

    status = f->payload[0];
    if (f->payload_len >= 6u) {
        gRadio.lastReason = f->payload[5];
    }
    if (status == IOP_WIFI_STATUS_CONNECTED) {
        gRadio.associated = true;
        gRadio.hasIp      = true;
        gRadio.ip[0] = f->payload[1];
        gRadio.ip[1] = f->payload[2];
        gRadio.ip[2] = f->payload[3];
        gRadio.ip[3] = f->payload[4];
        CONSOLE_PORT.printf("[connect] CONNECTED. IP address %u.%u.%u.%u\n",
                            (unsigned)f->payload[1], (unsigned)f->payload[2],
                            (unsigned)f->payload[3], (unsigned)f->payload[4]);
    } else {
        CONSOLE_PORT.printf("[connect] FAILED: %s (status 0x%02X)\n",
                            iop_wifi_status_name(status), (unsigned)status);
        /* The raw code, not just the radio's summary. This is the number to reason about: the
         * radio maps several distinct reasons onto one status, and the distinctions it discards
         * are exactly the ones that decide whether retrying is sensible. */
        if (f->payload_len >= 6u && f->payload[5] != 0u) {
            CONSOLE_PORT.printf("          raw 802.11 reason %u: %s\n",
                                (unsigned)f->payload[5],
                                iop_wifi_reason_name(f->payload[5]));
        }
        /* Say what to do next, not just what went wrong. */
        switch (status) {
            case IOP_WIFI_STATUS_WRONG_PASSWORD:
                CONSOLE_PORT.println(F("          The AP rejected the credentials. Check the"));
                CONSOLE_PORT.println(F("          passphrase; note it is case sensitive."));
                break;
            case IOP_WIFI_STATUS_NO_NETWORK:
                CONSOLE_PORT.println(F("          No AP with that SSID was heard. Run 'scan'"));
                CONSOLE_PORT.println(F("          to see what is actually in range. Remember"));
                CONSOLE_PORT.println(F("          the ESP32 radio is 2.4 GHz only."));
                break;
            case IOP_WIFI_STATUS_TIMEOUT:
                CONSOLE_PORT.println(F("          The AP was found but the join did not finish"));
                CONSOLE_PORT.println(F("          in time. Weak signal or a slow DHCP server."));
                break;
            default:
                break;
        }
    }
    clearPending(NULL);
}

/* handleRadioReady -- the radio has (re)started.
 *
 * This is the most important unsolicited frame in the protocol, because of one asymmetry: the
 * radio can reset without the brain resetting. A brownout when the transmitter keys up, a
 * watchdog, a nudged USB connector -- any of these returns the radio to a blank state while
 * this board carries on believing it is still associated with an access point.
 *
 * Without this frame the brain finds out minutes later, when something that should work does
 * not, and the symptom points nowhere near the cause. With it, the reboot is announced, the
 * cached state is invalidated immediately, and RESET_REASON usually names the culprit outright.
 */
static void handleRadioReady(const IopFrame *f)
{
    if (f->payload_len < 6u) {
        CONSOLE_PORT.println(F("[radio] malformed RADIO_READY"));
        return;
    }

    gRadio.protocolVersion = f->payload[0];
    gRadio.protocolMinor   = f->payload[1];
    gRadio.crcCheckValue   = (uint16_t)(((uint16_t)f->payload[2] << 8) | f->payload[3]);
    gRadio.resetReason     = f->payload[4];
    gRadio.capabilities    = f->payload[5];
    gRadio.reboots++;

    /* The radio restarted, so everything this brain believed about the link is now false.
     * Clearing it here rather than letting it decay is the whole point of being told. */
    gRadio.associated = false;
    gRadio.hasIp      = false;
    gRadio.ready      = true;

    CONSOLE_PORT.println();
    if (gRadio.reboots > 1u) {
        CONSOLE_PORT.println(F("[radio] *** THE RADIO RESTARTED WITHOUT BEING ASKED ***"));
        CONSOLE_PORT.println(F("        Any WiFi connection it had is gone. Cached state here"));
        CONSOLE_PORT.println(F("        has been invalidated."));
    } else {
        CONSOLE_PORT.println(F("[radio] radio announced itself"));
    }
    CONSOLE_PORT.printf("        reset reason ... %s\n", espResetReasonName(gRadio.resetReason));
    CONSOLE_PORT.printf("        capabilities ... WiFi:%s BLE:%s\n",
                        (gRadio.capabilities & IOP_CAP_WIFI) ? "yes" : "no",
                        (gRadio.capabilities & IOP_CAP_BLE)  ? "yes" : "no");

    /* RUNTIME PROTOCOL VERIFICATION. Both boards compile their own copy of interop_protocol.h,
     * and the classic failure is uploading a change to one board only. Comparing the version
     * and the CRC check value on the very first frame of the session catches that instantly --
     * instead of it presenting as an endless stream of inexplicable CRC errors. */
    if (gRadio.protocolVersion != IOP_PROTOCOL_VERSION ||
        gRadio.crcCheckValue   != IOP_CRC16_CHECK_VALUE) {
        CONSOLE_PORT.println(F("        *** PROTOCOL MISMATCH ***"));
        CONSOLE_PORT.printf("        radio: v%u.%u, CRC-16 check 0x%04X\n",
                            (unsigned)gRadio.protocolVersion, (unsigned)gRadio.protocolMinor,
                            (unsigned)gRadio.crcCheckValue);
        CONSOLE_PORT.printf("        brain: v%u.%u, CRC-16 check 0x%04X\n",
                            (unsigned)IOP_PROTOCOL_VERSION, (unsigned)IOP_PROTOCOL_MINOR,
                            (unsigned)IOP_CRC16_CHECK_VALUE);
        CONSOLE_PORT.println(F("        The two boards are running different builds of"));
        CONSOLE_PORT.println(F("        interop_protocol.h. Run tools/sync_headers.py and"));
        CONSOLE_PORT.println(F("        re-upload BOTH boards. Do not debug anything else."));
        /* LATCH IT. Previously this printed a warning and then carried on as if nothing were
         * wrong, so the one line that identified the fault scrolled away and every subsequent
         * symptom looked like a wiring problem. Latched, it gates WiFi commands (below), shows
         * on the display, and appears in 'radio' and 'sys' until the boards are re-synced. */
        gRadio.mismatch = true;
    } else {
        gRadio.mismatch = false;
        CONSOLE_PORT.printf("        protocol ....... v%u.%u, CRC-16 0x%04X (matches this board)\n",
                            (unsigned)gRadio.protocolVersion, (unsigned)gRadio.protocolMinor,
                            (unsigned)gRadio.crcCheckValue);
    }
    CONSOLE_PORT.print(F("> "));
}

/* handleWifiEvent -- asynchronous radio news. */
static void handleWifiEvent(const IopFrame *f)
{
    uint8_t type;
    uint8_t reason;

    if (f->payload_len < 6u) {
        CONSOLE_PORT.println(F("[radio] malformed WIFI_EVENT"));
        return;
    }
    type   = f->payload[0];
    reason = f->payload[1];

    switch (type) {
    case IOP_WIFI_EVT_CONNECTED:
        gRadio.associated = true;
        CONSOLE_PORT.println(F("\n[radio] associated with an access point"));
        break;

    case IOP_WIFI_EVT_GOT_IP:
        gRadio.associated = true;
        gRadio.hasIp      = true;
        gRadio.ip[0] = f->payload[2];
        gRadio.ip[1] = f->payload[3];
        gRadio.ip[2] = f->payload[4];
        gRadio.ip[3] = f->payload[5];
        CONSOLE_PORT.printf("\n[radio] got address %u.%u.%u.%u\n",
                            (unsigned)gRadio.ip[0], (unsigned)gRadio.ip[1],
                            (unsigned)gRadio.ip[2], (unsigned)gRadio.ip[3]);
        break;

    case IOP_WIFI_EVT_DISCONNECTED:
        gRadio.associated = false;
        gRadio.hasIp      = false;
        gRadio.lastReason = reason;
        gRadio.disconnects++;
        /* Print the RAW reason, decoded. This is the number the brain reasons about; the
         * friendly summary in a connect response is derived from it and is lossy. */
        CONSOLE_PORT.printf("\n[radio] disconnected: %s (raw 802.11 reason %u)\n",
                            iop_wifi_reason_name(reason), (unsigned)reason);
        break;

    case IOP_WIFI_EVT_SCAN_DONE:
        CONSOLE_PORT.println(F("\n[radio] a scan completed"));
        break;

    default:
        CONSOLE_PORT.printf("\n[radio] unknown event type 0x%02X\n", (unsigned)type);
        break;
    }
    CONSOLE_PORT.print(F("> "));
}

/* espResetReasonName -- decode the radio's reset cause.
 *
 * These are esp_reset_reason_t values, forwarded verbatim by the radio. The radio does not
 * interpret them, and it should not: the brain is what reports to the human, so the brain owns
 * the words. Values verified against esp_system.h in the installed ESP32 core.
 *
 * BROWNOUT is the one to watch for on this hardware. It means the 3.3 V rail sagged -- almost
 * always when the WiFi transmitter keyed up and pulled its ~300 mA peak. That is a power
 * problem, not a protocol problem, and it is exactly what the bulk capacitance between these
 * boards is there to prevent. */
static const char *espResetReasonName(uint8_t reason)
{
    switch (reason) {
        case 0:  return "unknown";
        case 1:  return "power-on (normal cold start)";
        case 2:  return "external reset pin";
        case 3:  return "software restart";
        case 4:  return "PANIC -- the radio firmware crashed";
        case 5:  return "interrupt watchdog";
        case 6:  return "task watchdog";
        case 7:  return "watchdog";
        case 8:  return "woke from deep sleep";
        case 9:  return "BROWNOUT -- the 3.3V rail sagged; check power and bulk capacitance";
        case 10: return "SDIO";
        case 11: return "USB peripheral";
        case 12: return "JTAG";
        case 13: return "eFuse error";
        default: return "unrecognised reset reason";
    }
}

/* ============================================================================================
 *  MESH -- the brain directing the ESP-NOW fleet through the hub
 * ============================================================================================ */

/* meshFindNode / meshTouchNode -- locate or create this brain's view of a node.
 *
 * Linear scan over 8 entries. At 200 Hz that is at most 8 six-byte compares per frame, roughly a
 * microsecond on a 600 MHz M7 -- measurably cheaper than a hash table would be at this size, and
 * far easier to be certain is correct. Reach for the clever structure when the measurement says
 * to, not before. */
static int meshFindNode(const uint8_t *mac)
{
    uint8_t i;
    for (i = 0; i < BRAIN_MAX_NODES; i++) {
        if (gMeshNodes[i].used && memcmp(gMeshNodes[i].mac, mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int meshTouchNode(const uint8_t *mac)
{
    uint8_t i;
    int idx = meshFindNode(mac);
    if (idx >= 0) {
        return idx;
    }
    for (i = 0; i < BRAIN_MAX_NODES; i++) {
        if (!gMeshNodes[i].used) {
            memset(&gMeshNodes[i], 0, sizeof(gMeshNodes[i]));
            memcpy(gMeshNodes[i].mac, mac, 6);
            gMeshNodes[i].used  = true;
            gMeshNodes[i].alive = true;
            return (int)i;
        }
    }
    return -1;   /* table full: a real fleet limit, reported rather than silently overwritten */
}

static void meshPrintMac(const uint8_t *mac)
{
    CONSOLE_PORT.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* handleMeshData -- an unsolicited node payload, relayed by the hub.
 *
 * Wire layout: [src_mac 6][the node's ESP-NOW payload, verbatim].
 *
 * THE PAYLOAD IS DECODED HERE, on the brain, and that is the point. The node sends raw sensor
 * counts; the hub forwards them without looking; this board -- which has an FPU, knows the
 * configured full-scale range, and is where the application lives -- is the first place that
 * turns them into physical units. Same rule as raw 802.11 reason codes: move the fact, interpret
 * at the top.
 *
 * Every field is bounds-checked against payload_len before it is read. The CRC proves the bytes
 * survived the wire; it proves nothing about whether the hub built a well-formed body. */
static void handleMeshData(const IopFrame *f)
{
    const uint8_t *mac;
    const uint8_t *body;
    uint16_t bodyLen;
    int idx;

    if (f->payload_len < IOP_MESH_MAC_LEN + 1u) {
        return;                     /* too short to contain a MAC and a type byte */
    }
    mac     = f->payload;
    body    = f->payload + IOP_MESH_MAC_LEN;
    bodyLen = (uint16_t)(f->payload_len - IOP_MESH_MAC_LEN);

    gMesh.dataFrames++;

    idx = meshTouchNode(mac);
    if (idx < 0) {
        return;
    }
    gMeshNodes[idx].alive  = true;
    gMeshNodes[idx].lastMs = millis();
    gMeshNodes[idx].frames++;

    /* HELLO -- a node answering 'who are you'. Identity is stored, never printed from here:
     * this runs in the receive path and a broadcast identify would print N times at once. */
    if (body[0] == MESH_MSG_HELLO && bodyLen >= 6u) {
        gMeshNodes[idx].identified = true;
        gMeshNodes[idx].fwMajor    = body[1];
        gMeshNodes[idx].fwMinor    = body[2];
        gMeshNodes[idx].caps       = body[3];
        gMeshNodes[idx].sensorOk   = body[4];
        gMeshNodes[idx].whoAmI     = body[5];
        if (consoleCanWrite(96)) {
            CONSOLE_PORT.printf("\n[mesh] ");
            meshPrintMac(mac);
            CONSOLE_PORT.printf(" is fw %u.%u caps=0x%02X sensor=%s WHO_AM_I=0x%02X\n",
                                (unsigned)body[1], (unsigned)body[2], (unsigned)body[3],
                                body[4] ? "ok" : "FAIL", (unsigned)body[5]);
        }
        return;
    }

    if (body[0] == MESH_MSG_SAMPLE && bodyLen >= 12u) {
        /* Read the packed fields by offset rather than casting to MeshSampleMsg*. The payload
         * sits at an arbitrary offset inside the frame buffer, and the Cortex-M7 will fault or
         * silently mis-read on an unaligned 32-bit access to a packed struct. Byte reads always
         * work and cost nothing measurable at this rate. */
        /* Offsets come from MESH_SAMPLE_OFF_* in mesh_protocol.h, which are derived from the
         * struct itself rather than counted by hand.
         *
         * THEY WERE COUNTED BY HAND ONCE, AND THEY WERE WRONG. The first version of this decoder
         * used 10 / 12 / 14 instead of 8 / 10 / 12 -- someone (me) mis-added the field widths and
         * every offset after t_first_us was two bytes late. Nothing failed: no CRC error, no
         * bounds violation, no NACK. The console simply printed
         *
         *     lat=1us a=0 0 0 g=0 0 0
         *
         * for a real accelerometer sitting on a desk, which should have read about 16384 on one
         * axis from gravity alone. An all-zero reading from a live sensor is not a plausible
         * measurement, and that implausibility was the only signal anything was wrong.
         *
         * The lesson is the fix: NEVER hand-count offsets into a packed struct that is defined in
         * a header you already share. Derive them, so the definition and the decoder cannot
         * disagree. */
        uint16_t seq   = (uint16_t)(body[MESH_SAMPLE_OFF_SEQ] |
                                    ((uint16_t)body[MESH_SAMPLE_OFF_SEQ + 1] << 8));
        uint16_t i2s   = (uint16_t)(body[MESH_SAMPLE_OFF_LATENCY] |
                                    ((uint16_t)body[MESH_SAMPLE_OFF_LATENCY + 1] << 8));
        uint8_t  count = (bodyLen > MESH_SAMPLE_OFF_COUNT) ? body[MESH_SAMPLE_OFF_COUNT] : 0u;

        if (gMeshNodes[idx].seqValid && seq != (uint16_t)(gMeshNodes[idx].lastSeq + 1u)) {
            gMeshNodes[idx].seqGaps++;
        }
        gMeshNodes[idx].lastSeq     = seq;
        gMeshNodes[idx].seqValid    = true;
        gMeshNodes[idx].mode        = body[1];
        gMeshNodes[idx].intToSendUs = i2s;

        /* Take the LAST sample in the packet: in batched mode that is the newest, and in
         * on-interrupt mode it is the only one. Showing the newest is what a live display wants.
         * The bounds check uses the derived stride, so it stays correct if MeshSample ever
         * gains a field. */
        if (count > 0u) {
            uint16_t off = (uint16_t)(MESH_SAMPLE_OFF_SAMPLES +
                                      ((uint16_t)(count - 1u) * MESH_SAMPLE_STRIDE));
            if ((uint32_t)off + MESH_SAMPLE_STRIDE <= (uint32_t)bodyLen) {
                gMeshNodes[idx].ax = (int16_t)(body[off + 0] | ((uint16_t)body[off + 1] << 8));
                gMeshNodes[idx].ay = (int16_t)(body[off + 2] | ((uint16_t)body[off + 3] << 8));
                gMeshNodes[idx].az = (int16_t)(body[off + 4] | ((uint16_t)body[off + 5] << 8));
                gMeshNodes[idx].gx = (int16_t)(body[off + 6] | ((uint16_t)body[off + 7] << 8));
                gMeshNodes[idx].gy = (int16_t)(body[off + 8] | ((uint16_t)body[off + 9] << 8));
                gMeshNodes[idx].gz = (int16_t)(body[off + 10] | ((uint16_t)body[off + 11] << 8));
            }
        }

        /* Printing is gated twice: by the operator's 'mesh watch', and by whether the USB host is
         * actually draining. Without the second guard a 200 Hz print would block loop() for
         * 120 ms at a time and destroy the very latency this project measures. */
        if (gMeshWatch && consoleCanWrite(96)) {
            CONSOLE_PORT.printf("[mesh] ");
            meshPrintMac(mac);
            CONSOLE_PORT.printf(" seq=%u a=%6d %6d %6d g=%6d %6d %6d lat=%uus\n",
                                (unsigned)seq,
                                gMeshNodes[idx].ax, gMeshNodes[idx].ay, gMeshNodes[idx].az,
                                gMeshNodes[idx].gx, gMeshNodes[idx].gy, gMeshNodes[idx].gz,
                                (unsigned)i2s);
        }
    }
}

/* handleMeshNodeEvent -- a node appeared or went silent.
 *
 * These are NOT gated by the stream subscription, deliberately. A node dying is a rare fact the
 * brain must not miss, and it is most important precisely when the data stream is turned off. */
static void handleMeshNodeEvent(const IopFrame *f)
{
    int idx;
    if (f->payload_len < 7u) {
        return;
    }
    idx = meshTouchNode(&f->payload[1]);

    CONSOLE_PORT.printf("\n[mesh] node ");
    meshPrintMac(&f->payload[1]);
    if (f->payload[0] == IOP_MESH_EVT_NODE_SEEN) {
        CONSOLE_PORT.println(F(" APPEARED"));
        if (idx >= 0) { gMeshNodes[idx].alive = true; }
    } else if (f->payload[0] == IOP_MESH_EVT_NODE_LOST) {
        CONSOLE_PORT.println(F(" LOST (silent past the liveness window)"));
        if (idx >= 0) { gMeshNodes[idx].alive = false; }
    } else {
        CONSOLE_PORT.printf(" event 0x%02X (unknown to this brain)\n", (unsigned)f->payload[0]);
    }
}

/* handleMeshPingResult -- decode a finished ping train.
 *
 * Arrives either as a RESPONSE (this brain asked) or as an EVENT (the hub autostarted one). The
 * decode is identical; only the framing differs, which is exactly as it should be -- the meaning
 * of a measurement does not depend on who asked for it. */
static void handleMeshPingResult(const IopFrame *f, bool solicited)
{
    uint32_t mn, mean, mx;

    if (f->payload_len < 14u) {
        CONSOLE_PORT.println(F("[mesh] ping result too short to decode"));
        return;
    }
    gMesh.pingSent = (uint16_t)((f->payload[1] << 8) | f->payload[2]);
    gMesh.pingRecv = (uint16_t)((f->payload[3] << 8) | f->payload[4]);
    mn   = ((uint32_t)f->payload[5] << 24) | ((uint32_t)f->payload[6] << 16) |
           ((uint32_t)f->payload[7] << 8)  | (uint32_t)f->payload[8];
    mean = ((uint32_t)f->payload[9] << 16) | ((uint32_t)f->payload[10] << 8) |
           (uint32_t)f->payload[11];
    mx   = ((uint32_t)f->payload[12] << 8) | (uint32_t)f->payload[13];

    gMesh.pingMinUs  = mn;
    gMesh.pingMeanUs = mean;
    gMesh.pingMaxUs  = mx;
    gMesh.pingValid  = true;

    CONSOLE_PORT.printf("\n[mesh] ESP-NOW round trip%s\n",
                        solicited ? "" : " (hub autostart -- nobody asked)");
    CONSOLE_PORT.printf("        delivered .... %u of %u",
                        (unsigned)gMesh.pingRecv, (unsigned)gMesh.pingSent);
    if (gMesh.pingSent > 0u) {
        CONSOLE_PORT.printf("  (%u%% loss)",
                            (unsigned)(100u - (100u * gMesh.pingRecv) / gMesh.pingSent));
    }
    CONSOLE_PORT.println();
    if (gMesh.pingRecv == 0u) {
        CONSOLE_PORT.println(F("        No node answered. Check the channel matches."));
        return;
    }
    CONSOLE_PORT.printf("        min .......... %lu us\n", (unsigned long)mn);
    CONSOLE_PORT.printf("        mean ......... %lu us\n", (unsigned long)mean);
    CONSOLE_PORT.printf("        max .......... %lu us\n", (unsigned long)mx);
    CONSOLE_PORT.printf("        one way ~..... %lu us  (mean / 2)\n", (unsigned long)(mean / 2u));
    CONSOLE_PORT.println(F("        Timed start-to-stop on the HUB's clock alone, so no"));
    CONSOLE_PORT.println(F("        agreement between the two boards' oscillators is assumed."));
}

/* handleMeshResponse -- replies to commands this brain issued on the MESH channel. */
static void handleMeshResponse(const IopFrame *f)
{
    gStats.responsesOk++;

    switch (f->cmd) {
    case IOP_CMD_MESH_INIT_RESP:
        if (f->payload_len < 8u) { break; }
        gMesh.state   = f->payload[0];
        gMesh.channel = f->payload[1];
        CONSOLE_PORT.printf("[mesh] %s on channel %u, hub MAC ",
                            (f->payload[0] == IOP_MESH_STATE_READY) ? "READY" : "FAILED",
                            (unsigned)f->payload[1]);
        meshPrintMac(&f->payload[2]);
        CONSOLE_PORT.println();
        break;

    case IOP_CMD_MESH_STREAM_RESP:
        if (f->payload_len < 2u) { break; }
        gMesh.stream = f->payload[1];
        if (f->payload[1] == IOP_MESH_STREAM_OFF) {
            CONSOLE_PORT.println(F("[mesh] stream OFF -- the hub still counts every packet"));
        } else if (f->payload[1] == IOP_MESH_STREAM_ALL) {
            CONSOLE_PORT.println(F("[mesh] stream ON -- every packet forwarded"));
        } else {
            CONSOLE_PORT.printf("[mesh] stream 1-in-%u\n", (unsigned)f->payload[1]);
        }
        break;

    case IOP_CMD_MESH_PEER_RESP:
        if (f->payload_len < 2u) { break; }
        CONSOLE_PORT.printf("[mesh] peer op %s, %u peer(s) registered\n",
                            (f->payload[0] == IOP_MESH_STATUS_OK) ? "ok" : "FAILED",
                            (unsigned)f->payload[1]);
        break;

    case IOP_CMD_MESH_SEND_RESP:
        CONSOLE_PORT.printf("[mesh] send %s\n",
                            (f->payload_len > 0u && f->payload[0] == IOP_MESH_STATUS_OK)
                                ? "queued" : "FAILED");
        break;

    case IOP_CMD_MESH_PING_RESP:
        handleMeshPingResult(f, true);
        break;

    /* NODES_RESP -- the fleet roster, polled. This is how a brain that rebooted (or was flashed
     * while the hub kept running) learns the MACs it missed the NODE_SEEN events for. */
    case IOP_CMD_MESH_NODES_RESP: {
        uint8_t count, i;
        uint16_t off = 1u;
        if (f->payload_len < 1u) { break; }
        count = f->payload[0];
        CONSOLE_PORT.printf("[mesh] fleet roster from the hub: %u node%s\n",
                            (unsigned)count, (count == 1u) ? "" : "s");
        for (i = 0; i < count; i++) {
            int idx;
            uint32_t pkts;
            /* Bounds-check every record before reading it. The count byte is the hub's claim;
             * payload_len is the only fact. A mismatch must truncate, never over-read. */
            if ((uint32_t)off + 11u > (uint32_t)f->payload_len) {
                CONSOLE_PORT.printf("       (truncated after %u -- hub claimed %u)\n",
                                    (unsigned)i, (unsigned)count);
                break;
            }
            idx  = meshTouchNode(&f->payload[off]);
            pkts = ((uint32_t)f->payload[off + 7] << 24) | ((uint32_t)f->payload[off + 8] << 16) |
                   ((uint32_t)f->payload[off + 9] << 8)  | (uint32_t)f->payload[off + 10];
            CONSOLE_PORT.printf("       [%u] ", (unsigned)i);
            meshPrintMac(&f->payload[off]);
            CONSOLE_PORT.printf("  %-5s  hub saw %lu packet%s\n",
                                f->payload[off + 6] ? "alive" : "LOST",
                                (unsigned long)pkts, (pkts == 1ul) ? "" : "s");
            if (idx >= 0) { gMeshNodes[idx].alive = (f->payload[off + 6] != 0u); }
            off = (uint16_t)(off + 11u);
        }
        if (count == 0u) {
            CONSOLE_PORT.println(F("       none -- the hub has heard from nobody yet"));
        } else {
            CONSOLE_PORT.println(F("       address them by index: 'mesh mode 0 idle', "
                                   "'mesh ping 0'"));
        }
        break;
    }

    case IOP_CMD_MESH_STATUS_RESP:
        if (f->payload_len < 10u) { break; }
        gMesh.state   = f->payload[0];
        gMesh.channel = f->payload[1];
        gMesh.mode    = f->payload[2];
        gMesh.hubRx   = ((uint32_t)f->payload[5] << 24) | ((uint32_t)f->payload[6] << 16) |
                        ((uint32_t)f->payload[7] << 8)  | (uint32_t)f->payload[8];
        /* Byte 10 was appended after the first release. Older hubs simply stop at 10 bytes, so
         * check the length rather than assuming -- this is the forward-compatibility rule the
         * whole protocol is built on, applied to our own recent change. */
        if (f->payload_len >= 11u) {
            gMesh.stream = f->payload[10];
        }
        gMesh.polled = true;
        CONSOLE_PORT.printf("[mesh] hub says: %s, ch %u, mode %s, %u peer(s) %u alive, "
                            "rx %lu, dropped %u\n",
                            (f->payload[0] == IOP_MESH_STATE_READY) ? "ready" :
                            (f->payload[0] == IOP_MESH_STATE_OFF)   ? "off" : "ERROR",
                            (unsigned)f->payload[1],
                            (f->payload[2] == IOP_MESH_MODE_ONLY) ? "mesh-only" : "with-wifi",
                            (unsigned)f->payload[3], (unsigned)f->payload[4],
                            (unsigned long)gMesh.hubRx, (unsigned)f->payload[9]);
        break;

    default:
        CONSOLE_PORT.printf("[mesh] unhandled response %s\n", iop_cmd_name(f->cmd));
        break;
    }
}

/* meshResolveTarget -- turn a console token into a destination MAC.
 *
 * Accepts an index from 'mesh nodes' ("0"), the literal "all" (the ESP-NOW broadcast address), or
 * a full MAC ("1C:DB:D4:46:D5:0C"). Returns false and explains itself on anything else.
 *
 * WHY INDICES AND NOT JUST MACs. Typing a MAC correctly every time is how operators make mistakes
 * at 2am. The index is stable for as long as the roster is, and 'mesh nodes' prints the mapping
 * immediately above wherever it will be used.
 *
 * WHY BROADCAST IS A FIRST-CLASS TARGET. Commanding a fleet means one packet reaching every node,
 * not N packets reaching a list. Broadcast is what makes the Teensy peer-like rather than a
 * remote control that happens to have several channels -- and it is the only way to make N nodes
 * change state at the SAME instant, which matters the moment anyone tries to correlate their
 * data. */
static bool meshResolveTarget(const char *tok, uint8_t *macOut, bool *isBroadcast)
{
    static const uint8_t kBroadcast[6] = MESH_BROADCAST_MAC;
    unsigned int b[6];
    int i;

    *isBroadcast = false;

    if (tok == NULL) {
        /* No target given: the first live node. Convenience for the common one-node case, and it
         * refuses rather than guessing when the roster is empty. */
        for (i = 0; i < (int)BRAIN_MAX_NODES; i++) {
            if (gMeshNodes[i].used && gMeshNodes[i].alive) {
                memcpy(macOut, gMeshNodes[i].mac, 6);
                return true;
            }
        }
        CONSOLE_PORT.println(F("[mesh] no live node known -- run 'mesh nodes' to poll the hub"));
        return false;
    }

    if (strcmp(tok, "all") == 0) {
        memcpy(macOut, kBroadcast, 6);
        *isBroadcast = true;
        return true;
    }

    if (sscanf(tok, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (i = 0; i < 6; i++) { macOut[i] = (uint8_t)b[i]; }
        return true;
    }

    if (tok[0] >= '0' && tok[0] <= '9' && tok[1] == '\0') {
        int idx = tok[0] - '0';
        if (idx < (int)BRAIN_MAX_NODES && gMeshNodes[idx].used) {
            memcpy(macOut, gMeshNodes[idx].mac, 6);
            return true;
        }
        CONSOLE_PORT.printf("[mesh] no node at index %d -- run 'mesh nodes'\n", idx);
        return false;
    }

    CONSOLE_PORT.printf("[mesh] cannot read '%s' as a target. Use an index from 'mesh nodes', "
                        "a full MAC, or 'all'.\n", tok);
    return false;
}

/* meshSendToNode -- wrap a node-level message in MESH_SEND_REQ and hand it to the hub.
 *
 * The hub does not inspect the bytes; it addresses the packet and transmits. So the node-level
 * protocol (mesh_protocol.h) and the link protocol (interop_protocol.h) stay completely
 * independent -- a new node message needs no hub change at all, which is the property that makes
 * the fleet extensible without touching the middle of the stack. */
static bool meshSendToNode(const uint8_t *mac, const uint8_t *msg, uint8_t msgLen)
{
    uint8_t payload[6 + 32];
    if (msgLen > 32u) {
        CONSOLE_PORT.println(F("[mesh] node message too long for this console helper"));
        return false;
    }
    memcpy(payload, mac, 6);
    memcpy(&payload[6], msg, msgLen);
    return sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_SEND_REQ, payload,
                       (uint16_t)(6u + msgLen), IOP_TIMEOUT_MESH_MS);
}

/* cmdMesh -- the fleet console.
 *
 * Sub-commands rather than eight top-level verbs, because everything here is one subsystem and
 * 'mesh' with no argument should tell you where you stand without sending anything. */
static void cmdMesh(char *args)
{
    char *sub = takeToken(&args);

    if (sub == NULL || strcmp(sub, "show") == 0) {
        uint8_t i, seen = 0;
        CONSOLE_PORT.println(F("Mesh (as modelled by this brain)"));
        CONSOLE_PORT.printf("  hub state .............. %s\n",
                            (gMesh.state == IOP_MESH_STATE_READY) ? "ready" :
                            (gMesh.state == IOP_MESH_STATE_OFF)   ? "off (or never polled)"
                                                                  : "ERROR");
        CONSOLE_PORT.printf("  channel ................ %u\n", (unsigned)gMesh.channel);
        CONSOLE_PORT.printf("  subscription ........... ");
        if (gMesh.stream == IOP_MESH_STREAM_OFF) {
            CONSOLE_PORT.println(F("OFF -- no node data reaches this board"));
        } else if (gMesh.stream == IOP_MESH_STREAM_ALL) {
            CONSOLE_PORT.println(F("every packet"));
        } else {
            CONSOLE_PORT.printf("1 in %u\n", (unsigned)gMesh.stream);
        }
        CONSOLE_PORT.printf("  data frames received ... %lu\n", (unsigned long)gMesh.dataFrames);
        if (gMesh.polled) {
            CONSOLE_PORT.printf("  hub air-side rx ........ %lu (at last poll)\n",
                                (unsigned long)gMesh.hubRx);
        }
        if (gMesh.pingValid) {
            CONSOLE_PORT.printf("  ESP-NOW round trip ..... min %lu / mean %lu / max %lu us "
                                "(%u of %u)\n",
                                (unsigned long)gMesh.pingMinUs, (unsigned long)gMesh.pingMeanUs,
                                (unsigned long)gMesh.pingMaxUs,
                                (unsigned)gMesh.pingRecv, (unsigned)gMesh.pingSent);
        }
        CONSOLE_PORT.println(F("  -- nodes --"));
        for (i = 0; i < BRAIN_MAX_NODES; i++) {
            if (!gMeshNodes[i].used) { continue; }
            seen++;
            CONSOLE_PORT.printf("  ");
            meshPrintMac(gMeshNodes[i].mac);
            CONSOLE_PORT.printf("  %-5s frames=%lu",
                                gMeshNodes[i].alive ? "alive" : "LOST",
                                (unsigned long)gMeshNodes[i].frames);
            if (gMeshNodes[i].frames > 0u) {
                CONSOLE_PORT.printf(" lat=%uus a=%d %d %d g=%d %d %d",
                                    (unsigned)gMeshNodes[i].intToSendUs,
                                    gMeshNodes[i].ax, gMeshNodes[i].ay, gMeshNodes[i].az,
                                    gMeshNodes[i].gx, gMeshNodes[i].gy, gMeshNodes[i].gz);
            }
            CONSOLE_PORT.println();
        }
        if (seen == 0u) {
            CONSOLE_PORT.println(F("  none seen yet -- 'mesh status' polls the hub"));
        }
        if (gMesh.stream != IOP_MESH_STREAM_ALL && seen > 0u) {
            CONSOLE_PORT.println(F("  NOTE: the stream is decimated, so per-node frame counts"));
            CONSOLE_PORT.println(F("        are a sampled view. The hub's totals are exact."));
        }
        return;
    }

    if (strcmp(sub, "status") == 0) {
        (void)sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_STATUS_REQ, NULL, 0, IOP_TIMEOUT_MESH_MS);
        return;
    }

    if (strcmp(sub, "init") == 0) {
        uint8_t payload[2];
        char *chTok   = takeToken(&args);
        char *modeTok = takeToken(&args);
        payload[0] = (chTok != NULL) ? (uint8_t)atoi(chTok) : 1u;
        payload[1] = (modeTok != NULL && strcmp(modeTok, "wifi") == 0)
                     ? IOP_MESH_MODE_WITH_WIFI : IOP_MESH_MODE_ONLY;
        if (payload[0] < 1u || payload[0] > 13u) {
            CONSOLE_PORT.println(F("[mesh] channel must be 1..13"));
            return;
        }
        if (sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_INIT_REQ, payload, 2, IOP_TIMEOUT_MESH_MS)) {
            CONSOLE_PORT.printf("[mesh] init channel %u, %s...\n", (unsigned)payload[0],
                                payload[1] ? "coexisting with WiFi" : "mesh only");
        }
        return;
    }

    if (strcmp(sub, "stream") == 0) {
        uint8_t divisor;
        char *tok = takeToken(&args);
        if (tok == NULL) {
            CONSOLE_PORT.println(F("[mesh] usage: mesh stream off | all | <N>"));
            CONSOLE_PORT.println(F("       off  no node data forwarded (the hub still counts it)"));
            CONSOLE_PORT.println(F("       all  every packet -- full rate, full cost"));
            CONSOLE_PORT.println(F("       N    every Nth packet, decimated on the hub"));
            return;
        }
        if (strcmp(tok, "off") == 0)      { divisor = IOP_MESH_STREAM_OFF; }
        else if (strcmp(tok, "all") == 0) { divisor = IOP_MESH_STREAM_ALL; }
        else                              { divisor = (uint8_t)atoi(tok); }
        (void)sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_STREAM_REQ, &divisor, 1,
                          IOP_TIMEOUT_MESH_MS);
        return;
    }

    if (strcmp(sub, "nodes") == 0) {
        (void)sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_NODES_REQ, NULL, 0, IOP_TIMEOUT_MESH_MS);
        return;
    }

    if (strcmp(sub, "ping") == 0) {
        uint8_t payload[7];
        uint8_t mac[6];
        bool bcast = false;
        char *target = takeToken(&args);
        char *countTok;

        if (!meshResolveTarget(target, mac, &bcast)) {
            return;
        }
        /* BROADCAST PING IS DELIBERATELY REFUSED, and the reason is worth stating rather than
         * silently allowing. The hub times one outstanding token at a time; a broadcast would
         * bring back N pongs all carrying that same token, and the hub would stop its timer on
         * whichever arrived first while attributing the result to "the fleet". That is not a
         * slow measurement, it is a meaningless one -- it would report the fastest node as if
         * it described every node. Ping nodes individually and compare. */
        if (bcast) {
            CONSOLE_PORT.println(F("[mesh] ping needs one node: N pongs share one token and the"));
            CONSOLE_PORT.println(F("       hub would time whichever arrived first. Ping each by"));
            CONSOLE_PORT.println(F("       index and compare -- 'mesh ping 0', 'mesh ping 1'."));
            return;
        }
        countTok = takeToken(&args);
        memcpy(payload, mac, 6);
        payload[6] = (countTok != NULL) ? (uint8_t)atoi(countTok) : 30u;
        if (sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_PING_REQ, payload, 7,
                        IOP_TIMEOUT_MESH_PING_MS)) {
            CONSOLE_PORT.printf("[mesh] %u round trips to ", (unsigned)payload[6]);
            meshPrintMac(mac);
            CONSOLE_PORT.println(F(", timed on the hub clock..."));
        }
        return;
    }

    /* mesh mode <target> <interrupt|batched|idle> [rate_hz] [batch]
     *
     * This is the command that makes the Teensy a controller rather than a listener: it changes
     * what a remote board DOES, not merely what this board hears about it. */
    if (strcmp(sub, "mode") == 0) {
        uint8_t mac[6];
        bool bcast = false;
        char *target  = takeToken(&args);
        char *modeTok = takeToken(&args);
        char *rateTok;
        char *batchTok;
        uint8_t msg[5];
        uint16_t rate = 0;
        uint8_t modeVal;

        if (modeTok == NULL) {
            CONSOLE_PORT.println(F("[mesh] usage: mesh mode <n|all> "
                                   "<interrupt|batched|idle> [rate_hz] [batch]"));
            return;
        }
        if (strcmp(modeTok, "interrupt") == 0)    { modeVal = MESH_MODE_ON_INTERRUPT; }
        else if (strcmp(modeTok, "batched") == 0) { modeVal = MESH_MODE_BATCHED; }
        else if (strcmp(modeTok, "idle") == 0)    { modeVal = MESH_MODE_IDLE; }
        else {
            CONSOLE_PORT.printf("[mesh] unknown mode '%s'\n", modeTok);
            return;
        }
        if (!meshResolveTarget(target, mac, &bcast)) {
            return;
        }
        rateTok  = takeToken(&args);
        batchTok = takeToken(&args);
        rate = (rateTok != NULL) ? (uint16_t)atoi(rateTok) : 0u;

        msg[0] = MESH_MSG_CONFIG;
        msg[1] = modeVal;
        /* rate_hz is LITTLE-endian here: it is memcpy'd into a packed struct on an ESP32, not
         * parsed by our big-endian frame layer. Two byte orders coexist in this project on
         * purpose, and each belongs to the protocol that defined it. */
        msg[2] = (uint8_t)(rate & 0xFFu);
        msg[3] = (uint8_t)(rate >> 8);
        msg[4] = (batchTok != NULL) ? (uint8_t)atoi(batchTok) : 0u;

        if (meshSendToNode(mac, msg, sizeof(msg))) {
            CONSOLE_PORT.printf("[mesh] mode %s -> ", modeTok);
            if (bcast) { CONSOLE_PORT.printf("ALL NODES"); }
            else       { meshPrintMac(mac); }
            if (rate > 0u) { CONSOLE_PORT.printf(" at %u Hz", (unsigned)rate); }
            CONSOLE_PORT.println();
        }
        return;
    }

    /* mesh id <target> -- ask a node, or every node at once, to say who it is. */
    if (strcmp(sub, "id") == 0) {
        uint8_t mac[6];
        uint8_t msg[1];
        bool bcast = false;
        char *target = takeToken(&args);

        if (!meshResolveTarget(target, mac, &bcast)) {
            return;
        }
        msg[0] = MESH_MSG_IDENTIFY;
        if (meshSendToNode(mac, msg, 1)) {
            CONSOLE_PORT.printf("[mesh] asking %s to identify; HELLO arrives as an event\n",
                                bcast ? "every node" : "the node");
            if (gMesh.stream == IOP_MESH_STREAM_OFF) {
                CONSOLE_PORT.println(F("       NOTE: HELLO is relayed as node data, so it needs"));
                CONSOLE_PORT.println(F("       the stream on -- try 'mesh stream all' first."));
            }
        }
        return;
    }

    /* mesh peer add|remove <mac|n> -- edit the hub ESP-NOW roster directly. */
    if (strcmp(sub, "peer") == 0) {
        uint8_t payload[7];
        uint8_t mac[6];
        bool bcast = false;
        char *op  = takeToken(&args);
        char *tgt = takeToken(&args);

        if (op == NULL || tgt == NULL) {
            CONSOLE_PORT.println(F("[mesh] usage: mesh peer add|remove <mac|n>"));
            return;
        }
        if (!meshResolveTarget(tgt, mac, &bcast)) {
            return;
        }
        payload[0] = (strcmp(op, "remove") == 0) ? IOP_MESH_PEER_REMOVE : IOP_MESH_PEER_ADD;
        memcpy(&payload[1], mac, 6);
        (void)sendRequest(IOP_CHAN_MESH, IOP_CMD_MESH_PEER_REQ, payload, 7, IOP_TIMEOUT_MESH_MS);
        return;
    }

    /* mesh send <target> <hex bytes> -- the escape hatch.
     *
     * Every other verb builds a message this sketch already knows. This one transmits arbitrary
     * bytes, which is what lets someone bring up a NEW kind of node without editing the brain at
     * all. A control layer with no raw path forces a firmware change for every experiment. */
    if (strcmp(sub, "send") == 0) {
        uint8_t mac[6];
        uint8_t msg[32];
        uint8_t n = 0;
        bool bcast = false;
        char *target = takeToken(&args);
        char *tok;

        if (!meshResolveTarget(target, mac, &bcast)) {
            return;
        }
        while ((tok = takeToken(&args)) != NULL && n < (uint8_t)sizeof(msg)) {
            unsigned int v;
            if (sscanf(tok, "%x", &v) != 1) {
                CONSOLE_PORT.printf("[mesh] '%s' is not a hex byte\n", tok);
                return;
            }
            msg[n++] = (uint8_t)v;
        }
        if (n == 0u) {
            CONSOLE_PORT.println(F("[mesh] usage: mesh send <n|all> <hex> [hex...]"));
            CONSOLE_PORT.println(F("       e.g. 'mesh send all 12' is a broadcast IDENTIFY"));
            return;
        }
        if (meshSendToNode(mac, msg, n)) {
            CONSOLE_PORT.printf("[mesh] %u raw byte%s -> ", (unsigned)n, (n == 1u) ? "" : "s");
            if (bcast) { CONSOLE_PORT.println(F("ALL NODES")); }
            else       { meshPrintMac(mac); CONSOLE_PORT.println(); }
        }
        return;
    }

    if (strcmp(sub, "watch") == 0) {
        char *tok = takeToken(&args);
        gMeshWatch = (tok != NULL && strcmp(tok, "off") == 0) ? false : true;
        CONSOLE_PORT.printf("[mesh] live sample printing %s\n", gMeshWatch ? "ON" : "OFF");
        if (gMeshWatch && gMesh.stream == IOP_MESH_STREAM_OFF) {
            CONSOLE_PORT.println(F("       (nothing will print: the stream is off."));
            CONSOLE_PORT.println(F("        try 'mesh stream 20' for a readable rate)"));
        }
        return;
    }

    CONSOLE_PORT.printf("[mesh] unknown sub-command '%s'\n", sub);
    CONSOLE_PORT.println(F("       observe : mesh | nodes | status | watch on|off"));
    CONSOLE_PORT.println(F("       control : mode <n|all> <interrupt|batched|idle> [hz] [batch]"));
    CONSOLE_PORT.println(F("                 id <n|all> | ping <n> [count] | send <n|all> <hex>"));
    CONSOLE_PORT.println(F("       fleet   : init [ch] [wifi] | peer add|remove <mac|n>"));
    CONSOLE_PORT.println(F("                 stream off|all|N"));
}

/* cmdStatus -- ask the radio what it is actually doing, rather than trusting our own model. */
static void cmdStatus(void)
{
    if (sendRequest(IOP_CHAN_WIFI, IOP_CMD_WIFI_STATUS_REQ, NULL, 0, IOP_TIMEOUT_STATUS_MS)) {
        CONSOLE_PORT.println(F("[status] polling the radio..."));
    }
}

/* cmdRadio -- what this brain currently believes about its radio. */
static void cmdRadio(void)
{
    CONSOLE_PORT.println(F("Radio (as modelled by this board, from unsolicited events)"));
    if (!gRadio.ready) {
        CONSOLE_PORT.println(F("  The radio has not announced itself since this board booted."));
        CONSOLE_PORT.println(F("  That is normal if the Teensy was reset on its own -- the"));
        CONSOLE_PORT.println(F("  RADIO_READY frame is only sent when the RADIO starts."));
        CONSOLE_PORT.println(F("  Type 'ping' to confirm the link is alive."));
        return;
    }
    CONSOLE_PORT.printf("  protocol ............... v%u.%u, CRC-16 0x%04X%s\n",
                        (unsigned)gRadio.protocolVersion, (unsigned)gRadio.protocolMinor,
                        (unsigned)gRadio.crcCheckValue,
                        (gRadio.protocolVersion == IOP_PROTOCOL_VERSION &&
                         gRadio.crcCheckValue == IOP_CRC16_CHECK_VALUE) ? " (matches)"
                                                                       : " *** MISMATCH ***");
    CONSOLE_PORT.printf("  capabilities ........... WiFi:%s BLE:%s\n",
                        (gRadio.capabilities & IOP_CAP_WIFI) ? "yes" : "no",
                        (gRadio.capabilities & IOP_CAP_BLE)  ? "yes" : "no");
    CONSOLE_PORT.printf("  last reset ............. %s\n", espResetReasonName(gRadio.resetReason));
    CONSOLE_PORT.printf("  announcements .......... %lu\n", (unsigned long)gRadio.reboots);
    CONSOLE_PORT.printf("  associated ............. %s\n", gRadio.associated ? "yes" : "no");
    if (gRadio.hasIp) {
        CONSOLE_PORT.printf("  address ................ %u.%u.%u.%u\n",
                            (unsigned)gRadio.ip[0], (unsigned)gRadio.ip[1],
                            (unsigned)gRadio.ip[2], (unsigned)gRadio.ip[3]);
    } else {
        CONSOLE_PORT.println(F("  address ................ none"));
    }
    CONSOLE_PORT.printf("  disconnects ............ %lu\n", (unsigned long)gRadio.disconnects);
    if (gRadio.lastReason != 0u) {
        CONSOLE_PORT.printf("  last disconnect reason . %u -- %s\n",
                            (unsigned)gRadio.lastReason,
                            iop_wifi_reason_name(gRadio.lastReason));
    }
    if (gRadio.reboots > 1u) {
        CONSOLE_PORT.println();
        CONSOLE_PORT.printf("  NOTE: the radio has announced itself %lu times. Each one is a\n",
                            (unsigned long)gRadio.reboots);
        CONSOLE_PORT.println(F("  radio restart. More than one means it is rebooting on its own,"));
        CONSOLE_PORT.println(F("  which is a power or firmware problem, not a link problem."));
    }
}

/* handleStatusResponse -- fold an authoritative snapshot into the brain's model.
 *
 * This is the correction path. Everything else in gRadio is built from unsolicited events, and
 * events can be lost -- by the radio's queue overflowing, by a receive overrun here, or by a
 * UART hardware overrun the Teensy core discards without setting any flag. A poll re-anchors the
 * model against the radio's own view, so a lost event costs a bounded staleness window instead
 * of being wrong forever. */
static void handleStatusResponse(const IopFrame *f)
{
    gStats.responsesOk++;

    if (f->payload_len < 12u) {
        CONSOLE_PORT.printf("[status] malformed: expected 12 payload bytes, got %u\n",
                            (unsigned)f->payload_len);
        clearPending(NULL);
        return;
    }

    gRadio.state         = f->payload[0];
    gRadio.lastReason    = f->payload[1];
    gRadio.rssi          = (int8_t)f->payload[3];
    gRadio.eventsDropped = f->payload[8];
    gRadio.capabilities  = f->payload[10];
    gRadio.polled        = true;

    gRadio.associated = (gRadio.state == IOP_WIFI_STATE_ASSOCIATED ||
                         gRadio.state == IOP_WIFI_STATE_GOT_IP);
    gRadio.hasIp      = (gRadio.state == IOP_WIFI_STATE_GOT_IP);
    if (gRadio.hasIp) {
        gRadio.ip[0] = f->payload[4];
        gRadio.ip[1] = f->payload[5];
        gRadio.ip[2] = f->payload[6];
        gRadio.ip[3] = f->payload[7];
    }

    CONSOLE_PORT.printf("[status] %s", iop_wifi_state_name(gRadio.state));
    if (gRadio.hasIp) {
        CONSOLE_PORT.printf(", %u.%u.%u.%u",
                            (unsigned)gRadio.ip[0], (unsigned)gRadio.ip[1],
                            (unsigned)gRadio.ip[2], (unsigned)gRadio.ip[3]);
    }
    if (gRadio.associated && gRadio.rssi != 0) {
        CONSOLE_PORT.printf(", RSSI %d dBm", (int)gRadio.rssi);
    }
    CONSOLE_PORT.println();

    if (gRadio.lastReason != 0u) {
        CONSOLE_PORT.printf("         last 802.11 reason %u: %s\n",
                            (unsigned)gRadio.lastReason,
                            iop_wifi_reason_name(gRadio.lastReason));
    }
    /* The radio's dropped-event counter, finally on the side of the wire that can act on it. */
    if (gRadio.eventsDropped > 0u) {
        CONSOLE_PORT.printf("         WARNING: the radio dropped %u event%s before sending "
                            "them.\n", (unsigned)gRadio.eventsDropped,
                            (gRadio.eventsDropped == 1u) ? "" : "s");
        CONSOLE_PORT.println(F("         Poll 'status' rather than trusting events alone."));
    }
    clearPending(NULL);
}

/* handleNack -- the ESP32 refused a command. Print the reason in full. */
static void handleNack(const IopFrame *f)
{
    gStats.nacksReceived++;

    if (f->payload_len < 3u) {
        CONSOLE_PORT.printf("[nack] malformed NACK: expected 3 payload bytes, got %u\n",
                            (unsigned)f->payload_len);
        clearPending(NULL);
        return;
    }
    CONSOLE_PORT.printf("[nack] radio rejected %s/%s: %s (error 0x%02X)\n",
                        iop_chan_name(f->payload[0]), iop_cmd_name(f->payload[1]),
                        iop_err_name(f->payload[2]), (unsigned)f->payload[2]);
    clearPending(NULL);
}

/* ===========================================================================================
 * LINK: sending
 * =========================================================================================== */

/* sendRequest -- build a request frame, transmit it, and arm the response deadline.
 *
 * Returns false (and explains why) rather than sending, if a request is already outstanding or
 * the frame could not be built. Refusing loudly beats sending something half-formed. */
static bool sendRequest(uint8_t chan, uint8_t cmd, const uint8_t *payload,
                        uint16_t len, uint32_t timeoutMs)
{
    uint16_t n;

    if (gPending.active) {
        CONSOLE_PORT.printf("[tx] refused: still waiting for a response to %s (seq=%u). "
                            "Wait for it to complete or time out.\n",
                            iop_cmd_name(gPending.cmd), (unsigned)gPending.seq);
        return false;
    }

    if (gRadio.mismatch && cmd != IOP_CMD_PING && cmd != IOP_CMD_WIFI_STATUS_REQ) {
        CONSOLE_PORT.printf("[tx] refused: the radio is running a DIFFERENT protocol build "
                            "(v%u.%u/0x%04X vs our v%u.%u/0x%04X).\n",
                            (unsigned)gRadio.protocolVersion, (unsigned)gRadio.protocolMinor,
                            (unsigned)gRadio.crcCheckValue,
                            (unsigned)IOP_PROTOCOL_VERSION, (unsigned)IOP_PROTOCOL_MINOR,
                            (unsigned)IOP_CRC16_CHECK_VALUE);
        CONSOLE_PORT.println(F("     Sending WiFi commands across a protocol mismatch risks"));
        CONSOLE_PORT.println(F("     the radio MISREADING them. Run tools/sync_headers.py and"));
        CONSOLE_PORT.println(F("     re-flash both boards. 'ping' and 'status' still work."));
        return false;
    }

    n = iop_build_frame(gTxBuf, sizeof(gTxBuf), gNextSeq, IOP_FLAG_NONE, chan, cmd,
                        payload, len);
    if (n == 0u) {
        CONSOLE_PORT.printf("[tx] refused: could not build a %s frame with a %u-byte payload "
                            "(limit is %u)\n",
                            iop_cmd_name(cmd), (unsigned)len, (unsigned)IOP_MAX_PAYLOAD_LEN);
        return false;
    }

    gPending.active     = true;
    gPending.seq        = gNextSeq;
    gPending.chan       = chan;
    gPending.cmd        = cmd;
    gPending.sentMs     = millis();
    gPending.deadlineMs = gPending.sentMs + timeoutMs;
    /* iop_next_seq, not gNextSeq++. A bare increment produces 0x00 once every 256 requests, and
     * 0x00 is reserved to mean "unsolicited" -- so that request's response would be mistaken for
     * a radio event. It is a bug that first appears after about four minutes of benchmarking
     * and never during a quick test, which is the worst possible schedule for finding it. */
    gNextSeq = iop_next_request_seq(gNextSeq);
    gStats.requestsSent++;

    /* Snapshot the cycle counter as late as possible before handing bytes to the UART, so the
     * measured interval is end-to-end application latency: from the instant we start
     * transmitting to the instant the last byte of the reply has been parsed. That figure
     * includes our transmit time, the wire time both ways, and the ESP32's turnaround -- which
     * is precisely what a caller experiences and therefore the number worth reporting. */
    gPending.sentCycles = ARM_DWT_CYCCNT;

    /* ONE bulk write, not a loop of single-byte writes. It hands the UART driver a contiguous
     * block and makes the frame far more likely to reach the wire as one uninterrupted burst,
     * which matters because the receiver is running an inter-byte timeout. */
    LINK_PORT.write(gTxBuf, n);
    mirrorRawFrame(gTxBuf, n, 'T');

    if (gHexDump) {
        CONSOLE_PORT.printf("[tx] %s seq=%u (%u bytes on the wire)\n",
                            iop_cmd_name(cmd), (unsigned)gPending.seq, (unsigned)n);
        hexDump("     ", gTxBuf, n);
    }
    return true;
}

/* sendRawBytes -- put arbitrary bytes on the link, bypassing the frame builder entirely.
 *
 * WHY THIS EXISTS: it is the only way to test the RECEIVER's error paths. The builder is
 * correct by construction, so it can never produce a bad CRC or an impossible length -- which
 * means without a raw escape hatch there is no way to prove the ESP32 rejects such frames.
 * Testing the happy path only tells you the happy path works. */
static void sendRawBytes(const uint8_t *bytes, uint16_t n, const char *why)
{
    CONSOLE_PORT.printf("[tx] RAW (%s), %u bytes:\n", why, (unsigned)n);
    hexDump("     ", bytes, n);
    LINK_PORT.write(bytes, n);
    mirrorRawFrame(bytes, n, 'T');
}

/* clearPending -- mark the in-flight request finished. */
static void clearPending(const char *reason)
{
    if (reason != NULL && gPending.active) {
        CONSOLE_PORT.printf("[tx] abandoning %s (seq=%u): %s\n",
                            iop_cmd_name(gPending.cmd), (unsigned)gPending.seq, reason);
    }
    gPending.active = false;
}

/* pendingPoll -- has the outstanding request run out of time?
 *
 * The comparison is written as a SIGNED difference against zero, which is the rollover-safe
 * way to ask "is now past deadline" with 32-bit millis(). Writing millis() > deadline breaks
 * once every 49.7 days, in a way that is essentially impossible to reproduce deliberately.
 *
 * NOTE: no automatic retry, by design. The brief calls for it and it is the right call during
 * bring-up -- a silent retry turns an intermittent wiring fault into a mystery, because the
 * command appears to work while the link is quietly failing half the time. The user decides.
 */
static void pendingPoll(void)
{
    if (!gPending.active) {
        return;
    }
    if ((int32_t)(millis() - gPending.deadlineMs) >= 0) {
        gStats.timeouts++;
        gPending.active = false;

        if (gBench.active) {
            /* During a benchmark a lost response is DATA, not an error: packet loss is one of
             * the numbers being measured. Count it and keep going rather than aborting, because
             * a run that stops at the first loss cannot tell you the loss RATE -- which is the
             * whole reason to run a thousand of them. */
            gBench.lost++;
            if (gBench.remaining > 0u) {
                gBench.remaining--;
                (void)sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS);
            } else {
                benchFinish();
                CONSOLE_PORT.print(F("> "));
            }
            return;
        }

        CONSOLE_PORT.printf("\n[timeout] no response to %s (seq=%u) after %lu ms\n",
                            iop_cmd_name(gPending.cmd), (unsigned)gPending.seq,
                            (unsigned long)(millis() - gPending.sentMs));
        CONSOLE_PORT.println(F("          Not retrying automatically -- retry manually so an"));
        CONSOLE_PORT.println(F("          intermittent fault stays visible instead of hiding."));
        CONSOLE_PORT.print(F("> "));
    }
}

/* ===========================================================================================
 * CONSOLE
 * =========================================================================================== */

/* consolePoll -- accumulate typed characters and execute on end-of-line.
 *
 * Character-at-a-time and non-blocking: the link keeps running while you are mid-word. A
 * readStringUntil() here would stall the whole sketch until you pressed Enter, during which a
 * scan response could overflow the UART buffer. */
static void consolePoll(void)
{
    static char   line[CONSOLE_LINE_MAX];
    static uint16_t len = 0;
    static bool   overflow = false;

    while (CONSOLE_PORT.available() > 0) {
        char c = (char)CONSOLE_PORT.read();

        if (c == '\r') {
            continue;               /* tolerate CR, CRLF and LF line endings alike */
        }
        if (c == '\n') {
            line[len] = '\0';
            if (overflow) {
                CONSOLE_PORT.println(F("[console] line too long, ignored"));
            } else if (len > 0u) {
                executeCommand(line);
            }
            len = 0;
            overflow = false;
            CONSOLE_PORT.print(F("> "));
            continue;
        }
        if (c == '\b' || c == 0x7F) { /* backspace / delete */
            if (len > 0u) {
                len--;
            }
            continue;
        }
        if (len < (CONSOLE_LINE_MAX - 1u)) {
            line[len++] = c;
        } else {
            overflow = true;        /* keep consuming until the newline, then report once */
        }
    }
}

/* skipSpaces / takeToken -- minimal, allocation-free tokenising.
 *
 * takeToken understands double quotes, so an SSID containing spaces can be given as
 *     connect "Guest Network" hunter2
 * Without that, any SSID with a space would be unreachable from this console -- and plenty of
 * real networks have one. strtok() is avoided because it mutates hidden global state, which
 * makes it a poor fit for a loop that may be re-entered. */
static char *skipSpaces(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static char *takeToken(char **cursor)
{
    char *s = skipSpaces(*cursor);
    char *start;

    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }
    if (*s == '"') {
        s++;
        start = s;
        while (*s != '\0' && *s != '"') {
            s++;
        }
    } else {
        start = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') {
            s++;
        }
    }
    if (*s != '\0') {
        *s = '\0';
        s++;
    }
    *cursor = s;
    return start;
}

/* executeCommand -- map a typed line to an action. */
static void executeCommand(char *line)
{
    char *cursor = line;
    char *verb   = takeToken(&cursor);

    if (verb == NULL) {
        return;
    }

    if (strcmp(verb, "help") == 0 || strcmp(verb, "?") == 0) {
        cmdHelp();
    } else if (strcmp(verb, "ping") == 0) {
        (void)sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS);
    } else if (strcmp(verb, "scan") == 0) {
        if (sendRequest(IOP_CHAN_WIFI, IOP_CMD_WIFI_SCAN_REQ, NULL, 0, IOP_TIMEOUT_SCAN_MS)) {
            CONSOLE_PORT.printf("[scan] requested; a scan takes a few seconds "
                                "(timeout %lu ms)...\n", (unsigned long)IOP_TIMEOUT_SCAN_MS);
        }
    } else if (strcmp(verb, "connect") == 0) {
        cmdConnect(cursor);
    } else if (strcmp(verb, "stats") == 0) {
        cmdStats();
    } else if (strcmp(verb, "bench") == 0) {
        cmdBench(cursor);
    } else if (strcmp(verb, "sys") == 0) {
        cmdSys();
    } else if (strcmp(verb, "radio") == 0) {
        cmdRadio();
    } else if (strcmp(verb, "status") == 0) {
        cmdStatus();
    } else if (strcmp(verb, "mesh") == 0) {
        cmdMesh(cursor);
    } else if (strcmp(verb, "hexdump") == 0) {
        char *arg = takeToken(&cursor);
        gHexDump = (arg != NULL && strcmp(arg, "off") == 0) ? false : true;
        CONSOLE_PORT.printf("[console] hex dump %s\n", gHexDump ? "ON" : "OFF");
    } else if (strcmp(verb, "corrupt") == 0) {
        cmdCorrupt();
    } else if (strcmp(verb, "badcmd") == 0) {
        cmdBadCommand();
    } else if (strcmp(verb, "oversize") == 0) {
        cmdOversize();
    } else if (strcmp(verb, "noise") == 0) {
        cmdNoise();
    } else {
        CONSOLE_PORT.printf("[console] unknown command '%s' -- type 'help'\n", verb);
    }
}

static void cmdHelp(void)
{
    CONSOLE_PORT.println(F("Commands"));
    CONSOLE_PORT.println(F("  help                     this list"));
    CONSOLE_PORT.println(F("  ping                     PING the ESP32 and wait for PONG"));
    CONSOLE_PORT.println(F("  scan                     scan for WiFi networks"));
    CONSOLE_PORT.println(F("  connect SSID PASSWORD    join a network"));
    CONSOLE_PORT.println(F("                           quote names with spaces:"));
    CONSOLE_PORT.println(F("                           connect \"My Network\" secret123"));
    CONSOLE_PORT.println(F("                           open networks: omit the password"));
    CONSOLE_PORT.println(F("  stats                    link health counters"));
    CONSOLE_PORT.println(F("  bench [N]                measure round-trip latency over N"));
    CONSOLE_PORT.println(F("                           pings (default 200): min/mean/max/sigma"));
    CONSOLE_PORT.println(F("                           at nanosecond resolution"));
    CONSOLE_PORT.println(F("  sys                      board, clock, temperature, link budget"));
    CONSOLE_PORT.println(F("  status                   ASK the radio what it is doing now"));
    CONSOLE_PORT.println(F("                           (authoritative; corrects any missed"));
    CONSOLE_PORT.println(F("                           unsolicited events)"));
    CONSOLE_PORT.println(F("  radio                    what this brain believes about the radio:"));
    CONSOLE_PORT.println(F("                           association, address, reboots, last"));
    CONSOLE_PORT.println(F("                           802.11 disconnect reason"));
    CONSOLE_PORT.println(F("  hexdump on|off           show every frame in hex"));
    CONSOLE_PORT.println(F("Mesh -- this board commands the ESP-NOW fleet through the radio."));
    CONSOLE_PORT.println(F("  A target is an index from 'mesh nodes', a full MAC, or 'all'"));
    CONSOLE_PORT.println(F("  ('all' is the ESP-NOW broadcast: one packet, every node, at once)."));
    CONSOLE_PORT.println(F("  -- see the fleet --"));
    CONSOLE_PORT.println(F("  mesh                     what this brain currently believes"));
    CONSOLE_PORT.println(F("  mesh nodes               POLL the hub for the roster + MACs."));
    CONSOLE_PORT.println(F("                           Needed after a Teensy reset: node-seen"));
    CONSOLE_PORT.println(F("                           events only reach a brain that was up"));
    CONSOLE_PORT.println(F("  mesh status              hub state and exact air-side totals"));
    CONSOLE_PORT.println(F("  mesh watch on|off        print live samples as they arrive"));
    CONSOLE_PORT.println(F("  -- command the fleet --"));
    CONSOLE_PORT.println(F("  mesh mode <t> <m> [hz] [batch]"));
    CONSOLE_PORT.println(F("                           m = interrupt | batched | idle."));
    CONSOLE_PORT.println(F("                           Changes what a remote board DOES"));
    CONSOLE_PORT.println(F("  mesh id <t>              ask who a node is (fw, WHO_AM_I)"));
    CONSOLE_PORT.println(F("  mesh ping <t> [count]    ESP-NOW round trip, timed on the hub"));
    CONSOLE_PORT.println(F("                           clock alone (default 30). One node only"));
    CONSOLE_PORT.println(F("  mesh send <t> <hex...>   raw bytes to a node -- bring up a new"));
    CONSOLE_PORT.println(F("                           kind of node without reflashing this one"));
    CONSOLE_PORT.println(F("  -- manage the fleet --"));
    CONSOLE_PORT.println(F("  mesh init [ch] [wifi]    start the mesh on a channel; add 'wifi'"));
    CONSOLE_PORT.println(F("                           to coexist with a station connection"));
    CONSOLE_PORT.println(F("  mesh peer add|remove <t> edit the hub's ESP-NOW roster"));
    CONSOLE_PORT.println(F("  mesh stream off|all|N    how much node data to forward here."));
    CONSOLE_PORT.println(F("                           OFF by default -- the hub keeps counting"));
    CONSOLE_PORT.println(F("                           either way, so N costs no statistics"));
    CONSOLE_PORT.println();
    CONSOLE_PORT.println(F("Error-path tests -- these SHOULD fail, that is the point:"));
    CONSOLE_PORT.println(F("  corrupt                  send a PING with a deliberately wrong"));
    CONSOLE_PORT.println(F("                           CRC. Expect NACK 0x02 from the ESP32."));
    CONSOLE_PORT.println(F("  badcmd                   send an undefined command byte."));
    CONSOLE_PORT.println(F("                           Expect NACK 0x01."));
    CONSOLE_PORT.println(F("  oversize                 claim a payload larger than the buffer."));
    CONSOLE_PORT.println(F("                           Expect NACK 0x05, then a working link."));
    CONSOLE_PORT.println(F("  noise                    send junk, then a valid PING, to prove"));
    CONSOLE_PORT.println(F("                           the parser resynchronises."));
}

/* cmdConnect -- parse "connect SSID [PASSWORD]" and send the request. */
static void cmdConnect(char *args)
{
    char    *cursor = args;
    char    *ssid   = takeToken(&cursor);
    char    *pass   = takeToken(&cursor);
    uint8_t  payload[1u + IOP_SSID_MAX_LEN + 1u + IOP_PASS_MAX_LEN];
    uint16_t offset = 0;
    size_t   ssidLen;
    size_t   passLen;

    if (ssid == NULL) {
        CONSOLE_PORT.println(F("[connect] usage: connect SSID PASSWORD"));
        CONSOLE_PORT.println(F("                 connect \"Name With Spaces\" password"));
        return;
    }
    ssidLen = strlen(ssid);
    passLen = (pass != NULL) ? strlen(pass) : 0u;

    /* Validate against the 802.11 limits BEFORE building the frame. Catching this here gives a
     * precise message; letting it through would produce a vague failure on the far side. */
    if (ssidLen == 0u || ssidLen > IOP_SSID_MAX_LEN) {
        CONSOLE_PORT.printf("[connect] SSID must be 1..%u characters (got %u)\n",
                            (unsigned)IOP_SSID_MAX_LEN, (unsigned)ssidLen);
        return;
    }
    if (passLen > IOP_PASS_MAX_LEN) {
        CONSOLE_PORT.printf("[connect] password must be at most %u characters (got %u)\n",
                            (unsigned)IOP_PASS_MAX_LEN, (unsigned)passLen);
        return;
    }

    payload[offset++] = (uint8_t)ssidLen;
    memcpy(&payload[offset], ssid, ssidLen);
    offset = (uint16_t)(offset + ssidLen);
    payload[offset++] = (uint8_t)passLen;
    if (passLen > 0u) {
        memcpy(&payload[offset], pass, passLen);
        offset = (uint16_t)(offset + passLen);
    }

    if (sendRequest(IOP_CHAN_WIFI, IOP_CMD_WIFI_CONNECT_REQ, payload, offset, IOP_TIMEOUT_CONNECT_MS)) {
        /* Print the SSID but NOT the password. Serial monitor output gets pasted into forum
         * posts and screenshots; a credential that is never printed cannot leak that way. */
        CONSOLE_PORT.printf("[connect] joining \"%s\" (%u-character password), "
                            "timeout %lu ms...\n",
                            ssid, (unsigned)passLen, (unsigned long)IOP_TIMEOUT_CONNECT_MS);
    }
}

static void cmdStats(void)
{
    CONSOLE_PORT.println(F("Link statistics"));
    CONSOLE_PORT.printf("  requests sent .......... %lu\n", (unsigned long)gStats.requestsSent);
    CONSOLE_PORT.printf("  responses received ..... %lu\n", (unsigned long)gStats.responsesOk);
    CONSOLE_PORT.printf("  NACKs received ......... %lu\n", (unsigned long)gStats.nacksReceived);
    CONSOLE_PORT.printf("  timeouts ............... %lu\n", (unsigned long)gStats.timeouts);
    CONSOLE_PORT.printf("  stale/unsolicited ...... %lu\n", (unsigned long)gStats.staleFrames);
    CONSOLE_PORT.println(F("  -- parser counters --"));
    CONSOLE_PORT.printf("  valid frames ........... %lu\n", (unsigned long)gParser.stat_frames_ok);
    CONSOLE_PORT.printf("  bad CRC ................ %lu\n", (unsigned long)gParser.stat_bad_crc);
    CONSOLE_PORT.printf("  impossible length ...... %lu\n", (unsigned long)gParser.stat_bad_length);
    CONSOLE_PORT.printf("  oversize ............... %lu\n", (unsigned long)gParser.stat_oversize);
    CONSOLE_PORT.printf("  frame timeouts ......... %lu\n", (unsigned long)gParser.stat_timeouts);
    CONSOLE_PORT.printf("  bytes discarded ........ %lu\n",
                        (unsigned long)gParser.stat_resync_bytes);
    CONSOLE_PORT.printf("  NACKs suppressed ....... %lu (rate limited)\n",
                        (unsigned long)gNackLimiter.suppressed);
    CONSOLE_PORT.printf("  console lines dropped .. %lu (USB host not draining)\n",
                        (unsigned long)gStats.consoleDropped);
    CONSOLE_PORT.printf("  radio events missed .... %lu (gaps in the unsolicited counter)\n",
                        (unsigned long)gStats.eventsMissed);
    CONSOLE_PORT.println();
    CONSOLE_PORT.println(F("  How to read this: on a healthy link, bad CRC and discarded bytes"));
    CONSOLE_PORT.println(F("  should be 0 or very near it. A steady trickle of discarded bytes"));
    CONSOLE_PORT.println(F("  with no valid frames usually means a baud rate mismatch. A few"));
    CONSOLE_PORT.println(F("  bad CRCs per thousand frames points at grounding or wire length."));
}

/* ---------------------------------------------------------------------------------------
 * DELIBERATE ERROR INJECTION
 * ---------------------------------------------------------------------------------------
 * These implement the milestone 1 verification steps. Each one builds a valid frame and then
 * breaks it in one specific way, so that exactly one failure mode is under test.
 */

/* cmdCorrupt -- a PING whose CRC byte is wrong. Verifies the receiver's integrity check. */
static void cmdCorrupt(void)
{
    uint16_t n = iop_build_frame(gTxBuf, sizeof(gTxBuf), gNextSeq, IOP_FLAG_NONE,
                                 IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0);
    gNextSeq = iop_next_request_seq(gNextSeq);
    if (n == 0u) {
        return;
    }
    /* Flip one bit of the CRC. One bit, not a random byte: CRC-8 is guaranteed to catch every
     * single-bit error, so this is the strongest possible statement of the test. If the ESP32
     * accepts this frame, its CRC implementation does not match ours. */
    gTxBuf[n - 1u] ^= 0x01u;
    sendRawBytes(gTxBuf, n, "PING with a deliberately corrupted CRC");
    CONSOLE_PORT.println(F("     Expect: the ESP32 reports a bad CRC and returns NACK 0x02."));
    CONSOLE_PORT.println(F("     No response arrives here because nothing is pending -- the"));
    CONSOLE_PORT.println(F("     NACK will be reported as an unexpected frame. That is correct."));
}

/* cmdBadCommand -- a well-formed frame carrying a command byte nobody implements. */
static void cmdBadCommand(void)
{
    uint16_t n = iop_build_frame(gTxBuf, sizeof(gTxBuf), gNextSeq, IOP_FLAG_NONE,
                                 IOP_CHAN_WIFI, 0x7Fu, NULL, 0);
    if (n == 0u) {
        return;
    }
    gPending.active     = true;
    gPending.seq        = gNextSeq;
    gNextSeq            = iop_next_request_seq(gNextSeq);
    gPending.chan       = IOP_CHAN_WIFI;
    gPending.cmd        = 0x7Fu;
    gPending.sentMs     = millis();
    gPending.deadlineMs = gPending.sentMs + IOP_TIMEOUT_PING_MS;
    gStats.requestsSent++;
    sendRawBytes(gTxBuf, n, "valid frame, undefined command 0x7F");
    CONSOLE_PORT.println(F("     Expect: NACK 0x01 (unknown command), reported below."));
}

/* cmdOversize -- a frame declaring a body far larger than either buffer.
 *
 * Only the header is sent, not the (nonexistent) body. The ESP32 must recognise the impossible
 * length, NACK it, and then recover -- which it does via the parser's SKIP state combined with
 * the inter-byte timeout, since the promised bytes never arrive. */
static void cmdOversize(void)
{
    uint8_t hdr[5];
    hdr[0] = (uint8_t)IOP_START_BYTE;
    hdr[1] = 0x7Fu;                    /* LEN_H */
    hdr[2] = 0xFFu;                    /* LEN_L -> 32767 bytes, far beyond the 1019 limit */
    hdr[3] = gNextSeq;                 /* SEQ, which the ESP32 must still echo in its NACK */
    gNextSeq = iop_next_request_seq(gNextSeq);
    hdr[4] = IOP_CMD_PING;             /* first body byte; will be discarded */
    sendRawBytes(hdr, sizeof(hdr), "frame claiming a 32767-byte body");
    CONSOLE_PORT.println(F("     Expect: NACK 0x05 immediately -- 32767 is past the believable"));
    CONSOLE_PORT.println(F("     limit, so the ESP32 judges it a false start and resyncs at"));
    CONSOLE_PORT.println(F("     once rather than waiting for bytes that never arrive."));
    CONSOLE_PORT.println(F("     Follow this with 'ping' to prove recovery."));
}

/* cmdNoise -- garbage followed by a real PING. Proves resynchronisation end to end. */
static void cmdNoise(void)
{
    static const uint8_t junk[12] = {
        0x00u, 0xFFu, 0xAAu, 0xAAu, 0x12u, 0x34u,
        0xAAu, 0x56u, 0x78u, 0x9Au, 0xBCu, 0xDEu
    };
    sendRawBytes(junk, sizeof(junk), "random bytes including stray 0xAA start markers");
    CONSOLE_PORT.println(F("     Now sending a real PING immediately after the junk."));
    (void)sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS);
    CONSOLE_PORT.println(F("     Expect: PONG. The junk is discarded and the link resyncs."));
}

/* benchRecord -- fold one round-trip measurement into the running statistics.
 *
 * Sum and sum-of-squares are accumulated in 64 bits so that the variance can be computed at the
 * end in one pass, without storing every sample. 10000 samples of ~400000 ns each gives a
 * sum-of-squares near 1.6e15 -- comfortably inside 64 bits, nowhere near inside 32. */
static void benchRecord(uint32_t rttNs)
{
    gBench.completed++;
    gBench.sumNs   += (uint64_t)rttNs;
    gBench.sumSqNs += (uint64_t)rttNs * (uint64_t)rttNs;
    if (rttNs < gBench.minNs) { gBench.minNs = rttNs; }
    if (rttNs > gBench.maxNs) { gBench.maxNs = rttNs; }

    /* A progress marker every 50 samples, so a long run does not look like a hang. */
    if ((gBench.completed % 50u) == 0u) {
        CONSOLE_PORT.print(F("."));
    }
}

/* benchFinish -- report the distribution, not just the average.
 *
 * WHY THE SPREAD MATTERS MORE THAN THE MEAN: a link with a 250 us mean and a 5 us standard
 * deviation is healthy. A link with the same 250 us mean and a 900 us maximum is not -- it has
 * a periodic stall, and it will drop frames the moment you shorten a timeout or add traffic.
 * The mean alone hides that completely, which is why min, max, sigma and loss are all printed.
 */
static void benchFinish(void)
{
    double mean, variance, sigma;

    gBench.active = false;

    CONSOLE_PORT.println();
    if (gBench.completed == 0u) {
        CONSOLE_PORT.printf("[bench] no responses at all (%lu lost). The link is down.\n",
                            (unsigned long)gBench.lost);
        return;
    }

    mean     = (double)gBench.sumNs / (double)gBench.completed;
    variance = ((double)gBench.sumSqNs / (double)gBench.completed) - (mean * mean);
    if (variance < 0.0) { variance = 0.0; }   /* floating-point noise near zero variance */
    sigma    = sqrt(variance);

    CONSOLE_PORT.println(F("[bench] round-trip latency, PING -> PONG"));
    CONSOLE_PORT.printf("        samples ...... %lu\n", (unsigned long)gBench.completed);
    CONSOLE_PORT.printf("        lost ......... %lu\n", (unsigned long)gBench.lost);
    CONSOLE_PORT.printf("        min .......... %lu.%03lu us\n",
                        (unsigned long)(gBench.minNs / 1000u),
                        (unsigned long)(gBench.minNs % 1000u));
    CONSOLE_PORT.printf("        mean ......... %lu.%03lu us\n",
                        (unsigned long)((uint32_t)mean / 1000u),
                        (unsigned long)((uint32_t)mean % 1000u));
    CONSOLE_PORT.printf("        max .......... %lu.%03lu us\n",
                        (unsigned long)(gBench.maxNs / 1000u),
                        (unsigned long)(gBench.maxNs % 1000u));
    CONSOLE_PORT.printf("        std dev ...... %lu.%03lu us\n",
                        (unsigned long)((uint32_t)sigma / 1000u),
                        (unsigned long)((uint32_t)sigma % 1000u));
    CONSOLE_PORT.println();

    /* A PING frame is 6 bytes out and 6 bytes back. At 921600 baud, 8N1, each byte is 10 bit
     * times = 10.851 us, so 12 bytes of pure wire time is 130.2 us. Printing that alongside the
     * measurement turns an abstract number into a verdict: subtract it and what remains is the
     * ESP32's turnaround plus both sketches' loop latency. */
    CONSOLE_PORT.println(F("        For reference, 12 bytes at 921600 baud 8N1 is 130.2 us of"));
    CONSOLE_PORT.println(F("        pure wire time. Anything above that is turnaround: the"));
    CONSOLE_PORT.println(F("        ESP32 noticing the frame, handling it, and replying."));
}

/* cmdBench -- start a latency measurement run. */
static void cmdBench(char *args)
{
    char    *cursor = args;
    char    *arg    = takeToken(&cursor);
    uint32_t count  = BENCH_DEFAULT_COUNT;

    if (gPending.active) {
        CONSOLE_PORT.println(F("[bench] a request is already in flight; wait for it to finish"));
        return;
    }
    if (arg != NULL) {
        long v = atol(arg);
        if (v < 1 || v > (long)BENCH_MAX_COUNT) {
            CONSOLE_PORT.printf("[bench] count must be 1..%lu\n", (unsigned long)BENCH_MAX_COUNT);
            return;
        }
        count = (uint32_t)v;
    }

    gBench.active    = true;
    gBench.remaining = count - 1u;    /* this call sends the first one */
    gBench.completed = 0;
    gBench.lost      = 0;
    gBench.minNs     = 0xFFFFFFFFu;
    gBench.maxNs     = 0;
    gBench.sumNs     = 0;
    gBench.sumSqNs   = 0;

    CONSOLE_PORT.printf("[bench] %lu round trips at %lu MHz core clock, one at a time...\n",
                        (unsigned long)count, (unsigned long)(F_CPU_ACTUAL / 1000000u));
    if (!sendRequest(IOP_CHAN_TRANSPORT, IOP_CMD_PING, NULL, 0, IOP_TIMEOUT_PING_MS)) {
        gBench.active = false;
    }
}

/* cmdSys -- what this board is and how it is doing.
 *
 * Every figure here has been useful at least once while debugging a link: the core clock
 * because it scales every timing measurement, the die temperature because the Teensy throttles
 * when hot (which changes F_CPU_ACTUAL underneath you), and the buffer sizes because "how much
 * slack do I actually have" is the first question when bytes go missing. */
static void cmdSys(void)
{
    uint32_t up = millis() / 1000u;

    CONSOLE_PORT.println(F("System"));
    CONSOLE_PORT.printf("  board .................. Teensy 4.1 (IMXRT1062)\n");
    CONSOLE_PORT.printf("  core clock ............. %lu MHz\n",
                        (unsigned long)(F_CPU_ACTUAL / 1000000u));
    CONSOLE_PORT.printf("  DWT resolution ......... %lu ps per tick\n",
                        (unsigned long)(1000000000000ULL / (uint64_t)F_CPU_ACTUAL));
    CONSOLE_PORT.printf("  die temperature ........ %.1f C\n", tempmonGetTemp());
    CONSOLE_PORT.printf("  uptime ................. %lud %luh %lum %lus\n",
                        (unsigned long)(up / 86400u), (unsigned long)((up / 3600u) % 24u),
                        (unsigned long)((up / 60u) % 60u), (unsigned long)(up % 60u));
    CONSOLE_PORT.println(F("  -- link --"));
    CONSOLE_PORT.printf("  baud ................... %lu\n", (unsigned long)LINK_BAUD);
    CONSOLE_PORT.printf("  byte time .............. %.3f us (10 bits, 8N1)\n",
                        10.0 * 1000000.0 / (double)LINK_BAUD);
    CONSOLE_PORT.printf("  RX buffer .............. %u bytes = %.1f ms of headroom\n",
                        (unsigned)sizeof(gLinkRxBuffer),
                        (double)sizeof(gLinkRxBuffer) * 10.0 * 1000.0 / (double)LINK_BAUD);
    CONSOLE_PORT.printf("  flow control ........... %s\n",
                        LINK_USE_FLOW_CONTROL ? "hardware RTS/CTS" : "none (not needed)");
    CONSOLE_PORT.println(F("  -- protocol --"));
    CONSOLE_PORT.printf("  version ................ v%u.%u\n",
                        (unsigned)IOP_PROTOCOL_VERSION, (unsigned)IOP_PROTOCOL_MINOR);
    CONSOLE_PORT.printf("  CRC-16 check value ..... 0x%04X\n", (unsigned)IOP_CRC16_CHECK_VALUE);
    CONSOLE_PORT.printf("  max frame .............. %u bytes\n", (unsigned)IOP_MAX_FRAME_SIZE);
    CONSOLE_PORT.printf("  parser state size ...... %u bytes\n", (unsigned)sizeof(IopParser));
    CONSOLE_PORT.printf("  scan capacity .......... %u networks per frame\n",
                        (unsigned)IOP_SCAN_MAX_NETWORKS);
}

/* mirrorRawFrame -- copy every frame, verbatim, to the second USB serial port.
 *
 * Only compiled when the sketch is built with Tools > USB Type = "Dual Serial". The console
 * port carries human-readable text; this one carries nothing but the exact bytes that crossed
 * the wire, prefixed with a direction marker and a length. A PC-side decoder can therefore read
 * a clean binary stream instead of scraping hex out of log text -- and because the two streams
 * are separate USB endpoints, heavy logging on one cannot corrupt or interleave the other.
 *
 * Compiles to nothing at all when USB Type is plain "Serial", so it costs non-users zero bytes.
 */
static void mirrorRawFrame(const uint8_t *data, uint16_t n, uint8_t direction)
{
#ifdef USB_DUAL_SERIAL
    uint8_t hdr[4];
    hdr[0] = 0xF5u;                              /* capture marker, distinct from IOP_START */
    hdr[1] = direction;                          /* 'T' = Teensy->ESP, 'E' = ESP->Teensy    */
    hdr[2] = (uint8_t)((n >> 8) & 0xFFu);
    hdr[3] = (uint8_t)(n & 0xFFu);
    SerialUSB1.write(hdr, sizeof(hdr));
    SerialUSB1.write(data, n);
#else
    (void)data; (void)n; (void)direction;
#endif
}

/* hexDump -- print bytes as hex, 16 per line, for eyeballing a frame.
 *
 * WHY 16 PER LINE: it is the universal convention, so the output can be compared directly
 * against a logic analyser capture or another tool's dump without mental arithmetic. */
static void hexDump(const char *prefix, const uint8_t *data, uint16_t n)
{
    uint16_t i;
    if (data == NULL || n == 0u) {
        return;
    }
    /* A full frame dumps as ~3 KB of text. If the host is not keeping up, drop the dump rather
     * than block the link for it -- a hex dump is a debugging luxury, a frame is not. */
    if (!consoleCanWrite((uint16_t)((n * 3u) + 16u))) {
        return;
    }
    for (i = 0; i < n; i++) {
        if ((i % 16u) == 0u) {
            if (i > 0u) {
                CONSOLE_PORT.println();
            }
            CONSOLE_PORT.print(prefix);
        }
        CONSOLE_PORT.printf("%02X ", (unsigned)data[i]);
    }
    CONSOLE_PORT.println();
}

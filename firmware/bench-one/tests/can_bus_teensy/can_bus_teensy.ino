/* ===========================================================================================
 *  can_bus_teensy -- measuring whether CAN can carry BENCH ONE's control plane
 * ===========================================================================================
 *
 *  WHAT THIS IS FOR
 *  ----------------
 *  The machine has 33 nodes and no way for them to talk about anything except work. There is no
 *  heartbeat, no health, no dispatch, and no way to power-cycle a board that has hung. Ethernet
 *  could carry all of it, but every node would need an address, a stack, and a switch port, and a
 *  node that is powered off would have to be discovered by its absence.
 *
 *  CAN gets all of that for one transceiver and two wires. It is multi-master with hardware
 *  arbitration, it needs no switch and no addressing, and an unpowered node's transceiver goes
 *  high-impedance rather than shorting the pair -- so any board can be power-cycled without
 *  disturbing the bus. On a machine whose whole point is many cheap boards, that last property is
 *  not a detail.
 *
 *  What CAN cannot do is carry activations. That has to be said with a number rather than a
 *  feeling, so this measures the number.
 *
 *
 *  WHAT IT MEASURES
 *  ----------------
 *    1. Round-trip latency between two nodes, with min, median, p99 and max. Compare against the
 *       148 us already measured for a machine-to-machine Ethernet hop.
 *    2. Sustained frame rate, which sets how much control traffic the bus can actually hold.
 *    3. The same latency test again with the bus loaded, because a control plane whose latency
 *       triples under traffic is not one.
 *    4. The CAN error counters at every stage. These are the fastest way to find a wiring fault,
 *       and on a bus built from six identical breakout modules there is one specific fault that
 *       is nearly guaranteed -- see TERMINATION below.
 *
 *
 *  TERMINATION -- READ THIS BEFORE WIRING SIX MODULES TOGETHER
 *  -----------------------------------------------------------
 *  A CAN bus takes exactly TWO 120 ohm terminators, one at each physical end of the pair. Every
 *  SN65HVD230 breakout module ships with one fitted. Six modules on one bus is six terminators in
 *  parallel, which is 20 ohms, and the transceiver is specified down to 45. It will not drive it.
 *  The symptom is not silence: it is a bus that half works, with the error counters climbing and
 *  the controller dropping to error-passive and then bus-off.
 *
 *  So: leave the terminator on the two modules at the ends of the run and remove it from the other
 *  four. On these boards it is the 120 ohm part next to the CANH/CANL screw terminal.
 *
 *  The other module-specific trap is the RS pin. It sets slew rate: a direct connection to ground
 *  is full speed, and a large resistor to ground deliberately slows the edges to reduce emissions.
 *  Ten kilohms is common on these boards and it will not survive 1 Mbit. If the error counters
 *  climb with the termination correct, that resistor is the next suspect.
 *
 *
 *  WIRING, PER NODE
 *  ----------------
 *    module 3V3  -> Teensy 3.3V        (NOT 5V -- the SN65HVD230 is a 3.3 V part)
 *    module GND  -> Teensy GND         (and every node's ground must be common)
 *    module CTX  -> Teensy pin 22      (CAN1 transmit)
 *    module CRX  -> Teensy pin 23      (CAN1 receive)
 *    module CANH -> the pair's H wire, module CANL -> the pair's L wire
 *
 *  Use a twisted pair for CANH/CANL. The BMW harness is full of it and that is the correct use for
 *  it: the noise rejection is the whole reason CAN is differential.
 *
 *  A Teensy 4.1 has three CAN controllers, so a node can sit on more than one bus if the control
 *  plane is ever split. CAN1 is pins 22/23, CAN2 is 1/0, CAN3 is 31/30 and is the CAN FD one.
 *
 *
 *  BUILD
 *      set NODE_ID below -- 0 is the master, 1..7 are workers -- then build.bat
 * ======================================================================================== */

#include <FlexCAN_T4.h>

/* 0 = master, anything else = worker. Every board on the bus needs a different one. */
#define NODE_ID 0

#define BITRATE   1000000UL
#define N_PING    2000       /* round trips per latency run */
#define LOAD_HZ   2000       /* background frames a second during the loaded run */

/* Identifier map. Lower wins arbitration, so the ordering here is the priority ordering: a node
 * shouting about a fault must beat a node reporting its temperature, and both must beat bulk. */
#define ID_DISCOVER  0x001              /* master -> all: who is there                     */
#define ID_PRESENT   0x080              /* + node: worker -> master, I am here             */
#define ID_HEARTBEAT 0x010              /* + node: state, temperature, free memory         */
#define ID_PING      0x100              /* + node: master -> worker, echo this             */
#define ID_PONG      0x200              /* + node: worker -> master                        */
#define ID_LOAD      0x700              /* deliberately low priority background traffic    */

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can;

static uint32_t lat[N_PING];
static int n_lat = 0;

/* ------------------------------------------------------------------------------------------- */

static void show_errors(const char *when)
{
    CAN_error_t e;
    if (!can.error(e, false)) {
        Serial.print(F("    "));
        Serial.print(when);
        Serial.println(F(": no error flags"));
        return;
    }
    Serial.print(F("    "));
    Serial.print(when);
    Serial.print(F(": state "));
    Serial.print(e.FLT_CONF);
    Serial.print(F(", tx errors "));
    Serial.print(e.TX_ERR_COUNTER);
    Serial.print(F(", rx errors "));
    Serial.print(e.RX_ERR_COUNTER);
    if (e.BIT1_ERR || e.BIT0_ERR) Serial.print(F(", BIT"));
    if (e.ACK_ERR)               Serial.print(F(", NO ACK -- nothing else is on the bus"));
    if (e.CRC_ERR)               Serial.print(F(", CRC"));
    if (e.FRM_ERR)               Serial.print(F(", FORM"));
    if (e.STF_ERR)               Serial.print(F(", STUFF"));
    Serial.println();
}

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void report_latency(const char *tag)
{
    if (n_lat < 8) {
        Serial.print(F("  "));
        Serial.print(tag);
        Serial.println(F(": no replies. Check termination, wiring and that a worker is running."));
        return;
    }
    qsort(lat, n_lat, sizeof(uint32_t), cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < n_lat; i++) sum += lat[i];

    Serial.print(F("  "));
    Serial.print(tag);
    Serial.print(F(" over "));
    Serial.print(n_lat);
    Serial.println(F(" round trips, microseconds:"));
    Serial.print(F("    min "));      Serial.print(lat[0]);
    Serial.print(F("   median "));    Serial.print(lat[n_lat / 2]);
    Serial.print(F("   mean "));      Serial.print((uint32_t)(sum / n_lat));
    Serial.print(F("   p99 "));       Serial.print(lat[(n_lat * 99) / 100]);
    Serial.print(F("   max "));       Serial.println(lat[n_lat - 1]);
    Serial.print(F("    jitter, p99 minus min: "));
    Serial.print(lat[(n_lat * 99) / 100] - lat[0]);
    Serial.println(F(" us"));
}

/* One round trip. Returns microseconds, or 0 if the worker did not answer in time. */
static uint32_t ping_once(uint8_t target, uint32_t seq)
{
    CAN_message_t tx;
    tx.id = ID_PING + target;
    tx.len = 8;
    memcpy(tx.buf, &seq, 4);

    const uint32_t t0 = micros();
    if (!can.write(tx)) return 0;

    while ((uint32_t)(micros() - t0) < 20000UL) {
        CAN_message_t rx;
        if (can.read(rx) && rx.id == (uint32_t)(ID_PONG + target)) {
            uint32_t got;
            memcpy(&got, rx.buf, 4);
            if (got == seq) return micros() - t0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------- */

static uint8_t find_worker(void)
{
    CAN_message_t tx;
    tx.id = ID_DISCOVER;
    tx.len = 1;
    tx.buf[0] = 0;
    can.write(tx);

    const uint32_t t0 = millis();
    while (millis() - t0 < 500) {
        CAN_message_t rx;
        if (can.read(rx) && rx.id >= ID_PRESENT && rx.id < ID_PRESENT + 64)
            return (uint8_t)(rx.id - ID_PRESENT);
    }
    return 0;
}

static void run_master(void)
{
    Serial.println(F("\n========================================================================"));
    Serial.println(F("  CAN as a control plane: what it costs and what it can hold"));
    Serial.println(F("========================================================================"));
    Serial.print(F("  1 Mbit/s, CAN1 on pins 22 and 23, node "));
    Serial.println(NODE_ID);

    show_errors("at startup");

    const uint8_t w = find_worker();
    if (!w) {
        Serial.println(F("\n  No worker answered discovery."));
        Serial.println(F("  A CAN frame needs one other node to acknowledge it, so a bus of one"));
        Serial.println(F("  cannot transmit at all. If tx errors are climbing and ACK is flagged"));
        Serial.println(F("  below, the wiring is right and the peer is missing. If the counters"));
        Serial.println(F("  are stuck at zero, nothing is being driven onto the pair."));
        show_errors("after discovery");
        return;
    }
    Serial.print(F("\n  worker found at node "));
    Serial.println(w);

    /* ---- 1. latency on a quiet bus ---------------------------------------------------------- */
    n_lat = 0;
    for (uint32_t i = 0; i < N_PING; i++) {
        const uint32_t us = ping_once(w, i);
        if (us) lat[n_lat++] = us;
    }
    report_latency("quiet bus");
    show_errors("after the quiet run");

    /* ---- 2. how many frames the bus will actually carry -------------------------------------- */
    CAN_message_t tx;
    tx.id = ID_LOAD;
    tx.len = 8;
    uint32_t sent = 0;
    const uint32_t t0 = millis();
    while (millis() - t0 < 1000) {
        memcpy(tx.buf, &sent, 4);
        if (can.write(tx)) sent++;
    }
    Serial.print(F("\n  sustained frame rate: "));
    Serial.print(sent);
    Serial.println(F(" frames a second of 8 data bytes"));
    Serial.print(F("    which is "));
    Serial.print(sent * 8 / 1024.0f, 1);
    Serial.println(F(" KB/s of payload"));
    Serial.print(F("    one 4 KB activation would take "));
    Serial.print(4096.0f / (sent * 8.0f) * 1000.0f, 1);
    Serial.println(F(" ms, against 0.148 ms measured over Ethernet"));
    Serial.println(F("    so this bus carries control and never carries work"));
    show_errors("after the throughput run");

    /* ---- 3. latency again, with the bus busy -------------------------------------------------- */
    n_lat = 0;
    uint32_t next_load = micros();
    const uint32_t load_period = 1000000UL / LOAD_HZ;
    for (uint32_t i = 0; i < N_PING; i++) {
        if ((int32_t)(micros() - next_load) >= 0) {
            CAN_message_t f;
            f.id = ID_LOAD;
            f.len = 8;
            memcpy(f.buf, &i, 4);
            can.write(f);
            next_load += load_period;
        }
        const uint32_t us = ping_once(w, 0x80000000UL | i);
        if (us) lat[n_lat++] = us;
    }
    report_latency("loaded bus");
    show_errors("after the loaded run");

    /* ---- what it means for the machine -------------------------------------------------------- */
    Serial.println(F("\n  WHAT 33 NODES WOULD ACTUALLY PUT ON THIS BUS"));
    Serial.println(F("    heartbeat, 1 frame per node at 10 Hz         330 frames a second"));
    Serial.println(F("    health telemetry, 2 frames per node at 1 Hz   66 frames a second"));
    Serial.println(F("    dispatch and completion, 2 per token          a few hundred"));
    Serial.print(F("    against a measured ceiling of "));
    Serial.print(sent);
    Serial.println(F(" frames a second"));
    Serial.println(F("    Control fits several times over. Work does not fit at all."));
}

static void run_worker(void)
{
    Serial.print(F("\n  worker "));
    Serial.print(NODE_ID);
    Serial.println(F(" on CAN1 at 1 Mbit/s, answering pings and discovery"));
    show_errors("at startup");

    uint32_t last_beat = 0;
    for (;;) {
        CAN_message_t rx;
        while (can.read(rx)) {
            if (rx.id == (uint32_t)(ID_PING + NODE_ID)) {
                CAN_message_t tx;
                tx.id = ID_PONG + NODE_ID;
                tx.len = 8;
                memcpy(tx.buf, rx.buf, 8);
                can.write(tx);
            } else if (rx.id == ID_DISCOVER) {
                CAN_message_t tx;
                tx.id = ID_PRESENT + NODE_ID;
                tx.len = 1;
                tx.buf[0] = NODE_ID;
                can.write(tx);
            }
        }

        /* A heartbeat every 100 ms, which is what a supervisor watches to decide a node is gone.
         * Deliberately a separate, higher-priority identifier than the ping traffic. */
        if (millis() - last_beat >= 100) {
            last_beat = millis();
            CAN_message_t hb;
            hb.id = ID_HEARTBEAT + NODE_ID;
            hb.len = 8;
            const uint32_t up = millis();
            memcpy(hb.buf, &up, 4);
            hb.buf[4] = (uint8_t)tempmonGetTemp();
            can.write(hb);
        }
    }
}

/* ------------------------------------------------------------------------------------------- */

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 4000) { }

    can.begin();
    can.setBaudRate(BITRATE);
    /* Accept everything. Filtering belongs in the protocol here, not in the mailboxes, because a
     * supervisor wants to see traffic it is not the target of. */
    can.setMaxMB(16);
    can.enableFIFO();
    can.enableFIFOInterrupt(false);

    if (NODE_ID == 0) run_master();
    else              run_worker();
}

void loop() { }

/* ===========================================================================================
 *  bench_master.cpp -- BENCH ONE hub logic for Teensy 1
 * ===========================================================================================
 */

#include "bench_master.h"
#include "bench_ports.h"
#include "bench_display.h"

BenchLink   gLuckfox;
BenchLink   gWorker;
BenchLink   gHmi;
BenchFabric gFabric;

/* Receive rings, one per link, sized from the traffic each actually carries.
 *
 * At 921600 a byte is 10.9 us, so 8 KB is about 89 ms of headroom against a busy loop().
 * The Luckfox link gets the big one because it is the only link that can be asked to carry bulk
 * results and a Linux box can burst harder than any MCU here. The display link gets less because
 * it receives almost nothing -- it is written to, and it answers with short touch events. */
static uint8_t gLuckfoxRx[8192];
static uint8_t gWorkerRx[4096];
static uint8_t gHmiRx[2048];

/* Event subscription, defaulting to OFF.
 *
 * This is Defect 1 from PLATFORM_BASELINE.md applied to the new links. That defect was an
 * unconditional relay: the radio pushed 202 packets per second at the brain, which never asked
 * and could not decline. The write-up's conclusion was that "should this telemetry go upward,
 * and how much of it" is a policy decision, and policy does not live on the node producing the
 * data.
 *
 * The same trap exists here, three times over: fabric interrupts, worker progress and display
 * touches all originate below the Luckfox. So the Luckfox subscribes to what it wants, and gets
 * nothing until it does. */
static uint32_t gEventMask = BENCH_EVMASK_NONE;
static uint8_t  gEventDivisor = 1u;
static uint32_t gEventCounter = 0;

/* Fabric interrupt: set by the ISR, consumed by loop().
 *
 * The ISR does NOTHING except set this flag. It cannot do more: servicing an MCP23017 means I2C
 * transactions, which take ~123 us at 400 kHz and call yield() internally on this core. Running
 * that from interrupt context would re-enter the serial event handlers from inside an ISR, which
 * is a class of bug that presents as random corruption rather than as a crash. */
static volatile bool gFabricIrqFlag = false;

static void fabricIsr(void) { gFabricIrqFlag = true; }

/* ===========================================================================================
 * PAYLOAD HELPERS -- explicit big-endian, never a struct cast
 * =========================================================================================== */
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ===========================================================================================
 * UPWARD EVENTS
 * =========================================================================================== */

static void emitEvent(uint32_t maskBit, uint8_t srcNode, uint8_t chan, uint8_t cmd,
                      const uint8_t *payload, uint16_t len)
{
    if ((gEventMask & maskBit) == 0u) { return; }

    /* Decimation, same shape as the mesh stream's divisor. A divisor of 20 means every 20th
     * event goes up: enough to see that something is happening and to sample its content,
     * without the link spending its whole budget on telemetry nobody is reading. */
    if (gEventDivisor > 1u) {
        gEventCounter++;
        if ((gEventCounter % gEventDivisor) != 0u) { return; }
    }

    static uint8_t buf[IOP_MAX_PAYLOAD_LEN];
    if ((uint32_t)len + 3u > IOP_MAX_PAYLOAD_LEN) { return; }

    buf[0] = srcNode;
    buf[1] = chan;
    buf[2] = cmd;
    if (len) { memcpy(&buf[3], payload, len); }

    gLuckfox.event(BENCH_CHAN_ORCH, BENCH_CMD_ORC_EVENT, buf, (uint16_t)(len + 3u));
}

/* ===========================================================================================
 * FABRIC CALLBACKS
 * =========================================================================================== */

static void onFabricFault(uint8_t code, uint8_t port, uint16_t detail)
{
    uint8_t p[8];
    p[0] = code;
    p[1] = port;
    put16(&p[2], detail);
    put32(&p[4], micros());

    emitEvent(BENCH_EVMASK_FABRIC_FAULT, BENCH_NODE_TEENSY1,
              BENCH_CHAN_FABRIC, BENCH_CMD_FAB_FAULT_EVENT, p, 8);

    Serial.printf("[fabric] FAULT %s port=%s detail=0x%04X\n",
                  bench_fault_name(code), bench_port_name(port), (unsigned)detail);
}

static void onFabricIrqEvent(uint8_t portId, uint16_t intf, uint16_t intcap, uint16_t gpio)
{
    uint8_t p[11];
    p[0] = portId;
    put16(&p[1], intf);
    put16(&p[3], intcap);
    put16(&p[5], gpio);
    put32(&p[7], micros());

    emitEvent(BENCH_EVMASK_FABRIC_IRQ, BENCH_NODE_TEENSY1,
              BENCH_CHAN_FABRIC, BENCH_CMD_FAB_IRQ_EVENT, p, 11);
}

/* ===========================================================================================
 * FABRIC COMMAND HANDLING -- channel 0x05
 * ===========================================================================================
 * These arrive from ANY link. The Luckfox is the usual caller, but the display node asking for a
 * sensor value and the radio asking to toggle an antenna switch are equally legal. Teensy1 is
 * the only node wired to the buses, so every one of them goes through here.
 */

static void handleFabric(BenchLink &lk, const IopFrame *f)
{
    static uint8_t out[IOP_MAX_PAYLOAD_LEN];
    uint8_t st;

    switch (f->cmd) {

    case BENCH_CMD_FAB_INFO_REQ: {
        const BenchFabricStats &s = gFabric.stats();
        out[0] = gFabric.isReady() ? BENCH_ST_OK : BENCH_ST_HW_FAULT;
        out[1] = BENCH_FABRIC_HW_REV;
        out[2] = IOP_PROTOCOL_VERSION;
        out[3] = BENCH_PROTOCOL_VERSION;
        out[4] = (uint8_t)BENCH_PORT_COUNT;
        put32(&out[5],  s.i2c_transactions);
        put32(&out[9],  s.i2c_nacks);
        put32(&out[13], s.spi_transfers);
        put32(&out[17], s.irq_events);
        put32(&out[21], s.irq_storms);
        put32(&out[25], s.faults);
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_INFO_RESP, out, 29);
        break;
    }

    case BENCH_CMD_FAB_PORTS_REQ: {
        /* The whole port table in one frame. 29 ports x 12 bytes = 348, comfortably inside the
         * 1015-byte payload, so the Linux side can never see a half-read map. Discovering the
         * fabric at runtime is what stops a second copy of the map existing to drift. */
        uint16_t n = 0;
        out[0] = BENCH_ST_OK;
        out[1] = (uint8_t)BENCH_PORT_COUNT;
        n = 2;
        for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
            const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
            if ((uint32_t)n + BENCH_PORT_DESC_WIRE_LEN > IOP_MAX_PAYLOAD_LEN) { break; }
            out[n++] = p->id;      out[n++] = p->kind;
            out[n++] = p->bus;     out[n++] = p->slot;
            out[n++] = p->first;   out[n++] = p->width;
            out[n++] = p->irq_bit; out[n++] = p->ctl_bit;
            put16(&out[n], p->flags);    n += 2;
            put16(&out[n], p->detected); n += 2;
        }
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PORTS_RESP, out, n);
        break;
    }

    case BENCH_CMD_FAB_SCAN_REQ: {
        uint8_t bus = (f->payload_len > 0u) ? f->payload[0] : 0u;
        uint8_t found[112];
        uint8_t n = gFabric.i2cScan(bus, found, sizeof(found));
        out[0] = BENCH_ST_OK;
        out[1] = bus;
        out[2] = n;
        memcpy(&out[3], found, n);
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SCAN_RESP, out,
                   (uint16_t)(3u + n));
        break;
    }

    case BENCH_CMD_FAB_PIN_MODE_REQ:
        st = (f->payload_len >= 3u)
             ? gFabric.pinMode_(f->payload[0], f->payload[1], f->payload[2])
             : BENCH_ST_BAD_ARG;
        out[0] = st;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PIN_MODE_RESP, out, 1);
        break;

    case BENCH_CMD_FAB_PIN_WRITE_REQ:
        st = (f->payload_len >= 3u)
             ? gFabric.pinWrite(f->payload[0], f->payload[1], f->payload[2])
             : BENCH_ST_BAD_ARG;
        out[0] = st;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PIN_WRITE_RESP, out, 1);
        break;

    case BENCH_CMD_FAB_PIN_READ_REQ: {
        uint8_t v = 0;
        st = (f->payload_len >= 2u)
             ? gFabric.pinRead(f->payload[0], f->payload[1], &v)
             : BENCH_ST_BAD_ARG;
        out[0] = st; out[1] = v;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PIN_READ_RESP, out, 2);
        break;
    }

    case BENCH_CMD_FAB_PORT_READ_REQ: {
        uint16_t v = 0;
        st = (f->payload_len >= 1u) ? gFabric.portRead16(f->payload[0], &v) : BENCH_ST_BAD_ARG;
        out[0] = st; out[1] = 2u; put16(&out[2], v);
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PORT_READ_RESP, out, 4);
        break;
    }

    case BENCH_CMD_FAB_PORT_WRITE_REQ:
        st = (f->payload_len >= 4u)
             ? gFabric.portWrite16(f->payload[0], get16(&f->payload[2]))
             : BENCH_ST_BAD_ARG;
        out[0] = st;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_PORT_WRITE_RESP, out, 1);
        break;

    case BENCH_CMD_FAB_IRQ_CFG_REQ:
        st = (f->payload_len >= 3u)
             ? gFabric.irqConfig(f->payload[0], get16(&f->payload[1]))
             : BENCH_ST_BAD_ARG;
        out[0] = st;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_IRQ_CFG_RESP, out, 1);
        break;

    case BENCH_CMD_FAB_SPI_XFER_REQ: {
        /* [port][mode][speed_khz16][flags][len16][tx...] -- the escape hatch. Any SPI module,
         * no new firmware. */
        if (f->payload_len < 7u) {
            out[0] = BENCH_ST_BAD_ARG; out[1] = 0; out[2] = 0;
            lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SPI_XFER_RESP, out, 3);
            break;
        }
        uint8_t  port = f->payload[0];
        uint8_t  mode = f->payload[1];
        uint32_t hz   = (uint32_t)get16(&f->payload[2]) * 1000ul;
        uint16_t len  = get16(&f->payload[5]);

        if ((uint32_t)len + 7u > f->payload_len) { len = (uint16_t)(f->payload_len - 7u); }
        if ((uint32_t)len + 3u > IOP_MAX_PAYLOAD_LEN) { len = IOP_MAX_PAYLOAD_LEN - 3u; }

        static uint8_t rx[IOP_MAX_PAYLOAD_LEN];
        st = gFabric.spiTransfer(port, mode, hz, &f->payload[7], rx, len);

        out[0] = st;
        put16(&out[1], (st == BENCH_ST_OK) ? len : 0u);
        if (st == BENCH_ST_OK && len) { memcpy(&out[3], rx, len); }
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SPI_XFER_RESP, out,
                   (uint16_t)(3u + ((st == BENCH_ST_OK) ? len : 0u)));
        break;
    }

    case BENCH_CMD_FAB_I2C_XFER_REQ: {
        /* [bus][addr7][wr_len][rd_len][wr...] -- write-then-read with a repeated START. */
        if (f->payload_len < 4u) {
            out[0] = BENCH_ST_BAD_ARG; out[1] = 0;
            lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_I2C_XFER_RESP, out, 2);
            break;
        }
        uint8_t bus   = f->payload[0];
        uint8_t addr  = f->payload[1];
        uint8_t wrLen = f->payload[2];
        uint8_t rdLen = f->payload[3];

        if ((uint32_t)wrLen + 4u > f->payload_len) { wrLen = (uint8_t)(f->payload_len - 4u); }
        if (rdLen > 200u) { rdLen = 200u; }   /* Wire's own buffer bounds this well below it   */

        static uint8_t rd[208];
        st = gFabric.i2cTransfer(bus, addr, &f->payload[4], wrLen, rd, rdLen);

        out[0] = st;
        out[1] = (st == BENCH_ST_OK) ? rdLen : 0u;
        if (st == BENCH_ST_OK && rdLen) { memcpy(&out[2], rd, rdLen); }
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_I2C_XFER_RESP, out,
                   (uint16_t)(2u + ((st == BENCH_ST_OK) ? rdLen : 0u)));
        break;
    }

    case BENCH_CMD_FAB_SHIFT_OUT_REQ: {
        if (f->payload_len < 2u) {
            out[0] = BENCH_ST_BAD_ARG;
            lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SHIFT_OUT_RESP, out, 1);
            break;
        }
        uint8_t chips = f->payload[1];
        if ((uint32_t)chips + 2u > f->payload_len) { chips = (uint8_t)(f->payload_len - 2u); }
        st = gFabric.shiftOut_(&f->payload[2], chips);
        out[0] = st;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SHIFT_OUT_RESP, out, 1);
        break;
    }

    case BENCH_CMD_FAB_SHIFT_IN_REQ: {
        uint8_t chips = (f->payload_len >= 2u) ? f->payload[1] : 1u;
        if (chips > FAB_MAX_165) { chips = FAB_MAX_165; }
        static uint8_t in[FAB_MAX_165];
        st = gFabric.shiftIn_(in, chips);
        out[0] = st;
        out[1] = (st == BENCH_ST_OK) ? chips : 0u;
        if (st == BENCH_ST_OK) { memcpy(&out[2], in, chips); }
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_SHIFT_IN_RESP, out,
                   (uint16_t)(2u + ((st == BENCH_ST_OK) ? chips : 0u)));
        break;
    }

    case BENCH_CMD_FAB_ADC_REQ: {
        uint16_t v = 0;
        uint8_t leaf = (f->payload_len > 0u) ? f->payload[0] : 0u;
        uint8_t ch   = (f->payload_len > 1u) ? f->payload[1] : 0u;
        uint8_t n    = (f->payload_len > 2u) ? f->payload[2] : 1u;
        st = gFabric.adcRead(leaf, ch, n, &v);
        out[0] = st;
        out[1] = (uint8_t)((leaf << 3) | (ch & 7u));
        put16(&out[2], v);
        put16(&out[4], v);
        out[6] = n;
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_ADC_RESP, out, 7);
        break;
    }

    case BENCH_CMD_FAB_ADC_SWEEP_REQ: {
        uint8_t first = (f->payload_len > 0u) ? f->payload[0] : 0u;
        uint8_t count = (f->payload_len > 1u) ? f->payload[1] : 8u;
        uint8_t n     = (f->payload_len > 2u) ? f->payload[2] : 1u;
        if (count > 64u) { count = 64u; }
        static uint16_t vals[64];
        st = gFabric.adcSweep(first, count, n, vals);
        out[0] = st; out[1] = first; out[2] = count;
        for (uint8_t i = 0; i < count; i++) { put16(&out[3 + i * 2], vals[i]); }
        lk.respond(f->seq, BENCH_CHAN_FABRIC, BENCH_CMD_FAB_ADC_SWEEP_RESP, out,
                   (uint16_t)(3u + count * 2u));
        break;
    }

    default:
        lk.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
        break;
    }
}

/* ===========================================================================================
 * ORCHESTRATOR COMMAND HANDLING -- channel 0x07, from the Luckfox
 * =========================================================================================== */

static void handleOrch(const IopFrame *f)
{
    static uint8_t out[IOP_MAX_PAYLOAD_LEN];
    uint32_t now = millis();

    switch (f->cmd) {

    case BENCH_CMD_ORC_SYS_REQ: {
        /* Whole-stack health in one round trip, so the Linux box does not have to poll four
         * things and stitch the answers together at four different instants. */
        uint8_t n = 0;
        out[n++] = BENCH_ST_OK;
        uint8_t mask = 0;
        if (gWorker.isUp(now))  { mask |= 0x01u; }
        if (gHmi.isUp(now))     { mask |= 0x02u; }
        if (gFabric.isReady())  { mask |= 0x04u; }
        out[n++] = mask;
        out[n++] = 3u;

        out[n++] = BENCH_NODE_TEENSY2;
        out[n++] = gWorker.isUp(now) ? 1u : 0u;
        put32(&out[n], now); n += 4;
        put16(&out[n], (uint16_t)gWorker.stats().timeouts); n += 2;

        out[n++] = BENCH_NODE_HMI;
        out[n++] = gHmi.isUp(now) ? 1u : 0u;
        put32(&out[n], now); n += 4;
        put16(&out[n], (uint16_t)gHmi.stats().timeouts); n += 2;

        out[n++] = BENCH_NODE_TEENSY1;
        out[n++] = gFabric.isReady() ? 1u : 0u;
        put32(&out[n], now); n += 4;
        put16(&out[n], (uint16_t)gFabric.stats().faults); n += 2;

        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_SYS_RESP, out, n);
        break;
    }

    case BENCH_CMD_ORC_SUB_REQ: {
        if (f->payload_len >= 5u) {
            gEventMask    = get32(&f->payload[0]);
            gEventDivisor = f->payload[4] ? f->payload[4] : 1u;
        }
        out[0] = BENCH_ST_OK;
        put32(&out[1], gEventMask);
        out[5] = gEventDivisor;
        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_SUB_RESP, out, 6);
        break;
    }

    case BENCH_CMD_ORC_SYNC_REQ: {
        /* Clock-offset probe, and it does NOT compare two clocks.
         *
         * The host's timestamp is echoed back untouched, and this node adds its own timestamp
         * from its own clock. The HOST then computes the offset from three numbers it owns two
         * of: t_send, t_recv and the echoed t_send. That keeps this project's rule intact --
         * every figure is a single-clock delta -- while still letting two stacks correlate
         * events. Nothing here subtracts one board's time from another's. */
        uint8_t n = 0;
        out[n++] = BENCH_ST_OK;
        uint8_t echoLen = (f->payload_len > 8u) ? 8u : (uint8_t)f->payload_len;
        memcpy(&out[n], f->payload, echoLen); n += echoLen;
        for (; echoLen < 8u; echoLen++) { out[n++] = 0; }
        uint32_t us = micros();
        put32(&out[n], 0); n += 4;
        put32(&out[n], us); n += 4;
        put32(&out[n], ARM_DWT_CYCCNT); n += 4;
        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_SYNC_RESP, out, n);
        break;
    }

    case BENCH_CMD_ORC_MARK_REQ: {
        uint8_t id = (f->payload_len > 0u) ? f->payload[0] : 1u;
        uint8_t pulses = (f->payload_len > 1u) ? f->payload[1] : 1u;
        uint32_t us = micros();
        gFabric.mark(pulses);
        out[0] = BENCH_ST_OK; out[1] = id;
        put32(&out[2], 0); put32(&out[6], us);
        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_MARK_RESP, out, 10);
        break;
    }

    case BENCH_CMD_ORC_SELFTEST_REQ: {
        uint8_t level = (f->payload_len > 0u) ? f->payload[0] : 0u;
        static uint8_t results[4 * 16];
        uint8_t quads = gFabric.selfTest(level, results, 16);
        out[0] = BENCH_ST_OK;
        out[1] = level;
        out[2] = quads;
        memcpy(&out[3], results, (size_t)quads * 4u);
        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_SELFTEST_RESP, out,
                         (uint16_t)(3u + quads * 4u));
        break;
    }

    case BENCH_CMD_ORC_FWD_REQ: {
        /* [dst_node][chan][cmd][len16][payload...] -- relay to a node this hub owns.
         *
         * The reply is NOT synchronous. This hub forwards, and the far end's answer comes back
         * later as an ORC_EVENT. Blocking here waiting for the downstream response would hold
         * the Luckfox's single request slot for as long as the downstream took, which for a WiFi
         * scan is fifteen seconds -- during which the Linux box could not even ask what was
         * happening. */
        if (f->payload_len < 5u) {
            out[0] = BENCH_ST_BAD_ARG;
            gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_FWD_RESP, out, 1);
            break;
        }
        uint8_t dst  = f->payload[0];
        uint8_t chan = f->payload[1];
        uint8_t cmd  = f->payload[2];
        uint16_t len = get16(&f->payload[3]);
        if ((uint32_t)len + 5u > f->payload_len) { len = (uint16_t)(f->payload_len - 5u); }

        BenchLink *target = 0;
        uint32_t timeout = BENCH_TIMEOUT_ORCH_MS;
        if      (dst == BENCH_NODE_TEENSY2) { target = &gWorker; timeout = BENCH_TIMEOUT_WORKER_BENCH_MS; }
        else if (dst == BENCH_NODE_HMI)     { target = &gHmi;    timeout = BENCH_TIMEOUT_HMI_MS; }

        if (!target) {
            /* The radio is deliberately NOT reachable this way. Its link belongs to his tested
             * sketch, and routing frames into it from here would mean two owners of one pending
             * slot -- which is the multi-master problem in a different costume. */
            out[0] = BENCH_ST_UNSUPPORTED;
            gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_FWD_RESP, out, 1);
            break;
        }

        uint8_t st = target->request(chan, cmd, &f->payload[5], len, timeout);
        out[0] = st;
        out[1] = dst;
        out[2] = chan;
        out[3] = cmd;
        put16(&out[4], 0);
        gLuckfox.respond(f->seq, BENCH_CHAN_ORCH, BENCH_CMD_ORC_FWD_RESP, out, 6);
        break;
    }

    default:
        gLuckfox.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CMD);
        break;
    }
}

/* ===========================================================================================
 * LINK CALLBACKS
 * =========================================================================================== */

static void onLuckfoxFrame(void *ctx, const IopFrame *f, bool solicited)
{
    (void)ctx; (void)solicited;

    if (f->chan == IOP_CHAN_TRANSPORT && f->cmd == IOP_CMD_PING) {
        gLuckfox.respond(f->seq, IOP_CHAN_TRANSPORT, IOP_CMD_PONG, 0, 0);
        return;
    }
    if (f->chan == BENCH_CHAN_FABRIC) { handleFabric(gLuckfox, f); return; }
    if (f->chan == BENCH_CHAN_ORCH)   { handleOrch(f);             return; }

    gLuckfox.nack(f->seq, f->chan, f->cmd, IOP_ERR_UNKNOWN_CHAN);
}

static void onWorkerFrame(void *ctx, const IopFrame *f, bool solicited)
{
    (void)ctx;

    if (f->chan == BENCH_CHAN_WORKER && f->cmd == BENCH_CMD_WRK_HELLO) {
        Serial.println(F("[worker] HELLO -- node booted"));
        /* Header-drift check, on the first frame of the session. A mismatched CRC check value
         * produces a stream of CRC failures that looks exactly like a wiring fault, and this
         * project has already paid for that lesson once. */
        if (f->payload_len >= 9u) {
            uint16_t peerCheck = get16(&f->payload[7]);
            if (peerCheck != IOP_CRC16_CHECK_VALUE) {
                Serial.printf("[worker] PROTOCOL MISMATCH: peer CRC check 0x%04X, ours 0x%04X\n",
                              (unsigned)peerCheck, (unsigned)IOP_CRC16_CHECK_VALUE);
                Serial.println(F("         The headers have drifted. Re-run sync_headers.py and "
                                 "REFLASH BOTH BOARDS."));
            }
        }
        emitEvent(BENCH_EVMASK_LINK, BENCH_NODE_TEENSY2, f->chan, f->cmd,
                  f->payload, f->payload_len);
        return;
    }

    /* Everything else from the worker goes upward if the Luckfox subscribed. Responses are
     * forwarded too, because a forwarded request's answer has to reach whoever asked. */
    emitEvent(solicited ? BENCH_EVMASK_WORKER_JOB : BENCH_EVMASK_WORKER_JOB,
              BENCH_NODE_TEENSY2, f->chan, f->cmd, f->payload, f->payload_len);
}

static void onHmiFrame(void *ctx, const IopFrame *f, bool solicited)
{
    (void)ctx; (void)solicited;

    if (f->chan == BENCH_CHAN_HMI && f->cmd == BENCH_CMD_HMI_HELLO) {
        Serial.println(F("[hmi] HELLO -- display node booted"));
        emitEvent(BENCH_EVMASK_LINK, BENCH_NODE_HMI, f->chan, f->cmd,
                  f->payload, f->payload_len);
        return;
    }

    if (f->chan == BENCH_CHAN_HMI && f->cmd == BENCH_CMD_HMI_INPUT_EVENT) {
        /* A touch is an EVENT, not a command. The display reports that a finger landed at a
         * coordinate; it does not decide what that means. Same rule as the radio -- mechanism at
         * the leaf, policy at the hub -- and it is what lets the display be unplugged without
         * anything else caring. */
        emitEvent(BENCH_EVMASK_HMI_INPUT, BENCH_NODE_HMI, f->chan, f->cmd,
                  f->payload, f->payload_len);
        return;
    }

    emitEvent(BENCH_EVMASK_HMI_INPUT, BENCH_NODE_HMI, f->chan, f->cmd,
              f->payload, f->payload_len);
}

static void onLinkTimeout(void *ctx, uint8_t chan, uint8_t cmd, uint8_t seq)
{
    (void)seq;
    const char *who = (const char *)ctx;
    Serial.printf("[%s] TIMEOUT waiting for a reply to %s/%s\n",
                  who ? who : "?", bench_chan_name(chan), bench_cmd_name(cmd));
}

/* ===========================================================================================
 * SETUP AND LOOP
 * =========================================================================================== */

void benchSetup(void)
{
    Serial.println(F("\n[bench] BENCH ONE extension starting"));

    gLuckfox.begin(&T1_LUCKFOX_SERIAL, "luckfox", BENCH_NODE_LUCKFOX, BENCH_LINK_BAUD,
                   gLuckfoxRx, sizeof(gLuckfoxRx));
    gWorker.begin(&T1_WORKER_SERIAL,  "worker",  BENCH_NODE_TEENSY2, BENCH_LINK_BAUD,
                  gWorkerRx, sizeof(gWorkerRx));
    gHmi.begin(&T1_HMI_SERIAL,        "hmi",     BENCH_NODE_HMI,     BENCH_LINK_BAUD,
               gHmiRx, sizeof(gHmiRx));

    gLuckfox.setHandlers((void *)"luckfox", onLuckfoxFrame, onLinkTimeout);
    gWorker.setHandlers((void *)"worker",   onWorkerFrame,  onLinkTimeout);
    gHmi.setHandlers((void *)"hmi",         onHmiFrame,     onLinkTimeout);

    uint8_t st = gFabric.begin(onFabricFault);
    if (st == BENCH_ST_OK) {
        Serial.printf("[bench] fabric ready, hw rev %u, %u ports\n",
                      (unsigned)BENCH_FABRIC_HW_REV, (unsigned)BENCH_PORT_COUNT);
        /* FALLING, not LOW. The MCP23017 INT lines are open-drain and wire-OR onto one pin, so
         * a level trigger would re-enter the handler continuously for as long as any expander
         * held the line -- which, before the service reads both ports, is always. */
        attachInterrupt(digitalPinToInterrupt(T1_FABRIC_IRQ), fabricIsr, FALLING);
    } else {
        Serial.printf("[bench] FABRIC DID NOT START: %s\n", bench_st_name(st));
        Serial.println(F("        The port table is incoherent. Nothing was initialised, on "
                         "purpose: running with a table that cannot be right is worse."));
    }

    /* The 2.4in TFT. Its own bus (SPI0), so its refreshes never queue behind fabric traffic --
     * and, more importantly, never land inside a latency measurement and get recorded as latency. */
    uint8_t ctrl = benchDisplayBegin();
    Serial.printf("[bench] display: %s (%s)\n", benchDisplayControllerName(),
                  benchDisplayIdentified() ? "read from the chip" : "ASSUMED -- MISO not readable");
    (void)ctrl;
    benchDisplaySetRow(0, "BENCH ONE");
    benchDisplayPrintf(1, ".fabric  %s", gFabric.isReady() ? "ready" : "FAULT");
    benchDisplayPrintf(2, ".ports   %u", (unsigned)BENCH_PORT_COUNT);
    benchDisplayRender();

    /* Announce upward. The Luckfox re-anchors its event counter on this, so a Teensy reflash
     * mid-session is reported as a reboot rather than as 127 missed events. */
    uint8_t p[10];
    p[0] = BENCH_NODE_TEENSY1;
    p[1] = IOP_PROTOCOL_VERSION;
    p[2] = IOP_PROTOCOL_MINOR;
    p[3] = BENCH_PROTOCOL_VERSION;
    p[4] = 0;
    put16(&p[5], 0x0000u);
    put16(&p[7], IOP_CRC16_CHECK_VALUE);
    p[9] = BENCH_FABRIC_HW_REV;
    gLuckfox.event(BENCH_CHAN_ORCH, BENCH_CMD_ORC_HELLO, p, 10);
}

void benchLoop(void)
{
    gLuckfox.poll();
    gWorker.poll();
    gHmi.poll();

    /* The ISR only set a flag. The actual service runs here, at task level, because it does I2C
     * -- roughly 123 us per expander at 400 kHz, and Wire's endTransmission() calls yield() on
     * this core, which would dispatch serial event handlers from inside an interrupt. */
    if (gFabricIrqFlag) {
        gFabricIrqFlag = false;
        gFabric.serviceIrq(onFabricIrqEvent);
    }

    /* Refresh the panel once a second, and only the rows that changed. Deliberately NOT gated on
     * a request being in flight: the display is alone on SPI0, so it cannot delay a fabric or
     * link operation the way the old shared-bus OLED could. */
    static uint32_t lastPaint = 0;
    uint32_t now = millis();
    if ((now - lastPaint) >= 1000u) {
        lastPaint = now;
        const BenchFabricStats &s = gFabric.stats();
        benchDisplayPrintf(1, "%sfabric %s", gFabric.isReady() ? "+" : "!",
                           gFabric.isReady() ? "ready" : "FAULT");
        benchDisplayPrintf(2, "%sluckfox %s", gLuckfox.isUp(now) ? "+" : ".",
                           gLuckfox.isUp(now) ? "up" : "down");
        benchDisplayPrintf(3, "%sworker  %s", gWorker.isUp(now) ? "+" : ".",
                           gWorker.isUp(now) ? "up" : "down");
        benchDisplayPrintf(4, "%shmi     %s", gHmi.isUp(now) ? "+" : ".",
                           gHmi.isUp(now) ? "up" : "down");
        benchDisplayPrintf(6, ".spi %lu  i2c %lu", (unsigned long)s.spi_transfers,
                           (unsigned long)s.i2c_transactions);
        benchDisplayPrintf(7, "%sfaults %lu", s.faults ? "!" : ".", (unsigned long)s.faults);
    }

    /* Rendering runs on a BUDGET, and only when no request is in flight on any link. Both
     * conditions matter: the budget bounds how long a single call can block, and the idle check
     * keeps a repaint from landing between a request and its response, where it would be
     * recorded as latency. That is the failure this project's bring-up log already documents. */
    if (!gLuckfox.pending() && !gWorker.pending() && !gHmi.pending()) {
        benchDisplayService(2000u);
    }
}

/* ===========================================================================================
 * CONSOLE
 * ===========================================================================================
 * Returns true if the line was consumed. His dispatcher is left untouched.
 */
bool benchConsole(const char *line)
{
    if (!line) { return false; }
    uint32_t now = millis();

    if (strcmp(line, "bench") == 0 || strcmp(line, "stack") == 0) {
        Serial.println(F("\n--- BENCH ONE stack ---"));
        Serial.printf("  luckfox  %-5s  tx %lu  rx %lu  timeouts %lu  stale %lu\n",
                      gLuckfox.isUp(now) ? "UP" : "down",
                      (unsigned long)gLuckfox.stats().frames_tx,
                      (unsigned long)gLuckfox.stats().frames_rx,
                      (unsigned long)gLuckfox.stats().timeouts,
                      (unsigned long)gLuckfox.stats().stale);
        Serial.printf("  worker   %-5s  tx %lu  rx %lu  timeouts %lu  stale %lu\n",
                      gWorker.isUp(now) ? "UP" : "down",
                      (unsigned long)gWorker.stats().frames_tx,
                      (unsigned long)gWorker.stats().frames_rx,
                      (unsigned long)gWorker.stats().timeouts,
                      (unsigned long)gWorker.stats().stale);
        Serial.printf("  hmi      %-5s  tx %lu  rx %lu  timeouts %lu  stale %lu\n",
                      gHmi.isUp(now) ? "UP" : "down",
                      (unsigned long)gHmi.stats().frames_tx,
                      (unsigned long)gHmi.stats().frames_rx,
                      (unsigned long)gHmi.stats().timeouts,
                      (unsigned long)gHmi.stats().stale);
        Serial.printf("  events   mask 0x%08lX  divisor %u\n",
                      (unsigned long)gEventMask, (unsigned)gEventDivisor);
        return true;
    }

    if (strcmp(line, "fabric") == 0) {
        const BenchFabricStats &s = gFabric.stats();
        Serial.println(F("\n--- fabric ---"));
        Serial.printf("  ready ............... %s\n", gFabric.isReady() ? "yes" : "NO");
        Serial.printf("  spi transfers ....... %lu\n", (unsigned long)s.spi_transfers);
        Serial.printf("  i2c transactions .... %lu\n", (unsigned long)s.i2c_transactions);
        Serial.printf("  i2c NACKs ........... %lu\n", (unsigned long)s.i2c_nacks);
        Serial.printf("  i2c recoveries ...... %lu\n", (unsigned long)s.i2c_recoveries);
        Serial.printf("  irq events .......... %lu\n", (unsigned long)s.irq_events);
        Serial.printf("  irq spurious ........ %lu   (line low, nobody claimed it)\n",
                      (unsigned long)s.irq_spurious);
        Serial.printf("  irq STORMS .......... %lu   (still low after full service)\n",
                      (unsigned long)s.irq_storms);
        Serial.printf("  adc conversions ..... %lu\n", (unsigned long)s.adc_conversions);
        Serial.printf("  adc discarded ....... %lu   (first-after-switch, on purpose)\n",
                      (unsigned long)s.adc_discarded);
        Serial.printf("  cs changes .......... %lu\n", (unsigned long)s.cs_changes);
        Serial.printf("  faults .............. %lu\n", (unsigned long)s.faults);
        return true;
    }

    if (strcmp(line, "scan") == 0) {
        uint8_t found[112];
        uint8_t n = gFabric.i2cScan(0, found, sizeof(found));
        Serial.printf("\n[scan] Wire: %u device%s\n", (unsigned)n, (n == 1u) ? "" : "s");
        for (uint8_t i = 0; i < n; i++) {
            Serial.printf("  0x%02X", (unsigned)found[i]);
            if (found[i] >= FAB_MCP_ADDR_BASE && found[i] <= FAB_MCP_ADDR_MAX) {
                Serial.print(F("   (MCP23017 address range -- also PCF8574 and many radios)"));
            }
            Serial.println();
        }
        if (n == 0u) {
            Serial.println(F("  nothing answered. Check: 2.2k pull-ups fitted ONCE at the "
                             "master, address straps not floating, 3.3V present."));
        }
        return true;
    }

    if (strcmp(line, "ports") == 0) {
        Serial.println(F("\n--- port map ---"));
        Serial.println(F("  name  id    kind        bus/slot  bits   present"));
        for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
            const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
            Serial.printf("  %-4s  0x%02X  %-10s  %u/0x%02X    %u+%-2u  %s%s\n",
                          BENCH_PORT_NAMES[i], (unsigned)p->id,
                          bench_port_kind_name(p->kind),
                          (unsigned)p->bus, (unsigned)p->slot,
                          (unsigned)p->first, (unsigned)p->width,
                          gFabric.portPresent(p->id) ? "yes" : "-",
                          (p->flags & BENCH_PORTF_RESERVED) ? "  [reserved]" : "");
        }
        return true;
    }

    if (strcmp(line, "selftest") == 0) {
        uint8_t results[4 * 16];
        uint8_t quads = gFabric.selfTest(0, results, 16);
        Serial.println(F("\n--- self-test (level 0, non-invasive) ---"));
        for (uint8_t i = 0; i < quads; i++) {
            uint8_t id = results[i * 4 + 0];
            uint8_t rs = results[i * 4 + 1];
            uint16_t detail = (uint16_t)((results[i * 4 + 2] << 8) | results[i * 4 + 3]);
            const char *rn = (rs == BENCH_TESTRESULT_PASS)    ? "PASS"
                           : (rs == BENCH_TESTRESULT_FAIL)    ? "FAIL"
                           : (rs == BENCH_TESTRESULT_SKIPPED) ? "skip" : "----";
            Serial.printf("  %-16s %s   0x%04X\n", bench_test_name(id), rn, (unsigned)detail);
        }
        Serial.println(F("  'skip' is not 'pass'. CS_EXCLUSIVE and CS_DESELECT cannot be read"));
        Serial.println(F("  back from inside the Teensy -- capture them on the analyser."));
        return true;
    }

    if (strcmp(line, "mark") == 0) {
        gFabric.mark(3);
        Serial.println(F("[mark] 3 pulses on the analyser marker pin"));
        return true;
    }

    if (strcmp(line, "park") == 0) {
        gFabric.parkSafe();
        Serial.println(F("[fabric] parked: decoder off, MISO gate Hi-Z, 595 cleared, mux off"));
        return true;
    }

    return false;
}

/* ===========================================================================================
 *  bench_fabric.cpp -- BENCH ONE logic fabric driver implementation
 * ===========================================================================================
 *  See bench_fabric.h for the six guarantees this file exists to keep.
 * ===========================================================================================
 */

#include "bench_fabric.h"
#include <SPI.h>
#include <Wire.h>

/* ===========================================================================================
 * CONSTRUCTION AND BRING-UP
 * =========================================================================================== */

BenchFabric::BenchFabric()
    : mFaultFn(0), mReady(false), mShiftChips(0), mCtlByte(0),
      mMcpPresent(0), mCsCurrent(0), mCsEnabled(false)
{
    memset(&mStats, 0, sizeof(mStats));
    memset(mShiftOut, 0, sizeof(mShiftOut));
    memset(mMcpOlat, 0, sizeof(mMcpOlat));
    memset(mMcpPullup, 0, sizeof(mMcpPullup));
    /* Inputs are the safe default for an expander whose wiring is not yet known: an input
     * cannot fight whatever is on the other end of the wire. IODIR = 1 means input. */
    for (uint8_t i = 0; i < FAB_MAX_MCP; i++) { mMcpIodir[i] = 0xFFFFu; }
}

uint8_t BenchFabric::begin(BenchFaultFn faultFn)
{
    mFaultFn = faultFn;

    /* The port table is checked BEFORE any pin is driven. If two ports claim one chip-select
     * slot, initialising the hardware would assert a select that two modules answer, and on a
     * shared MISO that is bus contention -- current through two fighting output stages. Refusing
     * to start is the correct response to a configuration that cannot be right. */
    uint8_t bad = bench_ports_validate();
    if (bad != 0u) {
        reportFault(BENCH_FAULT_CS_CONFLICT, bad, 0);
        return BENCH_ST_HW_FAULT;
    }

    /* ORDER MATTERS HERE, and getting it wrong is a real hazard rather than a style point.
     *
     * Every control line is driven to its safe state BEFORE it is made an output. Doing it the
     * other way round -- pinMode() then digitalWrite() -- makes the pin an output at whatever
     * value the port register happened to hold, which for a fresh boot is LOW. For T1_CS_ENABLE
     * that is harmless (low = disabled), but for T1_SHIFT_PL a low means "load", and for
     * T1_SHIFT_MISO_OE a low means "drive MISO" -- so the wrong order connects the '165 chain to
     * a bus that something else may already be driving, for as long as it takes the next line
     * of code to run. */

    digitalWriteFast(T1_CS_ENABLE, LOW);        /* decoder OFF: no chip select asserted        */
    pinMode(T1_CS_ENABLE, OUTPUT);
    digitalWriteFast(T1_CS_A0, LOW);
    digitalWriteFast(T1_CS_A1, LOW);
    digitalWriteFast(T1_CS_A2, LOW);
    pinMode(T1_CS_A0, OUTPUT);
    pinMode(T1_CS_A1, OUTPUT);
    pinMode(T1_CS_A2, OUTPUT);
    mCsCurrent = 0;
    mCsEnabled = false;

    digitalWriteFast(T1_SHIFT_MISO_OE, HIGH);   /* '373 /OE high = Hi-Z: OFF the MISO bus      */
    pinMode(T1_SHIFT_MISO_OE, OUTPUT);

    digitalWriteFast(T1_SHIFT_PL, HIGH);        /* /PL high = not loading                      */
    pinMode(T1_SHIFT_PL, OUTPUT);

    digitalWriteFast(T1_SHIFT_RCLK, LOW);       /* latch idle low; data moves on the rising edge */
    pinMode(T1_SHIFT_RCLK, OUTPUT);

    digitalWriteFast(T1_LA_MARK, LOW);
    pinMode(T1_LA_MARK, OUTPUT);

    /* The shared expander interrupt is open-drain with an external 4.7k pull-up. INPUT, not
     * INPUT_PULLUP: the Teensy's internal pull-up is ~22k and paralleling it with the external
     * resistor changes the rise time, which changes when the falling edge is seen relative to
     * the capture. Keep the external resistor the only one, so the analyser measures the real
     * network and not the network plus whatever the MCU added. */
    pinMode(T1_FABRIC_IRQ, INPUT);

    /* THIS CALL MUST COME BEFORE begin(), AND OMITTING IT KILLS THE RADIO LINK.
     *
     * SPI1's default MISO is pin 1, which is Serial1's TX -- the tested ESP32-S3 link. Calling
     * SPI1.begin() without moving MISO first hands pin 1 to the SPI peripheral, and the radio
     * link goes silent. Not an error, not a warning: the Teensy simply stops transmitting and
     * the radio waits forever for a request that was never sent.
     *
     * setMISO() only takes effect before begin(); calling it afterwards is ignored. */
    T1_SPI_PORT.setMISO(T1_SPI_MISO);
    T1_SPI_PORT.begin();

    Wire.begin();
    Wire.setClock(FAB_I2C_HZ);

    /* Teensy 4's Wire.setClock() selects one of three fixed profiles rather than computing a
     * divisor: below 400000 gives 100 kHz, 400000 to 999999 gives 400 kHz, 1000000 and above
     * gives 1 MHz. FAB_I2C_HZ is 400000 exactly, which lands on the profile boundary on purpose.
     *
     * The frequency actually seen on the wire will be BELOW 400 kHz, and that is normal: the
     * peripheral adds latency proportional to the SCL rise time, so 250 ns of rise gives about
     * 364 kHz and 500 ns gives about 333 kHz. A capture showing 360 kHz is the pull-ups and the
     * bus capacitance being visible, not a fault -- but a capture showing 285 kHz means roughly
     * 1 us of rise time, which is a real warning that the bus is too slow and the pull-ups want
     * to be stronger before more devices go on it. */

    analogReadResolution(FAB_ADC_BITS);
    analogReadAveraging(FAB_ADC_AVERAGING);

    /* Park the shift chain in a known state. Until this runs, the 595 outputs hold whatever
     * their flip-flops powered up as -- genuinely random -- and if anything is connected to
     * them, it is being driven by noise. */
    mShiftChips = FAB_MAX_595 > 4 ? 4 : FAB_MAX_595;   /* minimal core populates 4; harmless
                                                        * to clock more than are fitted        */
    memset(mShiftOut, 0, sizeof(mShiftOut));
    mCtlByte = 0;                                       /* mux tree disabled (enable bit clear) */
    mShiftOut[FABCTL_CHIP_INDEX] = mCtlByte;
    shiftOut_(mShiftOut, mShiftChips);

    mReady = true;

    /* Find out what is actually on the bus, and initialise every expander that answered. An
     * expander that does not answer is left alone and its port simply reports NOT_PRESENT. */
    rescan();

    for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
        if (p->kind == BENCH_PORT_KIND_GPIO_EXP && portPresent(p->id)) {
            mcpBegin(p->id);
        }
    }

    return BENCH_ST_OK;
}

void BenchFabric::parkSafe()
{
    csDeselect();
    digitalWriteFast(T1_SHIFT_MISO_OE, HIGH);   /* off the MISO bus                            */
    digitalWriteFast(T1_SHIFT_PL, HIGH);

    if (mReady) {
        memset(mShiftOut, 0, sizeof(mShiftOut));
        mCtlByte = 0;                            /* mux enable bit clear = every leaf off      */
        shiftOut_(mShiftOut, mShiftChips);
    }
}

void BenchFabric::resetStats()
{
    memset(&mStats, 0, sizeof(mStats));
}

void BenchFabric::reportFault(uint8_t code, uint8_t port, uint16_t detail)
{
    mStats.faults++;
    if (mFaultFn) { mFaultFn(code, port, detail); }
}

TwoWire *BenchFabric::busFor(uint8_t bus)
{
    switch (bus) {
        case 0: return &Wire;
        case 1: return &Wire1;
        case 2: return &Wire2;
        default: return 0;
    }
}

/* ===========================================================================================
 * CHIP SELECT -- break-before-make, private by design
 * =========================================================================================== */

void BenchFabric::csDeselect()
{
    /* One pin, and every one of the eight outputs goes high. This is the only reason the design
     * dedicates a pin to E3: a 74HC138 has no "select nothing" address, so without a global
     * enable there is no way to reach a state where no module is selected. */
    digitalWriteFast(T1_CS_ENABLE, LOW);
    mCsEnabled = false;
}

void BenchFabric::csSelect(uint8_t slot)
{
    /* BREAK. The decoder is disabled first, unconditionally, even if it already was.
     *
     * The three address lines do not change simultaneously -- they are three separate register
     * writes, tens of nanoseconds apart. Changing the address while the decoder is enabled walks
     * the active-low output across whatever intermediate addresses those partial writes spell.
     * Going from slot 3 (011) to slot 4 (100) passes through 010, 110 or 000 depending on write
     * order, so modules on slots 2, 6 or 0 each see a short low pulse on their chip select.
     *
     * That pulse is nanoseconds long. An SPI flash chip needs less than that to decide a
     * transaction has begun. This is the single most likely way to corrupt a device that nobody
     * was even talking to, and it is entirely prevented by three lines of code. */
    digitalWriteFast(T1_CS_ENABLE, LOW);

    digitalWriteFast(T1_CS_A0, (slot & 0x01u) ? HIGH : LOW);
    digitalWriteFast(T1_CS_A1, (slot & 0x02u) ? HIGH : LOW);
    digitalWriteFast(T1_CS_A2, (slot & 0x04u) ? HIGH : LOW);

    /* MAKE, after the decoder has settled. 100 ns is a deliberate over-budget: 74HC138
     * datasheets tabulate propagation delay at 2 V, 4.5 V and 6 V with no 3.3 V column, and
     * typical 4.5 V figures are 20-25 ns. CMOS slows as the supply falls, so the true 3.3 V
     * number is somewhere above that and below this. It costs nothing to be generous here --
     * chip selects change a few thousand times a second, not a few million. */
    delayNanoseconds(FAB_CS_SETTLE_NS);

    digitalWriteFast(T1_CS_ENABLE, HIGH);
    mCsCurrent = slot;
    mCsEnabled = true;
    mStats.cs_changes++;
}

/* ===========================================================================================
 * SPI
 * =========================================================================================== */

uint8_t BenchFabric::spiTransfer(uint8_t portId, uint8_t mode, uint32_t hz,
                                 const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (!mReady) { return BENCH_ST_HW_FAULT; }

    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }
    if (p->kind != BENCH_PORT_KIND_SPI) { return BENCH_ST_WRONG_MODE; }
    if (p->flags & BENCH_PORTF_RESERVED) { return BENCH_ST_BAD_PORT; }
    if (mode > 3u) { return BENCH_ST_BAD_ARG; }
    if (len == 0u) { return BENCH_ST_BAD_ARG; }
    if (hz == 0u || hz > FAB_SPI_HZ) { hz = FAB_SPI_HZ; }

    uint8_t spiMode;
    switch (mode) {
        case 0:  spiMode = SPI_MODE0; break;
        case 1:  spiMode = SPI_MODE1; break;
        case 2:  spiMode = SPI_MODE2; break;
        default: spiMode = SPI_MODE3; break;
    }

    /* beginTransaction BEFORE asserting the select, so the clock is already parked at the idle
     * level this mode requires. Asserting CS while SCK is at the wrong level makes the module
     * see a clock edge the instant the mode is applied, which shifts every subsequent bit by
     * one -- producing data that decodes cleanly and is entirely wrong. */
    T1_SPI_PORT.beginTransaction(SPISettings(hz, MSBFIRST, spiMode));
    csSelect(p->slot);

    for (uint16_t i = 0; i < len; i++) {
        uint8_t out = tx ? tx[i] : 0x00u;
        uint8_t in  = T1_SPI_PORT.transfer(out);
        if (rx) { rx[i] = in; }
    }

    /* Deselect before ending the transaction, and on every path out of this function. There is
     * no early return between the select and here, which is why guarantee 1 holds. */
    csDeselect();
    T1_SPI_PORT.endTransaction();

    mStats.spi_transfers++;
    return BENCH_ST_OK;
}

/* ===========================================================================================
 * I2C
 * =========================================================================================== */

uint8_t BenchFabric::i2cTransfer(uint8_t bus, uint8_t addr7,
                                 const uint8_t *wr, uint8_t wrLen,
                                 uint8_t *rd, uint8_t rdLen)
{
    TwoWire *w = busFor(bus);
    if (!w) { return BENCH_ST_BAD_ARG; }
    if (addr7 > 0x7Fu) { return BENCH_ST_BAD_ARG; }

    mStats.i2c_transactions++;

    if (wrLen > 0u) {
        w->beginTransmission(addr7);
        for (uint8_t i = 0; i < wrLen; i++) { w->write(wr[i]); }

        /* endTransmission(false) suppresses the STOP, so the following requestFrom issues a
         * REPEATED START. That matters: with a STOP in between, the bus is released between
         * writing the register pointer and reading the data. This fabric has exactly one
         * master so nothing else can take the bus -- but a device that is reset in that window
         * comes back with its pointer at register 0 and returns the wrong register's contents,
         * cleanly, with no error. A repeated start makes the pair indivisible. */
        uint8_t err = w->endTransmission(rdLen > 0u ? false : true);
        if (err != 0u) {
            mStats.i2c_nacks++;
            reportFault(BENCH_FAULT_I2C_NACK, BENCH_PORT_INVALID,
                        (uint16_t)((addr7 << 8) | err));
            return (err == 2u || err == 3u) ? BENCH_ST_NOT_PRESENT : BENCH_ST_BUS_ERROR;
        }
    }

    if (rdLen > 0u) {
        uint8_t got = w->requestFrom(addr7, rdLen);
        if (got != rdLen) {
            mStats.i2c_nacks++;
            reportFault(BENCH_FAULT_I2C_NACK, BENCH_PORT_INVALID, (uint16_t)(addr7 << 8));
            return BENCH_ST_NOT_PRESENT;
        }
        for (uint8_t i = 0; i < rdLen; i++) { rd[i] = (uint8_t)w->read(); }
    }

    return BENCH_ST_OK;
}

uint8_t BenchFabric::i2cScan(uint8_t bus, uint8_t *found, uint8_t maxFound)
{
    TwoWire *w = busFor(bus);
    if (!w || !found) { return 0; }

    uint8_t n = 0;
    /* 0x00-0x07 and 0x78-0x7F are reserved by the I2C specification. General call, CBUS, 10-bit
     * addressing and device-ID all live in those ranges, and probing them can put a compliant
     * device into a state it does not leave on its own. A scanner that walks 0x00-0x7F is a
     * scanner that occasionally bricks a bus until power cycle. */
    for (uint8_t a = 0x08u; a <= 0x77u && n < maxFound; a++) {
        w->beginTransmission(a);
        if (w->endTransmission(true) == 0u) {
            found[n++] = a;
        }
    }
    return n;
}

uint8_t BenchFabric::rescan()
{
    uint8_t found[112];
    uint8_t n = i2cScan(0, found, sizeof(found));

    mMcpPresent = 0;

    for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        BenchPortDesc *p = (BenchPortDesc *)&BENCH_PORT_TABLE[i];   /* runtime fields only     */

        if (p->kind != BENCH_PORT_KIND_GPIO_EXP && p->kind != BENCH_PORT_KIND_I2C) { continue; }

        bool was = (p->flags & BENCH_PORTF_PRESENT) != 0u;
        bool now = false;
        uint8_t addr = 0;

        if (p->kind == BENCH_PORT_KIND_GPIO_EXP) {
            for (uint8_t k = 0; k < n; k++) {
                if (found[k] == p->slot) { now = true; addr = p->slot; break; }
            }
            if (now) { mMcpPresent |= (uint8_t)(1u << (p->slot - FAB_MCP_ADDR_BASE)); }
        } else {
            /* A generic sensor slot has no fixed address. It is "present" if anything answered
             * that is not already claimed by an expander bank. `detected` records what actually
             * replied, so the address map in the repo can be generated from the hardware rather
             * than maintained by hand and allowed to drift. */
            for (uint8_t k = 0; k < n; k++) {
                if (found[k] >= FAB_MCP_ADDR_BASE && found[k] <= FAB_MCP_ADDR_MAX) { continue; }
                now = true; addr = found[k];
                break;
            }
        }

        if (now) { p->flags |= BENCH_PORTF_PRESENT; p->detected = addr; }
        else     { p->flags &= (uint16_t)~BENCH_PORTF_PRESENT; p->detected = 0; }

        /* Hot-plug becomes an event with a subject, rather than a mystery discovered later. */
        if (was && !now)  { reportFault(BENCH_FAULT_DEVICE_LOST, p->id, 0); }
        if (!was && now)  { reportFault(BENCH_FAULT_DEVICE_APPEARED, p->id, addr); }
    }

    return n;
}

bool BenchFabric::portPresent(uint8_t portId) const
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return false; }
    /* SPI, shift and analog ports have no way to acknowledge, so presence cannot be probed and
     * is assumed. That is stated rather than hidden: a write into an empty 74HC595 socket
     * genuinely does return success, and no amount of firmware can change that. */
    if (p->kind == BENCH_PORT_KIND_GPIO_EXP || p->kind == BENCH_PORT_KIND_I2C) {
        return (p->flags & BENCH_PORTF_PRESENT) != 0u;
    }
    return true;
}

uint8_t BenchFabric::i2cRecover(uint8_t bus)
{
    /* A device that was reset in the middle of sending a byte can be left holding SDA low,
     * waiting for the clocks that would finish the transfer. The master sees a permanently busy
     * bus and every transaction fails from then on -- the classic "it worked until I reset the
     * other board" fault. Nine clock pulses walk that device off the end of its byte; the STOP
     * then resynchronises it. */
    uint8_t sda, scl;
    switch (bus) {
        case 0: sda = T1_I2C_SDA;  scl = T1_I2C_SCL;  break;
        case 1: sda = T1_I2C1_SDA; scl = T1_I2C1_SCL; break;
        case 2: sda = T1_I2C2_SDA; scl = T1_I2C2_SCL; break;
        default: return BENCH_ST_BAD_ARG;
    }

    TwoWire *w = busFor(bus);
    if (w) { w->end(); }

    pinMode(sda, INPUT);            /* release SDA; the external pull-up decides its level     */
    pinMode(scl, OUTPUT);
    digitalWriteFast(scl, HIGH);

    for (uint8_t i = 0; i < 9u; i++) {
        digitalWriteFast(scl, LOW);
        delayMicroseconds(5);
        digitalWriteFast(scl, HIGH);
        delayMicroseconds(5);
    }

    /* Manual STOP: SDA low->high while SCL is high. */
    pinMode(sda, OUTPUT);
    digitalWriteFast(sda, LOW);
    delayMicroseconds(5);
    digitalWriteFast(scl, HIGH);
    delayMicroseconds(5);
    pinMode(sda, INPUT);
    delayMicroseconds(5);

    bool freed = (digitalReadFast(sda) == HIGH);

    if (w) { w->begin(); w->setClock(FAB_I2C_HZ); }

    mStats.i2c_recoveries++;
    if (!freed) {
        reportFault(BENCH_FAULT_I2C_WEDGED, BENCH_PORT_INVALID, bus);
        return BENCH_ST_BUS_ERROR;
    }
    return BENCH_ST_OK;
}

/* ===========================================================================================
 * MCP23017
 * =========================================================================================== */

uint8_t BenchFabric::mcpWrite8(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2cTransfer(bus, addr, buf, 2, 0, 0);
}

uint8_t BenchFabric::mcpWrite16(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t val)
{
    /* Relies on BANK=0 register interleaving and SEQOP=0 auto-increment: writing at an A
     * register and continuing lands the second byte in its B partner. */
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFFu), (uint8_t)(val >> 8) };
    return i2cTransfer(bus, addr, buf, 3, 0, 0);
}

uint8_t BenchFabric::mcpRead8(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t *val)
{
    return i2cTransfer(bus, addr, &reg, 1, val, 1);
}

uint8_t BenchFabric::mcpRead16(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    uint8_t st = i2cTransfer(bus, addr, &reg, 1, buf, 2);
    if (st == BENCH_ST_OK && val) {
        *val = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    }
    return st;
}

uint8_t BenchFabric::mcpBegin(uint8_t portId)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p || p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_BAD_PORT; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    uint8_t idx = (uint8_t)(p->slot - FAB_MCP_ADDR_BASE);
    uint8_t st;

    /* IOCON first. Every later access in this driver assumes MIRROR, ODR and BANK=0, so setting
     * it before anything else means no window exists in which those assumptions are false.
     *
     * Written to BOTH 0x0A and 0x0B. It is one physical register visible at two addresses, so
     * one write is enough -- but only if the chip is already in BANK=0. If it somehow is not
     * (a previous sketch set BANK=1 and the chip has not been power-cycled), then 0x0A and 0x0B
     * are different registers and only one write lands where it is needed. Writing both makes
     * this recovery deterministic instead of requiring a power cycle nobody will think of. */
    st = mcpWrite8(p->bus, p->slot, MCP_IOCONA, MCP_IOCON_FABRIC);
    if (st != BENCH_ST_OK) { return st; }
    st = mcpWrite8(p->bus, p->slot, MCP_IOCONB, MCP_IOCON_FABRIC);
    if (st != BENCH_ST_OK) { return st; }

    /* All pins input, no pull-ups, no interrupts, output latches cleared. An expander whose
     * wiring is unknown must not drive anything: an output fighting an external driver is the
     * one configuration that can damage hardware before anyone notices. */
    mcpWrite16(p->bus, p->slot, MCP_IODIRA,   0xFFFFu);
    mcpWrite16(p->bus, p->slot, MCP_GPPUA,    0x0000u);
    mcpWrite16(p->bus, p->slot, MCP_GPINTENA, 0x0000u);
    mcpWrite16(p->bus, p->slot, MCP_INTCONA,  0x0000u);   /* compare against previous value    */
    mcpWrite16(p->bus, p->slot, MCP_OLATA,    0x0000u);

    mMcpIodir[idx]  = 0xFFFFu;
    mMcpPullup[idx] = 0x0000u;
    mMcpOlat[idx]   = 0x0000u;

    /* Clear any latched interrupt left over from before this reset by reading both capture
     * registers. Starting with an interrupt already pending means the shared line is already
     * low and the first falling edge never happens. */
    uint16_t scratch;
    mcpRead16(p->bus, p->slot, MCP_INTCAPA, &scratch);
    mcpRead16(p->bus, p->slot, MCP_GPIOA,   &scratch);

    return BENCH_ST_OK;
}

uint8_t BenchFabric::pinMode_(uint8_t portId, uint8_t index, uint8_t mode)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }
    if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_WRONG_MODE; }
    if (index >= p->width) { return BENCH_ST_BAD_INDEX; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    uint8_t idx = (uint8_t)(p->slot - FAB_MCP_ADDR_BASE);
    uint16_t bit = (uint16_t)(1u << (p->first + index));

    switch (mode) {
        case BENCH_PINMODE_INPUT:
            mMcpIodir[idx]  |= bit;
            mMcpPullup[idx] &= (uint16_t)~bit;
            break;
        case BENCH_PINMODE_INPUT_PULLUP:
            mMcpIodir[idx]  |= bit;
            mMcpPullup[idx] |= bit;
            /* The internal pull-up is 40-115 uA, roughly 100k equivalent. That is fine for a
             * switch on the same board and far too weak for anything off-board: leakage and
             * cable capacitance turn it into an antenna with a bias. Off-board inputs get an
             * external 10k, and this mode is not a substitute for one. */
            break;
        case BENCH_PINMODE_OUTPUT:
        case BENCH_PINMODE_OUTPUT_SINK:
            mMcpIodir[idx]  &= (uint16_t)~bit;
            mMcpPullup[idx] &= (uint16_t)~bit;
            break;
        default:
            return BENCH_ST_BAD_ARG;
    }

    uint8_t st = mcpWrite16(p->bus, p->slot, MCP_GPPUA, mMcpPullup[idx]);
    if (st != BENCH_ST_OK) { return st; }
    return mcpWrite16(p->bus, p->slot, MCP_IODIRA, mMcpIodir[idx]);
}

uint8_t BenchFabric::setShiftBit(uint8_t chip, uint8_t bit, uint8_t value)
{
    if (chip >= mShiftChips || bit > 7u) { return BENCH_ST_BAD_ARG; }

    /* Read-modify-write against the SHADOW, never against the hardware. A 74HC595 chain cannot
     * be read back at all, so without a shadow copy there is no way to change one bit without
     * destroying the other seven. */
    uint8_t v = mShiftOut[chip];
    if (value) { v |= (uint8_t)(1u << bit); } else { v &= (uint8_t)~(1u << bit); }
    if (v == mShiftOut[chip]) { return BENCH_ST_OK; }   /* nothing to do; skip the bus traffic */

    mShiftOut[chip] = v;
    return shiftOut_(mShiftOut, mShiftChips);
}

/* ===========================================================================================
 * SPI PORT AUXILIARIES -- addressed as pin 0 and pin 1 of the port
 * ===========================================================================================
 * Rather than inventing two more commands, a SPI port answers the ordinary pin calls:
 *
 *      index 0  =  CTL, an OUTPUT, on 74HC595 chip 1     (reset, chip enable, mode strap)
 *      index 1  =  IRQ, an INPUT, on MCP_A               (interrupt, data-ready, busy)
 *
 * So `pinWrite("P3", 0, 1)` releases P3's reset and `pinRead("P3", 1)` reads its interrupt line,
 * over the existing wire commands, from Python, with no new firmware.
 *
 * The split is not arbitrary: a '595 output changes in about 2 us against an expander's 120 us,
 * and an expander pin is the only one that can raise a flag without being polled. Direction
 * decides which silicon carries the signal.
 */
uint8_t BenchFabric::portCtlWrite(uint8_t portId, uint8_t value)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p || p->kind != BENCH_PORT_KIND_SPI) { return BENCH_ST_BAD_PORT; }
    if (p->ctl_bit > 7u) { return BENCH_ST_UNSUPPORTED; }
    return setShiftBit(1u /* 595 chip 1 == the OA bank */, p->ctl_bit, value);
}

uint8_t BenchFabric::portIrqRead(uint8_t portId, uint8_t *value)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p || p->kind != BENCH_PORT_KIND_SPI) { return BENCH_ST_BAD_PORT; }
    if (p->irq_bit > 15u) { return BENCH_ST_UNSUPPORTED; }
    if (!portPresent(PORT_MCP_A)) { return BENCH_ST_NOT_PRESENT; }

    uint16_t all = 0;
    uint8_t st = portRead16(PORT_MCP_A, &all);
    if (st != BENCH_ST_OK) { return st; }
    if (value) { *value = (uint8_t)((all >> p->irq_bit) & 1u); }
    return BENCH_ST_OK;
}

uint8_t BenchFabric::pinWrite(uint8_t portId, uint8_t index, uint8_t value)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }

    if (p->kind == BENCH_PORT_KIND_SPI) {
        if (index == 0u) { return portCtlWrite(portId, value); }
        return BENCH_ST_WRONG_MODE;   /* index 1 is the IRQ, and it is an input */
    }

    if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_WRONG_MODE; }
    if (index >= p->width) { return BENCH_ST_BAD_INDEX; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    uint8_t idx = (uint8_t)(p->slot - FAB_MCP_ADDR_BASE);
    uint16_t bit = (uint16_t)(1u << (p->first + index));

    if (mMcpIodir[idx] & bit) { return BENCH_ST_WRONG_MODE; }   /* still an input              */

    /* The shadow is what makes this a single write instead of a read-modify-write. Reading GPIO
     * to modify one bit would read back INPUT pins too, and writing that value to OLAT changes
     * nothing for inputs but does make the output register depend on the state of unrelated
     * wires -- so a noisy input would appear to move an output. Shadowing OLAT avoids that
     * entirely, and halves the bus traffic. */
    if (value) { mMcpOlat[idx] |= bit; } else { mMcpOlat[idx] &= (uint16_t)~bit; }

    return mcpWrite16(p->bus, p->slot, MCP_OLATA, mMcpOlat[idx]);
}

uint8_t BenchFabric::pinRead(uint8_t portId, uint8_t index, uint8_t *value)
{
    const BenchPortDesc *pd = bench_port_find(portId);
    if (!pd) { return BENCH_ST_BAD_PORT; }

    if (pd->kind == BENCH_PORT_KIND_SPI) {
        if (index == 1u) { return portIrqRead(portId, value); }
        if (index == 0u) {
            /* Read the CTL line back from the shadow. The hardware genuinely cannot be read --
             * a 74HC595 has no readback path -- so this reports what we last commanded, not
             * what the pin is doing. That distinction is why it is stated here rather than
             * quietly presented as a measurement. */
            if (pd->ctl_bit > 7u) { return BENCH_ST_UNSUPPORTED; }
            if (value) { *value = (uint8_t)((mShiftOut[1] >> pd->ctl_bit) & 1u); }
            return BENCH_ST_OK;
        }
        return BENCH_ST_BAD_INDEX;
    }

    uint16_t all;
    uint8_t st = portRead16(portId, &all);
    if (st != BENCH_ST_OK) { return st; }

    const BenchPortDesc *p = pd;
    if (index >= p->width) { return BENCH_ST_BAD_INDEX; }

    if (value) { *value = (uint8_t)((all >> (p->first + index)) & 1u); }
    return BENCH_ST_OK;
}

uint8_t BenchFabric::portRead16(uint8_t portId, uint16_t *value)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }
    if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_WRONG_MODE; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    /* One transaction covering both ports. Reading GPIOA and GPIOB as two transactions samples
     * two different instants roughly 60 us apart at 400 kHz. For a 16-bit parallel bus, a keypad
     * matrix or a quadrature encoder, that produces a combined value that never existed on the
     * wires -- and it is perfectly stable, so it looks like data rather than like a bug. */
    return mcpRead16(p->bus, p->slot, MCP_GPIOA, value);
}

uint8_t BenchFabric::portWrite16(uint8_t portId, uint16_t value)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }
    if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_WRONG_MODE; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    uint8_t idx = (uint8_t)(p->slot - FAB_MCP_ADDR_BASE);
    mMcpOlat[idx] = value;
    return mcpWrite16(p->bus, p->slot, MCP_OLATA, value);
}

uint8_t BenchFabric::irqConfig(uint8_t portId, uint16_t mask)
{
    const BenchPortDesc *p = bench_port_find(portId);
    if (!p) { return BENCH_ST_BAD_PORT; }
    if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { return BENCH_ST_WRONG_MODE; }
    if (!portPresent(portId)) { return BENCH_ST_NOT_PRESENT; }

    /* INTCON = 0 means "interrupt on any change", comparing against the previous pin value
     * rather than against DEFVAL. Compare-against-DEFVAL is a level detector: it re-asserts
     * continuously while the pin sits at the interesting level, which on a shared open-drain
     * line holds it low forever and stops every other expander from being heard. Change
     * detection is the only mode that composes on a wire-OR. */
    uint8_t st = mcpWrite16(p->bus, p->slot, MCP_INTCONA, 0x0000u);
    if (st != BENCH_ST_OK) { return st; }

    return mcpWrite16(p->bus, p->slot, MCP_GPINTENA, mask);
}

uint8_t BenchFabric::serviceIrq(void (*onEvent)(uint8_t portId, uint16_t intf,
                                                uint16_t intcap, uint16_t gpio))
{
    uint8_t served = 0;

    for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
        if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { continue; }
        if (!portPresent(p->id)) { continue; }

        uint16_t intf = 0;
        if (mcpRead16(p->bus, p->slot, MCP_INTFA, &intf) != BENCH_ST_OK) { continue; }

        /* INTCAP FIRST, THEN GPIO, AND ALWAYS BOTH.
         *
         * INTCAP holds the pin values AT THE MOMENT the interrupt fired. GPIO holds them now.
         * They differ whenever the input bounced or changed again while the frame was being
         * built, and only INTCAP answers "what happened" -- reading GPIO alone turns a
         * 2 ms button press into no event at all if the service came 3 ms later.
         *
         * Reading them is also what CLEARS the interrupt. With MIRROR=1 the condition does not
         * clear until both GPIOA and GPIOB have been read, and this driver reads all sixteen
         * bits in one transaction precisely so that "both" cannot be got wrong. Servicing only
         * the port that mattered leaves INT asserted, no further falling edge ever arrives, and
         * interrupts stop system-wide with no error reported anywhere. */
        uint16_t intcap = 0, gpio = 0;
        mcpRead16(p->bus, p->slot, MCP_INTCAPA, &intcap);
        mcpRead16(p->bus, p->slot, MCP_GPIOA,   &gpio);

        if (intf != 0u) {
            served++;
            mStats.irq_events++;
            if (onEvent) { onEvent(p->id, intf, intcap, gpio); }
        }
    }

    if (served == 0u) {
        /* The line was low and no expander claimed it. Either something else is pulling it, or
         * the pull-up is missing, or a chip is in a state this driver did not put it in. It is
         * counted rather than ignored, because a rising spurious count is the earliest visible
         * sign of a marginal pull-up or a noisy ground. */
        mStats.irq_spurious++;
    }

    /* Still low after servicing everything? Then the clear did not work, which is the exact
     * silent-wedge failure this design is built to avoid. Say so loudly. */
    if (digitalReadFast(T1_FABRIC_IRQ) == LOW) {
        mStats.irq_storms++;
        reportFault(BENCH_FAULT_IRQ_STORM, BENCH_PORT_INVALID, served);
    }

    return served;
}

/* ===========================================================================================
 * SHIFT REGISTERS
 * =========================================================================================== */

uint8_t BenchFabric::shiftOut_(const uint8_t *data, uint8_t chips)
{
    if (!data || chips == 0u || chips > FAB_MAX_595) { return BENCH_ST_BAD_ARG; }

    /* The chain is not addressed by chip select -- every 74HC595 sees every clock. The decoder
     * is therefore parked before shifting, so that no SPI module is selected while the chain's
     * traffic goes past it. Without this, every shift-register update is also a burst of
     * meaningless SPI traffic delivered to whichever module happened to be selected. */
    csDeselect();

    T1_SPI_PORT.beginTransaction(SPISettings(FAB_SPI_HZ, MSBFIRST, SPI_MODE0));

    /* REVERSE ORDER, and this is the thing everyone gets wrong once.
     *
     * MOSI feeds chip 0's SER; chip 0's QH' feeds chip 1's SER, and so on. So a byte clocked in
     * first is pushed all the way along the chain by the bytes that follow it, and comes to rest
     * in the LAST chip. To make data[0] land in chip 0 -- the one nearest the Teensy, the one
     * the silk calls chip 0 -- data[0] must be transmitted LAST.
     *
     * Getting this backwards does not fail. It works perfectly, with the chips in the wrong
     * order, and presents as "the wiring is mirrored". */
    for (int16_t i = (int16_t)chips - 1; i >= 0; i--) {
        T1_SPI_PORT.transfer(data[i]);
    }

    T1_SPI_PORT.endTransaction();

    /* RCLK rising edge transfers the shift register into the output latch. Every output in the
     * chain changes on this one edge. Until it happens, the outputs still hold the previous
     * value -- which is exactly why SRCLK and RCLK must never be tied together: doing so puts
     * every intermediate shift state onto the pins, and on a relay or MOSFET bank that means
     * briefly energising loads that were never commanded. */
    digitalWriteFast(T1_SHIFT_RCLK, LOW);
    delayNanoseconds(50);
    digitalWriteFast(T1_SHIFT_RCLK, HIGH);
    delayNanoseconds(50);
    digitalWriteFast(T1_SHIFT_RCLK, LOW);

    if (data != mShiftOut) {
        memcpy(mShiftOut, data, chips);
    }
    mShiftChips = chips;
    mStats.shift_out_ops++;
    return BENCH_ST_OK;
}

uint8_t BenchFabric::shiftIn_(uint8_t *data, uint8_t chips)
{
    if (!data || chips == 0u || chips > FAB_MAX_165) { return BENCH_ST_BAD_ARG; }

    csDeselect();

    /* /PL LOW loads every input pin in the chain into its flip-flop SIMULTANEOUSLY. This is an
     * asynchronous parallel load, not a clocked one, which is what makes it a true snapshot: all
     * 8N inputs are captured at one instant rather than sampled one byte at a time as the shift
     * proceeds. For anything that changes faster than the shift takes -- a bus, a set of
     * quadrature signals -- that distinction is the difference between data and noise. */
    digitalWriteFast(T1_SHIFT_PL, LOW);
    delayNanoseconds(100);
    digitalWriteFast(T1_SHIFT_PL, HIGH);
    delayNanoseconds(50);

    /* Only now connect the chain to MISO, and only for as long as the read takes. See guarantee
     * 3: the '165's QH is a permanently-driving push-pull output with no chip select, so any
     * time it is on the bus it is fighting whatever else might drive it. The 74HC373 held
     * transparent is the tri-state gate the '165 does not have. */
    digitalWriteFast(T1_SHIFT_MISO_OE, LOW);

    T1_SPI_PORT.beginTransaction(SPISettings(FAB_SPI_HZ, MSBFIRST, SPI_MODE0));

    /* NO reversal on the way in, and the asymmetry with shiftOut_ is real rather than an
     * oversight. Chip 0 is the one whose QH drives the bus, so its byte arrives first; the
     * chips behind it feed forward into it. Output is push-then-settle, input is pull-then-
     * arrive, and the orders genuinely differ.
     *
     * Within each byte, the H input (pin 6) exits first and therefore lands in bit 7, with the
     * A input (pin 11) in bit 0. A bank wired A..H in ascending physical order reads back
     * mirrored unless the code accounts for it -- and a mirrored byte looks exactly like a
     * wiring mistake, which is how an afternoon disappears. */
    for (uint8_t i = 0; i < chips; i++) {
        data[i] = T1_SPI_PORT.transfer(0x00u);
    }

    T1_SPI_PORT.endTransaction();

    digitalWriteFast(T1_SHIFT_MISO_OE, HIGH);   /* off the bus again, immediately              */

    mStats.shift_in_ops++;
    return BENCH_ST_OK;
}

uint8_t BenchFabric::setControlByte(uint8_t value)
{
    mCtlByte = value;
    mShiftOut[FABCTL_CHIP_INDEX] = value;
    return shiftOut_(mShiftOut, mShiftChips);
}

/* ===========================================================================================
 * ANALOG -- the 74HC4051 tree
 * =========================================================================================== */

uint8_t BenchFabric::adcParkTree()
{
    /* Clearing the enable bit disables every leaf, disconnecting all 64 sensors from the ADC
     * pin. This state has to exist: without it there is no way to measure the tree's own
     * leakage, no way to tell a dead sensor from a dead mux, and no safe park for the input. */
    return setControlByte((uint8_t)(mCtlByte & (uint8_t)~FABCTL_MUX_ENABLE_BIT));
}

uint8_t BenchFabric::adcRead(uint8_t leaf, uint8_t channel, uint8_t samples, uint16_t *out)
{
    if (leaf >= FAB_MAX_MUX_LEAVES || channel > 7u) { return BENCH_ST_BAD_ARG; }
    if (samples == 0u) { samples = 1u; }
    if (!out) { return BENCH_ST_BAD_ARG; }

    uint8_t ctl = (uint8_t)(((channel << FABCTL_MUX_SEL_SHIFT)  & FABCTL_MUX_SEL_MASK) |
                            ((leaf    << FABCTL_MUX_LEAF_SHIFT) & FABCTL_MUX_LEAF_MASK));

    /* BREAK BEFORE MAKE, for the analog tree as well as the chip selects.
     *
     * The '4051's own break-before-make guarantee is about channels WITHIN one package. It says
     * nothing about two different packages, and that is exactly the overlap here: while the
     * leaf-decoder's address settles, two of its outputs can be LOW at the same time for a few
     * nanoseconds, connecting two analog sources to the same node through roughly 500 ohm.
     *
     * Latching the whole control byte on one RCLK edge already makes the address change
     * simultaneous, which is better than three GPIO writes -- but the decoder still has to
     * propagate, and during that propagation its outputs are not guaranteed exclusive. So the
     * enable bit is cleared first, in its own latched write, exactly as the chip-select path
     * does it. Two SPI transactions instead of one, for about a microsecond. */
    uint8_t st = setControlByte((uint8_t)(ctl & ~FABCTL_MUX_ENABLE_BIT));
    if (st != BENCH_ST_OK) { return st; }
    st = setControlByte((uint8_t)(ctl | FABCTL_MUX_ENABLE_BIT));
    if (st != BENCH_ST_OK) { return st; }

    /* WHAT SETS THIS WAIT, and it is not the ADC.
     *
     * Bussing eight Z pins puts roughly 200 pF on the node (8 x ~25 pF Ccom). The RT1062's
     * sample capacitor is about 2 pF -- a hundred times smaller -- so the BUS dominates
     * settling entirely. Teensy's stock 12-bit configuration gives a 2.4 us sample aperture,
     * which is already 18x more than the converter itself needs.
     *
     * With Ron of 300 ohm (the 2.0 V bound; see below) and a low-impedance source, tau is
     * about 60 ns and settling to half an LSB takes well under a microsecond. With a 10 kohm
     * sensor it is tau = 2.06 us and roughly 18.5 us. FAB_MUX_SETTLE_US is set for the 10 kohm
     * case, because that is the source impedance the design permits.
     *
     * NO 3.3 V NUMBER EXISTS. Neither Nexperia nor TI characterises the 74HC4051 at 3.3 V at
     * all -- Ron, ton and toff are specified only at 2.0 / 4.5 / 6.0 / 9.0 V. Every "70 ohm at
     * 3.3 V" figure in circulation is somebody's measurement of their own parts, usually
     * derived from the 4.5 V column and three to four times optimistic. This code designs
     * against the 2.0 V bound: Ron 300 ohm, ton 350 ns. Measure your own parts and tighten it. */
    delayMicroseconds(FAB_MUX_SETTLE_US);

#if FAB_MUX_DISCARD_FIRST
    /* Thrown away, never averaged, no option to keep it.
     *
     * The SAR's sample capacitor arrives still holding the PREVIOUS channel's voltage and
     * charge-shares with the bus. The error is about 0.79% of the step -- up to roughly 32 LSB
     * at 12 bits -- and, critically, it is PROPORTIONAL TO THE PREVIOUS CHANNEL. Keep it and
     * every reading becomes a small function of its neighbour: stable, plausible, repeatable and
     * wrong, which is the kind of wrong number that recruits you as its advocate.
     *
     * One discarded conversion removes it completely, because the second conversion samples a
     * node the first one already charged. */
    (void)analogRead(T1_ADC_MUX_IN);
    mStats.adc_discarded++;
#endif

    uint32_t acc = 0;
    for (uint8_t i = 0; i < samples; i++) {
        acc += (uint32_t)analogRead(T1_ADC_MUX_IN);
        mStats.adc_conversions++;
    }

    *out = (uint16_t)(acc / samples);
    return BENCH_ST_OK;
}

uint8_t BenchFabric::adcSweep(uint8_t first, uint8_t count, uint8_t samples, uint16_t *out)
{
    if (!out) { return BENCH_ST_BAD_ARG; }
    if ((uint16_t)first + count > (uint16_t)(FAB_MAX_MUX_LEAVES * 8u)) { return BENCH_ST_BAD_ARG; }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t abs_ch = (uint8_t)(first + i);
        uint8_t st = adcRead((uint8_t)(abs_ch >> 3), (uint8_t)(abs_ch & 0x07u), samples, &out[i]);
        if (st != BENCH_ST_OK) { return st; }
    }
    return BENCH_ST_OK;
}

/* ===========================================================================================
 * INSTRUMENTATION
 * =========================================================================================== */

void BenchFabric::mark(uint8_t pulses)
{
    if (pulses == 0u) { pulses = 1u; }
    for (uint8_t i = 0; i < pulses; i++) {
        digitalWriteFast(T1_LA_MARK, HIGH);
        delayNanoseconds(500);
        digitalWriteFast(T1_LA_MARK, LOW);
        delayNanoseconds(500);
    }
    /* 500 ns per level is comfortably above one sample period on a 24 MHz analyser sharing 8
     * channels (~125 ns), so every pulse is captured with several samples per level and the
     * count is unambiguous. Shorter pulses alias and turn a 3-pulse marker into a 2-pulse one. */
}

/* ===========================================================================================
 * SELF-TEST -- the L0..L7 bring-up ladder, in firmware
 * ===========================================================================================
 * Same tests as the manual procedure, so a PASS here and a PASS at the bench mean the same
 * thing. Hardware that is not fitted reports SKIPPED, never PASS: a self-test that passes by
 * not looking is worse than no self-test at all, because it is trusted.
 */

#define PUSH_RESULT(id, res, detail)                       \
    do {                                                   \
        if (n + 4u <= maxQuads * 4u) {                     \
            results[n++] = (id);                           \
            results[n++] = (res);                          \
            results[n++] = (uint8_t)((detail) >> 8);       \
            results[n++] = (uint8_t)((detail) & 0xFFu);    \
        }                                                  \
    } while (0)

uint8_t BenchFabric::selfTest(uint8_t level, uint8_t *results, uint8_t maxQuads)
{
    uint8_t n = 0;
    if (!results || maxQuads == 0u) { return 0; }

    /* ---- L2: both I2C lines idle high --------------------------------------------------- */
    /* If a pull-up is missing or a device is holding the bus, every later I2C test fails in a
     * way that looks like a device fault. Checking the wires first means the diagnosis is "no
     * pull-up" instead of "the expander is dead". */
    {
        Wire.end();
        pinMode(T1_I2C_SDA, INPUT);
        pinMode(T1_I2C_SCL, INPUT);
        delayMicroseconds(50);
        bool sda = digitalReadFast(T1_I2C_SDA) == HIGH;
        bool scl = digitalReadFast(T1_I2C_SCL) == HIGH;
        Wire.begin();
        Wire.setClock(FAB_I2C_HZ);
        uint16_t detail = (uint16_t)((sda ? 2u : 0u) | (scl ? 1u : 0u));
        PUSH_RESULT(BENCH_TEST_I2C_IDLE_HIGH,
                    (sda && scl) ? BENCH_TESTRESULT_PASS : BENCH_TESTRESULT_FAIL, detail);
    }

    /* ---- L3: every expected device answers, and nothing unexpected does ------------------ */
    {
        uint8_t found[112];
        uint8_t got = i2cScan(0, found, sizeof(found));
        uint8_t expected = 0, present = 0;
        for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
            const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
            if (p->kind != BENCH_PORT_KIND_GPIO_EXP) { continue; }
            expected++;
            for (uint8_t k = 0; k < got; k++) {
                if (found[k] == p->slot) { present++; break; }
            }
        }
        PUSH_RESULT(BENCH_TEST_I2C_SCAN,
                    (present == expected && expected > 0u) ? BENCH_TESTRESULT_PASS
                                                           : BENCH_TESTRESULT_FAIL,
                    (uint16_t)((present << 8) | got));
    }

    /* ---- MCP write/read-back ------------------------------------------------------------- */
    /* Level 2 only: it drives outputs. With modules attached, an output driven against an
     * external driver is the one configuration that can damage hardware. */
    if (level >= 2u) {
        uint8_t tested = 0, passed = 0;
        for (uint8_t i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
            const BenchPortDesc *p = &BENCH_PORT_TABLE[i];
            if (p->kind != BENCH_PORT_KIND_GPIO_EXP || !portPresent(p->id)) { continue; }
            tested++;
            uint8_t idx = (uint8_t)(p->slot - FAB_MCP_ADDR_BASE);
            uint16_t savedDir = mMcpIodir[idx], savedLat = mMcpOlat[idx];

            /* 0xA55A rather than 0xFFFF or 0x0000: an alternating pattern catches a stuck bit,
             * a shorted adjacent pair and a byte-swap, none of which an all-ones pattern can
             * distinguish from success. */
            mcpWrite16(p->bus, p->slot, MCP_IODIRA, 0x0000u);
            mcpWrite16(p->bus, p->slot, MCP_OLATA,  0xA55Au);
            uint16_t rb = 0;
            mcpRead16(p->bus, p->slot, MCP_GPIOA, &rb);
            if (rb == 0xA55Au) { passed++; }

            mcpWrite16(p->bus, p->slot, MCP_OLATA,  savedLat);
            mcpWrite16(p->bus, p->slot, MCP_IODIRA, savedDir);
        }
        PUSH_RESULT(BENCH_TEST_MCP_READBACK,
                    (tested == 0u) ? BENCH_TESTRESULT_SKIPPED
                                   : ((passed == tested) ? BENCH_TESTRESULT_PASS
                                                         : BENCH_TESTRESULT_FAIL),
                    (uint16_t)((passed << 8) | tested));
    } else {
        PUSH_RESULT(BENCH_TEST_MCP_READBACK, BENCH_TESTRESULT_SKIPPED, 0);
    }

    /* ---- L4: SPI loopback, needs a MOSI->MISO jumper ------------------------------------- */
    if (level >= 1u) {
        uint8_t pattern[4] = { 0x00u, 0xFFu, 0xA5u, 0x5Au };
        uint8_t ok = 1;
        csDeselect();
        digitalWriteFast(T1_SHIFT_MISO_OE, HIGH);   /* keep the '165 chain off the bus         */
        T1_SPI_PORT.beginTransaction(SPISettings(FAB_SPI_HZ_CAPTURE, MSBFIRST, SPI_MODE0));
        for (uint8_t i = 0; i < 4u; i++) {
            if (T1_SPI_PORT.transfer(pattern[i]) != pattern[i]) { ok = 0; break; }
        }
        T1_SPI_PORT.endTransaction();
        PUSH_RESULT(BENCH_TEST_SPI_LOOPBACK,
                    ok ? BENCH_TESTRESULT_PASS : BENCH_TESTRESULT_FAIL, 0);
    } else {
        PUSH_RESULT(BENCH_TEST_SPI_LOOPBACK, BENCH_TESTRESULT_SKIPPED, 0);
    }

    /* ---- L4: 595 -> 165 loopback, needs the jumper named in the bring-up doc -------------- */
    /* A walking one is used rather than a fixed pattern because it is the only test that proves
     * every bit position independently: a fixed byte cannot tell a correct chain from one with
     * two bits swapped. */
    if (level >= 2u) {
        uint8_t ok = 1;
        uint8_t saveCtl = mCtlByte;
        for (uint8_t bit = 0; bit < 8u && ok; bit++) {
            uint8_t out[FAB_MAX_595];
            memcpy(out, mShiftOut, sizeof(out));
            out[1] = (uint8_t)(1u << bit);          /* chip 1: first general-purpose 595       */
            shiftOut_(out, mShiftChips);
            delayMicroseconds(10);
            uint8_t in[FAB_MAX_165] = { 0 };
            shiftIn_(in, 1);
            if (in[0] != (uint8_t)(1u << bit)) { ok = 0; }
        }
        setControlByte(saveCtl);
        PUSH_RESULT(BENCH_TEST_SHIFT_LOOPBACK,
                    ok ? BENCH_TESTRESULT_PASS : BENCH_TESTRESULT_FAIL, 0);
    } else {
        PUSH_RESULT(BENCH_TEST_SHIFT_LOOPBACK, BENCH_TESTRESULT_SKIPPED, 0);
    }

    /* ---- Chip-select exclusivity and global deselect -------------------------------------- */
    /* These cannot be proven from inside the Teensy -- nothing reads the '138 outputs back. The
     * driver walks all eight addresses and pulses the marker pin so the ANALYSER can prove it,
     * and reports SKIPPED rather than PASS because claiming to have verified something no
     * instrument observed is exactly the dishonesty this project is built against. */
    {
        for (uint8_t slot = 0; slot < FAB_MAX_CS_SLOTS; slot++) {
            mark(1);
            csSelect(slot);
            delayMicroseconds(20);
            csDeselect();
            delayMicroseconds(20);
        }
        mark(3);
        PUSH_RESULT(BENCH_TEST_CS_EXCLUSIVE, BENCH_TESTRESULT_SKIPPED, 0xAA01u);
        PUSH_RESULT(BENCH_TEST_CS_DESELECT,  BENCH_TESTRESULT_SKIPPED, 0xAA02u);
    }

    /* ---- ADC rails: mux channels tied to GND and 3V3 -------------------------------------- */
    /* Reads leaf 0 channel 0 (expected near 0) and channel 7 (expected near full scale). This
     * is a wiring convention the bring-up doc asks for, so it is only meaningful once those two
     * channels are actually strapped -- which is why the tolerance is generous and the detail
     * carries the raw values for a human to judge. */
    {
        uint16_t lo = 0, hi = 0;
        uint8_t a = adcRead(0, 0, 4, &lo);
        uint8_t b = adcRead(0, 7, 4, &hi);
        adcParkTree();
        uint8_t res = BENCH_TESTRESULT_SKIPPED;
        if (a == BENCH_ST_OK && b == BENCH_ST_OK) {
            bool loOk = lo < (uint16_t)(FAB_ADC_FULL_SCALE / 20);        /* < 5%              */
            bool hiOk = hi > (uint16_t)(FAB_ADC_FULL_SCALE * 19 / 20);   /* > 95%             */
            res = (loOk && hiOk) ? BENCH_TESTRESULT_PASS : BENCH_TESTRESULT_FAIL;
        }
        PUSH_RESULT(BENCH_TEST_ADC_RAILS, res, (uint16_t)((lo & 0xFFu) << 8 | (hi >> 4)));
    }

    /* ---- Interrupt line at rest ----------------------------------------------------------- */
    /* With nothing changing, the shared open-drain line must be high. Low at rest means either
     * an interrupt was never cleared -- the MIRROR trap -- or the pull-up is missing. Both are
     * silent failures that stop interrupts working without producing any error, so they are
     * checked explicitly rather than discovered later. */
    {
        bool idleHigh = digitalReadFast(T1_FABRIC_IRQ) == HIGH;
        PUSH_RESULT(BENCH_TEST_IRQ_ROUNDTRIP,
                    idleHigh ? BENCH_TESTRESULT_PASS : BENCH_TESTRESULT_FAIL,
                    idleHigh ? 0u : 1u);
    }

    /* ---- Power budget: needs the INA219 sensors, which are on order ----------------------- */
    PUSH_RESULT(BENCH_TEST_POWER_BUDGET, BENCH_TESTRESULT_SKIPPED, 0);

    return (uint8_t)(n / 4u);
}

#undef PUSH_RESULT

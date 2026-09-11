/* ===========================================================================================
 *  bench_fabric.h -- BENCH ONE: the 74HC / MCP logic fabric driver
 * ===========================================================================================
 *
 *  Teensy 4.1 #1 only. This is the sole owner of SPI0 and Wire in this system; no other node is
 *  wired to those buses at all, which is how "exactly one bus master" is enforced by physics
 *  rather than by a comment.
 *
 *  WHAT THIS DRIVER GUARANTEES, AND WHY EACH GUARANTEE EXISTS
 *  -----------------------------------------------------------
 *
 *  1. A CHIP SELECT IS NEVER LEFT ASSERTED.
 *     There is no public "select" call. Selection only happens inside a transfer, which always
 *     deselects before returning, including on every error path. A caller cannot leak a
 *     selected chip, because the API gives it no way to.
 *
 *  2. TWO CHIP SELECTS ARE NEVER LOW AT ONCE.
 *     Every address change is break-before-make: disable the decoder, change the address, let
 *     it settle, re-enable. A 74HC138 whose address changes while enabled walks a low pulse
 *     across intermediate outputs -- the three address bits do not change simultaneously, so
 *     briefly the decoder points somewhere nobody asked for. That glitch is nanoseconds long
 *     and is more than enough to clock a byte into an SPI flash chip.
 *
 *  3. THE 74HC165 CHAIN IS DISCONNECTED FROM MISO UNLESS IT IS BEING READ.
 *     A '165's QH is always driving; it has no chip select and no tri-state. The driver gates
 *     it through a 74HC373 held transparent, enabling it only for the duration of a shift-in.
 *     Without this, every other read on the shared bus is contending with it.
 *
 *  4. AN MCP23017 INTERRUPT ALWAYS READS BOTH PORTS.
 *     With MIRROR=1 the interrupt does not clear until GPIOA and GPIOB have both been read.
 *     Servicing only one leaves INT asserted forever and interrupts stop, silently, with no
 *     error anywhere. This is the single most common way an MCP23017 design dies.
 *
 *  5. THE FIRST ADC SAMPLE AFTER A MUX CHANGE IS DISCARDED, ALWAYS.
 *     Charge injection makes it wrong in a repeatable direction, so averaging it in yields a
 *     stable, plausible, wrong number. A caller is never given the option to keep it.
 *
 *  6. EVERY OPERATION RETURNS A STATUS THAT DISTINGUISHES "NOT THERE" FROM "FAILED".
 *     A 74HC595 chain acknowledges nothing, so a write into an empty socket succeeds. Where the
 *     hardware cannot tell us, the port table does: a port without BENCH_PORTF_PRESENT returns
 *     BENCH_ST_NOT_PRESENT immediately instead of pretending.
 *
 *
 *  WHAT IT DELIBERATELY DOES NOT DO
 *  ---------------------------------
 *  No dynamic allocation. No blocking longer than a single transaction. No retries -- a retry
 *  hides a fault during bring-up, and this fabric is being characterised, not shipped. Faults
 *  are reported upward with context and the caller decides, which is the same rule the radio
 *  link already follows.
 * ===========================================================================================
 */

#ifndef BENCH_FABRIC_H
#define BENCH_FABRIC_H

#include <Arduino.h>
#include <stdint.h>
#include <Wire.h>   /* TwoWire appears in a private member signature below, so it must be a
                     * complete type HERE and not merely in the .cpp. Including it only in the
                     * implementation compiled fine on its own and failed the moment a sketch
                     * included this header first -- which is exactly the sort of order-dependent
                     * break that looks like a mysterious build failure. */

#include "bench_pins.h"
#include "bench_ports.h"
#include "bench_protocol.h"

/* ===========================================================================================
 * FAULT REPORTING
 * ===========================================================================================
 * The driver never prints and never decides. It calls this, and the sketch turns it into a
 * FAB_FAULT_EVENT frame, a console line, or both. Keeping policy out of the driver is what
 * lets the same driver run on the worker, in a test harness, or under a fuzzer.
 */
typedef void (*BenchFaultFn)(uint8_t code, uint8_t port, uint16_t detail);

/* ===========================================================================================
 * STATISTICS -- the counters that make a fault visible instead of merely felt
 * ===========================================================================================
 * Every one of these exists because its absence would make some real failure invisible.
 * They are reported by FAB_INFO_RESP and printed by the console 'fabric' command.
 */
typedef struct {
    uint32_t spi_transfers;
    uint32_t i2c_transactions;
    uint32_t i2c_nacks;          /* device did not ACK: unplugged, wrong address, dead bus     */
    uint32_t i2c_recoveries;     /* SDA found stuck low and clock pulses were issued           */
    uint32_t irq_events;
    uint32_t irq_spurious;       /* line went low, no INTF bit set anywhere -- noise or a fault */
    uint32_t irq_storms;         /* line STILL low after a full service of every expander      */
    uint32_t adc_conversions;
    uint32_t adc_discarded;      /* first-after-switch samples thrown away, on purpose         */
    uint32_t shift_out_ops;
    uint32_t shift_in_ops;
    uint32_t cs_changes;
    uint32_t faults;
} BenchFabricStats;

/* ===========================================================================================
 * THE FABRIC
 * =========================================================================================== */
class BenchFabric {
public:
    BenchFabric();

    /* --- lifecycle ------------------------------------------------------------------------ */

    /* Brings up the buses, parks every control line in its safe state, validates the port table
     * and scans for what is actually fitted. Returns BENCH_ST_OK, or BENCH_ST_HW_FAULT if the
     * port table is incoherent -- in which case nothing is initialised, because running with a
     * table that cannot be right is worse than not running. */
    uint8_t begin(BenchFaultFn faultFn);

    /* Parks everything: decoder disabled, MISO gate Hi-Z, all 595 outputs cleared and latched,
     * mux tree disabled. Safe to call at any time and idempotent. This is what a watchdog reset
     * handler or a fault path calls before doing anything else. */
    void parkSafe();

    bool isReady() const { return mReady; }
    uint8_t hwRev() const { return BENCH_FABRIC_HW_REV; }

    /* --- discovery ------------------------------------------------------------------------- */

    /* Probes every 7-bit address on a bus. Fills `found` with the addresses that ACKed and
     * returns how many. Addresses 0x00-0x07 and 0x78-0x7F are reserved by the I2C spec and are
     * skipped -- probing them can put a compliant device into a mode it will not leave. */
    uint8_t i2cScan(uint8_t bus, uint8_t *found, uint8_t maxFound);

    /* Re-runs the scan and updates BENCH_PORTF_PRESENT on every I2C-backed port. Emits
     * DEVICE_LOST / DEVICE_APPEARED faults for anything that changed since the last scan, which
     * is how hot-plug becomes an event rather than a mystery. */
    uint8_t rescan();

    bool portPresent(uint8_t portId) const;

    /* --- SPI: the escape hatch that makes any module supportable --------------------------- */

    /* One atomic unit: select the port's 74HC138 slot, clock len bytes, deselect. tx or rx may
     * be null for a write-only or read-only transfer. There is deliberately no way to hold a
     * selection across calls; a module needing that is a module needing a native chip select,
     * and it should take PORT_TFT's place in the table rather than bend this API.
     *
     * mode is the SPI mode (0-3) the MODULE needs, not the fabric's. Modules disagree about
     * this constantly and getting it wrong produces data that is shifted by one bit -- readable,
     * wrong, and easy to mistake for a broken sensor. */
    uint8_t spiTransfer(uint8_t portId, uint8_t mode, uint32_t hz,
                        const uint8_t *tx, uint8_t *rx, uint16_t len);

    /* --- I2C ------------------------------------------------------------------------------- */

    /* Write-then-read against one address. When rdLen > 0 a REPEATED START is used rather than
     * a STOP, because a register read that is interrupted between the pointer write and the
     * data read returns a different register's contents -- and on a bus with one master, a
     * repeated start makes that impossible rather than unlikely. */
    uint8_t i2cTransfer(uint8_t bus, uint8_t addr7,
                        const uint8_t *wr, uint8_t wrLen,
                        uint8_t *rd, uint8_t rdLen);

    /* Clocks SCL nine times with SDA released, then issues a STOP. This frees a bus where a
     * device was reset mid-byte and is still holding SDA low waiting for clocks that will never
     * come -- the classic symptom being every subsequent transaction failing forever after a
     * reset that "should not have mattered". */
    uint8_t i2cRecover(uint8_t bus);

    /* --- MCP23017 -------------------------------------------------------------------------- */

    uint8_t mcpBegin(uint8_t portId);

    /* --- SPI port auxiliaries ---------------------------------------------------------------
     * A SPI port carries two signals beyond the bus, and they live on different silicon because
     * they point in different directions:
     *
     *      CTL  index 0  OUTPUT, 74HC595 chip 1   reset / chip-enable / mode strap  (~2 us)
     *      IRQ  index 1  INPUT,  MCP_A            interrupt / data-ready / busy   (interrupt-capable)
     *
     * Both are reachable through the ordinary pinWrite/pinRead calls, so driving a module's
     * reset from Python needs no new command and no new firmware. */
    uint8_t portCtlWrite(uint8_t portId, uint8_t value);
    uint8_t portIrqRead(uint8_t portId, uint8_t *value);

    uint8_t pinMode_(uint8_t portId, uint8_t index, uint8_t mode);
    uint8_t pinWrite(uint8_t portId, uint8_t index, uint8_t value);
    uint8_t pinRead(uint8_t portId, uint8_t index, uint8_t *value);

    /* Reads GPIOA and GPIOB in ONE transaction. Two separate byte reads observe two different
     * instants, and anything sampling a bus, a keypad or an encoder across that gap reads a
     * combined state that never existed on the wires. */
    uint8_t portRead16(uint8_t portId, uint16_t *value);
    uint8_t portWrite16(uint8_t portId, uint16_t value);

    /* mask bits enable interrupt-on-change for the corresponding pins. */
    uint8_t irqConfig(uint8_t portId, uint16_t mask);

    /* Call from loop() when the shared IRQ line is low. Walks every expander, reads INTF to see
     * who asked, reads INTCAP (the value AT the interrupt) and then GPIO (the value now), and
     * hands both to the callback -- they differ whenever the input changed again in between,
     * and only INTCAP answers "what happened".
     *
     * Returns the number of expanders that had something to say. Zero means the line was low
     * but nobody claimed it: counted as irq_spurious, which is a real signal worth watching. */
    uint8_t serviceIrq(void (*onEvent)(uint8_t portId, uint16_t intf,
                                       uint16_t intcap, uint16_t gpio));

    bool irqAsserted() const { return digitalReadFast(T1_FABRIC_IRQ) == LOW; }

    /* Read-modify-write one bit of the 74HC595 chain, against the shadow copy. The chain has no
     * readback path at all, so without a shadow there is no way to change one bit without
     * destroying the other seven. */
    uint8_t setShiftBit(uint8_t chip, uint8_t bit, uint8_t value);

    /* --- shift registers ------------------------------------------------------------------- */

    /* data[] is in CHAIN ORDER: element 0 is the chip nearest the Teensy. The driver reverses it
     * internally, because the first byte clocked out of a 595 chain ends up in the FARTHEST
     * device. Exposing chain order means the caller's mental model matches the silk. */
    uint8_t shiftOut_(const uint8_t *data, uint8_t chips);

    /* Snapshots every 74HC165 input simultaneously via /PL, then clocks them in. Bit 7 of each
     * returned byte is the chip's H input (pin 6), because H exits first. Getting this backwards
     * produces a mirrored byte, which looks like a wiring error and is not one. */
    uint8_t shiftIn_(uint8_t *data, uint8_t chips);

    /* Rewrites only the fabric control byte and re-latches, leaving the rest of the chain as it
     * was. Used by the analog tree. */
    uint8_t setControlByte(uint8_t value);
    uint8_t controlByte() const { return mCtlByte; }

    /* --- analog -------------------------------------------------------------------------- */

    /* leaf 0-7, channel 0-7. Sets the mux, waits FAB_MUX_SETTLE_US, throws the first conversion
     * away and averages `samples` more. Pass samples=1 for a single (still post-discard) read. */
    uint8_t adcRead(uint8_t leaf, uint8_t channel, uint8_t samples, uint16_t *out);

    /* Reads `count` consecutive channels starting at absolute channel `first` (0-63), crossing
     * leaf boundaries as needed. */
    uint8_t adcSweep(uint8_t first, uint8_t count, uint8_t samples, uint16_t *out);

    /* Disables every mux leaf. The ADC pin then floats free of all 64 sensors, which is the only
     * state in which the tree's own leakage can be measured. */
    uint8_t adcParkTree();

    /* --- instrumentation ------------------------------------------------------------------- */

    /* Toggles the logic-analyser marker pin. `pulses` short pulses, so several distinct events
     * can be told apart in one capture by counting edges. The whole point is to put a timestamp
     * that firmware chose into a capture that hardware recorded. */
    void mark(uint8_t pulses);

    const BenchFabricStats &stats() const { return mStats; }
    void resetStats();

    /* --- self-test ------------------------------------------------------------------------- */

    /* level 0 = non-invasive (bus idle, scan, readback).
     * level 1 = adds loopbacks, which require the two jumpers named in the bring-up doc.
     * level 2 = adds tests that drive outputs, and must not run with modules attached.
     *
     * Writes one [test_id, result, detail_hi, detail_lo] quad per test into results[].
     * Returns the number of tests run. A test whose hardware is not fitted reports SKIPPED, not
     * PASS -- a self-test that passes by not looking is worse than no self-test. */
    uint8_t selfTest(uint8_t level, uint8_t *results, uint8_t maxQuads);

private:
    /* --- chip select, private on purpose: see guarantee 1 ---------------------------------- */
    void csDeselect();
    void csSelect(uint8_t slot);

    /* --- MCP low level --------------------------------------------------------------------- */
    uint8_t mcpWrite8(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t val);
    uint8_t mcpWrite16(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t val);
    uint8_t mcpRead8(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t *val);
    uint8_t mcpRead16(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t *val);

    TwoWire *busFor(uint8_t bus);
    void reportFault(uint8_t code, uint8_t port, uint16_t detail);

    BenchFaultFn      mFaultFn;
    BenchFabricStats  mStats;
    bool              mReady;

    /* Shadow of the 595 chain. Written-only hardware must be shadowed or a read-modify-write of
     * one bit would clear every other output in the chain. */
    uint8_t           mShiftOut[FAB_MAX_595];
    uint8_t           mShiftChips;
    uint8_t           mCtlByte;

    /* Shadow of each expander's OLAT and IODIR, for the same reason. */
    uint16_t          mMcpOlat[FAB_MAX_MCP];
    uint16_t          mMcpIodir[FAB_MAX_MCP];
    uint16_t          mMcpPullup[FAB_MAX_MCP];
    uint8_t           mMcpPresent;     /* bitmask over the 8 possible addresses               */

    uint8_t           mCsCurrent;      /* the slot currently addressed (not necessarily enabled) */
    bool              mCsEnabled;
};

/* ===========================================================================================
 * MCP23017 REGISTER MAP -- BANK = 0 (the power-on default)
 * ===========================================================================================
 * In BANK=0 the A and B registers interleave, so a 16-bit read starting at an A register picks
 * up its B partner in the next byte. That is what makes the one-transaction 16-bit read work,
 * and it is why this driver never sets BANK=1: doing so would silently break every 16-bit
 * access in this file while leaving every 8-bit access working, which is the worst kind of
 * half-broken.
 */
#define MCP_IODIRA   0x00u
#define MCP_IODIRB   0x01u
#define MCP_IPOLA    0x02u
#define MCP_IPOLB    0x03u
#define MCP_GPINTENA 0x04u
#define MCP_GPINTENB 0x05u
#define MCP_DEFVALA  0x06u
#define MCP_DEFVALB  0x07u
#define MCP_INTCONA  0x08u
#define MCP_INTCONB  0x09u
#define MCP_IOCONA   0x0Au   /* IOCON is mirrored at 0x0A and 0x0B -- one register, two addresses */
#define MCP_IOCONB   0x0Bu
#define MCP_GPPUA    0x0Cu
#define MCP_GPPUB    0x0Du
#define MCP_INTFA    0x0Eu
#define MCP_INTFB    0x0Fu
#define MCP_INTCAPA  0x10u
#define MCP_INTCAPB  0x11u
#define MCP_GPIOA    0x12u
#define MCP_GPIOB    0x13u
#define MCP_OLATA    0x14u
#define MCP_OLATB    0x15u

/* IOCON bits. */
#define MCP_IOCON_BANK   0x80u  /* 1 = separate A/B register banks. NEVER SET -- see above.    */
#define MCP_IOCON_MIRROR 0x40u  /* 1 = INTA and INTB are internally OR'd                       */
#define MCP_IOCON_SEQOP  0x20u  /* 0 = address auto-increments, which the 16-bit reads rely on */
#define MCP_IOCON_DISSLW 0x10u  /* 1 = disable SDA slew rate control                           */
#define MCP_IOCON_HAEN   0x08u  /* address enable -- MCP23S17 only, ignored on the I2C part    */
#define MCP_IOCON_ODR    0x04u  /* 1 = INT is open-drain; this OVERRIDES INTPOL                */
#define MCP_IOCON_INTPOL 0x02u  /* INT polarity when push-pull. Irrelevant once ODR is set.    */

/* The configuration this fabric uses, and the reason for each bit:
 *   MIRROR  -- one wire back to the Teensy no matter which port fired.
 *   ODR     -- open drain, so eight expanders wire-OR onto one pin with one pull-up.
 *   SEQOP=0 -- address auto-increment, so GPIOA+GPIOB come back in one transaction.
 *   BANK=0  -- the register interleave every 16-bit access in this driver assumes. */
#define MCP_IOCON_FABRIC  (MCP_IOCON_MIRROR | MCP_IOCON_ODR)

#endif /* BENCH_FABRIC_H */

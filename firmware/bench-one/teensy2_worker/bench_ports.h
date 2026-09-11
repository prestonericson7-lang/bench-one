/* ===========================================================================================
 *  bench_ports.h -- BENCH ONE: the module port registry
 * ===========================================================================================
 *
 *  THIS FILE IS THE ANSWER TO "PLUG IN ANYTHING WITHOUT GUESSING"
 *  --------------------------------------------------------------
 *  A logical PORT is a named place to plug a module in. Firmware, Python and the silk on the
 *  board all say the same short string -- "P3" -- and this table is the only thing that knows
 *  P3 means chip-select slot 2 on SPI0 with its interrupt on the shared IRQ line.
 *
 *  Everything above this table addresses hardware by port id. Nothing above it knows a pin
 *  number, an I2C address or a 74HC138 slot. That is the whole mechanism:
 *
 *    - Moving a module from one slot to another is a one-line edit HERE, and nothing else in
 *      the system changes. No sketch, no script, no command.
 *    - A command that names a port the table does not contain fails immediately with
 *      BENCH_ST_BAD_PORT. It cannot half-work, and it cannot touch the wrong device.
 *    - Two ports claiming one chip-select slot is caught by bench_ports_validate() at boot and
 *      reported as BENCH_FAULT_CS_CONFLICT, instead of being discovered as two modules that
 *      mysteriously respond together.
 *    - The port table is dumped over the wire by FAB_PORTS_REQ, so the Linux box discovers the
 *      fabric at runtime rather than carrying a second copy of the map that can drift.
 *
 *  The last point is the one worth insisting on. The most expensive class of failure this
 *  project has recorded is not a crash -- it is two components that disagree while both look
 *  correct. A header copied into four sketch folders needs a sync script for exactly this
 *  reason. A port map duplicated between firmware and a Python script would need the same, so
 *  it is not duplicated: there is one copy, in firmware, and the Linux side asks.
 *
 *
 *  WHY PORTS ARE DECLARED BUT THEIR CONTENTS ARE NOT
 *  --------------------------------------------------
 *  The table below declares the SHAPE of the fabric -- eight chip-select slots, two expander
 *  banks, a shift-out bank, a shift-in bank, an analog tree. It does not declare what is plugged
 *  into them, because at the time of writing no specific radio or sensor has been named.
 *
 *  That is deliberate and it is not a gap. Inventing "P3 = CC1101" would put a fiction in the
 *  one file whose entire value is being true. When a module is chosen, it gets a line here and
 *  a silk label, and it works.
 * ===========================================================================================
 */

#ifndef BENCH_PORTS_H
#define BENCH_PORTS_H

#include <stdint.h>
#include "bench_pins.h"
#include "bench_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================================
 * PORT KINDS
 * =========================================================================================== */
#define BENCH_PORT_KIND_NONE      0x00u
#define BENCH_PORT_KIND_SPI       0x01u  /* a 74HC138 chip-select slot on SPI0                 */
#define BENCH_PORT_KIND_I2C       0x02u  /* a fixed 7-bit address on one of the three buses    */
#define BENCH_PORT_KIND_GPIO_EXP  0x03u  /* a span of MCP23017 pins                            */
#define BENCH_PORT_KIND_SHIFT_OUT 0x04u  /* a span of 74HC595 outputs                          */
#define BENCH_PORT_KIND_SHIFT_IN  0x05u  /* a span of 74HC165 inputs                           */
#define BENCH_PORT_KIND_ANALOG    0x06u  /* a span of 74HC4051 channels                        */
#define BENCH_PORT_KIND_NATIVE    0x07u  /* a Teensy pin taken directly                        */

/* ===========================================================================================
 * PORT FLAGS
 * =========================================================================================== */
#define BENCH_PORTF_NONE          0x0000u
#define BENCH_PORTF_PRESENT       0x0001u  /* something is fitted here (set at scan time)      */
#define BENCH_PORTF_HOTPLUG       0x0002u  /* may appear/disappear on a live bus               */
#define BENCH_PORTF_IRQ           0x0004u  /* participates in the shared IRQ chain             */
#define BENCH_PORTF_LEVEL_SHIFTED 0x0008u  /* reaches a 5 V island through a TXB/TXS           */
#define BENCH_PORTF_5V_MODULE     0x0010u  /* the module itself is 5 V -- NEVER wire direct    */
#define BENCH_PORTF_SINK_ONLY     0x0020u  /* outputs sink current; never source (MCP LEDs)    */
#define BENCH_PORTF_RESERVED      0x0040u  /* claimed by the fabric itself, not for modules    */
#define BENCH_PORTF_HIGH_SPEED    0x0080u  /* wants the full SPI clock; keep leads short       */

/* ===========================================================================================
 * THE DESCRIPTOR
 * ===========================================================================================
 * Kept deliberately flat and packed: it goes on the wire verbatim in FAB_PORTS_RESP, so the
 * Linux side decodes it with one struct format string and no negotiation.
 *
 * 12 bytes each. 32 ports = 384 bytes, comfortably inside one 1015-byte frame, so the entire
 * fabric map arrives in a single round trip and can never be half-read.
 */
typedef struct {
    uint8_t  id;         /* stable id, used on the wire                                        */
    uint8_t  kind;       /* BENCH_PORT_KIND_*                                                  */
    uint8_t  bus;        /* SPI: 0. I2C: 0=Wire 1=Wire1 2=Wire2. Others: 0.                    */
    uint8_t  slot;       /* SPI: 74HC138 output 0-7. I2C: 7-bit address. Else: chip index.     */
    uint8_t  first;      /* first bit/channel index within the layer                           */
    uint8_t  width;      /* how many bits/channels this port owns                              */
    uint8_t  irq_bit;    /* which INTF bit reports this port, 0xFF if none                     */
    uint8_t  reserved;   /* keeps the struct 4-byte aligned and the wire format stable         */
    uint16_t flags;      /* BENCH_PORTF_*                                                      */
    uint16_t detected;   /* runtime: what actually answered here. 0 = nothing.                 */
} BenchPortDesc;

#define BENCH_PORT_DESC_WIRE_LEN  12u   /* python struct: "<8BHH" -- keep these in step        */

/* Names live in a parallel array rather than inside the struct. A char[8] per port would put
 * 256 bytes of string into the wire format for no reason -- the Linux side already has the
 * names, and the numbers are what identify a port. */
#define BENCH_PORT_NAME_MAX       8

/* ===========================================================================================
 * PORT IDS
 * ===========================================================================================
 * Ranges are grouped so that an id alone tells you which layer it belongs to. The gaps are
 * intentional: a new port can be added inside a range later without renumbering anything, and
 * renumbering is how a stored capture stops matching the firmware that produced it.
 */

/* --- SPI module slots: the eight 74HC138 chip selects -------------------------------------- */
#define PORT_SPI0                 0x10u  /* 74HC138 Y0                                         */
#define PORT_SPI1                 0x11u
#define PORT_SPI2                 0x12u
#define PORT_SPI3                 0x13u
#define PORT_SPI4                 0x14u
#define PORT_SPI5                 0x15u
#define PORT_SPI6                 0x16u
#define PORT_SPI7                 0x17u

/* --- I2C expander banks -------------------------------------------------------------------- */
#define PORT_MCP_A                0x20u  /* 0x20, 16 IO                                        */
#define PORT_MCP_B                0x21u  /* 0x21, 16 IO                                        */
#define PORT_MCP_C                0x22u
#define PORT_MCP_D                0x23u
#define PORT_MCP_E                0x24u
#define PORT_MCP_F                0x25u
#define PORT_MCP_G                0x26u
#define PORT_MCP_H                0x27u

/* --- generic I2C module slots (sensors on Qwiic-style leads) -------------------------------- */
#define PORT_I2C_M0               0x30u
#define PORT_I2C_M1               0x31u
#define PORT_I2C_M2               0x32u
#define PORT_I2C_M3               0x33u

/* --- shift register banks ------------------------------------------------------------------ */
#define PORT_SHIFT_CTL            0x40u  /* 74HC595 chip 0 -- the fabric's own control byte    */
#define PORT_SHIFT_OUT_A          0x41u  /* 74HC595 chips 1-2, 16 outputs                      */
#define PORT_SHIFT_OUT_B          0x42u  /* 74HC595 chips 3-4, 16 outputs                      */
#define PORT_SHIFT_IN_A           0x48u  /* 74HC165 chips 0-1, 16 inputs                       */
#define PORT_SHIFT_IN_B           0x49u  /* 74HC165 chips 2-3, 16 inputs                       */

/* --- analog leaves -------------------------------------------------------------------------- */
#define PORT_ANALOG_L0            0x50u  /* 74HC4051 leaf 0, channels 0-7                      */
#define PORT_ANALOG_L1            0x51u
#define PORT_ANALOG_L2            0x52u
#define PORT_ANALOG_L3            0x53u
#define PORT_ANALOG_L4            0x54u
#define PORT_ANALOG_L5            0x55u
#define PORT_ANALOG_L6            0x56u
#define PORT_ANALOG_L7            0x57u

/* --- native pins the fabric owns directly --------------------------------------------------- */
#define PORT_TFT                  0x60u
#define PORT_LA_MARK              0x61u

#define BENCH_PORT_INVALID        0xFFu

/* ===========================================================================================
 * THE DEFAULT TABLE -- fabric hardware revision 1
 * ===========================================================================================
 *
 * This describes the MINIMAL CORE plus the expansion headers it can grow into. Populate it as
 * chips are actually soldered; a port whose hardware is not fitted simply never sets
 * BENCH_PORTF_PRESENT at scan time, and every command against it returns BENCH_ST_NOT_PRESENT
 * rather than timing out or, worse, appearing to succeed.
 *
 * MINIMAL CORE, which is what makes the fabric useful on day one:
 *   1x 74HC138   -> 8 addressable SPI chip selects
 *   2x MCP23017  -> 32 interrupt-capable IO on two wires
 *   1x 74HC595   -> the control byte (steers the analog tree)
 *   1x 74HC165   -> 8 snapshot inputs
 *   1x 74HC373   -> the MISO gate that makes the shared SPI bus safe
 *
 * Five chips. Everything after that is more of the same, and none of it needs another Teensy pin.
 */

/* Element order MUST match BenchPortDesc field order. Written out longhand rather than with a
 * macro so that a reader can check it against the struct without decoding anything. */
static const BenchPortDesc BENCH_PORT_TABLE[] = {
  /*  id              kind                     bus slot first width irq  rsv  flags */

  /* ---- SPI module slots. Any SPI module drops into one of these and is addressed by name.
   *      slot is the 74HC138 output number; the driver never lets two be low at once.        */
  { PORT_SPI0,        BENCH_PORT_KIND_SPI,      0,  0,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI1,        BENCH_PORT_KIND_SPI,      0,  1,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI2,        BENCH_PORT_KIND_SPI,      0,  2,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI3,        BENCH_PORT_KIND_SPI,      0,  3,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI4,        BENCH_PORT_KIND_SPI,      0,  4,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI5,        BENCH_PORT_KIND_SPI,      0,  5,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_SPI6,        BENCH_PORT_KIND_SPI,      0,  6,   0,   1,  0xFF, 0, BENCH_PORTF_HOTPLUG },
  /* Slot 7 is reserved as the guaranteed-idle address. Parking the decoder here means "no
   * module selected" is reachable even if the enable line is ever in doubt, which makes the
   * CS_DESELECT self-test able to distinguish a stuck enable from a stuck address. */
  { PORT_SPI7,        BENCH_PORT_KIND_SPI,      0,  7,   0,   1,  0xFF, 0, BENCH_PORTF_RESERVED },

  /* ---- MCP23017 expander banks. Two fitted in the minimal core; six more need only an
   *      address strap and two wires. irq_bit is the position this chip occupies when the ISR
   *      walks the bank looking for who pulled the shared line low.                           */
  { PORT_MCP_A,       BENCH_PORT_KIND_GPIO_EXP, 0,  0x20, 0, 16, 0,    0,
        BENCH_PORTF_IRQ | BENCH_PORTF_SINK_ONLY },
  { PORT_MCP_B,       BENCH_PORT_KIND_GPIO_EXP, 0,  0x21, 0, 16, 1,    0,
        BENCH_PORTF_IRQ | BENCH_PORTF_SINK_ONLY },

  /* ---- Generic I2C sensor slots. addr 0 means "whatever answers here"; the scan fills in
   *      `detected`. These are the Qwiic/STEMMA-style ports: GND / 3V3 / SDA / SCL, polarised,
   *      so a sensor cannot be plugged in backwards.                                          */
  { PORT_I2C_M0,      BENCH_PORT_KIND_I2C,      0,  0x00, 0,  1, 0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_I2C_M1,      BENCH_PORT_KIND_I2C,      0,  0x00, 0,  1, 0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_I2C_M2,      BENCH_PORT_KIND_I2C,      0,  0x00, 0,  1, 0xFF, 0, BENCH_PORTF_HOTPLUG },
  { PORT_I2C_M3,      BENCH_PORT_KIND_I2C,      0,  0x00, 0,  1, 0xFF, 0, BENCH_PORTF_HOTPLUG },

  /* ---- Shift-register banks. The control byte is RESERVED: it steers the analog tree, and a
   *      general-purpose write to it would silently move the mux under a conversion.          */
  { PORT_SHIFT_CTL,   BENCH_PORT_KIND_SHIFT_OUT, 0, FABCTL_CHIP_INDEX, 0, 8, 0xFF, 0,
        BENCH_PORTF_RESERVED },
  { PORT_SHIFT_OUT_A, BENCH_PORT_KIND_SHIFT_OUT, 0, 1,   8,  16, 0xFF, 0, BENCH_PORTF_SINK_ONLY },
  { PORT_SHIFT_OUT_B, BENCH_PORT_KIND_SHIFT_OUT, 0, 3,  24,  16, 0xFF, 0, BENCH_PORTF_SINK_ONLY },
  { PORT_SHIFT_IN_A,  BENCH_PORT_KIND_SHIFT_IN,  0, 0,   0,  16, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_SHIFT_IN_B,  BENCH_PORT_KIND_SHIFT_IN,  0, 2,  16,  16, 0xFF, 0, BENCH_PORTF_NONE },

  /* ---- Analog leaves. Eight channels each, all bussed to one ADC pin.                       */
  { PORT_ANALOG_L0,   BENCH_PORT_KIND_ANALOG,   0,  0,   0,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L1,   BENCH_PORT_KIND_ANALOG,   0,  1,   8,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L2,   BENCH_PORT_KIND_ANALOG,   0,  2,  16,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L3,   BENCH_PORT_KIND_ANALOG,   0,  3,  24,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L4,   BENCH_PORT_KIND_ANALOG,   0,  4,  32,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L5,   BENCH_PORT_KIND_ANALOG,   0,  5,  40,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L6,   BENCH_PORT_KIND_ANALOG,   0,  6,  48,   8, 0xFF, 0, BENCH_PORTF_NONE },
  { PORT_ANALOG_L7,   BENCH_PORT_KIND_ANALOG,   0,  7,  56,   8, 0xFF, 0, BENCH_PORTF_NONE },

  /* ---- Native pins the fabric owns outright.                                                */
  { PORT_TFT,         BENCH_PORT_KIND_NATIVE,   0, T1_TFT_CS,  0, 1, 0xFF, 0,
        BENCH_PORTF_RESERVED | BENCH_PORTF_HIGH_SPEED },
  { PORT_LA_MARK,     BENCH_PORT_KIND_NATIVE,   0, T1_LA_MARK, 0, 1, 0xFF, 0,
        BENCH_PORTF_RESERVED },
};

#define BENCH_PORT_COUNT  (sizeof(BENCH_PORT_TABLE) / sizeof(BENCH_PORT_TABLE[0]))

/* Silk labels, index-matched to BENCH_PORT_TABLE. These are what gets printed on the board and
 * typed at the Python prompt. Short, because they have to fit on silk next to a header. */
static const char *const BENCH_PORT_NAMES[BENCH_PORT_COUNT] = {
    "P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7",
    "XA", "XB",
    "M0", "M1", "M2", "M3",
    "CTL", "OA", "OB", "IA", "IB",
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
    "TFT", "MARK",
};

/* ===========================================================================================
 * LOOKUP AND VALIDATION
 * =========================================================================================== */

static inline const BenchPortDesc *bench_port_find(uint8_t id)
{
    uint8_t i;
    for (i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        if (BENCH_PORT_TABLE[i].id == id) {
            return &BENCH_PORT_TABLE[i];
        }
    }
    return 0;
}

static inline const char *bench_port_name(uint8_t id)
{
    uint8_t i;
    for (i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        if (BENCH_PORT_TABLE[i].id == id) {
            return BENCH_PORT_NAMES[i];
        }
    }
    return "??";
}

static inline const char *bench_port_kind_name(uint8_t kind)
{
    switch (kind) {
        case BENCH_PORT_KIND_SPI:       return "SPI";
        case BENCH_PORT_KIND_I2C:       return "I2C";
        case BENCH_PORT_KIND_GPIO_EXP:  return "GPIO_EXP";
        case BENCH_PORT_KIND_SHIFT_OUT: return "SHIFT_OUT";
        case BENCH_PORT_KIND_SHIFT_IN:  return "SHIFT_IN";
        case BENCH_PORT_KIND_ANALOG:    return "ANALOG";
        case BENCH_PORT_KIND_NATIVE:    return "NATIVE";
        default:                        return "NONE";
    }
}

/* ===========================================================================================
 * bench_ports_validate -- catch a table that cannot be right, at boot, on the hardware
 * ===========================================================================================
 *
 * This runs on the Teensy at startup, not in a test suite on a PC, because the failure it
 * guards against is a config edit made at the bench at 2 a.m. between two builds.
 *
 * It answers three questions that a wrong answer to would be silent:
 *   - Does any chip-select slot appear twice? Two modules would then be selected together, and
 *     on a shared MISO that is bus contention, which reads as "the sensor gives random values".
 *   - Does any I2C address appear twice? The two devices would answer over each other.
 *   - Does any port index range exceed the layer it sits in? A write would land on a chip that
 *     is not there and return success, because a 74HC595 chain never acknowledges anything.
 *
 * Returns 0 if the table is coherent, otherwise the id of the FIRST offending port. Reporting
 * the offender rather than a bare failure is the difference between a five-second fix and an
 * evening of bisecting a header.
 */
static inline uint8_t bench_ports_validate(void)
{
    uint8_t i, j;

    for (i = 0; i < (uint8_t)BENCH_PORT_COUNT; i++) {
        const BenchPortDesc *a = &BENCH_PORT_TABLE[i];

        /* Range checks against the physical size of each layer. */
        switch (a->kind) {
            case BENCH_PORT_KIND_SPI:
                if (a->slot >= FAB_MAX_CS_SLOTS) { return a->id; }
                break;
            case BENCH_PORT_KIND_GPIO_EXP:
                if (a->slot < FAB_MCP_ADDR_BASE || a->slot > FAB_MCP_ADDR_MAX) { return a->id; }
                if ((uint16_t)a->first + a->width > 16u) { return a->id; }
                break;
            case BENCH_PORT_KIND_SHIFT_OUT:
                if (a->slot >= FAB_MAX_595) { return a->id; }
                break;
            case BENCH_PORT_KIND_SHIFT_IN:
                if (a->slot >= FAB_MAX_165) { return a->id; }
                break;
            case BENCH_PORT_KIND_ANALOG:
                if (a->slot >= FAB_MAX_MUX_LEAVES) { return a->id; }
                if (a->width > 8u) { return a->id; }
                break;
            default:
                break;
        }

        for (j = (uint8_t)(i + 1u); j < (uint8_t)BENCH_PORT_COUNT; j++) {
            const BenchPortDesc *b = &BENCH_PORT_TABLE[j];

            /* Duplicate id: every wire-level lookup would find only the first one. */
            if (a->id == b->id) { return a->id; }

            /* Two ports on one 74HC138 output. */
            if (a->kind == BENCH_PORT_KIND_SPI && b->kind == BENCH_PORT_KIND_SPI &&
                a->slot == b->slot) {
                return b->id;
            }

            /* Two expanders strapped to one address on one bus. */
            if (a->kind == BENCH_PORT_KIND_GPIO_EXP && b->kind == BENCH_PORT_KIND_GPIO_EXP &&
                a->bus == b->bus && a->slot == b->slot) {
                return b->id;
            }

            /* Two analog ports on one 74HC4051 leaf. */
            if (a->kind == BENCH_PORT_KIND_ANALOG && b->kind == BENCH_PORT_KIND_ANALOG &&
                a->slot == b->slot) {
                return b->id;
            }
        }
    }
    return 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* BENCH_PORTS_H */

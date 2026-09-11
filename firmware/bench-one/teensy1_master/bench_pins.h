/* ===========================================================================================
 *  bench_pins.h -- BENCH ONE: the single source of truth for every pin on every board
 * ===========================================================================================
 *
 *  Every sketch, every Python script and every silk label reads its pin numbers from here.
 *  If a wire moves, it moves in this file and nowhere else. A pin number typed anywhere but
 *  this file is a bug waiting for a bring-up session.
 *
 *  HOW THE TEENSY 4.1 PIN BUDGET WAS DECIDED
 *  ------------------------------------------
 *  Three assignments were fixed before anything else and are not negotiable:
 *
 *    1. Serial1 on pins 0/1 is the ESP32-S3 radio link. It is TESTED -- 53,530 frames, zero
 *       errors of every class -- and rewiring it would throw away the only measured baseline
 *       this project has.
 *    2. Wire on pins 18/19 is the only LPI2C1 pin pair on a Teensy 4.1. There is no choice.
 *    3. SPI2 is unreachable: its pins are the microSD socket's SDIO lines (42/43/45) and its
 *       alternates are bottom-side memory pads. A breadboard build cannot use it. That leaves
 *       SPI0 and SPI1, and the fabric gets one of each -- see below.
 *
 *
 *  THE FABRIC GETS ITS OWN SPI PORT, AND THAT WAS A CORRECTION
 *  ------------------------------------------------------------
 *  The first version of this file put everything on SPI0 and stated that SPI1 was unusable,
 *  because SPI1's default MISO is pin 1 -- which is Serial1's TX, the tested radio link.
 *
 *  That was wrong, and the datasheet research caught it. SPI1 (LPSPI3) has an ALTERNATE MISO on
 *  pin 39, selected with SPI1.setMISO(39) before SPI1.begin(). Its MOSI (26), SCK (27) and the
 *  alternate MISO (39) are all free in this build and all on the top-side headers.
 *
 *  So the split is:
 *
 *      SPI0  pins 11/12/13   ->  the 2.4in display, and nothing else
 *      SPI1  pins 26/27/39   ->  the entire logic fabric
 *
 *  Three things get better, and none of them is cosmetic:
 *
 *    - The display's refresh traffic and the fabric's traffic stop competing. A display push is
 *      thousands of bytes; a fabric operation is a handful. Sharing one bus meant every fabric
 *      read waited behind whatever the screen was doing.
 *    - The fabric's clock is no longer pin 13, which carries the onboard LED and its series
 *      resistor. That load is small but it is real, and it sits on the one net whose edge
 *      quality decides the maximum shift-register clock.
 *    - The 74HC165 contention problem shrinks from system-wide to fabric-local. The '165's
 *      output can no longer fight the display at all. It can still fight another module on the
 *      fabric bus, so the hardware gate below is still required -- but the blast radius is now
 *      one bus instead of two.
 *
 *
 *  THE ADDRESSING IDEA, IN ONE PARAGRAPH
 *  --------------------------------------
 *  Fifteen Teensy pins buy the entire fabric: 8 hardware chip selects, 128 expander IO, 160
 *  shift-register outputs, 120 shift-register inputs and 64 analog channels. It works because
 *  the fabric addresses ITSELF: the 74HC138 turns 4 pins into 8 exclusive chip selects, and the
 *  analog mux tree is steered by a byte inside the 74HC595 chain rather than by native GPIO, so
 *  adding 64 analog channels costs exactly one ADC pin and no digital pins at all.
 *
 *  That is the "minimal core, maximum expansion" rule made concrete, and it is why a new radio
 *  or sensor never needs a new Teensy pin -- only a free port on an existing layer.
 * ===========================================================================================
 */

#ifndef BENCH_PINS_H
#define BENCH_PINS_H

/* ===========================================================================================
 * PART 1 -- TEENSY 4.1 #1  (MASTER / HUB)
 * ===========================================================================================
 *
 * Teensy 4.1 pins 0-41 are all edge through-holes and reachable on a breadboard.
 * Pins 42-54 are underside SMD pads and are treated here as NOT AVAILABLE.
 */

/* ---- Link A: Luckfox Pico Mini B (Linux orchestrator) ---------------------------------- */
/* Serial2. Pins 7/8 are plain digital -- no ADC, no I2C, no SPI function lost. */
#define T1_LUCKFOX_SERIAL        Serial2
#define T1_LUCKFOX_RX            7      /* <- Luckfox pin 12, GPIO1_D0, UART3_TX_M1           */
#define T1_LUCKFOX_TX            8      /* -> Luckfox pin 13, GPIO1_D1, UART3_RX_M1           */

/* ---- Link B: ESP32-S3 radio -- EXISTING, TESTED, DO NOT MOVE ---------------------------- */
#define T1_RADIO_SERIAL          Serial1
#define T1_RADIO_RX              0      /* <- ESP32-S3 GPIO17 (TX)                            */
#define T1_RADIO_TX              1      /* -> ESP32-S3 GPIO18 (RX)                            */

/* ---- Link C: Teensy 4.1 #2 (worker) ----------------------------------------------------- */
/* Serial3. Costs A0 and A1, which is acceptable: the fabric's 64 analog channels arrive
 * through the 4051 tree on a single dedicated ADC pin, so on-board ADC pins are not scarce. */
#define T1_WORKER_SERIAL         Serial3
#define T1_WORKER_RX             15     /* <- Teensy2 pin 1, TX1                              */
#define T1_WORKER_TX             14     /* -> Teensy2 pin 0, RX1                              */

/* ---- Link D: E32R40T display node (ESP32-WROOM-32E) ------------------------------------- */
/* Serial7. Pins 28/29 are the cleanest free pair left: plain digital, no ADC, no alternate
 * peripheral function this build wants, and adjacent so the jumper pair is unambiguous. */
#define T1_HMI_SERIAL            Serial7
#define T1_HMI_RX                28     /* <- E32R40T TX  (which GPIO: SEE README-QUESTIONS)  */
#define T1_HMI_TX                29     /* -> E32R40T RX  (which GPIO: SEE README-QUESTIONS)  */

/* ---- Reserved: optional hardware flow control on the radio link ------------------------- */
/* His master defines these and leaves LINK_USE_FLOW_CONTROL at 0. Nothing else may claim them,
 * or enabling flow control one day silently fights whatever moved in. */
#define T1_RESERVED_RTS          2
#define T1_RESERVED_CTS          3

/* ---- Fabric: SPI1 (LPSPI3) -- the fabric's own bus --------------------------------------- */
/* MISO MUST be moved off its default pin 1 before SPI1.begin(), because pin 1 is the tested
 * radio link's TX. The driver calls SPI1.setMISO(T1_SPI_MISO) first; without that call, SPI1
 * would take over pin 1 and the radio link would go silent with no error reported anywhere. */
#define T1_SPI_PORT              SPI1
#define T1_SPI_MOSI              26
#define T1_SPI_MISO              39     /* ALTERNATE. Costs A15. Requires SPI1.setMISO(39).    */
#define T1_SPI_SCK               27

/* ---- Display: SPI0 (LPSPI4), reserved for the screen alone -------------------------------- */
#define T1_TFT_PORT              SPI
#define T1_TFT_MOSI              11
#define T1_TFT_MISO              12
#define T1_TFT_SCK               13     /* also the onboard LED                                */

/* Pin 13 carries the orange onboard LED and its series resistor, which loads the net slightly.
 * That load now sits on the DISPLAY's clock rather than the fabric's, where it matters far
 * less: the display runs a fixed, proven clock, while the fabric's maximum shift rate is the
 * number this project intends to push and measure. */

/* ---- Fabric: I2C ------------------------------------------------------------------------- */
/* LPI2C1's only pins on a Teensy 4.1. The 2.2k pull-ups to 3.3V live HERE and NOWHERE ELSE.
 * Eight MCP23017 modules each carrying their own 4.7k would parallel to 590 ohm, which at 3.3V
 * demands 5.6 mA from an open-drain pin rated far below that: the bus never reaches a valid
 * low, and it presents as "I2C stopped working when I added the last board". */
#define T1_I2C_SDA               18
#define T1_I2C_SCL               19

/* Expansion buses, unused in v1 but deliberately kept clear. Each adds another 8 MCP23017
 * (128 more IO) with no multiplexer and no address collision with the first bus. */
#define T1_I2C1_SDA              17
#define T1_I2C1_SCL              16
#define T1_I2C2_SDA              25
#define T1_I2C2_SCL              24

/* ---- Fabric: 74HC138 chip-select decoder ------------------------------------------------ */
/* Four pins become eight mutually exclusive, active-low chip selects.
 *
 * A0/A1/A2 are contiguous on purpose. On an 8-channel logic analyser they occupy three adjacent
 * probes and the address reads directly off the capture as a 3-bit number, which turns "did the
 * right chip get selected" from an inference into a measurement.
 *
 * CS_ENABLE drives E3 (active HIGH). It is the global deselect, and it exists because a 74HC138
 * has no "select nothing" address -- every one of the eight addresses asserts an output. It
 * carries a 10k pull-DOWN so that during the ~300 ms before setup() runs, when every Teensy pin
 * is a floating input, the decoder is disabled and no chip select can be spuriously asserted.
 * Without that resistor a floating enable plus a random address is a random CS held low across
 * boot, which is how SD cards and SPI flash get corrupted by a board that "did nothing yet". */
#define T1_CS_A0                 30
#define T1_CS_A1                 31
#define T1_CS_A2                 32
#define T1_CS_ENABLE             33     /* -> 74HC138 pin 6 (E3), 10k pull-DOWN to GND        */

/* ---- Fabric: shift registers ------------------------------------------------------------ */
/* RCLK is what makes a 74HC595 bank atomic: the shift register clocks through 8 (or 8N) states
 * while the output latch holds the previous value, and every output changes on one RCLK edge.
 * Tying RCLK to SRCLK -- a common shortcut -- makes every intermediate shift state appear on
 * the pins, which on a relay or MOSFET bank means briefly energising things that should never
 * have been energised. 10k pull-DOWN so a floating pin at boot cannot latch garbage. */
#define T1_SHIFT_RCLK            36     /* -> all 74HC595 pin 12 (RCLK), 10k pull-DOWN         */

/* /PL LOW takes a simultaneous snapshot of every 74HC165 input in the chain. It must be a
 * separate pin from RCLK, not shared, because a snapshot and a latch are different instants.
 * 10k pull-UP: idle high = not loading, which is the safe state. */
#define T1_SHIFT_PL              37     /* -> all 74HC165 pin 1 (/PL), 10k pull-UP to 3V3      */

/* ---- Fabric: MISO gate -- the fix for the one real hazard of a single SPI bus ------------ */
/*
 * A 74HC165's QH output is a plain push-pull driver. It is NEVER tri-stated. Chip select does
 * not help, because the '165 has no chip select. So the instant a '165 chain is wired to SPI0
 * MISO, it fights every other device that tries to drive that line -- the 2.4in display, an SD
 * card, any module on a CS slot. Two CMOS outputs driving opposite states is a near short
 * across the supply through both output stages: milliamps of shoot-through, corrupted reads,
 * and hardware that gets warm and eventually fails.
 *
 * THE FIX, USING ONLY PARTS HE ALREADY OWNS. There is no 74HC125 in his inventory. A 74HC373
 * does the same job: tie LE (pin 11) permanently HIGH so the latch is transparent, and the
 * device becomes an eight-bit tri-state buffer controlled by /OE (pin 1). Route the '165 chain's
 * QH through one bit of it. He owns fifteen 74HC373.
 *
 * /OE is active LOW and carries a 10k pull-UP to 3.3V, so the default at power-up and during
 * boot is Hi-Z: the '165 chain is disconnected from MISO until firmware deliberately connects
 * it. The safe state is the un-powered state, which is the only kind of safe state worth having.
 */
#define T1_SHIFT_MISO_OE         4      /* -> 74HC373 pin 1 (/OE), 10k pull-UP to 3V3          */

/* ---- Fabric: interrupt and instrumentation ---------------------------------------------- */
/*
 * Every MCP23017 INTA is configured MIRROR=1 + ODR=1, which makes it open-drain, so all of them
 * wire-OR onto this one pin with a single 4.7k pull-up. Any expander with something to say
 * pulls it low; the ISR then reads INTF on each chip to find out which.
 *
 * MIRROR=1 has a trap that wedges the whole mechanism SILENTLY: the interrupt does not clear
 * until BOTH GPIOA and GPIOB have been read. An ISR that reads only the port it cared about
 * leaves INT asserted forever, no further falling edge ever arrives, and interrupts simply stop
 * with no error anywhere. The driver reads both, always, and BENCH_FAULT_IRQ_STORM exists to
 * report the case where the line is still low after a full service.
 */
#define T1_FABRIC_IRQ            34     /* <- all MCP23017 INTA, open-drain, 4.7k pull-UP      */

/*
 * The logic-analyser marker. Firmware toggles this pin at a chosen instant, so a capture has an
 * unambiguous timestamp from inside the code rather than a guess about which edge was "the
 * one". It is the cheapest way to catch an intermittent: arm the analyser on this pin's edge
 * and the capture is centred on the exact moment firmware believed something happened.
 *
 * On a 24 MHz 8-channel analyser -- roughly 8 MSa/s across 8 channels -- one probe out of eight
 * is a real cost. It is still worth it, because without it every intermittent capture is a
 * hunt through milliseconds of traffic for an event with no signature.
 */
#define T1_LA_MARK               35

/* ---- Fabric: analog ---------------------------------------------------------------------- */
/*
 * One ADC pin serves all 64 analog channels. Pin 41 (A17) is chosen because it is at the far
 * end of the header from the SPI clock and the UART pins -- an analog input sitting next to a
 * 30 MHz clock edge picks it up, and this is the one signal in the design where physical
 * placement is part of the specification.
 *
 * The 4051 tree's SELECT lines are NOT native pins. They are driven from the fabric's own
 * 74HC595 control byte (see FABCTL_* below). That is what makes 64 analog channels cost one
 * pin instead of seven.
 */
#define T1_ADC_MUX_IN            41     /* A17. Bus of all eight 74HC4051 Z pins (pin 3)       */

/* ---- 2.4in display on the master -------------------------------------------------------- */
/*
 * UNRESOLVED SPEC, see README-QUESTIONS.md Q1. The BOM says 2.4in SPI TFT (ST7789); he has also
 * called it a "2.4 inch OLED". Those need different drivers and, if it is an I2C OLED, different
 * pins entirely. The pins below are the SPI-TFT case. The display is behind a compile-time
 * switch so the rest of the firmware does not care which is fitted.
 *
 * CS is a NATIVE pin, not a 74HC138 slot. The display is the highest-traffic SPI device in the
 * system and putting it behind the decoder would add the decoder's settle time to every single
 * transaction, for no benefit -- the '138 exists to multiply scarce chip selects, and the
 * display's CS was never scarce.
 */
#define T1_TFT_CS                10
#define T1_TFT_DC                9
#define T1_TFT_RST               6
#define T1_TFT_BL                5      /* PWM-capable: backlight dimming                      */

/* ---- Front-panel controls (optional) ---------------------------------------------------- */
#define T1_ENC_A                 20
#define T1_ENC_B                 21
#define T1_ENC_PUSH              22
#define T1_KEY0                  23

/* ===========================================================================================
 * PART 2 -- THE 74HC595 FABRIC CONTROL BYTE
 * ===========================================================================================
 *
 * Chip 0 of the 74HC595 chain -- the one physically nearest the Teensy -- is reserved. It is not
 * general-purpose output; it is the fabric steering its own analog tree.
 *
 * This is the trick that pays for the whole design. Driving eight 74HC4051 leaves conventionally
 * costs three select pins plus three address pins plus an enable: seven GPIO. Here it costs
 * zero, because those seven signals live in a byte that is already being shifted out.
 *
 * The price is honest and worth stating: changing an analog channel now costs one SPI
 * transaction plus an RCLK pulse -- roughly a microsecond -- instead of three near-instant pin
 * writes. That is irrelevant, because the 74HC4051 needs 5-20 us of settling before its output
 * is trustworthy anyway. The mux is the slow part, not the addressing. Nothing was lost.
 *
 *   bit 0..2   4051 S0/S1/S2   -- which of the 8 channels within the selected leaf
 *   bit 3..5   138-MUX A0/A1/A2 -- which of the 8 leaves
 *   bit 6      138-MUX E3       -- leaf enable, ACTIVE HIGH. 0 = every leaf disabled.
 *   bit 7      spare
 *
 * Bit 6 being a global disable matters for the same reason the chip-select decoder has one:
 * there must exist a state in which NOTHING is connected to the ADC bus. Without it the ADC pin
 * is always tied to some sensor through 150-300 ohm of switch resistance, and there is no way
 * to measure the tree's own leakage or to park the input safely.
 */
#define FABCTL_CHIP_INDEX        0      /* chain position of the control 595 (0 = nearest)     */
#define FABCTL_MUX_SEL_SHIFT     0
#define FABCTL_MUX_SEL_MASK      0x07u
#define FABCTL_MUX_LEAF_SHIFT    3
#define FABCTL_MUX_LEAF_MASK     0x38u
#define FABCTL_MUX_ENABLE_BIT    0x40u  /* 1 = a leaf is enabled                               */
#define FABCTL_SPARE_BIT         0x80u

/* ===========================================================================================
 * PART 3 -- FABRIC SIZING
 * ===========================================================================================
 * These are the compile-time limits of the driver. They are set to what he owns, not to what a
 * datasheet permits, so a build cannot claim capability that no chip in the box provides.
 */
#define FAB_MAX_MCP              8      /* 8 addresses per I2C bus: 0x20-0x27 = 128 IO         */
#define FAB_MAX_595              20     /* he owns 20; chip 0 is the control byte              */
#define FAB_MAX_165              15     /* he owns 15                                          */
#define FAB_MAX_CS_SLOTS         8      /* one 74HC138. Cascade to 64 later without firmware
                                         * change: the slot number simply grows.               */
#define FAB_MAX_MUX_LEAVES       8      /* eight 74HC4051 = 64 analog channels                 */
#define FAB_MAX_PORTS            32     /* logical module ports in the registry                */

/* MCP23017 base address. A2/A1/A0 strapped 000 -> 0x20, through 111 -> 0x27.
 * The strap pins are TTL buffers with NO internal bias. Left floating they are not "low", they
 * are undefined, and the chip answers at an address that can change with temperature or with a
 * hand near the board. Every strap gets a hard 10k to GND or a jumper to 3V3. */
#define FAB_MCP_ADDR_BASE        0x20u
#define FAB_MCP_ADDR_MAX         0x27u

/* This address block collides with PCF8574 expanders and with a number of I2C radio modules.
 * The address map in the repo is the mechanism that keeps that from being discovered at 2 a.m.
 * If a module must live at 0x20-0x27, it goes behind a TCA9548A or on Wire1/Wire2. */

/* ===========================================================================================
 * PART 4 -- I2C AND SPI BUS PARAMETERS
 * =========================================================================================== */

/* The MCP23017 reaches 1.7 MHz only at VDD >= 4.5 V -- every 1.7 MHz row in its Table 1-3 is
 * conditioned "4.5V - 5.5V". At 3.3 V it is a 400 kHz part, full stop.
 *
 * Teensy 4's Wire.setClock() is quantised to exactly three profiles, and the boundaries are
 * blunt: anything below 400000 gives the 100 kHz profile, 400000 through 999999 gives the
 * 400 kHz profile, and 1000000 or more gives 1 MHz. So setClock(200000) and setClock(399999)
 * both silently run at 100 kHz. Asking for a speed that is not one of the three does not fail;
 * it substitutes, and a later timing measurement then has no explanation.
 *
 * TWO THINGS TO KNOW WHEN THE ANALYSER DISAGREES WITH THIS NUMBER:
 *
 * 1. The achieved frequency falls with SCL rise time, because the peripheral inserts latency
 *    proportional to it. Measured mapping: 0 ns rise -> 400.0 kHz, 250 ns -> 363.6 kHz,
 *    500 ns -> 333.3 kHz, 1000 ns -> 285.7 kHz. A capture showing 360 kHz is not a fault, it is
 *    the pull-ups and the bus capacitance being visible. It IS a reason to check the pull-ups.
 *
 * 2. The Teensy 400 kHz profile's SCL low time is 1208 ns, and I2C Fast-mode requires a minimum
 *    of 1300 ns -- a 7.1% shortfall, which the MCP23017 also independently specifies. In
 *    practice real fall and rise times add roughly 90 ns of measured low time and pull it back
 *    over the line. This is stated rather than hidden because it means the margin here is thin,
 *    and it is the first thing to suspect if a long bus starts producing occasional NACKs. */
#define FAB_I2C_HZ               400000u

/* 74HC595 and 74HC165 are both SPI mode 0. The '595 shifts on the RISING edge of SRCLK and the
 * '165 also clocks on the rising edge, so one clock serves both: MOSI feeds the '595 chain and
 * MISO reads the '165 chain, in the same transaction if desired.
 *
 * The datasheet permits far more than 8 MHz at 3.3 V. This is a BREADBOARD limit, not a silicon
 * limit: unterminated jumper wire, several centimetres of loop area per net and a shared clock
 * fanning out to a dozen inputs produce ringing that a datasheet figure taken on a test fixture
 * says nothing about. Start here, prove it on the analyser, then raise it deliberately and
 * re-measure. The number that matters is the one his own capture shows, not this one.
 *
 * The 24 MHz 8-channel analyser samples at roughly 8 MSa/s per channel with all 8 in use, so a
 * clock above about 4 MHz cannot be honestly decoded during capture. FAB_SPI_HZ_CAPTURE is what
 * to run while measuring; FAB_SPI_HZ is what to run in service. Measuring at one speed and
 * shipping at another without saying so is how a "verified" timing claim becomes fiction. */
#define FAB_SPI_HZ               8000000u
#define FAB_SPI_HZ_CAPTURE       2000000u

/* 74HC138 settle time before a chip select may be trusted.
 *
 * This number went through two corrections, and both are worth recording because the value
 * survived while the REASONING behind it changed completely -- which is the difference between
 * a number that is right and a number that is merely lucky.
 *
 * Draft 1 used 100 ns, reasoning that "typical 4.5 V figures are 20-25 ns and CMOS slows as the
 * supply falls". Sound reasoning, wrong number: it interpolated where TI and Nexperia refuse to.
 *
 * Draft 2 used 500 ns, justified by TI's 2.0 V column (225 ns address->Y, 195 ns enable->Y) on
 * the grounds that it was "the only guaranteed bound that exists at 3.3 V". That justification
 * was ALSO wrong, and a fact-check caught it:
 *
 *   TI (SCLS107G) and Nexperia (74HC_HCT138 Rev 10) tabulate only 2.0 / 4.5 / 6.0 V.
 *   onsemi PUBLISHES A FULL GUARANTEED 3.0 V COLUMN for the MC74HC138A, at CL = 50 pF:
 *
 *       address A -> Y          90 ns (-55..+25 C)  /  125 ns (to +85 C)  /  165 ns (to +125 C)
 *       CS1 pin 6, active HIGH  85 ns               /  100 ns             /  125 ns
 *       CS2/CS3 active LOW      90 ns               /  120 ns             /  150 ns
 *       transition time tt      30 ns               /   40 ns             /   55 ns
 *
 * 3.3 V is above 3.0 V and HC propagation delay decreases monotonically with supply, so that
 * column is a valid AND far tighter guaranteed bound: 125 ns at 85 C, not 225 ns.
 *
 * (The same fact-check killed a second figure: TI's Figure 6-1 is not a measured curve, it is
 * two straight segments joining the three tabulated 25 C typicals, and reading 3.3 V off it
 * gives ~41 ns, not the ~25-30 ns that had been claimed -- an error of 35% in the unsafe
 * direction.)
 *
 * 500 ns stands, now with a real justification instead of an arbitrary one: it is 4x the
 * guaranteed worst case at 85 C. It costs nothing, because a chip select changes a few thousand
 * times a second here rather than a few million. Replace it with a measured value from his own
 * capture during bring-up -- that measurement is the whole point of writing the budget down. */
#define FAB_CS_SETTLE_NS         500u

/* 74HC4051 settle before an ADC conversion is trustworthy.
 *
 * Three effects stack here: the switch's own turn-on time (tens of ns), the RC formed by the
 * switch's 150-300 ohm on-resistance against the ADC's sampling capacitor and the bus
 * capacitance of eight paralleled Z pins, and charge injection from the switching itself
 * dumping a small packet of charge onto the node.
 *
 * The first conversion after a channel change is ALWAYS discarded and never averaged. That is
 * not belt-and-braces; charge injection makes the first sample wrong in a way that is
 * repeatable, so averaging it in produces a stable, plausible, wrong number -- the worst kind. */
#define FAB_MUX_SETTLE_US        20u
#define FAB_MUX_DISCARD_FIRST    1

/* Teensy 4.1 ADC. 12 bits is the honest resolution of this converter; asking for more returns
 * numbers with no information in the low bits. Hardware averaging of 4 costs about four
 * conversion times and removes most of the sampling noise. */
#define FAB_ADC_BITS             12
#define FAB_ADC_AVERAGING        4
#define FAB_ADC_FULL_SCALE       4095

/* ===========================================================================================
 * PART 5 -- TEENSY 4.1 #2  (WORKER)
 * ===========================================================================================
 * The worker talks to exactly one thing. Every other pin is free for compute jobs, which is the
 * entire point of having it: a second 600 MHz M7 whose timing nothing else can disturb.
 */
#define T2_MASTER_SERIAL         Serial1
#define T2_MASTER_RX             0      /* <- Teensy1 pin 14, TX3                              */
#define T2_MASTER_TX             1      /* -> Teensy1 pin 15, RX3                              */
#define T2_LA_MARK               35     /* same marker convention as the master                */

/* ===========================================================================================
 * PART 6 -- ESP32-S3  (RADIO)  -- EXISTING, TESTED, DO NOT MOVE
 * ===========================================================================================
 * Chosen originally because they are clear of every trap on an S3: strapping pins (0/3/45/46),
 * native USB (19/20), SPI flash (26-32) and octal PSRAM (33-37) on -N8R8 and -N16R8 modules.
 */
#define ESP_LINK_TX              17     /* -> Teensy1 pin 0, RX1                               */
#define ESP_LINK_RX              18     /* <- Teensy1 pin 1, TX1                               */

/* ===========================================================================================
 * PART 7 -- E32R40T  (4.0in DISPLAY NODE, ESP32-WROOM-32E -- NOT AN S3)
 * ===========================================================================================
 *
 * Onboard peripherals, taken from his own working firmware for this board, not from a guess:
 */
#define HMI_LCD_CS               15
#define HMI_LCD_DC               2
#define HMI_LCD_SCK              14
#define HMI_LCD_MOSI             13
#define HMI_LCD_MISO             12
#define HMI_LCD_BL               27     /* PWM backlight                                       */
#define HMI_TOUCH_CS             33     /* XPT2046, shares the LCD's HSPI bus                  */
#define HMI_TOUCH_IRQ            36     /* input-only pin                                      */
#define HMI_SD_CS                5
#define HMI_SD_MOSI              23
#define HMI_SD_SCK               18
#define HMI_SD_MISO              19
#define HMI_LED_R                22
#define HMI_LED_G                16
#define HMI_LED_B                17
#define HMI_BTN_BOOT             0
#define HMI_AUDIO_EN             4
#define HMI_AUDIO_DAC            26
#define HMI_BAT_ADC              34     /* input-only, divide-by-2                             */

/* Correction to the list above: the LCD's RESET is NOT a GPIO. It is tied to the EN pin and is
 * shared with the ESP32's own reset, so firmware cannot pulse the panel independently. */

/* ===========================================================================================
 * THE LINK CONNECTOR -- ANSWERED, from the vendor schematic
 * ===========================================================================================
 *
 * This board has no 0.1in headers. Every external signal is on a back-side 1.25 mm connector --
 * NOT JST-PH 2.0 mm, so 2.0 mm cables will not mate. There are seven:
 *
 *   P2  "UART"     4P   +5V | GND | TXD0=GPIO1 | RXD0=GPIO3
 *   P3  "SPI"      4P   GPIO23 | GPIO19 | GPIO18 | GPIO21          (no GND pin in the housing)
 *   P4  "I2C"      4P   3.3V | GPIO32 | GPIO25 | GND
 *   JP3 "expand"   2P   GPIO35 | GPIO39                            (both input-only)
 *   JP1 "SPEAKER"  2P   amplifier output, no GPIO
 *   JP2 "BAT"      2P   BAT+ | GND
 *   USB1           Type-C
 *
 * USE P4. Not the connector labelled UART -- P4, the one labelled I2C.
 *
 * WHY NOT P2, THE ONE ACTUALLY LABELLED "UART":
 *   P2 is UART0, wired in hard parallel with the onboard CH340C through only 100 ohm series
 *   resistors (R29/R30). With USB attached, the CH340C drives that net at low impedance while
 *   the Teensy can only reach it through 100 ohm: a Teensy pulling low is divided up to roughly
 *   2.2 V at the ESP32's RX pin, which is above VIH, so the ESP32 never sees the low at all.
 *   The link would appear dead for no visible reason whenever USB was plugged in.
 *   Worse, P2 pin 1 is a BIDIRECTIONAL 5 V node: a P-channel MOSFET (Q5, SL2305) whose gate is
 *   pulled to ground is permanently on, and its body diode conducts back into the board's +5 V
 *   rail. Bonding it to a Teensy 5 V rail ties two supplies together. Never wire P2 pin 1.
 *
 * WHY P4 IS RIGHT:
 *   One 4-pin cable carries 3.3 V, a real GND, and two full bidirectional GPIO. GPIO25 and
 *   GPIO32 are used by NOTHING on this board -- the vendor's own manual lists them as free for
 *   ordinary IO. The classic ESP32's GPIO matrix routes UART1/UART2 to any GPIO, so binding
 *   UART2 to these two pins is legal and costs nothing. LCD, touch, SD, RGB LED, audio and the
 *   USB console are all untouched, so the board can be reflashed and debugged over USB while
 *   the link stays connected.
 *
 * ONE THING NOT TO "FIX": R33 and R34 are 10k pull-ups on GPIO25 and GPIO32, fitted because the
 * vendor intended these as an I2C port. The vendor's manual suggests removing them for generic
 * IO use. DO NOT REMOVE THEM HERE. For a UART they are actively helpful: they hold both lines
 * at the idle-high state before begin() runs and while the cable is unplugged, which prevents
 * a floating line from being read as a spurious start bit.
 *
 * Both ends are 3.3 V push-pull. No level shifter -- one would only add delay and a failure mode.
 */
#define HMI_LINK_TX              25     /* P4 pin 3 -> Teensy1 pin 28 (RX7)                    */
#define HMI_LINK_RX              32     /* P4 pin 2 <- Teensy1 pin 29 (TX7)                    */
#define HMI_LINK_UART_NUM        2      /* UART2, matrix-routed to the pins above              */

/* Also free and brought out, if more signals are ever needed:
 *   GPIO21  on P3 pin 4  -- full bidirectional, the only P3 pin not shared with the SD card
 *   GPIO35  on JP3 pin 1 -- INPUT ONLY, no internal pull-up
 *   GPIO39  on JP3 pin 2 -- INPUT ONLY, no internal pull-up
 * His own earlier free-pin list named 25, 35 and 39 but missed GPIO32 and GPIO21. */

/* ===========================================================================================
 * PART 8 -- LUCKFOX PICO MINI B  (LINUX ORCHESTRATOR)
 * ===========================================================================================
 * Header pin numbers, not GPIO numbers. UART3_M1 is not enabled by default and is not the
 * console -- the console stays on UART2 (header pins 4/5) and must not be disturbed, because it
 * is the only way back in when a UART change goes wrong.
 */
#define LK_UART3_TX_HEADER_PIN   12     /* GPIO1_D0 -> Teensy1 pin 7 (RX2), via 100 ohm        */
#define LK_UART3_RX_HEADER_PIN   13     /* GPIO1_D1 <- Teensy1 pin 8 (TX2), via 100 ohm        */
#define LK_UART3_DEVICE          "/dev/ttyS3"
#define LK_CONSOLE_DEVICE        "/dev/ttyFIQ0"  /* NOT ttyS2 -- see below                     */

/* PUT 100 OHM IN SERIES ON BOTH LINK WIRES. This is a real requirement, not caution.
 *
 * If the Teensy is powered while the Luckfox is not, it drives 3.3 V into a pad whose supply
 * rail is dead. Current then flows through the pad's ESD diode into an unpowered VCCIO6, which
 * can latch the SoC or degrade it slowly -- and it looks like nothing at all until the board
 * stops booting one day. Luckfox fit 100 ohm on their own console lines (R6/R7) for exactly
 * this reason. Copy them.
 *
 * THE MARGIN THAT WILL BITE, IF ANYTHING DOES:
 *      RV1103 VOH min              2.40 V
 *      i.MX RT1062 VIH min         2.31 V  (0.7 x 3.3)
 *      worst case, Luckfox -> Teensy      90 mV
 * It works. But framing errors that are marginal, temperature-dependent, or present in ONE
 * DIRECTION ONLY point here -- not at the baud rate, which is exact (see below).
 *
 * 921600 IS EXACT ON THIS SOC, which is unusual enough to record. The Rockchip 8250_dw fork
 * sets the UART source clock to baud*16 above 230400; 14,745,600 Hz comes out of the 1188 MHz
 * GPLL exactly with m/n = 256/20625, so the 16550 divisor is exactly 1 and the error is 0.000%.
 * If this link misbehaves, the baud rate is not the suspect.
 *
 * THE CONSOLE IS NOT /dev/ttyS2. UART2 is claimed by the Rockchip FIQ debugger
 * (rockchip,serial-id = <2>) and appears as /dev/ttyFIQ0 at 115200 on header pins 4/5. It is a
 * different node and a different iomux register from UART3, so enabling UART3 cannot disturb it.
 *
 * HEADER NUMBERING IS DIP-STYLE, NOT RASPBERRY-PI-STYLE. Pins 1-11 run DOWN the left from the
 * USB-C end; pins 12-22 run UP the right. Pin 12 is bottom-right. Counting the right column
 * downward from the top lands on the SARADC pins instead. */

/* MEASURE BEFORE WIRING. The RV1103 has mixed IO voltage domains: VCCIO1 and the PMU bank are
 * 3.3 V, VCCIO3/4/6 are configurable 1.8 V or 3.3 V, and the MIPI/GPIO7 bank is 1.8 V only. The
 * header also carries a 1.8 V power pin. A 1.8 V pad driving a Teensy input is not a failure --
 * it is worse, it is marginal, and marginal is what produces an intermittent that costs a week.
 * Put a meter on the idle-high level of both UART3 pads before connecting anything. */

/* Luckfox SARADC, if analog is ever taken there: 1.8 V ABSOLUTE MAXIMUM, 10-bit, 2 channels,
 * and SARADC_IN0 is sampled at boot as a strapping pin. Use IN1 only, behind a rail-to-rail
 * buffer and a 10k/12k divider that maps 3.3 V to 1.80 V. Feeding it 3.3 V raw damages it. */

/* ===========================================================================================
 * PART 9 -- COMMON LINK PARAMETERS
 * =========================================================================================== */
#define BENCH_LINK_BAUD          921600u

/* Every link in the stack runs the same baud and the same frame. That is not tidiness, it is
 * what lets one analyser decoder, one Python codec and one parser serve all four links -- and
 * what lets a frame be forwarded between links without re-encoding anything. */

#endif /* BENCH_PINS_H */

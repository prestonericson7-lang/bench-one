# WIRING.md -- every wire, every part, both ends

This is the build sheet. One table per group, three columns that matter: **which Cu header pin**,
**what part the wire passes through**, **which DIMM contact**. Nothing here is inferred from a
picture or a forum post; the sources and how each number was double-checked are at the bottom.

Read [HARDWARE-SAFETY.md](../../../../HARDWARE-SAFETY.md) first. The short version: a DDR3 DIMM is a
1.5 V part and the Cu is a 3.3 V part, and connecting them directly destroys the DIMM.

---

## Before you cut a single wire

**Find pin 1 of each header with a meter.** Each Cu bank is 50 positions in two rows of 25. Every
third position counting from the end of a row is power or ground: 1, 4, 7 ... 25 in the first row and
26, 29, 32 ... 50 in the second. That is exactly why the signal numbers in the tables skip. Put a
meter on continuity between board ground and the position you believe is pin 1, and confirm that the
positions you believe are 4, 7 and 10 are also ground or 3.3 V. **Getting the row orientation
backwards mirrors all 32 signals on that bank**, and it is the one mistake on this page that costs
you a DIMM rather than an afternoon.

**Read the DIMM's SPD EEPROM before you power the DIMM at all.** Five wires, module unpowered
otherwise, and it tells you the real geometry instead of letting you assume it. VDDSPD is contact
236, SDA 238, SCL 118, SA0 117, SA2 119. If the module turns out not to be eight banks of 32768 rows
by 1 KB, the gateware's row and column widths are wrong and nothing below will work.

**Know what you are building.** Eight data lines reach exactly one x8 chip, so the addressable
capacity is 8 banks x 32768 rows x 1 KB = **256 MB**, not whatever is printed on the module label.
The other 56 data lines stay unconnected. The other chips still see every command and will store
nonsense from their floating inputs; that is harmless, because you never read those bytes.

---

## Parts

| Qty | Part | Job |
|---|---|---|
| 2 | TXB0108 on a breakout | the ten bidirectional lines: eight data, two strobe |
| 27 | resistor pair, see below | one divider per output-only line |
| 4 | 100 ohm 1% resistor | the two VREF dividers |
| 2 | 100 nF + 1 uF ceramic | VREF decoupling, mounted at the DIMM contact |
| 1 | 1.5 V supply, 1.5 A | DIMM VDD |
| 1 | 240-pin DIMM socket | or solder to the contacts directly |
| 2 | LED + 470 ohm | indicators, optional |

**Divider values depend on which configuration you build.** Both land the high level at 1.5 V; they
differ only in how fast they settle into roughly 20 pF of module capacitance.

| Configuration | Series (to 3.3 V) | Shunt (to GND) | High level | Source impedance | Settling | Current per line |
|---|---|---|---|---|---|---|
| 6.25 MHz memory -- first light | 1.2 kohm | 1.0 kohm | 1.500 V | 545 ohm | 11 ns | 1.5 mA |
| 15.625 MHz memory -- full | 330 ohm | 270 ohm | 1.485 V | 148 ohm | 3 ns | 5.5 mA |

At 6.25 MHz a half period is 80 ns, so 11 ns of settling is nothing. At 15.625 MHz it is 32 ns, and
the 1.2k/1.0k pair would be eating a third of the window -- use the 330/270 pair there. The fast
option costs 27 x 5.5 mA = **148 mA** out of the 3.3 V rail, which is 0.49 W of resistors; that
current goes to ground through the shunt leg and never enters the 1.5 V rail.

**Resistor dividers are the right answer for output-only lines, not a compromise.** They are stronger
than a TXB0108, they have no direction logic to get confused, and they cannot oscillate. An
SN74LS245 is not an alternative: one supply, it cannot output 1.5 V, and its 2.0 V input threshold
means it cannot read 1.5 V either.

**The TXB0108 is a first-light part, not a full-speed one.** Its datasheet gives 20 to 100 Mbps
depending on rails and tells you to keep any pull resistor above 50 kohm, which is how it says the
output drive is deliberately weak. DDR3 data is double-rate, so the line rate is twice the memory
clock: **12.5 Mbps at 6.25 MHz, inside the rating; 31.25 Mbps at 15.625 MHz, outside the comfortable
part of it.** Build the slow configuration on TXB0108s. For the fast one the part is an
**SN74AVC8T245**, which is direction-controlled and rated down to 1.2 V on both sides -- a
74LVC8T245 will not do, its VCCB floor is 1.65 V. Two of them, direction driven from the FPGA.

---

## The wiring

FPGA header pin in the second column is what you solder to. DIMM contact in the last column is the
other end.

### The ten bidirectional lines -- through a translator

| FPGA signal | Cu header pin | ball | translator channel | DIMM contact | DIMM signal |
|---|---|---|---|---|---|
| `ddr_dq[0]` | **A2** | M1 | U1  A1 <-> B1 | **3** | DQ0 |
| `ddr_dq[1]` | **A3** | L1 | U1  A2 <-> B2 | **4** | DQ1 |
| `ddr_dq[2]` | **A5** | J1 | U1  A3 <-> B3 | **9** | DQ2 |
| `ddr_dq[3]` | **A6** | J3 | U1  A4 <-> B4 | **10** | DQ3 |
| `ddr_dq[4]` | **A8** | G1 | U1  A5 <-> B5 | **122** | DQ4 |
| `ddr_dq[5]` | **A9** | G3 | U1  A6 <-> B6 | **123** | DQ5 |
| `ddr_dq[6]` | **A11** | E1 | U1  A7 <-> B7 | **128** | DQ6 |
| `ddr_dq[7]` | **A12** | D1 | U1  A8 <-> B8 | **129** | DQ7 |
| `ddr_dqs` | **A14** | C1 | U2  A1 <-> B1 | **7** | DQS0 |
| `ddr_dqs_n` | **A15** | B1 | U2  A2 <-> B2 | **6** | DQS0# |

### The twenty-seven output-only lines -- through a divider each

| FPGA signal | Cu header pin | ball | DIMM contact | DIMM signal |
|---|---|---|---|---|
| `ddr_ck_p` | **A17** | D3 | **184** | CK0 |
| `ddr_ck_n` | **A18** | C3 | **185** | CK0# |
| `ddr_a[0]` | **B2** | A6 | **188** | A0 |
| `ddr_a[1]` | **B3** | A7 | **181** | A1 |
| `ddr_a[2]` | **B5** | A10 | **61** | A2 |
| `ddr_a[3]` | **B6** | A11 | **180** | A3 |
| `ddr_a[4]` | **B8** | C9 | **59** | A4 |
| `ddr_a[5]` | **B9** | C10 | **58** | A5 |
| `ddr_a[6]` | **B11** | A12 | **178** | A6 |
| `ddr_a[7]` | **B12** | B14 | **56** | A7 |
| `ddr_a[8]` | **B14** | C14 | **177** | A8 |
| `ddr_a[9]` | **B15** | D14 | **175** | A9 |
| `ddr_a[10]` | **B17** | E14 | **70** | A10/AP |
| `ddr_a[11]` | **B18** | E12 | **55** | A11 |
| `ddr_a[12]` | **B20** | F14 | **174** | A12/BC |
| `ddr_a[13]` | **B21** | G14 | **196** | A13 |
| `ddr_a[14]` | **B23** | H12 | **172** | A14 |
| `ddr_ba[0]` | **B24** | J12 | **71** | BA0 |
| `ddr_ba[1]` | **B27** | H11 | **190** | BA1 |
| `ddr_ba[2]` | **B28** | G11 | **52** | BA2 |
| `ddr_cs_n` | **B30** | G12 | **193** | S0# |
| `ddr_ras_n` | **B31** | F12 | **192** | RAS# |
| `ddr_cas_n` | **B33** | F11 | **74** | CAS# |
| `ddr_we_n` | **B34** | E11 | **73** | WE# |
| `ddr_cke` | **B36** | D12 | **50** | CKE0 |
| `ddr_reset_n` | **B37** | D11 | **168** | RESET# |
| `ddr_odt` | **C2** | M3 | **195** | ODT0 |

### The Teensy link -- 3.3 V both ends, bare wire, no part in between

| FPGA signal | Cu header pin | ball | Teensy 4.1 pin | what it is |
|---|---|---|---|---|
| `qspi_cs_n` | **A42** | H1 | **51** | FlexSPI2 SS1 (the free select) |
| `qspi_sck` | **A43** | H3 | **53** | FlexSPI2 SCLK |
| `qspi_io[0]` | **A45** | K3 | **52** | DATA0 |
| `qspi_io[1]` | **A46** | K4 | **49** | DATA1 |
| `qspi_io[2]` | **A48** | N1 | **50** | DATA2 |
| `qspi_io[3]` | **A49** | P1 | **54** | DATA3 |

### Indicators -- an LED and a 470 ohm resistor to ground, if you want them

| FPGA signal | Cu header pin | ball | meaning |
|---|---|---|---|
| `led_init` | **C39** | P10 | lit once the DIMM has finished initialising |
| `led_act` | **C40** | P9 | flickers on activity, LATCHES ON if a fault was detected |
---

## The wires that go nowhere near the FPGA

These are the ones that get forgotten, and every single one of them makes the module look dead or
behave randomly rather than fail in a way that points at itself.

### The two voltage references are mandatory

DDR3 needs two reference voltages at VDD/2, **0.750 V**, each within 1% DC, with a separate limit on
AC noise. Every input comparator in every chip measures against them. Without them the module does
not respond at all and looks like a dead board.

| Reference | DIMM contact | How |
|---|---|---|
| VREFDQ | **1** | its own 100 ohm / 100 ohm divider off the 1.5 V rail |
| VREFCA | **67** | its own 100 ohm / 100 ohm divider off the 1.5 V rail |

**One independent divider each -- do not share one.** And use **100 ohm pairs, not 1 kohm**: the input
leakage of the chips on the module sags a 1 kohm divider by about 16 mV, which is 2% and out of spec
on its own, where 100 ohm costs 1.6 mV. Each divider draws 7.5 mA. Decouple each reference with
**100 nF plus 1 uF, mounted at the DIMM contact, not back at the divider** -- the 100 nF is the part
that meets the AC noise limit, and it only does that if it is where the noise is.

### The tie-offs

| DIMM contact | Signal | Tie to | Why |
|---|---|---|---|
| **125** | DM0 | **GND** | Write data mask for our byte lane. It is an INPUT. Left floating, writes are masked at random and reads come back as whatever was there before. This is the one that looks like a broken write path. |
| **169** | CKE1 | **GND** | Holds the second rank's clock off, which is what keeps the 1.5 V rail near 0.5 A instead of double. |
| **76** | S1# | **1.5 V** | Chip select is active low, so high deselects the second rank. The only line on this page that goes to the rail rather than to ground. |
| **77** | ODT1 | **GND** | On-die termination is unsupported with the DLL disabled. |

Contacts **120** and **240** are VTT. Leave them unconnected; terminating a hand-wired bus at these
speeds buys nothing and costs a 0.75 V supply that can source and sink. Contact **167** is TEST, for
bus analysers. Leave it. Contacts **2, 5, 8, 11 ...** and their equivalents on the back are VSS --
connect as many as you conveniently can to your ground plane, not just one.

### Power

About **0.5 A at 1.5 V** with the second rank held off by grounding CKE1; standby is 32 to 55 mA per
chip and refresh adds roughly 12 mA. **Size the supply for 1.5 A.** Bring **1.5 V up before or
together with 3.3 V**, because the output dividers otherwise inject current into unpowered inputs.

---

## Why the memory clock is 6.25 or 15.625 MHz and not something rounder

The fabric clock is not the board oscillator. The Cu's oscillator is 100 MHz and **this design does
not close at 100 MHz** -- measured across eight placer seeds it lands between 81.5 and 97.4 MHz, so
the best seed nearly makes it and a bitstream that works because of a lucky seed is not a bitstream.
sys_clk therefore comes from the PLL and the oscillator is only its reference.

| Build | sys_clk | CK_DIV | Memory clock | DRAM raw | Link | Effective at the Teensy | Worst-seed timing margin |
|---|---|---|---|---|---|---|---|
| `cfgA` first light | 50 MHz | 8 | 6.25 MHz | 12.5 MB/s | 1 line at 49.5 MHz = 6.19 MB/s | 5.4 MB/s | 1.75x |
| `cfgB` full | 62.5 MHz | 4 | 15.625 MHz | 31.25 MB/s | 4 lines at 49.5 MHz = 24.75 MB/s | 15.2 MB/s | 1.47x |

**The rate rule decides both rows.** FlexSPI cannot be stalled once a read has started, so the DRAM
has to fill the FPGA's buffer faster than the link drains it, with margin for the fixed overhead of
every DRAM request -- take 15%. DRAM delivers memory-clock x 2 MB/s on eight lines. The link delivers
SCLK/2 MB/s on four lines and SCLK/8 on one. cfgA has a 2.0x ratio and cfgB a 1.26x ratio. This is
also why **12.5 MHz is unreachable**: it gives 25 MB/s of DRAM, and the slowest four-line link
FlexSPI2 can produce is 24.75 MB/s, which is 1% of margin rather than 15%.

**Effective rate at the Teensy is not the link rate.** Reading and unpacking do not overlap on a
Cortex-M7 -- the same core does both -- so the two rates add as reciprocals. Unpacking measures
39.3 MB/s, which is where 24.75 becomes 15.2.

**So be clear about what this buys.** Measured PSRAM on the same Teensy is 18.2 MB/s. cfgB is 15.2.
**The DDR3 bank is not faster than PSRAM and was never going to be.** It is worth building for
capacity: 256 MB against 8 MB, a 32x difference, with the bandwidth roughly unchanged.

---

## Bring-up order

Do these in order and stop at the first one that fails. Every step after a failed one will mislead
you.

1. **DIMM unpowered, 3.3 V only.** Read the SPD EEPROM over its five wires. Confirm the geometry.
2. **Dividers and translators built, DIMM still out of the socket.** Power 3.3 V. Measure at the
   socket: every divider output should sit at 1.50 V or 0 V depending on what the FPGA is driving,
   and nothing anywhere should read above 1.6 V. **Check all 27.** One mis-stuffed resistor here is a
   dead DIMM later.
3. **VREF only.** Power 1.5 V with the DIMM still out. Contact 1 and contact 67 should each read
   0.750 V within 8 mV. Measure them separately -- this is where a shared divider shows itself.
4. **DIMM in. 1.5 V up first, then 3.3 V.** `led_init` should light within about a millisecond of
   reset being released. If it never lights, the controller is stuck in its initialisation sequence
   and the fault is in the command group, not the data group.
5. **Teensy connected, run the identity read.** The sketch reads command `0x9F` and expects
   `0x08805D9D` for cfgA or `0x04805D9D` for cfgB -- the top byte is CK_DIV, so a mismatch tells you
   the bitstream and the sketch disagree before any data is at risk.
6. **Run the calibration sweep.** The sketch walks read latency against sub-clock sample phase and
   prints a grid. Pick the middle of the widest clean run; the sketch does this for you. If there is
   no clean run at all, the data group is wrong -- and the most likely single cause is DQS0# not being
   driven, which leaves the module's write comparator with no reference.
7. **Then the data tests.** Round trip, a second block, the first block surviving the second, an
   unaligned read, and a 40 us hold to prove refresh is carrying the array.

---

## Flashing

The Cu has a USB-C programmer on board. No external programmer, no JTAG adapter, nothing else to buy.

```bash
openFPGALoader -b cu build/cfgA.bin
```

`-b cu` is the board profile, and it is the whole trick -- it knows the Cu's FTDI interface and the
iCE40's configuration protocol. `iceprog` also works on some revisions but is the wrong default here.
Configuration is volatile: the bitstream is gone at power-off unless you add `-f` to write the flash,
which is what you want once the thing works.

Build the bitstreams first:

```bash
./build.sh
```

That runs the simulations before it runs the tools, so a broken design never reaches a bitstream.

---

## Sources, and how each number was checked

Two independent sources for anything that gets soldered, because one is not enough.

| What | Source | Second check |
|---|---|---|
| FPGA ball designators | Alchitry Cu V2 schematic rev v14 -- https://cdn.alchitry.com/docs/Cu-V2/CuSchematic.pdf | the toolchain's own `cb132` ball table in `share/icebox/chipdb-8k.txt` |
| Header pin to ball | Alchitry's pin converter source, read as source: `AlchitryCuPinConverter.java` | Alchitry's `io.acf` composed with a third party's independently hand-converted `alchitry-io.pcf`; agrees on all 65 entries, zero disagreements |
| DIMM contacts | 240-pin UDIMM pin configuration table, JEDEC, from Samsung 2 Gb B-die UDIMM datasheet rev 1.03 section 4.0 | identical table in the Micron MT8JTF and Advantech AQD-D3L4GN16 datasheets |
| Clock-capable balls | `chipdb-8k.txt`: the `.gbufin` tile list crossed with `padin_glb_netwk`'s IO index and the `cb132` ball table | published Alchitry Cu constraint files use `set_io clk P7` and `set_io rst P8`, which matches GBIN5 and GBIN0 |
| DLL-disabled timing, tCK range, CL/CWL | DDR3 SDRAM datasheet, DLL-disable section; note on page 115 that tDQSCK may exceed tCK | the device model in `ddr3_model.v` enforces these and rejects a controller that violates them |
| TXB0108 and divider limits | TXB0108 datasheet: 20 to 100 Mbps, pull resistors above 50 kohm | settling computed against 20 pF and checked against the half period at each memory clock |

**What has NOT been checked on hardware:** all of it. Nothing on this page has touched a DIMM. The
gateware passes an end-to-end simulation against a DDR3 device model that enforces the real timing
numbers, both bitstreams place and route with margin on every seed tried, and every number above
traces to a datasheet -- but the first time this is powered is the first time it is tested.

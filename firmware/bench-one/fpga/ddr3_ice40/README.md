# DDR3 on a microcontroller, through a small FPGA

Give a Teensy 4.1 **256 MB of DDR3** instead of 16 MB of PSRAM, by putting an iCE40 between them
that pretends to be a quad SPI RAM. The DIMM is a scrapped desktop stick. The FPGA does no
arithmetic, so every number measured through it is the microcontroller's own.

Nothing here has run on hardware yet. Everything here is simulated, synthesised, and traced to a
datasheet page. Where a number is a calculation rather than a measurement, it says so.

---

## Why you would do this

A Teensy 4.1 tops out at 16 MB of external RAM, in two 8 MB PSRAM chips, and the second footprint is
usually empty. A DDR3 stick from a dead laptop holds 4 GB and costs nothing. The gap is that DDR3 is
a 1.5 V source-synchronous bus with a 200-page initialisation ritual, and a microcontroller has no
memory controller that can speak it.

An FPGA can. Not a fast one, and not many gates: this uses **1210 of an iCE40-HX8K's 7680 LUTs, 16%,
and 4 of its 16 block RAMs.** The whole design fits in a $30 board.

The result is not a speed win. It is a **capacity** win at roughly the same speed, and that is the
interesting trade: the same processor, the same clock, sixteen times the memory.

---

## What the numbers actually are

Three ceilings stack, and the lowest one wins. Knowing which is which saves weeks.

| | rate | where it comes from |
|---|---|---|
| Teensy external bus | **66.5 MB/s** | FlexSPI2, four data lines, 133 MHz single rate. RT1060 datasheet table 38. |
| DDR3 at 25 MHz, 8 lines | **50 MB/s** | 8 bytes per BL8 burst, one burst per 4 memory clocks. |
| Teensy nibble unpack | **39.3 MB/s** | measured on hardware, earlier in this project |
| Onboard PSRAM, for comparison | **33.9 MB/s** | measured on hardware |

Reading and unpacking **add** on a CPU; only fabric overlaps them. So effective throughput is
`1/(1/read + 1/unpack)`, which predicts the measured PSRAM figure of 18.2 MB/s exactly, and that is
why the rest of this table is worth believing:

| configuration | raw read | effective | vs PSRAM |
|---|---|---|---|
| PSRAM, 16 MB | 33.9 | 18.2 | 1.00× |
| this bridge, 12.5 MHz memory | 25.0 | 15.3 | 0.84× |
| this bridge, 25 MHz memory | 50.0 | 22.0 | 1.21× |
| bus-limited ceiling | 66.5 | 24.7 | 1.36× |
| if the link were infinite | — | 39.3 | 2.16× |

**First light is slower than the PSRAM it replaces.** That is expected and fine: it is proving
256 MB works at all. And note where the curve flattens — past about 66 MB/s the processor's own
nibble unpacking is the wall, so more bus speed buys nothing. The next real win after that is in the
unpack kernel, not the memory.

---

## Why 256 MB and not 4 GB

A DIMM rank is eight x8 chips sharing one address and command bus, each supplying eight of the
sixty-four data bits. Wire eight data lines and you are talking to **one chip**: 8 banks × 32768 rows
× 1 KB = 256 MB. The other 1.75 GB of a 4 GB stick sits behind the other fifty-six data lines.

This is not the disappointment it sounds like. The Teensy's memory-mapped window for this peripheral
is **240 MB** (0x7000_0000 to 0x7EFF_FFFF, reference manual page 35), so one chip very nearly fills
the aperture exactly. Going wider needs more translators, not more address lines.

---

## The three ideas that make it work

### 1. DDR3 has a documented slow mode

Everyone assumes DDR3 must run at hundreds of megahertz. It has a **DLL disable mode**, and Micron's
2 Gb datasheet specifies `tCK(DLL_DIS)` from **8 ns to 7800 ns** — 125 MHz down to 128 kHz. Running
at 25 MHz is not a trick or an overclock in reverse; it is the middle of a published window.

Every timing DDR3 quotes in nanoseconds (tRCD 13.75, tRP 13.75, tRAS 35, tRFC 160) is a **minimum**,
so a slow clock satisfies them in one or two cycles and the numbers that make DDR3 hard stop
applying. One thing does not relax: refresh is a wall-clock deadline, 8192 commands every 64 ms.

The mode costs you: only CL=6 and CWL=6 exist, on-die termination is unavailable, and Micron states
plainly that it does not warrant normal-mode timings or functionality there.

### 2. The strobe cannot be used to latch reads

Datasheet page 115, in substance: *with the DLL disabled, the value of tDQSCK could be larger than
tCK*. The strobe a DDR3 chip returns can arrive more than a whole clock after you expect it, and the
datasheet declines to bound it.

So this controller **does not latch read data on DQS**. It samples at a fixed offset from its own
clock, and that offset is the one number in the design that can only be found on real hardware. It is
settable at runtime (see below) precisely so finding it does not cost a rebuild per attempt.

Writes are the opposite case: the chip has no choice but to capture write data on the strobe *you*
generate, so DQS is driven out with a preamble and the data is centred on its edges. Note DQS is a
**differential pair** — drive only the true half and the chip's write receiver, a comparator, has no
reference and the data lands at random.

### 3. Pretend to be a PSRAM, so there is no driver

The Teensy already has a memory-mapped QSPI peripheral with a spare chip select. If the FPGA answers
the same command set a PSRAM does, the DIMM appears as ordinary pointers and the AHB bus does the
fetching — **the processor spends its cycles on arithmetic instead of moving bytes.** That is why
this beats bit-banged GPIO, which would be comparable on paper and would burn every cycle the real
work needs.

The hard part is that FlexSPI **cannot be stalled.** Once the dummy cycles end it clocks data out
relentlessly, and a DDR3 random read is slower than the six dummy cycles a stock PSRAM sequence
leaves. Two things close the gap, and the sketch sets both: a much longer dummy window (the LUT field
is 8 bits, so up to 255 cycles), and keeping the link clock no faster than the memory can supply.

---

## Files

| file | what it is |
|---|---|
| `ddr3_ctrl.v` | the DDR3 controller: initialisation, mode registers, refresh, streaming bursts |
| `qspi_slave.v` | the Teensy-facing side: impersonates a quad SPI RAM |
| `ddr3_bridge.v` | moves bytes between the buffer and DRAM, and nothing else |
| `ddr3_top.v` | top level: buffers, tristates, clock-domain crossing |
| `ddr3_model.v` | a DDR3 device that **checks its master** and fails loudly. Simulation only |
| `tb_ddr3.v` | controller against that model: init, round trip, refresh, read calibration |
| `tb_chain.v` | the whole path, with a FlexSPI-accurate master driving it |
| `alchitry_cu.pcf.template` | pin constraints, with the ball names deliberately left blank |
| `build.sh` | runs every check in order and stops at the first failure |

The Teensy side is one sketch: `../../tests/ddr3_bridge_teensy/`.

---

## Building and checking it

Install an [OSS CAD Suite](https://github.com/YosysHQ/oss-cad-suite-build) release, then:

```bash
OSS_CAD=/path/to/oss-cad-suite sh build.sh
```

That runs five things and stops at the first failure:

1. **The controller against a checking model.** The model enforces mode register order, the DLL
   disable bit, CL and CWL, Rtt being zero, tRCD, tRP, and the refresh debt limit. It refuses column
   commands until it has seen a complete initialisation.
2. **The read calibration sweep.** The model is told to return data 10 to 14 half-clocks late and the
   controller must be tunable to catch it every time. This is what replaces DQS.
3. **Memory clock limits.** 25 MHz passes; 50 MHz does not, and the reason is in the notes below.
4. **Synthesis for an iCE40-HX8K**, reporting utilisation.
5. **Place and route**, skipped until you supply the pin file.

### The two things you must supply

**Pin constraints.** `alchitry_cu.pcf.template` lists every signal with the DIMM contact it goes to,
and leaves the FPGA ball names as `PLACEHOLDER`. They are not guessed on purpose: a wrong ball on a
data line shorts a 1.5 V chip to a 3.3 V driver. Copy them from your board vendor's own constraint
file.

**A working `nextpnr`.** Some OSS CAD Suite extracts ship without the embedded Python standard
library, and `nextpnr-ice40` then aborts at startup with *failed to get the Python codec of the
filesystem encoding*. No environment variable fixes it; the files are absent. Reinstall from a
complete archive or install nextpnr separately. Simulation and synthesis are unaffected.

---

## Wiring it

All 38 DDR3 lines need level translation between 1.5 V and the FPGA's 3.3 V, and there is a
**mandatory piece people forget: VREF.** Two reference voltages at 0.750 V on contacts 1 and 67,
within ±1% of half VDD. Every input comparator on every chip measures against them. Without them the
module does not respond and looks dead.

Use **100 Ω divider pairs, not 1 kΩ**: the input leakage of sixteen chips sags a 1 kΩ divider by
about 16 mV, which is 2% and out of spec on its own.

The full bench sheet — rails, both VREF dividers, divider values for the 26 output-only lines,
the six tie-offs, decoupling, the verified contact list, and a bring-up order with a meter check at
each step — is here:

**https://claude.ai/code/artifact/d39d40f3-1e30-4302-9495-690aafe3a355**

Two part-selection findings worth knowing before you order anything:

- **A TXB0108 cannot carry DDR3 data at 25 MHz.** Its datasheet gives 20 to 100 Mbps depending on
  rails and instructs that any pull resistor exceed 50 kΩ, which is how it tells you the drive is
  weak. DDR3 data is double-rate, so 25 MHz means 50 Mbps. At a 6.25 MHz memory clock it is
  12.5 Mbps and comfortably inside — so a TXB0108 is a legitimate **first-light** part and not a
  full-speed one.
- **An SN74LS245 is not a level translator.** One supply, cannot output 1.5 V, and a 2.0 V input
  threshold so it cannot read 1.5 V either.

For the output-only lines, plain resistor dividers are the **right** answer rather than a
compromise: stronger than a weak-drive translator, no direction logic to confuse, and they cannot
oscillate.

---

## Bring-up

Flash the sketch with no DIMM wired first. It will report that the FPGA did not answer, which
confirms the sketch runs and the bus is alive before any 1.5 V part is at risk.

Then set `MEM_MHZ` in the sketch to match the `CK_DIV` the gateware was built with. You do not have
to get this right by memory — the gateware reports its own divider in the top byte of its identity
word and the sketch refuses to run on a mismatch, because disagreeing changes the dummy-cycle count
and produces fast, confident, wrong data that reads exactly like a wiring fault.

| `MEM_MHZ` | gateware `CK_DIV` | memory clock | data line rate | use |
|---|---|---|---|---|
| 6 | 16 | 6.25 MHz | 12.5 Mb/s | first light with weak translators |
| 12 | 8 | 12.5 MHz | 25 Mb/s | matched to the link's 49.5 MHz floor |
| 25 | 4 | 25 MHz | 50 Mb/s | full speed, needs proper translators |

The sketch then **calibrates itself.** It writes a pattern, sweeps every read latency and sample
offset, and prints a grid of which ones read back clean:

```
          sample:  0  1  2  3  4  5  6  7
    latency  5:    X  X  X  X  X  X  X  X
    latency  6:    X  .  .  .  .  X  X  X
    latency  7:    X  X  X  X  X  X  X  X
```

It picks the **middle** of the widest clean run, not the first entry, because the centre of a window
has margin on both sides and margin is the entire point of calibrating. If nothing passes, the output
says what that means: with a successful identity read the QSPI link is fine and the fault is on the
DDR3 side, starting with VREF.

After that it runs a memory test, holds for five seconds so that only refresh can be keeping the data
alive, re-tests, and reports both the raw read rate and the read-plus-unpack rate with a note saying
whether the limit was the link or the memory.

---

## Notes that cost real time to learn

**Read latency is `AL + CL - 1 + 1`, not the datasheet's figure.** The datasheet measures from the
edge on which the *device* latches a command. This controller registers commands on the falling edge
to buy half a period of setup, so the device sees them one clock later than they are issued
internally. Using the datasheet number directly samples one clock early and returns the previous
burst's last two bytes in front of every word — a very convincing wrong answer.

**25 MHz is the ceiling for a single-edge design.** Write data has to sit a quarter period either
side of the strobe edge that captures it. At a divider of 2 the quarter point collapses onto the edge
itself and writes fail. The way past it is a **wider bus, not a faster clock**: sixteen data lines at
25 MHz doubles throughput with the edge rate unchanged, which is far kinder to hand wiring than
doubling the clock.

**`req_ready` must mean "accepting now", not "available".** Requests are latched only on a memory
clock edge, which is one fabric cycle in four. An unqualified ready signal is true for three cycles
on which nothing can be accepted, and a requester that drops its valid on seeing ready loses the
request three times in four. The failure is silent and total.

**A flip-flop has one asynchronous reset.** Listing both chip select and reset as edge events is
legal Verilog and unsynthesisable. The registers that must *survive* chip select — the quad-mode
flag, the window base, the calibration, the write byte count — need their own blocks.

**Cross clock domains with a toggle, not a pulse.** A one-cycle pulse handed from the SPI clock to
the FPGA clock can be missed or latched metastable. The symptom would be a calibration sweep that
occasionally lies, which sends you hunting hardware that is fine.

**`cell` is a reserved word in Verilog-2001**, used in configuration blocks. A function named `cell`
produces a syntax error on its own declaration line with no hint why.

---

## What is deliberately not built

**Read-ahead.** A prefetch buffer that fetched the next chunk while the current one was being clocked
out would decouple link speed from memory speed entirely. It does not exist. The `prefetch_en` bit is
carried through the config command and reaches the bridge, and nothing acts on it. Instead the link is
kept no faster than the memory, and where it cannot be slowed far enough — 49.5 MHz is this
peripheral's floor — each transaction is shortened so the dummy window covers the whole fetch.

**ZQ calibration**, which trims output impedance and termination. Nothing is terminated at this speed
and the reset defaults are adequate.

**Bank interleaving, write levelling, and the second rank.** One rank, one chip's data width, one
bank open at a time.

---

## Licence and provenance

Every register value, timing and contact number traces to a datasheet page, named in the comment
beside it. The two primary sources are Micron's 2 Gb DDR3 datasheet and NXP's i.MX RT1060 reference
manual and datasheet. Pin mappings were checked against the Teensy core's own tables rather than
against any summary, after a pin number taken from a search result once nearly put 3.3 V on a data
line.

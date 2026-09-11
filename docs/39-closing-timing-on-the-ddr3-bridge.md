# 39 — Closing timing on the DDR3 bridge, and the ceiling nobody predicted

Date: 2026-09-11. Status: **simulated, placed, routed and packed. Still never on hardware.**

This is the document that corrects [38](38-ddr3-on-a-microcontroller.md). Doc 38 was written without
ever having run place-and-route, because the toolchain could not. Once it could, the headline number
changed, and so did which of the four ceilings actually binds.

---

## The answer, first

**The iCE40 fabric is the binding constraint on a DDR3 bank behind a Teensy.** Not the DRAM, not the
level shifting, not the microcontroller. The gateware will not clock at the Alchitry Cu's 100 MHz
oscillator, which caps the memory clock at 15.625 MHz, which caps the link at 24.75 MB/s, which lands
the whole thing **below the PSRAM it was meant to beat.**

| | |
|---|---|
| `cfgB`, the fast build | 15.2 MB/s effective |
| PSRAM, measured | 18.2 MB/s effective |

So the DDR3 bank is worth building for one reason: **256 MB against 8 MB, at roughly unchanged
bandwidth.** Thirty-two times the capacity, 0.84× the throughput. Stated plainly because the earlier
version of this claim was 1.07× and it was wrong.

---

## How the toolchain was broken, which is why this was never known

Two separate defects in one OSS CAD Suite extract, which together hid the whole back half of the flow:

- `nextpnr-ice40` aborted at startup with *failed to get the Python codec of the filesystem encoding*.
  The bundled Python standard library was absent. No `python311.zip`, and `lib/python3.11` held only
  `site-packages`. No environment variable fixes that; the files were not there.
- **`icepack`, `iceprog`, `icetime` and `icebram` were missing entirely.** Without `icepack` there is
  no path from a routed design to a bitstream.

Synthesis worked throughout, which is exactly why this went unnoticed for a day. Synthesis reports
utilisation and says nothing about timing or packing. **Check that the whole flow exists before
believing any part of it works.**

Fixed by extracting release `2026-09-11` over the old tree. Several published releases have shipped
with this defect, so the fix is a different dated release, not debugging.

---

## Fmax, and why a passing run proves nothing

First routed result: **61.20 MHz against a 100 MHz requirement.** Then, across eight placer seeds on
the same netlist:

| seed | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Fmax, MHz | 93.19 | 88.48 | 86.39 | 86.81 | 86.69 | 89.19 | **97.43** | 81.51 |

A 16 MHz spread from nothing but the seed. Seed 7 gets within 2.6% of 100 MHz, which is the trap: one
run would have looked like a pass waiting for a little polish. **A bitstream that closes because of a
lucky seed is not a bitstream**, and the build script now routes every configuration on four seeds and
fails if any of them misses.

---

## The four logic cuts

Each was found by reading the critical path report, not by guessing. The pattern in all four is the
same: **a combinational expression that crossed from one module to another, feeding arithmetic on the
far side.** On an HX8K that is routing-dominated — the worst path measured 3.04 ns of logic against
8.65 ns of routing — so the fix is always to put a register at the crossing, never to simplify the
logic.

**1. Chunk sizing was three carry chains in series.** `128 - burst_index`, then two comparisons, all
combinational off `addr` and `need`. 16.3 ns on its own. Split into a registered `to_row_end_r`, then
one comparison between two registered values alongside two against constants, which synthesise in
parallel. A new state `B_RD_CALC` holds for four cycles while it settles, which costs 1.6% of a chunk
transfer and in practice nothing, because the wait state after it is always longer. **61 → 82 MHz.**

**2. `req_ready` was combinational across the die.** It meant "I am accepting on this cycle", which is
correct and was itself an earlier bug fix, but it was `(st == S_IDLE) && (ref_owed == 0) && !in_init
&& ck_rise` feeding the bridge's request arithmetic. A request can only ever be accepted on a memory
clock rising edge, and `phase` is a free-running counter, so that cycle is **predictable one cycle
ahead**. Registered as `phase == CK_DIV-1`. It cannot double-accept because `CK_DIV` is never below 4.

**3. `init_done` and `busy` were decodes of the controller state leaving the module.** Both registered.
Neither is timing critical in meaning: `init_done` rises once, about 700 µs after power-on, and never
falls. **82 → 90 MHz.**

**4. `phase == rd_sample_r` was a register-against-register comparison** sitting in the middle of the
read-return path, which then crosses to the bridge. Predicted a cycle ahead as `rd_at_sample`.

Also: `need_next` and `addr_next` are now precomputed, so the accept cycle only loads registers rather
than computing a subtraction at the end of a long path.

After all four, worst seed reached **87.50 MHz**. Still short of 100.

---

## The PLL, and why the memory clock is 6.25 or 15.625 MHz

`sys_clk` now comes from `SB_PLL40_CORE` and the 100 MHz oscillator is only its reference. `SB_PLL40_CORE`
rather than `SB_PLL40_PAD` because the oscillator lands on P7, which this design already uses as an
ordinary clock input, so the reference is taken from fabric. The extra jitter is irrelevant when the
memory clock is a further division by four and the DIMM accepts anything from 8 ns to 7.8 µs.

Both settings use `DIVR = 0` so the phase detector runs at the full 100 MHz:

| build | DIVF | sys_clk | CK_DIV | memory | DRAM raw | link | worst-seed margin |
|---|---|---|---|---|---|---|---|
| `cfgA` | 7 | 50 MHz | 8 | 6.25 MHz | 12.5 MB/s | 1 line, 6.19 MB/s | 87.50 / 50 = **1.75×** |
| `cfgB` | 9 | 62.5 MHz | 4 | 15.625 MHz | 31.25 MB/s | 4 lines, 24.75 MB/s | 91.78 / 62.5 = **1.47×** |

`CK_DIV` cannot go below 4: write data has to sit a quarter period either side of a strobe edge, and
that position only exists if a quarter of a memory period is a whole number of fabric clocks. So
15.625 MHz is not a choice, it is 62.5 over 4, and 62.5 is what the fabric will hold with margin.

Both configurations pass the full end-to-end chain against the checking device model, and both route on
every seed tried. Utilisation is unchanged by any of this: 1412 and 1521 LUT4s of 7680, four block RAMs
of 32, 47 I/O of 95, one PLL.

**The consolation is that dropping from 25 to 15.625 MHz costs almost nothing.** The link, not the
memory, was already the limit. 25 MHz memory behind a reachable link would have given 19.5 MB/s
effective against 15.2 — a 28% loss for a configuration that cannot be built at all.

---

## What the wiring turned out to be missing

Writing the build sheet properly meant reading the JEDEC 240-pin table out of a datasheet rather than
trusting the contact list already written down. That turned up a real omission.

**DM0, contact 125, was missing from the repository's contact list.** It is the write data mask for the
byte lane being used, and it is a module **input**. Left floating, writes are masked at random and
reads return whatever was there before. The failure presents as a broken write path, which is the last
place anyone would look.

It had been captured once, in the published bench sheet's tie-off table, and then lost when the list
was carried into the source comments. That is the more useful lesson: the failure was not ignorance, it
was a fact living in one place and not the other. The wiring sheet is now generated from the pin file
itself, so the two cannot drift again.

Three more tie-offs in the same category, all inputs, all silent when floating:

| contact | signal | tie to |
|---|---|---|
| **125** | DM0 | GND |
| 169 | CKE1 | GND |
| 76 | S1# | **1.5 V** |
| 77 | ODT1 | GND |

---

## Two methodological notes worth keeping

**A ball designator is not a solderable thing.** The pin file names FPGA balls; a person holding an
iron needs a numbered header pin. Giving only the ball and the DIMM contact leaves out the one number
actually required. The build sheet now carries all three per signal, plus the part in between.

**Use two independent sources for anything that gets soldered.** The ball-to-header-pin map came from
Alchitry's own pin converter source, read as source rather than as a summary of source. It was then
checked a second way, by composing their `io.acf` with a third party's independently hand-converted
`alchitry-io.pcf`: **65 entries, zero disagreements.** Clock-capable balls came out of the toolchain's
own device database rather than any document, which revealed that only six of the eight global pads are
bonded on CB132 and only four of those reach a header. `qspi_sck` now sits on one of them.

---

## Still not measured

Everything. No DIMM has been powered. The gateware passes simulation against a model that enforces real
DDR3 timing and refuses to answer a controller that violates it, both bitstreams exist, and every
number here traces to a datasheet or to a tool's own report. First power-up remains the first test.

The build sheet is `firmware/bench-one/fpga/ddr3_ice40/WIRING.md`. Bring-up order is in it, and step
one reads the module's identity EEPROM with the DIMM otherwise unpowered.

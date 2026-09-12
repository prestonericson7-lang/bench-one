# 44 — 48 MB from one moved wire, and the pin map verified from primary sources

Date: 2026-09-11. Status: **measured on hardware** for the fault and the detector; the repair itself is
one soldering change and is not yet made.

Document 40 recorded that only 8 MB of the 48 wired is usable, because the 74LVC138A decoder's enable
shares a wire with the onboard chip's chip select so both answer at once, and concluded the repair was
to desolder the onboard chip or lift its pin 1. **Neither is necessary.** The decoder enable does not
need the chip gone; it needs a pin of its own.

---

## The repair

Move one wire: the decoder's enable, currently on the CS0 pad (Teensy pin 48), goes to **pin 5**.

| | decoder enable | CS0 | selects |
|---|---|---|---|
| a breadboard bank | asserted, address on A/B/C | held high | one of five, alone |
| the onboard chip | held high | asserted | the onboard chip, alone |

Nothing is desoldered, no pad is at risk, and the usable capacity goes from 8 MB to **48 MB**.

### Why pin 5 specifically

`teensy_pinmap` asks the core for the port and bit of every pin rather than reading them off the
pinout card, and reports that pin 5 is free and is **GPIO9 bit 8** — the same register as the clock
and the four data lines.

That matters more than it looks. The bus driver writes GPIO9 whole, once per clock edge. A chip select
in a different register would cost a second store on every burst boundary, and would be a second
thing to get wrong in exactly the way the decoder's *address* pins were got wrong once already: they
live in GPIO9 too, and writing the register whole with a stale idle value silently reset the bank on
every nibble.

The only other free GPIO9 pins are 33 (bit 7) and 29 (bit 31). Either would work; pin 5 is the lowest
and is on the board edge.

### Why the enable must be active low, deduced rather than assumed

It is currently driven by a chip select, which is active low, and all six chips *do* answer an
identity read. Had the enable been wired to the decoder's active-high G1 input, asserting CS0 would
have **disabled** the decoder and no breadboard bank could ever have responded to anything. So it is
on G2A or G2B, and pin 5 idles high and asserts low.

---

## The detector, and what it says today

`psram_bank6` writes a **different** pattern to each of the six banks, all six before any is read,
then reads them all back. Independent banks each return their own. Banks sharing a select return
whatever the last chip written holds.

It tries the repaired wiring first and falls back, so it is useful on both sides of the change — and
will report 48 MB without being edited once the wire moves.

| bank | 0 | 1 | 2 | 3 | 4 | 5 (onboard) |
|---|---|---|---|---|---|---|
| wrong of 65536, split mode attempted | 65277 | 65280 | 65279 | 65281 | 65281 | **0** |
| wrong of 65536, legacy mode | 65536 | 65536 | 65536 | 65536 | 65536 | **0** |

One bank of six, as expected. Two details in those rows are worth reading rather than skipping:

**The split-mode rows are not 65536, they are about 65280.** That is 256 bytes matching out of 65536,
which is 0.39%, which is exactly 1 in 256. Chance. With the enable still on CS0, driving pin 5 selects
nothing at all, the breadboard chips never come onto the bus, and the reads are random. A row that
matches at precisely the rate random bytes would is a stronger statement than a row of all-wrong.

**The legacy rows are exactly 65536, every byte.** The onboard chip faithfully returns bank 5's
pattern, and each bank's pattern differs from bank 5's by a constant offset at every byte, so nothing
can match by accident. Both modes are internally consistent, in opposite ways.

---

## The pin map, verified twice from primary sources

This started as a worry and ended as a confirmation, and the confirmation is the more useful half.

The driver clocks GPIO9 bit 25 and puts data on bits 26 to 29. I had half-remembered the QSPI footprint
as pin 52 = SCLK and pin 53 = DATA0, which would have meant the driver was clocking a data line and
working by accident — and, far worse, that the FlexSPI controller could never work on this wiring
regardless of clock rate. Both sources say otherwise.

**The core's pin table**, via `teensy_pinmap` on the board itself:

| pin | GPIO9 bit |
|---|---|
| 48 | 24 |
| 51 | 22 |
| 53 | 25 |
| 52 | 26 |
| 49 | 27 |
| 50 | 28 |
| 54 | 29 |

**The core's own IOMUXC setup** in `configure_external_ram()`, which is what the controller uses:

| pad | FlexSPI2 function | GPIO9 bit | pin |
|---|---|---|---|
| EMC_22 | SS1_B, the Flash select | 22 | 51 |
| EMC_24 | SS0_B, the RAM select | 24 | 48 |
| EMC_25 | **SCLK** | 25 | **53** |
| EMC_26 | DATA0 | 26 | 52 |
| EMC_27 | DATA1 | 27 | 49 |
| EMC_28 | DATA2 | 28 | 50 |
| EMC_29 | DATA3 | 29 | 54 |

So **pin 53 is SCLK and pin 52 is DATA0**, the driver's bit assignment is the controller's pad
assignment exactly, and my recollection was the thing that was wrong.

Two things follow. The wiring is controller-compatible, so the only thing standing between this bank
and the hardware controller is signal integrity and the 49.5 MHz floor — the perfboard rebuild has to
fix the wiring, not re-route it. And the wiring diagram already given to the bench is right, which
matters because a perfboard wired to "corrected" labels would have failed in a way that looks exactly
like a hardware fault.

---

## What to do with it

1. Move the decoder enable from pin 48 to pin 5. One wire.
2. Run `psram_bank6`. It should report six banks and 48 MB with no edit.
3. Then re-run `psram_margin` per bank. The five breadboard banks sit behind a decoder and longer
   wires than the onboard chip, so they may want a slower setting — and after document 41, the thing
   to measure is each bank's clean **window**, not the fastest setting that passes.

# 41 — The tuned settings were a cliff edge, and the driver was not running the tested code

Date: 2026-09-11. Status: **measured on hardware.** Same Teensy 4.1, same six ESP-PSRAM64H, same
breadboard as document 40.

Document 40 reported 8,388,608 bytes verified with zero errors at write 5 no-ops, read 8, 96-byte
bursts. Later the same day the **identical binary**, with nothing changed in software, reported about
6,900 wrong bytes in the same 8 MB, stable at that figure across five consecutive runs.

This is what that took to explain, and the explanation is mostly about how the first result was
obtained rather than about the hardware.

---

## The result

| | before | after |
|---|---|---|
| write | 5 no-ops, 23.69 MB/s | **6 no-ops, 21.31 MB/s** |
| read | 8 no-ops, 14.83 MB/s | **10 no-ops, 13.80 MB/s** |
| verified | 8 MB once | **8 MB, eleven consecutive rounds, 92 MB, zero wrong** |
| retention | holds | holds |

The configuration is 9% slower and is the first one in this project that has been qualified rather
than merely observed to pass.

---

## Two real faults, and four wrong answers on the way

### Fault one: the settings were the fast edge of a window

`psram_margin` asked a different question from the original sweep. Not "what is the fastest setting
that passes" but "where does the clean region begin, where does it end, and how wide is it".

| | clean window, over 128 kB | shipped setting |
|---|---|---|
| reads | 8 to at least 40 no-ops | 8 — the fast edge |
| writes | 4 to at least 24 | 5 — one step in |

A threshold search returns a cliff edge by construction. It cannot do anything else. The edge passed
once and did not pass twice, because the thing being measured is a breadboard on a bench next to
someone soldering, and that is not a fixed object.

### Fault two: the driver was not running the code that had been tested

With the settings corrected the driver still produced one to three wrong bytes per 8 MB, while four
separate soaks covering 264 MB insisted the configuration was clean. Both could not be right.

The driver's write loop was hand-unrolled eight bytes at a time. The tests used a plain loop.
Unrolling removes the loop branch between nibbles, so **at the same no-op count the driver wrote
21.89 MB/s against the tests' 20.97** — 4.4% faster, and out past the margin those tests had
measured.

> A no-op count is not a timing specification. It is a timing specification for one exact instruction
> sequence, and changing the surrounding loop changes what it means.

The unrolling is gone. The driver's transfer loops are now byte-for-byte the loops in the tests, and
the 4.4% stays unclaimed. Eleven consecutive 8 MB rounds, zero errors.

---

## The four wrong answers, and what disproved each

These are in the order they were believed. Each was plausible, each had a test, and each test was
flawed in a way the next one fixed.

**"Errors only appear over 8 MB, so it is an address line."** Over 128 kB the bus was clean at 8
no-ops; over 8 MB it was not. A 300-fold rate difference cannot describe a uniform error rate, and
the small test never drives any address line above A16.

*Disproved by `psram_addr_fault`*, which stores at every four-byte word that word's own address. A
wrong word then names its own fault: the XOR of where it was found with where it claims to be is the
failing address bit. Of 18,385 bad words, **28** read as another valid address. Not an address line.
And at 10 no-ops the full 8 MB came back clean twice, so not a broken wire either.

**"It is recovery after a long run of writing."** At 8 no-ops the first pass after the 8 MB write had
18,000 errors and the immediate second pass over the same bytes had zero, every time. So the bytes in
the chip were right and the read path worked — only the first read failed.

*Disproved by its own control line.* `psram_settle` swept a wait before reading and found 0 ms dirty
and every other wait clean, which read like a clean causal result. At the end of the same run, with
no wait at all, the read was clean. The wait was never the variable. **The 0 ms row was simply always
the first cycle**, and every row after it carried the previous row's warm-up.

Sweeping a parameter in a fixed order cannot distinguish the parameter from the order.

**"Then it is going idle that does it."** `psram_idle` ran three interleaved arms at the shipping
settings — cold after a ten-second idle, warm with no pause, and primed with 64 kB of discarded
reading after the idle. **264 MB across the three arms, zero errors in any of them.** Not idling.

**"Then the test data is easier than the driver's."** Every diagnostic stored ascending addresses,
which is the quietest pattern there is: three bytes in four barely change between words. The driver
writes a pseudorandom stream where consecutive nibbles differ in about half their bits, and switching
activity is what loads a supply through five jumper wires.

*Disproved by `psram_pattern`*, which held the settings fixed and varied only the data: ascending
addresses, the driver's own pseudorandom stream, 0x00/0xFF alternating so every line swings the full
rail on every nibble, and 0x55/0xAA so neighbouring lines always oppose. **All clean.** A good
hypothesis, and wrong.

Only after all four did the 4.4% discrepancy in the measured write rate become the thing worth
looking at — and it had been printed in every single run from the beginning.

---

## What a clean test is actually worth

The thing that made this take all afternoon is that a clean result was being read as zero.

A clean pass over 8 MB bounds the error rate below roughly one in eight million. The rate being
hunted was about two in ten million. **The sweeps were not lying; they had no resolution left at the
scale that mattered**, and none of them said so.

So `psram_soak` prints the bound instead of a verdict: accumulated bytes beside accumulated errors,
the rate in parts per billion, and for a configuration that has not failed yet, the bound that
stands in for a rate and falls as the soak runs. There is no such thing as a measured zero, only a
bound that has not been beaten.

| | |
|---|---|
| 8 MB clean | under 120 errors per billion bytes |
| 264 MB clean | under 3.8 per billion |
| what the driver was doing | about 240 per billion |

---

## What is now in the tree

| test | the one question it answers |
|---|---|
| `psram_margin` | where does the clean region begin and end, for each direction separately |
| `psram_addr_fault` | is a wrong byte a timing fault or an address line — every word carries its own address so it can say |
| `psram_settle` | does a wait before reading help. Kept because its control line is the clearest example in the project of an ordering artefact |
| `psram_coldstart` | cold read against warm read at the same setting, both orders, no writes at all |
| `psram_write_margin` | the write side across the whole part instead of 128 kB |
| `psram_pair` | both settings at once, because no single-variable sweep ever visits the failing combination |
| `psram_pattern` | does the error rate depend on how hard the data makes the lines switch |
| `psram_soak` | every candidate configuration, accumulating indefinitely, reporting bounds |

Each is one question with one answer. That shape is deliberate: the tests that misled were the ones
that answered two questions at once and could not tell which had failed.

---

## And one optimisation that turns out not to exist

Every speed figure in documents 40 to 43 is read as a property of the wiring: the bus needs about
37 ns per nibble because five jumper wires and a breadboard cannot carry an edge faster. That had
never actually been tested, because the pads driving those wires were left at whatever `pinMode`
leaves them, and `pinMode` is not trying to drive a memory bus.

What the core sets for an output pin, from `digital.c`, is `IOMUXC_PAD_DSE(7)` — maximum drive
strength, so that knob was already right — with the SPEED field **absent, therefore 0**, the slowest
of four bandwidth settings, and the SRE bit **absent, therefore slow slew**. An input pin additionally
gets `IOMUXC_PAD_HYS`, hysteresis, which rejects noise by refusing to believe an edge until it has
travelled far enough and pays for that in input delay — and the read path is the slow one, and the
only one that goes through it.

Three untouched knobs on the path that needs the most help. Which way they go is not predictable from
principle: a faster edge arrives sooner, which helps timing, and rings harder into an unterminated
jumper wire, which hurts everything. `psram_pads` sweeps eight combinations and reports the fastest
clean read setting for each, plus the error count one step faster so the cliff's shape is visible and
not just its location.

| pad configuration | fastest clean | MB/s | errors one step faster |
|---|---|---|---|
| baseline, what `pinMode` leaves | 9 | 14.53 | 3060 |
| SPEED 1 | 9 | 14.53 | 3024 |
| SPEED 2 | 9 | 14.53 | 3025 |
| SPEED 3 | 9 | 14.53 | 3026 |
| fast slew | 9 | 14.53 | 3014 |
| SPEED 3 + fast slew | 9 | 14.53 | 3030 |
| hysteresis off | 9 | 14.53 | 2950 |
| SPEED 3 + fast slew + no hysteresis | 9 | 14.53 | 2849 |

**Identical. All eight, to the same no-op count and the same 14.53 MB/s.** The error count one step
faster moves from 3060 to 2849, a 7% spread on a 1.1% error density, which is noise and not a trend.

So the pad configuration was never the limit, and the wiring is confirmed as the constraint by
measurement rather than by assumption. That closes the cheapest-looking route to more speed and leaves
the perfboard as the only one. A negative result, and worth the twenty minutes: the alternative was
continuing to believe the wiring story without ever having tested the one thing that could have made
it false.

---

## Rules this earned

1. **Never ship the fastest setting that passes.** Measure where the clean region ends on both sides
   and take the middle. A threshold search returns a cliff edge.
2. **Qualify the code that ships, not a copy of it.** An unrolled loop and a plain loop are different
   timing specifications even at identical no-op counts.
3. **Never sweep one parameter in a fixed order and read the result as causal.** Run it in both
   orders. `psram_settle` is kept in the tree as the example.
4. **Vary one thing.** Two single-variable sweeps can both come back clean while the combination
   fails, because neither of them ever visits it.
5. **Quote the bound, not the zero.** A clean result means "below one over the bytes tested". Print
   the bytes beside it.
6. **When two tests of the same thing disagree, diff the code before theorising about the hardware.**
   The 4.4% rate discrepancy was visible in the first run and went unread for four hypotheses.

Rule 2 is now mechanical rather than remembered. `python .claude/verify-psram-bus.py` compares the bus
layer across every `psram_*` sketch with the no-op counts normalised away, so a sketch that shares a
number has to share the instruction sequence that number describes. Planting the original unrolled loop
back into a copy of the driver makes it fail, which is the only way to know a checker works.

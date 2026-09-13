
## 2026-09-12 02:0x — 8 banks, 20 passes each

| bank | W/R | burst | steps | MB tested | errors | bound |
|---|---|---|---|---|---|---|
| 0 | 5/16 | 64 B | 1 | 152 | 0 | under 6.27 per billion |
| 1 | 4/16 | 64 B | 1 | 152 | 0 | under 6.27 per billion |
| 2-7 | 24/24 | 48 B | 0 | 0 | 0 | no data yet |

Banks 0 and 1 are soaking clean. Banks 2 to 7 accumulate nothing because they error at the
slowest setting in the table, so the soak resets their counters every round and the bound never
falls. They are at their wall for the bit-banged driver.

Aggregate at the time of writing: 7.37 MB/s, 14.74 MMAC/s across 64 MB.

### Optimisation attempt 1: split the read's command and address phase — REJECTED

Every burst pays 14 nibbles of overhead before a payload byte moves: two of command, six of address,
six of dummy clocks. Eight of those are the Teensy driving outward, which is the direction that has
always tolerated a faster edge, and all fourteen were being clocked at the slow read rate.

Implemented as a second template parameter so the address phase could be swept independently, with
the candidate set including "same as the payload" so the sweep could decline. Accepted only if the
partial sum stayed bit-identical.

| | aggregate |
|---|---|
| before | 7.37 MB/s, 14.74 MMAC/s |
| after | 7.38 MB/s, 14.76 MMAC/s |

Noise. And on banks 2 to 7 the sweep rejected address speeds 4, 6 and 10 outright — every one changed
the answer — so the outbound phase is not tolerating a faster edge on those banks either. Reverted.

What that rules out: the overhead nibbles are not free money. The banks are limited by something that
applies to the outbound direction as much as the inbound one.

---

## 03:25 — the old soak table was meaningless, and here is what replaced it

The perfboard soak that had been running reported this, and it is worth keeping as an example of a
measurement that says nothing:

| bank | W/R | burst | steps | MB tested | errors |
|---|---|---|---|---|---|
| 0–7 | 24/24 | 48 B | 12, 12, 0, 0, 0, 0, 0, 0 | 0 | 0 |

Zero megabytes tested on every bank, every bank escalated to the slowest setting in the table, and a
bound printed as "under inf per billion so far". Banks 0 and 1 had stepped twelve times and then
reported erroring at the slowest setting, which is the runaway this project already diagnosed: slowing
the clock lengthens chip-select-low, so escalating without recomputing the burst walks a bank out past
the refresh window instead of into margin.

It was also measuring the wrong thing. Six of the eight banks were not holding the data that had been
written to them — the same log said `ANSWER CHANGED` against all six — so the 7.38 MB/s aggregate it
reported was throughput over bytes that were not the bytes written.

### Current, verified numbers

Teensy reduced to a math engine, the Luckfox owning every decision. Ten rounds, 7.3 MB a pass, 146 MB
of sustained reads, every partial sum and every timing identical to the digit.

| bank | mode | write MB/s | read MB/s 4-bit | read MB/s 2-bit | setting |
|---|---|---|---|---|---|
| CS0 | quad | 21.70 | 9.70 | 9.13 | w4 r12 burst 64 setup 0 |
| CS1 | quad | 21.70 | 9.70 | 9.13 | w4 r14 burst 64 setup 0 |
| Y0 | single | 4.80 | 2.59 | 2.55 | w4 r8 burst 16 setup 16 |
| Y1 | single | 4.80 | 2.59 | 2.55 | w4 r8 burst 16 setup 16 |
| Y2 | single | 4.80 | 2.59 | 2.55 | w4 r8 burst 16 setup 16 |
| Y3 | single | 4.80 | 2.59 | 2.55 | w4 r8 burst 16 setup 16 |
| Y5 | single | 4.80 | 2.59 | 2.55 | w4 r8 burst 16 setup 16 |
| Y4 | — | — | — | — | reads in no mode at any setting |

Aggregate over all seven: 3.28 MB/s and 6.56 MMAC/s at four bits, 3.21 MB/s and 12.84 MMAC/s at two.
Quad-only over the two fast banks: 9.70 MB/s, 19.04 and 35.41 MMAC/s.

No escalations during the run. Every setting was confirmed over the full 1 MB span before the benchmark
started, and none of them moved afterwards.

The soak bound is now stated the way it should be: 146 MB read with zero wrong bytes bounds the error
rate below roughly 7 per billion bytes, which is a bound and not a zero.

---

## 03:50 — Optimisation attempt 2: the 0x0B fast read, and the compiler moving underneath it

### The attempt

Five of the seven usable banks read one bit to the clock and are the whole critical path: the two quad
banks finish a megabyte in about 100 ms and each single-bit bank takes nearly 400. They were using
command 0x03, which this part specifies to 33 MHz and which has no wait cycles. Command 0x0B is the same
read with wait cycles after the address, and exists so the clock can go faster than 0x03 allows. It
costs eight clocks once per burst and may buy a faster clock on all 128 payload clocks after it.

Measured over 256 kB per bank, sweeping the wait count and the clock, accepting only a full-span
read-back with zero wrong bytes:

| bank | 0x03 MB/s | 0x0B MB/s | gain |
|---|---|---|---|
| Y0 | 2.56 | 2.43 | 0.95x |
| Y1 | 2.56 | 3.03 | 1.19x |
| Y2 | 2.56 | 2.43 | 0.95x |
| Y3 | 2.56 | 3.09 | 1.21x |
| Y5 | 2.56 | 3.09 | 1.21x |

So it wins on three and loses on two, and the host now measures both per bank and keeps each bank's own
winner. Taking the first mode that merely works would have handed three banks a 20% loss.

### The thing that went wrong, which was worth more than the attempt

Adding the 0x0B path cost the two quad banks 21% on a setting that had not changed. Same chip, write 8,
read 8, burst 64, bit-identical partial sum:

| burst | before | after |
|---|---|---|
| 48 | 9.33 | 7.48 |
| 64 | 9.70 | 7.71 |
| 80 | 9.93 | 7.85 |
| 96 | 10.09 | 7.95 |

Reflashing the earlier archived image and repeating the measurement is what made that visible, which is
the entire reason the archive exists.

Two hypotheses were tested and both were wrong. Instruction RAM: the loops were moved with FASTRUN and
it changed nothing, because the symbol table shows both builds already put them in `.text.itcm`. Table
granularity: odd no-op values were added so the sweep could land nearer a window edge, and it helped the
single-bit banks and not the quad ones.

The symbol table gave the answer outright. `bread<8>` is **0x29C bytes in the fast build and 0x244 in
the slow one, from identical source.** GCC unrolls the payload loop while the translation unit is small
enough to justify it and stops when it is not, and the unrolled form is 21% faster.

That is docs/41 at a larger scale. A no-op count is a timing specification for one exact instruction
sequence, and here the compiler rewrote the sequence in response to code that has nothing to do with it.
**Every template instantiation added anywhere in this file is a change to the timing of every bus loop
in it.**

The fix was to pay for the new instantiations by removing others. The diagnostic tail of 32 to 128
no-ops had already done its job — it proved that slowing down does not rescue a bank that will not read
— and it cost 25 instantiations. Removing it brought the unrolled form back, and the loop came back
*faster* than the original, so the qualification picked different settings, which it does automatically.

### Result: kept

| | before tonight | after |
|---|---|---|
| 4-bit, 7 banks | 6.56 MMAC/s | **7.41** |
| 2-bit, 7 banks | 12.84 MMAC/s | **14.48** |
| quad-only, 16 MB, 4-bit | 19.04 MMAC/s | 19.00 |
| quad-only, 16 MB, 2-bit | 35.41 MMAC/s | **35.78** |
| CS0 read | 9.70 MB/s | **10.17** |
| single-bit banks | 2.56–2.59 MB/s | **2.91–3.11** |

Up 13% on the seven-bank configuration and level on the two-bank one. Partial sums identical to every
earlier run at both bit widths, across five rounds, so the arithmetic is unchanged and still verified
against the Luckfox.

---

## 04:30 — Optimisation attempt 3: the chip-select budget, and a benchmark that was not checking its answer

### What the budget was worth

The burst was bounded by a 7 µs chip-select-low budget against a datasheet tCEM of 8 µs. Nothing here
had ever tested that number, and it was expensive: at burst 16 a single-bit read spends 40 clocks on
command, address and wait for every 128 clocks of payload, so a fifth of the bus was gone before a byte
moved.

A refresh limit cannot be tested with one pass — a violation does not fail when it is committed, it
fails later when an unrefreshed row is read. So each burst was filled once and read back eight times
without rewriting, 8 MB per setting, on a quad bank and two single-bit ones:

| burst | chip-select low | wrong of 8 MB |
|---|---|---|
| 16 | 6.1 µs | 0 |
| 24 | 8.7 µs | 0 |
| 32 | 11.1 µs | 0 |
| 48 | 15.8 µs | 0 |
| 64 | 20.5 µs | 0 |
| 96 | 29.9 µs | 0 |

Clean to 29.9 µs, nearly four times the quoted limit. The budget is now **21 µs**, which selects the
20.5 µs point and keeps the 29.9 µs result as headroom rather than spending it.

### The more important thing it exposed

Raising the budget produced an 18% higher rate and a *different partial sum in four rounds out of ten*.
The partial sum is a free checksum over every byte read in a pass, and the benchmark had never compared
one round to the next. So the first real question was not whether the budget was safe but how long this
had been happening. At the original 7 µs budget: one round in five. **The drift was pre-existing and
nothing was looking at it.**

Four hypotheses were tested and three were wrong:

| hypothesis | test | result |
|---|---|---|
| the raised budget causes it | sweep 7, 12, 16, 21 µs | present at all of them; only 21 was clearly worse |
| a bank drifts on its own | 20 reads of 1 MB per bank, 140 MB | one distinct answer on all seven banks |
| the per-switch chip reset costs a row | remove it | **worse** — 7 rounds in 7 instead of 1 in 5 |
| refresh starvation over a megabyte | inter-burst gap 0, 40, 120, 400 no-ops | no change, and up to half the throughput gone |

Removing the reset making things worse is the informative one: the reset sequence carries a 2 ms delay
with chip select high, and that delay was the only substantial refresh window in the run.

Then a two-bank alternation was clean over 192 MB in eight configurations, a seven-bank rotation was
clean over 70 MB, and a seven-bank rotation alternating both bit widths — the benchmark's exact loop —
was clean over 140 MB. All with settings hardcoded from a larger-burst run.

Which located it: the drift was never in the loop, it was in the **settings the driver chose**.
`confirm()` accepted the first setting that read a full span back without a single error, and a setting
that works once is not a setting that works. It now requires four clean passes from one fill, which also
makes it a retention test.

### Result: kept

| | before | after |
|---|---|---|
| 4-bit, 7 banks | 7.41 MMAC/s | **8.54** |
| 2-bit, 7 banks | 14.48 MMAC/s | **16.56** |
| CS0 read | 10.17 MB/s | 10.38 |
| single-bit banks | 2.91–3.11 MB/s | **3.26–3.66** |
| rounds drifting, 4-bit | 1–2 of 6 | **0 of 13** |
| rounds drifting, 2-bit | not measured | 1 of 13 |

Up 15% on rate and substantially better on correctness, over 204 MB in the confirming run.

What remains is a residual intermittent read error of roughly one or two wrong bytes per 204 MB — under
about ten per billion — which is present at every configuration tried, including the one committed
before tonight. It is below what any sweep or single verify can see, and it is visible now only because
the benchmark checks whether its own answer keeps still. Y4 aside, that residual is the last thing
standing between this build and a clean bill of health, and on the evidence so far it belongs to the
same marginal external wiring as the quad-read failure.

---

## 08:30 — One bit a weight, batching, and the drift was my own logging

### The drift was never the memory

Every configuration drifted its partial sum roughly one round in six, and four separate hypotheses about
the hardware were tested and refuted: the chip-select budget, per-bank resets, an inter-burst refresh gap,
and per-bank reads in isolation. The answer was in the firmware, and it was something added for
convenience.

`say()` echoed every line of the link to USB. An unguarded `Serial.print` on this core **blocks when its
buffer is full and no host is draining it**, for seconds at a time, and keeps the USB interrupt busy
trying. One benchmark round stalled for 198 seconds. A reply came back with bytes missing out of the
middle, which desynced the link, and a corrupted `M <sum>` reply parses as a different number — so some of
the "answer changed" verdicts were the wire, not the memory.

Guarding it on `if (Serial)` and `availableForWrite()` costs nothing when nobody is watching:

| | before | after |
|---|---|---|
| rounds with a changed answer | ~1 in 6, every configuration | **0 of 20** |
| total read in the clean run | — | **438 MB, three widths, zero drift** |

That is the first clean bill of health this build has had.

### The pad configuration test had been wrong all along

The fields on this part are `DSE` bits 5:3, `SPEED` bits 7:6, `HYS` bit 16, taken from `imxrt.h` rather
than from memory. The earlier test put `SPEED` on top of `DSE`, so it ran with the pad bandwidth field at
zero — the slowest setting — and its "no effect" answer was meaningless.

`pinMode(OUTPUT)` writes drive strength and **no hysteresis**, so every read this project has ever done
sampled with a plain threshold. Measured properly over 256 kB on the single-bit path, a Schmitt input with
fast slew and full bandwidth took **Y5 from completely broken to zero wrong bytes and Y0 from 297 to
zero**, with no bank made worse. It still does nothing for the quad read on the external chips.

### One bit a weight

Eight weights to the byte, lane `j` is `(v >> j) & 0x01010101`, and a set bit means +1 rather than 1 so
the answer is twice the set-bit sum less the sum of all activations. Forty instructions for thirty-two
weights: the same 1.25 an instruction a weight as the 4-bit and 2-bit kernels.

| width | MMAC/s over 7 banks | against 4-bit |
|---|---|---|
| 4-bit | 8.34 | — |
| 2-bit | 16.22 | 1.95x |
| 1-bit | 30.63 | **3.67x** |

Verified bit-identical against arithmetic the Luckfox computes on its own core, at all three widths.

### Batching, which is the largest result here

A byte takes about 185 cycles to arrive on a single-bit bank and the 1-bit kernel spends roughly ten of
them on it. The other 175 are the processor waiting. So the weights are read once and scored against
several activation vectors out of the same bytes: the bus does identical work and the answer count
multiplies.

| batch | CS0 quad MMAC/s | x1 | Y0 single-bit MMAC/s | x1 |
|---|---|---|---|---|
| 1 | 55.94 | 1.00x | 24.44 | 1.00x |
| 2 | 93.83 | 1.68x | 45.09 | 1.84x |
| 4 | 141.37 | 2.53x | 77.92 | 3.19x |
| 8 | 189.32 | 3.38x | 122.52 | 5.01x |
| 16 | 228.00 | 4.08x | 171.64 | **7.02x** |
| 32 | **253.93** | **4.54x** | — | — |

The answer is stable at every batch size on both banks. The slow bank climbs harder because its bus shadow
is deeper, which is the point: batching converts exactly the time the bus was wasting.

**55.94 to 253.93 MMAC/s on one bank from reading the same bytes once.** Against the 14.74 MMAC/s this
build reported yesterday — a figure that was itself measured over six banks of garbage — the verified
single-bank rate is now 17x that, and the honest seven-bank batch-1 rate is 2.1x it.

### Housekeeping that was causing real failures

- Verbose logs filled the 67 MB root filesystem to 100% and killed a benchmark mid-round with `ENOSPC`.
  Logs now go to the 3.7 GB SD card.
- The host did not own the quad wait count, so a diagnostic script that set it to 96 and exited left it
  there and the next run silently failed both quad banks into single-bit mode. The host now sets every
  knob every time, which is the whole point of the split.
- The link now drains bytes still in flight before its first command and verifies that a reply begins with
  the letter of the command that asked for it. A truncated run used to leave a fragment in the wire, and
  the driver would read it, reject it, and announce that the board was not answering — while it answered a
  direct probe perfectly on the same port seconds later.

### Open

Both boards dropped off USB at the end of this session and need a physical check. The external chips still
cannot be read in quad, and that remains a wiring question, not a firmware one -- the two chips on the
Teensy's own pads have never produced a wrong byte.

---

## 09:05 — no measurement this cycle: both boards are off the USB bus

| | |
|---|---|
| Teensy (VID 0x16C0) | not enumerated |
| Luckfox (adb af9c5305363b077e) | not enumerated |
| serial loggers running | 0 |

Neither board has returned since the end of the previous cycle, so there are no new soak bounds, no
per-bank throughput and no escalations to record. Nothing was flashed and nothing was benchmarked: a
number produced without the hardware answering would be a fabrication, and the last thing this log needs
is another figure that turns out to have been measured over something other than what it claimed.

The last verified state, from `e4637d7`, stands as the current one:

| | |
|---|---|
| banks usable | 7 of 8, 56 MB (Y4 intermittent) |
| 4-bit, 7 banks | 8.34 MMAC/s |
| 2-bit, 7 banks | 16.22 MMAC/s |
| 1-bit, 7 banks | 30.63 MMAC/s |
| 1-bit, batch 32, CS0 | 253.93 MMAC/s |
| drift | 0 of 20 rounds over 438 MB at three widths |

What the hardware needs is a physical check of whatever both boards share — they went together, which
points at the hub or the rail rather than at either board.

---

## 09:55 — the Teensy came back, dropped again mid-flash, and the fallback that came out of it

The Teensy enumerated on COM38 at the start of this cycle; the Luckfox did not. It then disappeared during
the upload — the archived image is intact at 288,120 bytes but the six-second log window after it caught no
boot banner, so the flash did not finish. Both boards are off the bus again as this is written.

| | |
|---|---|
| Teensy | appeared on COM38, gone again mid-flash |
| Luckfox | never appeared |
| measurement taken | none |

They have now dropped together once and the Teensy alone once, which is what an intermittent connection on
something shared looks like rather than a fault in either board.

### What the outage forced, which is worth having

For a whole cycle the Teensy, its bus and its 64 MB were sitting there working and there was no way to
issue a command, because the only control path was a UART to a board that was absent. A bench that can
only be driven by a board that is missing is a bench that is down.

So a command may now also arrive over the Teensy's USB port, on its own line buffer, with the reply going
back to whichever port asked. The same driver reaches either transport through `BENCH_PORT`, so the
qualification, the stability gate and the arithmetic checks are identical and the numbers stay comparable:
every timing reported comes from the cycle counter around the bus work itself, not from anything the asker
does. UART3 and the Luckfox remain the link and the orchestrator; this is a fallback, and Luckfox-driven
runs remain the reference.

**This is unmeasured.** It is not a throughput change and it has not been run against hardware, because
the hardware left. What can be checked without a board passes: it compiles, and `bread<8>` is emitted at
0x2b4 — byte-identical to the build that produced the zero-drift numbers — so the bus timing is untouched
and nothing needs requalifying on account of it.

### For the next cycle

An interrupted upload can leave the board in its bootloader, which is harmless but means it will not answer
until it is flashed again. Reflash from `bench-archive/20260912-095331-psram_worker/` first, then the
missing measurement is the seven-bank aggregate at batch 32 — the only attempt at it so far was killed by
the Luckfox running out of memory.

---

## 10:30 — third cycle with no hardware, so the driver was made testable without it

Neither board has enumerated. Rather than write a third outage note, this cycle went at the thing that has
been costing more than the outages: **three benchmark windows were spent finding driver faults, not
hardware faults.**

| what was wrong | how it was found |
|---|---|
| a link-layer patch silently failed to apply | noticed by reading the file two cycles later |
| a fragment left in the wire reported as "no reply, check the two signal wires" | the board answered a direct probe seconds later on the same port |
| the driver's own logging filled a 67 MB root filesystem | `ENOSPC` mid-round |
| the batch-32 pass killed by the kernel | never produced the number it was run for |

Every one of those was found by spending working hardware on it, and hardware windows here are scarce.

### tools/fake_worker.py

A Teensy that does not exist. It speaks the worker's protocol well enough to exercise discovery,
qualification across all three bus modes, the full-span confirm, the stability gate, the benchmark at any
batch size, and the report. `BENCH_PORT=stub` selects it.

The bank behaviour it models is the board's measured behaviour, so the fallback paths get exercised: two
banks read in quad, five only one bit at a time, and one answers and then refuses to repeat itself.

**Nothing it returns is a measurement.** Its sums come from the same rules the host checks against, so the
kernel check passes by construction and means nothing. It proves the driver runs and decides sensibly, and
proves nothing whatever about the bus, the timing, the chips or the arithmetic.

### Two real bugs it found immediately

**A hardcoded `/root/banks.txt`.** The driver writes the chosen settings out so a second program need not
repeat the expensive qualification. The path was absolute and tied to the Luckfox, so running anywhere
else crashed the whole run *after* the qualification had been done and thrown away. It now writes beside
the log, and a failure there is a warning rather than the end of the run.

**A batched figure presented as a quantisation gain.** Batching only applies at one bit, so with it on the
1-bit number counts 32 answers a pass while the 4-bit number counts one. Dividing them printed *"1-bit
277.48 MMAC/s, 54.58x the 4-bit rate over the same bytes"* — two effects multiplied together wearing the
name of one of them, which is the same species of claim as the 7.38 MB/s headline that turned out to be
throughput over bytes nobody had written. A batched width is now labelled as not comparable, and the report
says outright that the like-for-like comparison needs a batch-1 run.

Both fixes are verified against the stub at batch 1 and batch 32. Neither is a performance change and
neither has seen hardware.

---

## 11:05 — fourth cycle with no hardware; a killed run now leaves its numbers behind

Neither board enumerates. One change, aimed squarely at the loss that still stings: the batch-32 pass, the
only thing that session was run for, was killed by the kernel partway through and left **nothing**, because
the summary printed only at the end. Every round had already measured exactly what was wanted.

The result so far is now written after every round. Verified by killing a run with the same signal that
killed the real one:

```
exit after SIGKILL: 137
  after round 0: 4-bit 5.08 MMAC/s, 2-bit 10.06 MMAC/s, 1-bit 277.48 MMAC/s
  after round 1: ...
  after round 2: ...
```

Three rounds of numbers survived where previously there would have been none. If a round's answers drifted,
the checkpoint line says so on the line itself, so a partial result cannot be mistaken for a clean one.

Those figures are the stub's fiction. No hardware was involved and this is not a performance change.

---

## 11:40 — fifth cycle with no hardware; the backup itself checked for the first time

Neither board enumerates. One thing worth doing rather than more harness padding: the hard rule here is that
the exact image flashed for every run is kept so any state the board has been in is one file away, and
**nothing had ever checked those files.** The plan for the next window is "reflash from the run whose upload
was interrupted", which is a plan resting on a file nobody has verified.

Intel HEX is self-describing, so it can be checked without a board: record byte counts against payload
length, every checksum, and the end-of-file record that a truncated write loses first.

| | |
|---|---|
| archived runs | 40 |
| images verified | 40 |
| bad | 0 |
| with logs but no image | 0 |
| payload range | 61,440 to 107,520 bytes |

**The image the next window needs is intact.** The backup promise holds.

The check did find one blemish: a directory in the archive with nothing in it at all, the signature of a
build that failed before producing anything. Benign, but it made the index claim a run that never happened.
`bench_run.py` now releases the slot when a build fails, and the verifier distinguishes an empty directory
from the genuinely alarming case of logs with no image.

No hardware, no measurement, no performance change.

---

## 2026-09-13 00:40 — the board came back with six chips off the bus, and it is 49% faster

The Teensy returned on COM41 after most of a day absent. The Luckfox did not, so this was driven over the
Teensy's USB port through `BENCH_PORT`, which is exactly why that fallback exists. The six external chips
are electrically absent — all six decoder routes answer the identity probe with zeroes, so the ribbon is
off, not merely marginal.

That is an accident, and it is the most useful measurement of the whole exercise.

### Two chips on the bus instead of eight

| | eight chips on the bus | two chips on the bus |
|---|---|---|
| read setting CS0 and CS1 qualify at | 20 no-ops, burst 72 | **4 no-ops, burst 96** |
| read rate per bank, 4-bit | 10.44 MB/s | **15.52 MB/s** |

Four no-ops is the **fastest entry in the table**. With eight chips hanging off the shared data lines the
same two chips needed a clock five times slower, and gave up 49% of their throughput for it.

So the six external chips were never only failing on their own account. They were loading the bus and
taking a third of the speed off the two that worked. Chips per bus is a first-order design parameter on a
bit-banged bus, and eight on one is well past the point where it pays.

### What that does to the rate

Two banks, 16 MB, answers identical in every round, all three kernels verified bit-identical against
arithmetic the host computes itself:

| | MMAC/s | against 4-bit |
|---|---|---|
| 4-bit | 31.05 | — |
| 2-bit | 55.95 | 1.80x |
| 1-bit | 92.94 | 2.99x |
| 1-bit, batch 32 | **269.23** | not comparable; includes the batch |

Yesterday the same two banks gave 19.74 MMAC/s at four bits. This is **+57%** on identical firmware and
identical settings logic, from nothing but taking six chips off the wire.

### What this suggests for the build

Worth testing deliberately rather than by accident: fewer chips per bus, more buses. The Teensy has the
pins for a second decoder and a second set of data lines, and two buses of four chips should each clock
far closer to four no-ops than one bus of eight ever will. That is a wiring experiment, not a firmware
one, and it is a much better use of the external chips than the shared bus they are on now.

### The echo wedged the board, and it is off now

Between the two runs above the Teensy stopped answering entirely and needed reflashing. The cause was the
USB echo for the third time. Guarding it on "is the port open" and "is there room" is not enough, because
the guard is not atomic with the write: the port is open and has room, the driver finishes its run and
closes the port, the buffer stops draining, and the write already committed never returns. The firmware was
dead until reflashed.

A debugging convenience has no business being able to wedge the machine. The echo is off by default now and
turned on with `O 1` when somebody is actually watching. A command arriving over USB still gets its reply
over USB, because that is a reply and not an echo. Verified: the board now answers immediately after a run
closes the port and another opens it.

### Sustained, with the echo off

Two banks, 16 MB, driven over USB, every answer identical in every round, all three kernels bit-identical:

| | MMAC/s |
|---|---|
| 4-bit | 31.00 |
| 2-bit | 55.87 (1.80x) |
| 1-bit | 93.28 (3.01x) |
| 1-bit, batch 32 | 269.21 |

At batch 32 one pass over 16 MB of one-bit weights — 128 million parameters — yields 32 answers in 15.2 s,
which is **2.10 tokens a second**. The same pass at batch 1 gives 0.066. That is a decode rate over the
whole model, not a prefill rate.

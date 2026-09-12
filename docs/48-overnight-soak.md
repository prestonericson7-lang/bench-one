
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

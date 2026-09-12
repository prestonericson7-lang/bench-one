
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

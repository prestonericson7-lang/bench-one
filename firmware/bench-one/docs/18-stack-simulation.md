
# 18 — Running the stack at real speeds

`tests/sim_stack.py`. Not to train a model, and the weights are thrown away. The point is to find
out whether the machine operates as intended before anything is soldered, so bench time goes into
building rather than debugging design.

```
python tests/sim_stack.py --vectors 800 --queries 80 --scale 0.004
```

---

## The speed model

Every node runs at its own speed, because the architecture exists as a response to them being
unequal. A simulation where all nodes are equally fast tests nothing.

| class | n | capacity each | µs per vector | stalls | source |
|---|---:|---:|---:|---:|---|
| Teensy 4.1 | 9 | 400 | 6.08 | 0% | **estimate**: 14.25 cyc/word × 256 / 600 MHz |
| ESP32-S3 | 15 | 200 | 21.3 | 20% | **estimate**; stalls are the core stopping for wifi and BLE |
| Luckfox | 10 | 20,000 | 1.00 | 5% | **estimate**: A7 NEON `VCNT`; stalls are Linux preemption |
| PSRAM stick | 6 | 81,920 | 330 | 0% | **estimate**: 1-bit SPI at 25 MHz ≈ 3.1 MB/s |
| Zynq + 1 GB DDR3 | 2 | 1,048,576 | 0.33 | 0% | area **measured** (yosys); bandwidth estimated |

Total 2,795,272 vectors, about 2.9 GB at 1 KB each.

Only the FPGA area is measured. The rest are derived from clock rates and bus widths and are stated
so they can be **replaced**, not believed. `tests/system_test_teensy/` prints the real Teensy figure;
`psram_bringup` prints the real stick figure. Timing is linear in vectors scanned, so behaviour is
verified at whatever scale Python carries and wall-clock figures scale exactly.

---

## Deadlines at full capacity

This is the question that decides whether the design works, and it is arithmetic once the speed
table is right.

| node | full sweep | verdict |
|---|---:|---|
| Teensy | 2.43 ms | fits the 50 ms deadline |
| ESP32-S3 | 4.26 ms | fits |
| Luckfox | 20.00 ms | fits |
| PSRAM stick, screened | 26.04 ms | fits |
| PSRAM stick, scanned linearly | 27.0 s | unusable |
| Zynq, full gigabyte | 346 ms | fits the 500 ms deep-tier deadline |

The two PSRAM rows are the justification for `bench_index.c` in one line. Screening a 512-bit prefix
out of the Luckfox's own DDR2 costs 4.92 ms for 81,920 entries; only survivors have their 1 KB body
pulled over SPI at 330 µs each, capped at 64 by the safety valve. Same capacity, three orders of
magnitude less time. Scanning the bodies directly would take 27 seconds, which is not a slow tier,
it is no tier at all.

---

## Learning, measured

800 experiences of 66 concepts:

| | |
|---|---:|
| stored as new | 98 |
| reinforced an existing exemplar | 702 |
| suppressed by the surprise gate | 88% |
| distance from a new instance to its match, early | 1135 |
| the same, after learning | 889 |
| the same, after consolidation | 801 |

Random vectors sit 4096 apart, so falling numbers mean sharpening. That metric matters because
recall saturates at 100% while the machine is still improving, so recall alone cannot show learning.

Answering: 74 of 80 correct, 6 honest "I do not recognise this", **0 confidently wrong**.

All nine intended properties hold, including that no query is answered confidently and wrongly, that
coverage never exceeds what was searched, that the fast tier carries the load, and that every
fast-tier node can sweep its own memory inside the deadline at full capacity.

---

## Two design bugs this found

Both were found by the simulation, not by reading the code, and neither would have shown up as a
crash on the bench.

**The surprise gate and consolidation were in direct conflict.** The gate discarded any experience
something already recognised. That keeps memory small, and it also means a node never accumulates
two instances of one concept, so consolidation has nothing to abstract from. The first run formed
zero prototypes and that is not a tuning problem, it is unreachable code. The fix is that a
recognised experience now **reinforces** the stored vector, moving it part way toward the new
instance with a step of 1/n. The exemplar drifts to the centre of its concept and becomes the
abstraction. Memory stays small and learning still happens.

**Bundling an even number of vectors needs a tie-break.** With two members there is no majority:
every bit has 2, 1 or 0 votes. Requiring `count >= k/2 + 1` means both bits set, which is plain AND,
and AND halves the density of ones. Everything in this representation depends on density staying
near half, so a sparse vector drifts toward every other sparse vector and away from the things it
was built from. The simulation reported it at once: merging two exemplars of the same concept made
recognition *worse*, 889 to 1043. Ties are now broken per bit at random, which preserves density
exactly, and merging improves recognition to 801 as it should.

Consolidation's remaining job is worth stating, because it changed. Two nodes can independently store
the same concept, since neither could see the other's contents when it judged the input novel.
Nothing is wrong at the time and no query detects it: both answer and the merge picks the nearer.
What it costs is capacity, and sharpness, because two half-trained exemplars are each worse than one
fully-trained one. So while idle the machine looks across nodes for exemplars that now sit within
recognition distance, replaces them with their majority, and frees the duplicate. It is the only
operation here needing a global view, which is why it happens when nothing else is going on.

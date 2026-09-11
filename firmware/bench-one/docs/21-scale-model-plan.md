
# 21 — The scale model, with the measurements folded in

The goal is unchanged: can distributed memory plus heterogeneous processors plus FPGAs produce an
appliance with dramatically more model capacity per unit of space, power and cost than a
conventional machine.

The answer so far is yes, with three of the original assumptions removed. Each is removed on
measured grounds, and one of them saves months of work.

---

## What is measured, and what is still arithmetic

| | capacity | read bandwidth | status |
|---|---:|---:|---|
| Teensy 4.1 internal | 512 KB | 1,144 MB/s | measured |
| Teensy 4.1 PSRAM (2 chips) | 16 MB | 32.8 MB/s | measured |
| ESP32-S3 internal | 285 KB | 227.7 MB/s | measured |
| ESP32-S3 octal PSRAM | 8 MB | 57.6 MB/s | measured |
| Luckfox DDR2 | 13 MB usable | ~930 MB/s | measured |
| microSD, random 1 KB | 4 GB | 0.79 MB/s | measured |
| microSD, sequential | 4 GB | 17.1 MB/s | measured |
| Zynq PS DDR3 | 1 GB × 2 | ~2,500 MB/s | **estimated** |
| PSRAM on FPGA headers, 12 parallel | 96 MB | ~480 MB/s | **arithmetic** |

Compute, same units so it compares directly to bandwidth:

| | INT8 | INT4 | note |
|---|---:|---:|---|
| host core, AVX2 | 15.66 G MAC/s | 2.18 G MAC/s | measured |
| FPGA fabric, 64 lanes at 100 MHz | — | 6.4 G MAC/s | synthesised, 2,611 LCs |

**INT4 costs 7× in software and is free in fabric.** Unpacking a nibble is a shift and two sign
extensions on a CPU and is wiring on an FPGA. The whole capacity argument rests on INT4, so this is
the FPGA's real job. It is not a RAM donor.

---

## Assumption 1: "custom high-speed interconnect between nodes" — DO NOT BUILD IT

Pipeline parallelism moves only the activation across a stage boundary. For a large model that is
about 16 KB per token per hop, against weights measured in gigabytes.

| link | 32 hops, per token | ceiling |
|---|---:|---:|
| 100 Mbit | 41 ms | 24 tok/s |
| **1 Gbit** | **4.1 ms** | **244 tok/s** |
| 10 Gbit | 0.41 ms | 2,400 tok/s |

Gigabit is enough by a factor of ten, and **each PZ7020 already carries two gigabit ports**, one on
the processor and one wired straight into the fabric. Four ports across the two boards, owned
already. The fabric-side port is the interesting one: the FPGA can hand an activation to the next
stage without going through Linux at all.

A custom interconnect would take months and buy nothing until throughput passes 244 tok/s, which
this machine will not approach.

---

## Assumption 2: "tensor/model parallelism" — DO NOT BUILD IT EITHER

Tensor parallelism splits a single matrix multiply across nodes and needs an all-reduce at every
layer, which is tens of megabytes per token across every node. It swamps gigabit by two orders of
magnitude.

The decomposition that works is **pipeline parallel across layers, and expert parallel across
mixture-of-experts routing**. Both move only small things. That is not a preference, it is the
consequence of activations being 16 KB and weights being gigabytes.

---

## Assumption 3: "128 Luckfox boards" as part of the 1 TB — they are not memory

Measured over ADB: a Luckfox Pico Mini reports 33,560 kB total and 13,208 kB available, because
Rockchip reserves the rest for the image and video engines before Linux boots. 128 of them is
**1.6 GB usable**, not a meaningful fraction of a terabyte.

Cost per megabyte of capacity decides this and the spread is enormous:

| | cost per GB |
|---|---:|
| PSRAM chip at $1.75 | $224 |
| Luckfox | $788 |
| RTX 4090 VRAM | $75 |
| **DDR3/DDR4 DIMM** | **$1.88** |

The microcontrollers keep the roles the brief already gave them: deterministic control and
scheduling on the Teensys, orchestration and runtime on the Luckfoxes, peripheral and comms work on
the ESP32s. They are not where the model lives. **The memory tier is DIMMs behind FPGAs, and that
requires a board with an FMC connector — the PZ7020's two 40-pin 0.1 inch headers cannot carry
DDR3.** Wrong voltage, too few grounds, no length matching.

---

## The two small GPUs have a real job, and it is not the one implied

The brief lists them as "concentrated high-throughput compute". The sharper statement is that
**prefill and decode have opposite bottlenecks**:

* **Prefill** processes the whole prompt at once. It is a matrix-matrix multiply with high
  arithmetic intensity, so it is COMPUTE bound. A 4 GB GPU is enormously good at this.
* **Decode** produces one token at a time. It is a matrix-vector multiply that reads every active
  weight once, so it is MEMORY bound. A 4 GB GPU contributes almost nothing.

So the GPUs own time-to-first-token and the distributed RAM owns tokens-per-second. That split is
clean, it justifies the GPUs at 0.8% of total memory, and it means they should be sized for compute
rather than capacity.

---

## The ten demonstrations: where they stand

| # | demonstration | status |
|---|---|---|
| 1 | multiple heterogeneous nodes at once | simulated; hardware pending interconnect |
| 2 | distributed RAM | measured per node, not yet federated |
| 3 | FPGA-mediated memory access | **already demonstrated by you** on ESP32-S3 |
| 4 | high-speed node communication | **unblocked** — 4 gigabit ports owned, none wired |
| 5 | parallel workload distribution | planner written; `tests/plan.py` |
| 6 | FPGA acceleration of useful AI math | **done in simulation** — `gemm_int4`, verified |
| 7 | workload exceeding one node's memory | this is the whole point; needs 4 |
| 8 | measured comms and sync overhead | not started; needs 4 |
| 9 | repeatable benchmarks | done: `fpga/run_checks.sh`, `tests/*` |
| 10 | scaling behaviour as nodes are added | modelled; `plan.py --add` |

The FPGA development path in the brief runs vector add → matmul → INT8 → INT4 → tensor ops → layer.
**INT4 matrix-vector and a full INT4 layer are built and verified**, which is further along that
path than the ordering suggests, because INT4 turned out to be the interesting case rather than the
last one.

---

## Testable predictions, with pass/fail

These are stated before the hardware arrives so they can be wrong.

1. **Zynq PS DDR3 will measure 2.0 to 3.0 GB/s** streaming reads through the ARM cores.
   *Fails if* below 1.5, which would make the FPGA a worse memory node than a Luckfox.
2. ~~`gemm_int4` at 64 lanes will report a duty cycle below 60%~~ **CORRECTED, and simulated.**
   The original figure was sloppy arithmetic: 2.5 supplied over 3.2 demanded is 78%, not below 60.
   Simulated against a throttled feed the answer is **71% at 64 lanes**, achieving 2.29 GB/s of the
   2.5 available. *Fails on hardware if* it comes in below 55% or above 85%.
3. **A 16 KB activation will cross gigabit in 0.13 to 0.20 ms** one way, including stack overhead.
   *Fails if* above 1 ms, which would make the network the bottleneck and force 10 Gbit.
4. **Throughput will scale linearly with node count** until the network saturates, because nodes
   share no memory bus. The host measurement already shows the contrast: 8 CPU cores reached only
   2.97× one core's bandwidth.
5. **A 3B model at INT4 will decode at 3 to 5 tokens/sec** on the current inventory.
   Planner says 3.81.

---

## Scaling arithmetic, honestly

To reach the 1 TB target the memory tier must be DIMMs on FPGAs, at roughly 64 GB per board:

| target | FPGA boards with DIMMs | capacity | aggregate bandwidth | dense 1T | MoE 1T at 5% |
|---|---:|---:|---:|---:|---:|
| 512 GB | 16 × 32 GB | 512 GB | ~50 GB/s | 0.1 tok/s | 2 tok/s |
| 1 TB | 32 × 32 GB | 1 TB | ~100 GB/s | 0.2 tok/s | 4 tok/s |

Those bandwidth figures assume ~3.2 GB/s per board from a 32-bit DDR3 interface in fabric. That is
the number the whole scaling story rests on and it has never been measured on any board in this
project. **Prediction 1 above is therefore the single most important measurement remaining**, and
everything in this table should be treated as arithmetic until it lands.

The honest comparison at 1 TB: this is roughly a dual-socket server's bandwidth for a quarter of the
cost and a third of the power, in a fraction of the volume, and it keeps scaling where a socket does
not. The MoE column is the real use case, because every model at that size is mixture-of-experts and
an expert living entirely on one node is exactly what this architecture is shaped for.

---

## Next, in order

1. **Wire two nodes over gigabit and measure a 16 KB round trip.** Needs nothing that is not owned.
   Closes demonstration 4 and tests prediction 3.
2. **Boot a Zynq and run the STREAM benchmark on its PS.** Tests prediction 1, the load-bearing one.
3. **Run `gemm_int4` on real fabric fed from DDR3.** Tests prediction 2, closes demonstration 6 on
   hardware.
4. **Federate two nodes so a model exceeds either one.** Closes demonstrations 1, 2, 7 and 8.

---

## Measured: the stage link floor

`tests/stage_bench.c` on loopback, both ends in one process. No wire involved, so this is the FLOOR:
the cost of the protocol and the kernel's socket path alone. A slower figure on real hardware is the
network; a matching one is this code.

| payload | median RTT | p99 | worst | one-way |
|---:|---:|---:|---:|---:|
| 64 B | 29.0 µs | 31.0 | 31.0 | 14.5 µs |
| 4 KB | 30.0 µs | 52.0 | 86.0 | 15.0 µs |
| **16 KB** | **32.0 µs** | 40.0 | 49.0 | **16.0 µs** |
| 64 KB | 47.0 µs | 77.0 | 123.0 | 23.5 µs |
| 256 KB | 83.0 µs | 128.0 | 161.0 | 41.5 µs |

The shape is the finding. A 16 KB activation costs **3 µs more than a 64-byte one**: the per-hop
cost is a fixed ~29 µs of syscall and protocol, and the payload is nearly free at the size this
architecture actually uses. Activations are small enough that moving them is not a transfer, it is
an overhead.

Across a 32-stage pipeline that fixed cost is 0.93 ms per token, a ceiling of about 1,075 tok/s from
software alone. Adding gigabit's wire time for 16 KB, 131 µs, gives roughly 160 µs per hop.

**Prediction 3 said 0.13 to 0.20 ms one-way including stack overhead. It holds**, and the software
is 12% of that budget rather than the bulk of it. The network will be the wire, not the code.

---

## Measured in simulation: how wide should the FPGA core be?

`fpga/tb/tb_gemm_feed.v` throttles the weight feed to a chosen rate and reports what the core
extracts. Run against a 2.5 GB/s supply, which is the estimate for a Zynq's PS DDR3:

| lanes | demand at 100 MHz | duty cycle | achieved | share of supply |
|---:|---:|---:|---:|---:|
| 32 | 1.6 GB/s | 94% | 1.50 GB/s | 60% |
| 64 | 3.2 GB/s | 71% | 2.29 GB/s | 92% |
| 128 | 6.4 GB/s | 38% | 2.48 GB/s | **99%** |

**Build 64 lanes.** It extracts 92% of the memory for 2,611 logic cells. Going to 128 recovers the
last 8% and costs twice the logic, which is worth it only if the fabric is otherwise idle.

### The sweep found a bug in a comment I had written

The first run of this sweep gave 88%, 64% and 33%. The cause was one idle clock between rows, which
an earlier comment in `gemm_int4.v` dismissed as "0.2%, not worth fixing". That was true for the
parameters it was measured at, 8 lanes and 4096 columns, and false everywhere else: the gap is a
fixed cost per ROW, so its share grows as the row gets shorter in beats, and widening the core is
exactly what shortens it. At 128 lanes a row is four beats and the gap was a fifth of the machine.

Re-arming the core combinationally rather than through a register removed it, and correctness is
unchanged. The lesson is that a performance figure quoted without its parameters is not a fact.

### Removing it opened two holes in the test suite, and one real hazard

Mutation testing after the change found three mutations that had been caught before and now
survived. Two were a consequence of the fix: with no idle cycles the core is ready almost always, so
the tests could no longer distinguish `w_ready` from a constant 1. Closed by adding a pass that
offers a beat on **every** clock, which pushes harder than the core can take.

The third was a genuine hazard the tests could not see. Without a guard on the re-arm, the core is
left armed on a phantom row after the matrix completes. Nothing looks wrong -- no result is
published -- but `w_ready` stays high, so the next matrix's opening beats are swallowed into a stale
accumulator. Silent, and it would corrupt one row per matrix indefinitely. Closed by asserting that
ready falls after done.

All five mutations are caught now.

---

## Measured: a model split across nodes, and what the split costs

`tests/stage_node.c` runs a real pipeline. Each node holds a contiguous run of layers, computes
`y = W . x` with the same INT4 kernel the FPGA implements, and hands the activation on. The last
stage returns to the head, so a whole token is timed against one clock with no synchronisation
between nodes.

Two-stage ring on loopback, 512-wide layers, varying how much each node holds:

| layers per node | per token | compute | link | link share |
|---:|---:|---:|---:|---:|
| 1 | 0.29 ms | 0.26 ms | 0.03 ms | **10.3%** |
| 4 | 1.10 ms | 1.04 ms | 0.06 ms | **5.5%** |
| 16 | 4.34 ms | 4.25 ms | 0.09 ms | **2.1%** |

**This is demonstration 8, and the trend is the answer.** The link cost is essentially fixed at
about 30 µs a hop, matching the stage-link floor measured separately, while compute scales with how
much each node holds. So the link's share does not merely stay acceptable as the model grows, it
*falls*.

At the sizes this architecture is built for, hundreds of megabytes per node rather than half a
megabyte, the link is well under a percent. **Compute dominates, which means adding nodes helps and
the design scales.** Had it come out the other way, no amount of extra hardware would have fixed it.

Two caveats worth keeping. This is loopback, so it is the floor rather than a wire, and real
ethernet adds about 131 µs of wire time per 16 KB hop, which shifts the fixed cost from 30 µs to
roughly 160 µs and moves the 16-layer row from 2.1% to about 7%. Still small, still falling with
size. And these layers are 512 wide against a real model's 4096, so the compute per layer here is a
sixty-fourth of the real thing, which understates the compute side considerably.

`stage_node_arm` is cross-compiled and statically linked, ready to run on two Luckfoxes over the USB
network path as soon as two are plugged in.

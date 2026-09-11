
# 20 — The actual thesis, with arithmetic

This document exists because the project drifted. The brief describes a distributed inference
appliance measured in tokens per second, memory bandwidth and interconnect cost. Work in this repo
went sideways into hyperdimensional associative memory, which is a different machine. This is the
correction, and it starts from the claim in the brief rather than from where the code happened to
be.

**The claim:** given enough RAM, a distributed heterogeneous stack runs large models better than
GPUs do.

**The verdict:** true, under two conditions, and the arithmetic below is why.

---

## Why it can be true at all

Single-stream decode is **memory-bandwidth bound, not compute bound**. Every token requires reading
the active weights once, so:

```
tokens/sec  ~=  memory bandwidth / bytes read per token
```

Compute barely enters it at batch size one. This is why a 4090 and an H100 differ in decode speed
by roughly their bandwidth ratio and not their FLOPS ratio.

A GPU is extraordinary inside VRAM and collapses outside it. Past that boundary weights come over
PCIe at 25 to 50 GB/s instead of 1000 to 3350, a 20 to 100 times cliff.

| system | bandwidth | capacity |
|---|---:|---:|
| RTX 4090 | 1008 GB/s | 24 GB |
| H100 SXM | 3350 GB/s | 80 GB |
| Mac Studio M2 Ultra | 800 GB/s | 192 GB |
| dual EPYC, 24-channel DDR5 | ~600 GB/s | up to 3 TB |
| one node of this stack (RK3588 class) | ~20 GB/s | 32 GB |

The last row looks hopeless on its own. It is not hopeless in aggregate, and that is the whole
point.

---

## Capacity and bandwidth are coupled here, and nowhere else

Fill a server socket with a terabyte and you gain capacity and **zero** bandwidth. The channel count
is fixed by the socket. Add a GPU and you gain both, at enormous cost in money and watts.

Add a node to this architecture and you gain both, linearly, because every node carries its own
memory controller. There is no shared bus to contend for.

| nodes | capacity | aggregate bandwidth | rough cost | rough power |
|---:|---:|---:|---:|---:|
| 16 | 512 GB | 320 GB/s | $2,000 | 130 W |
| 32 | 1 TB | 640 GB/s | $4,000 | 250 W |
| 64 | 2 TB | 1,280 GB/s | $8,000 | 500 W |

Against a dual-socket EPYC holding 1 TB at ~600 GB/s for roughly $15,000 and 700 W, the 32-node
configuration matches the bandwidth, matches the capacity, and costs a quarter as much at a third
of the power. And it keeps going where the server cannot: the 64-node row has no equivalent at any
socket count.

That is the thesis, and the arithmetic supports it.

---

## What it does to a model that does not fit a GPU

A 1T-parameter model at INT4 is about 500 GB of weights.

**Dense, every weight read per token:**

| | bytes/token | bandwidth | tokens/sec |
|---|---:|---:|---:|
| H100, streaming over PCIe | 500 GB | ~50 GB/s | **0.1** |
| 32-node stack, pipelined | 500 GB | 640 GB/s | **1.3** |

**Mixture of experts, which is how 1T models are actually built, ~5% active:**

| | bytes/token | bandwidth | tokens/sec |
|---|---:|---:|---:|
| H100, streaming over PCIe | 25 GB | ~50 GB/s | 2 |
| 32-node stack, experts resident | 25 GB | 640 GB/s | **26** |

The MoE row is the real case. An expert lives entirely on one node, is never streamed, and only
the routed experts do any work at all. That maps onto physically separate nodes with private memory
better than it maps onto anything else, including a GPU, where all experts must share one VRAM pool.

**This architecture is uniquely suited to MoE and merely adequate for dense.** That is the sharpest
finding here and it should drive every subsequent decision.

---

## Condition one: pipeline parallelism, never tensor parallelism

This decides whether the machine works at all.

**Tensor parallel** splits a single matrix multiply across nodes and needs an all-reduce at every
layer. Hidden state for a large model is roughly 16 KB per token per layer, all-reduced across every
node, every layer. That is tens of megabytes per token over the network. It swamps any interconnect
that fits in this budget. Do not build it.

**Pipeline parallel** gives each node a contiguous run of layers and passes only the activation
across the boundary. Roughly 16 KB per token per boundary:

| interconnect | per-token cost across 32 boundaries | ceiling |
|---|---:|---:|
| 100 Mbit | 512 KB at 12.5 MB/s = 41 ms | 24 tok/s |
| 1 Gbit | 512 KB at 125 MB/s = 4.1 ms | 244 tok/s |
| 10 Gbit | 512 KB at 1.25 GB/s = 0.41 ms | 2400 tok/s |

Gigabit is enough. That is the number that makes this buildable rather than theoretical, and it is
worth stating plainly: **the activations are small, the weights are what is huge, and pipeline
parallelism moves only the small thing.**

The cost is latency, not throughput. Time to first token is the sum of every stage, so 32 stages at
a few milliseconds each is a fraction of a second. Steady-state throughput is set by the slowest
single stage, because all stages work on different tokens at once.

---

## Condition two: the node must have real DRAM bandwidth

This is where the current parts list fails, and it fails by three orders of magnitude.

Measured on the bench today:

| part | measured bandwidth | to reach 640 GB/s you would need |
|---|---:|---:|
| Teensy 4.1, PSRAM | 32.7 MB/s | 19,500 boards |
| ESP32-S3, octal PSRAM | ~50 MB/s | 12,800 boards |
| Luckfox Pico Mini | 33 MB total RAM | not a memory node at any count |
| RK3588 class SoC | ~20 GB/s | 32 boards |

The Luckfox line is the one to absorb. Measured over ADB today: 33,560 kB total and 13,208 kB
available, because Rockchip reserves the rest for the image and video engines before Linux boots.
128 of them is 4 GB of usable memory. Not 1 TB. Not close.

**The microcontrollers are not the memory tier and cannot be made into it.** They are good at what
the brief already assigns them: deterministic control, scheduling, peripheral work, orchestration.
The memory tier has to be either SoCs with real DRAM controllers, or FPGAs driving commodity DIMMs.

And that second option contains the load-bearing assumption of the whole architecture, which
deserves examination rather than assumption: a Zynq-7000's hard memory controller addresses about
1 GB. Reaching 64 GB per FPGA means a custom DDR controller in the programmable logic driving DIMMs
directly, and then the bandwidth is limited by the PL's I/O rather than by the DIMM. That is a
serious project on its own and it is the single highest-risk item in the design. It should be
prototyped and measured before any of the scaling numbers above are trusted.

---

## What to measure next, in order

1. **Real memory bandwidth, not compute throughput.** Everything measured so far is a compare
   kernel, which mixes memory and arithmetic. A STREAM-style copy benchmark gives the number this
   whole analysis turns on. Twenty minutes on hardware already on the bench.
2. **Zynq DDR3 bandwidth through the PS**, then through a PL-driven path. This is the load-bearing
   assumption.
3. **Activation transfer cost over the interconnect**, end to end, with a real 16 KB payload.
4. **One transformer layer on one FPGA**, INT8 then INT4, following the progression in the brief.
   Vector add first, then matmul, then a layer.

The FPGA toolchain is set up and verified: yosys synthesises for xc7 and iverilog simulates, both
tested against a real design with mutation coverage. A host C compiler is available. ADB reaches
the Luckfox. The instrumentation is ready even though the architecture direction was wrong.

---

## Update: the planner settles which parts matter

`tests/plan.py` allocates a model across the measured hardware by water-filling -- each node gets
weights in proportion to its bandwidth, capped by its capacity, remainder redistributed. Pipeline
throughput is set by the slowest stage, so balancing TIME rather than bytes is the correct objective.

Run against the current inventory (9 Teensy, 15 ESP32-S3, 10 Luckfox, 2 Zynq), holding 2.38 GB at
15.1 GB/s aggregate:

| addition | approx cost | llama-3b tokens/sec |
|---|---:|---:|
| nothing | — | 3.81 |
| +20 Teensy | $600 | 4.29 |
| +20 Luckfox | $200 | 4.69 |
| +2 Zynq | $200 | **7.40** |
| +6 Zynq | $600 | **14.03** |

Two more FPGA boards nearly double throughput. Twenty more Teensys, at three times the cost, add
thirteen percent. To fit an 8B model at all takes 2 more Zynqs or **123 more Teensys**.

The reason is visible in the placement. The Luckfoxes finish their share in 14 ms and then idle,
because capacity runs out long before bandwidth does; the Zynqs grind for 262 ms. Bandwidth the
machine cannot fill is worth nothing, and that is the whole microcontroller tier in one sentence.

**Conclusion: the FPGA boards are the machine. Everything else is orchestration.** That is the
opposite of where this project started and it is what the measurements say.

---

## Update: the INT4 core, and a memory-shape bug worth remembering

`fpga/rtl/gemv_int4.v` computes INT4 weights against INT8 activations. Verified in Icarus against an
independently computed reference across six directed cases, with four RTL mutations all caught
(reversed nibble order, ragged-row padding counted, a narrowed accumulator, an inflated consumed
count). Two of those only became detectable after the testbench was strengthened -- padding with
zeros hid one, and random test values that never exceeded 16 bits hid another.

Synthesis for xc7, after a fix that mattered enormously:

| lanes | logic cells | weight rate at 100 MHz |
|---:|---:|---:|
| 8 | 420 | 0.40 GB/s |
| 16 | 814 | 0.80 GB/s |
| 32 | 1,579 | 1.60 GB/s |
| 64 | 2,595 | 3.20 GB/s |

The first version used **32,636 logic cells at 8 lanes and 64,551 at 16**, which does not fit a
Zynq-7020's 53,200. Nothing was wrong with the arithmetic. The activation memory was declared as a
byte array read at LANES independent addresses every clock, which is an eight-port memory, and no
block RAM has eight ports -- so the synthesiser built it out of LUTs. Storing LANES activations per
word makes it one wide read at one address and the cost fell 78 times.

The lesson generalises: on an FPGA, the SHAPE of a memory access decides the area, not the amount of
data. A design can be functionally perfect, pass every simulation, and not fit the part.

At 64 lanes the core uses 5% of a 7020 and consumes 3.2 GB/s, more than the DDR3 can supply. The
FPGA is memory bound, which is exactly where the bottleneck belongs.

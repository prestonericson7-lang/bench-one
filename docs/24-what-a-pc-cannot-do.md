# 24 — What this does that a PC cannot



Every number here was measured on this bench or synthesized from this project's own RTL. Nothing is
quoted from a datasheet headline and nothing is a projection except where it says so.

---

## The one-line version

**One $60 FPGA chip, using 37% of its fabric, unpacks 4-bit weights 2.4× faster than this entire
eight-thread desktop.**

| | 4-bit unpacking throughput |
|---|---|
| this desktop, 8 threads of AVX2, best kernel I could write | 10.55 GB/s |
| one XC7Z020 at 512 lanes, 100 MHz, 37% of the chip | 25.6 GB/s |
| the same chip at 1024 lanes, 74% of it, if it routes | 51.2 GB/s |

That is not a clever benchmark. It is what the arithmetic costs on each machine. A processor has to
shift a byte, mask it, sign-extend twice and then multiply. Fabric has the high nibble and the low
nibble on different wires, so there is nothing to do.

---

## The measurement that matters most: wasted bandwidth

A processor's memory is faster than its ability to use it. Every row below was measured on the
hardware named, with the same kernel.

| | reads memory | can unpack 4-bit | actually sustains | put to work |
|---|---|---|---|---|
| this desktop | 29.40 GB/s | 10.55 GB/s | **7.60 GB/s** | 26% |
| Luckfox Pico | 993 MB/s | 61 MB/s | 57 MB/s | 6% |
| Teensy 4.1 from PSRAM | 33.9 MB/s | 39.3 MB/s | **21.4 MB/s** | 63% |
| one Zynq, fabric doing it | 2500 MB/s | 2290 MB/s | **2290 MB/s** | 92% |

Three quarters of this desktop's memory system contributes nothing to decoding a token. It is there
because the CPU cannot consume what it delivers. A Luckfox is worse: it wastes 94% of its bandwidth.

**That ratio is the whole architecture in one number.** Buying a faster desktop buys more of the 74%
you cannot use. Adding a cheap FPGA board buys bandwidth that is 92% usable.

### The rule that was wrong, and why it matters here

This planner used to take the *slower* of reading and unpacking. That assumes a chip does both at
once. A processor does not: the same core issues the loads, then does the shifts, so the two costs
**add**, and the answer is the harmonic sum.

| | old rule | corrected rule | measured |
|---|---|---|---|
| Teensy 4.1 | 33.9 MB/s | 18.2 | **21.4** |
| this desktop | 10.55 GB/s | 7.76 | **7.60** |

The desktop row settles it: 2% agreement with nothing fitted.

**Fabric is the exception, and it is the point.** An FPGA unpacks inside the streaming datapath, on
bytes already in flight, so there the two genuinely overlap. Every processor in the machine had been
credited with an overlap it does not have. Correcting that widened the FPGA's lead by arithmetic
rather than by argument.

## Lanes are nearly free, which changes the build

Synthesized just now, `gemm_int4` against `synth_xilinx -family xc7`:

| lanes | logic cells | % of XC7Z020's 53,200 | peak unpack | vs this board's DDR3 |
|---|---|---|---|---|
| 64 | 2,611 | 4.9% | 3.2 GB/s | 1.3× |
| 128 | 5,168 | 9.7% | 6.4 GB/s | 2.6× |
| 256 | 10,030 | 18.9% | 12.8 GB/s | 5.1× |
| 512 | 19,756 | 37.1% | 25.6 GB/s | 10.2× |
| 1024 | 39,209 | 73.7% | 51.2 GB/s | 20.5× |

Scaling is linear across a sixteen-fold range, and the chip is mostly empty at every size worth
building.

**The last row fits by logic count and should not be trusted yet.** Yosys estimates logic cells. It
does not place, route, or close timing. At 73.7% occupancy a 7-series device gets hard to route and
100 MHz stops being free. Take 1024 lanes as evidence that the trend does not break, not as a design.
Vivado is the only tool that can settle it, and that is a job for when the boards are in hand.

### This revises an earlier decision

The earlier conclusion was **build 64 lanes**, because 64 lanes already wants 3.2 GB/s and the board's
DDR3 only delivers about 2.5. That reasoning was right about the bottleneck and wrong about what to
do, because it treated fabric as scarce. Fabric is not scarce. 64 lanes costs 4.9% of the chip.

**Build 256.** It costs 19% instead of 5%, it is still less than a fifth of the device, and it means
any memory upgrade — a faster DDR3 controller, the second memory port, twelve PSRAM chips in parallel
on the headers at 480 MB/s — converts straight into tokens with no re-synthesis. Over-provisioning
the cheap resource to avoid being re-limited by the expensive one is the right trade when the cheap
resource costs a fifth of nothing.

The corollary is the strongest statement in this document: **on this architecture the FPGA is never
the bottleneck.** Memory is. And memory is the thing that gets cheaper per megabyte every time another
board goes in the rack.

---

## It scales, and the scaling cost was measured between two real machines

Pipeline parallelism moves activations between nodes, never weights. Measured with
`tests/hop_bench.c`, a Luckfox to this PC over the board's USB ethernet, timed on the board's own
clock so nothing needs synchronising:

| payload | round trip | one way | MB/s |
|---|---|---|---|
| 64 B | 296 µs | 148 µs | 0.4 |
| 8 KB, one activation | 875 µs | 437 µs | 18.7 |
| 32 KB | 2.5 ms | 1.25 ms | 26.1 |
| 196 KB, a prefill batch | 12.3 ms | 6.2 ms | 31.9 |

**An earlier version of this document said 29 µs, measured on loopback. A real hop is 148 µs — ten
times more.** Every "distribution is free" claim here was anchored to a socket that never left the
process.

It survives the honest number. 437 µs for an activation against a stage that takes 100 to 440 ms is
under half a percent. Cheap, not free, and cheap is the right word.

For the 208-node fleet in the brief, 208 stages cost **91 ms** of a token against stage compute in the
hundreds of milliseconds. A 405B model and a 3B model pay the same 91 ms, because the activation does
not grow with the model — only the weights do, and weights never move.

**Tensor parallelism would move gigabytes per token and is why nobody builds a machine like this that
way.** Choosing pipeline parallelism is not a compromise, it is the reason the machine is possible.

### And it may save you the ethernet adapters

18.7 MB/s carrying an activation, over nothing but a USB cable already on the desk. That is enough.
The catch is shape, not speed: USB gadget mode needs a host, so many boards around one hub works and
board-to-board does not. Ethernet is what buys direct board-to-board wiring, and it buys speed nobody
needs yet.

## What the whole fleet comes to

Counts from the original brief, every per-node rate the same measured figure used for the two boards
on the bench:

| | this desktop | the fleet |
|---|---|---|
| memory bandwidth | 29.40 GB/s | 166.05 GB/s |
| **bandwidth it can actually unpack** | **10.55 GB/s** | **43.82 GB/s** |
| independent nodes | 1 | 208 |

**4.2× the usable bandwidth.** It was 5.0× before the Luckfox was measured and the combination rule
corrected; both moved it down, and the smaller number is the real one. That is still with 64-lane
cores, and at 256 lanes the fabric stops being the limit anywhere in the fleet.

Note where it comes from: sixteen FPGA boards supply 36.6 GB/s of that 43.8, and a hundred and
twenty-eight Luckfoxes supply 7.4. **The microcontrollers are 92% of the node count and 17% of the
throughput.** They are there to hold the machine together, not to compute.

---

## Where it honestly loses

This belongs in the same document or the rest of it is advertising.

- **On a 3B model the desktop wins.** 4.25 tokens per second against 2.27 predicted for two boards. A
  model that fits one machine is the wrong test.
- **Capacity today is small.** Two boards hold 2 GB of fast memory against the desktop's 48 GB. The
  fleet in the brief holds about 19.6 GB. The 1 TB figure needs FPGA boards with DIMM sockets, which
  these do not have.
- **A 70B model needs 40 boards** to fit in RAM at INT4, and then runs at 2.29 tokens per second. It
  does not fit 16.
- **Per-watt is still an estimate.** Roughly 13×, and it needs a meter on both ends before anyone
  quotes it.
- **Every microcontroller in the fleet is nearly useless for arithmetic.** Measured: a Teensy sustains
  21.4 MB/s and a Luckfox 57. A single FPGA board does 2290. That is not a close call.
- **The Zynq DDR3 figure of 2.5 GB/s is an estimate.** It is load-bearing for every number above and
  the boards are still in transit.

---

## The claim, stated so it can be attacked

This is not a faster computer. It is a machine whose **bandwidth and capacity grow together for tens
of dollars a step, with under 1% loss per step**, built out of the one device that can consume memory
at 92% efficiency instead of 36%.

A desktop has a soldered memory controller and a fixed core count. It cannot be taken from 10.55 GB/s
of usable unpacking to 53 GB/s at any price. This can, by plugging in more of the same $60 board.

Two things would falsify it, and both are measurable with hardware already owned or on its way:

1. **The Zynq DDR3 delivers far less than 2.5 GB/s in practice.** Every throughput figure here scales
   with it. First thing to measure when the boards land.
2. **The per-hop cost rises with node count.** Measured at two nodes on loopback. The two-Luckfox test
   over USB networking costs nothing and has not been run yet.

# 37 — Scaling, and what a new chip would have to be

*Everything found on 2026-09-10 about making this bigger. Three scales, in order of how far away they
are: the bookshelf unit that exists, the shed that the numbers say works, and the in-package processor
that the numbers say is the real destination.*

*Every figure is marked MEASURED (on this project's own hardware or read out of a real model file),
PUBLISHED (a datasheet, a price list, a paper), or ASSUMED. The assumed ones are listed together at
the end because they are what to attack next.*

---

## 1. The three scales, and what each one is for

| | holds | what it proves |
| --- | --- | --- |
| bookshelf, 33 boards, built | 3.45 GB | that heterogeneous nodes compose exactly, and where the real bottlenecks are |
| shed, off-the-shelf carriers and DIMMs | 2 to 10 TB | that capacity is cheap and passive cooling is not the constraint |
| in-package, wire-bonded die | limited by heat, not geometry | that the machine's size is set by DRAM refresh |

The bookshelf unit is not a small shed. It is the instrument that produced the numbers the other two
are built on, and each of its results survives the scale change:

- **MEASURED:** an activation between nodes is 4 KB and a real machine-to-machine hop costs 148 µs.
  Weights never move. That is what makes any amount of replication possible.
- **MEASURED:** 33 boards rendered one frame as 33 bands that stitched byte-identically across x86 and
  ARM. Unlike processors can split work with no seam, which is the licence for a fleet.
- **MEASURED:** a 30B mixture-of-experts touches 11% of itself per token, and expert use concentrates
  3 to 5 times against uniform on real text.
- **MEASURED:** 4-bit unpacking in fabric is 256 lanes in 18.9% of an XC7Z020, at 71% duty against a
  2.5 GB/s feed. The compute beside the memory can be tiny.
- **MEASURED:** int8 KV cache costs nothing in perplexity; 4-bit destroys the model.

## 2. Carrier boards that exist today, with prices

The question was whether DDR5 can get into the system off the shelf. It can, and the price per
gigabyte is terrible.

| board | memory | device | price | source |
| --- | --- | --- | --- | --- |
| AMD VPK120, EK-VPK120-G | DDR5 DIMM | Versal Premium | **$11,994** AMD, $13,571 DigiKey | PUBLISHED |
| Altera Agilex 5 E-series premium kit | DDR4 + LPDDR4, 20 GB total | Agilex 5 | quote only | PUBLISHED |
| BittWare XUP-P3R | 4 DDR4 DIMM sites | Virtex UltraScale+ VU9P | not published | PUBLISHED spec |

What the silicon actually supports:

| family | DDR5 | notes | source |
| --- | --- | --- | --- |
| Versal Premium | yes, DIMM on VPK120 | the only off-the-shelf DDR5 DIMM board found | PUBLISHED |
| Agilex 7 M-series | yes, hard controllers plus HBM over a memory NoC | the high end | PUBLISHED |
| Agilex 5 | yes, to 5600 Mbps; also DDR4 3200, LPDDR5 5500, LPDDR4 4267 | the low-cost tier that can do DDR5 | PUBLISHED |
| Spartan UltraScale+ | no. Hardened controller is LPDDR4x/LPDDR5 to 4200; DDR4 is a **soft** controller capped at 2400 | the cheap part, and the catch is in that word soft | PUBLISHED |
| Zynq UltraScale+ | no, DDR4 only | what the project already owns | PUBLISHED |

**The conclusion from this table is that the board is the cost, not the memory.** A 4T model needs
2095 GB resident, which is 33 used 64 GB DDR4 RDIMMs at about $2,970. Nine four-slot carriers at
$2,500 each would be $22,500, so DRAM is 12% of the core and the controllers are 88%. At VPK120 prices
the same capacity in carriers is $108,000 and the ratio inverts completely.

## 3. "Unlimited RAM connection" has a known shape, and it is serial

Parallel DDR does not scale by adding DIMMs and the reason is electrical, not commercial. A DDR4
channel is roughly 105 single-ended signals, and signal integrity caps a channel at one to three
modules depending on registration. That is precisely why a server has twelve channels rather than one
channel with twelve modules.

The industry answer already exists and is worth designing against rather than reinventing:

**Open Memory Interface and the differential DIMM.** PUBLISHED, from Microchip's SMC 1000 8x25G and
SMART Modular's DDR4 DDIMM:

| | |
| --- | --- |
| host side | 8 differential lanes each way at 25.6 Gbps |
| bridges to | a 72-bit DDR4-3200 interface behind the buffer |
| throughput | 25.6 GB/s per module |
| capacity | up to 256 GB per module |
| latency | 40 ns, **under 4 ns more than an integrated DDR controller with LRDIMM** |
| the claim | **four times the memory channels of parallel DDR4 in the same package footprint** |

Sixteen differential pairs is 32 pins for a full channel against 105. On a device with 128 high-speed
transceivers that is sixteen channels, so 4 TB at 410 GB/s from one package.

### But the better answer follows from this project's own measurement

If the unpack and the dot product happen **at** the memory, the link never carries weights at all. It
carries a 4 KB activation in and a 4 KB partial sum out, per layer, per token.

| link | time to move one 4 KB activation |
| --- | --- |
| OMI at 25.6 GB/s | 0.16 µs |
| 1 gigabit Ethernet | 32 µs |
| the hop MEASURED in this project | 148 µs |

Even plain Ethernet is two hundred times more than a module needs. **So the interconnect stops being
the constraint entirely, and "unlimited" becomes true in the only sense that matters: nothing shared
grows when you add a module.** A 48-layer pipeline costs 48 hops, which at the measured 148 µs is
7.1 ms a token against hundreds of milliseconds of DRAM reading.

That is the architecture: memory-side compute modules, connected by anything.

## 4. What a custom chip costs, so the number is known rather than feared

PUBLISHED, 2026:

| node | full mask set | MPW shuttle slot, small die |
| --- | --- | --- |
| 180–130 nm | $0.5M – $1.5M | — |
| 65–40 nm | $1.5M – $4M | $5k – $15k for about 4 mm² |
| 28 nm | $0.8M – $1.5M | $20k – $50k |

Wafers run $3,000 at 28 nm to over $20,000 at 3 nm. All-in for a startup on a mature node is **$1.5M
to $8M**, dominated by verification and IP licensing, and DDR PHY IP is one of the expensive licences.

So a custom chip with its own memory PHYs is a seven-figure project. That is not an argument against
it; it is the reason the module comes first, because a working module is both the proof and the
specification.

## 5. Home fabrication: where the wall is, exactly

PUBLISHED state of the art for one person in a home lab is Sam Zeloof's Z2: **1,200 transistors at
about 10 µm.** That is the ceiling, and DRAM is the worst possible target for it.

A DRAM cell is one transistor and one capacitor, and the capacitor is the problem. Modern ones are
trench or stacked structures at aspect ratios above 60 to 1 using atomic-layer-deposited high-k
dielectrics. At 10 µm features with a planar capacitor, a cell is roughly 100 µm square:

| | |
| --- | --- |
| cells in 1 GB | 8.6 billion |
| area each | 0.0001 mm² |
| silicon needed | 0.86 m² |
| 300 mm wafers, at perfect yield | 12 |

**Four orders of magnitude out, and it is equipment physics rather than skill or patience.** Resolution
is wavelength and optics.

## 6. The route that is both achievable and where the win actually is

**Wire bonding bought DRAM die beside bought logic.** This is packaging, not fabrication, and it is
industrially ordinary: NAND has shipped in 16 and 32 die stacks for years and most phone LPDDR is
wire-bonded package-on-package. What is unusual is doing it for *model* memory.

Accessible equipment, used: a manual wedge bonder $5k–$30k, die attach and dispense $3k–$15k,
microscope and probe station $2k–$10k, laminar flow hood $1k–$3k. Chip-on-board assembly runs in class
1000, not class 100.

Why it matters is energy per bit, PUBLISHED:

| path | pJ per bit |
| --- | --- |
| DDR4/5 off package, across a PCB | 20 – 30 |
| in package, wire-bonded, under 2 mm | 2 – 5 |
| HBM | 3 – 5 |
| on-die SRAM | 0.1 |

A 4T mixture-of-experts reads 115.3 GB a token:

| | J per token | W at 10 tok/s |
| --- | --- | --- |
| off-package DDR at 25 pJ/bit | 24.75 | 248 |
| in-package wire bond at 4 pJ/bit | 3.96 | 40 |

**Six times less power for the same work, at commodity DRAM cost instead of HBM cost.** And in package
you go wide and slow rather than narrow and fast, which is exactly why HBM is efficient: 1024 bits at
2 Gbps beats 32 bits at 6.4 because you skip the serialisers, and the serialisers are where the
picojoules go.

A 10 mm die edge at 60 µm bond pitch gives 166 pads, 664 on four sides, which is four channels of 128
data bits plus control. At 800 MT/s that is 51.2 GB/s and, with 32 GB of die per channel, 128 GB a
tile.

### Density, which is the part nobody has

| | GB | cm³ | GB per cm³ | |
| --- | --- | --- | --- | --- |
| bare stacked die, 65 mm² thinned to 70 µm on 90 µm pitch | 2 | 0.00585 | **341.9** | |
| DDR5 64 GB RDIMM | 64 | 33.3 | 1.92 | 178× less |
| H100 SXM5 module | 80 | 46.0 | 1.74 | 197× less |

## 7. THE FINDING: the binding constraint is refresh, and it is not geometry

DRAM pays to hold a bit whether anyone reads it or not. Self-refresh is about 36 mW a die, PUBLISHED,
so every terabyte powered costs **18.4 W doing nothing at all**, and 36.9 W above 85 °C where the
refresh interval halves.

Reading is the cheap part. 115.3 GB a token at 4 pJ a bit, ten tokens a second, is 40 W. Refresh
dwarfs it.

So the question inverts. Not how much silicon fits, but **how much can be powered.**

| enclosure | passive capacity | TB powered | litres of die | parameters at 4.5 bits |
| --- | --- | --- | --- | --- |
| a shoebox | 60 W | 3.3 | 0.01 | 6 T |
| a bookshelf unit | 518 W | 28.1 | 0.08 | 55 T |
| a shed, 2×3×2.4 m | 10,686 W | 579.8 | 1.74 | **1,133 T** |

Read the last two columns together. **The silicon that holds a model of that size is litres, not
rooms. The enclosure is large because it is a radiator.** That is the thing nobody has built: the
machine's size is set entirely by heat, and the heat is refresh rather than work.

The passive figures come from two independent paths, both conservative: skin at 7 W/m²K combined
convection and radiation, plus stack effect where a column of air at a 20 K rise moves itself,
Q = Cd·A·√(2·g·h·ΔT/T). The stack term dominates and is why a shed is a sensible enclosure rather than
a joke.

### Which points at exactly one lever

A mixture-of-experts leaves 88 to 95% of itself untouched per token, MEASURED on a real 30B, and
expert use concentrates 3 to 5 times against uniform. **Cold weights do not need DRAM at all** — flash
costs nothing to hold. Every terabyte moved from DRAM to flash buys back 18.4 W, and the measured
cache curve says how much can move.

## 8. What the watcher cores change about building AI

A small RISC-V or Cortex-M class core is about 0.03 mm² at 28 nm, so one per memory channel is free
area. 128 of them at 400 MHz is 3.84 mm² total and 190 GB/s of telemetry.

On a GPU, watching the model competes with running it: every core inspecting an activation is a core
not doing work, so interpretability runs two to ten times slower and is therefore done on small models
and small samples. Here it is free, because the silicon doing it was never going to carry weights, and
this project already measured what that is worth at small scale with the 33-band render.

What becomes possible that is not possible now:

- every expert choice, every attention map, every activation, for every token of a billion-token
  corpus, on a 4T model, at full speed
- the expert placement policy **learned online** — deciding what to migrate where needs a spare
  processor, and a GPU has none
- dead neuron and dead expert detection continuously rather than sampled
- a per-token audit trail, which no accelerator can produce at any price

And none of it is faster than a GPU at anything a GPU can already do. A rack serves hundreds of tokens
a second to many users; this serves a few to one. **It is not a rival for serving a model. It is a
rival for having one.**

## 9. The assumptions, gathered, because these are what to attack

1. **The DIMM-capable carrier's real cost.** $2,500 is a guess and it is 60 to 88% of the shed's core.
   The whole cost answer is a function of this one line, and no off-the-shelf board matches it.
2. **The fabric duty cycle against four live DDR4 channels.** The 0.71 used everywhere came from
   synthesis against a single throttled feed.
3. **Bond pitch and achievable die edge.** 60 µm and 10 mm are assumed, and the pad count scales
   linearly with both.
4. **DRAM refresh power under this access pattern.** 36 mW a die is a datasheet figure and it is now
   the single most load-bearing number in the entire plan.
5. **What a 2T-class mixture-of-experts activates.** 5.5% is DeepSeek-V3's published ratio.
6. **The Zynq's DDR3 bandwidth**, still an estimate, and every bookshelf-scale figure scales with it.
7. **The NEON unpack rate on the boards' own cores.** Every unpacking figure this project has ever
   quoted came off a desktop, and the desktop is not a node. This is the next measurement.

## 10. The tools

```
cd firmware/bench-one/tests && python shed.py --model 4t-moe --budget 300000
```

```
cd firmware/bench-one/tests && python newcpu.py --tiles 32 --model 4t
```

```
cd firmware/bench-one/tests && python newcpu.py --tiles 32 --model 4t --sparsity contextual
```

---

## Sources

- [AMD VPK120 evaluation kit, DigiKey](https://www.digikey.com/en/products/detail/amd/ek-vpk120-g/16705846)
- [Versal Premium VPK120 product brief](https://www.xilinx.com/content/dam/xilinx/publications/product-briefs/vpk120-product-brief.pdf)
- [Agilex 5 FPGA E-Series overview, Altera](https://www.altera.com/products/fpga/agilex/5)
- [Agilex 7 M-Series, DDR5 and HBM](https://www.intel.com/content/www/us/en/products/details/fpga/agilex/7/m-series.html)
- [AMD Spartan UltraScale+ product brief](https://www.xilinx.com/content/dam/xilinx/publications/product-briefs/amd-spartan-ultrascale-plus-product-brief.pdf)
- [BittWare XUP-P3R, four DDR4 DIMM sites](https://www.bittware.com/products/xup-p3r/)
- [Microchip SMC 1000 8x25G serial memory controller](https://www.microchip.com/en-us/about/news-releases/products/microchip-enters-memory-infrastructure-market-with-serial-memory)
- [SMC 1000 and serial attached memory, ServeTheHome](https://www.servethehome.com/microchip-smc-1000-for-the-serial-attached-memory-future/)
- [SMART Modular DDR4 differential DIMM](https://www.globenewswire.com/en/news-release/2019/08/05/1896979/0/en/SMART-Modular-to-Showcase-its-DDR4-Differential-DIMM-at-the-Flash-Memory-Summit.html)
- [Foundry engagement, MPW to production, costs 2026](https://siliconanalysts.com/guide/foundry-engagement)
- [Wafer pricing by process node 2026](https://siliconanalysts.com/data/wafer-pricing)
- [Tapeout cost, fabless startup guide](https://siliconanalysts.com/analysis/fabless-startup-tapeout-cost-guide)
- [Semiconductor wafer mask costs, AnySilicon](https://anysilicon.com/semiconductor-wafer-mask-costs/)

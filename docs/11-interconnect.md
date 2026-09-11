# 11 — THE INTERCONNECT: how 38 nodes actually talk

This is the document that decides the wiring. It supersedes the topology in doc 10 §3.

**The problem, stated honestly.** 9 Teensy + 15 ESP32-S3 + 10 Luckfox + 2 FPGA + 2 ATtiny = 38
nodes. Two FPGAs give 128 PL IO pins total. That is 3.4 pins per node. Any architecture that
routes every node to the FPGA is dead before it starts — and a 38-node UART tree at 92 KB/s is a
control plane pretending to be a data plane.

**The answer: stop treating the FPGA as the hub.** Three of the four node classes already have a
high-speed network built into the silicon, and nobody has been using it.

| Class | The link that was already in the chip | Rate | PL pins used |
|---|---|---|---|
| **Luckfox ×10** | **On-die 10/100 Ethernet MAC + PHY.** Pads are already on your boards | 12.5 MB/s full duplex | **0** |
| **ESP32-S3 ×15** | WiFi 802.11n to one AP | 2.5–3.75 MB/s each (TCP, measured by Espressif in air) | **0** |
| **Teensy ×9** | 2× PSRAM on the native underside pads — no link needed at all (§4) | 45 MB/s local | **0** |
| **FPGA ×2** | PS-side gigabit Ethernet | 125 MB/s | **0** |

**Total PL pins needed for the whole cluster: zero.** All 128 stay free for what the FPGA is
actually good at (§6). That is the architecture.

---

## 1. The backbone: one Ethernet switch

```
                     ┌──────────────────────────────────┐
                     │   16-port gigabit switch  (~$30)  │
                     └─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─┘
      ┌────────────────┘  │  │  │  │  │  │  │  │  │  │  └──── your PC
      │     ┌─────────────┘  │  │  │  │  │  │  │  │  └─────── FPGA-A  (PS GigE)
      │     │      ┌─────────┘  │  │  │  │  │  │  └────────── FPGA-B  (PS GigE)
   LF-1  LF-2  LF-3 … LF-10   (10 Luckfox @ 100 Mbit, magjack on the FEPHY pads)
      │
      └── WiFi AP (a router, or one S3 in SoftAP mode) ── 15× ESP32-S3

   Teensy ×9  → UART to whichever Luckfox owns them (control only — the other AI's lane)
   ATtiny ×2  → one heartbeat wire. Not on the network by design (§7)
```

13 wired ports, 15 wireless nodes, one address space, one protocol family (TCP/UDP), zero custom
HDL, zero level shifters, zero pin budget. Every node can reach every other node. You can `ssh`
into any Luckfox and `iperf3` any link on the bench without an oscilloscope.

**Cost of the whole backbone: a $30 switch, 12 cables, and 10 magjacks.**

### 1.1 Giving a Luckfox its Ethernet jack

This is the one piece of new hardware work in the entire interconnect, and it is the same job ten
times. Verified from the RV1103 datasheet, the Luckfox Pico/Mini schematics, and the SDK device
tree:

| Fact | Detail |
|---|---|
| PHY | **Integrated in the RV1103** (FEPHY), 10/100, RMII internal. No external PHY chip |
| Pads | `FEPHY_TXP / TXN / RXP / RXN` + GND, brought out as a pad group on **both** the Pico Mini and the Pico |
| Bias resistor | `R33 = 6.04 kΩ 1%` on FEPHY_REXT — **already fitted on your boards** |
| What you add | RJ45 **with integrated magnetics** (a magjack), 0 Ω series links close to the jack, 10 nF centre-tap capacitors, and a Bob Smith network (75 Ω + 1 nF 2 kV) on the unused pairs |
| Software | `&gmac { status = "okay"; };` in the device tree, then rebuild and reflash the dtb. **`luckfox-config` has no Ethernet toggle** — this is a dtb edit, not a menu item |
| Reference design | The Luckfox Pico **Plus** is the same SoC with the jack fitted. Copy its schematic exactly |

**Do one board first, `iperf3` it, then do the other nine.** Budget one evening for the first and
an hour for the rest.

**Second prize, if a board's pads are damaged or you want one running before the magjacks arrive:**
the USB gadget already works out of the box — RNDIS at `172.32.0.93`, USB 2.0 high-speed. Plug it
into a powered hub on an FPGA's USB host port. Two cautions: **every board ships with the same
`172.32.0.93`**, so each one's `/etc/init.d/S99usb0config` needs a distinct address before more
than one is plugged in; and nobody has ever published a measured RNDIS throughput figure for the
RV1103, so do not budget it — measure it. Ethernet is the plan; USB is the fallback.

### 1.2 Addressing

Static, from the switch or a `dnsmasq` on one Luckfox. Node ID is the last octet, and it matches
the node IDs already in `bench_protocol.h`:

```
10.0.0.1        your PC
10.0.0.10–11    FPGA-A, FPGA-B
10.0.0.20–29    Luckfox 1–10
10.0.0.40–54    ESP32-S3 1–15   (WiFi, DHCP reservation by MAC)
```

Teensys have no IP. They hang off a Luckfox's UART and are addressed as `10.0.0.2x:port` by the
Luckfox that owns them — the Luckfox is their network stack. That is the correct division of
labour: the Teensy should never spend a cycle on TCP.

---

## 2. What each node class is actually for

This is where I had it wrong in doc 10, and where you were right. The measured numbers:

| Node | int8 compute, from its **fast** memory | Fast memory | Big memory | Verified from |
|---|---|---|---|---|
| **ESP32-S3** (×15) | **1.4 GMAC/s per core** measured, ~2.4 dual-core | ~300 KB internal SRAM | 8 MB PSRAM @ 57 MB/s (R8 parts) | Espressif esp-dsp: 5.9 MAC/cycle on a 4096-MAC int8 dot product, PIE `EE.VMULAS.S8.ACCX` |
| **Teensy 4.1** (×9) | 1.2 GMAC/s @600, 1.63 @816 | **512 KB DTCM, single-cycle** | 16 MB PSRAM @ 45 MB/s | SMLAD, 2 MAC/cycle; §4 |
| **Luckfox A7** (×10) | **1.0 GMAC/s** measured | 32 KB L1 / 128 KB L2 | **64 MB DDR2** | ncnn mobilenet_int8, Cortex-A7 @1.2 GHz, 1 thread |
| **Luckfox NPU** (×10) | 0.5 TOPS nominal — **frozen models only** | — | shares the 64 MB | RKNN; weights are baked at conversion time |
| **FPGA PL** (×2) | ~30–50 GMAC/s practical | 220 DSP48 + BRAM | **1 GB DDR3** | 220 DSP48 @ ~200 MHz |

Read the totals off that table:

```
MCU array (9 Teensy + 15 S3):   ~32–50 GMAC/s   but only  8.1 MB of fast memory
Luckfox cluster (10):           ~10 GMAC/s      and       640 MB of real RAM + 10 NPUs
FPGA pair (2):                  ~60–100 GMAC/s  and       2 GB DDR3
```

**The ESP32-S3s are the fastest per-node int8 engines you own** — faster than a Teensy at 600 MHz —
and you have fifteen of them. **The Luckfoxes hold 640 MB of genuinely fast RAM**, eighty times the
entire MCU array's fast memory, and each one is a Linux box that can hold a whole layer without
tiling anything. Neither of those was reflected in doc 10. They are now.

### The two corrections that came out of checking

1. **The Luckfox A7 is not faster than a Teensy at GEMM.** ~1.0 GMAC/s measured, and the stock
   kernel clocks the RV1103 at **1104 MHz, not 1200** (`rv1103.dtsi` deletes the 1200 MHz OPP).
   Its value is the 64 MB and the Linux stack, not raw MACs.
2. **There is no numpy on a Luckfox and there cannot be.** The SDK toolchain is
   `arm-rockchip830-linux-uclibcgnueabihf` — **uClibc**, and Buildroot's numpy hard-depends on
   glibc or musl. No pip wheel exists for a uClibc interpreter either. So: orchestration stays
   pure Python, and every hot loop is C with NEON intrinsics built by the SDK toolchain and called
   through `ctypes`. Plan for that now, not after you try `pip install numpy` and it fails.

---

## 3. The algorithm this hardware is shaped for

A deep belief network trains **one layer at a time**. That is not a limitation here, it is the
whole reason this machine can work: while layer *k* trains, it needs nobody else's gradients. When
it finishes, it sends its **hidden activations** downstream and layer *k+1* starts.

Traffic on a layer boundary, per 100-sample batch, 500 hidden units, 1 byte each:

```
100 × 500 × 1 byte = 50 KB
    over 100 Mbit Ethernet  =  4 ms
    over WiFi (2.5 MB/s)    = 20 ms
```

Against a layer that takes seconds to train. **The network carries features, never weights and
never gradients.** This is why a 12.5 MB/s backbone is not a compromise — it is oversized for the
job, and it is why the FPGA's bandwidth was never the bottleneck it looked like.

**One node = one layer.** 34 compute nodes = up to 34 layers, or a wide layer split across several.
The layer goes where its weights fit:

| Layer size (int8) | Fits in | Runs at |
|---|---|---|
| ≤ 300 KB | **ESP32-S3** internal SRAM | 1.4–2.4 GMAC/s |
| ≤ 400 KB | **Teensy** DTCM | 1.2–1.6 GMAC/s |
| ≤ 16 MB | **Teensy** PSRAM, tiled | 45 MB/s bound |
| ≤ 40 MB | **Luckfox** DDR2 | ~1 GMAC/s |
| ≤ 1 GB | **FPGA** DDR3 | 30–50 GMAC/s |

---

## 4. The Teensys: 2 PSRAM each, and no FPGA link at all

**This is the decision that removes the hardest block from the critical path.**

Doc 10 had each Teensy reaching into FPGA DDR3 through a custom QSPI-slave bridge in the PL — six
wires, new HDL, 105 MHz timing on jumper wire, and nine Teensys sharing one FPGA. Compare it to
just filling both pads:

| | FPGA DDR3 window | **2× PSRAM on the pads** |
|---|---|---|
| Bandwidth | 30–47 MB/s, **shared** by 5 Teensys | **45 MB/s, private to that Teensy** |
| Capacity | large | 16 MB |
| Wires | 6 signal + 2 GND, 33 Ω each | **zero** |
| PL pins | 54 across both FPGAs | **zero** |
| New HDL | a QSPI slave → AXI bridge | **none** |
| Works before the FPGAs land? | no | **yes, today** |
| Aggregate across 9 Teensys | ~47 MB/s total | **405 MB/s total** |

The private-bandwidth column is the one that matters: nine independent 45 MB/s buses beat one
shared 47 MB/s bus by nine times, cost nothing, and need no Vivado.

**So: solder two ESP-PSRAM64H per Teensy on the underside pads. 16 MB memory-mapped per board,
144 MB across the array. The Teensy's FPGA link is deleted.** Teensyduino finds both chips at boot
with no code change — `startup.c` probes CS0, then probes CS1 and maps the second chip immediately
after the first, and `EXTMEM`/`extmem_malloc()` see one contiguous 16 MB pool.

Verified specifics, so the soldering goes right the first time:

- The **smaller pads are CS0** (`GPIO_EMC_24 = FLEXSPI2_A_SS0_B`) and are **mandatory for the first
  chip** — a single chip on the large pads is never detected.
- Large pads are CS1 (`GPIO_EMC_22 = FLEXSPI2_A_SS1_B`). Using them for PSRAM means **no QSPI flash
  chip** on that board, ever. Fine — that was for LittleFS, and the Luckfoxes hold the datasets.
- Teensyduino 1.62 clocks FlexSPI2 at **105.6 MHz** (`PODF(4)|CLK_SEL(3)`), not the 88 MHz that
  older documentation says. Do not plan against 132 MHz: the read-timing margin is **−0.4 ns** at
  132 (PSRAM tACLK 6 ns + RT1062 input setup 2 ns > the 7.58 ns period), and there is a documented
  case of a board passing memtest at 133 MHz and failing in service.
- Chip ID must read `0x5D0D`. Anything else and the core silently reports zero PSRAM.
- **Do not enable `AHBCR[PREFETCHEN]`.** PJRC leaves it off, and that is load-bearing: with
  PJRC's `BUFSZ(64)` a prefetch becomes a 512-byte fetch that holds CE# low for **9.8 µs**, over
  the PSRAM's **8 µs tCEM limit**, which blocks internal refresh and corrupts memory. As shipped,
  every transaction is a 32-byte cache line (~0.7 µs CE# low) and both the tCEM rule and the 84 MHz
  page-crossing rule are satisfied automatically.

Remaining chips: 60 − 18 = **42**. Six are solder-yield spares. The other 36 are the FPGA PSRAM
farm in §6 — phase 2, after the array is already training.

**PSRAM does not go on the Luckfoxes.** Its only path there is SPI0, 1-bit, ~6 MB/s, not
memory-mapped, so Linux cannot use it as RAM — the CPU can't execute from it and there is no MMU
path. As a block device it is slower than the microSD card already in the slot, and each Luckfox
already has 64 MB of real DDR2, which is 8× the capacity of one PSRAM chip at 100× the bandwidth.
Wiring PSRAM to a Luckfox makes it slower, not faster. The microSD is its storage tier; the DDR2
is its memory tier.

---

## 5. "Superposition": the bit-sliced ensemble

You asked for Shor's algorithm and for superposition in the codebase. Straight answer on the first
one: **Shor's speedup is quantum-only.** The exponent needs a real superposition of 2^n states;
simulating it classically costs 2^n memory, so Shor's on this machine would factor numbers more
slowly than trial division. It is not a fit and I am not spending your parts on it.

**The second one is real, it is buildable, and it is already half-built into your inventory.**

An RBM's units are *stochastic binary*. A unit does not hold a number — it holds a **probability**,
and each time you look, it collapses to 0 or 1. That is already the structure you were reaching
for. The implementation trick makes it literal:

> **Run 32 independent replicas of the network at once, one in each bit of a `uint32_t`.**

Bit *k* of every state word belongs to replica *k*. The network's state is then not a point but an
**ensemble of 32 simultaneous configurations**, evolving in parallel, in the same registers. You do
not read a value out of it — you **measure** it, by counting bits (`popcount`), and what you get
back is a probability. That is a genuine classical superposition-of-states, and it is exactly what
Gibbs sampling of a Boltzmann machine wants.

Why it is fast, not just cute:

| Operation on 32 replicas | Naive | Bit-sliced |
|---|---|---|
| AND a weight mask with a state | 32 ops | **1 op** |
| Sum contributions | 32 adds | **1 `popcount`** |
| Sample 32 units against a threshold | 32 RNG + 32 compares | **1 word of RNG + logic** |

And it lands on every chip you own:

| Chip | Replicas per operation | Why |
|---|---|---|
| Teensy 4.1 | **32** | 32-bit words, single-cycle `__builtin_popcount` on the M7 |
| ESP32-S3 | **128** | PIE's 128-bit Q registers |
| Luckfox A7 | **128** | NEON `VCNT` |
| FPGA PL | **thousands** | one adder tree per unit |
| **74HC fabric** | **8 per chip** | an AND gate *is* a multiplier here |

That last row is the one that matters for your lost-techniques thesis. With binary states and
**ternary weights {−1, 0, +1}**, a neuron's input is add / subtract / skip — no multiplier
anywhere. That is 74HC territory: '08 AND gates for the mask, '283 adders for the tree, '161
counters for the accumulation. Your 74HC chips stop being an I/O fabric and become **arithmetic**,
which is the first time in this project they have earned that. This is Gaines' stochastic computing
(1969), and it was abandoned because floating-point multipliers got cheap — not because it was
wrong. For a machine built from ternary weights and binary neurons it is the right representation,
and it is 1.58 bits per weight instead of 8, which is 5× more network in the same memory.

Implementation: `firmware/bench-one/shared/bench_bitslice.h` / `.c` — portable C99, one file,
no allocation, no floating point. Builds clean under `-Wall -Wextra` for Cortex-M7 and M4.

### What it has actually been measured doing

The kernel was mirrored line-for-line in Python and run, because a kernel that has never
executed is a guess. It learns, and getting there found three real bugs that would each have
looked like "the AI just doesn't work" on the bench:

| Test | Result |
|---|---|
| Memorise 4 random 36-bit patterns | **16/32 exact** deterministic reconstructions, 4.2% bit error |
| Bars-and-stripes, 124 patterns, held-out | **12/96 exact 36-of-36** reconstructions, **7.3% bit error** (chance is 50%) |
| Generative sampling of valid patterns | **0% — does not yet reach the data manifold. Open.** |

**The three bugs, because they are the kind that repeat:**

1. **`bs_rethreshold()` was never called in the training loop.** The shadow updated; the ternary
   planes that actually do the arithmetic never changed. Error sat at exactly chance.
2. **The sigmoid slope was fixed while activation spread scales with fan-in.** A 36-input layer
   had an activation spread of 2.5 against a table needing ±16 to move — every unit was a coin
   flip regardless of its weights. Fixed with a per-layer `gain`, set once from measured fan-in
   and *never* renormalised during training (renormalising caps the model's confidence forever).
3. **`sigmoid(0) == 128` and the deterministic rule was `p >= 128`,** so every unit with *no
   evidence* switched ON. In a sparse ternary network that is most units, so every reconstruction
   came out a solid block. One character: `>=` became `>`. This is the one that mattered most.

Also measured: **15% weight density trains better than 30%** — sparser is both more accurate and
faster here, since a zero weight costs nothing in the forward pass. That is now the default.

**Honest open item:** reconstruction works, generation does not — the persistent chain is not
settling on the data manifold. That is a training-dynamics problem (chain mixing, decay schedule,
threshold stability), not a wiring problem, and it does not block any hardware step. Bench task
B6 is where it gets solved, on real hardware, with real timing.

---

## 6. What the FPGAs do once the pins are free

With the cluster on Ethernet and the Teensys self-sufficient, **all 128 PL pins are unallocated**
and, more importantly, **the machine trains without the FPGAs at all.** They plug into the switch
and add capacity. Nothing waits on them. That is the "zero downtime when they land" answer: there
is no downtime, because there is no dependency.

In value order:

1. **A layer too big for any MCU** — 1 GB DDR3, 220 DSP48, 30–50 GMAC/s. This is the only place a
   100 M-weight layer can live in one piece.
2. **The bit-sliced engine in hardware** — §5 with thousands of replicas instead of 32. One adder
   tree per unit, no DSP needed, because ternary weights need no multiplier. This is the single
   highest-value PL block for this machine and it is *simpler* HDL than a QSPI bridge.
3. **The PSRAM farm** — 36 chips on 6 banks. 6 pins per bank (CLK + D0–3 + CS) plus 3 address pins
   into one of your 74HC138s selects 8 chips per bank: **8 pins per 8 chips, 48 pins for 36 chips**,
   6 independent buses × 42 MB/s = **250 MB/s of parallel bandwidth** the DDR3 can't match on
   scattered access, because it is six independent buses with fixed 6-cycle latency instead of one
   bus with row-activate penalties.
4. **The 74HC fabric's second home** — the simultaneous-capture front end from docs 03/04.

Order of work when they arrive: boot Puzhi's Linux → get on the switch → run the framed protocol
over TCP → *then* open Vivado. Steps 1–3 are hours and need no HDL at all.

---

## 7. The ATtiny85s stay off the network on purpose

Power sequencing and watchdog. 5 V native, internal 128 kHz watchdog oscillator that runs
independent of the CPU clock, brown-out fuse at 4.3 V. One heartbeat wire in from the orchestrator,
four zone-enable lines out. The reason it is not on Ethernet is the reason it exists: it has to
work when everything with an IP address has locked up. Full pinout in doc 10 §5.8.

---

## 8. Build order — nothing here waits on the FPGAs

| # | Step | Proves | Blocked by |
|---|---|---|---|
| 1 | Solder 2 PSRAM on one Teensy, run `extmem` memtest | 16 MB at 105.6 MHz | nothing |
| 2 | Repeat on all 9 | the hand-solder yield | nothing |
| 3 | Magjack one Luckfox, `&gmac` okay, rebuild dtb, `iperf3` | **the whole backbone thesis** | nothing |
| 4 | Repeat on all 10 | 10 Linux nodes on one switch | step 3 |
| 5 | Switch + PC + all 10 Luckfox, `ssh` to each | the cluster exists | step 4 |
| 6 | Bit-sliced RBM on one Teensy, 32 chains, reconstruction error falls | **the algorithm** | step 1 |
| 7 | Same kernel on one S3 (PIE, 128 chains) and one Luckfox (NEON) | one algorithm, three silicons | step 6 |
| 8 | Two layers on two Luckfoxes, activations over TCP | the DBN chain | steps 5, 7 |
| 9 | Scale to N layers across all 34 nodes | the machine | step 8 |
| 10 | FPGAs arrive: SD boot → switch → TCP → Vivado | capacity | none of the above |

Steps 1–9 are buildable with parts already in the building.

---

## 9. What to buy

| Item | Qty | Why | Rough |
|---|---|---|---|
| **16-port gigabit switch** | 1 | the backbone | $30 |
| **RJ45 magjack** (integrated magnetics) | 10 | one per Luckfox | $20 |
| 0 Ω 0603, 10 nF, 75 Ω, 1 nF 2 kV | — | the Bob Smith network, ×10 | $10 |
| CAT5e patch cables | 13 | | $20 |
| 5 V trunk supply, ≥ 18 A | 1 | doc 10 §4 | $25 |
| WiFi AP / router | 1 | for the 15 S3s (or use one S3 as SoftAP) | have one? |
| High-side switch modules **or** per-zone bucks | 4 | the D4184s you own are **low-side** and will break the shared ground — doc 10 §4 | $15 |

**Under $120 and the whole 38-node machine is wired.** No level shifters, no FPGA dependency, no
custom HDL, and the hardest remaining task is soldering a magjack ten times.

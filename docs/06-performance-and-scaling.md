# 06 — What this stack is actually fast at, and how to scale it

You said you want to stress-test it and optimise it "to perform like a machine", and that you are
coping because you want a Pi 5. Both halves get an honest answer here.

---

## Part 1 — The Luckfox versus a Pi 5

### The comparison you were bracing for

| | RV1103 (Luckfox Pico Mini B) | BCM2712 (Pi 5) |
|---|---|---|
| Cores | 1 × Cortex-A7 | 4 × Cortex-A76 |
| Usable RAM | ~34 MB | 8 GB |
| Integer, per core | ~4× slower | baseline |
| Perf per watt, general compute | ~630 7-zip MIPS/W | ~1135 MIPS/W |

It takes an estimated **14 to 20 Luckfoxes** to match one Pi 5's whole-machine 7-zip throughput.

That number is a fantasy, and it is worth understanding why rather than just discounting it. The
only interconnect between your nodes is a 921600-baud UART moving **92,160 bytes/second**. A Pi 5's
own measured `memcpy` runs at 5,270 MB/s. Your fabric between processors is about **57,000× slower
than the Pi 5's internal memory bandwidth.** Nothing that needs to share data parallelises across
that. The Luckfoxes would spend their lives waiting on each other.

There is no efficiency consolation prize either — the Pi 5 wins performance-per-watt at general
compute too.

### The part that actually settles it

**The RV1103 has no display controller.** No HDMI, no MIPI-DSI, no GPU, and no video *decoder*. It
has a video **encoder** and a camera input, and that is the shape of the chip: it is a camera SoC
with a CPU attached.

So it cannot render a desktop under any firmware. "Pi 5 replacement" was never a benchmark
question. That silicon is not on the die.

---

## Part 2 — What it wins at, by roughly four orders of magnitude

Not throughput. **Time.**

| | Measured |
|---|---|
| GPIO edge → first ISR instruction, 600 MHz Cortex-M7 | **11 cycles = 18.33 ns** (NXP) |
| Linux GPIO interrupt path, typical | ~2 µs average |
| Linux GPIO interrupt path, spikes | ~150 µs |

That is roughly an **8,000× determinism advantage**, and it is not something a Pi 5 can buy at any
price — it is a consequence of having an operating system. Add a scheduler, a page fault, or a
USB interrupt and the tail gets worse.

Two more real wins the Pi 5 simply does not have:

- **A hardware H.264/H.265 encoder** on the RV1103: 4 MP at 30 fps, 60 Mbps. The Pi 5 has no
  hardware encoder at all.
- **A 0.5 TOPS NPU** on the RV1103. The Pi 5 has no NPU.

So the honest framing is not "cheap Pi 5". It is: **a machine with a hard-real-time domain, a
dedicated radio, a hardware video encoder, an NPU, and 128+ addressable IO — for the price of a
Pi 5's power supply.** That is a different machine, and it does things the Pi 5 cannot.

---

## Part 3 — Stress tests that produce publishable numbers

The worker firmware already implements these. Every one runs byte-identically on the Teensy, the
Luckfox and the ESP32, which is the point — a comparison only means something if all three are
doing the same arithmetic.

| Job | Measures | Why this one |
|---|---|---|
| `nop` | Link + dispatch floor | The number every other result sits on top of |
| `echo` | Link round trip with payload | Separates transport cost from compute cost |
| `crc16` | Byte-serial, branchy integer | The M7's branch predictor versus the A7's |
| `sieve` | Integer, memory-bound | Where 34 MB and a small cache start to hurt |
| `float` | FPU throughput | The M7 has one; measure it honestly |
| `membw` | Memory bandwidth | The number that explains the sieve result |
| `sort` | Branch-heavy, cache-hostile | Separates a Cortex-M7 from a Cortex-A7 differently than arithmetic does |

```bash
python3 benchctl.py bench crc16 100000
python3 benchctl.py bench sort 4096
```

### The measurement discipline, which is the actual contribution

Every timing figure the firmware reports is a delta between **two reads of the same board's cycle
counter**. No figure is ever produced by subtracting one board's timestamp from another's.

Two free-running crystals differ by hundreds of parts per million and drift with temperature. A
cross-clock "measurement" reports that drift and calls it latency. The clock-offset probe
(`benchctl sync`) is explicit about this: it returns an offset with **an uncertainty of half the
round trip**, because the one-way delay cannot be split without more evidence.

Four things worth measuring that nobody has published for this combination:

1. **End-to-end latency, Luckfox command to fabric pin change.** Marker pin on the analyser at
   both ends. This is the number the whole architecture exists to make small.
2. **Jitter under load.** Same measurement while the radio is scanning and the worker is running
   a sieve. The mean will barely move; the tail is the interesting part.
3. **Sustained frames per second per link**, and where it breaks.
4. **Power per operation**, INA219 per rail, idle versus active versus faulted.

---

## Part 4 — The second stack

### How to link two stacks: the answer is Ethernet, between the Teensys

| Option | Verdict |
|---|---|
| **Teensy 1 ↔ Teensy 1 over Ethernet** | **Best.** 94 Mbit/s measured with lwIP, ~650 µs RTT. Needs two PJRC Ethernet Kits; the DP83825I has Auto-MDIX so a straight CAT5 works |
| Luckfox ↔ Luckfox over Ethernet | Possible but unmeasured. The RV1103 has a 10/100 MAC with embedded FEPHY and the Pico Mini breaks the pairs out to a 5-pad header, but it needs external magnetics and `gmac` enabled in the device tree |
| ESP-NOW between the two radios | Fine as a low-rate side channel. 5.6 ms median RTT for 12 bytes, 250 B per frame. **Never for timing or bulk** |
| Luckfox ↔ Luckfox over USB | **Impossible as shipped.** Both USB-C ports are hardwired as sinks — 5.1 kΩ pulldowns on CC1/CC2, VBUS feeds the system rail. Two boards cannot enumerate over a C-to-C cable without a butchered cable and external VBUS |
| Teensy ↔ Teensy over UART | Works, but it is 92 kB/s. Fine for control, useless for data |

Ethernet between the Teensys also doubles as the time-sync transport, which matters:

**PTP between two Teensy 4.1s is published at σ = 40.7 ns.** That is three orders of magnitude
better than anything the UART can offer, and it is what makes a two-stack measurement meaningful
rather than two separate measurements printed near each other.

### The Luckfox cannot be the time reference

The RV1103's GMAC appears to be synthesised **without the IEEE 1588 timestamp block**:
`MAC_HW_Feature0 = 0x400103E1`, with TSSEL (bit 12) = 0 and PPSOUTNUM = 0. No hardware
timestamping unit.

So **a Teensy must be grandmaster.** This is a genuinely useful thing to know before ordering
parts, because the intuitive plan — "the Linux boxes coordinate, the microcontrollers obey" — is
exactly backwards for timing.

### Addressing, and why the protocol did not change

The existing frame has no node address, and adding one would have meant touching the tested
parser. Instead a routed frame is **carried as the payload** of an ordinary point-to-point frame
on a new channel: the envelope names the destination, the inner bytes are a complete,
independently-CRC'd frame.

It costs 4 bytes per hop, needs no parser change, and a corrupted envelope cannot corrupt the
inner frame because the inner frame carries its own CRC. An older node that does not know the
channel returns `UNKNOWN_CHAN` — it fails loudly instead of mis-parsing a payload that looks
plausible.

Routing happens at the Luckfox, which is the only node with the memory and the scheduling freedom
to do it without hurting anyone's real-time behaviour.

---

## The claim worth planting a flag on

Nobody has published hard numbers for coordinating **this** family of cheap parts as one machine:
a Linux camera SoC, two hard-real-time M7s, a radio, and 128+ addressable IO on a decoded fabric,
with every link speaking one frame and every figure measured on one clock.

That is reachable with the parts on your desk, and none of it requires pretending the Luckfox is
something it is not.

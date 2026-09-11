# The discrete-logic thread — assessed against your actual box of chips

> *"lets experiment with ic chips fuck around and make a pc that can run a light linux or some
> shit some big some unheard of"*

You asked for scientific truth over hype. Here it is, and then the part that is actually good
news.

---

## 1. Linux is off the table, and it is arithmetic

Not pessimism, not a skill gap. Count the state.

Your entire usable 3.3 V inventory — 136 logic ICs plus 12 MCP23017 — contains **688 bits of
flip-flop and latch state.** That is 86 bytes.

| | Bits of state |
|---|---|
| **Everything you own** | **688** |
| Architectural register file alone of the smallest mainline-Linux-capable RISC-V core (VexRiscv "Linux balanced", RV32IMA + MMU) | 992 |

You are short before the register file is finished. Before the ALU. Before any RAM.

And the RAM is where it becomes absurd. The smallest documented no-MMU Linux boot on tiny silicon
is the Kendryte K210, and it needed **8 MB**. In 74HC373 packages:

| | |
|---|---|
| Packages required | 8,388,608 |
| Board area | ~1,762 m² |
| Leakage power, doing nothing | ~221 W |
| Solder joints | ~134,000,000 |

That is a warehouse of chips drawing a fifth of a kilowatt to sit idle.

### What the state of the art actually achieved

| Machine | Chips | Time | Result |
|---|---|---|---|
| Pineapple ONE | 230+ | 2 years | RV32I at 500 kHz. Does **not** run FreeRTOS, let alone Linux |
| Magic-1 | ~200 | years | 4.09 MHz, 22-bit physical address, real MMU. Runs **Minix 2**, not Linux |

Every project you have seen that "booted Linux on tiny silicon" — uARM on an ATmega1284p,
Linux/4004 — did it by **emulating an MMU CPU in software** on top of megabytes of bought DRAM.
The 4004 was not running Linux. It was running an emulator, at roughly one instruction every few
seconds, and the Linux ran inside that.

### You already own the Linux box

The Luckfox runs real Linux. That is its job in the stack. "A light-Linux PC" is not a thing you
need to build; it is plugged in.

---

## 2. What you can genuinely build

### Option A — a bit-serial CPU. **~17–20 chips, from stock, this weekend.**

A bit-serial accumulator or one-instruction-set machine processes one bit per clock instead of
eight or thirty-two in parallel. That collapses the chip count enormously.

| | |
|---|---|
| Chips from your existing stock | ~17–20 |
| Speed | 10,000–40,000 instructions/second |
| Historical peer | **PDP-8/S, 1966** — its TAD instruction took 36 µs |

This is the genuinely unusual option. Everyone builds an 8-bit parallel SAP; a working bit-serial
machine is rare, it is *understandable in one sitting*, and it costs you almost nothing you do not
already have. It is also honestly a computer — it fetches, decodes, executes and branches.

### Option B — a full SAP-style 8-bit CPU

Ben Eater class. Real, teachable, big. **You cannot build it from stock** — see the gap below.

### Option C — a coprocessor the Linux box commands

The architecturally interesting one, and the one I have to be straight with you about.

**For anything throughput-shaped, software wins outright.** A discrete LFSR at 4 MHz doing
CRC or hashing is beaten by the Teensy by a factor of about **240×**. Building a hardware hasher
out of 74HC parts is a demonstration, not an accelerator.

Discrete logic wins on exactly two things, and both are real:

- **Simultaneity.** Your 74HC165 chain can latch **152 inputs on one clock edge.** Not sampled
  quickly one after another — captured at one instant. No microcontroller with 152 free pins
  exists in your box, and one that polled them would smear the capture across milliseconds.
- **Jitter.** One RCLK edge updates 24 outputs with picoseconds of skew between them. A
  microcontroller writing three ports takes hundreds of nanoseconds and varies run to run.

If you build a coprocessor, build it for those. A 152-channel simultaneous logic capture front-end
is a real instrument, and it is a thing your stack could do that a Pi 5 with a USB analyser
cannot.

---

## 3. The inventory gap, exactly

This dominates every option above. You own **zero** of:

| Missing | Why it blocks things |
|---|---|
| **74HC86** (XOR) | No XOR means no adder carry, no LFSR, no parity, no comparator. You can make one from four 74HC00, but four packages per gate makes any real datapath impossible |
| **74HC74** (edge-triggered D flip-flop) | The '373 is a *level-sensitive latch*. A CPU needs edge-triggered state or its registers race their own clock |
| **74HC283** (4-bit adder) | No ALU without it, or without a great many XOR gates |
| **74HC161/163** (counter) | No program counter |
| **74HC245 / 74HC541** (octal tri-state buffer) | No shared bidirectional bus. Your only '245 is a 74LS245 and it is unusable at 3.3 V |
| **SRAM** (e.g. 62256) | No memory |
| **EEPROM** (28C64) | No microcode store |

Everything in that list is a few dollars. Nothing in it is exotic. But without XOR gates and
edge-triggered flip-flops, the box you have is an *IO expansion kit*, not a computer kit — which
is exactly what the fabric uses it as, and why the fabric works so well with it.

---

## 4. The GPU

You said you would find a way to wire one on, and that you are serious. The honest blocker is not
mechanical, and no amount of 3D printing changes it.

**A GPU speaks PCIe and nothing else. The RV1103 has no PCIe controller in the silicon.** Neither
does the Teensy, neither does the ESP32. There is no bus to adapt because there is no bus. This is
not a connector problem or a driver problem; the transceivers do not exist on the die.

The real path is a host that has PCIe — an RK3588 board, where people genuinely do run AMD cards
on mainline `amdgpu`. That is $100+, which you have ruled out, and I think you are right to rule
it out: the whole thesis here is cheap silicon in bulk pushed past what anyone documents. Bolting
a $100 board and a GPU onto it abandons the thesis to chase a number a desktop already wins.

There is a related fact worth knowing, because it changes what the Luckfox is for:

**The RV1103 has no display controller at all.** No HDMI, no MIPI-DSI, no GPU, and no video
*decoder*. It has a video **encoder** and a camera input. It is a camera SoC with a CPU attached.
It physically cannot render a desktop under any firmware, which is why "Pi 5 replacement" was
never on the table — not because it is slow, but because that silicon is not in it.

What it *does* have that a Pi 5 does not: a hardware H.264/H.265 encoder (4 MP at 30 fps,
60 Mbps) and a **0.5 TOPS NPU**. The Pi 5 has neither. If you want the Luckfox to do something a
Pi 5 cannot, that is the direction — not pixels.

---

## 5. My recommendation, since you asked for one

**Build the bit-serial CPU (Option A) as a self-contained sub-project**, and wire it as a
coprocessor on a fabric chip-select slot so the Luckfox can load and run programs on it through
Teensy 1.

Because:

- It costs ~18 chips you already own and no waiting for parts.
- A working bit-serial machine is genuinely rare — far more unusual than another SAP-1.
- Wired as a peripheral to a Linux box through a Teensy through a 74HC138 decoder, it is a thing
  **nobody has published**: a homebrew CPU as an addressable device on a heterogeneous MCU stack.
- Every claim it makes is measurable on equipment you own. Instruction rate on the analyser,
  power on the INA219, jitter against the marker pin.

That last point is what makes it publishable rather than just fun. Not "I built a CPU" — there are
hundreds of those — but "here is a discrete CPU characterised as a peripheral, with captures."

**But this is your call, and it is a sub-project.** It must not stall the fabric, which is the
mainline. Tell me which option and I will spec the exact parts list and the bring-up order.

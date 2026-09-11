# The 4051 as a look-up table — verified, and qualified

Round 1 of the research rated this its single best finding: a 74HC4051 with its select lines as
inputs and its eight channels tied to 0, 1, D or D-bar implements any four-variable Boolean
function, and driving those channels from a 74HC595 makes the truth table rewritable over SPI.

**Its verifier died before running.** I checked it myself, because I had already reported it.

---

## The attribution is correct

I pulled Lancaster's CMOS Cookbook and read the actual pages rather than trusting the citation.

Page 249, *Data-Selector Logic*, names the part explicitly:

> "With CMOS, we have a choice of conventional one-way selectors, such as the 4019/74C157, 4512,
> and 4539, or the two-way analog switches that include the **4051**, 4052, 4053, 4067, and 4097."

The folding technique is described on pages 250–251 exactly as claimed:

> "With our folding technique, we can build any of the 65,536 direct-logic, four-input gates by
> using only an eight-input selector. With one 16-input selector, we can build any of the over
> five billion five-input, direct-logic gates."

And Chart 3-2 confirms the numbers for this specific part:

| Device | Function | Direct | Folded | Residue | Truth tables per package |
|---|---|---|---|---|---|
| **4051 / 74C152** | 1-of-8 Analog Switch | 3 | 4 | 5 | 1 |

So: the technique is real, it is Lancaster's, and the 4051 is one of the parts he intended it for.

---

## What the research did not check, and what I found

**Note which device Lancaster actually draws.** Figures 3-25 and 3-26 both use a **4512** — a
digital 1-of-8 data selector with a buffered, restoring output. The 4051 appears in his *list* of
options, not in his schematics. That difference is the whole electrical story below.

### DC levels are fine

The switch passes the channel voltage to the common pin through its on-resistance. A 74HC input
leaks about 1 µA, so even 300 Ω drops 0.3 mV. Full rails arrive intact. **No problem here.**

### Speed is the real cost

Two effects, and only one matters.

The RC of 300 Ω against a 74HC input (~3.5 pF) plus wiring (~10 pF) is about **4 ns**. Negligible.

The 4051's own **switching time dominates completely**. The only bound that covers 3.3 V is the
2.0 V column, roughly **350 ns**. At 4.5 V it is nearer 60 ns typical, but that number does not
apply here.

| | Propagation |
|---|---|
| Real 74HC logic gate | 10–25 ns |
| 4051 used as a LUT | **~350 ns guaranteed bound** |

That is **fifteen to thirty-five times slower than a gate.** It is a LUT, and it is a slow one.

### The floating-node problem, which nobody raised

The 4051 is **break-before-make**. During a select transition every channel disconnects and the
common pin **floats**. A floating CMOS input on the following inverter is precisely the condition
this project's own rules forbid — it sits near threshold, oscillates, and draws shoot-through
current.

The proposed 100 kΩ pull-down does fix it, but check why: 100 kΩ against ~13.5 pF is a 1.35 µs
time constant, and the break window is ~350 ns. The node barely moves, so it **holds its previous
value** rather than drifting into the threshold band.

That is the right behaviour, but it means the output is stale during the transition. **This LUT
needs a settle time before its output can be read** — the same discipline the chip-select decode
and the analog mux tree already follow. Budget 500 ns.

### The rewritable version inherits a constraint

Driving the channel pins from a 74HC595 does work electrically. But the 595 chain shares MOSI and
clock with the fabric, so **the truth table can only be rewritten when no module is selected.**
Fine for reconfiguration between operations. Not usable as anything dynamic.

### Fan-out

One CMOS input, through the buffer. Lancaster says the same. He owns 24× 74HC04, so this is free.

---

## Honest verdict: revive-easy, not revive-better

The round 1 write-up called it "better than it ever was." **That overstates it, and by the
project's own test.**

The standing rule is that discrete logic only genuinely wins on simultaneity or jitter. This wins
on **neither** — it is slower than a real gate and its output is stale during transitions.

What it actually wins on is different and still worth having: **he owns zero XOR gates, zero
flip-flops and zero adders, and this gives him any four-variable function from parts already on
the desk, tonight, with nothing ordered.** Plus reconfigurability a fixed gate cannot offer.

So the honest framing:

- **Use it now** to unblock anything needing an XOR or an odd truth table, and to prove the
  technique on the analyser. It costs one 4051 and one inverter from stock.
- **Buy the 74HC86 anyway.** It is pennies, twenty times faster, needs no pull-down and no settle
  time. For any XOR in a real datapath it is simply the correct part.
- **Keep the 4051 LUT where reconfigurability is the point** — a trigger condition the Teensy can
  redefine in software, or glue whose function changes between experiments. There it earns its
  place, because a fixed gate genuinely cannot do it.

---

## What this says about the research process

The finding was real, the attribution was accurate, and the enthusiasm was unearned. That gap is
exactly what the adversarial verify pass exists to catch, and it caught nothing here because the
session limit killed that agent before it ran.

Worth remembering when reading anything from round 1 whose verifier is missing: **five of ten
verify agents died.** Their findings are sourced but unchecked.

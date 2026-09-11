# 07 — Parts and ports: what the research changed

Three findings from the verification pass. The first one reframes the whole fabric, the second is
a shopping list, and the third refutes part of my own port design.

---

## 1. The finding that reframes everything

**The 74HC family is specified nowhere at 3.3 V.**

Not "poorly specified". Not "specified in a footnote". The datasheet columns are 2.0 V, 4.5 V and
6.0 V, and nothing else. Verified directly in TI's SN74HC245 (SCLS095) and Nexperia's 74HC4051
Rev 12.

| VCC | What an HC bus driver guarantees |
|---|---|
| 4.5 V | ±6 mA |
| 3.3 V | **nothing at all** |
| 2.0 V | ±20 µA |

Every chip in the fabric — the '138, '595, '165, '373, '4051, '04 — is running at a voltage its
own datasheet declines to characterise. In practice HC at 3.3 V delivers 2–4 mA and it works
fine. But **no number backs it**, which means nothing can be predicted in advance or checked
against a specification afterwards.

That is not a reason to stop. It is a reason to measure, and it is a genuinely good reason for
this project to exist: the numbers nobody publishes are exactly the ones worth putting on a
logic analyser and writing down. It also means the buy list below is not shopping, it is
**closing the gap in the places where being unspecified actually bites.**

---

## 2. What to buy, in order

### The single best purchase — SN74LVC245AN, PDIP-20, about $1

Buy six.

It is the only DIP part on the list that is both specified at 3.3 V with real drive (24 mA,
6.3 ns propagation) **and 5.5 V-tolerant on its inputs while running at 3.3 V VCC.**

That second property is the enabler for the whole "any module plugs in" goal. Most hobby radio
and sensor breakouts are 5 V. The Teensy is 3.3 V only with a 3.6 V absolute maximum. One
one-dollar chip closes both the clock-fanout gap and the 5 V ingress gap, and nothing you
currently own can do either.

### The analog answer, and it is on a clock — SN74LV4051AN, PDIP-16

**Yes, a properly 3.3 V-specified 8:1 analog mux exists in DIP**, and it is a drop-in for the
74HC4051: same package, same pinout.

| | 74HC4051 (owned) | SN74LV4051AN |
|---|---|---|
| On-resistance at 3 V | **dashes — no number at all** | 30 Ω typ, 150 Ω at 25 °C, **190 Ω max over −40…85 °C** |
| Lowest real spec | 180 Ω max, but at 4.5 V | dedicated 3.3 V timing section |

This replaces an unmeasurable unknown with a guaranteed 190 Ω. For an analog front end that is
the difference between a design and a hope.

**The catch is severe: TI marks it Not Recommended For New Designs and it is out of stock at TI.**
It is a last-time-buy from distributor stock. If you want it, buy the lifetime supply in one
order — ten of them, about $1 each.

Two alternatives checked and rejected: ADG708/ADG728 exist in no DIP at all, and CD74HC4067E is
16-channel but still HC, so it inherits the identical no-3.3 V-spec problem.

### Clock fan-out — SN74AHC541N, not a '245

Buy four. The '541 is **unidirectional**, and that is the whole argument. A '245 is a
transceiver with a DIR pin, and a DIR pin that floats or glitches at power-up turns the buffer
around and drives back into a Teensy pin. On a 3.3 V-maximum part that is a real contention
hazard, for no benefit on a net that only ever goes one way.

### Two corrections to the earlier BOM

- **TPL7407LA does not exist in DIP.** SOIC and TSSOP only. The DIP answer is the Toshiba
  **TBD62083APG**, a DMOS array in P-DIP18 that is pin-for-pin identical to the ULN2803A.
- **The TCA9548A is unnecessary, and the reasoning is better than "not yet".** The address
  collision has *already* happened: twelve MCP23017 cannot coexist across eight addresses. But
  the fix is free rather than $7 — the Teensy 4.1 has **three hardware I²C ports**, Wire (18/19),
  Wire1 (17/16) and Wire2 (25/24), giving 24 expander addresses without a multiplexer. Two of
  those buses are already kept clear in the pin map.

**Tier 1 total: roughly $45.**

---

## 3. My port design was partly wrong

I proposed two auxiliary pins per SPI port: one interrupt on an MCP23017, one control line on a
74HC595. The count was nearly right. **The home was wrong, and I missed a structural problem.**

### The structural problem I missed

**The 74HC595 chain is fed from the same SPI1 MOSI and SCK as the modules.** So the control byte
cannot be shifted while any chip select is asserted — doing so clocks the module too.

That is fine for a reset, which happens outside a transaction. It rules out anything that must
change *during* one.

### Three aux, not two

Per-module counts against primary datasheets:

| Module | Aux needed | Notes |
|---|---|---|
| nRF24L01+ | 2 | CE out, IRQ in |
| CC1101 | 2 | GDO0, GDO2. **No reset pin at all** — reset is the SRES strobe |
| W5500 | 2 | INTn, RSTn |
| MCP2515 | 2 | INT, RESET |
| MCP3008 | **0** | It has no data-ready pin. My premise was wrong |
| 25Qxx flash | **0** | /WP and /HOLD are strap-high |
| MAX7219 | 0 | |
| MicroSD | 1 | Card detect, from the socket switch not the card |
| SX1276 / RFM95 | 2, or **3–4** under LoRaWAN | LMIC uses DIO0, DIO1, DIO2 |
| SX1262 / SX1268 | **3** | BUSY, DIO1, NRESET |
| ADS1256 | **3** | DRDY, RESET, SYNC |

### Four signals must never touch the expander

| Signal | Why | Correct home |
|---|---|---|
| Display D/CX | 10 ns setup/hold, flips *between bytes* inside one CS-low window | Native Teensy pin. Already is — pin 9 |
| SX126x BUSY | Sampled between NSS falling and the first clock edge on wake | Native pin, polled with a hard timeout |
| ADS1256 DRDY | 33 µs period at 30 kSPS | Native pin with a falling-edge interrupt |
| 25Qxx /HOLD | Acts only while CS is low | Tie high at the port, 10 kΩ |

The display consequence is worth stating plainly: **a fabric SPI port cannot host a display.**
Displays stay on SPI0 with DC on a native pin, which is what the build already does — but by luck
of an earlier decision, not because I had reasoned it through.

### The nRF24 near-miss, with numbers

Its CE line is *legal* on an expander: the minimum pulse is 10 µs with no maximum. But an
expander round trip costs about 240 µs of dead time per packet against roughly 165 µs of air
time. It works and it **more than halves throughput.**

### The expander interrupt ceiling

A two-byte MCP23017 read at 400 kHz is 48 bit-times, about 120 µs of bus time and 125 µs wall.
With a twelve-way wire-OR, identifying which chip fired takes 1–12 reads: roughly 750 µs
expected, 1.5 ms worst case.

**Hard ceiling near 1.3 kHz. Sane design ceiling around 330 Hz.**

And the datasheet's interrupt latch is one deep, so two events on the same 8-bit port inside that
window coalesce into one. Anything faster than a few hundred hertz needs a native pin.

---

## What this changes

Nothing that is built stops working. The corrections are to the *map*, not the code: the port
table gains a third auxiliary, four signal types get marked as native-pin-only, and the analog
layer gets a note that its on-resistance is unguaranteed until either measured or replaced with
an LV4051.

The 3.3 V specification gap is the one worth sitting with. Every one of those chips is being run
outside its characterised range, by everyone, everywhere, all the time — and almost nobody says
so. Measuring it properly on your own bench is a small, real contribution.

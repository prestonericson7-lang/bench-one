# BMW F30 / 330e — bus reference & how to tap it safely

Sourced from **BMW's own training documentation** (ST401 Body Electronics II — "Bus Systems"),
not forum guesswork. Everything marked ✅ is from that document.

---

## 1. The buses in an F-series BMW

| Bus | Rate | Wiring | Notes |
|-----|------|--------|-------|
| **K-CAN** (body) | **100 kbit/s** ✅ | twisted pair | Line topology. Comfort/body: lighting, seats, A/C. **Can run single-wire in a fault.** Woken via the bus — no separate wake wire. Has LIN sub-buses. |
| **K-CAN2** (body 2) | **500 kbit/s** ✅ | twisted pair, **60 Ω** ✅ | High-rate body bus. LIN sub-bus on all its modules; wakes via those sub-buses. CAS has a redundant link to DME (CAS-Bus, K-Bus protocol) for rapid start. |
| **PT-CAN** (powertrain) | **500 kbit/s** ✅ | **THREE wires** ✅ | 2 data + **a dedicated wake-up line** (third wire is wake only, not part of signalling). **All bus users connected in parallel.** ✅ |
| **PT-CAN2** | **500 kbit/s** ✅ | twisted pair + wake line, **60 Ω** ✅ | Redundancy for engine management; also fuel pump signals. |
| **F-CAN** (chassis) | 500 kbit/s ✅ | twisted pair | |
| **ICM-CAN** | 500 kbit/s ✅ | two-wire | Terminators (120 Ω each) live **inside the ICM and QMVH modules**. |
| **D-CAN** | diagnostic | — | The diagnostic bus (what a scan tool talks to). |
| **FlexRay** | **10 Mbit/s** ✅ | — | Chassis dynamics + engine control. "20× the rate of PT-CAN." Deterministic/TDMA. |
| **MOST** | 22.5 Mbit/s ✅ | fibre optic ring | Infotainment. |
| **LIN** | 9.6 kbit/s ✅ | single wire | Sub-bus hanging off K-CAN/K-CAN2 modules. |

**Gateway:** all buses are joined by the **central gateway (ZGM/ZGW)**, which on F-series is
integrated into the **FEM (Front Electronic Module)** and contains a star coupler with 4 bus
drivers. Nothing crosses between buses except through it.

---

## 2. Do you need 74HC logic chips to "split" the bus? **No.**

**CAN is already a multi-drop bus.** BMW's own wording: *"All bus users are connected in
parallel."* ✅ ISO 11898-2 defines it as a single linear trunk where each node taps on via a
**short stub**.

- **To add a node: wire its transceiver across the same CAN_H / CAN_L pair.** That's it.
- **Keep stubs short** (well under 0.3 m).
- **Node count is not your limit** — older transceivers capped around 32, modern
  high-input-impedance parts allow 100+. The real limit is combined transceiver loading, not
  a headcount. **You can scale to as many taps as you want.**
- **74HC logic chips would destroy it.** CAN is a *differential* bus with dominant/recessive
  arbitration; single-ended logic gates can't carry it and would wreck the signalling.

**Where your 74HC chips *are* useful:** single-ended digital work — GPIO expansion, buffering,
5 V↔3.3 V level shifting, multiplexing. Just never in the CAN path.

### ⚠ The mistake that will bite you: extra termination
The car already has **exactly two** 120 Ω terminators, one at each end of each bus.
**Most hobby CAN transceiver breakout boards ship with a 120 Ω resistor soldered on.**
If you tap the car with one of those still populated, you add a third terminator, the bus
drops below 60 Ω, and you overload the transceivers → intermittent faults that look like
everything except what they are.

**→ Remove or disable the 120 Ω resistor on every transceiver board you tap the car with.**

---

## 3. Identifying a bus with a multimeter — BMW's own procedure ✅

This is the no-guess way to confirm you've found a real CAN pair **before** connecting anything:

> Two 120 Ω terminators in parallel = **60 Ω measured between CAN_H and CAN_L.**
> *"When the supply voltage is switched off, this equivalent resistance can be measured
> between the data lines… Disconnect an easily accessible control unit from the bus, then
> measure the resistance on the connector between the CAN Low and CAN High lines."*

| Reading | Meaning |
|---------|---------|
| **~60 Ω** | ✅ A properly terminated, live CAN pair. This is your bus. |
| ~120 Ω | You're seeing only one terminator — you're at/near one end, or the bus is broken. |
| Open / very high | Wrong wires. |
| **< 60 Ω** | Over-terminated — something extra is on the bus (possibly your own board). |

Then confirm with the **logic analyser**: capture the pair and check the bit time matches the
expected rate (500 kbit/s → 2 µs/bit; 100 kbit/s → 10 µs/bit). Rate confirmed = you know which
bus class you're on.

### Voltage levels ✅
- **K-CAN (100 kbit/s)**: dominant bit = **3 V difference** between CAN-H and CAN-L.
  K-CAN Low to ground: **1–5 V**. K-CAN High to ground: **0–4 V**.
  (Note: BMW's low-speed K-CAN swings differently from textbook high-speed CAN.)
- **K-CAN2 / PT-CAN / any "high-speed" CAN**: scope the same way as PT-CAN. Values are
  approximate and shift a few hundred mV with bus load ✅.

---

## 4. What this means for the logger design

1. **Tap passively in parallel, hardware listen-only.** The Teensy logger
   ([firmware/car-can-logger](../firmware/car-can-logger)) brings both controllers up in
   FlexCAN `LISTEN_ONLY` — silicon-enforced silence, so it can never transmit, ACK, or
   error-frame the car's bus.
2. **No termination on our taps.** (See the warning above.)
3. **Scale is free.** Any number of additional nodes = more transceivers across the same pair,
   short stubs, no termination. No logic chips, no splitters.
4. **PT-CAN's third wire is a wake line** ✅ — useful: sensing it tells you the powertrain bus
   is waking, which is a cleaner sleep/wake trigger than watching for traffic.
5. **Buses sleep, and must be allowed to.** K-CAN/K-CAN2 wake via bus/sub-bus without a
   hardwire wake line. A device that keeps a bus awake drains the 12 V battery and throws
   unrelated faults. Our logger idles and closes its file when traffic stops.
6. **Some powertrain data is on FlexRay (10 Mbit/s), not CAN.** If a 330e signal can't be
   found on any CAN bus, that's likely why. FlexRay is TDMA and needs precise timing to
   decode — that's a Zynq PL job if we ever go there, not a Teensy one.

---

## 5. Open items to verify on the car (measure, don't assume)
- [ ] Which physical pairs behind the dash correspond to K-CAN / K-CAN2 / PT-CAN (60 Ω test + logic analyser bit-time).
- [ ] Exact OBD-II connector pin assignment on this VIN (F-series also carries ENET/Ethernet on the OBD connector; the diagnostic CAN is separate).
- [ ] Whether the 330e PHEV modules (electric machine electronics, battery management, charging electronics) broadcast on a CAN bus reachable behind the dash, or sit behind the gateway.

## Sources
- BMW ST401 Body Electronics II — *Bus Systems* (BMW technical training) — https://ia600902.us.archive.org/26/items/BMWTechnicalTrainingDocuments/ST401%20Body%20Electronics%20II/01_Bus%20Systems.pdf
- CAN termination & stub rules (ISO 11898-2 practice) — https://www.telewiretech.com/blogs/technical-resources/can-bus-wiring-stub-length-limits-termination-placement-and-daisy-chain-topology
- CAN multi-drop node limits / loading — https://tke.fi/can-bus-termination-explained-for-complex-installations/

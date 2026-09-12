# 51 — Chain build sheet: nine Teensys, one flash

*Wire this once. Flash every Teensy once, with the same file. After that the only board you ever reflash
is the head pair.*

## The rule this design exists to satisfy

Every Teensy runs the **identical binary** and works out where it is at power-up. Nothing is configured
at flash time — not the address, not how many PSRAM chips the node has, not whether it is first or last.
A node with eight chips and a node with two take the same image and neither is told anything.

That is what makes nine nodes maintainable. Reflashing the head Teensy and the head Luckfox changes the
behaviour of the whole chain, because the other eight only ever execute what they are sent.

## Wiring, node to node

Upstream is **Serial2 on pins 7 and 8** — the link the Luckfox already uses, unchanged. Downstream is
**Serial1 on pins 0 and 1**, going to the next node's Serial2.

| from node N | to node N+1 | signal |
|---|---|---|
| pin 1 (TX1) | pin 7 (RX2) | commands going down the chain |
| pin 0 (RX1) | pin 8 (TX2) | answers coming back up |
| GND | GND | required, one per link |

The head Teensy's pins 7 and 8 go to the Luckfox exactly as they do today. The last node's pins 0 and 1
go nowhere.

**One hazard, checked in the Teensy core and worth knowing.** Serial1 is LPUART6 and its *alternate*
pin pair is 52 and 53 — which on this build are PSRAM SIO0 and SCLK. The core defaults to pins 0 and 1
and this firmware never remaps it, so there is no conflict. Do not add a `Serial1.setRX`/`setTX` call.

Pins 0 and 1 are otherwise unused here: the PSRAM bus occupies 2, 3, 4, 5 and 48 through 54.

## Which node is which

All nine Teensys are pure workers. The head is simply the one the Luckfox is plugged into, and it holds
the 8-chip board because that is where the most memory should sit closest to the orchestrator. Every
other node carries the two chips on its own QSPI footprints.

    Luckfox ── Teensy 1 (8 chips, 64 MB) ── Teensy 2 (2 chips) ── ... ── Teensy 9 (2 chips)

Capacity: 64 MB on the head plus 16 MB across the other eight, so **80 MB**, which at one bit a weight
is **640 million parameters**.

## SD cards

Five cards go in the five Luckfoxes — each one boots from its card, so a Luckfox without one is not a
node. That uses five of the eight you have and leaves three spare.

The Teensys do not use SD cards in this design. They boot from their own flash and their weights live in
PSRAM, which is the whole point of the machine. Keep the three spares for Luckfox images.

## The protocol the chain adds

Two things on top of what already works, and the old unaddressed commands still behave exactly as before
so a single node on the bench is unchanged.

| | |
|---|---|
| `A 1` | sent once by the host. The first unnumbered node takes 1, offers 2 downstream, and so on until the chain runs out. The whole pile numbers itself. |
| `@<n> <cmd>` | run `<cmd>` on node n. Any other node passes the line down untouched and relays the answer back up, without interpreting or reformatting it. |

A node in the middle is a wire with a buffer. It never rewrites another node's traffic, because the
reply belongs to the host.

Discovery of how many chips a node has is *not* in the firmware. The host probes each node's ten selects
over the addressed protocol, which is the discovery that is already hardened and already knows the
difference between a chip answering `0D 5D` and an open bus. That is why the 8-chip node and the 2-chip
nodes need no distinction at flash time.

## Flashing, once each

    arduino-cli compile -b teensy:avr:teensy41:speed=816 firmware/bench-one/tests/psram_worker
    arduino-cli upload  -b teensy:avr:teensy41:speed=816 -p <port> firmware/bench-one/tests/psram_worker

816 MHz is the measured setting: it is stable under a clean port and worth 1.13x to 1.20x on the quad
banks, because batching has made the Teensy compute-bound and clock speed is close to linear in that
regime. Cooling is on the boards.

Do all nine, then wire the chain, then power up. From then on the head pair is the only thing that
changes.

## What to expect on first power-up

Each node prints two lines to its own USB port and then waits. Nothing runs unasked — no node touches
its PSRAM until the host tells it to, so a chain that is mis-wired is silent rather than damaging.

Send `A 1` and the reply tells you how many nodes answered. If it says fewer than nine, the break is at
the node after the last number returned.

## One thing that will bite, from experience today

Every serial port in this system tolerates exactly one reader. An orphaned process holding a port
produced garbled identity strings, replies welded together, "device reports readiness to read but
returned no data", and what looked for an hour like a hardware fault at 816 MHz. It was not. Before
blaming the chain, list who holds the port:

```bash
for d in /proc/[0-9]*; do for f in $d/fd/*; do readlink "$f" 2>/dev/null | grep -q ttyS3 && echo "$d"; done; done
```

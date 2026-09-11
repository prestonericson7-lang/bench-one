# 34 — The control plane, and why it is CAN

*Hardware on hand as of 2026-09-10: six SN65HVD230 transceiver modules and a complete BMW N20
harness. The sketch that measures this is `tests/can_bus_teensy/`, and it compiles.*

---

## 1. The machine has no way to talk about itself

Thirty-three nodes, and the only thing they exchange is work. No heartbeat, no health, no dispatch,
no way to tell a hung board to restart. Every measurement so far assumed a supervisor that does not
exist.

Ethernet could carry it. Every node would then need an address, a stack, and a switch port, and a
node that is powered off would have to be discovered by its absence rather than announced.

CAN gets all of it for one transceiver and two wires:

- **Multi-master with hardware arbitration.** No supervisor election, no collisions, no switch. The
  identifier is the priority, so a fault report beats a temperature reading by construction.
- **An unpowered node's transceiver goes high impedance.** Any board can be power-cycled without
  disturbing the bus. On a machine whose whole thesis is many cheap boards, that is not a detail.
- **A Teensy 4.1 already has three CAN controllers and no transceiver.** CAN1 on pins 22 and 23,
  CAN2 on 1 and 0, CAN3 with FD on 31 and 30. The modules are the only missing part.

## 2. It carries control and never carries work

| link | one 4 KB activation |
| --- | --- |
| CAN 2.0 at 1 Mbit/s | 57 ms |
| CAN FD at 5 Mbit/s | 8 ms |
| the Ethernet hop already measured | 0.148 ms |

That settles it. Activations stay where they are.

What the control plane actually needs, at 1 Mbit/s where an eight-byte frame costs about 127
microseconds:

| traffic | frames per second |
| --- | --- |
| heartbeat, one per node at 10 Hz | 330 |
| health telemetry, two per node at 1 Hz | 66 |
| dispatch and completion, two per token | a few hundred |
| ceiling at 1 Mbit/s | about 7800 |

Roughly a tenth of the bus. It fits several times over.

## 3. Wiring, per node

| module | Teensy |
| --- | --- |
| 3V3 | 3.3 V, never 5 V |
| GND | GND, common across every node |
| CTX | pin 22 |
| CRX | pin 23 |
| CANH, CANL | the twisted pair |

Use twisted pair for the H and L wires. The harness is full of it, and noise rejection is the entire
reason CAN is differential.

## 4. The one failure that is nearly guaranteed

Every SN65HVD230 module ships with a 120 ohm terminator fitted. A CAN bus takes exactly two, one at
each physical end.

Six modules on one bus is six terminators in parallel, which is 20 ohms. The transceiver is
specified down to 45. It will not drive it, and the failure is not silence: the bus half works, the
error counters climb, the controller drops to error-passive and then bus-off.

**Remove the terminator from all but the two modules at the ends of the run.**

If errors persist with termination correct, the next suspect is the RS pin. It sets slew rate, and
ten kilohms to ground is common on these boards and will not survive 1 Mbit.

## 5. The identifier map

Lower identifiers win arbitration, so this ordering is the priority ordering.

| identifier | meaning |
| --- | --- |
| 0x001 | discovery request, supervisor to all |
| 0x010 + node | heartbeat: uptime, temperature, free memory |
| 0x080 + node | discovery reply |
| 0x100 + node | ping |
| 0x200 + node | pong |
| 0x700 | bulk and background, deliberately lowest |

## 6. What to measure when it is wired

The sketch does four things and prints the CAN error counters after each, because those counters are
the fastest way to find a wiring fault:

1. Round-trip latency on a quiet bus, with min, median, p99 and max.
2. Sustained frame rate, which is the real ceiling rather than the arithmetic one.
3. The same latency test with the bus loaded, because a control plane whose latency triples under
   traffic is not one.
4. Whether anything acknowledged at all. A CAN frame needs one other node to acknowledge it, so a
   bus of one cannot transmit, and that shows up as the ACK flag with the transmit error counter
   climbing.

Set `NODE_ID` to 0 on one board and 1 on another, flash both, and open the serial monitor on the
first.

```
firmware\bench-one\tests\can_bus_teensy\build.bat
```

The jitter figure is the one that decides whether an ESP32 can sit on this bus at all. Wifi jitter
on those parts was measured at 0.55 ms, which is nothing against a 100 ms heartbeat and fatal
against anything timed finer.

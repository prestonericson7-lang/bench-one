# Wiring: Orange Pi 4 Pro + PZ7020 FPGA + Teensy 4.1

## What plugs into what

| From | To | Cable |
|---|---|---|
| Orange Pi **Ethernet** port | FPGA **PS Ethernet** jack (see below) | normal Ethernet cable |
| Orange Pi **USB-A** port | Teensy **micro-USB** | USB **data** cable (not charge-only) |
| Teensy pins | FPGA header **JM1** | 19 signal jumpers + 4 ground jumpers (table below) |
| FPGA **HDMI** | a monitor / TV | HDMI cable: this is the GPU's screen |
| 5 V USB charger, 1 A or more | FPGA **upper** Type-C, silkscreen `JTAG` | USB-C cable: powers the FPGA |
| Orange Pi's own supply | Orange Pi | as you use it now |

- The FPGA boot jumper goes on **SD**, with the SD card in the FPGA's slot (underside).
- The Teensy gets its power from the Orange Pi over USB. Never connect Teensy VIN or 3.3V to the FPGA, and never connect FPGA JM1 pin 1 (5 V) or pin 2 (3.3 V) to the Teensy. **Only ground and the signal wires connect the two boards.**
- Don't power the FPGA from its Type-C and its header 5 V pin at the same time. Those two are hard-wired together.

## Teensy 4.1 to FPGA JM1 (19 signals)

The FPGA side is 3.3 V and the Teensy is 3.3 V, so the wires go direct with no resistors or level shifters.

| JM1 pin | FPGA ball | Signal | Teensy pin |
|:---:|:---:|:---|:---:|
| 9  | E18 | D0  | 19 |
| 10 | F16 | D1  | 18 |
| 11 | E19 | D2  | 14 |
| 12 | F17 | D3  | 15 |
| 13 | G17 | D4  | 40 |
| 14 | B19 | D5  | 41 |
| 15 | G18 | D6  | 17 |
| 16 | A20 | D7  | 16 |
| 17 | D19 | D8  | 22 |
| 18 | C20 | D9  | 23 |
| 19 | D20 | D10 | 20 |
| 20 | B20 | D11 | 21 |
| 21 | J18 | D12 | 38 |
| 22 | K19 | D13 | 39 |
| 23 | H18 | D14 | 26 |
| 24 | J19 | D15 | 27 |
| 25 | K17 | SOR (start of record) | 3 |
| 26 | M17 | STROBE | 2 |
| 27 | K18 | BUSY (FPGA to Teensy) | 4 |
| **3, 4, 33, 34** | GND | **ground: use all four** | any **GND** pins on the Teensy |

Tips:
- Keep the jumpers **20 cm or shorter**, and run the ground wires alongside the bundle rather than off to one side.
- JM1 pins 5 and 7 are left free on purpose; your fan design uses them.
- The 40-pin headers can come unpopulated from the factory. If JM1 has bare holes, solder a 2x20 header first.

### Finding JM1 and its pin 1

Turn the FPGA board so the **Ethernet jacks are on the right** and the **USB-C ports are on the left**:
- **JM1 is the header along the top edge.** JM2 is along the bottom edge.
- **JM1 pin 1 is the square pad at the left end of the inner row** (the row nearer the middle of the board).
  The odd pins (1, 3, 5 ... 39) run left to right along the inner row. The even pins (2, 4, 6 ... 40) run along the outer row, with pin 2 next to pin 1.
- Your board repo has a printable sheet showing every pin: `D:\pz7020-starlite\docs\PZ7020-StarLite-pinout-sheet.pdf`.

Before you connect anything, check with a meter: pin 1 reads 5 V and pin 2 reads 3.3 V to pin 3 (GND) while the FPGA is powered.

## Which FPGA Ethernet jack

The board has two RJ45 jacks. Only the **PS** one works here, because the other belongs to the fabric side. The documents don't say which physical jack is which, so test it:
1. Plug the cable into one jack and power everything up.
2. On the Orange Pi, run `ping 10.77.0.2`.
3. If there's no reply after about a minute, move the cable to the other jack.

## Safe power-up order

Any order is safe. The Teensy keeps its bus pins switched off until it sees a live, configured FPGA driving BUSY low, so it never pushes current into an unpowered FPGA. The smoothest start is:
1. Power the FPGA. The HDMI screen shows **colour bars** once the GPU bitstream loads, then turns **dark blue** when the GPU daemon is running.
2. Power the Orange Pi, with the Teensy plugged into it.

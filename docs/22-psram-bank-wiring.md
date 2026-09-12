# 22 — Seven PSRAM chips on one Teensy: the wiring

> **Corrected by document 45.** The 39.3 MB/s here is the portable scalar kernel: a Teensy 4.1 is
> a Cortex-M7 and `gguf_dot.c`'s hand-written vector path was NEON, which never applied to it. A
> Cortex-M7 DSP path measures **66.97 MB/s on Q4_K and 67.69 on Q6_K, 1.60x on the real 69/31 mix**,
> bit-identical to the reference. Figures derived from 39.3 below are therefore low by that factor.

Two chips already on the underside pads, five new ones added as a stack. 56 MB on one Teensy 4.1.

Checked against the ESP-PSRAM64H and 74x138 datasheets on 2026-09-10. Corrections from that check are
marked **CORRECTED**.

---

## 1. The whole thing on one page

```
                    TEENSY 4.1, UNDERSIDE
        +-------------------------------------------+
        |                                           |
        |   [footprint 1]            [footprint 2]  |
        |    chip A                   chip B        |
        |    UNTOUCHED                pin 1 LIFTED  |
        |    8 MB on SS0              pins 2-8 stay |
        |         |                        |        |
        +---------|------------------------|--------+
                  |                        |
           Teensyduino                     |  pins 2-8 are the BUS TAP
           handles it,                     |  8 wires leave here
           nothing to do                   |
                                           |
        CE# PAD of footprint 2 -------------+------> decoder G2A# (pin 4)
        (the pad chip B's pin 1 left behind.  This pad carries the real SS1)
                                           |
                     +---------------------+-------------------+
                     |  SCLK  SIO0  SIO1  SIO2  SIO3  VCC  VSS |
                     |   (6)   (5)   (2)   (3)   (7)   (8)  (4) |
                     +----+-----+-----+-----+-----+----+----+---+
                          |     |     |     |     |    |    |
                          |     |     |     |     |    |    |     all shared,
                          v     v     v     v     v    v    v     every chip
        ============ THE STACK, 5 NEW CHIPS ========================
                                                                    soldered
          chip G  [1]  2  3  4  5  6  7  8      pin 1 -> Y5         column to
          chip F  [1]  2  3  4  5  6  7  8      pin 1 -> Y4         column,
          chip E  [1]  2  3  4  5  6  7  8      pin 1 -> Y3         dot to dot
          chip D  [1]  2  3  4  5  6  7  8      pin 1 -> Y2
          chip C  [1]  2  3  4  5  6  7  8      pin 1 -> Y1
        ===========================================================
          chip B  [1] lifted, wired out         pin 1 -> Y0
                  (still soldered by pins 2-8 on footprint 2)

          [1] = pin 1 bent OUT, its own wire. Everything else columns down.
```

```
                     74LVC138A   3-to-8 DECODER
                    +---------------------------+
   Teensy pin 2 --->| 1  A                 VCC  |16 <--- 3.3 V
   Teensy pin 3 --->| 2  B                 Y0   |15 ---> chip B pin 1 (lifted)
   Teensy pin 4 --->| 3  C                 Y1   |14 ---> chip C pin 1
  footprint2 CE# -->| 4  G2A#              Y2   |13 ---> chip D pin 1
            GND --->| 5  G2B#              Y3   |12 ---> chip E pin 1
          3.3 V --->| 6  G1                Y4   |11 ---> chip F pin 1
         (unused)   | 7  Y7                Y5   |10 ---> chip G pin 1
            GND --->| 8  GND               Y6   |9      (unused)
                    +---------------------------+

   10k from pin 1 to GND          these three are NOT optional, see section 5
   10k from pin 2 to GND
   10k from pin 3 to GND
```

**G2A# is the whole trick.** The decoder only pulls an output low when that enable is low. Tie it to
the Teensy's real SS1 and the chip select your PSRAM sees is the FlexSPI2 controller's own signal,
with the controller's own timing, steered to one chip of six. You are not faking a chip select in
software, you are routing a real one.

## 2. The wire list, which is what you actually work from

| # | from | to | notes |
|---|---|---|---|
| 1 | footprint 2 pin 6 (SCLK) | stack pin 6 column | **22 Ω in series, at the Teensy end** |
| 2 | footprint 2 pin 5 (SIO0) | stack pin 5 column | |
| 3 | footprint 2 pin 2 (SIO1) | stack pin 2 column | |
| 4 | footprint 2 pin 3 (SIO2) | stack pin 3 column | |
| 5 | footprint 2 pin 7 (SIO3) | stack pin 7 column | |
| 6 | footprint 2 pin 8 (VCC) | stack pin 8 column | |
| 7 | footprint 2 pin 4 (VSS) | stack pin 4 column | |
| 8 | a ground, any | run loose **inside** the bundle | the return path, section 7 |
| 9 | footprint 2 **CE# pad** | decoder pin 4 (G2A#) | the pad, not the lifted leg |
| 10 | chip B pin 1, lifted | decoder pin 15 (Y0) | |
| 11–15 | chips C–G pin 1 | decoder pins 14, 13, 12, 11, 10 | Y1 to Y5 |
| 16 | Teensy pin 2 | decoder pin 1 (A) | plus 10k to GND |
| 17 | Teensy pin 3 | decoder pin 2 (B) | plus 10k to GND |
| 18 | Teensy pin 4 | decoder pin 3 (C) | plus 10k to GND |
| 19 | decoder pin 5 (G2B#) | GND | |
| 20 | decoder pin 6 (G1) | 3.3 V | |
| 21 | decoder pin 16 | 3.3 V | 100 nF to GND right at the pin |
| 22 | decoder pin 8 | GND | |

Wires 1 to 7 and 9 to 15: **under 50 mm, all the same length.** 30 AWG wire-wrap.

## 3. ESP-PSRAM64H pinout

Dot in the corner marks pin 1. Numbering runs down that side and back up the other.

| pin | name | in the stack |
|---|---|---|
| 1 | CE#, active LOW | **its own wire, one per chip** |
| 2 | SIO1 | columned |
| 3 | SIO2 | columned |
| 4 | VSS | columned |
| 5 | SIO0 | columned |
| 6 | SCLK | columned |
| 7 | SIO3 | columned |
| 8 | VCC | columned |

## 4. Why both onboard chips stay, and why one loses its pin 1

Footprint 2's CE# pad is hardwired to SS1. Leave chip B as it is and it answers at the same instant as
whichever stacked chip you selected: two sets of output drivers fighting over four data lines, which
gives garbage that looks like data and is not good for either part.

You do not have to desolder it. **Lift pin 1 only.** Heat that one corner leg with a fine tip, lift it
clear of its pad with tweezers, wire it to Y0. The chip keeps its other seven joints and its place on
the bus, and its chip select now comes from the decoder like every other banked chip. The pad it left
behind is where you pick up SS1.

Do not substitute a spare GPIO for the chip select. The timing has to come from the FlexSPI hardware,
and the 8 microsecond limit in section 9 is why.

## 5. The three pulldowns are not optional

Teensyduino probes the QSPI bus before your code runs, so whichever bank the address pins happen to
select at power-on is the one it finds. Floating pins can select an unconnected decoder output,
nothing answers on SS1, and the core reports 8 MB instead of 16. That looks exactly like a bad solder
joint and will send you reflowing a stack that was fine.

## 6. CORRECTED: use the LVC part, this is not a preference

The earlier version of this document said a 74HC138 would "probably still work". That was too soft.

| part | propagation delay at 3.3 V | against one 88 MHz clock of 11.4 ns |
|---|---|---|
| 74HC138 | about 30 ns | nearly three clocks late |
| 74LVC138A | about 5 ns | under half a clock |

The decoder sits only in the chip-select path. SCLK goes straight through, so an HC part delivers CE#
up to three clocks after the controller thinks it asserted it, and the first read of every burst is
taken while no chip is listening. **Get the 74LVC138A.** If all you have is an HC, halve the FlexSPI2
clock and expect 16 MB/s instead of 33.

## 7. Breadboard: the decoder yes, the fast seven no

Delay is not the reason. A hundred millimetres of wire delays a signal about half a nanosecond against
an 11.4 ns clock, and nothing notices.

The reason is the **return path**. Every signal needs a ground beside it to come back along. A
breadboard gives a signal wire no ground neighbour, so current goes out along the wire and returns by
whatever route it can find, and that loop is an inductor. Inductance plus the two or three picofarads
in every breadboard contact makes the signal ring, and ringing at the wrong moment reads as the wrong
bit. Not every time. Occasionally.

**Occasionally is the worst possible failure.** A dead bank you find in a minute. A bank that is wrong
one time in ten thousand passes every test you are likely to run, gets trusted, and then a model
quietly produces slightly wrong numbers forever.

| on the breadboard | keep off it |
|---|---|
| the 74LVC138A itself | SCLK |
| the three 10k resistors | SIO0, SIO1, SIO2, SIO3 |
| 3.3 V and ground rails | the six chip selects, ideally |

The three address pins and the two enables are slow static signals and do not care about any of this.
The clock and the four data lines are the whole problem.

## 8. Passives

- **100 nF** from pin 8 to pin 4 on every chip, as close as it goes. One per chip across the VCC and
  VSS columns of the stack. Marked `104`.
- **10 µF** anywhere on the 3.3 V rail feeding the stack.
- **22 Ω in series on SCLK**, at the Teensy end, marked `220`. Damps the reflection from hanging six
  loads on one clock line. Leave it out and the symptom is occasional wrong bytes, not a dead board,
  which is far harder to find.
- **100 nF** at the decoder's pin 16.

Current is not a concern: one chip is selected at a time, so about 35 mA active plus a milliamp of
standby across the others. The 3.3 V regulator has 250 mA spare.

## 9. tCEM, which is already handled

The ESP-PSRAM64H is DRAM inside. Chip select may not stay low longer than **8 microseconds** or it
stops refreshing and loses data. At 33 MB/s that is 264 bytes, so no single read may exceed 256.
Teensyduino's FlexSPI2 setup already respects this for the onboard chips, and because the controller
believes the whole bank is one ordinary chip, you inherit it for free. This is the same limit that had
the Luckfox driver reading 1 KB in 5 milliseconds before it was fixed.

## 10. THE TRAP: the cache will lie to you

The upper 8 MB of `EXTMEM` is a window, and whichever bank the three GPIO select is what appears in it.
Switch banks without telling the cache and the processor hands you the **previous bank's data** out of
cache, with no error. Plausible wrong numbers, which is the worst failure mode there is.

```c
static inline void psram_bank_select(uint8_t bank)
{
    /* BEFORE the select changes, so nothing written to the old bank is still in cache and
     * nothing read from it can be served to a caller who now means a different chip. */
    arm_dcache_flush_delete((void *)0x70800000, 8u * 1024u * 1024u);

    digitalWriteFast(2, (bank >> 0) & 1);
    digitalWriteFast(3, (bank >> 1) & 1);
    digitalWriteFast(4, (bank >> 2) & 1);

    __asm__ volatile("dsb" ::: "memory");   /* let the decoder settle before the next SS1 */
}
```

**CORRECTED, with the arithmetic this document was missing.** That call walks the address range, not
the cache, so it issues one maintenance operation per 32-byte line: 8 MB over 32 bytes is 262,144 of
them, roughly 1.3 ms at 600 MHz. A 50.1 MB layer needs six window switches, so about 8 ms per token
against 2.75 s spent reading the layer. **The flush is 0.3% of the cost and does not matter.** The
earlier version called it "not free" and left you to guess; it is cheap, and `psram_bank_teensy`
measures the real figure rather than this estimate.

If it ever does matter, flush only the range you touched. The D-cache is 32 KB, so walking 8 MB of
addresses is 256 times more work than the cache could possibly be holding.

## 11. Build order, so a mistake is cheap

1. Stack and solder the five new chips. Nothing attached to the Teensy yet.
2. **Continuity check before power.** Every pin 1 isolated from every other pin 1 and from pins 2 to 8.
   Pins 2 to 8 each continuous through all five. **Pin 4 to pin 8 must NOT beep** — that is a short
   across the power rail and it would be the last thing the stack ever did.
3. Wire the decoder on the breadboard. Meter it: setting the three GPIO must pull exactly one Y low.
4. Lift chip B's pin 1. Wire it to Y0. Wire the CE# pad to G2A#.
5. Only then solder the seven bus wires from footprint 2 to the stack.
6. Power up and run `tests/psram_bank_teensy`.

## 12. How you know it worked

**Identity.** Each chip has a unique 8-byte ID at command `0x9F`. Six different IDs means six chips and
a working decoder. Six identical IDs means the decoder is stuck and you are talking to one chip.

**Pattern, all banks.** Write a distinct pattern to every bank, then read them all back **after** the
last write. Writing and immediately reading one bank passes even with the decoder disconnected, because
the data never left cache. Writing all six first is what makes the test real, and it is how the sketch
is built.

**Bandwidth.** Expect close to the 32.8 MB/s already measured on a single onboard chip. Half that means
the clock: the bus now carries six loads instead of one. Drop FlexSPI2 to 60 MHz, confirm stability,
work back up.

The soak loop runs the whole cycle twenty-five times and separates the two failures. Errors in the same
place every pass is a wiring mistake and slowing down will not help. Errors that move, with some passes
clean, is ringing.

## 13. CORRECTED: what 56 MB is actually worth

Capacity up 3.5 times, bandwidth unchanged, because one chip answers at a time. Ten chips would be
80 MB and still 32.8 MB/s.

Two numbers the earlier version conflated:

| | |
| --- | --- |
| raw read of a 49.2 MB layer at 32.8 MB/s | 1.5 s |
| **decode cost of that layer at 18.2 MB/s effective** | **2.75 s** |
| the same layer on a Zynq at 2290 MB/s | 21.5 ms |

The effective figure is the one that counts, because decode has to unpack as well as read, and on a
processor those costs add rather than overlap. 18.2 MB/s is the measured harmonic sum of 33.9 read and
39.3 unpack.

And the honest consequence, which the planner now prints: **a seven-chip Teensy becomes the first
microcontroller here that can hold a whole transformer layer, and the placer still will not give it
one.** A Zynq does fifteen layers in 434 ms. Handing one layer to a Teensy makes the slowest stage in
the pipeline 2.75 s, which is 6.3 times worse for the whole machine.

So what is it for:

- **one expert of a mixture-of-experts model.** An expert is 2.92 MB measured, so 56 MB holds nineteen,
  and nine Teensys reading their own banks at once is 305 MB/s aggregate against 35 MB/s from the SSD.
  That is the use with real arithmetic behind it.
- **a pipeline stage you can actually bring up and verify** without a Zynq in the loop.
- **the slow half of a KV cache**, which is plain int8 and needs no unpacking.

And for bulk capacity the 4 GB card already in the socket is 80 times the storage for none of the
soldering. Reach for PSRAM when something is read every token, and for the card when it is read
occasionally.

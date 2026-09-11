# usblink_bench — can a Luckfox actually feed a Teensy?

One unmeasured number decides an architecture. The proposal is that a Luckfox holds the model and does
all the talking, so a Teensy does nothing but matrix arithmetic. Whether that helps or hurts depends
entirely on how many megabytes per second crosses the USB link between them, and nobody has measured
it.

## What it has to beat

| path | effective rate |
|---|---|
| Teensy reading its own PSRAM | 18.2 MB/s, measured |
| Teensy reading DDR3 through the FPGA, `cfgB` | 15.2 MB/s, calculated |
| Teensy nibble unpack, the ceiling | 39.3 MB/s, measured |

Below roughly 20 MB/s, forwarding a weight over this link is slower than the Teensy fetching that
weight itself. The Luckfox would then be a bottleneck wearing the costume of a helper, and its value
would have to come from owning the protocol rather than from moving bytes.

Both ends are USB 2.0 High Speed, so 60 MB/s is the wire ceiling and bulk CDC lands well under it.
That gap is the question.

## Running it

```bash
arduino-cli compile -b teensy:avr:teensy41 usblink_teensy && arduino-cli upload -b teensy:avr:teensy41 usblink_teensy
```

The host program is already cross-compiled for the Luckfox as `usblink_host_arm`. Push it over and run
it:

```bash
adb push usblink_host_arm /root/ && adb shell chmod +x /root/usblink_host_arm
```

```bash
adb shell /root/usblink_host_arm /dev/ttyACM0 8
```

To build it yourself on any Linux box:

```bash
cc -O2 -Wall -o usblink_host usblink_host.c
```

## The trap that ruins the measurement

A Linux CDC-ACM port comes up as a **terminal, not a pipe.** In its default line discipline it
translates newlines, expands tabs, and treats 0x11 and 0x13 as flow control. Binary data arrives
altered, and a throughput figure measured over a corrupting channel is worse than no figure at all.

`cfmakeraw()` plus explicit `VMIN`/`VTIME` is not tuning, it is the difference between a measurement
and a fiction. The Teensy verifies every byte it receives for exactly this reason. **If it reports
mismatches, this is almost certainly why.** Baud rate is meaningless on USB CDC; the setting exists
because termios demands one.

If there is no reply at all, something else is holding the port. On most Linux images that is
ModemManager.

## What gets reported

- **host to Teensy**, which is the direction that matters because it is weights arriving. Timed on
  both clocks, because two clocks measuring one transfer disagree by the command latency and showing
  both makes that visible rather than arguable.
- **Teensy to host**, results leaving. Small in the real workload. The two directions use different
  endpoints and different host code paths, so they are not expected to match.
- **round trip over 1000 pings**, which prices the per-message cost rather than the per-byte cost.
  This is what decides whether the Luckfox can serve on demand or has to push in bulk. Compare against
  the 148 µs measured for a USB-Ethernet hop in `hop_bench.c`.

The program ends with a verdict computed from the inbound figure, not asserted in advance.

## See also

`../offload_teensy/` is the other half of the same question, and it runs on one board. It measures
whether taking work off a Teensy makes its memory faster, which is a different claim from the one this
directory tests. The short version: the memory does not get faster, but read and compute currently add
rather than overlap, and fixing that needs eDMA rather than a second board.

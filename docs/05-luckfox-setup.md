# 05 — Luckfox Pico Mini B setup

Everything needed to make the Linux node talk to Teensy 1. Nothing in the stack works until
`/dev/ttyS3` exists, so this is the first bench session.

---

## Do these two things before connecting any wire

### 1. Put 100 Ω in series on both link wires

Not optional, and not caution for its own sake. **If the Teensy is powered while the Luckfox is
not, the Teensy drives 3.3 V into a pad whose supply rail is dead.** Current flows through the
pad's ESD diode into an unpowered VCCIO6, which can latch the SoC or degrade it slowly. It looks
like nothing at all until the board stops booting.

Luckfox themselves fit 100 Ω in series on their own console lines (R6/R7 on UART2). Copy that.
The cost is about 0.1 V of output-high at CMOS input current, which the margin below can afford —
barely.

### 2. Measure header pin 12 before you trust it

The UART3_M1 pads sit in the **VCCIO6** domain, which is 1.8 V / 3.3 V selectable *in silicon*.
On the Pico Mini B it is strapped to 3.3 V by R20, a populated 0 Ω link, with the 1.8 V option
R21 marked do-not-populate. So on this board revision the pads are 3.3 V and wire directly to a
Teensy.

That is a **board-revision fact, not a chip fact.** A reworked board or a future variant could
ship the other strap, and a 3.3 V Teensy driving into a 1.8 V bank destroys it.

```bash
# Power the Luckfox alone, nothing connected to pins 12/13.
# DMM DC, black probe on H2 pin 2 (GND):
#   H2 pin 3   -> expect 3.30 V ±5%
#   H2 pin 22  -> expect ~1.80 V   (10 Ω series resistor, so measure unloaded)
#   H2 pin 12  -> expect near 0 V BEFORE UART3 is enabled (pad defaults to a GPIO input)
#   H2 pin 12  -> expect ~3.3 V AFTER UART3 is enabled (UART TX idles high)
```

If pin 12 idles at 1.8 V after enabling UART3, stop. That board needs a level shifter.

---

## The header is not laid out like a Raspberry Pi

Pins 1–11 run **down the left** from the USB-C end. Pins 12–22 run **up the right**, so pin 12 is
bottom-right and pin 22 is top-right.

Counting the right-hand column downward from the top gets you the SARADC pins where you expected
UART3. This has cost people real time.

| Signal | **Pico Mini B** (22-pin) | **Pico** (51 mm, 40-pin) | Teensy 4.1 #1 |
|---|---|---|---|
| Luckfox TX → Teensy RX | pin 12 · GPIO1_D0 · UART3_TX_M1 | **pin 19** · GPIO1_D0 · UART3_TX_M1 | pin 7 (RX2), via 100 Ω |
| Teensy TX → Luckfox RX | pin 13 · GPIO1_D1 · UART3_RX_M1 | **pin 20** · GPIO1_D1 · UART3_RX_M1 | pin 8 (TX2), via 100 Ω |
| Ground | pin 2 or 21 | 18 or 23 (eight GNDs: 3, 8, 13, 18, 23, 28, 33, 38) | any GND |

**Same GPIO, same mux, same enable procedure on both boards — but different header pins.** On
the Mini, UART3 is at the bottom of the *right* column (12/13). On the Pico it is at the bottom of
the *left* column (19/20). Count from the wrong diagram and you land on GPIO4_B0/B1, which are
plain GPIO and will simply never speak. The Pico's console UART2 is pins 1/2 (not 4/5).
Full side-by-side table for both boards is in `docs/10-build-sheet.md` §5.4.

No level shifter. Both ends are 3.3 V push-pull.

---

## Enable UART3

```bash
luckfox-config
# Advanced Options -> UART -> UART3_M1 -> enable -> exit
```

That applies a live configfs device-tree overlay setting
`&uart3 { pinctrl-0 = <&uart3m1_xfer>; status = "okay"; }`. No rebuild, no dtb edit.

Verify immediately:

```bash
ls -l /dev/ttyS3
grep UART3 /etc/luckfox.cfg
ls /sys/kernel/config/device-tree/overlays/
dmesg | grep ff4d0000
```

The `dmesg` line should read `ff4d0000.serial: ttyS3 at MMIO 0xff4d0000`.

### The console is not at risk

UART2 on pins 4/5 is not a `ttyS` at all — it is the Rockchip FIQ debugger, bound by
`rockchip,serial-id = <2>` and exposed as `/dev/ttyFIQ0` at 115200. Different node, different
iomux register. The `luckfox-config` UART menu on the Mini only ever offers UART3_M1 and
UART4_M1, so there is no way to break your console from that menu.

**Wire a CH340 to it anyway before you change anything else.** CH340 RX ← pin 4, CH340 TX → pin
5, GND ↔ pin 2, 115200 8N1. This board has no usable Ethernet (bare pads, no magnetics); if you later switch USB to
host mode for the AR9271, the RNDIS link at 172.32.0.93 disappears and the console is your only
way back in.

### It persists, but only just

The overlay is a **runtime** configfs change, not a persistent dtb edit. It survives reboot only
because `/etc/luckfox.cfg` records `UART3_M1_STATUS=1` and `/etc/init.d/S99luckfoxconfigload`
re-applies it at every boot. Reflash the rootfs, wipe `/etc`, or break that init script and
`/dev/ttyS3` silently stops existing.

---

## 921600 is exact here, which is unusual

The Rockchip fork of `8250_dw` sets the UART source clock to `baud × 16` for any baud above
230400. For 921600 that is 14,745,600 Hz, which the RV1103 fractional divider reaches exactly
from the 1,188,000,000 Hz GPLL with m/n = 256/20625 — both inside the 16-bit registers. The 16550
divisor comes out at exactly 1.

**Baud error: 0.000%.** Datasheet ceiling is 4 Mbps, with 64-byte TX and RX FIFOs.

So if this link ever misbehaves, the baud rate is not the suspect. Which brings us to the one
that is.

---

## The margin that will bite you, if anything does

| | |
|---|---|
| RV1103 output-high, minimum | 2.40 V |
| i.MX RT1062 input-high, minimum (0.7 × 3.3 V) | 2.31 V |
| **Worst-case margin, Luckfox → Teensy** | **90 mV** |

It works in practice. But if you ever see framing errors that are marginal, temperature
dependent, or present in **one direction only**, that number is the suspect — not the baud rate,
not the cable, and not the protocol.

The 100 Ω series resistors eat into this. They are still worth it: the sequencing hazard they
prevent is permanent damage, and this margin is merely thin.

---

## Running the orchestrator at boot

**There is no systemd on the stock Buildroot image.** It is BusyBox init with `/etc/init.d/SXX`
scripts. Check before assuming otherwise:

```bash
ls /lib/systemd/systemd    # absent on the stock image
```

Two traps, both of which produce a board that looks booted while half of it never started.

**The shipped `/etc/init.d/S99python` runs `python /root/main.py` without backgrounding it.** Put
the orchestrator there and init blocks permanently: no sshd restart, no later services, and a
console that looks completely normal.

**`S99luckfoxconfigload` is what creates `/dev/ttyS3`, and it runs at S99.** Anything earlier
starts before the port exists. Worse, `S99python` sorts *before* `S99luckfoxconfigload`
alphabetically, so even the same priority number is not enough.

The service in `firmware/bench-one/luckfox/S98orchestrator` handles both: it backgrounds properly
via `start-stop-daemon`, and it retries the port open rather than assuming it is there.

---

## Reclaiming RAM

The board config reserves `RK_BOOTARGS_CMA_SIZE="24M"` of the 64 MiB total for the camera ISP. If
you are not using the MIPI camera, rebuild with `4M` in
`BoardConfig-SPI_NAND-Buildroot-RV1103_Luckfox_Pico_Mini-IPC.mk` and disable the rkisp, rkcif and
csi nodes.

That is 20 MiB back out of 64 — a 31% increase in usable memory. Verify with `free -m` and
`grep Cma /proc/meminfo` before and after, so the win is measured rather than assumed.

---

## GPIO and the ADC, briefly

`python-periphery` is preinstalled. Prefer the character device:

```python
from periphery import GPIO
g = GPIO('/dev/gpiochip1', 24, 'out')   # GPIO1_D0 — offset WITHIN the bank, not the sysfs number
```

Legacy sysfs uses the global number: `pin = bank×32 + group×8 + X`.

Careful here: **package pin numbers and sysfs numbers nearly collide for exactly these two pins.**
GPIO1_D0 is package pin 57 but sysfs 56; GPIO1_D1 is package pin 56 but sysfs 57. At least one
widely-copied web source has them swapped. Derive from the formula, never from a table you found.

For the ADC, **use channel 1 only:**

```bash
cat /sys/bus/iio/devices/iio:device0/in_voltage1_raw   # 10-bit, 0-1023
# volts = raw × 1.8 / 1024 = raw × 1.7578 mV
```

Channel 0 is the **recovery strap** and must stay pulled up. Reading it is harmless; loading it is
not. That bank (VCCIO2) is 1.8 V only and supplied from USB_AVDD1V8 — never put more than 1.8 V on
header pin 19 or 20.

---

## The AR9271 is a bigger job than it looks

Not a plug-in. In order:

1. **Rebuild the kernel.** The stock defconfig has *no* 802.11 symbols at all — you need
   `CFG80211`, `MAC80211`, `ATH9K_HW`, `ATH9K_COMMON`, `ATH9K_HTC` as modules, plus `FW_LOADER`.
2. Add `libnl`, `wireless-tools`, `iw` and `wpa_supplicant` to Buildroot.
3. Install the `htc_9271.fw` firmware blob.
4. **Switch USB to host mode** — and this is the step that locks you out.

There is exactly **one USB port** on the RV1103. Host mode means no USB gadget networking at the
same time, so the RNDIS management link at 172.32.0.93 is gone permanently. `S99usb0config`
explicitly refuses to configure `usb0` unless `dr_mode` reads `peripheral`.

**Have the UART2 console working before you do this.** No Ethernet, one USB port. The microSD
slot (the Mini **B** has one; the Mini A does not) is storage, not a way in.

One more thing that catches people: the USB-C port has 5.1 kΩ Rd on both CC lines and no VBUS
source switch, so **the board never sources 5 V.** A passive USB-C-to-A OTG adapter enumerates
nothing, because the dongle gets no power. You need a powered hub or an external 5 V feed, and the
AR9271 draws several hundred milliamps.

---

## First contact

```bash
cd /root/bench-one
python3 iop.py            # proves the codec: CRC check must read 0x6F91
python3 benchctl.py sys   # whole-stack health in one round trip
python3 benchctl.py scan  # what is on the I2C bus
```

If `benchctl` cannot open the port, work down this list in order: `/dev/ttyS3` exists →
`luckfox.cfg` records it → pin 12 idles at 3.3 V → the wires are crossed TX-to-RX → grounds are
bonded.

One thing that is *not* a diagnostic: silence. The Teensy's RX pad has an internal pull-up, so an
unplugged link idles high and a plugged-but-unpowered peer idles high too. **Absence of bytes is
the only link-down signal there is**, which is exactly why the transport channel carries a
keepalive.

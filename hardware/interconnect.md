# How every node talks to every other node

Task 3. The answer is **one IP network as the spine**, because — verified below — *every*
node in the fleet can get an IP address, most of them over a wire.

**Confidence:** ✅ verified (multi-source or tested on our own hardware) · ⚠️ unverified.

---

## 1. What each node can actually do for networking

| Node | Network capability | Conf |
|---|---|---|
| **Orange Pi 4 Pro** | **Gigabit Ethernet** (Motorcomm YT8531, measured **>940 Mbit/s**), **WiFi 6** (AIC8800DC), USB 3.0 + USB 2.0 host | ✅ |
| **Zynq PZ7020-StarLite** | **2× Gigabit Ethernet** — one PS-side, one PL-side — plus USB 2.0 host | ✅ |
| **Teensy 4.1** | **10/100 Ethernet — the DP83825 PHY is already on the board.** Signals exit on a pad group; add the MagJack kit (RJ45 board + 6-pin ribbon). Libraries: **QNEthernet / NativeEthernet**, DMA to memory. Also USB. | ✅ |
| **Luckfox Pico Mini B** | **USB gadget = RNDIS Ethernet + ADB.** No extra hardware. | ✅ **tested on our hardware** — it enumerated as a network device at `172.32.0.93` |
| **ESP32-S3** | WiFi + Bluetooth. **No wired Ethernet.** | ✅ |
| **STM32H743 core board** | **No Ethernet PHY on the board** → it's a **UART / SPI / USB** node. (The H743 die has a MAC, but this board doesn't break out a PHY.) Onboard **CH340 USB-serial** for flashing + console. | ✅ resolved |

**Consequence: there is no node that can't be addressed.** That's what makes a single flat
network the right answer instead of a pile of point-to-point links.

---

## 2. Physical topology

```
                    ┌──────────────── Gigabit switch (12 V) ────────────────┐
                    │                    │                    │            │
            Orange Pi 4 Pro          Zynq 7020            Teensy 4.1        │
            (GbE, WiFi6 AP)        (GbE ×2, PL DSP)    (100M + MagJack)     │
                    │                    │                    │            │
       USB ─────────┤                    │                 CAN (listen-only, ×N)
        │           │                    │                    │
   Luckfox ×N       │              mics / sensors         BMW bus (tap in parallel)
 (RNDIS ethernet    │              (I2S/PDM direct,
  over USB)         │               NOT over network)
                    │
              WiFi ─┴─ ESP32-S3 (BLE/IoT, wake node)  +  phones / tablets
```

- **Wired spine:** Pi, Zynq, Teensy(s) on a small **12 V gigabit switch**.
- **Luckfox nodes** hang off the Pi's **USB** and appear as ordinary network interfaces (RNDIS).
- **ESP32-S3** joins over **WiFi** (it's the wake/BLE node, not a data mover).
- **STM32H743** connects by **UART/SPI/USB** to its nearest neighbour unless it turns out to
  have a PHY.

**Why the Zynq's two GbE ports matter:** one goes to the spine; the second can be a **dedicated
point-to-point link to the Pi**, isolating bulk sensor/audio traffic from everything else.

---

## 3. What must NOT go over the network

Ethernet has jitter and buffering. Anything with hard timing stays on dedicated wires:

| Path | Medium | Why |
|---|---|---|
| Mics → Zynq | **I²S / PDM direct** | Sample-accurate audio; beamforming/AEC need phase coherence between mics |
| Teensy ↔ Zynq (real-time) | **UART or SPI direct** | Deterministic; no scheduler or switch in the loop |
| Screen + encoder → local MCU | **SPI/GPIO, short** | SPI at tens of MHz doesn't survive a multi-foot cable run — see [car-system-architecture.md](car-system-architecture.md) |
| CAN tap → Teensy | **CAN, listen-only** | Silicon-enforced silence; see [bmw-f30-bus-reference.md](bmw-f30-bus-reference.md) |

Rule: **timing-critical = dedicated wire. Everything else = IP.**

---

## 4. Protocol layer (on top of IP)

| Need | Choice | Why |
|---|---|---|
| Telemetry fan-out (CAN signals, sensors, status) | **MQTT**, broker on the Pi | Pub/sub suits many small producers + several consumers; nodes come and go |
| Low-latency streams (audio features, sweep data) | **UDP** (multicast where several listeners) | No retransmit stalls |
| Config / control / queries | **HTTP REST** on the Pi | Debuggable with `curl`, scriptable |
| Bulk storage (logs, media, camera footage) | **NFS or SMB** from the Pi | So the Zynq/Luckfox can write straight to the 4.5 TB store |
| Time alignment | see [car-system-architecture.md](car-system-architecture.md) §2 | Zynq is time master; its ns stamps are authoritative |

**Design rule carried from the architecture doc:** every node **buffers locally and syncs
opportunistically**. Never assume the link is up — in a vehicle it frequently isn't.

---

## 5. How this reaches the car
Only through the **Teensy**, and only in one direction:

```
BMW bus ──(parallel tap, short stub, NO added termination)──> CAN transceiver
        ──> Teensy 4.1 (FlexCAN LISTEN_ONLY = silicon-silent)
        ──> Ethernet/MQTT ──> the rest of the fleet
```

**You do not need the 74HC chips for this.** CAN is already a multi-drop bus — BMW's own
documentation states bus users are *connected in parallel*. To add nodes you tap the same
CAN_H/CAN_L pair with short stubs. Full reasoning, the 60 Ω verification procedure, and the
**"remove the 120 Ω resistor from hobby transceiver boards"** warning are in
[bmw-f30-bus-reference.md](bmw-f30-bus-reference.md).

**Where the 74HC chips *are* useful:** single-ended digital work — GPIO expansion, buffering,
5 V↔3.3 V level shifting, multiplexing. Never in the CAN path.

---

---

## 5b. The STM32H743 boards are your display nodes, not network nodes ✅

Specs (both units, arriving): **STM32H743IIT6 Cortex-M7 @ 480 MHz**, 2 MB Flash + 1 MB SRAM
internal, **plus 16 MB Flash + 32 MB SDRAM + EEPROM + SD** external. Board **55 × 85 mm**.
**97 free IOs.** Onboard **CH340 USB-serial** for flashing and console.

**Display interfaces are the point:**
- **LCD1 — 40-pin RGB** (parallel, for a proper panel)
- **LCD2 — 20-pin SPI display + touch**
- **32 MB SDRAM = a real framebuffer**, which is why LVGL runs well on these.

**So assign them as HMI/display nodes**, not as data movers:
- A Teensy driving a 2.4" SPI screen is fine for the **vent module** (small screen, and the
  Teensy is already there for CAN + encoder + servo).
- But if you ever want a **larger dash display**, the H743 is the right driver — an M7 at
  480 MHz with 32 MB SDRAM and a parallel RGB interface will drive a real panel with LVGL,
  which a Teensy on SPI cannot.
- They connect to the fleet over **UART/SPI to a neighbour, or USB** — not Ethernet.

## 6. Open questions
- [ ] Switch choice: needs to be **12 V native** (not a 5 V wall-wart unit) and tolerate the thermal range.
- [x] The Zynq's **PL-side** GbE is a plain RGMII PHY (RTL8211F, address 2, BANK34): Linux on the PS drives it as `eth1` through GEM1-over-EMIO + the GMII-to-RGMII IP — no PL MAC. See `hardware/pz7020-starlite/PS-CONFIG.md` §5.
- [ ] Addressing plan: static IPs vs DHCP from the Pi (static is better for a fixed fleet).

## Sources
- Teensy 4.1 Ethernet (DP83825 PHY on board, MagJack kit) — https://www.pjrc.com/store/ethernet_kit.html
- Luckfox RNDIS-over-USB — **verified directly on this hardware** (enumerated at `172.32.0.93`)
- Orange Pi GbE throughput + WiFi chip — https://github.com/armbian/build/pull/10712
- Puzhi PZ7020-StarLite (2× GbE) — https://www.en.puzhi.com/detail/374.html

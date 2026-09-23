# 3D models — car install

## Design philosophy: don't reprint the vent

The OEM F30 vent is a hard part to reproduce — the photos show **metal spring clips**, a
**foam-gasketed duct throat**, and a **white vane/damper linkage**. Recreating all that means
leaks, rattles and bad fit.

**So we don't.** Keep the OEM vent assembly. Cut out **only the louvers and the centre tab**,
and drop [`vent-insert.scad`](vent-insert.scad) into the opening. Its flange hides the cut
edges. You keep the wood trim, chrome strip, thumbwheel, spring clips, duct seal and damper —
all the parts that are hard to make and easy to get wrong.

This also collapses the precision problem: instead of reverse-engineering a whole part, you
need **five caliper readings**.

---

## What the scan CAN and CANNOT give us — measured, not assumed

`bmw_drivers_air-vent/air-vent-drivers-side.glb` is **Meshy-generated**: one mesh,
**2,197,028 vertices**, 2,259,840 triangles, **no texture**, arbitrary units. I analysed the
actual mesh rather than guessing at its usefulness.

### ✅ USABLE: overall proportions
A bounding box over 2.2 M points is statistically robust even on a noisy surface:

> **overall W : H : D = 1 : 0.4709 : 0.6065**

**So one caliper reading scales the whole part.** Measure the overall vent width, set
`vent_overall_w` in the `.scad`, and it echoes the predicted overall height and depth as a
sanity check on your other measurements. (e.g. a 250 mm-wide vent ⇒ ~117.7 mm tall, ~151.6 mm
deep.) If your calipers disagree badly with that, re-measure — something's off.

### ❌ NOT USABLE: the aperture, corner radii, internal depth
I tried **two independent computational methods to extract the louver aperture and both
failed** — and the failure mode is itself the proof:

1. **Depth-map along the bounding-box axes.** Failed: the "face plane" captured only
   **1,131 of 16,856 cells (6.7%)**, and the detected "aperture" came out as **96.5% of the
   entire depth span** — it was measuring the duct, not an opening. Cause: the vent face is a
   **raked wedge** (clearly visible in `right.png`), not perpendicular to any axis.
2. **Face detection from surface normals**, then re-measuring in that plane. Failed: the
   largest normal cluster is **2,961 of 2,197,028 vertices — 0.1%**. A real molded part shows
   its flat face as a huge cluster. A 0.1% maximum means there are no flat faces at all, just
   reconstructed blob.

**Conclusion (demonstrated, not asserted): the scan has no recoverable fine geometry.** Fine
dimensions must come from calipers. The published route is also closed — searched CAD
repositories, parts catalogues, seller listings and retrofit threads; **no source publishes the
aperture geometry.**

### Useful by-product of that search
Genuine F30 left/driver fresh-air-grille part numbers: **64229218549** and **64229253218**
(F30/F31/F34/F35, 2012–2019). **Buy a cheap aftermarket one to cut up and prototype on, and
keep your OEM vent intact.**

Also note **P3Cars sells a display that integrates into the stock F30 vent** — proof the concept
fits, and they retain airflow. Gauge pods for this vent are **45 mm and 52 mm**, which is a
warning worth heeding: **if the aperture only comfortably takes ~52 mm round gauges, a 72 mm-wide
screen PCB may not fit.** The model's fit-check will catch this once you enter real numbers.

---

## The five measurements to take

| # | Measurement | Why it matters |
|---|---|---|
| 1 | Louver opening **width × height** (inner aperture of the black surround) | Sets the whole insert footprint |
| 2 | **Corner radius** of that opening | Fit and appearance |
| 3 | **Depth** from the face plane back to the first obstruction (damper/vane) | 🔴 **Critical** — decides whether the screen + PCB fit at all |
| 4 | **Lip/bezel** available for the flange to overlap | Whether the flange can hide the cut |
| 5 | Damper **shaft position + travel** | For the servo that closes the vent on hot air |

Put them into the `### MEASURE ###` parameters at the top of the `.scad`.

### Getting the vent out to measure it
From F-series removal write-ups:
1. Remove the **switch and the screw above the switch shaft** to free the vent assembly.
2. The outer vents are **clip-retained** — pry carefully with a **plastic trim tool**, not metal.
3. ⚠️ **There are one or two electrical connectors on the back for the ambient lighting.**
   Disconnect them before pulling the vent clear or you'll tear the harness.

**That connector is also an opportunity:** there is already switched, powered wiring routed to
this exact location — worth identifying before we run a new feed to the vent module.

⚠️ What sits directly behind the duct on an F30 is **not publicly documented** — no source found.
Measurement #3 (usable depth) has to come off the car itself.

---

## Using the model

### The insert now also vents, and carries the Teensy
- **Airflow slots** are cut either side of the screen pocket (auto-skipped where the encoder
  would foul them). A screen that blocks the whole aperture turns a working vent into a blank —
  and your servo/temp plan *depends* on air still moving (cold = free cooling, hot = damper
  shuts). P3Cars' commercial unit retains airflow too. If the aperture is too narrow for slots,
  the model says so rather than silently blocking the vent.
- **Mounting bosses** on the back accept a **Teensy carrier** (`render_mode="print_carrier"`),
  which holds the Teensy 4.1, provides a loom exit and zip-tie strain relief. Insert and carrier
  **share the same boss coordinates from one function**, so the two parts cannot drift apart.
- **Why the Teensy is here and not in the remote box:** the TFT is SPI at tens of MHz and won't
  survive a multi-foot run; encoder edges pick up noise. Local MCU → **one** robust cable
  (12 V + UART/CAN) back to the main enclosure.

```bash
# Preview: ghost screen/encoder/depth envelope + the carrier floating above
openscad vent-insert.scad

# Export printable STLs (ghosts excluded)
openscad -D 'render_mode="print"'         -o vent-insert.stl vent-insert.scad
openscad -D 'render_mode="print_carrier"' -o carrier.stl     vent-insert.scad

# Override a measurement without editing the file
openscad -D 'render_mode="print"' -D vent_w=142 -D vent_h=52 \
         -o vent-insert-print.stl vent-insert.scad
```

**It checks itself.** If the screen or encoder won't fit your measured aperture it prints
`*** FAIL:` / `*** WARN:` lines telling you exactly what's oversized and by how much. Verified
working — forcing `vent_w=60` correctly produced:
```
*** FAIL: screen PCB 72 mm wider than aperture 59.3 mm. Screen will not fit landscape.
*** FAIL: encoder falls outside the aperture.
*** WARN: screen + encoder need 88.5 mm across; aperture is 59.3 mm.
```
⚠️ Don't redirect output to `/dev/null` when testing — OpenSCAD suppresses the echoes.

---

## Verification status (actually run, not assumed)

| Check | Result |
|---|---|
| Compiles | ✅ exit 0, no errors/warnings |
| Print mode is one closed solid | ✅ `Simple: yes`, **`Volumes: 2`** (= 1 solid + outer space) |
| Ghosts excluded from print export | ✅ |
| Fit-checks fire on bad input | ✅ (see above) |

> **Bug found and fixed during testing:** the first version reported `Volumes: 4` — the insert
> was **3 disconnected bodies**, because parts only *touched* at coincident planes. CGAL doesn't
> reliably fuse those. All mating features now **overlap by 0.5 mm** (`ov`). Keep that pattern
> for any new feature you add.

---

## Known component dimensions used

| Part | Dimension | Source |
|---|---|---|
| 2.4" ILI9341 module | PCB **43 × 72 × 5 mm**; active area **36.72 × 48.96 mm** | ✅ vendor spec |
| EC11 encoder | body ~**12.5 mm** sq, **7.5 mm** tall behind panel, 5 mm mounting-hole spacing | ✅ datasheet |
| EC11 bushing / shaft length | **varies by variant** | ⚠️ confirm yours |
| Zynq PZ7020-StarLite | **90 × 60 mm** | ✅ Puzhi |
| Orange Pi 4 Pro | 89 × 56 mm | ⚠️ single source — **measure it** |
| Teensy 4.1 | 61 × 18 mm | ⚠️ confirm |

**Screen is mounted LANDSCAPE** (72 mm wide × 43 mm tall) because the vent aperture is wide and
short. That means the aperture must be **≥ ~72 mm wide** for the screen alone, and **≥ ~88 mm**
if the encoder sits beside it. If your aperture is narrower, the model will say so — and the fix
is to stack the encoder below the screen, or use a smaller display.

---

## Print settings

**ASA or PETG. Never PLA.** A dash in summer sun exceeds PLA's ~60 °C glass transition; the part
will sag and drop out. ASA is UV-stable and the right automotive choice; PETG is an acceptable
compromise. Black, matte.

---

---

# `enclosure.scad` — remote compute enclosure

**This does not go behind the vent.** Real footprints (Zynq **90 × 60 mm**, Pi ~**89 × 56 mm**)
plus storage, a DC-DC and a fan don't remotely fit an F30 dash cavity. Mount it in the
glovebox / under-seat / behind the console and run **one cable** to the vent module.

### Layout options — measured from the model, not estimated

| Layout | Outer envelope | Verdict |
|---|---|---|
| Side-by-side + power/storage | **207.8 × 192.8 × 39 mm** | Wide flat slab; awkward |
| Stacked + power/storage | **116.8 × 192.8 × 57 mm** | Half the width |
| **Stacked, boards only** | **106.8 × 76.8 × 57 mm** | ✅ **glovebox-friendly** |

> **Finding: the DC-DC and SSD dominate the size** — they add ~116 mm of depth on their own.
> **Recommendation: put power + storage in a second enclosure.** The compute box then drops to
> ~107 × 77 × 57 mm, and the converter (the heat source) sits near the power feed instead of
> cooking the boards.

### Both boxes are built — one file, two configurations
| Box | Contents | Outer envelope |
|---|---|---|
| `box="compute"` (stacked, no aux) | Zynq + Pi | **106.8 × 76.8 × 57 mm** |
| `box="power"` | DC-DC + SSD | **116.8 × 124.8 × 41 mm** |

```bash
openscad -D 'render_mode="print_base"' -D 'box="compute"' -D stacked=true -D reserve_aux=false -o compute-base.stl enclosure.scad
openscad -D 'render_mode="print_lid"'  -D 'box="compute"' -D stacked=true -D reserve_aux=false -o compute-lid.stl  enclosure.scad
openscad -D 'render_mode="print_base"' -D 'box="power"' -o power-base.stl enclosure.scad
openscad -D 'render_mode="print_lid"'  -D 'box="power"' -o power-lid.stl  enclosure.scad
```

> **Bug found and fixed by the Volumes check:** in `stacked` mode the upper-level standoffs were
> translated up by `level_gap` and **floated in mid-air** — the base exported as **5 disconnected
> solids**. They're now full-height pillars from the floor. Both configurations verify at
> `Volumes: 2`. *Always check `Volumes` before slicing anything from this file.*

```bash
openscad enclosure.scad                                   # preview w/ ghost boards + lid
openscad -D 'render_mode="print_base"' -o base.stl enclosure.scad
openscad -D 'render_mode="print_lid"'  -o lid.stl  enclosure.scad
openscad -D 'render_mode="print_base"' -D stacked=true -D reserve_aux=false \
         -o base.stl enclosure.scad                       # the compact option
```
It echoes its own envelope on every run, and warns if the fan won't fit or the box is getting
glovebox-hostile.

### Verification (actually run)
| Check | Result |
|---|---|
| Base compiles | ✅ `Simple: yes`, **`Volumes: 2`** (one closed solid) |
| Lid compiles | ✅ `Simple: yes`, **`Volumes: 2`** |
| Envelope self-report | ✅ works in all layout modes |

### ⚠️ What must be measured before printing this
**Mounting-hole patterns are not published for either board.** The `zynq_holes` / `pi_holes`
lists are placeholders. Measure hole centres from each board's bottom-left corner and replace
them, or the standoffs will be wrong. Also confirm screw size (`hole_d`), the Pi's outline
(89 × 56 mm is single-sourced), and your actual DC-DC/SSD sizes.

---

---

# `mic-array.scad` — 4-mic beamforming bar

**Unblocked — its critical dimension is derived, not measured.**

Beamforming aliases (grating lobes) when spacing `d > λ/2` at the top frequency. Whisper samples
at 16 kHz → **8 kHz Nyquist**; λ(8 kHz) = 343/8000 = **42.9 mm**, so **d ≤ 21.4 mm**.
**The bar uses 20 mm**, valid across the whole voice band.

The model **computes that limit itself** and validates your spacing:
```
MIC ARRAY: 4 mics @ 20 mm -> array span 60 mm, bar 91 x 27 mm
  spatial Nyquist limit at 8 kHz = 21.4375 mm; spacing 20 mm is OK
```
Set `-D spacing=45` and it says `*** TOO WIDE - WILL ALIAS ***`.

**Why print a bar instead of hand-placing modules:** the beamformer computes its delays *from*
the spacing. An error in `d` steers the beam the wrong way. Also: **all four mics must sample on
one shared master clock** — phase coherence is the entire basis of the technique.

Mics: **SPH0645 preferred (65 dB SNR) over INMP441 (61 dB)**. Both are I²S with L/R select, so
two mics share a bus → **4 mics = 2 I²S buses** into the Zynq.
⚠️ Measure your breakout's PCB size and acoustic-port position — vendors differ.

Verified: `Simple: yes`, `Volumes: 2`.

---

# `small-parts.scad` — knob + servo bracket
```bash
openscad -D 'part="knob"'          -o knob.stl  small-parts.scad
openscad -D 'part="servo_bracket"' -o servo.stl small-parts.scad
```
- **knob** — 24 × 16 mm, D-bore for the 6 mm EC11 shaft, grip flutes, index mark.
  ⚠️ Confirm your EC11's shaft type/length — variants differ.
- **servo_bracket** — motorises the **OEM damper** rather than building a new diverter:
  cold air → open (free cooling for the stack), hot air → close.
  🔴 **Design the linkage to FAIL CLOSED** — if the ESP32 dies you must not get heat on the Pi.
  ⚠️ It is a **parametric skeleton**; every dimension depends on **measurement #5**. The model
  says so on every run and it will not fit until you measure.

Both verified: `Simple: yes`, `Volumes: 2`.

---

## Still to build
- [ ] Second enclosure for power + storage (per the finding above).
- [ ] Teensy + screen carrier that mounts behind the vent insert.
- [ ] Camera mounts (Luckfox nodes) once positions are chosen.

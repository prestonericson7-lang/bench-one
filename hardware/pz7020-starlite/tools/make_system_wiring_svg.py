#!/usr/bin/env python3
"""make_system_wiring_svg.py -- the whole bench as one drawing: every cable between the Orange Pi, the
PZ7020 FPGA board and the Teensy 4.1, and every jumper wire between the FPGA's JM1 header and the Teensy.

The pin map is READ, not typed in, and cross-checked before anything is drawn:
  * Teensy firmware  accel/gpu/teensy/teensy_gpu/tg_teensy.cpp  (the D0..D15 order in its static_asserts,
                     PIN_SOR / PIN_STROBE / PIN_BUSY)
  * FPGA constraints hardware/pz7020-starlite/vivado/system.xdc  (ball + "JM1 pin N" for every bus port)
  * vendor ball table D:/pz7020-starlite/docs/pinout.md         (JM1 pin -> ball, which pins are GND/power)
  * the bench doc    accel/gpu/WIRING.md                         (the table the owner reads)
Refuses (exit 1) if any two disagree, if a JM1 pin or a Teensy pin carries two wires, or if a wire lands
on a power pin. Teensy 4.1 edge order is PJRC's pinout card (pjrc.com/store/teensy41.html, card 11a).

    python make_system_wiring_svg.py > ../system-wiring.svg
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
FW = os.environ.get("TG_FW", os.path.join(REPO, "accel/gpu/teensy/teensy_gpu/tg_teensy.cpp"))
XDC = os.environ.get("PZ_XDC", os.path.join(REPO, "hardware/pz7020-starlite/vivado/system.xdc"))
WIRING = os.environ.get("WIRING_MD", os.path.join(REPO, "accel/gpu/WIRING.md"))
VENDOR = os.environ.get("PZ_PINOUT", "D:/pz7020-starlite/docs/pinout.md")

# Teensy 4.1 long edges from the USB end (PJRC card 11a): 24 pins each
T_TOP = ["Vin", "GND", "3.3V", "23", "22", "21", "20", "19", "18", "17", "16", "15", "14", "13", "GND",
         "41", "40", "39", "38", "37", "36", "35", "34", "33"]
T_BOT = ["GND", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "3.3V",
         "24", "25", "26", "27", "28", "29", "30", "31", "32"]
# the Teensy's three edge GND pins, one ground wire each (T_TOP[1], T_TOP[14], T_BOT[0])
T_GNDS = [("top", 1), ("top", 14), ("bot", 0)]
JM1_GND_USED = [4, 33, 34]          # one per Teensy GND pin; 3 is the fan's, 35 and 36 stay free
FAN = {1: "5V", 3: "GND", 5: "PWM", 7: "TACH"}

errors = []


def need(cond, msg):
    if not cond:
        errors.append(msg)


# ---- 1. read the four sources ----
fw = open(FW, encoding="utf-8").read()
fw_data = {}
for pin, bit in re.findall(r"CORE_PIN(\d+)_BIT == (\d+)", fw):
    if 16 <= int(bit) <= 31:
        fw_data[int(bit) - 16] = int(pin)
fw_ctl = {k: int(v) for k, v in re.findall(r"#define PIN_(SOR|STROBE|BUSY)\s+(\d+)", fw)}
need(sorted(fw_data) == list(range(16)), f"firmware: D0..D15 not all found ({sorted(fw_data)})")
need(set(fw_ctl) == {"SOR", "STROBE", "BUSY"}, f"firmware: control pins {fw_ctl}")
m = re.search(r"D0\.\.D15 = pins ([\d ]+)=", fw)
if m:
    need([int(x) for x in m.group(1).split()] == [fw_data[i] for i in range(16)],
         "firmware: the header comment's D0..D15 order disagrees with the static_asserts")

xdc_ball, xdc_jm1 = {}, {}
for ball, port, jm1 in re.findall(r"PACKAGE_PIN (\w+)[^\]]*get_ports \{?([\w\[\]]+)\}?\]\s*;#\s*JM1 pin (\d+)",
                                  open(XDC, encoding="utf-8").read()):
    xdc_ball[port], xdc_jm1[port] = ball, int(jm1)

vendor = {}
for row in open(VENDOR, encoding="utf-8").read().split("## JM1")[1].split("## JM2")[0].splitlines():
    cells = [c.strip().strip("*").strip() for c in row.split("|")]
    for i in (1, 5):
        if len(cells) > i + 2 and cells[i].isdigit():
            vendor[int(cells[i])] = (cells[i + 1], cells[i + 2])     # (signal, ball)

doc = {}
for jm1, ball, sig, tpin in re.findall(r"^\|\s*(\d+)\s*\|\s*(\w+)\s*\|\s*([^|]+?)\s*\|\s*(\d+)\s*\|\s*$",
                                       open(WIRING, encoding="utf-8").read(), re.M):
    doc[sig.split()[0]] = (int(jm1), ball, int(tpin))
mg = re.search(r"^\|\s*\*\*([\d, ]+)\*\*\s*\|\s*GND", open(WIRING, encoding="utf-8").read(), re.M)
doc_gnd = [int(x) for x in mg.group(1).split(",")] if mg else []

# ---- 2. one wire list, every source agreeing ----
SIGS = [(f"D{i}", f"tb_d[{i}]", fw_data.get(i)) for i in range(16)] + \
       [("SOR", "tb_sor", fw_ctl.get("SOR")), ("STROBE", "tb_strobe", fw_ctl.get("STROBE")),
        ("BUSY", "tb_busy", fw_ctl.get("BUSY"))]
wires = []
for name, port, tpin in SIGS:
    jm1, ball = xdc_jm1.get(port), xdc_ball.get(port)
    need(jm1 is not None, f"{name}: {port} has no 'JM1 pin' in system.xdc")
    need(jm1 in vendor and vendor[jm1][1] == ball,
         f"{name}: system.xdc puts {port} on ball {ball} as JM1 pin {jm1}; the vendor table has {vendor.get(jm1)}")
    need(doc.get(name) == (jm1, ball, tpin),
         f"{name}: WIRING.md says {doc.get(name)}, firmware+xdc say (JM1 {jm1}, {ball}, Teensy {tpin})")
    wires.append(dict(sig=name, jm1=jm1, ball=ball, tpin=str(tpin), kind="ctl" if name in ("SOR", "STROBE", "BUSY") else "data"))
need(doc_gnd == JM1_GND_USED, f"WIRING.md ground row says JM1 {doc_gnd}, this drawing uses {JM1_GND_USED}")
for g in JM1_GND_USED:
    need(vendor.get(g, ("",))[0] == "GND", f"JM1 pin {g} is not GND in the vendor table ({vendor.get(g)})")
for p, what in FAN.items():
    need(vendor.get(p, ("?",))[0] in ("5 V", "GND") or p in (xdc_jm1.get("fan_pwm"), xdc_jm1.get("fan_tach")),
         f"fan pin {p} ({what}) is not what the vendor table / xdc say")
gw = [dict(sig="GND", jm1=j, ball="—", tpin="GND", kind="gnd", tedge=e, tidx=i)
      for j, (e, i) in zip(JM1_GND_USED, T_GNDS)]
wires = gw + wires
for n, w in enumerate(wires, 1):
    w["n"] = n
    if w["kind"] != "gnd":
        e = "top" if w["tpin"] in T_TOP else "bot"
        w["tedge"], w["tidx"] = e, (T_TOP if e == "top" else T_BOT).index(w["tpin"])

# conflicts: one wire per JM1 pin and per Teensy pin; never a power pin; never a fan pin
jm1_used = [w["jm1"] for w in wires]
need(len(jm1_used) == len(set(jm1_used)), f"a JM1 pin carries two wires: {sorted(jm1_used)}")
tpos = [(w["tedge"], w["tidx"]) for w in wires]
need(len(tpos) == len(set(tpos)), f"a Teensy pin carries two wires: {sorted(tpos)}")
for w in wires:
    lab = (T_TOP if w["tedge"] == "top" else T_BOT)[w["tidx"]]
    need(lab not in ("Vin", "3.3V"), f"W{w['n']} lands on Teensy {lab}")
    need(w["jm1"] not in FAN and w["jm1"] != 2, f"W{w['n']} lands on JM1 pin {w['jm1']} (fan / 3.3 V)")
    need((lab == "GND") == (w["kind"] == "gnd"), f"W{w['n']}: ground/signal mismatch at Teensy {lab}")

if errors:
    sys.stderr.write("REFUSING TO DRAW -- the sources disagree or a pin is double-booked:\n  " + "\n  ".join(errors) + "\n")
    sys.exit(1)
sys.stderr.write(f"checked: {len(wires)} wires ({len(wires) - 3} signals + 3 grounds), firmware = xdc = vendor balls = "
                 f"WIRING.md, every JM1 pin and Teensy pin used once, no power or fan pins\n")

# ---- 3. draw (dark background, large text: nothing smaller than 18 px) ----
C = dict(bg="#0b0f14", ink="#f4f7fa", dim="#c3ccd5", faint="#18212b", board="#6fdc8c", boardfill="#10261a",
         pi="#ffa56b", pifill="#2b1a0f", teensy="#7cc0ff", tfill="#0f2236", eth="#6fdc8c", usb="#d59bff",
         jump="#5aaeff", hdmi="#e3e9ee", pwr="#ff6b6b", data="#5aaeff", ctl="#ffb347", gnd="#e6e6e6",
         unused="#7d8894")
ON = {"data": "#ffffff", "ctl": "#111111", "gnd": "#111111"}      # text colour on a filled pin
W, H = 1600, 3000          # H is trimmed to the content at the end
o = []
w_ = o.append
w_(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif" '
   f'role="img" aria-labelledby="t">')
w_('<title id="t">Bench wiring: Orange Pi 4 Pro + PZ7020 FPGA + Teensy 4.1</title>')
w_(f'<rect width="{W}" height="{H}" fill="{C["bg"]}"/>')


def text(x, y, s, size=20, fill=None, anchor="start", weight="normal"):
    w_(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill or C["ink"]}" text-anchor="{anchor}" '
       f'font-weight="{weight}">{s}</text>')


def box(x, y, w, h, stroke, fill, r=12, dash=None, sw=3):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    w_(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def line(pts, color, sw=7, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
    w_(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{sw}" stroke-linejoin="round" '
       f'stroke-linecap="round"{d}/>')


def badge(x, y, n, color, fg="#0b0f14"):
    w_(f'<circle cx="{x}" cy="{y}" r="22" fill="{color}"/>')
    text(x, y + 8, n, 22, fg, "middle", "bold")


def ordinal(n):
    return f"{n}{'th' if 10 <= n % 100 <= 20 else {1: 'st', 2: 'nd', 3: 'rd'}.get(n % 10, 'th')}"


text(W / 2, 58, "Bench wiring", 44, anchor="middle", weight="bold")
text(W / 2, 100, "Orange Pi 4 Pro  +  PZ7020 FPGA board  +  Teensy 4.1", 28, C["dim"], "middle", "bold")
text(W / 2, 142, "Every cable and every wire. Nothing else connects: NOT the Pi's GPIO pins,", 22, C["dim"], "middle")
text(W / 2, 172, "NOT the FPGA's JM2 header, NOT the FPGA's lower Ethernet jack.", 22, C["dim"], "middle")

# ======== A. the cables ========
AY = 240
text(40, AY, "A.  The cables", 34, weight="bold")
# Teensy block, above the FPGA's JM1
TX, TY, TW, TH = 560, AY + 50, 430, 110
box(TX, TY, TW, TH, C["teensy"], C["tfill"])
text(TX + 180, TY + 50, "Teensy 4.1", 30, C["teensy"], "middle", "bold")
text(TX + 180, TY + 84, "geometry engine", 20, C["dim"], "middle")
w_(f'<rect x="{TX + TW - 24}" y="{TY + 36}" width="40" height="38" rx="5" fill="{C["usb"]}"/>')
text(TX + TW - 36, TY + 26, "micro-USB", 18, C["usb"], "end", "bold")
# FPGA board as it sits on the bench: USB-C + HDMI left, Ethernet right, JM1 top edge
FX, FY, FW_, FH = 480, TY + TH + 150, 560, 470
box(FX, FY, FW_, FH, C["board"], C["boardfill"])
w_(f'<rect x="{FX + 70}" y="{FY + 14}" width="{FW_ - 140}" height="44" rx="6" fill="{C["bg"]}" stroke="{C["jump"]}" stroke-width="3"/>')
text(FX + FW_ / 2, FY + 44, "JM1 header — Teensy wires", 22, C["jump"], "middle", "bold")
text(FX + FW_ / 2 + 30, FY + 225, "PZ7020 FPGA board", 32, C["board"], "middle", "bold")
text(FX + FW_ / 2 + 30, FY + 262, "top view", 20, C["dim"], "middle")
text(FX + FW_ / 2 + 30, FY + 296, "SD card in the underside slot", 20, C["dim"], "middle")
text(FX + FW_ / 2 + 30, FY + 326, "boot jumper cap on SD", 20, C["dim"], "middle")
w_(f'<rect x="{FX + 70}" y="{FY + FH - 58}" width="{FW_ - 140}" height="44" rx="6" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2" stroke-dasharray="8 6"/>')
text(FX + FW_ / 2, FY + FH - 28, "JM2 header — not used", 20, C["unused"], "middle")
J8Y, J2Y, HDY = FY + 110, FY + 170, FY + 380
for s1, y, c in (("J8 USB-C (upper)", J8Y, C["pwr"]), ("J2 USB-C (lower)", J2Y, C["dim"]), ("HDMI", HDY, C["hdmi"])):
    w_(f'<rect x="{FX - 20}" y="{y - 20}" width="40" height="40" rx="5" fill="{c}"/>')
    text(FX + 32, y + 8, s1, 20, c, weight="bold")
ETY, PLY = FY + 150, FY + 380
w_(f'<rect x="{FX + FW_ - 22}" y="{ETY - 26}" width="44" height="52" rx="5" fill="{C["eth"]}"/>')
text(FX + FW_ - 34, ETY - 2, "ETH-PS", 22, C["eth"], "end", "bold")
text(FX + FW_ - 34, ETY + 24, "upper jack", 20, C["eth"], "end")
w_(f'<rect x="{FX + FW_ - 22}" y="{PLY - 26}" width="44" height="52" rx="5" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2"/>')
text(FX + FW_ - 34, PLY - 2, "ETH-PL", 20, C["unused"], "end", "bold")
text(FX + FW_ - 34, PLY + 24, "lower: not used", 18, C["unused"], "end")
# Orange Pi
PX, PY, PW, PH = 1200, FY, 360, FH
box(PX, PY, PW, PH, C["pi"], C["pifill"])
w_(f'<rect x="{PX - 22}" y="{ETY - 26}" width="44" height="52" rx="5" fill="{C["eth"]}"/>')
text(PX + 34, ETY + 8, "Ethernet", 22, C["eth"], weight="bold")
w_(f'<rect x="{PX + 100}" y="{PY - 18}" width="56" height="36" rx="5" fill="{C["usb"]}"/>')
text(PX + 128, PY + 50, "USB-A", 22, C["usb"], "middle", "bold")
text(PX + PW / 2, PY + 225, "Orange Pi 4 Pro", 32, C["pi"], "middle", "bold")
text(PX + PW / 2, PY + 262, "boots from its own SD", 20, C["dim"], "middle")
box(PX + 26, PY + 300, PW - 52, 52, C["pi"], C["bg"], 8, sw=2.5)
text(PX + PW / 2, PY + 334, "NVMe inside → /mnt/nvme", 20, C["pi"], "middle", "bold")
box(PX + 26, PY + 370, PW - 52, 52, C["unused"], C["bg"], 8, dash="8 6", sw=2)
text(PX + PW / 2, PY + 404, "GPIO pins: nothing", 20, C["unused"], "middle", "bold")
w_(f'<rect x="{PX + PW / 2 - 24}" y="{PY + PH - 16}" width="48" height="32" rx="5" fill="{C["pwr"]}"/>')


def device(x, y, w, h, title, sub, c):
    box(x, y, w, h, c, C["bg"], 10, sw=3)
    text(x + w / 2, y + 36, title, 22, c, "middle", "bold")
    if sub:
        text(x + w / 2, y + 66, sub, 18, C["dim"], "middle")


DX, DW = 30, 330
device(DX, J8Y - 45, DW, 90, "5 V charger, 2 A+", "USB-A → USB-C cable", C["pwr"])
device(DX, J2Y + 70, DW, 90, "This PC (optional)", "boot console", C["dim"])
device(DX, HDY - 45, DW, 90, "Monitor / TV", "the GPU's screen", C["hdmi"])
device(PX + 20, PY + PH + 70, PW - 40, 60, "Pi's own supply", None, C["pwr"])

# numbered cables
line([(FX + FW_ + 22, ETY), (PX - 22, ETY)], C["eth"], 9)
badge((FX + FW_ + PX) / 2, ETY - 36, "1", C["eth"])
line([(TX + TW + 16, TY + 55), (PX + 128, TY + 55), (PX + 128, PY - 18)], C["usb"], 8)
badge(TX + TW + 140, TY + 55 - 36, "2", C["usb"])
for i in range(6):
    x = FX + 170 + i * 36
    line([(x, TY + TH), (x, FY + 14)], C["jump"], 4)
badge(FX + 170 + 5 * 36 + 60, (TY + TH + FY) / 2, "3", C["jump"])
text(FX + 170 + 5 * 36 + 94, (TY + TH + FY) / 2 + 8, "22 jumper wires (B)", 22, C["jump"], weight="bold")
line([(DX + DW, J8Y), (FX - 20, J8Y)], C["pwr"], 8)
badge((DX + DW + FX) / 2, J8Y - 34, "4", C["pwr"])
line([(DX + DW, J2Y + 115), (FX - 60, J2Y + 115), (FX - 60, J2Y), (FX - 20, J2Y)], C["dim"], 6, "12 8")
badge((DX + DW + FX - 60) / 2, J2Y + 115 - 34, "5", C["dim"])
line([(DX + DW, HDY), (FX - 20, HDY)], C["hdmi"], 8)
badge((DX + DW + FX) / 2, HDY - 34, "6", C["hdmi"])
line([(PX + PW / 2, PY + PH + 16), (PX + PW / 2, PY + PH + 70)], C["pwr"], 8)

# cable list
LY = PY + PH + 210
text(40, LY, "Cable by cable", 30, weight="bold")
rows = [("1", C["eth"], "Orange Pi Ethernet port  →  FPGA UPPER Ethernet jack (ETH-PS).",
         "Normal Ethernet cable. Pi = 10.77.0.1, FPGA = 10.77.0.2. Carries the GPU, matrix engine, RAM swap."),
        ("2", C["usb"], "Orange Pi USB-A port  →  Teensy micro-USB.",
         "A USB DATA cable, not charge-only. It powers the Teensy."),
        ("3", C["jump"], "Teensy pins  →  FPGA header JM1: 22 jumper wires.",
         "Table below. Wire them with BOTH boards unpowered, grounds first."),
        ("4", C["pwr"], "5 V USB-A charger, 2 A or more  →  FPGA UPPER USB-C (J8).",
         "Use a USB-A → USB-C cable. A C-to-C cable gives this board NO power."),
        ("5", C["dim"], "FPGA LOWER USB-C (J2)  →  this PC.  Optional.",
         "Lets watch_boot.ps1 record the FPGA's boot report."),
        ("6", C["hdmi"], "FPGA HDMI  →  a monitor or TV.",
         "The GPU's screen: colour bars, then dark blue when it is running.")]
for i, (n, c, a, b) in enumerate(rows):
    y = LY + 60 + i * 84
    badge(64, y - 8, n, c)
    text(110, y, a, 23, weight="bold")
    text(110, y + 32, b, 20, C["dim"])
y = LY + 60 + 6 * 84
text(40, y, "Also: the Orange Pi on its own power supply, and the NVMe drive in the Pi's M.2 slot.", 21, C["dim"])

# ======== B. the jumper wires ========
BY = y + 90
text(40, BY, "B.  The 22 jumper wires: FPGA JM1 ↔ Teensy", 34, weight="bold")
text(40, BY + 42, "Both boards drawn as they sit on the bench, top side up.", 22, C["dim"])
text(40, BY + 72, "Each wire has a number (W1–W22) marked on its pin on BOTH boards. Fit W1–W3 (grounds) first.", 22, C["dim"])
by_jm1 = {w["jm1"]: w for w in wires}
by_t = {(w["tedge"], w["tidx"]): w for w in wires}
col = {"data": C["data"], "ctl": C["ctl"], "gnd": C["gnd"]}

JX0, JP = 120, 71.5
JYO, JYI = BY + 250, BY + 350
w_(f'<rect x="{JX0 - 60}" y="{JYO - 110}" width="{19 * JP + 120}" height="{JYI - JYO + 222}" rx="12" fill="{C["boardfill"]}" stroke="{C["board"]}" stroke-width="3"/>')
w_(f'<line x1="{JX0 - 60}" y1="{JYO - 110}" x2="{JX0 + 19 * JP + 60}" y2="{JYO - 110}" stroke="{C["board"]}" stroke-width="8"/>')
text(JX0 - 50, JYO - 124, "FPGA board, TOP EDGE — header JM1", 24, C["board"], weight="bold")
text(JX0 + 19 * JP + 50, JYO - 124, "USB-C side ←      → Ethernet side", 22, C["dim"], "end")
for p in range(1, 41):
    x = JX0 + ((p - 1) // 2) * JP
    y = JYI if p % 2 else JYO
    wv = by_jm1.get(p)
    if wv:
        c = col[wv["kind"]]
        w_(f'<circle cx="{x}" cy="{y}" r="22" fill="{c}"/>')
        text(x, y + 7, str(p), 19, ON[wv["kind"]], "middle", "bold")
        wy, sy = (y - 36, y - 64) if p % 2 == 0 else (y + 54, y + 80)
        text(x, wy, f"W{wv['n']}", 22, c, "middle", "bold")
        text(x, sy, wv["sig"] if wv["sig"] != "STROBE" else "STRB", 18, c, "middle", "bold")
    else:
        stroke = C["pwr"] if p in (1, 2) else C["unused"]
        if p == 1:
            w_(f'<rect x="{x - 20}" y="{y - 20}" width="40" height="40" fill="{C["bg"]}" stroke="{stroke}" stroke-width="3"/>')
        else:
            w_(f'<circle cx="{x}" cy="{y}" r="20" fill="{C["bg"]}" stroke="{stroke}" stroke-width="2.5"/>')
        text(x, y + 7, str(p), 18, stroke, "middle", "bold" if p in (1, 2) else "normal")
        lab = {1: "5V", 2: "3.3V", 3: "fan", 5: "fan", 7: "fan"}.get(p)
        if lab:
            text(x, (y - 36) if p % 2 == 0 else (y + 54), lab, 18, C["pwr"] if p in (1, 2) else C["dim"], "middle")
NY = JYI + 150
text(40, NY, "□ Square pad = pin 1, at the left end of the inner row.  Odd pins: inner row.  Even pins: outer row.", 21, C["dim"])
text(40, NY + 32, "Pins 1–7 are power and the fan: never a Teensy wire there.  Pins 35 and 36 stay empty.", 21, C["dim"])

TX0, TP = 150, 56
TYT = NY + 190
TYB = TYT + 170
w_(f'<rect x="{TX0 - 46}" y="{TYT - 34}" width="{23 * TP + 92}" height="{TYB - TYT + 68}" rx="10" fill="{C["tfill"]}" stroke="{C["teensy"]}" stroke-width="3"/>')
w_(f'<rect x="{TX0 - 100}" y="{(TYT + TYB) / 2 - 30}" width="60" height="60" rx="6" fill="{C["usb"]}"/>')
text(TX0 - 70, (TYT + TYB) / 2 + 60, "USB", 20, C["usb"], "middle", "bold")
text(TX0 + 11.5 * TP, (TYT + TYB) / 2 - 4, "Teensy 4.1 — top side up, USB at the LEFT", 26, C["teensy"], "middle", "bold")
text(TX0 + 11.5 * TP, (TYT + TYB) / 2 + 28, "(as on PJRC's pinout card)", 20, C["dim"], "middle")
for edge, labels, y, up in (("top", T_TOP, TYT, True), ("bot", T_BOT, TYB, False)):
    for i, lab in enumerate(labels):
        x = TX0 + i * TP
        wv = by_t.get((edge, i))
        if wv:
            c = col[wv["kind"]]
            w_(f'<circle cx="{x}" cy="{y}" r="24" fill="{c}"/>')
            text(x, y + 7, lab, 16 if len(lab) > 2 else 20, ON[wv["kind"]], "middle", "bold")
            wy, sy = (y - 42, y - 70) if up else (y + 60, y + 86)
            text(x, wy, f"W{wv['n']}", 21, c, "middle", "bold")
            text(x, sy, wv["sig"] if wv["sig"] != "STROBE" else "STRB", 18, c, "middle", "bold")
        else:
            bad = lab in ("Vin", "3.3V")
            st = C["pwr"] if bad else C["unused"]
            w_(f'<circle cx="{x}" cy="{y}" r="22" fill="{C["bg"]}" stroke="{st}" stroke-width="2.5"/>')
            text(x, y + 6, lab, 14 if len(lab) > 2 else 18, st, "middle", "bold" if bad else "normal")
QY = TYB + 150
text(40, QY, "Top edge from the USB end: Vin, GND, 3.3V, 23 … 13, GND, 41 … 33.", 21, C["dim"])
text(40, QY + 32, "Bottom edge: GND, 0 … 12, 3.3V, 24 … 32.   Vin and 3.3V (red): NEVER wired.", 21, C["dim"])

# ======== C. wire by wire ========
CY = QY + 120
text(40, CY, "C.  Wire by wire", 34, weight="bold")
cols = [("Wire", 40), ("JM1", 150), ("where on JM1", 250), ("signal", 560), ("Teensy", 760),
        ("where on the Teensy", 890), ("direction", 1330)]
for s, x in cols:
    text(x, CY + 52, s, 21, C["dim"], weight="bold")
for i, wv in enumerate(wires):
    y = CY + 96 + i * 42
    c = col[wv["kind"]]
    if i % 2 == 0:
        w_(f'<rect x="28" y="{y - 30}" width="{W - 56}" height="42" fill="{C["faint"]}"/>')
    text(40, y, f"W{wv['n']}", 23, c, weight="bold")
    text(150, y, str(wv["jm1"]), 23, weight="bold")
    text(250, y, f"{'inner' if wv['jm1'] % 2 else 'outer'} row, {ordinal((wv['jm1'] - 1) // 2 + 1)} from left", 21)
    text(560, y, wv["sig"], 23, c, weight="bold")
    text(760, y, wv["tpin"], 23, weight="bold")
    text(890, y, f"{'top' if wv['tedge'] == 'top' else 'bottom'} edge, {ordinal(wv['tidx'] + 1)} from USB", 21)
    d = {"gnd": "ground", "ctl": "FPGA → Teensy" if wv["sig"] == "BUSY" else "Teensy → FPGA",
         "data": "Teensy → FPGA"}[wv["kind"]]
    text(1330, y, d, 20, C["dim"])
FY2 = CY + 96 + len(wires) * 42 + 20
text(40, FY2, "Checked when this was drawn: the Teensy firmware, the FPGA pin file, the vendor's JM1 table", 20, C["dim"])
text(40, FY2 + 30, "and WIRING.md agree on every wire. Every pin carries at most one wire. No wire touches 5 V,", 20, C["dim"])
text(40, FY2 + 60, "3.3 V or the fan's pins. Both boards are 3.3 V logic: wires go direct, no resistors.", 20, C["dim"])
H = int(FY2 + 100)
# screen pages (make_wiring_pages.py): boxes in drawing units, each becomes one 1920x1080 picture
RH = 42
PAGES = [
    dict(name="1-cables", title="1 / 6   The cables", box=[20, TY - 20, W - 20, PY + PH + 145]),
    dict(name="2-cable-by-cable", title="2 / 6   Cable by cable", box=[20, LY + 20, W - 20, LY + 60 + 6 * 84 + 20]),
    dict(name="3-jm1-fpga", title="3 / 6   Jumper wires on the FPGA header JM1", box=[40, JYO - 150, W - 30, NY + 45]),
    dict(name="4-teensy", title="4 / 6   Jumper wires on the Teensy 4.1", box=[40, TYT - 112, W - 30, QY + 45]),
    dict(name="5-wires-1-11", title="5 / 6   Wire by wire: W1 - W11",
         header=[28, CY + 20, W - 28, CY + 66], box=[28, CY + 66, W - 28, CY + 96 + 10 * RH + 14]),
    dict(name="6-wires-12-22", title="6 / 6   Wire by wire: W12 - W22",
         header=[28, CY + 20, W - 28, CY + 66], box=[28, CY + 96 + 10 * RH + 14, W - 28, CY + 96 + 21 * RH + 14]),
]
import json
with open(os.environ.get("PAGES_JSON", os.path.join(HERE, "..", "system-wiring.pages.json")), "w") as pj:
    json.dump(dict(width=W, height=H, bg=C["bg"], pages=PAGES), pj, indent=1)
w_("</svg>")
o[0] = o[0].replace(f'viewBox="0 0 {W} 3000"', f'viewBox="0 0 {W} {H}"')
o[2] = o[2].replace('height="3000"', f'height="{H}"')
sys.stdout.buffer.write(("\n".join(o) + "\n").encode("utf-8"))

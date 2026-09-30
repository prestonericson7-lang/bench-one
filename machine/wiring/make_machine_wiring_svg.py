#!/usr/bin/env python3
"""make_machine_wiring_svg.py -- the whole machine as one drawing, cut into one-screen pages.

Five items: two PZ7020-StarLite FPGA boards (the matrix engines), the Teensy 4.1 with its eight PSRAM
chips (the exact reference; its PSRAM is already wired, docs/47), two STM32H743 core boards (phase 2)
and the ESP32-P4-NANO (phase 2). The PC watches over USB consoles only; its Ethernet port stays on
the internet. Nothing here is a jumper wire except the speaker lead on the P4.

Board port positions are the manual's photo facts recorded in hardware/pz7020-starlite/README.md and
the pz7020 memory: JM1 = top edge, upper USB-C = J8 (power + JTAG), lower USB-C = J2 (CH340 console),
upper RJ45 = ETH-PS (eth0), lower RJ45 = ETH-PL (eth1, off until phase 2), USB-A host top-right beside
the boot jumper, SD slot underside, HDMI left.

    python make_machine_wiring_svg.py            -> machine-wiring.svg + machine-wiring.pages.json
    python make_pages.py                          -> png/*.png (1920x1080 each)
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "machine-wiring.svg")
META = os.path.join(HERE, "machine-wiring.pages.json")

C = dict(bg="#0b0f14", ink="#f4f7fa", dim="#c3ccd5", faint="#18212b", board="#6fdc8c", boardfill="#10261a",
         teensy="#7cc0ff", tfill="#0f2236", stm="#ffd166", sfill="#2a2410", p4="#f78fb3", p4fill="#2a1420",
         pc="#c3ccd5", pcfill="#161c23", eth="#6fdc8c", usb="#d59bff", pwr="#ff6b6b", hdmi="#e3e9ee",
         unused="#7d8894", phase2="#ffb347", spk="#ff9f68")
W = 1600
o = []
w_ = o.append
pages = []


def text(x, y, s, size=20, fill=None, anchor="start", weight="normal"):
    s = str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
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


def port(x, y, w, h, color, filled=True):
    if filled:
        w_(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="5" fill="{color}"/>')
    else:
        w_(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="5" fill="{C["bg"]}" stroke="{color}" stroke-width="2.5"/>')


def device(x, y, w, h, title, sub, c):
    box(x, y, w, h, c, C["bg"], 10, sw=3)
    text(x + w / 2, y + 30, title, 20, c, "middle", "bold")
    if sub:
        text(x + w / 2, y + 54, sub, 17, C["dim"], "middle")


def page(name, title, x0, y0, x1, y1):
    pages.append({"name": name, "title": title, "box": [x0, y0, x1, y1]})


def heading(y, s):
    text(40, y, s, 34, weight="bold")


def wrap(s, n):
    words, out, cur = s.split(), [], ""
    for wd in words:
        if len(cur) + len(wd) + 1 > n:
            out.append(cur); cur = wd
        else:
            cur = (cur + " " + wd).strip()
    out.append(cur)
    return out


# ---------------------------------------------------------------- title
text(W / 2, 58, "The machine: wiring", 44, anchor="middle", weight="bold")
text(W / 2, 100, "2 x PZ7020 FPGA  +  Teensy 4.1 with 8 PSRAM  +  2 x STM32H743  +  ESP32-P4", 28, C["dim"], "middle", "bold")
text(W / 2, 142, "Cables and cards. The only wire is the speaker lead on the ESP32-P4.", 22, C["dim"], "middle")
text(W / 2, 172, "The PC's Ethernet stays on the internet: the PC connects to the machine by USB consoles only.", 22, C["dim"], "middle")
y = 240

# ================================================================ 1. in this order
Y1 = y
heading(Y1, "1.  Do it in this order  (bench day)")
steps = [
    ("Nothing powered.", "Card #1 (32 GB, already written) in FPGA #1's underside slot. Card #2 (32 GB, I write it) in FPGA #2. Boot jumper cap on SD on both (right-hand pair, beside the USB-A port).", C["board"]),
    ("Cable 1.", "Ethernet, FPGA #1 UPPER jack  <->  FPGA #2 UPPER jack. A normal cable; no switch.", C["eth"]),
    ("Cables 2 and 3.", "FPGA #1 LOWER USB-C (J2) -> PC USB.  FPGA #2 LOWER USB-C (J2) -> PC USB.  These are consoles; they do NOT power the boards.", C["usb"]),
    ("Cable 4.", "Teensy micro-USB -> PC USB. It boots, proves its PSRAM and waits. Leave it.", C["teensy"]),
    ("Tell me.", "I arm the watcher on both consoles and the Teensy.", C["dim"]),
    ("Cable 5.", "5 V (USB-A -> USB-C cable) -> FPGA #1 UPPER USB-C (J8). FPGA #1 boots: the first Linux boot this board has ever done. I read its report (about a minute).", C["pwr"]),
    ("When I say so, cable 6.", "5 V -> FPGA #2 UPPER USB-C (J8). I check the link between the two over cable 1.", C["pwr"]),
    ("I run the machine's self-test", "from FPGA #1's console: engine, link, model, Teensy. You do nothing.", C["dim"]),
    ("Phase 2, one board at a time, when I ask:", "STM32 #1, STM32 #2, ESP32-P4 -- pages 6 to 8. Never before.", C["phase2"]),
    ("Power DOWN:", "unplug the 5 V from J8 first, then the consoles. Never unplug the Teensy while it is reading its card.", C["pwr"]),
]
yy = Y1 + 40
for i, (a, b, c) in enumerate(steps):
    yy += 74
    badge(64, yy - 8, i + 1, c)
    text(110, yy, a, 23, c, weight="bold")
    ls = wrap(b, 118)
    for k, ln in enumerate(ls):
        text(110, yy + 30 + k * 26, ln, 20, C["dim"])
    yy += 26 * (len(ls) - 1)
page("1-in-this-order", "1 / 9   Do it in this order", 20, Y1 - 46, 1580, yy + 40)
y = yy + 120

# ================================================================ 2. the machine, all cables
Y2 = y
heading(Y2, "2.  The machine: every cable")
A = Y2 + 70
# ---- PC
PX, PY, PW, PH = 30, A + 60, 250, 700
box(PX, PY, PW, PH, C["pc"], C["pcfill"])
text(PX + PW / 2, PY + 44, "This PC", 30, C["pc"], "middle", "bold")
text(PX + PW / 2, PY + 76, "USB only", 20, C["dim"], "middle")
PC_USB = [PY + 160, PY + 220, PY + 280]
text(PX + 20, PY + 128, "3 USB ports:", 18, C["usb"], weight="bold")
for py in PC_USB:
    port(PX + PW - 20, py - 14, 40, 28, C["usb"])
port(PX + PW - 70, PY - 14, 40, 28, C["usb"])           # top-edge port for the Teensy
text(PX + 20, PY + 380, "phase 2: a USB hub for the", 17, C["phase2"])
text(PX + 20, PY + 404, "STM32 and ESP32-P4 consoles", 17, C["phase2"])
box(PX + 20, PY + PH - 70, PW - 40, 46, C["unused"], C["bg"], 8, dash="8 6", sw=2)
text(PX + PW / 2, PY + PH - 40, "Ethernet: internet, not ours", 16, C["unused"], "middle")


def fpga(x, yb, name, ip, card):
    fw, fh = 640, 320
    box(x, yb, fw, fh, C["board"], C["boardfill"])
    w_(f'<rect x="{x + 70}" y="{yb + 12}" width="{fw - 300}" height="34" rx="6" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2" stroke-dasharray="8 6"/>')
    text(x + 70 + (fw - 300) / 2, yb + 35, "JM1 header - not used", 17, C["unused"], "middle")
    cx = x + fw / 2 + 40
    text(cx, yb + 108, name, 26, C["board"], "middle", "bold")
    text(cx, yb + 138, ip, 20, C["dim"], "middle")
    text(cx, yb + 168, f"top view  ·  {card} underneath  ·  jumper on SD", 18, C["dim"], "middle")
    w_(f'<rect x="{x + 70}" y="{yb + fh - 44}" width="{fw - 300}" height="32" rx="6" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2" stroke-dasharray="8 6"/>')
    text(x + 70 + (fw - 300) / 2, yb + fh - 22, "JM2 header - not used", 17, C["unused"], "middle")
    j8, j2, hd = yb + 70, yb + 130, yb + 270
    port(x - 20, j8 - 20, 40, 40, C["pwr"]); text(x + 30, j8 + 7, "J8 upper USB-C: 5 V IN", 19, C["pwr"], weight="bold")
    port(x - 20, j2 - 20, 40, 40, C["usb"]); text(x + 30, j2 + 7, "J2 lower USB-C: console", 19, C["usb"], weight="bold")
    port(x - 20, hd - 20, 40, 40, C["hdmi"], filled=False); text(x + 30, hd + 7, "HDMI (optional)", 18, C["hdmi"])
    eps, epl = yb + 205, yb + 280
    port(x + fw - 22, eps - 26, 44, 52, C["eth"]); text(x + fw - 34, eps - 2, "ETH-PS upper = eth0", 20, C["eth"], "end", "bold"); text(x + fw - 34, eps + 22, "cable 1", 17, C["eth"], "end")
    port(x + fw - 22, epl - 26, 44, 52, C["phase2"], filled=False); text(x + fw - 34, epl - 2, "ETH-PL lower = eth1", 20, C["phase2"], "end", "bold"); text(x + fw - 34, epl + 22, "phase 2", 17, C["phase2"], "end")
    ua = x + fw - 120
    port(ua - 28, yb - 18, 56, 36, C["phase2"], filled=False); text(ua, yb - 28, "USB-A host (phase 2)", 17, C["phase2"], "middle")
    return dict(j8=j8, j2=j2, hd=hd, eps=eps, epl=epl, uax=ua, uay=yb, fw=fw, fh=fh, yb=yb)


FX = 600
Z0 = fpga(FX, A + 60, "FPGA #1: engine + host", "10.20.0.2", "card #1")
Z1 = fpga(FX, A + 440, "FPGA #2: engine", "10.20.0.3", "card #2")
FR = FX + Z0["fw"]                       # 1240, the boards' right edge
# ---- 5 V supplies (cables 5, 6)
for z, lab, n in ((Z0, "5 V supply A", "5"), (Z1, "5 V supply B", "6")):
    device(330, z["j8"] - 34, 160, 68, lab, "USB-A -> C cable", C["pwr"])
    line([(490, z["j8"]), (FX - 20, z["j8"])], C["pwr"], 8)
    badge(FX - 60, z["j8"] - 32, n, C["pwr"])
# ---- consoles (cables 2, 3)
line([(FX - 20, Z0["j2"]), (306, Z0["j2"]), (306, PC_USB[0]), (PX + PW + 20, PC_USB[0])], C["usb"], 6, "12 8")
badge(FX - 60, Z0["j2"] + 36, "2", C["usb"])
line([(FX - 20, Z1["j2"]), (318, Z1["j2"]), (318, PC_USB[1]), (PX + PW + 20, PC_USB[1])], C["usb"], 6, "12 8")
badge(FX - 60, Z1["j2"] + 36, "3", C["usb"])
# ---- the engines' Ethernet (cable 1)
LE, LO, LP = FR + 26, FR + 48, FR + 70      # right-hand lanes: ethernet, OTG, P4 ethernet
line([(FR + 22, Z0["eps"]), (LE, Z0["eps"]), (LE, Z1["eps"]), (FR + 22, Z1["eps"])], C["eth"], 9)
badge(LE - 34, Z1["eps"] - 60, "1", C["eth"])
# ---- right column: Teensy, STM32 x2, ESP32-P4
RX, RW = 1380, 200
TY, TH = A + 60, 140
box(RX, TY, RW, TH, C["teensy"], C["tfill"])
text(RX + RW / 2 + 10, TY + 40, "Teensy 4.1", 22, C["teensy"], "middle", "bold")
text(RX + RW / 2 + 10, TY + 66, "+ 8 PSRAM", 20, C["teensy"], "middle", "bold")
text(RX + RW / 2 + 10, TY + 92, "already wired (docs/47)", 16, C["dim"], "middle")
text(RX + RW / 2 + 10, TY + 116, "own card · exact reference", 16, C["dim"], "middle")
port(RX - 16, TY + 50, 36, 36, C["usb"])
text(RX - 24, TY + 108, "USB", 16, C["usb"], "end")
line([(RX - 16, TY + 68), (RX - 48, TY + 68), (RX - 48, A + 8), (PX + PW - 50, A + 8), (PX + PW - 50, PY - 14)], C["usb"], 7)
badge(FX + 80, A + 8 - 30, "4", C["usb"])
text(FX + 118, A + 8 - 22, "Teensy -> PC: a data cable, it powers the Teensy", 20, C["usb"], weight="bold")


def stm(yb, name, z, n):
    box(RX, yb, RW, 120, C["stm"], C["sfill"], dash="10 7")
    text(RX + RW / 2 + 10, yb + 36, name, 20, C["stm"], "middle", "bold")
    text(RX + RW / 2 + 10, yb + 62, "phase 2", 16, C["dim"], "middle")
    text(RX + RW / 2 + 10, yb + 84, "TF card: the model", 16, C["dim"], "middle")
    text(RX + RW / 2 + 4, yb + 106, f"{n}a: PC hub  ·  {n}b: FPGA", 15, C["dim"], "middle")
    port(RX - 16, yb + 22, 36, 28, C["phase2"], filled=False)      # console
    port(RX - 16, yb + 70, 36, 28, C["phase2"], filled=False)      # OTG
    # console: a stub towards the PC hub (listed in the table; drawn short so nothing crosses)
    line([(RX - 16, yb + 36), (RX - 44, yb + 36)], C["phase2"], 5, "6 6")
    # OTG -> this FPGA's USB-A host port, entering from the right at the port's height
    line([(RX - 16, yb + 84), (LO, yb + 84), (LO, z["uay"]), (z["uax"] + 28, z["uay"])], C["phase2"], 6, "12 8")
    return yb + 84


s1 = stm(A + 270, "STM32H743 #1", Z0, 7)
badge(LO + 34, (s1 + Z0["uay"]) / 2, "7b", C["phase2"])
s2 = stm(A + 420, "STM32H743 #2", Z1, 8)
badge(LO + 34, s2 + 34, "8b", C["phase2"])
# ESP32-P4
P4Y, P4H = A + 560, 150
box(RX, P4Y, RW, P4H, C["p4"], C["p4fill"], dash="10 7")
text(RX + RW / 2 + 10, P4Y + 36, "ESP32-P4-NANO", 20, C["p4"], "middle", "bold")
text(RX + RW / 2 + 10, P4Y + 62, "phase 2: panel +", 16, C["dim"], "middle")
text(RX + RW / 2 + 10, P4Y + 84, "Ethernet node", 16, C["dim"], "middle")
text(RX + RW / 2 + 10, P4Y + 120, "USB-C: 10 PC hub", 16, C["dim"], "middle")
port(RX - 22, P4Y + 30, 44, 40, C["phase2"], filled=False)
port(RX - 16, P4Y + 95, 36, 28, C["phase2"], filled=False)
line([(RX - 16, P4Y + 109), (RX - 44, P4Y + 109)], C["phase2"], 5, "6 6")
line([(RX - 22, P4Y + 50), (LP, P4Y + 50), (LP, Z1["epl"]), (FR + 22, Z1["epl"])], C["phase2"], 6, "12 8")
badge(LP + 34, P4Y + 50 - 34, "9", C["phase2"])
port(RX + RW - 90, P4Y + P4H - 12, 40, 24, C["spk"])
text(RX + RW - 70, P4Y + P4H - 20, "SPK", 15, C["bg"], "middle", "bold")
device(RX + RW - 170, P4Y + P4H + 50, 170, 64, "8 ohm speaker", "2 wires, either way", C["spk"])
line([(RX + RW - 70, P4Y + P4H + 12), (RX + RW - 70, P4Y + P4H + 50)], C["spk"], 5)
badge(RX + RW - 70 - 40, P4Y + P4H + 30, "11", C["spk"])
# notes + legend
NY = Z1["yb"] + Z1["fh"] + 70
text(FX, NY, "12  HDMI from FPGA #1 to any monitor: optional (the GPU's screen).", 19, C["hdmi"])
text(40, NY + 46, "solid = today.   dashed orange = phase 2, only when I ask.   Cards: #1 and #2 are 32 GB; the Teensy keeps its own.", 20, C["dim"])
page("2-the-machine", "2 / 9   The machine: every cable", 20, Y2 - 46, 1580, NY + 80)
y = NY + 170

# ================================================================ 3. cable by cable (today)
Y3 = y
heading(Y3, "3.  Cable by cable: today")
rows = [("1", C["eth"], "FPGA #1 UPPER Ethernet jack  <->  FPGA #2 UPPER Ethernet jack.",
         "Normal Ethernet cable, direct, no switch. #1 = 10.20.0.2, #2 = 10.20.0.3. The engines' link."),
        ("2", C["usb"], "FPGA #1 LOWER USB-C (J2)  ->  PC USB.",
         "Console only (CH340). It does not power the board. The watcher records the boot."),
        ("3", C["usb"], "FPGA #2 LOWER USB-C (J2)  ->  PC USB.", "Same, for the second board."),
        ("4", C["usb"], "Teensy micro-USB  ->  PC USB.",
         "A DATA cable, not charge-only. Powers the Teensy and its PSRAM. Only flash it with the PC idle."),
        ("5", C["pwr"], "5 V supply A  ->  FPGA #1 UPPER USB-C (J8).",
         "USB-A -> USB-C cable. A C-to-C cable gives this board NO power. Plug this LAST, when I say."),
        ("6", C["pwr"], "5 V supply B  ->  FPGA #2 UPPER USB-C (J8).", "Same. After #1 has reported.")]
yy = Y3 + 40
for n, c, a, b in rows:
    yy += 84
    badge(64, yy - 8, n, c)
    text(110, yy, a, 23, weight="bold")
    text(110, yy + 32, b, 20, C["dim"])
yy += 60
text(40, yy, "Nothing else today. No jumper wires. Not the JM1/JM2 headers. Not the lower Ethernet jacks. Not the USB-A ports.", 21, C["dim"])
page("3-cables-today", "3 / 9   Cable by cable: today", 20, Y3 - 46, 1580, yy + 40)
y = yy + 120

# ================================================================ 4. cable by cable (phase 2)
Y4 = y
heading(Y4, "4.  Cable by cable: phase 2  (only when I ask, one board at a time)")
rows = [("7a", C["phase2"], "STM32 #1 console USB-C (the one by the CH340 chip)  ->  PC USB hub.",
         "Flashing and console. Powers the board."),
        ("7b", C["phase2"], "STM32 #1 other USB-C (OTG)  ->  FPGA #1 USB-A host port.",
         "Its data link into the machine (USB serial). Plug after the console."),
        ("8a", C["phase2"], "STM32 #2 console USB-C  ->  PC USB hub.", "As 7a."),
        ("8b", C["phase2"], "STM32 #2 other USB-C (OTG)  ->  FPGA #2 USB-A host port.", "As 7b."),
        ("9", C["phase2"], "ESP32-P4 RJ45  ->  FPGA #2 LOWER Ethernet jack (ETH-PL).",
         "Only after I have switched eth1 on (a boot-file change, tested on FPGA #1 first)."),
        ("10", C["phase2"], "ESP32-P4 USB-C (console/flash)  ->  PC USB hub.", "Powers it."),
        ("11", C["spk"], "8 ohm speaker  ->  ESP32-P4 speaker header (MX1.25, 2 pins).",
         "The one wire on this machine. Polarity does not matter for a lone speaker."),
        ("12", C["hdmi"], "FPGA #1 HDMI  ->  any monitor.  Optional.", "The GPU's screen: colour bars, then dark blue.")]
yy = Y4 + 40
for n, c, a, b in rows:
    yy += 84
    badge(64, yy - 8, n, c)
    text(110, yy, a, 23, weight="bold")
    text(110, yy + 32, b, 20, C["dim"])
page("4-cables-phase2", "4 / 9   Cable by cable: phase 2", 20, Y4 - 46, 1580, yy + 60)
y = yy + 140

# ================================================================ 5. the FPGA board: which port is which
Y5 = y
heading(Y5, "5.  The FPGA board: which port is which  (both boards are identical)")
BX, BY, BW, BH = 420, Y5 + 80, 760, 420
box(BX, BY, BW, BH, C["board"], C["boardfill"])
w_(f'<rect x="{BX + 40}" y="{BY + 14}" width="{BW - 300}" height="40" rx="6" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2" stroke-dasharray="8 6"/>')
text(BX + 40 + (BW - 300) / 2, BY + 41, "JM1 40-pin header (top edge) - nothing on it", 19, C["unused"], "middle")
w_(f'<rect x="{BX + 40}" y="{BY + BH - 54}" width="{BW - 300}" height="40" rx="6" fill="{C["bg"]}" stroke="{C["unused"]}" stroke-width="2" stroke-dasharray="8 6"/>')
text(BX + 40 + (BW - 300) / 2, BY + BH - 27, "JM2 40-pin header (bottom edge) - nothing on it", 19, C["unused"], "middle")
text(BX + BW / 2, BY + 200, "PZ7020-StarLite", 34, C["board"], "middle", "bold")
text(BX + BW / 2, BY + 236, "top view, as it sits on the bench", 20, C["dim"], "middle")
text(BX + BW / 2, BY + 270, "the SD card slot is UNDERNEATH", 22, C["ink"], "middle", "bold")
for label, yy_, c, sub in (("J8  upper USB-C", BY + 90, C["pwr"], "5 V IN (also JTAG). USB-A -> C cable only."),
                           ("J2  lower USB-C", BY + 160, C["usb"], "console (CH340), to the PC. No power."),
                           ("HDMI", BY + 330, C["hdmi"], "optional monitor")):
    port(BX - 22, yy_ - 20, 44, 40, c, filled=c != C["hdmi"])
    text(BX - 40, yy_ - 4, label, 22, c, "end", "bold")
    text(BX - 40, yy_ + 22, sub, 18, C["dim"], "end")
port(BX + BW - 22, BY + 100, 44, 56, C["eth"]); text(BX + BW + 40, BY + 118, "ETH-PS  upper jack = eth0", 22, C["eth"], weight="bold"); text(BX + BW + 40, BY + 144, "the engines' link (cable 1)", 18, C["dim"])
port(BX + BW - 22, BY + 250, 44, 56, C["phase2"], filled=False); text(BX + BW + 40, BY + 268, "ETH-PL  lower jack = eth1", 22, C["phase2"], weight="bold"); text(BX + BW + 40, BY + 294, "off until phase 2 (ESP32-P4)", 18, C["dim"])
port(BX + BW - 130, BY - 20, 60, 40, C["phase2"], filled=False); text(BX + BW - 100, BY - 30, "USB-A host", 18, C["phase2"], "middle")
# boot jumper: three pin pairs, the cap on the right pair
JX, JY = BX + BW - 290, BY + 70
for i, lab in enumerate(("JTAG", "QSPI", "SD")):
    w_(f'<rect x="{JX + i * 46}" y="{JY}" width="34" height="22" rx="3" fill="{C["bg"]}" stroke="{C["ink"]}" stroke-width="1.5"/>')
    text(JX + i * 46 + 17, JY + 16, lab, 12, C["ink"], "middle")
w_(f'<rect x="{JX + 2 * 46 - 4}" y="{JY - 4}" width="42" height="30" rx="4" fill="none" stroke="{C["board"]}" stroke-width="4"/>')
text(JX + 69, JY + 48, "boot jumper: cap on the RIGHT pair = SD", 17, C["board"], "middle")
text(BX + 40, BY + 330, "LEDs: PWR on with 5 V.  DONE lights when the PL bitstream loads (about 5 s after power).", 18, C["dim"])
text(BX + 40, BY + 356, "If DONE never lights: the card or the jumper. Tell me; do not press anything.", 18, C["dim"])
page("5-fpga-ports", "5 / 9   The FPGA board: which port is which", 20, Y5 - 46, 1580, BY + BH + 60)
y = BY + BH + 160

# ================================================================ 6. Teensy
Y6 = y
heading(Y6, "6.  Teensy 4.1 with the eight PSRAM: nothing new to wire")
box(420, Y6 + 70, 760, 200, C["teensy"], C["tfill"])
text(800, Y6 + 120, "Teensy 4.1 + 8 x ESP-PSRAM64H on the perfboard (docs/47)", 24, C["teensy"], "middle", "bold")
text(800, Y6 + 155, "microSD in the Teensy's own slot: the 3B model (qwen3b.gguf) as today", 20, C["dim"], "middle")
text(800, Y6 + 185, "micro-USB -> PC: data cable. That is all.", 22, C["ink"], "middle", "bold")
text(800, Y6 + 220, "It runs psram_llm v9g: at power-up it proves every chip and waits for a prompt.", 19, C["dim"], "middle")
text(800, Y6 + 248, "Later: the machine's small model added to its card, so it checks the engines' answers exactly.", 19, C["dim"], "middle")
text(40, Y6 + 320, "Rules that stand: flash only with the PC idle; never unplug it while it is reading the card (the card hangs until a USB power-cycle).", 19, C["dim"])
page("6-teensy", "6 / 9   Teensy 4.1 + PSRAM", 20, Y6 - 46, 1580, Y6 + 360)
y = Y6 + 440

# ================================================================ 7. STM32
Y7 = y
heading(Y7, "7.  STM32H743 core boards  (phase 2)")
SX, SY, SW_, SH_ = 420, Y7 + 70, 760, 300
box(SX, SY, SW_, SH_, C["stm"], C["sfill"])
text(SX + SW_ / 2, SY + 44, "STM32H743IIT6 core board, 55 x 85 mm", 24, C["stm"], "middle", "bold")
text(SX + SW_ / 2, SY + 76, "two USB-C on the short edge · two LCD ribbon sockets (not used) · TF slot on the back", 17, C["dim"], "middle")
port(SX + 200, SY + SH_ - 20, 60, 40, C["usb"]); text(SX + 230, SY + SH_ + 50, "USB-C 'UART' / by the CH340 chip", 18, C["usb"], "middle", "bold"); text(SX + 230, SY + SH_ + 74, "-> PC hub: flash + console (7a/8a)", 18, C["dim"], "middle")
port(SX + 500, SY + SH_ - 20, 60, 40, C["phase2"], filled=False); text(SX + 530, SY + SH_ + 50, "USB-C 'USB' / OTG", 18, C["phase2"], "middle", "bold"); text(SX + 530, SY + SH_ + 74, "-> FPGA USB-A host (7b/8b)", 18, C["dim"], "middle")
text(SX + SW_ / 2, SY + 130, "TF card (back): 4 GB, the small model file on it (I prepare the card).", 19, C["ink"], "middle")
text(SX + SW_ / 2, SY + 160, "SWD 4-pin, LCD sockets, pin headers: nothing.", 19, C["dim"], "middle")
text(SX + SW_ / 2, SY + 210, "Which USB-C is which: the one next to the small 16-pin CH340 chip is the console.", 19, C["phase2"], "middle", "bold")
text(SX + SW_ / 2, SY + 238, "Send a photo of both sides' silkscreen before phase 2: I need the vendor pins.", 18, C["phase2"], "middle", "bold")
page("7-stm32", "7 / 9   STM32H743 boards (phase 2)", 20, Y7 - 46, 1580, SY + SH_ + 110)
y = SY + SH_ + 200

# ================================================================ 8. ESP32-P4
Y8 = y
heading(Y8, "8.  ESP32-P4-NANO  (phase 2)")
QX, QY, QW, QH = 420, Y8 + 70, 760, 300
box(QX, QY, QW, QH, C["p4"], C["p4fill"])
text(QX + QW / 2, QY + 44, "Waveshare ESP32-P4-NANO", 24, C["p4"], "middle", "bold")
text(QX + QW / 2, QY + 76, "100 Mbit RJ45, USB-A (host/device), USB-C console, audio amp + speaker header, TF slot, 40-pin header", 18, C["dim"], "middle")
port(QX - 22, QY + 120, 44, 44, C["phase2"], filled=False); text(QX + 34, QY + 140, "RJ45 -> FPGA #2 LOWER jack (9)", 20, C["phase2"], weight="bold"); text(QX + 34, QY + 164, "after eth1 is on; 100 Mbit is plenty for text", 18, C["dim"])
port(QX + 300, QY + QH - 20, 60, 40, C["usb"]); text(QX + 330, QY + QH + 50, "USB-C console -> PC hub (10)", 18, C["usb"], "middle", "bold")
port(QX + QW - 22, QY + 200, 44, 28, C["spk"]); text(QX + QW - 40, QY + 190, "SPK header (MX1.25)", 18, C["spk"], "end", "bold"); text(QX + QW - 40, QY + 214, "speaker, 2 wires (11)", 18, C["dim"], "end")
text(QX + QW / 2, QY + 230, "No screen is owned, so it is a panel by web page and sound, not a display.", 19, C["ink"], "middle")
text(QX + QW / 2, QY + 258, "40-pin header, camera, TF: nothing.", 19, C["dim"], "middle")
page("8-esp32-p4", "8 / 9   ESP32-P4-NANO (phase 2)", 20, Y8 - 46, 1580, QY + QH + 90)
y = QY + QH + 180

# ================================================================ 9. what you should see
Y9 = y
heading(Y9, "9.  What you should see, and what to do about it")
rows = [(C["teensy"], "Teensy (cable 4)", "its LED blinks while it proves the PSRAM (about 30 s), then the console shows 'I bench-one psram_llm v9 banks N'.", "Nothing to do. If banks < 6 I decide."),
        (C["pwr"], "FPGA #1 (cable 5)", "PWR LED on; DONE LED on within ~5 s; console: SPL banner, U-Boot, kernel, then 'ZYNQ-REPORT' after ~60 s.", "Wait for me. This is the board's first Linux boot."),
        (C["pwr"], "FPGA #1: silent", "no console text within 20 s of 5 V.", "Unplug the 5 V. Check the card is seated and the jumper is on the right pair. Tell me; I do not guess at it."),
        (C["pwr"], "FPGA #1: DONE never lights", "PWR on, console alive, but DONE dark.", "Tell me. pl.bit did not load; the engine will report 'cpu' instead of 'pl' and I will know."),
        (C["eth"], "Cable 1", "both jacks' link LEDs on once both boards are up.", "If not: reseat the cable. Any straight or crossover cable works (auto-MDIX)."),
        (C["dim"], "The self-test", "runs from FPGA #1's console; takes 5 to 30 minutes depending on what the cards hold.", "Do nothing. I report once at the end, with the log archived.")]
yy = Y9 + 40
for c, a, b, d in rows:
    yy += 92
    w_(f'<rect x="44" y="{yy - 26}" width="14" height="70" rx="4" fill="{c}"/>')
    text(80, yy, a, 23, c, weight="bold")
    text(80, yy + 30, b, 19, C["dim"])
    text(80, yy + 56, d, 19, C["ink"])
page("9-what-you-see", "9 / 9   What you should see", 20, Y9 - 46, 1580, yy + 90)
H = yy + 150

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif" role="img" aria-labelledby="t">'
       f'<title id="t">The machine: wiring</title><rect width="{W}" height="{H}" fill="{C["bg"]}"/>' + "\n".join(o) + "</svg>\n")
open(SVG, "w", encoding="utf-8").write(svg)
json.dump({"width": W, "height": H, "bg": C["bg"], "pages": pages}, open(META, "w"), indent=1)
print(f"{SVG}  {W}x{H}  {len(pages)} pages")

#!/usr/bin/env python3
"""make_accel_wiring_svg.py -- the PZ7020-StarLite as the Orange Pi's accelerator: what goes on which pin.

Geometry is the vendor PCB DXF (04.Hardware/03. Form Factors/PZ-STARLITE-PCB-V1_1_TOP.dxf, mm, origin
bottom-left) at 7 px/mm, the same frame as docs/board-layout.svg in the owner's pz7020-starlite repo.
Connector identities are from the vendor schematic (U15 -> J6 = PS PHY, U16 -> J7 = PL PHY) and the
manual's board photo (silkscreen ETH-PS / ETH-PL, J8 JTAG, J2 UART, HDMI). JM1 signals are
accel/gpu/WIRING.md and vivado/system.xdc (checked against the vendor ball table by check_xdc.py).

    python make_accel_wiring_svg.py > ../accel-wiring.svg
"""
import sys

OX, OY, S = 250.0, 205.0, 7.0          # board origin on the canvas, px per mm


def X(mm):
    return OX + mm * S


def Y(mm):                              # DXF y is up, SVG y is down; board is 60 mm tall
    return OY + (60.0 - mm) * S


# JM1 (top-edge header): odd pins on the inner row, even pins on the outer row, pin 1 at the left.
PITCH = 2.54 * S
PIN1_X = OX + 118.65                    # from board-layout.svg (203.65 at OX=85)
ROW_OUT, ROW_IN = OY + 23.52, OY + 41.3


def pin_xy(n):
    return PIN1_X + ((n - 1) // 2) * PITCH, (ROW_IN if n % 2 else ROW_OUT)


C = dict(bg="#0d1117", board="#21262d", edge="#e6edf3", part="#161b22", line="#8b949e", text="#e6edf3",
         dim="#8b949e", data="#58a6ff", ctl="#f0883e", gnd="#8b949e", v5="#f85149", v33="#d2a8ff",
         fan="#3fb950", free="#30363d", ok="#3fb950")

# JM1 pin -> (label, kind, teensy pin)
TEENSY = {9: ("D0", 19), 10: ("D1", 18), 11: ("D2", 14), 12: ("D3", 15), 13: ("D4", 40), 14: ("D5", 41),
          15: ("D6", 17), 16: ("D7", 16), 17: ("D8", 22), 18: ("D9", 23), 19: ("D10", 20), 20: ("D11", 21),
          21: ("D12", 38), 22: ("D13", 39), 23: ("D14", 26), 24: ("D15", 27),
          25: ("SOR", 3), 26: ("STROBE", 2), 27: ("BUSY", 4)}
PINS = {}
for n in range(1, 41):
    if n in TEENSY:
        name, t = TEENSY[n]
        kind = "data" if name.startswith("D") else "ctl"
        PINS[n] = (f"{name} → T{t}", kind)       # "→" = wire to Teensy pin T# (not signal direction)
PINS[1] = ("5V → fan +", "v5")
PINS[2] = ("3.3V  (leave)", "v33")
PINS[3] = ("GND → fan −", "gnd")
PINS[4] = ("GND → Teensy", "gnd")
PINS[5] = ("FAN PWM (10 kΩ to GND)", "fan")
PINS[7] = ("FAN TACH", "fan")
for n in (33, 34, 35, 36):
    PINS[n] = ("GND → Teensy" if n in (33, 34) else "GND", "gnd")

out = []
w = out.append
W, H = 1200, 850
w(f'<svg viewBox="0 0 {W} {H}" xmlns="http://www.w3.org/2000/svg" role="img" aria-labelledby="t d" '
  f'font-family="Helvetica, Arial, sans-serif">')
w('<title id="t">PZ7020-StarLite accelerator wiring</title>')
w('<desc id="d">Top view of the PZ7020-StarLite (vendor PCB geometry) wired as the Orange Pi 4 Pro\'s accelerator: '
  'Teensy 4.1 geometry bus on JM1 pins 9-27 with grounds on 4, 33 and 34, fan on JM1 pins 1/3/5/7, '
  'ETH-PS to the Orange Pi, HDMI to a monitor, JTAG USB-C for power, UART USB-C for the console, boot jumper on SD.</desc>')
w(f'<rect width="{W}" height="{H}" fill="{C["bg"]}"/>')
w(f'<text x="{W/2}" y="34" font-size="24" fill="{C["text"]}" text-anchor="middle" font-weight="bold">'
  'PZ7020-StarLite — wired as the Orange Pi\'s accelerator</text>')
w(f'<text x="{W/2}" y="58" font-size="14" fill="{C["dim"]}" text-anchor="middle">'
  'top view, component side up • geometry from the vendor PCB drawing • JM1 signals from accel/gpu/WIRING.md</text>')

# board + parts (mm from the DXF)
w(f'<rect x="{X(0)}" y="{Y(60)}" width="{90*S}" height="{60*S}" rx="6" fill="{C["board"]}" stroke="{C["edge"]}" stroke-width="2.5"/>')


def part(x, y, wmm, hmm, label, sub=None, stroke=None, fs=15):
    sx, sy = X(x), Y(y + hmm)
    w(f'<rect x="{sx:.1f}" y="{sy:.1f}" width="{wmm*S:.1f}" height="{hmm*S:.1f}" rx="3" fill="{C["part"]}" '
      f'stroke="{stroke or C["line"]}" stroke-width="{2.5 if stroke else 1.5}"/>')
    cx, cy = sx + wmm * S / 2, sy + hmm * S / 2
    w(f'<text x="{cx:.1f}" y="{cy + (0 if sub else 5):.1f}" font-size="{fs}" fill="{C["text"]}" text-anchor="middle" font-weight="bold">{label}</text>')
    if sub:
        w(f'<text x="{cx:.1f}" y="{cy + 18:.1f}" font-size="12" fill="{C["dim"]}" text-anchor="middle">{sub}</text>')
    return sx, sy, wmm * S, hmm * S


zy = part(33.8, 21.0, 17.0, 17.0, "Zynq", "XC7Z020")
hd = part(-1.3, 8.4, 11.2, 15.0, "HDMI", None, C["data"], 14)
jt = part(-1.1, 43.4, 7.6, 9.0, "J8", None, C["v5"], 12)
ua = part(-1.1, 29.8, 7.6, 9.0, "J2", None, C["line"], 12)
ps = part(70.8, 27.2, 21.5, 17.5, "ETH-PS", "J6 • eth0", C["ok"])
pl = part(70.8, 8.6, 21.5, 17.5, "ETH-PL", "J7 • eth1")
usa = part(78.2, 45.8, 14.1, 8.4, "USB-A", None, None, 12)
# header outlines
for y0 in (0.0, 50.8):
    w(f'<rect x="{X(12.2):.1f}" y="{Y(y0 + 9.1):.1f}" width="{57.8*S:.1f}" height="{9.1*S:.1f}" rx="3" fill="none" stroke="#484f58" stroke-dasharray="6 4"/>')
w(f'<text x="{X(41):.1f}" y="{Y(0) - 70:.1f}" font-size="15" fill="{C["dim"]}" text-anchor="middle">JM2 (bottom header) — not used by this build</text>')

# JM2 holes (plain), JM1 holes coloured by role
for n in range(1, 41):
    x = PIN1_X + ((n - 1) // 2) * PITCH
    # JM2: pin 1 at the RIGHT end of the inner row (board-layout.svg); draw plain holes only
    jx = OX + 456.47 - ((n - 1) // 2) * PITCH
    jy = OY + (439.12 - 60 if n % 2 else 456.9 - 60)
    w(f'<circle cx="{jx:.2f}" cy="{jy:.2f}" r="5" fill="{C["free"]}"/>')
for n in range(1, 41):
    x, y = pin_xy(n)
    kind = PINS.get(n, (None, "free"))[1]
    col = C[kind]
    if n == 1:
        w(f'<rect x="{x-6.5:.2f}" y="{y-6.5:.2f}" width="13" height="13" fill="{col}"/>')
    else:
        w(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="6" fill="{col}"/>')

# labels: even (outer row) upward above the board, odd (inner row) downward into the board
for n, (label, kind) in PINS.items():
    x, y = pin_xy(n)
    col = C[kind]
    if n % 2 == 0:
        ty = OY - 8
        w(f'<line x1="{x:.2f}" y1="{y-7:.2f}" x2="{x:.2f}" y2="{ty+2:.2f}" stroke="{col}" stroke-width="1"/>')
        w(f'<text transform="translate({x+4:.2f},{ty:.2f}) rotate(-90)" font-size="12" fill="{col}">{n} {label}</text>')
    else:
        ty = y + 12
        w(f'<text transform="translate({x-4:.2f},{ty:.2f}) rotate(90)" font-size="12" fill="{col}">{n} {label}</text>')

# callouts: left side
def callout(px, py, tx, ty, lines, col, anchor="end"):
    w(f'<line x1="{px:.1f}" y1="{py:.1f}" x2="{tx:.1f}" y2="{ty:.1f}" stroke="{col}" stroke-width="1.5"/>')
    for i, (s, c2) in enumerate(lines):
        w(f'<text x="{tx + (-6 if anchor == "end" else 6):.1f}" y="{ty + 5 + i*17:.1f}" font-size="{15 if i == 0 else 13}" '
          f'fill="{c2 or col}" text-anchor="{anchor}"{" font-weight=\"bold\"" if i == 0 else ""}>{s}</text>')


callout(jt[0], jt[1] + jt[3] / 2, OX - 30, jt[1] - 18, [("J8 ‘JTAG’ USB-C", None), ("5 V in, 2 A+ USB-A charger", C["text"]), ("A-to-C cable (not C-to-C)", C["text"]), ("(also the JTAG programmer)", C["dim"])], C["v5"])
callout(ua[0], ua[1] + ua[3] / 2, OX - 30, ua[1] + 40, [("J2 ‘UART’ USB-C", C["text"]), ("console to the PC (A-to-C)", C["dim"]), ("(watch_boot.ps1)", C["dim"])], C["line"])
callout(hd[0], hd[1] + hd[3] / 2, OX - 30, hd[1] + hd[3] / 2 + 30, [("HDMI → monitor", C["data"]), ("the GPU: 1280×720 @ 60", C["text"])], C["data"])
# right side
callout(ps[0] + ps[2], ps[1] + ps[3] / 2, X(90) + 40, ps[1] + 30, [("ETH-PS → Orange Pi", C["ok"]), ("eth0: 10.20.0.2 + 10.77.0.2", C["text"]), ("GPU 7777 • engine 8093", C["dim"]), ("RAM (nbd) 10809", C["dim"])], C["ok"], "start")
callout(pl[0] + pl[2], pl[1] + pl[3] / 2, X(90) + 40, pl[1] + pl[3] / 2 + 20, [("ETH-PL", C["text"]), ("eth1, spare", C["dim"])], C["line"], "start")
callout(usa[0] + usa[2], usa[1] + 10, X(90) + 40, usa[1] - 34, [("boot jumper (above USB-A):", C["text"]), ("cap on the SD pair", C["ok"])], C["line"], "start")

# legend + rules
ly = int(Y(0)) + 62
items = [("data", "Teensy data D0–D15 (Teensy pin T#)"), ("ctl", "SOR / STROBE / BUSY"), ("gnd", "GND"),
         ("v5", "5 V (fan only)"), ("v33", "3.3 V — leave"), ("fan", "fan PWM / tach")]
for i, (k, s) in enumerate(items):
    lx = 60 + (i % 3) * 370
    yy = ly + (i // 3) * 26
    w(f'<circle cx="{lx}" cy="{yy - 5}" r="7" fill="{C[k]}"/><text x="{lx + 14}" y="{yy}" font-size="15" fill="{C["text"]}">{s}</text>')
rules = ["Only GND and the 19 signal wires join the Teensy and the FPGA (3.3 V both sides, no resistors).",
         "Never wire JM1 pin 1 (5 V) or pin 2 (3.3 V) to the Teensy — it is powered by the Pi's USB.",
         "Never power the board from J8 and JM1 pin 1 at the same time: they are one net."]
for i, s in enumerate(rules):
    w(f'<text x="60" y="{ly + 64 + i*20}" font-size="14" fill="{C["dim"]}">{s}</text>')
w('</svg>')
sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))

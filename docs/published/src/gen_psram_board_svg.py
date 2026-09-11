# Generates the big board-wiring SVG. Every coordinate is computed, so stubs, lanes and junction
# dots cannot drift apart, and nothing is hand-typed twice.
import io

CHIP_X0, CHIP_X1 = 720, 1000
CHIP_H = 200
CHIP_Y = [200, 500, 800, 1100]
PIN_DY = [40, 80, 120, 160]          # offsets of pins 1..4 (left) and 8..5 (right)

# Vertical lanes. Lane order is chosen so the feed runs across the top never cross one another:
# the further right the lane, the higher its feed run.
LEFT = [(680, 2, "SIO1", "dat", 120),
        (650, 3, "SIO2", "dat", 140),
        (620, 4, "VSS",  "gnd", 160)]
RIGHT = [(1040, 8, "VCC",  "pwr", 100),
         (1070, 7, "SIO3", "dat",  80),
         (1100, 6, "SCLK", "clk",  60),
         (1130, 5, "SIO0", "dat",  40)]
LANES = LEFT + RIGHT

# Teensy pads listed in the SAME top-to-bottom order as their feed lanes, and with escape x
# increasing downwards, so no escape run crosses another.
TEENSY = [("52", "IO0",  240, 210,  40, "dat"),
          ("53", "SCLK", 292, 228,  60, "clk"),
          ("54", "IO3",  344, 246,  80, "dat"),
          ("3V3", "",    396, 264, 100, "pwr"),
          ("49", "IO1",  448, 282, 120, "dat"),
          ("50", "IO2",  500, 300, 140, "dat"),
          ("GND", "",    552, 318, 160, "gnd")]

DEC_X0, DEC_X1 = 300, 470
DEC_PIN_Y = [790, 835, 880, 925, 970, 1015, 1060, 1105]
CS = [(15, 0, 510, 0), (14, 1, 540, 1), (13, 2, 570, 2), (12, 3, 600, 3)]  # pin, Yn, lane x, chip

C = {"dat": "var(--dat)", "clk": "var(--clk)", "pwr": "var(--pwr)",
     "gnd": "var(--gnd)", "cs": "var(--cs)"}
VB_W, VB_H = 1340, 1520

o = io.StringIO(); w = o.write
w('<svg viewBox="0 0 %d %d" xmlns="http://www.w3.org/2000/svg" role="img"\n' % (VB_W, VB_H))
w('     aria-label="Board wiring: the Teensy feeds seven shared lanes that run down the board, every '
  'PSRAM taps every lane, and the decoder gives each PSRAM its own chip-select wire">\n')
w('''  <defs><style>
    .body{fill:var(--panel-2);stroke:var(--ink);stroke-width:3}
    .lane{fill:none;stroke-width:5;stroke-linecap:round;stroke-linejoin:round}
    .stub{fill:none;stroke-width:5;stroke-linecap:round;stroke-linejoin:round}
    .leg{stroke:var(--ink);stroke-width:5;stroke-linecap:round}
    .cname{font-family:var(--disp);font-size:27px;font-weight:700;fill:var(--ink)}
    .csub{font-family:var(--disp);font-size:16px;font-weight:600;fill:var(--ink-3)}
    .pnum{font-family:var(--mono);font-size:19px;font-weight:700;fill:var(--ink)}
    .sig{font-family:var(--mono);font-size:15px;font-weight:600;fill:var(--ink-2)}
    .llbl{font-family:var(--mono);font-size:16px;font-weight:700}
    .grp{font-family:var(--disp);font-size:15px;font-weight:700;fill:var(--ink-3);letter-spacing:.12em}
    .note{font-family:"Source Sans 3",sans-serif;font-size:17px;font-weight:600;fill:var(--ink)}
    .note2{font-family:"Source Sans 3",sans-serif;font-size:15px;fill:var(--ink-3)}
  </style></defs>\n''')

# ---- shared lanes -------------------------------------------------------------------------
for x, pin, name, col, fy in LEFT:
    w('  <path class="lane" d="M%d %d L%d %d" stroke="%s"/>\n'
      % (x, fy, x, CHIP_Y[3] + PIN_DY[pin - 1], C[col]))
for x, pin, name, col, fy in RIGHT:
    w('  <path class="lane" d="M%d %d L%d %d" stroke="%s"/>\n'
      % (x, fy, x, CHIP_Y[3] + PIN_DY[8 - pin], C[col]))

# ---- Teensy feed runs into the top of each lane --------------------------------------------
for lbl, sub, py, ex, fy, col in TEENSY:
    lane = [l for l in LANES if l[4] == fy][0]
    w('  <path class="stub" d="M186 %d L%d %d L%d %d L%d %d" stroke="%s"/>\n'
      % (py, ex, py, ex, fy, lane[0], fy, C[col]))
    w('  <circle cx="%d" cy="%d" r="7" fill="%s"/>\n' % (lane[0], fy, C[col]))
    w('  <text class="llbl" x="%d" y="%d" fill="%s">%s</text>\n'
      % (ex + 12, fy - 12, C[col], lane[2]))

# ---- the four chips ------------------------------------------------------------------------
for i, cy in enumerate(CHIP_Y):
    w('  <rect class="body" x="%d" y="%d" width="%d" height="%d" rx="6"/>\n'
      % (CHIP_X0, cy, CHIP_X1 - CHIP_X0, CHIP_H))
    w('  <circle cx="%d" cy="%d" r="8" fill="var(--ink)"/>\n' % (CHIP_X0 + 26, cy + 26))
    mid = (CHIP_X0 + CHIP_X1) // 2
    w('  <text class="cname" x="%d" y="%d" text-anchor="middle">PSRAM %d</text>\n' % (mid, cy + 104, i + 1))
    w('  <text class="csub" x="%d" y="%d" text-anchor="middle">bank %d &#183; dot = pin 1</text>\n'
      % (mid, cy + 130, i))
    for k in range(4):
        y = cy + PIN_DY[k]
        w('  <line class="leg" x1="%d" y1="%d" x2="%d" y2="%d"/>\n' % (CHIP_X0, y, CHIP_X0 - 22, y))
        w('  <text class="pnum" x="%d" y="%d" text-anchor="end">%d</text>\n' % (CHIP_X0 - 30, y + 7, k + 1))
        w('  <line class="leg" x1="%d" y1="%d" x2="%d" y2="%d"/>\n' % (CHIP_X1, y, CHIP_X1 + 22, y))
        w('  <text class="pnum" x="%d" y="%d">%d</text>\n' % (CHIP_X1 + 30, y + 7, 8 - k))
    for x, pin, name, col, fy in LEFT:
        y = cy + PIN_DY[pin - 1]
        w('  <path class="stub" d="M%d %d L%d %d" stroke="%s"/>\n' % (CHIP_X0 - 22, y, x, y, C[col]))
        w('  <circle cx="%d" cy="%d" r="7" fill="%s"/>\n' % (x, y, C[col]))
    for x, pin, name, col, fy in RIGHT:
        y = cy + PIN_DY[8 - pin]
        w('  <path class="stub" d="M%d %d L%d %d" stroke="%s"/>\n' % (CHIP_X1 + 22, y, x, y, C[col]))
        w('  <circle cx="%d" cy="%d" r="7" fill="%s"/>\n' % (x, y, C[col]))
    # 100 nF from pin 8 round to pin 4
    capy = cy + 182
    w('  <path class="stub" d="M%d %d L%d %d L%d %d L%d %d L%d %d" stroke="var(--ink-3)" '
      'stroke-dasharray="9 7" stroke-width="3"/>\n'
      % (CHIP_X1 + 22, cy + PIN_DY[0], CHIP_X1 + 10, cy + PIN_DY[0], CHIP_X1 + 10, capy,
         CHIP_X0 - 10, capy, CHIP_X0 - 10, cy + PIN_DY[3]))
    w('  <rect x="%d" y="%d" width="58" height="26" fill="var(--panel)" stroke="var(--ink-3)" '
      'stroke-width="2.5"/>\n' % (mid - 29, capy - 13))
    w('  <text class="sig" x="%d" y="%d" text-anchor="middle">104</text>\n' % (mid, capy + 6))

# ---- chip-select wires ---------------------------------------------------------------------
for dec_pin, yn, lx, ci in CS:
    dy = DEC_PIN_Y[yn + 1]
    ty = CHIP_Y[ci] + PIN_DY[0]
    w('  <path class="stub" d="M%d %d L%d %d L%d %d L%d %d" stroke="%s"/>\n'
      % (DEC_X1 + 22, dy, lx, dy, lx, ty, CHIP_X0 - 22, ty, C["cs"]))
    w('  <text class="llbl" x="%d" y="%d" fill="%s">Y%d to pin 1</text>\n'
      % (lx + 10, ty - 14, C["cs"], yn))

# ---- the decoder ---------------------------------------------------------------------------
w('  <rect class="body" x="%d" y="%d" width="%d" height="%d" rx="6"/>\n'
  % (DEC_X0, DEC_PIN_Y[0] - 46, DEC_X1 - DEC_X0, DEC_PIN_Y[7] - DEC_PIN_Y[0] + 92))
w('  <path d="M%d %d a34 34 0 0 0 68 0" fill="var(--panel)" stroke="var(--ink)" stroke-width="3"/>\n'
  % (DEC_X0 + 51, DEC_PIN_Y[0] - 46))
dmid = (DEC_X0 + DEC_X1) // 2
w('  <text class="cname" x="%d" y="%d" text-anchor="middle">74LVC</text>\n' % (dmid, DEC_PIN_Y[3] + 4))
w('  <text class="cname" x="%d" y="%d" text-anchor="middle">138A</text>\n' % (dmid, DEC_PIN_Y[4] + 4))
w('  <text class="csub" x="%d" y="%d" text-anchor="middle">notch at the top</text>\n' % (dmid, DEC_PIN_Y[5]))

LN = ["A", "B", "C", "G2A", "G2B", "G1", "Y7", "GND"]
RN = ["VCC", "Y0", "Y1", "Y2", "Y3", "Y4", "Y5", "Y6"]
for k, y in enumerate(DEC_PIN_Y):
    w('  <line class="leg" x1="%d" y1="%d" x2="%d" y2="%d"/>\n' % (DEC_X0, y, DEC_X0 - 22, y))
    w('  <text class="pnum" x="%d" y="%d" text-anchor="end">%d</text>\n' % (DEC_X0 - 30, y + 7, k + 1))
    w('  <text class="sig" x="%d" y="%d">%s</text>\n' % (DEC_X0 + 10, y + 6, LN[k]))
    w('  <line class="leg" x1="%d" y1="%d" x2="%d" y2="%d"/>\n' % (DEC_X1, y, DEC_X1 + 22, y))
    w('  <text class="pnum" x="%d" y="%d">%d</text>\n' % (DEC_X1 + 30, y + 7, 16 - k))
    w('  <text class="sig" x="%d" y="%d" text-anchor="end">%s</text>\n' % (DEC_X1 - 10, y + 6, RN[k]))

# decoder VCC (pin 16) up and away to the right; G1 to 3.3 V; G2B and GND to ground
w('  <path class="stub" d="M%d %d L%d %d L%d %d" stroke="var(--pwr)"/>\n'
  % (DEC_X1 + 22, DEC_PIN_Y[0], DEC_X1 + 58, DEC_PIN_Y[0], DEC_X1 + 58, DEC_PIN_Y[0] - 70))
w('  <text class="llbl" x="%d" y="%d" fill="var(--pwr)">3.3 V</text>\n' % (DEC_X1 + 66, DEC_PIN_Y[0] - 70))
w('  <text class="note2" x="%d" y="%d">100 nF from pin 16 to pin 8</text>\n' % (DEC_X1 + 66, DEC_PIN_Y[0] - 48))
w('  <path class="stub" d="M%d %d L%d %d" stroke="var(--pwr)"/>\n'
  % (DEC_X0 - 22, DEC_PIN_Y[5], DEC_X0 - 88, DEC_PIN_Y[5]))
w('  <text class="llbl" x="%d" y="%d" text-anchor="end" fill="var(--pwr)">3.3 V</text>\n'
  % (DEC_X0 - 96, DEC_PIN_Y[5] + 6))
for k in (4, 7):
    w('  <path class="stub" d="M%d %d L%d %d" stroke="var(--gnd)"/>\n'
      % (DEC_X0 - 22, DEC_PIN_Y[k], DEC_X0 - 88, DEC_PIN_Y[k]))
    w('  <text class="llbl" x="%d" y="%d" text-anchor="end" fill="var(--gnd)">GROUND</text>\n'
      % (DEC_X0 - 96, DEC_PIN_Y[k] + 6))
w('  <text class="note2" x="%d" y="%d" text-anchor="end">Y7, Y6, Y5, Y4: leave bare</text>\n'
  % (DEC_X1 + 132, DEC_PIN_Y[7] + 44))

# ---- the Teensy ----------------------------------------------------------------------------
w('  <rect class="body" x="30" y="160" width="156" height="1090" rx="6" fill="var(--panel)"/>\n')
w('  <text class="cname" x="108" y="200" text-anchor="middle">TEENSY</text>\n')
w('  <text class="grp" x="44" y="226">UNDERSIDE PADS</text>\n')
for lbl, sub, py, ex, fy, col in TEENSY:
    w('  <rect x="44" y="%d" width="128" height="38" fill="var(--panel-2)" stroke="%s" '
      'stroke-width="2.5"/>\n' % (py - 19, C[col]))
    w('  <text class="pnum" x="58" y="%d">%s</text>\n' % (py + 7, lbl))
    if sub:
        w('  <text class="sig" x="104" y="%d">%s</text>\n' % (py + 6, sub))

w('  <rect x="44" y="596" width="128" height="38" fill="var(--panel-2)" stroke="var(--ok)" '
  'stroke-width="2.5" stroke-dasharray="8 6"/>\n')
w('  <text class="pnum" x="58" y="622" fill="var(--ok)">48</text>\n')
w('  <text class="sig" x="104" y="621" fill="var(--ok)">SS0</text>\n')
w('  <text class="note" x="196" y="616" fill="var(--ok)">LEAVE THIS ONE ALONE</text>\n')
w('  <text class="note2" x="196" y="640">the chip already soldered here is what turns the bus on</text>\n')

w('  <text class="grp" x="44" y="768">TOP PINS</text>\n')
for lbl, y in zip(("2", "3", "4"), DEC_PIN_Y[:3]):
    w('  <rect x="44" y="%d" width="128" height="38" fill="var(--panel-2)" stroke="%s" '
      'stroke-width="2.5"/>\n' % (y - 19, C["cs"]))
    w('  <text class="pnum" x="58" y="%d">%s</text>\n' % (y + 7, lbl))
    w('  <path class="stub" d="M172 %d L%d %d" stroke="%s"/>\n' % (y, DEC_X0 - 22, y, C["cs"]))

SS1_Y = 1180
w('  <text class="grp" x="44" y="1148">UNDERSIDE PAD</text>\n')
w('  <rect x="44" y="%d" width="128" height="38" fill="var(--panel-2)" stroke="%s" '
  'stroke-width="2.5"/>\n' % (SS1_Y - 19, C["cs"]))
w('  <text class="pnum" x="58" y="%d">51</text>\n' % (SS1_Y + 7))
w('  <text class="sig" x="104" y="%d">SS1</text>\n' % (SS1_Y + 6))
w('  <path class="stub" d="M172 %d L%d %d L%d %d L%d %d" stroke="%s"/>\n'
  % (SS1_Y, 250, SS1_Y, 250, DEC_PIN_Y[3], DEC_X0 - 22, DEC_PIN_Y[3], C["cs"]))
w('  <text class="note" x="196" y="%d" fill="%s">to G2A &mdash; the real chip select</text>\n'
  % (SS1_Y + 40, C["cs"]))

# ---- pulldown inset, drawn away from everything --------------------------------------------
IX, IY = 44, 1300
w('  <rect x="%d" y="%d" width="560" height="180" fill="var(--panel)" stroke="%s" '
  'stroke-width="3"/>\n' % (IX, IY, C["cs"]))
w('  <text class="note" x="%d" y="%d" fill="%s">THE THREE PULLDOWNS &mdash; not optional</text>\n'
  % (IX + 20, IY + 34, C["cs"]))
for k, dp in enumerate(("1", "2", "3")):
    ry = IY + 68 + k * 34
    w('  <text class="sig" x="%d" y="%d">decoder pin %s</text>\n' % (IX + 20, ry + 5, dp))
    w('  <path class="stub" d="M%d %d L%d %d" stroke="%s" stroke-width="3.5"/>\n'
      % (IX + 150, ry, IX + 210, ry, C["cs"]))
    w('  <rect x="%d" y="%d" width="62" height="24" fill="var(--panel-2)" stroke="%s" '
      'stroke-width="2.5"/>\n' % (IX + 210, ry - 12, C["cs"]))
    w('  <text class="sig" x="%d" y="%d" text-anchor="middle">10k</text>\n' % (IX + 241, ry + 6))
    w('  <path class="stub" d="M%d %d L%d %d" stroke="var(--gnd)" stroke-width="3.5"/>\n'
      % (IX + 272, ry, IX + 330, ry))
    w('  <text class="llbl" x="%d" y="%d" fill="var(--gnd)">GROUND</text>\n' % (IX + 338, ry + 6))
w('  <text class="note2" x="%d" y="%d">They hold bank 0 selected while the Teensy probes the bus,</text>\n'
  % (IX + 20, IY + 176 - 18))

# ---- legend ---------------------------------------------------------------------------------
LX, LY = 660, 1300
w('  <rect x="%d" y="%d" width="640" height="180" fill="var(--panel)" stroke="var(--rule)" '
  'stroke-width="3"/>\n' % (LX, LY))
w('  <circle cx="%d" cy="%d" r="8" fill="var(--ink)"/>\n' % (LX + 30, LY + 34))
w('  <text class="note" x="%d" y="%d">A DOT means the wires are JOINED there.</text>\n' % (LX + 48, LY + 40))
w('  <path class="stub" d="M%d %d L%d %d" stroke="var(--ink)" stroke-width="4"/>\n'
  % (LX + 18, LY + 74, LX + 44, LY + 74))
w('  <path class="stub" d="M%d %d L%d %d" stroke="var(--ink)" stroke-width="4"/>\n'
  % (LX + 31, LY + 61, LX + 31, LY + 87))
w('  <text class="note" x="%d" y="%d">Lines crossing with NO dot are NOT joined.</text>\n' % (LX + 62, LY + 80))
leg = [("SCLK, the clock", "clk"), ("SIO0 to SIO3, the four data lines", "dat"),
       ("3.3 V", "pwr"), ("ground", "gnd"), ("chip select, one per chip", "cs")]
for k, (t, col) in enumerate(leg):
    ly = LY + 108 + k * 14
    w('  <rect x="%d" y="%d" width="26" height="5" fill="%s"/>\n' % (LX + 18, ly - 4, C[col]))
    w('  <text class="note2" x="%d" y="%d">%s</text>\n' % (LX + 54, ly + 5, t))

w('</svg>\n')

out = o.getvalue()
open(r'C:\Users\Danie\AppData\Local\Temp\claude\D--espicpc\26cc7934-6933-4694-bbbe-c6c98ad69441\scratchpad\board.svg',
     'w', encoding='utf-8').write(out)

# ---- collision check -------------------------------------------------------------------------
boxes = {"teensy": (30, 160, 186, 1250), "decoder": (DEC_X0 - 22, DEC_PIN_Y[0] - 46, DEC_X1 + 22, DEC_PIN_Y[7] + 46),
         "inset": (IX, IY, IX + 560, IY + 180), "legend": (LX, LY, LX + 640, LY + 180)}
for i, cy in enumerate(CHIP_Y):
    boxes["chip%d" % (i + 1)] = (CHIP_X0 - 22, cy, CHIP_X1 + 22, cy + CHIP_H)
def hit(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])
names = list(boxes)
bad = [(names[i], names[j]) for i in range(len(names)) for j in range(i + 1, len(names))
       if hit(boxes[names[i]], boxes[names[j]])]
print("wrote board.svg  %d bytes" % len(out))
print("box collisions:", bad if bad else "none")
print("lanes x:", sorted(l[0] for l in LANES), " cs lanes x:", [c[2] for c in CS])
print("viewBox fits: bottom-most element y =", max(IY + 180, LY + 180, CHIP_Y[3] + CHIP_H + 40), "of", VB_H)

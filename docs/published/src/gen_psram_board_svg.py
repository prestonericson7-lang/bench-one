# Generates the board-wiring SVG for ONE PSRAM on SS0 plus FIVE on the decoder.
#
# Every coordinate is computed from one table, and every piece of text is registered in a list with
# its estimated bounding box so the checker at the bottom can find overlaps. The previous version was
# written blind and stacked seven lane labels on top of each other.
import io

CHIP_X0, CHIP_X1 = 720, 1000
CHIP_H  = 180
CHIP_Y  = [220, 470, 720, 970, 1220]          # FIVE chips on the bank
PIN_DY  = [36, 72, 108, 144]                  # pins 1..4 down the left, 8..5 down the right

# Vertical lanes. The further right the lane, the higher its feed run, so no feed run crosses another.
LEFT  = [(680, 2, "SIO1", "dat", 120),
         (650, 3, "SIO2", "dat", 140),
         (620, 4, "VSS",  "gnd", 160)]
RIGHT = [(1040, 8, "VCC",  "pwr", 100),
         (1070, 7, "SIO3", "dat",  80),
         (1100, 6, "SCLK", "clk",  60),
         (1130, 5, "SIO0", "dat",  40)]
LANES = LEFT + RIGHT

# Teensy pads in the same top-to-bottom order as their feed lanes, escape x increasing downwards.
TEENSY = [("52", "IO0",  240, 212,  40, "dat"),
          ("53", "SCLK", 292, 232,  60, "clk"),
          ("54", "IO3",  344, 252,  80, "dat"),
          ("3V3", "",    396, 272, 100, "pwr"),
          ("49", "IO1",  448, 292, 120, "dat"),
          ("50", "IO2",  500, 312, 140, "dat"),
          ("GND", "",    552, 332, 160, "gnd")]

DEC_X0, DEC_X1 = 300, 470
DEC_TOP = 860
DEC_STEP = 45
DEC_PIN_Y = [DEC_TOP + k * DEC_STEP for k in range(8)]        # pins 1..8, and 16..9 opposite
# decoder output pin -> Yn -> lane x -> chip index.  Y0..Y4 = pins 15,14,13,12,11.
CS = [(15, 0, 500, 0), (14, 1, 528, 1), (13, 2, 556, 2), (12, 3, 584, 3), (11, 4, 598, 4)]

C = {"dat": "var(--dat)", "clk": "var(--clk)", "pwr": "var(--pwr)",
     "gnd": "var(--gnd)", "cs": "var(--cs)", "ink": "var(--ink)", "ink3": "var(--ink-3)",
     "ok": "var(--ok)", "panel": "var(--panel)"}
VB_W, VB_H = 1340, 1720

texts = []          # (x, y, anchor, size, family, string) -- for the collision checker
segs = []           # (x1,y1,x2,y2) every drawn wire segment

o = io.StringIO(); w = o.write

def T(x, y, s, size=16, fam="mono", anchor="start", fill="ink", cls=None, skip=False):
    """Emit a <text> and register it for collision checking."""
    a = '' if anchor == "start" else ' text-anchor="%s"' % anchor
    w('  <text x="%d" y="%d"%s font-family="var(--%s)" font-size="%d" font-weight="%s" '
      'fill="%s">%s</text>\n'
      % (x, y, a, {"mono": "mono", "disp": "disp", "body": "body"}[fam], size,
         "700" if fam != "body" else "600", C.get(fill, fill), s))
    if not skip:
        texts.append((x, y, anchor, size, fam, s))

def W_(d, col, width=5, dash=None, pts=None):
    extra = ' stroke-dasharray="%s"' % dash if dash else ''
    w('  <path d="%s" fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" '
      'stroke-linejoin="round"%s/>\n' % (d, C.get(col, col), width, extra))
    if pts:
        for i in range(len(pts) - 1):
            segs.append((pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]))

def DOT(x, y, col, r=7):
    w('  <circle cx="%d" cy="%d" r="%d" fill="%s"/>\n' % (x, y, r, C.get(col, col)))

w('<svg viewBox="0 0 %d %d" xmlns="http://www.w3.org/2000/svg" role="img"\n' % (VB_W, VB_H))
w('     aria-label="Board wiring: one PSRAM stays on the Teensy, five more sit on a decoder. Seven '
  'lanes run down the board and every chip taps all seven; each chip also gets its own chip-select '
  'wire from a different decoder output.">\n')

# ---- shared lanes -------------------------------------------------------------------------
LANE_FOOT = CHIP_Y[-1] + CHIP_H + 40          # every lane ends on the same line, below every chip
for x, pin, name, col, fy in LANES:
    W_("M%d %d L%d %d" % (x, fy, x, LANE_FOOT), col, pts=[(x, fy), (x, LANE_FOOT)])

# ---- Teensy feed runs ------------------------------------------------------------------------
for lbl, sub, py, ex, fy, col in TEENSY:
    lane = [l for l in LANES if l[4] == fy][0]
    lx = lane[0]
    W_("M186 %d L%d %d L%d %d L%d %d" % (py, ex, py, ex, fy, lx, fy), col,
       pts=[(186, py), (ex, py), (ex, fy), (lx, fy)])
    DOT(lx, fy, col)

# lane names go at the BOTTOM of each lane, rotated, where there is nothing else. Seven names
# across seven different x at 30 px apart will not fit horizontally; rotated they never touch.
for x, pin, name, col, fy in LANES:
    w('  <text transform="translate(%d %d) rotate(90)" font-family="var(--mono)" font-size="18" '
      'font-weight="700" fill="%s">%s</text>\n' % (x + 7, LANE_FOOT + 14, C[col], name))
    # register the rotated box: width becomes the glyph height, height becomes the string length
    texts.append((x + 7 - 14, LANE_FOOT + 14 + len(name) * 18 * 0.60, "start", 18, "mono", " "))

# ---- the five chips --------------------------------------------------------------------------
mid = (CHIP_X0 + CHIP_X1) // 2
for i, cy in enumerate(CHIP_Y):
    w('  <rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="var(--panel-2)" '
      'stroke="var(--ink)" stroke-width="3"/>\n' % (CHIP_X0, cy, CHIP_X1 - CHIP_X0, CHIP_H))
    DOT(CHIP_X0 + 22, cy + 20, "ink", 7)
    # Name ABOVE the body. Inside it there is no row free between the pins, and outside it the
    # nearest lane is only 40 px away -- the first version put the pin numbers straight on top of it.
    T(mid, cy - 12, "PSRAM %d  \u00b7  bank %d" % (i + 1, i), 24, "disp", "middle")
    PNAME = {1: "CE#", 2: "SIO1", 3: "SIO2", 4: "VSS", 5: "SIO0", 6: "SCLK", 7: "SIO3", 8: "VCC"}
    for k in range(4):
        y = cy + PIN_DY[k]
        # left leg, number and name INSIDE the outline
        w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="5" '
          'stroke-linecap="round"/>\n' % (CHIP_X0, y, CHIP_X0 - 22, y))
        T(CHIP_X0 + 12, y + 7, str(k + 1), 19, "mono")
        T(CHIP_X0 + 34, y + 6, PNAME[k + 1], 15, "mono", fill="ink3")
        # right leg
        w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="5" '
          'stroke-linecap="round"/>\n' % (CHIP_X1, y, CHIP_X1 + 22, y))
        T(CHIP_X1 - 12, y + 7, str(8 - k), 19, "mono", "end")
        T(CHIP_X1 - 34, y + 6, PNAME[8 - k], 15, "mono", "end", fill="ink3")
    for x, pin, name, col, fy in LEFT:
        y = cy + PIN_DY[pin - 1]
        W_("M%d %d L%d %d" % (CHIP_X0 - 22, y, x, y), col, pts=[(CHIP_X0 - 22, y), (x, y)])
        DOT(x, y, col)
    for x, pin, name, col, fy in RIGHT:
        y = cy + PIN_DY[8 - pin]
        W_("M%d %d L%d %d" % (CHIP_X1 + 22, y, x, y), col, pts=[(CHIP_X1 + 22, y), (x, y)])
        DOT(x, y, col)
    # 100 nF, pin 8 round the bottom to pin 4
    capy = cy + 164
    W_("M%d %d L%d %d L%d %d L%d %d L%d %d"
       % (CHIP_X1 + 22, cy + PIN_DY[0], CHIP_X1 + 10, cy + PIN_DY[0], CHIP_X1 + 10, capy,
          CHIP_X0 - 10, capy, CHIP_X0 - 10, cy + PIN_DY[3]), "ink3", 3, dash="9 7")
    w('  <rect x="%d" y="%d" width="54" height="24" fill="var(--panel)" stroke="var(--ink-3)" '
      'stroke-width="2.5"/>\n' % (mid - 27, capy - 12))
    T(mid, capy + 6, "104", 15, "mono", "middle", fill="ink3")

# ---- chip-select wires -----------------------------------------------------------------------
for dec_pin, yn, lx, ci in CS:
    dy = DEC_PIN_Y[yn + 1] if yn + 1 < 8 else DEC_PIN_Y[7]
    ty = CHIP_Y[ci] + PIN_DY[0]
    W_("M%d %d L%d %d L%d %d L%d %d" % (DEC_X1 + 22, dy, lx, dy, lx, ty, CHIP_X0 - 22, ty),
       "cs", pts=[(DEC_X1 + 22, dy), (lx, dy), (lx, ty), (CHIP_X0 - 22, ty)])
    # Put the tag on the side the wire does NOT arrive from, centred on its own lane. The lanes are
    # only 22 px apart, so anything anchored sideways lands on a neighbour.
    T(lx, ty - 22 if ty < dy else ty + 28, "Y%d" % yn, 17, "mono", "middle", fill="cs")

# ---- the decoder -----------------------------------------------------------------------------
w('  <rect x="%d" y="%d" width="%d" height="%d" rx="6" fill="var(--panel-2)" stroke="var(--ink)" '
  'stroke-width="3"/>\n' % (DEC_X0, DEC_TOP - 44, DEC_X1 - DEC_X0, DEC_STEP * 7 + 88))
w('  <path d="M%d %d a32 32 0 0 0 64 0" fill="var(--panel)" stroke="var(--ink)" stroke-width="3"/>\n'
  % (DEC_X0 + 53, DEC_TOP - 44))
dmid = (DEC_X0 + DEC_X1) // 2
T(dmid, DEC_TOP - 58, "74LVC138A", 24, "disp", "middle")

LN = ["A", "B", "C", "G2A", "G2B", "G1", "Y7", "GND"]
RN = ["VCC", "Y0", "Y1", "Y2", "Y3", "Y4", "Y5", "Y6"]
for k, y in enumerate(DEC_PIN_Y):
    w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="5" '
      'stroke-linecap="round"/>\n' % (DEC_X0, y, DEC_X0 - 22, y))
    T(DEC_X0 + 12, y + 7, str(k + 1), 19, "mono")
    T(DEC_X0 + 40, y + 6, LN[k], 15, "mono", fill="ink3")
    w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="5" '
      'stroke-linecap="round"/>\n' % (DEC_X1, y, DEC_X1 + 22, y))
    T(DEC_X1 - 12, y + 7, str(16 - k), 19, "mono", "end")
    T(DEC_X1 - 40, y + 6, RN[k], 15, "mono", "end", fill="ink3")

# power / ground / spare, all on SHORT tags so nothing runs into a wire
# Pin 16 rises at x=484, inside the gap between the decoder legs and the first chip-select lane
# at x=500. Taking it out to +56 put the label straight across the Y2 lane.
W_("M%d %d L%d %d L%d %d" % (DEC_X1 + 22, DEC_PIN_Y[0], 484, DEC_PIN_Y[0], 484, DEC_TOP - 96),
   "pwr", pts=[(DEC_X1 + 22, DEC_PIN_Y[0]), (484, DEC_PIN_Y[0]), (484, DEC_TOP - 96)])
T(490, DEC_TOP - 102, "3.3 V", 17, "mono", "end", fill="pwr")
# The Teensy outline ends at x=186, so these stubs have only 30 px of clear space to their left.
# Labels therefore sit ABOVE each stub, left-aligned from 194, instead of being anchored at its end.
W_("M%d %d L%d %d" % (DEC_X0 - 22, DEC_PIN_Y[5], 200, DEC_PIN_Y[5]), "pwr",
   pts=[(DEC_X0 - 22, DEC_PIN_Y[5]), (200, DEC_PIN_Y[5])])
T(196, DEC_PIN_Y[5] - 10, "3V3", 15, "mono", fill="pwr")
for k in (4, 7):
    W_("M%d %d L%d %d" % (DEC_X0 - 22, DEC_PIN_Y[k], 200, DEC_PIN_Y[k]), "gnd",
       pts=[(DEC_X0 - 22, DEC_PIN_Y[k]), (200, DEC_PIN_Y[k])])
    T(196, DEC_PIN_Y[k] - 10, "GND", 15, "mono", fill="gnd")
T(196, DEC_PIN_Y[6] - 10, "Y7", 15, "mono", fill="ink3")

# ---- the Teensy ------------------------------------------------------------------------------
w('  <rect x="30" y="160" width="156" height="1240" rx="6" fill="var(--panel)" stroke="var(--ink)" '
  'stroke-width="3"/>\n')
T(108, 192, "TEENSY", 25, "disp", "middle")
T(44, 216, "UNDERSIDE PADS", 14, "disp", fill="ink3")
for lbl, sub, py, ex, fy, col in TEENSY:
    w('  <rect x="44" y="%d" width="128" height="36" fill="var(--panel-2)" stroke="%s" '
      'stroke-width="2.5"/>\n' % (py - 18, C[col]))
    T(56, py + 7, lbl, 19, "mono")
    if sub:
        T(102, py + 6, sub, 15, "mono", fill="ink3")

# SS0: short tag only. The explanation lives in the prose, not on top of a wire.
w('  <rect x="44" y="594" width="128" height="36" fill="var(--panel-2)" stroke="var(--ok)" '
  'stroke-width="2.5" stroke-dasharray="8 6"/>\n')
T(56, 619, "48", 19, "mono", fill="ok")
T(102, 618, "SS0", 15, "mono", fill="ok")
T(44, 658, "LEAVE ALONE", 15, "disp", fill="ok")
T(44, 678, "your 1 chip", 14, "mono", fill="ok")

T(44, DEC_PIN_Y[0] - 34, "TOP PINS", 14, "disp", fill="ink3")
for lbl, y in zip(("2", "3", "4"), DEC_PIN_Y[:3]):
    w('  <rect x="44" y="%d" width="128" height="36" fill="var(--panel-2)" stroke="%s" '
      'stroke-width="2.5"/>\n' % (y - 18, C["cs"]))
    T(56, y + 7, lbl, 19, "mono")
    W_("M172 %d L%d %d" % (y, DEC_X0 - 22, y), "cs", pts=[(172, y), (DEC_X0 - 22, y)])

SS1_Y = DEC_PIN_Y[3] + 220
T(44, SS1_Y - 32, "UNDERSIDE PAD", 14, "disp", fill="ink3")
w('  <rect x="44" y="%d" width="128" height="36" fill="var(--panel-2)" stroke="%s" '
  'stroke-width="2.5"/>\n' % (SS1_Y - 18, C["cs"]))
T(56, SS1_Y + 7, "51", 19, "mono")
T(102, SS1_Y + 6, "SS1", 15, "mono", fill="ink3")
W_("M172 %d L%d %d L%d %d L%d %d" % (SS1_Y, 240, SS1_Y, 240, DEC_PIN_Y[3], DEC_X0 - 22, DEC_PIN_Y[3]),
   "cs", pts=[(172, SS1_Y), (240, SS1_Y), (240, DEC_PIN_Y[3]), (DEC_X0 - 22, DEC_PIN_Y[3])])

# ---- legend, well clear of everything --------------------------------------------------------
LX, LY = 690, 1510
w('  <rect x="%d" y="%d" width="620" height="172" fill="var(--panel)" stroke="var(--rule)" '
  'stroke-width="3"/>\n' % (LX, LY))
DOT(LX + 28, LY + 30, "ink", 8)
T(LX + 46, LY + 36, "a DOT means JOINED here", 17, "body")
w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="4"/>\n'
  % (LX + 16, LY + 66, LX + 42, LY + 66))
w('  <line x1="%d" y1="%d" x2="%d" y2="%d" stroke="var(--ink)" stroke-width="4"/>\n'
  % (LX + 29, LY + 53, LX + 29, LY + 79))
T(LX + 46, LY + 72, "crossing with NO dot is NOT joined", 17, "body")
for k, (t, col) in enumerate([("clock", "clk"), ("data", "dat"), ("3.3 V", "pwr"),
                              ("ground", "gnd"), ("chip select", "cs")]):
    lx2 = LX + 20 + k * 118
    w('  <rect x="%d" y="%d" width="26" height="6" fill="%s"/>\n' % (lx2, LY + 112, C[col]))
    T(lx2, LY + 142, t, 14, "mono", fill="ink3")

w('</svg>\n')
out = o.getvalue()
open(r'C:\Users\Danie\AppData\Local\Temp\claude\D--espicpc\26cc7934-6933-4694-bbbe-c6c98ad69441\scratchpad\board.svg',
     'w', encoding='utf-8').write(out)

# ================= CHECKS =====================================================================
CW = {"mono": 0.60, "disp": 0.46, "body": 0.52}       # width per character, as a fraction of size

def bbox(t):
    x, y, anchor, size, fam, s = t
    wpx = len(s) * size * CW[fam]
    if anchor == "middle":  x0 = x - wpx / 2
    elif anchor == "end":   x0 = x - wpx
    else:                   x0 = x
    return (x0, y - size * 0.78, x0 + wpx, y + size * 0.24)

def olap(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])

rects = []          # (x0,y0,x1,y1) opaque boxes that text must not hide behind
for _l, _sub, _py, _ex, _fy, _c in TEENSY:
    rects.append((44, _py - 18, 172, _py + 18))
for _y in DEC_PIN_Y[:3]:
    rects.append((44, _y - 18, 172, _y + 18))
rects.append((44, 594, 172, 630))
rects.append((44, SS1_Y - 18, 172, SS1_Y + 18))

bad = []
for i in range(len(texts)):
    for j in range(i + 1, len(texts)):
        if olap(bbox(texts[i]), bbox(texts[j])):
            bad.append(("TEXT/TEXT", texts[i][5], texts[j][5],
                        tuple(round(v) for v in bbox(texts[i]))))

# text sitting on top of a wire
def seg_hits_box(s, b):
    x1, y1, x2, y2 = s
    if x1 == x2:                                   # vertical
        return b[0] <= x1 <= b[2] and not (max(y1, y2) < b[1] or min(y1, y2) > b[3])
    if y1 == y2:                                   # horizontal
        return b[1] <= y1 <= b[3] and not (max(x1, x2) < b[0] or min(x1, x2) > b[2])
    return False

for t in texts:
    b = bbox(t)
    for s in segs:
        if seg_hits_box(s, b):
            bad.append(("TEXT/WIRE", t[5], "seg %s" % (tuple(map(round, s)),),
                        tuple(round(v) for v in b)))
            break

for t in texts:
    b = bbox(t)
    for r in rects:
        # Text INSIDE a box is the label of that box and is fine. Only text that straddles an edge
        # is a fault: half of it is then hidden behind an opaque fill.
        inside = b[0] >= r[0] and b[2] <= r[2] and b[1] >= r[1] and b[3] <= r[3]
        if olap(b, r) and not inside:
            bad.append(("TEXT/BOX", t[5], "rect %s" % (r,), tuple(round(v) for v in b)))
            break

print("wrote board.svg  %d bytes   texts=%d  wire segments=%d" % (len(out), len(texts), len(segs)))
if bad:
    print("!! %d PROBLEM(S):" % len(bad))
    for b in bad[:40]:
        print("   ", b[0], repr(b[1]), "vs", repr(b[2]), b[3])
else:
    print("no text overlaps, no text sitting on a wire")

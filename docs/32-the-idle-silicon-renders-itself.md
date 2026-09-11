# 32 — The idle silicon renders itself, and the split is exact

*Everything below was measured on 2026-09-10 on a Luckfox Pico (armv7l, 33 MB of RAM) and this
desktop. No figure here is estimated. Where a prediction is quoted, the measurement that tested it
is quoted next to it.*

---

## 1. What was being proved

Twenty-nine of the thirty-three boards in BENCH ONE were given the verdict "carries no weights":
five Luckfox Picos at 13 MB, nine Teensy 4.1 at 16, fifteen ESP32-S3 at 8. During inference they
have nothing to do. The claim under test was that they are not therefore useless — that they can
carry the machine's entire visual surface, and carry it *together*, with the picture split across
them.

Splitting a picture is only interesting if the pieces fit back together perfectly. If band seams
show, the fleet is a pile of boards. If they do not, the fleet is one renderer.

## 2. The frame is bit-identical across architectures

`machine_view` draws the machine's own topology: thirty-three boxes, one per board, height set by
memory on a log scale, the lit box marking where the current token sits in the pipeline. Integer
edge functions, 16-bit z-buffer, RGB565, 480×320.

| where it ran | md5 of the 460,815-byte frame |
| --- | --- |
| this desktop, x86-64 | `f19fe057ae38e1f1809d8e2a59c4bbb7` |
| Luckfox Pico, armv7l | `f19fe057ae38e1f1809d8e2a59c4bbb7` |

Not "visually the same". The same bytes. The rasteriser uses integer edge functions and integer
z-comparison, so there is no floating-point rounding left in the pixel decision, and two different
instruction sets agree exactly.

That is the licence for everything that follows: any node can render any part of any frame, and the
result does not depend on which node did it.

## 3. Thirty-three bands stitch with zero error

Each node is given a row range `[BY0, BY1)` and allocates **only those rows**. A board that owns
1/33 of a 480×320 frame holds 16–18 KB of framebuffer plus depth, not 600 KB. That is the
difference between an ESP32 being able to help and not.

Three stitches were checked against the single-node frame:

| stitch | result |
| --- | --- |
| 33 bands, all rendered on the desktop | identical |
| 33 bands, all rendered on the Luckfox | identical |
| 17 bands on x86-64, 16 on ARM, interleaved | identical |

The third is the one that matters. Alternating bands between two different processors produces the
same 460,815 bytes as one processor doing all of it.

### The bug that had to be fixed to get there

The first binned version failed the stitch. `tri()` truncates its vertices toward zero when it casts
them to int, so a corner at y = −0.5 is *outside* the band by a float test and lands on row 0 once
it is an integer. The band-rejection test now carries two pixels of margin. Without the stitch check
this would have shipped as a one-pixel seam nobody would have found by looking.

## 4. Equal rows is the wrong split, and by how much

Over a 120-frame orbit the scene writes 3,355,365 pixels. Only rows 82 to 272 ever draw anything at
all — 188 of 320. Handing every node an equal number of rows hands twelve of thirty-three nodes an
empty band.

| split across 33 nodes | slowest node carries | share of the frame | speedup available |
| --- | --- | --- | --- |
| equal rows | 333,155 px | 9.9% | 10.1× |
| equal pixels | 122,559 px | 3.7% | 27.4× |

Measured on the board with five bands, the naive split gave 700.3 fps and the pixel-balanced split
1316.0 fps for the same picture — 1.9× for changing where the cuts go and nothing else.

## 5. A cost model that predicts the board to 1%

Equal-pixel splitting was predicted to give 1630 fps and measured 1316. The gap is real work that is
not proportional to lit pixels: clearing rows, and setting up all 1,584 triangles whether they land
in this band or not.

Fitting `time = c·frames + a·rows·frames + b·pixels` to ten measured bands:

| term | fitted value |
| --- | --- |
| fixed, per frame | 159.4 µs |
| per row swept and cleared | 607 ns |
| per pixel filled | 91.0 ns → 10.99 M px/s |

Worst error on the busy bands: 3.0%. The model then predicted one board alone at 345.0 fps; the
board measured 338.8. **1.8% error on a figure the model never saw.**

## 6. The fixed term is a ceiling, and binning moves it

`1 / 159.4 µs` = 6,274 fps. No number of boards beats that, because every board pays the triangle
setup for the whole scene. Splitting rows without splitting geometry stops scaling as soon as that
term dominates.

The fix is one comparison. The eight corners of each box are already projected; their y-range bounds
all twenty-four of its triangles, so one test throws the box away before any triangle is set up.

| | fixed per frame | per pixel | 33-board frame rate | hard ceiling |
| --- | --- | --- | --- | --- |
| unbinned | 159.4 µs | 91.0 ns | 3,953 fps | 6,274 fps |
| binned | 54.1 µs | 101.0 ns | 6,319 fps | 18,475 fps |

Binning costs 7% on a node that owns the whole frame — the scan never rejects anything there — so it
is skipped when `BY1 - BY0 == H`. Measured: 345 fps with the scan always on, 321 with it guarded.

## 7. What the fleet actually does

Measured on the Luckfox Pico, cost-model splits, the slowest band setting the frame rate:

| boards | slowest band | speedup | efficiency |
| --- | --- | --- | --- |
| 1 | 323.3 fps | 1.00× | — |
| 5 | 1,492.6 fps | 4.62× | 92% |
| 33 | 5,597.9 fps | 17.3× | 52% |

Predicted 1,476.5 for five (measured 1,492.6, **1.1% error**) and 6,319 for thirty-three (measured
5,597.9, 11% optimistic — the fixed term is not perfectly constant across very small bands).

A panel refreshes at 60 fps. Five of the cheapest boards in the machine overshoot that by 25×.

## 8. What this costs the model

Nothing. Not "a small amount" — nothing. The boards doing this hold no weights, appear in no
pipeline stage, and are idle for the entire decode. A graphics card running inference has no such
silicon: every shader core watching the model is a shader core not running it. That is the part of
heterogeneous that never shows up in a FLOPS total.

## 9. The 4.6% that profiling costs

The row histogram that makes the balanced split possible is one increment per lit pixel. Whole-frame
rate went from 338.8 fps to 323.3 with it compiled in. Worth it — without the histogram the split
is a guess, and the guess was 1.9× off.

## 10. Reproducing it

```
gcc -O3 -std=c11 -o machine_view.exe machine_view.c -lm
ROWPROFILE=rows.txt ./machine_view.exe 480 320 120 whole.ppm
./machine_view.exe 480 320 120 band_7.ppm 7 33          # band 7 of 33
./machine_view.exe 480 320 120 band.ppm 0 1 128 192     # explicit rows 128..192
```

Getting a file off a Luckfox: `adb push` reports success and writes nothing, and `adb exec-out`
returns zero bytes. What works is a python3 listener on the board plus `adb forward`, and
`base64 -w0` through `adb shell` stdout coming back. The board's tar is busybox and has no `-z`;
pipe through `gzip -c` instead.

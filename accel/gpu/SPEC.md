# FPGA-GPU system specification (v1)

Single source of truth. Every component (RTL, Zynq daemon, Pi software, Teensy firmware, golden
model, tests) implements exactly this. Binary layouts live in `common/gpu_proto.h`; APIs in
`common/gpu_setup.h`, `common/gpu_refrast.h`, `common/geom.h`. If this document and a header
disagree, the header's layout wins and this document must be fixed.

## 1. System

```
 Orange Pi 4 Pro (host, app runs here)
   |-- Gigabit Ethernet, TCP 7777: direct cable 10.77.0.1 <-> 10.77.0.2, or the repo's car LAN
   |      10.20.0.1 <-> 10.20.0.2 (the daemon listens on every Zynq address)
   |      -> Zynq PS (Debian 12, fpgagpud daemon) -> AXI GP0 -> PL GPU (PS command FIFO)
   |-- USB 2.0 HS (Teensy CDC serial)
          -> Teensy 4.1 geometry engine (transform, light, clip, project, triangle setup)
               -> 16-bit parallel bus on JM1 -> PL GPU (Teensy command FIFO)

 PL GPU (Zynq XC7Z020 fabric):
   two command FIFOs -> collector (per-frame list, double-buffered, BRAM)
   -> strip rasteriser (1280x16 strips in BRAM, Z-buffer, Gouraud, sprites)
   -> AXI HP1 writes strips into the back framebuffer in DDR3
   -> AXI HP0 scanout reads the front framebuffer -> TMDS -> HDMI 1280x720@60 (DVI signalling)
   AXI HP2 reads sprite pixels from DDR3 (uploaded by the daemon into the pool).
```

Two-way (section 13): finished frames come back from the FPGA to the Pi (return-capture buffer ->
daemon -> TCP -> window on the Pi's desktop), and the Teensy can return its computed triangles.

Roles: the Pi decides *what* to draw. The Teensy is the geometry/vertex stage (and does
triangle setup) for 3D meshes. The Zynq ARM does setup for triangles the Pi sends over
Ethernet, and manages sprites. The PL does all per-pixel work and the video output.

## 2. Clocks, resets, pins (PL)

- Input: 50 MHz on `U18` (LVCMOS33).
- One MMCME2_ADV: CLKIN 50 MHz, DIVCLK_DIVIDE 1, CLKFBOUT_MULT_F 14.875 -> VCO 743.75 MHz.
  - CLKOUT0_DIVIDE_F 5 -> **core 148.75 MHz** (collector, raster, AXI masters/slave, bus receiver)
    (v1 draft used divide 7 = 106.25 MHz; first OOC synthesis showed ~4.4 ns slack at 9.4 ns, so the
    core runs at 148.75 MHz = 6.72 ns. Any "106.25" left in older text means the core clock.)
  - CLKOUT1_DIVIDE 2  -> **serial 371.875 MHz** (TMDS OSERDES CLK, 5x pixel, DDR)
  - CLKOUT2_DIVIDE 10 -> **pixel 74.375 MHz** (video timing, TMDS encode, OSERDES CLKDIV)
  All via BUFG. Core and pixel domains are asynchronous to each other (CDC only through the
  async FIFO and 2-FF/toggle synchronisers); pixel and serial are related (OSERDES).
- Reset: `rst` (core) and `rst_pix` (pixel) are synchronous active-high, released a few cycles
  after MMCM LOCKED in each domain. The design does not use PS FCLKs or PS resets.
- The PS AXI ports used: M_AXI_GP0 (32-bit, regs at 0x43C00000), S_AXI_HP0 (64-bit, scanout
  reads), S_AXI_HP1 (64-bit, strip writes), S_AXI_HP2 (64-bit, sprite reads). All their ACLKs
  are the core clock.

Pin map (all LVCMOS33 unless noted; ball names from the Puzhi manual/xlsx):

| Signal | Ball | Dir | Note |
|---|---|---|---|
| clk50 | U18 | in | PL 50 MHz |
| led[0] (LED1) | R19 | out | heartbeat ~1 Hz |
| led[1] (LED2) | V13 | out | toggles every 30 rendered frames |
| hdmi_d0_p/n | V17/V18 | out | TMDS_33, blue + sync |
| hdmi_d1_p/n | W18/W19 | out | TMDS_33, green |
| hdmi_d2_p/n | N17/P18 | out | TMDS_33, red |
| hdmi_clk_p/n | T17/R18 | out | TMDS_33, pixel clock |
| hdmi_out_en | P16 | out | drive 1 |
| hdmi_hpd | P15 | in | monitor present |
| tb_d[0..15] | JM1 pins 9..24 | in | Teensy bus data, see WIRING table |
| tb_sor | JM1 pin 25 | in | start-of-record |
| tb_strobe | JM1 pin 26 | in | transfer strobe (toggle) |
| tb_busy | JM1 pin 27 | out | 1 = do not start a record |

JM1 pin -> ball: 9 E18, 10 F16, 11 E19, 12 F17, 13 G17, 14 B19, 15 G18, 16 A20, 17 D19, 18 C20,
19 D20, 20 B20, 21 J18, 22 K19, 23 H18, 24 J19, 25 K17, 26 M17, 27 K18. JM1 pins 3,4,33,34,35,36
are GND; the bus uses 4, 33, 34, 35. (JM1 pins 1/3/5/7 = 5 V, GND, H16, H17 stay free for the fan
design in the board repo.)

So: tb_d[0]=E18, [1]=F16, [2]=E19, [3]=F17, [4]=G17, [5]=B19, [6]=G18, [7]=A20, [8]=D19,
[9]=C20, [10]=D20, [11]=B20, [12]=J18, [13]=K19, [14]=H18, [15]=J19, tb_sor=K17,
tb_strobe=M17, tb_busy=K18.

## 3. Teensy parallel bus

Teensy 4.1 side (verified against the Teensy core `core_pins.h`, GPIO6 fast port bits 16..31):

| bus bit | Teensy pin | GPIO6 bit | JM1 pin |
|---|---|---|---|
| D0 | 19 | 16 | 9 |
| D1 | 18 | 17 | 10 |
| D2 | 14 | 18 | 11 |
| D3 | 15 | 19 | 12 |
| D4 | 40 | 20 | 13 |
| D5 | 41 | 21 | 14 |
| D6 | 17 | 22 | 15 |
| D7 | 16 | 23 | 16 |
| D8 | 22 | 24 | 17 |
| D9 | 23 | 25 | 18 |
| D10 | 20 | 26 | 19 |
| D11 | 21 | 27 | 20 |
| D12 | 38 | 28 | 21 |
| D13 | 39 | 29 | 22 |
| D14 | 26 | 30 | 23 |
| D15 | 27 | 31 | 24 |
| SOR | 3 | GPIO9 bit 5 | 25 |
| STROBE | 2 | GPIO9 bit 4 | 26 |
| BUSY (in) | 4 | GPIO9 bit 6 | 27 |
| GND | GND | | 4, 33, 34, 35 (pin 3 = fan GND, 36 spare) |

Writing the 16 data bits = one write: `GPIO6_DR_TOGGLE = ((prev ^ word) & 0xFFFF) << 16`.

Protocol (Teensy is the only driver of D/SOR/STROBE):
- A transfer: drive D[15:0] and SOR, wait >= 30 ns, toggle STROBE (each edge, rising or
  falling, is one transfer), wait >= 60 ns before changing D/SOR again. Default firmware
  timing: 30 ns setup, 60 ns hold.
- A 32-bit word = two transfers, low half first. A record = 24 words = 48 transfers.
  SOR = 1 on the first transfer of a record (low half of word 0) and 0 on all others.
- Before starting each record the Teensy waits for BUSY = 0. Once started, a record is sent
  completely without checking BUSY (the FPGA guarantees >= 64 free FIFO words when BUSY = 0).
- FPGA receiver (core clock, T = 6.72 ns): 2-FF synchronisers on all 18 inputs, then a STROBE
  glitch filter: a new STROBE level counts as one transfer only after it has been seen in 3
  consecutive synchronised samples. A pulse shorter than 2 T (13.4 ns, e.g. ringback or
  crosstalk) never counts; a level held >= 21 ns (3 T plus margin) always does. The accepting clock takes
  D/SOR as sampled 2 clocks after the first sample of the new STROBE level, i.e. 2..4 T
  (<= 27 ns) after the STROBE edge, inside the 60 ns hold. The minimum bus timing is unchanged:
  30 ns setup, 60 ns hold (so each STROBE level lasts >= 90 ns).
  SOR resets the half-word phase. Assembled words are pushed with bit 32 = SOR-of-word-0.
- BUSY = 1 when Teensy FIFO free < 64 entries, or in reset. The Teensy firmware keeps D, SOR
  and STROBE as high-impedance inputs until it has seen BUSY = 0 continuously for >= 10 ms
  (BUSY input has the Teensy pull-up, so an unpowered or unconfigured FPGA reads as busy).
  If BUSY stays 1 for > 500 ms while it needs to send, it returns the pins to inputs, reports
  GPU_ERR_BUS for that frame, and re-arms; while idle, BUSY = 1 for > 500 ms also releases them.
  So an unpowered FPGA is never driven before the bus is armed (only BUSY's 22 kOhm pull-up reaches
  it), but once armed the Teensy keeps driving the bus for at least 500 ms after the FPGA loses
  power: power the Teensy down before the FPGA (WIRING.md).

## 4. Setup math (gpu_setup.c) -- exact

All targets compile with fp-contract off. `double` is used for gradients; everything else is
integer. Rounding functions: `llrint` (round-half-even, default FP environment).

Input vertex: `x,y` float pixels (pixel (px,py) has centre (px+0.5, py+0.5)), `z` float clamped
to [0,1], `r,g,b` uint8.

1. Snap: `X_i = (int32)llrint((double)x_i * 16)`, `Y_i` likewise (units 1/16 px).
   Z_i = `(double)clamp(z_i,0,1) * 65535.0 * 4096.0`; C_i = `(double)c_i * 65536.0`.
2. `area2 = (int64)(X1-X0)*(Y2-Y0) - (int64)(X2-X0)*(Y1-Y0)`. If 0 -> return 0.
   area2 > 0 means clockwise on screen (y down). Cull: CULL_CW rejects area2 > 0, CULL_CCW
   rejects area2 < 0. If area2 < 0 after culling, swap vertices 1 and 2 (area2 becomes > 0).
3. Guard band (noclip variant): every vertex needs `GPU_GUARD_XMIN*16 <= X < GPU_GUARD_XMAX*16`
   and the same for Y, else return -1. `gpu_setup_tri` clips the triangle in float against
   x = -256, x = 1536, y = -256, y = 976 (Sutherland-Hodgman in that order, attributes z,r,g,b
   interpolated linearly with t = d0/(d0-d1) computed in double, colours rounded with
   `llrint` and clamped to 0..255), fans the polygon (v0, vi, vi+1) and sets up each piece
   with the noclip variant. A clip-generated vertex that still rounds outside is nudged
   inside by clamping X/Y to the guard band.
4. Bounding box (inclusive pixel indices whose centre can be inside):
   `px_min = ceil_div(minX - 8, 16)`, `px_max = floor_div(maxX - 8, 16)` (true floor/ceil on
   negatives), same for y; clamp to [0,1279] x [0,719]. If `px_min > px_max` or
   `py_min > py_max` -> return 0.
5. Edges (after the swap): edge0 = v1->v2, edge1 = v2->v0, edge2 = v0->v1. For edge a->b:
   `A = -16*(Yb-Ya)`, `B = 16*(Xb-Xa)`,
   `E = (int64)(Xb-Xa)*(16*py_min + 8 - Ya) - (int64)(Yb-Ya)*(16*px_min + 8 - Xa)`,
   top-left bias: if `!(A > 0 || (A == 0 && B > 0))` then `E -= 1`.
   A, B, E stored as int32 (the guard band guarantees |E| < 2^31 inside the screen).
6. Gradients (per pixel), attribute values V0,V1,V2 in fixed units (step 1), in double:
   `dvdx = 16 * ((V1-V0)*(Y2-Y0) - (V2-V0)*(Y1-Y0)) / area2`
   `dvdy = 16 * ((V2-V0)*(X1-X0) - (V1-V0)*(X2-X0)) / area2`
   `vstart = V0 + dvdx*(16*px_min + 8 - X0)/16 + dvdy*(16*py_min + 8 - Y0)/16`
   Gradients are stored as `(int32)clamp(llrint(value), INT32_MIN, INT32_MAX)`. **Start values
   (vstart) are stored modulo 2^32** (`(uint32)(int64)llrint(value)`), NOT clamped: the start is
   extrapolated to the bbox corner and can exceed int32 on steep triangles, while every covered
   pixel's value is in range; the renderer works modulo 2^32, so covered pixels come out exact.
   (Found and fixed during implementation; clamping gave wrong flat depth/colour.)
   (Vi, Xi, Yi are the snapped/fixed values after the swap.)
7. Record: w0 = TRI<<28 | (flags & (ZTEST|ZWRITE)) | px_max<<11 | px_min, w1 = py_max<<11 |
   py_min, then edges, z, r, g, b as in gpu_proto.h, w23 = 0.

Rect: bbox = [max(x0,0), min(x1,1280)-1] x [max(y0,0), min(y1,720)-1]; empty -> 0.
flags | NOEDGE; A/B/E all 0; z0 = llrint(clamp(z)*65535*4096), r0 = (R8<<16), g0, b0 where
R8/G8/B8 are the RGB565 expanded to 8 bits (r8 = r5<<3 | r5>>2, g8 = g6<<2 | g6>>4,
b8 = b5<<3 | b5>>2); all gradients 0.

Sprite: requires x % 4 == 0, w % 4 == 0, w >= 4, h >= 1, src % 8 == 0, stride % 8 == 0 else -1.
If y < 0: src += (-y)*stride, h += y, y = 0. If x < 0: src += (-x)*2, w += x, x = 0.
If x >= 1280 or y >= 720 or w <= 0 or h <= 0 -> 0. Clamp w to 1280, h to 720 (the PL clips at
the right/bottom screen edge itself).

## 5. Rendering semantics (PL and golden model) -- exact

Per frame the PL renders one list (records in arrival order, see section 7) into the back
buffer. The observable result is identical to this full-frame algorithm (the PL's 16-row
strips are an implementation detail):

```
fb[all] = clear_color ; zb[all] = 0xFFFF
for each record in list order:
  TRI:  for y in ymin..ymax, x in xmin..xmax:       (bbox is always inside the screen)
          dx = x-xmin ; dy = y-ymin                  (uint32 arithmetic, wraps)
          covered = NOEDGE or all (int32)(E0_i + A_i*dx + B_i*dy) >= 0
          if !covered: continue
          z  = (int32)(z0 + dzdx*dx + dzdy*dy)
          zp = z < 0 ? 0 : (z >> 12) > 65535 ? 65535 : z >> 12
          pass = !ZTEST || zp <= zb[y][x]
          if !pass: continue
          if ZWRITE: zb[y][x] = zp
          c(ch) = (int32)(ch0 + dchdx*dx + dchdy*dy) ; c8 = c<0 ? 0 : (c>>16)>255 ? 255 : c>>16
          fb[y][x] = (r8>>3)<<11 | (g8>>2)<<5 | (b8>>3)
  SPRITE: for row in 0..h-1 with y+row < 720, col in 0..w-1 with x+col < 1280:
          p = ddr16[src + row*stride + col*2]
          if COLORKEY and p == key: continue
          fb[y+row][x+col] = p                        (Z untouched)
  other types: ignored
```
Framebuffer in DDR: pixel (x,y) at byte `fb_base + (y*1280 + x)*2`, little-endian RGB565.
A 64-bit AXI beat carries pixels 4k..4k+3 with pixel 4k in bits [15:0].

## 6. Teensy geometry (geom.c) -- exact, float32, fp-contract off

Per draw: `MVP = viewproj * model` with `MVP[c*4+r] = ((VP[0*4+r]*M[c*4+0] + VP[1*4+r]*M[c*4+1])
+ VP[2*4+r]*M[c*4+2]) + VP[3*4+r]*M[c*4+3]` (column-major, summed in that order).

Per vertex (clip = MVP * (px,py,pz,1), each component summed in order m0*px + m1*py + m2*pz + m3):
- If LIGHTING: `n = M3 * (nx,ny,nz)` (upper-left 3x3 of model, same summation order),
  `len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z)`; if `len > 0` divide each component by len;
  `d = -((n.x*L.x + n.y*L.y) + n.z*L.z)`; `if (d < 0) d = 0`;
  `k = ambient + (1.0f - ambient) * d`. Else `k = 1.0f`.
- colour channel: `c = (float)vc * ((float)mul / 255.0f) * k`, then clamp to [0,255].

Per triangle (indices i0,i1,i2 in index order):
- Outcodes against the planes, in this order and with these distances:
  near `z + w`, far `w - z`, left `x + GX*w`, right `GX*w - x`, bottom `y + GY*w`,
  top `GY*w - y`; GX = 1.35f, GY = 1.65f (inside the setup guard band).
  All three vertices with distance < 0 for one plane -> trivial reject (culled).
  All distances >= 0 for all vertices -> no clipping.
  Otherwise clip the polygon (Sutherland-Hodgman, plane order above; t = d0/(d0-d1);
  x,y,z,w,r,g,b lerped as `a0 + t*(a1-a0)`), count it in tris_clipped.
- A clipped polygon with any vertex `w <= 1e-6f` is dropped (tris_culled).
- Project each polygon vertex: `iw = 1.0f/w; sx = (x*iw*0.5f + 0.5f)*1280.0f;
  sy = (0.5f - y*iw*0.5f)*720.0f; sz = z*iw*0.5f + 0.5f`; colour to uint8 as
  `(uint8_t)(c + 0.5f)` after clamping to [0,255].
- Fan (p0, pi, pi+1) and call `gpu_setup_tri_noclip` with flags ZTEST/ZWRITE from the draw and
  cull = CULL_CW if CULL_BACK else NONE. Result 1 -> emit; 0 -> tris_culled; -1 -> tris_culled.
- After all draws: emit END(frame_no).

Mesh convention: counter-clockwise front faces (OpenGL), outward normals.

## 7. Collector, lists, frames (PL)

- Two FIFOs, 33 bits wide ({sor, data}): Teensy (1024 entries), PS (512 entries).
- A record starts with a word whose sor = 1; exactly 24 words. A sor=1 word arriving mid-record
  discards the partial record (BAD_RECORDS += 1) and starts a new one. sor=0 words while no
  record is open are discarded (BAD_RECORDS += 1 once per run of such words).
- A list part is taken from each enabled source, **Teensy first, then PS**: the collector reads
  records from that source until it reads an END record. TRI and SPRITE records are appended
  to the current list (if the list already holds 1536, the record is dropped, LIST_OVERFLOW += 1).
  NOP is ignored. Unknown types: BAD_RECORDS += 1, ignored.
- A disabled source is continuously drained and discarded (DROPPED += words).
- When every enabled source has delivered its END, the list is complete (even if empty).
  If no source is enabled, nothing completes.
- Two lists ping-pong: the collector fills one while the raster renders the other.
- The raster renders a complete list into the back buffer (= !front), waits for all AXI write
  responses, then raises swap_req. The scanout reader swaps at the start of fetching its next
  frame (front ^= 1, FRAME_COUNT += 1, pulse swap_done), which guarantees no tearing and that
  the old front is no longer read. Then the raster frees that list and may start the next.
- SOFT_RESET (CONTROL bit 0): resets bus receiver, both FIFOs and the collector (open records
  and lists not currently being rendered are discarded). It never resets the raster, strip
  writer or any AXI master mid-transaction; a list being rendered finishes normally.

## 8. Scanout and video

- 1280x720@60 CEA timing at 74.375 MHz: H active 1280, front 110, sync 40, back 220 (1650);
  V active 720, front 5, sync 5, back 20 (750); HSYNC and VSYNC active high. DVI signalling
  (no data islands). Channel 0 = blue carries {vsync, hsync} as C1,C0 during blanking.
- TMDS encoding per DVI 1.0 (transition-minimised, DC-balanced); control tokens
  00: 10'b1101010100, 01: 10'b0010101011, 10: 10'b0101010100, 11: 10'b1010101011;
  serialised LSB first with OSERDESE2 master/slave 10:1 DDR; clock lane sends 10'b0000011111.
- RGB565 -> RGB888: r8 = {r5, r5[4:2]}, g8 = {g6, g6[5:4]}, b8 = {b5, b5[4:2]}.
- Reader (core domain) fetches the whole front buffer with 16-beat INCR bursts into a
  65-bit async FIFO ({sof, data64}), sof on the first word of each frame. The video side,
  at the first active pixel of line 0, discards words until one with sof; during active video
  it pops one word per 4 pixels; an unexpected sof mid-frame, or an empty FIFO, makes it output
  black until the next frame start (then it resynchronises).
- SCANOUT_EN = 0: output 8 vertical colour bars (white, yellow, cyan, green, magenta, red,
  blue, black) with a 1-pixel white border and a small square that moves one pixel per frame.
- VSYNC_COUNT counts video frames (toggle-synchronised into the core domain).

## 9. AXI rules (HP ports are AXI3)

- 64-bit data, AxSIZE = 3 (8 bytes), AxBURST = INCR, AxLEN <= 15 (16 beats), AxID = 0,
  AxCACHE = 4'b0011, AxPROT = 0, AxLOCK = 0, AxQOS = 0, WSTRB = 8'hFF, WID = AWID.
- No burst crosses a 4 KB boundary. Framebuffer strips (40960 B, 4 KB aligned) and the whole
  frame are written/read as 128-byte aligned bursts. Sprite reads split bursts at 4 KB.
- All handshakes obey VALID/READY rules (VALID never depends on READY; payload stable while
  VALID && !READY). Any BRESP/RRESP != OKAY increments AXI_ERRORS.
- GP0 slave (AXI3, 32-bit, 12-bit IDs): supports single beats and INCR bursts (each beat is its
  own register access; bursts to 0x100/0x104 push every beat), echoes IDs, OKAY responses.
  Writes to FIFO addresses when the PS FIFO is full are dropped (software checks
  PS_FIFO_FREE first). Unmapped reads return 0.

## 10. Zynq software

Platform: the repo's PZ7020-StarLite image (1 GB DDR3, Linux 6.12 armhf, built by
hardware/pz7020-starlite/linux/*.sh). The GPU's PL is merged into the platform bitstream at the same
addresses (registers 0x43C00000 on GP0, DDR window 0x1E000000 + 32 MB); the platform's own register
file pl_regs (ID 0x5A702001) is at 0x40000000.

- Device tree: `reserved-memory { gpu@1e000000 { reg = <0x1e000000 0x2000000>; no-map; } }`
  (with 1 GB the window is in the middle of RAM; the platform device tree must carry this node).
- Boot script: after a successful `fpga loadb` of pl.bit it appends `fpgagpu.pl_loaded=1` to
  bootargs (Linux's zynq-fpga driver clears PCFG_DONE when it probes), and it keeps U-Boot from
  relocating the device tree into a reserved window (`fdt_high`).
- Network: the daemon listens on 0.0.0.0:7777, so every eth0 address works: the platform's car-LAN
  10.20.0.2/24 and DHCP lease, and 10.77.0.2/24 where an image adds it.
- `fpgagpud` (C, static, runs as root via systemd, unit `zynq/fpgagpud.service`): before touching
  GP0 it checks that a bitstream is loaded (devcfg PCFG_DONE, or the boot marker with no PL reset
  since boot), the PS-PL level shifters, that no "System RAM" range in /proc/iomem overlaps the DDR
  window, and the ID/VERSION registers at 0x43C00000 (read in a forked child; 0x40000000 is read
  first and may hold the platform's pl_regs); mmaps regs (0x43C00000) and the 32 MB DDR window with
  O_SYNC; client handling (controller + observers, event-driven) per section 13.2
  (a new controller gets a soft reset, CONTROL = SRC_PS|SCANOUT_EN, sprite pool reset).
  At start it renders one frame of clear colour 0x0010 (dark blue): colour bars = bitstream
  loaded but daemon not running, dark blue = daemon running, anything else = an app.
- Pushing a record: wait until PS_FIFO_FREE >= 24, write word 0 to PS_FIFO_SOR, words 1..23
  to PS_FIFO_DATA.
- Sprite pool: per-id slot (id < 256); re-upload with a size that fits reuses the slot,
  otherwise bump-allocate (8-byte aligned, stride = w*2 rounded up to 8). RESET frees all.

## 11. Pi software

- `libfpgagpu` (C): connection to the daemon (TCP) and to the Teensy (tty, or `tcp:host:port`
  for the simulator). Teensy tty autodetect: `/dev/serial/by-id/*Teensy*`, then `/dev/ttyACM*`.
  Raw termios, blocking I/O with timeouts. Daemon address: `--fpga HOST[:PORT]`, else the
  environment `FPGAGPU_HOST=HOST[:PORT]`, else 10.77.0.2 and 10.20.0.2 tried together with
  10.77.0.2 preferred (10.20.0.2 is taken when 10.77.0.2 fails or has not answered within 150 ms).
- Tools: `gpu_selftest` (bit-exact end-to-end tests of both paths + Teensy geometry + fps),
  `gpu_demo` (Teensy-transformed 3D meshes + Pi-drawn HUD sprite over Ethernet), `gpu_stat`,
  `gpu_snap` (readback -> PPM/PNG), `gpu_image` (show a PPM image full screen).
- `pi_setup.sh`: adds 10.77.0.1/24 to the wired interface (NetworkManager profile if present),
  unless the repo's car LAN owns that port (NetworkManager profile `car-lan`, or 10.20.0.1 on it):
  then the network is left alone and the tools reach the Zynq at 10.20.0.2. Adds the user to
  `dialout`, installs the tools to /usr/local/bin.

## 13. Two-way path: results flow back to the Pi

The Pi submits work; the Teensy and the FPGA return results to the Pi.

### 13.1 FPGA frame return (PL)
- A 2 MB return-capture buffer at RET_ADDR (reset 0x1FE00000, inside the reserved window; the
  sprite pool is 0x1E400000..0x1FDFFFFF).
- The collector remembers, per list, `frame_no` = the w1 of the last END record that completed
  the list (the PS END if SRC_PS is enabled, else the Teensy END).
- When the raster starts a list and RET_ENABLE = 1 and RET_FULL = 0, it captures that frame:
  the strip writer writes every strip both to the back buffer and to RET_ADDR + s*0xA000
  (RET_CAPTURING = 1). When the frame's last write response arrives (the same moment swap_req
  rises) RET_FULL = 1, RET_CAPTURING = 0, RET_FRAME = that list's frame_no.
- Writing RET_CTRL with bit1 (RET_ACK) set clears RET_FULL (buffer released; the next frame that
  starts rendering is captured). ACK while not full does nothing. RET_ENABLE = 0 stops starting
  new captures (a capture in progress completes).
- LAST_FRAME_NO = frame_no of the list most recently swapped to the front.
- The capture never stalls or slows rendering of the visible framebuffer beyond the extra
  write bandwidth (a second 40 KB strip write per strip).

### 13.2 Zynq daemon
- Multi-client, event-driven (poll loop, never blocks on one client): up to 4 clients.
  NET_HELLO with empty payload (or flags without GPU_HELLO_OBSERVER) makes the client the
  controller (previous controller demoted to observer; soft reset, CONTROL = SRC_PS|SCANOUT_EN,
  pool reset, as before). Observers may only use HELLO, STATUS, WAIT_FRAME, READBACK, FRAME_GET,
  SYNC; other messages from observers are rejected (error counted, connection kept).
- WAIT_FRAME and FRAME_GET are parked and completed from the poll loop (1 ms register polling)
  so other clients keep being served.
- NET_FRAME_GET: the daemon sets RET_ENABLE while any FRAME_GET is pending. When RET_FULL:
  if `(int32)(RET_FRAME - min_frame_no) < 0` it ACKs (discard) and keeps waiting; otherwise it
  copies the buffer (scale 1: whole frame; scale 2: every 2nd pixel of every 2nd row) into a
  cached buffer, ACKs immediately (so the PL captures the next frame while the daemon sends),
  and replies with the pixels. Timeout -> GPU_ERR_TIMEOUT.
- Sim backend: identical capture semantics (copy of each rendered frame into the RET region when
  enabled and not full, RET_FRAME/LAST_FRAME_NO maintained).

### 13.3 Teensy geometry return
- T_FRAME honours geom_frame_hdr.flags: GEOM_FRAME_RETURN appends every emitted record (TRI and
  the END) to the reply (t_frame_reply.nrecs_returned then the records); GEOM_FRAME_NO_BUS skips the
  parallel bus entirely (pure geometry service: the Pi gets the setup-ready triangles back).

### 13.4 Pi
- `libfpgagpu`: `gpu_frame_get()` (FRAME_GET), observer connections, Teensy return mode.
- `gpu_view`: shows the frames the FPGA renders in a window on the Pi's own desktop. Dependency-
  free X11 client (raw X11 protocol over the UNIX socket /tmp/.X11-unix/X<n> from $DISPLAY,
  MIT-MAGIC-COOKIE-1 from $XAUTHORITY or ~/.Xauthority, ZPixmap PutImage split to the server's
  maximum request length, RGB565 converted to the server's 24/32-bit pixel format and byte
  order). Connects as an observer, loops FRAME_GET(min = last+1), window title shows fps.
  Options: --scale 1|2. Exits cleanly when the window is closed.
- `gpu_demo --view`: the demo also shows its own returned frames on the Pi's screen (in-process,
  same viewer code), so no second monitor is needed.
- `gpu_selftest`: adds tests for FRAME_GET (returned frame bit-exact vs the golden model, both
  scales), Teensy GEOM_FRAME_RETURN (returned records == locally predicted geom.c records), and
  reports return-path frames/s at both scales.

## 14. Teensy autonomous mode (the Teensy runs flat out on its own)

- T_SCENE stores a persistent scene: up to 64 objects (mesh, flags, base model matrix, spin axis
  and rate, colour multiplier), view + projection matrices, light, optional camera orbit, and up
  to 32 raw overlay records (e.g. SPRITE records for a HUD whose pixels the Pi updates in the
  daemon's pool by re-uploading the same sprite id; TRI/rect records also allowed).
- T_AUTO enable=1 starts the loop: t = seconds since enable (float, from the cycle counter /
  micros); for each object `model(t) = model * R(spin_axis, spin_rate*t)` (Rodrigues rotation
  matrix, column-major), `view(t) = view * RY(-cam_orbit_rate*t)`, `viewproj = proj * view(t)`;
  build geom_draws, run geom_frame (emitting to the bus) but insert the overlay records after the
  last TRI and before the END; frame_no = running counter; repeat immediately (or at max_fps).
  The FPGA paces it naturally (BUSY back-pressure + the list ping-pong + vsync-locked swaps).
- Between frames (and between records if a frame is long) the firmware keeps servicing USB
  messages, so T_SCENE / T_AUTO / T_STATS / T_HELLO work while running. T_FRAME / T_RECORDS
  while autonomous mode runs -> GPU_ERR_ARG. T_RESET stops it.
- The Pi sets CONTROL = SRC_TEENSY (+ SCANOUT_EN) on the daemon for autonomous mode (no PS END
  needed per frame). The two-way path (section 13) still works: gpu_view shows these frames on
  the Pi.
- Stats: t_stats_reply auto_* fields, cpu_busy_pct, tris_per_sec, cpu_mhz.
- Build: default 600 MHz; `build_teensy.cmd 816` builds the overclocked 816 MHz variant
  (Arduino menu speed=816).

## 12. Simulation (x86, WSL) -- the whole software stack without hardware

- `fpgagpud --sim`: same daemon, but the PL is modelled in software: FIFOs, collector
  semantics of section 7, `gpu_refrast_frame` for rendering, immediate swap. It also listens
  on TCP 7778 for the simulated Teensy bus (stream of raw 96-byte records).
- `teensy_sim`: runs geom.c + the Teensy message handler; listens on TCP 7779 for the Pi
  protocol; sends its records to 127.0.0.1:7778.
- Pi tools run on x86 with `--fpga 127.0.0.1 --teensy tcp:127.0.0.1:7779`.

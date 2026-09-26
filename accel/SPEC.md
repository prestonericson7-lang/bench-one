# zaccel — the Zynq and Teensy as the Orange Pi's accelerators

The user's goal, in his words: *"build the fpga and teensy to super boost the orange pi so that its
much much faster"*; *"the fpgas were the gpus"*; *"the fpga has 1gb of ddr3 it does add ram to the
system"*. This file is the contract every part is built to. Nothing here is optional scope.

```
 Orange Pi 4 Pro ──GbE── Zynq PS (Linux) ──AXI GP0──► AXI DMA regs
   libzaccel (C)           zaccel-server             │
   zaccel.py               nbd-server (RAM)          ▼ HP0 (DDR3)
   zaccel-bench            reserved DDR3 ──DMA──► zaccel_gemv (PL, AXI-Stream) ──DMA──► results
   nbd swap  ◄─────────── 1 GB DDR3 as the Pi's extra memory
```

Two things the Zynq adds to the Pi:
1. **Compute** — the PL matrix engine (the "GPU"): int4/int8 weights × int8 activations, batched,
   weights resident in the Zynq's DDR3 so only activations cross the wire.
2. **Memory** — part of the Zynq's DDR3 exported to the Pi as a network block device, used as
   swap above any disk swap.

---

## 1. PL engine: `zaccel_gemv` (AXI-Stream, 64-bit)

Clock: FCLK0 (100 MHz). One AXI-Stream slave `s_axis` (tdata 64, tvalid, tready, tlast ignored), one
AXI-Stream master `m_axis` (tdata 64, tkeep 8'hFF, tvalid, tready, tlast). Active-low reset `aresetn`.

### Input stream (from DMA MM2S), little-endian 64-bit beats
1. **Header**, one beat:
   | bits | field |
   |---|---|
   | 15:0 | magic `0x5A41` |
   | 19:16 | mode: 0 = W int4 × A int8, 1 = W int8 × A int8 |
   | 23:20 | batch `nb` − 1 (nb = 1..8 activation vectors) |
   | 31:24 | reserved, 0 |
   | 47:32 | cols `K` (1..4096) |
   | 63:48 | rows `N` (1..65535) |
   A beat whose magic is wrong is dropped; the engine keeps looking for a header.
2. **Activations**: for v = 0..nb−1, `ceil(K/8)` beats; byte i of beat j = activation `8j+i` of
   vector v (int8). Unused tail bytes are 0.
3. **Weights**: N rows; each row starts on a fresh beat.
   - mode 0: `ceil(K/16)` beats per row; byte i holds weight `2i` in bits 3:0 and `2i+1` in bits 7:4
     (low nibble first, signed −8..7). Unused tail nibbles are 0.
   - mode 1: `ceil(K/8)` beats per row; byte i = weight i (int8).
   Weights may arrive as a separate DMA transfer from the header/activations; the engine counts beats
   and ignores TLAST on input.

### Output stream (to DMA S2MM)
For each row r = 0..N−1, for each v = 0..nb−1, one signed int32 `y[r][v] = Σ_k W[r][k]·A[v][k]`,
packed two per beat in emission order (first value in bits 31:0). If the total count N·nb is odd the
last beat's bits 63:32 are 0. Then one **trailer** beat with TLAST=1:
| bits | field |
|---|---|
| 31:0 | cycles from header accepted to last weight beat accepted |
| 47:32 | rows completed (N mod 65536) |
| 63:48 | magic `0x5A45` |

Accumulators are 32-bit signed (|Σ| ≤ 4096·128·128 < 2³¹). Results are exact integers: the CPU
reference must match bit for bit.

### Throughput
One weight beat per clock when both streams flow: mode 0 = 16 weights × nb vectors per clock,
mode 1 = 8 × nb. At 100 MHz, nb = 8: 12.8 G MAC/s (int4), 6.4 G MAC/s (int8).

---

## 2. PL system (Vivado, `hardware/pz7020-starlite/vivado/build_system.tcl`)

| block | address | notes |
|---|---|---|
| `pl_regs` | 0x4000_0000, 4 KB | existing (LEDs, keys, fan, ID) |
| `axi_dma` (AXI DMA, simple mode, no SG, 64-bit streams, 26-bit length) | 0x4040_0000, 64 KB | MM2S + S2MM, memory side on **S_AXI_HP3** (HP0-2 belong to the GPU) |
| `zaccel_gemv` | — | between DMA M_AXIS_MM2S and S_AXIS_S2MM; `aresetn` = the DMA's MM2S and S2MM stream resets AND the system reset, so a DMA soft reset (DMACR bit 2) also clears an aborted job |

Weight transfers larger than the 26-bit length register are split into 32 MB chunks (the engine counts beats and ignores TLAST).
DMA registers used (PG021): MM2S_DMACR 0x00, MM2S_DMASR 0x04, MM2S_SA 0x18, MM2S_LENGTH 0x28,
S2MM_DMACR 0x30, S2MM_DMASR 0x34, S2MM_DA 0x48, S2MM_LENGTH 0x58. Polling (no interrupt needed).

## 3. Zynq Linux

- **Reserved DDR3 for the engine**: 384 MB at physical `0x2000_0000`–`0x37FF_FFFF`, `no-map`,
  exposed by a `generic-uio` node named `zaccel-mem`. The AXI DMA register window is a
  `generic-uio` node named `zaccel-dma`. Userspace finds them by `/sys/class/uio/uio*/name`.
- `boot.cmd` sets `fdt_high` and `initrd_high` to `0xffffffff` so U-Boot never relocates the
  device tree into the reserved range.
- `zaccel-server` (C, armhf, systemd) owns both UIO devices.
- `nbd-server` exports a RAM-backed file (tmpfs) of `ZACCEL_SWAP_MB` (default 256) as export
  `zynqram` on TCP 10809.

## 4. Wire protocol (TCP 8093, little-endian)

Request: `u32 magic 0x3151415A ("ZAQ1") · u32 op · u32 seq · u32 len · payload[len]`
Reply:   `u32 magic 0x3152415A ("ZAR1") · u32 status · u32 seq · u32 len · payload[len]`
status 0 = OK, 1 = bad request, 2 = no memory, 3 = engine error, 4 = unknown tensor.
One request at a time per connection; the server serves several connections.

| op | name | request payload | reply payload |
|---|---|---|---|
| 1 | INFO | — | `u32 version=1 · u32 engine (0 cpu, 1 pl) · u32 mem_total_mb · u32 mem_free_mb · u32 max_cols=4096 · u32 max_batch=8 · u32 selftest (0 pass, else fail code)` |
| 2 | LOAD | `u32 mode · u32 rows · u32 cols · bytes`: exactly `rows × rowbytes`, row-major, each row packed as in §1 **and padded to whole 8-byte beats**: `rowbytes = 8·ceil(cols/16)` (mode 0) or `8·ceil(cols/8)` (mode 1). Any other length → status 1. | `u32 tensor_id` (ids are per server, shared by all connections) |
| 3 | FREE | `u32 tensor_id` | — |
| 4 | GEMV | `u32 tensor_id · u32 nb · int8 A[nb][cols]` (plain, unpadded) | `u32 cycles · u32 engine_used · int32 Y[rows][nb]` |
| 5 | PING | any bytes | the same bytes |

`engine_used`: 1 when the PL ran it, 0 when the CPU fallback did. The server falls back to its CPU
path whenever the PL or DMA is absent, and the results are identical either way.

## 5. Orange Pi side (`accel/pi/`)

- `libzaccel` (C, no deps): connect, info, load, free, gemv; plus `zaccel_ref_gemv()` — the CPU
  reference that every result is checked against.
- `zaccel.py`: the same API over sockets (numpy optional).
- `zaccel-bench`: checks every answer, then measures on the units: Pi CPU alone, Zynq alone, and
  both at once with the rows split (the combined figure is the boost). Numbers only count from the
  real boards.
- `zaccel-swap`: attaches the Zynq's `zynqram` export with `nbd-client` and enables it as swap at
  priority 100 (above disk swap); systemd unit, idempotent, safe when the Zynq is absent.

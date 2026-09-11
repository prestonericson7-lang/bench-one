#!/usr/bin/env python3
"""
newcpu.py -- what a memory-centric processor computes that no PC or GPU can, and why

THE QUESTION
------------
Not "how fast". Today's machines are faster at everything they can do at all. The question is what
falls entirely outside what they can do, and the answer has one shape: models that do not fit, and
observation that cannot be afforded.

A GPU is a training machine used for inference by ignoring most of it. At batch one, decode reads
every active weight exactly once and there is almost no arithmetic to reuse, so an H100's thousand
teraflops sit idle behind 3.35 TB/s of memory. Its real limit is the 80 GB. A PC's real limit is
that its memory and its compute are at opposite ends of a bus.

This models the other shape: DRAM die wire-bonded in package beside logic that unpacks 4-bit weights
in fabric, with a small watcher core per memory channel, replicated until the model fits.

WHAT IS MEASURED, WHAT IS PUBLISHED, WHAT IS ASSUMED
----------------------------------------------------
MEASURED IN THIS PROJECT, on real hardware or out of real model files:
  * 4-bit unpack in fabric: 256 lanes is 18.9% of an XC7Z020, and 71% duty at 64 lanes against a
    2.5 GB/s feed. Unpacking is free in fabric and is the whole reason the compute can be this small.
  * an activation between nodes is 4 KB, and a real machine-to-machine hop costs 148 us.
  * a 30B mixture-of-experts touches 11% of itself per token, read out of the file.
  * expert use concentrates 3 to 5 times against uniform, measured from the router on real text.
  * int8 KV cache costs nothing in perplexity; 4-bit destroys the model.
  * 33 heterogeneous boards rendered one frame as 33 bands that stitched byte-identically, so
    splitting work across unlike processors composes exactly.

PUBLISHED:
  * energy per bit by memory path, accelerator capacity and bandwidth, DeepSeek-V3's 5.5% active
    fraction, the roughly 20% of feed-forward neurons that fire for a given token.

ASSUMED, and labelled at every use: bond pitch, achievable die edge, watcher core size.

RUN
    python newcpu.py [--tiles 32] [--model 4t] [--sparsity contextual|moe]
"""

import argparse
import sys

# ------------------------------------------------------------------------------------------------
#  ENERGY PER BIT. The term that decides whether this can be cooled without moving air.
#  PUBLISHED ranges; the midpoint is used.
# ------------------------------------------------------------------------------------------------
PJ_BIT = {
    "ddr_offpackage": 25.0,   # DDR4/5 across a PCB, I/O plus DRAM core
    "wirebond_inpkg": 4.0,    # short reach die to die, under 2 mm of wire
    "hbm":            4.0,    # through-silicon vias and microbumps
    "sram_ondie":     0.1,
}

# ------------------------------------------------------------------------------------------------
#  THE TILE. One logic die with DRAM wire-bonded beside it.
#
#  The pad budget is the design: a die edge at a bond pitch gives a pad count, and a wide-and-slow
#  channel is the lowest-energy way to spend it. That is why HBM is 1024 bits at 2 Gbps rather than
#  32 bits at 6.4 -- you skip the high-speed serialisers entirely, and the serialisers are where the
#  picojoules go.
# ------------------------------------------------------------------------------------------------
TILE = {
    "die_edge_mm":    10.0,   # ASSUMED: a 10 x 10 mm logic die
    "bond_pitch_um":  60.0,   # ASSUMED: achievable with a used manual wedge bonder
    "edges_used":     4,
    "chan_data_bits": 128,    # wide and slow
    "chan_ctrl_pads": 20,     # command, address, clock, strobes, power returns
    "chan_mt_s":      800.0,  # MT/s, deliberately slow: wire bond inductance is the limit
    "gb_per_stack":   32.0,   # ASSUMED: 4 x 64 Gbit DDR4 die wire-bonded per channel
    "watchers_per_chan": 1,   # a small core per channel, see WATCHER
}

WATCHER = {
    "mm2_at_28nm": 0.03,      # PUBLISHED scale: a small RISC-V or Cortex-M class core
    "mhz":         400.0,
    "bytes_per_cycle": 4.0,   # what it can record to its own channel without contending
}

# ------------------------------------------------------------------------------------------------
#  BARE DIE, STACKED AND WIRE BONDED. The assembly route that needs no fab, only a bonder.
#
#  This is where the density nobody has comes from, and it is not exotic: NAND has shipped in 16 and
#  32 die stacks for years, and most LPDDR in phones is wire-bonded package-on-package. The parts are
#  bought, not made. What is unusual is doing it for MODEL MEMORY rather than for storage.
#
#  All PUBLISHED or straightforwardly geometric except the packing fraction, which is the one real
#  unknown and is therefore swept rather than assumed.
# ------------------------------------------------------------------------------------------------
DIE = {
    "gb":            2.0,    # PUBLISHED: a 16 Gbit DDR4 x8 die
    "area_mm2":      65.0,   # PUBLISHED: 16 Gb DDR4 on a 1x nm node is 60-75 mm2
    "thinned_um":    70.0,   # PUBLISHED: DRAM is thinned to 50-80 um for stacking
    "attach_um":     20.0,   # die attach film between layers
    "refresh_mw":    36.0,   # PUBLISHED IDD6 self-refresh, ~30 mA at 1.2 V, PER DIE, DOING NOTHING
    "refresh_hot_x": 2.0,    # above 85 C the refresh interval halves, so the power doubles
}

#  What the same memory costs in volume the way it is sold today. PUBLISHED dimensions.
#  name: (GB, cm3)
PACKAGED = {
    "DDR5 64 GB RDIMM":  (64.0,  33.3),   # 133.35 x 31.25 x 8 mm with its heat spreader
    "H100 SXM5 module":  (80.0,  46.0),   # about 82 x 56 x 10 mm
}

# ------------------------------------------------------------------------------------------------
#  MODELS. total and active parameters in billions, bits per weight.
# ------------------------------------------------------------------------------------------------
MODELS = {
    "30b":  (30.0,   3.0,  4.5, "MEASURED in this project: 11% touched per token"),
    "671b": (671.0,  37.0, 4.5, "PUBLISHED DeepSeek-V3 shape, 5.5% active"),
    "4t":   (4000.0, 220.0, 4.5, "5.5% active held at 4T"),
    "20t":  (20000.0, 1100.0, 4.5, "5.5% active held at 20T"),
}

# PUBLISHED: roughly a fifth of feed-forward neurons fire for a given token, and which fifth is
# predictable from the activation. A GPU cannot exploit it because it pays for a whole cache line and
# a whole warp either way. A processor per memory channel can skip at row granularity.
CONTEXTUAL_SPARSITY = 0.20

# ------------------------------------------------------------------------------------------------
#  WHAT EXISTS, for the capacity ceiling comparison. PUBLISHED specs.
#  name: (GB per unit, GB/s per unit, watts per unit, max units in one machine, note)
# ------------------------------------------------------------------------------------------------
EXISTING = {
    "desktop PC":        (192.0,  100.0,  150.0, 1,  "2 channels of DDR5, consumer platform ceiling"),
    "RTX 5090":          (32.0,   1790.0, 575.0, 1,  "GDDR7"),
    "H100 80GB":         (80.0,   3350.0, 700.0, 8,  "HBM3, 8 per SXM node"),
    "MI300X":            (192.0,  5300.0, 750.0, 8,  "HBM3, 8 per node"),
    "2-socket server":   (6144.0, 921.0,  800.0, 1,  "24 DDR5-4800 channels, 256 GB RDIMMs"),
    "Groq LPU":          (0.23,   80000.0, 375.0, 576, "SRAM only, which is the whole problem"),
    "Cerebras WSE-3":    (44.0,   21000.0, 23000.0, 1, "wafer scale SRAM"),
}


def gb(params_b, bits):
    return params_b * 1e9 * bits / 8.0 / 1024.0 ** 3


def tile_spec(t):
    """Pads, channels, capacity and bandwidth for one tile."""
    pads_per_edge = int(t["die_edge_mm"] * 1000.0 / t["bond_pitch_um"])
    pads = pads_per_edge * t["edges_used"]
    per_chan = t["chan_data_bits"] + t["chan_ctrl_pads"]
    chans = pads // per_chan
    gbs = chans * t["chan_data_bits"] / 8.0 * t["chan_mt_s"] / 1000.0
    cap = chans * t["gb_per_stack"]
    return pads, chans, cap, gbs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiles", type=int, default=32)
    ap.add_argument("--model", default="4t", choices=sorted(MODELS))
    ap.add_argument("--sparsity", default="moe", choices=("moe", "contextual"))
    a = ap.parse_args()

    pads, chans, tile_gb, tile_gbs = tile_spec(TILE)
    total_b, active_b, bits, mnote = MODELS[a.model]
    need = gb(total_b, bits)
    active = gb(active_b, bits)
    if a.sparsity == "contextual":
        active *= CONTEXTUAL_SPARSITY

    sys_gb = tile_gb * a.tiles
    sys_gbs = tile_gbs * a.tiles * 0.71          # MEASURED fabric duty cycle

    print()
    print("=" * 76)
    print("  a memory-centric processor: what it computes that nothing else can")
    print("=" * 76)

    # ---- 1. the tile --------------------------------------------------------------------------
    print("  1. ONE TILE, from the pad budget outward")
    print("     %.0f x %.0f mm die at %.0f um bond pitch      %5d pads on %d edges"
          % (TILE["die_edge_mm"], TILE["die_edge_mm"], TILE["bond_pitch_um"], pads,
             TILE["edges_used"]))
    print("     %d data + %d control pads a channel          %5d channels"
          % (TILE["chan_data_bits"], TILE["chan_ctrl_pads"], chans))
    print("     %d bits at %.0f MT/s, wide and slow          %7.1f GB/s a tile"
          % (TILE["chan_data_bits"], TILE["chan_mt_s"], tile_gbs))
    print("     %.0f GB of DDR4 die wire-bonded a channel    %7.1f GB a tile"
          % (TILE["gb_per_stack"], tile_gb))
    print("     unpack lanes needed, MEASURED: 256 lanes is 18.9% of an XC7Z020, so the")
    print("     logic beside the memory is small. The memory is the machine.")
    print()

    # ---- 2. the system ------------------------------------------------------------------------
    print("  2. %d TILES" % a.tiles)
    print("     resident model memory                      %8.1f GB" % sys_gb)
    print("     aggregate bandwidth, at the measured 0.71   %8.1f GB/s" % sys_gbs)
    print("     watcher cores, one per channel               %6d" % (chans * a.tiles))
    print()

    # ---- 3. what it runs ----------------------------------------------------------------------
    print("  3. WHAT IT RUNS")
    print("     %-10s %10s %10s %12s %10s" % ("model", "resident", "per token", "seconds", "tok/s"))
    for k in ("30b", "671b", "4t", "20t"):
        tb, ab, bw, _ = MODELS[k]
        n, act = gb(tb, bw), gb(ab, bw)
        if a.sparsity == "contextual":
            act *= CONTEXTUAL_SPARSITY
        fits = n <= sys_gb
        s = act / sys_gbs
        print("     %-10s %9.1f GB %9.1f GB %11.3f s %9.2f %s"
              % (k, n, act, s, 1.0 / s if s else 0.0, "" if fits else "<- DOES NOT FIT"))
    if a.sparsity == "contextual":
        print("     contextual sparsity applied: %.0f%% of feed-forward neurons per token."
              % (100.0 * CONTEXTUAL_SPARSITY))
        print("     A GPU cannot use this. It fetches 128-byte lines and runs whole warps, so")
        print("     skipping four neurons in five costs it nothing. One processor per channel")
        print("     skips at row granularity, which is where that 5x actually lands.")
    print()

    # ---- 4. the capacity ceiling, which is the real comparison --------------------------------
    print("  4. CAN IT HOLD THE MODEL AT ALL -- the only comparison that matters")
    print("     %-18s %10s %9s %12s %s" % ("machine", "max GB", "GB/s", "watts", "holds a 4T?"))
    need4 = gb(*MODELS["4t"][:1], bits=MODELS["4t"][2]) if False else gb(4000.0, 4.5)
    for k, (g, bwps, w, n, note) in sorted(EXISTING.items(), key=lambda kv: -kv[1][0] * kv[1][3]):
        tot, totbw, totw = g * n, bwps * n, w * n
        print("     %-18s %9.1f %9.0f %11.0f  %s"
              % (k + (" x%d" % n if n > 1 else ""), tot, totbw, totw,
                 "yes" if tot >= need4 else "NO"))
    print("     %-18s %9.1f %9.0f %11s  %s"
          % ("this, %d tiles" % a.tiles, sys_gb, sys_gbs, "see 6",
             "yes" if sys_gb >= need4 else "NO"))
    print()
    print("     A 4T model needs %.0f GB resident. Everything above the line either cannot hold"
          % need4)
    print("     it or needs a node costing seven figures. That is the gap, and it is a capacity")
    print("     gap rather than a speed gap.")
    print()

    # ---- 5. observability, which is the part with no equivalent ------------------------------
    wat = chans * a.tiles
    rec_gbs = wat * WATCHER["mhz"] * 1e6 * WATCHER["bytes_per_cycle"] / 1024.0 ** 3
    area = wat * WATCHER["mm2_at_28nm"]
    print("  5. OBSERVATION THAT COSTS NOTHING, which has no equivalent anywhere")
    print("     %d watcher cores at %.0f MHz               %8.1f GB/s of telemetry"
          % (wat, WATCHER["mhz"], rec_gbs))
    print("     silicon they occupy, at 28nm               %8.2f mm2 total" % area)
    print()
    print("     On a GPU, watching the model competes with running it: every core inspecting an")
    print("     activation is a core not doing work, so interpretability runs 2 to 10 times slower")
    print("     and is therefore done on small models and small samples.")
    print()
    print("     Here it is free, because the silicon doing it was never going to carry weights.")
    print("     This project already measured what that is worth on a smaller scale: 33 boards")
    print("     that hold no model rendered one frame as 33 bands, stitched byte-identically.")
    print()
    print("     What becomes possible that is not possible now:")
    print("       * every expert choice, every attention map, every activation, for every token")
    print("         of a billion-token corpus, on a 4T model, at full speed")
    print("       * the expert placement policy LEARNED ONLINE. Expert use was measured to")
    print("         concentrate 3 to 5 times against uniform and a cache served 42% of fetches;")
    print("         deciding what to migrate where needs a spare processor, and a GPU has none")
    print("       * dead neuron and dead expert detection continuously rather than sampled")
    print("       * a per-token audit trail, which no accelerator can produce at any price")
    print()

    # ---- 6. power -----------------------------------------------------------------------------
    tb, ab, bw, _ = MODELS[a.model]
    act_bits = active * 1024.0 ** 3 * 8.0
    print("  6. POWER, and why in-package is the whole argument")
    print("     %-28s %12s %14s" % ("memory path", "J per token", "W at 10 tok/s"))
    for k in ("ddr_offpackage", "wirebond_inpkg", "hbm"):
        j = act_bits * PJ_BIT[k] * 1e-12
        print("     %-28s %11.2f J %13.0f W" % (k, j, j * 10.0))
    ratio = PJ_BIT["ddr_offpackage"] / PJ_BIT["wirebond_inpkg"]
    print("     in package is %.1fx lower energy than the same DRAM across a PCB, at commodity"
          % ratio)
    print("     DRAM cost rather than HBM cost. That is the gap nothing on the market fills:")
    print("     Groq has the bandwidth and 230 MB of it, Cerebras 44 GB, and the PIM parts put")
    print("     weak processors inside the EXPENSIVE memory.")
    print()

    # ---- 7. context ---------------------------------------------------------------------------
    #  MEASURED: int8 KV costs nothing in perplexity and 4-bit destroys the model, so int8 it is.
    kv_per_tok_mb = 48 * 512 * 1.0 / 1048576.0 * 1024      # layers x kv_dim, int8, MB per 1k tokens
    print("  7. CONTEXT, which stops being a compute problem and becomes a purchase")
    print("     int8 KV cache, MEASURED to cost nothing in perplexity")
    for ctx in (128, 1024, 8192):
        per_seq = kv_per_tok_mb * ctx / 1024.0
        print("       %7dk tokens   %8.1f GB a sequence   %6.0f concurrent sequences here"
              % (ctx, per_seq, (sys_gb - need) / per_seq if sys_gb > need and per_seq else 0))
    print("     On an H100 a million-token context is a third of the card and competes with the")
    print("     weights for the same HBM. Here you add DRAM.")
    print()
    # ---- 8. density -----------------------------------------------------------------------------
    pitch_mm = (DIE["thinned_um"] + DIE["attach_um"]) / 1000.0
    die_cm3 = DIE["area_mm2"] * pitch_mm / 1000.0
    gb_cm3 = DIE["gb"] / die_cm3

    print("  8. DENSITY, from buying die instead of modules")
    print("     a 16 Gbit DDR4 die is %.0f mm2 thinned to %.0f um, stacked on %.0f um pitch"
          % (DIE["area_mm2"], DIE["thinned_um"], pitch_mm * 1000.0))
    print("     %-22s %10s %12s %14s" % ("", "GB", "cm3", "GB per cm3"))
    print("     %-22s %10.1f %12.5f %14.1f" % ("bare stacked die", DIE["gb"], die_cm3, gb_cm3))
    for k, (g, v) in sorted(PACKAGED.items()):
        print("     %-22s %10.1f %12.2f %14.2f   <- %.0fx less dense"
              % (k, g, v, g / v, gb_cm3 / (g / v)))
    print()
    print("     Nothing here is a new process. NAND has shipped in 16 and 32 die stacks for years")
    print("     and most phone LPDDR is wire-bonded package on package. What is unusual is doing")
    print("     it for MODEL memory rather than for storage, with the unpack logic butted against")
    print("     it: die edge to die edge is 100 to 200 um, so a bond wire is under a millimetre")
    print("     instead of the 10 to 30 mm of PCB trace a DIMM needs. That is the whole %.0fx"
          % (PJ_BIT["ddr_offpackage"] / PJ_BIT["wirebond_inpkg"]))
    print("     energy difference, and it is a distance, not an invention.")
    print()

    # ---- 9. the real wall -----------------------------------------------------------------------
    dies_per_tb = 1024.0 / DIE["gb"]
    w_per_tb = dies_per_tb * DIE["refresh_mw"] / 1000.0
    w_per_tb_hot = w_per_tb * DIE["refresh_hot_x"]

    print("  9. THE BINDING CONSTRAINT IS REFRESH, AND IT IS NOT GEOMETRY")
    print("     DRAM pays to hold a bit whether anyone reads it or not. Self-refresh is %.0f mW a"
          % DIE["refresh_mw"])
    print("     die, PUBLISHED, so every terabyte powered costs %.1f W doing nothing at all,"
          % w_per_tb)
    print("     and %.1f W above 85 C where the refresh interval halves." % w_per_tb_hot)
    print()
    print("     Reading is the cheap part. At %.0f pJ a bit in package, %.1f GB a token at"
          % (PJ_BIT["wirebond_inpkg"], active))
    print("     10 tokens a second is %.0f W. Refresh dwarfs it."
          % (active * 1024.0 ** 3 * 8.0 * PJ_BIT["wirebond_inpkg"] * 1e-12 * 10.0))
    print()
    print("     So the question inverts. Not how much silicon fits, but how much can be POWERED.")
    print()
    ENCLOSURES = (("a shoebox", 0.30, 0.20, 0.12), ("a bookshelf unit", 0.8, 0.4, 0.4),
                  ("a shed", 2.0, 3.0, 2.4))
    print("     %-18s %10s %12s %12s %16s" % ("enclosure", "passive W", "TB powered",
                                              "litres of die", "parameters"))
    for name, ww, dd, hh in ENCLOSURES:
        skin = (2 * (ww * hh) + 2 * (dd * hh) + ww * dd) * 7.0 * 20.0
        g, T0, rho, cp, Cd = 9.81, 293.0, 1.2, 1005.0, 0.6
        vent = min(0.25, ww * dd * 0.1)
        qair = Cd * vent * (2.0 * g * hh * 20.0 / T0) ** 0.5
        stack = rho * cp * qair * 20.0
        cap_w = skin + stack
        tb = cap_w / w_per_tb
        litres = tb * 1024.0 / gb_cm3 / 1000.0
        params_t = tb * 1024.0 * 1024.0 ** 3 * 8.0 / 4.5 / 1e12
        print("     %-18s %9.0f W %11.1f TB %11.2f L %13.0f T" %
              (name, cap_w, tb, litres, params_t))
    print()
    print("     Read the last two columns together. The silicon that holds a model of that size")
    print("     is litres, not rooms. The enclosure is large because it is a RADIATOR, not")
    print("     because the memory is big. That is the thing nobody has built: the machine's")
    print("     size is set entirely by heat, and the heat is refresh rather than work.")
    print()
    print("     Which points straight at the one lever that matters. A mixture-of-experts leaves")
    print("     88 to 95%% of itself untouched per token, MEASURED on a real 30B, and expert use")
    print("     concentrates 3 to 5 times against uniform. Cold weights do not need DRAM at all:")
    print("     NAND costs nothing to hold. Every terabyte moved from DRAM to flash buys back")
    print("     %.1f W, and the measured cache curve says how much can move." % w_per_tb)
    print()

    print("  " + "-" * 72)
    print("  WHAT CHANGES ABOUT BUILDING AI, in one line each")
    print("    models stop being sized to the accelerator and start being sized to the problem")
    print("    interpretability stops being a sampling exercise and becomes a complete record")
    print("    context length becomes a purchasing decision rather than an architecture one")
    print("    routing and placement become learnable at runtime, because something is watching")
    print("    and none of it is faster than a GPU at anything a GPU can already do")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

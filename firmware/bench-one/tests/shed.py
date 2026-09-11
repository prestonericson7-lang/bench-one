#!/usr/bin/env python3
"""
shed.py -- does $300k of parts in a shed, passively cooled, hold a 2 to 4 trillion parameter model

WHAT THIS IS FOR
----------------
The unit on the bookshelf is the proof. The shed is the thesis: scale the FPGA-and-DIMM core, scale
the Teensys and Luckfoxes alongside it, spend about three hundred thousand on parts, and hold a model
no data centre holds for under a million.

That is four separate claims and each one has a number attached:

  1. CAPACITY      can that money buy 1.1 to 2.3 TB of resident quantized weights
  2. BANDWIDTH     can it read the ACTIVE slice of those weights fast enough to be usable
  3. THERMAL       does the heat leave a shed without a single fan on a single board
  4. COST          what does the same resident capacity cost in HBM, and at what wall power

This computes all four and says which one binds. Nothing here is measured on this hardware, because
none of it is bought; every figure is labelled published, market or estimate, and the estimates that
matter most are named at the end as the things to measure first.

WHAT IT DELIBERATELY DOES NOT DO
--------------------------------
Compare against a desktop. A PC is not a node in this machine and cannot hold the model at all, which
is the entire point. The comparison is against the only thing that can: a rack of HBM.

RUN
    python shed.py [--budget 300000] [--model 2t-moe|4t-moe] [--shed 2x3x2.4]
"""

import argparse
import sys

# ------------------------------------------------------------------------------------------------
#  PARTS. price_usd is market or published; watts is published TDP or a measured draw where noted.
#
#  kind: (usd, watts, gb_fast_memory, gb_per_s, note)
# ------------------------------------------------------------------------------------------------
PARTS = {
    # The core. A DIMM-capable FPGA carrier is the only part that turns cheap commodity DRAM into
    # addressable model memory with a processor next to it. Four DDR4 slots, four hard controllers.
    #
    # There is no $200 board that does this. An Alveo U200 class card with 64 GB of DDR4 is the
    # nearest off-the-shelf proxy and goes for $2-4k used; a custom carrier around a Zynq
    # UltraScale+ is the real answer and is the single biggest engineering item in the whole plan.
    "fpga_carrier": (2500.0, 25.0, 0.0, 0.0,
                     "ESTIMATE: 4-slot DDR4 carrier, UltraScale+ class, 4 hard memory controllers"),
    # Commodity DRAM. This is the part everyone assumes is expensive and it is not.
    "ddr4_64gb":    (90.0,  4.5, 64.0, 12.8,
                     "MARKET: 64 GB DDR4-3200 ECC RDIMM, used server pull; 12.8 GB/s a channel"),
    # The muscle layer, at the prices already paid for the bookshelf unit.
    "teensy":       (32.0,  0.5, 0.016, 0.0214,
                     "PAID: Teensy 4.1; memory and 21.4 MB/s unpack MEASURED on the board"),
    "luckfox":      (9.0,   0.7, 0.013, 0.061,
                     "PAID: Luckfox Pico Mini B; 13 MB usable and 61 MB/s unpack MEASURED"),
    "lyra":         (80.0,  2.5, 0.400, 0.199,
                     "PAID $79.99: Lyra Ultra W PoE; unpack SCALED 3.26x from the Pico, unmeasured"),
    # Everything that is not compute: PoE switches, cabling, PSUs, racking, the shed itself.
    "infrastructure": (0.0, 0.0, 0.0, 0.0, "priced as a fraction of the total, see INFRA_FRAC"),
}
INFRA_FRAC = 0.15          # ESTIMATE: power supplies, switches, cabling, racking, the shed

# ------------------------------------------------------------------------------------------------
#  MODELS AT THE SHED'S SCALE.
#
#  A dense model at this size is not a candidate and the arithmetic below says why: decode reads every
#  weight every token, so a 2 TB dense model reads 2 TB a token and no amount of memory helps.
#
#  Sparsity is the whole reason the shed is possible. DeepSeek-V3 is the published reference point:
#  671B total, 37B active, which is 5.5%. These two rows hold that ratio at 2T and 4T.
#
#  name: (total_params_b, active_params_b, bits_per_weight, note)
# ------------------------------------------------------------------------------------------------
MODELS = {
    "2t-moe":  (2000.0, 110.0, 4.5, "5.5% active, the published DeepSeek-V3 ratio, at 2T"),
    "4t-moe":  (4000.0, 220.0, 4.5, "5.5% active at 4T"),
    "2t-dense": (2000.0, 2000.0, 4.5, "included only to show why dense is not a candidate"),
    # A real one, for calibration against something that exists.
    "deepseek-671b": (671.0, 37.0, 4.5, "PUBLISHED shape, and a real GGUF exists at this size"),
}

# ------------------------------------------------------------------------------------------------
#  HBM, the only thing that currently holds a model this size.
#
#  published: memory per card, card price, card TDP, node multiplier for everything that is not the
#  accelerator itself (CPU, board, NIC, PSU, chassis).
# ------------------------------------------------------------------------------------------------
HBM = {
    "h100-80":  (80.0,  30000.0, 700.0, 1.35, "PUBLISHED: H100 SXM 80 GB, 3.35 TB/s"),
    "a100-80":  (80.0,  15000.0, 400.0, 1.35, "PUBLISHED: A100 SXM 80 GB, 2.0 TB/s"),
    "mi300x":   (192.0, 20000.0, 750.0, 1.35, "PUBLISHED: MI300X 192 GB, 5.3 TB/s"),
}


def gb_for(total_b, bits):
    """Resident gigabytes for a parameter count at a given bits-per-weight."""
    return total_b * 1e9 * bits / 8.0 / 1024.0 ** 3


def passive_watts(w, d, h, dT=20.0, vent_m2=0.25):
    """How much heat leaves a vented shed with no fan, by stack effect plus skin.

    Two independent paths, both conservative:

      SKIN      natural convection plus radiation off the outside, h about 7 W/m^2K combined.
      STACK     air drawn in low and out high by its own buoyancy. The driving head is the shed
                height and the temperature difference, and the flow is what the vents allow:
                    Q = Cd * A * sqrt(2 * g * h * dT / T)
                then the heat it carries is rho * cp * Q * dT.

    The stack term dominates and it is the reason a shed is a plausible enclosure at all. What this
    does NOT answer is the chip-to-air path: a DIMM with no airflow over it will cook whatever the
    shed is doing, so heat spreaders and vertical card orientation are a real design item rather
    than an afterthought.
    """
    skin_area = 2.0 * (w * h) + 2.0 * (d * h) + w * d
    skin = skin_area * 7.0 * dT

    g, T0, rho, cp, Cd = 9.81, 293.0, 1.2, 1005.0, 0.6
    q = Cd * vent_m2 * (2.0 * g * h * dT / T0) ** 0.5
    stack = rho * cp * q * dT
    return skin, stack, skin_area, q


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget", type=float, default=300000.0)
    ap.add_argument("--model", default="2t-moe", choices=sorted(MODELS))
    ap.add_argument("--shed", default="2x3x2.4", help="width x depth x height in metres")
    ap.add_argument("--dT", type=float, default=20.0, help="internal temperature rise allowed")
    ap.add_argument("--muscle", type=float, default=0.10,
                    help="fraction of the compute budget spent on Teensys and Luckfoxes")
    a = ap.parse_args()

    w, d, h = (float(x) for x in a.shed.lower().split("x"))
    total_b, active_b, bits, mnote = MODELS[a.model]
    need_gb = gb_for(total_b, bits)
    active_gb = gb_for(active_b, bits)

    print()
    print("=" * 74)
    print("  a shed, passively cooled, against a rack of HBM")
    print("=" * 74)
    print("  model   %s: %.0fB total, %.0fB active, %.1f bits a weight" % (a.model, total_b, active_b, bits))
    print("          %s" % mnote)
    print("  budget  $%s in parts" % format(int(a.budget), ","))
    print("  shed    %.1f x %.1f x %.1f m, %.0f K internal rise allowed" % (w, d, h, a.dT))
    print()
    print("  RESIDENT WEIGHTS NEEDED          %9.1f GB" % need_gb)
    print("  READ PER TOKEN                   %9.1f GB   (%.1f%% of the model)"
          % (active_gb, 100.0 * active_b / total_b))
    print()

    # -------------------------------------------------------------------------------------------
    #  1. CAPACITY. Buy memory first, then exactly the carriers needed to address it.
    # -------------------------------------------------------------------------------------------
    dimm_usd, dimm_w, dimm_gb, dimm_bw, _ = PARTS["ddr4_64gb"]
    car_usd, car_w, _, _, _ = PARTS["fpga_carrier"]
    SLOTS = 4

    n_dimm = int(-(-need_gb // dimm_gb))
    n_car = int(-(-n_dimm // SLOTS))
    core_usd = n_dimm * dimm_usd + n_car * car_usd
    core_w = n_dimm * dimm_w + n_car * car_w
    core_bw = n_dimm * dimm_bw                      # one channel per DIMM, all reading at once

    print("  1. CAPACITY")
    print("     %4d x 64 GB DDR4          $%9s   %6.0f W   %7.1f GB"
          % (n_dimm, format(int(n_dimm * dimm_usd), ","), n_dimm * dimm_w, n_dimm * dimm_gb))
    print("     %4d x FPGA carrier        $%9s   %6.0f W   %d DIMM slots each"
          % (n_car, format(int(n_car * car_usd), ","), n_car * car_w, SLOTS))
    print("     core subtotal              $%9s   %6.0f W" % (format(int(core_usd), ","), core_w))
    print("     DRAM is %.0f%% of the core cost. The controllers are the expensive half, which is"
          % (100.0 * n_dimm * dimm_usd / core_usd))
    print("     why a DIMM-capable carrier is the most valuable unbought item in the plan.")
    print()

    # -------------------------------------------------------------------------------------------
    #  2. THE MUSCLE LAYER, scaled alongside.
    # -------------------------------------------------------------------------------------------
    budget_compute = a.budget * (1.0 - INFRA_FRAC)
    muscle_usd = budget_compute * a.muscle
    # Split the muscle money the way the bookshelf unit is split: mostly Luckfoxes by count,
    # Teensys for the hard-real-time and bit-exact work, Lyras for the capacity tier.
    share = {"luckfox": 0.40, "teensy": 0.35, "lyra": 0.25}
    muscle = {}
    for k, frac in share.items():
        usd, wt, gb, bw, _ = PARTS[k]
        n = int(muscle_usd * frac // usd)
        muscle[k] = (n, n * usd, n * wt, n * gb, n * bw)

    m_usd = sum(v[1] for v in muscle.values())
    m_w = sum(v[2] for v in muscle.values())
    m_gb = sum(v[3] for v in muscle.values())
    m_bw = sum(v[4] for v in muscle.values())

    print("  2. THE MUSCLE LAYER, %.0f%% of the compute budget" % (100.0 * a.muscle))
    print("     %-12s %6s %11s %8s %10s %12s" % ("kind", "count", "cost", "watts", "GB", "GB/s"))
    for k in ("luckfox", "teensy", "lyra"):
        n, u, wt, gb, bw = muscle[k]
        print("     %-12s %6d $%10s %7.0f W %9.1f %11.1f"
              % (k, n, format(int(u), ","), wt, gb, bw))
    print("     %-12s %6s $%10s %7.0f W %9.1f %11.1f"
          % ("subtotal", "", format(int(m_usd), ","), m_w, m_gb, m_bw))
    print()
    print("     These hold %.1f GB between them, which is %.2f%% of the model. They are not"
          % (m_gb, 100.0 * m_gb / need_gb))
    print("     memory. What they are is %d independent processors that cost nothing to idle,"
          % sum(muscle[k][0] for k in muscle))
    print("     and the graphics result already measured says what that is worth: 33 boards")
    print("     rendered one frame as 33 bands that stitched byte-identically.")
    print()

    total_usd = (core_usd + m_usd) / (1.0 - INFRA_FRAC)
    total_w = core_w + m_w
    infra_usd = total_usd - core_usd - m_usd

    # -------------------------------------------------------------------------------------------
    #  3. BANDWIDTH. The claim stands or falls here, not on capacity.
    # -------------------------------------------------------------------------------------------
    #  A DDR4 channel's peak is not what a node gets. The measured duty cycle for fabric unpacking
    #  4-bit weights against a throttled feed was 71%, and that is the only measured efficiency this
    #  project owns, so it is the one used.
    DUTY = 0.71
    bw_real = core_bw * DUTY
    sec = active_gb / bw_real if bw_real else 0.0

    print("  3. BANDWIDTH, which is what decides usability")
    print("     %4d channels x 12.8 GB/s      %8.1f GB/s peak" % (n_dimm, core_bw))
    print("     x 0.71, the MEASURED fabric duty  %8.1f GB/s real" % bw_real)
    print("     %.1f GB a token / %.1f GB/s      %8.2f s per token  ->  %.2f tok/s"
          % (active_gb, bw_real, sec, 1.0 / sec if sec else 0.0))
    if a.model.endswith("dense"):
        print()
        print("     THIS IS WHY DENSE IS NOT A CANDIDATE. A dense model reads every weight every")
        print("     token, so the figure above is the whole model divided by the whole bandwidth")
        print("     and no amount of memory changes it. Sparsity is not an optimisation here, it")
        print("     is the precondition.")
    print()

    # -------------------------------------------------------------------------------------------
    #  4. THERMAL. No fan on any board.
    # -------------------------------------------------------------------------------------------
    skin, stack, area, q = passive_watts(w, d, h, a.dT)
    print("  4. PASSIVE COOLING, no fan anywhere")
    print("     shed skin %.0f m2 at 7 W/m2K      %8.0f W" % (area, skin))
    print("     stack effect, %.2f m3/s of air   %8.0f W" % (q, stack))
    print("     total passive capacity            %8.0f W" % (skin + stack))
    print("     the machine draws                 %8.0f W   (%.0f%% of it)"
          % (total_w, 100.0 * total_w / (skin + stack)))
    if total_w < (skin + stack) * 0.5:
        print("     PASSES with margin. The shed is not the constraint; the chip-to-air path is.")
        print("     A DIMM with no air moving over it cooks regardless of what the shed can shed,")
        print("     so heat spreaders and vertical card orientation are real design items.")
    elif total_w < skin + stack:
        print("     PASSES, but with little margin. Larger vents or a bigger rise, or fewer parts.")
    else:
        print("     FAILS. This needs fans, a bigger shed, or less silicon.")
    print()

    # -------------------------------------------------------------------------------------------
    #  5. THE COMPARISON. Against the only hardware that can hold the model.
    # -------------------------------------------------------------------------------------------
    print("  5. THE SAME RESIDENT CAPACITY IN HBM")
    print("     %-10s %6s %12s %9s %14s" % ("card", "cards", "cost", "watts", "note"))
    for k, (gb, usd, wt, node, note) in sorted(HBM.items()):
        n = int(-(-need_gb // gb))
        print("     %-10s %6d $%11s %8.0f W  %s"
              % (k, n, format(int(n * usd * node), ","), n * wt * node, note))
    print()

    cheapest = min(int(-(-need_gb // gb)) * usd * node for gb, usd, wt, node, _ in HBM.values())
    least_w = min(int(-(-need_gb // gb)) * wt * node for gb, usd, wt, node, _ in HBM.values())

    print("  " + "-" * 70)
    print("  THE SHED")
    print("    resident model memory            %9.1f GB" % (n_dimm * dimm_gb))
    print("    parts, including %.0f%% infrastructure  $%8s" % (100.0 * INFRA_FRAC,
                                                                format(int(total_usd), ",")))
    print("    wall power                       %9.0f W" % total_w)
    print("    decode                           %9.2f tok/s" % (1.0 / sec if sec else 0.0))
    print("    GB of model per $1000            %9.2f" % (n_dimm * dimm_gb / (total_usd / 1000.0)))
    print("    GB of model per watt             %9.2f" % (n_dimm * dimm_gb / total_w))
    print()
    print("  AGAINST THE CHEAPEST HBM THAT HOLDS IT")
    print("    cost                              %8.1fx less" % (cheapest / total_usd))
    print("    power                             %8.1fx less" % (least_w / total_w))
    print()
    print("    And it loses on throughput, badly, and that has to be said plainly: a rack of")
    print("    HBM serves hundreds of tokens a second to many users at once. The shed serves")
    print("    about one. It is not a rival for SERVING a model. It is a rival for HAVING one.")
    print()

    if total_usd > a.budget:
        print("  OVER BUDGET by $%s. Fewer DIMMs, or a cheaper carrier."
              % format(int(total_usd - a.budget), ","))
    else:
        print("  UNDER BUDGET by $%s, which at $%.0f a DIMM buys another %d GB."
              % (format(int(a.budget - total_usd), ","), dimm_usd,
                 int((a.budget - total_usd) * (1.0 - INFRA_FRAC) / dimm_usd) * dimm_gb))
    print()
    print("  THE FOUR NUMBERS THAT WOULD MOVE ALL OF THIS, NONE OF THEM MEASURED")
    print("    1. a DIMM-capable carrier's real cost. $2500 is a guess and it is 60% of the core.")
    print("    2. the real duty cycle of fabric unpacking against four live DDR4 channels. The")
    print("       0.71 used here came from synthesis against a throttled single feed.")
    print("    3. what a 2T-class mixture-of-experts actually activates. 5.5% is DeepSeek's")
    print("       published ratio and the expert CACHE hit rate measured on a 30B was 42% at the")
    print("       memory available, which is the number that decides how much of the active slice")
    print("       has to cross a bus at all.")
    print("    4. DIMM power under this access pattern. 4.5 W each is a datasheet figure and 64 of")
    print("       them is the single largest load in the shed.")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

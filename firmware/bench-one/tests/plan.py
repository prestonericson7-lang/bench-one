#!/usr/bin/env python3
"""
plan.py -- given the hardware you own and a model, what runs and how fast

Every number marked "measured" was measured on the bench. The point of this file is to answer "will
this work" arithmetically, before anything is soldered or bought, because the cheapest mistake is the
one caught in a spreadsheet.

TWO CEILINGS, NOT ONE. THIS IS THE CHANGE THAT MATTERS.
-------------------------------------------------------
Earlier versions of this file modelled memory bandwidth and carried a note admitting that compute was
not modelled. Running a real model settled it, and the note was hiding the larger of the two limits.

Measured on this host with tests/decode_limit.c, 8 threads:

    memory read, sum only                    29.40 GB/s
    unpack by writing floats to memory        5.20 GB/s   <- the first implementation
    unpack FUSED, integer dot, no round trip  10.55 GB/s  <- tests/fast_path.c, proven exact
    real decode, fused path                   7.60 GB/s   (4.25 tok/s on 1834 MB a token)

THE FIRST VERSION OF THIS NOTE SAID SIX TIMES AND WAS WRONG. It measured an implementation that wrote
256 floats to memory per 144-byte block so a dot product could read them back once. That is a bad
loop, not a property of 4-bit weights, and charging it to the format flattered the FPGA with slowness
that belonged to the software. The fused kernel took most of the gap back.

What survives is 2.8x, and it is real: a desktop with eight threads of AVX2 still cannot unpack as
fast as it can read. Decode on a general-purpose processor is bound by turning nibbles into numbers.
Every processor in this machine is weaker at that than the one those numbers came from.

So each node now carries TWO rates and the slower one wins:

    bw       bytes per second it can read from its memory
    unpack   bytes per second it can turn into arithmetic

An FPGA is the one device where unpack is free, because splitting a byte into nibbles is wiring
rather than instructions. That is why the zynq appears twice below, as zynq_ps and zynq_pl, and the
difference between those two rows is the entire argument for having FPGAs in the design. That
difference is now about 15x. It was quoted as 31x before the fused kernel existed.

THREE MEMORY TIERS
------------------
    RAM       2 GB of Zynq DDR3 at 2500 MB/s, plus PSRAM on the microcontrollers at 33-58 MB/s
    SD card   4 GB on every Teensy and Luckfox, 128 GB on a Zynq, around 20 MB/s
    SSD       2 TB on the other Zynq over USB 2.0, around 35 MB/s

2.2 TB of storage means capacity constrains nothing. Bandwidth constrains everything.

THE ALLOCATION IS WHOLE LAYERS, EXCEPT THE OUTPUT HEAD
-----------------------------------------------------
Pipeline parallelism cuts between layers, never inside one, so a node holds a whole layer or none of
it. The output head is different: it is a matrix-vector product over the vocabulary, and vocabulary
splits anywhere. Each node computes logits for its own slice and sends back its local best, which is
a few bytes instead of 600 KB. That one asymmetry is what makes a 3B model fit two boards.

RUN
    python plan.py --model qwen2.5-coder-3b
    python plan.py --model qwen2.5-coder-3b --ctx 32768 --requests 4   (long chats, several at once)
    python plan.py --model qwen2.5-coder-3b --kv f32                   (what the runtime does today)
    python plan.py --model qwen2.5-coder-3b --ps-only          (no FPGA fabric: what it costs)
    python plan.py --model llama-70b --scale target            (the fleet in the brief)
    python plan.py --model llama-8b --add zynq_pl=6
"""

import sys

# name: (count, MB each, MB/s read, MB/s unpack, source)
#
# UNPACK is the rate at which the node can turn packed 4-bit and 6-bit weights into arithmetic. The
# host figure is measured; the rest are scaled from it by clock and issue width and are marked
# ESTIMATE. tests/decode_limit_arm is cross-compiled and waiting for a Luckfox to be plugged in,
# which converts the most important of these estimates into a measurement.
# The unpack figures are scaled from this host's MEASURED fused rate of 1.32 GB/s per thread, by clock
# and issue width. They rose 1.93x when the fused kernel replaced the float round trip, and that
# scaling is conservative for the small cores: the fused inner loop is integer multiply-accumulate,
# which a Cortex-M7 or A7 does relatively better at than the float work it replaced.
# name: (count, MB each, MB/s read, MB/s unpack, overlaps?, source)
#
# OVERLAPS says whether reading and unpacking happen at the same time. Fabric: yes. A processor: no,
# because one core does both in sequence. See eff() for the three measurements that settle it.
NODES = {
    # Teensy unpack is MEASURED on the board with the project's own kernel: 43.1 MB/s Q4_K and 33.0
    # Q6_K from internal RAM, so 39.3 for the real 69/31 mix. The estimate here was 58.
    "teensy":  (9,   16, 33.9,   39.3, 0,
                "BOTH MEASURED: 33.9 MB/s PSRAM read, 39.3 MB/s unpack. Together 21.4, measured."),
    # Scaled from the Teensy's measured figure by clock and issue width, not from the desktop any more.
    "esp32s3": (15,   8, 57.6,   11.0, 0,
                "bw measured (octal PSRAM, 240 MHz); unpack ESTIMATE scaled from the Teensy"),
    # Five left, after the pile of Picos was traded for two Lyras.
    "luckfox": (5,   13, 993.0,  61.0, 0,
                "BOTH MEASURED on hardware: 0.97 GB/s read, 61 MB/s unpack for the real 69/31 mix"),
    # Luckfox Lyra Ultra W: RK3506B, 3x Cortex-A7 at 1.2 GHz, 512 MB DDR3, 8 GB eMMC, WiFi 6.
    #
    # THIS BOARD CROSSES THE LINE THAT MATTERS. A Pico has 13 MB usable and cannot hold a single
    # 49.2 MB transformer layer, so however many of them there are they contribute nothing to a dense
    # model at all. 400 MB usable holds eight. That is not a better version of the same thing, it is a
    # different category of node.
    #
    # Unpack is SCALED from the Pico's measured 61 MB/s by three cores and a slightly higher clock,
    # so 3.26x. Bandwidth is an estimate for external DDR3 on this SoC. Both want measuring on the
    # board: the scaled guesses in this table have been wrong by 2x before.
    "lyra":    (2,  400, 1200.0, 199.0, 0,
                "ESTIMATE: unpack scaled 3.26x from the measured Pico; DDR3 bandwidth unmeasured"),
    # The same two PZ7020-StarLite boards twice over, because who does the arithmetic decides
    # everything. PS is the pair of Cortex-A9 cores. PL is the fabric running gemv_int4.
    "zynq_ps": (2, 1024, 2500.0, 154.0, 0,
                "bw ESTIMATE (32-bit PS DDR3 via AXI HP); unpack ESTIMATE, 2x Cortex-A9 667 MHz"),
    # 64 lanes x 4 bits x 100 MHz is 3200 MB/s on paper. The figure used is the MEASURED duty cycle
    # from synthesis against a throttled 2.5 GB/s feed: 71% at 64 lanes, which is 2290 MB/s of weights
    # actually consumed, with the read and the unpack already overlapped inside it.
    "zynq_pl": (2, 1024, 2500.0, 2290.0, 1,
                "measured 71% duty at 64 lanes against a 2.5 GB/s feed; fabric, so unpack overlaps"),
    # The board that is worth saving for: an FPGA with a real DIMM socket. Capacity stops being the
    # problem entirely and bandwidth becomes the whole story.
    #
    # 15 GB/s is an ESTIMATE for a 64-bit DDR4-2400 controller in fabric, and it needs about 300 lanes
    # to keep up -- 4 bits x 100 MHz is 50 MB/s per lane. Synthesis says 256 lanes is 18.9% of an
    # XC7Z020, so 300 is around 22%. The fabric is not the obstacle. Add with --add fpga_dimm=N.
    "fpga_dimm": (0, 262144, 15000.0, 15000.0, 1,
                  "ESTIMATE: 256 GB DDR4 on a DIMM-capable FPGA board, ~300 lanes to match it"),
}

# Storage already owned and already fitted. Capacity is enormous, bandwidth is not, and the gap
# between the two is why this is a separate table instead of extra megabytes in NODES.
#
# A planner that folded an SD card into its host capacity would cheerfully put a transformer layer
# there and report a throughput number 75x too high.
#
# kind: (count, MB each, MB/s read, source)
STORAGE = {
    "teensy_sd":  (9,      4096, 20.0, "ESTIMATE: SDIO 4-bit on the Teensy 4.1 socket. MEASURABLE NOW"),
    "luckfox_sd": (10,     4096, 17.7, "MEASURED on the board: 17.7 MB/s cold read, 10.4 MB/s write"),
    "zynq_ssd":   (1,   2097152, 35.0, "ESTIMATE: 2 TB SSD on the StarLite USB 2.0 host port"),
    "zynq_sd":    (1,    131072, 23.0, "ESTIMATE: 128 GB card in the StarLite SD slot"),
}

# Ten extra ESP-PSRAM64H per node, 8 MB each, behind a 74HC138 so one answers at a time.
# Bandwidth is shared, not multiplied: ten chips give ten times the capacity and exactly the same
# bytes per second, because only one is selected at a time. See docs/22-psram-bank-wiring.md.
PSRAM_ADDON = {
    # WHAT IS ACTUALLY ON THE BENCH, 2026-09-10: fifty 64 Mbit ESP-PSRAM64H chips, 8 MB each.
    #
    # Five per Teensy uses forty-five of them and leaves five spare. That takes a Teensy from 16 MB
    # to 56 MB, and 56 MB is the first time one of them can hold a whole 49.2 MB transformer layer.
    # Before this they held nothing and appeared in the plan only as "carrying no weights".
    #
    # The bandwidth does NOT improve and that is the entire catch. The chips sit behind a 74HC138 so
    # exactly one answers at a time: ten chips are ten times the capacity and the same bytes per
    # second. See docs/22-psram-bank-wiring.md.
    # MERGES with the host, BUT ONLY AS A SLIDING WINDOW, and the difference decides the plan.
    #
    # FlexSPI2 has two chip selects and both of the Teensy's own PSRAM pads use them. A bank shares
    # SS1 through a 74HC138, so the chip already on SS1 becomes bank 0 and the added chips are banks
    # 1 upward. The processor still only addresses 16 MB at any instant whatever the bank holds:
    # 8 on SS0 plus whichever bank member is selected.
    #
    # Four added chips is 48 MB and five is 56. 48 is BELOW the 50.1 MB a layer of this model
    # needs, so only the five-chip build can hold one -- and the placer refuses either way,
    # because a Teensy reads a layer 126x slower than the fabric does.
    #
    # So a layer larger than 16 MB can live here only if it is read SEQUENTIALLY, with a cache flush
    # at every 8 MB boundary. That is fine for decode, which walks every row once and never goes
    # back, and it is useless for anything needing random access to the whole layer. A 50.1 MB layer
    # costs 6 window switches per token.
    #
    # The flush cost is the unmeasured number and psram_bank_teensy measures it. Until it is
    # measured this entry is optimistic by however much that costs.
    "teensy4_qspi":  (32, 32.8, "ON HAND: 4 of the 50 chips per Teensy, 36 used, 14 spare", True),
    "teensy5_qspi":  (40, 32.8, "5 per Teensy instead, 45 used, the last config that holds a layer",
                      True),
    "teensy_qspi":   (80, 32.8, "decoder on FlexSPI2 SS1, measured chip speed", True),
    "teensy7_qspi":  (48, 32.8, "6 chips on a stack behind a decoder plus 1 onboard = 56 MB", True),
    # Does NOT merge: 3 MB/s against the host's 33.9 is a different tier entirely, and folding it
    # in would let the planner allocate it at host speed.
    "teensy_spi":    (80, 3.00, "ESTIMATE: 1-bit SPI, and tCEM caps a burst at 30 bytes", False),
    "luckfox_stick": (80, 3.10, "ESTIMATE: 1-bit SPI at 25 MHz; the header has no quad lines", False),
    # NOT POSSIBLE on the StarLite boards: no FMC, headers cannot make 1.5 V for SSTL15, and six
    # grounds per 32 signals. Kept only to price what a board that COULD take one would be worth.
    "fpga_dimm":     (32768, 2500.0, "NOT POSSIBLE on these boards -- needs FMC or a custom carrier",
                      False),
    # What the 40-pin headers CAN drive: quad-mode PSRAM in parallel, all chips reading at once.
    "fpga_psram12":  (96, 480.0, "12 PSRAM in quad mode on the 40-pin headers, read in parallel", False),
}

# REAL MODELS. The first row was read out of the GGUF file on this machine by gguf_inspect.py and
# confirmed by the C loader; the rest are from published shapes in the same format.
#
# head_mb is the output projection and is SPLITTABLE by vocabulary. tied means the file has no
# output.weight, so the embedding table serves as the head -- which means it is read IN FULL every
# token and cannot be parked on an SD card, however much that would have helped.
#
# name: (total GB, active GB/token, layers, dim, MB per layer, MB head, tied, vocab, kv_dim)
#
# kv_dim is n_kv_heads x head_dim, which is what one position of one layer costs in the KV cache. It is
# NOT dim: every model here uses grouped-query attention, and for the 3B that makes the cache eight
# times smaller than the naive figure. Measured from the file for the first row.
MODELS = {
    "qwen2.5-coder-3b": (2.03, 2.03, 36, 2048, 49.2, 243.4, True, 151936, 256),
    "llama-8b":      (4.3,  4.3,  32, 4096, 128.0, 260.0, False, 128256, 1024),
    # MEASURED from the file on this disk, replacing published-shape guesses: 48 layers of 165.1 MB
    # and a 1026.8 MB head, so 8951.6 MB of weights. Dense, so decode reads all of it every token.
    "qwen-14b":      (8.74, 8.74, 48, 5120, 165.1, 1026.8, False, 151936, 1024),
    "llama-70b":     (37.0, 37.0, 80, 8192, 450.0, 500.0, False, 128256, 1024),
    "mixtral-8x7b":  (25.0,  7.0, 32, 4096, 760.0, 260.0, False, 32000, 1024),
    "llama-405b":    (210.0, 210.0, 126, 16384, 1630.0, 900.0, False, 128256, 1024),
    "deepseek-671b": (350.0, 19.0, 61, 7168, 5600.0, 700.0, False, 129280, 512),
    # A dense 300B at 4 bits, which is what "run a 300B" actually means in memory.
    "dense-300b":    (150.0, 150.0, 96, 12288, 1562.0, 1200.0, False, 128256, 1024),
    # Qwen3 Coder 30B A3B, MEASURED from the file already on this disk. A mixture of experts, so the
    # layer size below is a whole layer WITH ALL 128 OF ITS EXPERTS: 128 x 2.86 + 11.82 = 378.9 MB.
    #
    # That is the decomposition that matters. Scattering experts across nodes means shipping an
    # activation to eight different boards per layer and back, which measured out at 12 MB a token and
    # over a second of pure network. Keeping every expert of a layer on ONE node makes routing a local
    # decision -- the activation arrives, the node picks its own eight, and one activation leaves.
    # Ordinary pipeline parallelism, 8 KB a hop, and the routing problem simply disappears.
    #
    # active_gb is what decode actually reads: 8 of 128 experts plus the shared parts, 2094.6 MB.
    "qwen3-30b-moe": (18.11, 2.05, 48, 2048, 378.9, 430.3, False, 151936, 512),
}

# Bytes per element in the KV cache. The runtime currently uses float, which is four times what the
# cache needs and is the single easiest large saving left in the design.
KV_BYTES = {"f32": 4, "f16": 2, "int8": 1}

# MEASURED on the USB ethernet gadget a Luckfox presents: 18.7 MB/s at one activation, rising to
# 31.9 MB/s on a 196 KB prefill batch. That is USB 2.0 territory and it is already enough, which is
# the finding: the ethernet adapters may not need buying at all.
# wifi is an ESTIMATE for 2.4 GHz WiFi 6 in a real room, and it is the interesting one: the Lyra has
# a radio, so boards can reach each other with no host in the middle and no adapters bought. USB
# gadget mode always needed a host and could never do board-to-board at all.
# The Lyra Ultra W has BOTH a radio and an RJ45, and they are not equivalent for this machine.
#
#   usb   18.7 MB/s, MEASURED between a Pico and this PC. Fastest of the three, and it always needs a
#         host in the middle: USB gadget mode cannot do board-to-board at all.
#   100M  the Lyra's own ethernet port. Slower per link than USB, and SWITCHED -- every pair gets the
#         full link to itself, so it does not care how many boards there are.
#   wifi  2.4 GHz, SHARED. Two boards is fine. Sixteen boards all transmit into the same air and
#         divide one channel between them, so it gets worse as the machine gets bigger, which is the
#         opposite of what this design needs.
LINK_MBS = {"wifi": 3.0, "usb": 18.7, "100M": 11.0, "1G": 125.0, "10G": 1250.0}

# Node counts at each scale. "owned" is what is in the house today. "target" is the count in the
# original brief, and it is a projection of COUNT only -- every per-node rate it uses is the same
# measured number as the owned case, so nothing here assumes a faster part than the bench has seen.
SCALE = {
    "owned":  {"zynq_pl": 2,  "zynq_ps": 2,  "luckfox": 5,   "teensy": 9,  "esp32s3": 15,
               "lyra": 2},
    "target": {"zynq_pl": 16, "zynq_ps": 16, "luckfox": 32,  "teensy": 32, "esp32s3": 32,
               "lyra": 16},
}

# All measured on this host. decode and tok_s are the FUSED path; the float reference managed
# 1.28 tok/s and 2.28 GB/s, which is what the earlier version of this file quoted as the only number.
HOST_MEASURED = {"mem": 29.40, "unpack": 10.55, "decode": 7.60, "tok_s": 4.25, "mb_tok": 1833.9,
                 "ref_tok_s": 1.28}


def eff(node):
    """The rate a node actually sustains, and MIN IS THE WRONG RULE for a processor.

    Taking the slower of reading and unpacking assumes the two happen at the same time. On a processor
    they do not: the same core issues the loads and then does the shifts, so the two costs ADD and what
    survives is the harmonic sum. Three measurements say so:

        Teensy 4.1   read 33.9, unpack 39.3, MEASURED TOGETHER 21.4 MB/s   (min would say 33.9)
        desktop      read 29.4, unpack 10.55, MEASURED TOGETHER 7.60 GB/s  (min would say 10.55)
        harmonic sum predicts 18.2 and 7.76 -- within 15% and 2%

    The desktop figure is the convincing one: 7.76 predicted against 7.60 measured, from a rule with
    nothing fitted to it.

    FABRIC IS DIFFERENT, and this is the whole reason the distinction is worth making. An FPGA unpacks
    inside the streaming datapath, so the nibble splitting happens to bytes already in flight and the
    two genuinely overlap. For those nodes min is right, and the figure used is already a measured duty
    cycle against a throttled feed, so the overlap is in the number.
    """
    bw, unp = node["bw"], node["unpack"]
    if node.get("overlap"):
        return min(bw, unp)
    if bw <= 0 or unp <= 0:
        return 0.0
    return 1.0 / (1.0 / bw + 1.0 / unp)


def build_nodes(extra, addon=None, ps_only=False):
    out = []
    for name, (count, mb, mbs, unp, ov, _) in NODES.items():
        # zynq_ps and zynq_pl are the SAME boards. Counting both would double the hardware.
        if name == "zynq_pl" and ps_only:
            continue
        if name == "zynq_ps" and not ps_only:
            continue
        n = count + extra.get(name, 0)
        for _i in range(n):
            out.append({"kind": name, "cap": float(mb), "bw": mbs, "unpack": unp, "overlap": ov,
                        "load": 0.0, "layers": 0, "head": 0.0})
    # A PSRAM stick is a SEPARATE node, not extra capacity on its host, because it has its own much
    # slower bandwidth. Folding it in would let the planner allocate it at host speed, which is the
    # mistake this whole exercise exists to catch.
    if addon:
        for host, kind in addon.items():
            mb, mbs, _, merges = PSRAM_ADDON[kind]
            base = NODES[host][0] if host in NODES else 0
            n = base + extra.get(host, 0)
            if merges:
                # Same bus, same speed, one address space: add the capacity to the host nodes that
                # are already in the list rather than inventing new ones.
                hits = [o for o in out if o["kind"] == host][:n]
                for o in hits:
                    o["cap"] += float(mb)
                    o["kind"] = host + "+psram"
                continue
            for _i in range(n):
                out.append({"kind": kind, "cap": float(mb), "bw": mbs,
                            "unpack": NODES[host][3] if host in NODES else mbs,
                            "overlap": 0,
                            "load": 0.0, "layers": 0, "head": 0.0})
    return out


def storage_total_mb():
    return sum(c * mb for c, mb, _, _ in STORAGE.values())


def best_storage():
    k = max(STORAGE, key=lambda k: STORAGE[k][1])
    return k, STORAGE[k][1], STORAGE[k][2]


def try_place(nodes, n_layers, mb_layer, mb_head, n_head_ways):
    """Spread the head across the n_head_ways fastest capable nodes, then fill with whole layers.

    Returns layers left unplaced. Mutates the nodes, so the caller resets between attempts.

    THE HEAD IS THE ONLY TENSOR THAT SPLITS FREELY. Logits for token 0..9999 can be computed on one
    node and 10000..19999 on another with no communication at all, because each output depends on the
    whole activation but on only its own slice of the weights. Each node then sends back its local
    best few candidates -- tens of bytes -- instead of 600 KB of logits.

    A layer cannot do that. Splitting a layer across two nodes means exchanging activations inside
    attention, several times per token, and the all-reduce costs more than the weights saved.
    """
    for n in nodes:
        n["load"] = 0.0
        n["layers"] = 0
        n["head"] = 0.0

    capable = [n for n in sorted(nodes, key=lambda n: -eff(n)) if n["cap"] >= mb_layer]
    ways = min(n_head_ways, len(capable))
    if ways == 0:
        return n_layers
    share = mb_head / ways
    for n in capable[:ways]:
        n["head"] = share
        n["load"] += share

    # PACKING IS THE WRONG PLACEMENT, and this used to pack.
    #
    # The old loop filled the fastest node to capacity, then the next. That is correct for fitting and
    # wrong for speed: pipeline throughput is one over the SLOWEST stage, so a node holding ninety
    # layers while three sit empty runs at a ninety-layer pace. It stayed hidden while two 1 GB boards
    # were too small to hold the model anyway and capacity forced a spread; the moment a larger board
    # existed it put a whole 300B model on one of four and reported no gain from the other three.
    #
    # Each layer now goes to whichever node would finish EARLIEST with it, which balances by rate
    # rather than by count. A node twice as fast ends up with twice the layers, and that is the
    # arrangement where every stage takes the same time.
    left = n_layers
    while left > 0:
        best, best_t = None, None
        for n in nodes:
            if n["cap"] - n["load"] < mb_layer:
                continue
            t = (n["load"] + mb_layer) / eff(n)
            if best_t is None or t < best_t:
                best, best_t = n, t
        if best is None:
            break                      # nothing left with room for another whole layer
        best["layers"] += 1
        best["load"] += mb_layer
        left -= 1
    return left


def place(nodes, n_layers, mb_layer, mb_head):
    """Fewest head splits that fit. Fewer splits means fewer nodes doing vocabulary work."""
    best = None
    for ways in range(1, len(nodes) + 1):
        left = try_place(nodes, n_layers, mb_layer, mb_head, ways)
        if left == 0:
            return 0, ways
        if best is None or left < best[0]:
            best = (left, ways)
    left = try_place(nodes, n_layers, mb_layer, mb_head, best[1])
    return left, best[1]


def main():
    model_key = "qwen2.5-coder-3b"
    extra, addon = {}, {}
    link = "1G"
    ps_only = False
    scale = "owned"
    ctx = 2048          # positions of conversation to hold
    kv_fmt = "int8"     # what the cache is stored as
    requests = 1        # independent conversations resident at once
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--model" and i + 1 < len(args):
            model_key = args[i + 1]
        if a == "--link" and i + 1 < len(args):
            link = args[i + 1]
        if a == "--add" and i + 1 < len(args):
            k, _, v = args[i + 1].partition("=")
            extra[k] = int(v)
        if a == "--psram" and i + 1 < len(args):
            for pair in args[i + 1].split(","):
                h, _, k = pair.partition("=")
                addon[h] = k
        if a == "--ps-only":
            ps_only = True
        if a == "--scale" and i + 1 < len(args):
            scale = args[i + 1]
        if a == "--ctx" and i + 1 < len(args):
            ctx = int(args[i + 1])
        if a == "--kv" and i + 1 < len(args):
            kv_fmt = args[i + 1]
        if a == "--requests" and i + 1 < len(args):
            requests = int(args[i + 1])

    if model_key not in MODELS:
        print("models: " + ", ".join(MODELS))
        return 1

    total_gb, active_gb, layers, hidden, mb_layer, mb_head, tied, vocab, kv_dim = MODELS[model_key]

    # --scale replaces the counts in NODES rather than adding to them, so "target" means exactly the
    # brief's fleet and not the brief's fleet plus what is already on the bench.
    if scale in SCALE:
        for k, n in SCALE[scale].items():
            if k in NODES:
                c, mb, bw, unp, ov, src = NODES[k]
                NODES[k] = (n, mb, bw, unp, ov, src)
    nodes = build_nodes(extra, addon, ps_only)

    print()
    print("=" * 74)
    print("  measured on this host first, because it anchors every estimate below")
    print("=" * 74)
    print("  memory read                    %6.2f GB/s" % HOST_MEASURED["mem"])
    print("  unpack, fused integer dot      %6.2f GB/s   (5.20 before the kernel was rewritten)"
          % HOST_MEASURED["unpack"])
    print("  real decode, fused             %6.2f GB/s   (%.2f tok/s on %.0f MB a token)"
          % (HOST_MEASURED["decode"], HOST_MEASURED["tok_s"], HOST_MEASURED["mb_tok"]))
    print("  real decode, float reference     2.28 GB/s   (%.2f tok/s, same model, same machine)"
          % HOST_MEASURED["ref_tok_s"])
    print()
    print("  A desktop has %.1fx more bandwidth than it can unpack 4-bit weights. Decode on"
          % (HOST_MEASURED["mem"] / HOST_MEASURED["unpack"]))
    print("  any general-purpose processor is limited by UNPACKING, not by memory, and every")
    print("  processor here is weaker at it than that one. Hence two rates per node.")

    print()
    print("=" * 74)
    print("  tier 1: RAM -- the only tier a layer can run from")
    print("=" * 74)
    print("  %-11s %5s %8s %10s %10s %11s" %
          ("kind", "count", "MB each", "MB/s read", "MB/s unpk", "MB/s EFF"))
    shown = set(n["kind"] for n in nodes)
    for name, (count, mb, mbs, unp, ov, _) in NODES.items():
        if name not in shown:
            continue
        n = count + extra.get(name, 0)
        e = min(mbs, unp) if ov else 1.0 / (1.0 / mbs + 1.0 / unp)
        print("  %-11s %5d %8.0f %10.1f %10.1f %11.1f%s" %
              (name, n, mb, mbs, unp, e,
               "   <- fabric, overlapped" if ov else "   <- processor, costs add"))
    for host, kind in (addon or {}).items():
        mb, mbs, _, merges = PSRAM_ADDON[kind]
        look = (host + "+psram") if merges else kind
        hits = [n for n in nodes if n["kind"] == look]
        if not hits:
            continue
        h = hits[0]
        print("  %-11s %5d %8.0f %10.1f %10.1f %11.1f   <- %s"
              % (look, len(hits), h["cap"], h["bw"], h["unpack"], eff(h),
                 "storage, 16 MB addressable at a time" if merges
                 else "a separate and slower tier"))
        if merges:
            print("               %d chips behind a decoder on SS1. Sequential reads only, and one"
                  % int(mb / 8.0 + 1))
            print("               cache flush every 8 MB. Decode walks rows once so that is legal;")
            print("               the flush cost is UNMEASURED and psram_bank_teensy measures it.")
    cap_mb = sum(n["cap"] for n in nodes)
    print("  %.2f GB of memory, %.2f GB/s of EFFECTIVE aggregate rate"
          % (cap_mb / 1024.0, sum(eff(n) for n in nodes) / 1024.0))

    st_gb = storage_total_mb() / 1024.0
    sk, scap, sbw = best_storage()
    # ------------------------------------------------------------------------------------------
    #  WHAT THIS DOES THAT A DESKTOP CANNOT
    #
    #  Not a slogan. Every number below is a measured per-node rate multiplied by a count, compared
    #  against a measured figure from the same bench.
    #
    #  The headline is not capacity. It is the fraction of memory bandwidth a node can actually
    #  CONSUME. A desktop reads 29.40 GB/s and can only unpack 10.55 of it, so 64% of its memory
    #  system does nothing for decode. An FPGA unpacks for free, so it consumes 92% of its DDR3.
    #  That ratio is the whole architecture in one number, and it is why adding cheap boards beats
    #  adding expensive bandwidth.
    # ------------------------------------------------------------------------------------------
    agg_bw = sum(n["bw"] for n in nodes)
    agg_eff = sum(eff(n) for n in nodes)
    host_use = HOST_MEASURED["unpack"] / HOST_MEASURED["mem"]
    node_use = agg_eff / agg_bw if agg_bw else 0.0

    print()
    print("=" * 74)
    print("  what this does that a desktop cannot  (scale: %s)" % scale)
    print("=" * 74)
    print("  %-38s %9s %9s" % ("", "this PC", "the stack"))
    print("  %-38s %9.2f %9.2f" % ("memory bandwidth, GB/s",
                                   HOST_MEASURED["mem"], agg_bw / 1024.0))
    print("  %-38s %9.2f %9.2f" % ("bandwidth it can actually unpack, GB/s",
                                   HOST_MEASURED["unpack"], agg_eff / 1024.0))
    print("  %-38s %8.0f%% %8.0f%%" % ("fraction of its memory put to work",
                                       100 * host_use, 100 * node_use))
    print("  %-38s %9d %9d" % ("independent nodes", 1, len(nodes)))
    print()
    if agg_eff / 1024.0 > HOST_MEASURED["unpack"]:
        print("  %.1fx the USABLE bandwidth of this desktop, from measured per-node rates."
              % ((agg_eff / 1024.0) / HOST_MEASURED["unpack"]))
    print("  A desktop cannot buy more. Its memory controller is soldered and its core count is")
    print("  fixed. Every board added here raises capacity and unpacking throughput together.")
    print()
    # MEASURED BETWEEN TWO REAL MACHINES, not on loopback. tests/hop_bench.c, a Luckfox to this PC
    # over USB ethernet: 296 us round trip for 64 B and 875 us for an 8 KB activation, so 148 us and
    # 437 us one way. Loopback said 29 us, which was a tenth of the truth and the kind of number that
    # makes a distributed design look free when it is merely cheap.
    #
    # Cheap is still the right word. 437 us against a stage that takes hundreds of milliseconds is
    # under half a percent, so the conclusion survives the honest number.
    hop_us = 437.0
    n_stage_max = len(nodes)
    print("  AND IT SCALES, which is the part that is easy to doubt and was measured:")
    print("    one hop costs %.0f us one way, MEASURED between a Luckfox and this PC over USB" % hop_us)
    print("    ethernet, carrying %.1f KB of activation. Loopback claimed 29 us; that was a tenth"
          % (hidden * 2 / 1024.0))
    print("    of the truth, and the real number still costs under half a percent of a stage.")
    print("    %d stages therefore cost %.2f ms of the whole token, no matter how big the model"
          % (n_stage_max, n_stage_max * hop_us / 1000.0))
    print("    pipeline parallelism moves ACTIVATIONS, %.1f KB a hop, never weights" % (hidden * 2 / 1024.0))
    print("    tensor parallelism would move gigabytes and is why nobody builds this with it")

    print()
    print("=" * 74)
    print("  tier 2: STORAGE -- %.0f GB, so capacity constrains nothing" % st_gb)
    print("=" * 74)
    for name, (count, mb, mbs, _) in STORAGE.items():
        print("  %-11s %5d x %6.0f GB   %5.1f MB/s" % (name, count, mb / 1024.0, mbs))
    print("  one %.1f MB layer read per token off %s at %.0f MB/s takes %.2f s."
          % (mb_layer, sk, sbw, mb_layer / sbw))
    print("  Storage holds models. RAM runs them.")

    print()
    print("=" * 74)
    print("  %s at INT4: %.1f GB, %d layers of %.1f MB, head %.1f MB"
          % (model_key, total_gb, layers, mb_layer, mb_head))
    print("=" * 74)
    print()
    if tied:
        print("  TIED OUTPUT HEAD. This file has no output.weight -- 434 tensors is exactly")
        print("  36 layers x 12 plus token_embd and output_norm. The embedding table IS the")
        print("  output projection.")
        print("  So the %.1f MB table is READ IN FULL every token, not looked up one row at a" % mb_head)
        print("  time, and it cannot live on an SD card. An earlier version of this planner had")
        print("  it on storage and was wrong by %.0f seconds a token."
              % (mb_head / sbw))
        # ------------------------------------------------------------------------------------------
    #  THE KV CACHE, which the planner used to ignore entirely
    #
    #  Weights are fixed. The cache grows with every token AND with every conversation held at once,
    #  and this machine has to hold several at once or n-1 of its nodes idle. Two multipliers on a
    #  number that was not being counted at all is how a design fits on paper and not on the bench.
    # ------------------------------------------------------------------------------------------
    kvb = KV_BYTES.get(kv_fmt, 1)
    kv_per_pos_layer = 2.0 * kv_dim * kvb                    # key and value
    kv_mb = kv_per_pos_layer * ctx * layers * requests / 1048576.0
    print()
    print("=" * 74)
    print("  the KV cache: %d positions x %d conversations at %s" % (ctx, requests, kv_fmt))
    print("=" * 74)
    print("  %.1f KB per position for the whole model, so %.1f MB in total"
          % (kv_per_pos_layer * layers / 1024.0, kv_mb))
    print("  against %.0f MB of weights, that is %.0f%% as much memory again"
          % (total_gb * 1024.0, 100.0 * kv_mb / (total_gb * 1024.0)))
    for f in ("f32", "f16", "int8"):
        mb = kv_per_pos_layer / kvb * KV_BYTES[f] * ctx * layers * requests / 1048576.0
        print("    as %-4s %8.1f MB%s" % (f, mb, "   <- what the runtime uses today"
                                          if f == "f32" else ""))
    print("  Grouped-query attention is doing the heavy lifting here: this model caches %d values"
          % kv_dim)
    print("  per position per layer, not %d, which is %.0fx less than attention without it."
          % (hidden, hidden / float(kv_dim)))

    # The cache is charged to the nodes before any layer is placed, because a node cannot hold a layer
    # in memory the cache has already taken.
    kv_per_layer_mb = kv_per_pos_layer * ctx * requests / 1048576.0
    mb_layer_eff = mb_layer + kv_per_layer_mb
    if kv_per_layer_mb > 0.05 * mb_layer:
        print()
        print("  a node holding one layer must also hold %.1f MB of that layer's cache," % kv_per_layer_mb)
        print("  so the real cost of a layer is %.1f MB, not %.1f. Placement uses the real one."
              % (mb_layer_eff, mb_layer))

    print()
    print("  a layer is %.1f MB, so a node holds a whole one or none of it" % mb_layer_eff)
    # A PSRAM stick is a node kind too, and its capacity lives in PSRAM_ADDON rather than
    # NODES. Looking only in NODES threw a KeyError the first time a stick was fitted, which
    # is the correct kind of failure: the stick IS a candidate to hold a layer now.
    def cap_of(kind):
        if kind in NODES: return NODES[kind][1]
        if kind in PSRAM_ADDON: return PSRAM_ADDON[kind][0]
        # A merged host carries its own memory plus the sticks fitted to it.
        for o in nodes:
            if o["kind"] == kind: return o["cap"]
        return 0
    too_small = [k for k in shown if cap_of(k) < mb_layer_eff]
    if too_small:
        print("  CANNOT HOLD A LAYER: %s" % ", ".join(sorted(too_small)))
        print("  bandwidth is irrelevant for those -- they carry no weights at all")

    left, ways = place(nodes, layers, mb_layer_eff, mb_head)

    if left > 0:
        print()
        print("  DOES NOT FIT IN RAM. %d of %d layers unplaced, best case %d-way head split."
              % (left, layers, ways))
        free = sum(n["cap"] - n["load"] for n in nodes)
        print("  free RAM: %.0f MB, still needed: %.0f MB" % (free, left * mb_layer_eff))
        if free >= left * mb_layer_eff:
            print("  Fits by total and misses by granularity: the free RAM is in pieces smaller")
            print("  than one %.1f MB layer." % mb_layer_eff)
        big = [(k, NODES[k]) for k in shown if NODES[k][1] >= mb_layer_eff]
        if big:
            k, v = max(big, key=lambda kv: kv[1][1])
            per = int(v[1] // mb_layer_eff)
            print("  each %s holds %d layers; you need %d more of them."
                  % (k, per, -(-left // max(1, per))))
        sec = active_gb * 1024.0 / sbw
        print()
        print("  FROM STORAGE it still runs: %.1f GB a token off %s at %.0f MB/s is %.0f s"
              % (active_gb, sk, sbw, sec))
        print("  per token, %.1f minutes. A batch machine, not a chat machine, and an honest" % (sec / 60.0))
        print("  use of a 2 TB disk.")
        print()
        return 0

    frac = active_gb / total_gb
    used = [n for n in nodes if n["layers"] > 0 or n["head"] > 0]
    # Decode reads every cached position of every layer it holds, on top of the weights. At short
    # context that rounds to nothing; at long context it is most of the traffic, and it is plain
    # bytes rather than packed nibbles so it moves at memory speed rather than unpack speed.
    times = [(n["load"] * frac) / eff(n) + (n["layers"] * kv_per_layer_mb) / n["bw"] for n in used]
    stage_s = max(times) if times else 0.0
    tok_s = 1.0 / stage_s if stage_s > 0 else 0.0

    print()
    print("  FITS. head split %d ways, whole layers everywhere else." % ways)
    print("  %-11s %6s %7s %9s %9s %12s" % ("kind", "nodes", "layers", "head MB", "total MB", "ms/token"))
    seen = {}
    for n in used:
        seen.setdefault(n["kind"], []).append(n)
    for kind, group in seen.items():
        g = group[0]
        print("  %-11s %6d %7d %9.1f %9.1f %12.2f" %
              (kind, len(group), g["layers"], g["head"], g["load"],
               (g["load"] * frac) / eff(g) * 1000.0))
    idle = sorted(k for k in shown if k not in seen)
    if idle:
        print("  carrying no weights: %s" % ", ".join(idle))

    # THE HEADLINE NUMBER NEEDS A CONDITION ATTACHED, and until this was measured it did not have
    # one. 1/(slowest stage) is the SATURATED rate: it requires at least as many independent requests
    # in flight as there are stages. With one request in flight only one stage works at a time and the
    # rate is 1/(sum of stages), which for many stages is far lower.
    #
    # Measured on four processes running the real model: 1.55 tok/s at one sequence in flight against
    # 5.25 at four. Same hardware, same weights, 3.4x apart. Reporting only the saturated figure would
    # overstate a single-user machine by exactly the stage count.
    sum_s = sum(times) if times else 0.0
    tok_s_1 = 1.0 / sum_s if sum_s > 0 else 0.0

    # HOW MUCH CONVERSATION ACTUALLY FITS.
    #
    # This is the constraint the planner was blind to, and it collides head-on with the other one.
    # The machine needs several requests in flight or n-1 nodes idle -- and every extra request
    # multiplies the KV cache. Context and concurrency compete for the same megabytes.
    free_mb = sum(n["cap"] - n["load"] for n in nodes)
    kv_pos_mb = kv_per_pos_layer * layers / 1048576.0
    total_pos = free_mb / kv_pos_mb if kv_pos_mb > 0 else 0
    print()
    print("  CONVERSATION THAT FITS IN WHAT IS LEFT")
    print("  %-34s %10.0f MB" % ("free after weights and cache", free_mb))
    print("  %-34s %10.1f KB" % ("one more position costs", kv_pos_mb * 1024.0))
    print("  %-34s %10.0f positions, ALL conversations together" % ("so there is room for", total_pos))
    for r in (1, 2, 4, 8):
        print("      %d conversation%s of %.0f tokens each" % (r, " " if r == 1 else "s", total_pos / r))
    print("  Context and concurrency spend the same megabytes. The machine wants several requests")
    print("  in flight to keep every node busy, and each one costs a full cache. That tension is")
    print("  real and it is why the cache format matters more than it looks.")

    print()
    print("  slowest stage      : %.2f ms" % (stage_s * 1000.0))
    print("  every stage summed  : %.2f ms" % (sum_s * 1000.0))
    print("  DECODE, %d+ requests in flight : %.2f tokens/sec   <- the saturated rate"
          % (len(used), tok_s))
    print("  DECODE, one request in flight  : %.2f tokens/sec   <- one user, one conversation"
          % tok_s_1)
    print("  %.1fx apart, and the gap IS the stage count. Measured on four nodes: 1.55 against 5.25."
          % (tok_s / tok_s_1 if tok_s_1 > 0 else 0.0))
    print()
    print("  against this PC    : %.2f tokens/sec measured, so %.2fx saturated, %.2fx for one user"
          % (HOST_MEASURED["tok_s"], tok_s / HOST_MEASURED["tok_s"],
             tok_s_1 / HOST_MEASURED["tok_s"]))
    if tok_s < HOST_MEASURED["tok_s"]:
        # Saying this out loud matters. Once the fused kernel existed, the desktop overtook the
        # two-board prediction, and a planner that hid that would be selling the architecture rather
        # than testing it.
        print("  THE DESKTOP IS FASTER at this model. Pretending otherwise would be dishonest.")
        print("  The claim was never speed on a model that already fits one machine. It is watts,")
        print("  and it is what happens when the model does not fit. Two Zynq boards draw about")
        print("  10 W against this desktop's few hundred, which is roughly %.0fx the tokens per"
              % ((tok_s / 10.0) / (HOST_MEASURED["tok_s"] / 250.0)))
        print("  watt, and adding boards adds capacity AND unpacking throughput in one step.")

    act_kb = hidden * 2 / 1024.0
    stages = len(used)
    per_token_mb = act_kb * stages / 1024.0
    link_mbs = LINK_MBS.get(link, 125.0)
    net_s = per_token_mb / link_mbs
    print()
    print("  activation per hop : %.1f KB, %d stages = %.2f MB per token" %
          (act_kb, stages, per_token_mb))
    print("  over %-4s          : %.2f ms, ceiling %.0f tok/s" %
          (link, net_s * 1000.0, 1.0 / net_s if net_s > 0 else 0))
    print("  head reduction     : %d x a few bytes of local best, NOT %.0f KB of logits"
          % (ways, vocab * 4 / 1024.0))
    if ways > 1:
        print("  NOTE: this assumes the output head is SPLIT %d ways by vocabulary. pipe_model does" % ways)
        print("  not do that yet -- it puts the whole projection on the head node, which then has to")
        print("  hold one layer instead of its share. Measured cost of that: the head spent 142.8 ms")
        print("  on the projection against 18.3 ms on its single layer. Splitting it is the next build.")

    if net_s > stage_s:
        print("  BOTTLENECK: the network. Weights are not the problem, wiring is.")
    else:
        limiter = "unpacking" if any(n["unpack"] < n["bw"] for n in used) else "memory bandwidth"
        print("  BOTTLENECK: %s." % limiter)

    print()
    print("  cold start: %.1f GB off %s at %.0f MB/s = %.0f s to fill RAM, once."
          % (total_gb, sk, sbw, total_gb * 1024.0 / sbw))
    print()
    if not ps_only:
        print("  This run used zynq_pl: the FPGA fabric does the unpacking and the arithmetic.")
        print("  Run again with --ps-only to see the same hardware with the ARM cores doing it.")
    else:
        print("  This run used zynq_ps: the ARM cores do the unpacking. Drop --ps-only to see")
        print("  what the fabric is worth.")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

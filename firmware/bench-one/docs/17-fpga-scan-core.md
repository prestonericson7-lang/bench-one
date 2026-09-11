
# 17 — The FPGA scan core

The link between the fabric and the rest of BENCH ONE. This is the first part of the project that
has been **simulated rather than syntax-checked**, and the first with synthesis numbers from a real
tool instead of arithmetic.

Files:

| | |
|---|---|
| `fpga/rtl/hd_popcount.v` | recursive adder tree, W bits to a count in one clock |
| `fpga/rtl/hd_scan.v` | the scan engine: streams vectors, keeps the running best, reports a partial |
| `fpga/tb/tb_hd_scan.v` | six directed cases, self-checking, Icarus Verilog |
| `fpga/tb/hd_scan_model.py` | cycle-accurate model cross-checked against `bench_hdc_shard.c` |
| `fpga/run_checks.sh` | runs both, plus synthesis |

---

## Why the FPGA is a memory tier, not a fast node

Each Zynq board carries **1 GB of DDR3**. At 1 KB per 8192-bit hypervector that is 1,048,576
vectors on one board. A Teensy 4.1 holds a few hundred in usable RAM.

So the FPGA is not a Teensy that goes quicker. It is where the deep memory lives, streamed past a
comparator at whatever rate DDR3 can feed. Compute is not the constraint and is not supposed to be:

| Beat width | Logic cells | Flip-flops | Distributed RAM | Bandwidth consumed at 100 MHz |
|---:|---:|---:|---:|---:|
| 128 bits | 455 | 348 | 0 | 1.6 GB/s |
| 256 bits | 797 | 475 | 43 × RAM32M | 3.2 GB/s |
| 512 bits | 1,464 | 730 | 86 × RAM32M | 6.4 GB/s |

Measured with `yosys synth_xilinx -family xc7`. An XC7Z010 has 17,600 LUTs, so even the widest
version is about 8% of the smaller part, and the DDR3 sits on the PS hard memory controller so it
costs no fabric at all.

**Use W = 256.** A Zynq-7000's 32-bit PS DDR3 delivers roughly 3 GB/s through the AXI HP ports in
practice, so 256 bits per beat already saturates the memory and 512 buys nothing but a harder
timing closure and twice the logic. W = 512 stays in the file for a board with wider or faster
memory. That recommendation comes from the table above, not from taste.

Rough throughput at 3 GB/s and 1 KB per vector: about 3 million vectors compared per second, so a
full sweep of a whole gigabyte takes around a third of a second. The DDR3 figure is an estimate
from the part's width and clock, not something measured on your board. Everything else here was
measured.

---

## The contract, which is the actual point

The coordinator must not be able to tell an FPGA reply from a Teensy reply. The whole system rests
on the merge being a monoid so that replies can arrive late, out of order or twice and still
combine to one answer. That is a property of the **algebra**, not of the C, and it only holds while
every node obeys the same rules. One node that breaks ties differently makes the same query return
different answers depending on which packet the network delivered first, and nothing looks broken.

Four rules, taken from `bench_hdc_shard.c` rather than remembered:

1. **Strictly less-than, lowest index wins a tie.** A scan walks indices upward, so plain `<`
   already gives lowest-index-wins. Using `<=` hands ties to the highest index.
2. **An unfinished vector is neither a result nor coverage.** On halt mid-vector the partial
   distance is discarded and `scanned` does not count it. Both halves matter: a partial sum over
   three of sixteen beats is a small number, and small is what a good match looks like.
3. **Empty is `best_local = 0xFFFF`, `best_dist = 0xFFFFFFFF`.**
4. **The reply is 16 bytes, little-endian**, in `hd_partial_pack`'s field order.

---

## What was actually verified, and how

Both layers were mutation-tested. A test that cannot fail proves nothing, so each rule above was
deliberately broken and the checks had to catch it.

**RTL, in Icarus Verilog 14.** Six directed cases pass: full scan with self-termination, a planted
tie, halt three beats into a vector, halt before anything completes, a stream that overruns the
shard, and the packet layout. Then four mutations of `hd_scan.v`:

| Mutation | Caught by |
|---|---|
| `<` becomes `<=` | tie went to index 7 instead of 3 |
| no self-termination at end of shard | core never raised `done` |
| commit reads `acc` instead of `acc_next` | distance 195 instead of 208 |
| halted partial vector counted | `scanned` inflated by one in all four cases |

The third is worth keeping. The test plants 13 flipped bits in every beat, so dropping the last
beat should cost exactly 13 of 208. It reported 195.

**Design, in Python.** 4,000 randomised trials against an independent implementation of the C's
rules, with halts at random beat positions, gaps in the stream, deliberate distance ties, and
streams that run past the end of the shard.

One mutation there **survived**, and that is recorded rather than hidden: removing the
`vec_done != r_total` term from `s_ready` changed no result. The state machine's own exit already
stops the scan, and the single stray beat that can slip through lands in a partial vector the drain
throws away. That term is defensive, not load bearing. The comment in the RTL now says so; it
previously claimed more.

---

## What is still not verified

Timing closure. Yosys estimates resources, not the critical path on a real part. The 512-bit
popcount tree is nine adder levels between registers, which should close at 100 MHz on a 7-series,
but only Vivado's static timing analysis settles it, and Vivado is not installed here. If it fails,
set `W = 256`, which the table above says you want anyway.

Also absent: the DDR3 and AXI plumbing that feeds this core, and the Ethernet path that carries the
16-byte reply. This is the compute core and its contract, proven in isolation.

---

## Running the checks

```
fpga/run_checks.sh
```

Needs `iverilog`, `vvp` and `yosys`. The OSS CAD Suite provides all three as a portable extract
with no installer and no account. On Windows its binaries resolve their DLLs through `PATH`, so the
suite's `bin` must be on `PATH` rather than merely being the directory the executable sits in —
otherwise `vvp` exits 127 with `libvvp-1.dll: cannot open shared object file`.

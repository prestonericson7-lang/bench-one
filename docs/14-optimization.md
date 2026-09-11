# 14 - Optimisation: bytes, not instructions

No new features in this pass. One function got faster, and it is the function everything else
runs through.

## The bug that was in every comparison the machine ever made

`hd_hamming` is 256 XORs and 256 popcounts. `__builtin_popcount` looks free. It is not:

```
   14:  4058        eors  r0, r3
   16:  f7ff fffe   bl    0 <__popcountsi2>      <-- a FUNCTION CALL, per word
```

The Cortex-M7 has no popcount instruction, so GCC emits a call to a libgcc helper for **every
one of the 256 words**, on every comparison. Disassembled, confirmed.

### Measured, loop body per word, M7 -O2

| kernel | instructions/word | note |
|---|---|---|
| `__builtin_popcount` | 7 | **plus a call, ~15-20 cycles** |
| inline SWAR | 17 | no call |
| SWAR + `USAD8` | 16 | |
| **4 words batched, one `USAD8`** | **14.25** | shipped |

`USAD8` is an ARMv7E-M DSP instruction that sums four byte lanes in one cycle. Four words'
nibble counts can be summed before the horizontal step because each byte holds at most 8, and
4x8=32 is far under the 255 that would carry between lanes. **Measured worst lane over 200,000
random batches: 29.**

Result: **~24 -> ~14 cycles/word, about 1.7x.**

## Then the bigger picture

That was the wrong thing to optimise first. **A search is memory-bound, not compute-bound.**

```
one comparison         reads 2 KB
20,000-concept scan    moves 20 MB
PSRAM delivers         45 MB/s
```

Faster arithmetic does nothing about that. Reading fewer bytes does.

### The first 256 bits decide what 1024 bytes would have

With a query at 25% noise, over the first 256 bits: the true match sits at **46**, an unrelated
vector at **128**. That is **10.3 sigma** apart. 32 bytes is enough to reject a stranger.

So `hd_mem_best` screens on a 32-byte prefix and only reads full vectors for survivors. Two
cheap passes rather than an array of distances - an allocation that scales with memory size is
not free on a 64 MB node.

### The safety margin was chosen by measurement, not taste

| gate | result |
|---|---|
| 4 sigma | 30x saving - **but loses 2% of answers at 45% noise** |
| **6 sigma** | **zero answers lost in 2,400 trials**, 13x saving at 25% noise |

Verified across 500-8,000 concepts, 15-48% noise, with and without correlated near-duplicate
families. 6 sigma ships.

### Traffic saving, and the property that matters most

| noise | saving |
|---|---|
| 10% | **16.0x** |
| 20% | **15.4x** |
| 30% | 4.5x |
| 40% | 1.1x |
| 47% | **1.0x** |

That last row is the point. When the query is too corrupted to screen, everything survives and
the method silently becomes an ordinary full scan. **It never trades correctness for speed.**

## Correctness gate

Both optimisations must return a bit-identical winner to a naive full scan. Verified:

- 200,000 random words: every nibble-count lane within 0..8, zero violations
- 200,000 four-word batches: zero mismatches against true popcount, worst lane 29 of 255
- 2,000 full 256-word comparisons: zero mismatches
- 300 early-exit searches: zero winner mismatches
- **750 full searches** through the shipped two-pass path, across every noise level and both
  correlated and uncorrelated memories: **zero mismatches**

## What this is worth

On a Teensy searching DTCM, the machine is compute-bound and the kernel fix gives ~1.7x.
On anything searching PSRAM, DDR2 or DDR3 - which is every node holding a real memory - traffic
dominates and the prefix screen gives **~15x at realistic noise**.

Nothing was added. One file changed. Every module in the system calls `hd_mem_best`, so all of
them got faster at once.

# 12 — The NPU question, settled

## Can the Luckfox NPU run the search?

**No, not via the matmul API.** Verified by downloading the actual runtime the RV1103 uses and
reading its symbol table:

```
rknpu2/runtime/Linux/librknn_api/armhf-uclibc/   <- the uClibc target = RV1103/RV1106
    librknnmrt.so    223,304 bytes               <- the MICRO runtime, the only one shipped
```

Exported `rknn_*` symbols, complete list:

```
rknn_init            rknn_run             rknn_query           rknn_destroy
rknn_inputs_set      rknn_outputs_get     rknn_outputs_release rknn_dup_context
rknn_create_mem      rknn_create_mem2     rknn_create_mem_from_fd
rknn_create_mem_from_phys                 rknn_destroy_mem     rknn_mem_sync
rknn_set_io_mem      rknn_set_internal_mem  rknn_set_weight_mem
rknn_set_input_shape rknn_set_input_shapes rknn_wait
```

**`matmul` symbol count: 0.** `rknn_matmul_run` and `rknn_matmul_api.h` exist in the full
`librknnrt.so` for RK3566/3568/3588 — they are not in the micro runtime. Anything written
against the matmul API will compile against the header and fail to link on this board.

## The workaround the symbol list exposes

`rknn_set_weight_mem` is there. That is the interesting one, and it changes the answer from
"impossible" to "unverified but concrete":

1. Convert **one** fixed ONNX model that is a single MatMul of shape `(1 x D) x (D x N)`.
2. At runtime, point its weight buffer at the concept memory with `rknn_set_weight_mem`.
3. Feed the query as the input. The N outputs are dot products.

For bipolar vectors, `dot = D - 2 x hamming`, so dot products **are** Hamming distances. The
memory can then change between calls without reconverting the model, which is the thing that
made the naive "bake the memory into a model" idea useless for continual learning.

## What it would buy, and what it would cost

| | A7 with NEON | NPU via weight-mem |
|---|---|---|
| Throughput | ~1 GMAC/s | 0.5 TOPS = ~250 GMAC/s |
| 20,000-concept scan at D=8192 | ~164 ms | **~0.7 ms** |
| Speedup | 1x | **~250x** |

**The cost is memory, and it is severe.** A concept is 1,024 bytes as packed bits. As int8 NPU
weights it is 8,192 bytes — eight times larger. The Luckfox has 64 MB.

```
packed bits   40 MB usable / 1,024 B  =  ~40,000 concepts
int8 weights  40 MB usable / 8,192 B  =   ~5,000 concepts
int4 weights  40 MB usable / 4,096 B  =  ~10,000 concepts   (RV1103 does support int4)
```

So the NPU is **250x faster over 4-8x fewer concepts.** That is a good trade for stage 1 and a
pointless one for stage 2, which only ever scores 32 candidates.

## Status

**Unverified, and it needs the hardware.** Three things have to hold and none can be checked
from a desk:

1. `rknn_set_weight_mem` accepts an arbitrary runtime buffer, not just a model-owned one.
2. A MatMul-only ONNX converts for target `rv1103` in rknn-toolkit2.
3. Quantization can be pinned so ±1 weights survive as ±1.

Until all three are shown on a board, the A7 NEON path is what the design assumes. Nothing in
`bench_hdc*.c` depends on the NPU, so this is an optimisation to bolt on, never a dependency.

## The honest ceiling

The NPU is fixed silicon at 0.5 TOPS. No amount of external PSRAM changes that number — TOPS is
gates, not memory. What raises the machine's total search rate is more nodes, and what raises
its certainty is `--slice`. Both are already built.

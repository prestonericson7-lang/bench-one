---
name: run-bench-one
description: Build, run, test, benchmark and drive BENCH ONE — the distributed local-AI machine in this repo. Use for running a real GGUF model on this host, splitting it across processes, measuring the decode bottleneck, checking perplexity, planning capacity, rendering the machine view, or pushing and running code on an attached Luckfox board.
---

# Running BENCH ONE

BENCH ONE has no window and no server. Its surface is a set of command-line programs that load a
real GGUF model and either generate text, measure a bottleneck, or split themselves across several
processes and do it together. There is also an ARM target that runs on an attached Luckfox board.

**Everything is driven through one script.** Paths below are relative to the repo root (`D:/espicpc`).

```bash
bash .claude/skills/run-bench-one/driver.sh build
```

| command | what it does | takes |
| --- | --- | --- |
| `build` | compiles every host binary, and the ARM ones if the cross compiler is present | ~90 s |
| `smoke` | loads the real model, generates text, fails unless it says Paris | ~15 s |
| `dist` | same model across 4 processes, checked byte-for-byte against 1 process | ~3 min |
| `bench` | the decode bottleneck: memory, unpacking, the fused kernel | ~30 s |
| `quality` | perplexity with float32 and int8 KV cache | ~4 min |
| `plan` | capacity and throughput planner | ~5 s |
| `boards` | lists attached hardware | ~5 s |
| `graphics` | renders the machine view, splits it 33 ways, checks the stitch is exact | ~20 s |
| `push` | copies a file to an attached Luckfox and verifies md5 both ends | ~10 s |
| `all` | build + smoke + dist | ~4 min |

## Prerequisites

Nothing to install. The compiler is vendored at `tools/w64devkit`, the ARM cross compiler at
`tools/arm-linux-gnueabihf`, and `adb` at `tools/platform-tools`. The driver puts them on PATH.

A real model is required. The default is a Qwen2.5-Coder-3B blob in the local Ollama store; override
with `MODEL=/path/to/a.gguf`.

## Checking the machine is sound

Run these three. If all three pass, the maths, the split and the renderer are all correct.

```bash
bash .claude/skills/run-bench-one/driver.sh smoke
```

```bash
bash .claude/skills/run-bench-one/driver.sh dist
```

```bash
bash .claude/skills/run-bench-one/driver.sh graphics
```

`dist` prints the same sentence twice and then `OK: byte for byte identical`. `graphics` prints
`33 bands stitched: IDENTICAL`. Both are exact-match checks — a one-pixel or one-token difference
fails them, which is the point.

## Driving an attached Luckfox

```bash
bash .claude/skills/run-bench-one/driver.sh push firmware/bench-one/tests/machine_view_arm /tmp/mv
```

It prints the local and board md5. They must match. Then run it over adb — note the `</dev/null`,
which is not optional:

```bash
tools/platform-tools/adb.exe shell "/tmp/mv 480 320 120 /tmp/w.ppm" </dev/null
```

Getting the result back off the board:

```bash
tools/platform-tools/adb.exe shell "gzip -c /tmp/w.ppm | base64 -w0" </dev/null | tr -d '\r\n' > w.b64
```

Then decode `w.b64` with `base64.b64decode` and `gzip.decompress` in Python.

## Running one part directly

The binaries take arguments; the driver is only a convenience.

```bash
cd firmware/bench-one/tests && ./machine_view.exe 480 320 120 band7.ppm 7 33
```

Arguments are `width height frames out.ppm [band nbands [y0 y1]]`. A node given `7 33` renders rows
`67..76` and allocates only those rows. Explicit `y0 y1` overrides the equal split, which is how a
balanced split is handed out. `ROWPROFILE=rows.txt` dumps pixels-per-row so the balance can be
computed rather than guessed.

## Gotchas

- **`adb push` on the Luckfox image reports success and writes nothing.** `adb exec-out` returns
  zero bytes. `adb reverse` returns `error: closed`. busybox on the board has no `nc`. The only
  path that works is a python3 listener on the board plus `adb forward`, which is what `push` does.
- **`adb shell` inside a loop eats the loop's stdin.** Every `adb shell` needs `</dev/null` or the
  loop runs once and stops.
- **The board's `tar` is busybox and has no `-z`.** Use `tar cf - x | gzip -c`.
- **`MSYS_NO_PATHCONV=1` is set because adb needs it, and it breaks everything else.** With it set,
  Git Bash stops rewriting `/d/espicpc/...` into `D:/espicpc/...`, and the vendored tools in
  `w64devkit` are BusyBox-w32 Windows programs that cannot open a `/d/` path. The driver converts
  its own paths with `cygpath -m` for this reason. `PATH` is the exception and stays unix-style,
  because it is colon-separated and a `D:/...` entry parses as a directory called `D`.
- **Absolute unix paths reach the compiler verbatim** for the same reason, and it reports the source
  file as missing. All compiles happen from the tests directory with relative source paths.
- **`gguf_bits.c` must be in the source list.** It exists so a microcontroller can link the kernels
  without linking stdio; on the host it is simply a fourth file, and leaving it out gives undefined
  references to `gguf_dequant`.
- **Anything including `stage_link.c` needs `-lws2_32`.** Windows sockets are not in libc.
- **`clock_gettime` is hidden by `-std=c11`.** The ARM builds use `-std=gnu11` or `now_s()` fails to
  compile with an implicit-declaration error.
- **Four processes on eight cores need `OMP_NUM_THREADS=2`**, or each grabs eight threads and they
  fight. The driver sets it.
- **Start the pipeline stages before the head.** The head closes the ring; a stage that starts after
  it is never connected to.

## Troubleshooting

| symptom | cause | fix |
| --- | --- | --- |
| `gcc: command not found` | PATH got a Windows-style entry | PATH must stay unix-style, see Gotchas |
| `cat: can't open '/d/espicpc/...'` | BusyBox-w32 tool given an MSYS path | convert with `cygpath -m` |
| `dist` reports files missing in scratch | a previous run left stray `pipe_model.exe` processes writing the same files | `taskkill //F //IM pipe_model.exe`, then re-run |
| `dist` hangs with one `pipe_model.exe` alive | a stage started after the head | kill it and re-run; the driver already orders them correctly |
| `graphics` says `DIFFERENT` | band rejection is dropping geometry it should keep | `tri()` truncates vertices toward zero, so the band test needs a two-pixel margin |
| board transfer returns 0 bytes | `adb exec-out` on this image | use `base64 -w0` through `adb shell` instead |

## Human path

There isn't a meaningfully different one. Every program is a command-line tool; the driver just
knows the arguments. `docs/` carries the measurements and the reasoning behind each one.

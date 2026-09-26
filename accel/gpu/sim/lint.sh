#!/bin/bash
# sim/lint.sh -- Verilator (4.038) lint of the PL RTL + two consistency checks.
#
#   bash sim/lint.sh            lint the I/O + top modules individually, then the whole design
#                               with top gpu_top (rtl/*.v + rtl/sim_stubs/ for primitives and
#                               the ps7_bd_wrapper stand-in), then:
#                               * rtl/gpu_defs.vh values == common/gpu_proto.h (compiled with gcc)
#                               * rtl/sim_stubs/ps7_bd_wrapper.v port list == the list Vivado
#                                 2026.1 generated (fpga/probe/ps7_bd_wrapper_ports.txt): names,
#                                 directions, widths and order
#   EXTRA_V="a.v b.v" bash sim/lint.sh
#                               add files to the full-design lint (e.g. stand-ins for modules
#                               that do not exist yet)
# Exit status 0 only if every lint run is clean (-Wall, warnings are fatal) and both checks pass.
set -u
cd "$(dirname "$0")/.." || exit 2

VL="verilator --lint-only -Wall -Irtl"
fail=0
ran=0

lint_one() {
    local top=$1; shift
    echo "== lint $top: $*"
    if $VL --top-module "$top" "$@"; then
        echo "   OK"
    else
        echo "   FAIL ($top)"
        fail=1
    fi
    ran=$((ran + 1))
}

# ---- individual modules (engineer D) ----
lint_one sync_fifo   rtl/sync_fifo.v
lint_one sync_fifo   -GW=33 -GAW=9 rtl/sync_fifo.v
lint_one par_rx      rtl/par_rx.v
lint_one axi_gp_regs rtl/axi_gp_regs.v
lint_one clkgen      -y rtl/sim_stubs rtl/clkgen.v

# ---- full design, top gpu_top ----
missing=""
for m in core_top scanout gpu_top; do
    if ! grep -qs "^\s*module\s\+$m\b" rtl/*.v ${EXTRA_V:-}; then
        missing="$missing $m"
    fi
done
if [ -n "$missing" ]; then
    echo "== gpu_top: SKIPPED, module(s) not found:$missing (set EXTRA_V to supply stand-ins)"
    fail=1
else
    # shellcheck disable=SC2086
    # -Og: Verilator 4.038 hits an internal error in V3Gate (rtl/async_fifo.v bin2gray,
    # "Consumer doesn't match lhs of assign") on the full design; gate optimisation is not
    # needed for lint.
    lint_one gpu_top -Og -y rtl/sim_stubs rtl/*.v ${EXTRA_V:-}
fi

# ---- consistency checks ----
CHK=$(mktemp -d /tmp/lint_chk.XXXXXX)
trap 'rm -rf "$CHK"' EXIT

echo "== check rtl/gpu_defs.vh against common/gpu_proto.h"
if python3 - "$CHK" <<'PYEOF'
import re, subprocess, sys
work = sys.argv[1]
defs = {}
for line in open("rtl/gpu_defs.vh"):
    m = re.match(r"\s*`define\s+(\w+)\s+([0-9]+'[hdb][0-9A-Fa-f_]+|[0-9]+)", line)
    if not m:
        continue
    name, v = m.group(1), m.group(2).replace("_", "")
    if "'" in v:
        base = {"h": 16, "d": 10, "b": 2}[v.split("'")[1][0]]
        v = int(v.split("'")[1][1:], base)
    else:
        v = int(v)
    defs[name] = v
# Verilog name -> C expression (from gpu_proto.h)
cmap = {
    "GPU_STRIP_WORDS": "(GPU_ROW_WORDS * GPU_STRIP_H)",
    "GPU_FRAME_WORDS": "(GPU_W * GPU_H / GPU_PPW)",
    "GPU_FB_ZTEST": None, "GPU_FB_ZWRITE": None, "GPU_FB_NOEDGE": None, "GPU_FB_COLORKEY": None,
    "CONTROL_RESET": "GPU_CTL_SRC_PS",
    "FB0_RESET": "GPU_FB0_ADDR", "FB1_RESET": "GPU_FB1_ADDR", "RET_ADDR_RESET": "GPU_RET_ADDR",
    "T_BUSY_FREE": "",                        # PL-internal, no counterpart
}
bits = {"GPU_FB_ZTEST": "GPU_F_ZTEST", "GPU_FB_ZWRITE": "GPU_F_ZWRITE",
        "GPU_FB_NOEDGE": "GPU_F_NOEDGE", "GPU_FB_COLORKEY": "GPU_F_COLORKEY"}
checks = []
for n, v in defs.items():
    if n in bits:
        checks.append((n, "(unsigned long long)(%s)" % bits[n], 1 << v))
    elif n in cmap and cmap[n] == "":
        continue
    elif n in cmap:
        checks.append((n, "(unsigned long long)(%s)" % cmap[n], v))
    elif n.startswith("R_"):
        checks.append((n, "(unsigned long long)(GPU_%s)" % n, v))
    else:
        checks.append((n, "(unsigned long long)(%s)" % n, v))
src = '#include <stdio.h>\n#include "gpu_proto.h"\nint main(void) {\n'
for n, e, v in checks:
    src += '    printf("%%s %%llu\\n", "%s", %s);\n' % (n, e)
src += "    return 0;\n}\n"
open(work + "/chk.c", "w").write(src)
r = subprocess.run(["gcc", "-std=c99", "-Icommon", "-o", work + "/chk", work + "/chk.c"],
                   capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr)
    print("   FAIL: could not compile the gpu_proto.h value dump (a gpu_defs.vh name has no C counterpart?)")
    sys.exit(1)
out = subprocess.run([work + "/chk"], capture_output=True, text=True).stdout.split("\n")
got = dict((l.split()[0], int(l.split()[1])) for l in out if l.strip())
bad = 0
for n, e, v in checks:
    if got.get(n) != v:
        print("   MISMATCH %s: gpu_defs.vh 0x%X, gpu_proto.h 0x%X" % (n, v, got.get(n, -1)))
        bad += 1
print("   %d defines compared, %d mismatches" % (len(checks), bad))
sys.exit(1 if bad else 0)
PYEOF
then echo "   OK"; else echo "   FAIL (gpu_defs.vh)"; fail=1; fi
ran=$((ran + 1))

echo "== check rtl/sim_stubs/ps7_bd_wrapper.v ports against fpga/probe/ps7_bd_wrapper_ports.txt"
if [ ! -f fpga/probe/ps7_bd_wrapper_ports.txt ]; then
    echo "   SKIPPED (no Vivado port list); build.tcl still compares against the real wrapper"
elif python3 - <<'PYEOF'
import re, sys
gen = []
for line in open("fpga/probe/ps7_bd_wrapper_ports.txt"):
    if line.startswith("#") or line.startswith("module") or not line.strip():
        continue
    d, w, _bits, n = line.split()
    gen.append((d, "" if w == "-" else w, n))
txt = open("rtl/sim_stubs/ps7_bd_wrapper.v").read()
txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
txt = re.sub(r"//[^\n]*", " ", txt)
hdr = txt[txt.index("module ps7_bd_wrapper"):]
hdr = hdr[:hdr.index(");")]
stub = [(d, (w or "").replace(" ", ""), n) for d, w, n in re.findall(
    r"\b(input|output|inout)\s+(?:wire\s+)?(\[[^\]]*\])?\s*([A-Za-z_]\w*)", hdr)]
bad = 0
for i in range(max(len(gen), len(stub))):
    g = gen[i] if i < len(gen) else None
    s = stub[i] if i < len(stub) else None
    if g != s:
        bad += 1
        if bad <= 10:
            print("   port %d: Vivado %s  stub %s" % (i, g, s))
print("   Vivado wrapper %d ports, stub %d ports, %d differences (name/dir/width/order)"
      % (len(gen), len(stub), bad))
sys.exit(1 if bad else 0)
PYEOF
then echo "   OK"; ran=$((ran + 1)); else echo "   FAIL (ps7_bd_wrapper stub)"; fail=1; ran=$((ran + 1)); fi

echo
if [ $fail -eq 0 ]; then
    echo "LINT PASS ($ran runs)"
else
    echo "LINT FAIL"
fi
exit $fail

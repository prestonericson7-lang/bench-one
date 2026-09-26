#!/bin/bash
# run_core.sh -- simulate gpu_core (Icarus) on one or more scene directories and check the result.
#
#   run_core.sh [options] <scene_dir> [<scene_dir> ...]
#   run_core.sh --lint                 Verilator lint + Icarus elaboration of the core RTL
# options:
#   --seed N     random seed for the TB stall / latency generators (default 1)
#   --stall P    % of cycles each AXI ready/valid and FIFO push is withheld (default 30)
#   --perf       --stall 0 (best case: HP ports always ready, minimum latency)
#   --teensy     feed rec.hex through the Teensy FIFO instead of the PS FIFO
#                (cfg.txt 'path=teensy' does the same)
#   --alt        run the scene through the OTHER command FIFO: rec.hex scenes use the opposite
#                of their cfg path; single-source stream scenes swap ps.hex/teensy.hex and the
#                CONTROL source bits (tb +swapsrc); 'path=both' scenes are skipped (they
#                already use both FIFOs and their semantics depend on the order)
#   --cap        RET_ENABLE = 1 from reset (tb +retcap): frame 0 must be captured (only frame 0,
#                nobody acknowledges) and its return buffer must equal expected frame 0; for
#                scenes whose streams drive RET_* themselves the scene's ret_caps apply
#   --png        always write PNGs of the frames (default: only on mismatch)
#   --noret      DUT = gpu_core (rtl/INTERFACES.md gpu_core ports, return capture tied off)
#                instead of core_top; RET stream ops then have no effect (ret_caps expects none)
# A scene passes when the TB reports 0 protocol/assertion errors, every frame matches its
# expected_*.hex bit-exactly, and the counters match cfg.txt (nrender, expect_*).
# cfg.txt extras checked when present: expect_fnos=a,b,.. (frame_no per frame),
# expect_last_fno=N, ret_caps=frame:fno,... (exactly these frames captured; the return buffer
# dump of each must equal that frame's expected image), expect_ring_stall=1 (the collector must
# have been held by a full record ring for at least one cycle).
# Work files go to sim/py_scenes/_work/<parent>__<scene>/.
set -u
# SIM_DIR / RTL_DIR / TB_FILE may be overridden (regressions run from a frozen snapshot)
SIM="${SIM_DIR:-$(cd "$(dirname "$0")" && pwd)}"
RTL="${RTL_DIR:-$SIM/../rtl}"
TB="${TB_FILE:-$SIM/tb_core.v}"
CORE_FILES="gpu_core.v core_top.v core_listram.v core_collector.v core_fetch.v core_tri.v core_sprite.v core_writer.v core_frame.v core_ram.v core_ram2.v core_fifo.v"

SEED=1; STALL=30; FORCE_T=0; ALT=0; CAP=0; PNG=0; SCENES=(); DEFS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --seed) SEED="$2"; shift 2 ;;
        --stall) STALL="$2"; shift 2 ;;
        --perf) STALL=0; shift ;;
        --teensy) FORCE_T=1; shift ;;
        --alt) ALT=1; shift ;;
        --cap) CAP=1; shift ;;
        --png) PNG=1; shift ;;
        --noret) DEFS="-DNO_RET_PORTS"; shift ;;   # DUT = gpu_core wrapper
        --lint)
            cd "$RTL" || exit 1
            echo "== verilator 4.038 --lint-only -Wall, top gpu_core (INTERFACES.md ports) =="
            verilator --lint-only -Wall --top-module gpu_core $CORE_FILES
            V=$?
            echo "verilator exit: $V"
            echo "== verilator 4.038 --lint-only -Wall, top core_top (+ SPEC 13.1 ports) =="
            verilator --lint-only -Wall --top-module core_top ${CORE_FILES/gpu_core.v /}
            V2=$?
            echo "verilator exit: $V2"
            V=$((V | V2))
            echo "== iverilog -g2001 elaboration =="
            iverilog -g2001 -Wall -s gpu_core -o /tmp/gpu_core_lint.vvp $CORE_FILES
            I=$?
            echo "iverilog exit: $I"
            [ $V -eq 0 ] && [ $I -eq 0 ]; exit $? ;;
        -*) echo "unknown option $1"; exit 2 ;;
        *) SCENES+=("$1"); shift ;;
    esac
done
[ ${#SCENES[@]} -gt 0 ] || { sed -n '2,26p' "$0"; exit 2; }

WORKROOT="$SIM/py_scenes/_work"
mkdir -p "$WORKROOT"
VVP="$WORKROOT/tb_core_$$.vvp"
trap 'rm -f "$VVP"' EXIT
( cd "$RTL" && iverilog -g2001 $DEFS -o "$VVP" -s tb_core "$TB" $CORE_FILES ) || { echo "COMPILE FAILED"; exit 1; }

TOTAL_FAIL=0
for SC in "${SCENES[@]}"; do
    SC="$(cd "$SC" && pwd)" || { echo "FAIL $SC (no such dir)"; TOTAL_FAIL=1; continue; }
    NAME="$(basename "$(dirname "$SC")")__$(basename "$SC")"
    if [ "$STALL" = "0" ]; then NAME="${NAME}__perf"
    elif [ "$STALL" != "30" ]; then NAME="${NAME}__stall$STALL"; fi
    [ "$SEED" != "1" ] && NAME="${NAME}__seed$SEED"
    [ "$FORCE_T" = "1" ] && NAME="${NAME}__teensy"
    [ "$ALT" = "1" ] && NAME="${NAME}__alt"
    [ "$CAP" = "1" ] && NAME="${NAME}__cap"
    [ -n "$DEFS" ] && NAME="${NAME}__noret"
    WK="$WORKROOT/$NAME"
    rm -rf "$WK"; mkdir -p "$WK/scene"
    # snapshot the whole scene first: a scene regenerated while we run must not mix versions
    cp "$SC"/*.hex "$SC/cfg.txt" "$WK/scene/" 2>/dev/null
    SC_ORIG="$SC"
    SC="$WK/scene"
    for f in rec.hex ddr.hex ps.hex teensy.hex; do
        [ -f "$SC/$f" ] && cp "$SC/$f" "$WK/$f"
    done
    # ---- cfg.txt
    CLEAR=0000; NFRAMES=1; NRENDER=-1; PATHSEL=ps; E_OVF=-1; E_BAD=-1; E_DROP=-1
    E_FNOS=""; E_LAST=-1; RET_CAPS=""; E_RSTALL=0
    while IFS='=' read -r k v; do
        v="${v%$'\r'}"
        case "$k" in
            clear_color) CLEAR="$v" ;;
            nframes) NFRAMES="$v" ;;
            nrender) NRENDER="$v" ;;
            path) PATHSEL="$v" ;;
            expect_overflow) E_OVF="$v" ;;
            expect_bad) E_BAD="$v" ;;
            expect_dropped) E_DROP="$v" ;;
            expect_fnos) E_FNOS="$v" ;;
            expect_last_fno) E_LAST="$v" ;;
            ret_caps) RET_CAPS="$v" ;;
            expect_ring_stall) E_RSTALL="$v" ;;
        esac
    done < "$SC/cfg.txt"
    CAPFNO=""
    if [ "$CAP" = "1" ]; then
        if [ -n "$RET_CAPS" ] || grep -qs '^[78]' "$SC/ps.hex" "$SC/teensy.hex"; then
            echo "SKIP $(basename "$(dirname "$SC_ORIG")")/$(basename "$SC_ORIG") --cap: the scene drives RET_* itself"
            continue
        fi
        CAPFNO=auto        # frame 0 must be captured; its fno is taken from the FRAME 0 line
    fi
    # the gpu_core wrapper ties the return capture off: no captures, whatever the stream asks
    [ -n "$DEFS" ] && RET_CAPS="" && CAPFNO=""
    NWORDS=0
    [ -f "$SC/rec.hex" ] && NWORDS=$(grep -c '[0-9a-fA-F]' "$SC/rec.hex")
    ARGS="+seed=$SEED +stall=$STALL +nframes=$NFRAMES +nwords=$NWORDS +clear=$CLEAR"
    [ "$CAP" = "1" ] && ARGS="$ARGS +retcap"
    if [ "$ALT" = "1" ]; then
        if [ "$PATHSEL" = "both" ]; then
            echo "SKIP $(basename "$(dirname "$SC_ORIG")")/$(basename "$SC_ORIG") --alt: path=both scene"
            continue
        elif [ -f "$SC/rec.hex" ] && [ ! -f "$SC/ps.hex" ] && [ ! -f "$SC/teensy.hex" ]; then
            [ "$PATHSEL" != "teensy" ] && ARGS="$ARGS +teensy"
        else
            ARGS="$ARGS +swapsrc"
        fi
    elif [ "$FORCE_T" = "1" ] || [ "$PATHSEL" = "teensy" ]; then
        ARGS="$ARGS +teensy"
    fi
    ( cd "$WK" && timeout 10800 vvp -n "$VVP" $ARGS > sim.log 2>&1 )
    FAIL=0
    [ "$NFRAMES" = "1" ] && [ -f "$WK/actual_0.hex" ] && cp "$WK/actual_0.hex" "$WK/actual.hex"
    grep -q "TB_RESULT errors=0 frames=$NFRAMES " "$WK/sim.log" || FAIL=1
    grep "TB ERROR" "$WK/sim.log" | head -5
    CYCLES=()
    for ((n = 0; n < NFRAMES; n++)); do
        if [ "$NFRAMES" = "1" ]; then EXP="$SC/expected.hex"; else EXP="$SC/expected_$n.hex"; fi
        if [ ! -f "$WK/actual_$n.hex" ]; then echo "  frame $n: no actual_$n.hex"; FAIL=1; continue; fi
        if [ "$PNG" = "1" ]; then P="$WK/frame_$n"; else P=""; fi
        python3 "$SIM/cmp_fb.py" "$WK/actual_$n.hex" "$EXP" $P > "$WK/cmp_$n.log" 2>&1 || { FAIL=1; cat "$WK/cmp_$n.log" | head -12; }
        RC=$(grep "^FRAME $n " "$WK/sim.log" | sed -n 's/.*render_cycles=\([0-9]*\).*/\1/p')
        CYCLES+=("${RC:-?}")
    done
    LASTF=$(grep "^FRAME $((NFRAMES - 1)) " "$WK/sim.log")
    CNT=$(grep "^COUNTERS" "$WK/sim.log")
    getv() { echo "$1" | sed -n "s/.* $2=\([0-9]*\).*/\1/p"; }
    PC=$(getv "$LASTF" prim_count)
    if [ "$NRENDER" != "-1" ] && [ "$PC" != "$NRENDER" ]; then echo "  prim_count $PC != nrender $NRENDER"; FAIL=1; fi
    for pair in "overflow:$E_OVF" "bad:$E_BAD" "dropped:$E_DROP"; do
        k="${pair%%:*}"; e="${pair##*:}"
        a=$(getv " $CNT" "$k")
        if [ "$e" != "-1" ] && [ "$a" != "$e" ]; then echo "  counter $k=$a expected $e"; FAIL=1; fi
    done
    if [ -n "$E_FNOS" ]; then
        GOT=$(grep "^FRAME " "$WK/sim.log" | sed -n 's/.* fno=\([0-9]*\).*/\1/p' | paste -sd, -)
        if [ "$GOT" != "$E_FNOS" ]; then echo "  frame numbers [$GOT] expected [$E_FNOS]"; FAIL=1; fi
    fi
    RS=$(getv " $CNT" ring_stall)
    if [ "$E_RSTALL" = "1" ] && [ "${RS:-0}" = "0" ]; then echo "  ring_stall=0: the ring back-pressure was not exercised"; FAIL=1; fi
    LF=$(getv " $CNT" last_fno)
    if [ "$E_LAST" != "-1" ] && [ "$LF" != "$E_LAST" ]; then echo "  last_fno=$LF expected $E_LAST"; FAIL=1; fi
    GOTCAP=$(grep "^RETCAP " "$WK/sim.log" | sed -n 's/^RETCAP \([0-9]*\) fno=\([0-9]*\).*/\1:\2/p' | paste -sd, -)
    if [ "$CAPFNO" = "auto" ]; then
        F0=$(grep "^FRAME 0 " "$WK/sim.log" | sed -n 's/.* fno=\([0-9]*\).*/\1/p')
        RET_CAPS="0:${F0:-?}"
    fi
    if [ "$GOTCAP" != "$RET_CAPS" ]; then echo "  return captures [$GOTCAP] expected [$RET_CAPS]"; FAIL=1; fi
    for c in ${RET_CAPS//,/ }; do
        n="${c%%:*}"
        if [ "$NFRAMES" = "1" ]; then EXP="$SC/expected.hex"; else EXP="$SC/expected_$n.hex"; fi
        python3 "$SIM/cmp_fb.py" "$WK/ret_$n.hex" "$EXP" > "$WK/cmpret_$n.log" 2>&1 || { FAIL=1; echo "  return buffer of frame $n:"; head -8 "$WK/cmpret_$n.log"; }
    done
    FPS=""
    for c in "${CYCLES[@]}"; do
        if [ "$c" != "?" ] && [ "$c" != "" ]; then FPS="$FPS $(awk -v c="$c" 'BEGIN{printf "%.1f", 148750000.0/c}')"; fi
    done
    if [ $FAIL = 0 ]; then R=PASS; else R=FAIL; TOTAL_FAIL=1; fi
    [ -n "$GOTCAP" ] && CNT="$CNT ret_caps=$GOTCAP"
    TAGS="${DEFS:+ (noret)}"; [ "$ALT" = "1" ] && TAGS="$TAGS (alt path)"
    [ "$CAP" = "1" ] && TAGS="$TAGS (capture)"
    [ "$FORCE_T" = "1" ] && TAGS="$TAGS (teensy)"
    echo "$R $(basename "$(dirname "$SC_ORIG")")/$(basename "$SC_ORIG")$TAGS seed=$SEED stall=$STALL frames=$NFRAMES render_cycles=[${CYCLES[*]}] fps@148.75MHz=[${FPS# }] ${CNT#COUNTERS }"
done
exit $TOTAL_FAIL

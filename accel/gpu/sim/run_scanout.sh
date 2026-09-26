#!/bin/bash
# run_scanout.sh -- build + run every scanout/TMDS test (engineer C: PL video output).
#
#   sim/run_scanout.sh [all|lint|tmds|small|hd|sims|mutants]  (default: all = lint tmds small hd)
#   sims = small + hd started together (parallel; JOBS=N limits parallel simulations, default 6)
#
# Needs: iverilog/vvp 11, verilator 4.038, python3.  Build products and logs go to
# out/scanout/ inside the project (override with OUT=...).  Exit status 0 only if every
# selected test passed.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/out/scanout}"
mkdir -p "$OUT"
cd "$ROOT"

RTL="rtl/scanout.v rtl/video_reader.v rtl/video_pixel.v rtl/video_serializer.v rtl/async_fifo.v rtl/tmds_encoder.v"
STUBS="rtl/sim_stubs/OSERDESE2.v rtl/sim_stubs/OBUFDS.v"
FAILS=0
SUMMARY=""

note() { SUMMARY="$SUMMARY$1\n"; echo "$1"; }

run_lint() {
    echo "== lint: verilator --lint-only -Wall (scanout + submodules + stubs)"
    if verilator --lint-only -Wall --top-module scanout $RTL $STUBS > "$OUT/lint.log" 2>&1; then
        note "lint    PASS (verilator -Wall: $(grep -c Warning "$OUT/lint.log") warnings)"
    else
        cat "$OUT/lint.log"; note "lint    FAIL"; FAILS=$((FAILS+1))
    fi
    echo "== lint: iverilog -Wall"
    if iverilog -g2001 -Wall -o "$OUT/lint.vvp" -s scanout $RTL $STUBS > "$OUT/lint_iv.log" 2>&1 \
       && [ ! -s "$OUT/lint_iv.log" ]; then
        note "iverilog PASS (no warnings)"
    else
        cat "$OUT/lint_iv.log"; note "iverilog FAIL/WARN"; FAILS=$((FAILS+1))
    fi
}

run_tmds() {
    local N=400000
    echo "== tmds: python reference self-test"
    python3 sim/tmds_ref.py selftest | tee "$OUT/tmds_selftest.log"
    grep -q "errors 0" "$OUT/tmds_selftest.log" || { note "tmds selftest FAIL"; FAILS=$((FAILS+1)); }
    for SEED in 1 2; do
        echo "== tmds: $N vectors, seed $SEED"
        python3 sim/tmds_ref.py gen $N $SEED "$OUT/tmds_vec_$SEED.hex" | tee "$OUT/tmds_gen_$SEED.log"
        iverilog -g2001 -Wall -o "$OUT/tb_tmds.vvp" sim/tb_tmds.v rtl/tmds_encoder.v 2>&1 \
            | grep -v "timescale" | grep -v "inherited timescale" || true
        vvp -n "$OUT/tb_tmds.vvp" +vec="$OUT/tmds_vec_$SEED.hex" +n=$N +dump="$OUT/tmds_dump_$SEED.hex" \
            | tee "$OUT/tb_tmds_$SEED.log"
        python3 sim/tmds_ref.py check "$OUT/tmds_vec_$SEED.hex" "$OUT/tmds_dump_$SEED.hex" \
            | tee "$OUT/tmds_check_$SEED.log"
        if grep -q "tb_tmds: PASS" "$OUT/tb_tmds_$SEED.log" && grep -q "check: PASS" "$OUT/tmds_check_$SEED.log"; then
            note "tmds    seed $SEED PASS ($(grep 'tb_tmds:' "$OUT/tb_tmds_$SEED.log" | head -1))"
        else
            note "tmds    seed $SEED FAIL"; FAILS=$((FAILS+1))
        fi
    done
}

run_tb() {   # name, defines, plusargs  -- writes $OUT/$NAME.res (one summary line)
    local NAME="$1" DEFS="$2" ARGS="$3"
    rm -f "$OUT/$NAME.res"
    if ! iverilog -g2001 $DEFS -o "$OUT/$NAME.vvp" -s tb_scanout sim/tb_scanout.v $RTL $STUBS \
            > "$OUT/$NAME.build.log" 2>&1 || [ -s "$OUT/$NAME.build.log" ]; then
        echo "$NAME BUILD FAIL/WARN (see $OUT/$NAME.build.log)" > "$OUT/$NAME.res"; return
    fi
    local T0=$(date +%s)
    vvp -n "$OUT/$NAME.vvp" $ARGS > "$OUT/$NAME.log" 2>&1
    local T1=$(date +%s)
    if grep -q "tb_scanout: PASS" "$OUT/$NAME.log"; then
        echo "$NAME PASS ($((T1-T0)) s) $(grep 'video frames' "$OUT/$NAME.log")" > "$OUT/$NAME.res"
    else
        echo "$NAME FAIL (see $OUT/$NAME.log)" > "$OUT/$NAME.res"
    fi
}

# Runs the given "name|defines|plusargs" jobs in parallel (JOBS at a time, default 6) and
# collects their result lines.
run_tbs() {
    local J="${JOBS:-6}" n=0 spec NAME
    local names=""
    for spec in "$@"; do
        IFS='|' read -r NAME DEFS ARGS <<< "$spec"
        echo "== $NAME: iverilog $DEFS / vvp $ARGS"
        run_tb "$NAME" "$DEFS" "$ARGS" &
        names="$names $NAME"
        n=$((n + 1))
        if [ $((n % J)) -eq 0 ]; then wait; fi
    done
    wait
    for NAME in $names; do
        grep -v "^\[" "$OUT/$NAME.log" 2>/dev/null | grep -v "^tb_scanout: PASS" | tail -8
        local R; R="$(cat "$OUT/$NAME.res" 2>/dev/null || echo "$NAME FAIL (no result)")"
        note "$R"
        case "$R" in *" PASS "*) ;; *) FAILS=$((FAILS+1)) ;; esac
    done
}

# Core clock (SPEC 2) = 148.75 MHz -> half period 3361 ps (the tb default).  The sweep also runs
# the core exactly phase-locked at 2x the pixel clock (3360 ps, like the real MMCM outputs), faster
# (3000 ps = 166 MHz), and slower (4706 ps = the old 106.25 MHz, 6000 ps = 83 MHz).
run_small() {
    run_tbs "small_s1|| +seed=1" \
            "small_s2|| +seed=2 +core_ps=3000" \
            "small_s3|| +seed=3 +core_ps=6000" \
            "small_s4|| +seed=4 +core_ps=4706" \
            "small_s5|| +seed=5 +core_ps=3360" \
            "med_256x128|-DMED| +seed=5"
}

run_hd() {
    run_tbs "hd_1280x720|-DHD| +seed=7 +verbose"
}

# Mutation check of the testbenches: each mutant is a copy of the RTL with one deliberate bug;
# every mutant must make its testbench FAIL ("killed").  A surviving mutant = a test hole.
run_mutants() {
    local M="$OUT/mut"
    rm -rf "$M"; mkdir -p "$M"
    python3 - "$M" <<'PYEOF2'
import os, shutil, sys
M = sys.argv[1]
files = ["rtl/scanout.v", "rtl/video_reader.v", "rtl/video_pixel.v", "rtl/video_serializer.v",
         "rtl/async_fifo.v", "rtl/tmds_encoder.v", "rtl/sim_stubs/OSERDESE2.v", "rtl/sim_stubs/OBUFDS.v"]
muts = [
 # name, file, old, new, testbench
 ("serial_bit_order", "rtl/video_serializer.v", ".D1        (d[0]),\n        .D2        (d[1]),",
  ".D1        (d[1]),\n        .D2        (d[0]),", "small"),
 ("slave_bits_swapped", "rtl/video_serializer.v", ".D3        (d[8]),\n        .D4        (d[9]),",
  ".D3        (d[9]),\n        .D4        (d[8]),", "small"),
 ("green_expand", "rtl/video_pixel.v", "px[10:5], px[10:9]", "px[10:5], px[6:5]", "small"),
 ("vsync_not_cea_aligned", "rtl/video_pixel.v", "            if (hc == C_HS_START)\n                vs1 <=",
  "            if (hc == 12'd0)\n                vs1 <=", "small"),
 ("hsync_width", "rtl/video_pixel.v", "localparam HS_END   = HS_START + H_SYNC;", "localparam HS_END   = HS_START + H_SYNC - 1;", "small"),
 ("sync_on_wrong_bits", "rtl/scanout.v", ".c0(v_hs), .c1(v_vs)", ".c0(v_vs), .c1(v_hs)", "small"),
 ("clock_lane_pattern", "rtl/scanout.v", ".d(10'b0000011111)", ".d(10'b0000111110)", "small"),
 ("swap_mid_frame", "rtl/video_reader.v", "wire do_swap   = !in_frame && swap_req && swap_armed;",
  "wire do_swap   = swap_req && swap_armed;", "small"),
 ("frame_end_no_drain", "rtl/video_reader.v", "!rd_arvalid && (reserved == R_ZERO);", "!rd_arvalid;", "small"),
 ("no_space_check", "rtl/video_reader.v", "room_r   <= issue ? room2 : room1;", "room_r   <= 1'b1;", "small"),
 ("room_ignores_issue", "rtl/video_reader.v", "room_r   <= issue ? room2 : room1;", "room_r   <= room1;", "small"),
 ("swap_not_armed", "rtl/video_reader.v", "            end else if (!swap_req) begin\n                swap_armed  <= 1'b1;",
  "            end else begin\n                swap_armed  <= 1'b1;", "small"),
 ("axi_err_only_decerr", "rtl/video_reader.v", "if (rd_rresp != 2'b00)", "if (rd_rresp == 2'b11)", "small"),
 ("pop_unexpected_sof", "rtl/video_pixel.v", "            if (wstart0) begin\n                if (f_valid && !f_sof) begin",
  "            if (wstart0) begin\n                if (f_valid) begin", "small"),
 ("underflow_not_black", "rtl/video_pixel.v", "                    synced_nx = 1'b0;               // underflow or unexpected sof",
  "                    synced_nx = f_valid ? 1'b0 : synced;", "small"),
 ("no_seek_discard", "rtl/video_pixel.v", "            f_pop = f_valid && !f_sof;              // seek: discard up to the next sof",
  "            f_pop = 1'b0;", "small"),
 ("square_frozen", "rtl/video_pixel.v", "sq_x     <= (sq_x >= C_SQX_MAX) ? 12'd0 : sq_x + 12'd1;", "sq_x     <= sq_x;", "small"),
 ("pattern_bar_color", "rtl/video_pixel.v", "3'd3: bar_rgb = 24'h00FF00;", "3'd3: bar_rgb = 24'h00FE00;", "small"),
 ("no_pattern_border", "rtl/video_pixel.v", "|| (vc1 == C_V_LAST);", ";", "small"),
 ("scanout_en_ignored", "rtl/video_pixel.v", "pat_mode <= !en_s2;", "pat_mode <= 1'b0;", "small"),
 ("vsync_count_double", "rtl/scanout.v", "if (vt_s2 ^ vt_s3)", "if (vt_s1 ^ vt_s3)", "small"),
 ("fifo_gray_is_binary", "rtl/async_fifo.v", "wgray <= wbin_nx ^ (wbin_nx >> 1);", "wgray <= wbin_nx;", "small"),
 ("rresp_exokay_not_counted", "rtl/video_reader.v", "if (rd_rresp != 2'b00)", "if (rd_rresp[1])", "small"),
 ("vsync_count_offset", "rtl/scanout.v", "vsync_cnt_r <= 32'd0;", "vsync_cnt_r <= 32'd1;", "small"),
 ("one_read_outstanding", "rtl/video_reader.v", "wire          can_issue = in_frame && (bursts_left != 24'd0) && room_r;",
  "wire          can_issue = in_frame && (bursts_left != 24'd0) && room_r && (reserved == R_ZERO);", "small"),
 ("tmds_disparity_branch", "rtl/tmds_encoder.v", "(cnt_pos && n1_gt_n0) || (cnt_neg && n0_gt_n1)", "(cnt_pos && n1_gt_n0)", "tmds"),
 ("tmds_xnor_rule", "rtl/tmds_encoder.v", "((n1d == 4'd4) && !d[0])", "((n1d == 4'd4) && d[0])", "tmds"),
 ("tmds_token_01", "rtl/tmds_encoder.v", "2'b01:   q <= 10'b0010101011;", "2'b01:   q <= 10'b0010101010;", "tmds"),
]
for name, f, old, new, tb in muts:
    d = os.path.join(M, name)
    os.makedirs(os.path.join(d, "rtl", "sim_stubs"))
    for src in files:
        shutil.copy(src, os.path.join(d, src))
    s = open(f).read()
    n = s.count(old)
    if n != 1:
        print("MUTANT SETUP ERROR %s: pattern found %d times" % (name, n)); sys.exit(1)
    open(os.path.join(d, f), "w").write(s.replace(old, new))
    open(os.path.join(d, "tb"), "w").write(tb)
print("created %d mutants" % len(muts))
PYEOF2
    [ $? -eq 0 ] || { note "mutants SETUP FAIL"; FAILS=$((FAILS+1)); return; }
    [ -f "$OUT/tmds_vec_1.hex" ] || python3 sim/tmds_ref.py gen 400000 1 "$OUT/tmds_vec_1.hex" > /dev/null
    export OUT
    ls "$M" | xargs -P "${JOBS:-$(nproc)}" -I{} bash -c '
        d="$OUT/mut/{}"; cd "'"$ROOT"'"
        R="$d/rtl"; F="$R/scanout.v $R/video_reader.v $R/video_pixel.v $R/video_serializer.v $R/async_fifo.v $R/tmds_encoder.v $R/sim_stubs/OSERDESE2.v $R/sim_stubs/OBUFDS.v"
        if [ "$(cat $d/tb)" = "tmds" ]; then
            iverilog -g2001 -o $d/sim.vvp sim/tb_tmds.v $R/tmds_encoder.v > $d/build.log 2>&1
            timeout 600 vvp -n $d/sim.vvp +vec=$OUT/tmds_vec_1.hex +n=400000 +dump=$d/dump.hex > $d/log 2>&1
            grep -q "tb_tmds: PASS" $d/log && echo "SURVIVED {}" || echo "killed   {}"
        else
            iverilog -g2001 -o $d/sim.vvp -s tb_scanout sim/tb_scanout.v $F > $d/build.log 2>&1
            timeout 900 vvp -n $d/sim.vvp +seed=1 > $d/log 2>&1
            grep -q "tb_scanout: PASS" $d/log && echo "SURVIVED {}" || echo "killed   {}"
        fi' | sort | tee "$OUT/mutants.log"
    local NM=$(ls "$M" | wc -l) NK=$(grep -c "^killed" "$OUT/mutants.log")
    if [ "$NK" -eq "$NM" ]; then note "mutants PASS ($NK/$NM killed)"; else note "mutants FAIL ($NK/$NM killed)"; FAILS=$((FAILS+1)); fi
}

WHAT="${1:-all}"
case "$WHAT" in
    lint)  run_lint ;;
    tmds)  run_tmds ;;
    small) run_small ;;
    hd)    run_hd ;;
    mutants) run_mutants ;;
    all)   run_lint; run_tmds; run_small; run_hd ;;
    sims)  JOBS="${JOBS:-7}"
           run_tbs "hd_1280x720|-DHD| +seed=7 +verbose" \
                   "small_s1|| +seed=1" "small_s2|| +seed=2 +core_ps=3000" \
                   "small_s3|| +seed=3 +core_ps=6000" "small_s4|| +seed=4 +core_ps=4706" \
                   "small_s5|| +seed=5 +core_ps=3360" "med_256x128|-DMED| +seed=5" ;;
    *) echo "usage: $0 [all|lint|tmds|small|hd|sims|mutants]"; exit 2 ;;
esac
echo
echo "================ summary ================"
printf "$SUMMARY"
if [ $FAILS -eq 0 ]; then echo "ALL PASS"; exit 0; else echo "$FAILS FAILURE(S)"; exit 1; fi

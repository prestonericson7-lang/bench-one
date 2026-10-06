#!/bin/bash
# qemu_plcheck_test.sh -- the PL guard (zynq-plcheck), A against B, through the PRODUCTION U-Boot at the
# board's clocks (qemu_uboot_test.sh), on today's image:
#   A = card #1's own boot.scr (no clk_ignore_unused) and device tree (fclk-enable = 0), lifted out of the
#       copy taken before that card is rewritten: what froze the board on 2026-10-05;
#   B = today's boot.scr and device tree (clk_ignore_unused, fclk-enable = <0x1>).
# QEMU 6.2 cannot run "fpga loadb" (no PCAP model), so U-Boot never marks the PL loaded and the guard would
# have nothing to protect. Both scripts therefore get, just before bootz, what U-Boot does after a
# SUCCESSFUL load: PS/PL level shifters on (LVL_SHFTR_EN = 0xF), PL resets released (FPGA_RST_CTRL = 0)
# and fpgagpu.pl_loaded=1 on the command line. Everything else is the real chain. QEMU's SLCR keeps the
# gate bit Linux writes at "clk: Disabling unused clocks" (measured: FPGA0_THR_CNT = 1 with card #1's
# device tree), so:  A must say "FCLK0 gated", BLOCK, read nothing and hold fpgagpud/zaccel-server back with
# the board up;  B must find nothing wrong, attempt the PL reads (QEMU has no PL: they return 0) and pass.
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_plcheck_test.sh
set -uo pipefail
L=$(cd "$(dirname "$0")" && pwd)
CARD1=${CARD1:-/mnt/d/start/machine-cards/evidence/fpga1-card.img}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
MKIMAGE=$(command -v mkimage || echo /root/zynq/u-boot/tools/mkimage)
W=/root/zynq/plcheck-test; rm -rf "$W"; mkdir -p "$W"
fail=0; ok() { echo "  PASS  $*"; }; bad() { echo "  FAIL  $*"; fail=1; }

mcopy -n -i "$CARD1@@1M" ::/boot.scr "$W/A.scr.orig" && mcopy -n -i "$CARD1@@1M" ::/zynq-pz7020-starlite.dtb "$W/A.dtb" \
  && mcopy -n -i "$IMG@@1M" ::/boot.scr "$W/B.scr.orig" && mcopy -n -i "$IMG@@1M" ::/zynq-pz7020-starlite.dtb "$W/B.dtb" \
  || { echo "cannot read the boot files"; exit 1; }
for c in A B; do   # the script text is after the 64-byte image header and the 8-byte size table
  dd if="$W/$c.scr.orig" bs=72 skip=1 status=none | tr -d '\000' > "$W/$c.cmd"
  grep -q '^bootz ' "$W/$c.cmd" || { echo "$c: no bootz line in the script"; exit 1; }
  sed -i 's/^bootz /echo "QEMU TEST: as after a successful fpga loadb: level shifters on, PL resets released, marker set"\nmw.l 0xF8000008 0x0000DF0D\nmw.l 0xF8000900 0x0000000F\nmw.l 0xF8000240 0x00000000\nmw.l 0xF8000004 0x0000767B\nsetenv bootargs "${bootargs} fpgagpu.pl_loaded=1"\nbootz /' "$W/$c.cmd"
  "$MKIMAGE" -A arm -T script -C none -n "plcheck test $c" -d "$W/$c.cmd" "$W/$c.scr" >/dev/null || { echo "mkimage failed"; exit 1; }
  cp "$IMG" "$W/$c.img"
  mcopy -o -i "$W/$c.img@@1M" "$W/$c.scr" ::/boot.scr && mcopy -o -i "$W/$c.img@@1M" "$W/$c.dtb" ::/zynq-pz7020-starlite.dtb || { echo "cannot patch $c"; exit 1; }
done
grep -q clk_ignore_unused "$W/A.cmd" && { echo "A's script has clk_ignore_unused: not card #1's configuration"; exit 1; }
grep -q clk_ignore_unused "$W/B.cmd" || { echo "B's script lacks clk_ignore_unused: not today's configuration"; exit 1; }
echo "A: card #1 fclk-enable = $(fdtget -t x "$W/A.dtb" /axi/slcr@f8000000/clkc@100 fclk-enable 2>/dev/null || echo '?')   B: today fclk-enable = $(fdtget -t x "$W/B.dtb" /axi/slcr@f8000000/clkc@100 fclk-enable 2>/dev/null || echo '?')"

# console lines arrive as "[ t] python3[pid]: <line>" (journal+console): match after that prefix
row() { grep -a -E "(^|: )$2 " "$1" | tail -1 | sed -E 's/^.*: ('"$2"' )/\1/'; }
for c in A B; do
  echo "== $c"
  IMG="$W/$c.img" KEEP_LOG="$W/$c.serial" bash "$L/qemu_uboot_test.sh" > "$W/$c.run" 2>&1
  S=$W/$c.serial
  [ -s "$S" ] || { bad "$c: no serial log"; continue; }
  CL=$(grep -a -m1 "Kernel command line:" "$S"); echo "   ${CL:0:200}"
  echo "$CL" | grep -q "fpgagpu.pl_loaded=1" && ok "$c: the PL counts as loaded (marker on the kernel command line)" || bad "$c: no marker on the kernel command line"
  grep -a -E "zynq-plcheck|python3\[[0-9]+\]: \[" "$S" | grep -a -E "fclk0 in|FPGA0_THR_CNT|problem:|reading 0x|bus error|  0x|verdict:" | sed -E 's/^.*python3\[[0-9]+\]: /   /' | cut -c1-200
  R=$(row "$S" accel); P=$(row "$S" plcheck); G=$(row "$S" pl_regs)
  echo "   report: $R"
  [ -n "$R" ] && [ -n "$P" ] && [ -n "$G" ] && grep -aq "ZYNQ-REPORT END" "$S" && ok "$c: the board stayed up and printed the report with the guard's rows" || bad "$c: report or its rows missing"
  if [ $c = A ]; then
    echo "$CL" | grep -q clk_ignore_unused && bad "A: clk_ignore_unused present" || ok "A: card #1's command line (no clk_ignore_unused)"
    grep -aq "fclk0 in the clock framework: enable_count 0" "$S" && ok "A: nothing holds fclk0 (enable count 0)" || bad "A: fclk0 enable count not 0"
    grep -aq "FPGA0_THR_CNT 0x00000001" "$S" && ok "A: Linux set FCLK0's gate bit (FPGA0_THR_CNT = 1)" || bad "A: gate bit not set"
    grep -aq "problem: FCLK0 gated" "$S" && ok "A: the guard names it: FCLK0 gated" || bad "A: guard did not see FCLK0 gated"
    [ "$(grep -ac 'problem:' "$S")" = "$(grep -ac 'problem: FCLK0 gated' "$S")" ] && ok "A: FCLK0 is the only problem (shifters on, resets released)" || bad "A: other problems: $(grep -a 'problem:' "$S" | grep -v FCLK0 | sed -E 's/^.*problem/problem/' | tr '\n' ' ')"
    grep -aq "verdict: BLOCKED" "$S" && ok "A: BLOCKED" || bad "A: not blocked"
    grep -aq "reading 0x4" "$S" && bad "A: the PL was read anyway" || ok "A: no PL read attempted"
    [ -n "$R" ] && ! echo "$R" | grep -q "zaccel-server active" && ! echo "$R" | grep -q "fpgagpud active" && ok "A: zaccel-server and fpgagpud held back" || bad "A: services: $R"
    echo "$P" | grep -q BLOCKED && ok "A: the report carries BLOCKED" || bad "A: plcheck row: $P"
    echo "$G" | grep -q "unsafe" && ok "A: the report skipped its own PL read" || bad "A: pl_regs row: $G"
  else
    echo "$CL" | grep -q clk_ignore_unused && ok "B: today's command line (clk_ignore_unused)" || bad "B: no clk_ignore_unused"
    grep -aq "fclk0 in the clock framework: enable_count 1" "$S" && ok "B: the device tree holds fclk0 (enable count 1)" || bad "B: fclk0 enable count not 1"
    grep -aq "FPGA0_THR_CNT 0x00000000" "$S" && ok "B: FCLK0's gate bit clear" || bad "B: gate bit not clear"
    grep -aq "problem:" "$S" && bad "B: problems: $(grep -a 'problem:' "$S" | sed -E 's/^.*problem/problem/' | tr '\n' ' ')" || ok "B: no problem found"
    grep -aq "reading 0x40000000" "$S" && grep -aq "reading 0x43c00000\|reading 0x43C00000" "$S" && ok "B: both PL ID reads attempted, each logged before the read" || bad "B: PL reads not attempted"
    grep -aq "verdict: OK" "$S" && ok "B: verdict OK" || bad "B: verdict not OK"
    [ -z "$(grep -a 'verdict: BLOCKED' "$S")" ] && ! echo "$R" | grep -q "zynq-plcheck failed" && ok "B: nothing held back" || bad "B: $R"
  fi
done
[ $fail = 0 ] && echo "PL GUARD TEST: PASS" || echo "PL GUARD TEST: FAIL"
exit $fail

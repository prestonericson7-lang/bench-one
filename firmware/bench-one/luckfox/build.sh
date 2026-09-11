#!/usr/bin/env bash
# ===========================================================================================
#  build.sh -- cross-compile and deploy the HDC node to eleven Luckfoxes
# ===========================================================================================
#
#  WHY A SCRIPT AND NOT A README LINE
#  -----------------------------------
#  Eleven boards is where "just scp it over" stops being a plan. Every node needs a DIFFERENT
#  --node id and a DIFFERENT --base, and getting either one wrong is silent:
#
#    * two nodes sharing an id     -> the coordinator's dedup mask counts them as one, so half
#                                     the cluster's coverage vanishes and nothing logs an error
#    * two nodes sharing a base    -> two different concepts claim the same global index, and
#                                     recall returns the wrong one with full confidence
#
#  Neither shows up as a crash. Both show up as "the AI is a bit dumb", weeks later. So the
#  ids are computed here, once, from the position in the list.
#
#  USAGE
#      ./build.sh                          # compile only
#      ./build.sh deploy 10.0.0.20 ...     # compile, then push to each address in order
#      ./build.sh start  10.0.0.20 ...     # start the daemon on each
#      ./build.sh stat   10.0.0.20 ...     # ask each how it is doing
#
#  The coordinator's second stage is opt-in:
#      ./build/hdc_coord --peers ... --self-test --deep
#  It rescores stage 1's shortlist at the full 90,112 bits and costs one extra round trip.
# ===========================================================================================

set -u

SHARED=../shared
OUT=build
SHARD=${SHARD:-20000}          # vectors per node. 20000 x 1 KB = 20 MB, safe inside 64 MB
                               # once CMA is cut to 4M. Raise to 40000 if you rebuilt the
                               # kernel with RK_BOOTARGS_CMA_SIZE=4M (doc 05 "Reclaiming RAM").
PORT=${PORT:-9000}
FIRST_NODE=${FIRST_NODE:-20}   # matches docs/11 addressing: Luckfoxes are 20..30

# --- find the toolchain ------------------------------------------------------------------
CC=""
for c in arm-rockchip830-linux-uclibcgnueabihf-gcc \
         arm-rockchip830-linux-uclibcgnueabihf-gcc.exe; do
    command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
done
if [ -z "$CC" ]; then
    for d in "$HOME/luckfox-pico/tools/linux/toolchain"/*/bin \
             /opt/luckfox/*/bin /c/luckfox*/tools/linux/toolchain/*/bin; do
        [ -x "$d/arm-rockchip830-linux-uclibcgnueabihf-gcc" ] && \
            { CC="$d/arm-rockchip830-linux-uclibcgnueabihf-gcc"; break; }
    done
fi
if [ -z "$CC" ]; then
    echo "Luckfox toolchain not found."
    echo "It ships inside the SDK:  luckfox-pico/tools/linux/toolchain/*/bin/"
    echo "Add that bin/ to PATH, or set CC= and re-run."
    echo
    echo "Do NOT substitute a generic arm-linux-gnueabihf-gcc: the stock Luckfox rootfs is"
    echo "uClibc, and a glibc binary will build fine and then fail to exec on the board."
    exit 1
fi
echo "toolchain : $CC"

mkdir -p "$OUT"

# -O2 not -O3: the hot loop is XOR + popcount over 256 words and is memory-bound, so the extra
# unrolling buys nothing and costs I-cache on a 32 KB L1.
CFLAGS="-O2 -std=c99 -Wall -Wextra -Wformat=2 -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,--gc-sections"
# The three files BOTH programs need: the vector primitives, the sharded scan and merge, and the
# deep second stage.
SRC_COMMON="$SHARED/bench_hdc.c $SHARED/bench_hdc_shard.c $SHARED/bench_hdc_deep.c"

# The node needs three more, and leaving them out is why `./build.sh` used to stop with three
# undefined references -- pb_read, pb_open and dream_init -- before it had built anything at all.
# The node paged vectors out of PSRAM and ran consolidation while idle for as long as this script
# has existed, and the script never listed either file. The coordinator does neither, so it keeps
# the shorter list and --gc-sections has less to throw away.
SRC_NODE="psram_bank.c $SHARED/bench_dream.c $SHARED/bench_index.c"

echo "building  : hdc_node"
$CC $CFLAGS -I. -I"$SHARED" -o "$OUT/hdc_node"  hdc_node.c  $SRC_COMMON $SRC_NODE $LDFLAGS || exit 1
echo "building  : hdc_coord"
$CC $CFLAGS -I. -I"$SHARED" -o "$OUT/hdc_coord" hdc_coord.c $SRC_COMMON $LDFLAGS || exit 1

for b in hdc_node hdc_coord; do
    printf '  %-10s %s bytes\n' "$b" "$(wc -c < "$OUT/$b")"
done

CMD=${1:-}
[ -z "$CMD" ] && { echo; echo "built. add 'deploy <ip> <ip> ...' to push."; exit 0; }
shift || true
[ $# -eq 0 ] && { echo "no addresses given"; exit 1; }

i=0
for ip in "$@"; do
    node=$((FIRST_NODE + i))
    base=$((i * SHARD))
    case "$CMD" in
    deploy)
        echo "--> $ip  node=$node base=$base"
        scp -O "$OUT/hdc_node" "$OUT/hdc_coord" "root@$ip:/root/" || echo "    scp FAILED"
        ssh "root@$ip" "chmod +x /root/hdc_node /root/hdc_coord" || true
        ;;
    start)
        # SLICE=1 -> dimension sharding: node i holds slice i of EVERY concept,
        #            so the concept is 8192*nodes bits wide. Max certainty.
        # unset    -> item sharding: node i holds whole concepts. Max capacity.
        extra=""
        [ "${SLICE:-}" = "1" ] && extra="--slice $i"
        # DREAM=1 -> the node forms concepts from what it has seen whenever it is
        # idle. Costs cap/8 extra vectors of RAM and makes the cluster better at
        # generalising the longer it is left running.
        [ "${DREAM:-}" = "1" ] && extra="$extra --dream"
        echo "--> $ip  starting node=$node base=$base shard=$SHARD $extra"
        # setsid + nohup because the stock image is BusyBox init, not systemd: a process
        # started from ssh dies with the session otherwise (doc 05 has the same trap for
        # S99python, which blocks init entirely if it is not backgrounded properly).
        ssh "root@$ip" "setsid nohup /root/hdc_node --node $node --base $base \
             --shard $SHARD --port $PORT --file /root/hdc_shard.bin $extra \
             > /root/hdc.log 2>&1 < /dev/null &" || echo "    ssh FAILED"
        ;;
    stop)
        echo "--> $ip  stopping"
        ssh "root@$ip" "killall -TERM hdc_node 2>/dev/null; sleep 1" || true
        ;;
    stat)
        printf -- "--> %-14s " "$ip"
        # SIGTERM makes the node save its shard, so 'stop' is how you checkpoint the machine.
        ssh "root@$ip" "tail -1 /root/hdc.log 2>/dev/null || echo '(no log)'" 2>/dev/null \
            || echo "(unreachable)"
        ;;
    *)
        echo "unknown command: $CMD"; exit 1;;
    esac
    i=$((i + 1))
done

if [ "$CMD" = "start" ]; then
    echo
    peers=$(printf '%s,' "$@"); peers=${peers%,}
    echo "cluster up. try:"
    echo "  ssh root@$1 '/root/hdc_coord --peers $peers --teach 2000 --self-test'"
    [ "${DREAM:-}" != "1" ] && echo "  (add DREAM=1 to have them form concepts while idle)"
fi

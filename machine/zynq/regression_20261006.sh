#!/bin/bash
# regression_20261006.sh -- every QEMU and NVMe test, one at a time, on the final image (WSL, root).
# The harness scripts changed on 2026-10-06 (qemu_serial_tcp.sh per-port image, qemu_uboot_test.sh
# SERIAL_TCP mode, qemu_spl_chain_test.sh loads the SPL with its device tree), so all of them run again.
#   bash /mnt/d/espicpc/machine/zynq/regression_20261006.sh
set -u
L=/mnt/d/espicpc/hardware/pz7020-starlite/linux; M=/mnt/d/espicpc/machine/zynq; A=/mnt/d/espicpc/accel
O=$M/regression-20261006; mkdir -p $O
sed -i 's/\r$//' $L/qemu_*.sh $M/qemu_machine_test.sh $A/pi/test_nvme_pi_drive.sh $A/pi/test_nvme_claim.sh $A/test_stage_pi_card.sh
echo "image $(sha256sum < /root/zynq/pz7020-starlite-sd.img | cut -c1-16)  start $(date +%T)"
r() { local t=$1; shift; local s=$SECONDS; "$@" > $O/$t.log 2>&1; local rc=$?
      echo "== $t rc=$rc ($((SECONDS - s)) s): $(grep -a -c '  PASS' $O/$t.log) pass, $(grep -a -c '  FAIL' $O/$t.log) fail"
      grep -a -E "  FAIL|TEST: |STAGE: |QEMU BOOT|SPL CHAIN" $O/$t.log | tail -6; }
r uboot          bash $L/qemu_uboot_test.sh
r boot           bash $L/qemu_test.sh
r spl-chain      bash $L/qemu_spl_chain_test.sh
r accel          bash $L/qemu_accel_test.sh
r agent          bash $L/qemu_agent_test.sh
r machine        bash $M/qemu_machine_test.sh
r plcheck-ab     bash $L/qemu_plcheck_test.sh
r nvme-wsl       bash $A/pi/test_nvme_pi_drive.sh
r nvme-pi-tools  env PI_IMG=/root/opi/opi4pro-accel.img bash $A/pi/test_nvme_pi_drive.sh
r nvme-claim     bash $A/pi/test_nvme_claim.sh
r stage-pi-card  env OPI_IMG=/root/opi/opi4pro-accel.img bash $A/test_stage_pi_card.sh
echo "end $(date +%T)"

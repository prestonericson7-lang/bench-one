#!/bin/bash
# qemu_uboot_test.sh -- run the PRODUCTION U-Boot proper (u-boot-dtb.bin, the same code and device tree as
# u-boot.img) on QEMU xilinx-zynq-a9 at 512 MB with the production SD image: U-Boot finds boot.scr, the
# script loads pl.bit into memory U-Boot has not reserved, runs fpga loadb, loads zImage + DTB and starts
# Linux with the SAME kernel command line the board gets (no cpufreq.off), and Linux runs to the serial
# autologin and the zynq-report.
# The SLCR clock registers are first set to what ps7/ps7_init_gpl.c leaves them at on the board (ARM PLL
# 1533 MHz, CPU 766.67 MHz, 6:2:1) by a stub that then jumps to U-Boot, as the SPL does, so Linux's clock
# tree and cpufreq see the board's clocks, not QEMU's reset values (-device loader data writes cannot do
# it: QEMU resets the SLCR after the loaders run, and Linux then starts at 216 MHz). That is the path that panicked on the first hardware boot (2026-09-24: BUG in
# cpufreq_online, "Attempted to kill init") before the DT carried a 766 MHz operating point.
# Stages this catches: a reserved-memory window at the pl.bit staging address ("Reading file would
# overwrite reserved memory" -> DONE never lights), the kernel unpacking over the DT, a cpufreq/OPP mismatch.
# NOT covered: the BootROM, the SPL and ps7_init itself (hardware only; see BOOT-SD-runbook.md), and the
# second A9 core (QEMU 6.2's xilinx-zynq-a9 takes one CPU; both cores share the one clock and cpufreq policy).
#     wsl -d Ubuntu-22.04 -u root --exec bash /mnt/d/espicpc/hardware/pz7020-starlite/linux/qemu_uboot_test.sh
set -uo pipefail
UB=${UB:-/root/zynq/u-boot/u-boot-dtb.bin}
IMG=${IMG:-/root/zynq/pz7020-starlite-sd.img}
PS7=${PS7:-/mnt/d/espicpc/hardware/pz7020-starlite/ps7/ps7_init_gpl.c}
T=${TIMEOUT:-600}
W=$(mktemp -d /tmp/quboot.XXXX)
QPID=
trap '[ -n "$QPID" ] && kill $QPID 2>/dev/null; rm -rf "$W"' EXIT
cp "$IMG" "$W/sd.img" && truncate -s 2G "$W/sd.img"
echo "u-boot-dtb.bin $(sha256sum < "$UB" | cut -c1-16)  image $(sha256sum < "$IMG" | cut -c1-16)"

# final value of each SLCR clock register after ps7_init's silicon-3.0 pll + clock tables
CLK=$(python3 - "$PS7" <<'EOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
regs = {}
for name in ("pll", "clock"):
    body = re.search(r"unsigned long ps7_%s_init_data_3_0\[\] = \{(.*?)\n\};" % name, src, re.S).group(1)
    for op in re.finditer(r"EMIT_(MASKWRITE|WRITE)\((0X[0-9A-F]+),\s*(0x[0-9A-F]+)U\s*(?:,\s*(0x[0-9A-F]+)U)?\)", body):
        a = int(op.group(2), 16)
        if op.group(1) == "WRITE":
            regs[a] = int(op.group(3), 16)
        else:
            mk, v = int(op.group(3), 16), int(op.group(4), 16)
            regs[a] = (regs.get(a, 0) & ~mk) | (v & mk)
# ARM/DDR/IO PLL, ARM_CLK, DDR_CLK, SDIO, FPGA0, CLK_621_TRUE. Not UART_CLK_CTRL (0xF8000154): the board
# gates UART1's clock (0xA01) and QEMU 6.2's cadence_uart then divides by a 0 Hz clock and dies with
# SIGFPE before the first instruction. QEMU does not time the serial line, so the UART clock changes nothing.
print(" ".join("0x%08X=0x%X" % (a, regs[a]) for a in
               (0xF8000100, 0xF8000104, 0xF8000108, 0xF8000120, 0xF8000124, 0xF8000150,
                0xF8000170, 0xF80001C4)))
EOF
) || { echo "cannot read the clock registers from $PS7"; exit 1; }
echo "board clocks: $CLK"
# stub at 0x03F00000: unlock the SLCR, write each register, lock it again (as ps7_init does), jump to U-Boot
{ echo ".arm"; echo ".global _start"; echo "_start:"
  echo "  ldr r1, =0xDF0D"; echo "  ldr r2, =0xF8000008"; echo "  str r1, [r2]"
  for kv in $CLK; do echo "  ldr r1, =${kv#*=}"; echo "  ldr r2, =${kv%=*}"; echo "  str r1, [r2]"; done
  echo "  ldr r1, =0x767B"; echo "  ldr r2, =0xF8000004"; echo "  str r1, [r2]"
  echo "  ldr pc, =0x04000000"; echo ".ltorg"; } > "$W/stub.S"
arm-linux-gnueabihf-as -o "$W/stub.o" "$W/stub.S" && arm-linux-gnueabihf-ld -Ttext=0x03F00000 -o "$W/stub.elf" "$W/stub.o" \
  && arm-linux-gnueabihf-objcopy -O binary "$W/stub.elf" "$W/stub.bin" || { echo "cannot build the clock stub"; exit 1; }

qemu-system-arm -M xilinx-zynq-a9 -m 512M -nographic -serial mon:stdio \
  -device loader,file="$UB",addr=0x04000000,force-raw=on \
  -device loader,file="$W/stub.bin",addr=0x03F00000,force-raw=on -device loader,addr=0x03F00000,cpu-num=0 \
  -drive file="$W/sd.img",if=sd,format=raw </dev/null > "$W/log" 2>&1 &
QPID=$!
end=$((SECONDS + T))
while kill -0 $QPID 2>/dev/null && [ $SECONDS -lt $end ]; do
  tr -d '\r' < "$W/log" > "$W/l"
  grep -aq -E "ZYNQ-REPORT END|Kernel panic|Internal error|U-BOOT STAGE ABORT" "$W/l" && { sleep 2; break; }
  sleep 3
done
kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; QPID=
tr -d '\r' < "$W/log" > "$W/l"
echo "ran $SECONDS s"
grep -a -E "^U-Boot 20|^DRAM:|Found U-Boot script|Loading PL bitstream|would overwrite reserved|No pl.bit|Starting kernel|Booting Linux|Kernel command line|cpufreq|Internal error|Kernel panic|PC is at|login:|cpu_khz|mem_total|cpus  " "$W/l" | cut -c1-160 | head -40
fail=0
chk() { if grep -aq -E "$2" "$W/l"; then echo "  PASS  $1"; else echo "  FAIL  $1"; fail=1; fi; }
chk "U-Boot proper runs, DRAM from its DT is 512 MiB" "^DRAM:.*512 MiB"
chk "boot.scr found and run" "Found U-Boot script|## Executing script"
chk "pl.bit loaded (not refused as reserved memory)" "Loading PL bitstream pl.bit"
if grep -aq "would overwrite reserved memory" "$W/l"; then echo "  FAIL  U-Boot refused a load into reserved memory"; fail=1; fi
chk "kernel started with the PL marker decision made" "Starting kernel"
chk "Linux booting" "Booting Linux"
chk "command line is the board's (no cpufreq.off)" "Kernel command line: console=ttyPS0,115200"
if grep -aq "cpufreq.off" "$W/l"; then echo "  FAIL  cpufreq.off on the command line: the board's cpufreq path is not being run"; fail=1; fi
if grep -aq -E "Internal error|Kernel panic|BUG:" "$W/l"; then echo "  FAIL  kernel Oops/BUG/panic"; fail=1; fi
chk "Linux sees the board's CPU clock: cpufreq at 766666 kHz" "cpu_khz +766666"
if grep -aq "unlisted initial frequency" "$W/l"; then echo "  FAIL  Linux did not start at the board's 766666 kHz (the clock stub did not take)"; fail=1; fi
chk "serial autologin reached a root shell" "root@|login: root \(automatic login\)"
chk "zynq-report printed to the end" "ZYNQ-REPORT END"
[ $fail = 0 ] && echo "U-BOOT STAGE: PASS" || { echo "U-BOOT STAGE: FAIL"; tail -25 "$W/l" | cut -c1-160; }
exit $fail

# PZ7020-StarLite — boot from SD, hands-off

Put the card in the board, plug the UART into the PC and power the board. The card boots Linux,
logs root in on the serial console by itself and prints a facts report. `linux/watch_boot.ps1` on
the PC records the whole boot and writes the summary. Nobody types anything.

## State — 2026-09-24

| | Fact | How it's known |
|---|---|---|
| Card | 32 GB microSD (29.53 GiB physical) | Windows `Get-Disk` |
| Contents | `linux/out/pz7020-starlite-sd.img` (1666 MiB), sha256 `f061fc58089d1b1a220255031239b461aba78c62d6643c8dd72066a33cb0418c` | hash of the decompressed `.img.xz` = `sd-image.sha256` |
| Written | unbuffered + write-through, 16.0 MB/s (real card speed, not cache) | `linux/write_sd.py` log |
| Verified | the first 1,746,927,616 bytes read back unbuffered (18.2 MB/s) hash **identical** to the image | same log |
| Partition 1 | FAT32 `BOOT`, 128 MiB @ 1 MiB: `boot.bin` `u-boot.img` `boot.scr` `zImage` `zynq-pz7020-starlite.dtb` `pl.bit` | `Get-Partition` after the write + `mdir` at build |
| Partition 2 | ext4 rootfs, 1536 MiB @ 129 MiB — Debian 12 bookworm armhf | `Get-Partition` after the write |
| Rest of card | ~27.9 GiB unallocated (grow the rootfs from the board later if wanted) | — |
| Serial console | root logs in automatically on `ttyPS0` (`linux/serial-autologin.conf`); SSH still needs the password | QEMU: `zynq login: root (automatic login)` |
| Boot report | `zynq-report`: model, CPUs, memory, whether the PL is configured, PL registers, die temperature, eth0, SD size, failed units, kernel warnings. Runs at every boot (`zynq-report.service`) and on demand | QEMU: printed at uptime 83 s, `failed_units none` |
| Watcher | `linux/watch_boot.ps1`, tested three ways: scripted U-Boot (CR in the autoboot window → typed `boot`; panic → exit 3), full Linux boot in QEMU (both reports → exit 0), board already running (one CR → report → exit 0) | `linux/qemu_serial_tcp.sh` + `watch_boot.ps1 -Tcp` |
| Emulation limits | QEMU `xilinx-zynq-a9` skips SPL/ps7_init and has no DDR PHY, real SD timing or PL | the board run is the first test of those |
| Login | `root` / `zynq` — hostname `zynq`, `eth0` DHCP, SSH root login on | checked against the rootfs `/etc/shadow` (yescrypt) |

Why the card showed "32 GB but only 9 GB": it came with **one 9.34 GB FAT32 partition and 20.19 GB
unallocated**. Windows shows partitions, not the card. Not a fault.

---

## Board setup (positions from the User Manual photos, pp. 10–11)

1. **Power off** — both USB-C unplugged.
2. **Boot jumper → SD.** Top-right corner of the board, beside the USB-A port: three pin-pairs
   silkscreened `JTAG · QSPI · SD`. Put the cap on the **right-hand pair (SD)**. Once; it stays there.
3. **Insert the card** in the microSD slot on the underside of the board.
4. **`UART` USB-C (J2, the lower port) → the PC.** Its 5 V feeds only the CH340E (schematic sheet 9); it cannot power the board.
5. **`JTAG` USB-C (J8, the upper port) → any USB power**, a PC port or a phone charger. This powers the board; the PWR LED goes blue.

The order of 4 and 5 doesn't matter. If the board is already up when the watcher attaches, the
watcher finds the logged-in shell and asks for the report.

## Watching the boot

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File D:\espicpc\hardware\pz7020-starlite\linux\watch_boot.ps1
```

It waits up to 12 h for the board's CH340 COM port (a CH340 that appears after it starts, or the
only one present), listens at 115200 8N1 with DTR/RTS not asserted, and writes
`linux/captures/boot-<time>/`:

- `console.log` — every byte the board sent, plus timestamped `[watcher ...]` notes.
- `summary.md` — how far the boot got with times (SPL → U-Boot → DRAM → boot.scr → pl.bit → kernel →
  rootfs → systemd → autologin → report), both copies of the board report, the U-Boot console
  verbatim, and every line that says error / fail / timeout / warn.

`linux/captures/status.txt` holds the live state. Exit code: 0 report captured, 2 boot stalled,
3 kernel panic, 4 board never spoke, 5 booted without autologin (an older image).

What it types, and nothing else:
- a CR every 30 s while the line is silent and no boot has been seen, to find a board that is already running;
- `boot` if U-Boot's `Zynq> ` prompt appears, meaning a CR landed in the 2 s autoboot window;
- `zynq-report`, once, 20 s after the boot report, so the second copy has DHCP and a finished boot.

The SPL line `spl_load_image_fat_os: error reading image uImage` is expected. This SPL is built with
falcon mode: it tries `uImage` + `system.dtb` first and loads `u-boot.img` when they are absent
(`common/spl/spl_mmc.c`).

## Find it on the network

`eth0` asks for DHCP; the report's `eth0` row gives the address. Without the console: plug the
PS-side ethernet jack into the router, then look for the Xilinx OUI `00-0a-35` (adjust the subnet):
```powershell
1..254 | % { (New-Object System.Net.NetworkInformation.Ping).SendPingAsync("192.168.2.$_",600) } | Out-Null
Start-Sleep 3; arp -a | Select-String '00-0a-35'
ssh root@<ip>        # password: zynq
```

---

## Windows popup: "You need to format the disk… Format disk?" → **No**

Windows can't read the ext4 root partition and offers to wipe it. Formatting destroys the image.
It appears whenever the card is plugged into Windows. Always answer **No**.

## Rewriting the card

```powershell
Start-Process python -Verb RunAs -ArgumentList '"D:\espicpc\hardware\pz7020-starlite\linux\write_sd.py"'
Get-Content "$env:TEMP\pz7020_write_sd.log"      # last line DONE rc=0 = success
```
Approve the UAC prompt (raw disk access needs elevation). The tool finds the card by facts
(USB, 28–34 GiB, SD-reader name) and refuses if zero or several disks match; checks the image
hash; writes unbuffered with the **partition table written last**; reads the whole range back
and hashes it. Writing the MBR first fails: Windows re-scans the card as soon as a valid partition
table appears and invalidates the raw handle (measured: WinError 433 at 516 MiB).

## If the console stays silent (watcher exit 4)

- Jumper not on the SD pair, or the card not fully seated → re-check steps 2–3.
- PWR LED not blue → the `JTAG` port (J8) isn't powered.
- Wrong baud shows as *garbage*, not silence. Silence at every baud = nothing is running.
- `deploy-sd.ps1` is only for copying a folder of boot files onto a FAT32 card; this card was
  written as a whole image, so it isn't needed.

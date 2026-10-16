# Card evidence, 2026-10-05

The raw logs behind what docs/61 and `hardware/pz7020-starlite/linux/EXPERIMENT.md` say about the first
real boot of FPGA #1 and the Orange Pi's second boot. Both SD cards were read in the PC's card reader,
read-only, after the owner had booted them on the bench. Card #1 was first copied whole (`fpga1-card.img`,
1,746,927,616 bytes, sha256 `aadadb18…`, kept on the PC, not committed) and read from the copy.

| File | What it is | Made by |
|---|---|---|
| `read_cards.log` | the card read: disks, partitions, the copy's sha256 | `machine/cards/read_cards.py` |
| `fpga1-journal.txt` | FPGA #1's own systemd journal from that boot, as it reached the card (kernel log from 0 s; the last flushed entry is at 5.55 s) | `journalctl --file system.journal -o short-monotonic`, on a copy of the root partition with its ext4 journal replayed, mounted read-only |
| `pi-files.txt`, `pi-extents.txt` | the Pi card: chosen files and directory listings; where the bytes of `nvme-auto` and `/etc/fstab` sit on the card | `read_cards.py` (read-only ext4 reader) |
| `pi-more.txt` | the Pi's superblock and the logs that survived (its /var/log is in RAM) | `read_pi_more.py` |
| `write_fpga1.log`, `write_cards.log` | the rewrite of both cards (2026-10-05 21:31-21:35): card #1 written with image `eeb4afce…` and read back unbuffered byte for byte; the Pi's `nvme-auto` rewritten in place and read back | `machine/cards/write_cards.py` |
| `verify_cards.log` | the rewritten files read back through the filesystems, by path | `machine/cards/verify_cards.py` |

What these show about FPGA #1 (2026-10-05), all on the hardware:
- SPL, DDR3L, U-Boot, the bitstream, Linux 6.12 and systemd ran; `Memory: 160384K/524288K available`
  (512 MiB, the rest reserved for the fabric).
- no `cpufreq` BUG: the 2026-09-24 boot panicked there at 1.95 s (`linux/captures/boot-20260924-161853`);
  this one went on into systemd at 4.0 s, so the single 766,666 kHz operating point fixed it on the board.
- `clk: Disabling unused clocks` at 1.91 s, with FCLK0 unheld in that device tree.
- the last write to reach the card is at 8.014 s of uptime (ext4 journal commits,
  `machine/cards/fpga1-first-boot-forensics.txt`); when the board stopped after that, the card cannot say.

The Pi files list only names and sizes under `/etc/netplan`; no network configuration contents are here.
The NUL bytes PowerShell left in two logs were stripped; nothing else was changed. Not included: a
file-access timeline of card #1. It rested on access times, and the image mounts its root filesystem
`noatime` (`/etc/fstab`: `/dev/mmcblk0p2 / ext4 defaults,noatime`), so reads never update them and
they cannot date what the board did. An inference drawn from it was withdrawn.

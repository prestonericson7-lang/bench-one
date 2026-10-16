# The cards: what goes on each, ready before they are plugged in

`python machine/cards/stage.py` gathers everything below into `D:\start\machine-cards\` with a SHA256SUMS
file per card, so writing a card is one step when it is in the reader. Nothing here touches a card by
itself; the commands to write are listed and are run when the owner says the card is in.

| card | for | contents | how it is written |
|---|---|---|---|
| **32 GB #1** (in FPGA #1 today) | FPGA #1, `zynq1` | the machine image (boot files unchanged since 2026-09-26; root with `/opt/machine/models/qwen3b.gguf` and `qwen05b.gguf`, the tools, `machine-bench`); `zynq-node.txt` = `1` | `write_sd.py` (elevated; it finds the one 28–34 GiB USB card reader by itself, checks the image hash, writes unbuffered with the partition table last, reads everything back): `Start-Process python -Verb RunAs -ArgumentList '"D:\espicpc\hardware\pz7020-starlite\linux\write_sd.py"'` |
| **32 GB #2** | FPGA #2, `zynq2` | the same image, then `zynq-node.txt` = `2` on the BOOT drive letter | the same writer, then `copy D:\start\machine-cards\zynq\zynq-node-2.txt <BOOT>:\zynq-node.txt` |
| **the Teensy's card** (in the Teensy) | the exact reference | as today (`qwen3b.gguf`, `qwen3b.tok`) plus `qwen05b.gguf` copied to the root; the board picks a model with `::model qwen05b.gguf` (writes `model.txt`, restarts) | plain file copy onto the FAT/exFAT card in a reader |
| **4 GB × 2** | STM32 #1, #2 (phase 2) | `qwen05b.gguf` (676 MB) | plain file copy, FAT32 |
| 64 GB / 128 GB | spare | — | — |

Model files: `qwen3b.gguf` = the Ollama `qwen2.5-coder:3b` blob (1,929,903,072 bytes, the Teensy's model);
`qwen05b.gguf` = `Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF` `q8_0` (675,710,848 bytes, sha256
`e1a77721fa97d412f121878223eec81fb4ae6f271e18f922d746711f67b344d1`, downloaded 2026-09-29 and verified).

Why Q8_0 for the small model: its rows are 896 wide, which cannot be Q4_K (block 256), and the exact core
takes Q4_K, Q6_K and Q8_0 only — Q8_0 is the one file every part of the machine reads.

Both FPGA cards are written with the machine image before bench day. Its first stage (`boot.bin`,
`u-boot.img`, `zImage`, the DTB, `pl.bit`) is byte for byte the set card #1 carries now, so FPGA #1's
first boot tests that first stage exactly as the old card would have, and the root filesystem it then
runs is the one booted in QEMU (`machine/zynq/qemu_machine_test.sh`).

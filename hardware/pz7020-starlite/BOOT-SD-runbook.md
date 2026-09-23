# PZ7020-StarLite — boot-from-SD runbook (ready to run)

Goal: boot the board from the SD card and see it over the console + ethernet.
Card `E:\` is already **FAT32, label ZYNQBOOT** (formatted). Nothing here can harm the
board — no unsafe/other-board image is ever used.

**Prereq:** the boot files for THIS board, from the AITH Dropbox bundle (order-gated,
emailed to support@aithtech.com). Do NOT substitute another board's image.

---

## Step 1 — put the boot image on the card
The bundle arrives in one of two shapes:

### Shape A — a folder of boot files (`BOOT.BIN`, `image.ub`, maybe `boot.scr`, `*.dtb`)
One command:
```powershell
cd D:\espicpc\hardware\pz7020-starlite
.\deploy-sd.ps1 -Src "C:\path\to\the\sd_boot_folder"
```
It verifies `E:` is the FAT32 SD (won't touch the SSD/NVMe), copies the files, and
confirms `BOOT.BIN` landed.

### Shape B — a single `.img` / `.wic` disk image
Don't copy it — **flash** it (it carries its own partitions):
1. Install **balenaEtcher** (free).
2. Flash → select the `.img` → target the **SD card's disk** (verify size ~29–32 GB, NOT the 2 TB SSD) → Flash. It verifies automatically.
(This overwrites the FAT32 format — expected.)

---

## Step 2 — set the board to boot from SD
1. **Power off** the board (unplug both USB-C).
2. Set the **boot-mode jumper/switch to `SD`** (not JTAG, not QSPI).
3. Insert the card.
4. Plug in: **power/JTAG** USB-C (blue PWR light) **+** **UART** USB-C **+** **ethernet** to your router.

---

## Step 3 — watch it boot (console)
Find the CH340 UART COM number and listen at 115200 while it powers up:
```powershell
$ch = Get-CimInstance Win32_PnPEntity | ? { $_.PNPDeviceID -match 'VID_1A86' -and $_.Name -match '\(COM(\d+)\)' }
$com = ([regex]'\(COM(\d+)\)').Match($ch.Name).Groups[1].Value
$p = New-Object System.IO.Ports.SerialPort(("COM"+$com),115200); $p.DtrEnable=$false; $p.RtsEnable=$false
$p.Open(); Start-Sleep 20; $p.ReadExisting(); $p.Close()
```
Expect: FSBL → U-Boot → Linux kernel log → login prompt.

## Step 4 — find it over ethernet
After Linux is up, sweep the LAN and look for the new device (Xilinx MAC `00-0a-35`):
```powershell
1..254 | % { (New-Object System.Net.NetworkInformation.Ping).SendPingAsync("192.168.2.$_",600) } | Out-Null
Start-Sleep 3
arp -a | Select-String '00-0a-35|00-05-b6'   # Xilinx PS ethernet MAC
```
Then SSH in (typical vendor creds are `root`/`root` or `xilinx`/`xilinx` — the bundle's
manual states the real ones):
```powershell
ssh root@<the-ip-found>
```

---

## If the console stays silent after Step 3
- Jumper not on **SD**, or `BOOT.BIN` not in the card root → re-check Step 1/2.
- Confirm blue PWR light is on (powered) and the UART cable is in the other port.
- Wrong baud shows as *garbage* (not silence); if garbage, tell me and I'll decode.

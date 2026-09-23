<#
  deploy-sd.ps1 — copy a Zynq SD boot set onto the FAT32 card, safely.
  Usage:
    .\deploy-sd.ps1 -Src "C:\path\to\bundle\sd_boot_folder"   # folder with BOOT.BIN etc.
  For a single .img file, do NOT use this — flash it with balenaEtcher (see runbook).

  Guards: refuses to write unless the target drive is FAT32 and small (an SD card),
  so it can never scribble on the 2 TB SSD / NVMe by a drive-letter mixup.
#>
param(
  [Parameter(Mandatory=$true)][string]$Src,
  [string]$Drive = "E"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Src)) { Write-Error "Source not found: $Src"; exit 1 }
if ((Get-Item $Src).PSIsContainer -eq $false) {
  Write-Host "Src is a file, not a folder. If it's a .img, flash it with balenaEtcher instead (see BOOT-SD-runbook.md)." -ForegroundColor Yellow
  exit 1
}

$v = Get-Volume -DriveLetter $Drive
$sizeGB = [math]::Round($v.Size/1GB,1)
if ($v.FileSystem -ne "FAT32") { Write-Error "$($Drive): is $($v.FileSystem), not FAT32. Re-run the format first."; exit 1 }
if ($v.Size -gt 130GB) { Write-Error "$($Drive): is $sizeGB GB — too big to be the SD card. Refusing (protecting the SSD/NVMe)."; exit 1 }

Write-Host ("Target OK: {0}: FAT32 {1} GB label '{2}'" -f $Drive,$sizeGB,$v.FileSystemLabel) -ForegroundColor Green
Write-Host ("Copying from: {0}" -f $Src)
Copy-Item -Path (Join-Path $Src '*') -Destination ("{0}:\" -f $Drive) -Recurse -Force

Write-Host "--- card contents now ---"
Get-ChildItem ("{0}:\" -f $Drive) | Select-Object Name,Length | Format-Table -AutoSize

$boot = Test-Path ("{0}:\BOOT.BIN" -f $Drive)
if ($boot) { Write-Host "BOOT.BIN present -> card is bootable. Set jumper to SD and power-cycle." -ForegroundColor Green }
else { Write-Host "WARNING: no BOOT.BIN in the card root. A Zynq SD boot needs BOOT.BIN there. Check the source folder." -ForegroundColor Yellow }

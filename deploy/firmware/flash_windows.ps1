<#
flash_windows.ps1 -- program the car's microcontrollers from this PC (the same images /opt/car/flash.sh
uses on the Orange Pi). Plug in ONLY the board you are flashing.

  powershell -ExecutionPolicy Bypass -File flash_windows.ps1 logger    # Teensy 4.1 CAN logger
  powershell -ExecutionPolicy Bypass -File flash_windows.ps1 display   # Teensy 4.1 vent display
  powershell -ExecutionPolicy Bypass -File flash_windows.ps1 climate   # ESP32-S3 climate node

Each image is checked against SHA256SUMS before anything is sent. Teensy: arduino-cli's Teensy
uploader (it reboots a running Teensy into its bootloader; press the program button if it asks).
ESP32-S3: the esptool that ships with arduino-cli's esp32 core, merged image at 0x0.
#>
param([Parameter(Mandatory = $true)][ValidateSet('logger', 'display', 'climate')][string]$Board)
$ErrorActionPreference = 'Stop'
$FwDir = $PSScriptRoot
$ImageFor = @{ logger = 'car-can-logger.ino.hex'; display = 'vent-display.ino.hex'; climate = 'vent-climate-node.ino.merged.bin' }
$ImageName = $ImageFor[$Board]
$ImagePath = Join-Path $FwDir $ImageName

# ---- checksum ----
$SumLine = Get-Content (Join-Path $FwDir 'SHA256SUMS') | Where-Object { $_ -match ([regex]::Escape($ImageName) + '$') } | Select-Object -First 1
if (-not $SumLine) { Write-Host "no checksum recorded for $ImageName"; exit 2 }
$Expected = ($SumLine -split '\s+')[0].ToLower()
$Actual = (Get-FileHash -Algorithm SHA256 $ImagePath).Hash.ToLower()
if ($Expected -ne $Actual) { Write-Host "image $ImageName failed its checksum"; exit 2 }
Write-Host "image ok: $ImageName  sha256 $Actual"

$Cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $Cli) { $Cli = 'C:\Program Files\Arduino CLI\arduino-cli.exe' }
if (-not (Test-Path $Cli)) { Write-Host 'arduino-cli not found'; exit 3 }

if ($Board -eq 'climate') {
  $EspPort = Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match 'VID_303A' -and $_.Name -match '\((COM\d+)\)' } |
    ForEach-Object { [regex]::Match($_.Name, '\((COM\d+)\)').Groups[1].Value } | Select-Object -First 1
  if (-not $EspPort) { Write-Host 'no ESP32-S3 (USB VID 303A) found -- plug the climate node in'; exit 4 }
  $EspTool = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esptool_py') -Recurse -Filter 'esptool.exe' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $EspTool) { Write-Host 'esptool.exe not found in the arduino-cli esp32 core'; exit 3 }
  Write-Host "flashing $ImageName to $EspPort with $($EspTool.FullName)"
  & $EspTool.FullName --chip esp32s3 --port $EspPort --baud 921600 write_flash 0x0 $ImagePath
  if ($LASTEXITCODE -ne 0) { Write-Host 'FLASH FAILED'; exit 5 }
} else {
  $BoardList = & $Cli board list --format json | ConvertFrom-Json
  $Ports = @($BoardList.detected_ports | Where-Object { $_.port.protocol -eq 'teensy' -or $_.port.properties.vid -eq '0x16C0' })
  if ($Ports.Count -eq 0) { Write-Host 'no Teensy found -- plug the board in (USB cable with data lines)'; exit 4 }
  if ($Ports.Count -gt 1) { Write-Host "$($Ports.Count) Teensys connected -- plug in only the one to flash"; exit 4 }
  $TeensyPort = $Ports[0].port
  Write-Host "flashing $ImageName to $($TeensyPort.address) ($($TeensyPort.protocol))"
  & $Cli upload -b teensy:avr:teensy41 -p $TeensyPort.address -l $TeensyPort.protocol --input-file $ImagePath
  if ($LASTEXITCODE -ne 0) { Write-Host 'FLASH FAILED'; exit 5 }
}
Write-Host "FLASHED $ImageName"

# Capture serial output from a board after it is programmed.
# Finds whichever COM port the given USB vendor id enumerated as, because that
# number changes between boards and between reboots.
param([string]$Vid, [string]$Until, [string]$Out)
$deadline = (Get-Date).AddSeconds(420); $idle = 0
while ((Get-Date) -lt $deadline) {
  $port = $null
  Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
    Where-Object { $_.PNPDeviceID -like "*$Vid*" -and $_.Name -match '\(COM(\d+)\)' } |
    ForEach-Object { if ($_.Name -match '\(COM(\d+)\)') { $port = "COM$($matches[1])" } }
  if (-not $port) { Start-Sleep -Milliseconds 300; continue }
  try {
    $sp = New-Object System.IO.Ports.SerialPort $port,115200,'None',8,'one'
    $sp.ReadTimeout = 2000; $sp.Open()
    while ($sp.IsOpen -and (Get-Date) -lt $deadline) {
      try {
        $line = $sp.ReadLine()
        Add-Content -Path $Out -Value $line -Encoding utf8
        Write-Host "    $line"
        $idle = 0
        if ($Until -and $line -match $Until) { Start-Sleep -Seconds 1; $sp.Close(); return }
      } catch [TimeoutException] { $idle++; if ($idle -gt 25) { $sp.Close(); return } }
      catch { break }
    }
    $sp.Close()
  } catch { Start-Sleep -Milliseconds 300 }
}

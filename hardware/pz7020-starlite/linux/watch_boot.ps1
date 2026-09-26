<#
watch_boot.ps1 -- hands-off boot capture for the Puzhi PZ7020-StarLite.

  Physical setup (the only manual part):
    * SD card in the board; boot jumper on SD.
    * Lower USB-C (J2, UART, CH340E) -> this PC.  Upper USB-C (J8, JTAG) -> a 5 V USB-A charger (2 A or more) with a USB-A to USB-C cable; it
      powers the board. A C-to-C cable gives no power (the board's USB-C ports have no CC resistors).
      (The UART port's 5 V only feeds the CH340E, never the board.)
  Run (leave it running; order of plugging does not matter):
    powershell -NoProfile -ExecutionPolicy Bypass -File watch_boot.ps1
  Output:
    captures\boot-<yyyyMMdd-HHmmss>\console.log   every byte the board sent, plus [watcher ...] notes
    captures\boot-<yyyyMMdd-HHmmss>\summary.md    how far the boot got, with times, and the board's own report
    captures\status.txt                           the live state, one line

  The only things it ever types into the console:
    * CR, while the line is silent and no boot has been seen -- finds a board that is already running
      (the image logs root in on the serial console by itself).
    * "boot", if U-Boot's "Zynq> " prompt appears -- a keypress landed in the 2 s autoboot window.
    * "zynq-report", once, after Linux is up -- a second copy of the board report, after DHCP and boot settle.

  Exit codes: 0 report captured | 2 boot stalled | 3 kernel panic | 4 board never spoke | 5 booted, but no autologin
  -Tcp host:port reads a TCP socket instead of a COM port (used to test this script against QEMU).
#>
param(
  [double]$WaitHours = 12,
  [int]$Baud = 115200,
  [string]$Com = "",
  [int]$StallSeconds = 120,
  [string]$Tcp = "",
  [string]$OutRoot = ""
)
$ErrorActionPreference = 'Stop'
if (-not $OutRoot) { $OutRoot = Join-Path $PSScriptRoot 'captures' }
$dir = Join-Path $OutRoot ('boot-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$rawf  = Join-Path $dir 'console.log'
$sumf  = Join-Path $dir 'summary.md'
$statf = Join-Path $OutRoot 'status.txt'
$latin1 = [Text.Encoding]::GetEncoding(28591)
$utf8   = New-Object Text.UTF8Encoding($false)
$clock  = [Diagnostics.Stopwatch]::StartNew()
$rbuf   = New-Object byte[] 65536
[IO.File]::WriteAllText($rawf, '', $latin1)

# boot stages in the order they happen: key, regex on a cleaned console line, what it proves
$Stages = @(
  @('spl',     'U-Boot SPL \d{4}',                 'SPL banner: BootROM loaded BOOT.BIN, ps7_init (clocks, MIO, DDR) is DONE and DDR works (the SPL needs DDR before it prints)'),
  @('uboot',   '^U-Boot \d{4}\.',                  'U-Boot proper: SPL brought DDR up and loaded u-boot.img'),
  @('dram',    '^DRAM:\s',                         'U-Boot reports the memory size from its device tree (not a measurement)'),
  @('bootscr', 'Found U-Boot script|## Executing script', 'U-Boot found boot.scr on the SD card'),
  @('plload',  'Loading PL bitstream',             'boot.scr is loading pl.bit into the fabric'),
  @('kernel',  'Starting kernel',                  'U-Boot handed over to Linux'),
  @('linux',   'Booting Linux on',                 'Linux kernel running'),
  @('mem',     '\] Memory: ',                      'Linux sees its memory'),
  @('rootfs',  'EXT4-fs \(mmcblk0p2\): mounted',   'root filesystem on the SD card mounted'),
  @('systemd', 'Welcome to .*Debian',              'systemd (userspace) started'),
  @('login',   'automatic login',                  'serial console logged root in by itself'),
  @('report',  'ZYNQ-REPORT BEGIN',                'board report printing'),
  @('panic',   'Kernel panic',                     'KERNEL PANIC')
)
$Reached = [ordered]@{}
$Lines = New-Object 'System.Collections.Generic.List[object]'
$Typed = New-Object 'System.Collections.Generic.List[object]'
$State = @{ first = $null; lastByte = $null; openAt = 0.0; lastPoke = -1e9; pokes = 0; bootSent = @(); shell = $null
        reqAt = $null; panicAt = $null; ends = @(); partial = ''; link = ''; inReport = $false; wasUp = $false }
$script:sp = $null; $script:tc = $null; $script:ns = $null

function Now { $clock.Elapsed.TotalSeconds }
function Rel([double]$t) { if ($null -eq $State.first) { $t } else { $t - $State.first } }
function Status([string]$s) {
  $line = '{0:HH:mm:ss}  {1}' -f (Get-Date), $s
  [Console]::WriteLine($line)
  [IO.File]::WriteAllText($statf, "$line`r`ncapture folder: $dir`r`n", $utf8)
}
function Note([string]$s, [switch]$Quiet) {
  $t = Now
  $msg = '[watcher {0:HH:mm:ss.fff}] {1}' -f (Get-Date), $s
  [IO.File]::AppendAllText($rawf, "`r`n$msg`r`n", $latin1)
  $Lines.Add([pscustomobject]@{ t = $t; s = $msg; w = $true; r = $false })
  if (-not $Quiet) { Status $s }
}
function Clean([string]$s) {
  ($s -replace '\x1b\[[0-9;?]*[ -/]*[@-~]', '' -replace '\x1b[()][0-9A-Za-z]', '' -replace '[\x00-\x08\x0b-\x1f\x7f]', '').TrimEnd()
}

# ---- link: CH340 COM port (or TCP for tests) ----
function Ch340Ports {
  @(Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match 'VID_1A86&PID_(7523|5523)' -and $_.Name -match '\(COM\d+\)' } |
    ForEach-Object { ([regex]'\((COM\d+)\)').Match($_.Name).Groups[1].Value })
}
function Pick-Com {
  if ($Com) { return $Com }
  $now = @(Ch340Ports)
  $new = @($now | Where-Object { $baseline -notcontains $_ })
  if ($new.Count) { return $new[0] }
  if ($baseline.Count -eq 1 -and $now -contains $baseline[0]) { return $baseline[0] }
  return $null
}
function Open-Link {
  if ($Tcp) {
    $hp = $Tcp.Split(':')
    try { $c = New-Object Net.Sockets.TcpClient; $c.Connect($hp[0], [int]$hp[1]); $script:tc = $c; $script:ns = $c.GetStream(); return "tcp $Tcp" }
    catch { return $null }
  }
  $c = Pick-Com
  if (-not $c) { return $null }
  $p = New-Object IO.Ports.SerialPort($c, $Baud, ([IO.Ports.Parity]::None), 8, ([IO.Ports.StopBits]::One))
  $p.DtrEnable = $false; $p.RtsEnable = $false; $p.Handshake = [IO.Ports.Handshake]::None
  $p.ReadTimeout = 200; $p.WriteTimeout = 2000
  try { $p.Open(); $script:sp = $p; return "$c $Baud 8N1, DTR/RTS not asserted" }
  catch { Status "cannot open ${c}: $($_.Exception.Message)"; return $null }
}
function Read-Link {
  if ($script:ns) {
    if ($script:ns.DataAvailable) {
      $n = $script:ns.Read($rbuf, 0, $rbuf.Length)
      if ($n -le 0) { throw 'connection closed' }
      return $latin1.GetString($rbuf, 0, $n)
    }
    if ($script:tc.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead) -and $script:tc.Client.Available -eq 0) { throw 'connection closed' }
    return ''
  }
  $n = $script:sp.BytesToRead
  if ($n -gt 0) { $k = $script:sp.Read($rbuf, 0, [Math]::Min($n, $rbuf.Length)); return $latin1.GetString($rbuf, 0, $k) }
  return ''
}
function Close-Link {
  try { if ($script:sp) { $script:sp.Close() } } catch {}
  try { if ($script:tc) { $script:tc.Close() } } catch {}
  $script:sp = $null; $script:tc = $null; $script:ns = $null
}
function Send([string]$s, [string]$why, [switch]$Quiet) {
  $b = $latin1.GetBytes($s)
  if ($script:ns) { $script:ns.Write($b, 0, $b.Length) } else { $script:sp.Write($b, 0, $b.Length) }
  $shown = $s -replace "`r", '<CR>'
  if (-not $Quiet) { $Typed.Add([pscustomobject]@{ t = (Now); s = ('typed {0}  ({1})' -f $shown, $why) }) }
  Note "typed $shown ($why)" -Quiet:$Quiet
}

# ---- what the board said ----
function ShellSeen { if ($null -eq $State.shell) { $State.shell = Now; $State.wasUp = ($Reached.Count -eq 0); Status 'Linux shell is up on the serial console' } }
function UBootPrompt {
  if ($State.bootSent.Count -ge 2) { return }
  if ($State.bootSent.Count -and ((Now) - $State.bootSent[-1]) -lt 10) { return }
  $State.bootSent += (Now)
  Send "boot`r" 'U-Boot is sitting at its Zynq> prompt; resuming the boot'
}
function Line([string]$s, [double]$t) {
  if ($s -match 'ZYNQ-REPORT BEGIN') { $State.inReport = $true }
  $Lines.Add([pscustomobject]@{ t = $t; s = $s; w = $false; r = $State.inReport })
  if ($s -match 'ZYNQ-REPORT END') { $State.inReport = $false }
  foreach ($g in $Stages) {
    if (-not $Reached.Contains($g[0]) -and $s -match $g[1]) {
      $Reached[$g[0]] = [pscustomobject]@{ t = $t; s = $s.Trim(); what = $g[2] }
      Status ('{0}  <-  {1}' -f $g[2], $s.Trim())
    }
  }
  if ($s -match 'ZYNQ-REPORT END') { $State.ends += $t }
  if ($s -match 'Kernel panic' -and $null -eq $State.panicAt) { $State.panicAt = $t }
  if ($s -match 'automatic login') { ShellSeen }
}
function Take([string]$chunk) {
  $t = Now
  [IO.File]::AppendAllText($rawf, $chunk, $latin1)
  if ($null -eq $State.first) { $State.first = $t; Status 'console active: first byte' }
  $State.lastByte = $t
  $State.partial += $chunk
  $parts = $State.partial -split "`n"
  $State.partial = $parts[-1]
  for ($i = 0; $i -lt $parts.Count - 1; $i++) { Line (Clean $parts[$i]) $t }
  $tail = Clean $State.partial
  if ($tail -match 'Zynq> ?$') { UBootPrompt }
  if ($tail -match 'root@zynq:[^\r\n]*[#$] ?$') { ShellSeen }
}

# ---- the write-up ----
function Finish([int]$rc, [string]$why) {
  Close-Link
  if ($State.partial) { Line (Clean $State.partial) (Now); $State.partial = '' }
  $sb = New-Object Text.StringBuilder
  $A = { param($x) [void]$sb.AppendLine($x) }
  & $A ('# PZ7020-StarLite boot capture, {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
  & $A ''
  & $A ('Result: **{0}** (exit {1}). Link: {2}. Times are host seconds from the first console byte.' -f $why, $rc, $State.link)
  & $A ''
  & $A '## How far it got'
  & $A ''
  & $A '| t (s) | stage | console line |'
  & $A '|---:|---|---|'
  foreach ($g in $Stages) {
    if ($Reached.Contains($g[0])) { $e = $Reached[$g[0]]; & $A ('| {0:0.00} | {1} | `{2}` |' -f (Rel $e.t), $e.what, (($e.s -replace '\|', '\|') -replace '`', "'")) }
  }
  $notseen = @($Stages | Where-Object { $_[0] -ne 'panic' -and $_[0] -ne 'plload' -and -not $Reached.Contains($_[0]) } | ForEach-Object { $_[0] })
  & $A ''
  & $A ('Stages not seen: {0}' -f $(if ($notseen.Count) { $notseen -join ', ' } else { 'none' }))
  if ($State.wasUp) { & $A ''; & $A 'The board was already running when the watcher attached, so the boot itself was not observed; only the report below was.' }
  & $A ''
  & $A '## What the watcher typed'
  & $A ''
  if ($Typed.Count) { foreach ($x in $Typed) { & $A ('- t={0:0.00}s  {1}' -f (Rel $x.t), $x.s) } } else { & $A '- nothing' }
  if ($State.pokes -gt 1) { & $A ('- CR pokes while waiting: {0} in total, one every 30 s of silence' -f $State.pokes) }
  # board reports: between the BEGIN/END markers; journal prefixes stripped
  $reports = @(); $cur = $null; $curT = 0
  foreach ($x in $Lines) {
    if ($x.w) { continue }
    $s = $x.s -replace '^\[\s*\d+\.\d+\]\s+python3\[\d+\]:\s?', ''
    if ($s -match 'ZYNQ-REPORT BEGIN') { $cur = New-Object Text.StringBuilder; $curT = $x.t; continue }
    if ($null -ne $cur) {
      if ($s -match 'ZYNQ-REPORT END') { $reports += ,@($curT, $cur.ToString().TrimEnd()); $cur = $null }
      else { [void]$cur.AppendLine($s) }
    }
  }
  foreach ($r in $reports) {
    $how = if ($null -ne $State.reqAt -and $r[0] -gt $State.reqAt) { 'typed by the watcher after the boot settled' } else { 'printed by the board during boot (zynq-report.service)' }
    & $A ''
    & $A ('## Board report, {0}, t={1:0.0} s' -f $how, (Rel $r[0]))
    & $A ''
    & $A '```'
    & $A $r[1]
    & $A '```'
  }
  if (-not $reports.Count) { & $A ''; & $A '## Board report'; & $A ''; & $A 'None captured.' }
  # the U-Boot part of the console, verbatim: SPL banner (or U-Boot banner) through the kernel handoff
  $u0 = if ($Reached.Contains('spl')) { $Reached['spl'].t } elseif ($Reached.Contains('uboot')) { $Reached['uboot'].t } else { $null }
  if ($null -ne $u0) {
    $u1 = if ($Reached.Contains('kernel')) { $Reached['kernel'].t } else { [double]::MaxValue }
    $ul = @($Lines | Where-Object { -not $_.w -and $_.t -ge $u0 -and $_.t -le $u1 } | Select-Object -First 150 | ForEach-Object { $_.s })
    & $A ''
    & $A '## U-Boot console, verbatim (SPL banner to kernel handoff)'
    & $A ''
    & $A '```'
    foreach ($x in $ul) { & $A $x }
    & $A '```'
    if (@($ul | Where-Object { $_ -match 'uImage' }).Count) {
      & $A 'A line naming uImage is expected: this SPL is built with falcon mode, tries to load uImage + system.dtb first, and falls back to u-boot.img when they are absent (common/spl/spl_mmc.c).'
    }
  }
  $err = @($Lines | Where-Object { -not $_.w -and -not $_.r -and $_.s -match '(?i)error|fail|timed? ?out|unable|not found|panic|oops|warn' } | Select-Object -First 40)
  & $A ''
  & $A '## Console lines that say error / fail / timeout / warn, outside the reports (first 40, not interpreted)'
  & $A ''
  & $A '```'
  if ($err.Count) { foreach ($x in $err) { & $A ('{0,8:0.00}  {1}' -f (Rel $x.t), $x.s) } } else { & $A '(none)' }
  & $A '```'
  & $A ''
  & $A '## Last 30 console lines'
  & $A ''
  & $A '```'
  foreach ($x in @($Lines | Select-Object -Last 30)) { & $A ('{0,8:0.00}  {1}' -f (Rel $x.t), $x.s) }
  & $A '```'
  & $A ''
  & $A 'Every byte the board sent is in console.log next to this file.'
  [IO.File]::WriteAllText($sumf, $sb.ToString(), $utf8)
  Status ('DONE rc={0}: {1}. Summary: {2}' -f $rc, $why, $sumf)
  exit $rc
}

# ---- main loop ----
$baseline = @(if (-not $Tcp -and -not $Com) { Ch340Ports })
Status ('watching; CH340 ports already present: {0}; capture folder {1}' -f $(if ($baseline.Count) { $baseline -join ',' } else { 'none' }), $dir)
if ($baseline.Count -gt 1) { Status 'several CH340 ports already present: plug the board UART in now (a new port is taken), or rerun with -Com COMx' }
while ($true) {
  if (-not $script:sp -and -not $script:ns) {
    $d = Open-Link
    if ($d) { $State.link = $d; $State.openAt = Now; Note "listening on $d" }
    else {
      if ($null -eq $State.first -and (Now) -ge $WaitHours * 3600) { Finish 4 ("the board console never appeared in {0} h" -f $WaitHours) }
      Start-Sleep -Milliseconds 1000; continue
    }
  }
  try { $chunk = Read-Link } catch { Note "link lost ($($_.Exception.Message)); waiting for it to come back"; Close-Link; continue }
  if ($chunk) { Take $chunk }

  $t = Now
  $tail = Clean $State.partial
  $silent = if ($null -ne $State.lastByte) { $t - [Math]::Max($State.lastByte, $State.openAt) } else { $t - $State.openAt }
  # stray bytes with nothing recognisable after them: forget them, keep waiting for a real boot
  if ($null -ne $State.first -and $Reached.Count -eq 0 -and $null -eq $State.shell -and $silent -ge 10 -and $tail -notmatch 'login: ?$' -and ($t - $State.first) -ge 30) {
    Note 'bytes arrived but no boot followed; still waiting'; $State.first = $null
  }
  # silent line, no boot seen: poke for a board that is already up
  if ($Reached.Count -eq 0 -and $null -eq $State.shell -and $silent -ge 20 -and ($t - $State.lastPoke) -ge 30) {
    $State.lastPoke = $t; $State.pokes++
    Send "`r" 'line silent and no boot seen: checking for a board that is already running' -Quiet:($State.pokes -gt 1)
  }
  # a plain login prompt that stays put: Linux booted, but not from the current image (no autologin)
  if ($tail -match '(^|\s)login: ?$' -and ($t - $State.lastByte) -ge 5) { Finish 5 'Linux booted to a plain login prompt: this card does not hold the autologin image' }
  # Linux is up: ask for the settled report once
  if ($null -ne $State.shell -and $null -eq $State.reqAt) {
    $bootEnd = if ($State.ends.Count) { $State.ends[0] } else { $null }
    $ready = ($Reached.Count -eq 0) -or ($null -ne $bootEnd -and ($t - $bootEnd) -ge 20) -or (($t - $State.shell) -ge 90)
    if ($ready -and ($t - $State.lastByte) -ge 2) { $State.reqAt = $t; Send "zynq-report`r" 'Linux is up: asking the board for its report after the boot settled' }
  }
  if ($null -ne $State.reqAt) {
    $got = @($State.ends | Where-Object { $_ -gt $State.reqAt }).Count
    if ($got -and ($t - $State.lastByte) -ge 2) { Finish 0 'board report captured' }
    if (($t - $State.reqAt) -ge 60) { Finish $(if ($State.ends.Count) { 0 } else { 2 }) 'the typed zynq-report did not answer within 60 s' }
  }
  if ($null -ne $State.panicAt -and ($t - $State.panicAt) -ge 8) { Finish 3 'kernel panic' }
  if ($Reached.Count -gt 0 -and $null -eq $State.shell -and ($t - $State.lastByte) -ge $StallSeconds) { Finish 2 ("no console output for {0} s, before Linux was up" -f $StallSeconds) }
  if ($Reached.Count -gt 0 -and $null -eq $State.reqAt -and ($t - $State.first) -ge 900) { Finish 2 'Linux not up within 15 min of the first boot line' }
  if ($null -eq $State.first -and $Reached.Count -eq 0 -and $t -ge $WaitHours * 3600) { Finish 4 ("the board never spoke in {0} h" -f $WaitHours) }
  Start-Sleep -Milliseconds 100
}

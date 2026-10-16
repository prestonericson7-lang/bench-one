# run_xsim.ps1 -- pl_regs.v (the RTL in vivado/build/system.bit) under Vivado's simulator:
#   tb_pl_regs.v            every register, the time master, PWM, tach, keys
#   tb_pl_regs_clockstop.v  what the FCLK0 experiment assumes: the registers plx.py reads, the counter
#                           stopping for exactly the cycles its clock is held, a read stalling while the
#                           clock is held and completing once it runs
#     powershell -ExecutionPolicy Bypass -File hardware\pz7020-starlite\ps7-axi\run_xsim.ps1
$ErrorActionPreference = 'Continue'
$V = 'D:\2026.1\Vivado\bin'
$S = $PSScriptRoot
$W = Join-Path $env:TEMP 'pl_regs_xsim'
Remove-Item -Recurse -Force $W -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $W | Out-Null
Set-Location $W
$fail = 0
foreach ($tb in 'tb_pl_regs', 'tb_pl_regs_clockstop') {
    "== $tb"
    & "$V\xvlog.bat" "$S\$tb.v" "$S\pl_regs.v" *> "$tb.xvlog.log"
    if ($LASTEXITCODE) { "xvlog failed"; $fail = 1; continue }
    & "$V\xelab.bat" $tb -s "${tb}_sim" -debug off *> "$tb.xelab.log"
    if ($LASTEXITCODE) { "xelab failed"; $fail = 1; continue }
    & "$V\xsim.bat" "${tb}_sim" -R *> "$tb.xsim.log"
    $out = Get-Content "$tb.xsim.log" | Where-Object { $_ -match '  ok  |PASS|FAIL' }
    $out
    if (-not ($out -match '^PASS')) { $fail = 1 }
}
if ($fail) { 'XSIM: FAIL'; exit 1 } else { 'XSIM: PASS'; exit 0 }

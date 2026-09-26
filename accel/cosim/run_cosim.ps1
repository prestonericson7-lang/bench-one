# run_cosim.ps1 -- the real AXI DMA IP + zaccel_gemv + gemv_reset under xsim, driven by tb_cosim.v.
#   powershell -ExecutionPolicy Bypass -File accel\cosim\run_cosim.ps1
# Vivado generates the xsim scripts; they are run here directly, because Vivado's own spawn of
# compile.bat fails on this PC ("Spawn failed: The system cannot find the file specified") while
# the same scripts run fine from cmd with Vivado's bin on PATH.
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Viv = 'D:\2026.1\Vivado\bin'
$env:Path = "$Viv;" + $env:Path
Push-Location $Here
try {
    python gen_cosim.py
    & "$Viv\vivado.bat" -mode batch -nojournal -nolog -source run_cosim.tcl *> run_cosim_tcl.txt
    $Sim = Join-Path $Here 'prj\cosim.sim\sim_1\behav\xsim'
    if (-not (Test-Path "$Sim\simulate.bat")) { Write-Host 'xsim scripts were not generated -- see run_cosim_tcl.txt'; exit 2 }
    Push-Location $Sim
    [Environment]::CurrentDirectory = $Sim
    foreach ($Step in 'compile.bat', 'elaborate.bat', 'simulate.bat') {
        cmd.exe /c "$Sim\$Step" *> "$Here\cosim_$($Step -replace '\.bat$','').txt"
        if ($LASTEXITCODE -ne 0 -and $Step -ne 'simulate.bat') { Write-Host "$Step failed (exit $LASTEXITCODE) -- see cosim_*.txt"; exit 3 }
    }
    Pop-Location
    $Out = Get-Content "$Here\cosim_simulate.txt"
    $Out | Where-Object { $_ -match '^(job|COSIM|FAIL)' }
    if ($Out -match 'COSIM PASS') { exit 0 } else { exit 1 }
} finally { Pop-Location }

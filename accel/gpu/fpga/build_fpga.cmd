@echo off
rem fpga\build_fpga.cmd -- Vivado batch build of the FPGA-GPU bitstream (runs fpga\build.tcl).
rem   build_fpga.cmd                     normal build
rem   build_fpga.cmd -tclargs jobs=8     extra arguments are passed to Vivado
rem Result: ..\out\pl.bit (+ reports, out\build_summary.txt). Log: fpga\vivado_build.log
rem Exit code: 0 ok, 2 bitstream written but timing NOT met,
rem            3 bitstream written and timing met but CRITICAL WARNINGs (see out\build_summary.txt),
rem            anything else = build failed.
setlocal
cd /d "%~dp0"
set "VIVADO=D:\2026.1\Vivado\bin\vivado.bat"
if not exist "%VIVADO%" (
    echo ERROR: %VIVADO% not found. Install Vivado 2026.1 or edit VIVADO in build_fpga.cmd.
    exit /b 1
)
call "%VIVADO%" -mode batch -notrace -nojournal -log vivado_build.log -source build.tcl %*
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" goto ok
if "%RC%"=="2" goto timing
if "%RC%"=="3" goto critical
echo.
echo BUILD FAILED, exit code %RC%. See fpga\vivado_build.log - search for "[build] FAILED".
exit /b %RC%
:timing
echo.
echo BUILD FINISHED BUT TIMING NOT MET: out\pl.bit written, see out\TIMING_FAILED.txt
exit /b 2
:critical
echo.
echo BUILD FINISHED, TIMING MET, BUT CRITICAL WARNINGS: out\pl.bit written, see out\build_summary.txt
exit /b 3
:ok
echo.
echo BUILD OK: out\pl.bit
exit /b 0

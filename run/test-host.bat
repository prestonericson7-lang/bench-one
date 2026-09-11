@echo off
REM ===========================================================================
REM  test-host.bat -- everything that needs NO hardware at all
REM
REM  Safe to run any time. Nothing here touches a board, so it cannot break
REM  anything and cannot be affected by what is or is not plugged in.
REM  NOTE: the .\ prefixes below are required. This machine has
REM  NoDefaultCurrentDirectoryInExePath set, so cmd will not run an executable
REM  from the current folder unless the path is explicit. Removing them breaks
REM  every line with a silent "not recognized" error.
REM ===========================================================================
call "%~dp0_env.bat"
set "LOG=%LOGS%\host-%STAMP%.txt"
set "PATH=%HOSTCC%;%PATH%"

echo Logging to %LOG%
echo. > "%LOG%"
call :run "compiler present" "gcc --version"
cd /d "%SRC%\tests"

echo Building host tools...
gcc -O2 -std=gnu11 -I..\shared -o born_test.exe born_test.c ..\shared\bench_born.c ..\shared\bench_hdc.c 2>>"%LOG%"
gcc -O2 -std=gnu11 -I..\shared -o wonder_test.exe wonder_test.c ..\shared\bench_wonder.c ..\shared\bench_born.c ..\shared\bench_hdc.c 2>>"%LOG%"
gcc -O2 -std=gnu11 -I..\shared -o gemv_host.exe gemv_host.c 2>>"%LOG%"
gcc -O2 -std=gnu11 -I..\shared -o host_kernel.exe host_kernel.c ..\shared\bench_hdc.c 2>>"%LOG%"
gcc -O3 -march=native -std=gnu11 -I..\shared -o host_native.exe host_kernel.c ..\shared\bench_hdc.c 2>>"%LOG%"
gcc -O2 -std=gnu11 -pthread -o host_scaling.exe host_scaling.c 2>>"%LOG%"

call :run "natal frame properties"        ".\born_test.exe"
call :run "superposition and self-direction" ".\wonder_test.exe"
call :run "quantized matmul, portable"    ".\gemv_host.exe"
call :run "HDC kernel, portable build"    ".\host_kernel.exe"
call :run "HDC kernel, AVX2 build"        ".\host_native.exe"
call :run "does adding cores add bandwidth" ".\host_scaling.exe"
call :run "placement planner, llama-3b"   "python plan.py --model llama-3b"
call :run "placement planner, what to buy" "python plan.py --model llama-3b --add zynq=2"
call :run "capacity per dollar and litre" "python fit.py"

echo.
echo ======================================================
echo  DONE. Full log: %LOG%
echo ======================================================
pause
exit /b 0

:run
echo. >> "%LOG%"
echo ====================================================== >> "%LOG%"
echo  %~1 >> "%LOG%"
echo ====================================================== >> "%LOG%"
echo   %~1
%~2 >> "%LOG%" 2>&1
if errorlevel 1 echo   ^(returned an error, see log^)
exit /b 0

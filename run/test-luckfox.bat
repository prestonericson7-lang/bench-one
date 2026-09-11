@echo off
REM ===========================================================================
REM  test-luckfox.bat -- the measurement that decides the Luckfox's role
REM
REM  NEEDS: a Luckfox on USB, booted, showing up over ADB. If ADB finds nothing,
REM  the board has not finished booting or has no image on it.
REM
REM  Answers: is a Luckfox memory bound or compute bound? Its memory delivers
REM  about 930 MB/s. If its CPU cannot consume INT4 weights that fast, the
REM  bandwidth is unreachable and the planner needs correcting.
REM ===========================================================================
call "%~dp0_env.bat"
set "LOG=%LOGS%\luckfox-%STAMP%.txt"
cd /d "%SRC%\tests"

echo Logging to %LOG%
"%ADB%" devices > "%LOG%" 2>&1
"%ADB%" devices | findstr /R "device$" >nul || (echo   No Luckfox over ADB. & pause & exit /b 1)

if not exist luckfox_bench (
  echo   Cross-compiling...
  "%ARMCC%\arm-none-linux-gnueabihf-gcc.exe" -O2 -static -std=gnu11 -march=armv7-a ^
     -mfpu=neon-vfpv4 -mfloat-abi=hard -I..\shared -o luckfox_bench ^
     luckfox_bench.c ..\shared\bench_hdc.c >> "%LOG%" 2>&1
)

echo   Pushing and running...
"%ADB%" push luckfox_bench /mnt/sdcard/luckfox_bench >> "%LOG%" 2>&1
"%ADB%" shell "chmod +x /mnt/sdcard/luckfox_bench; echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null; /mnt/sdcard/luckfox_bench" >> "%LOG%" 2>&1
type "%LOG%" | findstr /C:"MB/s" /C:"BOUND" /C:"clock" /C:"NEON" /C:"DSP"

echo.
echo  DONE. Full log: %LOG%
pause
exit /b 0

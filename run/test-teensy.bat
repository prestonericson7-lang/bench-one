@echo off
REM ===========================================================================
REM  test-teensy.bat -- every Teensy 4.1 measurement, in order
REM
REM  NEEDS: a Teensy 4.1 on USB. PSRAM and an SD card for tests 2 and 3.
REM  If the Teensy does not auto-reboot for programming, PRESS ITS BUTTON once
REM  when told to. That is normal for a board running someone else's sketch.
REM ===========================================================================
call "%~dp0_env.bat"
set "LOG=%LOGS%\teensy-%STAMP%.txt"
set "FQBN=teensy:avr:teensy41"
cd /d "%SRC%\tests"

echo Logging to %LOG%
"%ACLI%" board list > "%LOG%" 2>&1
findstr /I "teensy" "%LOG%" >nul || (echo   No Teensy found on USB. Plug one in. & pause & exit /b 1)
for /f "tokens=1" %%P in ('"%ACLI%" board list ^| findstr /I "teensy41"') do set "PORT=%%P"
echo   Found Teensy at %PORT%

bash sync_teensy.sh >> "%LOG%" 2>&1

call :one "1 memory bandwidth"   stream_teensy  "MoE, 50 MB"
call :one "2 PSRAM verify+speed" psram_teensy   "press reset to run again"
call :one "3 SD card as a store" sd_teensy      "TIER|TOO SLOW"
call :one "4 system properties"  system_test_teensy "EXIT:"

echo.
echo  DONE. Full log: %LOG%
pause
exit /b 0

:one
echo. >> "%LOG%"
echo ============ %~1 ============ >> "%LOG%"
echo   %~1
"%ACLI%" compile --fqbn %FQBN% --upload -p %PORT% %2 >> "%LOG%" 2>&1
if errorlevel 1 (echo     upload failed - press the Teensy button and rerun & exit /b 0)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0capture.ps1" -Vid "VID_16C0" -Until "%~3" -Out "%LOG%"
exit /b 0

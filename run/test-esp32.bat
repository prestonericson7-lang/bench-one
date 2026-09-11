@echo off
REM ===========================================================================
REM  test-esp32.bat -- ESP32-S3 measurements
REM  NEEDS: an ESP32-S3 on USB. Built for octal PSRAM; if it reports 0 MB the
REM  board has quad PSRAM instead, so change PSRAM=opi to PSRAM=enabled below.
REM ===========================================================================
call "%~dp0_env.bat"
set "LOG=%LOGS%\esp32-%STAMP%.txt"
set "FQBN=esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi"
cd /d "%SRC%\tests"

echo Logging to %LOG%
"%ACLI%" board list > "%LOG%" 2>&1
for /f "tokens=1" %%P in ('"%ACLI%" board list ^| findstr /I "esp32"') do set "PORT=%%P"
if "%PORT%"=="" (echo   No ESP32 found on USB. & pause & exit /b 1)
echo   Found ESP32 at %PORT%

call :one "1 memory bandwidth" stream_esp32   "MoE, 50 MB|could not allocate"
call :one "2 kernel and radio" kernel_esp32s3 "reply partially"

echo.
echo  DONE. Full log: %LOG%
pause
exit /b 0

:one
echo. >> "%LOG%"
echo ============ %~1 ============ >> "%LOG%"
echo   %~1
"%ACLI%" compile --fqbn "%FQBN%" --upload -p %PORT% %2 >> "%LOG%" 2>&1
if errorlevel 1 (echo     upload failed & exit /b 0)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0capture.ps1" -Vid "VID_303A" -Until "%~3" -Out "%LOG%"
exit /b 0

@echo off
rem flash_teensy.cmd -- upload the firmware built by build_teensy.cmd to a Teensy 4.1.
rem   flash_teensy.cmd                  600 MHz build (out\600), port auto-detected
rem   flash_teensy.cmd 816              816 MHz build (out\816)
rem   flash_teensy.cmd 600 COM7         explicit port (as listed by "arduino-cli board list")
rem Uses arduino-cli upload (Teensy Loader). If no Teensy port is found (e.g. a new Teensy that is
rem still in its bootloader, or no USB serial port yet), it falls back to the Teensy tools that the
rem Arduino IDE uses: teensy_post_compile opens the Teensy Loader with the hex file; press the
rem Teensy's program button if it does not start by itself.
setlocal
cd /d "%~dp0"

set SPEED=600
if not "%~1"=="" set SPEED=%~1
set PORT=%~2
if not "%SPEED%"=="600" if not "%SPEED%"=="816" (
  echo flash_teensy: speed must be 600 or 816, got "%SPEED%"
  exit /b 2
)
set OUTDIR=out\%SPEED%
set FQBN=teensy:avr:teensy41:usb=serial,speed=%SPEED%,opt=o2std,keys=en-us

if not exist "%OUTDIR%\teensy_gpu.ino.hex" (
  echo flash_teensy: %OUTDIR%\teensy_gpu.ino.hex not found - run build_teensy.cmd %SPEED% first
  exit /b 1
)

if "%PORT%"=="" (
  for /f "tokens=1" %%P in ('arduino-cli board list 2^>nul ^| findstr /i "teensy"') do if not defined PORT set PORT=%%P
)

if not "%PORT%"=="" (
  echo flash_teensy: uploading %OUTDIR%\teensy_gpu.ino.hex to %PORT%
  arduino-cli upload --fqbn %FQBN% -p %PORT% --input-dir "%OUTDIR%" teensy_gpu
  if errorlevel 1 (
    echo flash_teensy: UPLOAD FAILED
    exit /b 1
  )
  echo flash_teensy: OK
  exit /b 0
)

echo flash_teensy: no Teensy serial port found - using Teensy Loader directly
set TT=
for /d %%D in ("%LOCALAPPDATA%\Arduino15\packages\teensy\tools\teensy-tools\*") do set TT=%%D
if "%TT%"=="" (
  echo flash_teensy: Teensy tools not found under %LOCALAPPDATA%\Arduino15\packages\teensy\tools
  exit /b 1
)
"%TT%\teensy_post_compile.exe" -file=teensy_gpu.ino "-path=%CD%\%OUTDIR%" "-tools=%TT%" -board=TEENSY41 -reboot
if errorlevel 1 (
  echo flash_teensy: Teensy Loader FAILED
  exit /b 1
)
echo flash_teensy: handed to the Teensy Loader (press the program button if it waits)
exit /b 0

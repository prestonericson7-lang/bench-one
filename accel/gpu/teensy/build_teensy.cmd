@echo off
rem build_teensy.cmd -- build the Teensy 4.1 geometry-engine firmware with arduino-cli.
rem   build_teensy.cmd          600 MHz (default)   -> out\600\teensy_gpu.ino.hex
rem   build_teensy.cmd 816      816 MHz overclock   -> out\816\teensy_gpu.ino.hex
rem Copies the shared sources from ..\common into teensy_gpu\src\ first (the sketch includes them
rem from there), then compiles for teensy:avr:teensy41 with USB type Serial and "Faster" (-O2).
rem All intermediate files stay in teensy\out\<speed>\build (nothing outside the project).
rem Needs arduino-cli on PATH with the Teensy core (teensy:avr) installed.
setlocal
cd /d "%~dp0"

set SPEED=600
if not "%~1"=="" set SPEED=%~1
if not "%SPEED%"=="600" if not "%SPEED%"=="816" (
  echo build_teensy: speed must be 600 or 816, got "%SPEED%"
  exit /b 2
)
set OUTDIR=out\%SPEED%
set FQBN=teensy:avr:teensy41:usb=serial,speed=%SPEED%,opt=o2std,keys=en-us

where arduino-cli >nul 2>nul
if errorlevel 1 (
  echo build_teensy: arduino-cli not found on PATH
  exit /b 2
)

if not exist teensy_gpu\src mkdir teensy_gpu\src
for %%F in (gpu_proto.h gpu_setup.h gpu_setup.c geom.h geom.c) do (
  copy /Y "..\common\%%F" "teensy_gpu\src\%%F" >nul
  if errorlevel 1 (
    echo build_teensy: cannot copy ..\common\%%F
    exit /b 1
  )
)

if not exist "%OUTDIR%\build" mkdir "%OUTDIR%\build"
echo build_teensy: %FQBN% -^> %OUTDIR%
rem (the Teensy platform ignores --warnings; teensy\sim "make teensy-strict" checks -Wall -Wextra -Werror)
arduino-cli compile --fqbn %FQBN% --warnings all --build-path "%OUTDIR%\build" --output-dir "%OUTDIR%" teensy_gpu
if not errorlevel 1 goto built
rem A failed archive step can leave a broken core.a behind (seen once: ar.exe "unable to copy file
rem ... core.a: Invalid argument"), which then breaks every later link. Retry once from a clean build.
echo build_teensy: compile failed - retrying once with a clean build directory
rmdir /s /q "%OUTDIR%\build"
mkdir "%OUTDIR%\build"
arduino-cli compile --fqbn %FQBN% --warnings all --build-path "%OUTDIR%\build" --output-dir "%OUTDIR%" teensy_gpu
if errorlevel 1 (
  echo build_teensy: BUILD FAILED
  exit /b 1
)
:built
echo build_teensy: OK  %OUTDIR%\teensy_gpu.ino.hex
exit /b 0

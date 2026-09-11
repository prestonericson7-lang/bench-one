@echo off
REM Build and flash gfx_teensy.
REM
REM The copy is not optional. The Teensy build recipe accepts no extra include path, so
REM machine_scene.h is refreshed from ..\..\shared on every build. That keeps shared\ the single
REM master and makes the local copy disposable -- and it is the only way the claim "the same file
REM compiles on all three" stays true rather than drifting.
setlocal
set HERE=%~dp0
set SHARED=%HERE%..\..\shared
set CLI="C:\Program Files\Arduino CLI\arduino-cli.exe"

copy /Y "%SHARED%\machine_scene.h" "%HERE%machine_scene.h" >nul

%CLI% compile -b teensy:avr:teensy41 "%HERE%."
if errorlevel 1 exit /b 1
%CLI% upload -b teensy:avr:teensy41 "%HERE%."

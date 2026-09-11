@echo off
REM Build and flash can_bus_teensy.
REM
REM Set NODE_ID in the .ino before each board: 0 is the master that runs the measurements, 1 and up
REM are workers that answer. Two boards is enough to measure anything; a CAN frame needs one other
REM node to acknowledge it, so a bus of one cannot even transmit.
setlocal
set HERE=%~dp0
set CLI="C:\Program Files\Arduino CLI\arduino-cli.exe"

%CLI% compile -b teensy:avr:teensy41 "%HERE%."
if errorlevel 1 exit /b 1
%CLI% upload -b teensy:avr:teensy41 "%HERE%."

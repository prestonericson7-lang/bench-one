@echo off
REM Build and flash unpack_teensy.
REM
REM The copy step is not optional. shared_kernels.cpp compiles the project's real kernels, and the
REM Teensy build recipe accepts no extra include path, so the originals are refreshed here on every
REM build. That keeps ..\..\shared as the single master and makes the local .c files disposable.
setlocal
set HERE=%~dp0
set SHARED=%HERE%..\..\shared
set CLI="C:\Program Files\Arduino CLI\arduino-cli.exe"

copy /Y "%SHARED%\gguf.h"       "%HERE%gguf.h"       >nul
copy /Y "%SHARED%\gguf_bits.c"  "%HERE%gguf_bits.c"  >nul
copy /Y "%SHARED%\gguf_dot.c"   "%HERE%gguf_dot.c"   >nul

%CLI% compile -b teensy:avr:teensy41 "%HERE%."
if errorlevel 1 exit /b 1
%CLI% upload -b teensy:avr:teensy41 "%HERE%."

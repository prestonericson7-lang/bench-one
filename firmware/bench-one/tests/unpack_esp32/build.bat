@echo off
REM Build and flash unpack_esp32.
REM
REM The copy step is not optional. The sketch must compile the project's REAL kernels, because a
REM benchmark of a duplicate measures a different machine. Arduino copies every source into a temp
REM directory before compiling, so the originals cannot be referenced in place; they are refreshed
REM here on every build, which keeps ..\..\shared as the single master.
REM
REM Set PORT to the board's COM port. PSRAM must be enabled for the second half of the test to run.
setlocal
set HERE=%~dp0
set SHARED=%HERE%..\..\shared
set CLI="C:\Program Files\Arduino CLI\arduino-cli.exe"
set FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashSize=8M
if "%1"=="" (set PORT=COM27) else (set PORT=%1)

copy /Y "%SHARED%\gguf.h"      "%HERE%gguf.h"      >nul
copy /Y "%SHARED%\gguf_bits.c" "%HERE%gguf_bits.c" >nul
copy /Y "%SHARED%\gguf_dot.c"  "%HERE%gguf_dot.c"  >nul

%CLI% compile -b %FQBN% "%HERE%."
if errorlevel 1 exit /b 1
%CLI% upload -b %FQBN% -p %PORT% "%HERE%."

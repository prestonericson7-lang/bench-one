@echo off
REM Build (and with "upload", flash) psram_llm.
REM The copy step is not optional: the Teensy build takes no include path, so the shared kernels and the
REM model core are copied from ..\..\shared on every build. .claude\verify-shared-copies.py checks them.
setlocal
set HERE=%~dp0
set SHARED=%HERE%..\..\shared
set CLI="C:\Program Files\Arduino CLI\arduino-cli.exe"
for %%F in (gguf.h gguf_bits.c gguf_dot.c tl_core.c tl_core.h tl_plat.h pretok_qwen2.h unicode_ln.h tl_math.h) do copy /Y "%SHARED%\%%F" "%HERE%%%F" >nul
REM psram_llm_board.cpp is GENERATED from psram_llm_board.inc (the file that gets edited): regenerate it too
python "%HERE%make_ino.py"
if errorlevel 1 exit /b 1
%CLI% compile -b teensy:avr:teensy41 "%HERE%."
if errorlevel 1 exit /b 1
if /I "%1"=="upload" %CLI% upload -b teensy:avr:teensy41 "%HERE%."

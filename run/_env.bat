@echo off
REM ===========================================================================
REM  _env.bat -- shared setup. Every other .bat calls this first.
REM
REM  Sets tool paths and makes a timestamped log file. Nothing here runs a test.
REM  Toolchains live in D:\espicpc\tools so they survive a temp folder cleanup;
REM  they were downloaded once and are free and portable.
REM ===========================================================================
set "ROOT=D:\espicpc"
set "TOOLS=%ROOT%\tools"
set "SRC=%ROOT%\firmware\bench-one"
set "LOGS=%ROOT%\run\logs"

set "OSSCAD=%TOOLS%\oss-cad-suite"
set "HOSTCC=%TOOLS%\w64devkit\bin"
set "ARMCC=%TOOLS%\arm-linux-gnueabihf\bin"
set "ADB=%TOOLS%\platform-tools\adb.exe"
set "ACLI=C:\Program Files\Arduino CLI\arduino-cli.exe"

if not exist "%LOGS%" mkdir "%LOGS%"
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
exit /b 0

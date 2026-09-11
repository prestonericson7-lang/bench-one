@echo off
REM ===========================================================================
REM  test-pipeline.bat -- a model split across two stages, and what the split costs
REM
REM  Runs both ends on this PC over loopback. That is the FLOOR: no real network
REM  beats it, so a slower figure on hardware is the wire and a matching one is
REM  the software.
REM
REM  The number that matters is the LINK SHARE. If compute dominates, adding
REM  nodes helps and the architecture scales. If the link dominates, it does not.
REM ===========================================================================
call "%~dp0_env.bat"
set "LOG=%LOGS%\pipeline-%STAMP%.txt"
set "PATH=%HOSTCC%;%PATH%"
cd /d "%SRC%\tests"

if not exist stage_node.exe (
  echo Building...
  gcc -O2 -std=gnu11 -I..\shared -o stage_node.exe stage_node.c ..\shared\stage_link.c -lws2_32 2>>"%LOG%"
)

echo Logging to %LOG%
echo   layers/node   per token   link share
for %%L in (1 4 16) do (
  start /b "" ".\stage_node.exe" tail 9501 127.0.0.1 9200 %%L 512 >> "%LOG%" 2>&1
  timeout /t 2 /nobreak >nul
  ".\stage_node.exe" head 127.0.0.1 9501 %%L 512 30 >> "%LOG%" 2>&1
  timeout /t 1 /nobreak >nul
)
findstr /C:"per token" /C:"compute" /C:"everything else" "%LOG%"
echo.
echo  Full log: %LOG%
pause

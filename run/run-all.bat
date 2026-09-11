@echo off
REM ===========================================================================
REM  run-all.bat -- everything that needs no hardware, then whatever is plugged in
REM
REM  Start here if you are not sure what to run.
REM ===========================================================================
call "%~dp0_env.bat"
echo.
echo  1. host tests      -- no hardware needed, always safe
echo  2. Teensy 4.1      -- needs a Teensy on USB
echo  3. ESP32-S3        -- needs an ESP32-S3 on USB
echo  4. Luckfox         -- needs a Luckfox booted on USB
echo  5. AI research     -- runs the local models on the task queue, hours
echo  6. AI status       -- what the research worker has produced
echo  7. MONITOR         -- live window, leave it open to watch progress
echo  8. JUDGE MUTANTS   -- apply the AI mutations, let the testbench decide
echo  9. PIPELINE        -- a model split across stages, and what the split costs
echo.
set /p C=Which? 
if "%C%"=="1" call "%~dp0test-host.bat"
if "%C%"=="2" call "%~dp0test-teensy.bat"
if "%C%"=="3" call "%~dp0test-esp32.bat"
if "%C%"=="4" call "%~dp0test-luckfox.bat"
if "%C%"=="5" call "%~dp0ai-research.bat"
if "%C%"=="6" call "%~dp0ai-status.bat"
if "%C%"=="7" call "%~dp0monitor.bat"
if "%C%"=="8" call "%~dp0judge-mutants.bat"
if "%C%"=="9" call "%~dp0test-pipeline.bat"

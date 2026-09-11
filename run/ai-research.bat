@echo off
REM ===========================================================================
REM  ai-research.bat -- put the local models to work, unattended, for hours
REM
REM  Reads task files from run\ai\queue, asks Ollama, writes answers into
REM  run\ai\findings. Safe to close and rerun: finished tasks move to run\ai\done
REM  so it picks up where it left off.
REM
REM  ONE REQUEST AT A TIME. It will not saturate the machine and you can keep
REM  using the PC while it runs. It stops by itself after 12 hours or when the
REM  queue is empty.
REM
REM  Everything it produces is a CANDIDATE to be checked, never a conclusion.
REM ===========================================================================
title Odysseus research worker
call "%~dp0_env.bat"

echo Checking Ollama...
curl -s -m 4 http://localhost:11434/api/tags >nul 2>&1
if not errorlevel 1 goto up
echo   not running, starting it
set "OLLAMA_MODELS=D:\start\ollama-models"
set "OLLAMA_HOST=0.0.0.0:11434"
if exist "%LOCALAPPDATA%\Programs\Ollama\ollama.exe" (
   start "" "%LOCALAPPDATA%\Programs\Ollama\ollama.exe" serve
) else (
   echo   Ollama not found. Run D:\start\START.bat first.
   pause & exit /b 1
)
echo   waiting for it...
for /l %%i in (1,1,20) do (
  timeout /t 3 /nobreak >nul
  curl -s -m 3 http://localhost:11434/api/tags >nul 2>&1 && goto up
)
echo   Ollama did not come up.
pause & exit /b 1
:up
echo   ok
call "%~dp0lowpri.bat"
echo.
python "%~dp0ai\worker.py"
echo.
echo Findings are in %~dp0ai\findings
pause

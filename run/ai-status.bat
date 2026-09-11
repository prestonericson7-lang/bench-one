@echo off
REM  Quick look at what the research worker has done so far.
call "%~dp0_env.bat"
echo.
echo  QUEUED:
dir /b "%~dp0ai\queue\*.task" 2>nul || echo    (none)
echo.
echo  DONE:
dir /b "%~dp0ai\done\*.task" 2>nul || echo    (none)
echo.
echo  FINDINGS:
dir /b /o-d "%~dp0ai\findings\*.md" 2>nul || echo    (none)
echo.
pause

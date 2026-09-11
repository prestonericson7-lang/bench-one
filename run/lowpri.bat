@echo off
REM  Drop Ollama below normal priority.
REM
REM  A 14B model keeps about a quarter of its weights on the CPU on this machine,
REM  so left at normal priority it competes with everything else: compiles, the
REM  editor, the PC simply being usable. Below normal means it gets the cycles
REM  nobody else wants, which for an overnight job is exactly right.
powershell -NoProfile -Command "Get-Process ollama* -ErrorAction SilentlyContinue | ForEach-Object { $_.PriorityClass = 'BelowNormal' }" >nul 2>&1
exit /b 0

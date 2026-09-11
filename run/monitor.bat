@echo off
REM  Opens the research monitor window. Leave it open and glance at it.
start "BENCH ONE monitor" powershell -NoProfile -ExecutionPolicy Bypass -NoExit -File "%~dp0monitor.ps1"

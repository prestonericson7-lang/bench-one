@echo off
REM ===========================================================================
REM  judge-mutants.bat -- apply the model's mutations, let the testbench decide
REM
REM  The model generates candidate edits. This applies each one and runs the
REM  real testbench against it. Nothing the model claims is trusted; the
REM  simulator does all the judging.
REM
REM  A SURVIVOR is the valuable result: the behaviour changed and every test
REM  still passed, so the test suite has a hole.
REM ===========================================================================
call "%~dp0_env.bat"
set "PATH=%OSSCAD%\bin;%OSSCAD%\lib;%PATH%"
set "LOG=%LOGS%\mutants-%STAMP%.txt"
cd /d "%SRC%\fpga\tb"

echo Logging to %LOG%

for %%F in ("%~dp0ai\findings\01-mutants-gemv-*.md") do (
  echo. & echo === gemv_int4 ===
  python "%~dp0ai\apply_mutants.py" "%%F" "..\rtl\gemv_int4.v" ^
    "iverilog -g2005 -o _m.vvp {MUT} tb_gemv_int4.v && vvp _m.vvp" >> "%LOG%" 2>&1
)
for %%F in ("%~dp0ai\findings\02-mutants-gemm-*.md") do (
  echo. & echo === gemm_int4 ===
  python "%~dp0ai\apply_mutants.py" "%%F" "..\rtl\gemm_int4.v" ^
    "iverilog -g2005 -o _m.vvp ..\rtl\gemv_int4.v {MUT} tb_gemm_int4.v && vvp _m.vvp" >> "%LOG%" 2>&1
)
del _m.vvp 2>nul

type "%LOG%" | findstr /C:"SURVIVED" /C:"caught " /C:"no survivors" /C:"BASELINE"
echo.
echo  Full detail: %LOG%
pause

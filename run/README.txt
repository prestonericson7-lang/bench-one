BENCH ONE -- how to run everything
==================================

Double-click run-all.bat and pick a number. That is the whole interface.

WHAT EACH ONE DOES
------------------
test-host.bat     Everything that needs no hardware. Always safe to run.
                  Verifies the natal frame, the superposition and self-direction
                  mechanism, the quantized matmul kernels, whether adding CPU
                  cores adds memory bandwidth, and the placement planner.

test-teensy.bat   Four measurements on a Teensy 4.1: memory bandwidth, PSRAM
                  verify and speed, the SD card as a weight store, and the
                  system properties. If the upload fails, PRESS THE BUTTON on
                  the Teensy and run it again. That is normal.

test-esp32.bat    Memory bandwidth and the compare kernel on an ESP32-S3, plus
                  how much the wifi radio interferes. Built for octal PSRAM; if
                  it reports 0 MB, edit the FQBN line and change PSRAM=opi to
                  PSRAM=enabled.

test-luckfox.bat  The measurement that decides the Luckfox's role: is it memory
                  bound or compute bound. Needs the board booted and visible
                  over ADB.

ai-research.bat   Puts the local Ollama models to work on the task queue. One
                  request at a time so the PC stays usable. Runs for up to 12
                  hours or until the queue empties, and can be closed and
                  restarted safely.

ai-status.bat     What the research worker has produced so far.

WHERE THINGS GO
---------------
run\logs\             every test run, timestamped, keep them
run\ai\queue\         research tasks waiting
run\ai\findings\      what the models produced -- CANDIDATES, not results
run\ai\done\          tasks already run

ADDING YOUR OWN RESEARCH TASK
-----------------------------
Drop a text file ending .task into run\ai\queue. Format:

    model: qwen2.5-coder:32b
    files: firmware/bench-one/fpga/rtl/gemv_int4.v
    ---
    What you want it to look at.

Both header lines are optional. Paths are relative to D:\espicpc.

ONE STANDING RULE
-----------------
Nothing in ai\findings is a result. Every real finding in this project came from
a measurement. The models produce suspicions worth checking, which is genuinely
useful and completely different from being right.

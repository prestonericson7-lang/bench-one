teensy/ -- Teensy 4.1 geometry engine firmware + x86 simulator (SPEC.md sections 3, 6, 13.3, 14)
====================================================================================================

Firmware (Windows, arduino-cli with the Teensy core 1.62.0):
  build_teensy.cmd            600 MHz  -> out\600\teensy_gpu.ino.hex (accel\gpu\teensy\out\600\)
  build_teensy.cmd 816        816 MHz overclock -> out\816\teensy_gpu.ino.hex
  (FQBN teensy:avr:teensy41:usb=serial,speed=600|816,opt=o2std,keys=en-us; arduino-cli build
  files in out\<speed>\build)
  flash_teensy.cmd [600|816] [COMx]   upload (arduino-cli / Teensy Loader)
  The build copies common/{gpu_proto.h,gpu_setup.[ch],geom.[ch]} into teensy_gpu/src/ first.
  Memory (both speeds): FLASH code 29004 + data 4040; RAM1 variables 275584 + code 26456
  (215936 free for the stack); RAM2 (DMAMEM mesh store) 290944 (233344 free).

Files
  teensy_gpu/teensy_gpu.ino   setup/loop; DTR rising edge -> tg_host_reset
  teensy_gpu/tg_core.c/.h     PORTABLE core, shared verbatim with the simulator: T_* message parser
                              and handlers, parallel-bus safety state machine, T_FRAME (+RETURN /
                              NO_BUS), T_RECORDS, autonomous mode (T_SCENE/T_AUTO), statistics
  teensy_gpu/tg_teensy.cpp    Teensy-only platform layer (tg_plat.h): USB CDC, GPIO6/GPIO9 bus
                              driver with DWT cycle-counter timing (30 ns setup / 60 ns hold), LED
  teensy_gpu/tg_plat.h        the platform interface; tg_geom_ext.h: streamed mesh upload API
                              (implemented in common/geom.c); tg_common.h: include paths
  sim/teensy_sim.c            x86 platform layer: TCP 7779 instead of USB, records to the daemon
                              simulator's Teensy bus 127.0.0.1:7778 (connect retried every 250 ms;
                              BUSY = not connected / >16 KB still queued in the kernel)
  sim/geom_ref.c              test helper: plain geom_frame / gpu_refrast calls on stdin/stdout
  sim/test_teensy_sim.py      simulator tests (python3 stdlib only)

Simulator + tests (WSL/Linux):
  cd teensy/sim
  make                 build/teensy_sim, build/geom_ref, build/test_geom
  make test            test_teensy_sim.py (+ end-to-end through ../../zynq/build/fpgagpud_sim if built)
  make test-geom       tests/test_geom.c
  make sanitize        simulator tests under ASan/UBSan
  make teensy-strict   firmware sources through the Teensy gcc with -Wall -Wextra -Werror and a
                       check for fused multiply-add in the arduino-cli objects (run the build first;
                       the Teensy platform ignores arduino-cli --warnings)
  ./build/teensy_sim [--port 7779] [--bind 127.0.0.1] [--bus-port 7778] [--cpu-mhz 600] [-v]
  Pi tools: --teensy tcp:127.0.0.1:7779

Behaviour notes (details in the comments at the top of tg_core.c)
  - Bus pins D/SOR/STROBE stay high-Z until BUSY read 0 continuously for 10 ms; BUSY = 1 for
    > 500 ms while a record waits (or while idle) -> pins high-Z again, bus_timeouts++, the frame
    reports GPU_ERR_BUS, re-arm. LED 13: 1 Hz blink = alive, bus high-Z; solid = bus driven.
  - T_FRAME with GEOM_FRAME_RETURN computes the frame twice (bus pass, then straight into USB), so
    no record buffer is needed; us_total covers the first pass only.
  - Autonomous mode starts frames only while the bus is driven (no FPGA -> the loop idles);
    frame_no counts completed autonomous frames since power-up (not reset by T_AUTO/T_RESET).
    Inside a frame only T_HELLO/T_STATS are executed; other messages wait for the frame end.
    T_FRAME / T_RECORDS while running -> GPU_ERR_ARG. Draws naming a mesh that is not stored are
    skipped. Overlay records must be TRI, SPRITE or NOP (else T_SCENE -> GPU_ERR_ARG).

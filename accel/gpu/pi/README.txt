pi/ -- Orange Pi 4 Pro host library (libfpgagpu) and tools for the FPGA-GPU (SPEC 11, 13.4, 14)
=====================================================================================================

Build (WSL/Linux on the PC; cross compilers from Ubuntu 22.04)
  make pi          static AArch64 binaries for the Orange Pi   -> build/aarch64/
  make x86         static x86-64 binaries (for the simulators) -> build/x86/
  make native      with the host's own cc (e.g. on the Orange Pi itself) -> build/native/
  make check       offline self-test (meshes, font, local geometry)
  All files: -O2 -Wall -Wextra -ffp-contract=off, zero warnings (WERROR=-Werror makes them fatal).
  fp-contract off matters: gpu_selftest predicts the Teensy's and the PL's output bit-exactly with
  common/geom.c, common/gpu_setup.c and common/gpu_refrast.c.

Install on the Orange Pi (copy pi/ over, then)
  sudo ./pi_setup.sh          wired port -> 10.77.0.1/24 (NetworkManager profile "fpgagpu", manual,
                              never-default, autoconnect; plain "ip addr add" without NM), your user
                              -> dialout, udev rule keeping ModemManager off the Teensy, tools ->
                              /usr/local/bin, ping the Zynq. Idempotent. --dry-run shows the commands.
  NOTE: with NetworkManager the wired port then belongs to the FPGA link (use Wi-Fi for the network).
  Car LAN: if the repo's "car-lan" profile (deploy/orangepi/install.sh: wired port 10.20.0.1/24, the
  Zynq is 10.20.0.2) exists, or the port already has 10.20.0.1, pi_setup.sh leaves the network alone:
  the daemon listens on every Zynq address and the tools find it at 10.20.0.2 by themselves.

Where the tools look for the Zynq daemon (TCP 7777)
  --fpga HOST[:PORT]          explicit (wins over everything)
  FPGAGPU_HOST=HOST[:PORT]    environment, when --fpga is not given
  default                     10.77.0.2 (direct cable, pi_setup.sh) and 10.20.0.2 (car LAN) are tried
                              at the same time; 10.77.0.2 wins unless it has not answered within
                              150 ms of 10.20.0.2 (so a car-LAN Pi pays 150 ms, not a 3 s timeout)
  -v prints the address that was used.

Tools (all: --fpga HOST[:PORT]  --port N  --teensy auto|/dev/ttyACMn|tcp:HOST:PORT|none
       --timeout MS  -v)
  gpu_selftest   bit-exact end-to-end test: PS path (TRIS incl. guard-band clipping, RECT, sprites,
                 RECORDS, list overflow), FRAME_GET at scale 1 and 2, Teensy T_RECORDS path, Teensy
                 geometry (T_MESH/T_FRAME == local geom.c), GEOM_FRAME_RETURN (+NO_BUS), autonomous
                 mode (static frame bit-exact incl. overlays, frames produced, T_FRAME refused while
                 running, animation, stop), throughput (frames/s, tris/s, Teensy us/frame, return
                 path frames/s at both scales), health counters. PASS/FAIL/SKIP per test, exit 0 only
                 if nothing failed; mismatches -> DIR/selftest_<test>_{expected,got,diff}.ppm
                 (--dump DIR). --quick, --seed N, --frames N, --return-seconds S, --offline.
                 --teensy defaults to auto (none found -> Teensy tests SKIPped, said loudly).
  gpu_demo       lit torus + cube + sphere, HUD sprite with fps/frame/tris. Default: the Teensy does
                 the geometry (T_FRAME), the Pi draws the HUD (PS path). --teensy none: the Pi
                 transforms and sends NET_TRIS. --auto [--max-fps N]: T_SCENE + T_AUTO, the Teensy
                 renders on its own, the Pi only re-uploads the HUD sprite. --view [--scale 2]: the
                 returned frames (FRAME_GET) in an X11 window. --frames N, --seconds S, Ctrl-C.
  gpu_view       observer: FRAME_GET loop -> X11 window (raw X11 protocol, viewer/x11view.c), fps in
                 the title. --scale 1|2, --seconds S, --frames N. Close button / Esc / q to quit.
  gpu_stat       observer: HELLO + all PL registers decoded + daemon counters; --watch [MS] shows
                 rates; --teensy auto adds T_HELLO/T_STATS (default none: opening the Teensy's tty
                 would disturb an application using it).
  gpu_snap       observer: READBACK (front buffer) or --frame-get [--scale 1|2] (next rendered
                 frame) -> -o FILE.png | FILE.ppm
  gpu_image      PPM (P6/P3) -> 1280x720 (--mode fit|fill|center, --bg RRGGBB, ordered dither unless
                 --no-dither) -> one full-screen sprite frame; stays on screen after exit.

Library (libfpgagpu.h; build/<arch>/libfpgagpu.a also contains the helpers below)
  gpu_connect(host, port, observer, ...)   TCP (TCP_NODELAY, keepalive, connect/reply timeouts) +
                                           NET_HELLO as controller or observer; host NULL/"" ->
                                           $FPGAGPU_HOST, else 10.77.0.2 then 10.20.0.2
  gpu_set_config / gpu_tris / gpu_rect / gpu_sprite_upload / gpu_sprite_draw / gpu_records /
  gpu_end_frame / gpu_wait_frame / gpu_status / gpu_readback / gpu_reset / gpu_sync /
  gpu_frame_get (+ _send/_recv split for event loops) / gpu_wait_frame_no (helper)
  teensy_open("auto" | "/dev/..." | "tcp:host:port" | "none")   raw termios, TIOCEXCL, T_HELLO
                                           handshake (one retry across the DTR-reset edge)
  teensy_hello / teensy_wait_ready / teensy_mesh / teensy_frame (+ _send/_recv; GEOM_FRAME_RETURN
  records) / teensy_records / teensy_stats / teensy_bus_mode / teensy_reset / teensy_scene /
  teensy_auto
  gpu_math.[ch]  column-major mat4 (m4_mul in the firmware's summation order, m4_rotate = the
                 firmware's Rodrigues matrix), perspective, look_at
  mesh.[ch]      cube (per-face colours), UV sphere, torus: CCW front faces, outward unit normals;
                 mesh_check() verifies both
  font8x8.[ch]   public-domain 8x8 font (font8x8_basic) -> RGB565 text (scaled, outlined)
  img.[ch]       RGB565 -> PPM / PNG (own deflate, no zlib), PPM reader
  Errors: 0, GPU_ERR_* (peer status) or FGPU_ERR_* (local); after FGPU_ERR_IO/LTIMEOUT reconnect.

Frame limits worth knowing (SPEC 7): the PL keeps at most 1536 TRI+SPRITE records per frame; more
are dropped (LIST_OVERFLOW). gpu_demo's meshes are sized to stay near 1050 visible triangles.

Simulation (WSL): bash pi/sim_e2e.sh   (builds everything, starts zynq/build/fpgagpud_sim and
teensy/sim/build/teensy_sim on free ports, runs every tool, writes PNGs to pi/build/preview/; as
root it also checks the default address choice in a network namespace, and pi_setup.sh --dry-run
next to a car-lan profile with a stub nmcli)

zynq/ -- Zynq PS side of the FPGA-GPU: fpgagpud daemon, simulator, tests, device tree, SD image
=================================================================================================
(SPEC sections 10, 12, 13.2. Run everything in WSL Ubuntu-22.04; image/qemu targets as root.)

Target: the repo's PZ7020-StarLite platform (1 GB DDR3, 32-bit; Linux 6.12 armhf Debian image
built by hardware/pz7020-starlite/linux/*.sh). The GPU's PL is merged into the platform bitstream:
GPU registers at 0x43C00000 (GP0), the GPU's reserved DDR window 0x1E000000 + 32 MB, the platform's
pl_regs at 0x40000000 (ID 0x5A702001), the matrix engine at 0x20000000-0x37FFFFFF and its AXI DMA at
0x40400000. The platform image installs build/fpgagpud and fpgagpud.service (below).

Files
  fpgagpud.c            the daemon: TCP 7777 on every address, <= 4 clients (controller +
                        observers), poll loop, record pushing with PS_FIFO_FREE flow control, sprite
                        pool, WAIT_FRAME / FRAME_GET parked and completed from the loop (1 ms polling)
  fpgagpud.service      systemd unit for the platform image (-> /etc/systemd/system/, enabled;
                        binary -> /usr/local/bin/fpgagpud; optional /etc/default/fpgagpud with
                        FPGAGPUD_OPTS). Its header lists what the image must provide.
  backend.h             PL interface used by the daemon (registers, PS FIFO push, DDR window)
  backend_hw.c          real PL through /dev/mem (O_SYNC): PL-configured check, level shifters,
                        reserved-window check (/proc/iomem), GP0 ID reads in a forked child with a
                        1 s timeout, then regs + DDR maps
  backend_sim.c         software PL for --sim: FIFOs + collector (SPEC 7, mirrors
                        rtl/core_collector.v), gpu_refrast rendering, return capture (13.1),
                        simulated Teensy bus on TCP 7778 (raw 96-byte records, BUSY modelled)
  os.h, os_posix.c      OS layer (glibc: x86 simulator build)
  zrt/                  freestanding ARM EABI runtime for the static Zynq binary (no armhf libc
                        needed on the build host; raw syscalls, own printf/malloc/llrint)
  test_sim.py, test_ref.c  end-to-end tests (python3 stdlib only) + independent reference renderer
  Legacy (the original standalone 512 MB SD image; superseded by the platform image, not used for
  the 1 GB board and not run here): dts/ (board dts + reserved-memory, build_dtb.sh), boot/boot.cmd,
  rootfs/, make_sd_image.sh, qemu_boot_test.sh (make dtb / image / qemu-test). boot/boot.cmd still
  shows the two boot-script lines the platform needs (see "Boot flow").

Build and test (outputs in build/)
  make WERROR=-Werror sim arm      x86 simulator build + static ARMv7 build, zero warnings
                                   build/fpgagpud      static ARMv7-A hard-float (the board)
                                   build/fpgagpud_sim  x86-64 (run with --sim), build/test_ref
  make test                        test_sim.py against fpgagpud --sim
  make test-arm                    the same tests against the ARM binary under qemu-arm

Run the simulator by hand
  build/fpgagpud_sim --sim [-v]    Pi protocol on 7777, simulated Teensy bus on 7778

What the daemon checks before it touches the PL (backend_hw.c; each failure is logged and the
daemon answers GPU_ERR_NOPL, re-checking every 2 s)
  1. A bitstream is loaded: devcfg INT_STS PCFG_DONE, or "fpgagpu.pl_loaded=1" on the kernel command
     line with INT_STS PCFG_INIT_NE (PL reset since boot) clear. Linux's zynq-fpga driver writes
     all-ones to the write-1-to-clear INT_STS when it probes (drivers/fpga/zynq-fpga.c, checked in
     the platform's 6.12 tree; CONFIG_FPGA_MGR_ZYNQ_FPGA=y there), so PCFG_DONE reads 0 under Linux
     for a bitstream U-Boot loaded: the platform's boot.scr must append the marker after a
     successful "fpga loadb" (boot/boot.cmd shows the lines). A later PL reset is "PL lost".
  2. SLCR LVL_SHFTR_EN = 0xF (else every GP0 access hangs).
  3. The DDR window 0x1E000000..0x1FFFFFFF is not Linux RAM: no "System RAM" range in /proc/iomem
     may overlap it. With 1 GB the window is in the middle of RAM, so the platform device tree must
     carry reserved-memory gpu@1e000000 { reg = <0x1e000000 0x2000000>; no-map; } and Linux must
     have accepted it (it refuses a no-map node that overlaps an earlier reservation, e.g. a device
     tree U-Boot relocated into the window -- boot.scr's fdt_high prevents that).
  4. GP0, read in a forked child (1 s timeout, so a decode error or a hang costs only the child):
     0x40000000 (on the platform: pl_regs, ID 0x5A702001; logged) then ID 0x47505531 ('GPU1') and
     VERSION major 1 at 0x43C00000. A platform bitstream without the GPU answers 0x43C00000 with a
     decode error or with pl_regs' ID (open-toolchain pz7020_ps7_top aliases it): refused.
  Only then are the registers written and the 32 MB window mapped (O_SYNC, 64-bit accesses).

Network
  The daemon listens on 0.0.0.0:7777: every eth0 address works -- the platform's 10.20.0.2/24
  (car LAN, systemd-networkd), a DHCP lease, and 10.77.0.2 if the image adds it. The Pi tools try
  10.77.0.2 and 10.20.0.2 (pi/README.txt); FPGAGPU_HOST or --fpga name any other address.

Service
  fpgagpud.service (installed by the platform image), options in /etc/default/fpgagpud.
  journalctl -u fpgagpud shows why the PL is or is not used.

Screen colours: colour bars = bitstream loaded but daemon not running; dark blue (0x0010) =
daemon running; anything else = an app.

Security note: TCP 7777 is unauthenticated (SPEC). The daemon only writes DDR inside its own
window; raw SPRITE records in NET_RECORDS whose pixel reads would leave the window are dropped.

On the real board, first checks
  journalctl -u fpgagpud -b      expect "PL ready: FPGA-GPU ID 0x47505531 ... reserved ..."
  cat /proc/cmdline              expect fpgagpu.pl_loaded=1 (U-Boot loaded pl.bit)
  grep "System RAM" /proc/iomem  expect no range covering 1e000000-1fffffff
  dmesg | grep "reserved mem"    expect 0x1e000000..0x1fffffff nomap gpu@1e000000

tests/ -- shared C tests, golden model checks, RTL scene generator
====================================================================

Run on Linux / WSL (Ubuntu 22.04: gcc, make, python3; optional cross gcc + qemu-user):

  make              build build/test_common and build/gen_scene (x86-64, -O2 -Wall -Wextra -ffp-contract=off)
  make test         unit tests for common/gpu_setup.c + common/gpu_refrast.c (exit 0 = pass)
  make scenes       write all scenes to build/scenes/<name>/ and re-verify the files
                    (make scenes SCENES=../sim/scenes: where the RTL testbench runs used to read them;
                    sim/run_core.sh takes the scene directories as arguments)
  make preview      scenes + PNG previews of expected.ppm in build/preview/
  make cross        -Werror -pedantic compiles for AArch64, ARMv7, Zynq Cortex-A9 and (if the
                    Teensy core is installed, TEENSY_GCC=...) Cortex-M7; checks that no fused
                    multiply-add is generated (with a positive control)
  make crosstest    static AArch64 (and ARMv7 if armhf libc headers exist) builds run under qemu:
                    record/frame hashes and every scene file must equal the x86-64 ones
  make sanitize     tests + generator under ASan/UBSan
  make check        all of the above;  make clean  removes build/ (every output is under build/)

Files
  test_common.c   unit tests: watertightness (jittered quad grids, fans, huge guard-band-clipped
                  meshes: every pixel centre covered exactly once), top-left rule, culling,
                  degenerate/guard-band edges/NaN, Z semantics, Gouraud endpoints, rects (all
                  65536 colours), sprites, int32 ranges + attribute accuracy on ~30000 random
                  triangles, gpu_refrast_frame vs a literal full-frame SPEC 5 transcription.
  gen_scene.c     gen_scene <scene> <dir> | all <root> | verify <dir> | verify-all <root> | list
                  scenes: one_tri rects zbuf gouraud sprites strips mixed overflow empty
                  (format: see the comment at the top of gen_scene.c; 'verify' re-reads a scene
                  directory the way the RTL testbench does and re-renders it)
  ppm2png.py      PPM (P6/P3) -> PNG, python3 standard library only

Scene directory (per scene): rec.hex, ddr.hex, cfg.txt, expected.hex, expected.ppm.
  rec.hex has nrec records + END(frame 1); the PL keeps nrender = min(nrec, 1536) records.
  ddr.hex holds only sprite source words ($readmemh, word index = (addr - 0x1E000000) / 8);
  a scene without sprites gets a single zero word at the pool base (0x80000) so the file is
  never empty. expected.hex = 230400 64-bit beats, pixel 4k in bits [15:0].

Output is deterministic and platform independent (integer PRNG, no libm transcendental
functions, every PRNG call in its own statement because C leaves argument evaluation order
unspecified).

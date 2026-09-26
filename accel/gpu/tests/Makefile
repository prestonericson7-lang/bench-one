# tests/Makefile -- shared C (gpu_setup.c, gpu_refrast.c) tests + RTL scene generator.
# Run on Linux / WSL from this directory:
#   make            build build/test_common and build/gen_scene (x86-64)
#   make test       run the unit tests
#   make scenes     write every scene to build/scenes/<name>/ (SCENES=DIR for another place,
#                   e.g. the RTL testbench: sim/run_core.sh takes scene directories as arguments)
#   make preview    PNG previews of a few scenes into build/preview/
#   make cross      compile the shared C for AArch64 + ARMv7 with -Werror, check for fused mul-add
#   make crosstest  build static AArch64 + ARMv7 binaries, run them under qemu-user and compare
#                   the record hash and every generated scene file with the x86-64 results
#   make sanitize   run the tests and the generator under ASan + UBSan
#   make check      all of the above

CC       = gcc
CFLAGS   = -std=c99 -O2 -Wall -Wextra -ffp-contract=off -I../common
LDLIBS   = -lm
COMMON   = ../common/gpu_setup.c ../common/gpu_refrast.c
HDRS     = ../common/gpu_setup.h ../common/gpu_refrast.h ../common/gpu_proto.h
BUILD    = build
SCENES   = $(BUILD)/scenes
PREVIEW  = one_tri rects zbuf gouraud sprites strips mixed overflow
XCFLAGS  = -std=c99 -pedantic -O2 -Wall -Wextra -Werror -ffp-contract=off -I../common
TC       = $(BUILD)/test_common
GS       = $(BUILD)/gen_scene

.PHONY: all test scenes preview cross crosstest sanitize check clean

all: $(TC) $(GS)

$(TC): test_common.c $(COMMON) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_common.c $(COMMON) $(LDLIBS)

$(GS): gen_scene.c $(COMMON) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ gen_scene.c $(COMMON) $(LDLIBS)

test: $(TC)
	$(TC)

scenes: $(GS)
	$(GS) all $(SCENES)
	$(GS) verify-all $(SCENES)

preview: scenes
	mkdir -p $(BUILD)/preview
	for s in $(PREVIEW); do python3 ppm2png.py $(SCENES)/$$s/expected.ppm $(BUILD)/preview/$$s.png || exit 1; done

# Portability: the shared C must compile warning-free as C99 for the Zynq (ARMv7-A hard-float),
# AArch64 (Orange Pi) and the Teensy (Cortex-M7, FPv5-D16). No fused multiply-add may appear in
# gpu_setup (bit-exact contract).
#  - arm-linux-gnueabihf-gcc: if its sysroot has no libc headers (libc6-dev-armhf-cross not
#    installed), gpu_setup.c/gpu_refrast.c are compiled -ffreestanding against a 2-header shim
#    (llrint/isfinite/memcpy/memset declarations only) -- still the real ARMv7 code generator.
#  - TEENSY_GCC (Teensy core's arm-none-eabi-gcc, real newlib headers): Cortex-M7 exactly as the
#    Teensy build, and Cortex-A9. Skipped if not present.
#  - the FMA check is proven by a positive control (pragma stripped + -ffp-contract=fast must
#    produce fused ops), and the pragma alone is checked under -std=gnu11 (GCC's default there is
#    -ffp-contract=fast, as in Arduino builds).
TEENSY_GCC ?= /mnt/c/Users/Danie/AppData/Local/Arduino15/packages/teensy/tools/teensy-compile/15.2.1/arm/bin/arm-none-eabi-gcc.exe
FMA_A64   = '\s(fn?madd|fn?msub)\s'
FMA_ARM   = '\svfn?m[as]'
X         = $(BUILD)/cross

cross:
	mkdir -p $(X)/shim
	printf '#ifndef SHIM_MATH_H\n#define SHIM_MATH_H\nlong long llrint(double x);\n#define isfinite(x) __builtin_isfinite(x)\n#endif\n' > $(X)/shim/math.h
	printf '#ifndef SHIM_STRING_H\n#define SHIM_STRING_H\n#include <stddef.h>\nvoid *memcpy(void *restrict d, const void *restrict s, size_t n);\nvoid *memset(void *d, int c, size_t n);\n#endif\n' > $(X)/shim/string.h
	if echo '#include <math.h>' | arm-linux-gnueabihf-gcc -E -x c - > /dev/null 2>&1; then \
	  echo "armv7: using the toolchain's libc headers"; ARMINC=""; \
	else echo "armv7: no armhf libc headers installed -> -ffreestanding + shim"; ARMINC="-ffreestanding -I$(X)/shim"; fi; \
	for f in gpu_setup gpu_refrast; do \
	  aarch64-linux-gnu-gcc $(XCFLAGS) -c ../common/$$f.c -o $(X)/$$f.aarch64.o || exit 1; \
	  arm-linux-gnueabihf-gcc $(XCFLAGS) $$ARMINC -c ../common/$$f.c -o $(X)/$$f.armv7.o || exit 1; \
	  arm-linux-gnueabihf-gcc $(XCFLAGS) $$ARMINC -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -marm \
	      -c ../common/$$f.c -o $(X)/$$f.zynq.o || exit 1; \
	done; \
	aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I../common -c ../common/gpu_setup.c \
	    -o $(X)/gpu_setup.aarch64-gnu11-pragma-only.o || exit 1; \
	sed '/pragma GCC optimize/d' ../common/gpu_setup.c > $(X)/gpu_setup_nopragma.c; \
	aarch64-linux-gnu-gcc -std=gnu11 -O2 -ffp-contract=fast -I../common -c $(X)/gpu_setup_nopragma.c \
	    -o $(X)/control.aarch64.o || exit 1; \
	arm-linux-gnueabihf-gcc -std=gnu11 -O2 -ffp-contract=fast $$ARMINC -I../common -mfpu=vfpv4 \
	    -c $(X)/gpu_setup_nopragma.c -o $(X)/control.armv7.o || exit 1
	@n=`aarch64-linux-gnu-objdump -d $(X)/control.aarch64.o | grep -Ec $(FMA_A64)`; \
	 m=`arm-linux-gnueabihf-objdump -d $(X)/control.armv7.o | grep -Ec $(FMA_ARM)`; \
	 echo "positive control (no pragma, -ffp-contract=fast): $$n aarch64 / $$m armv7 fused ops"; \
	 test $$n -gt 0 && test $$m -gt 0 || { echo "FMA detector does not work"; exit 1; }
	@if aarch64-linux-gnu-objdump -d $(X)/gpu_setup.aarch64.o $(X)/gpu_setup.aarch64-gnu11-pragma-only.o \
	  | grep -E $(FMA_A64); then echo "FUSED MULTIPLY-ADD FOUND (aarch64)"; exit 1; fi
	@if arm-linux-gnueabihf-objdump -d $(X)/gpu_setup.armv7.o $(X)/gpu_setup.zynq.o \
	  | grep -E $(FMA_ARM); then echo "FUSED MULTIPLY-ADD FOUND (armv7)"; exit 1; fi
	@if [ -x "$(TEENSY_GCC)" ]; then \
	  TF=""; \
	  case "$(TEENSY_GCC)" in *.exe) if command -v wslpath > /dev/null; then \
	    W=`wslpath -m "$(dir $(TEENSY_GCC)).."`; V=`"$(TEENSY_GCC)" -dumpversion | tr -d '\r'`; \
	    TF="-B$$W/libexec/gcc/arm-none-eabi/$$V/ -B$$W/arm-none-eabi/bin/ -isystem $$W/lib/gcc/arm-none-eabi/$$V/include \
	        -isystem $$W/lib/gcc/arm-none-eabi/$$V/include-fixed -isystem $$W/arm-none-eabi/include"; fi;; esac; \
	  for f in gpu_setup gpu_refrast; do \
	    "$(TEENSY_GCC)" $$TF $(XCFLAGS) -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16 \
	        -c ../common/$$f.c -o $(X)/$$f.teensy41.o || exit 1; \
	    "$(TEENSY_GCC)" $$TF $(XCFLAGS) -mcpu=cortex-a9 -marm -mfloat-abi=hard -mfpu=vfpv3 \
	        -c ../common/$$f.c -o $(X)/$$f.cortexa9-newlib.o || exit 1; \
	  done; \
	  "$(TEENSY_GCC)" $$TF -std=gnu11 -O2 -Wall -Wextra -Werror -I../common -mcpu=cortex-m7 -mthumb \
	      -mfloat-abi=hard -mfpu=fpv5-d16 -c ../common/gpu_setup.c -o $(X)/gpu_setup.teensy41-gnu11-pragma-only.o || exit 1; \
	  "$(TEENSY_GCC)" $$TF -std=gnu11 -O2 -ffp-contract=fast -I../common -mcpu=cortex-m7 -mthumb \
	      -mfloat-abi=hard -mfpu=fpv5-d16 -c $(X)/gpu_setup_nopragma.c -o $(X)/control.teensy41.o || exit 1; \
	  n=`arm-linux-gnueabihf-objdump -d $(X)/control.teensy41.o | grep -Ec $(FMA_ARM)`; \
	  echo "positive control teensy41: $$n fused ops"; test $$n -gt 0 || exit 1; \
	  if arm-linux-gnueabihf-objdump -d $(X)/gpu_setup.teensy41.o $(X)/gpu_setup.teensy41-gnu11-pragma-only.o \
	      $(X)/gpu_setup.cortexa9-newlib.o | grep -E $(FMA_ARM); then echo "FUSED MULTIPLY-ADD FOUND (teensy)"; exit 1; fi; \
	  echo "teensy gcc (`"$(TEENSY_GCC)" -dumpversion`): cortex-m7 + cortex-a9 objects OK"; \
	else echo "TEENSY_GCC not found: Cortex-M7 check skipped"; fi
	@echo "cross: aarch64 + armv7 + zynq(cortex-a9) objects OK (-Werror -pedantic), no fused multiply-add"

# Runtime bit-exactness: the same tests/generator built for other ISAs (static, run under
# qemu-user) must print the same record hash and write byte-identical scene files.
# ARMv7 Linux needs libc6-dev-armhf-cross; without it that leg is skipped (reported).
XT = $(BUILD)/xt
crosstest: $(TC) $(GS)
	mkdir -p $(XT)
	$(TC) > $(XT)/x86.txt
	rm -rf $(XT)/scenes-x86
	$(GS) all $(XT)/scenes-x86 > /dev/null
	@legs=0; for a in aarch64:aarch64-linux-gnu-gcc:qemu-aarch64-static armv7:arm-linux-gnueabihf-gcc:qemu-arm-static; do \
	  n=$${a%%:*}; r=$${a#*:}; cc=$${r%%:*}; q=$${r#*:}; \
	  if ! echo '#include <stdio.h>' | $$cc -E -x c - > /dev/null 2>&1; then \
	    echo "crosstest: $$n SKIPPED (no libc headers for $$cc)"; continue; fi; \
	  $$cc $(CFLAGS) -static -o $(XT)/test_common.$$n test_common.c $(COMMON) $(LDLIBS) || exit 1; \
	  $$cc $(CFLAGS) -static -o $(XT)/gen_scene.$$n gen_scene.c $(COMMON) $(LDLIBS) || exit 1; \
	  $$q $(XT)/test_common.$$n > $(XT)/$$n.txt || { echo "$$n tests FAILED"; exit 1; }; \
	  h1=`grep 'record hash' $(XT)/x86.txt`; h2=`grep 'record hash' $(XT)/$$n.txt`; \
	  echo "x86-64 : $$h1"; echo "$$n: $$h2"; tail -n 1 $(XT)/$$n.txt; \
	  test "$$h1" = "$$h2" || { echo "RECORD HASH DIFFERS ($$n)"; exit 1; }; \
	  rm -rf $(XT)/scenes-$$n; $$q $(XT)/gen_scene.$$n all $(XT)/scenes-$$n > /dev/null || exit 1; \
	  diff -r -q $(XT)/scenes-x86 $(XT)/scenes-$$n || { echo "SCENES DIFFER ($$n)"; exit 1; }; \
	  echo "crosstest: $$n scenes byte-identical to x86-64"; rm -rf $(XT)/scenes-$$n; legs=$$((legs+1)); \
	done; rm -rf $(XT)/scenes-x86; test $$legs -gt 0

sanitize:
	mkdir -p $(BUILD)/san
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $(BUILD)/san/test_common test_common.c $(COMMON) $(LDLIBS)
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $(BUILD)/san/gen_scene gen_scene.c $(COMMON) $(LDLIBS)
	$(BUILD)/san/test_common > $(BUILD)/san/test.txt 2>&1; st=$$?; tail -n 3 $(BUILD)/san/test.txt; exit $$st
	rm -rf $(BUILD)/san/scenes
	$(BUILD)/san/gen_scene all $(BUILD)/san/scenes > /dev/null
	rm -rf $(BUILD)/san/scenes
	@echo "sanitize: no ASan/UBSan reports"

check: test cross crosstest sanitize scenes

clean:
	rm -rf $(BUILD)

# zynq/Makefile -- fpgagpud (Zynq PS daemon) + simulator + tests + SD image.  Run in WSL/Linux.
#
#   make arm        build/fpgagpud      static ARMv7-A hard-float binary for the Zynq (Debian 12 armhf)
#   make sim        build/fpgagpud_sim  x86-64 build (run with --sim) + build/test_ref (test helper)
#   make test       x86 simulator end-to-end tests (test_sim.py)
#   make test-arm   the same tests against the ARM binary under qemu-arm (binfmt) -- checks the
#                   freestanding runtime, syscalls and the ARM floating-point setup path
#   Legacy (the original standalone 512 MB SD image; the repo's platform image built by
#   hardware/pz7020-starlite/linux/*.sh replaces it and installs build/fpgagpud + fpgagpud.service):
#   make dtb        dts/build_dtb.sh: board dtb + reserved-memory -> build/dts/ (+ verification)
#   make image      make_sd_image.sh (as root): /root/zynq/gpu-out/pz7020-gpu-sd.img[.xz]
#   make qemu-test  qemu_boot_test.sh (as root): boots a copy of that image in QEMU and checks
#                   reserved memory, /dev/mem, network (with and without DHCP) and the service
#   make WERROR=-Werror ...   treat warnings as errors
#
# The ARM toolchain on the build host has no armhf C library, so by default the ARM binary uses
# zrt/zrt.c (freestanding runtime on raw Linux EABI syscalls). With libc6-dev-armhf-cross
# installed, "make arm ARM_LIBC=glibc" links statically against glibc instead (os_posix.c).

CC        ?= gcc
CROSS     ?= arm-linux-gnueabihf-
ARM_CC    := $(CROSS)gcc
ARM_LIBC  ?= zrt
WERROR    ?=
PYTHON    ?= python3
COMMON    := ../common
BUILD     := build

SRC_D     := fpgagpud.c backend_hw.c backend_sim.c
SRC_C     := $(COMMON)/gpu_setup.c $(COMMON)/gpu_refrast.c
HDRS      := os.h backend.h $(COMMON)/gpu_proto.h $(COMMON)/gpu_setup.h $(COMMON)/gpu_refrast.h

CSTD      := -std=gnu99
WARN      := -Wall -Wextra $(WERROR)
SIM_CFLAGS := $(CSTD) -O2 $(WARN) -ffp-contract=off -I. -I$(COMMON)

ARM_BASE  := $(CSTD) -O2 $(WARN) -ffp-contract=off -marm -march=armv7-a -mfpu=vfpv3-d16 \
             -mfloat-abi=hard -ffunction-sections -fdata-sections -I. -I$(COMMON)
ifeq ($(ARM_LIBC),zrt)
ARM_GCCINC := $(shell $(ARM_CC) -print-file-name=include 2>/dev/null)
ARM_CFLAGS := $(ARM_BASE) -nostdinc -isystem $(ARM_GCCINC) -Izrt/include -ffreestanding \
             -fno-stack-protector -fno-pie -fno-tree-loop-distribute-patterns -fno-unwind-tables \
             -fno-asynchronous-unwind-tables
ARM_LDFLAGS := -static -nostdlib -no-pie -Wl,--gc-sections -Wl,-z,noexecstack
ARM_LIBS  := -lgcc
ARM_OS    := zrt/zrt.c
ARM_DEPS  := zrt/zrt.c zrt/include/string.h zrt/include/math.h
else
ARM_CFLAGS := $(ARM_BASE)
ARM_LDFLAGS := -static -Wl,--gc-sections
ARM_LIBS  := -lm
ARM_OS    := os_posix.c
ARM_DEPS  := os_posix.c
endif

.PHONY: all arm sim test test-arm dtb image qemu-test clean

all: sim arm

arm: $(BUILD)/fpgagpud
sim: $(BUILD)/fpgagpud_sim $(BUILD)/test_ref

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/fpgagpud: $(SRC_D) $(SRC_C) $(HDRS) $(ARM_DEPS) Makefile | $(BUILD)
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $(SRC_D) $(ARM_OS) $(SRC_C) $(ARM_LDFLAGS) $(ARM_LIBS)

$(BUILD)/fpgagpud_sim: $(SRC_D) os_posix.c $(SRC_C) $(HDRS) Makefile | $(BUILD)
	$(CC) $(SIM_CFLAGS) -o $@ $(SRC_D) os_posix.c $(SRC_C) -lm

$(BUILD)/test_ref: test_ref.c $(SRC_C) $(HDRS) Makefile | $(BUILD)
	$(CC) $(SIM_CFLAGS) -o $@ test_ref.c $(SRC_C) -lm

test: sim
	$(PYTHON) test_sim.py --daemon $(BUILD)/fpgagpud_sim --ref $(BUILD)/test_ref

test-arm: arm sim
	$(PYTHON) test_sim.py --daemon $(BUILD)/fpgagpud --ref $(BUILD)/test_ref --slow

dtb:
	bash dts/build_dtb.sh

image: arm dtb
	bash make_sd_image.sh

qemu-test:
	bash qemu_boot_test.sh

clean:
	rm -rf $(BUILD)

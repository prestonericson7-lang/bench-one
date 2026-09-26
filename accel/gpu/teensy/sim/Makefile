# teensy/sim/Makefile -- x86 Teensy simulator + tests.  Run in WSL/Linux (gcc, make, python3).
#
#   make              build/teensy_sim  (the firmware core tg_core.c + geom.c + gpu_setup.c on a TCP
#                     platform layer), build/geom_ref (test helper), build/test_geom
#   make test         test_teensy_sim.py against build/teensy_sim (+ an end-to-end part through the
#                     daemon simulator if ../../zynq/build/fpgagpud_sim exists)
#   make test-geom    tests/test_geom.c (common/geom.c unit tests)
#   make sanitize     teensy_sim + geom_ref under ASan/UBSan, then the same simulator tests
#   make teensy-strict  our firmware sources through the Teensy gcc with -Wall -Wextra -Werror, and
#                     no fused multiply-add in the objects arduino-cli built (run build_teensy.cmd
#                     first; needs WSL interop for the Windows Teensy compiler)
#   make check        all of the above;  make clean
#   make WERROR=      build without -Werror

CC       ?= gcc
PYTHON   ?= python3
COMMON   := ../../common
FW       := ../teensy_gpu
BUILD    := build
WERROR   ?= -Werror
CFLAGS   := -std=gnu99 -O2 -g -Wall -Wextra $(WERROR) -ffp-contract=off -I$(FW) -I$(COMMON)
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
DAEMON   ?= ../../zynq/build/fpgagpud_sim

SIM_SRC  := teensy_sim.c $(FW)/tg_core.c $(COMMON)/geom.c $(COMMON)/gpu_setup.c
SIM_HDR  := $(FW)/tg_common.h $(FW)/tg_core.h $(FW)/tg_plat.h $(FW)/tg_geom_ext.h \
            $(COMMON)/gpu_proto.h $(COMMON)/gpu_setup.h $(COMMON)/geom.h
REF_SRC  := geom_ref.c $(COMMON)/geom.c $(COMMON)/gpu_setup.c $(COMMON)/gpu_refrast.c

# Teensy toolchain: the Windows exe, run through cmd.exe (WSL interop) so that it finds its own
# cc1/cc1plus; called from inside ../teensy_gpu with relative paths.
TEENSY_BIN_W ?= C:/Users/Danie/AppData/Local/Arduino15/packages/teensy/tools/teensy-compile/15.2.1/arm/bin
TEENSY_GCC   ?= cmd.exe /c $(TEENSY_BIN_W)/arm-none-eabi-gcc.exe
TEENSY_GXX   ?= cmd.exe /c $(TEENSY_BIN_W)/arm-none-eabi-g++.exe
TEENSY_CORE_W ?= C:/Users/Danie/AppData/Local/Arduino15/packages/teensy/hardware/avr/1.62.0/cores/teensy4
TEENSY_FLAGS := -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -nostdlib -mthumb \
                -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -D__IMXRT1062__ -DTEENSYDUINO=160 \
                -DARDUINO=10607 -DARDUINO_TEENSY41 -DF_CPU=600000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH
TEENSY_CXX := -std=gnu++17 -fno-exceptions -fpermissive -fno-rtti -fno-threadsafe-statics \
              -felide-constructors -Wno-psabi -Wno-maybe-uninitialized
ARDUINO_OBJ ?= ../out/600/build/sketch
FMA_RE   := 'vfma|vfms|vfnma|vfnms'

.PHONY: all test test-geom sanitize teensy-strict check clean

all: $(BUILD)/teensy_sim $(BUILD)/geom_ref $(BUILD)/test_geom

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/teensy_sim: $(SIM_SRC) $(SIM_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SIM_SRC) -lm

$(BUILD)/geom_ref: $(REF_SRC) $(COMMON)/gpu_refrast.h $(COMMON)/geom.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(REF_SRC) -lm

$(BUILD)/test_geom: ../../tests/test_geom.c $(COMMON)/geom.c $(COMMON)/gpu_setup.c $(COMMON)/gpu_refrast.c \
                    $(COMMON)/geom.h $(FW)/tg_geom_ext.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ ../../tests/test_geom.c $(COMMON)/geom.c $(COMMON)/gpu_setup.c \
	    $(COMMON)/gpu_refrast.c -lm

$(BUILD)/teensy_sim_san: $(SIM_SRC) $(SIM_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -O1 -o $@ $(SIM_SRC) -lm

$(BUILD)/geom_ref_san: $(REF_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(SAN) -O1 -o $@ $(REF_SRC) -lm

test: $(BUILD)/teensy_sim $(BUILD)/geom_ref
	$(PYTHON) test_teensy_sim.py --sim $(BUILD)/teensy_sim --ref $(BUILD)/geom_ref --daemon $(DAEMON)

test-geom: $(BUILD)/test_geom
	mkdir -p $(BUILD)/geom_out
	./$(BUILD)/test_geom $(BUILD)/geom_out

sanitize: $(BUILD)/teensy_sim_san $(BUILD)/geom_ref_san
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
	    $(PYTHON) test_teensy_sim.py --sim $(BUILD)/teensy_sim_san --ref $(BUILD)/geom_ref_san \
	    --daemon $(DAEMON) --slow

teensy-strict: | $(BUILD)
	@for f in gpu_proto.h gpu_setup.h gpu_setup.c geom.h geom.c; do \
	  cmp -s $(COMMON)/$$f $(FW)/src/$$f || { echo "$(FW)/src/$$f differs from common/ - run build_teensy.cmd"; exit 1; }; done
	mkdir -p $(BUILD)/strict
	cd $(FW) && for f in tg_core.c src/geom.c src/gpu_setup.c; do \
	  $(TEENSY_GCC) $(TEENSY_FLAGS) -std=gnu11 -c $$f -o ../sim/$(BUILD)/strict/`basename $$f`.o || exit 1; done
	cd $(FW) && $(TEENSY_GXX) $(TEENSY_FLAGS) $(TEENSY_CXX) -isystem $(TEENSY_CORE_W) -x c++ -c tg_teensy.cpp \
	    -o ../sim/$(BUILD)/strict/tg_teensy.o
	cd $(FW) && $(TEENSY_GXX) $(TEENSY_FLAGS) $(TEENSY_CXX) -isystem $(TEENSY_CORE_W) -include Arduino.h \
	    -x c++ -c teensy_gpu.ino -o ../sim/$(BUILD)/strict/teensy_gpu_ino.o
	@echo "teensy-strict: -Wall -Wextra -Werror clean (tg_core.c geom.c gpu_setup.c tg_teensy.cpp teensy_gpu.ino)"
	printf 'float f(float a, float b, float c) { return a * b + c; }\n' > $(BUILD)/strict/fma_ctl.c
	cd $(BUILD)/strict && $(TEENSY_GCC) -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 \
	    -std=gnu11 -c fma_ctl.c -o fma_ctl.o
	@n=`arm-linux-gnueabihf-objdump -d $(BUILD)/strict/fma_ctl.o | grep -Ec $(FMA_RE)`; \
	  echo "positive control (a*b+c, default contraction): $$n fused ops"; test $$n -gt 0
	@test -f $(ARDUINO_OBJ)/tg_core.c.o || { echo "no Arduino build in $(ARDUINO_OBJ)"; exit 1; }
	@if arm-linux-gnueabihf-objdump -d $(ARDUINO_OBJ)/tg_core.c.o $(ARDUINO_OBJ)/src/geom.c.o \
	     $(ARDUINO_OBJ)/src/gpu_setup.c.o $(BUILD)/strict/tg_core.c.o $(BUILD)/strict/geom.c.o \
	     $(BUILD)/strict/gpu_setup.c.o | grep -E $(FMA_RE); then echo "FUSED MULTIPLY-ADD FOUND"; exit 1; fi
	@echo "teensy-strict: no fused multiply-add in the arduino-cli objects or the strict objects"

check: test-geom test sanitize teensy-strict

clean:
	rm -rf $(BUILD) __pycache__

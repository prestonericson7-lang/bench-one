# viewer/Makefile -- dependency-free X11 viewer (x11view) + test program.
#
#   make              x86_64 + aarch64: static test_view and libx11view.a per architecture
#   make x86 | aarch64
#   make selftest     offline conversion / DISPLAY / .Xauthority self-test (x86_64)
#   make run [SECS=5] test_view against the X server in $$DISPLAY for SECS seconds (x86_64)
#   make mocktest     test matrix against tools/mock_xserver.py (formats, auth, MIT-SHM, events, ...)
#   make asan         AddressSanitizer + UBSan build (dynamic) in build/asan/
#   make test         selftest + mock matrix + live run, then the same under ASan/UBSan
#   make clean
#
# Static binaries are built with -DX11V_NO_GETADDRINFO: host names in DISPLAY are then resolved
# from /etc/hosts only (numeric addresses and localhost always work), so the binaries never need
# glibc's NSS shared libraries at run time. Set STATIC_DEFS= to use getaddrinfo instead.
# (32-bit ARM: add a rule with arm-linux-gnueabihf-gcc; needs libc6-dev-armhf-cross, which is not
# installed on the machine this was developed on, so it is not a default target.)

X86_CC  ?= gcc
X86_AR  ?= ar
A64_CC  ?= aarch64-linux-gnu-gcc
A64_AR  ?= aarch64-linux-gnu-ar
PYTHON  ?= python3

WARN        = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-align=strict
CFLAGS     ?= -std=c99 -O2 -pipe $(WARN)
STATIC_DEFS ?= -DX11V_NO_GETADDRINFO
LDFLAGS    ?= -static
BUILD      ?= build
SECS       ?= 5
SANFLAGS    = -std=c99 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all $(WARN)

.PHONY: all x86 aarch64 selftest run mocktest asan test clean

all: x86 aarch64

x86:     $(BUILD)/x86_64/test_view  $(BUILD)/x86_64/libx11view.a
aarch64: $(BUILD)/aarch64/test_view $(BUILD)/aarch64/libx11view.a

# $(1) = build subdir, $(2) = compiler, $(3) = archiver
define ARCH_RULES
$(BUILD)/$(1)/x11view.o: x11view.c x11view.h
	@mkdir -p $$(@D)
	$(2) $$(CFLAGS) $$(STATIC_DEFS) -c -o $$@ $$<
$(BUILD)/$(1)/test_view.o: test_view.c x11view.h
	@mkdir -p $$(@D)
	$(2) $$(CFLAGS) -c -o $$@ $$<
$(BUILD)/$(1)/libx11view.a: $(BUILD)/$(1)/x11view.o
	rm -f $$@
	$(3) rcs $$@ $$^
$(BUILD)/$(1)/test_view: $(BUILD)/$(1)/test_view.o $(BUILD)/$(1)/libx11view.a
	$(2) $$(LDFLAGS) -o $$@ $$^
endef

$(eval $(call ARCH_RULES,x86_64,$(X86_CC),$(X86_AR)))
$(eval $(call ARCH_RULES,aarch64,$(A64_CC),$(A64_AR)))

$(BUILD)/asan/test_view: x11view.c x11view.h test_view.c
	@mkdir -p $(@D)
	$(X86_CC) $(SANFLAGS) -o $@ x11view.c test_view.c

asan: $(BUILD)/asan/test_view

selftest: $(BUILD)/x86_64/test_view
	cd $(BUILD) && ./x86_64/test_view --selftest

run: $(BUILD)/x86_64/test_view
	cd $(BUILD) && ./x86_64/test_view $(SECS)

mocktest: x86 aarch64
	$(PYTHON) tools/run_tests.py --logdir $(BUILD)/testlogs

test: all asan
	cd $(BUILD) && ./x86_64/test_view --selftest && ./aarch64/test_view --selftest
	$(PYTHON) tools/run_tests.py --logdir $(BUILD)/testlogs
	cd $(BUILD) && ./x86_64/test_view $(SECS)
	$(PYTHON) tools/run_tests.py --logdir $(BUILD)/testlogs-asan --bin $(BUILD)/asan/test_view
	cd $(BUILD) && ASAN_OPTIONS=detect_leaks=1 ./asan/test_view 2

clean:
	rm -rf $(BUILD)

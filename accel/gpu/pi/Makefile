# pi/Makefile -- Orange Pi host library (libfpgagpu) + tools. Run in WSL/Linux.
#
#   make pi       static AArch64 binaries for the Orange Pi 4 Pro   -> build/aarch64/
#   make x86      static x86-64 binaries (simulator runs on the PC) -> build/x86/
#   make          both
#   make native   build/native/ with the host's own gcc (e.g. on the Orange Pi itself)
#   make check    offline self-test of the library pieces (meshes, font, PNG, math), x86
#   make clean
#
# Every file is compiled with -O2 -Wall -Wextra -ffp-contract=off (fp-contract off is required:
# gpu_selftest predicts the Teensy's and the PL's results bit-exactly with common/geom.c,
# common/gpu_setup.c and common/gpu_refrast.c). WERROR=-Werror makes warnings fatal.
# The X11 viewer (../viewer/x11view.c, read-only here) is compiled into the tools.
# -DX11V_NO_GETADDRINFO: static binaries never need glibc's NSS shared objects at run time.

A64_CC    ?= aarch64-linux-gnu-gcc
X86_CC    ?= gcc
NATIVE_CC ?= cc
WERROR    ?=
CFLAGS    := -std=gnu99 -O2 -Wall -Wextra -ffp-contract=off $(WERROR)
DEFS      := -DX11V_NO_GETADDRINFO
INC       := -I. -I../common -I../viewer
LDFLAGS   := -static
LIBS      := -lm
BUILD     := build

LIB_SRC   := libfpgagpu.c gpu_math.c mesh.c font8x8.c img.c tool_common.c \
             ../common/gpu_setup.c ../common/gpu_refrast.c ../common/geom.c ../viewer/x11view.c
HDRS      := libfpgagpu.h gpu_math.h mesh.h font8x8.h img.h tool_common.h \
             ../common/gpu_proto.h ../common/gpu_setup.h ../common/gpu_refrast.h ../common/geom.h \
             ../viewer/x11view.h Makefile
TOOLS     := gpu_selftest gpu_demo gpu_view gpu_stat gpu_snap gpu_image

.PHONY: all pi x86 native check clean

all: pi x86

# $(1) = build dir name, $(2) = compiler
define ARCH_RULES
$(1)_OBJ := $$(addprefix $(BUILD)/$(1)/obj/,$$(notdir $$(LIB_SRC:.c=.o)))

$(BUILD)/$(1)/obj/%.o: %.c $$(HDRS)
	@mkdir -p $$(@D)
	$(2) $$(CFLAGS) $$(DEFS) $$(INC) -c -o $$@ $$<
$(BUILD)/$(1)/obj/%.o: ../common/%.c $$(HDRS)
	@mkdir -p $$(@D)
	$(2) $$(CFLAGS) $$(DEFS) $$(INC) -c -o $$@ $$<
$(BUILD)/$(1)/obj/%.o: ../viewer/%.c $$(HDRS)
	@mkdir -p $$(@D)
	$(2) $$(CFLAGS) $$(DEFS) $$(INC) -c -o $$@ $$<
$(BUILD)/$(1)/libfpgagpu.a: $$($(1)_OBJ)
	rm -f $$@
	$(3) rcs $$@ $$^
$(BUILD)/$(1)/%: $(BUILD)/$(1)/obj/%.o $(BUILD)/$(1)/libfpgagpu.a
	$(2) $$(LDFLAGS) -o $$@ $$< $(BUILD)/$(1)/libfpgagpu.a $$(LIBS)
$(1): $$(addprefix $(BUILD)/$(1)/,$$(TOOLS)) $(BUILD)/$(1)/libfpgagpu.a
endef

A64_AR    ?= aarch64-linux-gnu-ar
X86_AR    ?= ar
NATIVE_AR ?= ar

$(eval $(call ARCH_RULES,aarch64,$(A64_CC),$(A64_AR)))
$(eval $(call ARCH_RULES,x86,$(X86_CC),$(X86_AR)))
$(eval $(call ARCH_RULES,native,$(NATIVE_CC),$(NATIVE_AR)))

pi: aarch64

.SECONDARY:

check: $(BUILD)/x86/gpu_selftest
	$(BUILD)/x86/gpu_selftest --offline

clean:
	rm -rf $(BUILD)

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Lazy-loading trampolines for the display and GPU libraries
#
# Included always; active when DLOPEN_GPU_LIBS=yes (mk/cross.mk, the Pi and x86
# DRM builds). Every function libdrm, libgbm, libEGL and libinput export becomes
# a symbol in our binary that dlopen()s its library on the first call, so the
# binary carries no NEEDED entry for them and starts on a system without them.
# libGLESv2 is not here: GLES2 entry points are resolved through
# include/gles2_loader.h instead.
#
# The trampolines are generated at build time by the vendored Implib.so
# (tools/implib, pinned in its README) from the target's own libraries in the
# toolchain sysroot, so they always match what the binary is compiled against.
# Every caller asks include/gpu_libs.h first. A call that gets past it into a
# library that is not installed does not abort, as upstream's loader would: the
# callbacks in src/platform/display/gpu_libs.cpp log it and hand back a function returning 0.
# scripts/check_gpu_lib_callers.py keeps the callers to a reviewed list.

IMPLIB_OBJS :=
IMPLIB_GOALS :=

ifeq ($(DLOPEN_GPU_LIBS),yes)

IMPLIB_GEN := tools/implib/implib-gen.py
IMPLIB_SYSROOT_LIBS := /usr/lib/$(TARGET_TRIPLE)
IMPLIB_SONAMES := libdrm.so.2 libgbm.so.1 libEGL.so.1 libinput.so.10
IMPLIB_DIR := $(BUILD_DIR)/implib
IMPLIB_INIT_C := $(foreach lib,$(IMPLIB_SONAMES),$(IMPLIB_DIR)/$(lib).init.c)
# The headers the return-kind table reads each function's return type from.
IMPLIB_HEADERS := /usr/include/xf86drm.h /usr/include/xf86drmMode.h /usr/include/gbm.h \
                  /usr/include/EGL/egl.h /usr/include/libinput.h
IMPLIB_RETURNS := $(IMPLIB_DIR)/gpu_lib_returns
IMPLIB_OBJS := $(foreach lib,$(IMPLIB_SONAMES),$(IMPLIB_DIR)/$(lib).tramp.o $(IMPLIB_DIR)/$(lib).init.o) \
               $(IMPLIB_RETURNS).o

# One run writes both files, so they share the rule (a multi-target pattern
# rule is grouped).
$(IMPLIB_DIR)/%.tramp.S $(IMPLIB_DIR)/%.init.c: $(IMPLIB_SYSROOT_LIBS)/% $(IMPLIB_GEN) tools/implib/arch/common/init.c.tpl mk/implib.mk
	$(Q)mkdir -p $(IMPLIB_DIR)
	$(ECHO) "$(CYAN)[IMPLIB]$(RESET) $*"
	$(Q)python3 $(IMPLIB_GEN) -q --target $(TARGET_TRIPLE) --outdir $(IMPLIB_DIR) \
		--dlopen-callback helix_gpu_lib_open --dlsym-callback helix_gpu_lib_symbol $<

$(IMPLIB_DIR)/%.tramp.o: $(IMPLIB_DIR)/%.tramp.S
	$(Q)$(CC) $(TARGET_CFLAGS) -c $< -o $@

$(IMPLIB_DIR)/%.init.o: $(IMPLIB_DIR)/%.init.c
	$(Q)$(CC) $(TARGET_CFLAGS) -w -c $< -o $@

# What each function returns, so the stand-in for a missing library fails the
# way its caller expects (src/platform/display/gpu_libs.cpp#helix_gpu_lib_symbol).
$(IMPLIB_RETURNS).c: $(IMPLIB_INIT_C) $(IMPLIB_HEADERS) scripts/gpu_lib_return_kinds.py
	$(Q)python3 scripts/gpu_lib_return_kinds.py --out $@ --init-c $(IMPLIB_INIT_C) --header $(IMPLIB_HEADERS)

$(IMPLIB_RETURNS).o: $(IMPLIB_RETURNS).c
	$(Q)$(CC) $(TARGET_CFLAGS) -c $< -o $@

# Generated sources are kept, not deleted as intermediates, so a rebuild does
# not regenerate them.
.PRECIOUS: $(IMPLIB_DIR)/%.tramp.S $(IMPLIB_DIR)/%.init.c $(IMPLIB_RETURNS).c

# The main link (mk/rules.mk) links $^, so these join it as prerequisites.
$(TARGET): $(IMPLIB_OBJS)

# Only reviewed objects may call into these libraries; run once everything
# that links the trampolines is built.
IMPLIB_GOALS := verify-gpu-callers

.PHONY: verify-gpu-callers
verify-gpu-callers: $(TARGET) $(BUILD_DIR)/bin/helix-splash $(BUILD_DIR)/bin/helix-watchdog
	$(Q)python3 scripts/check_gpu_lib_callers.py --build-dir $(BUILD_DIR) --nm $(CROSS_COMPILE)nm

endif

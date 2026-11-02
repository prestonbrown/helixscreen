# SPDX-License-Identifier: GPL-3.0-or-later
#
# EGL presentation path — the GPU rung, linked into the DRM binary
#
# Included when ENABLE_EGL_RUNG=yes. The DRM binary $(TARGET) carries LVGL's
# two DRM drivers side by side, and the app picks one at startup:
#
#   HELIX_DISPLAY_BACKEND=egl   LVGL presents through EGL/GBM on the GPU
#   HELIX_DISPLAY_BACKEND=drm   LVGL presents into DRM dumb buffers  (default)
#
# scripts/helix-launcher.sh chooses between them at boot by running
# `helix-screen --probe-egl`, which exits 0 only on real GPU hardware.
#
# Which driver LVGL compiles is a config macro, so the EGL one is a second
# compile of the few translation units that read it: LVGL's DRM and OpenGL ES
# drivers and src/api/display_backend_drm.cpp. Nothing else under src/,
# include/ or LVGL's core references LV_USE_OPENGLES, LV_LINUX_DRM_USE_EGL or
# LV_USE_EGL, so the rest of the binary is shared.
#
# The two compiles define four of the same public LVGL functions. The EGL
# compile renames its copies (EGL_SYMBOL_RENAMES) in the driver and in the
# backend that calls them alike, so each backend binds to its own driver.
# DisplayBackendDRM sits in an inline namespace chosen by the same macro
# (include/display_backend_drm.h), and the EGL one is reached only through
# make_drm_egl_backend().

EGL_VARIANT_DIR := $(BUILD_DIR)/egl-variant
EGL_LVGL_VARIANT_DIR := $(EGL_VARIANT_DIR)/lvgl

# HELIX_ENABLE_OPENGLES is the only knob. lv_conf.h maps it to LV_USE_OPENGLES
# and LV_LINUX_DRM_BACKEND_EGL, and lv_conf_internal.h derives LV_LINUX_DRM_USE_EGL
# and then LV_USE_EGL from those.
EGL_DEFINE := -DHELIX_ENABLE_OPENGLES

# Every public function both DRM drivers define. A fifth one makes the link
# fail with a duplicate definition, which is the cue to add it here.
EGL_SYMBOL_RENAMES := \
    -Dlv_linux_drm_create=lv_linux_drm_egl_create \
    -Dlv_linux_drm_set_file=lv_linux_drm_egl_set_file \
    -Dlv_linux_drm_set_mode_cb=lv_linux_drm_egl_set_mode_cb \
    -Dlv_linux_drm_get_fd=lv_linux_drm_egl_get_fd

EGL_APP_CXXFLAGS := $(CXXFLAGS) $(EGL_DEFINE) $(EGL_SYMBOL_RENAMES)
EGL_LVGL_CFLAGS := $(SUBMODULE_CFLAGS) $(EGL_DEFINE) $(EGL_SYMBOL_RENAMES)
EGL_LVGL_CXXFLAGS := $(SUBMODULE_CXXFLAGS) -fpermissive $(EGL_DEFINE) $(EGL_SYMBOL_RENAMES)

# =============================================================================
# LVGL driver objects that change with the config
# =============================================================================
# The DRM drivers decide which of the two back ends compiles at all, and the
# OpenGL ES drivers compile to nothing without it. The shader asset is held out
# because it changes COMPILER, not just flags — see its rule below.

EGL_LVGL_SHADER_SRC := $(LVGL_DIR)/src/drivers/opengles/assets/lv_opengles_shader.c

EGL_LVGL_VARIANT_SRCS := $(filter-out $(EGL_LVGL_SHADER_SRC), \
    $(filter $(LVGL_DIR)/src/drivers/display/drm/% $(LVGL_DIR)/src/drivers/opengles/%, \
             $(LVGL_SRCS)))

EGL_LVGL_VARIANT_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(EGL_LVGL_VARIANT_DIR)/%.o,$(EGL_LVGL_VARIANT_SRCS))
EGL_LVGL_SHADER_OBJ := $(EGL_LVGL_VARIANT_DIR)/src/drivers/opengles/assets/lv_opengles_shader.o

# The dumb-buffer driver and the shared mode helpers come from the base build.
# Their EGL compiles are an empty translation unit and a duplicate of the base
# object, so neither is linked.
EGL_LVGL_LINKED_OBJS := $(EGL_LVGL_SHADER_OBJ) $(filter-out \
    $(EGL_LVGL_VARIANT_DIR)/src/drivers/display/drm/lv_linux_drm.o \
    $(EGL_LVGL_VARIANT_DIR)/src/drivers/display/drm/lv_linux_drm_common.o, \
    $(EGL_LVGL_VARIANT_OBJS))

$(EGL_LVGL_VARIANT_DIR)/%.o: $(LVGL_DIR)/%.c lv_conf.h $(PATCHES_STAMP) $(ABI_STAMP) | $(PATCH_MARKER_STAMP)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC/egl]$(RESET) $<"
	$(Q)$(CC) $(EGL_LVGL_CFLAGS) $(DEPFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# The shader asset holds its GLSL in C++11 raw string literals, so it compiles
# as C++ when the config is on and as plain C when it is off. Both spellings
# must land at DIFFERENT object paths: the base build already wrote a C object
# for this source, and linking that one into the EGL binary produces a binary
# with no shaders and no error. An explicit rule beats the pattern above.
$(EGL_LVGL_SHADER_OBJ): $(EGL_LVGL_SHADER_SRC) lv_conf.h $(PATCHES_STAMP) $(ABI_STAMP) | $(PATCH_MARKER_STAMP)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX/egl]$(RESET) $< (raw string literals)"
	$(Q)$(CXX) $(EGL_LVGL_CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# =============================================================================
# App objects that change with the config
# =============================================================================
# display_backend_drm.cpp is the whole list: it is the only file under src/ or
# include/ that reads an EGL config macro. The base build's compile of it stays
# in the link as the dumb-buffer backend.

EGL_APP_VARIANT_OBJS := $(EGL_VARIANT_DIR)/display_backend_drm.o

$(EGL_VARIANT_DIR)/display_backend_drm.o: src/api/display_backend_drm.cpp $(LIBHV_LIB) $(PATCHES_STAMP) $(ABI_STAMP) | $(EGL_VARIANT_DIR) $(PATCH_MARKER_STAMP)
	$(ECHO) "$(CYAN)[CXX/egl]$(RESET) $<"
	$(Q)$(CXX) $(EGL_APP_CXXFLAGS) $(DEPFLAGS) $(FORCED_INCLUDE) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(EGL_VARIANT_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Link
# =============================================================================
# Extra prerequisites of the DRM link in mk/rules.mk, whose recipe links $^.
# The fbdev link leaves them out (mk/pi-dual-link.mk): it carries no GPU path.
EGL_PATH_OBJS := $(EGL_APP_VARIANT_OBJS) $(EGL_LVGL_LINKED_OBJS)

$(TARGET): $(EGL_PATH_OBJS)

# =============================================================================
# Verification — the stale-object trap has no other symptom
# =============================================================================
# If the EGL objects did not make it into the link, the binary still builds and
# still runs, and HELIX_DISPLAY_BACKEND=egl finds no EGL backend and presents
# through dumb buffers while the launcher believes it is on the GPU.
#
# The tell is the GLES shader source. lv_opengles_shader.c holds its GLSL in
# string literals, so a binary that really carries the OpenGL ES driver has the
# uniform names in .rodata and one that does not has nothing. Unlike a symbol,
# .rodata survives `strip` — which matters because these binaries are stripped,
# and `nm` on a stripped binary reports no symbols at all, so a symbol test
# would pass by finding nothing rather than by checking anything.
EGL_SHADER_TOKEN := u_SwapRB

.PHONY: verify-egl

verify-egl: $(TARGET)
	@echo "Verifying $(TARGET) carries the OpenGL ES driver..."
	@if ! strings -a $(TARGET) | grep -q '$(EGL_SHADER_TOKEN)'; then \
		echo "$(RED)ERROR: $(TARGET) carries no GLES shaders — the EGL path's"; \
		echo "       objects did not reach the link, and HELIX_DISPLAY_BACKEND=egl"; \
		echo "       would present through dumb buffers. Check EGL_PATH_OBJS.$(RESET)"; \
		exit 1; \
	fi
	@echo "$(GREEN)✓ $(TARGET) carries the EGL presentation path$(RESET)"

-include $(wildcard $(EGL_VARIANT_DIR)/*.d)
-include $(shell find $(EGL_LVGL_VARIANT_DIR) -name '*.d' 2>/dev/null)

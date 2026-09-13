# SPDX-License-Identifier: GPL-3.0-or-later
#
# EGL link — the GPU presentation rung
#
# Included when ENABLE_EGL_RUNG=yes. Produces a third binary beside the DRM and
# fbdev ones:
#
#   helix-screen-egl    LVGL presents through EGL/GBM on the GPU
#   helix-screen        LVGL presents into DRM dumb buffers            (default)
#   helix-screen-fbdev  LVGL presents into /dev/fb0
#
# scripts/helix-launcher.sh picks between them at boot by running
# `helix-screen-egl --probe-egl`, which exits 0 only on real GPU hardware.
#
# Only a few translation units read the EGL config macros, so this links the
# DRM build's objects and recompiles just those. The app, the widgets and all
# of LVGL's core are byte-identical between the two configurations: nothing
# outside src/api/display_backend_drm.cpp and LVGL's own drivers references
# LV_USE_OPENGLES, LV_LINUX_DRM_USE_EGL or LV_USE_EGL.
#
# Build output:
#   $(BIN_DIR)/helix-screen-egl

EGL_TARGET := $(BIN_DIR)/helix-screen-egl

EGL_VARIANT_DIR := $(BUILD_DIR)/egl-variant
EGL_LVGL_VARIANT_DIR := $(EGL_VARIANT_DIR)/lvgl

# HELIX_ENABLE_OPENGLES is the only knob. lv_conf.h maps it to LV_USE_OPENGLES,
# and lv_conf_internal.h derives LV_LINUX_DRM_USE_EGL and then LV_USE_EGL from
# that — so setting either of those directly does nothing.
EGL_DEFINE := -DHELIX_ENABLE_OPENGLES

EGL_APP_CXXFLAGS := $(CXXFLAGS) $(EGL_DEFINE)
EGL_LVGL_CFLAGS := $(SUBMODULE_CFLAGS) $(EGL_DEFINE)
EGL_LVGL_CXXFLAGS := $(SUBMODULE_CXXFLAGS) -fpermissive $(EGL_DEFINE)

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

# The base-build objects these replace.
EGL_LVGL_REPLACED := $(patsubst $(LVGL_DIR)/%.c,$(OBJ_DIR)/lvgl/%.o, \
    $(EGL_LVGL_VARIANT_SRCS) $(EGL_LVGL_SHADER_SRC))
EGL_LVGL_OBJS := $(filter-out $(EGL_LVGL_REPLACED),$(LVGL_OBJS))

$(EGL_LVGL_VARIANT_DIR)/%.o: $(LVGL_DIR)/%.c lv_conf.h $(PATCHES_STAMP) $(ABI_STAMP)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CC/egl]$(RESET) $<"
	$(Q)$(CC) $(EGL_LVGL_CFLAGS) $(DEPFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# The shader asset holds its GLSL in C++11 raw string literals, so it compiles
# as C++ when the config is on and as plain C when it is off. Both spellings
# must land at DIFFERENT object paths: the base build already wrote a C object
# for this source, and linking that one into the EGL binary produces a binary
# with no shaders and no error. An explicit rule beats the pattern above.
$(EGL_LVGL_SHADER_OBJ): $(EGL_LVGL_SHADER_SRC) lv_conf.h $(PATCHES_STAMP) $(ABI_STAMP)
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "$(CYAN)[CXX/egl]$(RESET) $< (raw string literals)"
	$(Q)$(CXX) $(EGL_LVGL_CXXFLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

# =============================================================================
# App objects that change with the config
# =============================================================================
# display_backend_drm.cpp is the whole list: it is the only file under src/ or
# include/ that reads an EGL config macro. It guards the dumb-buffer-only entry
# points, which do not exist once the EGL driver is the one compiled.

EGL_APP_VARIANT_OBJS := $(EGL_VARIANT_DIR)/display_backend_drm.o
EGL_APP_REPLACED := $(OBJ_DIR)/api/display_backend_drm.o
EGL_APP_OBJS := $(filter-out $(EGL_APP_REPLACED),$(APP_OBJS))

$(EGL_VARIANT_DIR)/display_backend_drm.o: src/api/display_backend_drm.cpp $(LIBHV_LIB) $(PCH) $(ABI_STAMP) | $(EGL_VARIANT_DIR)
	$(ECHO) "$(CYAN)[CXX/egl]$(RESET) $<"
	$(Q)$(CXX) $(EGL_APP_CXXFLAGS) $(DEPFLAGS) $(PCH_FLAGS) $(INCLUDES) $(LV_CONF) -c $< -o $@

$(EGL_VARIANT_DIR):
	$(Q)mkdir -p $@

# =============================================================================
# Link
# =============================================================================
# Serialised behind the other binaries with an order-only prerequisite. Each
# whole-program link peaks at multiple GB, and three of them at once under
# `make -j` is how a Pi release build gets OOM-killed. Ordering costs nothing:
# the EGL link reuses objects the DRM link already needed.
ifdef PI_DUAL_LINK
EGL_LINK_AFTER := $(FBDEV_TARGET)
else
EGL_LINK_AFTER := $(TARGET)
endif

$(EGL_TARGET): $(APP_C_OBJS) $(EGL_APP_OBJS) $(EGL_APP_VARIANT_OBJS) $(APP_MODULE_OBJS) \
               $(REMOTE_LINENOISE_OBJ) $(OBJCPP_OBJS) \
               $(EGL_LVGL_OBJS) $(EGL_LVGL_VARIANT_OBJS) $(EGL_LVGL_SHADER_OBJ) \
               $(HELIX_XML_OBJS) $(THORVG_OBJS) $(LV_MARKDOWN_OBJS) $(QUIRC_OBJS) \
               $(FONT_OBJS) $(TRANS_OBJS) $(APP_DNS_RESOLV_OBJ) $(WPA_DEPS) \
               | $(EGL_LINK_AFTER)
	$(Q)mkdir -p $(BIN_DIR)
	$(ECHO) "$(MAGENTA)$(BOLD)[LD/egl]$(RESET) $@"
	$(Q)$(CXX) $(CXXFLAGS) $(filter-out %.a %.h %.hh %.hpp %.hxx,$^) -o $@ $(LDFLAGS) || { \
		echo "$(RED)$(BOLD)✗ EGL linking failed!$(RESET)"; \
		exit 1; \
	}

# =============================================================================
# Verification — the stale-object trap has no other symptom
# =============================================================================
# If the variant objects did not make it into the link, the binary still builds,
# still runs, and silently presents through dumb buffers while the launcher
# believes it is on the GPU. Nothing else distinguishes the two.
#
# The tell is the GLES shader source. lv_opengles_shader.c holds its GLSL in
# string literals, so a binary that really carries the OpenGL ES driver has the
# uniform names in .rodata and one that does not has nothing. Unlike a symbol,
# .rodata survives `strip` — which matters because these binaries are stripped,
# and `nm` on a stripped binary reports no symbols at all, so a symbol test
# would pass by finding nothing rather than by checking anything.
EGL_SHADER_TOKEN := u_SwapRB

.PHONY: verify-egl strip-egl

verify-egl: $(EGL_TARGET) $(TARGET)
	@echo "Verifying EGL binary actually carries the OpenGL ES driver..."
	@if ! strings -a $(EGL_TARGET) | grep -q '$(EGL_SHADER_TOKEN)'; then \
		echo "$(RED)ERROR: $(EGL_TARGET) carries no GLES shaders — it was linked"; \
		echo "       against the base build's objects and presents through dumb"; \
		echo "       buffers. Check the egl-variant object paths.$(RESET)"; \
		exit 1; \
	fi
	@if strings -a $(TARGET) | grep -q '$(EGL_SHADER_TOKEN)'; then \
		echo "$(RED)ERROR: $(TARGET) carries GLES shaders — the base build picked"; \
		echo "       up EGL objects it should not have.$(RESET)"; \
		exit 1; \
	fi
	@echo "$(GREEN)✓ EGL binary carries the GPU path; base binary does not$(RESET)"

strip-egl: verify-egl
ifeq ($(STRIP_BINARY),yes)
	$(Q)$(NM_CMD) -nC $(EGL_TARGET) > $(EGL_TARGET).sym 2>/dev/null || true
	$(Q)$(OBJCOPY_CMD) --only-keep-debug $(EGL_TARGET) $(EGL_TARGET).debug
	$(Q)$(STRIP_CMD) $(EGL_TARGET)
	@echo "Stripped: $(EGL_TARGET)"
else
	@echo "STRIP_BINARY not set — skipping strip of $(EGL_TARGET)"
endif

-include $(wildcard $(EGL_VARIANT_DIR)/*.d)
-include $(shell find $(EGL_LVGL_VARIANT_DIR) -name '*.d' 2>/dev/null)

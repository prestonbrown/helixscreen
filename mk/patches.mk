# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Upstream Patch Management Module
# Handles automatic application of patches to LVGL and other dependencies

# Files modified by LVGL patches (used by reset-patches)
# XML patches are no longer needed — those sources are in lib/helix-xml with patches baked in
LVGL_PATCHED_FILES := \
	src/drivers/sdl/lv_sdl_window.c \
	src/themes/default/lv_theme_default.c \
	src/drivers/display/fb/lv_linux_fbdev.c \
	src/drivers/display/fb/lv_linux_fbdev.h \
	src/core/lv_refr.c \
	src/core/lv_observer.c \
	src/widgets/slider/lv_slider.c \
	src/stdlib/clib/lv_string_clib.c \
	src/stdlib/builtin/lv_string_builtin.c \
	src/draw/sw/blend/lv_draw_sw_blend.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c \
	src/draw/sw/lv_draw_sw_letter.c \
	src/drivers/display/drm/lv_linux_drm.c \
	src/drivers/display/drm/lv_linux_drm.h \
	src/drivers/display/drm/lv_linux_drm_egl.c

# Files modified by libhv patches
LIBHV_PATCHED_FILES := \
	http/client/requests.h \
	base/hsocket.c \
	base/dns_resolv.c \
	base/dns_resolv.h

# ============================================================================
# PATCH STAMP FILE - Skip checking if patches haven't changed
# ============================================================================
# The stamp file tracks when patches were last verified/applied.
# Re-check only when: patch files change, submodule HEAD changes, or stamp missing.
PATCHES_STAMP := $(BUILD_DIR)/.patches-applied
PATCH_FILES := $(wildcard patches/*.patch)

# Submodule HEAD files - changes when submodule is updated
# Note: In regular repos, submodules use .git/modules/<name>/HEAD
# In worktrees, .git is a file pointing to main repo's .git/worktrees/<name>/
# So we need to resolve the actual git modules path
# In Docker/non-git contexts (rsync'd source), these won't exist — that's fine,
# patches will be re-checked based on patch file changes only.
GIT_DIR := $(shell git rev-parse --git-dir 2>/dev/null || echo ".git")
GIT_COMMON_DIR := $(shell git rev-parse --git-common-dir 2>/dev/null || echo ".git")
LVGL_HEAD_CANDIDATE := $(GIT_COMMON_DIR)/modules/lvgl/HEAD
LIBHV_HEAD_CANDIDATE := $(GIT_COMMON_DIR)/modules/libhv/HEAD
LVGL_HEAD := $(wildcard $(LVGL_HEAD_CANDIDATE))
LIBHV_HEAD := $(wildcard $(LIBHV_HEAD_CANDIDATE))

# Reset all patched files in LVGL submodule to upstream state
reset-patches:
	$(ECHO) "$(YELLOW)Resetting LVGL patches to upstream state...$(RESET)"
	$(Q)for file in $(LVGL_PATCHED_FILES); do \
		if ! git -C $(LVGL_DIR) diff --quiet $$file 2>/dev/null; then \
			echo "$(YELLOW)→ Resetting:$(RESET) $$file"; \
			git -C $(LVGL_DIR) checkout $$file; \
		else \
			echo "$(DIM)  (clean) $$file$(RESET)"; \
		fi \
	done
	$(ECHO) "$(GREEN)✓ All LVGL patches reset$(RESET)"

# Force reapply all patches (reset first, then apply)
reapply-patches: reset-patches force-apply-patches
	$(ECHO) "$(GREEN)✓ All patches reapplied$(RESET)"

# apply-patches: File-based target that skips if stamp is current
# Dependencies: patch files + submodule HEADs (re-run if submodule updated)
apply-patches: $(PATCHES_STAMP)

# Force patch application (used by reapply-patches)
.PHONY: force-apply-patches
force-apply-patches:
	@rm -f $(PATCHES_STAMP)
	@$(MAKE) $(PATCHES_STAMP)

# The actual stamp file - only rebuilt when patches or submodules change
$(PATCHES_STAMP): $(PATCH_FILES) $(LVGL_HEAD) $(LIBHV_HEAD)
	@mkdir -p $(BUILD_DIR)
	$(ECHO) "$(CYAN)Checking LVGL patches...$(RESET)"
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_window.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SDL window patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_sdl_window.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_sdl_window.patch && \
			echo "$(GREEN)✓ SDL window patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL SDL window patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/themes/default/lv_theme_default.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL theme breakpoints patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_theme_breakpoints.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_theme_breakpoints.patch && \
			echo "$(GREEN)✓ Theme breakpoints patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL theme breakpoints patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev stride bpp detection patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_fbdev_stride_bpp.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_fbdev_stride_bpp.patch && \
			echo "$(GREEN)✓ Fbdev stride bpp detection patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev stride bpp detection patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/fb/lv_linux_fbdev.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev skip-unblank patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_fbdev_skip_unblank.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_fbdev_skip_unblank.patch && \
			echo "$(GREEN)✓ Fbdev skip-unblank patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev skip-unblank patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_observer.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer debug info patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_observer_debug.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_observer_debug.patch && \
			echo "$(GREEN)✓ Observer debug info patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL observer debug info patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/widgets/slider/lv_slider.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL slider scroll chain patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_slider_scroll_chain.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_slider_scroll_chain.patch && \
			echo "$(GREEN)✓ Slider scroll chain patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL slider scroll chain patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/stdlib/clib/lv_string_clib.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL strdup NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-strdup-null-guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-strdup-null-guard.patch && \
			echo "$(GREEN)✓ strdup NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL strdup NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/blend/lv_draw_sw_blend.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_blend_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_blend_null_guard.patch && \
			echo "$(GREEN)✓ Blend NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend color NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_blend_color_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_blend_color_null_guard.patch && \
			echo "$(GREEN)✓ Blend color NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend color NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_letter.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL label draw NULL font guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_draw_sw_label_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_draw_sw_label_null_guard.patch && \
			echo "$(GREEN)✓ Label draw NULL font guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL label draw NULL font guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/drm/lv_linux_drm.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM plane rotation patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-drm-plane-rotation.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-drm-plane-rotation.patch && \
			echo "$(GREEN)✓ DRM plane rotation patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM plane rotation patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/drm/lv_linux_drm_egl.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM EGL getters patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-drm-egl-getters.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-drm-egl-getters.patch && \
			echo "$(GREEN)✓ DRM EGL getters patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM EGL getters patch already applied$(RESET)"; \
	fi
	$(ECHO) "$(CYAN)Checking libhv patches...$(RESET)"
	$(Q)if git -C $(LIBHV_DIR) diff --quiet http/client/requests.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv streaming upload patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check ../../patches/libhv-streaming-upload.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply ../../patches/libhv-streaming-upload.patch && \
			echo "$(GREEN)✓ libhv streaming upload patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ libhv streaming upload patch already applied$(RESET)"; \
	fi
	$(Q)if [ -d "$(LIBHV_DIR)/include/hv" ]; then \
		if ! diff -q "$(LIBHV_DIR)/http/client/requests.h" "$(LIBHV_DIR)/include/hv/requests.h" >/dev/null 2>&1; then \
			echo "$(YELLOW)→ Syncing patched requests.h to include/hv/$(RESET)"; \
			cp "$(LIBHV_DIR)/http/client/requests.h" "$(LIBHV_DIR)/include/hv/requests.h" && \
			echo "$(GREEN)✓ Patched header synced$(RESET)"; \
		fi \
	fi
	$(Q)if git -C $(LIBHV_DIR) diff --quiet base/hsocket.c 2>/dev/null && \
	    [ ! -f "$(LIBHV_DIR)/base/dns_resolv.c" ]; then \
		echo "$(YELLOW)→ Applying libhv DNS resolver fallback patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check ../../patches/libhv-dns-resolver-fallback.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply ../../patches/libhv-dns-resolver-fallback.patch && \
			echo "$(GREEN)✓ DNS resolver fallback patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ libhv DNS resolver fallback patch already applied$(RESET)"; \
	fi
	@touch $@

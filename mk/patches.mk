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
	src/draw/sw/blend/lv_draw_sw_blend_to_argb8888.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_argb8888_premultiplied.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_rgb565_swapped.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_a8.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_l8.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_al88.c \
	src/draw/sw/blend/lv_draw_sw_blend_to_i1.c \
	src/draw/sw/blend/neon/lv_draw_sw_blend_neon_to_rgb888.c \
	src/draw/sw/blend/neon/lv_draw_sw_blend_neon_to_rgb565.c \
	src/draw/lv_draw.c \
	src/draw/lv_draw_buf.c \
	src/draw/sw/lv_draw_sw_mask_rect.c \
	src/draw/sw/lv_draw_sw_letter.c \
	src/draw/sw/lv_draw_sw_img.c \
	src/draw/sw/lv_draw_sw_blur.c \
	src/drivers/display/drm/lv_linux_drm.c \
	src/drivers/display/drm/lv_linux_drm.h \
	src/drivers/display/drm/lv_linux_drm_egl.c \
	src/drivers/evdev/lv_evdev.c \
	src/draw/lv_draw_arc.c \
	src/widgets/arc/lv_arc.c \
	src/draw/opengles/lv_draw_opengles.c \
	src/draw/sdl/lv_draw_sdl.c \
	src/display/lv_display.c \
	src/display/lv_display.h \
	src/display/lv_display_private.h \
	src/lv_conf_internal.h \
	src/misc/lv_event.c \
	src/misc/lv_event.h \
	src/core/lv_obj_event.c \
	src/core/lv_obj_pos.c \
	src/core/lv_obj_tree.c \
	src/core/lv_obj.c \
	src/core/lv_obj_style.c \
	src/draw/sw/lv_draw_sw.c \
	lv_conf_template.h

# Files modified by libhv patches
LIBHV_PATCHED_FILES := \
	Makefile \
	Makefile.in \
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
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_sw.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SDL SW android debug + blendmode fix patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_sdl_sw_android_debug.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_sdl_sw_android_debug.patch && \
			echo "$(GREEN)✓ SDL SW android debug + blendmode fix patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL SDL SW android debug + blendmode fix patch already applied$(RESET)"; \
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
	$(Q)if ! grep -q 'swap_rb' $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev BGR swap patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-fbdev-bgr-swap.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-fbdev-bgr-swap.patch && \
			echo "$(GREEN)✓ Fbdev BGR swap patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev BGR swap patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'LV_DRAW_BUF_ALIGN' $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev buffer alignment patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-fbdev-buffer-align.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-fbdev-buffer-align.patch && \
			echo "$(GREEN)✓ Fbdev buffer alignment patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev buffer alignment patch already applied$(RESET)"; \
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
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_observer_remove_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer remove NULL guard patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_observer_remove_null_guard.patch && \
		echo "$(GREEN)✓ Observer remove NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL observer remove NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'lv_subject_set_int: subject is NULL' $(LVGL_DIR)/src/core/lv_observer.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer subject NULL guards patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_observer_null_guards.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_observer_null_guards.patch && \
			echo "$(GREEN)✓ Observer subject NULL guards patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL observer subject NULL guards patch already applied$(RESET)"; \
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
	$(Q)if ! grep -q 'Clip blend_area to the layer' $(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend buffer bounds clip patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_blend_buf_bounds_clip.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_blend_buf_bounds_clip.patch && \
			echo "$(GREEN)✓ Blend buffer bounds clip patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend buffer bounds clip patch already applied$(RESET)"; \
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
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/lv_draw.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL signed draw coords patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-fix-signed-unsigned-draw-coords.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-fix-signed-unsigned-draw-coords.patch && \
			echo "$(GREEN)✓ Signed draw coords patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL signed draw coords patch already applied$(RESET)"; \
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
		echo "$(YELLOW)→ Applying LVGL DRM flush rotation patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-drm-flush-rotation.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-drm-flush-rotation.patch && \
			echo "$(GREEN)✓ DRM flush rotation patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM flush rotation patch already applied$(RESET)"; \
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
	$(Q)if ! grep -q 'lv_linux_drm_set_preferred_mode' $(LVGL_DIR)/src/drivers/display/drm/lv_linux_drm.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM preferred mode patch (#766)...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-drm-preferred-mode.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-drm-preferred-mode.patch && \
			echo "$(GREEN)✓ DRM preferred mode patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM preferred mode patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_refr.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL refr reshape NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_refr_reshape_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_refr_reshape_null_guard.patch && \
			echo "$(GREEN)✓ Refr reshape NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL refr reshape NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_img.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL img goto_xy NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_img_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_img_null_guard.patch && \
			echo "$(GREEN)✓ Img goto_xy NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL img goto_xy NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_blur.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blur goto_xy NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_blur_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_blur_null_guard.patch && \
			echo "$(GREEN)✓ Blur goto_xy NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blur goto_xy NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_obj_pos_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj_pos NULL guards patch (blur_walk_cb + layout_update_core)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_obj_pos_null_guards.patch && \
		echo "$(GREEN)✓ obj_pos NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj_pos NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if grep -q 'LV_ASSERT_MALLOC(draw_buf)' $(LVGL_DIR)/src/draw/lv_draw_buf.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_buf OOM guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_draw_buf_oom_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_draw_buf_oom_guard.patch && \
			echo "$(GREEN)✓ Draw_buf OOM guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL draw_buf OOM guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/evdev/lv_evdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL evdev Protocol-A touch release patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-evdev-protocol-a.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl-evdev-protocol-a.patch && \
			echo "$(GREEN)✓ Evdev Protocol-A touch release patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL evdev Protocol-A touch release patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/lv_draw_arc.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL arc draw guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_arc_draw_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_arc_draw_guard.patch && \
			echo "$(GREEN)✓ Arc draw guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL arc draw guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_arc_subject_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL arc subject NULL guard patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_arc_subject_null_guard.patch && \
		echo "$(GREEN)✓ Arc subject NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL arc subject NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_draw_sw_img_buf_height_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_sw_img buf_h guard patch (upstream ca18403)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_draw_sw_img_buf_height_guard.patch && \
		echo "$(GREEN)✓ draw_sw_img buf_h guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL draw_sw_img buf_h guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_drm_egl_render_mode_fix.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM EGL render mode fix (upstream ce112eb)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_drm_egl_render_mode_fix.patch && \
		echo "$(GREEN)✓ DRM EGL render mode fix applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL DRM EGL render mode fix already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_texture_cache_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL texture cache NULL guard patch (upstream ec053a0)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_texture_cache_null_guard.patch && \
		echo "$(GREEN)✓ Texture cache NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL texture cache NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_draw_sdl_stride_fix.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_sdl aligned stride fix...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_draw_sdl_stride_fix.patch && \
		echo "$(GREEN)✓ draw_sdl stride fix applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL draw_sdl stride fix already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_display_sync_cb.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL display sync callback patch (upstream 4170bcb)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_display_sync_cb.patch && \
		echo "$(GREEN)✓ Display sync callback patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL display sync callback patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_obj_delete_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj delete NULL guards patch (event depth guard + mark_deleted + obj_destructor + obj_delete_core)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_obj_delete_null_guards.patch && \
		echo "$(GREEN)✓ obj delete NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj delete NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_obj_delete_async_dedup.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj delete async dedup patch (dedup + UAF guard + diagnostics)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_obj_delete_async_dedup.patch && \
		echo "$(GREEN)✓ obj delete async dedup patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj delete async dedup patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_obj_get_screen_cycle_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj_get_screen cycle guard patch (cap parent-walk depth to 128)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_obj_get_screen_cycle_guard.patch && \
		echo "$(GREEN)✓ obj_get_screen cycle guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj_get_screen cycle guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_async_del_crumb.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL async-delete breadcrumb patch (#840/#906 sync+async diagnostic)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_async_del_crumb.patch && \
		echo "$(GREEN)✓ async-delete breadcrumb patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL async-delete breadcrumb patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/widgets/label/lv_label.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL label text transform patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_label_text_transform.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_label_text_transform.patch && \
			echo "$(GREEN)✓ Label text transform patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL label text transform patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl-sw-draw-wait-for-finish.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SW draw wait_for_finish + NULL guard patch (#739)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl-sw-draw-wait-for-finish.patch && \
		echo "$(GREEN)✓ SW draw wait_for_finish patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL SW draw wait_for_finish patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_obj_event.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event crash-diagnostic hook patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_event_crash_hook.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply ../../patches/lvgl_event_crash_hook.patch && \
			echo "$(GREEN)✓ Event crash-diagnostic hook patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL event crash-diagnostic hook patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_event_mark_deleted_defensive.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL lv_event_mark_deleted defensive bail patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_event_mark_deleted_defensive.patch && \
		echo "$(GREEN)✓ lv_event_mark_deleted defensive bail patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL lv_event_mark_deleted defensive bail patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_event_pop_unwind_safe.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event-pop unwind-safe patch (RPHAV9T7 / L081 root cause)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_event_pop_unwind_safe.patch && \
		echo "$(GREEN)✓ event-pop unwind-safe patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL event-pop unwind-safe patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_event_dispatch_depth_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event-dispatch-depth guard (cluster:pstat-async-delete / #906)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_event_dispatch_depth_guard.patch && \
		echo "$(GREEN)✓ event-dispatch-depth guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL event-dispatch-depth guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_event_stack_array.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL #907 array-backed event stack (replaces e->prev linked list)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_event_stack_array.patch && \
		echo "$(GREEN)✓ #907 array-backed event stack patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL #907 array-backed event stack patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_obj_event_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj-event NULL guards (VHTR49QJ — recoverable bail + telemetry)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_obj_event_null_guards.patch && \
		echo "$(GREEN)✓ obj-event NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj-event NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check ../../patches/lvgl_style_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL style NULL guards patch (null style pointers in transitions/cache)...$(RESET)"; \
		git -C $(LVGL_DIR) apply ../../patches/lvgl_style_null_guards.patch && \
		echo "$(GREEN)✓ Style NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL style NULL guards patch already applied$(RESET)"; \
	fi
	$(ECHO) "$(CYAN)Checking libhv patches...$(RESET)"
	$(Q)if git -C $(LIBHV_DIR) diff --quiet Makefile.in 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv OpenSSL/static build hook patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check ../../patches/libhv-openssl-static-link.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply ../../patches/libhv-openssl-static-link.patch && \
			echo "$(GREEN)✓ libhv OpenSSL/static build hook patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ libhv OpenSSL/static build hook patch already applied$(RESET)"; \
	fi
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

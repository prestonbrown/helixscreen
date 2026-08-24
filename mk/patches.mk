# Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen UI Prototype - Upstream Patch Management Module
# Handles automatic application of patches to LVGL and other dependencies

# Files modified by LVGL patches (used by reset-patches)
# XML patches are no longer needed — that engine is lib/helix-xml, our own
# submodule, edited and pushed directly rather than patched.
LVGL_PATCHED_FILES := \
	src/drivers/sdl/lv_sdl_window.c \
	src/themes/default/lv_theme_default.c \
	src/drivers/display/fb/lv_linux_fbdev.c \
	src/drivers/display/fb/lv_linux_fbdev.h \
	src/core/lv_refr.c \
	src/core/lv_observer.c \
	src/widgets/slider/lv_slider.c \
	src/widgets/image/lv_image.c \
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
	src/layouts/flex/lv_flex.c \
	src/layouts/grid/lv_grid.c \
	src/misc/lv_assert.h \
	src/drivers/sdl/lv_sdl_sw.c \
	src/core/lv_global.h \
	src/misc/lv_event_private.h \
	src/widgets/label/lv_label.c \
	src/widgets/label/lv_label.h \
	src/widgets/label/lv_label_private.h \
	src/libs/lodepng/lodepng.c \
	src/others/translation/lv_translation.c \
	lv_conf_template.h
# NOTE: src/misc/lv_check_arg.h is deliberately absent — the backport patch
# CREATES it, so it is untracked upstream and `git checkout` cannot restore it.
# reset-patches removes it explicitly instead.

# Files modified by libhv patches
LIBHV_PATCHED_FILES := \
	Makefile \
	Makefile.in \
	http/client/requests.h \
	base/hsocket.c \
	base/hplatform.h \
	base/hlog.c \
	base/dns_resolv.c \
	base/dns_resolv.h \
	cpputil/hthreadpool.h \
	evpp/TcpClient.h \
	http/HttpMessage.h \
	http/client/WebSocketClient.h \
	http/client/WebSocketClient.cpp

# ============================================================================
# PATCH STAMP FILE - Skip checking if patches haven't changed
# ============================================================================
# The stamp file tracks when patches were last verified/applied.
# Re-check only when: patch files change, submodule HEAD changes, or stamp missing.
PATCHES_STAMP := $(BUILD_DIR)/.patches-applied
PATCH_FILES := $(wildcard patches/*.patch)

# Absolute path to this repo's patches/. It MUST be absolute. The apply rules
# below run as `git -C $(LVGL_DIR) apply <path>`, and git resolves that path
# after chdir'ing into the submodule. scripts/setup-worktree.sh symlinks lib/
# into the main tree, so from a worktree `lib/lvgl` really is the main tree's
# lib/lvgl, and the old relative `../../patches/` landed on the MAIN tree's
# patches/ — a patch that existed only in the worktree was invisible.
PATCH_DIR := $(abspath patches)

# Patches applied outside this file. Keep this list empty if you can; an entry
# here means something applies the patch by hand, so nothing verifies it.
# libnl-socket-time-include.patch (65d0ba93a, GCC 14+ libnl build fix) has no
# applier anywhere in the tree — it is dead, kept only pending a decision to
# either wire it up or delete it.
PATCH_EXEMPT := libnl-socket-time-include.patch

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

# Restore one submodule's patched files to upstream state.
#   $(1) submodule dir, $(2) file list (paths relative to it)
#
# Two cases, and the second is why a plain `git checkout` loop is not enough: a
# patch that CREATES a file leaves that file untracked, where checkout fails with
# "did not match any file(s) known to git". It has to be deleted instead, or the
# re-apply then fails the other way with "already exists".
define reset_submodule_patches
	$(Q)for file in $(2); do \
		if ! git -C $(1) ls-files --error-unmatch "$$file" >/dev/null 2>&1; then \
			if [ -e "$(1)/$$file" ]; then \
				echo "$(YELLOW)→ Removing (patch-created):$(RESET) $$file"; \
				rm -f "$(1)/$$file"; \
			else \
				echo "$(DIM)  (absent) $$file$(RESET)"; \
			fi; \
		elif ! git -C $(1) diff --quiet "$$file" 2>/dev/null; then \
			echo "$(YELLOW)→ Resetting:$(RESET) $$file"; \
			git -C $(1) checkout "$$file"; \
		else \
			echo "$(DIM)  (clean) $$file$(RESET)"; \
		fi \
	done
endef

# Reset all patched files in both submodules to upstream state.
#
# libhv used to be missing here, which made `make reapply-patches` unable to fix
# the one thing it is advertised to fix. A tree carrying an older revision of a
# libhv patch fails `git apply --check` on the newer one, and the recipe tells you
# to run reapply-patches — which reset only LVGL, left the stale libhv hunks in
# place, and failed identically next time. That is how the #1212 null-hloop guard
# stayed out of a tree with no way to get it back short of editing by hand.
#
# Deliberately NOT reset: config.mk and hconfig.h. Both are dirty in a built tree
# but neither is patched — libhv's own ./configure writes them, and the libhv
# build regenerates them.
reset-patches:
	$(ECHO) "$(YELLOW)Resetting LVGL patches to upstream state...$(RESET)"
	$(call reset_submodule_patches,$(LVGL_DIR),$(LVGL_PATCHED_FILES))
	@# Not in LVGL_PATCHED_FILES: the backport patch creates it, so there is no
	@# tracked version to compare against.
	$(Q)rm -f $(LVGL_DIR)/src/misc/lv_check_arg.h
	$(ECHO) "$(YELLOW)Resetting libhv patches to upstream state...$(RESET)"
	$(call reset_submodule_patches,$(LIBHV_DIR),$(LIBHV_PATCHED_FILES))
	$(ECHO) "$(GREEN)✓ All patches reset$(RESET)"

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
	$(ECHO) "$(CYAN)Verifying patch wiring...$(RESET)"
	@# Both directions, because every failure mode here is silent. The apply
	@# blocks are hand-written, so a new patches/*.patch with no block is simply
	@# never applied; and `git apply --check` also fails when the patch file is
	@# unreadable, which the blocks' else-branch reports as "already applied".
	@# Either way the build links unpatched submodule code and says nothing.
	@fail=0; \
	for p in $(PATCH_FILES); do \
		b=$$(basename $$p); \
		case " $(PATCH_EXEMPT) " in *" $$b "*) continue;; esac; \
		grep -q "$$b" mk/patches.mk || { \
			echo "$(RED)✗ $$b has no apply block in mk/patches.mk — it would never be applied$(RESET)"; \
			fail=1; }; \
	done; \
	for r in $$(grep -o '$$(PATCH_DIR)/[a-zA-Z0-9_.-]*\.patch' mk/patches.mk | sed 's|.*/||' | sort -u); do \
		[ -f "$(PATCH_DIR)/$$r" ] || { \
			echo "$(RED)✗ mk/patches.mk applies $$r but $(PATCH_DIR)/$$r does not exist$(RESET)"; \
			fail=1; }; \
	done; \
	[ $$fail -eq 0 ] || { echo "$(RED)Refusing to build against unpatched submodules.$(RESET)"; exit 1; }
	$(ECHO) "$(GREEN)✓ Patch wiring consistent$(RESET)"
	$(ECHO) "$(CYAN)Checking LVGL patches...$(RESET)"
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_window.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SDL window patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_sdl_window.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_sdl_window.patch && \
			echo "$(GREEN)✓ SDL window patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL SDL window patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/sdl/lv_sdl_sw.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SDL SW android debug + blendmode fix patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_sdl_sw_android_debug.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_sdl_sw_android_debug.patch && \
			echo "$(GREEN)✓ SDL SW android debug + blendmode fix patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL SDL SW android debug + blendmode fix patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/themes/default/lv_theme_default.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL theme breakpoints patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_theme_breakpoints.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_theme_breakpoints.patch && \
			echo "$(GREEN)✓ Theme breakpoints patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL theme breakpoints patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev stride bpp detection patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_fbdev_stride_bpp.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_fbdev_stride_bpp.patch && \
			echo "$(GREEN)✓ Fbdev stride bpp detection patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev stride bpp detection patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/fb/lv_linux_fbdev.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev skip-unblank patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_fbdev_skip_unblank.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_fbdev_skip_unblank.patch && \
			echo "$(GREEN)✓ Fbdev skip-unblank patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev skip-unblank patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'swap_rb' $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev BGR swap patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-fbdev-bgr-swap.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-fbdev-bgr-swap.patch && \
			echo "$(GREEN)✓ Fbdev BGR swap patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev BGR swap patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'LV_DRAW_BUF_ALIGN' $(LVGL_DIR)/src/drivers/display/fb/lv_linux_fbdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev buffer alignment patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-fbdev-buffer-align.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-fbdev-buffer-align.patch && \
			echo "$(GREEN)✓ Fbdev buffer alignment patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL fbdev buffer alignment patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_observer.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer debug info patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_observer_debug.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_observer_debug.patch && \
			echo "$(GREEN)✓ Observer debug info patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL observer debug info patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_observer_remove_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer remove NULL guard patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_observer_remove_null_guard.patch && \
		echo "$(GREEN)✓ Observer remove NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL observer remove NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'lv_subject_set_int: subject is NULL' $(LVGL_DIR)/src/core/lv_observer.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL observer subject NULL guards patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_observer_null_guards.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_observer_null_guards.patch && \
			echo "$(GREEN)✓ Observer subject NULL guards patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL observer subject NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/widgets/slider/lv_slider.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL slider scroll chain patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_slider_scroll_chain.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_slider_scroll_chain.patch && \
			echo "$(GREEN)✓ Slider scroll chain patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL slider scroll chain patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/stdlib/clib/lv_string_clib.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL strdup NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-strdup-null-guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-strdup-null-guard.patch && \
			echo "$(GREEN)✓ strdup NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL strdup NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/blend/lv_draw_sw_blend.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_blend_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_blend_null_guard.patch && \
			echo "$(GREEN)✓ Blend NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'Clip blend_area to the layer' $(LVGL_DIR)/src/draw/sw/blend/lv_draw_sw_blend.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend buffer bounds clip patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_blend_buf_bounds_clip.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_blend_buf_bounds_clip.patch && \
			echo "$(GREEN)✓ Blend buffer bounds clip patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend buffer bounds clip patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blend color NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_blend_color_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_blend_color_null_guard.patch && \
			echo "$(GREEN)✓ Blend color NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blend color NULL guard patch already applied$(RESET)"; \
	fi
	# Sentinel is `apply --check` rather than "is lv_draw.c dirty?": this patch no
	# longer touches lv_draw.c (see patches/README.md), and lvgl_draw_render_thread_acquire
	# does, so a file-dirty test here would report "already applied" when it is not.
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-fix-signed-unsigned-draw-coords.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw-area clip patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-fix-signed-unsigned-draw-coords.patch && \
		echo "$(GREEN)✓ Draw-area clip patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL draw-area clip patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_draw_render_thread_acquire.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL render-thread acquire/release barrier patch (ARM64 layer-buffer UAF)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_draw_render_thread_acquire.patch && \
		echo "$(GREEN)✓ Render-thread acquire/release barrier patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL render-thread acquire/release barrier patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_letter.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL label draw NULL font guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_draw_sw_label_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_draw_sw_label_null_guard.patch && \
			echo "$(GREEN)✓ Label draw NULL font guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL label draw NULL font guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/drm/lv_linux_drm.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM flush rotation patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-drm-flush-rotation.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-drm-flush-rotation.patch && \
			echo "$(GREEN)✓ DRM flush rotation patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM flush rotation patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/display/drm/lv_linux_drm_egl.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM EGL getters patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-drm-egl-getters.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-drm-egl-getters.patch && \
			echo "$(GREEN)✓ DRM EGL getters patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM EGL getters patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'lv_linux_drm_set_preferred_mode' $(LVGL_DIR)/src/drivers/display/drm/lv_linux_drm.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM preferred mode patch (#766)...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-drm-preferred-mode.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-drm-preferred-mode.patch && \
			echo "$(GREEN)✓ DRM preferred mode patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM preferred mode patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q 'drmSetMaster' $(LVGL_DIR)/src/drivers/display/drm/lv_linux_drm.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM set-master patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-drm-set-master.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-drm-set-master.patch && \
			echo "$(GREEN)✓ DRM set-master patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL DRM set-master patch already applied$(RESET)"; \
	fi
# Sentinel is `apply --check`, not a file-dirty test: three other patches already
# dirty lv_linux_drm.c, so "is the file modified?" answers the wrong question here
# (see patches/README.md, "Apply-check sentinels").
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-drm-mmap64.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM 64-bit mmap offset patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-drm-mmap64.patch && \
		echo "$(GREEN)✓ DRM 64-bit mmap offset patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL DRM 64-bit mmap offset patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_refr.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL refr reshape NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_refr_reshape_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_refr_reshape_null_guard.patch && \
			echo "$(GREEN)✓ Refr reshape NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL refr reshape NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_img.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL img goto_xy NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_img_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_img_null_guard.patch && \
			echo "$(GREEN)✓ Img goto_xy NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL img goto_xy NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/widgets/image/lv_image.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL image-warn obj-name patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_img_warn_obj_name.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_img_warn_obj_name.patch && \
			echo "$(GREEN)✓ Image-warn obj-name patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL image-warn obj-name patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/sw/lv_draw_sw_blur.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL blur goto_xy NULL guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_blur_null_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_blur_null_guard.patch && \
			echo "$(GREEN)✓ Blur goto_xy NULL guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL blur goto_xy NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_obj_pos_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj_pos NULL guards patch (blur_walk_cb + layout_update_core)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_obj_pos_null_guards.patch && \
		echo "$(GREEN)✓ obj_pos NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj_pos NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_grid_update_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL grid_update freed-container guard patch (#973)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_grid_update_guard.patch && \
		echo "$(GREEN)✓ grid_update guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL grid_update guard patch already applied$(RESET)"; \
	fi
	$(Q)if grep -q 'LV_ASSERT_MALLOC(draw_buf)' $(LVGL_DIR)/src/draw/lv_draw_buf.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_buf OOM guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_draw_buf_oom_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_draw_buf_oom_guard.patch && \
			echo "$(GREEN)✓ Draw_buf OOM guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL draw_buf OOM guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/drivers/evdev/lv_evdev.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL evdev Protocol-A touch release patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-evdev-protocol-a.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-evdev-protocol-a.patch && \
			echo "$(GREEN)✓ Evdev Protocol-A touch release patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL evdev Protocol-A touch release patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/draw/lv_draw_arc.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL arc draw guard patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_arc_draw_guard.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_arc_draw_guard.patch && \
			echo "$(GREEN)✓ Arc draw guard patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL arc draw guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_arc_subject_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL arc subject NULL guard patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_arc_subject_null_guard.patch && \
		echo "$(GREEN)✓ Arc subject NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL arc subject NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_draw_sw_img_buf_height_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_sw_img buf_h guard patch (upstream ca18403)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_draw_sw_img_buf_height_guard.patch && \
		echo "$(GREEN)✓ draw_sw_img buf_h guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL draw_sw_img buf_h guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_lodepng_bpp_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL lodepng bit-depth guard (16-bit PNG heap overflow)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_lodepng_bpp_guard.patch && \
		echo "$(GREEN)✓ lodepng bit-depth guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL lodepng bit-depth guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_drm_egl_render_mode_fix.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL DRM EGL render mode fix (upstream ce112eb)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_drm_egl_render_mode_fix.patch && \
		echo "$(GREEN)✓ DRM EGL render mode fix applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL DRM EGL render mode fix already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_texture_cache_null_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL texture cache NULL guard patch (upstream ec053a0)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_texture_cache_null_guard.patch && \
		echo "$(GREEN)✓ Texture cache NULL guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL texture cache NULL guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_draw_sdl_stride_fix.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL draw_sdl aligned stride fix...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_draw_sdl_stride_fix.patch && \
		echo "$(GREEN)✓ draw_sdl stride fix applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL draw_sdl stride fix already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_display_sync_cb.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL display sync callback patch (upstream 4170bcb)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_display_sync_cb.patch && \
		echo "$(GREEN)✓ Display sync callback patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL display sync callback patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_obj_delete_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj delete NULL guards patch (event depth guard + mark_deleted + obj_destructor + obj_delete_core)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_obj_delete_null_guards.patch && \
		echo "$(GREEN)✓ obj delete NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj delete NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_obj_delete_async_dedup.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj delete async dedup patch (dedup + UAF guard + diagnostics)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_obj_delete_async_dedup.patch && \
		echo "$(GREEN)✓ obj delete async dedup patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj delete async dedup patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_obj_get_screen_cycle_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj_get_screen cycle guard patch (cap parent-walk depth to 128)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_obj_get_screen_cycle_guard.patch && \
		echo "$(GREEN)✓ obj_get_screen cycle guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj_get_screen cycle guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_async_del_crumb.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL async-delete breadcrumb patch (#840/#906 sync+async diagnostic)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_async_del_crumb.patch && \
		echo "$(GREEN)✓ async-delete breadcrumb patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL async-delete breadcrumb patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_translation_warn_once.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL translation warn-once patch (missing-language warning once per language)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_translation_warn_once.patch && \
		echo "$(GREEN)✓ translation warn-once patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL translation warn-once patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/widgets/label/lv_label.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL label text transform patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_label_text_transform.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_label_text_transform.patch && \
			echo "$(GREEN)✓ Label text transform patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL label text transform patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl-sw-draw-wait-for-finish.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL SW draw wait_for_finish + NULL guard patch (#739)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl-sw-draw-wait-for-finish.patch && \
		echo "$(GREEN)✓ SW draw wait_for_finish patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL SW draw wait_for_finish patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/core/lv_obj_event.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event crash-diagnostic hook patch...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_crash_hook.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_crash_hook.patch && \
			echo "$(GREEN)✓ Event crash-diagnostic hook patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL event crash-diagnostic hook patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_mark_deleted_defensive.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL lv_event_mark_deleted defensive bail patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_mark_deleted_defensive.patch && \
		echo "$(GREEN)✓ lv_event_mark_deleted defensive bail patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL lv_event_mark_deleted defensive bail patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_pop_unwind_safe.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event-pop unwind-safe patch (RPHAV9T7 / L081 root cause)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_pop_unwind_safe.patch && \
		echo "$(GREEN)✓ event-pop unwind-safe patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL event-pop unwind-safe patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_dispatch_depth_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL event-dispatch-depth guard (cluster:pstat-async-delete / #906)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_dispatch_depth_guard.patch && \
		echo "$(GREEN)✓ event-dispatch-depth guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL event-dispatch-depth guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_stack_array.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL #907 array-backed event stack (replaces e->prev linked list)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_stack_array.patch && \
		echo "$(GREEN)✓ #907 array-backed event stack patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL #907 array-backed event stack patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_event_dispatch_cb_guard.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL dispatch-cb bounds gate + widget identity (3XNZQB2R)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_event_dispatch_cb_guard.patch && \
		echo "$(GREEN)✓ dispatch-cb guard + widget identity patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL dispatch-cb guard patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_obj_event_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL obj-event NULL guards (VHTR49QJ — recoverable bail + telemetry)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_obj_event_null_guards.patch && \
		echo "$(GREEN)✓ obj-event NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL obj-event NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_style_null_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL style NULL guards patch (null style pointers in transitions/cache)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_style_null_guards.patch && \
		echo "$(GREEN)✓ Style NULL guards patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL style NULL guards patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) diff --quiet src/layouts/flex/lv_flex.c 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL flex hidden+grow gap fix (upstream #9897 backport)...$(RESET)"; \
		if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_flex_hidden_grow_gap.patch 2>/dev/null; then \
			git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_flex_hidden_grow_gap.patch && \
			echo "$(GREEN)✓ Flex hidden+grow gap fix applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ LVGL flex hidden+grow gap fix already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_check_arg_backport.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL LV_CHECK_ARG backport patch (master macro for v9.5.0; drop at upgrade)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_check_arg_backport.patch && \
		echo "$(GREEN)✓ LV_CHECK_ARG backport patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL LV_CHECK_ARG backport patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_fbdev_arg_guards.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL fbdev arg-guard + log-order patch (uses backported LV_CHECK_ARG)...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_fbdev_arg_guards.patch && \
		echo "$(GREEN)✓ Fbdev arg-guard patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL fbdev arg-guard patch already applied$(RESET)"; \
	fi
	$(ECHO) "$(CYAN)Checking libhv patches...$(RESET)"
	$(Q)if git -C $(LIBHV_DIR) diff --quiet Makefile.in 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv OpenSSL/static build hook patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-openssl-static-link.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-openssl-static-link.patch && \
			echo "$(GREEN)✓ libhv OpenSSL/static build hook patch applied$(RESET)"; \
		else \
			echo "$(YELLOW)⚠ Cannot apply patch (already applied or conflicts)$(RESET)"; \
		fi \
	else \
		echo "$(GREEN)✓ libhv OpenSSL/static build hook patch already applied$(RESET)"; \
	fi
	$(Q)if git -C $(LIBHV_DIR) diff --quiet http/client/requests.h 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv streaming upload patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-streaming-upload.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-streaming-upload.patch && \
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
	$(Q)if ! grep -q "dns_resolv_resolve" "$(LIBHV_DIR)/base/hsocket.c" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv DNS resolver fallback patch...$(RESET)"; \
		rm -f "$(LIBHV_DIR)/base/dns_resolv.c" "$(LIBHV_DIR)/base/dns_resolv.h"; \
		git -C $(LIBHV_DIR) checkout -- base/hsocket.c 2>/dev/null || true; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-dns-resolver-fallback.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-dns-resolver-fallback.patch && \
			echo "$(GREEN)✓ DNS resolver fallback patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply DNS resolver fallback patch (conflicts) — embedded DNS will be BROKEN$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv DNS resolver fallback patch already applied$(RESET)"; \
	fi
	@# Sentinel is the NEWEST marker in this patch, not the oldest. A tree that
	@# already carries an earlier revision of the patch must fail loudly here —
	@# matching an old marker would report "already applied" and silently drop
	@# the newer hunks, and libhv headers are -isystem so nothing rebuilds to
	@# reveal it. The fix for that red line is `make reapply-patches`.
	@#
	@# A failed apply must `exit 1` rather than warn and carry on. The recipe
	@# ends in `touch $@`, so a warning-only branch stamps the tree as fully
	@# patched: the red line scrolls past once and every later build reports
	@# "Nothing to be done for 'apply-patches'". That is how the #1212 null-hloop
	@# guard sat missing from this tree for hours while `make test` — which skips
	@# apply-patches entirely — kept building a binary that segfaulted on the
	@# regression test written to catch exactly that.
	$(Q)if ! grep -q "reconn_timer_id" "$(LIBHV_DIR)/evpp/TcpClient.h" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv TcpClient reconnect resilience patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-tcpclient-reconnect-resilience.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-tcpclient-reconnect-resilience.patch && \
			echo "$(GREEN)✓ libhv TcpClient reconnect resilience patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply TcpClient reconnect patch — run 'make reapply-patches'. Until then a pending auto-reconnect can fault in createsocket() during teardown (#1212)$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv TcpClient reconnect resilience patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q "saved_reconn_valid_" "$(LIBHV_DIR)/http/client/WebSocketClient.cpp" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv WebSocket backoff patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-websocket-backoff-on-upgrade.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-websocket-backoff-on-upgrade.patch && \
			echo "$(GREEN)✓ libhv WebSocket backoff patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply WebSocket backoff patch (conflicts) — a failed WS upgrade will reconnect at 5Hz$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv WebSocket backoff patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q "PATCH NOTE(helixscreen)" "$(LIBHV_DIR)/cpputil/hthreadpool.h" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv HThreadPool wait() lock patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-hthreadpool-wait-lock.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-hthreadpool-wait-lock.patch && \
			echo "$(GREEN)✓ libhv HThreadPool wait() lock patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply HThreadPool wait() patch — run 'make reapply-patches'. Until then wait() races a worker's pop_front() (nightly TSAN via ThumbnailProcessor::wait_for_completion)$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv HThreadPool wait() lock patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q "PATCH NOTE(helixscreen)" "$(LIBHV_DIR)/base/hlog.c" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv hlog localtime_r patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-hlog-thread-safe-localtime.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-hlog-thread-safe-localtime.patch && \
			echo "$(GREEN)✓ libhv hlog localtime_r patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply hlog localtime_r patch — run 'make reapply-patches'. Until then every logging thread races on localtime()'s shared struct tm and on tzset's TZ string (nightly TSAN, two reports in logger_print)$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv hlog localtime_r patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q "PATCH NOTE(helixscreen)" "$(LIBHV_DIR)/http/HttpMessage.h" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv HttpRequest cancel atomic patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-http-request-cancel-atomic.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-http-request-cancel-atomic.patch && \
			echo "$(GREEN)✓ libhv HttpRequest cancel atomic patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply HttpRequest cancel patch — run 'make reapply-patches'. Until then CameraStream::stop() races the stream thread's ParseUrl() (nightly TSAN in HttpRequest::Cancel)$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv HttpRequest cancel atomic patch already applied$(RESET)"; \
	fi
	$(Q)if ! grep -q "install the TcpClient-level channel callbacks" "$(LIBHV_DIR)/http/client/WebSocketClient.cpp" 2>/dev/null; then \
		echo "$(YELLOW)→ Applying libhv WebSocketClient install-once callbacks patch...$(RESET)"; \
		if git -C $(LIBHV_DIR) apply --check $(PATCH_DIR)/libhv-websocket-open-install-once.patch 2>/dev/null; then \
			git -C $(LIBHV_DIR) apply $(PATCH_DIR)/libhv-websocket-open-install-once.patch && \
			echo "$(GREEN)✓ libhv WebSocketClient install-once patch applied$(RESET)"; \
		else \
			echo "$(RED)✗ Cannot apply WebSocketClient install-once patch — run 'make reapply-patches'. Until then concurrent connect() can corrupt the heap (SIGABRT free(): invalid next size)$(RESET)"; \
			exit 1; \
		fi \
	else \
		echo "$(GREEN)✓ libhv WebSocketClient install-once patch already applied$(RESET)"; \
	fi
	$(Q)if [ -d "$(LIBHV_DIR)/include/hv" ]; then \
		for h in evpp/TcpClient.h http/client/WebSocketClient.h cpputil/hthreadpool.h http/HttpMessage.h; do \
			base=$$(basename $$h); \
			if ! diff -q "$(LIBHV_DIR)/$$h" "$(LIBHV_DIR)/include/hv/$$base" >/dev/null 2>&1; then \
				echo "$(YELLOW)→ Syncing patched $$base to include/hv/$(RESET)"; \
				cp "$(LIBHV_DIR)/$$h" "$(LIBHV_DIR)/include/hv/$$base" && \
				echo "$(GREEN)✓ Patched $$base synced$(RESET)"; \
			fi; \
		done; \
	fi
	@touch $@

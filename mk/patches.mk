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
	src/draw/lv_draw_private.h \
	src/draw/lv_draw_buf.c \
	src/draw/sw/lv_draw_sw_mask_rect.c \
	src/draw/sw/lv_draw_sw_letter.c \
	src/draw/sw/lv_draw_sw_img.c \
	src/draw/sw/lv_draw_sw_blur.c \
	src/drivers/display/drm/lv_linux_drm.c \
	src/drivers/display/drm/lv_linux_drm.h \
	src/drivers/display/drm/lv_linux_drm_egl.c \
	src/drivers/evdev/lv_evdev.c \
	src/drivers/evdev/lv_evdev.h \
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
	src/indev/lv_indev.c \
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

# The patched sources themselves, for use as build prerequisites, so an applied
# patch invalidates the objects built from it and not only the stamp.
LIBHV_PATCHED_SRCS := $(wildcard $(addprefix $(LIBHV_DIR)/,$(LIBHV_PATCHED_FILES)))

# ============================================================================
# THIRD-PARTY HEADER ABI STAMP
# ============================================================================
# Our objects reach the patched LVGL and libhv headers through -isystem, and
# DEPFLAGS is -MMD, which by design leaves system headers out of the generated
# .d files. A patch that adds a member to a shared type therefore moves every
# member after it without invalidating a single .o. Two of them do exactly
# that: hv::TcpClientEventLoopTmpl and hv::WebSocketClient.
#
# lib/ is shared between worktrees while build/ is not, so those headers also
# change under a build that is already in flight. The objects compiled before
# the change and the ones compiled after then disagree about where a member
# lives. Nothing complains: the link succeeds, and a std::mutex read at the
# wrong offset locks bytes that were never a mutex. macOS libc++ checks the
# mutex signature and throws EINVAL; glibc accepts a zeroed pthread_mutex_t as
# a valid unlocked one, so the same tree passes on Linux and aborts on a Mac.
#
# The stamp holds a hash of those headers' CONTENT, not their mtimes:
# reapply-patches rewrites the files whether or not the bytes change, and only
# a real change should cost a rebuild. cksum is POSIX, so this also works on
# the BusyBox and Buildroot hosts.
#
# The list is spelled out rather than globbed. libhv's own build installs the
# include/hv/ copies our -isystem path resolves to, so on a checkout that has
# not been built they do not exist yet, and a glob would silently stop watching
# the very headers the objects compile against.
ABI_HEADERS := \
	$(addprefix $(LIBHV_DIR)/,$(filter %.h,$(LIBHV_PATCHED_FILES))) \
	$(addprefix $(LIBHV_DIR)/include/hv/,$(notdir $(filter %.h,$(LIBHV_PATCHED_FILES)))) \
	$(addprefix $(LVGL_DIR)/,$(filter %.h,$(LVGL_PATCHED_FILES)))
ABI_STAMP := $(BUILD_DIR)/.thirdparty-abi

# Defined here rather than beside PATCHES_STAMP because the hash below needs it:
# on a checkout whose patches are not applied yet, the headers on disk are
# upstream's, and the layout the compilers will see is those headers plus these
# patches. Two patch sets over one upstream tree must not share a hash.
PATCH_FILES := $(wildcard patches/*.patch)

# /dev/null leads the list so cat always has a file: given no arguments at all
# it reads standard input instead, and make blocks there forever on a terminal.
ABI_HASH_CMD = cat /dev/null $(ABI_HEADERS) $(PATCH_FILES) 2>/dev/null | cksum
ABI_HASH := $(shell $(ABI_HASH_CMD))

# The stamp makes make decide to recompile; it does not make the compiler
# produce a different object. ccache sits between the two, and in depend mode
# it keys an entry on the source plus the files -MMD names - which is exactly
# the set that omits these -isystem headers. Make reruns the compile, ccache
# answers it from the entry built against the previous layout, and the stamp
# buys nothing.
#
# Carrying the hash as a -D closes that, because ccache hashes the command line
# in every mode. Nothing reads HELIX_TP_ABI; its only job is to be part of the
# key. cksum prints a checksum and a byte count, so the separating space is
# folded to an underscore to keep the value a single token.
ABI_EMPTY :=
ABI_SPACE := $(ABI_EMPTY) $(ABI_EMPTY)
ABI_DEFINE := -DHELIX_TP_ABI=$(subst $(ABI_SPACE),_,$(strip $(ABI_HASH)))

# Every rule carrying $(ABI_STAMP) draws its flags from one of these four, so
# they move together or a path recompiles into the same stale cache entry.
CFLAGS += $(ABI_DEFINE)
CXXFLAGS += $(ABI_DEFINE)
SUBMODULE_CFLAGS += $(ABI_DEFINE)
SUBMODULE_CXXFLAGS += $(ABI_DEFINE)

# Written at parse time so the stamp is in place before the first compile, and
# only by the make that started this BUILD_DIR's build.
#
# A build re-reads this file over and over: `all` re-invokes itself to fix up
# -j, libhv and SDL2 and the translations come through sub-makes, and the
# cross-compile targets re-invoke with a PLATFORM_TARGET. Letting a descendant
# re-stamp would record a header a second worktree rewrote mid-build as if it
# had been there from the start, and the link guard below would then find
# nothing to complain about. The one below that must still stamp is the
# cross-compile re-invocation, which is a different BUILD_DIR and so a build of
# its own - which is what this names, rather than depth.
$(shell mkdir -p $(BUILD_DIR); \
	{ [ "$(ABI_STAMPED_FOR)" = "$(BUILD_DIR)" ] && [ -f $(ABI_STAMP) ]; } && exit 0; \
	[ "$$(cat $(ABI_STAMP) 2>/dev/null)" = "$(ABI_HASH)" ] \
		|| printf "%s" "$(ABI_HASH)" > $(ABI_STAMP))
export ABI_STAMPED_FOR := $(BUILD_DIR)

# Record the headers as they stand now. Applying the patches and installing
# libhv's headers both change them, and both happen inside the build, after the
# parse-time value above was computed and before the first object that sees the
# result is compiled, so the recipes that make those changes call this once
# they are done, and the stamp names the layout the objects actually get.
#
# Rewritten only when the value moves: every object depends on this file, so a
# fresh mtime carrying an unchanged value would buy a full rebuild for nothing.
define record_abi_stamp
	$(Q)mkdir -p $(BUILD_DIR); \
	abi_now="$$($(ABI_HASH_CMD))"; \
	[ "$$(cat $(ABI_STAMP) 2>/dev/null)" = "$$abi_now" ] \
		|| printf "%s" "$$abi_now" > $(ABI_STAMP)
endef

# Fail a link whose objects were not all compiled against the headers present
# now. The reference is the stamp rather than the parse-time value, because the
# build changes these headers itself and re-records the stamp when it does.
# What is left over is a change nothing in this build made: a second worktree
# re-patching a shared lib/, which is exactly what has to fail here.
define check_abi_unchanged
	$(Q)if [ "$$($(ABI_HASH_CMD))" != "$$(cat $(ABI_STAMP) 2>/dev/null)" ]; then \
		echo "$(RED)$(BOLD)Third-party headers changed while this build was running.$(RESET)"; \
		echo "$(YELLOW)  lib/ is shared between worktrees. Objects compiled before the$(RESET)"; \
		echo "$(YELLOW)  change disagree with the ones after about member offsets, and$(RESET)"; \
		echo "$(YELLOW)  the binary would misbehave at runtime rather than fail here.$(RESET)"; \
		echo "$(YELLOW)  Re-run this target once the other tree is done.$(RESET)"; \
		exit 1; \
	fi
endef

# ============================================================================
# PATCH STAMP FILE - Skip checking if patches haven't changed
# ============================================================================
# The stamp file tracks when patches were last verified/applied.
# Re-check only when: patch files change, submodule HEAD changes, or stamp missing.
PATCHES_STAMP := $(BUILD_DIR)/.patches-applied

# Absolute path to this repo's patches/. It MUST be absolute. The apply rules
# below run as `git -C $(LVGL_DIR) apply <path>`, and git resolves that path
# after chdir'ing into the submodule, so a relative `../../patches/` names
# whatever sits two levels above the submodule's real location rather than the
# patches/ of the tree make is running in.
PATCH_DIR := $(abspath patches)

# Patches applied outside this file. Keep this list empty if you can; an entry
# here means something applies the patch by hand, so nothing verifies it.
# libnl-socket-time-include.patch (65d0ba93a, GCC 14+ libnl build fix) has no
# applier anywhere in the tree — it is dead, kept only pending a decision to
# either wire it up or delete it.
PATCH_EXEMPT := libnl-socket-time-include.patch

# Submodule HEAD files - the stamp is stale once a submodule is moved to another
# revision. Ask each submodule where its own git dir is rather than composing a
# path: a worktree gives lvgl and libhv a PRIVATE checkout under
# .git/worktrees/<name>/modules/, so a path built from --git-common-dir names the
# MAIN tree's HEAD, which is a different revision on a different schedule.
#
# A pre-commit hook exports GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE pointing at
# the superproject, and those leak into any git spawned under it — `git -C
# lib/lvgl rev-parse` would then answer with the superproject's git dir, whose
# HEAD moves on every commit. Scrub them so the question is answered by the -C
# path.
# In Docker/non-git contexts (rsync'd source), there is no git dir — that's fine,
# patches will be re-checked based on patch file changes only.
GIT_NOENV := env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_OBJECT_DIRECTORY git
GIT_DIR := $(shell git rev-parse --git-dir 2>/dev/null || echo ".git")
LVGL_GIT_DIR := $(shell $(GIT_NOENV) -C $(LVGL_DIR) rev-parse --absolute-git-dir 2>/dev/null)
LIBHV_GIT_DIR := $(shell $(GIT_NOENV) -C $(LIBHV_DIR) rev-parse --absolute-git-dir 2>/dev/null)
LVGL_HEAD := $(if $(LVGL_GIT_DIR),$(wildcard $(LVGL_GIT_DIR)/HEAD))
LIBHV_HEAD := $(if $(LIBHV_GIT_DIR),$(wildcard $(LIBHV_GIT_DIR)/HEAD))

# The record of WHICH patch revision is currently applied, written by
# check_patch_drift.py --write-stamp after the apply blocks below run. It lives
# in each submodule's git directory, beside the checkout it describes, and is
# named the way the gate names it: the gate asks the submodule for its own
# --absolute-git-dir, so make has to ask the same question rather than compose
# a path, or it watches a file nothing ever writes.
#
# It has to be a prerequisite of the stamp, because it is the only prerequisite
# that moves when ANOTHER worktree re-patches lib/. The others are this tree's
# own patches/ and submodule HEADs, and a foreign apply touches neither: the
# verification below is then skipped, every apply guard greps a marker string
# that the foreign revision also contains and reports "already applied", and the
# tree compiles against a patch revision that is not the one in its patches/.
# That is silent, and on a branch whose patches differ it is a different binary
# than the branch describes.
#
# Named unconditionally rather than globbed: a clean checkout has no record yet,
# and a glob would drop the path make is meant to watch for the moment one
# appears. In Docker/non-git contexts there is no git dir to name at all.
ifneq ($(LVGL_GIT_DIR),)
LVGL_APPLIED_STAMP := $(LVGL_GIT_DIR)/helix-patches-applied.json
endif
ifneq ($(LIBHV_GIT_DIR),)
LIBHV_APPLIED_STAMP := $(LIBHV_GIT_DIR)/helix-patches-applied.json
endif
APPLIED_STAMPS := $(LVGL_APPLIED_STAMP) $(LIBHV_APPLIED_STAMP)

# Hashed rather than depended on directly, for the same reason as ABI_STAMP: the
# record is rewritten on every apply whether or not its contents move, and a
# same-branch worktree re-applying an identical patch set must not cost every
# other worktree a full rebuild. The JSON is derived purely from file hashes -
# no timestamp - so identical patch sets produce identical bytes.
#
# /dev/null leads the list for the same reason as the ABI hash: with no
# arguments cat reads standard input, and a checkout that has never been
# patched has no record for it to read.
APPLIED_STAMP_ID := $(BUILD_DIR)/.patches-applied-id
APPLIED_STAMP_HASH_CMD = cat /dev/null $(APPLIED_STAMPS) 2>/dev/null | cksum
APPLIED_STAMP_HASH := $(shell $(APPLIED_STAMP_HASH_CMD))

$(shell mkdir -p $(BUILD_DIR); \
	[ "$$(cat $(APPLIED_STAMP_ID) 2>/dev/null)" = "$(APPLIED_STAMP_HASH)" ] \
		|| printf "%s" "$(APPLIED_STAMP_HASH)" > $(APPLIED_STAMP_ID))

# The apply recipe writes the record it is a proxy for, so it re-reads it once
# the apply is done. Without that the id lags a build behind and the whole
# verification runs a second time for nothing.
define record_applied_stamp_id
	$(Q)mkdir -p $(BUILD_DIR); \
	id_now="$$($(APPLIED_STAMP_HASH_CMD))"; \
	[ "$$(cat $(APPLIED_STAMP_ID) 2>/dev/null)" = "$$id_now" ] \
		|| printf "%s" "$$id_now" > $(APPLIED_STAMP_ID)
endef

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
	@# The drift stamp describes a PATCHED checkout. Everything above just put
	@# the checkout back to pristine, so the stamp now describes nothing; left
	@# in place it would report every restored file as "changed since apply" and
	@# block the force-apply-patches half of reapply-patches.
	$(Q)if command -v python3 >/dev/null 2>&1; then \
		python3 scripts/check_patch_drift.py --clear-stamp || true; \
	fi
	$(ECHO) "$(GREEN)✓ All patches reset$(RESET)"

# Force reapply all patches (reset first, then apply)
#
# Sub-makes rather than prerequisites: `make -jN reapply-patches` is free to run
# two prerequisites of the same target concurrently, and here the second one
# rewrites the very files the first one is restoring. Recipe lines are ordered
# unconditionally.
reapply-patches:
	$(Q)$(MAKE) reset-patches
	$(Q)$(MAKE) force-apply-patches
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
$(PATCHES_STAMP): $(PATCH_FILES) $(LVGL_HEAD) $(LIBHV_HEAD) $(APPLIED_STAMP_ID)
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
	@# Every guard below asks "is this file already dirty?", never "is it dirty
	@# with the CURRENT revision of this patch". So editing an applied patch is
	@# a no-op for anyone whose submodule carries the old one, and the guard
	@# reports it as "already applied" (86560d156: lv_evdev_get_last_raw landed
	@# in the patch, never in lib/lvgl, and every device cross-build broke while
	@# the desktop suite stayed green). check_patch_drift.py compares a stamp
	@# written after the last apply against the patches on the shelf.
	@#
	@# This must run BEFORE the apply blocks and BEFORE the stamp is rewritten
	@# below: reaching the rewrite with an edited-but-unapplied patch would
	@# record the new hash over an old application and switch the gate off.
	@# --pre-apply lets a brand new patch through, since its guard tests a
	@# marker that is not in the tree and the blocks below really will apply it.
	$(Q)if command -v python3 >/dev/null 2>&1; then \
		python3 scripts/check_patch_drift.py --pre-apply; \
	else \
		echo "$(YELLOW)⚠ python3 not found - patch drift check skipped$(RESET)"; \
	fi
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
	$(Q)if git -C $(LVGL_DIR) apply --check $(PATCH_DIR)/lvgl_indev_delete_cancels_anim.patch 2>/dev/null; then \
		echo "$(YELLOW)→ Applying LVGL indev-delete animation cancel patch...$(RESET)"; \
		git -C $(LVGL_DIR) apply $(PATCH_DIR)/lvgl_indev_delete_cancels_anim.patch && \
		echo "$(GREEN)✓ indev-delete animation cancel patch applied$(RESET)"; \
	else \
		echo "$(GREEN)✓ LVGL indev-delete animation cancel patch already applied$(RESET)"; \
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
	@# Record what is now applied: the sha256 of every patch file, and of every
	@# submodule file those patches touch. The first catches an edited patch on
	@# the next build, the second catches the submodule being reset or updated
	@# out from under it. Written into the submodule's own git dir, so it tracks
	@# the checkout it describes (shared with every worktree symlinked at lib/)
	@# and never shows up as untracked noise in `git status`.
	$(Q)if command -v python3 >/dev/null 2>&1; then \
		python3 scripts/check_patch_drift.py --write-stamp; \
	fi
	$(call record_applied_stamp_id)
	@# The headers just moved from upstream's bytes to ours. Everything compiled
	@# from here on sees the patched layout, so that is what the link must be
	@# checked against.
	$(call record_abi_stamp)
	@touch $@

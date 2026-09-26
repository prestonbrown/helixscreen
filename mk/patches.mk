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
	src/drivers/display/drm/lv_linux_drm_egl_private.h \
	src/drivers/opengles/lv_opengles_egl.c \
	src/drivers/opengles/lv_opengles_egl.h \
	src/drivers/opengles/lv_opengles_driver.c \
	src/drivers/opengles/lv_opengles_driver.h \
	src/drivers/opengles/assets/lv_opengles_shader.c \
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
	src/core/lv_obj.h \
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
	src/libs/lodepng/lv_lodepng.c \
	src/libs/bin_decoder/lv_bin_decoder.c \
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
# below hand each patch to scripts/apply_submodule_patch.sh, which runs
# `git -C <submodule> apply <path>`; git resolves that path after chdir'ing
# into the submodule, so a relative `../../patches/` names whatever sits two
# levels above the submodule's real location rather than the patches/ of the
# tree make is running in.
PATCH_DIR := $(abspath patches)

# The one apply verdict shared by every stanza below. Centralised in a script
# because a bare `git apply --check` else-branch cannot tell "already applied"
# from "drifted and will never apply" — and after a sibling patch moves shared
# context, a correctly applied patch passes neither a forward nor a reverse
# check, so only a from-clean run can judge. reapply-patches sets
# HELIX_PATCHES_FROM_CLEAN=1 to make "will not apply from clean" fatal there.
APPLY_PATCH := bash scripts/apply_submodule_patch.sh
export HELIX_PATCHES_FROM_CLEAN

# The recipe-start guard below writes "1" or "0" here, and the helper reads it
# back on every stanza: the flag claims the run started from pristine
# submodules, and the claim is verified once, before any stanza has dirtied a
# shared file. Per-patch checks cannot answer it — after the first apply the
# tree is legitimately dirty for the rest of the recipe.
HELIX_FROM_CLEAN_SENTINEL := $(BUILD_DIR)/.patches-from-clean
export HELIX_FROM_CLEAN_SENTINEL

# Presence, not applicability. Each wired patch names one line in
# mk/patch-markers.tsv that it adds (or removes) and upstream never contained,
# and every build greps the checkout for it. The apply verdict above needs git
# and stays ambiguous while sibling patches share files; a marker is a plain
# text search, so it also works in a docker tree rsynced from a worktree, where
# the submodules are not git repositories at all. 'make regen-patch-markers'
# rederives the table; the recorded patch hash makes a changed patch fail
# loudly instead of silently checking a marker that no longer exists.
PATCH_MARKERS_TSV := mk/patch-markers.tsv
PATCH_MARKER_STAMP := $(BUILD_DIR)/.patch-markers-verified
PATCH_MARKER_CHECK := python3 scripts/check_patch_markers.py \
	--mk mk/patches.mk --tsv $(PATCH_MARKERS_TSV) --patch-dir $(PATCH_DIR) \
	--lvgl $(LVGL_DIR) --libhv $(LIBHV_DIR)
# wildcard, not the bare list: a patch can CREATE the file a marker lives in
# (libhv's dns_resolv.c), and on an unpatched tree - a fresh clone before its
# first apply, or right after reset-patches - a plain prerequisite that does
# not exist yet is a graph error, not a missing patch. The checker reads the
# table directly, so a file the wildcard drops this parse is still verified;
# it just becomes an mtime trigger one build later. Same shape as
# LIBHV_PATCHED_SRCS below.
PATCH_MARKER_DEPS := $(wildcard $(shell awk -F'\t' 'NR>1 && !seen[$$4"/"$$5]++ {printf "%s/%s ", ($$4=="LVGL_DIR"?"$(LVGL_DIR)":"$(LIBHV_DIR)"), $$5}' $(PATCH_MARKERS_TSV) 2>/dev/null))

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
	@# Same for the header the EGL partial upload patch creates.
	$(Q)rm -f $(LVGL_DIR)/src/drivers/display/drm/lv_linux_drm_egl_upload.h
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
	$(Q)$(MAKE) force-apply-patches HELIX_PATCHES_FROM_CLEAN=1
	$(ECHO) "$(GREEN)✓ All patches reapplied$(RESET)"

# apply-patches: File-based target that skips if stamp is current
# Dependencies: patch files + submodule HEADs (re-run if submodule updated)
apply-patches: $(PATCHES_STAMP)

# Force patch application (used by reapply-patches)
.PHONY: force-apply-patches
force-apply-patches:
	@rm -f $(PATCHES_STAMP)
	@$(MAKE) $(PATCHES_STAMP)

# The marker stamp re-verifies whenever the patch stamp, the table, or any file
# a marker reads is newer, so restoring a submodule file between builds cannot
# hide a missing patch behind a current patch stamp: the restored file is newer
# than this stamp, and the check runs before anything compiles against it.
$(PATCH_MARKER_STAMP): $(PATCHES_STAMP) $(PATCH_MARKERS_TSV) $(PATCH_MARKER_DEPS)
	$(Q)if [ "$${HELIX_MARKER_DERIVING:-0}" = 1 ]; then \
		echo "$(CYAN)ℹ marker gate suspended - deriving a new table$(RESET)"; \
		exit 0; \
	fi
	$(Q)if ! command -v python3 >/dev/null 2>&1; then \
		echo "$(YELLOW)⚠ python3 not found - patch marker check skipped$(RESET)"; \
		exit 0; \
	fi
	$(Q)$(PATCH_MARKER_CHECK)
	$(Q)touch $@

# Rederive the marker table. Reapplies patches first: derivation reads the
# patched checkout, and a checkout missing a patch has no marker to find.
# The reapply suspends the marker gates: the stale table that blocks an
# ordinary apply is the table this target is about to rewrite, so enforcing
# it here would deadlock the very remedy the gate prescribes. Derivation
# rewrites the table with a newer mtime, so verification re-arms itself on
# the next ordinary build.
.PHONY: regen-patch-markers
regen-patch-markers:
	$(Q)HELIX_MARKER_DERIVING=1 $(MAKE) reapply-patches
	$(Q)python3 scripts/gen_patch_markers.py --write --mk mk/patches.mk \
		--tsv $(PATCH_MARKERS_TSV) --patch-dir $(PATCH_DIR) \
		--lvgl $(LVGL_DIR) --libhv $(LIBHV_DIR)

# The actual stamp file - only rebuilt when patches or submodules change
$(PATCHES_STAMP): $(PATCH_FILES) $(LVGL_HEAD) $(LIBHV_HEAD) $(APPLIED_STAMP_ID)
	@mkdir -p $(BUILD_DIR)
	$(ECHO) "$(CYAN)Verifying patch wiring...$(RESET)"
	@# Both directions, because every failure mode here is silent. The apply
	@# stanzas are hand-wired, so a new patches/*.patch with no stanza is
	@# simply never applied, and the build links unpatched submodule code
	@# without a word.
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
	@# The apply stanzas below verify content in both directions, but on an
	@# already-patched tree a patch whose context a later sibling moved reads
	@# the same as one that never applied — only a from-clean run can tell
	@# those apart, and an incremental build does not start from clean. So
	@# between applies, check_patch_drift.py compares a stamp written after the
	@# last apply against the patches on the shelf, which is what catches an
	@# edited patch that no checkout carries yet.
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
	@# HELIX_PATCHES_FROM_CLEAN=1 licenses the fatal verdict below, but only if
	@# the run actually started from pristine submodules. `make clean` deletes
	@# the stamp yet leaves the submodules patched, and on a patched tree a
	@# healthy shared-file patch reads "neither" just like a dead one — so the
	@# claim is settled once, here, before any stanza has dirtied anything, and
	@# a tree that is not pristine downgrades to in-place verdicts with the
	@# remedy named. A submodule git cannot read (non-git Docker rsync) also
	@# downgrades rather than fail the build.
	$(Q)if [ "$(HELIX_PATCHES_FROM_CLEAN)" = "1" ]; then \
		ok=1; \
		for pair in "$(LVGL_DIR)|$(LVGL_PATCHED_FILES) src/misc/lv_check_arg.h" \
		            "$(LIBHV_DIR)|$(LIBHV_PATCHED_FILES)"; do \
			dir=$${pair%%|*}; files=$${pair#*|}; \
			if [ -n "$$($(GIT_NOENV) -C "$$dir" status --porcelain -- $$files 2>/dev/null)" ] || \
			   ! $(GIT_NOENV) -C "$$dir" status --porcelain -- $$files >/dev/null 2>&1; then \
				ok=0; \
				echo "$(YELLOW)⚠ $$dir is not pristine, so this run cannot judge patches from clean — using in-place verdicts. Run 'make reapply-patches' to reset and judge from clean.$(RESET)"; \
			fi; \
		done; \
		printf '%s' "$$ok" > $(HELIX_FROM_CLEAN_SENTINEL); \
	fi
	$(ECHO) "$(CYAN)Checking LVGL patches...$(RESET)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_sdl_window.patch "LVGL SDL window patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_sdl_sw_android_debug.patch "LVGL SDL SW android debug + blendmode fix patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_theme_breakpoints.patch "LVGL theme breakpoints patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_fbdev_stride_bpp.patch "LVGL fbdev stride bpp detection patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_fbdev_skip_unblank.patch "LVGL fbdev skip-unblank patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-fbdev-bgr-swap.patch "LVGL fbdev BGR swap patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-fbdev-buffer-align.patch "LVGL fbdev buffer alignment patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_observer_debug.patch "LVGL observer debug info patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_observer_remove_null_guard.patch "LVGL observer remove NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_observer_null_guards.patch "LVGL observer subject NULL guards patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_slider_scroll_chain.patch "LVGL slider scroll chain patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-strdup-null-guard.patch "LVGL strdup NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_blend_null_guard.patch "LVGL blend NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_blend_buf_bounds_clip.patch "LVGL blend buffer bounds clip patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_blend_color_null_guard.patch "LVGL blend color NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-fix-signed-unsigned-draw-coords.patch "LVGL draw-area clip patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_draw_render_thread_acquire.patch "LVGL render-thread acquire/release barrier patch (ARM64 layer-buffer UAF)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_draw_sw_label_null_guard.patch "LVGL label draw NULL font guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-drm-flush-rotation.patch "LVGL DRM flush rotation patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-drm-egl-getters.patch "LVGL DRM EGL getters patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-drm-preferred-mode.patch "LVGL DRM preferred mode patch (#766)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-drm-set-master.patch "LVGL DRM set-master patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-drm-mmap64.patch "LVGL DRM 64-bit mmap offset patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_refr_reshape_null_guard.patch "LVGL refr reshape NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_img_null_guard.patch "LVGL img goto_xy NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_img_warn_obj_name.patch "LVGL image-warn obj-name patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_blur_null_guard.patch "LVGL blur goto_xy NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_pos_null_guards.patch "LVGL obj_pos NULL guards patch (blur_walk_cb + layout_update_core)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_grid_update_guard.patch "LVGL grid_update freed-container guard patch (#973)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_draw_buf_oom_guard.patch "LVGL draw_buf OOM guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-evdev-protocol-a.patch "LVGL evdev Protocol-A touch release patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_arc_draw_guard.patch "LVGL arc draw guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_arc_subject_null_guard.patch "LVGL arc subject NULL guard patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_draw_sw_img_buf_height_guard.patch "LVGL draw_sw_img buf_h guard patch (upstream ca18403)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_lodepng_bpp_guard.patch "LVGL lodepng bit-depth guard (16-bit PNG heap overflow)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_drm_egl_render_mode_fix.patch "LVGL DRM EGL render mode fix (upstream ce112eb)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-egl-vsync.patch "LVGL EGL vsync setter patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-egl-partial-upload.patch "LVGL EGL partial upload patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-egl-xrgb-shader.patch "LVGL EGL XRGB display shader patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_texture_cache_null_guard.patch "LVGL texture cache NULL guard patch (upstream ec053a0)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_draw_sdl_stride_fix.patch "LVGL draw_sdl aligned stride fix"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_display_sync_cb.patch "LVGL display sync callback patch (upstream 4170bcb)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_delete_null_guards.patch "LVGL obj destructor NULL guard patch (obj_destructor_null telemetry)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_delete_async_dedup.patch "LVGL obj delete async dedup patch (dedup + UAF guard + diagnostics)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_get_screen_cycle_guard.patch "LVGL obj_get_screen cycle guard patch (cap parent-walk depth to 128)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_async_del_crumb.patch "LVGL async-delete breadcrumb patch (#840/#906 sync+async diagnostic)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_translation_warn_once.patch "LVGL translation warn-once patch (missing-language warning once per language)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_label_text_transform.patch "LVGL label text transform patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-sw-draw-wait-for-finish.patch "LVGL SW draw wait_for_finish + NULL guard patch (#739)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl-image-cache-oversize-uncached.patch "LVGL oversize-image uncached-draw patch (image larger than the cache draws instead of vanishing)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_lodepng_variable_sniff_guard.patch "LVGL lodepng variable-source sniff guard (#1673)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_crash_hook.patch "LVGL event crash-diagnostic hook patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_mark_deleted_defensive.patch "LVGL lv_event_mark_deleted defensive bail patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_pop_unwind_safe.patch "LVGL event-pop unwind-safe patch (RPHAV9T7 / L081 root cause)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_indev_delete_cancels_anim.patch "LVGL indev-delete animation cancel patch"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_dispatch_depth_guard.patch "LVGL event-dispatch-depth guard (cluster:pstat-async-delete / #906)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_stack_array.patch "LVGL #907 array-backed event stack (replaces e->prev linked list)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_event_dispatch_cb_guard.patch "LVGL dispatch-cb bounds gate + widget identity (3XNZQB2R)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_event_null_guards.patch "LVGL obj-event NULL guards (VHTR49QJ — recoverable bail + telemetry)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_style_null_guards.patch "LVGL style NULL guards patch (null style pointers in transitions/cache)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_obj_flag_screen_parent_null_guard.patch "LVGL obj flag screen-parent NULL guard (hide/unhide a screen)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_flex_hidden_grow_gap.patch "LVGL flex hidden+grow gap fix (upstream #9897 backport)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_check_arg_backport.patch "LVGL LV_CHECK_ARG backport patch (master macro for v9.5.0; drop at upgrade)"
	$(Q)$(APPLY_PATCH) $(LVGL_DIR) $(PATCH_DIR)/lvgl_fbdev_arg_guards.patch "LVGL fbdev arg-guard + log-order patch (uses backported LV_CHECK_ARG)"
	$(ECHO) "$(CYAN)Checking libhv patches...$(RESET)"
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-openssl-static-link.patch "libhv OpenSSL/static build hook patch"
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-streaming-upload.patch "libhv streaming upload patch"
	$(Q)if [ -d "$(LIBHV_DIR)/include/hv" ]; then \
		if ! diff -q "$(LIBHV_DIR)/http/client/requests.h" "$(LIBHV_DIR)/include/hv/requests.h" >/dev/null 2>&1; then \
			echo "$(YELLOW)→ Syncing patched requests.h to include/hv/$(RESET)"; \
			cp "$(LIBHV_DIR)/http/client/requests.h" "$(LIBHV_DIR)/include/hv/requests.h" && \
			echo "$(GREEN)✓ Patched header synced$(RESET)"; \
		fi \
	fi
	@# The libhv patches take the same verdict as the LVGL ones, with one
	@# addition: the note names what breaks if the patch is missing, because
	@# libhv headers are -isystem, so a silently missing patch changes nothing
	@# the build itself notices.
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-dns-resolver-fallback.patch "libhv DNS resolver fallback patch" "Without it embedded DNS resolution is broken."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-tcpclient-reconnect-resilience.patch "libhv TcpClient reconnect resilience patch" "Without it a pending auto-reconnect can fault in createsocket() during teardown (#1212)."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-websocket-backoff-on-upgrade.patch "libhv WebSocket backoff patch" "Without it a failed WS upgrade reconnects at 5Hz."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-hthreadpool-wait-lock.patch "libhv HThreadPool locking patch" "Without it wait() races a worker's pop_front() (nightly TSAN via ThumbnailProcessor::wait_for_completion), and stop() can lose its wakeup and stall 60s joining an idle worker."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-hlog-thread-safe-localtime.patch "libhv hlog localtime_r patch" "Without it every logging thread races on localtime()'s shared struct tm and tzset's TZ string (nightly TSAN, logger_print)."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-http-request-cancel-atomic.patch "libhv HttpRequest cancel atomic patch" "Without it CameraStream::stop() races the stream thread's ParseUrl() (nightly TSAN in HttpRequest::Cancel)."
	$(Q)$(APPLY_PATCH) $(LIBHV_DIR) $(PATCH_DIR)/libhv-websocket-open-install-once.patch "libhv WebSocketClient install-once patch" "Without it concurrent connect() corrupts the heap (SIGABRT free(): invalid next size)."
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
	@# Everything below records "this state is the applied one". A stanza above
	@# that only warned must not get that recording: its patch's effect is
	@# absent, and a stamp written over the gap would read as consistent on
	@# every later build while the patch stays missing. The marker precondition
	@# runs first and fails the recipe, so the tree is re-judged (and the warn
	@# re-printed) on every build until it is repaired.
	$(Q)if [ "$${HELIX_MARKER_DERIVING:-0}" = 1 ]; then \
		echo "$(CYAN)ℹ marker gate suspended - deriving a new table$(RESET)"; \
	elif command -v python3 >/dev/null 2>&1; then \
		$(PATCH_MARKER_CHECK); \
	else \
		echo "$(YELLOW)⚠ python3 not found - patch marker check skipped$(RESET)"; \
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

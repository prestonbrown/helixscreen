# Submodule Patches

Local patches applied to git submodules. Managed by `mk/patches.mk` — run `make reapply-patches` to reset and reapply all.

## Base Version

**LVGL**: v9.6.0 (commit `80ca777e3`)

## Upstream PR Status

Several patches have been submitted upstream to [lvgl/lvgl](https://github.com/lvgl/lvgl), from the fork at [prestonbrown/lvgl-fork](https://github.com/prestonbrown/lvgl-fork) (the LVGL PR fork — not to be confused with `lib/helix-xml`). **Do not delete that fork while any PR below is open; deleting it closes them.**

A patch here is droppable only when its PR is *merged*. **Closed does not mean merged** — read the status cell before dropping anything on a version bump.

| PR | Title | Patches Included | Status |
|----|-------|-----------------|--------|
| [#9827](https://github.com/lvgl/lvgl/pull/9827) | fix(string): NULL guard for lv_strdup | `lvgl-strdup-null-guard` | **Closed, rejected.** LVGL keeps POSIX semantics (NULL to `strdup` is UB). Never droppable — keep permanently |
| [#9828](https://github.com/lvgl/lvgl/pull/9828) | fix(slider): block perpendicular scroll chain while dragging | `lvgl_slider_scroll_chain` | **Closed, withdrawn.** Still required at v9.6.0: upstream's 95421b26c fixes the knob updating during a scroll, not the scroll chain |
| [#9829](https://github.com/lvgl/lvgl/pull/9829) | fix(evdev): Protocol-A multitouch release handling | `lvgl-evdev-protocol-a` | **Merged** as fccb46a1f (v9.6.0); the patch still carries what the merge lacks |
| [#9830](https://github.com/lvgl/lvgl/pull/9830) | fix(arc): guard against negative inner radius | `lvgl_arc_draw_guard` | **Merged** as e9d81b3a9, in v9.6.0; patch retired |
| [#9831](https://github.com/lvgl/lvgl/pull/9831) | fix(draw): comprehensive NULL safety for SW draw pipeline | `lvgl_blend_null_guard`, `lvgl_blend_buf_bounds_clip`, `lvgl_blend_color_null_guard`, `lvgl-fix-signed-unsigned-draw-coords`, `lvgl_draw_sw_label_null_guard`, `lvgl_refr_reshape_null_guard`, `lvgl_img_null_guard`, `lvgl_blur_null_guard`, `lvgl_draw_buf_oom_guard` | Open. Only the draw-coords clip reached v9.6.0 (2efe79d92), so `lvgl-fix-signed-unsigned-draw-coords` is retired; the other eight stay |
| [#9832](https://github.com/lvgl/lvgl/pull/9832) | fix(fbdev): stride-based bpp, BGR auto-detect, buffer alignment, skip-unblank | none (all four retired) | **Merged** as 508dfc7e5 (v9.6.0) |

## LVGL Patches

Applied in order by `mk/patches.mk`. Grouped by subsystem.

### Display Drivers

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl-drm-flush-rotation.patch` | `lv_linux_drm.c`, `include/lvgl/drivers/display/lv_linux_drm.h` | DRM plane rotation API + 180deg software rotation via shadow buffer + legacy drmModeSetCrtc fallback | Project-specific |
| `lvgl-drm-mmap64.patch` | `lv_linux_drm.c` | `_FILE_OFFSET_BITS 64` so the dumb-buffer `mmap()` keeps DRM's >4 GiB map offset on 32-bit targets (pi32), widen `drm_buffer_t::offset` to 64 bits, fix the `%u`/`%lu` log formats | Upstream bug, not yet submitted |
| `lvgl-drm-egl-getters.patch` | `lv_linux_drm_egl.c` | EGL implementation of `lv_linux_drm_get_fd()` (declared by drm-flush-rotation) | Project-specific |
| `lvgl-drm-preferred-mode.patch` | `lv_linux_drm.c`, `include/lvgl/drivers/display/lv_linux_drm.h` | `lv_linux_drm_set_preferred_mode()`: the dumb-buffer driver picks the connector mode matching a requested hdisplay x vdisplay before `set_file` (#766) | Project-specific |
| `lvgl-drm-set-master.patch` | `lv_linux_drm.c` | The dumb-buffer driver retries `drmSetMaster()` for about 1 s before modesetting, and logs a clear cause if another process keeps master | Project-specific |
| `lvgl-egl-vsync.patch` | `lv_opengles_egl.c`, `.h`, `lv_linux_drm_egl.c`, `include/lvgl/drivers/display/lv_linux_drm.h` | `lv_opengles_egl_set_vsync()` and the display-level `lv_linux_drm_egl_set_vsync()`: the EGL context hardcodes `vsync = false`, so a frame finished while a flip is in flight is replaced unshown. `HELIX_EGL_VSYNC=1` turns the wait on (`src/api/display_backend_drm.cpp`) | Project-specific |
| `lvgl-egl-partial-upload.patch` | `lv_linux_drm_egl.c`, `lv_linux_drm_egl_private.h`, `include/lvgl/drivers/display/lv_linux_drm.h`, `lv_opengles_texture.c`, `lv_opengles_texture_private.h`; creates `lv_linux_drm_egl_upload.h` | `lv_linux_drm_egl_set_partial_upload()` and `lv_linux_drm_egl_request_full_upload()`: the flush sends the display texture only the flushed areas (`glTexSubImage2D`), and the whole buffer on the first frame, into a new texture or size, and after a request. The decision is `lv_linux_drm_egl_upload_plan()`, a pure header the unit tests include. `HELIX_EGL_PARTIAL_UPLOAD=1` turns it on (`src/api/display_backend_drm.cpp`). 9.6 presents through the shared `lv_opengles_texture_render_display()`; the patch splits its software branch so `lv_opengles_texture_present_display()` presents without re-uploading, and the DRM flush uploads per flush (full via the stock function, sub-areas via `glTexSubImage2D` in the display's color format) | Project-specific |
| `lvgl-egl-xrgb-shader.patch` | `lv_opengles_driver.c`, `include/lvgl/drivers/opengles/lv_opengles_driver.h`, `assets/lv_opengles_shader.c` | `lv_opengles_render_display()` sets `u_ColorDepth` 24 for an `XRGB8888` display (9.6 uses the format's bpp, 32), and both display shaders (GLSL 100 and 300 es) then ignore the texture's fourth byte. `lv_opengles_driver.h` gains `HELIX_LV_OPENGLES_XRGB_IGNORES_X`, and `display_backend_drm.cpp` refuses to compile the EGL backend without it. `HELIX_EGL_XRGB=1` keeps the display `XRGB8888` | Project-specific |
| `lvgl-opengles-texture-align.patch` | `lv_opengles_texture.c`, `lv_opengles_texture_private.h` | The OpenGL ES texture's draw buffer is allocated aligned to `LV_DRAW_BUF_ALIGN`, which `lv_display_set_buffers()` asserts. malloc aligns to 8 bytes on 32-bit targets, where a plain allocation fails that assertion at display creation and on every rotation reshape. The block is over-allocated and `fb1_alloc` keeps the pointer to free | Project-specific |
| `lvgl-opengles-quarter-turn-direction.patch` | `lv_opengles_driver.c` (after `lvgl-egl-xrgb-shader.patch`) | `lv_opengles_render_display()` presents `LV_DISPLAY_ROTATION_90` and `_270` in LVGL's own direction. The vertex table turns a quarter turn clockwise, while `lv_display_rotate_point()` maps pointer input counter-clockwise; left as is, touch on a GPU-rotated display is reversed on both axes. The two quarter turns trade places before the table is filled. In 9.6 the DRM, SDL-EGL and Wayland paths all present through the shared texture code, with `v_flip=false` under software rendering and `v_flip=true` under `LV_USE_DRAW_OPENGLES`. Only the DRM software path is verified; a build presenting through another path needs the direction checked | Project-specific |

### Draw Pipeline

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_blend_null_guard.patch` | `lv_draw_sw_blend.c` | NULL check for layer/draw_buf at blend entry | PR #9831 |
| `lvgl_blend_buf_bounds_clip.patch` | `lv_draw_sw_blend.c` | Clip blend_area to layer->buf_area | PR #9831 |
| `lvgl_blend_color_null_guard.patch` | `lv_draw_sw_blend_to_*.c` (16 files) | NULL dest_buf checks in all per-format blend functions | PR #9831 |
| `lvgl_draw_sw_label_null_guard.patch` | `lv_draw_sw_letter.c` | NULL check for font/glyph before all glyph format rendering | PR #9831 |
| `lvgl_draw_buf_oom_guard.patch` | `lv_draw_buf.c` | Remove redundant LV_ASSERT_MALLOC before NULL check | PR #9831 |
| `lvgl_refr_reshape_null_guard.patch` | `lv_refr.c` | NULL guard on draw_buf reshape failure, skip render gracefully | PR #9831 |
| `lvgl_img_null_guard.patch` | `lv_draw_sw_img.c` | NULL guard after go_to_xy in image mask path | PR #9831 |
| `lvgl_blur_null_guard.patch` | `lv_draw_sw_blur.c` | NULL checks after all ~15 lv_draw_buf_goto_xy() calls | PR #9831 |
| `lvgl_draw_render_thread_acquire.patch` | `lv_draw.c`, `lv_draw_private.h` | Make `lv_draw_task_t.state` a real atomic handshake (`LV_DRAW_TASK_STATE_GET`/`SET`) instead of trusting `volatile`, and wait for the draw units to finish before destroying a layer's `draw_buf` | Project-specific (the `volatile == atomic` assumption is upstream's; candidate to submit) |
| `lvgl-sw-draw-wait-for-finish.patch` | `lv_draw_sw.c` | Acquire/release accessors on the SW unit's `task_act` and `exit_status`, publishing task completion with the release store above, and skip a task whose `draw_dsc` is NULL rather than dereferencing it. The wait itself is upstream (fee343d59) | Project-specific |

### Widgets & Input

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_slider_scroll_chain.patch` | `lv_slider.c` | Block perpendicular scroll chain during drag (touchscreen UX) | PR #9828 closed — still needed at v9.6.0 |
| `lvgl-evdev-protocol-a.patch` | `lv_evdev.c`, `include/lvgl/drivers/indev/lv_evdev.h` | What 9.6's Protocol-A fix (fccb46a1f) lacks: released slots keep their last coordinates (9.6 zeroes them, causing phantom edge taps on Holtek 04d9:8030 / BTT-HDMI5) plus a per-slot trust gate for never-reported slots, `lv_evdev_get_last_raw()` (pre-swap, pre-scale reading for three-point calibration, #1259, #1276), the `HELIX_DEBUG_TOUCH` range log, and the `ABS_MT_POSITION` range fallback | Project-specific (Protocol-A release synthesis merged upstream as fccb46a1f) |
| `lvgl_scroll_throw_time_based.patch` | `lv_indev.c`, `lv_indev_private.h`, `lv_indev_scroll.c` | Scroll momentum by elapsed time: the throw vector is sampled in pixels per `LV_DEF_REFR_PERIOD` however slowly the device reads, and each throw step decays and moves by the ms since the last one, so a 300 ms frame glides like ten 33 ms frames. Identical to stock at the nominal period | Project-specific |

### Core & Stdlib

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl-strdup-null-guard.patch` | `lv_string_builtin.c`, `lv_string_clib.c` | NULL input guard for lv_strdup | PR #9827 rejected — permanent |
| `lvgl_observer_debug.patch` | `lv_observer.c` | Enhanced error logging with pointer/type info | Project-specific |
| `lvgl_observer_remove_null_guard.patch` | `lv_observer.c` | NULL guard for observer removal | Project-specific |
| `lvgl_subject_deinit_refuses_observers.patch` | `lv_observer.c`, `include/lvgl/core/lv_observer.h`, `lv_obj_style.c`, `lv_span.c` | `lv_subject_deinit()` marks the subject `deinitialized` and both `lv_subject_add_observer*` entry points refuse it with a warning until the next init, which would otherwise drop the observer list and orphan anything attached in between; the style and span binders release their user data instead of dereferencing a refused (NULL) observer (#1279) | Project-specific |
| `lvgl_thread_check_hook.patch` | `lv_observer.c`, `lv_timer.c`, `lv_obj_tree.c`; creates `lv_helix_thread_check.h` | Calls the weak `helix_lvgl_thread_check()` hook at every subject set/notify, `lv_timer_create`/`lv_timer_delete` and `lv_obj_delete`/`lv_obj_clean`, so `--test` and the unit binary catch LVGL calls made off the LVGL thread (`src/system/lvgl_thread_check.cpp`). Compiled out unless `lv_conf.h` sets `LV_HELIX_THREAD_CHECK`, which release builds and the firmware do not. `lv_subject_notify` checks its external callers only: the set functions reach the body through a static `subject_notify()` so one call reports once (#1942) | Project-specific |
| `lvgl_obj_delete_null_guards.patch` | `lv_obj.c` | NULL guard at the top of `lv_obj_destructor` reporting `obj_destructor_null` through the helix telemetry hook | Project-specific |
| `lvgl_obj_is_valid_no_deref.patch` | `lv_obj_tree.c` | `lv_obj_is_in_widget_tree()` (which `lv_obj_is_valid` maps to) searches the screens' trees top-down for the pointer instead of walking `obj->parent` up from it, so asking about a freed object never reads through it. The app's guard calls and `lvgl_grid_update_guard.patch` pass pointers that may already be freed | Project-specific |
| `lvgl_obj_flag_screen_parent_null_guard.patch` | `lv_obj.c`, `include/lvgl/core/lv_obj.h` | NULL-parent guards on the layout-dirty calls in `lv_obj_add_flag`/`lv_obj_remove_flag`, so a screen (no parent) can be hidden and unhidden; `ScreenHideHold` unhides the active screen after a screensaver or software sleep. `lv_obj.h` gains `HELIX_LV_OBJ_FLAG_SCREEN_PARENT_GUARD`, and `display_manager.cpp` refuses to compile without it | Upstream bug, not yet submitted |
| `lvgl_event_crash_hook.patch` | `lv_obj_event.c` | Weak-linked `helix_crash_note_event()` call at top of `event_send_core` — records innermost dispatch target+code for crash diagnostic reports | Project-specific |

### Project-Specific (not submitted upstream)

| Patch | File(s) | Purpose |
|-------|---------|---------|
| `lvgl_label_text_transform.patch` | `lv_label.c`, `include/lvgl/widgets/lv_label.h`, `lv_label_private.h` | text_transform_upper flag for i18n-safe uppercase at text-set time |
| `lvgl_translation_tail_packs.patch` | `lv_translation.c`, `include/lvgl/core/lv_translation.h` (after `lvgl_translation_threadsafe.patch`) | `lv_translation_set_new_packs_at_tail()`: while set, dynamic packs are appended to the END of the lookup order, so plugin packs only supply tags the app does not translate | Project-specific |
| `lvgl_sdl_window.patch` | `lv_sdl_window.c` | Multi-display positioning, Android support, macOS crash fix |
| `lvgl_draw_sdl_stride_fix.patch` | `lv_draw_sdl.c` | Four `lv_draw_sdl` fixes: real stride for `SDL_UpdateTexture`, zero the full aligned buffer, normalise polyline points, decline canvas targets in evaluate |
| `lvgl_sdl_sw_android_debug.patch` | `lv_sdl_sw.c` | `SDL_BLENDMODE_NONE` for the framebuffer texture (Android GLES promotes RGB888 to RGBA) and a NULL guard in `deinit_display` for a failed init |
| `lvgl_theme_breakpoints.patch` | `lv_theme_default.c` | Custom breakpoint tuning for 480-800px |

## Dropped Patches (v9.5.0)

LVGL 9.5 removed the entire XML system from core. These patches are now in `lib/helix-xml/`:

- `lv_xml.c` / `.h` -- `lv_xml_get_const_silent()` addition
- `lv_xml_style.c` -- `translate_x`/`translate_y` using `lv_xml_to_size()`
- `lv_xml_image_parser.c` -- image "contain"/"cover" alignment enums

## Retired at v9.6.0

v9.6.0 does what these did, so they were deleted at the bump:

| Patch | Upstream |
|-------|----------|
| `lvgl_display_sync_cb.patch` | 4170bcb1e: `lv_display_set_sync_cb()` and the sync events |
| `lvgl_check_arg_backport.patch` | 78f2b5139, 0043765ad: `LV_CHECK_ARG` in `include/lvgl/debugging/lv_check_arg.h` |
| `lvgl-fix-signed-unsigned-draw-coords.patch` | 2efe79d92 clips the mask-rect `draw_area` to `buf_area`; the out-of-range `goto_xy` log is now an `LV_CHECK_ARG` |
| `lvgl_arc_draw_guard.patch` | e9d81b3a9 (#9830) |
| `lvgl_arc_subject_null_guard.patch` | Unreachable: `lv_arc_bind_value()`, the callback's only registrant, refuses a NULL subject (ccdda9149, needs `LV_USE_CHECK_ARG`) |
| `lvgl_draw_sw_img_buf_height_guard.patch` | ca1840333 (#9776) clamps `buf_h` at all four sites |
| `lvgl_lodepng_bpp_guard.patch` | Unreachable: ff65e702e (#9882) rejects bit depths above 8 in `lodepng_inspect()` before the decode allocates |
| `lvgl_flex_hidden_grow_gap.patch` | 171196b95, which it backported |
| `lvgl_fbdev_stride_bpp.patch` | 508dfc7e5: bpp from `line_length / xres_virtual` (xres when 0), standard depths only |
| `lvgl_fbdev_skip_unblank.patch` | 508dfc7e5 |
| `lvgl-fbdev-bgr-swap.patch` | 508dfc7e5 |
| `lvgl-fbdev-buffer-align.patch` | 508dfc7e5 |
| `lvgl_fbdev_arg_guards.patch` | ed6ebda9f (`LV_CHECK_ARG` in the fbdev setters) and 508dfc7e5 (bpp logged after correction) |
| `lvgl_drm_egl_render_mode_fix.patch` | ce112eba1 (#9806), which it was |
| `lvgl_texture_cache_null_guard.patch` | ec053a0d9 (#9770), which it was |

## libhv Patches

| Patch | Purpose |
|-------|---------|
| `libhv-dns-resolver-fallback.patch` | Direct UDP DNS resolution fallback for statically-linked builds where `getaddrinfo()` fails |
| `libhv-hlog-thread-safe-localtime.patch` | `_POSIX_C_SOURCE` define before the headers so `localtime_r()` is declared under `-std=c99` — the implicit-int return becomes a garbage pointer and the first `tm` dereference segfaults |
| `libhv-hthreadpool-wait-lock.patch` | Take `task_mutex` in `hthreadpool` `wait()`/`commit()` — the unlocked `tasks` read raced a worker's pop (ThreadSanitizer via `ThumbnailProcessor`) — and reserve the worker slot in `createThread()` under `thread_mutex` before spawning, so concurrent commits cannot both pass the cap check and run live workers past `max_thread_num` (#1585) |
| `libhv-http-request-cancel-atomic.patch` | Dedicated `std::atomic` for `HttpRequest::Cancel()` — as a bitfield it shared a word with the redirect/proxy bits, so a cross-thread cancel raced `ParseUrl()`'s read-modify-write |
| `libhv-http-request-hook.patch` | `hv_set_http_request_hook()`: one hook sees every client request in `http_client_make_request`, after its URL is parsed, so `moonraker_auth` attaches `X-Api-Key` to the requests bound for the configured Moonraker without every call site doing it |
| `libhv-openssl-static-link.patch` | OpenSSL/static build hook |
| `libhv-streaming-upload.patch` | Streaming upload support |
| `libhv-tcpclient-reconnect-resilience.patch` | `TcpClient` reconnect hardening: re-resolve the host on every retry (a hostname whose IP changed is no longer dialed at its stale address forever), guard the reconnect timer against a stopped event loop, defer old-channel destruction to the loop thread (the onclose closure was freed while still running), and reschedule instead of dropping the retry chain on transient `::socket()` failures |
| `libhv-websocket-backoff-on-upgrade.patch` | Undo `open()`'s premature backoff reset when the upgrade handshake never reached WS_OPENED — a failing upgrade used to restart the reconnect delay from scratch on every attempt |
| `libhv-websocket-open-install-once.patch` | Install the TcpClient-level channel callbacks exactly once in the constructor — `open()` reassigned those `std::function` members from the caller's thread, freeing the closure's heap storage under a concurrently running callback |

## Lua Patches

Base: Lua v5.4.9 (`lib/lua`). Lua is compiled as C++ for the plugin runtime (`src/plugin/`).

| Patch | Purpose |
|-------|---------|
| `lua-pattern-step-budget.patch` | Every 16384 calls to `lstrlib.c`'s `match()`, run the state's count hook with a NULL `lua_Debug`. The count hook counts VM instructions, so it never fires inside a C function, and a backtracking pattern such as `("a"):rep(1e4):find(".-.-.-.-x")` would freeze the UI past the plugin time budget. `LuaRuntime::budget_hook` is that hook |

A worktree set up before `lib/lua` joined the private checkouts still has it as a symlink into the main tree, so its build patches the shared copy. Convert it with `scripts/setup-worktree.sh <branch> --setup-only` from the main tree; `test -L lib/lua` in the worktree then reports false.

## Usage

```bash
# Automatic (preferred) — applies all patches if needed
make apply-patches

# Force reset and reapply all
make reapply-patches

# Regenerate a patch after manual edits in lib/lvgl/ (or lib/libhv/, lib/lua/) — SEE THE WARNING BELOW FIRST
git -C lib/lvgl diff src/path/to/file.c > patches/patch_name.patch
```

### Regenerating a patch whose file is shared

That `git diff` recipe is only safe when exactly one patch touches the file. **A dozen-plus files
are touched by more than one patch**, so for those it silently folds every other patch's hunks
into the one being regenerated. `src/misc/lv_event.c` has seven patches; `lv_obj_event.c` and
`lv_obj_tree.c` have four each.

Check before regenerating:

```bash
# how many patches claim this file?
grep -l "diff --git a/src/path/to/file.c" patches/*.patch
```

If more than one, do not use `git diff`. Take the pristine file, apply only this patch's own
changes to it, and diff that:

```bash
git -C lib/lvgl show v9.6.0:src/path/to/file.c > /tmp/pristine.c
cp /tmp/pristine.c /tmp/patched.c
# edit /tmp/patched.c with only this patch's changes
diff -u --label a/src/path/to/file.c --label b/src/path/to/file.c /tmp/pristine.c /tmp/patched.c \
  | sed '1i\
diff --git a/src/path/to/file.c b/src/path/to/file.c' > patches/patch_name.patch
```

Then `make reapply-patches` from clean and confirm every patch still reports as applied. A
folded patch usually still applies on a clean tree, so the duplication only surfaces later as
a conflict or a doubled hunk.

### A patch superseded by a later patch

A from-clean fatal on a patch that regenerating does not fix is usually this: a later patch
now owns every file the failing one touches — its guards were rewritten, its data structures
replaced — so the old patch's hunks describe a tree that no longer exists anywhere. Because
`git apply` is atomic, even one dead file kills the live hunks in the others.

Distinguish it from ordinary drift before regenerating: for each file the failing patch
touches, ask which patches also touch it and whether a later one now carries the same guard.

```bash
# which patches touch a file, in apply order (the order in mk/patches.mk)?
grep -l "diff --git a/src/path/to/file.c" patches/*.patch
```

A wholly superseded patch is deleted — from `patches/`, from its `mk/patches.mk` stanza, and
from `mk/patch-markers.tsv` (`make regen-patch-markers` after the first two) — not
regenerated: regenerating it against the current tree just folds the later patch's hunks in.
Keep the filename out of the graveyard debate entirely; a partially superseded patch (some
files live, some dead) is reduced to its live sections and regenerated the shared-file way
above, per file.

**Apply verdicts:** each patch's block in `mk/patches.mk` calls
`scripts/apply_submodule_patch.sh <submodule-dir> <patch> <label> [note]`, which decides three ways:
apply it, recognize it as already applied (reverse check), or refuse. A bare `apply --check`
cannot tell "already applied" from "drifted and will never apply" — both exit non-zero — and a
file-dirty test breaks when a patch stops touching X or when another patch dirties it first.
Patches that share a file can fail both checks on a correctly patched tree, so in-place runs
warn and `make reapply-patches` is the run that judges: from clean, a patch that will not take
its apply branch is fatal. The optional note (libhv patches carry one) names the runtime
consequence of building without the patch, appended to the two verdicts that mean "this
patch may be missing".

The flag is a claim about the tree — "this run started from pristine submodules" — and the
claim is verified, not assumed. `mk/patches.mk` settles it once at recipe start (before any
stanza has dirtied a shared file) by asking git whether the patched files are pristine, and
writes the answer where the helper reads it. A checkout that is not pristine — the state
`make clean` leaves behind, stamp gone but submodules still patched — downgrades the fatal to
the in-place warn with the remedy named, instead of failing a healthy tree and telling you to
regenerate correct patches. CI runners are fresh clones, so every workflow sets
`HELIX_PATCHES_FROM_CLEAN=1` at the workflow level, reaching applies inside plain `make -j`
build steps — `scripts/check_workflow_submodules.py` fails a workflow that drops it.

**Patch markers:** presence is checked separately from applicability. `mk/patch-markers.tsv`
names, for every file each wired patch touches, one line it adds (or removes) there that
upstream never contained, and
`scripts/check_patch_markers.py` greps the checkout for each on every build, failing with the
patch's name and its consequence when one is missing. The marker is a plain text search: it
needs no git, so it holds in a docker build rsynced from a worktree where the submodules are
not repositories at all, and a sibling patch moving shared context cannot make a healthy tree
fail it the way an apply-check can. Three states fail loudly rather than pass vacuously: a
wired stanza with no row (new patch), a patch file whose hash no longer matches the table
(changed patch), and a missing or reintroduced marker (missing patch). `make
regen-patch-markers` reapplies from clean and rederives the table; run it whenever a patch
changes. Per file, not per patch: a patch whose other files were reverted would otherwise
pass on the one file that kept its change. Each file qualifies with one added or removed line
of at least 12 characters that upstream does not contain and no other patch introduces —
every file of every wired patch has one, and the generator refuses rather than leave a silent
gap.

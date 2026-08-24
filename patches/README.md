# Submodule Patches

Local patches applied to git submodules. Managed by `mk/patches.mk` — run `make reapply-patches` to reset and reapply all.

## Base Version

**LVGL**: v9.5.0 (commit `85aa60d18`)

## Upstream PR Status

Several patches have been submitted upstream to [lvgl/lvgl](https://github.com/lvgl/lvgl), from the fork at [prestonbrown/lvgl-fork](https://github.com/prestonbrown/lvgl-fork) (the LVGL PR fork — not to be confused with `lib/helix-xml`). **Do not delete that fork while any PR below is open; deleting it closes them.**

A patch here is droppable only when its PR is *merged*. **Closed does not mean merged** — read the status cell before dropping anything on a version bump.

| PR | Title | Patches Included | Status |
|----|-------|-----------------|--------|
| [#9827](https://github.com/lvgl/lvgl/pull/9827) | fix(string): NULL guard for lv_strdup | `lvgl-strdup-null-guard` | **Closed, rejected.** LVGL keeps POSIX semantics (NULL to `strdup` is UB). Never droppable — keep permanently |
| [#9828](https://github.com/lvgl/lvgl/pull/9828) | fix(slider): block perpendicular scroll chain while dragging | `lvgl_slider_scroll_chain` | **Closed, withdrawn** — upstream master grew an equivalent drag-scoped fix. Not in v9.5.0 though: its `LV_EVENT_PRESSING` has no scroll-chain removal, so the patch is still required at our pin. Re-check when bumping past v9.5.0 |
| [#9829](https://github.com/lvgl/lvgl/pull/9829) | fix(evdev): Protocol-A multitouch release handling | `lvgl-evdev-protocol-a` | Open, CI green |
| [#9830](https://github.com/lvgl/lvgl/pull/9830) | fix(arc): guard against negative inner radius | `lvgl_arc_draw_guard` | Open, CI green |
| [#9831](https://github.com/lvgl/lvgl/pull/9831) | fix(draw): comprehensive NULL safety for SW draw pipeline | `lvgl_blend_null_guard`, `lvgl_blend_buf_bounds_clip`, `lvgl_blend_color_null_guard`, `lvgl-fix-signed-unsigned-draw-coords`, `lvgl_draw_sw_label_null_guard`, `lvgl_refr_reshape_null_guard`, `lvgl_img_null_guard`, `lvgl_blur_null_guard`, `lvgl_draw_buf_oom_guard` | Open, CI green |
| [#9832](https://github.com/lvgl/lvgl/pull/9832) | fix(fbdev): stride-based bpp, BGR auto-detect, buffer alignment, skip-unblank | `lvgl_fbdev_stride_bpp`, `lvgl-fbdev-bgr-swap`, `lvgl-fbdev-buffer-align`, `lvgl_fbdev_skip_unblank` | Open, CI green |

## LVGL Patches

Applied in order by `mk/patches.mk`. Grouped by subsystem.

### Display Drivers

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_fbdev_stride_bpp.patch` | `lv_linux_fbdev.c` | Fix incorrect bpp on AD5M displays (calculate from stride) | PR #9832 |
| `lvgl_fbdev_skip_unblank.patch` | `lv_linux_fbdev.c`, `.h` | Skip FBIOBLANK during splash handoff | PR #9832 |
| `lvgl-fbdev-bgr-swap.patch` | `lv_linux_fbdev.c`, `.h` | Auto-detect BGR framebuffers and swap R/B channels (Allwinner R818) | PR #9832 |
| `lvgl-fbdev-buffer-align.patch` | `lv_linux_fbdev.c` | Over-allocate for LV_DRAW_BUF_ALIGN alignment | PR #9832 |
| `lvgl-drm-flush-rotation.patch` | `lv_linux_drm.c`, `.h` | DRM plane rotation API + 180deg software rotation via shadow buffer + legacy drmModeSetCrtc fallback | Project-specific |
| `lvgl-drm-mmap64.patch` | `lv_linux_drm.c` | `_FILE_OFFSET_BITS 64` so the dumb-buffer `mmap()` keeps DRM's >4 GiB map offset on 32-bit targets (pi32), widen `drm_buffer_t::offset` to 64 bits, fix the `%u`/`%lu` log formats | Upstream bug, not yet submitted |
| `lvgl-drm-egl-getters.patch` | `lv_linux_drm_egl.c` | EGL display/context/config getters (implementation only; header decls are in drm-flush-rotation) | Project-specific |

### Draw Pipeline

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_blend_null_guard.patch` | `lv_draw_sw_blend.c` | NULL check for layer/draw_buf at blend entry | PR #9831 |
| `lvgl_blend_buf_bounds_clip.patch` | `lv_draw_sw_blend.c` | Clip blend_area to layer->buf_area | PR #9831 |
| `lvgl_blend_color_null_guard.patch` | `lv_draw_sw_blend_to_*.c` (16 files) | NULL dest_buf checks in all per-format blend functions | PR #9831 |
| `lvgl-fix-signed-unsigned-draw-coords.patch` | `lv_draw_buf.c`, `lv_draw_sw_mask_rect.c` | Clip `draw_area` to the layer's `buf_area` in `lv_draw_sw_mask_rect`, so neither edge writes out of bounds; downgrade the OOB log to WARN | PR #9831 (proposed, awaiting agreement) |
| `lvgl_draw_sw_label_null_guard.patch` | `lv_draw_sw_letter.c` | NULL check for font/glyph before all glyph format rendering | PR #9831 |
| `lvgl_draw_buf_oom_guard.patch` | `lv_draw_buf.c` | Remove redundant LV_ASSERT_MALLOC before NULL check | PR #9831 |
| `lvgl_refr_reshape_null_guard.patch` | `lv_refr.c` | NULL guard on draw_buf reshape failure, skip render gracefully | PR #9831 |
| `lvgl_img_null_guard.patch` | `lv_draw_sw_img.c` | NULL guard after go_to_xy in image mask path | PR #9831 |
| `lvgl_blur_null_guard.patch` | `lv_draw_sw_blur.c` | NULL checks after all ~15 lv_draw_buf_goto_xy() calls | PR #9831 |

### Widgets & Input

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_slider_scroll_chain.patch` | `lv_slider.c` | Block perpendicular scroll chain during drag (touchscreen UX) | PR #9828 closed — still needed at v9.5.0 |
| `lvgl_arc_draw_guard.patch` | `lv_draw_arc.c`, `lv_arc.c` | Guard negative inner radius and zero-radius arc invalidation | PR #9830 |
| `lvgl-evdev-protocol-a.patch` | `lv_evdev.c` | Protocol-A touch release synthesis for Goodix GT9xx | PR #9829 |

### Core & Stdlib

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl-strdup-null-guard.patch` | `lv_string_builtin.c`, `lv_string_clib.c` | NULL input guard for lv_strdup | PR #9827 rejected — permanent |
| `lvgl_observer_debug.patch` | `lv_observer.c` | Enhanced error logging with pointer/type info | Project-specific |
| `lvgl_observer_remove_null_guard.patch` | `lv_observer.c` | NULL guard for observer removal | Project-specific |
| `lvgl_obj_delete_null_guards.patch` | `lv_global.h`, `lv_event.c`, `lv_obj.c`, `lv_obj_tree.c` | Event depth counter for corruption detection, NULL guards + alignment/depth-limit checks in event_mark_deleted, async cancel before child recursion in obj_delete_core | Pending |
| `lvgl_event_crash_hook.patch` | `lv_obj_event.c` | Weak-linked `helix_crash_note_event()` call at top of `event_send_core` — records innermost dispatch target+code for crash diagnostic reports | Project-specific |

### Project-Specific (not submitted upstream)

| Patch | File(s) | Purpose |
|-------|---------|---------|
| `lvgl_label_text_transform.patch` | `lv_label.c`, `lv_label.h`, `lv_label_private.h` | text_transform_upper flag for i18n-safe uppercase at text-set time |
| `lvgl_sdl_window.patch` | `lv_sdl_window.c` | Multi-display positioning, Android support, macOS crash fix |
| `lvgl_sdl_sw_android_debug.patch` | SDL files | SDL software renderer Android debug support |
| `lvgl_theme_breakpoints.patch` | `lv_theme_default.c` | Custom breakpoint tuning for 480-800px |

## Dropped Patches (v9.5.0)

LVGL 9.5 removed the entire XML system from core. These patches are now in `lib/helix-xml/`:

- `lv_xml.c` / `.h` -- `lv_xml_get_const_silent()` addition
- `lv_xml_style.c` -- `translate_x`/`translate_y` using `lv_xml_to_size()`
- `lv_xml_image_parser.c` -- image "contain"/"cover" alignment enums

## libhv Patches

| Patch | Purpose |
|-------|---------|
| `libhv-dns-resolver-fallback.patch` | Direct UDP DNS resolution fallback for statically-linked builds where `getaddrinfo()` fails |
| `libhv-hlog-thread-safe-localtime.patch` | `_POSIX_C_SOURCE` define before the headers so `localtime_r()` is declared under `-std=c99` — the implicit-int return becomes a garbage pointer and the first `tm` dereference segfaults |
| `libhv-hthreadpool-wait-lock.patch` | Take `task_mutex` in `hthreadpool` `wait()`/`commit()` — the unlocked `tasks` read raced a worker's pop (ThreadSanitizer via `ThumbnailProcessor`) |
| `libhv-http-request-cancel-atomic.patch` | Dedicated `std::atomic` for `HttpRequest::Cancel()` — as a bitfield it shared a word with the redirect/proxy bits, so a cross-thread cancel raced `ParseUrl()`'s read-modify-write |
| `libhv-openssl-static-link.patch` | OpenSSL/static build hook |
| `libhv-streaming-upload.patch` | Streaming upload support |
| `libhv-tcpclient-reconnect-resilience.patch` | `TcpClient` reconnect hardening: re-resolve the host on every retry (a hostname whose IP changed is no longer dialed at its stale address forever), guard the reconnect timer against a stopped event loop, defer old-channel destruction to the loop thread (the onclose closure was freed while still running), and reschedule instead of dropping the retry chain on transient `::socket()` failures |
| `libhv-websocket-backoff-on-upgrade.patch` | Undo `open()`'s premature backoff reset when the upgrade handshake never reached WS_OPENED — a failing upgrade used to restart the reconnect delay from scratch on every attempt |
| `libhv-websocket-open-install-once.patch` | Install the TcpClient-level channel callbacks exactly once in the constructor — `open()` reassigned those `std::function` members from the caller's thread, freeing the closure's heap storage under a concurrently running callback |

## Usage

```bash
# Automatic (preferred) — applies all patches if needed
make apply-patches

# Force reset and reapply all
make reapply-patches

# Regenerate a patch after manual edits in lib/lvgl/ — SEE THE WARNING BELOW FIRST
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
git -C lib/lvgl show v9.5.0:src/path/to/file.c > /tmp/pristine.c
cp /tmp/pristine.c /tmp/patched.c
# edit /tmp/patched.c with only this patch's changes
diff -u --label a/src/path/to/file.c --label b/src/path/to/file.c /tmp/pristine.c /tmp/patched.c \
  | sed '1i\
diff --git a/src/path/to/file.c b/src/path/to/file.c' > patches/patch_name.patch
```

Then `make reapply-patches` from clean and confirm every patch still reports as applied. A
folded patch usually still applies on a clean tree, so the duplication only surfaces later as
a conflict or a doubled hunk.

**Apply-check sentinels:** each patch's block in `mk/patches.mk` decides whether to apply. Some
older blocks test "is file X dirty?", which breaks when a patch stops touching X or when another
patch dirties it first. Prefer `git -C $(LVGL_DIR) apply --check <patch>` as the condition — it
asks the real question and does not depend on file ownership.

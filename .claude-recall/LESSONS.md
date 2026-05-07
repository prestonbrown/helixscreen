# LESSONS.md - Project Level

> **Lessons System**: Cite lessons with [L###] when applying them.
> Stars accumulate with each use. At 50 uses, project lessons promote to system.
>
> **Add lessons**: `LESSON: [category:] title - content`
> **Categories**: pattern, correction, decision, gotcha, preference

## Active Lessons

### [L008] [***--|****-] Design tokens and semantic widgets
- **Uses**: 27 | **Velocity**: 3.5 | **Learned**: 2025-12-14 | **Last**: 2026-05-06 | **Category**: pattern | **Type**: informational
> No hardcoded colors/spacing. Use semantic widgets (ui_card, ui_button, text_*, divider_*) — they apply tokens. Don't restate built-in defaults (style_radius on ui_card, button_height on ui_button). Defaults: docs/LVGL9_XML_GUIDE.md § "Custom Semantic Widgets".

### [L009] [***--|***--] Icon font sync workflow
- **Uses**: 30 | **Velocity**: 1.8125 | **Learned**: 2025-12-14 | **Last**: 2026-04-21 | **Category**: gotcha | **Type**: constraint
> Add icon to codepoints.h → add to regen_mdi_fonts.sh → `make regen-fonts` → rebuild. Skip any step = missing icon.

### [L011] [***--|*----] No mutex in destructors
- **Uses**: 11 | **Velocity**: 0.1875 | **Learned**: 2025-12-14 | **Last**: 2026-04-15 | **Category**: gotcha | **Type**: constraint
> No mutex locks in dtors during static destruction — other objects may already be gone, deadlocks/crashes on exit.

### [L014] [***--|***--] Register all XML components
- **Uses**: 40 | **Velocity**: 1.25 | **Learned**: 2025-12-14 | **Last**: 2026-04-21 | **Category**: gotcha | **Type**: constraint
> New XML components need `lv_xml_component_register_from_file()` in main.cpp. Forgetting = silent failure.

### [L020] [***--|*----] ObserverGuard for cleanup
- **Uses**: 15 | **Velocity**: 0.0625 | **Learned**: 2025-12-14 | **Last**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Use `ObserverGuard` RAII for `lv_subject` observers. Manual cleanup → UAF on panel destruction.

### [L021] [*----|-----] Centidegrees for temps
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2025-12-14 | **Last**: 2025-12-31 | **Category**: pattern | **Type**: informational
> Centidegrees (int) for temp subjects to keep 0.1°C resolution. Float subjects lose precision in LVGL bindings.

### [L025] [**---|*----] Button content centering
- **Uses**: 6 | **Velocity**: 0.0625 | **Learned**: 2025-12-21 | **Last**: 2026-03-13 | **Category**: pattern | **Type**: constraint
> Text-only buttons: `align="center"` on child. Icon+text with `flex_flow="row"` need all three: `style_flex_main_place="center"` (horiz), `style_flex_cross_place="center"` (cross), `style_flex_track_place="center"` (row position). Without track_place content sits at top.

### [L031] [*****|*****] XML no recompile
- **Uses**: 100 | **Velocity**: 14.06296875 | **Learned**: 2025-12-27 | **Last**: 2026-04-25 | **Category**: gotcha | **Type**: constraint
> ui_xml/*.xml loads at RUNTIME — never rebuild for XML-only changes (layout, styling, bindings, event cbs). Just relaunch. Rebuild only for C++ changes.

### [L039] [**---|**---] Unique XML callback names
- **Uses**: 7 | **Velocity**: 0.625 | **Learned**: 2025-12-30 | **Last**: 2026-04-23 | **Category**: pattern | **Type**: constraint
> XML `event_cb` names live in a flat global namespace (no scoping). Use `on_<component>_<action>` to avoid collisions. Generic names (on_modal_ok_clicked) collide across components.

### [L040] [***--|**---] Inline XML attrs override bind_style
- **Uses**: 10 | **Velocity**: 0.625 | **Learned**: 2025-12-30 | **Last**: 2026-04-16 | **Category**: gotcha | **Type**: constraint
> Inline style attrs (style_bg_color, style_text_color, …) outrank `bind_style` in LVGL's cascade. For reactive visuals, drop the inline attr and use TWO bind_styles (one per state) — no inline styling on the reactive property.

### [L042] [**---|**---] XML bind_flag exclusive visibility
- **Uses**: 9 | **Velocity**: 0.75 | **Learned**: 2025-12-31 | **Last**: 2026-04-20 | **Category**: pattern | **Type**: informational
> Multiple `bind_flag_if_eq` on the same object = independent observers, last write wins (race). For "show when X==v" use a single `bind_flag_if_not_eq` with the inverted ref. Eg `bind_flag_if_not_eq ref_value="0"` shows only when value IS 0.

### [L045] [*----|*----] XML dropdown options use &#10; entities
- **Uses**: 1 | **Velocity**: 0.1875 | **Learned**: 2026-01-06 | **Last**: 2026-03-23 | **Category**: gotcha | **Type**: constraint
> LVGL dropdown options separator is `&#10;` (newline entity): `options="Auto&#10;3D View&#10;2D Heatmap"`. Never expand to literal newlines — XML normalizes them to spaces in attrs (per spec), silently merging all options into one entry. format-xml.py preserves `&#10;` via lxml; other tools won't.

### [L046] [*----|*----] XML subject shadows C++ subject
- **Uses**: 2 | **Velocity**: 0.25 | **Learned**: 2026-01-06 | **Last**: 2026-04-20 | **Category**: correction | **Type**: constraint
> An XML `<subjects>` declaration shadows a same-named C++ subject (UI_SUBJECT_INIT_AND_REGISTER_*) — the local one wins, bindings stick at default. Don't declare XML subjects for values C++ owns.

### [L048] [*----|*----] Async tests need queue drain
- **Uses**: 3 | **Velocity**: 0.25 | **Learned**: 2026-01-08 | **Last**: 2026-04-20 | **Category**: pattern | **Type**: constraint
> Tests calling async setters (helix::async::invoke / ui_queue_update) must `UpdateQueue::instance().drain_queue_for_testing()` before assertions, else the update is still queued and the subject reads stale. Pattern: test_printer_state.cpp.

### [L051] [*----|*----] LVGL timer lifetime safety
- **Uses**: 1 | **Velocity**: 0.125 | **Learned**: 2026-01-08 | **Last**: 2026-03-28 | **Category**: gotcha | **Type**: constraint
> `lv_timer_create` cb fires after the owning object may be destroyed. Don't pass raw `this` as user_data. Use `AsyncLifetimeGuard::token()` (CLAUDE.md § Threading): capture `tok` in the timer cb, call `tok.defer([this](){ ... })` so the body only runs if `this` is still alive. Older `alive_guard` / `weak_ptr<bool>` patterns are deprecated.

### [L052] [***--|****-] Tag thread/network tests as [slow] to prevent hangs
- **Uses**: 25 | **Velocity**: 2 | **Learned**: 2026-01-09 | **Last**: 2026-04-18 | **Category**: gotcha | **Type**: constraint
> Tests using `std::thread` / `std::condition_variable` / `hv::EventLoop` MUST be tagged `[slow]` — `make test-run` filters `~[.] ~[slow]`, so untagged thread tests deadlock parallel shards. Concurrency, not speed. Known offenders: MoonrakerRobustnessFixture, MoonrakerClientSecurityFixture, NewFeaturesTestFixture, EventTestFixture, BedMeshRenderThread tests. When tests hang, check untagged thread tests FIRST.

### [L053] [**---|*----] Reset static fixture state in destructor
- **Uses**: 5 | **Velocity**: 0.375 | **Learned**: 2026-01-10 | **Last**: 2026-04-15 | **Category**: gotcha | **Type**: constraint
> Test fixtures using static state (`static bool queue_initialized`) MUST reset in dtor — otherwise it persists, init gets skipped on next test, shutdown leaves stale state. Pattern: dtor calls shutdown() then resets the flag to false.

### [L054] [*----|*----] Clear pending queues on shutdown
- **Uses**: 4 | **Velocity**: 0.125 | **Learned**: 2026-01-10 | **Last**: 2026-02-25 | **Category**: gotcha | **Type**: constraint
> Singleton queues (UpdateQueue) MUST clear pending callbacks in shutdown(), not just null the timer — stale entries fire on next init() against destroyed pointers → UAF. Pattern: `std::queue<T>().swap(pending_)`, then null the timer.

### [L055] [**---|*----] LVGL pad_all excludes flex gaps
- **Uses**: 5 | **Velocity**: 0.375 | **Learned**: 2026-01-10 | **Last**: 2026-03-28 | **Category**: gotcha | **Type**: constraint
> `style_pad_all` only sets edge padding (top/bottom/left/right), NOT inter-item spacing. For zero-gap flex layouts, also need `style_pad_row="0"` (column) or `style_pad_column="0"` (row), or `style_pad_gap="0"` for both.

### [L056] [*----|-----] lv_subject_t no shallow copy
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-14 | **Last**: 2026-01-16 | **Category**: gotcha | **Type**: constraint
> `lv_subject_t` cannot be shallow-copied — internal state breaks. Move ctors/assigns must reinitialize the subject in the destination, not copy.

### [L057] [*----|-----] Subject deinit before destruction
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-14 | **Last**: 2026-01-16 | **Category**: gotcha | **Type**: constraint
> Classes owning `lv_subject_t` members must call `lv_subject_deinit()` in dtor. Else observers leak and fire on freed subject → UAF.

### [L059] [**---|**---] LVGL object deletion: pick the RIGHT strategy
- **Uses**: 7 | **Velocity**: 0.5625 | **Learned**: 2026-01-20 | **Last**: 2026-04-15 | **Category**: pattern | **Type**: constraint
> Pick by scenario:
> 1. `safe_delete(obj)` — sync, shutdown-safe, auto-nulls. Use in dtors/teardown when NOT inside an UpdateQueue/async batch.
> 2. `safe_delete_deferred(obj)` — UpdateQueue-deferred. Use inside async cbs (timers, network responses). Nulls now, deletes next drain.
> 3. `lv_obj_delete_async(obj)` — LVGL builtin; auto-cancelled by `obj_delete_core()`. Use when another path may delete first. Custom `lv_async_call` lambdas are NOT cancelled (#399).
> 4. `lv_obj_delete(obj)` — raw, no guards. LVGL internals only.
> NEVER `lv_async_call(..., lv_obj_delete)` — uncancellable. NEVER `safe_delete()` inside `queue_update`/`async_call` lambdas — multiple sync deletes in one batch corrupt LVGL's event list (#356). ALWAYS cancel anims first ([L068]).

### [L060] [*****|*****] Interactive UI testing requires user
- **Uses**: 100 | **Velocity**: 13.875625 | **Learned**: 2026-02-01 | **Last**: 2026-05-06 | **Category**: correction | **Type**: constraint
> Don't fake automation with timed delays. Pattern:
> 1. `Bash` with `run_in_background: true`: `./build/bin/helix-screen --test -vv -p panel_name 2>&1 | tee /tmp/test.log` — NOT shell `&` or `timeout`.
> 2. Tell user exactly what to click.
> 3. Wait for confirmation.
> 4. `Read /tmp/test.log`.
> Failures: shell `&`, `timeout X cmd &`, retrying, assuming auto-nav. One bg task, tee to log, user interacts, you read.

### [L061] [**---|**---] AD5M test printer environment
- **Uses**: 5 | **Velocity**: 0.5625 | **Learned**: 2026-02-07 | **Last**: 2026-04-20 | **Category**: system
> AD5M (192.168.1.67, root@). armv7l Linux 5.4.61 (BusyBox). Gotchas: (1) wget no HTTPS, no curl. (2) No sftp-server — `scp -O`. (3) Logs go to BOTH `/tmp/helixscreen.log` AND syslog (`/var/log/messages`); syslog is current session, file may be stale. Default level WARN. (4) `/etc/ssl/certs/` empty — breaks all outbound HTTPS (libhv, wget); ship `ca-certificates.crt`. (5) No `openssl` CLI. (6) No inotify. (7) No WiFi (wpa_supplicant present, no interfaces — but see project_ad5m_wifi_actually_works.md). (8) OpenSSL 1.1 at `/usr/lib/libssl.so.1.1`. (9) Binary at `/opt/helixscreen/`, config `/opt/helixscreen/config/helixconfig.json`. (10) `ldd` may return empty for static ARM binaries.

### [L062] [***--|***--] AD5M build and deploy targets
- **Uses**: 10 | **Velocity**: 1.6875 | **Learned**: 2026-02-07 | **Last**: 2026-04-29 | **Category**: build
> AD5M build: `make ad5m-docker` (Docker ARM cross), NOT `make pi-test` (Pi). Deploy: `AD5M_HOST=192.168.1.67 make ad5m-deploy`.

### [L064] [***--|****-] Commit generated translation artifacts
- **Uses**: 26 | **Velocity**: 2.625 | **Learned**: 2026-02-10 | **Last**: 2026-04-23 | **Category**: i18n
> After syncing translation YAMLs, also stage the compiled artifacts: `src/generated/lv_i18n_translations.{c,h}` and `ui_xml/translations/translations.xml`. Tracked (not gitignored) for cross-compile, regenerated by build, but not auto-staged.

### [L065] [**---|***--] No test-only methods on production classes
- **Uses**: 6 | **Velocity**: 1.25 | **Learned**: 2026-02-11 | **Last**: 2026-05-04 | **Category**: patterns
> WRONG: public `reset_for_testing()` / `clear_*_for_testing()` on production classes — pollutes API, ships test code to users, creates coupling. Audit found 40+ instances (AbortManager 15 callback simulators, sensor managers, printer state). RIGHT: friend pattern — `friend class FooTestAccess;` in private section, define `FooTestAccess` in the test .cpp with statics that touch privates. Eg `FilamentSensorManagerTestAccess::reset(mgr)` instead of `mgr.reset_for_testing()`. For state-machine cbs (AbortManager) prefer a testable interface/mock over exposing every transition.

### [L066] [**---|**---] LVGL flex_grow row_wrap trick
- **Uses**: 7 | **Velocity**: 0.6875 | **Learned**: 2026-02-11 | **Last**: 2026-04-20 | **Category**: lvgl
> `flex_grow` + `flex_flow=row_wrap`: LVGL wraps against natural (content) width, not the grown width — children overflow. Fix: `width="1" flex_grow="1"` to force wrap against the allocated width.

### [L067] [***--|****-] Wrap C++ UI strings in lv_tr()
- **Uses**: 18 | **Velocity**: 3.1875 | **Learned**: 2026-02-14 | **Last**: 2026-05-04 | **Category**: ui
> All user-visible English in C++ goes through `lv_tr()` (labels, help text, toasts, etc.). Dropdown options are concatenated strings, harder to translate; do those carefully but don't skip the rest.

### [L068] [*----|*----] Cancel LVGL animations before object deletion
- **Uses**: 2 | **Velocity**: 0.3125 | **Learned**: 2026-02-15 | **Last**: 2026-03-25 | **Category**: lvgl
> Cancel animations BEFORE deleting their object — `lv_anim_delete` may fire the completion cb synchronously, UAF if obj is freed. Order: (1) null member ptr, (2) clear state flags, (3) `lv_anim_delete`, (4) `lv_obj_delete`. For anims with `this` as var: set guard flags false BEFORE lv_anim_delete so cbs no-op.

### [L069] [***--|***--] Never assume lv_obj user_data ownership — it may already be set
- **Uses**: 13 | **Velocity**: 1.625 | **Learned**: 2026-02-15 | **Last**: 2026-04-27 | **Category**: architecture
> `lv_obj_set_user_data()` is a single shared slot. Custom XML widgets, component handlers, and LVGL internals may set it during construction (ui_button stores button_data_t*, severity_card stores a string). NEVER `delete/free` `lv_obj_get_user_data()` unless you set it on that exact object. NEVER use it as general storage on objects you didn't fully create. **CRITICAL**: NEVER walk the parent chain looking for any non-null user_data — ui_button etc. set their own; you'll find the wrong data and miscast (SEGV in AmsOperationSidebar/AmsDryerCard). Walk by `lv_obj_get_name()` for a known name, then read user_data from THAT named object. For per-item data: (1) per-callback event user_data, (2) C++-side map keyed by obj ptr, or (3) `lv_obj_find_by_name` to a hidden child label.

### [L071] [***--|***--] XML child click passthrough
- **Uses**: 20 | **Velocity**: 1.6875 | **Learned**: 2026-02-21 | **Last**: 2026-03-31 | **Category**: ui | **Type**: constraint
> If a parent has `event_cb="clicked"`, all children (lv_obj, icon, text_body, text_tiny…) need `clickable="false" event_bubble="true"` — otherwise they absorb the click. LVGL objects are clickable by default.

### [L070] [***--|****-] Don't lv_tr() non-translatable strings
- **Uses**: 24 | **Velocity**: 2.9375 | **Learned**: 2026-02-17 | **Last**: 2026-04-29 | **Category**: i18n
> Don't `lv_tr()`: product names (Spoolman, Klipper, Moonraker, HelixScreen), URLs/domains, standalone tech abbreviations (AMS, QGL, ADXL), universal terms (OK, WiFi). Mark with `// i18n: do not translate`. Sentences containing product names ARE translatable ("Restarting HelixScreen…" — "Restarting" translates). Material names (PLA, PETG, ABS, TPU, PA) are also not translated, no translation_tag in XML.

### [L072] [***--|****-] Never capture bare this in async/WebSocket callbacks
- **Uses**: 14 | **Velocity**: 3.125 | **Learned**: 2026-02-22 | **Last**: 2026-05-05 | **Category**: gotcha | **Type**: constraint
> Callbacks to `execute_gcode()` / `send_jsonrpc()` / Moonraker fire from the WS thread, possibly after the widget is gone. Never capture raw `[this]`. Use `AsyncLifetimeGuard::token()` + `tok.defer(...)` (CLAUDE.md § Threading). Older `weak_ptr<bool>` / `shared_ptr<atomic<bool>>` patterns are deprecated.

### [L073] [***--|***--] ObserverGuard release vs reset
- **Uses**: 10 | **Velocity**: 1.5 | **Learned**: 2026-02-22 | **Last**: 2026-04-22 | **Category**: gotcha | **Type**: constraint
> `obs.reset()` when subjects are ALIVE (normal cleanup, repopulate) — unsubs and frees the LambdaObserverContext, expires weak_alive so deferred cbs skip. `obs.release()` ONLY when subjects may be DESTROYED (shutdown, pre-deinit) — avoids double-free. Wrong pick: reset on dead = double-free; release on live = zombie observer (context lives, weak_alive never expires, deferred cbs fire on stale `this`). Caused 17× #579: release() in unregister_slot_data left zombies → NEON blend SIGSEGV. Fixed by switching to reset() during normal widget delete; pre-deinit `cleanup_all_slot_data()` correctly uses release().

### [L074] [***--|***--] Generation counter for deferred observer callbacks
- **Uses**: 11 | **Velocity**: 1.25 | **Learned**: 2026-02-22 | **Last**: 2026-03-28 | **Category**: pattern | **Type**: informational
> When repopulating dynamic widget lists with observers, bump a generation counter BEFORE cleanup. Capture in cbs: `if (gen != self->gen_) return;`. Skips stale deferred cbs from `observe_int_sync` that fire after old widgets are gone (UAF guard).

### [L075] [*----|*----] Validate lv_obj before accessing children
- **Uses**: 1 | **Velocity**: 0.0625 | **Learned**: 2026-02-22 | **Last**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Before `lv_obj_find_by_name()` / `lv_obj_get_child()` / `lv_obj_get_child_count()` on a cached pointer: null-check + `AsyncLifetimeGuard` token check. NOT `lv_obj_is_valid()` (O(n), stack-overflows on Pi — see [L076]). Use `safe_delete_obj()` to null pointers post-delete. For async cbs detecting panel destruction: capture `tok = lifetime_.token()` and gate with `tok.defer(...)` (CLAUDE.md § Threading); older `weak_ptr<bool>` alive-guard pattern is deprecated.

### [L076] [***--|***--] NEVER use lv_obj_is_valid() in hot paths or async guards
- **Uses**: 13 | **Velocity**: 1.375 | **Learned**: 2026-02-22 | **Last**: 2026-04-27 | **Category**: gotcha
> `lv_obj_is_valid()` does a recursive O(n) walk of all screens + children (`obj_valid_child`). On Pi with thousands of widgets → stack overflow SIGSEGV. NEVER in: observer cbs, anim cbs (pulse_anim_cb), timer cbs, loops, dtor paths, `safe_delete_obj()`, async deletion guards. Use null checks. For deferred deletion guards: app-level tracking (ModalStack membership) or `lv_obj_delete_async()` (self-cancels). It can return TRUE on recycled memory → if LVGL reuses the address you delete a live obj (#399). Only safe in one-shot user click handlers (tree stable, called once).

### [L077] [*----|**---] Dynamic subject observers MUST use SubjectLifetime tokens
- **Uses**: 4 | **Velocity**: 0.5 | **Learned**: 2026-02-22 | **Last**: 2026-04-19 | **Category**: gotcha
> Observing dynamic subjects (per-fan/per-sensor/per-extruder): always use the `get_*_subject(name, lifetime)` overload and pass the token to the observer factory. Without it, `lv_subject_deinit()` frees the observer; `ObserverGuard::reset()` then calls `lv_observer_remove()` on freed memory → SEGV. Static singleton subjects don't need tokens.

### [L078] [-----|-----] lv_obj transform_scale invisible without background
- **Uses**: 0 | **Velocity**: 0 | **Learned**: 2026-03-13 | **Last**: 2026-03-13 | **Category**: gotcha
> `transform_scale` on an `lv_obj` with transparent bg only affects the object's own draw (border/bg), not children (separate draw units). For press feedback on transparent containers (back buttons), use `lv_style_set_opa` — applies to the entire object layer including children.

### [L079] [*----|*----] LVGL 9.5 DRAW_TASK_ADDED cannot add draw tasks
- **Uses**: 1 | **Velocity**: 0.125 | **Learned**: 2026-03-29 | **Last**: 2026-03-29 | **Category**: lvgl
> LVGL 9.5: `DRAW_TASK_ADDED` cbs fire AFTER `DRAW_MAIN_END/DRAW_POST` — `lv_draw_rect/_triangle/_fill` from there draws nothing. Broke chart gradient fills that worked in 9.4-pre. Fix: do custom fills in `DRAW_MAIN_END`, compute positions via `lv_chart_get_y_array()` + `lv_map()`. Gotcha: `lv_draw_fill` VER gradient `frac=0` is BOTTOM, `frac=255` is TOP. Use `lv_draw_fill` (not `lv_draw_rect`) for gradient-only fills to avoid bg_color bleed.

### [L080] [***--|****-] Verify deployment chain before user interaction
- **Uses**: 11 | **Velocity**: 3 | **Learned**: 2026-04-16 | **Last**: 2026-05-05 | **Category**: gotcha
> Before asking user to interact on-device, verify in one pass: (1) NEW binary running (PID start time / version in log), (2) logs land where you expect (journalctl/file/console), (3) required state on (telemetry, debug level in helixscreen.env), (4) logs reachable via SSH. Each failed round-trip burns user patience. Pi: systemctl → journalctl; `deploy-pi-fg` uses `ssh -t` (console only); nohup drops output. Production log capture: systemd + journalctl.

### [L081] [***--|****-] lifetime_.defer does NOT escape UpdateQueue batch
- **Uses**: 15 | **Velocity**: 3.625 | **Learned**: 2026-04-18 | **Last**: 2026-04-28 | **Category**: gotcha | **Type**: constraint
> `lifetime_.defer` / `tok.defer` / our `helix::ui::async_call` are thin wrappers over `queue_update` — the cb fires in the next `process_pending` tick, still inside a UpdateQueue batch with other sync deletions. AsyncLifetimeGuard's gen counter only protects `this`, not LVGL event-list. Any comment claiming "defer is outside process_pending" is wrong — fix it. Observer cbs (`observe_int_sync`, `observe_string`) are also queued since #82, same batch. BANNED inside any queued/deferred cb: `safe_delete(ptr)`, `lv_obj_delete(obj)`, `lv_obj_clean(container)`. INSTEAD: `safe_delete_deferred(ptr)`, `lv_obj_delete_async(obj)`, `helix::ui::safe_clean_children(container)` — all route through LVGL's async list, outside our batch. Multiple sync deletes in one batch → SIGSEGV in `lv_event_mark_deleted` (#776, #190, #80). CLAUDE.md § "No sync widget deletion in queued callbacks", `include/ui_utils.h`.

### [L082] [*----|*----] Percent size inside LV_SIZE_CONTENT parent collapses to 0
- **Uses**: 1 | **Velocity**: 0.25 | **Learned**: 2026-04-20 | **Last**: 2026-04-22 | **Category**: gotcha | **Type**: constraint
> LVGL percent sizing (`width="50%"`, `style_min_width="50%"`) resolves against parent content area. If parent is `LV_SIZE_CONTENT` → circular dep, percent collapses to 0, child vanishes. Symptom: `long_mode="wrap"` + `flex_grow="1"` wraps near-per-character (super-tall cards); flex rows show only fixed-width children with the growing one squeezed out. Fix: give parent an explicit width, then child `width="100%"`. Never nest percent kids in content-sized parents. Bit us in toast stacking refactor 26573f1f2 — LV_SIZE_CONTENT stack between `lv_layer_top` and toast_root collapsed `min_width="50%"`.

### [L083] [**---|****-] Never `std::thread(...).detach()` for fire-and-forget work
- **Uses**: 6 | **Velocity**: 2.25 | **Learned**: 2026-04-22 | **Last**: 2026-05-06 | **Category**: gotcha | **Type**: constraint
> On AD5M/CC1/MIPS32, `pthread_create` returns EAGAIN under thread exhaustion → `std::thread` ctor throws `std::system_error`. Propagating through an LVGL C event-dispatch frame or a `noexcept` boundary → `std::terminate without active exception`, near-impossible to diagnose because the crash looks unrelated (#724, #837 debug-bundle upload, #811-adjacent RatOS HTTP-thread storm).
> **HTTP work**: `helix::http::HttpExecutor::fast()` (4 workers — REST/API/thumbnails/small uploads) or `::slow()` (1 worker — multi-MB transfers, debug bundles). Submitted lambdas still need `helix::ui::queue_update()` / `tok.defer()` for UI. `include/http_executor.h`.
> **Non-HTTP IO** (BT/USB/RFCOMM, QR decode, device discovery): use an existing managed pool / BusThread, OR wrap `std::thread(...).detach()` in `try { ... } catch (const std::system_error&) { ... toast + error cb ... }` so spawn failure surfaces a toast instead of terminating. `feedback_no_bare_threads_arm.md`.
> **Member `std::thread` joined in dtor** (WifiBackendNetworkManager::connect_thread_, CameraStream::stream_thread_) is fine — the issue is one-shot detached spawns under load.
> Before adding a new `std::thread`, check if HttpExecutor or another managed pool already owns that domain.

### [L084] [*----|**---] SubjectLifetime must be a member, never a local
- **Uses**: 3 | **Velocity**: 0.75 | **Learned**: 2026-04-22 | **Last**: 2026-05-04 | **Category**: gotcha | **Type**: constraint
> Observing a dynamic subject (per-fan/per-sensor/per-extruder): the `SubjectLifetime` token MUST outlive the observer → it MUST be a member, never a local. A local `SubjectLifetime lt;` paired with a member `ObserverGuard` is a UAF: when `lt` falls off the stack, the observer's weak_ptr is dead but the observer is still registered against the (possibly recreated) subject. Companion to [L077].
> Rule: every member `ObserverGuard` on a dynamic subject needs a paired member `SubjectLifetime` next to it in the header. Same for vector members.
> Per-item collections (carousel pages, slot lists) → parallel vectors, kept in lockstep, lifetimes cleared BEFORE observers (#705):
>   `std::vector<ObserverGuard> carousel_observers_;`
>   `std::vector<SubjectLifetime> carousel_lifetimes_;`
> Read-only one-shot reads can use a local — but prefer the no-lifetime overload (`tsm.get_temp_subject(name)`).
> Reference: `src/ui/panel_widgets/fan_widget.cpp:218` (`speed_lifetime_`). Audit 2026-04-22 cleaned thermistor_widget.cpp; codebase clean.

### [L085] [*----|-----] release() is NEVER the default — reset() is
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-04-22 | **Last**: 2026-04-22 | **Category**: correction | **Type**: constraint
> New ObserverGuard cleanup: always `obs.reset()`. `release()` is NOT "safer" — it skips `lv_observer_remove()`, leaks the LambdaObserverContext, leaves a zombie observer whose deferred cb fires on stale `this`. `reset()` already handles shutdown via `s_subjects_valid` + `lv_is_initialized()`, safe even mid-`lv_deinit`.
> Only correct uses of `release()`: (a) `StaticSubjectRegistry::register_deinit()` cbs (pre-`lv_deinit`), (b) explicit shutdown paths where the subject is already destroyed. Widget `LV_EVENT_DELETE` during normal runtime is NOT one of those — use `reset()`.
> If you reason "release() seems safer because it skips the observer-remove" — that's the misconception that caused 17× #579. The remove call IS the point; skipping it leaks context and corrupts rendering state. Companion to [L073]. Audit 2026-04-22 fixed `ui_ams_current_tool.cpp`, `ui_heating_animator.cpp`, `ui_ams_mini_status.cpp`.

### [L086] [*----|-----] OpenWrt/procd silently skips plain SysV init scripts at boot
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-04-28 | **Last**: 2026-04-28 | **Category**: gotcha | **Type**: constraint
> On OpenWrt-derived firmware (Tina Linux on K2 series, possibly future Creality/Allwinner), procd's boot iterator only invokes `/etc/init.d/<name>` scripts that have BOTH `#!/bin/sh /etc/rc.common` shebang AND a `DEPEND=...` directive. Plain SysV (`#!/bin/sh` + `case "$1" in start)`) is silently skipped — even when symlinked from `/etc/rc.d/SXXname`. No log, no error.
> Symptom: device hangs at boot animation; no UI; SSH in → no `/tmp/helixscreen.log`, no helix-screen procs, no syslog entries. Manual `/etc/init.d/S99helixscreen start` works.
> Fix: install a procd shim at `/etc/init.d/<name>` with `#!/bin/sh /etc/rc.common`, `START=99 STOP=01 DEPEND=done`, and `boot()/start()/stop()/restart()/status()` that delegate to the SysV `/etc/init.d/SXX<name>`. Then `<shim> enable` for rc.d symlinks. See `install_procd_shim_k2()` in `scripts/lib/installer/service.sh`. Diag: `head -1 /etc/init.d/<name>` must show `/etc/rc.common`; `<script> boot; echo $?` must succeed; post-reboot `grep <tag> /var/log/messages` must show boot invocation.

### [L087] [*----|***--] Default-constructed nlohmann::json is NULL — `.value()` throws
- **Uses**: 2 | **Velocity**: 1 | **Learned**: 2026-05-06 | **Last**: 2026-05-06 | **Category**: gotcha | **Type**: constraint
> `nlohmann::json j;` is **JSON null**, not `{}`. `.value("k", def)` throws `type_error::306` on null. Bites upgrade paths: loader does `if (item.contains("config")) widget_config = item["config"];` — if absent, stays null → consumer `.value()` blows up (`5ac58e051` → `c3835003f`).
> Fix source: init with `json::object()`. Fix consumer: `j.is_object() && j.value("k", def)`. Do both.


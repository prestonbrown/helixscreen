# LESSONS.md - Project Level

> **Lessons System**: Cite lessons with [L###] when applying them.
> Stars accumulate with each use. At 50 uses, project lessons promote to system.
>
> **Add lessons**: `LESSON: [category:] title - content`
> **Categories**: pattern, correction, decision, gotcha, preference

## Active Lessons

### [L008] [**---|***--] Design tokens and semantic widgets
- **Uses**: 8 | **Velocity**: 1 | **Learned**: 2025-12-14 | **Last**: 2026-02-24 | **Category**: pattern | **Type**: informational
> No hardcoded colors or spacing. Prefer semantic widgets (ui_card, ui_button, text_*, divider_*) which apply tokens automatically. Don't redundantly specify their built-in defaults (e.g., style_radius on ui_card, button_height on ui_button). See docs/LVGL9_XML_GUIDE.md "Custom Semantic Widgets" for defaults.

### [L009] [***--|****-] Icon font sync workflow
- **Uses**: 13 | **Velocity**: 2 | **Learned**: 2025-12-14 | **Last**: 2026-02-21 | **Category**: gotcha | **Type**: constraint
> After adding icon to codepoints.h: add to regen_mdi_fonts.sh, run make regen-fonts, then rebuild. Forgetting any step = missing icon

### [L011] [***--|***--] No mutex in destructors
- **Uses**: 12 | **Velocity**: 1 | **Learned**: 2025-12-14 | **Last**: 2026-02-21 | **Category**: gotcha | **Type**: constraint
> Avoid mutex locks in destructors during static destruction phase. Other objects may already be destroyed, causing deadlock or crash on exit

### [L014] [***--|*****] Register all XML components
- **Uses**: 33 | **Velocity**: 7 | **Learned**: 2025-12-14 | **Last**: 2026-02-24 | **Category**: gotcha | **Type**: constraint
> When adding new XML components, must add lv_xml_component_register_from_file() call in main.cpp. Forgetting causes silent failures

### [L020] [***--|***--] ObserverGuard for cleanup
- **Uses**: 18 | **Velocity**: 1 | **Learned**: 2025-12-14 | **Last**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Use ObserverGuard RAII wrapper for lv_subject observers. Manual observer cleanup is error-prone and causes use-after-free on panel destruction

### [L021] [*----|-----] Centidegrees for temps
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2025-12-14 | **Last**: 2025-12-31 | **Category**: pattern | **Type**: informational
> Use centidegrees (int) for temperature subjects to preserve 0.1C resolution. Float subjects lose precision in LVGL binding

### [L025] [**---|-----] Button content centering
- **Uses**: 8 | **Velocity**: 0 | **Learned**: 2025-12-21 | **Last**: 2026-01-30 | **Category**: pattern | **Type**: constraint
> Text-only buttons: use `align="center"` on child. Icon+text buttons with flex_flow="row": need ALL THREE flex properties - style_flex_main_place="center" (horizontal), style_flex_cross_place="center" (vertical align items), style_flex_track_place="center" (vertical position of row). Missing track_place causes content to sit at top.

### [L031] [****-|*****] XML no recompile
- **Uses**: 55 | **Velocity**: 35.0075 | **Learned**: 2025-12-27 | **Last**: 2026-02-26 | **Category**: gotcha | **Type**: constraint
> XML files are loaded at RUNTIME - never rebuild after XML-only changes. Just relaunch the app. This includes layout changes, styling, bindings, event callbacks - anything in ui_xml/*.xml. Only rebuild when C++ code changes.

### [L039] [*----|***--] Unique XML callback names
- **Uses**: 3 | **Velocity**: 1 | **Learned**: 2025-12-30 | **Last**: 2026-02-24 | **Category**: pattern | **Type**: constraint
> All XML event_cb callback names must be globally unique using on_<component>_<action> pattern. LVGL's XML callback registry is a flat global namespace with no scoping. Generic names like on_modal_ok_clicked cause collisions when multiple components register handlers.

### [L040] [**---|****-] Inline XML attrs override bind_style
- **Uses**: 6 | **Velocity**: 3 | **Learned**: 2025-12-30 | **Last**: 2026-02-24 | **Category**: gotcha | **Type**: constraint
> When using bind_style for reactive visual changes, inline style attributes (style_bg_color, style_text_color, etc.) have higher priority in LVGL's style cascade. bind_style cannot override them. Solution: use TWO bind_styles (one per state) with NO inline styling for properties you want to change reactively.

### [L042] [*----|***--] XML bind_flag exclusive visibility
- **Uses**: 2 | **Velocity**: 1 | **Learned**: 2025-12-31 | **Last**: 2026-02-25 | **Category**: pattern | **Type**: informational
> Multiple bind_flag_if_eq on same object creates independent observers where last one wins (race condition). For 'show only when X=value' logic, use single bind_flag_if_not_eq instead. Example: bind_flag_if_not_eq ref_value="0" shows only when value IS 0.

### [L045] [*----|-----] XML dropdown options use &#10; entities
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-06 | **Last**: 2026-01-23 | **Category**: gotcha | **Type**: constraint
> LVGL dropdown options in XML use &#10; (newline entity) as separator: options="Auto&#10;3D View&#10;2D Heatmap". NEVER expand &#10; to literal newlines — XML parsers normalize literal newlines in attributes to SPACES (per XML spec), silently breaking all dropdown options into one entry. The format-xml.py correctly round-trips &#10; through lxml, but any tool that writes literal newlines into XML attributes will destroy them.

### [L046] [*----|-----] XML subject shadows C++ subject
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-06 | **Last**: 2026-01-30 | **Category**: correction | **Type**: constraint
> When XML <subjects> declares a subject with the same name as a C++-registered subject (UI_SUBJECT_INIT_AND_REGISTER_*), the XML component-local subject shadows the global C++ one. XML bindings will find the local subject (stuck at default value) instead of the C++ one. Solution: Don't declare XML subjects for values managed entirely by C++.

### [L048] [*----|-----] Async tests need queue drain
- **Uses**: 3 | **Velocity**: 0 | **Learned**: 2026-01-08 | **Last**: 2026-01-12 | **Category**: pattern | **Type**: constraint
> Tests calling async setters (functions using helix::async::invoke or ui_queue_update) must call helix::ui::UpdateQueue::instance().drain_queue_for_testing() before assertions. Without draining, the update is still pending and subjects won't have the new value. See test_printer_state.cpp for examples.

### [L051] [*----|-----] LVGL timer lifetime safety
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-08 | **Last**: 2026-01-23 | **Category**: gotcha | **Type**: constraint
> When using lv_timer_create with object pointer as user_data, wrap in struct that captures alive_guard. Check alive_guard BEFORE dereferencing object pointer to prevent use-after-free if object destroyed during timer delay.

### [L052] [**---|-----] Tag hv::EventLoop tests as slow
- **Uses**: 7 | **Velocity**: 0 | **Learned**: 2026-01-09 | **Last**: 2026-01-14 | **Category**: gotcha | **Type**: constraint
> Tests using hv::EventLoop (libhv network operations) MUST be tagged [slow] or they cause parallel test shards to hang indefinitely. This includes fixtures like MoonrakerRobustnessFixture, MoonrakerClientSecurityFixture, NewFeaturesTestFixture, EventTestFixture. The [slow] tag excludes them from default `make test-run` which uses filter `~[.] ~[slow]`.

### [L053] [*----|-----] Reset static fixture state in destructor
- **Uses**: 4 | **Velocity**: 0 | **Learned**: 2026-01-10 | **Last**: 2026-01-10 | **Category**: gotcha | **Type**: constraint
> Test fixtures using static state (e.g., static bool queue_initialized) MUST reset that state in the destructor. Otherwise, state persists across tests causing: 1) initialization to be skipped when it shouldn't, 2) shutdown to leave stale state for next test. Pattern: destructor calls shutdown(), then resets static flag to false.

### [L054] [**---|****-] Clear pending queues on shutdown
- **Uses**: 6 | **Velocity**: 2 | **Learned**: 2026-01-10 | **Last**: 2026-02-25 | **Category**: gotcha | **Type**: constraint
> Singleton queues (like UpdateQueue) MUST clear pending callbacks in shutdown(), not just null the timer. Without clearing, stale callbacks remain queued and execute on next init() with pointers to destroyed objects → use-after-free. Pattern: std::queue<T>().swap(pending_) to clear, then null timer.

### [L055] [*----|***--] LVGL pad_all excludes flex gaps
- **Uses**: 2 | **Velocity**: 1 | **Learned**: 2026-01-10 | **Last**: 2026-02-21 | **Category**: gotcha | **Type**: constraint
> `style_pad_all` only sets edge padding (top/bottom/left/right), NOT inter-item spacing. For zero-gap flex layouts, also need `style_pad_row="0"` (column) or `style_pad_column="0"` (row), or `style_pad_gap="0"` for both.

### [L056] [*----|-----] lv_subject_t no shallow copy
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-14 | **Last**: 2026-01-16 | **Category**: gotcha | **Type**: constraint
> lv_subject_t must NEVER be shallow-copied in move constructors/assignment. The struct contains internal state that becomes invalid when copied. In move operations, reinitialize the subject in the moved-to object instead of copying.

### [L057] [*----|-----] Subject deinit before destruction
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-14 | **Last**: 2026-01-16 | **Category**: gotcha | **Type**: constraint
> Classes owning lv_subject_t members must call lv_subject_deinit() in their destructor. Without deinit, observers attached to the subject leak and may fire after destruction causing use-after-free.

### [L059] [*----|-----] Use lv_obj_safe_delete for LVGL cleanup
- **Uses**: 1 | **Velocity**: 0 | **Learned**: 2026-01-20 | **Last**: 2026-01-20 | **Category**: pattern | **Type**: constraint
> Always use lv_obj_safe_delete() instead of raw lv_obj_delete() - it guards against shutdown race conditions by checking lv_is_initialized() and lv_display_get_next() before deletion, and auto-nulls the pointer to prevent use-after-free

### [L060] [***--|*****] Interactive UI testing requires user
- **Uses**: 27 | **Velocity**: 26.009999999999998 | **Learned**: 2026-02-01 | **Last**: 2026-02-25 | **Category**: correction | **Type**: constraint
> NEVER use timed delays expecting automatic navigation. THE EXACT PATTERN THAT WORKS:
> **Step 1** - Start app with Bash tool using `run_in_background: true`:
> ```bash
> ./build/bin/helix-screen --test -vv -p panel_name 2>&1 | tee /tmp/test.log
> ```
> Key: Use Bash tool parameter `run_in_background: true` - NOT shell `&` or `timeout`.
> **Step 2** - Tell user EXACTLY what to click/do in the running app.
> **Step 3** - WAIT for user to confirm they're done.
> **Step 4** - Read the log file: `Read tool on /tmp/test.log`
> FAILURES: shell `&`, `timeout X cmd &`, multiple attempts, assuming automatic navigation. ONE background task, tee for logging, user does interaction, you read results.

### [L061] [*----|****-] AD5M test printer environment
- **Uses**: 2 | **Velocity**: 2 | **Learned**: 2026-02-07 | **Last**: 2026-02-24 | **Category**: system
> AD5M (192.168.1.67, root@) runs armv7l Linux 5.4.61 (BusyBox). Key gotchas: (1) No curl, only wget - and wget has NO HTTPS support (compiled without SSL). (2) No sftp-server - use 'scp -O' (legacy protocol) instead of default scp. (3) Logging: default level is WARN, app logs to BOTH /tmp/helixscreen.log AND syslog (/var/log/messages) - syslog has the CURRENT session, /tmp/helixscreen.log may be stale from previous session. (4) No CA certificate bundle shipped - /etc/ssl/certs/ is empty, breaks ALL outbound HTTPS (libhv, wget). Must ship ca-certificates.crt with install. (5) No openssl CLI command. (6) No inotify support. (7) No WiFi (wpa_supplicant present but no interfaces). (8) OpenSSL 1.1 libs exist at /usr/lib/libssl.so.1.1. (9) Binary at /opt/helixscreen/, config at /opt/helixscreen/config/helixconfig.json. (10) ldd may return empty for statically-linked ARM binaries.

### [L062] [**---|*****] AD5M build and deploy targets
- **Uses**: 5 | **Velocity**: 5 | **Learned**: 2026-02-07 | **Last**: 2026-02-25 | **Category**: build
> AD5M cross-compilation uses 'make ad5m-docker' (Docker-based ARM cross-compile), NOT 'make pi-test' (which targets Raspberry Pi). Deploy with 'AD5M_HOST=192.168.1.67 make ad5m-deploy'. The pi-test target is for a different device entirely.

### [L064] [*----|****-] Commit generated translation artifacts
- **Uses**: 3 | **Velocity**: 3 | **Learned**: 2026-02-10 | **Last**: 2026-02-24 | **Category**: i18n
> After syncing translation YAML files, must also regenerate and commit the compiled artifacts: src/generated/lv_i18n_translations.c, src/generated/lv_i18n_translations.h, and ui_xml/translations/translations.xml. These are tracked in git (not gitignored) for cross-compilation support. The build regenerates them automatically, but they won't be staged unless you explicitly add them.

### [L065] [-----|-----] No test-only methods on production classes
- **Uses**: 0 | **Velocity**: 0 | **Learned**: 2026-02-11 | **Last**: 2026-02-11 | **Category**: patterns
> WRONG: Adding public methods like reset_for_testing(), clear_startup_grace_period_for_testing() on production classes. Pollutes API, ships test code to users, creates coupling. FOUND: 40+ instances across AbortManager (15 callback simulators), sensor managers, printer state classes. RIGHT: Use friend class pattern — add 'friend class FooTestAccess;' in private section, define FooTestAccess in the test .cpp file with static methods that access private members. Example: FilamentSensorManagerTestAccess::reset(mgr) instead of mgr.reset_for_testing(). For state machine callbacks (like AbortManager), consider a testable interface/mock instead of exposing every internal transition.

### [L066] [*----|*****] LVGL flex_grow row_wrap trick
- **Uses**: 4 | **Velocity**: 4 | **Learned**: 2026-02-11 | **Last**: 2026-02-24 | **Category**: lvgl
> When using flex_grow on a container with flex_flow=row_wrap, LVGL calculates wrap points based on the container's natural (content) width, NOT the flex-allocated width. Fix: set width="1" + flex_grow="1" — forces LVGL to use the grown width for wrapping. Without this, children overflow instead of wrapping.

### [L067] [*----|****-] Wrap C++ UI strings in lv_tr()
- **Uses**: 2 | **Velocity**: 2 | **Learned**: 2026-02-14 | **Last**: 2026-02-24 | **Category**: ui
> All user-visible English strings in C++ code must be wrapped in lv_tr() for i18n. Dropdown options are concatenated strings so they're harder to translate - but labels, help text, toasts, etc. must use lv_tr().

### [L068] [-----|-----] Cancel LVGL animations before object deletion
- **Uses**: 0 | **Velocity**: 0 | **Learned**: 2026-02-15 | **Last**: 2026-02-15 | **Category**: lvgl
> When deleting LVGL objects that have animations with completion callbacks, ALWAYS cancel animations FIRST (lv_anim_delete) before lv_obj_delete/lv_obj_safe_delete. The completion callback may fire synchronously during lv_anim_delete, causing use-after-free if the object is already freed. Pattern: (1) nullify member pointer, (2) clear state flags, (3) lv_anim_delete, (4) lv_obj_delete. For animations using 'this' as var: set guard flags to false BEFORE lv_anim_delete so callbacks become no-ops.

### [L069] [**---|*****] Never assume lv_obj user_data ownership — it may already be set
- **Uses**: 6 | **Velocity**: 6 | **Learned**: 2026-02-15 | **Last**: 2026-02-24 | **Category**: architecture
> LVGL's lv_obj_set_user_data() is a single shared slot per object. Custom XML widgets, component handlers, and LVGL internals may set user_data during object creation (e.g., ui_button stores button_data_t*, severity_card stores a severity string). NEVER call delete/free on lv_obj_get_user_data() unless you are 100% certain you set it yourself on that specific object. NEVER use user_data as general-purpose storage on objects you didn't fully create — XML components and custom widgets may have claimed it already. **CRITICAL: NEVER walk parent chain checking any non-null user_data to find an instance pointer** — ui_button and other widgets set their own user_data, so the traversal will find the WRONG data and miscast it (caused SEGFAULT in AmsOperationSidebar/AmsDryerCard). Instead, walk parents checking `lv_obj_get_name()` for a specific known name, THEN read user_data from that named object. For per-item data, prefer: (1) event callback user_data (separate per-callback), (2) a C++ side container (map/vector indexed by object pointer), or (3) lv_obj_find_by_name to stash data in a hidden child label.

### [L071] [**---|*****] XML child click passthrough
- **Uses**: 7 | **Velocity**: 6 | **Learned**: 2026-02-21 | **Last**: 2026-02-26 | **Category**: ui | **Type**: constraint
> When a parent view has an event_cb for "clicked", all child objects (lv_obj, icon, text_body, text_tiny, etc.) must have `clickable="false" event_bubble="true"` or they absorb the click before it reaches the parent's callback. LVGL objects are clickable by default.

### [L070] [*----|****-] Don't lv_tr() non-translatable strings
- **Uses**: 3 | **Velocity**: 3 | **Learned**: 2026-02-17 | **Last**: 2026-02-24 | **Category**: i18n
> Never wrap product names (Spoolman, Klipper, Moonraker, HelixScreen), URLs/domains, technical abbreviations used as standalone labels (AMS, QGL, ADXL), or universal terms (OK, WiFi) in lv_tr(). Add '// i18n: do not translate' comment explaining why. Sentences CONTAINING product names ARE translatable — 'Restarting HelixScreen...' is fine because 'Restarting' translates. Material names (PLA, PETG, ABS, TPU, PA) also don't get translated or translation_tag in XML.

### [L072] [**---|*****] Never capture bare this in async/WebSocket callbacks
- **Uses**: 6 | **Velocity**: 6 | **Learned**: 2026-02-22 | **Last**: 2026-02-25 | **Category**: gotcha | **Type**: constraint
> Callbacks passed to execute_gcode(), send_jsonrpc(), or any Moonraker API call fire from the WebSocket thread AFTER the widget/panel may be destroyed. NEVER capture [this] — use weak_ptr<bool> alive guard or capture value copies only. Pattern: `std::weak_ptr<bool> weak = alive_; api->call([weak, name_copy]() { if (weak.expired()) return; ... });`

### [L073] [*----|****-] ObserverGuard release vs reset
- **Uses**: 2 | **Velocity**: 2 | **Learned**: 2026-02-22 | **Last**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Use obs.reset() when subjects are ALIVE (normal cleanup, repopulate) — properly unsubscribes. Use obs.release() ONLY when subjects may already be DESTROYED (shutdown, pre-deinit) — avoids double-free. Wrong choice = crash: reset() on dead subject = double-free, release() on live subject = dangling observer = use-after-free.

### [L074] [*----|****-] Generation counter for deferred observer callbacks
- **Uses**: 2 | **Velocity**: 2 | **Learned**: 2026-02-22 | **Last**: 2026-02-23 | **Category**: pattern | **Type**: informational
> When repopulating dynamic widget lists with observers, bump a generation counter BEFORE cleanup. Capture the counter in observer callbacks and check `if (gen != self->gen_) return;` to skip stale deferred callbacks that fire after the old widgets are deleted. Prevents use-after-free from observe_int_sync's deferred dispatch.

### [L075] [*----|***--] Validate lv_obj before accessing children
- **Uses**: 1 | **Velocity**: 1 | **Learned**: 2026-02-22 | **Last**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Before calling lv_obj_find_by_name(), lv_obj_get_child(), or lv_obj_get_child_count() on a cached widget pointer, ensure the pointer is not stale. Use null checks and alive guards (weak_ptr pattern) — NOT lv_obj_is_valid() which is O(n) recursive and can stack overflow on Pi (see L076). Use safe_delete_obj() instead of raw lv_obj_delete() to null pointers after deletion. For async callbacks, use alive guards to detect if the owning panel was destroyed.

### [L076] [*----|****-] NEVER use lv_obj_is_valid() in hot paths
- **Uses**: 2 | **Velocity**: 2 | **Learned**: 2026-02-22 | **Last**: 2026-02-25 | **Category**: gotcha
> lv_obj_is_valid() does a RECURSIVE O(n) walk of ALL screens and ALL children via obj_valid_child(). On Pi with thousands of widgets, this causes stack overflow SIGSEGV. NEVER use in: observer callbacks, animation callbacks (pulse_anim_cb), timer callbacks, loops, destructor paths, or safe_delete_obj(). Use simple null pointer checks instead. Only safe in one-shot event handlers (button clicks) where tree is stable and call happens once. This caused a real user crash in v0.10.14 — HeatingIconAnimator::apply_color() called lv_obj_is_valid(icon_) from observer callback during startup, recursed infinitely, SIGSEGV after 1 second.

### [L077] [-----|-----] Dynamic subject observers MUST use SubjectLifetime tokens
- **Uses**: 0 | **Velocity**: 0 | **Learned**: 2026-02-22 | **Last**: 2026-02-22 | **Category**: gotcha
> When observing dynamic subjects (per-fan, per-sensor, per-extruder), always use the get_*_subject(name, lifetime) overload and pass the lifetime token to the observer factory function. Without it, lv_subject_deinit() frees the observer but ObserverGuard::reset() calls lv_observer_remove() on freed memory → SEGV. Static singleton subjects don't need tokens.


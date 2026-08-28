# LESSONS.md - Project Level

> **Lessons System**: Cite lessons with [L###] when applying them.
> Stars accumulate with each use. At 50 uses, project lessons promote to system.
>
> **Add lessons**: `LESSON: [category:] title - content`
> **Categories**: pattern, correction, decision, gotcha, preference

## Active Lessons

### [L008] Design tokens and semantic widgets
- **Learned**: 2025-12-14 | **Category**: pattern | **Type**: informational
> No hardcoded colors/spacing. Use semantic widgets (ui_card, ui_button, text_*, divider_*) — they apply tokens. Do not restate built-in defaults (style_radius on ui_card, button_height on ui_button). Defaults: `docs/devel/LVGL9_XML_GUIDE.md` § "Custom Semantic Widgets".

### [L009] Icon font sync workflow
- **Learned**: 2025-12-14 | **Category**: gotcha | **Type**: constraint
> Add icon to codepoints.h → add to regen_mdi_fonts.sh → `make regen-fonts` → rebuild. Skip any step = missing icon.

### [L011] No mutex in destructors
- **Learned**: 2025-12-14 | **Category**: gotcha | **Type**: constraint
> No mutex locks in dtors during static destruction — other objects may already be gone, deadlocks/crashes on exit.

### [L014] Register all XML components
- **Learned**: 2025-12-14 | **Category**: gotcha | **Type**: constraint
> New XML components need a `lv_xml_register_component_from_file()` call in `src/xml_registration.cpp` (via the local `register_xml()` helper) — NOT main.cpp, and note the word order: `register_component`, not `component_register`. Forgetting = silent failure (component resolves to nothing, no error).

### [L020] ObserverGuard for cleanup
- **Learned**: 2025-12-14 | **Category**: gotcha | **Type**: constraint
> Use `ObserverGuard` RAII for `lv_subject` observers. Manual cleanup → UAF on panel destruction.

### [L025] Button content centering
- **Learned**: 2025-12-21 | **Category**: pattern | **Type**: constraint
> Text-only buttons: `align="center"` on child. Icon+text with `flex_flow="row"` need all three: `style_flex_main_place="center"` (horiz), `style_flex_cross_place="center"` (cross), `style_flex_track_place="center"` (row position). Without track_place content sits at top.

### [L031] XML no recompile
- **Learned**: 2025-12-27 | **Category**: gotcha | **Type**: constraint
> ui_xml/*.xml loads at RUNTIME — never rebuild for XML-only changes (layout, styling, bindings, event cbs). Just relaunch. Rebuild only for C++ changes.

### [L039] Unique XML callback names
- **Learned**: 2025-12-30 | **Category**: pattern | **Type**: constraint
> XML `event_cb` names live in a flat global namespace (no scoping). Use `on_<component>_<action>` to avoid collisions. Generic names (on_modal_ok_clicked) collide across components.

### [L040] Inline XML attrs override bind_style
- **Learned**: 2025-12-30 | **Category**: gotcha | **Type**: constraint
> Inline style attrs (style_bg_color, style_text_color, …) outrank `bind_style` in LVGL's cascade. For reactive visuals, drop the inline attr and use TWO bind_styles (one per state) — no inline styling on the reactive property.

### [L042] XML bind_flag exclusive visibility
- **Learned**: 2025-12-31 | **Category**: pattern | **Type**: informational
> Multiple `bind_flag_if_eq` on the same object = independent observers, last write wins (race). For "show when X==v" use a single `bind_flag_if_not_eq` with the inverted ref. Eg `bind_flag_if_not_eq ref_value="0"` shows only when value IS 0.

### [L045] XML dropdown options use &#10; entities
- **Learned**: 2026-01-06 | **Category**: gotcha | **Type**: constraint
> LVGL dropdown options separator is `&#10;` (newline entity): `options="Auto&#10;3D View&#10;2D Heatmap"`. Never expand to literal newlines — XML normalizes them to spaces in attrs (per spec), silently merging all options into one entry. format-xml.py preserves `&#10;` via lxml; other tools won't.

### [L046] XML subject shadows C++ subject
- **Learned**: 2026-01-06 | **Category**: correction | **Type**: constraint
> An XML `<subjects>` declaration shadows a same-named C++ subject (UI_SUBJECT_INIT_AND_REGISTER_*) — the local one wins, bindings stick at default. Don't declare XML subjects for values C++ owns.

### [L048] Async tests need queue drain
- **Learned**: 2026-01-08 | **Category**: pattern | **Type**: constraint
> Tests calling async setters (helix::async::invoke / ui_queue_update) must `UpdateQueue::instance().drain()` before assertions, else the update is still queued and the subject reads stale. Pattern: test_printer_state.cpp. NOTE: this lesson previously named `drain_queue_for_testing()`, which does not exist anywhere in the tree — verified by repo-wide grep 2026-07-25 after it was cited into a subagent brief and correctly rejected. The API is `drain()`, declared include/ui_update_queue.h:218.

### [L051] LVGL timer lifetime safety
- **Learned**: 2026-01-08 | **Category**: gotcha | **Type**: constraint
> `lv_timer_create` cb fires after the owning object may be destroyed. Do not pass raw `this` as user_data. Use `AsyncLifetimeGuard::token()`: capture `tok` in the timer cb, then `tok.defer([this](){ ... })` so the body only runs if `this` is still alive. Older `alive_guard` / `weak_ptr<bool>` patterns are deprecated. Full rules: `docs/devel/THREADING.md` §2.

### [L052] Tag thread/network tests as [slow] to prevent hangs
- **Learned**: 2026-01-09 | **Category**: gotcha | **Type**: constraint
> Tests using `std::thread` / `std::condition_variable` / `hv::EventLoop` MUST be tagged `[slow]` — `make test-run` filters `~[.] ~[slow]`, so untagged thread tests deadlock parallel shards. Concurrency, not speed. Known offenders: MoonrakerRobustnessFixture, MoonrakerClientSecurityFixture, NewFeaturesTestFixture, EventTestFixture, BedMeshRenderThread tests. When tests hang, check untagged thread tests FIRST.

### [L053] Reset static fixture state in destructor
- **Learned**: 2026-01-10 | **Category**: gotcha | **Type**: constraint
> Test fixtures using static state (`static bool queue_initialized`) MUST reset in dtor — otherwise it persists, init gets skipped on next test, shutdown leaves stale state. Pattern: dtor calls shutdown() then resets the flag to false.

### [L054] Clear pending queues on shutdown
- **Learned**: 2026-01-10 | **Category**: gotcha | **Type**: constraint
> Singleton queues (UpdateQueue) MUST clear pending callbacks in shutdown(), not just null the timer — stale entries fire on next init() against destroyed pointers → UAF. Pattern: `std::queue<T>().swap(pending_)`, then null the timer.

### [L055] LVGL pad_all excludes flex gaps
- **Learned**: 2026-01-10 | **Category**: gotcha | **Type**: constraint
> `style_pad_all` only sets edge padding (top/bottom/left/right), NOT inter-item spacing. For zero-gap flex layouts, also need `style_pad_row="0"` (column) or `style_pad_column="0"` (row), or `style_pad_gap="0"` for both.

### [L056] lv_subject_t no shallow copy
- **Learned**: 2026-01-14 | **Category**: gotcha | **Type**: constraint
> `lv_subject_t` cannot be shallow-copied — internal state breaks. Move ctors/assigns must reinitialize the subject in the destination, not copy.

### [L057] Subject deinit before destruction
- **Learned**: 2026-01-14 | **Category**: gotcha | **Type**: constraint
> Classes owning `lv_subject_t` members must call `lv_subject_deinit()` in dtor. Else observers leak and fire on freed subject → UAF.

### [L059] LVGL object deletion: pick the RIGHT strategy
- **Learned**: 2026-01-20 | **Category**: pattern | **Type**: constraint
> Pick by scenario: (1) `safe_delete(obj)` — sync, shutdown-safe, auto-nulls; use in dtors/teardown when NOT inside an UpdateQueue/async batch. (2) `safe_delete_deferred(obj)` — routes through LVGL own async list, so it escapes the batch; use inside async cbs (timers, network responses). (3) `lv_obj_delete_async(obj)` — LVGL builtin, auto-cancelled by `obj_delete_core()`; use when another path may delete first, but NOT if a parent `lv_obj_clean()` may run before it fires (double-free). (4) `lv_obj_delete(obj)` — raw, no guards, LVGL internals only. (5) `helix::ui::safe_delete_subtree(obj)` — teardown-safe for a whole grid/flex subtree before a rebuild (#983). NEVER `lv_async_call(..., lv_obj_delete)` — uncancellable (#399). NEVER `safe_delete`/`lv_obj_delete`/`lv_obj_clean` inside any queued or deferred cb — multiple sync deletes in one batch corrupt LVGL event list → SIGSEGV in `lv_event_mark_deleted` (#776, #190, #80). ALWAYS cancel anims first ([L068]). Full rules: `docs/devel/THREADING.md` §3.

### [L060] Drive the UI with ctl; ask the user only for visual confirmation
- **Learned**: 2026-02-01 | **Category**: correction | **Type**: constraint
> The `-p/--panel` flag is GONE — use `helix-screen ctl` (navigate/click/ls/set_value/scroll/screenshot, or `repl`); the server auto-starts under `--test`. Reaching a panel, clicking a widget, and capturing a screenshot are all scriptable now, so do NOT ask the user for those. What still needs a human is judging what the pixels look like. When you do need that: (1) `Bash` with `run_in_background: true`: `./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log` — NOT shell `&` or `timeout`; (2) drive it to the state with `ctl`; (3) tell the user exactly what to look at; (4) wait for confirmation; (5) `Read /tmp/test.log`. Never fake verification with timed delays. For an active print use `--sim-speed 4..10` instead of waiting ~95s.

### [L061] AD5M test printer environment
- **Learned**: 2026-02-07 | **Category**: system
> AD5M (192.168.1.67, root@). armv7l Linux 5.4.61 (BusyBox). Gotchas: (1) wget no HTTPS, no curl. (2) No sftp-server — `scp -O`. (3) Logs go to BOTH `/tmp/helixscreen.log` AND syslog (`/var/log/messages`); syslog is current session, file may be stale. Default level WARN. (4) `/etc/ssl/certs/` empty — breaks all outbound HTTPS (libhv, wget); ship `ca-certificates.crt`. (5) No `openssl` CLI. (6) No inotify. (7) No WiFi (wpa_supplicant present, no interfaces — but see project_ad5m_wifi_actually_works.md). (8) OpenSSL 1.1 at `/usr/lib/libssl.so.1.1`. (9) Binary at `/opt/helixscreen/`, config `/opt/helixscreen/config/helixconfig.json`. (10) `ldd` may return empty for static ARM binaries.

### [L062] AD5M build and deploy targets
- **Learned**: 2026-02-07 | **Category**: build
> AD5M build: `make ad5m-docker` (Docker ARM cross), NOT `make pi-test` (Pi). Deploy: `AD5M_HOST=192.168.1.67 make ad5m-deploy`.

### [L064] Commit generated translation artifacts
- **Learned**: 2026-02-10 | **Category**: i18n
> After syncing translation YAMLs (`make translation-sync`), also stage the generated XML artifacts: `ui_xml/translations/translations.xml` **and the per-language `ui_xml/translations/*.xml`** (regenerated by `make translations`). Tracked (not gitignored), not auto-staged. NOTE: the old compiled `src/generated/lv_i18n_translations.{c,h}` were RETIRED in `8fb3ca3de` (XML-only pipeline now) — they no longer exist; do not look for them. Workflow: wrap new user-facing strings in `lv_tr("...")` (code) or `label_tag="<literal text>"` (XML), then `make translation-sync` (extracts from XML+C++, adds keys to all 9 langs as empty-string placeholders) → `make translations` → stage YAMLs + `ui_xml/translations/*.xml`.

### [L065] No test-only methods on production classes
- **Learned**: 2026-02-11 | **Category**: patterns
> No public `*_for_testing()` on production classes (ships test code, couples API; audit found 40+). Use friend `FooTestAccess` in test .cpp touching privates, e.g. `FilamentSensorManagerTestAccess::reset(mgr)`. State-machine cbs → testable interface/mock over exposing transitions. See [L088].

### [L066] LVGL flex_grow row_wrap trick
- **Learned**: 2026-02-11 | **Category**: lvgl
> `flex_grow` + `flex_flow=row_wrap`: LVGL wraps against natural (content) width, not the grown width — children overflow. Fix: `width="1" flex_grow="1"` to force wrap against the allocated width.

### [L067] Wrap C++ UI strings in lv_tr()
- **Learned**: 2026-02-14 | **Category**: ui
> All user-visible English in C++ goes through `lv_tr()` (labels, help text, toasts, etc.). Dropdown options are concatenated strings, harder to translate; do those carefully but don't skip the rest.

### [L068] Cancel LVGL animations before object deletion
- **Learned**: 2026-02-15 | **Category**: lvgl
> Cancel animations BEFORE deleting their object — `lv_anim_delete` may fire the completion cb synchronously, UAF if obj is freed. Order: (1) null member ptr, (2) clear state flags, (3) `lv_anim_delete`, (4) `lv_obj_delete`. For anims with `this` as var: set guard flags false BEFORE lv_anim_delete so cbs no-op.

### [L069] Never assume lv_obj user_data ownership — it may already be set
- **Learned**: 2026-02-15 | **Category**: architecture
> `lv_obj_set_user_data()` = single shared slot; XML widgets/handlers/LVGL internals may already own it (ui_button→button_data_t*, severity_card→string). NEVER free/cast user_data you didn't set on THAT exact obj. NEVER walk the parent chain for non-null user_data (finds ui_button's → miscast SEGV, AmsOperationSidebar/AmsDryerCard). Find by `lv_obj_get_name()`, read user_data from that named obj. Per-item data: per-cb event user_data, C++ map keyed by ptr, or hidden named child.

### [L071] XML child click passthrough — lv_obj is clickable by default, and clickable="false" does NOT inherit
- **Learned**: 2026-02-21 | **Category**: ui | **Type**: constraint
> Root with a click handler → every absorbing descendant needs `clickable="false" event_bubble="true"`. `lv_obj`/`ui_card`/`ui_dialog` are clickable by DEFAULT (`lv_obj_constructor`, lv_obj.c:584); only lv_image/label/line/menu/spinner aren't — tell: "thumbnail works, text area dead" (#1101). `lv_indev_search_obj` (lv_indev.c:618) recurses on GEOMETRY, deepest child wins → guard EVERY offender (#1101 needed 4). `clickable="true"` on lv_obj = no-op. Don't lint-"fix" the inverse: backdrop-dismiss roots (context_backdrop/menu) WANT absorb — test instead: `lv_indev_search_obj(test_screen(), &p)` in XMLTestFixture asserts tap target. Refs: test_print_file_card_hittest.cpp, ui_xml/setting_action_row.xml.

### [L070] Don't lv_tr() non-translatable strings
- **Learned**: 2026-02-17 | **Category**: i18n
> Don't `lv_tr()`: product names (Spoolman, Klipper, Moonraker, HelixScreen), URLs/domains, standalone tech abbreviations (AMS, QGL, ADXL), universal terms (OK, WiFi). Mark with `// i18n: do not translate`. Sentences containing product names ARE translatable ("Restarting HelixScreen…" — "Restarting" translates). Material names (PLA, PETG, ABS, TPU, PA) are also not translated, no translation_tag in XML.

### [L072] Never capture bare this in async/WebSocket callbacks
- **Learned**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Callbacks to `execute_gcode()` / `send_jsonrpc()` / Moonraker fire from the WS thread, possibly after the widget is gone. Never capture raw `[this]`. Use `lifetime_.bg_cb(tag, fn)`, or `AsyncLifetimeGuard::token()` + `tok.defer(...)`. Never write a bare `if (tok.expired()) return;` on a bg thread — that is the TOCTOU anti-pattern the lint gate rejects. Older `weak_ptr<bool>` / `shared_ptr<atomic<bool>>` patterns are deprecated. Full rules: `docs/devel/THREADING.md` §2.

### [L073] ObserverGuard: reset() is the default, release() almost never
- **Learned**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Always `reset()` for normal cleanup (panel teardown, LV_EVENT_DELETE, repopulate) — it unsubscribes, frees the context, and expires weak_alive, and it already handles shutdown via the `s_invalidation_epoch` generation counter + `lv_is_initialized()` (ui_observer_guard.h:206-208; replaced the old global `s_subjects_valid` boolean). `release()` is NOT "safer": it skips `lv_observer_remove()`, leaks the context, and leaves a zombie observer that fires a deferred cb on stale `this` — the misconception behind 17 separate #579 reports (release in unregister_slot_data → NEON blend SIGSEGV). The remove IS the point. `release()` only for StaticSubjectRegistry::register_deinit cbs where the subject is already destroyed. Full rules: `docs/devel/THREADING.md` §6.

### [L074] Generation counter for deferred observer callbacks
- **Learned**: 2026-02-22 | **Category**: pattern | **Type**: informational
> When repopulating dynamic widget lists with observers, bump a generation counter BEFORE cleanup. Capture in cbs: `if (gen != self->gen_) return;`. Skips stale deferred cbs from `observe_int_sync` that fire after old widgets are gone (UAF guard).

### [L075] Validate lv_obj before accessing children
- **Learned**: 2026-02-22 | **Category**: gotcha | **Type**: constraint
> Before `lv_obj_find_by_name()` / `lv_obj_get_child()` / `lv_obj_get_child_count()` on a cached pointer: null-check + `AsyncLifetimeGuard` token check. NOT `lv_obj_is_valid()` (O(n), stack-overflows on Pi — see [L076]). Use `safe_delete_obj()` to null pointers post-delete. For async cbs detecting panel destruction: capture `tok = lifetime_.token()` and gate with `tok.defer(...)` (`docs/devel/THREADING.md` §2); the older `weak_ptr<bool>` alive-guard pattern is deprecated.

### [L076] NEVER use lv_obj_is_valid() in hot paths or async guards
- **Learned**: 2026-02-22 | **Category**: gotcha
> `lv_obj_is_valid()` = recursive O(n) walk of all screens+children → Pi stack-overflow SIGSEGV. NEVER in observer/anim/timer cbs, loops, dtors, `safe_delete_obj()`, async guards — use null checks. Deferred-delete guards: app tracking (ModalStack) or `lv_obj_delete_async()`. Can return TRUE on recycled memory → delete a live obj (#399). Only safe in one-shot click handlers.

### [L077] Hand the fetched SubjectLifetime to observe_* (member vs local is not the rule)
- **Learned**: 2026-02-22 | **Category**: gotcha
> Observing a dynamic subject (per-fan/sensor/extruder): use the `get_*_subject(name, lifetime)` overload AND pass that token to the observer factory. The factories take it as a defaulted 4th parameter (`const SubjectLifetime& lifetime = {}`, observer_factory.h:333), so omitting it is SILENT — the guard gets no token, `subject_dead` never becomes true, and `ObserverGuard::reset()` calls `lv_observer_remove()` on memory freed by `lv_subject_deinit()` → SEGV (#705). Real occurrence: print_status_widget.cpp fetched both extruder lifetimes and passed neither; fixed 3a32d1140.
> 
> CORRECTION 2026-07-26: the old "token MUST be a member, never a local" claim was FALSE and is retired. The accessors ASSIGN the owner's own shared_ptr into your variable (printer_fan_state.cpp:534, temperature_sensor_manager.cpp:510, printer_temperature_state.cpp), and the owner keeps its copy, so a caller's local dying never drops the refcount or expires the guard's weak_ptr. Owners signal death via `*lifetime = false`, which is what `reset()` actually reads. ~10 local-lifetime sites in the tree were audited and none is a bug. Reset/declaration order is likewise not load-bearing for owner-held tokens — though declaring the token BEFORE the observer stays correct if a token ever becomes exclusively owned, so prefer it. Collections → parallel vectors. Static singleton subjects need no token. Full rules: docs/devel/THREADING.md §5 (rewritten in 66d4aa9d2).

### [L078] lv_obj transform_scale invisible without background
- **Learned**: 2026-03-13 | **Category**: gotcha
> `transform_scale` on an `lv_obj` with transparent bg only affects the object's own draw (border/bg), not children (separate draw units). For press feedback on transparent containers (back buttons), use `lv_style_set_opa` — applies to the entire object layer including children.

### [L079] LVGL 9.5 DRAW_TASK_ADDED cannot add draw tasks
- **Learned**: 2026-03-29 | **Category**: lvgl
> LVGL 9.5: `DRAW_TASK_ADDED` cbs fire AFTER `DRAW_MAIN_END/DRAW_POST` — `lv_draw_rect/_triangle/_fill` from there draws nothing. Broke chart gradient fills that worked in 9.4-pre. Fix: do custom fills in `DRAW_MAIN_END`, compute positions via `lv_chart_get_y_array()` + `lv_map()`. Gotcha: `lv_draw_fill` VER gradient `frac=0` is BOTTOM, `frac=255` is TOP. Use `lv_draw_fill` (not `lv_draw_rect`) for gradient-only fills to avoid bg_color bleed.

### [L080] Verify deployment chain before user interaction
- **Learned**: 2026-04-16 | **Category**: gotcha
> Before asking user to interact on-device, verify in one pass: (1) NEW binary running (PID start time / version in log), (2) logs land where you expect (journalctl/file/console), (3) required state on (telemetry, debug level in helixscreen.env), (4) logs reachable via SSH. Each failed round-trip burns user patience. Pi: systemctl → journalctl; `deploy-pi-fg` uses `ssh -t` (console only); nohup drops output. Production log capture: systemd + journalctl.

### [L082] Percent size inside LV_SIZE_CONTENT parent collapses to 0
- **Learned**: 2026-04-20 | **Category**: gotcha | **Type**: constraint
> LVGL percent size (`width="50%"`, `min_width="50%"`) resolves vs parent content area; parent `LV_SIZE_CONTENT` → circular dep, collapses to 0, child vanishes. Symptom: `long_mode="wrap"`+`flex_grow` wraps near-per-char (super-tall cards); grown flex child squeezed out. Fix: explicit parent width, then child `100%`. Never nest percent kids in content-sized parents (toast stack 26573f1f2).

### [L083] Never `std::thread(...).detach()` for fire-and-forget work
- **Learned**: 2026-04-22 | **Category**: gotcha | **Type**: constraint
> `pthread_create` EAGAIN under thread exhaustion (AD5M/CC1/MIPS32) → `std::thread` ctor throws → through LVGL C frame / noexcept boundary → `std::terminate`, crash looks unrelated (#724, #837, #811-adjacent RatOS HTTP storm).
> HTTP: `HttpExecutor::fast()` (4w: REST/thumbs/small uploads) / `::slow()` (1w: big transfers). Lambdas still need `queue_update`/`tok.defer` for UI. `include/http_executor.h`.
> Non-HTTP IO (BT/USB/RFCOMM/QR/discovery): managed pool/BusThread, OR wrap detach in `try{…}catch(std::system_error){toast+err cb}`.
> Member `std::thread` joined in dtor is fine; issue is one-shot detached spawns. Check for an existing pool first.

### [L086] OpenWrt/procd silently skips plain SysV init scripts at boot
- **Learned**: 2026-04-28 | **Category**: gotcha | **Type**: constraint
> OpenWrt/procd (Tina Linux K2, Allwinner) boot iterator only runs `/etc/init.d/<name>` with BOTH `#!/bin/sh /etc/rc.common` shebang AND a `DEPEND=`. Plain SysV silently skipped even if symlinked SXXname — no log. Symptom: hang at boot anim, no UI/helix procs/log, manual `/etc/init.d/SXX start` works. Fix: procd shim `/etc/init.d/<name>` (`START=99 STOP=01 DEPEND=done`, boot/start/stop delegate to SysV), then `<shim> enable`. See `install_procd_shim_k2()` service.sh. Check: `head -1` shows rc.common.

### [L087] Default-constructed nlohmann::json is NULL — `.value()` throws
- **Learned**: 2026-05-06 | **Category**: gotcha | **Type**: constraint
> `nlohmann::json j;` = JSON null, not `{}`; `.value("k",def)` throws type_error::306 on null. Bites loaders: absent key stays null → consumer `.value()` blows up (5ac58e051→c3835003f). Fix both: init `json::object()`; consume `j.is_object() && j.value(...)`.

### [L088] Test-only methods belong in TestAccess friend classes
- **Learned**: 2026-05-22 | **Category**: pattern
> tests/shell/test_code_lint.bats forbids _for_testing suffix methods in include/*.h or src/*.cpp. Pattern: declare 'friend class FooTestAccess;' on the production class, define FooTestAccess in tests/test_helpers/foo_test_access.h with static methods that access private members (e.g., 'static void apply_sample(PerformanceState& ps, const PerfSample& s) { ps.apply_sample(s); }'). Mocks (*_mock.h) are exempt — whole file is test infra. Template: tests/test_helpers/update_queue_test_access.h.

### [L089] Regen XML linter schema after adding C++ widget
- **Learned**: 2026-05-22 | **Category**: gotcha
> After registering a new widget via lv_xml_register_widget() in src/ui/*.cpp (custom widgets like helix_sparkline, ui_card, helix_3d_viewer), run 'make regen-xml-schema' and commit tools/xml-linter/schema/schema.json. The linter auto-discovers from C++ source at schema-generation time but reads the *committed* schema in CI — forgetting this fails the XML Lint workflow with 'unknown-widget'. Analogous to L064 (translation artifacts).

### [L090] resolve-backtrace.sh orphans addr2line against the big pi DWARF
- **Learned**: 2026-06-12 | **Category**: gotcha
> scripts/resolve-backtrace.sh forks one addr2line PER address vs multi-GB pi.debug DWARF (~2.6G); each child grows lazily 4→8G+. Kill it → subshell+addr2line children ORPHAN, invisible to pkill (name truncates 'aarch64-linux-g'); 3 parallel resolves once ~26G, near-OOM. RULES: (1) run_in_background:true from the START (harness owns the tree); (2) don't hand-fork addr2line in a chainable shell; (3) one resolver, no parallel retries; (4) cleanup = kill PARENT resolve-backtrace.sh (`pgrep -af resolve-backtrace`), find big procs via /proc/PID/cmdline.

### [L091] Stale-but-200 R2 manifest silently suppresses updates fleet-wide
- **Learned**: 2026-06-12 | **Category**: gotcha
> "New version not showing on ANY device" = source of truth, not per-device: updater fetches releases.helixscreen.org/<ch>/manifest.json FIRST, trusts any HTTP-200 (update_checker.cpp fetch_stable_release), only falls back to GitHub on FETCH FAILURE not staleness. v0.99.76 cause: release.yml R2 upload non-blocking, manifest uploaded AFTER big zips; a 504 on k2.zip aborted before manifest → R2 pinned at .75, run green. Diagnose: curl live manifest .version vs tag; check the R2 upload job. Fixed 942bcbd51/d0034b282: manifest before zips, s3cp retry, read-back assert version==tag. Verify the SERVED artifact, never trust upload success.

### [L092] make | tail masks exit code; -j hides the real build error
- **Learned**: 2026-06-12 | **Category**: gotcha
> Piping make through tail/head makes the Bash step report exit 0 even when make failed (the exit code is tail's). Capture it separately: 'make ...; echo $? > /tmp/exit'. When a (cross-)build dies with NO 'error:' line and a DIFFERENT failure point each run, suspect interleaved parallel output hiding the real first error OR resource contention (check 'free -h' and 'pgrep -af cc1plus' for a sibling session building on the same box) — drop to -j2 to serialize and surface the true first error before concluding root cause. Bit me triaging a worktree snapmaker-u1 cross-build; real cause was missing $(LV_CONF) in the splash/display/watchdog sub-builds, invisible under -j.

### [L093] Pure-decision-function tests need input realism
- **Learned**: 2026-06-16 | **Category**: gotcha
> A pure decision function's tests are only as strong as whether their inputs match what the function actually receives at runtime. decide_preview_action() tests passed while it had a deadlock because they fed view_mode=1/2 (3D/2D), but at print start the view-mode subject is 0 (thumbnail) and only flips after gcode loads. Result: green tests + on-device failure. When a pure fn takes a runtime-derived input, assert against the value it actually holds at the call site (0 at print start), not a convenient one.

### [L094] Don't gate a load/fetch on display-output state it produces
- **Learned**: 2026-06-16 | **Category**: gotcha
> Gating a load decision on a state that only updates AFTER the load completes deadlocks. The print-status gcode download was gated on the view-mode subject being 3D/2D, but that subject only becomes 3D/2D once gcode is loaded -> gcode never downloads, mode never leaves thumbnail, 3D render never appears (user saw 'thumbnail not 3D'). Gate loads on intent/settings (want_viewer + render-mode setting), never on the rendered result. Found in PrintStatusPanel preview unification.

### [L095] Verify feature existence in code, not from issue phrasing + commit messages
- **Learned**: 2026-07-01 | **Category**: correction
> Don't claim a capability is absent from issue wording + commit messages — grep/read the actual code first (reporter "can't find X" usually = discoverability gap, not missing). Spoolman picker existed (AmsEditModal, behind "Choose Spool") despite 6 fix-commits implying otherwise (#1071). Corollary: don't inherit a subagent's "race" claim from a stale comment — verify current code. **Extends to reporter-proposed root-cause MECHANISMS, not just existence:** #1124's two bugs each had detailed, plausible reporter archaeology pointing at the WRONG cause — bug 2 "panel graph never migrated to the backfill path" (it uses TempGraphController + backfill already; real cause = persistent graph backfilled pre-WebSocket-connect), bug 1 "init_fans resets the subject to 0" (struct+subject zero in lockstep, snapshot re-fires; real cause = e3f92c3f4's front-most fan fallback, a 2-day-old regression). Both real causes were RECENT commits. Trace the suspect area in current code AND `git log -S`/blame it before adopting the reporter's mechanism; a confident, well-argued mechanism from a technical reporter is still a hypothesis.

### [L096] queue_prev tag-ring names the victim, not the crash — resolve real frames first
- **Learned**: 2026-07-02 | **Category**: correction
> Crash-handler queue_prev/queue_prev2 = last N *completed* UpdateQueue cbs (victim context), NOT a stack — don't investigate the named cb. Run `scripts/resolve-backtrace.sh --crash-file <f> <platform>` FIRST for real PC/RA + FP-walk. #983 grid walk-off signature: LV_COORD_MAX 0x1FFFFFFF in a reg + fault_addr==heap_end+1 + deep repeated layout_update_core/grid_update. Generic guard v0.99.76; PrinterImageWidget-attach arm (lv_image_set_src→layout recursion) v0.99.78 (#1025). Burned twice (PerformanceState::apply_sample, TSM::update_subjects). Companion L090/L095.

### [L097] LV_SYMBOL_OK renders as tofu on body-font labels — use icon font for C++-built glyphs
- **Learned**: 2026-07-02 | **Category**: gotcha
> Montserrat LV_SYMBOL_OK/CHECK aren't in body/text fonts → C++ `lv_label_set_text(lbl, LV_SYMBOL_OK)` renders tofu. For glyphs in C++-built rows: resolve icon font `lv_xml_get_font(nullptr, lv_xml_get_const(nullptr,"icon_font_xs"))` + `ui_icon::lookup_codepoint("check")`, apply to label, fixed-width column for alignment (mirror PrinterSwitchMenu/MaterialPickerMenu). Unit tests miss missing glyphs — caught in interactive verify. Related L009.

### [L098] Python Moonraker-plugin mocks must reflect the REAL API, not an imagined one
- **Learned**: 2026-07-12 | **Category**: gotcha
> helix_print.py coded against a fantasy Moonraker API; hand-rolled mocks implemented the fantasy → wrong calls shipped GREEN a month (bundle RA6EPJTZ: `'KlippyConnection' has no attribute 'run_gcode'`). Real API (Moonraker d5ee171): Klipper via `lookup_component("klippy_apis")` (run_gcode/start_print/do_restart), NOT klippy_connection (only request(WebRequest)); `database`=sql_execute; `history`=get_job/save_job. FIX: mocks `MagicMock(spec_set=[real method names])` so nonexistent-method calls raise AttributeError (reproduces the crash, no Moonraker import). spec_set catches nonexistent attr, not wrong signature. Companion L088.

### [L099] Recycled PanelWidget keeps layout bool → stale imperative DOM
- **Learned**: 2026-07-16 | **Category**: pattern
> PanelWidgetManager reuses widget instances across rebuilds (attach(new)+on_size_changed on the SAME instance); a member layout flag (is_wide_/is_column_) persists but the fresh XML component starts at its defaults. on_size_changed's `if(mode==flag_)return` then skips the apply when the new size matches the stale flag → stuck at XML default (#1109 active_spool white spool; print_status card stuck column at 1x2/3x2). Fix: hoist the imperative apply to a helper, call from attach() too. Immune: widgets recomputing every call (nozzle_temps/tips) or driven by retained subjects.

### [L100] Lossy member vector leaks through every public getter
- **Learned**: 2026-07-20 | **Category**: correction
> Compacting/filtering/reordering a class member vector silently changes the semantics of EVERY public getter that returns it — audit all accessor consumers, not just your feature's render path. FilamentMappingCard::set_used_tools compacted tool_info_ to used-tools-only (correct for the chip row), but that member is also returned by get_filament_tool_info(); two print-start dialogs indexed it positionally as tool_info[tool_index] (valid only while position==tool_index on the full palette). After compaction the "no matching filament" dialog rendered EMPTY and the material-mismatch warning was silently suppressed — for exactly the used-but-unresolved case the feature existed to surface. No crash (bounds guards), so unit tests + the chip screenshot were green; only adversarial review caught it. Fix: key consumers by identity (find_by_tool_index), or keep the member full and filter at render. Positional-access-by-domain-id is a latent fragility even before you make the member lossy. Merged 746dd0c74.

### [L101] No auto-creating getter in shutdown callbacks
- **Learned**: 2026-07-24 | **Category**: gotcha
> StaticSubjectRegistry::deinit_all() runs AFTER StaticPanelRegistry::destroy_all(), so a deinit lambda calling get_global_X_panel() RESURRECTS the singleton. The replacement is never destroyed while LVGL/spdlog are alive — its dtor runs in __run_exit_handlers and reads the freed spdlog registry (43 valgrind invalid accesses in ~Modal, MacrosPanel + PIDCalibrationPanel). Test the pointer instead: register_deinit("X", []{ if (g_x_panel) g_x_panel->deinit_subjects(); }). Same rule for any teardown callback reaching a lazy getter.

### [L102] bats '! cmd' is exempt from errexit — mid-test negative assertions are no-ops
- **Learned**: 2026-07-26 | **Category**: gotcha
> POSIX exempts the '!' reserved word from errexit, and bats runs each @test body under 'set -e'. So a bare '! grep -q X file' that is NOT the final statement of the body is a SILENT NO-OP: its non-zero status is swallowed and only the last command decides the test result. A trailing one is fine. Found 2026-07-26: 65 such assertions across 25 files in tests/shell/ were proving nothing; the suite was green either way, so a passing run is NOT evidence the fix worked. Use refute / refute_sh / refute_grep from tests/shell/helpers.bash instead. Detect: parse @test bodies, flag /^\s*!\s/ lines that are not the last statement (skip '! cmd || { ...; return 1; }' and 'if ! ...; then' — those work). Verify any fix by A/B: refute a pattern that IS present must fail the test, where the old '!' form passes.

### [L103] Read the routed subsystem doc before reverse-engineering
- **Learned**: 2026-08-09 | **Category**: correction
> CLAUDE.md's doc table already routes every subsystem; check it FIRST, then verify against code. Cost of skipping: extracted a ZMOD firmware tarball and ran a web search to establish that _IFS_VARS/variable_backup come from the lessWaste/bambufy plugins and not stock ZMOD, when docs/devel/FILAMENT_MANAGEMENT.md:2037 already said 'Stock lacks this'. COROLLARY, and why reading alone is not enough: docs/devel/plans/*.md are point-in-time and some PRESCRIBE approaches the code has since diverged from. plans/2026-06-25-ad5x-ifs-seated-chan-robustness.md:63-65 instructs you to gate on head_filament_, which include/ams_backend_ad5x_ifs.h:808-825 documents as untrustworthy on its own (shipped gate: head_switch_seen_ && !head_switch_present_). Read plans as history, not instructions. Also expect self-contradiction inside long docs: FILAMENT_MANAGEMENT.md:2039 and :2118 say 'less_waste / zmod', attributing the prefix to stock ZMOD and contradicting :2037 two lines above. When a doc disagrees with itself, the code decides.

### [L104] Touching ui_xml means make regen-tokens
- **Learned**: 2026-08-10 | **Category**: gotcha
> There are now FOUR generated-artifact gates that fail AFTER your change looks done, not at build time. Adding or editing any ui_xml file changes the design-token set, so src/generated/theme_token_table.cpp goes stale and tests/unit/test_theme_token_table.cpp fails with an unreadable 'REQUIRE(table == scanned)' full of {?} placeholders - the test's own header comment is the only hint: 'run: make regen-tokens'. It surfaces in the FULL suite (shard ~83, tag [theme][tokens]), not in the tag you were iterating on, so a green [ams] run proves nothing. CRITICALLY, the PRE-COMMIT HOOK DOES NOT CHECK ANY OF THESE - it runs 40+ gates and reports 'Quality checks passed', which is NOT evidence the generated artifacts are in sync. Committing on a green hook is exactly how this ships. Only make test-run catches it. The full set: make regen-tokens for ui_xml token changes; make translation-sync + make translations for new lv_tr() strings, staging ui_xml/translations/*.xml (L064); make regen-fonts for new icon codepoints (L009); make regen-xml-schema after lv_xml_register_widget, committing tools/xml-linter/schema/schema.json (L089) - though note the hook DOES regenerate and re-stage schema.json itself, which is why that one feels handled and the others are not. Note a NEW XML COMPONENT needs regen-tokens but NOT regen-xml-schema - the schema only tracks registered C++ widgets. Corollary on verification: do not end a build check with 'grep -c error:' - grep returns exit 1 on zero matches, so a clean build reports as a failed command. Capture the build rc separately.

### [L105] set_active() same-panel early return swallows on_activate()
- **Learned**: 2026-08-12 | **Category**: gotcha
> NavigationManager::set_active() returns immediately when panel_id == active_panel_ (ui_nav_manager.cpp), so NO lifecycle callback fires. Opening an overlay does NOT change active_panel_ - push_overlay() only calls on_deactivate() on the main panel. So tapping the navbar button for the panel you are ALREADY on while an overlay is open runs switch_to_panel_impl(): it clears the overlay stack and un-hides the panel, then set_active() no-ops, and the panel is left VISIBLE BUT DEACTIVATED forever. Any widget that restarts work in on_activate() (CameraWidget::start_stream) stays dead until you bounce to a different panel and back. go_back() is immune because it uses the separate restore_activation_pending_ latch + activate_restored_target(). This is the THIRD bug in this family (e3127a1cf, 80128a213, and this one) - when a panel renders blank/stale after closing an overlay, always diff the two close paths in a -vvv trace and look for 'Deactivating main panel N for overlay' with no matching 'Activating main panel N after overlay closed'. Note the nav activation logs are spdlog::trace, invisible at -vv.

### [L106] Non-trivial change = mandatory docs pass, devel AND user
- **Learned**: 2026-08-12 | **Category**: correction
> After ANY non-trivial feature add/change/removal, do a docs pass BEFORE calling it done - not just devel docs, user-facing too when the change is visible to a user. Evidence: ams/force_bypass_controls ('Enable Bypass Controls', the override that surfaces bypass + external spool on CFS/ACE/Snapmaker/toolchanger/QIDI and actually ENABLES bypass on Happy Hare) shipped with ZERO doc coverage - grep for it across docs/ returned nothing. Worse, the existing user docs asserted the opposite: filament.md said the Bypass toggle is 'Only shown if your hardware supports bypass', which the override makes false. Undiscoverable except by scrolling the overlay. Checklist for a user-visible setting: (1) docs/user/guide/<area>.md - the how and why, (2) docs/user/guide/settings/<page>.md - where it lives in the UI, (3) docs/user/CONFIGURATION.md - the JSON key, its type/default, and the SECTION (per-printer df()+'ams/...' keys are NOT the top-level 'ams' block), (4) docs/devel/<SUBSYSTEM>.md - the mechanism + a per-backend/per-case matrix, (5) any per-printer doc in docs/devel/printers/ that has a capability row the change touches, (6) re-read nearby claims and fix ones the change falsified. Then run scripts/check_doc_refs.py. docs/user/CLAUDE.md already states the rule ('New settings: Add to appropriate guide/settings/*.md AND CONFIGURATION.md') - follow it. Also generalize: Preston caught me documenting the override as CFS-only when 5 backends share the shape and Happy Hare behaves differently; enumerate every backend/platform that hits the code path, don't document the one that prompted the question.

### [L107] Responsive token ladders: measure every tier, never derive from one
- **Learned**: 2026-08-16 | **Category**: correction
> Adding or changing a responsive px ladder in ui_xml/globals.xml (*_micro.._xxlarge) means rendering and MEASURING at every breakpoint. In #1277 I measured micro, screenshotted it, reported the fix verified - and derived the other six tiers by arithmetic. Five of seven were wrong: medium clipped the modal button row by 33px, large by 72px, tiny by 1px. Deriving-instead-of-measuring is the exact mistake that CREATED the bug (dialog_content_max was one global ladder sized for one chrome shape), so reproducing it while fixing it is very easy. Method: if the screen is unreachable in mock mode add a hook to show_demo_overlay() in src/application/application.cpp (the ams-error-toast hook documents this as the sanctioned reason); then per size, HELIX_SCREEN_SIZE=WxH ./build/bin/helix-screen --test --remote-socket <sock>, ctl demo <name>, ctl geom <widget>. Derive true chrome as (clamped card height + overhang past card bottom - content height), then cap = 85%cap - chrome with ~8px spare. Resolutions that actually SELECT each tier (the suffix follows the cramped axis, so these are not guesses): 480x272 micro, 480x320 tiny, 640x400 small, 800x480 medium, 1024x600 large, 1200x900 xlarge, 1280x1024 and above xxlarge. Judge from ctl geom numbers, not screenshots - a screenshot only proves the one breakpoint you rendered.

### [L108] Widening a predicate reaches previously unreachable code paths
- **Learned**: 2026-08-16 | **Category**: gotcha
> Loosening a guard admits inputs into callees that were correct only because the old guard excluded that case. Swapping the Widget Catalog from is_enabled() to is_placed() was two lines, but is_placed() searches all pages, so it made rows clickable for widgets whose entry lives on another page - and place_widget_from_catalog() only searched the current page, so it push_backed a duplicate ID. Two entries, widget renders on two pages, delete_entry() removes only the first: silent config corruption from the supposedly safe narrow fix. Ask of every widened predicate: what does the callee assume that the old guard used to guarantee?

### [L109] A green wrapper exit is not a green test run
- **Learned**: 2026-08-16 | **Category**: correction
> `make test-run > log 2>&1; echo "EXIT=$?"` reports the SHELL LINE's status, so the harness reads exit 0 even when make returned 1. I reported "all shards green, 95/95" off that while the log's last line was `X One or more test shards failed!` - 2 assertions in test_theme_token_table, from adding px tokens to ui_xml without `make regen-tokens` (L104). Same family as piping make through tail (L092): the filter's exit code masks the real one. Never conclude from an exit code you did not read directly; grep the log itself for `assertions: .*failed`, `FAILED:` and the summary line, and treat a green wrapper as no evidence at all. Corollary for RUNTIME probes: spdlog's file sink buffers, so SIGTERMing the app truncates the log tail - a missing log line is NOT proof the code path did not run. Read live state instead with `helix-screen ctl get <subject>` / `ctl geom <widget>`; note `ctl geom` answers "Widget not found" for a hidden widget, which is itself the cheapest proof that a `hidden` binding fired.

### [L110] Photo reads via the vision bridge are hypotheses, not locale evidence
- **Learned**: 2026-08-17 | **Category**: correction
> A glare-heavy phone photo of the print-details page came back from the vision bridge as "appears to be Chinese/Japanese characters" — explicitly low-confidence — and I built an entire diagnosis on it (zh locale + old release window) when the user was on ENGLISH UI hitting the v0.99.114 en.xml-skip regression I had already reproduced live. The correct move: treat a vision read of a PHOTO as unreadable unless confident, and confirm the UI language from settings.json / logs / a debug bundle before letting locale drive root cause. Corollary from the same bug: when a commit's premise is a data invariant ("en.xml maps all 2739 tags to themselves"), ENUMERATE the data before trusting it — the 19 non-identity keys were one grep away, and the 'fix the data, not the machinery' answer (rename keys to their English text, keep the optimization) only became visible once enumerated. Prefer fixes that make the failure mode impossible (English-text tags self-heal to English) over runtime machinery that compensates.

### [L111] Zero log-file growth at debug = buffered sink, not a dead logger
- **Learned**: 2026-08-19 | **Category**: gotcha
> logging_init.cpp sets logger->flush_on(warn): info/debug lines sit in the rotating file sink's userspace buffer indefinitely on an idle printer, so 0 bytes of file growth and a mid-line-truncated tail are NORMAL steady state, not a wedge or dead pipeline. Signature that fooled me on the U1 (2026-08-19): healthy process (main thread hrtimer_nanosleep, WS thread epoll_wait, TCP established), live UI confirmed by the user, yet log frozen mid-word for minutes. Flush probe when ctl is not compiled into the cross build: trigger any warn-level event - a klippy FIRMWARE_RESTART produces the 'Klipper disconnected' warning which flushed ~67KB of 'missing' debug lines in one shot. Read the flush policy BEFORE the /proc/net/tcp and wchan forensics; I nearly A/B-deployed a base build chasing a bug that did not exist. Extends L109 (SIGTERM truncation).

### [L112] Dilate-and-overpaint is not a silhouette algorithm
- **Learned**: 2026-08-19 | **Category**: gotcha
> Drawing an object wider in a highlight colour then painting its own strokes over the middle only leaves a rim where the covering strokes are denser than the dilation. It floods on sloped geometry: a cone's outer wall shifts laterally each layer so 7px halos tile across the slope and ~2px of white survives PER LAYER (solid white blob by layer 120/218), while a cylinder looks perfect because its walls stack vertically and the halos coincide. The 3D equivalent, inverted-hull (mesh pushed along normals + glCullFace(GL_FRONT)), assumes a WATERTIGHT mesh so back faces sit behind the real surface; G-code is a soup of independent extrusion tubes, so back faces poke through as ~6% white speckle. Both need edge detection on a per-object coverage mask. Zero-memory route: apply_ssao() already edge-detects 'filled pixel with an empty neighbour' - draw the selected object with a sentinel alpha (254) and detect on that, instead of a 143KB object-id buffer. Also apply_ssao darkens every boundary pixel by OUTLINE_DARKEN=0.3, so a 1px white rim is consumed entirely (255->76), and SSAO is ON by default so any test disabling it for determinism tests a config production never runs.

### [L113] Instrument any complicated path you cannot directly observe
- **Learned**: 2026-08-19 | **Category**: pattern
> Whenever you need to see what a path is doing and cannot observe it directly - a render pass, an async chain, a cache/invalidation path, a state machine, anything with multiple plausible failure points - add instrumentation BEFORE adjusting parameters. I adjusted constants three times, then blamed two other subsystems, before one counter found the cause on its first run: the code under test was not the code being changed. Corollaries, each of which cost a cycle: a missing log line proves nothing until you confirm the path executes at all; never build an 'until grep -q <marker>' wait on a marker that only exists if the feature works (it hangs instead of failing); ALWAYS read the build exit code before believing test or runtime output (a make exit of 2 handed me stale-binary results twice and I accepted both); a mutation whose pattern matches no source text is a no-op that reads green, so assert the match count; and a measurement script that fails silently is worse than none - mine inferred the background as 'most common colour', which on a saturated image picked the foreground and reported 16 hits on an image that was obviously saturated. Log the derived values too, not just a hit count - one line showing an input was 1 pixel wide explained the whole behaviour.

### [L114] Verify WHICH gcode renderer is live before judging appearance
- **Learned**: 2026-08-19 | **Category**: gotcha
> Auto resolves to 3D on any ENABLE_GLES_3D build, so print-status defaults to GLES on desktop/pi. The viewer's top-right '3D' badge names the live renderer and is the cheapest check. The cube button in the viewer is NOT a 2D/3D toggle - on_view_toggle_clicked flips complete_view_mode_ (printed-so-far vs whole-model, ghost off). 'ctl set settings_gcode_render_mode N' usually does nothing: it writes the subject directly, bypassing DisplaySettingsManager::set_gcode_render_mode (so no persistence), and the print-status observer only acts if (gcode_viewer_ && is_active_), so setting it from another panel is silently dropped. What works: seed display/gcode_render_mode before launch, in settings-test.json under --test (0=Auto 1=3D 2=2D 3=Thumbnail), confirmed by the log pair 'Set G-code render mode: N (settings)' then 'Render mode set to 2D_LAYER'. GLES cannot run headless: SDL_CreateWindow fails under SDL_VIDEODRIVER=dummy, so 3D verification needs a real display.

### [L115] The sample that works may be the best case - pick an adversarial one
- **Learned**: 2026-08-19 | **Category**: pattern
> A borrowed technique carries preconditions, and the first test case can satisfy them by accident. A selection outline built on dilate-then-overpaint looked perfect on a cylinder (vertical walls, so every layer's dilation lands in the same place) and turned the object into a solid white blob on a cone (sloped walls, so each layer's dilation tiles across the surface). The 3D variant used inverted-hull, which requires a WATERTIGHT mesh; toolpath geometry is unconnected tubes, so it speckled. Both preconditions were invisible until a shape violated them. Before declaring a visual or geometric technique working, name its precondition out loud and choose a sample that breaks it - sloped vs vertical, concave vs convex, sparse vs dense, one layer vs hundreds. Same shape of error as tests that assert 'some effect appeared': mine passed through both a full flood and striping, because 'white > 0' and 'coverage grew' are true of the broken output too. Bound the effect (a rim is a small FRACTION of the object) rather than asserting its presence.

### [L116] Cherry-pick main-based fixes onto a devel-branch, never merge
- **Learned**: 2026-08-19 | **Category**: gotcha
> devel/1.1 and main diverge by hundreds of commits, so merging a main-based fix branch into a devel/1.1-based feature branch drags in all of it - my attempt conflicted across input-shaper, temp-graph and nine translation YAMLs, none related to the fix. Cherry-pick the specific commits instead. When both branches appended tests to the same file, git's hunks cut THROUGH functions: concatenating 'ours then theirs' splices one side's block into the middle of the other's function body and fails to compile. Rebuild the file instead - 'git show <sha>:<path>' for the incoming version, then re-append only your own contiguous additions plus includes. A failed 'cherry-pick --continue' also leaves sequencer state that makes the NEXT cherry-pick fail with 'already in progress'; 'git cherry-pick --quit' clears it without touching HEAD or the working tree, unlike --abort which reverts. Landing on main itself is the opposite case - prefer a 3-way merge there.

### [L117] New src/ files need an ESP32 decision, and the exclusion file has sections
- **Learned**: 2026-08-19 | **Category**: gotcha
> A pre-commit gate fails while any src/ file is 'not decided for the ESP32 firmware build', and it lists files OTHER branches added, so it blocks commits that never touched them (I had to classify another feature's ui_keycap_style.cpp to land unrelated work). app_srcs.txt = compiled (the keyboard subsystem is in the v1 Core+AMS cut); app_srcs_excluded.txt = not, tagged '# not in the v1 Core+AMS cut', where all src/rendering/gcode_* lives. The trap: that file is NOT one flat sorted list - an early section excludes whole directories ('src/calibration/  # all 5 src/ files beneath') and a later '# --- individual files ---' section holds per-file entries, so a naive sorted insert lands ~100 lines from its siblings in the wrong section. Anchor the insert on a neighbouring file in the same directory. Do not use --write-exclusions; it answers 'exclude' for every undecided file at once.

### [L118] Idle signal without a report means the final message never landed - ping for a re-send
- **Learned**: 2026-08-21 | **Category**: gotcha
> Spawned subagents (implementers AND reviewers) repeatedly finished with an idle_notification carrying no content - the agent's final message never arrived as a message. This happened 8+ times in one orchestration session, once per agent, and each one stalls a task loop that is waiting on a report or a review verdict. The fix is a single SendMessage: "Your idle notification arrived but the report did not - re-send verbatim." It works every time; the agent still holds its context and re-emits the report in one round-trip. Do NOT investigate (TaskOutput does not know the agent name), do NOT re-dispatch the task (wasted duplicate work), and above all do NOT Read the task's .output file to recover it - for local_agent tasks that path is a symlink to the full JSONL transcript and will overflow your context. Treat idle-without-report as the expected failure mode of long report-carrying agents, not a sign the agent died.

### [L119] A '--' inside an XML comment silently kills component registration
- **Learned**: 2026-08-25 | **Category**: gotcha
> XML forbids '--' inside comments. A header comment like 'being one -- an lv_textarea' makes lv_xml_register_component_from_data fail with 'XML parsing error; not well-formed (invalid token) on line N', the component never registers, and every use of that tag is skipped with its children REPARENTED into the enclosing parent (so the panel half-renders instead of erroring). The runtime message is actively misleading: 'XML tag X is not a known widget/element/component/slot -- likely an unregistered widget in a STALE BINARY (rebuild required)' sends you rebuilding for nothing. ALWAYS grep a new/edited ui_xml file for '--' inside <!-- --> before running. Use ':' or an em-less dash instead. Cost me a full relaunch cycle on setting_value_field.xml.

### [L120] Closing a pushed overlay re-activates the parent and clobbers just-entered values
- **Learned**: 2026-08-25 | **Category**: gotcha
> ui_keypad_show() pushes the keypad via NavigationManager, so ui_keypad_hide() pops it and fires the PARENT overlay's on_activate() again. Any panel that refreshes there (MachineLimitsOverlay::query_and_show, RetractionSettingsOverlay::sync_from_printer_state) will overwrite the value the user just typed with the printer's pre-edit value. Timeline observed: Confirmed value=50000 at T+0ms, on_activate at T+6ms, Got machine limits accel=10000 at T+12ms, then the 250ms debounced apply SENT 10000. Silent data loss -- the panel just looks like it ignored you. Fix: a returning_from_keypad_ flag set before ui_keypad_show() and consumed in on_activate() to skip the refresh. Set it unconditionally BEFORE show, not from ui_keypad_is_visible() after -- if the push were ever async the conditional version silently reintroduces the bug. OverlayBase has no 'returning from child' concept, so every panel adding keypad entry needs this guard.

### [L121] A cross-singleton push inside init_subjects() needs the pushee registered FIRST
- **Learned**: 2026-08-28 | **Category**: gotcha
> AmsState::init_subjects() asks the factory for a backend and syncs it through build_ams_topology() straight into ToolState::set_ams_topology() - but SubjectInitializer::init_ams_subjects() brought ToolState up six lines LATER. lv_subject_set_int() on a zeroed lv_subject_t only WARNS ("Subject type is not LV_SUBJECT_TYPE_INT (type=0)"; type 0 is LV_SUBJECT_TYPE_INVALID = never initialized, not wrong type) and drops the write, while the plain C++ members the same call rebuilt keep the new value. tools_ said 4 tools, tool_count said 0, needs_rebuild suppressed every later republish, and the boot log cheerfully printed "AMS topology applied: 4 tools (version 1)" the whole time. It only recovered because PrinterDiscovery's init_tools() happened to clear the override 0.3s later - pure luck, not design. Finding it took one gdb run: `break lv_observer.c:131` + `break lv_observer.c:151`, commands { bt 25; continue }, and the backtrace names the caller directly - do NOT grep for it. Two rules fall out: (1) any init_subjects() that reaches into another singleton must have that singleton registered FIRST, which also puts the pushing side ahead of it in StaticSubjectRegistry's reverse deinit order; (2) a mutator that publishes to subjects must refuse the whole call before its own init_subjects() rather than half-apply it, because half-applied state diverges silently and nothing republishes.


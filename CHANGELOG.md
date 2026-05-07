# Changelog

All notable changes to HelixScreen will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.99.56] - 2026-05-06

The headline is the **structural fix** for the cluster:pstat-async-delete crash family — v0.99.55 already closed the render-thread half (#929), and v0.99.56 closes the other: an **array-backed event stack** that replaces LVGL's linked-list `event_head` with a global array, eliminating the wild-pointer dereference path that's been the dominant production crash signature for weeks (#793/#840/#871/#878/#880/#906). Plus a defensive NULL-guard layer on `lv_obj_event.c` for the new VHTR49QJ signature, and a suppress-the-dialog fix for truncated crash files that were generating useless bundle submissions. Round it out with a **K2 phase-tracker fix** that stops mis-flagging BED_MESH on profile-load echoes, a **theme-init DPI fix** for BTT CB1 / sun4i framebuffers reporting bogus `width_mm`, and a **label-printer font-fit fix** for longer vendor strings on continuous tape.

### Fixed

- **Array-backed LVGL event stack (#907 — cluster:pstat-async-delete structural fix)** — `lv_event_mark_deleted` previously walked a linked list of stack-allocated `lv_event_t` via `e->prev` pointers chained through `event_head`. Any frame returning without popping (exception unwind, control-flow bug) stranded a stack pointer that the next walk would dereference, often after heap reuse had written ASCII bytes over it — the dominant crash signature in #793/#840/#871/#878/#880/#906. The chain is now a fixed `lv_event_t * event_stack[256]` in `_lv_global_t`; mark_deleted iterates the array and never dereferences `e->prev`. Per-slot stack-bounds + alignment validation (inherited from the v0.99.54 dispatch-depth-guard work) catches any remaining stomp into the array. Also consolidates the file-static `event_dispatch_depth` into `_lv_global_t::event_depth` so post-mortem heap snapshots can read it.
- **NULL-guards on `lv_obj_event.c` accessors (VHTR49QJ defense)** — `lv_obj_get_event_count`, `lv_obj_get_event_dsc`, `lv_obj_remove_event`, `lv_obj_remove_event_dsc`, `lv_obj_remove_event_cb`, and `lv_obj_remove_event_cb_with_user_data` all had release-mode `LV_ASSERT_NULL` (a no-op) on the obj argument. Bundle VHTR49QJ (v0.99.53/ad5x, 2026-05-04) crashed with `fault_addr=0x0` reaching the field load via a deferred async callback that didn't validate widget lifetime. Replace the ASSERT with a runtime check that bails to a sane return value and surfaces telemetry — matches the established `lvgl_observer_null_guards` pattern.
- **Suppress crash dialog for unparseable crash files (CHUQCNAE)** — bundle CHUQCNAE shipped a `crash.txt` containing only `signal:\n` after the signal handler was killed mid-write (OOM / watchdog / power loss). Application::run was still showing the crash report dialog and letting the user submit a useless empty bundle. Now consumes the file and skips the dialog when `read_crash_file` returns null. Regression test pins the contract.
- **K2 phase tracker no longer false-fires BED_MESH on profile load** — K2 `PRINT_START` emits `BED_MESH_PROFILE LOAD="default"` early in pre-print to reuse the saved mesh (no calibration). The profile's BED_MESH regex matched these echoes and falsely transitioned to BED_MESH for several seconds before HEATING_BED took over. Reduces the alternation to `BED_MESH_CALIBRATE` (still covers `BED_MESH_CALIBRATE_START_PRINT`). Also drops three dead profile entries that logged warnings on every load (signal_format with `pattern` instead of `prefix`; HEATING_CHAMBER and LOADING_FILAMENT response patterns referencing phases not in the `PrintStartPhase` enum).
- **TempGraphWidget null-config crash on legacy home-panel layouts** — `set_config` was reading `config.value("follow_overlay", false)` without guarding against JSON null. Layouts saved before v0.99.54 omit `"config"` and `parse_widget_array` hands a default-constructed (= JSON null) config to widgets, throwing `json::type_error::306` from `HomePanel::finalize_setup`. The throw escaped `run()` before v0.99.55's main_loop catch could absorb it, exiting 134. Reported by d0u8l3m, who originally requested the follow-overlay feature.
- **Theme DPI defaults to LV_DPI_DEF on bogus width_mm reports** — on BTT CB1 (sun4i-drmdrmfb) the kernel reports the 800×480 panel as 890mm × 500mm (off by ~6×), making LVGL compute DPI=23. With sub-160 DPI, `LV_DPX_CALC` clamps `PAD_SMALL` to 1px, collapsing dropdown/input padding. Now forces `lv_display_set_dpi(disp, args.dpi > 0 ? args.dpi : LV_DPI_DEF)` before theme init so widget padding survives garbage display metadata. The existing `if(width > 0)` guard in `lv_linux_fbdev.c` only catches drivers that admit ignorance, not drivers that lie.
- **Continuous-tape label fonts shrink to fit longer vendor strings** — vendor names like PRUSAMENT / POLYMAKER were truncating on 62mm tape. Drops phantom `avail_h` from 300 to 260 (STANDARD scale becomes lg=5 / md=4 / sm=3, was 6/5/4), tightens the horizontal-fit floor from 4 to 8 chars, and applies the narrow-tape margin/QR shrink to 29mm and 38mm continuous (was die-cut only).

## [0.99.55] - 2026-05-06

v0.99.54 shipped a regression and a returning crash signature; v0.99.55 closes both. The bigger story is the **Klipper error UX overhaul** — gcode errors used to surface as raw debug text like *retrude error, retrude but not trigger buffer empty limit*; now they map through a friendly-message table with a one-tap recovery path that tries the K2's deeper `helix_recover` shell_command before falling back to `firmware_restart`. Pairs with a busybox `/sbin/reboot` fallback that finally lets the shutdown widget actually work on K2/AD5M/CC1/K1C/SonicPad. Plus a **pre-print phase tracker** that turns the K2's generic 5-minute *starting print* spinner into BED_MESH / HEATING / FILAMENT_LOAD / PURGE phase progress, and a long-overdue **installer fix** that ships the KIAUH extension in the tarball (silently broken since v0.99.34).

### Added

- **Friendly Klipper error toasts with one-tap recovery** — Klipper emits errors as JSON like `{"code":"key851","msg":"retrude error..."}` where `msg` is raw debug log text. The pipeline now extracts the `code`, looks up a friendly message+hint via `CfsErrorDecoder::lookup_message()`, and routes recovery through `PrinterRecoveryService` — which tries the K2's `helix_recover` `[shell_command]` (bouncing the klipper_mcu daemon) before falling back to `printer.firmware_restart`. C++ stays platform-blind: each platform installs its own snippet via `scripts/lib/installer/recovery.sh`. key851 now displays as *Retract didn't reach buffer empty limit* instead of the raw debug string.
- **CFS error toasts include unit/slot locator** — JSON `values` arrays (e.g. `[1,"B"]` for a key849 retract failure) are stringified into a human locator and appended, so users see *which* CFS slot failed.
- **Pre-print phase tracker (K2/CFS)** — `PrintPhaseTracker` exposes coarse pre-print phases (BED_MESH, HEATING, FILAMENT_LOAD, PURGE) as LVGL subjects, replacing the generic *starting print* spinner that left K2 users staring through ~5 min of bed-mesh + heating. Inputs: `print_stats.state` plus the `notify_gcode_response` tag stream (`// [PRTOUCH_MOVE]`, `// [PROBE_STEP_INFO]`, `// [G29_TIME]`, `// [box]`). Per-firmware-family matchers — adding Bambu/RatOS/K1 variants is a sibling matcher, no parser surgery. Mirrored into the legacy `print_start_phase` / `print_start_message` / `print_start_progress` subjects so the existing `preparing_overlay` UI shows the progression with no panel rewire.
- **Shutdown/reboot widget works on busybox/procd hosts** — `SystemPower::reboot_local()` and `shutdown_local()` now fall through from logind/systemctl to `/sbin/reboot` and `/sbin/poweroff`. K2/AD5M/CC1/K1C/SonicPad shutdown buttons now actually do something (procd's sysinit hook handles graceful service teardown on those calls).

### Fixed

- **Crash loop on v0.99.54 startup** (#931) — `Application::main_loop()` now wraps each iteration in `try/catch`. When a callback throws, the type and `what()` are logged, the recent breadcrumb ring is dumped to stderr (so the next user log captures the throw site), telemetry records the event, and the loop continues with a user-facing error toast. A runaway counter (5 catches in 30s) breaks out cleanly to avoid a tight throw-catch loop. Removes the watchdog's *HelixScreen Keeps Crashing* path for any callback throw that previously exited 134.
- **MoonrakerRequestTracker error-callback path** — symmetrized exception handling: `route_response()` already absorbed throws from success callbacks, but error callbacks were unwrapped. A throwing `error_cb` now logs and continues (matching the success path).
- **Render-thread UAF in argb8888_image_blend** (#929, cluster:pstat-async-delete Mechanism B) — `GCodeLayerRenderer::destroy_cache()` / `destroy_ghost_cache()` / `destroy_ssao_cache()` and `GCodeGLESRenderer::~GCodeGLESRenderer()` (plus the resize-time recreate path) all now call `lv_draw_wait_for_finish()` before `lv_draw_buf_destroy()`. The parallel render thread reads `dsc.src` after `lv_draw_image()` returns; freeing the buffer while a draw task was still in flight unmapped the source page and segfaulted in `argb8888_image_blend`. The wait drains pending tasks before the buffer goes away. Single-threaded builds (`LV_USE_OS == 0`) are unaffected — `lv_draw_wait_for_finish` is a no-op there.
- **CFS load no longer transitions to IDLE prematurely** — `AmsBackendCfs` was flipping `action=IDLE` the moment the toolhead extruder `filament_switch_sensor` triggered, but the K2 load script has 5 steps (CR_BOX_PRE_OPT → EXTRUDE → WASTE → FLUSH → END_OPT) and the sensor trips at the END of step 2. The remaining WASTE + FLUSH (~109 mm at 240 °C, ~3 min) ran while the UI told the user the load was idle. IDLE now defers to script completion, and the toolhead is parked during load.
- **No more execute_gcode spam while Klipper is halted** — with `klippy_state=ERROR` after a key298 rpi-MCU shutdown, wiggling the fan slider previously fired ~30 M106 commands per tick that Klipper rejected on each attempt. Now refuses early when state is SHUTDOWN or ERROR; recovery RPCs (`firmware_restart`, `emergency_stop`, `shell_command`) are unaffected.
- **Phase tracker forward-only ordering** — K2 `[PROBE_STEP_INFO]` (mesh probing) and `[WHY_DEBUG]target_temp` (heating) interleave during early mesh because bed heat ramps in parallel; the tracker no longer flips back from HEATING → BED_MESH on the late probe lines.
- **Phase tracker maps PRINTING → IDLE for legacy overlay** — the legacy `preparing_overlay` panel observer treats any non-IDLE `PrintStartPhase` as *still preparing*; mapping PRINTING → COMPLETE was leaving the overlay stuck up for the rest of the print.
- **Print-start no longer enters BED_MESH from QGL/Z_TILT probe lines** — Voron 2.4 PRINT_START runs QGL before BED_MESH_CALIBRATE. QGL probes 4 corner pads with `samples=3` = 12 *probe at X,Y* lines, which tripped the collector's `MESH_PROBE_ENTRY_THRESHOLD=3` and mis-promoted the phase to BED_MESH (off-by-12 probe count, QGL phase invisible). Collector now ignores probe lines until BED_MESH is announced.
- **Installer KIAUH extension actually ships in the tarball** — silently broken since v0.99.34: release tarballs (built by `mk/cross.mk`) never copied `scripts/kiauh/` into the package, so `install_kiauh_extension()` always hit *source files not found* and bailed. Now copied into all 10 platform release targets. Also closes a PII leak — release tarballs were shipping any `telemetry_*.json`, `tool_spools.json`, `crash_report.txt` present in the dev tree (only `settings.json`/`helixconfig*.json` were stripped). And collapsed the longstanding drift between `install-dev.sh` and the bundler's MAIN heredoc into `scripts/lib/installer/main.sh` (sourced by both, install-dev.sh shrinks 295→48 lines).
- **Documentation gallery images render on github.com** (#928) — `GALLERY.md` lives at `docs/devel/` but images live at `docs/images/`; the bare `images/` refs resolved to a non-existent `docs/devel/images/`. Reported by @TMTYD.

### Changed

- **PrintPhaseTracker unified into PrintStartCollector** — internal refactor consolidating the two pre-print tracking surfaces into a single owner.
- **Keyboard per-release event log demoted to trace** — was cluttering `-vv` debug output during normal typing.

## [0.99.54] - 2026-05-04

The follow-up to last week's hotfix. Headline pieces: the **L081 unwind-safe `lv_event_pop`** fix that landed eleven hours after v0.99.53 tagged (so it missed that release), plus a second layer of defensive guards in `lv_event_mark_deleted` for the **cluster:pstat-async-delete** crash family (#906) that's been the dominant production signature since 4/29. The **pre-print options framework** lands across four phases — a uniform contract for every per-printer pre-print Klipper option (K2 Plus AI detect, K1C/SonicPad bed leveling, etc.) replacing the old ad-hoc per-option JSON. Round it out with **in-place fan-carousel dial drag** (no more popping a separate fan-control overlay), **AD5X-IFS hardening** (stale plugin data, custom materials, lane-data sync on external CHANGE_ZCOLOR), the long-pending **L083 std::thread audit** wrapping every remaining bare detached spawn, and a **memory-leak fix** for AMS detail panel.

### Added

- **In-place fan-speed control** — drag the arc on a fan card in the Controls fan carousel to change the speed directly. No more bouncing into a separate fan-control overlay for the common case.
- **"Follow my graph selection" toggle on the temperature-graph card** — when on, dragging through the time axis pins the readout to the touched sample instead of always tracking live.
- **Pre-print options framework (Phases 1-4)** — every per-printer pre-print option (K2 Plus AI detect, bed leveling, calibration prompts, etc.) now flows through a uniform `PrePrintOption` contract. Phase 1 introduced the framework, Phase 2 migrated the JSON DB and all call sites, Phase 3 rebuilt the UI to generate rows dynamically with category subheaders, Phase 4 added K2 Plus `ai_detect` via the `PreStartGcode` strategy.
- **moonraker-timelapse component detection** (#926) — the Settings row appears for users with a pre-existing moonraker-timelapse install (MainsailOS, manual config), not just those who ran the in-app install wizard.
- **Favorite-macro home-screen tap honors safety settings** (#925) — dangerous-macro confirmation prompts now fire from the home-screen tap path the same way they do from the macros panel.
- **Debug bundle now captures platform config files + CFS Klipper objects** — bundles for K2 / Pi printers carry the full Klipper objects payload for CFS units.

### Fixed

- **LVGL event-dispatch unwind safety (L081 root cause)** — when a C++ exception unwinds through `lv_obj_send_event` or `lv_event_push_and_send`, the explicit `lv_event_pop` was being skipped, leaving `event_head` pointing at a defunct stack frame. The next `lv_event_mark_deleted` walk SIGBUSed reading garbage. Patched both entry points to use GCC `__attribute__((cleanup))` (with `-fexceptions` enabled on LVGL TUs) so pop fires on exception unwinds too. Closes the design gap behind crash families #793/#840/#871/#878/#880 and a substantial fraction of the cluster:pstat-async-delete (#906) hits.
- **Cluster:pstat-async-delete defensive guards (#906)** — `lv_event_mark_deleted` now layers two additional checks: a dispatch-depth coherence guard (logs `event_head_stale_leak` anomaly if `event_head` is non-NULL with no active send frame) and a stack-bounds check (logs `event_head_stack_oob` if the chain pointer is outside the current thread's stack range). Both reset `event_head` and bail safely. Catches corruption modes the cleanup-attribute path can't address (heap reuse, memory stomp). Plus per-step breadcrumbs in `PrintStatusPanel::on_activate` so the next field crash names which step set up the corruption. Breadcrumb ring expanded 128→256 to keep async_d/sync_d entries from rotating off.
- **L083 thread spawn audit completes** — every remaining bare `std::thread(...).detach()` site is now wrapped in try/catch surfacing a toast on `std::system_error` from `pthread_create` EAGAIN. Prevents `std::terminate` aborts under thread exhaustion on resource-constrained ARM (AD5M, K1C, MIPS targets).
- **AD5X-IFS stale plugin data + custom materials** (#904) — Adventurer5M.json reads now guard against stale lessWaste/bambufy `save_variables` rows that persist after plugin uninstall, and user-defined materials in `/mod_data/user.cfg` now surface in the slot picker alongside built-ins.
- **AD5X-IFS sync on external CHANGE_ZCOLOR** — when the user runs `CHANGE_ZCOLOR` from Mainsail/Fluidd or the AD5X touchscreen, HelixScreen now syncs `lane_data` and `_IFS_VARS` to match instead of drifting out of agreement.
- **AD5X auto-detect on ZMOD firmware** — was misdetected as AD5M Pro on certain ZMOD revisions.
- **AMS detail panel destroyed on close** — was holding ~MB of widget state for the lifetime of the app.
- **AMS bypass spool position** — pinned to the canvas tube line on size-changed events instead of drifting on resize.
- **AD5X-IFS post-print runout toast suppression** — was firing every print-end since the IFS auto-unloads after the job; now only triggers on real runout.
- **Thermistor widget rebinds temp observer when sensor name changes** (#916) — the bound subject is dynamic; the widget now uses a paired `SubjectLifetime` member so observer cleanup matches the dynamic-subject pattern.
- **Fan-carousel arc value-changed no longer records spurious interactions** — the auto-debounce was treating programmatic arc updates as user input.
- **FilamentConsumptionTracker stopped before `destroy_all` on shutdown** (#927) — closes a use-after-free observed during AD5X tear-down.
- **Print Select self-heals stuck refresh_in_flight_ flag after 30s** — a never-arriving file-list response could lock out subsequent refreshes; a watchdog now clears the flag.
- **Splash process orphaned on DRM platforms (Snapmaker U1)** — `helixscreen.init` starts `helix-splash` early before the display backend is selected, but `helix-watchdog`'s "skip external splash on DRM" branch (added because fbdev splash mmap conflicts with DRM) only avoided spawning a *new* splash; the externally-adopted PID was silently dropped, never forwarded to helix-screen via `--splash-pid`, and never reaped. Splash kept running after the UI took over the display, burning ~60% CPU on a single core forever (rockchipdrmfb's fbdev compat layer kept it from failing). Watchdog now reaps the orphan when DRM is selected. Belt-and-suspenders: `cleanup_splash` escalates to SIGKILL after a 600 ms SIGTERM grace, and the splash binary self-exits after 30 s if no signal arrives.
- **Moonraker component-capability flags cleared on each discovery** — stale capability bits from a prior connection no longer leak into a new printer's profile.
- **Favorite-macro context leak on backdrop/ESC dismiss + i18n confirm body** (#925) — the ButtonCallbackData was leaking when the picker was dismissed via backdrop tap or ESC.
- **Macros-panel dangerous-macro confirm body** — i18n keys hooked up and the body text formatted for readability.

### Changed

- **Color swatches and filament-mapping cards driven by subjects** — converted from imperative C++ visibility toggles to declarative XML `bind_flag_if_eq` against a subject. Removes the last `can_show_*` zombie subjects from `printer_state.cpp`.
- **Debug bundle HTTP fetch + sanitization factored out**; `user.cfg` redaction fixed (was leaking certain custom-material entries verbatim).
- **AD5X re-reads `Adventurer5M.json` on AMS detail-view entry** — picks up out-of-band edits from the AD5X firmware native UI without requiring a HelixScreen restart.

## [0.99.53] - 2026-04-28

A hotfix release. The headline fix: **v0.99.51 and v0.99.52 could brick AD5X stock-ZMOD printers** by corrupting `Adventurer5M.json` to zero bytes through a Moonraker upload that crossed mount boundaries. On the next firmware restart, Klipper's zmod_color.py couldn't parse the empty file and never finished `connect` — recovery required SSH. Both releases have been pulled from the GitHub release feed; v0.99.53 ships the corrected write path. While we were here, this release also lands the per-printer gcode console filter system, OrcaSlicer-compatible bed/nozzle temperature emission to `lane_data` slot records, and a small theme polish.

### Added
- **Per-printer gcode console filter** — every printer can have its own customizable filter list for the gcode console panel. Settings overlay lets you add/remove patterns and toggle the filter on/off without rebooting; defaults are seeded from the printer database so common boilerplate (heartbeat polls, M105 noise) is suppressed out of the box.
- **OrcaSlicer-compatible bed/nozzle temperatures in `lane_data`** — HelixScreen now emits `bed_temp` and `nozzle_temp` on every filament-slot save so OrcaSlicer 2.3.2+ can sync them to its filament presets. Source priority: explicit user entry > bound Spoolman spool's profile > internal material database default. Spec docs bumped to v1.1.

### Fixed
- **AD5X stock-ZMOD bricked at boot from corrupted Adventurer5M.json** — v0.99.51 switched IFS color/material persistence from `CHANGE_ZCOLOR` (which pops a Mainsail/Fluidd "Select print materials" prompt on every edit) to a Moonraker HTTP upload of `Adventurer5M.json`. On AD5X stock-ZMOD, `/root/printer_data/tmp/` and `/usr/prog/config/` are on different mounts and Moonraker's config root entry symlinks across them; the upload's `os.rename(tmp → dest)` throws `EXDEV` and the destination ends up empty. On the next firmware restart, zmod_color.py's `json.load(file)` raised `JSONDecodeError`, klippy never finished `connect`, and the printer was unrecoverable without SSH. Bundle DQK7X96B and DIEHARDave's report on Discord. Fix: when helix-screen runs on the same host as Moonraker (the typical install), skip the upload and write `Adventurer5M.json` directly via filesystem APIs with an atomic temp-then-rename inside the destination directory. The Moonraker upload path remains as a fallback for remote/screen-only installs that genuinely aren't on the same machine.
- **ActionPromptModal footer divider used hardcoded color** — switched to the border design token so it tracks theme.

## [0.99.52] - 2026-04-28

A bug fix release dominated by killing a class of crash loops triggered by Moonraker subscription-restricted JSON nulls. Klipper objects don't always implement every field a subscription names; Moonraker faithfully delivers `null` for the missing ones, and several of our parsers were calling `.get<T>()` straight through, throwing `type_error.302` and unwinding into `main()`'s top-level catch (exit 134). Every restart hit the same bug, and the watchdog couldn't break out. Now: every parser type-guards before extracting, the dispatch path absorbs callback throws so one rogue parser can't take down the others, a CI lint gate fails any new violation, and the watchdog detects same-signature crash loops (≥3 in 90s) and offers a Safe Mode boot that defers the Moonraker connection so the user can reach Settings.

### Added
- **Watchdog Safe Mode boot** — when the same crash signature repeats 3+ times in 90s (typical of a deterministic state-driven crash), the recovery dialog switches to a loop-aware variant with the auto-restart countdown disabled and "Safe Mode" as the primary button. Picking it writes a one-shot marker, restarts the app, and Application defers the Moonraker connection on the next launch with a sticky banner. A clean reboot exits Safe Mode automatically.

### Fixed
- **Subscription-restricted-null crash loops** — Moonraker delivers `null` for subscribed fields the underlying Klipper object lacks (Snapmaker U1's `filament_motion_sensor.detection_count` is the prototypical case). Multiple parsers used unguarded `.get<T>()` that threw `json::type_error::302` on null, unwinding past the dispatch loop and exiting 134. Hardened `printer_state.update_from_notification`, `printer_print_state.update_from_status`, sensor parsers, `led_controller` color channels, and `filament_sensor`. The initial-subscription apply path (`MoonrakerClient::dispatch_status_update`) now also wraps each callback in try/catch so one throwing parser can't take down the rest. A CI lint gate (`scripts/check_subscription_null_safety.py`) prevents regressions.
- **AD5X panel widget rebuilds could abort under memory pressure** — the panel widget manager allocated a small `AsyncCtx` struct per gate-observer firing and queued it via `lv_async_call`. On memory-tight AD5X (~107MB RAM) a `std::bad_alloc` here propagated through LVGL's C event-dispatch frame and aborted the process via `std::terminate`, surfacing as unrelated-looking crashes (L083 family). Now uses a stable per-panel slot stored in the manager singleton — no per-firing allocation in the hot path.
- **AD5X IFS slot overrides wiped on boot** — on startup, `parse_save_variables` ran `update_slot_from_state` for every slot before `Adventurer5M.json` had been fetched. With no firmware color reading yet, the swap-detection helper saw the `SlotInfo` default (0x808080 gray) as the baseline; seconds later when the real color landed it was misread as a "physical spool swap" and the user's override was cleared (brand, spoolman_id, weights, material). On the next boot 0 overrides loaded because boot 1 had wiped them. raza616 caught the symptom: PETG slot flipping back to firmware-truth PLA on every power cycle. Fix: treat empty `colors_[idx]` as a no-signal reading so the baseline is only established from real firmware data. Bundle AQ6DALWG.
- **AD5X IFS: stale lessWaste/bambufy save_variables ignored after plugin uninstall** — `parse_save_variables` was reading `<prefix>_tools` / `_current_tool` / `_external` unconditionally whenever the keys were present. Moonraker's `save_variables` rows persist in `printer_data/database/` long after a plugin uninstall removes the gcode macro, so a user who removed lessWaste/bambufy would silently keep using the dead plugin's last tool map and active-tool guess on every boot. Now those reads are gated on `has_ifs_vars_` (= macro confirmed loaded), matching the contract the latch already advertised.
- **G-code viewer renderer stalls now recover instead of going blank** — added a renderer-stall watchdog with cache-state logging; on detected stall the viewer reinitialises rather than freezing on a blank or partial render.
- **Niimbot B1 cancelled print before the printer started moving** — the BLE PrintEnd packet was being treated as job-finished too early; now we wait for actual motion before allowing the cancel path to close the job.
- **AD5X stock ZMOD: GET_ZCOLOR spam quieted** — the ZMOD slot poller was firing the macro every tick instead of only on JSON content change. Now polls the JSON contents and only emits the macro when something changed.
- **AD5X installer refuses to run outside ZMOD chroot** — preflight now bails with a clear error rather than silently corrupting a non-ZMOD AD5X install.
- **Snapmaker U1 install: self-heal stale S99screen patches** — old patched init scripts from prior installs are detected and replaced rather than skipped.
- **ALSA buffer-underrun spam on Pi** — continuous underrun warnings during sound playback were burning log volume; the underrun path now backs off cleanly.
- **Label printer: faster BlueZ unpair + truthful Forget toast** — the Forget action would hang on BlueZ's slow unpair, then toast success even when the unpair had failed. Now uses a faster path and surfaces real status.

### Changed
- **Debug bundle exposes canonical platform_model** — bundles now include `printer.platform_model` (derived from the build-time platform key) alongside the user-picked `printer.model`, plus a `platform_model_mismatch` flag when they disagree. Lets the triage dashboard surface AD5X-running-as-AD5M-Pro mismatches without trusting the wizard pick blindly.
- **Shutdown observer-release contract test generalised** — the structural check that caught #888 is now table-driven; new singletons destroyed after `StaticSubjectRegistry::deinit_all()` can be added with one row.

## [0.99.51] - 2026-04-28

A targeted fix release. K2 install path works end-to-end now: a procd shim makes the autostart actually fire (devices were sitting at the boot logo on fresh install), uninstall re-enables Creality's stock UI, and a 68s boot trim gets K2 Plus into HelixScreen in ~36s. AD5X stock-ZMOD users get IFS active-slot tracking via GET_ZCOLOR, and slot edits no longer pop Mainsail/Fluidd "Select print materials" prompts or native touchscreen confirmation dialogs on every sync.

### Fixed
- **K2 install autostart never fired** — K2 (Tina Linux / OpenWrt procd) silently skips plain SysV init scripts at boot; procd's iterator only invokes scripts with both the rc.common shebang and a DEPEND directive. Our shared S99helixscreen had neither. Install now writes a procd-compatible shim at `/etc/init.d/helixscreen` that delegates to S99helixscreen, so K2 (Pro/Plus/Max) actually starts HelixScreen at boot instead of sitting at the boot logo. Reported by jacekruf (K2 Pro, F012 board).
- **K2 uninstall left device with no UI** — `hooks-k2.sh` runs `/etc/init.d/app disable` on every helix-screen launch (suppressing the stock procd-managed UI), but the modular uninstaller had no K2 case to re-enable it. Devices booted into the Creality logo with no UI after uninstall. Now enable+start runs against `/etc/init.d/app` on K2 series.
- **AD5X stock ZMOD: active loaded slot not tracked** — `parse_zcolor_silent()` correctly extracted `extruder_slot` from GET_ZCOLOR but `apply_zcolor_result()` dropped the field. For users without lessWaste, `active_tool_` stayed at -1 forever and the panel showed stale slot info on unload. Now derived from `extruder_slot` when no IFS vars are present. Reported by raza616.
- **AD5X stock ZMOD: every slot edit popped two dialogs** — `CHANGE_ZCOLOR` always emits a Mainsail/Fluidd "Select print materials" prompt and (on `display=True` setups) an HTTP-driven confirmation dialog on the native AD5X touchscreen. Both fired every time HelixScreen synced a slot — QR scans, Spoolman pulls, tool-mapping changes. Now writes `Adventurer5M.json` directly (zmod's source of truth) and only fires `_IFS_VARS` for lessWaste users. Confirmed by DIEHARDave.

### Changed
- **K2 boot time trimmed by 68s** — empirical traces on the dev K2 Plus showed `platform_wait_for_services` was treating its 30 timeout as iteration count (not seconds) and `platform_pre_start`'s WiFi association ran synchronously. Now: timeout tracks wall time via `date +%s` and the wpa_supplicant + udhcpc dance is backgrounded. K2 Plus fresh boot: 104s → 36s; most of the remaining 30s is honoring the service-wait budget while Moonraker comes up.
- **i18n** — upgrade banner and shutdown-modal Reboot/Shutdown buttons newly tagged for translation; 15 strings translated across de, es, fr, it, ja, pt, ru, zh. All locales now at 100% coverage (1940/1940).
- **Settings: printer host description binding** — moved from imperative C++ (`lv_obj_find_by_name` + `lv_label_bind_text`) to declarative `bind_description` attribute, aligning with the project's "DATA in C++, APPEARANCE in XML" rule. Internal cleanup; no behavior change.

## [0.99.50] - 2026-04-27

A perf and stability patch. Headlines: animated screensaver now defaults to Off on BASIC (Pi 3B-class) and EMBEDDED (AD5M / AD5X) tiers to keep the CPU out of Klipper's print loop, with a one-time migration notice and reduced frame rate when users re-enable on those tiers; another sweep of L081-family teardown crashes (`lv_event_mark_deleted`); plus targeted fixes for AD5X IFS color sync, AFC `LANE_UNLOAD` serialization, and the Z-offset probe sequence. Re-tagged after the initial CI release build broke on cross-compile targets and a SonicPad user reported the shutdown widget was unreachable when Moonraker failed to connect.

### Fixed
- **Release build broken on cross-compile targets** — the v15→v16 screensaver migration referenced `PlatformCapabilities::detect()` and `platform_tier_to_string()` unconditionally in `config.cpp`. Those symbols ship in helix-screen but not in `helix-splash` / `helix-watchdog`'s extra-obj lists, so every cross-compile target (pi32, ad5m, etc.) failed at link time on the auxiliary binaries. Both call sites are now gated behind the existing `HELIX_SPLASH_ONLY` / `HELIX_WATCHDOG` guard — splash/watchdog don't render the screensaver and treat the migration as a no-op.
- **Shutdown widget unusable when Moonraker can't be reached** — feedback from a SonicPad user: when Klipper failed to boot, the WebSocket never connected and the shutdown button stayed disabled, leaving the hardware switch as the only way to power off. The button is now always clickable; in same-host single-scope mode (helix-screen and Moonraker on the same device) the buttons fall back to `SystemPower::shutdown_local()` / `reboot_local()` (logind → systemctl) when the RPC isn't available. Dual-scope users on a remote screen already had a Screen-only path; ungating the button makes it reachable when the printer is unresponsive.
- **K1C boot logo persisted on SimpleAF firmware** ([#890]) — `boot_display` was only killed when `/etc/init.d/S99start_app` existed, but SimpleAF removes that script while keeping `S12boot_display` in place. The Creality boot logo now stops on all K1 firmware variants when HelixScreen takes over.
- **AD5X identify-wizard sleep regression** — removed the wizard block that force-wrote stale pre-#431 display values on every confirmation. New v14→v15 config migration restores the AD5X backlight-off sleep preset for users polluted by the pre-fix wizard. Resolves the "random RGB colors during sleep" symptom documented in `FLASHFORGE_AD5X_SUPPORT.md`.
- **AD5X IFS color and material type sync** — slot color/type changes from the panel now use Flashforge's `CHANGE_ZCOLOR` macro so the IFS controller actually receives the update.
- **AFC `LANE_UNLOAD` requests are now serialized** — tapping Eject on multiple lanes in quick succession queues the requests and runs them one-at-a-time instead of overlapping AFC macros. Mitigates Turtle_1 "Timer too close" shutdowns from competing stepper / LED-animation work.
- **Shutdown crash with active observer chains** ([#893]) — `Application::shutdown()` now invalidates `ObserverGuard`s *before* tearing down `MoonrakerManager`. The prior teardown order let pending subject notifications fire callbacks against destroyed Moonraker state, producing L081-family crashes during exit.
- **`MoonrakerManager::m_print_duration_observer` not released on shutdown** ([#888]) — observer lingered through the static destructor chain; explicit release on shutdown closes the dangling-observer window.
- **Temp graph chart deletion now async** ([#867]) — chart child cleanup during temp-graph teardown moved off the synchronous LVGL event-list iteration path, with breadcrumb logging for any remaining anomalies.
- **L081 family: panel gate observer rebuilds coalesced via `lv_async_call`** — multiple gate-state subject changes within a single tick now produce one rebuild instead of N synchronous rebuilds, eliminating the multi-sync-delete-per-tick pattern that corrupts LVGL's global event linked list.
- **Z-offset wizard no longer auto-prefixes `CLEAN_NOZZLE` to `PROBE_CALIBRATE`** — the auto-prefix fired on printers without a wipe macro, hanging the calibration sequence. Users with a wipe macro can wire it into their start gcode.

### Changed
- **Animated screensaver disabled by default on BASIC and EMBEDDED tiers** — Raspberry Pi 3B-class (BASIC) and AD5M / AD5X (EMBEDDED) hardware now ship with the screensaver off to prevent print failures from CPU contention with Klipper. Users upgrading with Flying Toasters still enabled on these tiers are migrated to Off with a one-time notification; the setting can be re-enabled under Settings → Display. When re-enabled on these tiers, the screensaver runs at a reduced frame rate (~7 fps) with fewer sprites (Flying Toasters capped at 10) to stay out of the print loop's way.
- **Spool canvas scroll smoothness** — dedup pass, LRU pixel cache, and a sqrt LUT in the canvas pixel renderer. Noticeable on filament panel scroll on EMBEDDED-tier hardware.
- **Crash bundle `queue_prev` ring coalesces consecutive identical tags** — high-frequency callbacks (e.g. `TSM::update_subjects` per WebSocket tick) used to fill all four slots and bury genuinely distinct prior callbacks. The producer now bumps a parallel count on tag-pointer match instead of advancing the ring; emitted as `queue_prev: tag (xN)` when N > 1. Restores diagnostic runway for tracing post-callback heap corruption (#840 / #851 family).

## [0.99.49] - 2026-04-26

A targeted hotfix release for AD5X (ZMOD firmware) and other platforms with aggressive supervisor lifecycles. Real-time triage of bundle EE8X6GSK plus a live SSH session on the affected printer revealed a crash-loop chain combining three independent bugs: ZMOD's display-handoff Klipper macro periodically `killall`s helix-screen, our SIGTERM handler ran the full `Application::shutdown()` teardown, the teardown crashed with SIGBUS in late-stage cleanup (the crash handler was uninstalled in the first line of `shutdown()`, so no diagnostic landed on disk), and the watchdog burned through restart credits leaving a blank screen. All three sides are now hardened.

### Fixed
- **HelixScreen no longer crash-loops on platforms whose firmware aggressively respawns the UI process (AD5X / ZMOD)** — SIGTERM now triggers immediate `_exit(0)` without running `Application::shutdown()`. External supervisors only care about a clean exit code, not whether teardown ran. Skipping the fragile teardown path (LVGL deinit, observer cleanup, static destructors) avoids the L081-family SIGBUS that ZMOD's killall-and-respawn cycle was hitting on AD5X every ~20 s after Klipper restart. SIGINT (Ctrl+C from terminal) keeps the graceful shutdown path. Persisted state (settings, telemetry queue, crash history) is written on each change so nothing is lost.
- **Crash handler stays installed through the entire shutdown sequence** — `crash_handler::uninstall()` was the *first* line of `Application::shutdown()`; now the *last*. Any SIGBUS/SIGSEGV during widget deletion, observer cleanup, or `lv_deinit` is now captured to `crash.txt` instead of falling through to the kernel default with no diagnostic. Critical for surfacing the L081-family teardown crashes seen post-v0.99.46.
- **Crash diagnostics survive misconfigured config dirs (chroot, overlay, PrivateTmp)** — when the signal handler can't `open()` the primary crash path, it now falls back to writing the dump to stderr (with `=== HELIX_CRASH_DUMP ===` marker prefix). Stderr is always open and gets captured by both journald (systemd) and the watchdog's stderr pipe, so users on platforms where `$HELIX_CONFIG_DIR` resolves to a non-writable layer no longer get silent diagnostic blackouts.
- **Debug bundles now include crash data on platforms that set `$HELIX_CONFIG_DIR`** (ZMOD, RatOS, etc.) — collector previously used an ad-hoc `$HOME/helixscreen/config` probe that missed overlay installs entirely. Now uses the canonical `helix::get_user_config_dir()` resolver, matching where the crash handler writes. New `crash_txt` bundle key includes the raw signal-handler dump even when the next-boot reporter hasn't run yet.
- **Webcam discovery skipped on platforms without a camera widget** — Moonraker `server.webcams.list` and the local-endpoint probes (127.0.0.1:8080/8081/4408) now respect the `HELIX_HAS_CAMERA` compile-time gate. Skipped on AD5M/AD5X/CC1/K1/K2/MIPS/SnapmakerU1. On AD5X specifically, the firmware's kernel H.264 codec driver crashes on every `v4l2_open` (`dma_coherent_mem_available` NULL+12 deref) — we never opened `/dev/video*` directly so this isn't the trigger for the recent crash family, but probing for a webcam we can't render is wasted RPC.
- **Installer disk-check uses POSIX `head -n 1`** so BusyBox doesn't reject `head -1` (AD5M/AD5X/K1/K2).
- **Updater download-directory candidate list now includes the install root** — previously skipped install-root candidates, breaking updates on platforms where the install root is the only writable location.

## [0.99.48] - 2026-04-26

A performance and correctness patch. Headlines: drastically narrowed Moonraker subscriptions (the prior nullptr-everything pattern was firing per-frame led_effect notifications on AFC hardware — a Voron Trident bundle showed 46 LED-effect objects updating per render frame); a tangle of six interlocking fixes to the wizard preset path so AD5M Pro on ForgeX firmware actually skips hardware steps on fresh install; and a UAF / OOB-read fix for a shared-component XML callback collision caught in bundle SSHGTVZQ.

### Added
- **Jog soft-stops driven by kinematic envelope** — `MotionPanel::jog()` now refuses moves that would push the toolhead past `toolhead.axis_minimum`/`axis_maximum` and toasts the user. Skipped when bounds aren't published yet or the axis isn't homed.

### Fixed
- **Wizard preset application: AD5M Pro ForgeX skipping hardware steps on fresh install** — six interlocking issues (preset path resolution, late `preset_mode` capture, Moonraker connect gated behind wizard, validate-before-detect ordering, wrong fan names in `ad5m_*_forgex.json`, `_zmod` probe firing on `_forgex` preset, missing aux fan slot recognition).
- **Layer-source precedence within a status update** — slicer `SET_PRINT_STATS_INFO` now wins over `virtual_sdcard.layer` within the same tick (sdcard branch was running second and silently overwriting). Across updates, last source still wins.
- **`toolhead.max_velocity` subscription regression** — caught in code review of subscription narrowing; `max_velocity_` would have stuck at its initial value, breaking the print-tune overlay and machine-limits panel after every reconnect. Field list is now locked by a 19-case / 178-assertion regression test naming each parser file it mirrors.
- **XML callback name collision could SEGV on click** (bundle SSHGTVZQ, Qidi Q2, pi) — first-write-wins for `lv_xml_register_event_cb` meant two C++ owners registering the same callback name globally for a shared XML component (e.g., `WizardWifiStep` + `NetworkSettingsOverlay` on `wifi_network_item`) silently dropped the second registration. The first owner's handler then fired against the second owner's `user_data`, casting a small struct as a much larger one and reading ~400 bytes past valid memory. Now last-write-wins.

### Changed
- **Moonraker subscription narrowing — heaters, fans, sensors, tools, macros** — eliminated the remaining `nullptr` (= all-fields) subscriptions in the discovery sequence. Narrowed to the actual fields each parser consumes (heaters → `{temperature, target}`, standard fans → `{speed}`, native LEDs → `{color_data}`, etc.). Dropped `idle_timeout` entirely (no parser reads it).
- **Moonraker subscription narrowing — `led_effect`, AFC, core motion** — biggest perf win on AFC/BoxTurtle: a Voron Trident bundle showed 46 `led_effect` objects firing per-frame animation updates because we'd been subscribing with `nullptr`. Now `led_effect` is just `[enabled]`. Also narrowed all `AFC*` objects to their parser fields, and the per-step core motion objects (`toolhead`, `gcode_move`, `motion_report`).
- **Launcher `nice +10` when co-hosted with Klipper/Moonraker** — `helix-launcher.sh` reduces UI process priority on shared-CPU hardware (Pi, SonicPad, AD5M class) so the printer control loop keeps CPU headroom for stepper timing and MCU comms. Detection is process-based (`pgrep -f`) with a Unix-socket fallback. Standalone displays (remote-display SonicPad, dev workstation, kiosk pointed at a network printer) are unaffected. Override with `HELIX_NICE=<n>` in `helixscreen.env`; `HELIX_NICE=0` disables.
- **`make clean-tests` now wipes the PCH and all sanitizer test binaries** — recover from a sanitizer-contaminated tree without `make clean`. Previously, ASan/TSan test runs reused the PCH compiled with `-fsanitize=address`, and a subsequent `make test-run` would fail with "ASan runtime does not come first."

## [0.99.47] - 2026-04-26

A defensive-hardening patch release continuing the L081 (`lv_event_mark_deleted` SEGV/SIGBUS) eradication campaign begun in v0.99.45/v0.99.46. Sweeps the remaining unsafe `lv_obj_clean()` callsites that escape UpdateQueue batches, plugs lifetime gaps in two control panel callbacks, and lands a stress harness that will catch regressions of the multiple-async-delete-per-tick race plugged in v0.99.46.

### Fixed
- **L081 family (`lv_event_mark_deleted` SEGV/SIGBUS) — final sweep** ([#873]–[#878]) — 18 remaining `lv_obj_clean()` callsites that ran inside event-dispatch frames or queued callbacks were converted to `safe_clean_children()` so child deletion happens via `lv_obj_delete_async()` rather than synchronously inside an LVGL event-list iteration. Pairs with v0.99.46's wizard root-cause fix to close the #840-adjacent crash family.
- **Container child cleanup escapes event-dispatch batch** — top-level containers cleaned during input-event handling now defer child deletion to the next async tick, eliminating the multiple-sync-delete-per-tick race that corrupted LVGL's global event linked list.
- **Z-offset save and flow up/down callbacks lacked lifetime guards** — the controls panel could fire on a freed `MotionPanel` if a save/flow tap landed during teardown. All three callbacks now use `tok.defer()` with lifetime tokens, matching the rest of the panel.
- **Updater "no space" check used a hardcoded 50 MB floor** — but the current pi32 zip is 69 MB, so users could pass the precheck and fail mid-stream. The check now uses the asset size from the GitHub/R2 manifest with a 1.2× headroom and a 200 MB fallback for unknown-size assets, plus an actionable error message naming the asset and the shortfall (driven by bundle 7ZGHW5KX where systemd namespace weirdness caused statvfs to misreport free space).
- **Debug bundles missed helixscreen log on ZMOD AD5X** — log lives at `/opt/config/mod_data/log/helixscreen.log` (real path `/usr/data/config/mod_data/log/`); both are now in the log-tail cascade.
- **About panel: licenses container border radius** — was hardcoded; now uses the `#border_radius` design token so it matches the rest of the panel.

### Changed
- **action_prompt_modal show/hide stress harness** — `tests/unit/test_action_prompt_modal_stress.cpp` adds seven Catch2 variants (rapid show/hide, #877 burst pattern, alternating shapes, stacked-under-base, click-driven hide, single-tick drain, queue_update racer) tagged `[stress][action_prompt][.ui_integration]`. Will trip if the multiple-async-delete-per-tick race is reintroduced. Override iteration count via `ACTION_PROMPT_STRESS_ITERATIONS`.

## [0.99.46] - 2026-04-26

A targeted fix release. The headline is the ASAN-confirmed root-cause fix for the chronic wizard step-transition crash family that has driven roughly half a dozen patch attempts since v0.99.34. With AddressSanitizer wired into the Pi cross-build, two distinct heap-use-after-frees were caught within minutes of human interaction with the wizard — replacing weeks of victim-site whack-a-mole.

### Fixed
- **Wizard step-transition heap corruption — root cause** ([#880], [#871], [#870], [#872], [#873], [#874], [#875], [#877]) — `WizardConnectionStep::auto_probe_timer_` is a one-shot LVGL timer; LVGL deletes one-shot timers internally after the callback returns, but `attempt_auto_probe()` only nulled the member pointer on its non-early-return path. When the timer fired and early-returned (no IP yet), the member was left pointing into freed memory. On back-nav from the Connection step, `cleanup()` called `lv_timer_set_cb(nullptr)` against the freed timer, corrupting LVGL's timer linked list and the heap region behind it. Downstream effects manifested as crashes in `lv_event_mark_deleted`, `trans_anim_start_cb`, `lv_draw_sw_blend_image`, and the wider LVGL anim/event/render paths. The member is now nulled at the start of the timer callback, before any path can early-return. ASAN-confirmed.
- **Test Connection click crash** — the libhv WebSocket `onopen` lambda captured `const char* url` directly, but callers pass a stack-local `std::string`'s `.c_str()`. The lambda fires on a libhv worker thread after the caller's scope unwinds, and spdlog's format-arg processing strlen'd the freed buffer. Lambda now captures by string-copy. ASAN-confirmed.
- **Connection step IP/port pre-fill** — fields appeared empty on every visit (instead of showing 127.0.0.1/7125 or the saved config) and lost user-typed values on back/forward navigation. `init_subjects()` now seeds buffers only on first init so user input survives re-visits, and `create()` drives pre-fill from the buffer (the source of truth) rather than the subject (which can be transiently empty during XML widget construction).
- **AD5M Wi-Fi step on first launch** — Forge-X firmware doesn't auto-load the Realtek RTL8821CU USB driver, and wpa_supplicant only starts after the user has supplied credentials via the stock UI. Fresh installs running HelixScreen as the launcher therefore never enumerated `wlan0` and reported "no WiFi hardware found." Platform hooks now insmod `8821cu.ko` and start `wpa_supplicant` in `platform_pre_start()`, both idempotent. No-op on AD5M boards without a USB Wi-Fi dongle.
- **Wizard subtree purge regression** — the v0.99.45 fix to cancel style transitions during cleanup recursed into the root container and stripped its flex layout, collapsing new step content into a thin column. Root container now only gets `lv_anim_delete`; descendants still get the full style strip.
- **AMS `set_slot_info` clobbered `mapped_tool`** when the caller didn't pass it; persists across calls now. Horizontal card scrolling on the AMS panel also restored.

### Changed
- **AddressSanitizer build for Pi** — new `make pi-asan-docker` target produces a fully-instrumented binary (LVGL, helix-xml, libhv, helix-screen) with `-static-libasan` for self-contained deployment. Output to `build/pi-asan/` so it doesn't clobber regular builds. Strip is forced off; debug info is split into a separate `.debug` file kept on the build host for symbol resolution.
- **Wizard step-transition stress harness** — Catch2 fixture (`tests/unit/test_wizard_step_stress.cpp`) drives `ui_wizard_navigate_to_step` programmatically in 2↔3 bounces and full-sweep patterns, configurable via `WIZARD_STRESS_ITERATIONS`. Tagged `[wizard][stress][.ui_integration]` — hidden from default test runs.

## [0.99.45] - 2026-04-25

A defensive-hardening release. The dominant theme is converting hard-to-debug LVGL crashes into logged anomalies that telemetry can act on, plus a scope-aware shutdown/reboot UX for users running multiple Klipper hosts.

### Added
- **Scope-aware shutdown/reboot modal** with split-button dual-host UX. When you have more than one host (e.g., a satellite Klipper running over MCU-share), the shutdown widget now lets you target Just This Printer or Both Hosts. Selecting Both chains the second action on the first host's ack; the host_identity cache is invalidated on each action so subsequent shutdowns don't bounce off stale state.
- **Label printer error toasts** — raw BlueZ / vendor error strings are now mapped to actionable toast messages ("Niimbot is busy printing — wait for the current job" instead of `org.bluez.Error.Failed: Operation already in progress`).
- **About panel: install root, config dir, cache dir rows** — pull from the new `app_globals` helpers, with a folder-home MDI glyph for the Install Root row.

### Fixed
- **L081-family LVGL crashes** ([#190], [#80], [#776], [#840], [#873]–[#878]) — `lv_event_mark_deleted` now bails defensively when `event_head` is corrupt (chain depth > 256 or pointer misaligned), reporting via `helix_lvgl_anomaly` with the head/target/depth context and a hex backtrace instead of segfaulting. Combined with the new async-delete breadcrumb hook (below), the next crash report from this family will name the widget being torn down at corruption time.
- **Markdown null-deref on tight-memory devices** ([#879]) — md4c could null-deref deep inside `md_analyze_inlines` when an internal allocation failed (observed on K1, 209 MB RAM). The observer text path now short-circuits empty/null inputs and caps input length at 256 KB before handing off to md4c.
- **Wizard step transition UAF** ([#871]) — style transitions queued by the previous step's widgets could fire `trans_anim_start_cb` on a freed widget after `lv_obj_clean`. The wizard now walks the subtree and cancels every pending animation before the clean.
- **Print Select detail view: clear thumbnail by passing nullptr** — passing the buffer pointer instead of nullptr left the image cache thinking the slot was still live.
- **Advanced panel: timelapse-setup row visibility race** — nested `bind_flag_if_eq` / `bind_flag_if_not_eq` on the same widget produced a last-one-wins race; the row is now nested inside a parent that owns the visibility binding.
- **Modal button row: danger styling now renders** — the `primary_variant` XML prop wasn't wired through, so destructive modals ("Delete profile") rendered with primary blue instead of danger red.
- **SDL sound: resize mix buffer before unpausing audio device** — fixes a transient pop / silence on the first sound after device init.
- **Bluetooth BLE-only device connect** — fall back to `ConnectProfile` when `Connect` fails on BR/EDR-less peripherals, instead of surfacing a generic "connection refused".
- **About panel: Install Root row icon** — was referencing `folder_home` which isn't in the MDI font subset; falls back to the shipped `folder` glyph.

### Changed
- **Crash-capture diagnostics** — LVGL async-delete now leaves an `async_d <class> <ptr>` breadcrumb naming the root widget being torn down (#840 diagnostic). Breadcrumb ring doubled from 64 to 128 slots, plus gated cache-eviction crumbs (#851 family). Pure diagnostics; no behavioral change.
- **i18n** — UI strings synced across 8 locales; 59 dead keys dropped from translation files.
- **Logging** — thumbnail flow is now visible at debug; the 5-second poll log is quieted.
- **CI** — stable-tag releases now dispatch a docs deploy to helixscreen-website automatically.

## [0.99.44] - 2026-04-23

### Added
- **SonicPad hardware brightness** — Creality Sonic Pad (allwinner,r818) now gets real backlight control via `/dev/disp` DISP2 ioctls. The AD5M-specific boot-time PWM-inversion reset is skipped on SonicPad, where it logged "pwm device hdl is NULL" and dragged the display pipeline.
- **Shutdown/Reboot verification** — the shutdown widget now verifies the host actually went down. Moonraker can reply `machine.shutdown`/`machine.reboot` with success but silently no-op on some firmwares (observed on SonicPad Jpe230). If the WebSocket has reconnected 20s after a successful RPC, an error toast tells the user the host is still reachable.
- **Test Screensaver button** in Settings lets you audition the selected screensaver without waiting for the dim timeout.
- **"Confirm before running" macro toggle** (Settings → Safety & Notifications, default on) gates macro execution behind a confirmation dialog. Skipped when params are required or when the macro is flagged dangerous.
- **Memory pressure responders for print status tree and LVGL image cache** — on warning+, the cached print-status widget tree (~400–800 KB) is destroyed when off the nav stack; on critical, the LVGL decoded-image cache is dropped.
- **Memory-monitor diagnostics** — pressure log lines now include our RSS/VmSize/VmSwap, 5-min growth, and system-available; on warning+ the `smaps_rollup` breakdown is dumped; a post-response summary reports the whole-app RSS delta so operators can see whether responders actually recovered memory.
- **Configurable scroll_guard cooldown** — `/input/scroll_guard_cooldown_ms` (and `HELIX_SCROLL_GUARD_COOLDOWN_MS`), clamped to 20–500 ms. Default stays 80 ms; AD5X users still seeing phantom clicks at lift-off can try 150–200 without a code change.
- **AD5X fresh installs ship the AMS widget on the home grid by default** — previously fell through to the generic default layout whose filament/AMS swap depended on `ams_slot_count` being live at wizard-completion time. Existing installs keep their saved layout.
- **Android Play Store publishing pipeline** — CI now builds and uploads signed AABs to Google Play end-to-end, gated on the Play service-account secret.
- **Contributor doc set** — CONTRIBUTOR_GOTCHAS.md, YOUR_FIRST_CONTRIBUTION.md, THEME_CONTRIBUTOR_GUIDE.md, TRANSLATION_CONTRIBUTOR_GUIDE.md, and a CONTRIBUTING.md front door at the repo root.

### Fixed
- **Text-input null-font crash** ([#853], [#864]) — `text_font` is now bound at creation time so a forced theme refresh can't land a null font pointer on the widget mid-layout.
- **Upgrade banner missing / floating "Update" + "X" buttons on every panel** — `upgrade_banner.xml` was registered with the wrong path (it lives under `ui_xml/components/`), and the banner also used an unregistered `<ui_icon>` tag that desynced the XML parser's parent stack and left the child buttons parented to the top layer. Rebuilt against theme tokens matching `toast_notification.xml`.
- **Stuck AMS widget at (-1,-1) with filament sensor occupying its cell** — a one-shot home-panel migration swaps AMS into filament's cell on any page where AMS is enabled-but-unplaced while filament is placed. Fixes existing installs caught by the `build_default_grid` timing window before the AD5X preset ship.
- **AD5M detected over AD5X when a chamber LED is present** — added `macro_exclude` on `SET_EXTRUDER_SLOT` (unique to AD5X IFS) to the four AD5M-family entries, so AD5X no longer loses the tiebreak to AD5M Pro when both fingerprints score 100.
- **AD5X IFS: ejected spool still shown as full** on the `GET_ZCOLOR` / save_variables path — mirrored the #631 eject-clear into `parse_save_variables` (only when IDLE).
- **Niimbot BLE re-pair required on every restart** — the label printer overlay's brand-based dedup mistook Niimbot's rotating BLE random address for a "wrong transport" migration and cleared the saved MAC. Same-transport duplicates are now dropped; the saved MAC sticks. Real BlueZ errors also now surface instead of "unknown error" from post-teardown reads.
- **Moonraker subscription leak across reconnects** — `lifetime_weak()` was handing `SubscriptionGuard` the same guard that `disconnect()` resets, so every reconnect looked like "client destroyed" and the real unsubscribe was skipped. Split into two guards.
- **WiFi wizard step re-entry hardening** — `apply_ethernet_status` and the deferred scan callback now early-out when the step is mid-teardown. Not a root-cause fix for the heap corruption in MCPKABEE/QLCCZKRQ, but removes a known re-entry race.
- **Print status panel "already in stack" warning spam** on long sessions — the print-start auto-push now checks `is_panel_in_stack()` before queuing.

### Changed
- **Crash-capture breadcrumbs** — finer-grained wifi-step tags (`create_enter`, `xml_create`, `scan_fire`, `populate_begin`, `cleanup_enter` under a `wifi` category), plus post-Moonraker-connect discovery and printer-identify breadcrumbs. Diagnostic only.
- **Touch-feel documentation** — CONFIGURATION.md, TROUBLESHOOTING.md, and ENVIRONMENT_VARIABLES.md now carry a symptom → setting table for `scroll_limit`, `scroll_throw`, `jitter_threshold`, and `scroll_guard`, plus a new "Unintended Clicks While Scrolling" troubleshooting entry. The docs also listed the `jitter_threshold` default as 15 (has been 5 since v2→v3); corrected along with two clamp-range typos.
- **Bluetooth setup guide** covers USB barcode scanners and USB-MCU users.

## [0.99.43] - 2026-04-22

### Added
- **Niimbot B-series support (B1 / B18 / B3S)** — added to the brand table with the shared 384-dot printhead profile. Previously these models were unsupported.
- **New label sizes** — 30×20, 30×40, 40×40, 25×30, 14×40, 14×60. Fixes **"feeds but prints blank" on B1 with 30×20 stock**, where the printer previously defaulted to a 50×30 template and advanced 50 mm of material across 2.5 of the user's actual labels per print.

### Fixed
- **Dual-mode Bluetooth label printers no longer fail to connect** — dual-mode Niimbots (D110 and siblings) enumerate as two BlueZ devices with the same name: a BR/EDR half exposing only SPP/PnP and a BLE half carrying the vendor GATT service. Discovery dedup was MAC-based, so both entries appeared in the picker and selecting the BR/EDR half always failed with `br-connection-profile-unavailable`. Dedup now resolves same-name conflicts via the brand table and keeps the vendor's preferred transport. Users whose saved pairing points at the wrong-transport sibling are auto-migrated on the next discovery (saved MAC cleared) so a single re-pair puts them on the right half.
- **"(Saved)" indicator showing on every Bluetooth device** — the saved-pair tag was flagging any device with a non-empty MAC (i.e., all of them); it now compares against the actual saved `bt_address`.
- **AD5M: crash reporter recursion on uncaught exception** — the exception record path is now async-signal-safe. When a C++ exception escaped, the crash writer recursed and the process aborted with "std::terminate without active exception" before the report could be written. Reports from AD5M devices will now arrive intact.
- **Thread exhaustion crash on small devices** ([#837]) — raw detached `std::thread(...)` sites are now routed through managed pools (`HttpExecutor::fast/slow`, `BusThread`) or wrapped in try/catch so `pthread_create` EAGAIN surfaces as a toast instead of terminating. Most visible on AD5M/CC1 under thread pressure.
- **WiFi panel use-after-free on scroll** ([#850]) — the LVGL input device is now reset before clearing the network list, closing the window where the scroll indev dereferenced a freed scrollable mid-rebuild.
- **Overlay dismiss use-after-free** ([#840]) — widget deletion in `destroy_overlay_ui` is now deferred, removing the window where a pending async callback could fire on a freed overlay.
- **Thermistor rediscover UAF** — paired `SubjectLifetime` as a member next to `ObserverGuard` so per-sensor observers expire cleanly when dynamic temperature subjects are destroyed on reconnect.
- **Observer `release()` → `reset()` in widget `LV_EVENT_DELETE` callbacks** — closes a zombie-observer class that caused rare render-state corruption behind the #579 report cluster.
- **Config: `telemetry_enabled` source-of-truth** on v13→v14 migration — resolves cases where the in-app toggle state could disagree with the stored value.
- **Crash-handler diagnostics** ([#851]) — `queue_prev` ring expanded to 4 slots for richer last-actions context; silenced an empty-name image-lookup spam path during crash capture.

### Changed
- **Toast contrast** — toast cards now sit on `elevated_bg` with a theme border, reading cleanly against busy backgrounds on all themes.
- **Printer log noise** — per-tick hot-path logs only emit on an actual state change, significantly lowering journal volume on idle devices.

## [0.99.42] - 2026-04-22

### Fixed
- **Snapmaker U1 self-update bricked on v0.99.41** — the in-app updater fell through to the Raspberry Pi tarball because `HELIX_PLATFORM_SNAPMAKER_U1` had no mapping in `get_platform_key()`. U1 devices ended up with a Pi aarch64 binary missing libsystemd/libinput/libEGL/libGLESv2/libgbm/libasound, and helix-screen failed at exec time with "cannot open shared object file". A CI guardrail now blocks any new platform from shipping without the right asset mapping. **Recovery from v0.99.41:** the broken binary can't self-update out of this bug — reinstall the Snapmaker U1 tarball manually using the instructions on the release page.

## [0.99.41] - 2026-04-21

### Added
- **Spoolman: spool lists sorted by last used** — AMS edit modal and Spoolman panel show the most recently used spool first; never-used spools sink to the bottom.

### Fixed
- **Spoolman new-spool-on-save** — the external spool modal's Save path failed to create the spool in Spoolman and never told Moonraker it was active. Density and diameter are now supplied on POST /v1/filament; initial_weight on POST /v1/spool; AmsEditModal fires sync_active_spool after create and re-syncs on every linked save (recovers from Moonraker state loss).
- **Crash-report modal occluded by install wizard** ([#849]) — backdrop now parents to the top layer so Send/Dismiss is always reachable.
- **i18n: icon_font_hero missing on smaller font tiers** — previously mapped to xxlarge-only glyphs, producing font-not-linked warnings and empty icons on CC1, AD5M, pi32, and K2. Now maps to each tier's largest available glyph.
- **Wizard Back/Next crash during install** ([#848], [#843]) — wizard navigation routes through lv_async_call so the clean+rebuild runs outside the input-release tick.
- **AMS AFC: redundant per-update hashtable lookup** — caller loops already know the slot index; closes the torn-pointer fault seen in bundle 8SA9DQZ4.

### Changed
- **i18n memory: −4.7 MB peak RSS on CC1** — dropped dead lv_i18n path; translations load per-locale on demand. Saves ~1.2 MB binary / ~521 KB VmExe; VmHWM drops 19 MB → 14 MB on CC1 (under the 15 MB OOM warning threshold). All 9 languages remain runtime-selectable.

## [0.99.40] - 2026-04-21

### Added
- **XML hot-reload** — set `HELIX_HOT_RELOAD=1` to watch `ui_xml/` (including breakpoint subdirs) and rebuild the active panel, overlay, or modal in place when a file saves. Panels, overlays, modals, and the navigation stack all implement a `rebuild()` hook; after-reload callbacks re-show the top modal and refresh observer bindings. The watcher warns when a reload would dangle an XML subject pointer so you see it immediately instead of as a later crash.
- **Print Select:** 3-column card grid on 480px breakpoints (previously 2-column) — more prints visible without scrolling on wider portraits.

### Fixed
- **Android update check** — libhv is built without SSL on Android, so HTTPS to R2 and GitHub silently failed with "Connection failed (R2 + GitHub)". Update-check traffic now routes through Android's system TLS stack via a JNI bridge to `HttpURLConnection`. The nightly auto-check and its in-app "New Version Available" notification now work, and the Install button opens the Play Store listing instead of attempting a tarball install.
- Action-prompt modal: remove button event callbacks before freeing `user_data` to avoid a use-after-free on dismissal ([#840]).
- LVGL: guard against a garbage non-null font pointer in the `sw_label` renderer that crashed under rare layout cascades ([#842]).
- AMS edit modal: async-delete spool list children to avoid the LVGL event-list corruption pattern (L081) ([#845]).
- Wizard: stop the connection spinner animation before `lv_obj_clean` to prevent a use-after-free on step transitions ([#843]).
- AMS AD5X IFS: close the race where `_IFS_VARS` was enabled before the macro had been verified present on the printer.
- Discovery (AD5X): IFS runout sensors now appear in `filament_sensor_names` so the runout row shows on AD5X.
- Runout modal: removed stray XML `event_cb` declarations that produced "callback not found" warnings in the log.
- Hot-reload: reset `error_code` between filesystem queries and use the non-throwing iterator increment — stops the watcher from aborting on transient `ENOENT`.
- About settings: Check-for-Updates row description is visible at every breakpoint, so the "Error" / version text doesn't get hidden on narrow layouts.
- Update flow: skip the redundant confirmation modal when the user already confirmed "Install" on the in-app update notification.

### Changed
- AD5M / AD5X launcher: enable `MALLOC_CHECK_=3` and `MALLOC_PERTURB_` for heap diagnostics on the printers most prone to memory-pressure crashes ([#838]).

## [0.99.39] - 2026-04-20

### Added
- **Synth PCM backends on VoiceSlot** — ALSA and SDL backends now render per-sample through `VoiceSlot` with per-sample envelope and filter state. The sequencer publishes `NoteEvent` for continuous-audio backends and per-tick events for PWM/buzzer backends, unifying envelope behavior across every platform.
- **Sound preview overlay** — Browse and preview individual sounds from the sound settings; a new "Preview Sounds" button opens `SoundPreviewOverlay`, a dynamic button grid populated from `SoundManager::get_sound_names()`.
- **Build:** New `PLATFORM_TARGET=ad5m-br` mode and `make install DESTDIR=...`
  target, enabling external build systems (starting with the AD5M Klipper Mod
  firmware) to package HelixScreen as a native variant. See
  `docs/devel/AD5M_KMOD_VARIANT.md`. The existing `ad5m` target is unchanged.
- Per-printer first-print phase defaults — each preset ships its own pre-print warm-up timing.
- Header bar: per-widget `icon_size` override for oversized action-button glyphs.
- Crash diagnostics: breadcrumb on the `discovery-complete` path to narrow the recurring copy-assign crash signature.

### Fixed
- **Installer (K1 / K2 / Snapmaker U1):** Repaired five regressions from the
  earlier `config/` → `assets/config/` refactor. On affected boards the
  installer was silently leaving the stock Creality UI running alongside
  HelixScreen (`[WARN] No platform hooks for: k2`) and skipping Mainsail/Fluidd
  config symlinks (`[INFO] No printer_data/config found, skipping config
  symlink`). The underlying issues: init script sourced hooks from a path
  that no longer existed; `deploy_platform_hooks` looked under
  `config/platform/` instead of `assets/config/platform/`; `KLIPPER_HOME`
  defaulted to `/root` on K1 and K2 whose `printer_data` lives on `/usr/data`
  and `/mnt/UDISK` respectively; Snapmaker U1's detection of `/home/lava` was
  fragile on freshly-flashed units. All five fixes auto-apply on the next
  self-update. Already-installed users who cannot update can apply the
  migration manually:
  ```sh
  /etc/init.d/app stop; /etc/init.d/app disable   # K2 only
  mkdir -p /opt/helixscreen/platform
  cp /opt/helixscreen/assets/config/platform/hooks-k2.sh \
     /opt/helixscreen/platform/hooks.sh
  chmod +x /opt/helixscreen/platform/hooks.sh
  sed -i 's|/assets/config/platform/hooks\.sh|/platform/hooks.sh|' \
     /etc/init.d/S99helixscreen
  /etc/init.d/S99helixscreen restart
  ```
  Verified on K1C, K2 Plus, AD5M Forge-X, Snapmaker U1, and CC1 hardware.
- Discovery: `box` and `enclosure` now match as chamber-sensor keywords — Elegoo COSMOS `temperature_sensor box` and common modder `enclosure` names enable the chamber row on CC1 and similar builds.
- Presets: Snapmaker U1 macros and hotend fan aligned with hardware; CC1 hardware list completed and filament sensor pre-declared; K1C `fan2` role and filament macros aligned with stock firmware; K2 Plus hardware list corrected (bogus "K2 Max" entry removed).
- Display: bundle minimal zoneinfo so IANA timezones resolve on CC1 (ships without `/usr/share/zoneinfo`).
- Synth: step-boundary silence no longer kills active PCM notes; `button_tap` suppressed in the preview overlay; per-sample envelope restores attack/release shape; filter state preserved across buffer boundaries; smaller SDL ring buffer cuts perceived latency.
- Print status: low-memory deactivation clears gcode-viewer dedup guards; `end_overlay` observer reset before subject deinit; MICRO metadata sizing and end-overlay race.
- Print: `ActivePrintMediaManager` uses an immediate observer to close the stale-thumbnail race during rapid print transitions.
- Print select: egg placeholder removed; history/queue fetch guards hardened so transient Moonraker outages don't leave the panel empty.
- Toast: responsive width restored after the stacking refactor.

### Changed
- Synth architecture: `set_voice_envelope` removed (deprecated); PCM backends now own per-sample rendering through `VoiceSlot`.
- Build: libhv's symlinked `LIBHV_DIR` cleaned of stale artifacts; `ad5m-br` added to sound + `.PHONY` filters; cmake scaffolding pruned from the installed `ui_xml/` tree.

## [0.99.38] - 2026-04-20

### Added
- Toast notifications now stack when multiple fire simultaneously; startup warnings (NOTIFY_*) queue through PendingStartupWarnings so none are lost during init.
- QIDI Box AMS backend scaffolding — placeholder for future PLUS4/Q2/MAX4 filament system support.
- Per-extruder filament consumption routing: multi-extruder printers track spool usage independently per tool.
- Tool-changer identity mapping: `slot_for_extruder()` routes the correct AMS slot to each physical tool.
- Printer discovery treats 'cavity' as a chamber synonym (Snapmaker U1 compatibility).

### Fixed
- Wizard: connection step migrated to AsyncLifetimeGuard, preventing use-after-free on rapid step transitions; WiFi subject re-init is now guarded.
- Wizard: manual printer-type pick now correctly applies the selected preset (#837).
- Presets: ForgeX split from KlipperMod profile; ZMOD stepper fan name corrected.
- AD5M ZMOD: probe for `/dev/fb0` before launching to avoid cold-boot race condition.
- Android: read-only seed bundle now ships via build-time packaging manifest.
- Presets: AD5M fan defaults aligned with actual hardware (#837).
- Print start: deduplicate probe samples and complete detection on first extrusion move.
- Home panel: widget ID cache reads from populate snapshot, not post-populate re-read.
- Home panel: favorite macro label no longer overlays icon; long names truncate with ellipsis.
- Home panel: Home label placed on same row as homing buttons.
- AMS panel: blank weight field shown for unknown spool instead of "-1".
- Per-extruder filament subjects pre-populated to prevent null-subject observer crashes.

### Changed
- Print status panel refactored from imperative UI state to declarative XML bindings.

## [0.99.37] - 2026-04-19

### Added
- Filament slot metadata now persists across restarts and reconnects on AD5X IFS, Snapmaker U1, ACE, and CFS — brand, spool name, Spoolman link, and weights survive Klipper reloads and printer reboots.
- OrcaSlicer 2.3.2+ automatic filament sync: HelixScreen writes the AFC-originated `lane_data` Moonraker DB convention, so Orca's filament panel picks up your slot metadata with zero configuration.
- "Clear Spool" entry in the AMS slot context menu — clears user overrides and falls the slot back to firmware-reported state. `Spool Info` and `Clear Spool` are now separate menu actions.
- Empty-but-overridden slots now ghost-render (20% opacity) across all backends, matching the previous AFC-only behavior.
- Automatic override clearing on detected physical spool swap: color change for IFS, RFID UID change for Snapmaker, material+color fingerprint change for CFS, status transition (empty→loaded) for ACE.
- Per-backend material whitelist and alias normalization for AMS (e.g., `PLA-BASIC` → `PLA`).
- Public specification at `docs/specs/filament_slots.md` for the shared `lane_data` convention.
- Crash diagnostics: breadcrumbs for wizard step transitions, language changes, and the previous queue-callback tag.
- ARM32 frame-pointer unwinding fallback so Pi32 and SonicPad crashes produce usable backtraces when `backtrace()` fails.
- Translation strings for camera, QR code, and print stats labels.

### Fixed
- Snapmaker U1: `set_slot_info` honors the `persist` parameter — user edits were previously discarded on the next Klipper status poll.
- Snapmaker U1: RFID `SUB_TYPE` is now mapped to the spool name and shown in the edit modal.
- Wizard: freeze `UpdateQueue` around step transitions to prevent async-callback races on quick advance (#827).
- Crash handler: reorder so `backtrace()` runs last, with FP-walk fallback (#827).
- Crash reporter: filter post-crash lines out of the log tail (#827).
- Crash: `std::terminate` exit encodes `128 + SIGABRT` so the watchdog surfaces the recovery dialog.
- Brother QL label print: route network send through HttpExecutor — Test Print no longer crashes from unbounded thread spawns.
- Spoolman: `Connect` probe runs on HttpExecutor and the button is disabled in-flight.
- Home/widget/tool/temp rebuilds use `safe_clean_children` to escape the UpdateQueue batch (#834, #776).
- AMS pre-print filament runout warning suppressed for auto-unload backends (IFS).
- AMS edit modal: skip Spoolman save when Spoolman is unavailable so local overrides still persist.
- AMS panel: `remaining_weight_g=-1` displays as "unknown" rather than empty; bypass label escapes the canvas; MICRO/TINY layouts no longer clip spool cards or temp graph.
- Moonraker API: `database.get_item` / `database.get_namespace` now unwrap the `response["result"]` envelope.
- AD5X IFS: defer initial `GET_ZCOLOR` until Klippy reports ready.
- AD5X IFS: latch the `_IFS_VARS` macro-missing check so `save_variables` notifications can't re-enable the broken path.
- Color picker: selected-preset outline, segmented tab fill, MICRO/TINY polish.
- Dropdown: honor theme `border_radius` as local style; narrow backup dropdown at MICRO/TINY.
- Filament panel: temperature graph fills and spool card sizes to content so the whole card stays on-screen at MICRO/TINY.
- Network widget: re-detect Wi-Fi when backend reports READY so "Disconnected" unsticks after async init (#819).
- IFS: pre-update baseline color in `set_slot_info` to avoid self-wiping the override it just set.
- Moonraker: tighten `delete_item` missing-key detection (prefer HTTP 404).

### Changed
- ACE: user override fields merge with firmware-reported data field-by-field (empty override fields fall through to firmware). Previously the override replaced the entire slot record.
- Pre-existing `helix-screen:ace_slot_overrides` / `helix-screen:cfs_slot_overrides` DB entries auto-migrate to `lane_data` on first launch; legacy JSON caches are removed after migration.

## [0.99.36] - 2026-04-18

### Added
- Live spool weight tracking without Spoolman: HelixScreen now decrements the locally-stored external spool's weight during a print, persists it on pause/end, re-snapshots when you edit the weight externally, and warns before queuing a print that needs more filament than the spool has left.
- Per-preset default home layouts — fresh installs get curated layouts per printer preset instead of one generic grid.
- Default home anchors nozzle + bed temperature widgets stacked next to print status across every breakpoint.
- Default home swaps the single-slot filament widget for the AMS widget when an AMS is present.
- Unified crash log collection: reports pull log tails from file, syslog, or journalctl automatically (AD5M/AD5X via syslog, Pi via journald) and auto-attach a debug bundle to the report for triage.

### Fixed
- GridEditMode: clear cached widget pointers after async rebuild to prevent SIGSEGV when deferred selection-chrome destroy runs on freed objects.
- Screensaver and confetti teardown routed through safe deferred delete so parent cleanup no longer races with pending async widget deletion.
- LVGL: disable blur tree walk unconditionally (#820).
- LVGL: cap `lv_obj_get_screen` parent walk to survive cyclic parent trees.
- Wi-Fi: NetworkWidget wakes when the backend reports READY after async init (#819).
- AD5X: backlight-off sleep restored for pre-#431 units via config v13 migration.
- FlashForge AD5M: Screws Tilt directions corrected (no longer inverted).
- Bed mesh: calibrate modal sizes to its content.
- CC1: bed mesh calibration sequence and progress count for load_cell_probe.
- AD5X IFS: use `GET_ZCOLOR SILENT=1` for live loaded-slot state.
- Edit mode: widget chrome buttons responsive across breakpoints.
- Default home: carousel deferred until after the first-run wizard dismisses.
- Console: gcode console font bumped to 10px on the micro breakpoint.
- AMS state: `get_external_spool_info` now mutex-locks the in-memory cache.

### Changed
- Theme: Soft corner radius bumped to 3px on the micro breakpoint (was 2px) — less sharp on 480x272 screens.
- Crash report worker renders breadcrumbs, heap snapshot, LVGL event log, and debug-bundle link in filed issues (#826).

## [0.99.35] - 2026-04-17

### Added
- Touch-friendly filament mapping: print-file details card shows a 2×2 grid of pills with the tool number (Tx) centered inside the gcode color dot; overflow past six tools collapses into a "+N" indicator. Mapping modal rows and slot-picker popup rows are taller, wider, and consistent with the same Tx-in-dot treatment.

### Fixed
- Settings → About: update modal reshows correctly after backdrop/ESC dismiss; DownloadStatus is reset before reopening.
- Updater: detect a wedged `/var/log` before launching install.sh; set `O_CLOEXEC` on the instance lock so post-install restart is not blocked.
- Restart: exec in-place instead of fork+exec to avoid a zombie race.
- Barcode scanner: AZERTY punctuation mapping corrected with a cross-layout regression test; BT pairing UX, keycode diagnostics, accent variant, reduced log noise.
- Bluetooth: register BlueZ `Agent1` so pairing completes into a real bond.
- Temperature graph: chamber series now shown for sensor-only (no thermistor) setups.
- Wi-Fi: require a live NetworkManager daemon before choosing the NM backend.
- Crash reports (AD5X / MIPS): surface the return-address register so MIPS backtraces resolve (prestonbrown/helixscreen#818).
- Bed mesh rendering: close a missed-wakeup race in the render thread's stop/request handshake.

### Changed
- Render performance: pre-blended card borders, 1:1 thumbnail blit, and camera FPS throttling cut per-frame CPU load.

## [0.99.34] - 2026-04-16

Hotfix for the v0.99.33 release: cross-compiled release bundles for every embedded platform (ad5m, ad5x, cc1, k1, k2, pi, pi32, snapmaker-u1, x86) shipped without `assets/config/`, causing "Could not load printer database" on first launch and breaking shipped themes, platform init hooks, print start profiles, and sound themes. v0.99.33 artifacts have been withdrawn.

### Fixed
- Release bundle omitted `assets/config/` seed tree — the refactor that split RO seeds out of `config/` updated `scripts/package.sh` but missed `mk/cross.mk`, which is the actual pipeline every embedded release uses. All `release-*` targets now ship `assets/config/printer_database.json`, `printing_tips.json`, `default_layout.json`, `helix_macros.cfg`, `themes/defaults/`, `presets/`, `print_start_profiles/`, `sounds/`, and `platform/hooks-*.sh`.

## [0.99.33] - 2026-04-16

Major Bluetooth reliability overhaul, new barcode scanner settings UI, first-run guided tour, HttpExecutor for bounded HTTP threading, responsive setting rows that collapse 7 micro/ XML variants, and a broad config refactor splitting read-only seed data from writable state.

### Added
- First-run guided tour with coach-mark overlay, responsive tooltips, AMS-conditional steps, and replay from Settings > Help
- Barcode scanner settings overlay with BT device discovery, pairing, MAC binding, and USB device list
- HttpExecutor — bounded-worker HTTP executor (fast lane: 4 workers, slow lane: 1) replacing unbounded thread spawns
- Responsive setting rows with info icon, collapsing 7 micro/ XML layout variants (#805)
- New XML binding attributes: hidden_if_prop_eq/not_eq/empty and bind_style_if_eq/not_eq/gt/ge/lt/le (#805)
- Frame performance telemetry: idle filtering, per-panel breakdown, and separate render/flush timers
- LVGL display anomaly section in stability dashboard
- Bluetooth HID scanner binding by MAC address with exclusive grab
- BT HID link verification after pairing with bond-refusal warning
- Bluetooth `enumerate_known` API for paired device listing
- `HELIX_CONFIG_DIR` and `HELIX_DATA_DIR` env vars for Yocto/read-only rootfs deployments
- Config path resolver helpers (`find_readable`, `writable_path`, `get_data_dir`)
- Crash diagnostics: activity breadcrumb ring, cached heap snapshot, LVGL event dispatch hook
- Silenced hardware items logged at startup for easier debugging
- Moonraker silent request mode — suppresses `REQUEST_TIMEOUT` events for background queries
- Cycling encouragement messages during long installs (#809)
- Responsive keyboard sizing with elevated keycaps and smart contrast text
- KIAUH extension shipped and registered on release installs

### Fixed
- Crash: defer GridEditMode rebuild + harden LVGL event chain (#814, #812)
- Bluetooth: serialize all D-Bus operations (discovery, pairing, GATT, notifications) through BusThread, eliminating race conditions and thread-safety issues (#811)
- Bluetooth: `thread_id_` race, submit TOCTOU, slot unref routing, `StartNotify` fallback guard
- Network: async backend init eliminates UI-thread blocking; self-join deadlock and ethernet thread pool fixes
- Exclude object: sync removals from Klipper status, drop stuck optimistic visuals on print end, silence spurious pre-print RPC timeouts
- Moonraker API callbacks guarded with lifetime tokens to prevent use-after-free
- Controls: segmented homing button bar on controls and micro controls panels
- Slider: responsive knob padding at tiny/micro breakpoints; fix overflow_visible attribute name
- Z-offset: compact format and wider temp icon gap at tiny breakpoint
- Camera: sleep callback token survives stream stop/start cycles
- Watchdog: bail out of restart loop on persistent failure instead of infinite retries
- Tour: re-target highlight on breakpoint change; cancel on navigation away from Home
- AMS: unlink external spool updates UI when previous filament color was black
- Scanner: dismiss progress toast on pair failure; wrap BT thread spawns in try/catch
- Label renderer: render negative spool IDs as 'TEST'
- Tool state: atomic write for `tool_spools.json` prevents corruption on crash
- API/camera: catch EAGAIN on `join_helper` thread spawn in destructors
- Print status idle card: micro breakpoint polish, subject-driven visibility
- Installer: prefer `/user-resource` for temp dir on CC1
- Help icon: resolve via responsive theme token
- Updater: 2min timeout extended for slow printers

### Changed
- Config layout: read-only seed configs moved to `assets/config/`, writable state stays in `config/`
- Bluetooth: D-Bus operations serialized through dedicated BusThread instead of ad-hoc thread spawns
- Power-device API calls migrated to HttpExecutor with tok.defer lifecycle safety
- Bluez detection uses pkg-config instead of compile-probe
- Yocto build support: `PLATFORM_TARGET=yocto` mode, bitbake LDFLAGS, Docker dev loop

## [0.99.32] - 2026-04-15

Adds COSMOS firmware support for the Elegoo Centauri Carbon (CC1) and new per-channel display color correction (gamma, warmth, tint). Also includes a motion home widget, extensive crash/observer telemetry, and numerous discovery, installer, and widget fixes.

### Added
- CC1 (Elegoo Centauri Carbon) support on COSMOS firmware with factory white-balance calibration and live-hardware-driven preset
- Per-channel gamma + warmth display color correction (#803)
- Tint axis (G shift) for purple/magenta correction (#803)
- Motion home widget that opens the motion overlay directly
- Type icon stacked under cancel badge for gated panel widgets
- LVGL anomaly detection with call traces and arc/observer silent null-return logging

### Fixed
- Crash handler: stack-scan backtrace fallback for aarch64 and x86_64 (#795, #796)
- Crash: backtrace captured in terminate_handler (#801)
- Grid edit: stale child_count debug loop crash removed (#800)
- PrinterState: aliased hash copy eliminated in set_hardware (#799)
- Print status: Pause/Tune/Cancel enabled during Preparing phase (#798)
- Discovery: live hardware passed to auto_detect_and_save (#802)
- Discovery: retry on klippy state transitions, suppress retry toast, and differentiate deferred vs failed discovery
- Preset: deep-merge full preset and populate printer type from DB; CC1 gamma dropped to 1.0 to stop washing out shadows
- Panel widgets: eliminate load-on-every-access churn (#804); gate-aware rebuild short-circuit so ungating takes effect
- Touch calibration: settings-path accuracy, safety, and watchdog
- Update queue: queue_critical bypass for one-shot init callbacks
- Config: gate preset printer-type lookup out of splash/watchdog builds
- AD5X IFS: use IFS_REMOVE_PRUTOK when unloading active slot
- Snapmaker U1: keep WiFi alive across helixscreen stop/update (#797)
- Installer: escalate to SIGKILL without tripping set -e on orphaned procs; add CC1 + Snapmaker install dirs to uninstall search; work around COSMOS config-manager screen_ui allowlist on CC1
- Updater: cover all release platforms and prefer tar.gz on GitHub fallback
- Scanner picker: re-find device_list widget to avoid stale cached pointer
- LVGL: instrument lv_obj_delete_async for double-schedule + UAF telemetry
- About: source contributor list from committed CONTRIBUTORS.txt
- Home: observe every gate subject directly and shrink coalesce window so late-arriving capabilities un-gate widgets promptly
- Release manifest: default to tar.gz-only until pre-v0.99.31 ages out
- Build: stub helix_lvgl_anomaly() for splash and watchdog binaries
- Tests: stabilize eventloop shard flakes

### Changed
- Print status: button enable driven by subjects + XML bindings
- Home: panel rebuild triggered on capabilities_version instead of coalesce timer

## [0.99.31] - 2026-04-14

Adds a new XXLARGE responsive breakpoint tier and HiDPI font scaling for displays above 1000px tall (1440p/4K), with per-platform font pruning to keep binary size in check on constrained devices. Also includes a telemetry-driven printer database updater, bed mesh nozzle preheat, and a handful of crash and correctness fixes.

### Added
- XXLARGE responsive breakpoint tier for HiDPI displays (1440p/4K) with font, icon, spacing, and component scaling (#773)
- XLARGE/XXLARGE text and icon font assets with CJK fallback mappings
- Per-platform font tier pruning — each platform ships only the font sizes it needs
- Smart tier-aware fallback warnings when fonts are missing for the active breakpoint
- Interactive telemetry-driven printer database updater script with top-10 cap, dedup, auto-skip, and unique-device counting
- Bed mesh preheats the nozzle and bed via TEMPERATURE_WAIT before probing for more consistent results

### Fixed
- PrinterHardware dangling-reference crash after PrinterDiscovery snapshot change — guess_bed_heater / guess_hotend_heater / fan wizard now hold owned copies
- Theme rotation refresh preserving XXLarge breakpoint via shared helper
- Event depth counter detects event_head corruption on AD5X (#795)
- Network list deferred deletion during wizard cleanup causing heap corruption (#793)
- Wizard set_status() during cleanup causing blur walk crash (#792)
- Printer image caches now invalidated when the user changes printer image
- AD5X/ForgeX fan names, detection heuristics, and config paths corrected
- Empty hardware snapshots and false alerts after std::move of api->hardware()
- Release pipeline dispatches archive verification by format; MIPS ELF validation added
- K1/AD5X zip assets omitted from release manifest to unblock 0.99.29→0.99.30 updates
- FONTS_CORE extended and CJK xlarge/xxlarge guarded for platforms with pruned font sets

### Changed
- Unused mdi_icons sizes (20/28/40/56) removed, FONTS_CORE tightened
- AD5X IFS documentation clarifies stock zMod vs lessWaste/bambufy Moonraker visibility

## [0.99.30] - 2026-04-12

### Added
- ZeroG Hydra printer variants and updated Nebula naming in the printer database
- Power Devices chip in the printer manager overlay alongside LED controls
- Android `--test` mode launchable via intent extra

### Fixed
- Use-after-free in gradient cache during layout walk (#788)
- Hash table iteration crash in PrinterDiscovery copy-assignment (#789)
- Self-sizing gradient canvas replaces pre-rendered .bin files for correct rendering on high-res displays
- Diagonal smearing in backdrop blur caused by stride mismatch
- Aligned stride handling in the no-downscale blur path
- Spool canvas draw buffer resize use-after-free
- Modal dialog scroll confined to content area; added minimum height for breathing room
- Print select no longer repopulates when the file list is unchanged
- Metadata overlay flush and reduced thumbnail offset in print select
- Android display buffer resize after scaling, SW/GPU renderer routing, and GLES screen corruption
- Android nav bar white scrim eliminated; persistent nav bar with wider swipe edge zone
- Android contributor marquee animation on wide screens
- Android sounds and tracker MOD files extracted to writable storage
- Android update checker enabled with Play Store redirect
- Barcode scanners filtered from label printer Bluetooth dropdown (#779)
- AFC preferred over Snapmaker backend on U1 with aftermarket MMU (#779)
- Keyboard base character no longer inserted on long-press alternate key
- Perceptual volume curve replaces linear scaling for more natural sound levels

### Changed
- Android software renderer switched from FULL to DIRECT mode for better performance

## [0.99.29] - 2026-04-12

### Added
- High-DPI display support: DRM auto-downscale for panels exceeding 1920px, xlarge font constants for large displays, and fbdev resolution warnings (#773, #774)
- Android system keyboard toggle using native IME via SDL_StartTextInput (#774)
- Android ghost navigation bar with edge-swipe reveal and inactivity auto-hide
- GPU-accelerated SDL drawing backend with fixed temperature graph gradient bands
- Pre-print overlay now dismisses via authoritative Moonraker PRINTING state and RESPOND gcode completion match, replacing heuristic layer/progress triggers
- Zip archive support in release pipeline with tar.gz fallback
- Heap stats and startup phase snapshots in telemetry
- UX micro-breakpoints for responsive layouts (#763)

### Fixed
- Android black screen on app resume caused by missed SDL events while backgrounded (#774)
- Android high-DPI aliasing eliminated with integer display scaling (#774)
- Android crash reporter HTTPS failures bridged via JNI on devices without libhv SSL (#774)
- WiFi wizard click handler crash on deferred-deleted list items (#778)
- Nozzle temperature rows going stale due to lifetime token gate on version observer (#782)
- Printer discovery SIGSEGV from unsynchronized hardware struct copy (#777)
- DRM auto-downscale now correctly skipped when user explicitly sets `-s` (#773)
- Bed mesh fallback probe count divided by samples-per-point to match actual probe density
- G-code viewer streaming load failure now shows an error toast instead of crashing
- Null-guard on Moonraker API notify callback (#765)
- IFS spool eject detection via empty color field on Adventurer5M (#631)
- QWERTZ and AZERTY barcode scanner keymap support
- Webcam list filtered by service type to exclude unsupported streams
- Print select retries empty thumbnails on panel revisit
- G-code viewer layer re-frozen on terminal to idle transition

## [0.99.28] - 2026-04-10

### Added
- `-s WxH` CLI flag now honored end-to-end on DRM and fbdev backends: DRM connector mode selection, simpledrm detection with fbdev fallback, and fbdev kernel-size mismatch warnings surfaced as toasts once the UI is ready (#766)
- Bluetooth SDP channel resolver with cache — MakeID and Brother PT label printer backends auto-discover the correct RFCOMM channel and invalidate on stale-cache failures
- Forget button in the scanner picker modal and label printer settings panel for paired Bluetooth devices (plugin ABI: `helix_bt_remove_device`)
- Long-press a file card in print select to open the delete confirmation dialog
- Runtime log level setting in System settings
- Debug bundle default log tail raised from 200 to 2000 lines

### Fixed
- Back-to-back WiFi scans in the setup wizard could crash inside `std::sort` when a background callback rewrote the cached network list mid-sort (#769)
- LED toggle and brightness sent an all-zero color when the stored config was in a poisoned state, turning the strip off unintentionally
- Scroll position in print select card view was jarringly reset on every refresh tick
- Pre-print ETA now uses wall-clock total on printers with sparse phase detection, instead of producing wildly inaccurate phase-based estimates
- Modal destructor could dereference a stale backdrop pointer
- Printer discovery could double-fire hardware-discovered callbacks after the setup wizard, crashing in a race
- PrinterDiscovery hw-discovered callbacks accessed a destroyed instance when the discovery loop outlived the caller (#761)
- TipsWidget and CoalescedTimer could corrupt LVGL's timer linked list when cancelled during event dispatch (#760)
- Ethernet backend failed to detect non-standard interface names (#762)
- Emergency-stop warning text and position restored on probe calibration panel; cartographer calibration UX tightened (#754)
- BT Forget thread spawns wrapped in try/catch to survive ARM thread limits
- Cross-compilation targets serialize via a mkdir lock to prevent concurrent libhv source tree corruption
- Splash and watchdog binaries now link the display helpers added for #766
- Missing translations for settings menu across all languages
- Invalid `text_tag` XML attribute replaced with `translation_tag` on label printer and other panels
- Wizard defaulted to an empty host on fresh installs; now restores `127.0.0.1` when no config exists

### Changed
- Enhanced 2D G-code shading enabled by default (opt out via `HELIX_SSAO=0`); normal shading reverted to bidirectional at 0.12 strength for legibility

## [0.99.27] - 2026-04-09

### Added
- Enhanced 2D G-code shading with normal-based lighting, anti-aliased lines, and silhouette outlines
- Per-overlay visit tracking in telemetry dashboard

### Fixed
- Thumbnail display bugs in print select and detail views
- Print start timing heuristics for AD5M Klipper mod
- Use-after-free crashes in scanner picker modal and four overlay subclasses with shadowed lifetime guards
- Camera buffer use-after-free and timer deletion crash in setup wizard
- Unbounded mDNS pending records map growth from incomplete entries
- x86 self-update downloading wrong platform tarball (missing HELIX_PLATFORM_X86)
- Bluetooth discovery filter blocking barcode scanners
- WebSocket connection missing proper User-Agent header
- Race condition in G-code render completion flag ordering
- Modal button styling inconsistencies in action prompt and color picker
- Invalid text_secondary XML token replaced with text_muted

## [0.99.26] - 2026-04-08

### Added
- Material mismatch warning before starting a print when loaded filament doesn't match slicer expectations
- Telemetry tracking for in-app vs external print source distribution
- Zero G Nebula 370 printer image

### Fixed
- IFS native ZMOD port presence now inferred from save_variables and slot edits on AD5X
- Pre-print prediction history returning true with no entries, causing stale progress estimates
- Predicted total using dual atomics instead of mutex, risking torn reads
- Predicted weight computation not holding mutex during write
- Legacy bucket 0 entries lost when saving prediction history
- Weighted phase update not tracking detected phases for progress display
- Adaptive pre-print timeout completing prematurely on bed-first start macros
- BusyBox curl detection and HTTP mirror fallback for K1/AD5M installer downloads
- Filament mapping rows restyled as dropdown triggers with anchored picker and overflow clamp
- Dropdown trigger border not highlighted while filament picker is open
- Redundant 'nozzle too cold' warning toasts during filament preheat
- Part fan slider handle clipping at left edge
- Scanner picker not using declarative XML binding for Bluetooth availability
- Telemetry thumbnail/AMS rates, build volume source, and print source field corrections
- macOS CI test hang and TSAN timeout in nightly suite
- Moonraker external update not triggering restart on all platforms

### Changed
- Filament mapping rows now display as dropdown triggers instead of flat list items
- Bypass load routing encapsulated in AmsBackend::requires_slot_selection_for_load()

## [0.99.25] - 2026-04-08

### Added
- Adaptive pre-print time estimation: temperature-driven heating progress, thermal rate learning from PID calibration, and finish time ETA integration
- Bluetooth QR scanner support: discover, pair, and persist BT scanners from the scanner picker
- RGBW LED support: white channel toggle, detection, and color swatch (#737)
- White-only LED detection from Klipper configfile pin config (#748)
- Temperature graph legend chips displayed at 2x widget height or larger
- Auto-preheat for extrude/retract/purge when nozzle is cold with a known spool loaded
- Android Play Store readiness: AAB bundle build, back button handling, lifecycle pause/resume, and display diagnostics
- Klipper shutdown state detection with error message in recovery dialog
- Bed mesh probe progress tracking during pre-print
- Zero G Mercury One.1 and Nebula printer definitions

### Fixed
- Bed observer use-after-free from missing SubjectLifetime token (#746)
- Duplicate Chamber Temperature sensor from mock sensor pollution
- Fan widget 1x1 content centering and resolved display names at 2x1+
- Tool remap colors in 2D G-code renderer and chamber temp alignment
- Pre-print time composite remaining from thermal model + predictor
- Config migration using wrong JSON key for phase durations
- ETA extrapolation restored for progress 1–4% when no slicer estimate available
- IFS native ZMOD presence detection and dirty flag race on AD5X (#716)
- Spoolman active spool not syncing with Moonraker on assignment change
- Bluetooth scanner use-after-free from background thread callbacks
- Printer name discarded on manager overlay dismiss instead of saving
- Split button dropdown toggle, click-outside dismiss, and positioning
- Wizard preset mode incorrectly enabled for secondary printers
- Stale notification warnings after subject reinit
- Android: SDL.h build failure on embedded targets, WAKE_LOCK permission, USB sysfs guard, cache directory path, wizard localhost default
- Test runner false failures from teardown crashes and skipped test detection
- 2D G-code renderer elevation angle mismatch with OrcaSlicer thumbnail camera
- Navigation go_back re-activating closing overlay when animations disabled
- Initial fan/sensor status lost on multi-printer due to queue ordering race (#740)
- Fan speed percentage overlapping slider — moved inline to save vertical space
- G-code render mode not restored on print status reactivation, thumbnail offset
- Print status temperature card not fully clickable for temperature overlay
- External spool info not centered in card when no AMS present
- Pre-print predictor cache not invalidated on view open
- Release builds failing from transient apt-get network errors in Docker toolchain images

### Changed
- AMS spool edit actions consolidated into a split button ("Choose Spool")
- ETA display rounds to 30s/10s buckets for stable readout
- Print status temperature card is now fully clickable to open temperature overlay

## [0.99.24] - 2026-04-07

### Added
- Printer name sync: automatically resolve and write back printer name via Mainsail/Fluidd database on connect, rename, and wizard save
- Bed temperature widget as conditional last widget on home panel
- AMS widget automatically enabled on home panel when AMS detected during setup wizard
- Telemetry enhancements: periodic snapshots, frame time sampling, performance metrics, feature adoption tracking, and analytics dashboard with Performance, Features, and UX Insights views

### Fixed
- Render thread use-after-free from missing wait_for_finish_cb in SW draw unit (#739)
- Network list corruption when clearing entries (safe_delete_deferred)
- LED toggle on now respects saved brightness and color instead of defaulting to full white
- Custom keypad temperature ignored due to expired lifetime token
- Invalid text_primary theme token replaced with correct token
- systemd service startup ordering: use plymouth-quit-wait.service instead of multi-user.target (#536)
- Home screen widgets no longer disabled or show toast when grid is temporarily full during firmware_restart
- Chamber temperature regression in heater gcode generation (#745)
- Wi-Fi connection failures caused by PrivateTmp DGRAM socket isolation
- AMS loading error modal button wiring (#735)
- Use-after-free in bed/chamber observer subjects during grid edit (#734, #736)
- Print status view toggle button padding
- Print thumbnail now visible during Preparing Print phase on home screen
- Fan widget crash when subjects missing at startup
- Level screws showing checkmark icon for all screws, not just reference
- Telemetry timer pointers not nulled when LVGL torn down before shutdown
- Print completion dialog showing wrong icon for Failed/Cancelled states
- Fan arc excluded from long-press rename to prevent conflict with speed control
- Default widget grid layout bugs causing missing and misplaced widgets
- ForgeX printer detection: removed non-specific sensor heuristics
- Noisy warnings silenced on first unconfigured run
- PID calibration ETA smoothed with EMA dampening and 1-second countdown updates
- Artillery M1 Pro cooldown preset now includes chamber

### Changed
- Printer name editing converted to declarative subject binding

## [0.99.23] - 2026-04-06

A stability and polish release focused on probe handling, PID calibration UX, print status improvements, settings reorganization, and crash fixes across multiple subsystems.

### Added
- Smart PID calibration progress tracking with phase detection, ETA, and history persistence
- Timezone selection in display settings with IANA timezone support and UTC offsets
- G-code viewer progress/complete view toggle during prints
- Print Files button on print status widget to configure file picker toggles
- Centered thumbnail on print status widget when all action buttons are hidden

### Fixed
- Use-after-free in G-code streaming memory pressure callback (TOCTOU race, #733)
- Probe Z-offset not loading for FlashForge AD5M/AD5X with loadcell probes (#733)
- Probe discovery not seeding z_offset from config during initial setup
- Single-probe setups not auto-assigning the Z_PROBE role
- Ghost preview rendering too dark for short objects under 50mm
- Bed mesh probe clobbering existing default mesh when using temp profiles
- Probe accuracy test using wrong sample count and not pre-positioning to bed center
- Input shaper peak dot misaligned with graph, progress bar and table spacing issues
- Input shaper triggering disconnect dialog during SAVE_CONFIG restart
- Screws tilt adjust not auto-homing, unsanitized errors, small icons, button heights
- PID calibration progress bar not reflecting actual phase timing
- Nozzle temperature widget re-entrant drain crash replaced with lifetime invalidation (#732)
- Print status widget using positional child index instead of name-based lookup
- Timelapse webcam detection failures and missing retry option
- Camera probe crash and metadata re-fetching on AD5M (#724)
- Sound tracker playback disabled on AD5M/AD5X to prevent print kills
- IFS dirty flag not cleared on native ZMOD persist (#716)
- Multiple crash fixes: observer UAF, style cascade SIGSEGV, AFC null deref (#726, #728, #729, #731)
- Shutdown guards, deferred delete consolidation, and dynamic subject lifetime hardening
- Z-offset display not updating on main panel
- G-code viewer not activating when preview is set to thumbnail
- Chamber temperature not shown on unified temp card
- Screensaver option missing in micro layout

### Changed
- Settings reorganized into 6 category sub-panels
- Power Control renamed to Power Devices
- AMS Management renamed to Multi-Filament Management, card styling removed from rows
- Macro Buttons moved from Hardware to Printing settings
- AD5M/AD5M Pro split into ForgeX and stock printer database entries with macro_exclude heuristic
- Macros widget icon updated to match Advanced Settings

## [0.99.22] - 2026-04-05

### Added
- Artillery M1 Pro printer support with preset, print start profile, and platform hooks
- `--no-sound` flag and `disable_sound` setting to skip audio backend initialization
- Event-driven IFS re-read triggers using Adventurer5M.json instead of CHANGE_ZCOLOR macro
- Reset button in Widget Catalog overlay header
- 2 new translated strings across all languages

### Fixed
- SIGBUS crash in Moonraker health timer after long uptimes from destructor race (#717)
- Re-entrant rebuild crash in nozzle temperature widget (#723)
- IFS sensor re-read trigger narrowed to sensor changes only (was firing on unrelated events)
- Cancel/Reprint button visibility not syncing on panel activation (#546)
- File list not refreshing on reconnect and overlapping RPCs (#577)
- Invalid `flex_align` value in filament panel XML
- IFS dirty flag not cleared on color write failure
- Snapmaker U1 daemon directory and platform hooks deployment (#710)
- Concurrent `connect()` calls in test fixture causing flaky tests

### Changed
- IFS backend reads filament data from Adventurer5M.json via Moonraker HTTP instead of parsing GET_ZCOLOR output

## [0.99.21] - 2026-04-04

### Fixed
- Temperature keypad silently dropping commands when lifetime token expired during overlay hide
- IFS filament slot material/color reverting to stale values when editing multiple slots
- IFS material label not refreshing when only material changed (color unchanged)
- M300 beep command crashing backend from dangling MoonrakerClient pointer (#714)

### Changed
- AMS edit modal spool actions condensed into a split button dropdown to prevent overflow in translated UIs
- Modal button row now supports translation tags for primary/secondary buttons

## [0.99.20] - 2026-04-04

### Fixed
- Print file thumbnails showing placeholder icons instead of actual images
- Use-after-free crashes in async callbacks and observer guards (#704–#708)
- Background-thread lifetime callbacks using unsafe `this` pointer (#707)
- Deferred UI callbacks crashing when container layout is pending (#711)
- Sound settings not visible until hardware discovery completes
- Ghost taps after scrolling on capacitive touchscreens
- Snapmaker U1 installer referencing nonexistent init script
- Junk directories created in working directory from corrupted HOME environment
- CI release artifacts duplicated across platform prefixes

## [0.99.19] - 2026-04-03

### Added
- Chamber temperature control on the controls panel — set target, view status, and graph (community PR #688)
- Chamber temperature mini graph on the filament panel
- Filament eject icon with retract animation at slot sensor
- Safety warning automatically hidden when active spool material is known
- Runtime preset loading — FlashForge AD5M/Pro/5X presets applied automatically after detection
- ZMOD firmware auto-detection with firmware-specific preset support
- Nozzle and bed edit buttons now open the editor directly instead of the graph
- 8 new translated strings across all languages

### Fixed
- Crashes from observer use-after-free, event chain corruption, and DNS SIGSEGV (#697, #698, #700)
- Snapmaker U1 getcwd error and wrong init script path (#703)
- Sound sequencer stalling on thread-starved systems
- Touch calibration crosshair flash not appearing when animations disabled
- Modal dialog text clipping
- Screen artifacts on graceful shutdown (framebuffer not cleared)
- Print file card crashes from stale pool pointers and missing thumbnails
- AFC hub-routed lanes with per-lane extruders now correctly get PARALLEL topology
- IFS backend no longer incorrectly claims firmware spool persistence
- FlashForge AD5M/Pro presets split correctly; non-stock hardware removed
- Quick Actions header visible when macro row 2 active
- Ripple effect not rendering; settings reading calibration from wrong config path
- Duplicate crash reports in telemetry
- AD5M/AD5X preset naming updated for ForgeX and ZMOD firmware

## [0.99.18] - 2026-04-02

### Added
- Unified post-operation cooldown manager turns off extruder heater after filament operations complete (configurable delay, default 2 minutes)

### Fixed
- Touch not registering on AD5M/AD5X after upgrade — device-specific calibration removed from presets so each device calibrates during first-run wizard

## [0.99.17] - 2026-04-02

### Added
- Dynamic grid dimensions for ultrawide and portrait screen layouts
- 32 new HelixScreen feature tips with modal overflow fix
- Filament auto-preheat on load/unload with 2-minute delayed cooldown
- Snapmaker U1 default widget layout preset

### Fixed
- CFS filament swap now works correctly (M8200 param bug bypassed with direct CR_BOX commands)
- CFS active slot detection, partial update state preservation, and K1/K2 nozzle rendering
- Thumbnails now extracted from gcode headers on printers with old Moonraker lacking metascan
- Auto-home before filament load/unload/tool-change across all AMS backends
- Accurate layer tracking using virtual_sdcard.layer instead of linear estimation
- Temperature graph redraws throttled to 1Hz (was ~4Hz per series)
- Memory thresholds use actual available RAM for overlay lifecycle decisions
- Extruder selector rebuild deferred to prevent flex layout crash
- Favorite macro placeholder visibility and catalog ordering improved
- Filament path line gaps closed when backend has no prep sensors
- Non-rectangular 1x1 card backgrounds decomposed into maximal rectangles
- Hardware config synced with actual printer hardware
- Width sensor config loading and display values
- Boot persistence and WiFi for K1/K2 deploy targets
- Error icon token corrected in calibration panels
- AMS mini status widget now shows pressed visual feedback

### Changed
- Default display dim timeout bumped to 10 minutes, sleep to 20 minutes

## [0.99.16] - 2026-04-02

### Added
- Snapmaker U1 preset configuration with automatic wizard skip on detection
- Snapmaker U1 platform detection in installer with auto-start boot support
- `make dev` target for faster debug builds (-O0)

### Fixed
- Crash from unsafe DNS resolution on ARM and LVGL event chain corruption (#689, #690, #691)
- Abort detection now uses printer.info instead of gcode probe to correctly identify Kalico (#685)
- Snapmaker AMS backend not receiving status updates or populating slots
- Snapmaker AMS deadlock in status handler and incorrect extruder subscriptions
- Snapmaker active tool slot not marked as LOADED from toolhead.extruder
- Snapmaker active tool oscillation from incremental status updates
- Snapmaker filament slot using appended sub_type instead of base filament type
- Snapmaker filament color chips now parsed from Moonraker metadata and gcode headers
- Filament material matching uses compatibility groups instead of exact string comparison
- Card gradient backgrounds stretched to fill and rendered at exact dimensions
- Context menu item spacing increased for small screens
- IFS filament system now stores color/type natively without lessWaste/bambufy conversion
- Moonraker request timeout increased from 30s to 60s to prevent timeouts on slow networks
- IFS, CFS, and SnapSwap names displayed correctly in AMS wizard
- Snapmaker per-update debug logging removed (was causing UI freeze)
- Save Z-Offset button hidden for printers with auto-persisted z-offset
- AFC multi-unit ordering sorted by lane number instead of name (#554)
- Wizard step titles and subtitles now translated across all languages
- WiFi wpa_supplicant config persisted after connecting
- Printer name on home screen refreshes after editing in printer manager
- Redundant Power Devices entry removed from advanced panel
- TINY breakpoint (480x320) readability and AMS widget sizing improved
- Snapmaker S99screen recursion prevention and direct GUI restart

### Changed
- Z-offset GCODE_OFFSET strategy renamed to FIRMWARE_MANAGED
- Clog detection widget disabled by default
- Printer display name uses shared helper across UI

## [0.99.15] - 2026-04-01

### Added
- Snapmaker U1 DRM display backend with CRTC keepalive and persistent /userdata deployment

### Fixed
- Crash in temperature graph when parent object is invalid (#674)
- Config symlinks not restored after Moonraker web-type update
- Compressed fonts not rendering (LV_USE_FONT_COMPRESSED was disabled)
- Barcode scanner device ID read from wrong config source (#659)
- Chamber heater UI not showing when heater exists without a dedicated temperature sensor

## [0.99.14] - 2026-03-31

### Added
- Snapmaker U1 filament system support with RFID tag parsing and extruder state tracking
- Snapmaker U1 automatic detection via filament_detect printer object
- Filament macro detection with G-code fallbacks and parameter modal for manual load/unload
- Artillery Sidewinder X2 and Genius Pro added to printer database
- Spoolman fuzzy search with Levenshtein distance for typo-tolerant filament lookups
- Manual barcode scanner selection via USB vendor:product ID for devices not auto-detected

### Fixed
- Chamber heater UI not showing when heater exists without a dedicated temperature sensor
- Snapmaker U1 platform hooks corrected to use SysV init scripts instead of systemd
- Barcode scanners excluded from LVGL keyboard input to prevent ghost keypresses (#659)
- AMS material label not refreshing when slot color changes
- Docker cross-compilation portability for macOS/ARM (#649)
- Build dependency fixes for tools (#670)

### Changed
- CJK font files use RLE compression (~1MB savings)
- Source fonts excluded from release builds (~35MB savings)

## [0.99.13] - 2026-03-31

### Added
- Energy monitoring: power device widget with energy carousel, sensor picker, and mock data for testing
- Moonraker sensor discovery and subscription with SensorState singleton
- Fan rename via Settings panel and long-press on fan cards
- USB HID barcode scanner detection for any keyboard-class device

### Fixed
- External spool weight not syncing from Spoolman when no AMS backend is active
- Spoolman active spool notification handler parsing incomplete JSON-RPC messages
- Filament edit modal defaulting vendor dropdown to empty instead of Generic
- Double-free of libinput device paths on DRM teardown (#650)
- Boot display process not killed on K1 startup, causing stale boot logo (#642)
- libinput keyboard scan SIGABRT at startup on some devices (#648)
- Crash reporter stack-scanned backtrace for ARM32/MIPS plus race condition fixes
- Energy page layout tightened to avoid carousel dot overlap
- Fan touch feedback, event bubble for rename, output_pin prefix stripping
- WebSocket client missing close() call and missing ctime include

### Changed
- Power and power_device widgets consolidated into single two-column layout
- Request tracker decoupled from AbortManager and UI dependencies
- Inspector tool uses real request tracker, drops moonraker_client dependency

## [0.99.12] - 2026-03-30

This release adds fan management with output_pin support and RPM display, temperature graph overhaul via TempGraphController, touch calibration for DRM displays, K1 pre-print phase detection, and flexible nozzle temp widget layouts — alongside crash fixes, graph rendering improvements, and printer database additions.

### Added
- Fan management overlay in Settings with fan listing, type classification, and long-press rename
- Output_pin fan support with M106 P<index> control, fan_feedback RPM display, and Creality fan role detection
- Touch calibration support for DRM backend (#643)
- K1 series pre-print phase detection and progress display
- Nozzle temps widget now supports 1×1 and 2×1 layouts
- Printer name displayed on homescreen widget (#641)
- RH3D E3NG and Artillery M1 Pro added to printer database (#646)
- Exception message plumbed through crash reporter pipeline (#645)

### Fixed
- Temperature graph: consistent scaling across all contexts, correct 5-min and 20-min windows, gradient support at all sizes, target lines on mini graph
- Temperature graph: skip spurious initial rebuild on panel switch, limit history backfill to buffer capacity
- AFC spool assignment routed through AFC backend instead of bypassing it (#644)
- Print cancel RPC timeout increased to 5 minutes for large prints
- Fan role mapping corrected for K1/K1C, unconfigured fans detected, speaker override added
- Accelerometer detection uses AccelSensorManager instead of raw objects list
- GCode viewport scaling uses extrusion-only bounding box
- Smart home button with persistent overlays, printer image aspect ratio fix (#607)
- Touch press-on-capture fix for ns2009 calibration on Ender 3 V3 KE
- Guard against null callback in MoonrakerClient::connect (#639)
- Guard lv_obj_delete_async_cb against use-after-free (#638)
- Control/Filament buttons disabled on startup when Klipper not running (#640)
- IFS: removed unsupported SHOW=0 param, seed Moonraker DB on first load
- ACE: subscribe to ace Klipper object for realtime filament updates
- Friendly error message for accelerometer SPI communication failures
- Graph time axis uses POSIX strftime %I for musl compatibility
- M141 fan role hint corrected from Chamber Circulation to Chamber Exhaust
- Fan rename modal stripped down to fix MIPS SEGV crash at startup

### Changed
- Temperature graph internals refactored to TempGraphController for unified state management
- TempControlPanel renamed to TemperatureService
- Framebuffer cleared when crash report dialog closes
- HELIX_DEBUG_TOUCH and HELIX_DEBUG_TOUCHES environment variables unified

## [0.99.11] - 2026-03-30

This release adds QR scanner improvements and probe accuracy UX, alongside performance enhancements for Spoolman and camera handling, plus fixes for modal stale callbacks, temperature graph rendering, and multiple crash fixes.

### Added
- QR scanner snapshot fallback with local camera auto-discovery at startup
- Auto-lower bed to 150mm on moving-bed printers when opening QR scanner
- Probe accuracy test progress UX with live sample readout and quality assessment
- Color picker for spool edit modal
- Spoolman spool details bridge button in AMS edit modal
- Commanded and actual Z position stacking on position card
- Persistent disk cache for printer image at exact widget dimensions

### Fixed
- QR scanner overlay now renders above modals instead of beneath them
- QR scanner crash on overlay teardown and use-after-free from frame buffer cleanup
- QR decode crash and incorrect callback ordering during result handling
- Auto-save spool data directly from QR scan to prevent data loss
- Stale on_hide callbacks clearing active modal instance after re-show
- Overlay stack close callbacks causing SIGSEGV from synchronous subject observer invocation
- Temperature graph gradient rendering: uniform opacity, correct orientation, proper clipping
- Temperature graph background color mismatch on filament panel mini graph
- JSON error object parsing in motion panel; duplicate raw error toasts suppressed
- Spoolman 'method not found' toast when Spoolman not configured
- Spoolman list performance regression from repeated widget lookups on scroll
- Camera probe using HTTP GET instead of HEAD (mjpg_streamer rejects HEAD)
- Display animations defaulting to enabled on platforms without support
- Compressed .bin source images not detected by cache handler
- USB source tab button styling with proper contrast via declarative XML bindings
- USB button incorrectly hidden when Moonraker returns empty file list
- Filament spool click target dead zones eliminated
- Spoolman settings row always visible so users can configure it

### Changed
- Camera decoding at display resolution instead of full frame for better performance

## [0.99.10] - 2026-03-29

This release adds spool management enhancements — direct weight editing, tool remapping with dropdown, and remaining filament display — alongside fixes for print status not refreshing after navigation, multiple crash fixes, and expanded platform and internationalization support.

### Added
- Direct filament weight editing and remaining weight display in spool edit modal (#629)
- Tool dropdown in spool edit modal for remapping filament to tools (#630)
- Tool labels (T0, T1, etc.) shown in filament mapping card (#554)
- Warning-color tool badge when user has overridden the default tool mapping
- Touch calibration debug logging via HELIX_DEBUG_TOUCH environment variable
- AD5X IFS (Infinite Filament System) support with bambufy macro compatibility

### Fixed
- Print preview not showing after switching navbar tabs (#632, #633)
- Async deletion double-free crash from overlapping parent/child lv_obj_delete_async calls (#632)
- Use-after-free crash when camera stream thread is detached (#624)
- Use-after-free crash on shutdown from client destroyed after API/macro managers (#628)
- Spurious service restart after self-update and NTP clock sync (#536)
- Slot editing now works on CFS and ACE backends with persistent overrides
- AFC partial status updates no longer regress slot loaded state (#631)
- Dryer temperature limits enforced in UI; ACE max corrected to 55°C
- Humidity UI hidden for backends without humidity sensors (ACE)
- Stale print data persisting after print complete/cancel (#546)
- Temperature chart gradient fills restored; overlay lifecycle fixed (#616)
- Chamber icon corrected to fridge_industrial to match other panels
- Temperature labels use icons at small breakpoints to prevent clipping
- K1/K1C SSL enabled for update server connectivity
- Self-update no longer double-starts via path watcher sentinel

### Changed
- 190 new strings translated across 8 languages with regenerated CJK fonts
- Splash screen logo PNGs now include alpha transparency

## [0.99.9] - 2026-03-28

This release focuses on stability with fixes for multiple crashes, adds manual chamber sensor/heater assignment, and improves print history performance with virtual scrolling.

### Added
- Manual chamber sensor and heater assignment in temperature sensor settings
- USB gcode thumbnail extraction with Creality PNG format support (#610)
- Virtual scroll for print history list, improving performance with large job histories (#619)
- Sound settings promoted out of beta with tracker test button
- ACE environment sensor data (temperature, humidity) now exposed

### Fixed
- Crash from backdrop deletion during AMS notification dispatch (#620, #621)
- Crash from synchronous widget deletion in picker and busy overlay dismiss
- Crash from unguarded async gcode callbacks in bed mesh calibration (#611)
- Crash from null subject API calls during startup or reconnection (#617)
- Crash from PowerDeviceState observer firing before PrinterState initialized
- ETA display now respects 12/24-hour time format setting (#597)
- Touchscreen: NS2009 detection fixed, affine calibration disabled during recalibration (#623)
- Filament load/unload now routes through AMS backend when active
- Print history fetches all jobs instead of only the first 500 (#619)
- Print history lifetime stats now use server totals for accuracy (#619)
- Temperature chart sizing and double-deactivate in temp graph overlay (#616)
- Print status memory-aware widget caching and correct temp row height (#617, #618)
- Systemd unit self-healing and removal of unsupported Moonraker options (#617)
- NTP time correction no longer triggers unnecessary service restart (#536)
- Boot race on Plymouth systems resolved by waiting for multi-user.target (#536)
- Filament type JSON array normalization from Moonraker (#554)
- USB file browser: K1C /tmp/udisk mount detection, source selector shown on startup (#610)
- Motion panel: out-of-range errors now show Klipper axis limits (#610)
- Duplicate T0 extruder entry in multi-tool initialization

## [0.99.8] - 2026-03-28

This release enriches the print status panel with speed/flow indicators, estimated finish time, Z height, and a unified temperature card with chamber support. It also fixes several crashes and UI issues across filament management, temperature controls, and navigation.

### Added
- Print status: speed and flow rate indicators visible on medium and larger screens (#597)
- Print status: estimated finish time ETA in metadata overlay (#597)
- Print status: Z height shown alongside layer progress (#597)
- Print status: unified temperature card with chamber temp support (#597)

### Fixed
- Nozzle temps widget: color-coded temperature display replaces progress bars; compact font and bed icon on small screens
- Speed/flow row on print status panel is now clickable to open the Tune overlay (#597)
- Spoolman error toasts no longer appear when Spoolman is not configured (#609)
- Bed mesh probe modal now shows emergency stop instead of cancel
- Power widgets now work when Moonraker is connected but Klipper is not running (#587)
- File list always refreshes when returning to print select panel (#577)
- AMS slot crash from dangling widget pointers during deferred deletion (#604, #579)
- Overlay state not clearing when switching navbar tabs (#607)
- Material temperature save button hidden when preheat macro was selected (#588)
- Temperature chart uses deci-degrees for smoother lines (#600)
- Z offset baby stepping: added MOVE=1, clamped to ±2mm, fixed float rounding (#592)

## [0.99.7] - 2026-03-27

### Added
- Bed mesh calibration now shows determinate progress by querying the configured probe count

### Fixed
- Bed mesh progress bar could get stuck; suppress spurious disconnect toast on profile save
- Spoolman active spool now auto-assigns to the active tool on tool changers (#543)
- AMS slot and path updates deferred to prevent race conditions during hardware discovery (#562, #563)
- About page marquee scroll on wide screens
- Installer self-update on SonicPad (kernel 4.9) when sudo is unavailable
- Startup restart loop on systems with Plymouth boot splash (Armbian, Raspberry Pi OS) (#536)

## [0.99.6] - 2026-03-27

This release adds ACE filament system support, overhauls the filament mapping UI with an inline slot picker and smarter color matching, and continues hardening async callback safety across the UI.

### Added
- ACE filament system support: auto-detection, WebSocket subscription with REST fallback, feed/retract/feed assist device actions, and missing bridge warning
- Inline slot picker context menu replacing the separate picker modal (#554)
- Material-aware color matching with positional fallback when no color match is found
- Warning indicators for tools mapped to empty slots or with material mismatches
- Auto color map toggle in the filament mapping modal
- Gcode material label shown on filament mapping tool rows
- Multi-unit AFC slot label disambiguation

### Fixed
- Deferred object cleanup in AMS panel and exclude objects overlay to prevent crashes (#555)
- Async callback safety migrations for controls panel, Z-offset calibration, camera stream, print status, and screensaver (#550, #552, #553, #555)
- Scrollbar on mapping and picker dialogs when content overflows
- Wizard skip button blocked by touch overlay, stale callbacks, and sample counter off-by-one
- Incorrect theme token, invalid flex stretch, and splash screen timeout

### Changed
- ValgACE renamed to ACE throughout codebase and translations
- Filament mapper allows slot re-use across backends instead of enforcing uniqueness
- QR scanner latency reduced via frame subsampling and offloaded decode
- Updated CJK fonts and added do-not-translate markers for technical terms

## [0.99.5] - 2026-03-27

This release introduces AsyncLifetimeGuard — a unified mechanism for safe async callback handling that replaces all ad-hoc guard patterns — and migrates every modal, overlay, panel, widget, backend, and state manager to use it. Memory usage on constrained devices is further reduced through compile-time feature gates and runtime optimizations.

### Added
- AsyncLifetimeGuard for unified async callback safety, integrated into Modal and OverlayBase base classes
- Compile-time feature gates (CFS, IFS, label printer) to reduce binary size on constrained devices (#546)
- Anet ET5 Pro printer detection

### Fixed
- Crash from gcode renderer use-after-free when destroyed after streaming controller
- DebugBundleModal upload callback not marshalled to UI thread
- Premature restart during update when release_info.json not preserved during atomic swap (#547)
- RGB565 spool canvas breaking transparency on devices with alpha blending

### Changed
- All async callback handling migrated from ad-hoc guard patterns to AsyncLifetimeGuard
- Build hardened with FORTIFY_SOURCE, stack protector, and frame pointers across all platforms
- Reduced memory on constrained devices: smaller canvas sizes, lighter sound theme, tighter gcode cache

## [0.99.4] - 2026-03-26

### Added
- ALSA sound backend with 4-voice polyphony and MOD/MED tracker music playback
- Active Spool widget showing current filament via Spoolman integration (#545)
- Open source licenses section on the About page
- Auto-detect tape width for Brother PT label printers

### Fixed
- Crash from object deletion during LVGL event processing (#543)
- Stale print outcome not clearing when starting a new print after complete/cancel (#546)
- K1C: stabilized time estimates, fixed layer count, added arc support, improved pre-print status
- K2 platform misidentified as AD5M during installation (#544)
- ui_button user_data collisions causing crashes in temperature presets, macros, modal buttons, and print status controls
- Stale modal stack entries when animations are disabled
- Brother PT label printing: single RFCOMM connection, QR code clipping on narrow tape, rotation, auto-cut, dropdown corruption, and reconnect timing
- Die-cut label layout too wide for 38mm tape
- Print status buttons not anchored to bottom of controls column
- Notification badge positioned incorrectly on content-sized containers

### Changed
- Display rotation probe moved from DisplayManager init to Application startup

## [0.99.3] - 2026-03-26

### Added
- Brother PT (P-Touch) label printer support via Bluetooth with auto-detection, PTCBP raster protocol, status feedback, and PackBits compression
- Startup sounds for UI themes

### Fixed
- 5 auto-reported crash bugs with defensive guards (#525+)
- Emergency stop modal validation and toolchanger spool persistence (#540)
- SSH (dropbear) preserved when disabling stock UI on Creality K1 (#535)
- Hidden network WiFi connection and password validation for secured networks
- Config loss during in-app upgrade with backup restore and corruption recovery
- Update service template surviving Moonraker extraction

### Changed
- Print abort timer replaced with RAII lifecycle wrapper
- Cleanup guards added to unguarded queue_update callbacks

## [0.99.2] - 2026-03-25

### Added
- Per-unit environment display and dryer controls for AMS
- Hardware-gated widgets shown as disabled instead of hidden
- x86_64 platform detection and binary validation in installer

### Fixed
- Crash on RGB565 embedded builds from blur tree walk (#528)
- Crash from stale parent pointer in modal dialogs (#522, #523, #524)
- Crash from use-after-free in action prompt modal (#514, #515, #521)
- Crash from PrintStatusWidget use-after-free during macro timeout (#522)
- Brother QL label printing: raster alignment, die-cut support, auto-detection, and async Bluetooth
- Print metadata not fetched when thumbnail was already set (#526)
- Debug bundle upload spinner and text alignment

## [0.99.1] - 2026-03-25

### Added
- Creality K1 and K2 toolhead rendering with auto-detection and settings dropdown
- CFS device actions: refresh, auto-refill toggle, and nozzle clean
- Turbo jog mode with 10mm and 50mm step sizes on motion panel
- Easier carousel swiping and smart home button on home panel
- Telemetry tracking for home panel widget placement and interactions

### Fixed
- Crash from fan widget use-after-free during carousel rebuild (#517, #518, #519, #520)
- Crash from fan picker backdrop deletion corrupting LVGL event list
- Screensaver UI bleed-through from panel lifecycle not being suspended
- Home carousel page tracking using wrong observer type
- Dashboard Y-axis labels cut off; missing CFS/IFS AMS type entries
- Toolhead heat glow drawn behind toolhead body; AntHead glow position corrected
- K2 renderer heat block visible through U-cutout
- Tool changer spool assignments not loaded on startup
- Telemetry hardware profile recorded before build volume was available
- Toolhead settings dropdown not respecting test mode for debug options

### Changed
- Toolhead dropdown separated into native vs aftermarket styles
- Dashboard utilities extracted into shared module

## [0.99.0] - 2026-03-24

A major release bringing multi-page home screen, exclude object map, temperature graph widget, Creality K2/CFS support, preheat macros, tool changer improvements, and a settings.json rename — across 100+ commits.

### Added
- Multi-page home screen with carousel navigation, per-page widget layout, and page deletion (#484)
- Exclude object overhead map view on print status panel with convex hull outlines and touch-to-exclude (#511)
- Temperature graph dashboard widget with sensor config modal, color picker, and adaptive sizing
- Creality K2 Max support: printer definition, preset, platform hooks, and deploy targets
- Creality Filament System (CFS) backend with RFID material database, slot addressing, and status parsing
- Tool changer improvements: tool switcher widget, nozzle temps widget, preheat all tools (#493)
- Preheat custom macro support with per-material macro picker, toggle, and cool down button (#486)
- Fine/Coarse jog toggle on motion panel (#505)
- Optional text labels on icon-only home screen widgets (#501)
- First-class prtouch_v2 z-offset calibration for K1/K1C/K1 Max
- Probe accuracy test results displayed in formatted modal
- x86_64 Debian release target for x86 SBCs
- Creality K1, K1 Max, and K1C linked to k1 preset
- 74 new translated strings across all 8 languages to 100% coverage
- IPP sheet label printing for standard inkjet/laser printers

### Fixed
- Print status thumbnail not visible on first navigation in thumbnail-only mode
- Crash from camera stream callback race and LRU cache splice (#491)
- Crash from macros panel param modal at startup with null screen (#491)
- Crash from null pointer in home panel arrow event callbacks on non-SDL displays
- Crash from wifi wizard use-after-free on network item click
- Crash from null guards in LVGL object deletion path (#511)
- AFC error state not clearing on modal dismiss (#497)
- Bed mesh auto-home before calibration with sanitized error messages
- K1C probe using wrong calibration method (now uses standard probe_calibrate)
- Power disconnect dialog suppressed for all power-off events (#469)
- Service double-restart from pending update watcher on self-update (#509)
- Dangling symlink install error (#496)
- K2 display rotation set to 270° in preset
- Crash from fan widget animation teardown race condition
- CFS slot parsing, subscription, and load/unload operations now use M8200 protocol directly

### Changed
- Config file renamed from `helixconfig.json` to `settings.json` with automatic migration
- Controls panel reorganized: QGL/Z-Tilt moved to Calibration card, enlarged quick action buttons with slots 3 & 4
- Spoolman layout compacted with side-by-side sync toggle and interval
- Dropdown styling improved with responsive item spacing and clipped corners
- Thermistor widget renamed to Temperature Sensors
- Persistently disable stock Creality UI on K1 devices (#495)
- Print status buttons converted to outline style

## [0.98.12] - 2026-03-23

### Fixed
- Systemctl shell syntax in service restart wrapper (#495)

## [0.98.11] - 2026-03-23

### Added
- Camera rotation and flip configuration with edit mode modal (#483)
- Multi-instance thermistor and fan dashboard widgets with icon picker (#342)
- Thermometer variant icons added to icon font (#342)
- `HELIX_SCREEN_SIZE` environment variable as alternative to `-s` flag

### Fixed
- Crash from overlay close callback using synchronous deletion (#491)
- Crash from null style pointer dereferences (#490, #439, #480)
- Crash from blur walk callback and bed mesh layout update (#417, #419, #420)
- Print status auto-navigation blocking the UI thread (#450)
- EGL rotation fallback now continues unrotated with user guidance instead of crashing (#457)
- EGL rotation working on GL renderer path (#457)
- AFC tool-slot reconciliation using registry and merging stepper lanes in mixed setups (#421)
- Update watcher paused during startup to prevent spurious restart (#470)
- Self-restart via fork+exec when no watchdog supervises the process
- Sensor list and power picker card scrollable so content is not cut off (#342, #467)
- AmsState subjects initialized before testing sync_active_spool_after_edit

## [0.98.10] - 2026-03-22

A stability-focused release addressing over 30 crash reports, plus a new power device dashboard widget, multi-instance widget system, and fixes for power device detection, config symlinks, and memory management.

### Added
- Power device dashboard widget with live on/off state, device picker, and icon customization (#467)
- Multi-instance widget system: mint, delete, and rearrange multiple copies of the same widget (#342)
- Favorite macro widget migrated to multi-instance system (#342)
- Power-related MDI icons added to icon font (#467)
- Hardware name telemetry for improved printer detection analytics

### Fixed
- Multiple crash-causing animation callbacks, null dereferences, threading violations, and delete-during-iteration (#442–#482)
- Crash handler using async-signal-unsafe `memset` (#441, #445, #447, #481)
- Cache loader crash from dangling reference and unguarded layer load (#446)
- Power device names not parsed from Moonraker array response (#466, #469)
- Disconnect triggered when turning off power devices (#469)
- Config save breaking symlinks for helixconfig.json (#471)
- Shutdown crash from JobQueueState destruction ordering
- Shutdown crash from held SubjectLifetime in power device widget (#467)
- Memory monitor improved with hysteresis, smoothed growth tracking, and pressure responders
- Debug bundle warnings silenced, printer database compacted for memory savings
- Fan arc widget not applying track width when arc already at target size

## [0.98.9] - 2026-03-17

### Fixed
- Multiple crash-causing synchronous object deletions inside UpdateQueue timer callbacks (#422, #423, #429, #430, #435, #436, #437)
- Overlay animation crash when panel is freed mid-animation (#428, #436, #439, #440)
- Bluetooth thread capturing loader reference that could dangle after thread detach
- GCode viewer using unsafe custom async deletion instead of LVGL's built-in cancellable delete
- Screws tilt adjust panel not initializing API before lazy panel creation (#402)
- AD5X backlight staying on during sleep mode (#431)

## [0.98.8] - 2026-03-15

### Added
- Crash reporter now includes stack dumps, extended ARM32 registers, and full memory maps
- Crash analysis worker displays stack scanning results and memory map details

### Fixed
- Temperature keypad showing raw centidegree values instead of degrees (#401 related)
- Temperature validation falsely flagging valid temperatures as out-of-range
- Heater display dividing by 100 instead of 10 for centidegree conversion
- Config files in Fluidd/Mainsail not editable due to Moonraker reserved path conflict (#401)
- Screensaver async deletion crash and arc subject null guard (#409, #410, #411, #412)
- XML style constants triggering LVGL warnings from incorrect hex color prefix
- Bluetooth plugin missing from Pi release packages
- C++17 structured binding capture incompatibility with GCC 11

### Changed
- Animations disabled on K1, AD5M, and AD5X platforms for improved performance

## [0.98.7] - 2026-03-14

### Added
- Spoolman server setup: configure, change, or remove Spoolman directly from Settings
- MakeID/Wewin BLE label printer support (beta)
- QR scanner flash-bulb effect on successful recognition
- Beacon onboard accelerometer detection for input shaper
- Configurable sections in print status widget
- K1 platform preset with PRINT_PREPARED pre-start gcode
- HTTP fallback installer for no-SSL environments (K1/AD5M)
- K1C and K1 SE printer images

### Fixed
- First-boot rotation probe leaving overlays at half-width until restart
- QR scanner use-after-free crash on successful scan
- Overlay use-after-hide crash from stale alive guard
- Buffer meter destructor crash from dangling on_draw callback (#400)
- KIAUH installer PermissionError when probing install paths (#403)
- Installer failing to stop K1 stock Creality UI during setup
- Installer missing trailing newline when appending to moonraker.asvc (#408)
- LED wizard not saving selection to selected_strips config
- Docs URL pointing to non-existent docs.helixscreen.org

## [0.98.6] - 2026-03-13

### Added
- Bluetooth Low Energy (BLE) read support for Niimbot label printers, fixing D110 printing
- Creality K1C setup guide for installation documentation

### Fixed
- Modal double-free crash from synchronous deletion during LVGL event processing (#399)
- Wayland CSD title bar eating SDL content area
- AMS panel layout with full-height sidebar and text overflow
- Print status widget spacing and hidden icons at 2x2 breakpoint
- Theme engine crash when parsing empty palette for single-mode themes
- Belt tension mock CSV download and results layout
- Info QR modal icon alignment on small breakpoints
- Muted text color lost in button auto-contrast and widget label visibility
- Niimbot protocol packet sequence and blank row handling for D110 compatibility
- Slot registry not clearing old slot when remapping a tool
- Test runner silently swallowing shard failures due to pipeline exit code bug

### Changed
- AMS and micro header titles use uppercase text transform (i18n-safe)
- Midnight dark theme added to theme selection
- Theme engine supports handle_style/handle_color properties and transparent button variant

## [0.98.5] - 2026-03-13

### Added
- Belt tension tuning panel with resonant frequency measurement, strobe fine-tuning, and hardware auto-detection [BETA]
- Belt tension tuning guide in user documentation

### Fixed
- Print cancel now uses `printer.print.cancel` RPC instead of raw CANCEL_PRINT gcode
- Belt tension panel crash from missing JSON `result` wrapper, incorrect icon name, and missing translations
- False mouse cursor appearing on Allwinner SoCs due to MCE IR receiver detected as USB mouse
- Header bar back buttons missing pressed opacity feedback
- Controls panel missing translations for Nozzle, Off, Cooling, fan speeds, macro names, and sensor count
- Content-sized metadata overlays and non-responsive filament mapping pills
- Startup touch grace period reduced from 5s to 1s
- Installer now handles turbojpeg package name differences across distros

### Changed
- Belt tension help text uses progressive disclosure (help icon modals) to reduce scrolling
- CJK fonts regenerated with 1073 characters covering new translation strings
- Translation sync improved: 60 missing keys translated, backfill and coverage truncation fixed

## [0.98.4] - 2026-03-12

Timelapse video browser, USB mouse support, runtime CJK fonts, and filament system improvements.

### Added
- Timelapse video browser with thumbnail grid, render progress tracking, and video playback
- USB mouse support for DRM and fbdev backends with sysfs capability scanning
- Runtime CJK font loading — wizard codepoints compiled in, full character sets loaded on demand
- AntHead toolhead renderer for PrintersForAnts printers
- Library card on home panel with Print Files, Print Last, and Recent actions
- Compact locale-aware date formatting utility

### Fixed
- HDMI CEC devices falsely matched as mouse input, causing spurious pointer events
- AMS ZMOD IFS detection without lessWaste plugin installed
- AFC UNSELECT_TOOL sent invalid TOOL= parameter; added extruder LED support
- Timelapse empty state centering and missing icons
- Icon codepoint sort order in font generation
- Print file detail header bar layout on narrow breakpoints
- Timelapse switch background transparency on options card
- Crash from deferred MacrosPanel rebuild and unguarded CameraStream callbacks
- Duplicate linker symbols for input device scanner in Pi fbdev builds
- Print thumbnails showing grey box and uploaded files not appearing

### Changed
- Timelapse graduated from beta — feature gates removed
- AMS overview and detail headers unified with context-aware back button
- GitHub Actions upgraded from Node 20 to Node 24

## [0.98.3] - 2026-03-12

Macro browser, toolhead renderer, touch and filament fixes, and internationalization improvements.

### Added
- Macro browser panel with description display, one-tap execution with toast feedback, and chevron hidden for no-parameter macros
- JabberWocky V80 toolhead renderer
- Client-side crash report deduplication to prevent repeated uploads
- Default display brightness raised to 80% with automatic migration for existing users
- USB keyboard support for DRM and fbdev backends on Pi builds

### Fixed
- AFC direct_load hub field treated as direct instead of hub-routed (#392, #364)
- Tool changer uses Tn gcode for tool changes, fixing load/unload/purge on non-T0 tools
- IFS real-time sensor updates, stuck operations, and unload homing
- Touch calibration auto-detects and corrects swapped touch axes
- Display wakes on touch during dim/screensaver
- Crash from pending layout flush before widget tree rebuild
- Crash from safe_delete loop in MacrosPanel (#394)
- Post-update restart no longer shows crash report modal or sends crash telemetry

### Changed
- Advanced panel sections reordered by usage frequency, emergency bar pinned at bottom
- Color mismatch dialog shows color names instead of hex codes
- Wizard and macro enhance strings wrapped for internationalization (20 new translation keys across 8 languages)

## [0.98.2] - 2026-03-12

### Fixed
- QR scanner now shows live camera feed with proper mirror flip and visible close button with 60-second auto-timeout
- DRM display prefers PRIMARY plane over OVERLAY and suppresses fbcon bleed-through (#334)
- Bed mesh calibration uses StandardMacros for command instead of hardcoding
- Bed mesh no longer starts async render thread when no mesh data is loaded
- Clog meter and fan carousel widget spacing tightened (#331)
- macOS build uses pkg-config-compatible libusb include path

## [0.98.1] - 2026-03-11

Lock screen, probe management, QR spool scanning, Happy Hare device actions, and HTLF mixed topology support.

### Added
- Lock screen with PIN entry, auto-lock on wake, and Security settings for PIN management
- QR spool scanner for Spoolman spool assignment from the filament panel
- Probe management with Cartographer, Beacon, and Generic/Klicky type-specific panels, Z-offset calibration, and live config editing
- Happy Hare device actions: LED, eSpooler, and flowguard control with topology filtering, persistence across reconnects
- AMS HTLF mixed topology rendering with direct+hub lane paths, bezier curves, and hub indicator
- Input shaper Kalico smooth shaper support for calibration
- Bluetooth setup guide for Raspberry Pi and BTT Pi

### Fixed
- Crash from ghost thread when switching gcode/streaming controller (#387)
- Crash from CameraStream shared_ptr race and ToastManager observer corruption
- Crash from telemetry auto-send timer firing when telemetry disabled (#380)
- Crash from animation on modal backdrop deletion with stale dialog pointer
- Crash from missing CA certificates on AD5M breaking all HTTPS connections
- Home panel SIGSEGV during overlay push from null active_widgets_ iteration (#362)
- Config migration for printer switcher default on v5 fresh installs
- Config wizard not re-running after restoring from backup

### Changed
- Probe management graduated from beta
- Cherry-picked 4 upstream LVGL fixes from post-9.5.0 master
- Symbol files now attached to GitHub releases for permanent retention
- Crash symbolication tool supports AD5M, AD5X, and CC1 platforms

## [0.98.0] - 2026-03-10

Bluetooth label printing, filament mapping, Spoolman location support, and new home panel widgets.

### Added
- Bluetooth label printing with Brother QL (RFCOMM), Phomemo (SPP), and Niimbot (BLE GATT) support
  - BlueZ D-Bus device discovery and pairing UI in label printer settings
  - Bluetooth plugin loaded dynamically via dlopen for optional dependency
- Filament mapping modal for print-time tool-to-spool remapping
- Spoolman location field — filter by location, edit in spool modal, shown inline in list rows
- Bed temperature home panel widget (1x1, scalable to 2x2)
- AD5X IFS backend for FlashForge Adventurer 5X Intelligent Filament Switching
  - 4-lane switching with tool mapping, color/material/presence tracking, bypass mode
  - Mock mode: `HELIX_MOCK_AMS=ifs`
- Venture Delta printer to database
- Label printing user guide for Brother, Phomemo, and Niimbot printers

### Fixed
- AD5X heuristics strengthened to prevent AD5M misdetection (#375)
- Screws tilt panel always shows results table with success banner (#309)
- LED strip list no longer lost when strips are pruned during discovery (#373)
- LVGL observer null-guard prevents crash on subject removal (#378)
- AFC lane tracking on print start with toolchanger tool_number parse (#379)
- AMS spoolman_actions visibility race on modal open (#311)
- Screensaver activation on devices without backlight dimming
- macOS build using pkg-config for libusb

### Changed
- Home panel widget guide updated with all current widgets

## [0.97.5] - 2026-03-10

AFC device configuration improvements with toolhead distance editing and multi-extruder support.

### Added
- AFC Toolhead section with editable extruder distance actions and live extruder overlays
- Multi-extruder toolhead actions with per-lane dist_hub configuration
- Editable numeric text inputs replacing slider value labels in AMS device sections
- Creality Sonic Pad to supported platforms

### Fixed
- AFC HTLF lanes grouped by physical extruder for correct nozzle count (#364)
- AFC textarea jumping, corrected purge/wipe config sources, removed dead config section
- AFC mutex added to get_device_sections to prevent race condition
- AMS section rows always navigating to Setup due to wrong event target

## [0.97.4] - 2026-03-09

### Added
- USB label printer support (Phomemo M110) with auto-detection via libusb
- Split button widget with primary action and dropdown selector
- Preheat home panel widget
- Duplicate option in Spoolman spool context menu
- udev rule for USB label printer device access

### Fixed
- SIGSEGV from synchronous object deletion in timer callbacks (#367)
- SIGSEGV in BufferStatusModal destruction during show (#366)
- Null libusb handle dereference on AD5M platform (#368)
- AMS edit dropdown defaults not syncing for empty slots
- AMS edit modal width too narrow on smaller screens
- Label printer icon rendering, dropdown alignment, and spool edit layout
- A4T toolhead drawing too small on AMS canvas
- Printer config validation accepting non-printer object keys
- WiFi polkit rule priority and permission error detection

### Changed
- MIPS crash reports now include register extraction, stack dumps, and unwind tables (#365)
- In-app self-updates use atomic directory swap for reliability
- Translation sync with 15 new keys

## [0.97.3] - 2026-03-08

Label printing, Spoolman improvements, and startup performance.

### Added
- Print labels directly from the spool edit modal via connected label printers
- mDNS auto-discovery for label printers in Spoolman integration
- Improved label printer presets with richer Standard layout and test print
- Pass downloaded config content through to print start analysis

### Fixed
- WiFi polkit permission errors now detected and displayed correctly, including "insufficient privilege" variant
- PARALLEL AFC lane count inflated by cross-unit remap (#363)
- Null dereference in home panel during widget drain (#362)
- Null static widget pointers during multi-printer switch teardown
- Active spool not synced when editing the currently loaded AFC slot
- Checkbox checkmark invisible when accent color matched background
- WebSocket SIGSEGV in onclose callback during destruction (#357)
- Active spool not synced to filament panel after Spoolman edits
- Stale LED strip names persisted after hardware discovery (#360)
- Startup race between PrintPreparationManager and MacroModificationManager
- Label printer raster mode and horizontal flip issues

### Changed
- WebSocket connection now starts during splash screen for faster startup
- Panel widget rebuilds skipped when widget list is unchanged
- Full translation sync with 100% coverage
- Installer re-bundles polkit restart for immediate rule activation

## [0.97.2] - 2026-03-08

### Added
- Decimal point button and Android-style bottom row layout in numeric keypad
- Actual speed and flow rate display in tuning overlay

### Fixed
- Crash from object deletion in UpdateQueue callbacks; shared library addresses now detected in crash reports (#355, #356)
- Shutdown hang from DRM page-flip poll blocking indefinitely (#334)
- Stale LED macro entries persisted after strip deletion or config reload (#329)
- Slider and arc knob clipping in fan and PID menus (#331)
- Self-update restart loop caused by stale PathExists systemd watcher
- Missing polkit rule not detected or auto-created during self-update
- Polkit auto-creation failing on systems using .pkla format instead of .rules
- Crash worker extracting metadata from wrong debug bundle JSON keys

### Changed
- Printer switcher toggle moved from settings to Manage Printers overlay
- Sensors overlay headers updated to use consistent section header style with status badges

## [0.97.1] - 2026-03-07

### Added
- Callback tagging in UpdateQueue for crash diagnostics (#345)
- Filament system data collection (AFC, MMU, Spoolman) in debug bundles
- Toggle to show/hide printer switcher in navbar

### Fixed
- Modal use-after-free, label buffer overflow, and missing unwind tables in crash handler
- Input shaper chart and table data missing in dual-axis calibration (#341)
- LED macros with empty display names not rejected (#329)
- BGR framebuffer color swap not auto-detected on some displays (#344)
- AFC Vivid stepper motors incorrectly counted as lanes
- AFC toolchanger+hub setups showing wrong topology and tool labels
- Printer switcher toggle always visible instead of respecting settings
- Polkit permission errors not reported in WiFi NetworkManager backend
- Crash report modal layout broken when no network available for QR display
- Empty spool list when opening Spoolman picker from Extruder tab (#311)
- Bed screw calibration results never showing (#309)
- AFC/AMS reactive lane highlighting and current_slot stability (#336)
- A4T toolhead render size too small

## [0.97.0] - 2026-03-06

Multi-printer support (beta) and selectable toolhead renderers. Configure multiple printers in a single HelixScreen instance and switch between them from the navbar or settings. The A4T toolhead style is now available alongside the default and Stealthburner renderers.

### Added
- Multi-printer support: configure, switch, and manage multiple Klipper printers from one device (beta feature flag)
- Printer switcher badge in navbar with connection status indicator
- Printer management overlay for adding, switching, and deleting printers
- Toolhead style selector in display settings (Auto, Default, Stealthburner, A4T)
- A4T toolhead renderer with SVG-traced polygon geometry

### Fixed
- Self-update crash from CWD deletion causing SIGABRT on re-exec
- In-app updates not offered on non-Klipper systemd installs
- Config loss on Moonraker-triggered updates for K1/SysV devices
- Cancel button not working in add-printer wizard flow
- Null JSON values in config causing crashes instead of returning defaults
- AFC filament loaded state detection returning false negatives
- Abort dialog ignoring cancel_escalation_enabled setting

## [0.96.9] - 2026-03-06

### Fixed
- Camera snapshot polling use-after-free crash after thread detach
- Binary GPIO backlight truncating brightness to OFF on wake (#326)
- LED effects stopping on all strips when toggling a single strip (#329)
- LED macro entries persisted as empty on Add (#330)
- Input shaper results modal not dismissed before Klipper restart (#328)
- Modal button focus stealing in host power dialog (#333)
- About screen logo not registered and excess padding (#333)
- Info QR modal OK button not wired (#332)
- Buffer status modal cancel button not wired (#323)
- Power panel using inconsistent overlay layout
- Discovery failure toasts shown during startup on slow devices
- Deploy targets failing with "Text file busy" when process still exiting

### Changed
- Renamed "Filament Sensors" to "Sensors" in settings panel

## [0.96.8] - 2026-03-05

### Added
- AFC buffer health indicators with safe-state green checkmark visualization
- QR code info modals for Discord and Documentation links in settings

### Fixed
- Home screen edit mode triggered while dragging fan arc or slider knobs
- Clog detection widget and single-page carousel not passing clicks through to parent
- Spoolman spool item not accessible from extruder menu (#289)
- Filament preset layout broken with 3-up display; cooldown not stopping bed and nozzle heating together
- G-code viewer showing blank 3D preview when streaming mode is active
- Memory: reduce heap churn, fix string leak, prevent unbounded growth
- Telemetry dashboard data mapping for hardware, panel usage, memory, and themes
- Build compatibility with GCC 10 (std::from_chars float overloads)
- Crash handler preprocessor branches for macOS ARM64 compilation

### Changed
- Default dark theme palette darkened for improved contrast

## [0.96.7] - 2026-03-05

Stability-focused release addressing multiple crash vectors, WiFi driver concurrency, async callback safety, and widget deletion during events. Also adds CJK font support, debug bundle improvements, and numerous UI fixes.

### Added
- CJK glyph support in text fonts with automatic font regeneration on translation changes
- User note field on debug bundle submission for additional crash context

### Fixed
- Multiple crash vectors: async callback use-after-free with alive guards, widget deletion deferred to prevent event corruption, WLED thread safety, and AMS child deletion re-entrancy
- WiFi SIGSEGV from concurrent wpa_ctrl access without mutex protection
- Dual-axis input shaper calibration discarding stale callbacks (#310)
- Screws tilt calibration callbacks not routed to UI thread (#309)
- Tips rotation timer not stopped on panel deactivation (#296)
- Tips label opacity not reset on reactivation after interrupted fade
- Phantom fan widget shown when alternate part fan is configured
- Debug bundle missing syslog and embedded crash file paths
- Bed mesh 3D graph not refreshing on profile switch; row clicks falling through to panel beneath (#307)
- AMS filament edit modal opening on picker view instead of form view
- Spoolman spool list race condition causing incomplete filament data (#311)
- MPC calibration control type incorrectly detected on non-Kalico printers (#306)
- Async observer use-after-free crash on ARM32 with enhanced diagnostics (#317)
- Display waking with blank screen due to FBIOBLANK race on software overlay (#303)
- Slider knob clipped at edges in fan and PID tuning menus (#306)
- Dim/sleep constraint applied on devices without dimming support (#313)
- Installer crash on missing dependencies during self-update (#314)
- Display orientation probe not applying LVGL rotation (#315)
- Grid widget selection before layout update causing misalignment (#308)
- Toast dismissal crash from synchronous deletion during event processing (#316)
- Console log output suppressed by isatty check
- Crash diagnostics: recover actual crash location from CPU registers on shallow ARM32 backtraces

### Changed
- About screen uses native LVGL scroll with prerendered logo replacing custom marquee animation (#312)

## [0.96.5] - 2026-03-05

### Added
- Compile-time ENABLE_MOCKS flag to exclude mock backends from production builds

### Fixed
- Print status card requiring multiple clicks to navigate (grace period timer started on first click instead of app launch)
- Camera stream dying during fullscreen transitions and panel navigation
- Crash serialization cascading failures when writing crash entries
- LED color parsing errors with signed char values and toggle icon state drift
- Click events not reaching parent widget on print status card and other dashboard cards

### Changed
- MJPEG camera decoding uses libturbojpeg for SIMD-accelerated performance
- 3D renderer releases CPU geometry and GPU vertex buffers when no longer needed
- Consistent pressed-state touch feedback across all clickable dashboard widgets
- AMS buffer status extracted into declarative XML modal with corrected meter drawing
- Translations updated: 17 new strings across 9 languages

## [0.96.4] - 2026-03-04

This release adds AMS dryer status, proportional buffer feedback for Happy Hare, a filament health carousel, LED startup brightness control, and proactive memory monitoring. Camera stability received major crash fixes, and G-code viewer memory usage was cut in half.

### Added
- AMS dryer info bar showing humidity and dryer status on filament panels
- AMS proportional buffer meter with color-coded bias visualization and fault context modals
- AMS filament health carousel replacing the clog detection widget
- Visual press feedback on AMS filament slot taps
- LED startup brightness slider in LED settings
- Last printed file thumbnail on idle home screen print status card
- Proactive memory monitoring with device-tier thresholds and telemetry dashboard
- PFA Stealthfork printer profile and updated Micron heuristics
- Single-instance file lock to prevent dual-process DRM conflicts

### Fixed
- Camera use-after-free crashes from detached stream threads and stalled ScopedFreeze rebuilds
- Camera stream not recovering after detach/reattach or widget drag rebuild
- 3D bed mesh view not rendering on initial load
- Crash diagnostics: ARM32 static-PIE load_base capture and AD5M filtered memory maps (#296)
- Object deletion during LVGL input event processing causing child list corruption
- Phantom print starts on startup before printer state is fully synced
- Config backup loss during Moonraker update wipes
- Config backup failing when /var/log is unwritable
- LED toggle state inverted; LED state not queried on startup
- Fan widgets not shown or not disabled when printer disconnected
- AMS toolchanger unload using wrong slot index; AFC current_slot not preserved
- Thumbnail picker selecting smallest instead of best-fit resolution

### Changed
- Camera stream and extended sleep suspended during display off to reduce idle power
- Screensaver CPU usage reduced across all three modes (pre-decoded sprites, optimized toasters)
- G-code ToolpathSegment halved to 40 bytes; loading deferred 5s after print start; streaming forced on ≤4GB RAM
- Adaptive main loop sleep replaces fixed 5ms delay
- Settings reorganized: G-code preview consolidated, Time Format moved to Appearance

## [0.96.3] - 2026-03-03

This release adds a floating emergency stop button, temperature sensor carousel with multi-sensor picker, 3D box effects on AMS trays with Happy Hare Type B hub detection, and improved DRM display rotation robustness. QR label printing for Brother QL printers is available as a beta feature.

### Added
- Floating emergency stop button with confirmation dialog (enabled by default)
- Temperature sensor carousel mode with multi-sensor picker
- AMS tray 3D box effect using oblique projection draw callbacks
- Happy Hare Type B MMU hub topology detection with adjusted tray transparency
- QR label printing for Brother QL printers (beta-gated)
- DRM display rotation auto-fallback to fbdev and legacy atomic commit fallback (#288)

### Fixed
- GCode viewer dangling pointer in deferred ghost label deletion (#290)
- Spoolman spool canvas transparency using ARGB8888 and explicit API query limit (#289)
- Software keyboard appearing during scroll gestures
- Print status panel dedup guards not clearing on navigation, thumbnail binding, and card layout sizing
- Update download modal callbacks registered after modal shown from notification
- Installer polkit PKLA generated inline, eliminating deploy failures
- Macro `variable_*` extraction removed entirely from parameter handling
- AMS unit card radius now fixed for consistent 3D tray box alignment

## [0.96.2] - 2026-03-03

### Added
- QGL and Z-Tilt leveling buttons on the motion overlay

### Fixed
- Dropdown selection always picking the last item on Goodix GT9xx touchscreens (Protocol A release coordinate regression)
- Installer polkit directory checks now use sudo when required (reported by @BO_Andy)

## [0.96.1] - 2026-03-02

This release adds a GCode console with full command history, Mainsail-style per-field macro parameter editing with Klipper variable support, spool temperature presets on filament and temperature panels, and a preset-aware setup wizard. Grid edit mode stability received major crash fixes.

### Added
- GCode console panel with monospace font, timestamped command history, and home screen widget
- Per-field macro parameter inputs with placeholders, `variable_*` field support, and scrollable modal
- Macro picker Done button with responsive height and icon/color customization
- Spool preset button on nozzle, bed, and filament panels using active material temperatures
- ActiveMaterial provider with priority-based resolution across filament sources
- Auto-pass PURGE_TEMP from active spool material to purge macros
- Preset-aware setup wizard that skips hardware steps for preconfigured builds
- Dedicated telemetry opt-in step in wizard for preset mode
- Hex color support in action prompt buttons (#278)
- Toast notification when taking in-app screenshots
- Translated panel widget names and descriptions in catalog overlay

### Fixed
- Grid edit mode SIGSEGV crashes from double-free during overlay deletion and external rebuilds
- Touch axis swap auto-detection removed — broke Nebula Pad and Sonic Pad calibration
- LVGL arc draw crash from negative inner radius and zero-sweep edge cases
- About panel callback names mismatched with XML, leaving update buttons dead
- Print source selector no longer shown when no USB drive is present
- Input field contrast on elevated surfaces and filename truncation (#283)

### Changed
- Console graduated from beta with full documentation
- Preset packages use convention-based lookup by platform target name
- Macro parameter detection uses `'NAME' in params` pattern in addition to dot-access

## [0.96.0] - 2026-03-02

This release adds camera streaming and fullscreen view, TMC stepper driver temperature monitoring, and smarter macro parameter handling. Widget stability and Android support received significant improvements, along with fixes for several crash reports.

### Added
- Camera fullscreen view with MJPEG streaming, separate connect/active timeouts, and data arrival tracking
- TMC stepper driver temperature support with corrected display names
- Macro parameter pre-parsing during discovery for faster parameter dialogs

### Fixed
- Home screen startup jumpiness from uncached grid rows and deferred card backgrounds
- Widget config loss from duplicate PanelWidgetConfig instances during rebuilds
- Edit mode long-press triggering during scroll/swipe on home screen
- Newly added widgets not auto-selected in grid edit catalog
- AD5M crash loops and stream exceptions with unwind tables enabled (#280, #281)
- Binary backlights that only support on/off now handled correctly (#276)
- About panel version subjects not bound and marquee scroll performance (#275)
- NaN/Inf float values in JSON responses no longer cause crashes (#277)
- Installer self-heals un-substituted polkit pkla template (thanks @BO_Andy)
- Android startup crash, tofu glyphs on ARM64, and shutdown crash
- Protocol A touch release for Goodix GT9xx capacitive panels
- Temperature graph now supports up to 16 series for TMC stepper temps
- Invalid theme token references (text_secondary, radius_md) replaced
- Macro config key lookup with improved error logging

### Changed
- Fan arc refactored into shared fan_arc_core component with format_fan_speed helper
- Widget rebuilds skip redundant work with wider coalesce window for gate observers
- Temperature carousel dot spacing now matches fan carousel

## [0.95.3] - 2026-03-02

### Added
- Live webcam panel widget with snapshot polling for camera monitoring
- Print statistics widget with 4 responsive size modes for the home dashboard
- Macro parameter dialog — macros with parameters now prompt for values before execution
- Dangerous macro confirmation — SAVE_CONFIG, FIRMWARE_RESTART, etc. require confirmation
- Locale-aware date/time formatting with translation support across all languages
- Clog detection configuration modal for tuning sensitivity and thresholds

### Fixed
- Macro parameter detection now queries Klipper configfile instead of printer objects, which always returned null
- LED toggle state now syncs with actual hardware on bind and before toggle
- Preparing Print overlay no longer gets stuck after print start
- Clock widget timer restarts correctly after home screen rebuild
- Filament preset button temperatures now pull from filament database
- Assert handler re-entrancy and InputShaper threading violations causing crashes
- Moonraker update detection broken by release name prefix (#270)
- Wizard kinematics filter now supports Kalico and hybrid variants
- Input fields inside dialogs now have proper contrasting background color
- Camera widget initial overlay layout — full-width text and centered spinner

### Changed
- Text input widgets auto-register with software keyboard — no manual wiring needed
- Temperature stack padding matches fan stack; enlarged carousel icons
- Fan arc widget sizing reduced with proper padding in carousel

## [0.95.2] - 2026-03-01

### Added
- Clog detection arc meter on loaded AMS card for real-time filament flow monitoring
- Starfield and 3D Pipes screensavers with selection dropdown
- Buffer detail info modal accessible by tapping filament path coil
- Filament buffer visualization on AMS path canvas
- Full-screen color picker layout with tab switching for tiny screens

### Fixed
- Re-entrancy guard for fan arc resize preventing concurrent animation crashes
- Color picker HSV sizing and bottom-pinned buttons for responsive layouts
- G-code viewer reload after destroy-on-close cycle on print status panel
- NEON alignment, empty vector, and stale widget crashes identified from telemetry
- Telemetry active device count now uses separate query for correct COUNT(DISTINCT)

### Changed
- Build system: static-link libhv OpenSSL for K1/MIPS, avoid double-wrapping compilers with ccache

## [0.95.1] - 2026-03-01

### Added
- Widget catalog now shows icons and descriptions for each widget type
- LED Controls widget for quick access to LED settings from the dashboard
- Configure button in edit mode for widgets that support settings (macros, temperature stacks)
- ZMOD firmware detection for AD5M/AD5M Pro installer (#251)

### Fixed
- Use-after-free crash during WebSocket reconnection (#255)
- SIGSEGV from wrong-pointer lv_anim_delete on bar animations (#259)
- Font linked-list validation to prevent SIGSEGV on corrupted font data (#244)
- Fan button states not updating when animations are disabled (#258)
- AD5X platform key missing from update checker (#253)
- SIGSEGV when registering XML event callbacks after XML parsing
- SIGSEGV from stale input device reference after grid rebuild
- Dangling observer pointer in AMS cross-singleton cleanup during reconnection
- Backlight stays on during sleep mode for AD5X/CC1 displays (#235)
- Job queue fetch race condition at startup before WebSocket is connected
- Spoolman spool editing: vendor, material, color, and filament_id now sync correctly
- Happy Hare gate_spool_id parsing for Spoolman fill gauge display
- AMS slot editing from overview panel context menu
- Internal macros (underscore-prefixed) hidden from macro picker
- Fan panel: temperature_fan classified as auto-controlled, knob hidden; primary color for arc indicator; dial clipping fixed
- Navigation: overlays restored via go_back() now receive on_activate()
- OpenGL ES rendering stride alignment for blit operations
- E-stop button repositioned to top center of print status widget
- Humidity sensor log spam reduced to display-precision changes only
- Pressed feedback added to favorite macro widgets
- Widget auto-shrinks to fit when default size exceeds available space
- Toast notification when no room for widget placement

### Changed
- Translations updated: 47 new strings across all 8 languages
- Home Widgets settings overlay removed (replaced by widget catalog)
- Removed static linking on AD5X platform

## [0.95.0] - 2026-03-01

The biggest release yet — HelixScreen's home panel is now a fully customizable grid dashboard. Drag widgets to reposition, resize from any edge, add new widgets from a catalog, and remove what you don't need. The layout persists per-breakpoint and survives restarts. Also includes significant memory optimizations, a new screensaver, material temperature presets, and a standalone About overlay.

### Added
- Customizable grid dashboard replaces the fixed home panel layout — drag to reposition, resize from any edge, and persist per-breakpoint
- Widget catalog overlay for browsing and adding widgets to the dashboard
- Digital Clock widget with responsive font scaling
- Job Queue widget with queue management actions (start, pause, cancel)
- Shutdown/Reboot widget with modal confirmation
- Material temperature overrides with per-material nozzle and bed customization
- Flying Toasters screensaver (After Dark, 1989)
- Standalone About overlay extracted from settings panel
- Default widget layouts defined in runtime JSON with per-breakpoint anchors

### Fixed
- Job queue filename contrast and empty state visibility
- Clock widget centering and font scaling across breakpoints
- Printer image snapshot transparency (ARGB8888 instead of RGB565)
- Tips widget accent bar sizing and content centering
- Widget click targets improved to prevent use-after-free on child elements

### Changed
- Memory optimizations: ~2.5MB savings from overlay destroy-on-close, observer suspension, and RGB565 color depth option
- Gate observer rebuilds coalesced (300ms window) reducing startup from 4x to 2x
- Over 1000 lines of dead HomePanel code removed after widget extraction

## [0.13.13] - 2026-02-28

### Fixed
- AFC filament system now discovers units with generic `AFC_` prefix, improving compatibility across AFC configurations
- NULL pointer checks added to helix-xml parsing and rotation probe to prevent OOM crashes
- Orientation detection logic corrected for display rotation

## [0.13.12] - 2026-02-28

This release adds MPC calibration support for Kalico/Danger Klipper firmware, a unified temperature graph overlay, and significant performance and stability improvements.

### Added
- MPC (Model Predictive Control) calibration UI with Kalico detection, config migration flow, and mock support
- Unified temperature graph overlay replacing three separate per-sensor overlays with side-by-side layout and per-mode controls
- Clickable mini temperature graph on filament panel opens full overlay
- Friendly status screen displayed before restart during updates
- Klipper config editor: `ConfigEdit` and `safe_multi_edit` for safe multi-key config changes

### Fixed
- Fan speeds stuck at 0% due to race condition in fan state updates
- G-code viewer continues rendering when print status panel is hidden, wasting CPU
- NEON alignment and NULL guard crashes in LVGL software rendering path
- Blend area clipped to buffer bounds to prevent NEON SEGV (#242)
- Black screen on SysV self-update due to unnecessary stop/start cycle
- User config files now preserved across installer updates
- Redundant 'Temperature' suffix stripped from temp graph chip labels

### Changed
- Subject notifications skip redundant updates when values unchanged, reducing UI redraws
- AMS backend eliminates redundant subject fires and cascading redraws
- Theme lookups cached and canvas dirty guards added for improved render performance

## [0.13.11] - 2026-02-28

### Added
- ViViD filament system support in AFC backend with unit discovery and key mapping
- Dedicated logos for Happy Hare and AFC filament systems
- Improved 5GHz Wi-Fi band detection across all backends

### Fixed
- DRM display rotation reworked to eliminate flickering on inverted panels
- EMU filament system compatibility: color formats, gate counts, dryer state, filament names, and sensor readings
- Printer setup wizard no longer loses printer type list when navigating between steps (#231)
- Backlight now fully turns off during sleep mode on sysfs-based displays
- Bed mesh rendering blit failure and axis label offset corrected
- AD5X platform detection improved with secondary /ZMOD indicator (#225)
- Input shaper calibrate-all mode shows progress text instead of premature "Complete" (#225)
- Spoolman spool picker no longer shows empty vendor list on reopen (#225)
- DRM NEON blend buffer overrun prevented on reshape failure (#229)
- Crash loop detection and defensive widget creation for improved stability
- Missing translation tags on header bar action buttons
- Spoolman uses PATCH to update existing filaments correctly

### Known Issues
- Inverted/upside-down panels in DRM mode may exhibit minor flickering during heavy rendering

## [0.13.10] - 2026-02-28

### Added
- Automatic display orientation detection and software rotation for DRM displays

## [0.13.9] - 2026-02-27

### Added
- Asynchronous bed mesh rendering with double-buffered worker thread and adaptive quality degradation
- Off-screen pixel buffer rasterizer for bed mesh visualization
- Animated connector tube through LINEAR selector box in filament path view

### Fixed
- Backdrop blur re-enabled with NULL guards across all color formats and NEON paths
- Bed mesh panel forces initial paint on re-entry
- G-code viewer forces refresh after first 3D GPU render
- Home All sends bare G28 instead of G28 X Y Z
- Global extruder subjects sync correctly on tool selection in temperature panel

## [0.13.8] - 2026-02-27

### Added
- Memory-aware geometry budget system for 3D G-code viewer — automatically selects detail tier based on available memory with graceful 2D fallback
- GPU-accelerated backdrop blur for modals
- Shutdown and reboot widget with modal confirmation dialog
- Speed and flow rate increment buttons replace sliders for precise control (#219)
- Lemontron, Sovol SV08 Max, and Sovol Zero added to printer database
- FlashForge Adventurer 5X support with independent platform toolchain (#203)

### Fixed
- UI freeze during 3D geometry VBO upload eliminated
- AMS panel and spool picker back button click targets enlarged for easier navigation
- Goodix capacitive touch on Creality K1 Max and standalone builds
- DRM plane rotation fallback for VC4 displays (90/270 unsupported)
- Spoolman request flooding prevented with debounce and circuit breaker
- Spoolman filament creation sends required density and diameter fields
- Klippy readiness checked before querying printer objects during discovery
- Android display corruption from conditional style reset reverted
- Signed coordinate crash in LVGL draw path
- CoalescedTimer repeat count bug

## [0.13.7] - 2026-02-27

### Added
- Dropdown options now support translation via `options_tag` attribute
- Broad internationalization pass across C++ UI code with `lv_tr()` calls
- Touch calibration can be forced on startup with `HELIX_TOUCH_CALIBRATE` environment variable
- Thumbnail Only option for G-code render mode in display settings

### Fixed
- AMS tray height reduced for better proportions in slot grid
- G-code metadata parser rejects percentage values in extrusion width fields
- Cancel button now appears immediately when starting a print
- AFC multi-unit bugs: nozzle navigation, lane sorting, and current tool derivation

## [0.13.6] - 2026-02-26

### Added
- Fan speed overlay opens directly when tapping the fan icon in carousel mode
- Dual-output Pi builds: single compilation produces both DRM and fbdev binaries

### Fixed
- LED light state now syncs correctly from hardware on status updates
- Empty AMS slots are clickable with placeholder circle and context menu
- LED widget initial state and reactive bindings fixed
- Print details delete button is now icon-only, giving more room for the print button
- 2D fallback disabled on print details panel; thumbnails used instead
- AMS slot positioning fixes for hidden spool containers and LINEAR selector box sizing
- Keyboard overlay crash when cleanup nulls alternatives mid-use (#207)
- Use-after-free in LED and temperature widget button user data
- Tool badge now shown on empty unassigned AMS gates
- Printer database JSON parsing hardened against type mismatches
- ELF architecture validation uses platform key instead of uname
- Updated Ender 5 Max printer image
- Installer preserves user files in `printer_database.d` during upgrades
- AMS flow animation no longer runs infinitely, fixing 50%+ CPU usage on AMS panel

### Changed
- XML event callbacks registered at startup instead of widget attach time
- Panel switching and widget creation optimized for ARM
- Spoolman vendor list fetched from dedicated endpoint instead of downloading all spools
- AMS gate observer rebuilds coalesced to reduce startup churn

## [0.13.5] - 2026-02-26

### Added
- Touch jitter filter for noisy controllers like Goodix GT9xx with automatic breakout detection
- Auto-detect swapped touch axes during calibration, especially for Creality SonicPad and similar devices with misreported axis orientation
- Power Devices entry in System settings
- AMS filament system header bar now shows system logo and name with declarative bindings
- AMS LINEAR output path with animated slide beneath active slot
- Update telemetry tracking: success/failure recording across update lifecycle with analysis tools
- Seven new telemetry event types with thread-safe recording
- `--debug-touches` flag for touch input diagnostics

### Fixed
- Header back button touch target expanded to full title width for easier navigation
- AMS bypass spool centered on filament line with label moved beneath
- AMS spools centered inside tray with support for variable AFC lane count
- Happy Hare AMS now uses LINEAR topology with SELECTOR butted against prep sensors
- Float-to-int conversion guards against NaN/Inf values (#206)
- AD5X platform now correctly maps to K1 MIPS binary (#203)
- Async callback use-after-free in Spoolman spool selection
- Null guards for keyboard events and NEON blend path (#207, #208)
- Installer no longer shows misleading 'corrupt download' message on slow CDN connections
- Jitter filter correctly disabled after breakout for smooth scrolling

### Changed
- HomePanel refactored into self-contained widgets
- CI build split into separate compile and test jobs

## [0.13.4] - 2026-02-25

### Added
- BMP and GIF format support for custom printer images
- Invalid custom printer images shown as disabled with lazy import and instant gallery refresh
- In-process fbdev display fallback when DRM initialization fails (no restart needed)
- Top-level exception handler prevents unhandled crashes from silently terminating the application

### Fixed
- SonicPad Goodix (gt9xxnew_ts) touchscreen now triggers calibration wizard when kernel reports zero ABS ranges
- DRM backend no longer falls back to `/dev/dri/card0` when no suitable DRM device exists
- Fan carousel arc thumb disabled on auto-controlled fans that don't accept manual speed changes
- Observer cleanup ordering hardened to prevent cascading use-after-free during shutdown
- Thread safety and crash telemetry improvements across observer guards and lifecycle management

### Changed
- Macro search resolves only active include-chain config files, improving performance on large configurations
- Touch calibration and wizard skip decisions promoted to info-level logging for easier diagnostics

## [0.13.3] - 2026-02-25

### Added
- AFC tool change progress display on print status panel with current/total tool change counter
- AFC mock tool change progress in test mode for development

### Fixed
- MT-only touchscreens (e.g., Goodix gt9xxnew_ts on Nebula Pad) now detected correctly — previously invisible due to missing ABS_MT_POSITION_X/Y bit checks
- ABS range queries fall back to multitouch axes when legacy ABS_X/ABS_Y are absent, enabling rotation mismatch detection on MT-only devices
- Installer now preserves user data directories (custom_images, printer_database.d) during upgrades
- Watchdog no longer launches external splash screen in DRM mode, preventing a crash loop
- ObserverGuard cleanup lambda prevents use-after-free when releasing observers
- Print status filament row decluttered by removing redundant "Filament" and "Active" labels
- AFC mock toolchange progress default now set in constructor for consistent test behavior

### Changed
- README LVGL badge updated to 9.5, added helixscreen.org link

## [0.13.2] - 2026-02-25

### Added
- FlashForge AD5X platform support (#203)
- Touch calibration CLI flag (`--calibrate-touch`) and `input.force_calibration` config option
- Touch calibration standalone user guide
- Improved touch calibration UX with tap-to-begin, progress counting, and flash feedback

### Fixed
- Touch input on fbdev devices no longer applies a redundant rotation transform (#186)
- Shutdown sequence hardened against use-after-free crashes
- DRM device configuration now validates before use and falls back to auto-detection on failure
- Diagnostic logging added for DRM initialization failures and startup platform info
- Alive guards and lifecycle safety added to 5 crash-prone components
- WebSocket disconnected before clearing app globals to prevent spurious shutdown errors
- TemperatureSensorManager shutdown crash prevented with alive guard
- Happy Hare MMU slot data now received via mmu object subscription (#214)
- Splash screen skipped on DRM-only systems to prevent master contention
- Telemetry queue file writes now atomic to prevent empty file on interrupted save
- DRM rotation patch includes header declaration (fixes Pi cross-compilation)
- Launcher e2e tests mock system commands to avoid hitting dev machine

## [0.13.1] - 2026-02-25

### Added
- Robust touch calibration with multi-sample input filtering, ADC saturation rejection, and post-compute validation
- Smart calibration auto-revert with 10-second timeout and broken-matrix detection
- DRM plane rotation support for `rotate` config on Raspberry Pi

### Fixed
- Use-after-free crashes in AMS modal destructor and sidebar (#199, #201)
- Static-linked OpenSSL for Pi fbdev variants (fixes missing libssl.so.1.1 on some systems)
- Creality SonicPad/Nebula display backlight no longer killed by display-sleep service
- OTA update downloads no longer fail for releases larger than 50 MB (limit raised to 150 MB)
- Touch calibration verify handler simplified with dead wizard callbacks removed

## [0.13.0] - 2026-02-24

The rendering engine gets a major upgrade — the 3D G-code viewer is ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading, and Pi builds gain GPU-accelerated DRM+EGL rendering with automatic framebuffer fallback. A first-boot rotation probe auto-detects display orientation, and the UI gains carousel modes for temperature and fan widgets, frosted-glass modal backdrops, and a new shared progress bar component with gradient indicators.

### Added
- GPU-accelerated DRM+EGL rendering for Raspberry Pi with automatic fbdev fallback
- 3D G-code renderer ported from TinyGL to OpenGL ES 2.0 with per-pixel Phong shading and camera-following light
- First-boot display rotation probe with auto-rotating touch coordinates
- Automatic DRM/fbdev binary selection and dual-binary Pi releases
- Carousel display mode for temperature and fan stack widgets with long-press toggle
- Shared progress bar component with dynamic gradient indicator
- Frosted-glass backdrop blur effect on modals
- G-code render mode setting visible when 3D rendering is available
- Chamber temperature overlay on controls panel
- Print lifecycle state extraction for cleaner print status management
- 3D G-code viewer in print file detail panel with async loading and AMS color support
- Pinch-to-zoom gesture support for 3D G-code viewer
- Icons on Delete and Print buttons in print details card
- SSL enabled for native desktop builds
- Translation updates with 7 obsolete keys removed
- External spool widget for printers without AMS
- Loading spinner overlay and 3D rotate hint icon on print file detail panel
- AFC tool changer support with proper SELECT_TOOL/UNSELECT_TOOL commands and extruder dropdown

### Fixed
- Use-after-free crashes in G-code viewer, power panel, mDNS callbacks, thumbnail loading, and AMS widget cleanup (#182, #192, #193)
- Scroll jitter in virtual list views caused by layout-invalidating calls
- Safe name-based widget lookup prevents miscast crashes in event handlers (#194, #195)
- GLES 3D build correctly disabled on macOS (no EGL headers)
- Stale thumbnail and progress data no longer persists when a new print starts
- AMS spools no longer show as full when Spoolman initial_weight is null
- AMS loaded filament card swatch color now updates reactively
- Overlays now close when clicking the navbar button for the active panel
- Progress bar draw no longer triggers lv_inv_area assertion
- Overflow row click passthrough on controls panel
- Temperature chart validates widget before updating series data
- Renamed Voron Micron to PFA Micron in printer database
- AMS edit modal remaining weight defaults and display
- Header bar back button click area expanded for better touch targeting
- 3D viewer camera framing and gesture responsiveness
- Pinch-to-zoom rendering during gesture
- DRM flush timeout from glReadPixels on cached frames
- Print buttons stay at bottom when preprint options are hidden
- Empty preprint options card hidden when no options available
- Z-adjust button icons corrected for bed-moving printers
- Z-adjust button order fixed so down arrow is on bottom
- 3D viewer crash when loading UI triggers on hidden widgets
- Timelapse callback registration

### Changed
- Tertiary theme color changed from orange to blue-violet
- Inline progress bars replaced with shared progress_bar component
- AMS panels refined with more compact loaded filament card and tighter spacing
- Edit icons changed from secondary to tertiary color
- Deprecated AMS mock environment variables removed

## [0.12.1] - 2026-02-23

### Added
- Daily active devices and cumulative growth charts in analytics dashboard
- Zstd compression for debug symbol uploads (~60% size reduction)

### Fixed
- XML `inner_align="contain"` and `inner_align="cover"` now work correctly — images were rendering at native size instead of scaling to fit their containers
- Telemetry device counting uses unique devices instead of sessions for cumulative growth
- Discord invite links updated across all documentation
- Worktree setup script resolves main tree path correctly when run from inside a worktree
- Android CMake build includes helix-xml library
- GitHub release titles no longer show duplicate version numbers

## [0.12.0] - 2026-02-23

A major infrastructure release — LVGL is upgraded to v9.5.0, and the XML layout engine has been extracted into its own library (`helix-xml`) for independent development. The Android port lands with initial build system and CI pipeline support. Developer experience improves with XML hot reload for live UI editing.

### Added
- Android port: build system, APK packaging, asset extraction, CI builds, and release pipeline
- XML hot reload for live UI editing during development (`HELIX_HOT_RELOAD=1`)
- Klipper ERROR state recovery dialog with firmware restart option
- AMS unit names are now pretty-printed for readability
- Widget-safe async callback utilities for thread-safe UI updates

### Fixed
- Use-after-free in deferred observer callbacks (#174)
- Modal use-after-free crash during navigation
- USB drive callback crash from background thread (now marshaled to main thread)
- AMS crash on quit from unjoinable scenario/dryer threads
- Spoolman weight polling skipped when filament backend already tracks weight
- Spoolman active spool management for Happy Hare and AFC backends
- AFC Unload button re-enables after lane scan resets filament state
- AFC mixed topology: correct tool count, hub sensor detection, and status display
- AMS current slot label accuracy for multi-unit and tool changer setups
- AMS overview right column capped at 200px to prevent layout overflow
- Keyboard restores screen position when dismissed via backdrop click
- Firmware restart widget shown for all non-READY Klippy states
- Responsive tokens applied to picker layouts, fan name widths, and panel widget spacing
- Font clipping and padding on panel widgets
- Gcode viewer segfault
- Update check errors now visible in the UI
- Installer uses printf for ANSI escape compatibility

### Changed
- LVGL upgraded from 9.4-pre to v9.5.0
- XML engine extracted to `lib/helix-xml/`, decoupled from LVGL internals
- MoonrakerAPI decomposed into domain-specific modules (Rest, Job, Motion, File, FileTransfer, Timelapse, Advanced)
- Z-offset utilities extracted into shared module
- Wi-Fi status polling refactored to async with responsive connect/disconnect updates

## [0.11.1] - 2026-02-22

### Added
- Panel widgets dim when Moonraker is disconnected or Klippy is not ready
- AMS slot bars resize responsively based on home panel row density
- Bypass spool widget and filament path topology support for AMS systems
- Happy Hare v4 parsing with full v3 backwards compatibility

### Fixed
- AMS "Currently Loaded" display now shows the correct filament in multi-backend setups (e.g., AMS_2 load no longer snaps to AMS_1 state)
- Use-after-free crash on AMS overview back-navigation
- Use-after-free crash when temperature graph chart widget is destroyed
- Bypass path and toggle hidden for tool changers (not applicable)
- Watchdog uses fork-based reboot fallback for crash dialog reliability
- Auto-restart after update install instead of showing unnecessary restart dialog

### Changed
- Filament page: purge button separated from extrude, operations layout reorganized

## [0.11.0] - 2026-02-22

A feature-rich release — fan speeds are now a first-class widget with spinning animations and density-aware labels, chamber temperature gets its own full control panel, and the Printer Manager overlay is available to everyone (no longer gated behind beta). Under the hood, dynamic observer lifetime safety prevents use-after-free crashes, and the MoonrakerClient has been decomposed for maintainability.

### Added
- Chamber temperature panel with graph, presets, and sensor-only monitoring mode — tap the chamber row in the Temperatures widget to open it
- Fan stack widget enabled by default with density-aware compact labels (P/H/C when space is tight)
- Spinning fan icon animations on dials, status cards, and home panel widgets
- Crash history tracking in debug bundles — past crash submissions with GitHub issue references
- SubjectLifetime tokens for safe observation of dynamic per-fan, per-sensor, and per-extruder subjects
- Micro layout support (480x272) with compact controls, theme editor, and display overlays
- Ender-3 V3 KE printer image and hostname detection
- MoonrakerClient decomposition: RequestTracker and DiscoverySequence extracted as independent modules
- Translations synced and fan stack labels localized across all languages

### Fixed
- Printer Manager overlay no longer gated behind beta features — accessible to all users
- Setup wizard auto-fills default port 7125 when the port field is left empty on Test Connection
- Printer detection: hostname_exclude heuristic prevents Ender-3 V3/V3 KE model ambiguity
- Heap corruption during change-host reconnection (double-free in observer cleanup)
- Stack overflow on Pi from `lv_obj_is_valid()` in hot paths (HeatingIconAnimator crash)
- HeatingIconAnimator theme observer uses ObserverGuard to prevent heap corruption
- Config preservation during in-app upgrades (three-layer merge prevents config loss)
- Factory reset now restarts the app automatically
- Installer download progress bar and error reporting
- Release download logic with timeout and speed limits
- Thermistor picker position and temp stack click targets
- Printer image snapshot alignment when clearing
- Home panel widget spacing, fan labels, and click targets
- Print start toast and phase tracking properly gated behind beta features
- Various compiler warnings and dead code removed

### Changed
- MoonrakerClient internals decomposed: discovery callbacks stored as members, stale connection guard added
- Navigation bar widened at medium and large breakpoints
- Printer image snapshotted to eliminate per-frame scaling (performance improvement)
- Install Update row moved under Check for Updates in About section
- Orphan AboutOverlay removed (was unreachable dead code)

## [0.10.14] - 2026-02-22

### Fixed
- AFC unload button and context menu now work on AFC firmware versions that don't expose a top-level `filament_loaded` field (e.g., Box Turtle with `lane_data_enabled=false`)
- AFC `current_load` field parsed as fallback when `current_lane` is absent, fixing loaded lane detection on newer AFC versions
- Crash-hardened 15 vectors found during 48-hour audit

## [0.10.13] - 2026-02-22

Crash hardening and new features — favorite macro widgets let you pin and run macros from the home panel, filament controls get dedicated Extrude/Retract buttons, and Wi-Fi status updates are now async and responsive. Under the hood, the MoonrakerAPI monolith has been split into domain-specific modules.

### Added
- Favorite macro panel widgets with macro picker and automatic parameter detection
- Extrude and Retract buttons replace Purge on filament page, with configurable speed
- Wi-Fi status updates respond immediately to connect/disconnect events

### Fixed
- Multiple crash fixes: glyph null guard, use-after-free in async AMS/telemetry callbacks, SIGSEGV in widget cleanup and render thread race
- AFC Unload button now re-enables after lane scan detects filament state change
- Observer generation counters prevent stale callbacks after controls repopulate
- Unknown CLI arguments warn instead of crash-looping
- Z-offset tune overlay save bug
- Macro parameter modal no longer stomps across widget slots
- Widget picker and overlay cleanup uses safe deletion
- USB drive callbacks marshaled to main thread to prevent crashes
- Panel widget padding and font clipping on small screens

### Changed
- MoonrakerAPI split into 8 domain-specific modules (Job, Motion, File, FileTransfer, Advanced, Rest, Timelapse, History)
- Wi-Fi backend uses async status polling instead of blocking queries
- Z-offset utilities extracted into shared module

## [0.10.12] - 2026-02-21

A stability and polish release focused on crash fixes, responsive UI improvements, and internal refactoring. The home panel widget system is now fully decoupled from HomePanel, keyboard input is more reliable, and several threading bugs have been resolved.

### Added
- Carousel widget component with wrap-around, auto-advance timer, indicator dots, and scroll detection
- Thermistor widget for monitoring custom temperature sensors on the home panel
- Responsive breakpoint subject (`ui_breakpoint`) for reactive visibility changes across screen sizes
- `HELIX_LOG_LEVEL` env var and `--log-level` CLI flag for fine-grained log control
- `HELIX_DPI` env var for overriding display DPI
- `HELIX_SKIP_SPLASH` env var to bypass splash screen
- Long-press auto-insert for alternate keyboard characters
- DWARF debug info pipeline for better crash backtrace resolution

### Fixed
- AFC mutex deadlock when error messages were emitted during state parsing
- Startup deadlock and shutdown race condition in mock mode
- Keyboard backspace and character insertion broken when cursor is mid-string
- Keyboard crash when textarea widget is deleted while keyboard is open
- Framebuffer stomping between splash screen and main process on Pi
- Kernel console text bleeding through LVGL UI on framebuffer devices
- Carousel wrap setting ignored in scroll end callback
- Resistive touchscreen detection for NS2009/NS2016 controllers (#135)
- Fan status hidden on tiny screens
- Slider row padding increased to prevent handle clipping
- Dropdown rows now wrap text responsively in settings
- Network item click handling uses correct event target
- Soft keyboard registered for hidden network modal inputs
- Self-update handles NoNewPrivileges; stale `.old` files cleaned on startup
- Installer polkit rules no longer contain untemplated placeholders
- `enP*` interface naming cleaned up (#145)

### Changed
- Home panel widgets fully decoupled from HomePanel — PanelWidget system with self-registration, per-panel config, and independent lifecycle
- AMS backends share extracted `AmsSubscriptionBackend` base class
- MoonrakerAPI split: `MoonrakerHistoryAPI` and `MoonrakerSpoolmanAPI` extracted
- Settings consumers migrated to domain-specific managers (Display, System, Input, Audio, Safety)
- Panels and overlays migrated to batch callback registration
- Temperature formatting consolidated into `ui_temperature_utils`
- Observer factory adopted across all remaining legacy observers
- Shell test suite optimized from ~4 min to ~90s

## [0.10.11] - 2026-02-20

### Added
- Customizable home panel widgets with drag-to-reorder — choose which widgets appear and arrange them to your preference
- External spool support — set filament type and color for the bypass/direct-drive spool, visible in system path canvas and detail views with Spoolman quick-assign
- SlotRegistry unified slot state management across all AMS backends (AFC, Happy Hare, ValgACE, mock)
- 3D tube rendering for filament paths with curved routing, glow effects, and flow particle animations
- Pipe-routed tube drawing with cable-harness nesting for multi-tool overview
- Pulse animation on target slot during filament swap
- Infimech TX printer support (#139)
- Sovol SV06 printer image

### Fixed
- Graceful shutdown on SIGINT/SIGTERM — no more crash on quit
- Filament subjects now update on first Moonraker status even when state matches defaults
- AMS endcap seam eliminated at straight-to-curve tube junctions
- AMS per-unit topology for filament segment in mixed mode
- AMS layout matching works regardless of initialization order
- AMS context menu positioning and badge sizing for 2-digit slots and tools
- AMS global slot numbering and detail view swatch color
- AFC virtual bypass sensor toggle uses correct sensor name
- Stealthburner polygon centering offset corrected
- Wi-Fi SSID retrieved from wifi list instead of invalid device field
- ABS mismatch detection re-enabled with generic HID range exclusion (#135, #137)
- Default print completion alert changed from notification to alert
- Installer drops unnecessary sudo from systemctl checks and adds polkit error handling
- Home panel falls back to generic printer image when file missing
- Home panel uses filament sensor count for hardware-dependent widget gating

### Changed
- AMS overview uses Stealthburner toolhead for Voron printers
- AMS detail view uses badge-style tool labels
- Sensor rows use status_pill component for type badges
- AMS sidebar extracted as shared component across panel types

## [0.10.10] - 2026-02-19

### Added
- Output pin LED backend for brightness-only chamber lights and enclosure LEDs — auto-detects `[output_pin]` devices with PWM slider or on/off toggle
- Individual X and Y homing buttons in controls quick actions
- Clear Spool context menu action for assigned-but-empty AMS slots
- AFC version warning when firmware is below v1.0.35
- Configurable Allwinner backlight ENABLE/DISABLE ioctls for broader SBC compatibility
- Udev and polkit rules for non-root backlight and Wi-Fi access on Pi

### Fixed
- Self-update under systemd NoNewPrivileges — installer now correctly skips privileged operations during in-place updates
- Installer preserves settings.json, helixscreen.env, and config across updates
- Render thread crash from NULL draw buffer race condition
- AFC unit topology now uses name-based matching instead of fragile index ordering
- Toolchanger uses SELECT_TOOL instead of ASSIGN_TOOL to avoid remapping
- Thumbnail paths resolved correctly for files in subdirectories
- File browser poll timer resumes after returning to print selection panel
- Timeouts added to long-running G-code calls to prevent UI hangs
- Systemd service dependency cycle from multi-user.target removed
- Self-restart uses `_exit(0)` instead of `exit(0)` to avoid background thread races
- Mock sensor dots restored for AMS prep sensors

### Changed
- Update check cooldown reduced from 60 minutes to 10 minutes
- SDL display hints cleaned up for better cross-platform performance

## [0.10.9] - 2026-02-19

### Added
- AMS sensor error states with visual indicators for hub, extruder, and lane sensor faults
- Happy Hare pre-gate sensor support for filament detection at each gate
- External tool change step progress detection — swaps and loads initiated from gcode or other UIs now show correct progress steps

### Fixed
- AFC gcode commands corrected to match actual AFC-Klipper-Add-On API: `CHANGE_TOOL`, `TOOL_UNLOAD`, `SET_MAP`, `RESET_FAILURE`, and `SET_BOWDEN_LENGTH` now use correct command names and parameters
- AFC per-lane commands (`SET_LONG_MOVE_SPEED`, `AFC_RESET_MOTOR_TIME`) now apply to all lanes instead of only the first
- AFC bowden length per-extruder now maps through unit membership to find the correct hub
- AMS mini status widget fills available height in multi-unit stacked layouts
- AMS tool badge labels use pre-formatted buffers with auto-sized badge width
- AMS backend skipped for tool changes when backend doesn't manage the tool
- AMS nozzle count corrected for mixed topology with unique per-lane tool mappings
- Happy Hare tip method detection reads from configfile on startup
- Mock mixed topology corrected to match real hardware (Box Turtle=HUB, AMS_2=PARALLEL)
- Worktree setup script auto-detects worktree when run without arguments

## [0.10.8] - 2026-02-19

### Added
- Debug bundle now collects Moonraker state, config, and Klipper/Moonraker logs via REST API
- PII sanitization in debug bundles for emails, API tokens, webhook URLs, and MAC addresses

### Fixed
- Crash from running animations when navigating away from a panel (#128)
- Crash from NULL font pointer during AMS bar layout rebuild
- Crash from stale async callbacks in gcode viewer
- Systemd update watcher stuck in infinite restart loop due to PathExists check
- Debug bundle log fetching handles HTTP 416 Range Not Satisfiable responses

## [0.10.7] - 2026-02-18

### Fixed
- AMS context menu UX: hidden tool dropdown, auto-close on backup select, conflict toast, and infinity icon for unlimited backup
- AMS Load/Unload/Eject buttons now work correctly from context menu
- AMS filament sensor toasts suppressed during active load/unload operations
- AFC `SET_RUNOUT` parameter corrected from `RUNOUT_LANE` to `RUNOUT`
- AFC 'Loaded' hub status correctly mapped to available instead of loaded
- AFC tip method detection from config with inline comment stripping
- Spoolman polling log noise suppressed unless spool weights actually changed
- Touch calibration wizard disabled ABS mismatch override for HDMI devices
- Power device probe no longer shows error toast on printers without power component
- Moonraker update manager switched from `type: zip` to `type: web` with systemd restart watcher

### Changed
- Removed dead AmsSlotEditPopup code replaced by context menu

## [0.10.6] - 2026-02-18

### Fixed
- Infinite CPU loop when saving Spoolman spool assignments on AFC and Happy Hare systems — Spoolman weight polling now updates slot state without sending G-code back to firmware, breaking a feedback cycle that saturated the CPU

## [0.10.5] - 2026-02-18

### Added
- **Android port**: Initial Android build system with CMake/Gradle, APK asset extraction, SDL fullscreen, and CI release pipeline
- **Power panel**: Moonraker power device control with home panel toggle and advanced menu integration
- Widget-safe async callback utilities for LVGL event handling

### Fixed
- AMS crash on quit from unjoinable scenario/dryer threads
- AMS right column capped at 200px max width for proper flex layout
- AMS tool count, hub sensor, and status corrected for mixed-topology AFC
- AMS current slot label improved for multi-unit and tool changer displays
- AMS 'Tooled' status handled correctly with production data regression tests
- Gcode viewer SEGV from unsafe async callback
- History totals computed from job list instead of hardcoded mock values
- Update check errors now visible in settings UI
- Installer uses printf for ANSI escapes instead of echo for POSIX compliance

### Changed
- Test output cleaned up: ~637 spurious warning/error lines silenced

## [0.10.4] - 2026-02-18

Slicer-preferred progress, Klipper M117 display messages, interactive AMS toolheads, and a batch of AMS rendering and stability fixes.

### Added
- Slicer-preferred progress via `display_status` — uses slicer-reported percentage over file position when available (#122)
- Klipper M117 display message shown on home panel print card (#124)
- Clickable AMS toolheads with docked dimming for parallel topology
- Per-lane eject and reset actions in AMS context menu
- Opacity and dim support for nozzle renderers
- Animated icons on controls panel

### Fixed
- Stale "Print" button text when print state changes (#125)
- Touch calibration wizard now shown for capacitive screens with ABS range mismatch (#123)
- AMS backend priority: MMU preferred over toolchanger when both are present
- AMS filament path lanes aligned with spool visual centers at all breakpoints
- AMS nozzle unloaded color unified and tool changer filament segments corrected
- AMS nozzle tip color changed to charcoal with idle path line fix
- AMS mini status bar sizing no longer applies 2/3 height scaling
- AMS toolchangers use `T{n}` gcode with click lockout during operations
- AFC slots only marked LOADED when `tool_loaded` is true
- Crash from NULL font pointer in AMS panel backend selector (#110)
- Touch input auto-detection scoring improved for multi-input systems (#117)
- `touch_device` config setting now read from the correct location
- Stale thumbnail no longer persists when a new print is started externally
- Self-update survives systemd cgroup kill (#118)
- UI switch size preset initialized before optional parse
- Filament panel left column layout flattened for proper flex_grow on temperature graph
- Noisy `assign_spool` warning downgraded to trace for virtual tool mappings
- Moonraker update manager release name uses tag-only format for compatibility
- `systemctl restart` uses `--no-block` to eliminate race window during updates
- Docker build uses GitHub mirror for zlib download

### Changed
- Crash reporting worker converted from JavaScript to TypeScript with GitHub App integration for dedup issue creation

## [0.10.3] - 2026-02-17

Big AMS release — unified slot editor with Spoolman integration, mixed-topology AFC support, error state visualization, and a major DRY refactor of shared drawing utilities across all AMS panels. Also adds 34 new translations and fixes several installer issues.

### Added
- **Unified Slot Editor**: New AMS slot edit modal with inline Spoolman picker, side-by-side vendor/material dropdowns, cancel/save flow, and in-use spool disabling
- **Spoolman Slot Saver**: Change detection and automatic save flow for slot-to-spool assignments with filament persistence
- **Mixed AFC Topology**: Box Turtle PARALLEL + OpenAMS HUB coexisting in a single AFC system
- **AMS Error Visualization**: Slot error dots, hub tinting, pulsing animations, severity colors, and aligned error detection across mini status and slot views
- **Shared Drawing Utilities**: Consolidated color, contrast, severity, fill, bar-width, display-name, logo fallback, pulse, error badge, slot bar, and container helpers
- Canonical `SlotInfo::is_present()` presence check for consistent slot detection
- Picker sub-view XML and header declarations for the unified edit modal
- 34 new translations across all 8 target languages with missing `translation_tag` attributes added
- Debug bundle fetch/display helper script
- ARM unwind tables and /proc/self/maps in crash reports

### Fixed
- Non-translatable strings (product names, URLs, OK) incorrectly wrapped in lv_tr()
- AMS edit modal Spoolman callbacks not marshalled to main thread
- AMS bypass detection, Happy Hare speed params, and other deferred TODOs resolved
- AMS bypass, dryer, reset, and settings hidden for tool changers
- Brand/spoolman_id missing from AFC and multi-AMS mock slots
- Load button enabled when slot already loaded
- Change Spool button label not updating correctly (ui_button_set_text)
- Static instance pointer for edit modal callbacks
- Updater diagnostic logs too noisy (downgraded to debug), added 2-min install timeout
- zlib updated from 1.3.1 to 1.3.2; Ubuntu CI build timeout bumped to 45min
- Installer stale .old directory blocking repeated updates (PR #102, thanks @bassco)
- Installer false-fail when cleanup_old_install hits root-owned hooks.sh

### Changed
- AMS panels refactored to use shared drawing utilities (DRY across 5 UI files: slot, mini status, overview, spool canvas, panel)
- Assign Spool removed from AMS context menu, replaced by unified slot editor
- Deprecated C-style wrapper APIs and legacy compatibility code removed
- Bundled installer regenerated with latest module changes

## [0.10.2] - 2026-02-17

This release significantly improves multi-tool printer support with per-tool spool persistence and an extruder selector for filament management, decouples Spoolman from AMS backends for cleaner architecture, and fixes several crash bugs and installer issues.

### Added
- Extruder selector dropdown for multi-tool printers in filament management
- Per-tool spool persistence decoupled from AMS backends
- Crash analytics dashboard with crash list view
- Load base and platform metadata in crash telemetry events
- Filament type tracking in print outcome telemetry events
- Discord notifications on successful releases

### Fixed
- Use-after-free during toast notification replacement (fixes #98)
- Dangling pointer after external modal deletion in AMS dryer dialog (fixes #97)
- Crash from null font pointer in AMS mini status overflow label (fixes #90, #91)
- Unsafe move operators corrupting lv_subject_t linked lists in setup wizard
- OTA updater "Installer not found" regression from systemd PATH resolution
- ELF architecture validation for K1/MIPS platform
- Static-linked OpenSSL for pi32 with post-install ldd verification
- AMS context menu positioning and ghost button borders
- Hidden tray and redundant tool badges for tool changers
- AMS bypass, dryer, reset, and settings visibility for tool changers
- Click-through on nozzle icon component
- Navigation bar buttons not filling available width, with lingering focus rings

### Changed
- Spoolman integration decoupled from AMS backends into standalone architecture
- Nozzle icon extracted into reusable component with consolidated tool badge logic
- Codebase migrated to `helix::` namespace with modernized enum classes
- Telemetry worker updated to support schema v2 nested fields

## [0.10.1] - 2026-02-16

### Added
- Debug bundle upload for streamlined support diagnostics
- Unified active extruder temperature tracking across multi-tool setups
- Dynamic nozzle label showing tool number for multi-tool printers
- Configurable size property for filament sensor indicator
- PrusaWire added to printer database
- Email notifications for debug bundle uploads (crash worker)
- 69 new translations across 8 languages

### Fixed
- Use-after-free in deferred observer callbacks (fixes #83)
- Crash from error callbacks firing during MoonrakerClient destruction
- Crash from SubscriptionGuard accessing destroyed MoonrakerClient on shutdown
- Crash from theme token mismatch in AMS backend selector
- Observer crash on quit from NavigationManager init ordering
- Spdlog call in ObserverGuard static destructor causing shutdown hang
- Tool badge showing unnecessarily with single-tool printers
- Hardware discovery falsely flagging expected devices as new
- Setup wizard not clearing hardware config on re-run
- Wizard port input accepting non-numeric characters
- Splash screen suppressing rendering without an external splash process
- Noisy WLED and REST 404 logs downgraded from warn to debug
- AMS slot info updates logged on every poll instead of only on change
- Installer using bare sudo instead of file_sudo for release swap/restore
- AMS edit modal Spoolman callbacks not marshalled to main thread

### Changed
- Spoolman vendor/filament creation moved to modal dialogs
- Spool wizard graduated from beta
- Lifetime checks added to SubscriptionGuard and ObserverGuard
- Shutdown cleanup self-registered in all init_subjects() methods

## [0.10.0] - 2026-02-15

Major feature release bringing full Spoolman spool management, a guided spool creation wizard, multi-unit AMS support for AFC and Happy Hare, probe management, and a Klipper config editor. Also adds Elegoo Centauri Carbon 1 support and fixes several crash bugs.

### Added
- **Spoolman Management**: Browse, search, edit, and delete spools with virtualized list, context menu, and inline edit modal
- **New Spool Wizard** (beta): 3-step guided creation (Vendor → Filament → Spool Details) with dual-source data from Spoolman server and SpoolmanDB catalog, atomic creation with rollback
- **Multi-unit AMS**: Support for multiple AMS/AFC/Happy Hare units with per-unit overview panel, shared spool grid components, and error/buffer health visualization
- **Probe Management** (beta): BLTouch panel with deploy/retract/self-test, probe type detection for Cartographer, Beacon, Tap, and Klicky
- **Klipper Config Editor**: Structure parser with include resolution, targeted edits, and post-edit health check with automatic backup restore
- **Elegoo Centauri Carbon 1**: Platform support with dedicated build toolchain and presets
- AFC error notifications with deduplication and action prompt suppression
- Android-style clear button for all search inputs
- Toast notifications for AMS device actions
- Internationalization for remaining hardcoded UI strings

### Fixed
- Crash from re-entrant observer destruction during callback dispatch (fixes #82)
- Use-after-free when destroying widgets from event callbacks (fixes #80)
- AMS slot tray visibility behind badge/halo overlays
- AFC buffer fault warnings not clearing on recovery
- Happy Hare reason_for_pause not clearing on idle
- Icon font validation locale handling
- Focus on close/context menu buttons causing unintended list scroll
- Modal dialog bind_text subject references missing @ prefix

### Changed
- AMS detail views refactored to shared ams_unit_detail and ams_loaded_card components
- Spoolman and history panel search inputs use shared text_input clear button
- R2 release retention policy added to prune old releases

## [0.9.24] - 2026-02-15

### Fixed
- OTA updates now correctly extract the installer from release tarballs (path mismatch between packaging and extraction)
- Button visual shift on release by setting transform pivot in base button style

## [0.9.23] - 2026-02-15

### Added
- LED colors stored as human-readable #RRGGBB hex strings with automatic legacy integer migration
- ASLR auto-detection in backtrace resolver for more accurate crash report symbol resolution

### Fixed
- Crash from LVGL object user_data ownership collisions causing SIGABRT
- Crash from NULL pointer passed to lv_strdup
- Use-after-free in animation completion callbacks
- Use-after-free when replacing toast notifications during exit animation

## [0.9.22] - 2026-02-15

### Added
- Timelapse phase 2: event handling, render notifications, and video management
- AD5M ready-made firmware image as primary install option in docs

### Fixed
- **Critical**: install.sh now included in release packages, fixing "Installer not found" error during UI-initiated updates (thanks @bassco)

### Changed
- CI release pipeline refactored to matrix builds for easier platform maintenance (thanks @bassco)

## [0.9.21] - 2026-02-14

### Added
- G-code console gated behind beta features setting
- Cancel escalation system: configurable e-stop timeout with settings toggle and dropdown
- Internationalization for hardcoded settings strings

### Fixed
- Nested overlay backdrops no longer double-stack
- Crash handler and report dialog disabled in test mode to prevent test interference
- Installer now extracts install.sh from tarball to prevent stale script failures
- Operation timeout guards increased for homing, QGL, and Z-tilt commands
- Touch calibration option hidden for USB HID input devices

### Changed
- G-code console and cancel escalation documented in user guide

## [0.9.20] - 2026-02-14

This release adds multi-extruder temperature support, tool state tracking, multi-backend AMS (allowing printers with multiple filament systems), and fixes a critical installer bug that prevented Moonraker from starting on ForgeX AD5M printers after reboot.

### Added
- Multi-extruder temperature support with dynamic ExtruderInfo discovery and selection panel
- Tool state tracking (ToolState singleton) with active tool badge and tool-prefixed temperature display
- Multi-backend AMS: per-backend slot storage, event routing, backend selector UI, and multi-system detection
- ASLR-aware crash reports: ELF load base emitted for accurate symbol resolution
- AD5M boot diagnostic script for troubleshooting boot/networking issues
- Russian translation updates (thanks @kostake, @panzerhalzen)
- Telemetry Analytics Engine dashboard

### Fixed
- **Critical**: ForgeX installer logged wrapper broke S99root boot sequence, preventing Moonraker from starting after reboot (#36)
- Splash screen no longer triggers LVGL rendering while it owns the framebuffer
- Exception-safe subject updates in sensor callbacks
- UpdateQueue crash protection with try-catch in process_pending
- Notification and input shaper overlays use modal alert system instead of manual event wiring
- Invalid text_secondary design token replaced with text_muted
- LED macro preset UX improvements and stale deletion bug fix
- systemd service adds tty to SupplementaryGroups for console suppression

### Changed
- Async invoke simplified to forward directly to ui_queue_update
- LED preset labels auto-generated from macro names instead of manual naming

## [0.9.19] - 2026-02-13

### Added
- Crash reports now include fault info, CPU registers, and frame pointers for better diagnostics
- XLARGE breakpoint tier for responsive UI on larger displays
- Responsive fan card rendering with dynamic arc sizing and tiny breakpoint support
- Unified responsive icon sizing via design tokens
- Geralkom X400/X500 and Voron Micron added to printer database
- HelixScreen Discord community link in documentation

### Fixed
- Overlay close callback deferred to prevent use-after-free crash (#70)
- macOS build error caused by libhv gettid() conflict

### Changed
- 182 missing translation keys added across the UI
- Navigation bar width moved from C++ to XML for declarative layout control
- Qidi and Creality printer images updated; Qidi Q2 Pro removed

## [0.9.18] - 2026-02-13

### Added
- Actionable notifications: tapping notification history items now dispatches their associated action (e.g. navigate to update panel)
- Skipped-update notifications persist in notification history with tap-to-navigate

### Fixed
- LED macro integration: macro backend now correctly tracks LED state and handles device transitions
- Pre-rendered generic printer images updated with correct corexy model

## [0.9.17] - 2026-02-13

### Added
- Full LED control system with four backends, auto-state mapping editor, macro device configuration, and settings overlay
- Crash report dialog with automatic submission, QR code for manual upload, and local file fallback
- Layer estimation from print progress when slicer lacks SET_PRINT_STATS_INFO (#37)
- Rate limiting on crash and telemetry ingest workers

### Fixed
- Crash reporter now shows modal before TelemetryManager consumes the crash file
- LED strip auto-selection on first discovery, lazy LED reads, icon and dropdown fixes
- Installer config file operations use minimal permissions instead of broad sudo

### Changed
- Motion overlay refactored to declarative UI with homing indicators and theme colors
- LED settings layout extracted to reusable XML components
- User guide restructured into sub-pages with new screenshots

## [0.9.16] - 2026-02-12

### Added
- Printer Manager overlay accessible from home screen with tap-to-open, custom printer images, inline name editing, and capability chips
- Theme-aware markdown viewer
- Custom printer image selection with import support and list+preview layout

### Fixed
- Setup wizard now defaults IP to 127.0.0.1 for local Moonraker connections
- Whitespace in IP and port input fields no longer causes validation errors

### Changed
- All modals standardized to use the Modal system with ui_dialog
- AMS modals refactored to use modal_button_row component
- Release assets now include install.sh (thanks @Major_Buzzkill)
- Markdown submodule updated with faux bold fix

## [0.9.15] - 2026-02-12

### Fixed
- Touchscreen calibration wizard no longer appears on capacitive displays (#40)
- Calibration verify step now applies new calibration so accept/retry buttons are tappable
- Debug logging via HELIX_DEBUG=1 in env file now works correctly after sourcing order fix
- Release pipeline R2 upload failing when changelog contains special characters
- Symbol resolution script using wrong domain (releases.helixscreen.com → .org)
- User docs referencing `--help | head -1` instead of `--version` for version checks

## [0.9.14] - 2026-02-12

### Fixed
- Installer fails on systems without hexdump (e.g., Armbian) with "Cannot read binary header" error

## [0.9.13] - 2026-02-11

### Added
- Frequency response charts for input shaper calibration with shaper overlay toggles
- CSV parser for Klipper calibration frequency response data
- Filament usage tracking with live consumption during printing and slicer estimates on completion modal
- Unified error modal with declarative subjects and single suppression
- Ultrawide home panel layout for 1920x480 displays
- Internationalization support for header bar and overlay panel titles
- Demo mode for PID and input shaper calibration screenshots
- Klipper/Moonraker pre-flight check in AD5M and K1 installers

### Fixed
- getcwd errors during AD5M startup (#36)
- Installer permission denied on tar extraction cleanup (#34)
- Print tune panel layout adjusted to fit 800x480 screens
- CLI hyphen normalization for layout names

### Changed
- Input shaper graduated from beta to stable
- Width-aware Bresenham line drawing for G-code layer renderer
- Overlay content padding standardized across panels
- Action button widths use percentages instead of hardcoded pixels

## [0.9.12] - 2026-02-11

### Added
- QIDI printer support with detection heuristics and print start profile
- Snapmaker U1 cross-compile target, printer detection, and platform support
- Layout manager with auto-detection for alternative screen sizes and CLI override
- Input shaper panel redesigned with config display, pre-flight checks, per-axis results, and save button
- PID calibration: live temperature graph, progress tracking, old value deltas, abort support, and 15-minute timeout
- Multi-LED chip selection in settings replacing single dropdown
- Macro browser (gated behind beta features)

### Fixed
- Crash on shutdown from re-entrant Moonraker method callback map destruction
- Installer: BusyBox echo compatibility for ANSI colors and temp directory auto-detection
- Missing translations for telemetry, sound, PID, and timelapse strings
- Unwanted borders on navigation bar and home status card buttons
- Scroll-on-focus in plugin install modal
- Beta feature flag conflict hiding hardware check rows in advanced settings

### Changed
- PID calibration ungated from beta features — now available to all users
- Moonraker API abstraction boundary enforced — UI no longer accesses WebSocket client directly
- Test-only methods moved to friend test access pattern (cleaner production API)

## [0.9.11] - 2026-02-10

Sound system, KIAUH installer, display rotation, PID tuning, and timelapse support — plus a splash screen fix for AD5M.

### Added
- Sound system with multi-backend synthesizer engine (SDL audio, PWM sysfs, M300 G-code), JSON sound themes (minimal, retro chiptune), toggle sounds, and theme preview
- Sound settings overlay with volume slider and test beep on release
- KIAUH installer integration for one-click install from KIAUH menu
- Display rotation support for all three binaries (main, splash, watchdog)
- PID tuning calibration with fan control and material presets
- Timelapse plugin detection, install wizard, and settings UI (beta)
- Versioned config migration system
- Shadow support and consistent borders across widgets
- Platform-aware cache directory resolution for embedded targets
- Telemetry analytics pipeline with admin API, pull script, and analyzer

### Fixed
- Splash process not killed on AD5M when pre-started by init script (display flashing)
- Layer count tracking with G-code response fallback
- Print file list 15-second polling fallback for missed WebSocket updates
- Display wake from sleep on SDL click
- Translation sync with extractor false-positive cleanup
- Cross-compiled binaries now auto-stripped after linking
- Build system tracks lv_conf.h as LVGL compile prerequisite
- LayerTracker debug log spam reduced to change-only logging

### Changed
- Sound system and timelapse gated behind beta features flag
- Bug report and feature request GitHub issue templates added

## [0.9.10] - 2026-02-10

Hotfix release — gradient and flag images were broken for all users due to a missing decoder setting, and WiFi initialization caused a 5-second startup delay on NetworkManager-based systems.

### Added
- Optional bed warming step before Z-offset calibration
- Reusable multi-select checkbox widget
- Symbol maps for crash backtrace resolution
- KIAUH extension discovery tests

### Fixed
- Gradient and flag images failing to load (LV_BIN_DECODER_RAM_LOAD not enabled)
- WiFi backend now tries NetworkManager first, avoiding 5-second wpa_supplicant timeout on most systems
- Observer crash on shutdown from subject lifetime mismatch
- Connection wizard mDNS section hidden, subtitle improved

### Changed
- Project permission settings organized into .claude/settings.json

## [0.9.9] - 2026-02-09

Telemetry, security hardening, and a bundled uninstaller — plus deploy packages are now ~60% smaller.

### Added
- Anonymous opt-in telemetry with crash reporting, session recording, and auto-send scheduler
- Hardware survey enrichment for telemetry sessions (schema v2)
- Telemetry opt-in step in setup wizard with info modal
- Cloudflare Worker telemetry backend
- Bundled uninstaller with 151 shell tests
- Creality K2 added to GitHub release workflow

### Fixed
- Framebuffer garbage on home panel from missing container background
- Observer crash on quit from subject/display deinit ordering
- Stale subject pointers in ToastManager and WizardTouchCalibration on shutdown
- Print thumbnail offset and outcome overlay centering
- Confetti particle system rewritten to use native LVGL objects
- Print card thumbnail overlap — e-stop relocated to print card
- Auto-navigation to print status suppressed during setup wizard
- KIAUH extension discovery uses native import paths (fixes #30)
- Data root auto-detected from binary path with missing globals.xml abort
- NaN/Inf guards on all G-code generation paths
- Safe restart via absolute argv[0] path resolution
- Replaced system() with fork/execvp in ping_host()
- Tightened directory permissions, replaced strcpy with memcpy
- K2 musl cross-compilation LDFLAGS
- Telemetry opt-in enforced for crash events
- Telemetry enabled state synced at startup with API key auth

### Changed
- Deploy footprint reduced ~60% with asset excludes and LZ4 image compression
- Shell test gate added to release workflow

## [0.9.8] - 2026-02-09

### Added
- G-code toolpath render uses AMS/Spoolman filament colors for accurate color previews
- Reprint button shown for all terminal print states (error, cancelled, complete)
- Config symlinked into printer_data for editing via Mainsail/Fluidd file manager
- Async button timeout guard to prevent stuck UI on failed operations
- 35 new translation strings synced across all languages

### Fixed
- Slicer time estimate preserved across reprints instead of resetting to zero
- Install directory ownership for Moonraker update manager (fixes #29)
- Python 3.9 compatibility for Sonic Pad KIAUH integration (fixes #28)
- Display sleep using software overlay for unrecognized display hardware (#23)
- Z-offset controls compacted for small displays (#27)
- Print error state handled with badge, reprint button, and automatic heater shutoff
- WebSocket callbacks deferred to main thread preventing UI race conditions
- Responsive breakpoints based on screen height instead of max dimension
- Cooldown button uses TURN_OFF_HEATERS for reliable heater shutoff
- Splash screen support for ultra-wide displays
- 32-bit userspace detection on 64-bit Pi kernels
- Graph Y-axis label no longer clips top padding
- Print card info column taps now navigate to status screen
- Watchdog double-instance prevented on supervised restart
- Internal splash skipped when external splash process is running
- Resolution auto-detection enabled at startup

### Changed
- Z-offset scale layout dynamically adapts to measured label widths
- Filament panel temperature updates are targeted instead of full-refresh
- Machine limits G-code debounced to reduce unnecessary sends
- Delete button on print detail uses danger styling

## [0.9.7] - 2026-02-08

Z-offset calibration redesigned from scratch with a Prusa-style visual meter,
plus display reliability fixes and hardware detection improvements.

### Added
- Z-offset calibration overhaul: Prusa-style vertical meter with draw-in arrow animation, horizontal step buttons, auto-ranging scale, saved offset display, and auto-navigation when calibration is in progress
- Z-offset calibration strategy system for printer-specific save commands
- Automatic update notifications with dismiss support
- Sleep While Printing toggle to keep display on during prints
- Hardware detection: mainboard identification heuristic, non-printer addon exclusion, and kinematics filtering
- Calibration features gated behind beta feature flag

### Fixed
- Crash from rapid filament load/unload button presses
- Crash dialog not initializing touch calibration config
- Keyboard shortcuts firing when typing in text inputs
- Parent directory (..) not always sorted first in file browser
- Splash screen crash when prerendered assets missing
- Console bleed-through on fbdev displays
- Display not repainting fully after wake from sleep
- Moonraker updates switched from git_repo to zip type for reliability
- Thumbnail format forced to ARGB8888 for correct rendering
- Print outcome badges misaligned above thumbnail
- Scroll-on-focus causing unwanted panel jumps
- Install service filtering to only existing system groups
- Screws tilt adjust detection from configfile fallback
- Wizard saving literal 'None' instead of empty string for unselected hardware
- Mock printer kinematics matching actual printer type
- Touch calibration detection unified with headless pointer support

### Changed
- Dark mode applies live without restart
- Calibration button layout redesigned Mainsail-style
- Textarea widgets migrated to text_input component
- Redundant kinematics polling eliminated

## [0.9.6] - 2026-02-08

### Added
- Per-object G-code toolpath thumbnails in Print Objects overlay
- AFC (Armored Turtle) support: live device state, tool mapping, endless spool, per-lane reset, maintenance and LED controls, quiet mode, and mock simulation
- Active object count shown on layer progress line during printing
- Change Host modal for switching Moonraker connection in settings
- Z movement style override setting and E-Stop relocated to Motion section
- K1 dynamic linking toolchain and build target
- Creality K2 series cross-compilation target (ARM, static musl — untested, needs hardware validation)
- CDN-first installer downloads with GitHub fallback
- Multi-channel R2 update distribution with GitHub API fallback

### Fixed
- Toasts now render on top layer instead of active screen (fixes toasts hidden behind overlays)
- Print cancel timeout increased to 15s with active state observation for more reliable cancellation
- Pre-print time estimates seeded from slicer data with blended early progress
- Thread-safe slicer estimate seeding during print start
- G-code viewer cache thrash from current_object changes during exclude-object prints
- ForgeX startup framebuffer stomping by S99root init script
- Wrong-platform binary install prevented with ELF architecture validation and safe rollback
- Use-after-free crash on Print Objects overlay close
- Isometric thumbnail rendering with shared projection, depth shading, and thicker lines
- Install warning text centered in update download modal
- Missing alert_circle icon codepoint
- Settings About section consolidated with cleaner version row layout
- Z baby step icons and color swatch labels
- Exclude object mock mode: objects populated from G-code on print start with proper status dispatch

## [0.9.5] - 2026-02-07

### Added
- Exclude object support for streaming/2D mode with selection brackets and long-press interaction
- Print Objects list overlay showing defined objects during a print
- LED selection dropdown in settings for multi-LED printers
- Version number displayed on splash screen
- Beta and dev update channels with UI toggle and R2 upload script
- Beta feature wrapper component with badge indicator
- 32-bit ARM (armv7l) Raspberry Pi build target (#10)
- Auto-publish tagged releases to R2 with platform detection
- Exclude object G-code parsing and status dispatch in mock mode

### Fixed
- Use-after-free race in wpa_supplicant backend shutdown (#8)
- Deadlock in Happy Hare and ToolChanger AMS backend start (#9)
- DNS resolver fallback for static glibc builds
- Crash when navigating folders during metadata fetch in print selection
- LED detection excluding toolhead LEDs from main LED control
- WebSocket max message size increased from 1MB to 5MB (#7)
- Elapsed/remaining time display during mock printing
- Crash on window close from SDL event handling during shutdown
- Accidental scroll taps by increasing scroll limit default
- G-code parser now reads layer_height, first_layer_height, object_height from metadata
- Invalid text_secondary color token replaced with text_muted
- KIAUH metadata wrapper key and moonraker updater path (#3)
- Installer sparse checkout for updater repo (#11)
- Output_pin lights detected as LEDs with fallback to first LED (#14)
- Percentage rounding instead of truncating to fix float precision (#14)
- Z offset display sync when print tune overlay opens (#14)
- CoreXZ treated as gantry-moves-Z instead of bed-moves (#14)

### Changed
- Log levels cleaned up: INFO is concise, DEBUG is useful without per-layer/shutdown spam
- Duplicate log bugs fixed (PrintStartCollector double-completion, PluginManager double-unload)
- Settings panel version rows deduplicated
- Exclude object modal XML registration and single-select behavior

## [0.9.4] - 2026-02-07

### Added
- Pre-print time predictions based on historical heating/homing data
- Heater status text on temperature cards (Heating, Cooling, At Target)
- Slicer estimated time fallback for remaining time
- Seconds in duration display under 5 minutes

### Fixed
- Crash on 16bpp HDMI screens from forced 32-bit color format
- Elapsed time using wall-clock duration instead of print-only time
- Pre-print overlay showing when it shouldn't
- Backlight not turning off on AD5M
- Heater status colors (heating=red, added cooling state)
- AMS row hidden when no AMS connected
- Modal button alignment
- Install script version detection on Pi (#6)

## [0.9.3] - 2026-02-06

First public beta release. Core features are complete — we're looking for early
adopters to help find edge cases.

**Supported platforms:** Raspberry Pi (aarch64), FlashForge AD5M (armv7l),
Creality K1 (MIPS32)

> **Note:** K1 binaries are included but have not been tested on hardware. If you
> have a K1, we'd love your help verifying it works!

### Added
- Print start profiles with modular, JSON-driven signal matching for per-printer phase detection
- NetworkManager WiFi backend for broader Linux compatibility
- `.3mf` file support in print file browser
- Non-printable file filtering in print selection
- Beta features gating system for experimental UI (HelixPrint plugin)
- Platform detection and preset system for zero-config installs
- Settings action rows with bind_description for richer UI
- Restart logic consolidated into single `app_request_restart_service()` entry point

### Fixed
- Print start collector not restarting after a completed print
- Sequential progress regression on repeated signals during print start
- Bed mesh triple-rendering and profile row click targets
- Wizard WiFi step layout, password visibility toggle, and dropdown corruption
- Touch calibration skipped for USB HID touchscreens (HDMI displays)
- CJK glyph inclusion from C++ sources in font generation
- File ownership for non-root deploy targets
- Console cursor hidden on fbdev displays

### Changed
- Pi deploys now use `systemctl restart` instead of stop/start
- fbdev display backend for Pi (avoids DRM master contention)
- Comprehensive architectural documentation from 5-agent audit
- Troubleshooting guide updated with debug logging instructions

## [0.9.2] - 2026-02-05

Major internal release with live theming, temperature sensor support, and
extensive UI polish across all panels.

### Added
- Live theme switching without restart — change themes in settings instantly
- Dark/light gradient backgrounds and themed overlay constants
- Full-screen 3D splash images with dark/light mode support
- Temperature sensor manager for auxiliary temp sensors (chamber, enclosure, etc.)
- Responsive fan dial with knob glow effect
- Software update checker with download progress and install-during-idle safety
- Platform hook architecture for modularized installer functions
- Auto-detect Pi install path from Klipper ecosystem
- AD5M preset with auto-detection for zero-config setup
- Beta features config flag for gating experimental UI
- CJK glyph support (Chinese, Japanese, Russian) in generated fonts
- Pencil edit icons next to temperature controls
- OS version, MCU versions, and printer name in About section
- Shell tests (shellcheck, bats) gating release builds

### Fixed
- Shutdown crash: stop animations before destroying panels to prevent use-after-free
- Observer crash: reorder display/subject teardown sequence
- Stale widget pointer guards for temperature and fan updates
- Theme palette preservation across dark/light mode switches
- Button text contrast for layout=column buttons with XML children
- Navbar background not updating on theme toggle
- Dropdown corruption with `&#10;` newline entities in XML
- Wizard initialization: fan subscriptions, sensor select, toast suppression
- Kinematics detection and Z button icons for bed-moves printers
- Bed mesh data normalization and zero plane visibility
- Filament panel deferred `set_limits` to main thread
- Touch calibration target spread and full-screen capture

### Changed
- Pi builds target Debian Bullseye for wider compatibility
- Static-link OpenSSL for cross-platform SSL support
- Binaries relocated to `bin/` subdirectory in deploy packages
- Fan naming uses configured roles instead of heuristics
- HelixScreen brand theme set as default
- Installer modularized with platform dispatchers
- Release build timeout increased to 60 minutes

## [0.9.1] - 2026-02-04

Initial tagged release. Foundation for all subsequent development.

### Added
- 30 panels and 16 overlays covering full printer control workflow
- First-run setup wizard with 8-step guided configuration
- Multi-material support: AFC, Happy Hare, tool changers, ValgACE, Spoolman
- G-code preview and 3D bed mesh visualization
- Calibration tools: input shaper, mesh leveling, screws tilt, PID, firmware retraction
- Internationalization system with hot-reload language switching
- Light and dark themes with responsive 800x480+ layout
- Cross-compilation for Pi (aarch64), AD5M (armv7l), K1 (MIPS32)
- Automated GitHub Actions release pipeline
- One-liner installation script with platform auto-detection

[0.99.56]: https://github.com/prestonbrown/helixscreen/compare/v0.99.55...v0.99.56
[0.99.55]: https://github.com/prestonbrown/helixscreen/compare/v0.99.54...v0.99.55
[0.99.54]: https://github.com/prestonbrown/helixscreen/compare/v0.99.53...v0.99.54
[0.99.53]: https://github.com/prestonbrown/helixscreen/compare/v0.99.52...v0.99.53
[0.99.52]: https://github.com/prestonbrown/helixscreen/compare/v0.99.51...v0.99.52
[0.99.51]: https://github.com/prestonbrown/helixscreen/compare/v0.99.50...v0.99.51
[0.99.50]: https://github.com/prestonbrown/helixscreen/compare/v0.99.49...v0.99.50
[0.99.49]: https://github.com/prestonbrown/helixscreen/compare/v0.99.48...v0.99.49
[0.99.48]: https://github.com/prestonbrown/helixscreen/compare/v0.99.47...v0.99.48
[0.99.47]: https://github.com/prestonbrown/helixscreen/compare/v0.99.46...v0.99.47
[0.99.46]: https://github.com/prestonbrown/helixscreen/compare/v0.99.45...v0.99.46
[0.99.45]: https://github.com/prestonbrown/helixscreen/compare/v0.99.44...v0.99.45
[0.99.44]: https://github.com/prestonbrown/helixscreen/compare/v0.99.43...v0.99.44
[0.99.43]: https://github.com/prestonbrown/helixscreen/compare/v0.99.42...v0.99.43
[0.99.42]: https://github.com/prestonbrown/helixscreen/compare/v0.99.41...v0.99.42
[0.99.41]: https://github.com/prestonbrown/helixscreen/compare/v0.99.40...v0.99.41
[0.99.40]: https://github.com/prestonbrown/helixscreen/compare/v0.99.39...v0.99.40
[0.99.39]: https://github.com/prestonbrown/helixscreen/compare/v0.99.38...v0.99.39
[0.99.38]: https://github.com/prestonbrown/helixscreen/compare/v0.99.37...v0.99.38
[0.99.37]: https://github.com/prestonbrown/helixscreen/compare/v0.99.36...v0.99.37
[0.99.36]: https://github.com/prestonbrown/helixscreen/compare/v0.99.35...v0.99.36
[0.99.35]: https://github.com/prestonbrown/helixscreen/compare/v0.99.34...v0.99.35
[0.99.34]: https://github.com/prestonbrown/helixscreen/compare/v0.99.33...v0.99.34
[0.99.33]: https://github.com/prestonbrown/helixscreen/compare/v0.99.32...v0.99.33
[0.99.32]: https://github.com/prestonbrown/helixscreen/compare/v0.99.31...v0.99.32
[0.99.31]: https://github.com/prestonbrown/helixscreen/compare/v0.99.30...v0.99.31
[0.99.30]: https://github.com/prestonbrown/helixscreen/compare/v0.99.29...v0.99.30
[0.99.29]: https://github.com/prestonbrown/helixscreen/compare/v0.99.28...v0.99.29
[0.99.28]: https://github.com/prestonbrown/helixscreen/compare/v0.99.27...v0.99.28
[0.99.27]: https://github.com/prestonbrown/helixscreen/compare/v0.99.26...v0.99.27
[0.99.26]: https://github.com/prestonbrown/helixscreen/compare/v0.99.25...v0.99.26
[0.99.25]: https://github.com/prestonbrown/helixscreen/compare/v0.99.24...v0.99.25
[0.99.24]: https://github.com/prestonbrown/helixscreen/compare/v0.99.23...v0.99.24
[0.99.23]: https://github.com/prestonbrown/helixscreen/compare/v0.99.22...v0.99.23
[0.99.22]: https://github.com/prestonbrown/helixscreen/compare/v0.99.21...v0.99.22
[0.99.21]: https://github.com/prestonbrown/helixscreen/compare/v0.99.20...v0.99.21
[0.99.20]: https://github.com/prestonbrown/helixscreen/compare/v0.99.19...v0.99.20
[0.99.19]: https://github.com/prestonbrown/helixscreen/compare/v0.99.18...v0.99.19
[0.99.18]: https://github.com/prestonbrown/helixscreen/compare/v0.99.17...v0.99.18
[0.99.17]: https://github.com/prestonbrown/helixscreen/compare/v0.99.16...v0.99.17
[0.99.16]: https://github.com/prestonbrown/helixscreen/compare/v0.99.15...v0.99.16
[0.99.15]: https://github.com/prestonbrown/helixscreen/compare/v0.99.14...v0.99.15
[0.99.14]: https://github.com/prestonbrown/helixscreen/compare/v0.99.13...v0.99.14
[0.99.13]: https://github.com/prestonbrown/helixscreen/compare/v0.99.12...v0.99.13
[0.99.12]: https://github.com/prestonbrown/helixscreen/compare/v0.99.11...v0.99.12
[0.99.11]: https://github.com/prestonbrown/helixscreen/compare/v0.99.10...v0.99.11
[0.99.10]: https://github.com/prestonbrown/helixscreen/compare/v0.99.9...v0.99.10
[0.99.9]: https://github.com/prestonbrown/helixscreen/compare/v0.99.8...v0.99.9
[0.99.8]: https://github.com/prestonbrown/helixscreen/compare/v0.99.7...v0.99.8
[0.99.7]: https://github.com/prestonbrown/helixscreen/compare/v0.99.6...v0.99.7
[0.99.6]: https://github.com/prestonbrown/helixscreen/compare/v0.99.5...v0.99.6
[0.99.5]: https://github.com/prestonbrown/helixscreen/compare/v0.99.4...v0.99.5
[0.99.4]: https://github.com/prestonbrown/helixscreen/compare/v0.99.3...v0.99.4
[0.99.3]: https://github.com/prestonbrown/helixscreen/compare/v0.99.2...v0.99.3
[0.99.2]: https://github.com/prestonbrown/helixscreen/compare/v0.99.1...v0.99.2
[0.99.1]: https://github.com/prestonbrown/helixscreen/compare/v0.99.0...v0.99.1
[0.99.0]: https://github.com/prestonbrown/helixscreen/compare/v0.98.12...v0.99.0
[0.98.12]: https://github.com/prestonbrown/helixscreen/compare/v0.98.11...v0.98.12
[0.98.11]: https://github.com/prestonbrown/helixscreen/compare/v0.98.10...v0.98.11
[0.98.10]: https://github.com/prestonbrown/helixscreen/compare/v0.98.9...v0.98.10
[0.98.9]: https://github.com/prestonbrown/helixscreen/compare/v0.98.8...v0.98.9
[0.98.8]: https://github.com/prestonbrown/helixscreen/compare/v0.98.7...v0.98.8
[0.98.7]: https://github.com/prestonbrown/helixscreen/compare/v0.98.6...v0.98.7
[0.98.6]: https://github.com/prestonbrown/helixscreen/compare/v0.98.5...v0.98.6
[0.98.5]: https://github.com/prestonbrown/helixscreen/compare/v0.98.4...v0.98.5
[0.98.4]: https://github.com/prestonbrown/helixscreen/compare/v0.98.3...v0.98.4
[0.98.3]: https://github.com/prestonbrown/helixscreen/compare/v0.98.2...v0.98.3
[0.98.2]: https://github.com/prestonbrown/helixscreen/compare/v0.98.1...v0.98.2
[0.98.1]: https://github.com/prestonbrown/helixscreen/compare/v0.98.0...v0.98.1
[0.98.0]: https://github.com/prestonbrown/helixscreen/compare/v0.97.5...v0.98.0
[0.97.5]: https://github.com/prestonbrown/helixscreen/compare/v0.97.4...v0.97.5
[0.97.4]: https://github.com/prestonbrown/helixscreen/compare/v0.97.3...v0.97.4
[0.97.3]: https://github.com/prestonbrown/helixscreen/compare/v0.97.2...v0.97.3
[0.97.2]: https://github.com/prestonbrown/helixscreen/compare/v0.97.1...v0.97.2
[0.97.1]: https://github.com/prestonbrown/helixscreen/compare/v0.97.0...v0.97.1
[0.97.0]: https://github.com/prestonbrown/helixscreen/compare/v0.96.9...v0.97.0
[0.96.9]: https://github.com/prestonbrown/helixscreen/compare/v0.96.8...v0.96.9
[0.96.8]: https://github.com/prestonbrown/helixscreen/compare/v0.96.7...v0.96.8
[0.96.7]: https://github.com/prestonbrown/helixscreen/compare/v0.96.5...v0.96.7
[0.96.5]: https://github.com/prestonbrown/helixscreen/compare/v0.96.4...v0.96.5
[0.96.4]: https://github.com/prestonbrown/helixscreen/compare/v0.96.3...v0.96.4
[0.96.3]: https://github.com/prestonbrown/helixscreen/compare/v0.96.2...v0.96.3
[0.96.2]: https://github.com/prestonbrown/helixscreen/compare/v0.96.1...v0.96.2
[0.96.1]: https://github.com/prestonbrown/helixscreen/compare/v0.96.0...v0.96.1
[0.96.0]: https://github.com/prestonbrown/helixscreen/compare/v0.95.3...v0.96.0
[0.95.3]: https://github.com/prestonbrown/helixscreen/compare/v0.95.2...v0.95.3
[0.95.2]: https://github.com/prestonbrown/helixscreen/compare/v0.95.1...v0.95.2
[0.95.1]: https://github.com/prestonbrown/helixscreen/compare/v0.95.0...v0.95.1
[0.95.0]: https://github.com/prestonbrown/helixscreen/compare/v0.13.13...v0.95.0
[0.13.13]: https://github.com/prestonbrown/helixscreen/compare/v0.13.12...v0.13.13
[0.13.12]: https://github.com/prestonbrown/helixscreen/compare/v0.13.11...v0.13.12
[0.13.11]: https://github.com/prestonbrown/helixscreen/compare/v0.13.10...v0.13.11
[0.13.10]: https://github.com/prestonbrown/helixscreen/compare/v0.13.9...v0.13.10
[0.13.9]: https://github.com/prestonbrown/helixscreen/compare/v0.13.8...v0.13.9
[0.13.8]: https://github.com/prestonbrown/helixscreen/compare/v0.13.7...v0.13.8
[0.13.7]: https://github.com/prestonbrown/helixscreen/compare/v0.13.6...v0.13.7
[0.13.6]: https://github.com/prestonbrown/helixscreen/compare/v0.13.5...v0.13.6
[0.13.5]: https://github.com/prestonbrown/helixscreen/compare/v0.13.4...v0.13.5
[0.13.4]: https://github.com/prestonbrown/helixscreen/compare/v0.13.3...v0.13.4
[0.13.3]: https://github.com/prestonbrown/helixscreen/compare/v0.13.2...v0.13.3
[0.13.2]: https://github.com/prestonbrown/helixscreen/compare/v0.13.1...v0.13.2
[0.13.1]: https://github.com/prestonbrown/helixscreen/compare/v0.13.0...v0.13.1
[0.13.0]: https://github.com/prestonbrown/helixscreen/compare/v0.12.1...v0.13.0
[0.12.1]: https://github.com/prestonbrown/helixscreen/compare/v0.12.0...v0.12.1
[0.12.0]: https://github.com/prestonbrown/helixscreen/compare/v0.11.1...v0.12.0
[0.11.1]: https://github.com/prestonbrown/helixscreen/compare/v0.11.0...v0.11.1
[0.11.0]: https://github.com/prestonbrown/helixscreen/compare/v0.10.14...v0.11.0
[0.10.14]: https://github.com/prestonbrown/helixscreen/compare/v0.10.13...v0.10.14
[0.10.13]: https://github.com/prestonbrown/helixscreen/compare/v0.10.12...v0.10.13
[0.10.12]: https://github.com/prestonbrown/helixscreen/compare/v0.10.11...v0.10.12
[0.10.11]: https://github.com/prestonbrown/helixscreen/compare/v0.10.10...v0.10.11
[0.10.10]: https://github.com/prestonbrown/helixscreen/compare/v0.10.9...v0.10.10
[0.10.9]: https://github.com/prestonbrown/helixscreen/compare/v0.10.8...v0.10.9
[0.10.8]: https://github.com/prestonbrown/helixscreen/compare/v0.10.7...v0.10.8
[0.10.7]: https://github.com/prestonbrown/helixscreen/compare/v0.10.6...v0.10.7
[0.10.6]: https://github.com/prestonbrown/helixscreen/compare/v0.10.5...v0.10.6
[0.10.5]: https://github.com/prestonbrown/helixscreen/compare/v0.10.4...v0.10.5
[0.10.4]: https://github.com/prestonbrown/helixscreen/compare/v0.10.3...v0.10.4
[0.10.3]: https://github.com/prestonbrown/helixscreen/compare/v0.10.2...v0.10.3
[0.10.2]: https://github.com/prestonbrown/helixscreen/compare/v0.10.1...v0.10.2
[0.10.1]: https://github.com/prestonbrown/helixscreen/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/prestonbrown/helixscreen/compare/v0.9.24...v0.10.0
[0.9.24]: https://github.com/prestonbrown/helixscreen/compare/v0.9.23...v0.9.24
[0.9.23]: https://github.com/prestonbrown/helixscreen/compare/v0.9.22...v0.9.23
[0.9.22]: https://github.com/prestonbrown/helixscreen/compare/v0.9.21...v0.9.22
[0.9.21]: https://github.com/prestonbrown/helixscreen/compare/v0.9.20...v0.9.21
[0.9.20]: https://github.com/prestonbrown/helixscreen/compare/v0.9.19...v0.9.20
[0.9.19]: https://github.com/prestonbrown/helixscreen/compare/v0.9.18...v0.9.19
[0.9.18]: https://github.com/prestonbrown/helixscreen/compare/v0.9.17...v0.9.18
[0.9.17]: https://github.com/prestonbrown/helixscreen/compare/v0.9.16...v0.9.17
[0.9.16]: https://github.com/prestonbrown/helixscreen/compare/v0.9.15...v0.9.16
[0.9.15]: https://github.com/prestonbrown/helixscreen/compare/v0.9.14...v0.9.15
[0.9.14]: https://github.com/prestonbrown/helixscreen/compare/v0.9.13...v0.9.14
[0.9.13]: https://github.com/prestonbrown/helixscreen/compare/v0.9.12...v0.9.13
[0.9.12]: https://github.com/prestonbrown/helixscreen/compare/v0.9.11...v0.9.12
[0.9.11]: https://github.com/prestonbrown/helixscreen/compare/v0.9.10...v0.9.11
[0.9.10]: https://github.com/prestonbrown/helixscreen/compare/v0.9.9...v0.9.10
[0.9.9]: https://github.com/prestonbrown/helixscreen/compare/v0.9.8...v0.9.9
[0.9.8]: https://github.com/prestonbrown/helixscreen/compare/v0.9.7...v0.9.8
[0.9.7]: https://github.com/prestonbrown/helixscreen/compare/v0.9.6...v0.9.7
[0.9.6]: https://github.com/prestonbrown/helixscreen/compare/v0.9.5...v0.9.6
[0.9.5]: https://github.com/prestonbrown/helixscreen/compare/v0.9.4...v0.9.5
[0.9.4]: https://github.com/prestonbrown/helixscreen/compare/v0.9.3...v0.9.4
[0.9.3]: https://github.com/prestonbrown/helixscreen/compare/v0.9.2...v0.9.3
[0.9.2]: https://github.com/prestonbrown/helixscreen/compare/v0.9.1...v0.9.2
[0.9.1]: https://github.com/prestonbrown/helixscreen/releases/tag/v0.9.1

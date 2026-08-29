# RC follow-up — deliberately deferred from the v0.99.117..HEAD review

**Status:** in progress. **Branch:** `fix/rc-followup`.
**Origin:** the pre-1.0-RC review. The regressions from that range are already fixed and merged (`0c5adc32b`). Everything here was found by the same review but deliberately left out of that branch — it is either pre-existing, out of scope for the RC, or cleanup.

Every claim below was verified in the code during the review. File:line references were accurate at `0c5adc32b`; re-check them before editing rather than trusting them.

**Standing rule: NO BULLSHIT TESTS.** A real test FAILS if the feature is removed. For every test written here, state the mutation that makes it fail. Vacuous assertions (`REQUIRE(true)`, unsigned `>= 0`, a value compared to itself), happy-path-only tests, tests that never reach the code under test, and `REQUIRE_THROWS` through a deferred/queued path (vacuous here — assert on the synchronous entry point) are all rejected.

---

## 1. Two use-after-frees, same family (highest value)

Both are the `LV_EVENT_DELETE`-hook-carrying-`this` pattern tracked by `#1298`, and both are the exact shape of the ASan/TSan-confirmed `PowerPanel` bug fixed in `b0afff654`. The mechanism in both: a hook carrying `this`, plus a **deferred** delete, so the callback fires after the object is gone. Pre-existing — shipped in .116/.117.

**1a. `NetworkSettingsOverlay`** — `src/ui/ui_overlay_network_settings.cpp:1058, 1089, 1162, 1577`

Four modals get `lv_obj_add_event_cb(..., on_modal_deleted, LV_EVENT_DELETE, this)`. The destructor never uninstalls any of them; it calls `modal_hide()`, which routes to `ModalStack::animate_exit()` (`src/ui/ui_modal.cpp:355`) — an exit *animation*. The tree is deleted when the animation completes, well after `~NetworkSettingsOverlay()` has returned and freed `this`. `on_modal_deleted` then writes through the freed pointer (`*cached = nullptr` over `self->password_modal_` et al.).

Triggers on normal navigation away from network settings with a modal open, and again at shutdown.

**1b. `PowerDeviceWidget`** — `src/ui/widgets/power_device_widget.cpp:778`

The picker backdrop carries a `this`-hook. `~PowerDeviceWidget()` → `detach()` → `dismiss_device_picker()`, which calls `helix::ui::safe_delete_deferred(backdrop)` — deferred by definition. The lambda later does `self->picker_backdrop_ = nullptr` and `s_active_picker_ == self` on freed memory. `PanelWidget` instances are recycled across rebuilds, so this detach/attach cycling is routine.

**Fix for both:** uninstall before the deferred delete, matching what `~PowerPanel()` already does — `lv_obj_remove_event_cb_with_user_data(widget, cb, this)`. Note `ui_panel_bed_mesh.cpp:127` solves the same problem with plain `lv_obj_remove_event_cb()` and a good explanatory comment; either is fine, be consistent with the file.

**Test:** `tests/unit/test_power_panel_teardown_uaf.cpp` is the model — it reads LVGL's event list directly rather than trying to trigger the crash, so it fails in a plain build without needing ASan. Do the same here.

---

## 2. The 3D selection tag is walls-only

`src/rendering/gcode_geometry_builder.cpp:790-797` gates run collection on `selection::halo_feature()`, which admits only `OuterWall`/`OverhangWall`/`Unknown`. Those runs are exactly what `render_selection_tag()` draws, so `TopSurface`, `SolidInfill`, `InnerWall` and `Bridge` are **never tagged in 3D**.

This contradicts both the 2D path (unfiltered, `src/rendering/gcode_layer_renderer.cpp:934-936`) and the documented rule at `include/gcode_selection_style.h:204-211` ("The tag path does NOT use this and deliberately tags every extrusion").

Traced to abandoned WIP: `2cdfa4129` added the filter for the old inverted-hull shell; `94f1cabb6` replaced the shell with the alpha tag and never revisited the builder.

Effect: viewing a selected object from above on a `;TYPE`-annotated file (PrusaSlicer/Orca, i.e. most), the top face is untagged and the perimeter ring is narrower than `2 * rim_px`, so the ring goes solid white and every hole gets its own ring. On a flat plate viewed from above the silhouette breaks into fragments.

**Fix:** drop the `halo_feature()` condition at `:796` and correct the stale "Walls only … dilated copy" comment above it. Verify visually — GLES cannot run headless, see `reference_gcode_render_mode_selection`.

---

## 3. Doc-citation gate integrity

`scripts/doc_cite_anchors.py` is 1,188 lines that rewrite `.md` files. On *content* it correctly fails closed. Three real holes:

**3a. The `max-unresolved` ceiling is never enforced on the path CI takes.** `scripts/check_doc_refs.py:934` receives `anchor_stats` but reads only `['in_place']` (at `:955`, for a message). It never reads `stats['unresolved']` and never calls `load_ceiling()`. The ceiling lives only in `doc_cite_anchors.py:main()` under `--check`, whose sole caller is `make check-doc-anchors` (`mk/tools.mk:311`) — **which nothing invokes** (verified: it appears only in `.PHONY` and its own target). An unresolvable citation *path* produces no Finding (`:717-726` just bumps a counter), so it is invisible to `quality-checks.sh`, both git hooks, and CI. Currently sitting at 9 unresolved against a ceiling of 9, all benign. ~5 lines: have `check_anchors()` return stats and apply `load_ceiling()`.

**3b. Deleting the sidecar turns the gate green.** `doc_cite_anchors.py:1039` returns 0 with a warning when `scripts/doc_cite_anchors.tsv` is absent, and `check_doc_refs.py` prints it without touching `exit_code`.

**3c. Write is the default, and all three writes truncate in place.** `check_only = args.check or args.diff or args.audit`, then `write = not check_only` — so a bare `python3 scripts/doc_cite_anchors.py` rewrites the corpus, and `--write-baseline` is not in that list either. Writes at `:818-821` (docs), `:589-590` (sidecar), `:642-643` (baseline) truncate with no temp+rename. `tests/shell/test_qc_doc_serialization.bats:17-20` already documents the window; the mitigation chosen was scheduling (`QC_SERIAL`), which closes the parallel-worker race and leaves the interrupt window open — and `--auto-fix` runs inside `.githooks/pre-commit`, where Ctrl-C is normal.

**Fix:** enforce the ceiling; fail closed on a missing sidecar; write via temp+rename. Extend `tests/shell/test_doc_anchors_gate.bats` for each — the gate has meta-tests already, follow their shape.

---

## 4. Fixes that revert green (no regression test)

From the coverage audit: 11 production changes in the reviewed range can be reverted with the suite still passing. The highest-value ones:

| Change | Why it matters |
|---|---|
| `src/remote/remote_control_server.cpp:1257` | The key packs a rank at bit 40 and `long` is 32 bits on **every** 32-bit device target (K1/K2/AD5M/CC1/Pi), where the shift is UB and rank collapses into index. `topmost_visible` has **zero** test references. Breaks `ctl click` on-device — i.e. it silently breaks device-side verification, which is how it hid. |
| `include/lv_draw_buf_guard.h` | New, consolidates the #929 UAF drain and adds the `lv_draw_wait_for_finish()` that was missing from `GCodeGLESRenderer::clear_cached_frame()`. `safe_draw_buf_destroy` has 0 test refs — a UAF fix with no regression test. |
| `src/ui/ui_emergency_stop.cpp:253` | Clears the pending latch on klippy READY. `pending_recovery_reason_` had zero test references before this branch; confirm the new estop tests now cover it, and add one if not. |
| `src/printer/macro_executor.cpp:165-174` | `ExpectedHalt` must return **without** `begin_expected_klippy_restart()`, because that suppresses the recovery dialog and a halted printer needs it promptly. `tests/unit/test_macro_restart_analysis.cpp:222,225` assert only that the classifier *returns* `ExpectedHalt` — the consequence is unprotected. |
| `src/ui/ui_panel_print_status.cpp:1866` | `get_tools_used()` made mode-independent (the U1 streamed-file bug, where streamed files answered empty 100% of the time). Uncovered. |

---

## 5. Cleanup

- **`tool_available()`** — `src/system/update_checker.cpp:362`, anonymous namespace, zero callers. Dead. Surfaced only because the review's build recompiled that TU; incremental builds had been hiding it.
- **`-Wunused-variable status_panel`** — `src/ui/ui_print_start_controller.cpp:271`. Not dead: `get_global_print_status_panel()` lazily constructs the panel and registers its destroy handler, so the line is load-bearing but reads as an accident. Either `(void)` it with a comment saying so, or remove it if construction is guaranteed by that point — check.
- **208 × `-Wmissing-field-initializers`** on `helix::AvailableSlot::unit_display_name` / `multi_color_hexes`, all in `tests/unit/test_filament_mapper.cpp`. **Do not hand-edit 208 call sites.** Prefer one struct-level change (default member initializers on the two fields in `include/filament_mapper.h`) that silences all of them at once. If that does not work, propose a generator + gate rather than a bulk hand-edit.
- **`GCodeLayerIndex::get_entry()`** — `src/rendering/gcode_layer_index.cpp:732` returns `StreamingLayerEntry{0,0,0.0f,0,0}` as its out-of-range sentinel, indistinguishable from a legitimate layer 0 at Z=0. The struct's own comment says a zeroed `start_x/y/z` is exactly the bug those fields were added to prevent. Fine if every caller bounds-checks — audit the callers, then either make the sentinel detectable or document why it is safe.
- **`line_wu` negative-`frac` UB** — `src/rendering/gcode_raster.cpp:85-88`. `static_cast<int>(intery)` truncates toward zero, so `intery ∈ (-1,0)` gives a negative `frac`, making `(1-frac)*255 > 255` and `frac*255 < 0` — both UB converting to `uint8_t`. Screen coords are unclipped, so any model taller than the canvas hits it; `blend_coverage`'s clipping bounds the damage to row 0.
- **`tests/unit/test_ams_device_ops_bypass_guard.cpp:212`** — flaked once in five full-suite runs; passes 10/10 standalone on both binaries. `settle_until` → `wait_until(..., timeout_ms = 5000)` is a **wall-clock** bound (`tests/lvgl_test_fixture.h:127`), so it is load-sensitive. Either raise the bound or make the wait deterministic.

---

## 6. Recommended before tagging (not code)

**Do one full from-scratch build.** Every warning inventory taken during the review came from incremental builds (35 TUs in main's case), so this tree's complete warning set has not actually been seen by anyone. The one compiler warning that *was* read — `-Wtype-limits` — was pointing at a genuinely vacuous test in the shipped suite. Item 5's `tool_available` surfaced the same way, by accident.

## 7. Not in this branch

Test-quality tooling (mirror-test detection, vacuous-assertion gates, mutation-by-hunk-revert, coverage screening) is owned by `feature/test-quality-gates`, per `docs/devel/plans/2026-08-28-test-quality-hardening.md`. Do not duplicate it here. That session has already promoted `-Wtype-limits` to `-Werror` for `tests/`, which may conflict with item 5 — coordinate before touching test warnings.

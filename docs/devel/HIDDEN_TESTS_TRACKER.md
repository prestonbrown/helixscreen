# Hidden Tests Tracker

Hidden tests are excluded from normal runs using Catch2's `[.]` tag prefix. They exist for legitimate reasons (benchmarks, stress tests, destructive global-state cycles, tests that need the `ui_xml/` tree on disk) and should be run manually when relevant.

```bash
make test-hidden                                  # the whole hidden set, serially, from the repo root
make test-hidden HIDDEN_FILTER='[.ui_integration]'  # one slice
make test-hidden-list                             # inventory without running

# Or drive the binary directly — but only from the repo root (see below)
./build/bin/helix-tests "[.]"
./build/bin/helix-tests "[.xml_required]"
```

> **`[.]` selects the whole hidden set, not just the tests literally tagged `[.]`.** Catch2 appends a bare `.` tag to every hidden test at registration (`TestCaseInfo` ctor: `if (isHidden()) internalAppendTag("."_sr)`), and the spec parser splits a `[.foo]` pattern into a required `.` plus a required `foo`. So one pattern covers `[.ui_integration]`, `[.xml_required]`, `[.disabled]`, `[.skip]`, `[.slow]`, `[.benchmark]`, `[.memprobe]` and `[.integration]` alike.

> **Run from the repo root.** Every `[.ui_integration]` and `[.xml_required]` test reads `ui_xml/` by relative path. From any other cwd they fail or skip en masse, which reads as a regression. `make test-hidden` `cd`s to `$(CURDIR)` for exactly this reason.

> **`make test-hidden` is not part of `make unit-sweep`** — these tests cannot share that run's sharded, parallel, arbitrary-cwd execution. It *is* gated in `scripts/quality-checks.sh`, but only when the test binary is already current: the gate never builds one, so a cold checkout and the CI Code Quality runner both skip it with an instruction rather than eating a ten-minute build on a commit hook. Broad `src/` coverage of this set therefore still belongs in nightly.

> **Scope:** this tracker covers `[.]`-prefixed hidden tests in compiled `tests/**/*.cpp`. Two other categories are tracked separately and are NOT covered here: `[!mayfail]` tests (which run but are allowed to fail) and any `*.cpp.disabled` files (excluded from the build entirely).

---

## Current state of the hidden set

**The set is green, which is what makes it gateable.** Run it serially from the repo root. Cases reported as skipped are `SKIP()` calls inside otherwise-passing cases (absent hardware, absent XML component, platform guards), not silent failures.

A hidden test that no longer passes is a finding: leave it red and fix it, because burying it in an exclusion list is how it stays buried.

---

## Counts

The binary is the authority, so this document states no counts. These commands produce them:

```bash
./build/bin/helix-tests "[.]" --list-tests | tail -2               # the whole hidden set
./build/bin/helix-tests "[.xml_required]" --list-tests | tail -2   # one tag
./build/bin/helix-tests "[.]" --list-tags                          # every tag in the hidden set, with its count
```

`--list-tags` prints a hidden tag without its dot: `[.ui_integration]` appears as `[ui_integration]`. Grepping the sources for `[.` over-counts, because several tests carry a literal `"[.]"` inside an unrelated string (regex fixtures in the Klipper-config parser tests, for one), and it under-counts a `TEMPLATE_TEST_CASE`, which registers one case per type.

## Summary

| Tag | File(s) | Why it is hidden |
|-----|---------|------------------|
| `[.xml_required]` | `test_ui_panel_bindings.cpp`, `test_fan_settings_xml.cpp` | Subject-binding assertions needing the XML tree |
| `[.ui_integration]` | 7 files (below) | Real widget tree built from `ui_xml/` |
| `[.slow]` | `test_wifi_manager.cpp` | WiFiManager integration against the mock backend (`use_mock_backend()`, since CoreWLAN on macOS refuses to start without Location Services); slow (2-3s simulated scan/connect per case) |
| `[.]` (generic) | `test_moonraker_api_exclude_object.cpp`, `test_nozzle_render_gallery.cpp`, `test_swatch_gallery.cpp`, `test_example_plugin_calibration_bridge.cpp`, `test_screensaver_sim_contract.cpp`, `test_gcode_crease_bench.cpp`, `test_ams_paging_panel.cpp`, `test_ams_system_info_snapshot.cpp` | Security injection fixtures, galleries and screenshots that write files, cost benchmarks |
| `[.memprobe]` | `test_gcode_memory_probe.cpp` | Memory measurement probe |
| `[.integration]` | `test_moonraker_client_security.cpp` | Timeout-callback deadlock check |
| `[.benchmark]` | `test_wizard_connection.cpp` | Performance measurement |

---

## `[.ui_integration]`

These build a real widget tree from `ui_xml/`. They are hidden because they depend on the working directory, **not** because they are broken: they pass when run from the repo root.

| File | What it covers | Replaced by `tests/ui/`? |
|------|----------------|--------------------------|
| `test_action_prompt_modal_stress.cpp` | L081 crash family (#875 SIGBUS, #877 SEGV, #906 cluster): rapid show/hide, burst cadence, stacked modals, click-driven teardown, single-tick async-delete drain, `queue_update` racer. Every case ends on a **widget census** (recursive count over the screen + `lv_layer_top()` + `lv_layer_sys()`), `dialog() == nullptr` / `is_visible() == false`, `UpdateQueue::pending_count() == 0`, and a heap-growth ceiling. The top layer is the load-bearing part: `safe_delete_deferred_raw()` reparents there before `lv_obj_delete_async()`, so a dropped async delete strands the tree **off** the screen where a screen-child assertion cannot see it | **No.** In-process race harness; out-of-process `ctl` cannot drive it |
| `test_wizard_connection_ui.cpp` | Wizard connection step XML structure: widget names, title text, flex layout | **No.** Wizard is pre-first-boot; `ctl` cannot reach it |
| `test_wizard_step_stress.cpp` | Wizard step-transition churn (bounce 2↔3, full sweep) | **No.** Same reason |
| `test_info_qr_modal_stress.cpp` | InfoQrModal open/close with the software render thread live and a synchronous refresh between cycles (#1673). An unbalanced free surfaces as an ASAN use-after-free | **No.** In-process race harness, same reason |
| `test_wizard_welcome_font.cpp` | The Welcome header's per-breakpoint face: the ladder, the wiring from `wizard_language_chooser.xml`, and the restyle on a mid-wizard resize (#1599, #1612) | **No.** Wizard is pre-first-boot |
| `test_spaghetti_detection_modal.cpp` | `SpaghettiDetectionModal` resume/abort/tune callbacks + self-delete on hide; Tune deliberately does *not* hide | **No.** Sole coverage of this modal |
| `test_gcode_error_routing_e2e.cpp` | Uncoded jam while paused renders a modal holding the full untruncated message | **No.** File also holds a running `[ui_integration]` sibling test |

`tests/ui/` (pytest driving a live binary via `helix-screen ctl`, added 2026-07-25) is **not** a replacement for any of these. It is a harness-validation suite — freeze/reset/text/navigation/screenshot mechanics — plus golden screenshots for 8 base panels. It has no wizard, modal, action-prompt, or crash-race coverage.

---

## Tag Conventions

| Tag | Meaning | When to Use |
|-----|---------|-------------|
| `[.]` | Generic hidden | Crashes, instability, destructive global state, catch-all |
| `[.network]` | Requires network | Live server, hardware |
| `[.benchmark]` | Performance measurement | Timing-sensitive |
| `[.slow]` | Too slow for normal runs | >5 seconds execution |
| `[.disabled]` | Temporarily disabled | Awaiting fix or decision |
| `[.flaky]` | Intermittent failures | Race conditions, timing |
| `[.ui_integration]` | Needs `ui_xml/` on disk | Real widget tree, cwd-dependent |
| `[.xml_required]` | Needs `ui_xml/` on disk | Subject-binding assertions |

---

## Verification

```bash
make test-hidden-list                                  # builds the tests, then lists the set; or, without the build:
./build/bin/helix-tests "[.]" --list-tests | tail -2
```

# Hidden Tests Tracker

**Last Updated:** 2026-08-17
**Total Hidden Tests:** 81 (Catch2's own count — `./build/bin/helix-tests "[.]" --list-tests | tail -1`)

> 2026-08-17: 81, down from 89. The seven `test_ui_ams_slot.cpp` cases left the
> hidden set entirely (see the `[.skip]` row removal below), and the concurrent
> connect/disconnect robustness case was re-enlisted into the nightly
> `[eventloop]` job after its libhv heap-corruption root cause was fixed
> (patches/libhv-websocket-open-install-once.patch; 2.56s measured, no `[slow]`).

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

> **`make test-hidden` is not part of `make test-run`** — these tests cannot share that run's sharded, parallel, arbitrary-cwd execution. It *is* gated in `scripts/quality-checks.sh`, but only when the test binary is already current: the gate never builds one, so a cold checkout and the CI Code Quality runner both skip it with an instruction rather than eating a ten-minute build on a commit hook. Broad `src/` coverage of this set therefore still belongs in nightly.

> **Scope:** this tracker counts `[.]`-prefixed hidden tests in compiled `tests/**/*.cpp`. Two other categories are tracked separately and are NOT in the count below: `[!mayfail]` tests (which run but are allowed to fail) and any `*.cpp.disabled` files (excluded from the build entirely).

---

## Current state of the hidden set

Measured on macOS (Darwin 24.3.0, arm64), serial, from the repo root, ~65s:

```
test cases:   81 |   41 passed | 40 skipped (2026-08-17 recount; previously 89)
```

The 41 "skipped" are `SKIP()` calls inside otherwise-passing cases (absent hardware, absent XML component, platform guards) — not silent failures.

**The set is green, which is what makes it gateable.** It was not always: six cases were red, and both groups reproduced in isolation, so neither was an ordering artefact. What they turned out to be:

| Test(s) | File | What was actually wrong |
|---------|------|-------------------------|
| 5 cases: *Backend initialization state*, *Network scanning lifecycle*, *Scan callback preservation*, *WiFi edge cases*, *WiFi network information* | `test_wifi_manager.cpp` (`[.disabled]`) | They assert **mock-backend** data (10 seeded networks, a 2s simulated scan) but ran against whatever `WifiBackend::create()` picked — CoreWLAN on macOS, which refuses to start without Location Services. Not a permission problem to skip around: against a *granted* permission they would have failed too, on `networks.size() == 10`. Fixed by pointing them at the mock the way the `[observers]` cases in the same file already did (`use_mock_backend()`), so they now run on every platform. Their `wait_for_condition` also had to drain the `UpdateQueue` — scan results reach the caller through `queue_update()`. |
| *ams_slot: material label binds to subject* | `test_ui_ams_slot.cpp` (`[.skip]`) | Missing arrangement, not a superseded assertion: the case never called `AmsState::init_subjects()`, so `sync_from_backend()`'s `lv_subject_copy_string` landed on an uninitialised subject and the label stayed `"--"`. It is the only coverage of the **backend → subject** leg; the `[1065]` widget test next to it seeds the subject directly and covers subject → label. Mutating away the `slot_materials_` write in `sync_from_backend()` reds this case and leaves `[1065]` green, which is the proof the two are not duplicates. |

Both groups were left red rather than filtered out on purpose while they were red — a hidden test that no longer passes is a finding, and burying it in an exclusion list is how it stays buried.

---

## Summary

| Tag | Count | File(s) | Description |
|-----|------:|---------|-------------|
| `[.xml_required]` | 41 | `test_ui_panel_bindings.cpp` | Panel subject-binding assertions needing the XML tree |
| `[.ui_integration]` | 17 | 5 files (below) | Real widget tree built from `ui_xml/` |
| `[.disabled]` | 11 | `test_wifi_manager.cpp` | WiFiManager integration against the mock backend; slow (2-3s simulated scan/connect per case) |
| `[.]` (generic) | 8 | `test_config.cpp`, `test_moonraker_client_robustness.cpp`, `test_moonraker_api_exclude_object.cpp`, `test_nozzle_render_gallery.cpp` | Destructive global state, event-loop concurrency, BMP-writing gallery |
<!-- [.skip] row removed 2026-08-17: all seven test_ui_ams_slot.cpp cases left
     the hidden set — the tag predated the init_subjects() fix and every case
     passes in the default run (79 assertions across 25 ams_slot cases). They
     are LVGLUITestFixture C++-widget tests, not cwd-coupled XML-tree tests, so
     the "run from the repo root" constraint never applied to them. -->
| `[.slow]` | 2 | `test_async_callback_safety.cpp` | Stress tests, too slow for CI |
| `[.memprobe]` | 1 | `test_gcode_memory_probe.cpp` | Memory measurement probe |
| `[.integration]` | 1 | `test_moonraker_client_security.cpp` | Timeout-callback deadlock check |
| `[.benchmark]` | 1 | `test_wizard_connection.cpp` | Performance measurement |

---

## `[.ui_integration]` (17 tests, 5 files)

These build a real widget tree from `ui_xml/`. They are hidden because they depend on the working directory, **not** because they are broken — all 17 pass when run from the repo root.

| File | Tests | What it covers | Replaced by `tests/ui/`? |
|------|------:|----------------|--------------------------|
| `test_action_prompt_modal_stress.cpp` | 8 | L081 crash family (#875 SIGBUS, #877 SEGV, #906 cluster): rapid show/hide, burst cadence, stacked modals, click-driven teardown, single-tick async-delete drain, `queue_update` racer. Every case ends on a **widget census** (recursive count over the screen + `lv_layer_top()` + `lv_layer_sys()`), `dialog() == nullptr` / `is_visible() == false`, `UpdateQueue::pending_count() == 0`, and a heap-growth ceiling. The top layer is the load-bearing part: `safe_delete_deferred_raw()` reparents there before `lv_obj_delete_async()`, so a dropped async delete strands the tree **off** the screen where a screen-child assertion cannot see it | **No.** In-process race harness; out-of-process `ctl` cannot drive it |
| `test_wizard_connection_ui.cpp` | 5 | Wizard connection step XML structure: widget names, title text, flex layout | **No.** Wizard is pre-first-boot; `ctl` cannot reach it |
| `test_wizard_step_stress.cpp` | 2 | Wizard step-transition churn (bounce 2↔3, full sweep) | **No.** Same reason |
| `test_spaghetti_detection_modal.cpp` | 1 | `SpaghettiDetectionModal` resume/abort/tune callbacks + self-delete on hide; Tune deliberately does *not* hide | **No.** Sole coverage of this modal |
| `test_gcode_error_routing_e2e.cpp` | 1 | Uncoded jam while paused renders a modal holding the full untruncated message | **No.** File also holds a running `[ui_integration]` sibling test |

`tests/ui/` (pytest driving a live binary via `helix-screen ctl`, added 2026-07-25) is **not** a replacement for any of these. It is a harness-validation suite — freeze/reset/text/navigation/screenshot mechanics — plus golden screenshots for 8 base panels. It has no wizard, modal, action-prompt, or crash-race coverage.

---

## Destructive Global State (`test_config.cpp`)

| Line | Test | Tags |
|------|------|------|
| 2703 | StaticSubjectRegistry supports deinit/re-init cycles | `[.][core][registry]` |
| 2717 | StaticSubjectRegistry deinit_all runs callbacks in LIFO order | `[.][core][registry]` |

Hidden because they tear down and rebuild global subject-registry state, which is unsafe to interleave with the rest of the suite. Run in isolation.

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
make test-hidden-list                                  # or, equivalently:
./build/bin/helix-tests "[.]" --list-tests | tail -1
```

Expected: `81 matching test cases`. Catch2's count is the authority here — grepping the
sources for `[.` over-counts, because several tests carry a literal `"[.]"` inside an
unrelated string (regex fixtures in the Klipper-config parser tests, for one).

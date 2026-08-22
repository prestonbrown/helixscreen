# PrintStatusPanel test isolation: no safe lifetime exists

**Date:** 2026-07-20
**Status:** Open — needs a production decision
**Found during:** M117 pre-print visibility work (branch `feature/m117-preprint-visibility`), Task 9
**Severity:** Blocks widget-level tests for `panel_widget_print_status.xml`; a latent UAF either way

## Summary

There is currently **no safe way to construct a `PrintStatusPanel` in a test**. Both
available lifetimes produce a use-after-free:

| Lifetime | Failure |
|---|---|
| Stack-local (per-fixture) | `lv_xml_register_subject()` has no unregister, so the process-global XML subject registry is left pointing into a dead stack frame → SIGSEGV |
| Process-lifetime singleton | The panel holds ~25 `ObserverGuard`s on `PrinterState`-owned subjects; every later `LVGLUITestFixture` test calls `deinit_subjects()`, freeing those subjects while the observers remain registered → `lv_observer_remove()` on freed memory (the #705 pattern) |

This blocked adding widget-level visibility tests for the M117 rows in
`ui_xml/components/panel_widget_print_status.xml`. Those tests were written, proven to
discriminate by mutation, and then reverted (`eb9d247d1`) because they could not run
without one of the two hazards above.

## Evidence

**Singleton path — confirmed live, not latent:**

```
./build/bin/helix-tests "~[slow]"
→ tests/unit/test_wizard_step_logic.cpp:318: FAILED
  registry: step_by_id stays valid after panel teardown (3rd-printer UAF)
  SIGSEGV - Segmentation violation signal
  test cases: 8715 | 8713 passed | 1 failed | 1 skipped
  exit 139
```

Reproducible across 7 of 8 randomized-order seeds. Valgrind attributes it to
`lv_observer_remove` reading a node freed by `lv_subject_deinit` via
`PrinterState::deinit_subjects()`.

Note the pair `[print_status]` + the wizard teardown test alone does **not** reproduce it
(passes, 205 assertions / 54 cases) — it needs enough of the suite to have cycled
`deinit_subjects()` first.

**Stack-local path:** reverting only the fixture change and keeping the new tests
SIGSEGVs at tests/unit/test_print_status_widget_m117.cpp:50.

## Fixture-level fixes attempted and rejected

Three approaches, all failed:

1. **`deinit_subjects()` + `init_subjects()` on the global panel after `PrinterState`
   re-init** (the obvious repair) — crashes immediately when called after the
   `PrinterState` deinit.
2. **Reordering that repair before the deinit** — leaves the ~25 *constructor*-created
   guards stale. Valgrind: invalid read in `~PrintStatusPanel`.
3. **Destroying panel singletons via `StaticPanelRegistry::destroy_all()` while subjects
   are alive, then recreating and re-registering** — cures the wizard crash (green, 11/12
   seeds, valgrind clean) but *creates* a new failure in `ams_mini spool mode`: a
   recreated panel observes `ams_slot_count`, which the AMS tests re-init mid-test.
   Removing the recreation restores the original SIGSEGV.

`ObserverGuard::invalidate_all()` was also considered and rejected — it would orphan live
observers on `AmsState` / `DisplaySettingsManager` subjects.

**Conclusion:** this is not fixable from the fixtures. A process-lifetime
`PrintStatusPanel` cannot coexist with a suite that rebuilds `PrinterState` / `AmsState`
subjects per test.

## Proposed fix (needs a decision — critical path)

Split the XML-registration side effect out of observer creation:

```cpp
// PrintStatusPanel
void register_xml_subjects();  // lv_xml_register_subject() calls only, no observers
void init_subjects();          // observers, as today
```

A fixture then gets stable global registrations from a panel that registers **no**
observers, so nothing dangles when `PrinterState` subjects are torn down.

This touches `PrintStatusPanel` and the subject-registration path, both of which
`CLAUDE.md` classifies as critical. It deserves its own branch, its own review, and its
own bisect point rather than riding along with a bug fix.

## Why this was deferred rather than fixed

The M117 work it surfaced (delta-clobber fix, pre-print visibility, idle surfaces, two
declarative refactors, mock M117 support) is independent, complete, reviewed, and
covered by `test_display_message.cpp`, `test_print_start_collector.cpp`, and
`test_moonraker_mock_behavior.cpp`. Holding a user-facing bug fix behind a
test-infrastructure redesign on a critical path was the worse trade.

## What is lost until this is fixed

XML row visibility for `panel_widget_print_status.xml` has no automated coverage. That is
the same gap that allowed a clipped, dead M117 row to exist in view 4 undetected. Verification
for those rows is currently manual (runtime launch + screenshot).

The reverted tests are recoverable from `ef34fe998` and `79b723199`; they were correct and
mutation-verified. Only their fixture could not be made safe.

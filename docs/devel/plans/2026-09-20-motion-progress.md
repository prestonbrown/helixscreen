# SDD ledger — plan: docs/devel/plans/2026-09-20-motion-1-settings-and-z-clamp.md

Spec: docs/devel/plans/2026-09-20-motion-panel-enhancements-design.md (read, reachable)
Worktree: .worktrees/motion-settings-z-clamp, branch feature/motion-settings-z-clamp
Base: db422b224 (origin/main). Verified: 0 of the 27 local-main-only commits touch any
file this plan modifies, so worktree content is identical to the tree the plan was
written against.
Claim: worktree:motion-settings-z-clamp held (pid 1371187).
Build parallelism: `scripts/helix-claim jobs` = 10 at start; a peer holds
build:u1-screws-tilt. Re-read it before each build rather than hardcoding -j.

## Pre-flight scan

### Task pairs sharing a file or interface

| A | B | Shared | Finding |
|---|---|---|---|
| 2 | 5 | `include/ui_panel_motion.h`, `src/ui/ui_panel_motion.cpp` | Sequential, disjoint regions (latch/helper vs distance fn). Clean. |
| 3 | 4 | `include/settings_manager.h`, `src/system/settings_manager.cpp`, `src/system/config.cpp` | Additive in the same regions; 3 runs first and creates the test file 4 appends to. Order is correct in the plan. Clean. |
| 3 | 5 | `get_jog_speed_xy/z` | Signatures match (int mm/min both sides). Clean. |
| 4 | 5 | `get_jog_distance(JogMode, bool)` | Signatures match (float mm both sides). Clean. |
| 1 | 7 | `action_button_2_*` props | 7 uses `action_button_2_min_width="0"`, which 1 creates with default 90. Clean. |
| 6 | 7 | `show_motion_settings_overlay()` | 6 produces, 7 consumes. Single opener, no third copy. Clean. |
| 2 | — | `clamp_axis_and_warn` | Produced by 2, consumed by nothing else. Clean. |

### Per-task internal consistency

| Task | Finding |
|---|---|
| 1 | Steps agree; before/after `ctl geom` is a real check. Clean. |
| 2 | **DEFECT** — Step 6's test constructs `MotionPanel panel;`. See Ruling 1. |
| 3 | Test asserts the defaults the impl sets; clamp range agrees between test, header doc and setter. Clean. |
| 4 | Test asserts the defaults `jog_distance_default()` returns; key derivation is single-sourced. Clean. |
| 5 | **HAZARD** — struct label type changes under an existing caller. See Ruling 2. |
| 6 | Self-consistent. Carries one unresolved gap (safety-limits accessor), already flagged in the plan's Self-Review. |
| 7 | Steps agree; verifies back-navigation returns to the motion panel, not the settings tree. Clean. |

## Rulings

Ruling 1: Task 2's clamp-plus-latch splits into a pure function plus a thin panel
method. `helix::clamp_jog_with_warn(current, uncommitted, delta, min, max,
already_warned) -> {double allowed; bool warn;}` goes in `include/jog_coalescer.h`
beside `clamp_jog_delta`, which is the same rule family and already has a test file.
`MotionPanel::clamp_axis_and_warn` becomes a wrapper that calls it, emits the matching
`lv_tr` literal, and updates `edge_warned_[helix::axis_index(axis)]`. Task 2's Step 6
test targets the pure function and constructs no MotionPanel.
— Why: `MotionPanel` has a real constructor and a `get_global_motion_panel()` accessor,
and XML subjects register into LVGL's global scope, so a second instance in a unit test
invites subject-name collisions. CLAUDE.md asks for a pure function when the rule has no
I/O.
— Cost if wrong: two more symbols in `jog_coalescer.h` than strictly needed. Low.

Ruling 2: Task 5 must preserve `ui_jog_pad.cpp`'s label handling explicitly. That file
copies `mode_dist.inner_label` / `outer_label` into local `const char*` and hands them to
`label_dsc.text`. After the struct's labels become owned buffers on a by-value return,
those pointers reference the `const auto&`-bound temporary. That is valid to end of
scope and the draw happens in the same scope, so the change is safe, but the implementer
is told to verify it rather than meet it by surprise.
— Why: silent lifetime change under an existing caller the task does not otherwise edit.
— Cost if wrong: a dangling label pointer in the jog pad draw path, visible as garbage
ring labels. Caught by Task 5 Step 5's `ctl text` check.

## Progress

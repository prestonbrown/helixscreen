# GOD-7: NavigationManager narrow header and extractions

Audit finding GOD-7 (`2026-09-30-architecture-audit.html#GOD-7`). `ui_nav_manager.cpp` is 2,349
lines, the header 889, included by ~154 files (33 src includers call nothing on it). Of the 60 src
includers that call it, 45 use only a hot set; 15 need the full class. Delete this file when the
tranche ships.

## Seams

- `include/ui_nav.h` (~40 lines, `<functional>` plus forward declarations), `namespace
  helix::nav` forwards defined in `ui_nav_manager.cpp`: `push_overlay`, `go_back`,
  `close_overlay`, `register_overlay`/`unregister_overlay`, `on_close`/`clear_on_close`,
  `set_active`, `is_on_top`/`is_in_stack`. `PanelId` and `OverlayCloseCallback` move there. The
  unregister/clear forwards absorb the hand-written `is_destroyed()` guards.
- Classes owned by value by the manager: `OverlayBackdrop` (~190), `RailEstop` (~100),
  `PrinterBadgeMenu` (~70), `PanelRegistry` (~110). Each takes part in delete scrub and hot-reload
  rekey via `scrub(obj)`/`rekey(old,new)` fanned out by the manager.
- Folded duplicates: `retire_overlay(root)` (written 4 times), `is_main_panel(obj)` (4 times),
  `reset_overlay_transform(obj)` (4 copies that already drifted: two still reset translate_y and
  scale).
- Not extracted: transitions (one slide left).

## Rules to keep

- push/go_back/close always go through `queue_update`; decisions made in queue order (#1221).
- Close callbacks run on the next tick via `defer_close_callback`. Exception today: the
  buried-overlay path of `close_overlay` calls the callback synchronously, safe only because every
  in-tree callback defers its own delete. C5 routes it the deferred way.
- Backdrops, scrim and snapshots are deleted only with `safe_delete_deferred`.
- A navbar switch still runs close callbacks of overlays sliding out.
- Resurrected-panel sweeps must not hide the rail E-stop (keep the chrome marker).
- `activate_restored_target` clears its latch before dispatching (re-entrancy).

## Commits (pinning test first, `make mutate-diff` named)

1. Delete the 33 dead includes (and cli_args.h's), re-adding only what each file needed
   transitively (~-35).
2. `ui_nav.h` and forwards (~+90).
3. Scripted rewrite of hot-set calls to `helix::nav::`; alias sites in the plugin host and
   overlay_base by hand; a ratchet so the full-header includer count can only fall.
4. `is_main_panel` and `reset_overlay_transform` (~-30).
5. `retire_overlay`, moving the buried path to deferred callbacks; pinned by a test that no close
   callback runs before the next tick on all four paths. Behaviour change, ships alone.
6. `PrinterBadgeMenu`. 7. `RailEstop`. 8. `OverlayBackdrop`, after Lua phase 5 merges (it changes
   `adopt_overlay_backdrop`). 9. `PanelRegistry`.

An edit to `ui_nav_manager.h` rebuilds ~155 TUs today, ~35 after C1-C3. Header ends near 600 lines,
the .cpp near 1,950.

## Risks

- Critical path: `[nav]` and `[overlay]` per commit, one full-test-run and one zeus ASAN at the end.
- C1 may break transitive includes (ui_widget_ref.h, panel_lifecycle.h, ui_observer_guard.h,
  lvgl.h); run `check_esp32_app_srcs.py`.
- Tests keep the full header (61 includers, 14 use TestAccess).
- Main-only; do not port to release/1.0.

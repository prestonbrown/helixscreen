# GOD-8: ControlsPanel and SettingsPanel passengers and duplicates

Audit finding GOD-8 (`2026-09-30-architecture-audit.html#GOD-8`). Settings is mostly done (the
settings-root table took it from 1,360 to 703 lines); Controls (`src/ui/ui_panel_controls.cpp`,
1,909 lines) is the work. Delete this file when the tranche ships.

## What's there

Controls:
- Dead: secondary temperature rows (~170 lines). `populate_secondary_temps` has `max_visible = 0`,
  so the rows and their observers never exist; only `test_controls_secondary_temp_scale.cpp`
  reaches them. The one live output is the "N more sensors" row. `setup_card_handlers` is a
  find-and-log that does nothing.
- Duplicate: the Z-offset save (~120 lines) copies `z_offset_utils.cpp#save_dirty_offsets_shared`,
  which the header button uses. The homed-axis subjects are derived here and in `MotionPanel`.
- Passengers: quick-action macro/light slots (~270 lines, `quick_action_slots.cpp` already owns
  the model); secondary fan rows (~230 lines, built in C++ with inline styles; `fan_display_priority`
  name-matches "chamber", a twin of `is_chamber_fan_candidate`); 26 nav trampolines.

Settings: `populate_led_chips` is a no-op whose only caller is `discovery_steps.cpp#led_chips_step`;
`sync_slider_value_label` twins a Touch overlay helper; Touch, System and Connection page
callbacks still live in the panel.

## Commits (pinning test first)

- C1/C2: pin the "N more sensors" row, then delete the dead temp rows (bound label in both
  `controls_panel.xml` and `micro/controls_panel.xml`) (~-150).
- C3/C4: pin Save Z-Offset (confirm, SAVE_CONFIG sent, a second click in flight ignored), then
  route it to `save_dirty_offsets_shared` with a re-entry flag added there (~-105).
- C5: `QuickActionButtons` beside `quick_action_slots.cpp` (existing tests pin it).
- C6: pin fan rows, then build them from `controls_fan_row.xml`; observers and lifetimes copied
  verbatim. The `is_chamber_fan_candidate` swap is a separate one-line commit.
- C7: nav callbacks into one registration table; delete `setup_card_handlers`.
- C8 (optional, MAJOR, touches PrinterState): homed subjects published once by
  `PrinterMotionState`.
- S1: delete `populate_led_chips` and `led_chips_step`.
- S2-S4: Touch, System and Connection callbacks move to their overlays' `register_callbacks()`,
  each pinned first (callbacks resolve before the overlay first opens).

Controls ends near 1,250 lines, Settings near 380.

## Behaviour changes needing a decision

- Z-offset save through the shared path: the confirm wording becomes generic, the confirm is
  skipped when nothing will restart, and the success toast loses the "(+0.050mm)" value.
- Fan order: the role-registry check moves nevermore, filter and temperature_fan fans higher.

## Risks and checks

- A moved callback must be registered before the XML naming it is created, or the row goes dead
  silently.
- Every Controls XML edit also goes into the micro layout.
- Fan observers' `SubjectLifetime` stays in a member vector aligned with its observer.
- Goldens (`tests/ui/test_screens.py`): `settings` byte-identical through S1-S4, `motion` guards
  C8. Controls is not in the golden set (mock temperatures drift), so C1-C7 use `ctl text`/`geom`
  on a pinned-socket mock.

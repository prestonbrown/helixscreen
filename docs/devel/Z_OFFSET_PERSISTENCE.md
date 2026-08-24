# Z-Offset Persistence

**Where the authoritative z-offset lives, when that is not Klipper's live one, and
how to add a firmware.**

Module: `include/z_offset_persistence.h` + `src/printer/z_offset_persistence.cpp`.
Tests: `tests/unit/test_z_offset_persistence.cpp`.

---

## The problem

On most printers the z-offset the next print will apply **is** Klipper's live
`gcode_move.homing_origin[2]`, so reading that subject is the whole story.

Some firmware instead keeps the authoritative value in its own storage and only
pushes it into `gcode_move` for the duration of a print, clearing the live offset in
its `END_PRINT` / `CANCEL_PRINT` macros. On those printers `homing_origin[2]` reads
**0.000 whenever the printer is idle** and is an outright lie about what the next
print will use.

Two things break if you believe the live value there:

1. **Display.** The Z-Offset row shows 0.000 while a real offset is stored.
2. **Adjustment.** A relative `SET_GCODE_OFFSET Z_ADJUST=` resolves against
   `homing_origin`, so an idle nudge lands on just the delta — and firmware that
   persists on every `SET_GCODE_OFFSET` then stores that, **discarding the user's
   real offset**. This is silent data loss, not a display glitch.

Reported against ZMOD on the FlashForge AD5M / AD5X.

---

## The abstraction

Generic code never names a firmware (root `CLAUDE.md` § "Vendor Knowledge Stays
Behind an Abstraction"). It asks three capability questions:

| Question | Asked by | Answer for a plain printer |
|----------|----------|----------------------------|
| `required_status_objects(hw)` | `MoonrakerDiscoverySequence::build_subscription_objects()` | empty |
| `read_persisted_offset_microns(status)` | `PrinterMotionState::update_from_status()` | `nullopt` |
| `persistence_enable_gcode(hw)` | `Application` discovery-complete | empty string |

Plus `should_enable_persistence(needs_enable, print_active, already_sent)` — the
once-per-session, idle-only gate on sending the enable command.

### Subjects

| Subject | Meaning |
|---------|---------|
| `gcode_z_offset` | Klipper's live offset, microns. Unchanged, always parsed |
| `persisted_z_offset` | Firmware-stored offset, microns |
| `persisted_z_offset_valid` | 1 once a stored offset has been reported. Separate because **0 microns is a legitimate stored value** and cannot double as "nothing stored" |

`PrinterState::get_persisted_z_offset_microns()` folds the pair into
`std::optional<int>` for the display/adjust helpers.

### Choosing what to show and what to send

Both rules are pure functions in `helix::zoffset` (`include/z_offset_utils.h`),
unit-tested in `tests/unit/test_z_offset_utils.cpp`:

- `displayed_z_offset_microns(live, persisted, print_active)` — live during a print
  (baby steps land there first) or when nothing is stored; persisted otherwise.
- `build_z_adjust_gcode(base, live, delta, all_homed)` — relative `Z_ADJUST=` when
  the base **is** the live offset, absolute `Z=` when it is not.

The UI calls `displayed_z_offset_microns(PrinterState&)` so the rule lives in one
place. **The base you adjust from must be the value you displayed** — that
equivalence is what makes the absolute form correct.

---

## Delta-only status objects

The storing object is typically delta-only: Moonraker re-sends it when it changes,
not every frame. So `read_persisted_offset_microns()` returning `nullopt` means
**"no news"**, never "cleared". `PrinterMotionState` only assigns when a frame
actually carries a value; blanking on absence would wipe the reading on the very
next `gcode_move` frame. `tests/unit/test_printer_motion_state_persisted_zoffset.cpp`
pins this.

---

## Adding a firmware

One row in the `providers()` table in `src/printer/z_offset_persistence.cpp`. No
call site changes:

```cpp
{"ZMOD", "SAVE_ZMOD_DATA", {"save_variables"}, "SAVE_ZMOD_DATA LOAD_ZOFFSET=1", &read_zmod},
//  name   detect macro     status objects      enable gcode (or nullptr)       reader
```

- **detect macro** — matched via `PrinterDiscovery::has_macro()`, case-insensitive.
- **reader** — pulls microns out of a status frame; must return `nullopt` for any
  frame that does not carry the value, and must tolerate the firmware's
  not-yet-set placeholder. Round rather than truncate: stored values accumulate
  relative deltas, so a nominal `-0.150` arrives as `-0.1499999`.

`read_persisted_offset_microns()` dispatches **by schema**, not by detected
firmware, because it runs on the status path where no `PrinterDiscovery` is in
hand. Keep each reader's key path distinctive enough to identify itself.

---

## ZMOD specifics

See `docs/devel/printers/FLASHFORGE_AD5X_SUPPORT.md` § "ZMOD z-offset storage" for
the macro-level detail: `SET_GCODE_OFFSET` override, `LOAD_GCODE_OFFSET` at
`START_PRINT`, the `load_zoffset` gate, and the separate native-screen offset that
`LOAD_ZOFFSET_NATIVE` copies (which HelixScreen does not call).

---

## Related

- `z_offset_calibration_strategy` in the printer database (`docs/devel/PRINTER_MANAGER.md`)
  is a **different** axis: it selects the *calibration* command sequence and whether
  a Save button is offered. A printer can be `firmware_managed` there without having
  a persistence provider here.

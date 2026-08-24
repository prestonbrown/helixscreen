# MedusaHC (Tool-Changer Add-On)

MedusaHC is a budget **hotend changer**: it swaps only the hot end (heater, thermistor,
fan), not a whole toolhead. It is not its own `AmsType` and has no backend of its own.
It is a klipper-toolchanger printer, so `AmsBackendToolChanger` drives it, and everything
MedusaHC adds on top lives in one module.

- Hardware / config: [Irbis3D/MedusaHC](https://github.com/Irbis3D/MedusaHC)
- Experimental Python controller: [Irbis3D/MedusaHC-Python-Controller](https://github.com/Irbis3D/MedusaHC-Python-Controller)

## Why it is not a backend

Upstream `printer.cfg` has an uncommented `[include toolchanger.cfg]`, and that file
declares `[toolchanger]` plus `[tool T0..T3]`. Those are exactly the objects
`AmsBackendToolChanger` already reads, so a stock MedusaHC is detected and driven with no
MedusaHC-specific code at all. Adding a ninth `AmsType` would have forked a working
backend for no gain.

What a hotend changer genuinely adds is two things klipper-toolchanger cannot answer, and
those are the whole content of `include/toolchanger_addon.h` +
`src/printer/toolchanger_addon.cpp`.

## The two add-ons

### 1. Dock sensors are the authority on which tool is mounted

`Macros/toolchanger.cfg` ships `verify_tool_pickup: False` and
`require_tool_present: False`, so klipper-toolchanger never verifies a pickup.
`toolchanger.tool_number` is **only ever "what `SELECT_TOOL` last set"** - after a failed
or partial pickup it names a tool that is not on the head.

The physical answer comes off dock sensors through MedusaHC's own pin_watch Klipper extra
(Scripts/pin_watch.py upstream). It is not a stock Klipper module - it is absent from all
136 files in Klipper3d/klipper's klippy/extras - so the object name is a real signature.
The official controller agrees: its `_current_tool()` returns `pin_watch.current_tool`.

`AmsBackendToolChanger::apply_tool_sensor_locked()` therefore runs **after**
`parse_toolchanger_state()` in the same frame, so the sensor answer wins.

`current_tool == -2` is a third state, distinct from "no tool": the sensors cannot tell.
Reporting it as `-1` would invite a tool change against an unknown carriage state, so the
backend holds the last known tool and raises `AmsAction::ERROR` instead.

### 2. A filament feeder

Only the hot end travels, so the filament is held by a servo gripper on the frame
(`[servo my_servo]`) that has to be released around a swap. There is no analogue in
klipper-toolchanger, because a toolhead changer carries its own extruder.

Surfaced as two `DeviceAction`s (`open_feeder` / `close_feeder`) in the AMS Device
Operations overlay. They are gated by
`check_preconditions(/*requires_toolhead_motion=*/true)`: the gripper is the only thing
holding the filament, so opening it mid-print drops the strand and kills the job, and the
overlay itself has no print gate.

## Three shipping configurations

Object presence cannot separate (b) from (c) - both register `[medusahc]` - so
`read_medusahc()` discriminates on **field names**, reading the Irbis3D names first. The
fork's names are a fallback, never a default.

| | Objects | Status schema | Feeder macros |
|---|---|---|---|
| **(a) Irbis3D stock** | `[pin_watch io]`, `[toolchanger]`, `[tool T0..3]` | `pin_watch` only: `{"current_tool": int}` | `OPEN` / `CLOSE` |
| **(b) Irbis3D Python Controller** | adds `[medusahc]` | `operation`, `current_tool`, `target_tool`, `last_error`, `feeder_open`, `layer`, `sensor_error`, `tool_count`, `sensors` | `MHC_OPEN` / `MHC_CLOSE`, plus legacy aliases |
| **(c) third-party forks** | adds `[medusahc]` | `state`, `error`, flat `toolN_docked` | `OPEN` / `CLOSE` |

`operation` is finer than klipper-toolchanger's single `changing`: `picking` and
`dropping` name the direction, and they arrive even when the swap was started from
Mainsail or the console.

Feeder macro selection is capability-driven rather than hardcoded: `MHC_OPEN`/`MHC_CLOSE`
when discovery sees those macros, else `OPEN`/`CLOSE`. Config (b) ships legacy aliases
forwarding to `MHC_*`, so both work there; preferring the native command keeps (a)
working too.

## Overriding the feeder macros

The command names forked upstream, and a machine mid-migration can have either set,
both, or legacy aliases repointed at something custom. So the macro each button sends is
user-selectable, not hardcoded.

`AmsBackendToolChanger::get_device_actions()` adds two `DROPDOWN` actions
(`feeder_open_macro` / `feeder_close_macro`) whose options come from
`feeder_macro_candidates()`: the macros the printer actually reports, filtered to `MHC_*`
plus anything containing `OPEN`, `CLOSE`, `FEEDER` or `GRIP`. A raw macro list runs to
hundreds of entries and is unusable as a picker; a free-text field cannot be typo-checked.

The first option is the `auto` sentinel, which restores whatever detection chose. That is
what makes a later migration free: a user who picks `auto` today gets `OPEN` now and
`MHC_OPEN` the moment the controller registers it, without revisiting the setting.

`Feeder` carries all of it - `open_gcode` (what is actually sent), `detected_open` (what
detection chose, kept so `auto` can be restored), `open_choice` (the stored pick) and
`macro_options`.

Persisted per-printer via `SettingsManager::get_feeder_open_macro()` /
`set_feeder_open_macro()` (and the `close` pair), under
`wizard::FEEDER_OPEN_MACRO` / `FEEDER_CLOSE_MACRO`. Per-printer rather than global because
two MedusaHC machines on one network can be at different points in the migration.

## Detection

`toolchanger_addon::present(hw)` is `has_pin_watch() && has_tool_changer()`.

Both halves are required. `pin_watch` alone is just the extra; `[toolchanger]` alone is
any of the many klipper-toolchanger builds.

**It deliberately does not touch `has_mmu_` / `mmu_type_`.** Those pick which AMS backend
gets built. An unguarded write there replaces a working backend on any printer that
happens to run `pin_watch`, and the same applies to detecting on `T<n>` macros - every
multi-tool Klipper setup has those. `tests/unit/test_medusahc_detection.cpp` carries the
regression matrix (Snapmaker U1, Happy Hare, AD5X IFS, QIDI Box, plain IDEX), each of
which was measured resolving to MedusaHC when the signal was wired as an MMU type.

`PrinterDiscovery` records only the plain object facts (`has_pin_watch()`,
`pin_watch_object_name()`). Deciding that pin_watch + toolchanger *means* MedusaHC is the
add-on module's business - see the vendor-abstraction rule in the root `CLAUDE.md`.

## Adding another hotend changer

Add one `Provider` row to the table in `toolchanger_addon.cpp`: a detection predicate,
which status objects to subscribe, and the feeder gcode. No call site changes.

## Printer database

Two entries in `assets/config/printer_database.json`:

- `duender_medusahc` - scored on `pin_watch` (92) and hostname `medusa` (85) only.
  `toolchanger` is corroboration at 20. `tool_count_4` and `kinematics_match corexy` are
  deliberately absent: they describe any 4-tool CoreXY toolchanger, and since
  `has_pattern()` is a case-insensitive substring match they would claim machines with no
  `pin_watch` at all.
- `duender` - hostname only. [Irbis3D/Duender](https://github.com/Irbis3D/Duender) is a
  mechanical Ender-3 to CoreXY conversion that ships **no Klipper config**, so a bare
  Duender has nothing in `printer.objects.list` distinguishing it from any other CoreXY.
  A `corexy` heuristic here would compete with the Voron and RatRig entries on every one
  of their machines. The wizard's manual-pick path covers it instead.

The bare-Duender hostname deliberately does not appear on the MedusaHC entry, or the
`duender` entry could never outscore it (scoring is highest-match + 3 per extra match).

## Known gaps

Not yet reconciled, and worth knowing before extending this:

(Per-tool spool metadata used to be listed here. It is now persisted - see
[FILAMENT_SLOT_METADATA.md](FILAMENT_SLOT_METADATA.md#tool-changer-is-the-odd-one-out).)

- **Tool offsets have two stores.** MedusaHC persists per-tool offsets in `save_variables`
  (`t{N}_gcode_{x,y,z}_offset`) and pushes them into a `TOOL_OFFSET` macro;
  klipper-toolchanger has its own offset model. Two sources of truth for one physical
  quantity.
- **`layer`, `PRIME_FLAGS_*`, `MHC_CLEAN`** - per-tool priming and cleaning scheduled
  against layer number. No analogue in the toolchanger model, unused today.

The Python controller is marked experimental and is moving; prefer widening the provider
table over hardcoding against any single revision of it.

---

Part of the filament system - see [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) for the
shared architecture and [FILAMENT_BACKEND_TOOLCHANGER.md](FILAMENT_BACKEND_TOOLCHANGER.md)
for the backend that drives it.

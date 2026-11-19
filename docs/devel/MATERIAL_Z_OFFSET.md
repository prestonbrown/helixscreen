# Z Offset and Bed Mesh by Material

**One Z offset, and optionally one stored bed mesh, per material, applied by the
print-start macro and saved per material from the screen** (prestonbrown/helixscreen#2142).

Module: `include/material_z_offset.h` + `src/printer/material_z_offset.cpp` (the provider
table and every pure rule), `include/material_z_offset_state.h` +
`src/printer/material_z_offset_state.cpp` (the live table, the setting, the writes).
Tests: `tests/unit/test_material_z_offset.cpp`, `test_material_z_offset_config_edit.cpp`,
`test_material_z_offset_state.cpp` (tag `[material_zoffset]`).

---

## The signal

A heated bed expands with temperature, so a first layer tuned at PLA's 60°C is wrong at
PETG's 80°C or ABS's 110°C. People already keep one offset per material in their start
macro: Jacobean's K2 Plus Kalico ships a `[gcode_macro _START_PRINT_VARS]` with
`variable_offset_<MAT>` lines, and AD5M and Voron owners write the same by hand. The key
is the material name the slicer passes, not the bed temperature; the Settings rows show
each material's usual bed temperature only to explain its value.

**The value is the gcode offset for that material.** The start macro applies it with an
absolute `SET_GCODE_OFFSET Z=` on top of the probe's own `z_offset`. Nothing here folds it
into the probe: no `Z_OFFSET_APPLY_PROBE`, no `SAVE_CONFIG`
(`tests/unit/test_material_z_offset.cpp` pins it for every row). Folding it would double
it at the next print, the hazard [Z_OFFSET_PERSISTENCE.md](Z_OFFSET_PERSISTENCE.md)
records as #1401.

---

## Rows

One provider row per storage scheme, in the `z_offset_persistence` shape
(`.claude/rules/vendor-abstraction.md`). Row order is match priority.

| Row | Detected by | Reads | Writes | Mesh |
|-----|-------------|-------|--------|------|
| `_START_PRINT_VARS` (K2 Kalico, hand-made copies) | `gcode_macro _START_PRINT_VARS` | `offset_<mat>` from the macro's status; `offset_default` is DEFAULT, `offset_probe` is not a material | `SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=offset_<mat>` for now, plus one line in the file that defines the section | no |
| HelixScreen's macro | `gcode_macro HELIX_MATERIAL_OFFSET` | `helix_zoffset_<key>` from `save_variables`, `bed_mesh.profiles` named `helix_<key>`, `_HELIX_MATERIAL.applied` / `apply_count` | `SAVE_VARIABLE VARIABLE=helix_zoffset_<key>`, which persists at once | yes |

No row matches on a printer a `z_offset_persistence` provider matches (ZMOD, Forge-X,
Helper-Script): that firmware already owns the gcode offset and re-applies its own.

**`_START_PRINT_VARS` first.** A printer whose own start macro keeps the table applies it
on every print; HelixScreen's macro is in the helper pack on every printer that installs
it, so it comes second.

**Keys.** Klipper lower-cases option names, so `variable_offset_PETG` reads back as
`offset_petg`. The vendor macro looks up `"offset_" + MATERIAL|lower` verbatim, so its key
is the lower-cased name (letters, digits, `_ + . -`). HelixScreen's macro maps `-`, ` `,
`+` and `.` to `_` (Jinja has no regex), so its key is `[a-z0-9_]`; `helix_key()` and the
macro's chain of `replace` filters are the same rule. A material neither can store is
refused, not mangled.

**A material the vendor macro does not declare yet.** `SET_GCODE_VARIABLE` refuses an
undeclared variable, so a new material is a file edit only and applies after the next
Klipper restart; the save message says so. HelixScreen never restarts Klipper for it.

---

## The config edit

`edit_config_text()` is the only thing that writes a user's config file, so it is pure and
tested hard: replace the value of exactly one `variable_offset_<MAT>:` line inside exactly
one `[gcode_macro _START_PRINT_VARS]`, keeping the key's spelling, the separator, the
spacing and any inline comment, or insert a new line after the section's last `variable_`
option (after any continuation lines of that option). Every other byte is kept, CRLF and a
missing final newline included. It refuses when the section is not there exactly once,
when the material's line appears twice, or when its value continues onto the next line.

**Which file.** `locate_config_file()` walks `printer.cfg` and everything it includes
(globs matched against the config root's file list, at most 64 files, none over 1 MB), so
a backup copy nothing loads (`printer-20261118_101500.cfg`) never counts as a second
definition. Exactly one holder is required. The path is cached for the session and the
file is read fresh before every write. Absolute includes are outside the config root and
are not followed.

---

## The printer-side switch

HelixScreen's macro applies nothing unless `helix_material_offset_enabled` is 1 in
`[save_variables]`. `MaterialZOffsetState::set_enabled()` writes it with the setting
(`enable_gcode()`), and `reconcile()` (when Settings opens) writes 1 where the setting is on
and the printer reads 0. The row is `active()` only once the switch reads on, so a value is
never saved per material while the macro ignores the table. The vendor row has no switch:
its start macro applies the table on every print.

**The fold is refused whenever the start macro applies offsets on its own**
(`start_macro_applies()`: always for the vendor row, while the switch is on for HelixScreen's),
whatever the screen's setting. `save_route()` answers `MacroOwnsOffset` and the Save explains
itself; `save_dirty_offsets()` drops `global_dirty` itself in that case, so no caller (the
header and Controls save, the tool-offset calibration panel, a later one) can fold the
material offset into the probe. Callers ask `folds_global(facts)`, never `saves_global()`
alone. `save_dirty_offsets()` also refuses anything ending in `SAVE_CONFIG` while a job holds
the machine.

## Which material a Save writes

`MaterialZOffsetState::save_target()`:

- **Feature off, or no row ready:** nullopt. The machine-wide save is exactly what it was.
- **HelixScreen's macro:** what the macro last applied (`_HELIX_MATERIAL.applied`), once it
  has run since Klipper started (`apply_count > 0`). Before then the material is unknown
  and **no save is offered** (`save_withheld()`): with the feature on, the machine-wide
  save is never the fallback, because the start macro would re-apply on top of the fold.
- **`_START_PRINT_VARS`:** the first filament of the current or last print's file, which is
  what the slicer passes as `MATERIAL=`.
- **An empty material** (the slicer passed none) is DEFAULT, for applying and for saving.

`current_save_availability()` asks the same target: with the feature on, the offset is
dirty when the live offset differs from what the macro applies for that material, not when
it is non-zero.

**During a print.** `helix::zoffset::save_route()` is the one decision: nothing to save,
per-material, machine-wide, or refused (machine-wide while `machine_motion_blocked`). The
machine-wide save restarts Klipper, so a job holding the machine refuses it, in
`save_dirty_offsets_shared()` as well as at the button; a per-material save moves nothing and
restarts nothing, so it goes through. Every Save button (header, Controls, both home-widget
sizes) is one button guarded by `z_offset_save_motion_blocked`, published beside
`z_offset_save_available` and 1 only while the route would be refused. The toolhead census
counts that binding as a guard (`kDerivedGuards` in `tests/unit/test_job_holds_machine.cpp`),
so the buttons stay classified exactly as they were with `moves_machine="true"`.

Why not `moves_machine`: that attribute binds the button to `machine_motion_blocked`
unconditionally, so it would grey a per-material Save during the very print it exists for.
Two buttons (one guarded, one not) would fork the UI and leave an unguarded path. The derived
subject keeps one button whose guard is exactly the route's answer, and the census allowlist
(`kDerivedGuards`, each entry naming its proving test) keeps that form from becoming a
general escape hatch.

**Primary material for a print the screen starts:** the tool that prints the most by
weight (`primary_material_for_file()`, reusing `print_cost::file_tool_grams()` and the
footer's per-tool grams), else the first filament. HelixScreen's macro takes it through
`_HELIX_MATERIAL.next`, set just before the start and cleared by the macro (or by the
screen when the start does not go ahead). The hint names its file in
`_HELIX_MATERIAL.next_file`, and the macro uses it only when that matches
`print_stats.filename`, so a hint a cancelled start left behind never reaches another job.
Every screen start sends it, the clearing form when the feature is off, there is no material,
or the file name cannot travel in a G-code parameter (`' " \ # ;` or a control character).
The vendor macro has no such input, so on that row a multi-material file uses the slicer's
first filament.

---

## Meshes

HelixScreen's macro loads `helix_<key>` when that profile exists, before the user's own
mesh step, so an adaptive `BED_MESH_CALIBRATE` later in PRINT_START replaces it, which is
the right order. After a calibration on a heated bed, the Bed Mesh panel offers "Save as
<material> mesh?" for the material whose usual bed temperature is closest to the bed's
target (`mesh_offer()`); a cold-bed mesh is offered to none. Klipper keeps profiles in
`printer.cfg`, so this one ends in the panel's usual SAVE_CONFIG prompt.

---

## Setup

The helper pack (`assets/config/helix_macros.cfg`, v2.3.0) defines `HELIX_MATERIAL_OFFSET`
and `_HELIX_MATERIAL`. `MacroManager` adds a `[save_variables]` section
(`helix_variables.cfg` beside `printer.cfg`, path from `printer.info`) to the staged file
on a printer that has none, and an update keeps a section the installed file carries. A
printer with its own `[save_variables]` keeps it. User steps:
[PRINT_START_INTEGRATION.md](PRINT_START_INTEGRATION.md#z-offset-and-bed-mesh-by-material).

The Settings overlay (`ui_settings_material_z_offset.cpp`) shows the setup when no row
matches (`material_zoffset_status` 0), an update prompt when the macro is there without
`[save_variables]` (1), and the toggle and rows when ready (2). The setting is per printer,
`material_z_offset/enabled` under the printer's config node.

---

## Limitations

Each one is a decision or a gap a reader will otherwise rediscover:

- **The last absolute `SET_GCODE_OFFSET Z=` wins.** Nothing stops a later macro (a toolchanger
  pickup, a probe touch routine such as `CARTOGRAPHER_TOUCH_HOME`) from replacing the material's
  offset. HelixScreen cannot see the start sequence's order; the user doc says to call
  `HELIX_MATERIAL_OFFSET` last.
- **The vendor row takes the slicer's material only.** `_START_PRINT_VARS` has no input for
  the screen's most-by-weight pick, so on a multi-material file a screen start applies and
  saves the first filament. Passing it would mean rewriting the file's `START_PRINT` line.
- **HelixScreen's row needs the macro to have run since Klipper started.** Before
  `apply_count` moves, the material is unknown and the save is withheld; a `PRINT_START` that
  never calls the macro never offers a save while the feature is on.
- **A file the walk cannot find reads but does not persist.** Absolute includes, a section
  in two loaded files, or a walk the 64-file cap stops with files unread: the live `SET_GCODE_VARIABLE` lands, the edit
  is refused, and the user sees "Applied for this session only". No backup is written before
  an edit, deliberately, so a save per print does not litter the config directory.
- **A new vendor-row material is file-only** until the next Klipper restart (`live_now`).
- **Settings lists DEFAULT and seven materials** (`MaterialZOffsetOverlay::kMaterials`).
  Entries for other materials apply and save through prints but have no row.
- **Key collisions.** HelixScreen's row folds `-`, ` `, `+` and `.` to `_`, so `PETG-CF` and
  `PETG CF` share an entry; any other non-alphanumeric character makes the material
  unstorable, and that print uses DEFAULT.
- **A per-material mesh needs `SAVE_CONFIG`.** Klipper keeps profiles in `printer.cfg`; only
  the offsets avoid a restart. The offer appears only for a heated-bed calibration, and only
  for the seven listed materials.
- **Job Queue starts** (`QueuedJobStarter`) do not send the most-by-weight hint; only the file
  view's Print does (`PrintStartController::execute_print_start`).
- **A rewritten copy loses the hint.** `modify_and_print` starts a copy under another name, so
  `next_file` does not match `print_stats.filename` and the slicer's material applies.
- **No row on firmware that owns the offset.** ZMOD, Forge-X and Helper-Script re-apply their
  own stored value at print start, and a per-head toolchanger sets each head's offset
  absolutely at pickup, so a second per-material value would be overwritten or doubled.

## Adding a row

One entry in `providers()` in `src/printer/material_z_offset.cpp`: detect macro, status
objects, `ready`, the key rule, the merge, the write, and optionally the config section, a
hint macro and a mesh prefix. Nothing else names a storage scheme.

## Mock

`HELIX_MOCK_MATERIAL_ZOFFSET=start_print_vars|helix_macro`
([MOCK_ENVIRONMENT_VARIABLES.md](MOCK_ENVIRONMENT_VARIABLES.md)) runs each row end to end.

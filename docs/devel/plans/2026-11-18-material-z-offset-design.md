# Z offset and bed mesh by material (#2142)

Design agreed with Preston on 2026-11-18. Delete this file in the change that ships the work.

## Intent

A heated bed expands differently at each temperature, so a first layer tuned at PLA's
60°C is wrong at PETG's 80°C or ABS's 110°C. People already work around this with macros
on AD5Ms, Vorons and the K2 (the `_START_PRINT_VARS` / `variable_offset_<MAT>` pattern).
HelixScreen should keep one Z offset, and optionally one stored bed mesh, per material,
apply it at print start whichever program started the print, and let a correction made
during a PETG print change only PETG's value.

Material stands in for bed temperature: people use a handful of materials at a handful of
bed temperatures, and they think in materials.

## Goals

- A per-material Z offset table with a DEFAULT entry, applied at print start.
- An optional stored bed mesh per material, for people who do not mesh every print.
- Save during a print writes the primary material's offset, and nothing else moves.
- Works for prints started from anywhere (Mainsail, Fluidd, slicer upload, the screen).
- Generic: one capability module with a provider row per storage scheme.

## Non-goals

- Keying by bed temperature. The material is the key; its usual bed temperature is shown
  only to explain the value.
- Per-material offsets on printers whose firmware already owns the Z offset (ZMOD, Helper
  Script, toolchangers with per-head offsets). Out of this version.
- Changing how the probe's own `z_offset` is calibrated.

## Anti-goals (a failure even if it works)

- An offset applied twice, or a per-material value folded into the probe's `z_offset`.
  `docs/devel/Z_OFFSET_PERSISTENCE.md` records #1401, where a doubled offset grew on every
  save until the nozzle was on the bed.
- A Save that silently changes other materials' first layers.
- A Klipper restart to save a per-material value.

## Decisions (Preston)

| Question | Decision |
|---|---|
| Approach | C: a provider table, with HelixScreen's own macro as one row |
| Key | Material name; show each entry's usual bed temperature beside it |
| Scope of a Save | Only the primary material's value changes |
| Primary filament | Screen starts: the tool that prints the most by weight. External starts: the first filament the model prints, as the slicer passes it |
| Unknown material | DEFAULT |
| Mesh | In scope: a stored mesh per material, for prints that do not mesh |
| Turning it on | Opt-in in Settings |

## The capability module

`include/material_z_offset.h` + `src/printer/material_z_offset.cpp`, in the
`z_offset_persistence` shape (`.claude/rules/vendor-abstraction.md`). Generic code asks
these questions and never names a firmware:

- Which row matches this printer (detection predicate over discovery), or none.
- The status objects to subscribe so the table can be read.
- Read the table (material -> offset, DEFAULT, mesh profile names) from a status frame.
- The G-code that writes one material's offset now and makes it persist.
- Whether the row can store a mesh per material, and the profile name for a material.

Rows:

1. **`_START_PRINT_VARS` (K2 Kalico and hand-made copies).** Detected by
   `gcode_macro _START_PRINT_VARS` carrying `variable_offset_*`. Read from the macro's
   status. Write: `SET_GCODE_VARIABLE MACRO=_START_PRINT_VARS VARIABLE=offset_<mat>` for
   now, plus an edit of the `variable_offset_<MAT>:` line (added when missing) in the file
   that defines the section, through Moonraker's file API. Kalico keeps it in
   `overrides.cfg`. A hand-made copy is found by scanning the config files for the
   `[gcode_macro _START_PRINT_VARS]` section; if it cannot be found exactly once, the row
   reads but refuses to write, and says why. The start macro applies the value itself.
2. **HelixScreen's macro.** `helix_macros.cfg` gains `HELIX_MATERIAL_OFFSET MATERIAL=<m>`,
   which reads `helix_zoffset_<m>` (else `helix_zoffset_default`) from `[save_variables]`,
   sends `SET_GCODE_OFFSET Z=<value>`, and loads `BED_MESH_PROFILE LOAD=helix_<m>` when that
   profile exists. Write: `SAVE_VARIABLE` (persists at once, no restart). Needs
   `[save_variables]`; the setup flow adds it when missing. The user's `PRINT_START` calls
   the macro after homing and probing and before its own mesh step, and the slicer passes
   `MATERIAL=`. A later adaptive `BED_MESH_CALIBRATE` replaces the stored mesh, which is
   the right order.
3. **No row.** The feature is off; the Settings toggle explains the one-time setup and
   offers to install the macro, as the print-start integration does.

Printers that match a `z_offset_persistence` provider (firmware-managed offset) get no
row, so the feature is not offered there.

## Behavior

- **Applied offset is absolute.** The value is the gcode offset for that material, on top
  of the probe's `z_offset`, never folded into it.
- **Save during a print** (when the feature is on and a row matches): write the current
  live gcode offset into the primary material's entry. No `Z_OFFSET_APPLY_PROBE`, no
  `SAVE_CONFIG`. Today's machine-wide save stays the path when the feature is off.
- **Primary material:** where HelixScreen knows more, it uses it: a print the screen
  starts passes the most-by-weight material (file metadata per-tool grams). An externally
  started print uses the first filament the sliced model prints, which the slicer passes
  as `MATERIAL=` (the setup docs give the slicer start G-code, e.g.
  `MATERIAL={filament_type[initial_extruder]}` in Orca/Prusa). Preston, 2026-11-18.
- **Unknown material:** DEFAULT is used for applying and for saving.
- **Mesh:** after a mesh calibration with the feature on, offer "Save as <material> mesh"
  (the material whose bed temperature is closest, preselected).

## UI

- Settings: a "Z Offset by Material" overlay. A toggle, then one row per material with its
  usual bed temperature and offset (edit via keypad), DEFAULT, and a stored-mesh indicator
  per material. The setup explanation shows when no row matches.
- Print Tune / Controls: the Save Z Offset confirmation names the material it writes
  ("Save +0.080 mm for PETG").
- Every string translatable; declarative XML bound to subjects.

## Left to the builder

- Exact macro text, its version bump, and how the setup flow adds `[save_variables]`.
- The materials listed by default (the material database's common set).
- How the config file scan for hand-made copies is bounded.

## Tests

- The module: detection per row, reading each row's table (including missing DEFAULT and
  materials not in the table), write G-code per row, refusal when a hand-made copy's file
  is ambiguous, no row on firmware-managed printers.
- Primary material selection by weight, with the first-layer fallback.
- The Save path: per-material save never emits `Z_OFFSET_APPLY_PROBE` or `SAVE_CONFIG`.
- The mock: a `_START_PRINT_VARS` printer and a helix-macro printer, each run end to end.

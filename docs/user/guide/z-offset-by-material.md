# Z Offset by Material

Keep a separate first-layer Z offset, and if you like a separate bed mesh, for each material you print. HelixScreen applies the right one at the start of every print and saves your corrections to that material only.

For calibration, baby steps and saving in general, see the [Z offset guide](z-offset.md).

---

## Why a Z Offset per Material?

A heated bed grows a little as it heats up, and the bed surface moves with it. PLA usually prints on a 60°C bed, PETG on 80°C and ABS on 100-110°C, so a first layer you tuned for PLA can come out too squished or too high on PETG or ABS. Many people fix this by hand every time they change material.

With **Z Offset by Material**, each material keeps its own offset:

- PLA prints start with PLA's offset, PETG prints with PETG's, and so on.
- When you fine-tune the first layer during a PETG print and tap **Save**, only PETG's offset changes. Your PLA and ABS first layers stay exactly as they were.
- Each material can also keep its own stored bed mesh, measured at that material's bed temperature, for prints that don't make a fresh mesh.

It works for prints started from the screen, from Mainsail or Fluidd, or uploaded straight from your slicer, because the printer's own start macro applies the offset.

**Use it when** you switch materials often and see the first layer change between them. If you print one material, or your first layer is the same across materials, you don't need it.

---

## Turning It On

1. Open **Settings > Printing > Z Offset by Material**.
2. If the screen shows the list of materials, switch on **Z offset by material**. You're done.
3. If it shows **One-time setup** or asks you to update the helper macros, follow the setup for your printer below, then come back and switch it on.

The setting is kept for each printer separately.

---

## Setup

There are three cases. HelixScreen picks the right one by itself; you only need to do the steps for yours.

### Creality K2 Plus with Jacobean's firmware

Jacobean's firmware already keeps an offset for each material in a macro called `_START_PRINT_VARS`, and its start macro applies it. No setup is needed: open **Settings > Printing > Z Offset by Material** and switch it on.

Make sure your slicer passes the material to `START_PRINT` (see [The slicer's start G-code](#the-slicers-start-g-code) below), as the firmware's own setup notes ask. Without it, every print uses the default offset.

HelixScreen writes your saved values to the file that holds `_START_PRINT_VARS` (on this firmware, `overrides.cfg`).

### Your own `_START_PRINT_VARS` macro

If you made your own copy of this pattern, it works the same way. HelixScreen recognizes it when:

- a section `[gcode_macro _START_PRINT_VARS]` holds lines like `variable_offset_PLA: 0.02` and `variable_offset_DEFAULT: 0`, and
- your start macro applies the value for the print's material with `SET_GCODE_OFFSET Z=`, for example:

  ```
  {% set vars = printer["gcode_macro _START_PRINT_VARS"] %}
  {% set offset = vars.get("offset_" ~ params.MATERIAL|lower, vars.offset_default) %}
  SET_GCODE_OFFSET Z={offset}
  ```

The section must appear in exactly one file that your `printer.cfg` loads, directly or through `[include ...]` lines. HelixScreen finds that file itself.

### Any other printer: HelixScreen's macro

1. **Install or update the helper macros.** Tap **Install Helper Macros** on the setup screen, or use **Helper Macros** on the **Advanced** panel. They add a macro called `HELIX_MATERIAL_OFFSET`. Restart Klipper when asked.

   The offsets are stored in Klipper's `[save_variables]`. If your printer doesn't have a `[save_variables]` section, the helper macros add one, which stores its values in `helix_variables.cfg` next to your `printer.cfg`. If you already have your own `[save_variables]`, HelixScreen uses yours.

2. **Call the macro in your `PRINT_START`.** Put it after homing and probing, and before your own bed mesh step:

   ```
   [gcode_macro PRINT_START]
   gcode:
       G28
       # heat the bed, level the gantry, and so on
       HELIX_MATERIAL_OFFSET MATERIAL={params.MATERIAL|default("")}
       BED_MESH_CALIBRATE ADAPTIVE=1    ; optional, see "Stored meshes" below
       _HELIX_READY
   ```

3. **Pass the material from your slicer.** See the next section.

### The slicer's start G-code

Your slicer has to tell the printer which material it's printing. Add `MATERIAL=` to the `PRINT_START` (or `START_PRINT`) line of the printer's start G-code:

| Slicer | Add to the start line |
|---|---|
| OrcaSlicer | `MATERIAL={filament_type[initial_extruder]}` |
| PrusaSlicer, SuperSlicer | `MATERIAL={filament_type[initial_extruder]}` |
| Bambu Studio | `MATERIAL={filament_type[initial_extruder]}` |
| Cura | `MATERIAL={material_type}` |

A full OrcaSlicer start line looks like this:

```
PRINT_START BED_TEMP=[first_layer_bed_temperature] EXTRUDER_TEMP=[first_layer_temperature] MATERIAL={filament_type[initial_extruder]}
```

The name the slicer sends is the material type of your filament profile (`PLA`, `PETG`, `ABS`, `PETG-CF`...). Upper and lower case don't matter.

---

## The Settings Screen

**Settings > Printing > Z Offset by Material** lists:

- **Any other material**: the default offset, used for a material without its own entry and for prints where the slicer passed no material.
- **PLA, PETG, ABS, ASA, TPU, PA and PC**, each with its usual bed temperature to remind you why its offset differs.
- A grid mark beside a material that has a stored bed mesh.

Tap an offset to type a new value in millimeters, with up to three decimals (for example 0.035). Negative values move the nozzle closer to the bed. The new value is saved straight away and used from the next print of that material.

---

## Saving During a Print

1. Start the print and watch the first layer.
2. Adjust it with the baby steps in **Tune** (see [Z-Offset / Baby Steps](printing.md#z-offset--baby-steps)).
3. Tap **Save** (the check mark in the top bar, or **Save Z-Offset** on the Controls panel).
4. HelixScreen asks, for example, **"Save +0.040 mm for PETG? Other materials keep their own offsets."** Tap **Save**.

The saved value is the whole offset for that material, not just your last adjustment. Saving does not restart Klipper and does not move the printer, so you can save in the middle of a print. You can also save after the print ends; the value goes to the material of the print that just ran.

**Which material is "the print's material"?**

- A print you start **from the screen** uses the material the file prints the most of, by weight. On a multi-color print that is usually the main color's material, not the first one. (With HelixScreen's macro, the screen tells the printer which one; see the gotchas for the K2.)
- A print started **anywhere else** uses the first filament your slicer names in `MATERIAL=`.

---

## Stored Meshes per Material

This needs HelixScreen's macro; the K2 firmware handles its meshes its own way.

1. Heat the bed to the temperature you print the material at.
2. Calibrate a bed mesh on the **Bed Mesh** screen.
3. When it finishes, HelixScreen offers **"Save as PETG mesh?"**, picking the material whose usual bed temperature is closest to your bed's. Tap **Save**.
4. Accept the usual prompt to save the configuration. Klipper keeps bed meshes in `printer.cfg`, so this one restarts Klipper. Do it while the printer is idle.

From then on, `HELIX_MATERIAL_OFFSET` loads that mesh at the start of each print of that material. If your `PRINT_START` makes its own mesh afterwards, that fresh mesh is used instead.

---

## The Default Offset

**Any other material** (DEFAULT) is used when:

- the print's material has no entry of its own, or
- the slicer didn't pass a material at all.

If you never set it, it is 0, which means no offset beyond your probe's own.

---

## Gotchas and Limitations

- **The offset is on top of your probe's Z offset.** The per-material value is a G-code offset added to whatever your probe's `z_offset` already gives. It is never written into the probe's own setting. Tune the probe once with the normal Z calibration, then use the per-material values for the small differences between materials.
- **Turning it on changes what Save does.** With the setting on, **Save** always writes one material's value and never changes the probe's offset. Turn it off to go back to the normal machine-wide save.
- **Screen starts and other starts can pick different materials.** A screen start uses the material printed most by weight; a start from Mainsail, Fluidd or the slicer uses the first filament the slicer names. On a single-material print they are the same.
- **K2 (`_START_PRINT_VARS`) prints always use the slicer's material.** The K2's start macro reads only the slicer's `MATERIAL=`, so a multi-material print started from the screen still uses the first filament, and Save writes to that material.
- **No `MATERIAL=`, no per-material offset.** If the slicer doesn't pass a material, the default offset is used, and Save writes the default.
- **Not offered when your firmware stores the Z offset itself.** On the FlashForge AD5M and AD5X with ZMOD or Forge-X, Creality printers with the Helper Script's Z-offset saving, and tool changers whose firmware keeps an offset per head, the firmware re-applies its own value at every print and would fight a second one. The setting isn't shown there.
- **Something that sets the offset after `HELIX_MATERIAL_OFFSET` wins.** The last `SET_GCODE_OFFSET Z=` in your start sequence decides. If a later macro sets its own offset (a tool pickup on a tool changer, a probe "touch" routine, or another offset macro), the per-material value is replaced. Call `HELIX_MATERIAL_OFFSET` after anything like that, or remove the other one.
- **An adaptive mesh replaces the stored one.** A `BED_MESH_CALIBRATE` after `HELIX_MATERIAL_OFFSET` in your `PRINT_START` makes a fresh mesh, so the stored per-material mesh is only used by prints that don't mesh.
- **The macro must have run since Klipper last started.** With HelixScreen's macro, the screen learns the print's material from the macro itself. Until the first print after a Klipper restart has run it, no Save is offered. If Save never appears, check that your `PRINT_START` calls `HELIX_MATERIAL_OFFSET`.
- **A `_START_PRINT_VARS` file HelixScreen can't find: changes don't survive a restart.** If the section is in more than one loaded file, or in none that `printer.cfg` loads (for example only in a file included by an absolute path), the screen still reads the values and Save still changes the running value, but it can't write the file. You'll see **"Applied for this session only"** with the reason. Keep the section in one loaded file.
- **A new material on a K2 waits for a restart.** If you save for a material that `_START_PRINT_VARS` doesn't list yet, HelixScreen adds its line to the file, and the value starts working after the next Klipper restart.
- **Only seven materials are listed in Settings.** Other materials (PETG-CF, PLA+, PVA...) still get their own value when you save during a print, but you can't edit them from the Settings screen.
- **Material names.** With HelixScreen's macro, `-`, spaces, `+` and `.` in a material name become `_`, so `PETG-CF` and `PETG CF` share one entry. A name with other symbols (such as `/`) can't be stored, and that print uses the default.
- **Saving a mesh restarts Klipper.** Unlike the offsets, a stored mesh is written by Klipper's `SAVE_CONFIG`. A mesh calibrated on a cold bed isn't offered for any material.
- **Prints started from the Job Queue widget** use the slicer's first filament rather than the most-by-weight material.

---

## Related

- [Printing](printing.md): starting a print and the Tune overlay
- [Z offset guide](z-offset.md)
- [Settings: Printing](settings/printing.md#z-offset-by-material)
- [Advanced](advanced.md): installing the helper macros

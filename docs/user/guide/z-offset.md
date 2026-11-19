# Z Offset

The Z offset sets how close the nozzle sits to the bed when a print starts. A few hundredths of a millimeter decide whether the first layer sticks, squishes or scrapes. This page explains what the Z offset is, how to calibrate it, how to adjust it during a print, what **Save** does, and where printers behave differently from the standard case.

Jump to [Gotchas and limitations](#gotchas-and-limitations) if you only want the traps.

## Contents

- [What the Z offset is](#what-the-z-offset-is)
- [Where HelixScreen shows it](#where-helixscreen-shows-it)
- [Calibrating the Z offset](#calibrating-the-z-offset)
- [Adjusting during a print (baby steps)](#adjusting-during-a-print-baby-steps)
- [Saving the offset](#saving-the-offset)
- [Printers whose firmware keeps the offset itself](#printers-whose-firmware-keeps-the-offset-itself)
- [Per-tool offsets on tool changers](#per-tool-offsets-on-tool-changers)
- [The Save Z-Offset Buttons setting](#the-save-z-offset-buttons-setting)
- [Z offset by material](#z-offset-by-material)
- [Gotchas and limitations](#gotchas-and-limitations)

---

## What the Z offset is

Klipper needs to know where the bed is. It learns that from one of two places:

- **A probe** (a BLTouch, an inductive probe, a Klicky, or a scanning probe such as a Cartographer or Beacon). The probe triggers a little above or below the nozzle's real contact point, and the **probe Z offset** in your config is the distance between the two.
- **A Z endstop switch** at the end of the Z axis. The distance from the switch to the bed is the **position endstop** value in your config.

Whichever one your printer uses holds your "Z zero". Calibrating means finding the number that puts the nozzle exactly at the bed, and saving it means writing that number into your printer config so it survives a restart.

On top of that saved number, Klipper keeps a temporary **gcode offset** for the current session. Baby steps change the gcode offset. It is live, it applies to the print that is running, and it disappears when Klipper restarts unless something saves it. Saving folds the gcode offset into the saved number (the probe offset or the endstop position) and then clears it.

HelixScreen picks the right save method for your printer automatically: probe, endstop, or "the firmware handles it". You do not choose.

---

## Where HelixScreen shows it

| Place | What you get |
|-------|--------------|
| **Advanced > Z-Offset Calibration** | The guided calibration, for setting the offset from scratch. Also reachable as **Z-Offset Cal** in the Probe Management overlay, and as a **Z Calibration** button on the Controls panel. |
| **Tune** on the print screen | Baby steps during a print. See [Print Tune Overlay](printing.md#print-tune-overlay). |
| **Z-Offset** row on the **Controls** panel | The current offset, the same baby-step controls, and the **Save Z-Offset** button once you have an unsaved change. |
| **Z-Offset** home widget | The offset, the baby-step buttons and **Save** on the home screen. See [Z-Offset widget](home-widgets-print.md#z-offset-widget). |

Next to the offset, the Print Tune overlay can show a line such as "Saved 0.150 mm", the value stored in your printer config, or "Saved 0.150 mm, 0.200 mm after Save" when saving now would change it. The "after Save" number moves opposite to your baby steps, because saving folds the adjustment into the stored value. The line is left out where it would mislead: on firmware-managed printers, when saving is turned off, on tool changers with per-tool offsets, and on probes (such as Cartographer and Beacon) that keep their offset in their own calibration instead of in a simple config value.

---

## Calibrating the Z offset

Use calibration when you first set up a printer, after changing a nozzle, probe or build plate, or whenever the first layer is far off and baby steps are not enough. The panel opens at **Advanced > Z-Offset Calibration**.

1. Make sure nothing is on the bed and the nozzle is clean. HelixScreen asks **Is the bed clear?** first if your last print may still be on the plate.
2. Optionally turn on **Warm Bed**. The bed heats to 45 degrees C before calibrating, which accounts for thermal expansion. Leave it off if you only ever print with a cold bed.
3. Tap **Start**. The printer homes if needed, moves over the center of the plate, and starts the calibration.
4. Do the paper test. Slide a sheet of paper under the nozzle and use the **+/-** buttons until the paper drags slightly but still moves. Smaller steps are available for the last fine adjustments.
5. Tap **Accept** when the paper test feels right, or **Abort** to cancel without changing anything.
6. HelixScreen saves the result. For most printers this restarts Klipper, so the screen disconnects briefly and then reconnects.

Calibrate at your normal printing temperatures if you can. A hot nozzle and a heated bed expand, so a paper test on a cold machine can leave you a little off at print temperature. A bed warm-up is the quick way to close most of that gap.

### Which command HelixScreen runs

HelixScreen looks at your printer and picks one of these. The screen you see is the same in each case.

| Your printer | What HelixScreen runs | What Accept saves |
|--------------|-----------------------|-------------------|
| A standard probe (BLTouch, inductive, Klicky and similar) | `PROBE_CALIBRATE` | The probe's Z offset |
| A **Cartographer** | `CARTOGRAPHER_SCAN_CALIBRATE` | The probe's own calibration |
| A **Beacon** | `BEACON_CALIBRATE` | The probe's own calibration |
| Another **eddy-current probe** (for example BTT Eddy) | `PROBE_EDDY_CURRENT_CALIBRATE` | The probe's own calibration |
| A Z endstop switch (Voron 2.4 style or an Ender with a mesh-only probe) | `Z_ENDSTOP_CALIBRATE` | The stepper's endstop position |
| Firmware-managed (see below) | A manual "lower to Z 0.1 and adjust" routine | The offset, applied to the firmware, which stores it |

A printer is treated as using an endstop when its Z stepper has a numeric position endstop in the config. A printer with no probe and no such value is treated as an endstop machine, except delta printers with no probe, which are treated as firmware-managed.

### Scanning probes: Cartographer, Beacon, eddy current

These calibrate differently from a touch probe, and the difference matters:

- The calibration starts with the paper test at the **nozzle**, then the printer moves the sensor over the same spot and runs its own scan. Accept can take noticeably longer than on other probes, because the probe sweeps after you accept.
- The result lives in the probe's own calibration model, not in a single Z-offset number. That is why the "Saved ... mm" line does not appear for these probes.
- On a printer whose firmware otherwise manages the offset (see [below](#printers-whose-firmware-keeps-the-offset-itself)), a scanning probe still gets its own calibration, because that calibration is the probe's own measurement and saving it never adds your gcode offset on top.
- If your printer uses **one shared probe on a tool changer**, the probe's offset belongs to one tool. HelixScreen asks you to confirm before calibrating with a different tool mounted, and does not change tools for you. See [Per-tool offsets on tool changers](#per-tool-offsets-on-tool-changers).

---

## Adjusting during a print (baby steps)

Baby steps change the gcode offset while a print is running, so you can nudge the nozzle with the first layer going down in front of you.

1. Start the print and tap **Tune**, or use the **Z-Offset** row on **Controls**, or the **Z-Offset** home widget.
2. Pick a step size: 0.05, 0.025, 0.01 or 0.005 mm. HelixScreen remembers your choice.
3. Tap an arrow button. The nozzle moves closer to the bed or farther from it right away. Follow the help line under the buttons ("Closer = more squish | Farther = less squish"), since the arrow icons differ on printers whose bed moves in Z.
4. Repeat until the lines are flat and joined without scraping.

For what a good or bad first layer looks like, see the symptom table in [Z-Offset / Baby Steps](printing.md#z-offset--baby-steps).

**A limit on how far you can go:** baby steps stop at 2 mm either side of the offset you started from. On the Print Tune overlay the starting point is the offset when you open the overlay, so reopening it starts a new range. On the Z-Offset home widget it is the offset when the widget appeared, and it resets when you save. This stops a stuck button from driving the nozzle into the bed. If you really need more than that, calibrate instead.

### What the tune hint means

Under the Z-offset controls in Print Tune, a short hint says what will happen to your adjustment.

| Hint | What it means |
|------|---------------|
| **Temporary adjustment for this print, unless saved on Controls panel** | The normal case. The adjustment lasts until the print ends or Klipper restarts. Save it from Controls to keep it. |
| **Z-offset is auto-saved by firmware** | Your printer's firmware stores the offset itself. What you dial in is kept and applied at the start of the next print. There is nothing for you to save. |
| **Saved for the mounted head; other heads keep their own** | The firmware keeps a separate offset for each toolhead and saves the one you adjust. |
| **Temporary adjustment for this print** | Nothing will save it, usually because **Save Z-Offset Buttons** is turned off. Turn it on again if you want to save. |
| **Temporary for this print; a tool change resets it** | Nothing saves it, and the next tool change puts that head's own stored offset back. This is how the Snapmaker U1 works. |

---

## Saving the offset

After a print finishes or is canceled, an adjustment you made with baby steps is still only the temporary gcode offset. To keep it:

1. Tap **Save Z-Offset** on the **Controls** panel (or **Save** on the Z-Offset home widget). The button appears once you have an unsaved change and is grayed out while a print is running or paused.
2. Read the confirmation. HelixScreen warns you because saving **restarts Klipper**, and the printer disconnects for a few seconds.
3. Tap **Save**. A "Z-offset saved" notice appears once Klipper is back.

What happens behind the scenes: HelixScreen asks Klipper to write the adjustment into the probe's Z offset (or the endstop position), then runs `SAVE_CONFIG`, which rewrites your printer config and restarts Klipper. The temporary offset resets to zero because it is now part of the saved value.

When the Save button never restarts Klipper: on a firmware-managed printer, nothing is sent to Klipper at all, and on tool changers whose firmware persists a tool's offset immediately, a per-tool save may not restart anything. HelixScreen only shows the restart warning when a restart is actually coming.

If your printer's Klipper does not offer the command Klipper uses to fold the offset into the config (`Z_OFFSET_APPLY_PROBE` for a probe, `Z_OFFSET_APPLY_ENDSTOP` for an endstop), HelixScreen refuses to save and says "This printer has no Z_OFFSET_APPLY_PROBE command, so the Z-offset cannot be saved." It does this rather than report a save that kept nothing. HelixScreen checks the printer's command list first, so this appears before anything is changed. You can still calibrate, and you can set the number by hand in your config.

Calibration saves its own result when you tap **Accept**, so you do not need the Save button afterward.

---

## Printers whose firmware keeps the offset itself

Some printers run firmware that stores the Z offset in its own place instead of in your Klipper config. The firmware usually pushes the stored value into Klipper at the start of a print and clears it again when the print ends.

HelixScreen detects this on its own when it connects. The firmware it recognizes:

| Firmware | Typical printers | How HelixScreen finds it |
|----------|------------------|--------------------------|
| **ZMOD** | FlashForge Adventurer 5M, 5M Pro and 5X (AD5M, AD5X); the Creator 5 head changer is handled per head | ZMOD's save command |
| **Forge-X** | FlashForge Adventurer 5M and 5M Pro | The Forge-X parameter macro |
| **Helper Script** | Creality K1, K1C and K1 Max, and others running Guilouz's Helper Script with its save-zoffset feature | A wrapper around Klipper's `SET_GCODE_OFFSET` command together with a stored `zoffset` value |

When one of these is detected, HelixScreen changes how it behaves:

- **There is no Save step.** The **Save Z-Offset** button never appears, the **Save Z-Offset Buttons** setting is hidden, and the tune hint reads "Z-offset is auto-saved by firmware". Whatever you adjust is stored by the firmware.
- **The number shown is the stored offset, not Klipper's live one.** On these firmwares Klipper's own value reads 0.000 whenever the printer is idle, even though a real offset is stored. Other interfaces such as Mainsail or Fluidd may show 0.000. HelixScreen shows the stored value while idle, so the number matches what your next print will use. During a print it shows the live value, since baby steps land there first.
- **Adjustments while idle are applied as an exact value.** Nudging a firmware-managed printer while idle sets the new total offset directly instead of sending a small change on top of zero, which would make the firmware throw away the stored value. You do not need to do anything for this, but it explains why idle changes behave correctly here.
- **Calibration applies the result and the firmware saves it.** The paper-test routine for these printers applies your final value, and the firmware stores it. Saving to the Klipper config is skipped on purpose, because on these machines it would stack the stored offset on top of the saved one every time and the nozzle would sink closer to the bed with each save.
- **HelixScreen turns on the firmware's stored-offset loading once.** On ZMOD it sends `SAVE_ZMOD_DATA LOAD_ZOFFSET=1`, and on Forge-X `SET_MOD PARAM="load_zoffset" VALUE=1`, so the firmware applies the stored offset at print start. It sends it once per printer, only while no print is active, and not at all if the firmware already reports the setting on. If the command fails, HelixScreen tries again the next time it connects. The Helper Script needs nothing and gets nothing.
- **Scanning probes still calibrate with their own routine.** See [Scanning probes](#scanning-probes-cartographer-beacon-eddy-current).

> **A caveat on Helper Script detection.** The Helper Script is recognized by the wrapper it puts around `SET_GCODE_OFFSET`. Other setups wrap that command too (for logging, limits or per-tool offsets) without storing anything. HelixScreen treats a printer as firmware-managed the moment it sees the wrapper, because treating a saving printer as non-saving is the dangerous mistake. It backs off only when the printer reports its stored variables and there is no stored `zoffset` at all. A printer with such a wrapper and no stored-variables section can therefore keep the Save button hidden even though Klipper would save normally. If that is you, set the offset in your config by hand or use calibration with a probe command.

The FlashForge pages cover these printers further: [Adventurer 5M](install-ad5m.md) and [Adventurer 5X](install-ad5x.md). For what the display shows when ZMOD is the firmware, see [Printing: Z-Offset](printing.md#z-offset--baby-steps).

---

## Per-tool offsets on tool changers

Printers with several toolheads have a separate Z offset for each tool, on top of the machine-wide one.

- On the Z-offset controls, a selector chooses what an adjustment applies to. **Global** moves every tool together. A tool button adjusts only that tool and leaves the others alone, which helps when one nozzle sits a little higher or lower than the rest. The other target's offset shows in muted text beside the selector.
- HelixScreen refuses to nudge a tool whose offset the firmware has not reported yet, rather than guess a starting value and overwrite what is stored.
- The calibration steps use your probe's own offset. When a probe is shared by all tools, it belongs to one reference tool. HelixScreen asks you to confirm, naming the reference tool, if a different tool is mounted when you start, and does not mount a tool for you.
- Some tool changers measure every tool's offsets automatically. See [Tool Offsets](calibration.md#tool-offsets) for the automatic procedure, and [Tool changers](filament-tool-changers.md) for hardware notes.
- **Save buttons are off by default on tool changers**, because a machine-wide save folds the live offset into the probe or endstop underneath every tool at once. One stray tap would change all of them. Turn it on in the [save setting](#the-save-z-offset-buttons-setting) if you really want it. The save then writes the tool offsets (and the machine-wide one if it changed), and a save that restarts Klipper puts your unsaved live offset back once Klipper returns.
- **Printers that keep an offset per head (Snapmaker U1, and the Creator 5 when HelixScreen finds Z-Mod's per-head storage):** the Z-Offset calibration panel refuses to accept a result with the message "Each head keeps its own Z offset on this printer. Use its tool offset calibration instead." On the Snapmaker U1 each head's stored offset is re-applied whenever that head is picked up, so a baby step lasts only until the next tool change or the end of the print. Use the printer's own tool offset calibration instead: [Toolhead Offset Calibration](filament-tool-changers.md#toolhead-offset-calibration) on the U1, [Tool Offsets](calibration.md#tool-offsets) elsewhere.

---

## The Save Z-Offset Buttons setting

**Settings > Printing > Motion > Save Z-Offset Buttons** controls whether HelixScreen offers to save Z-offset changes to your printer config at all.

| State | Effect |
|-------|--------|
| **On** | The Save button shows on the Controls panel, the Z-Offset home widget and the header whenever there is an unsaved change, and calibration saves its result on Accept. |
| **Off** | Every Save button is gone. Calibration still runs, but Accept stages the result without saving it: Klipper holds it until the next config save from anywhere (for example, from Mainsail), and the completion screen says so. Baby steps still adjust the current session. |

The default depends on the printer: **on** for ordinary printers, **off** on tool changers where each tool keeps its own Z offset. Switching it writes an explicit choice for that printer, and the choice is never reset to the default. The row does not appear on firmware-managed printers, because there is no save to turn off.

The same setting appears as `z_offset.save_mode` in [Printer settings](../configuration/printer.md#z-offset), and the baby-step size is `z_offset.step_index`.

---

## Z offset by material

Different build surfaces and materials often want a slightly different first-layer height. See [Z offset by material](z-offset-by-material.md) for how to handle that.

---

## Gotchas and limitations

- **An unsaved baby step is lost.** The adjustment is a temporary gcode offset. It disappears when Klipper restarts (including after any config save from another interface or a firmware restart), and on some printers it does not outlast the print. Save it from Controls after the print if you want to keep it.
- **Save restarts Klipper.** It runs `SAVE_CONFIG`. Expect a brief disconnect, an interrupted view of the print screen, and a reconnect. It is blocked while a print is running or paused, for that reason. HelixScreen asks you to confirm each time.
- **Save may be hidden, grayed out or behave differently.** It is hidden on firmware-managed printers, when **Save Z-Offset Buttons** is off (the default on tool changers), and when there is nothing to save. It is grayed out during a print.
- **Your start macro can override your offset.** If your `PRINT_START` (or the slicer's start G-code) calls `SET_GCODE_OFFSET Z=...`, or loads a stored value, it can replace what you dialed in. The same applies to a baby step made during the pre-print steps, which the start macro may overwrite. Adjust once the print has begun, and keep one source of truth for the offset.
- **Scanning probes calibrate differently.** Cartographer, Beacon and eddy-current probes run their own calibration with a scan after you accept, and they keep their result in the probe's own model. The "Saved" line does not apply, and a plain Z-offset number in your config may not exist at all.
- **Firmware-managed printers show the stored offset, not Klipper's.** Mainsail and Fluidd can show 0.000 while idle. That is the firmware clearing its live copy, not a lost offset.
- **Do not save through another interface on a firmware-managed printer.** Folding the offset into the probe while the firmware also re-applies it stacks the offset on every save. On one printer this took a probe offset from 0.060 to 2.515 mm over five saves and ended with the nozzle on the bed.
- **Tool changers have per-tool offsets.** A machine-wide save moves every tool at once, which is why saving is off by default there. On a Snapmaker U1, a baby step lasts only until the next tool change.
- **Baby steps stop 2 mm from where a run of steps began.** Calibrate if you need more.
- **Calibration warm-up is only the bed.** The nozzle is not heated by the Z-Offset panel's **Warm Bed** option.
- **Some firmware cannot save.** If Klipper does not offer the apply command for your setup, HelixScreen says so and does not save.
- **Delta printers without a probe** (and no printer-database entry that says otherwise) are treated as firmware-managed, so HelixScreen offers no Save step. Adjustments are temporary unless something on the printer stores them.

---

## Related

- [Printing: Z-Offset / Baby Steps](printing.md#z-offset--baby-steps): reading a first layer and the Print Tune overlay.
- [Calibration & Tuning](calibration.md): the other calibrations, tool offsets and probe management.
- [Settings: Printing](settings/printing.md) and [Motion settings](motion.md#motion-settings): where the save setting lives.
- [Z-Offset home widget](home-widgets-print.md#z-offset-widget)
- [Tool changers](filament-tool-changers.md)

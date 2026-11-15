# Filament Mapping

Which slot feeds which tool, for one print and as a standing table, and how the assignments reach OrcaSlicer. Back to [Filament Management](filament.md).

---

## Filament Mapping

<a id="tool-mapping"></a>

For multi-tool prints, you can control which slot feeds each tool the slicer expects (T0, T1, T2, …). The **FILAMENTS** card on the file detail screen shows one chip for each tool the print uses.

**Reading a chip.** Each chip is split in two:

- **The top band** is what the file asks for: it is filled with the file's color for that tool and labeled with the file's material (or the tool's name, such as **T2**, when the file names no material). On 800x480 screens and larger the material is in bold.
- **The bottom** is what the printer will feed: a spool dot in the color of the slot the tool is mapped to, the slot's number and a small arrow. On a filament system with one unit the number is the slot itself ("3"). With several units it reads unit, then slot: "2·1" is the first slot of the second unit. A tool with no slot shows an empty dashed spool and the arrow; an empty slot shows a red ring instead of the spool dot.
- **The outline** tells you whether the pairing is a problem:
  - **Grey**: the slot holds what the file asks for.
  - **Amber**: the material matches but the color is different. The chip only turns amber when the file says which color it wants.
  - **Red**: the tool has no slot, its slot is empty, or the slot holds a different material.

Chips are narrower on small screens. When the print uses more tools than fit in the row, a **+N** pill stands in for the rest.

**Changing one tool's slot.** Tap a chip. A list opens under it, headed with the tool and its material (for example **T2 · PLA**):

- **Auto** comes first, and lets HelixScreen pick the closest loaded match.
- Then every slot, best match first, each with its color, name, material and a short reason: **exact match**, **color differs**, **material differs** or **empty**. A slot another tool already uses adds that tool, for example "exact match, also T0". Two tools may share a slot.
- The tool's current pick is highlighted.
- **More options…** at the bottom opens the full **Filament Mapping** dialog.

Pick a slot and the chip updates straight away. If your filament system can't remap this file (see [Printers that map filament by rewriting the file](#printers-that-map-filament-by-rewriting-the-file)), the chips' arrows are hidden: tapping a chip offers to install the HelixPrint plugin when that is what's missing, and otherwise does nothing.

Tapping the card's header opens the full **Filament Mapping** dialog, with one row per tool in the file:

- **Map to closest colors with matching material** (toggle at the top): When on, HelixScreen auto-assigns each tool to the loaded slot with the nearest color and a compatible material; the rows become read-only. When off, you assign tools yourself.
- **Manual assignment**: With the toggle off, tap a tool's row to pick which slot feeds it. The list is the same one a chip opens.
- **Mismatch warnings**: A warning icon appears on any row mapped to an empty slot or a slot whose material doesn't match what the tool needs.

Tap **Done** to keep your mapping, or **Cancel** to discard it.

**Printers with no filament system.** On a single-extruder printer without a filament system, the card still shows when the file names its material, so you can check it against what is loaded. There is one chip per tool the print uses. The first chip's bottom is a spool dot in the color of the filament loaded in the extruder, and its outline compares the file with that filament, judged by the same outline colors. Later chips show a swap icon (two arrows), because a file that changes filament mid-print (with `M600`) asks you to swap spools; they stay grey. When HelixScreen doesn't know what is loaded, the first chip shows a question mark with a grey outline. These chips have no arrow and open nothing: there is nothing to map.

> **Tip:** When you actually start the print, HelixScreen re-checks these mappings and stops with a **Check filament** dialog if any required tool points at an empty slot: unless bypass is engaged, in which case the mappings aren't feeding the print and the check is skipped. See [Print Monitoring & Failure Detection](print-monitoring.md#before-a-print-starts).

> **Note:** On a filament system, the card only appears when the system supports Filament Mapping. On fixed 1:1 systems (Snapmaker U1, ACE) tools always map directly to their matching slot, so there's nothing to assign.

> **Note:** The card also hides itself while **bypass is engaged on a single-tool file**, because the print takes its filament from the external spool and the mapping decides nothing: showing it would offer an assignment the print ignores. The **Bypass active** note on the file detail screen appears in its place. A *multi-tool* file with bypass engaged still shows the card: those prints do use the lanes.

## Printers that map filament by rewriting the file

<a id="printers-that-remap-by-rewriting-the-file"></a>

Most filament systems carry out your mapping themselves: HelixScreen tells the printer "tool 0 comes from slot 2" and the printer does the rest. A few can't, because their firmware has no mapping table. On those, HelixScreen rewrites a copy of the G-code with the new tool numbers and prints that copy. Today that means a tool changer that swaps with its own `T<n>` macros, such as a [MedusaHC](filament-tool-changers.md#medusahc-hotend-changer).

This needs the **HelixPrint plugin** on your printer. Without it the mapping card offers to install it instead. A current plugin makes the rewritten copy on the printer itself; with an older one, the screen downloads the file, rewrites it and uploads the copy. With it, the finished job is listed in print history under its original name ([details](print-history.md)).

**What the rewrite changes:**

- Tool changes: `T0`, `T1`, ...
- Temperature commands for a specific tool: `M104 T1 S220`, `M109 T0 S230`
- An `INITIAL_TOOL=` or `TOOL=` value on any command line, such as `PRINT_START INITIAL_TOOL=0` or `SET_TOOL_TEMPERATURE TOOL=1`, so a start macro that picks up or primes the first tool picks up the right one

**What it does not change:** any other per-tool value you pass to your start macro, such as `EXTRUDER_TEMP=`, `EXTRUDER1_TEMP=`, `T0_TEMP=`, `TOOL_TEMP=` or `T=`, and a tool given by name rather than number (`TOOL=T0`). Those names are your own macro's, and HelixScreen can't know whether `EXTRUDER_TEMP` means "tool 0" or "the tool this print starts with", so it leaves them as sliced. If your start macro heats tools from those values, a job whose tool 0 you map to tool 1 heats tool 0 and then switches to a cold tool 1.

When the file you picked passes values like these and your picks move at least one tool to a different number, the **Filament Mapping** dialog shows a warning naming them, for example:

> Your PRINT_START line passes per-tool settings that Filament Mapping does not change (EXTRUDER_TEMP, EXTRUDER1_TEMP). A tool you map filament to may not be heated.

The warning appears and disappears as you change the picks: leave every tool on its own number and there is nothing to warn about, because the file prints as sliced. It doesn't stop you mapping filament; if your macro doesn't use those values to heat anything, you can ignore it.

**Recommended start G-code.** Do the tool heating in plain `M104`/`M109` lines, which the rewrite does change, and keep only the bed, homing and mesh in `PRINT_START`. In OrcaSlicer's **Machine start G-code** (one `M104` line per tool your printer has):

```
PRINT_START INITIAL_TOOL=[initial_tool] BED_TEMP=[bed_temperature_initial_layer_single]
{if is_extruder_used[0]}M104 T0 S{idle_temperature[0]}{endif}
{if is_extruder_used[1]}M104 T1 S{idle_temperature[1]}{endif}
{if is_extruder_used[2]}M104 T2 S{idle_temperature[2]}{endif}
{if is_extruder_used[3]}M104 T3 S{idle_temperature[3]}{endif}
M109 T[initial_tool] S{first_layer_temperature[initial_tool]}
```

Keep the `M109` for the first tool last. It sets that tool from its idle temperature up to printing temperature and waits, so it has to come after the idle lines. `INITIAL_TOOL=` and `TOOL=` are fine to keep if your macro uses them: the rewrite moves them along with everything else. OrcaSlicer adds a `T[initial_tool]` line right after the start G-code, and that line is rewritten too.

## Syncing with OrcaSlicer (2.3.2 and later)

When you edit spool info in HelixScreen, on any supported filament system (AD5X IFS, Snapmaker U1, ACE, CFS), that information is saved to your printer in the standard location OrcaSlicer 2.3.2 and later reads automatically. Open OrcaSlicer after editing and your slot's vendor, material, color, and temperatures show up in the filament panel with no extra setup.

**AFC (Box Turtle) and Happy Hare** work the same way automatically: your lane assignments flow through to OrcaSlicer with nothing to configure.

Either way, your printer's filament info and OrcaSlicer stay in sync. The sync is one-way (your printer → OrcaSlicer): editing in OrcaSlicer doesn't change what's loaded in your AMS.

**Precise names on-screen, matchable names in OrcaSlicer.** You can name your filament as specifically as you like ("ASA-GF", "PLA Silk", "PPS-CF"), and HelixScreen keeps showing that exact name on the printer. OrcaSlicer only recognizes broader material families, so when HelixScreen syncs it automatically translates your precise name to the closest one OrcaSlicer knows. That's why a slot showing "ASA-GF" on your printer may appear as "ASA" in OrcaSlicer. This is expected, the color and temperatures still come across correctly, and OrcaSlicer now picks a real ASA preset instead of falling back to a generic PLA one.

> **Tip:** For an unusual material OrcaSlicer doesn't recognize at all, the slot may sync with its color and temperatures but no material selected, rather than a wrong guess. Just pick the filament yourself in OrcaSlicer that one time: your printer keeps showing the precise name.

> **Requirements:** OrcaSlicer 2.3.2 or newer, connected to the same printer's Moonraker. Nothing to enable on the HelixScreen side: it's automatic.

> **OrcaSlicer 2.5:** sync works the same way, with nothing to change. 2.5 also re-reads the slots when you switch OrcaSlicer to a different printer, so each printer's filaments show up without a manual sync. When a slot's filament is a product from HelixScreen's filament catalog with its own OrcaSlicer preset, 2.5 also selects that exact preset (for example a brand's own PLA) instead of a generic one for the material. Earlier versions match by material type only.

> **Nothing comes across on OrcaSlicer 2.4?** If your Moonraker connection in OrcaSlicer has no API key, 2.4 sends a placeholder key that Moonraker rejects, so the sync silently fails. See [Troubleshooting](../TROUBLESHOOTING.md#orcaslicer-doesnt-pick-up-your-slot-colors-or-materials).


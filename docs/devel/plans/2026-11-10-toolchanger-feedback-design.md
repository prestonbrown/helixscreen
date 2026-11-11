# Toolchanger usability fixes for 1.1 (Medusa HC and U1 feedback)

Status: design, approved answers from Preston 2026-11-10. Lands on `release/1.1`, ported to `main`.

## Why

Two toolchanger owners (a Medusa HC and a Snapmaker U1) reported the same friction: the
screens treat "the tool on the carriage" as the only tool you can act on. You cannot heat a
docked tool from the Filament panel, the pencil always edits the mounted tool, the dropdown
that would let you choose a tool physically changes tools, and you cannot put a tool back
from it. Several smaller rough edges ride along. Preston wants all of it in 1.1 final.

Success: a toolchanger owner can stand at the Filament panel, choose T3, heat it, load it,
and extrude from it, and never had to think about which tool is mounted until the machine
needed it on the carriage.

## Goals (seven reported items)

### 1. The Filament panel's Tool dropdown selects; it no longer changes tools

On every multi-head printer (parallel toolchangers such as U1, Medusa HC,
klipper-toolchanger; and plain multi-extruder or IDEX printers with no AMS backend), choosing
a tool in the Multi-Filament card's dropdown only makes it the panel's **target tool**. This
is the meaning the dropdown already has on hub-style AMS printers
(`FilamentPanel::handle_extruder_changed` returns early for non-PARALLEL backends); the
PARALLEL and no-backend branches that issue a tool change go away.

Everything on the panel aims at the target tool:

- The temperature card (current, target, label "Nozzle T3" or the existing
  `ToolState::nozzle_label()` form for a non-active tool) and its mini graph show the target
  tool's extruder.
- Material presets (PLA/PETG/...) and Cool Down set the target tool's nozzle. Bed is shared.
- The preheat that Load/Unload/Extrude/Purge run first (`run_after_preheat`) heats the
  target tool's extruder, and the "ready for filament operations" status reads the target
  tool's temperature.
- Load/Unload act on the target tool's slot (already true via `selected_op_slot()`).
- **Extrude, Retract and Purge** need the tool on the carriage. When the target tool is not
  mounted, the panel picks it up first, then runs the move. "Silent" (Preston) means the panel
  adds no confirm of its own: the pickup goes through the one shared tool-change decision
  (`offer_tool_change` / `decide_tool_change`), so a user with the "Confirm tool changes"
  safety setting on (the default) still gets that standard dialog, and a paused print still
  gets its own. No bypass flag.
- The move waits until the active tool **reads** the target (ToolState's active tool
  subject), not just until the change is accepted: `on_success` means accepted, and MedusaHC
  confirms a swap from its dock sensors later. If the change fails or times out, the move is
  not sent and the failure is reported. A `G1 E` must never reach the previous nozzle. (My
  call.)
- After the move the picked-up tool stays mounted. (My call: the user aimed at it, and
  returning the old tool doubles carriage moves.)

**Default selection:** with a tool mounted, the selection moves to it on every tool change
(anywhere). Opening the panel does not reset it, so "choose T3, go elsewhere, come back"
keeps T3. At boot, and with nothing mounted and no pick yet, T0. (My call on the
builder's "follows the carriage" ambiguity.)

### 2. Pick up / Park button beside the dropdown

One button next to the dropdown in the Multi-Filament card. It reads **Pick up** when the
target tool is not mounted and **Park** when it is. Pick up does the tool change through the
existing tool-change path (`ToolChangeHooks`); Park uses the same park the Tool Changer
sidebar's Park button uses. This replaces the "add a None (park) entry" idea: with a
select-only dropdown, parking is a button, not a dropdown entry. The closed dropdown no
longer shows "None"; it shows the target tool.

Hidden on printers whose dropdown is selection-only for a single head (hub AMS), where there
is nothing to pick up. On multi-head printers that cannot park (`supports_tool_park()` false:
plain multi-extruder and IDEX with no backend, where a tool is always active), the button
only ever reads **Pick up** (sends the ordinary tool change) and is hidden when the target
tool is already active. (My call.)

### 3. Pencils edit the tool the panel shows

- Filament panel: the nozzle pencil's keypad sets the target tool's nozzle.
- Controls panel: the nozzle row in the Temperatures card gets a tool chooser on multi-head
  printers, and its pencil edits the chosen tool. The row shows that tool's temperatures.
- Temperature overlay: remembers the last nozzle picked for the rest of the session instead
  of dropping it in `open()`. A pick of an extruder that no longer exists falls back to the
  active one (`displayed_extruder_name()` already does this).

### 4. Per-axis homing on the Motion panel

The Controls panel's Quick Actions card has a segmented All / X / Y / XY home bar. Motion
gets the same bar, as one shared component used by both panels (extract it; do not copy the
XML). On Motion it sits in the bottom row area; the jog pad's center button keeps homing all.
Axes offered: Motion offers All / X / Y / Z (Preston's "home x/y/z"); whether Controls keeps
XY or switches to the same set is the builder's call, as long as both panels use one
component with the axis set as a parameter.

### 5. Filament system sidebar: Settings moves to the header

The sidebar is shared by every filament system (`ui_xml/components/ams_sidebar.xml`,
`AmsOperationSidebar`). For **every** system, Settings becomes a gear icon in the panel
header, still gated on `ams_has_device_ops`. The action grid then holds only actions
(Load..., Unload..., Mount..., Park), so it never strands a lone button.

On **multi-head printers only** (capability gate, no vendor name), the Currently Loaded card
(`ams_loaded_card.xml`) gets a "Mounted: T2" / "No tool mounted" line **above** its existing
details. Spool colour, weight, material, clog and buffer meters stay. (Preston.)

### 6. "All" row in the Load/Unload picker

`BatchFilamentModal` gets a checkbox row above the tool rows that toggles every enabled row.
It is hidden when the per-operation cap (`max_heads_per_op`) is smaller than the number of
enabled rows, and in single-select modes (Mount, Seat). Checking it then unchecking one tool
unchecks "All".

### 7. Picker rows say only what is known

`contents_suffix()` in `src/ui/filament/ui_batch_filament_modal.cpp` claims "ready to load"
whenever a lane has a material and is not at the toolhead. New rule:

| State | Row text |
|---|---|
| At toolhead | `PLA - loaded` |
| Sensor says present | `PLA - ready to load` |
| Sensor says empty | `PLA - empty` |
| No sensor (presence unknown) | `PLA` |
| No material, sensor empty | `Empty` (unchanged) |

## Non-goals

- No change to hub/linear AMS behavior beyond what falls out of shared code; their dropdown
  already selects.
- No new print-time tool handling. The Print Status nozzle picker is out of scope.
- No change to how the backends load filament or change tools; this is UI targeting.

## Anti-goals

- A tool change the user did not ask for, other than the pickup for Extrude/Retract/Purge.
  Opening the panel, choosing in the dropdown, heating and presets must never move the
  carriage. Load and Unload send nothing from the panel beyond what they do today; a backend
  that mounts a tool inside its own load (MedusaHC's `needs_mount_to_feed()` sequence) keeps
  doing so, since backends are out of scope.
- Two notions of "selected tool" that disagree: the dropdown, temp card, pencil, presets,
  preheat and op buttons all read one target-tool value.
- Vendor branches in the panel. Topology and capability questions only
  (`.claude/rules/vendor-abstraction.md`).

## Constraints

- Ships on `release/1.1` (beta, then 1.1 final), ported to `main`. One batch, one land gate.
- Temperature sends go through `TemperatureController::set_target(extruder_name, ...)`
  (lint-enforced), the same way the temperature overlay already targets a picked extruder.
- Declarative UI rules: the button label, its visibility, and the "All" row's visibility bind
  to subjects; no new imperative visibility or `lv_label_set_text`.
- Layout must fit 800x480 and the smallest breakpoint. Measure the Multi-Filament card,
  Motion's bottom row and the Tool Changer header with `ctl geom` before placing controls.
- Translations: every new string through `lv_tr()` and the translation files.

## Decided by Preston (2026-11-10)

Select-only dropdown on every multi-head printer; pickup for Extrude/Retract/Purge with no
extra confirm, respecting the safety setting; one Pick up/Park button; selection follows the
mounted tool, else last pick; pencils target the panel's tool and the overlay remembers its
pick; shared segmented home bar on Motion; Settings as a header gear for every filament
system, with a "Mounted: Tn" line on multi-head printers above the card's details; "All" row
at the top of the picker; row status says only what is known; all of it in 1.1.

My calls (check these): the move waits for the active tool to read the target; the
picked-up tool stays mounted afterwards; opening the panel never resets the selection; no-park
multi-head printers get a Pick-up-only button.

## Left to the builder

- Where the target-tool value lives (panel member vs a small shared helper) as long as there
  is exactly one per panel and the Controls nozzle row reuses the same resolution rule.
- How the Controls nozzle-row chooser looks (chips vs compact dropdown), within the measured
  space.
- Exact copy for "Mounted: T2" / "No tool mounted" using the backend's tool noun.
- Test plan: unit tests for the row-status table, "All" row visibility versus the cap, the
  default-selection rule, and that choosing in the dropdown issues no gcode on a PARALLEL
  backend; one screenshot per changed screen at the finish.

# Preset macros you can find: the mark, the label, the hold row

Status: design, agreed with Preston on 2026-11-25 and 2026-11-26 against mockups at 800x480. Milestone
1.2. Closes prestonbrown/helixscreen#2183 when it ships, apart from the follow-ups named at the end.

## Intent

A material preset can already run the material's macro. Settings > Printing > Material Temperatures gives
each material a macro, and a tap on a Filament panel preset runs it (`cf6ef090c1`). Nothing on the screen
says so. The requester on #2183 runs 1.1 and filed the feature as missing. This pass makes the behavior
that ships findable, and removes two ways it can go wrong on a real print. It adds no capability beyond
that.

## What ships today

Read in the code and run in the mock on 2026-11-26.

- `src/printer/macro_executor.cpp#plan_material_preheat` picks one of four modes from the material's
  override. No macro: temperatures only. Macro missing from the printer's macros: temperatures, a log
  line, no notice. Macro with "Macro handles heating" on: the macro alone. Macro with it off:
  temperatures, then the macro.
- With a macro picked and the switch untouched, a tap on ABS/ASA sends only the macro. The log reads
  `Preheat via macro 'CLEAN_NOZZLE' (handles_heating=true)`. The switch starts on.
- The ABS/ASA button reads the override stored under ABS. `src/system/preset_materials.cpp` keeps the
  name "ABS" as the key, and a macro set on the ASA row never runs from that button.
- The spool preset runs its material's macro by the same path
  (`src/ui/filament/ui_panel_filament.cpp#handle_spool_preset_button`).
- Three things read in the code, not run:
  - A preset's macro send gives up after `IMoonrakerAPI::kMacroTimeoutMs`, five minutes. The comment beside
    it notes that a cold ASA soak to 105 C runs past that, which is the requester's own macro.
  - Nothing asks whether a job holds the machine before the macro is sent.
  - A second tap while the macro runs sends it again.

## Decisions

| # | Decision | Why |
|---|----------|-----|
| 1 | The mark is the `script_text` glyph, the name's color, before the material's name. | It is the glyph on everything labeled "Macros": the Home tile, the Macro Browser row, Macro Buttons, "Confirm before running macros", the Helper Macros rows. `play` also means start print and play video. The name line has the room: a 14 px glyph and a 5 px gap move the name 11 px right of center and leave the temperature line within 1 px. Before the temperatures, the line would span x662 to x777 in a button ending at x781. |
| 2 | The field reads "Macro" while "Macro handles heating" is on and "Preheat Macro" while it is off. | On, the macro is the material's whole heat-up. Off, it is one step of preheating. |
| 3 | A hold still opens the Choose Filament dialog. The dialog gets a row under its title: the glyph, the label from decision 2, the macro's name, a chevron. A tap opens the material's setting. | The hold keeps its meaning. The row shows what the button will run, where the user already looks. The list card must end above its buttons. |
| 4 | Code publishes subjects and the markup binds. No code touches a widget. | `.claude/rules/declarative-ui.md`. |
| 5 | While a job holds the machine, a preset sets its temperatures, skips the macro, and says so. | A soak or wipe macro sent into a running print can ruin it. |
| 6 | A preset's macro gets the limit the pre-start macros have, `kPreStartMacroTimeoutMs`, and no failure while the printer still reports busy. | The requester's macro runs longer than five minutes. `src/ui/print/ui_print_preparation_manager.cpp` already waits on a timeout while the printer is busy instead of failing. |
| 7 | A second tap sending the macro again is a follow-up issue, not this pass. | It is the same kind of guard as the job check, and the pass stays small without it. |

## Subjects

Code publishes these. The markup binds to them.

| Subject | Type | Published by | Bound by |
|---------|------|--------------|----------|
| `preset_material_<slot>_has_macro` | int, 0 or 1 | `src/system/preset_materials.cpp#refresh_subjects`, beside the slot's name and temperatures | the glyph on `ui_xml/components/filament_preset_button.xml`, hidden at 0 |
| `filament_spool_preset_has_macro` | int, 0 or 1 | `src/ui/filament/ui_panel_filament.cpp#update_spool_preset`, beside the spool preset's name | the glyph on `ui_xml/components/filament_spool_preset_button.xml` |
| `material_macro_heats` | int, 0 or 1 | `MaterialTempsOverlay`: on open from the override, then from the switch's `value_changed` | the switch's `checked` state (`bind_state_if_eq`), and which of the two labels shows |
| `catalog_picker_macro_shown` | int, 0 or 1 | `FilamentCatalogPickerModal::show`, as `catalog_picker_reset_shown` is: 1 when the caller set a macro callback | the row's `hidden` flag |
| `catalog_picker_macro_name` | string | the same, from the preset material's override | the row's value text |
| `catalog_picker_macro_heats` | int, 0 or 1 | the same | the row's label, by decision 2 |

"Has a macro" follows the setting, not the printer: the glyph shows whenever the material has a macro
configured, also when this printer lacks it and the preset falls back to temperatures.

## Design

### The mark

`filament_preset_button.xml` wraps its name in a centered row: the glyph, then the name. The row is
`clickable="false"`. With a clickable wrapper under the pointer, the hold stopped opening the dialog in the
mock, because the wrapper took the press; made non-clickable, a hold opens it and a tap still heats.

`filament_spool_preset_button.xml` takes the same row, bound to its own subject.

The panel calls `presets::refresh_subjects()` from `on_activate()`, so a macro set or cleared under
Settings shows on the button when the panel comes back.

### The label

In `ui_xml/material_temps_overlay.xml` the macro column's label becomes two texts, "Macro" and "Preheat
Macro", each hidden by a `bind_flag_if_eq` on `material_macro_heats` for the other value. The switch binds
its `checked` state to the same subject, the way `ui_xml/components/temp_graph_config_modal.xml` binds
`follow_switch`. The two `lv_obj_add_state` calls that set the switch today go with that, since the binding
replaces them, and `handle_save` reads the subject.

The help line under the dropdown keeps its text in both states.

### The hold row

`ui_xml/components/filament_catalog_picker.xml` gets the row as the first child of the dialog body. It is
shown by `catalog_picker_macro_shown`, so the other callers of the picker, the AMS slot editor among them,
see nothing new. Its click runs the callback the Filament panel sets before `show()`, which closes the
dialog and opens Material Temperatures on the preset's material in its edit view.

The selector fills the body's whole height today (`height="100%"`), so a row beside it would push the
product list under the footer. In the picker the selector sits in a host that grows to fill what the body
has left, and the selector fills the host. The fragment keeps its own sizing for its other hosts.

Measured at 800x480 in the mock: the list card goes from 222 px to 176 px and ends at y=375, above the
buttons at y=393. The three products of a short list still show. On a long vendor list one product fewer
shows per screen. The other breakpoints get the same measurement in the build.

### While a job holds the machine

`plan_material_preheat` takes the answer of `job_holds_machine(print_state().get_print_lifecycle())` and
returns a fifth mode, held by the job: temperatures only, and a notice that the macro was skipped. The
Filament panel's two preset taps and the Home preheat widget pass it. In "Macro handles heating" mode the
notice is the only thing that says why nothing ran yet, so the temperatures are still sent.

### The send's limit

`execute_material_preheat` sends the macro with `MachineGcode::timeout_ms` set to
`IMoonrakerAPI::kPreStartMacroTimeoutMs`. When that limit expires and the printer still reports busy
(`calibration_state().get_idle_timeout_printing_subject()` is 1), the send is treated as still running: no
failure notice. Otherwise the send reports as it does today.

## Tests

Each one goes red first, by breaking the line it pins.

1. A preset whose material has a macro shows the glyph. One without hides it. The spool preset likewise.
2. On a marked preset a tap heats and a hold opens the dialog.
3. With the glyph hidden the name sits where it does today.
4. With `material_macro_heats` at 1 the visible label reads "Macro", at 0 "Preheat Macro". Toggling the
   switch flips it through the subject.
5. Saving with the switch off stores `macro_handles_heating` false, and the planner then sets temperatures
   before the macro.
6. The hold row shows for the preset caller and not for another caller. It names the override's macro. The
   product list's bottom is above the footer's top at 800x480 and at the smallest breakpoint. Tapping it
   opens the preset's material in Material Temperatures.
7. With a job holding the machine and a macro configured, the temperatures are sent, no macro is sent, and
   one notice is published. Without a job the macro is sent.
8. The preheat send carries `kPreStartMacroTimeoutMs`. A timeout with the printer still busy publishes no
   failure.

## Follow-ups, not this pass

- A second tap while the preset's macro runs sends it again.
- A macro the printer lacks falls back to temperatures with a log line only. The glyph still shows.
- The temperature graph overlay's own preset buttons are labeled with the material names and set a plain
  temperature for the heater shown (`src/ui/controls/ui_overlay_temp_graph.cpp#on_temp_graph_preset_clicked`).
- The ABS/ASA button ignores a macro set on the ASA row.

## Verification

While building, the tags each task touches. At the finish, one review, one screenshot per changed screen
(the Filament panel with a marked preset, the edit view in both label states, the dialog with its row), and
one gate on the merged result, landed by the coordinator.

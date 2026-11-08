# Tool Number Style per Printer - Design

Status: design for review, tracked in prestonbrown/helixscreen#2014. No code yet. Open
questions for Preston are at the end; nothing below them is blocked on answers except
where a question says so.

## Why

Every surface in the app numbers a tool the same way today: a 1-based physical position
("Tool 1", nozzle badge "1"), with the 0-based G-code name ("T0") beside it only where
there is room. That is right for a Snapmaker U1 or a Creator 5, whose heads are
labeled 1-4 on the machine and in the stock UI. It is wrong for a klipper-toolchanger
owner whose config says `[tool T0]` .. `[tool T5]`, whose tools carry "T0".."T5" on the
dock plates, and who reads "Tool 1" on the nozzle chip as the wrong tool. The request
came from toolchanger users on Discord, the same group behind the "other tools" chip
(#1818).

## The signal: what a tool number means on each family

A "tool" is the slicer's `T<n>`. What the owner calls that tool on the machine depends
on whether a tool is a physical head or a lane feeding one shared head.

| Family | What a tool is | What the machine and its own UI call it | Evidence | Style |
|---|---|---|---|---|
| Snapmaker U1 | One of four heads, each with its feeder | "Toolhead 1".."Toolhead 4", "Feeder 1".."Feeder 4"; G-code `T0`-`T3` | `docs/devel/FILAMENT_BACKEND_SNAPMAKER_U1.md` (the stock firmware UI table); `include/ams_backend_snapmaker.h` sets `tool_noun = Toolhead` | Physical |
| Flashforge Creator 5 / 5 Pro | Four heads | Heads 1-4; Z-Mod speaks 1-based slots (`extruder_pos1..4`, `tools "2,,1"`) | `docs/devel/printers/FLASHFORGE_CREATOR5_PRO_SUPPORT.md`; the issue text | Physical |
| klipper-toolchanger (StealthChanger, TapChanger, MadMax) | A head per `[tool <name>]` section | The section name the owner wrote: almost always `T0`..`Tn`, sometimes "Left"/"Right" | `docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md` ("Tool order"); `src/printer/printer_discovery_parse.cpp#order_tools_by_number` | Gcode when every section is `T<n>`; a configured name otherwise |
| Bondtech INDX | A head per tool | "Tool 3 (T2)" in our own prompts | `docs/devel/FILAMENT_BACKEND_INDX.md` | Physical |
| MedusaHC | A head per dock | Generated `T0`..`T5`, no `[tool]` sections | `docs/devel/FILAMENT_BACKEND_MEDUSAHC.md` | Physical (Q4) |
| Prusa-style changers (XL) | A head per tool | Prusa's own LCD and PrusaSlicer say "Tool 1".."Tool 5" while the G-code is `T0`-`T4`. Not in the repo: Prusa firmware is not a HelixScreen target, and an XL on Klipper would arrive as klipper-toolchanger | Vendor UI, not verified in-tree | Physical |
| IDEX (RatOS V-Core IDEX, plain `dual_carriage`) | Two heads, `extruder` and `extruder1` | RatOS macros and docs say `T0`/`T1`; the carriages are left and right | `assets/config/printer_database.json` `ratrig_vcore_idex`; RatOS docs, not verified in-tree | Q3 |
| Multi-extruder, no changer (Chimera-less dual) | One head per `extruderN` | Generated `T<n>` names, nothing on the machine | `src/printer/tool_state.cpp#init_tools` (extruder branch) | Physical |
| Creality CFS (K2 family) | A lane into the one head | Bays "1A".."4D" | `include/cfs_status_parse.h#CfsMaterialDb/slot_to_tnn`; `lane_name()` on `fix/cfs-labels` | Not a tool-number question |
| Bambu-like AMS hubs (ACE, QIDI Box, AD5X IFS, OpenAMS) | A lane into the one head | Slots 1-4 (ACE, IFS ports 1-4, QIDI `slot1..slot16`) | `docs/devel/FILAMENT_BACKEND_AD5X_IFS.md` (1-based ports); `docs/devel/FILAMENT_BACKEND_QIDI_BOX.md` | Not a tool-number question |
| Happy Hare | A gate into the one head | Gates counted from 0 in HH's own commands (`MMU_SELECT GATE=0`) | `include/ams_backend_happy_hare.h` (`lane_noun = Gate`) | Not a tool-number question (Q5) |
| AFC (Box Turtle, multi-extruder AFC) | A lane; with several extruders, lanes map onto heads | Lanes named in config (`lane1`..); heads are `extruderN` | `docs/devel/FILAMENT_BACKEND_AFC.md` (lane-to-extruder table) | Physical for the heads |

Two consequences shape the design:

- **Only machines with more than one nozzle have a tool-number question.** Behind a hub,
  the nozzle badge is hidden (`ToolState::names_mounted_nozzle()` is false) and a tool is
  already named by its lane: `src/ui/filament/ui_panel_filament.cpp#populate_extruder_dropdown`
  spells it "Slot 3 (T0)", and on `fix/cfs-labels` "1A (T0)". The lane name hook on that
  branch (`AmsBackend::lane_name()`, `helix::ui::backend_lane_label()`) is the precedent
  for a backend supplying its own spelling, and it already covers CFS. Lane numbering
  (HH gates from 0) is a separate question, kept out of this design.
- **The only family that wants Gcode style today is klipper-toolchanger, and it has no
  printer-database identity.** A StealthChanger Voron detects as "Voron 2.4", the same
  entry as a stock 2.4. A database field cannot reach it; the hardware can.

## Options

**A. Printer-database field** (`"tool_number_style": "gcode"`), resolved like
`toolhead_style`. Reaches only printers with an entry. The machines that want Gcode
style have none, and the ones with an entry (U1, C5) already have a backend that knows.
Rejected as the primary source.

**B. Backend capability, derived from Klipper.** A `BackendTraits` member, default
Physical. The tool-changer backend sets Gcode when the tools come from
`[tool <name>]` sections and every name has the `T<n>` shape. Reaches every
klipper-toolchanger machine with no database work, and keeps the vendor knowledge in
the one module that already owns it (`helix::toolchanger_addon`,
`.claude/rules/vendor-abstraction.md`).

**C. User setting only.** Simple, but every toolchanger owner has to find it, and the
default stays wrong for exactly the people who asked.

**Recommendation: B, plus a per-printer setting override (Auto / 1, 2, 3 / T0, T1, T2).**
The setting is the escape hatch for an owner whose sections say `T0` but whose dock
plates say 1, and for IDEX if Q3 lands on "user's choice". It mirrors
`src/system/settings_manager.cpp#get_effective_toolhead_style`
(setting, then hardware), minus the database step until a printer needs one.

## Interface

All new declarations were syntax-checked as a snippet against the flags
`src/ui/display_numbering.cpp` builds with (the firmware's `c++17` included); a
deliberately broken use failed as expected.

### `include/display_numbering.h`

```cpp
enum class ToolNumberStyle : uint8_t {
    Physical, ///< 1-based position: "Tool 1", badge "1". The default.
    Gcode,    ///< The G-code name the machine is labeled with: "T0", badge "T0".
};

/// The persisted per-printer override. Values are the stored ints, so never reorder.
enum class ToolNumberStyleSetting : uint8_t { Auto = 0, Physical = 1, Gcode = 2 };

std::string tool_number_text(ToolNumberStyle style, std::string_view name, int index);
std::string tool_badge_label(ToolNumberStyle style, std::string_view name, int index);
std::string tool_name_label(LaneNoun noun, ToolNumberStyle style, std::string_view name, int index);
std::string tool_label_in_form(LaneNoun noun, ToolNumberStyle style, std::string_view name,
                               int index, ToolLabelForm form);
ToolNumberStyle resolve_tool_number_style(ToolNumberStyleSetting setting,
                                          ToolNumberStyle backend_hint);
ToolNumberStyle active_tool_number_style();
std::string active_tool_badge_label(std::string_view name, int index);
```

Behaviour, for the implementer:

- `tool_number_text`: Physical is `lane_number_text(index)` ("1"). Gcode is the G-code
  identity, the same string `ToolLabelForm::Code` produces today: the name when it has
  the generated shape, else `tool_label(index)`. A `[tool T5]` at slot 2 is "T5", not
  "T2". Empty for a negative index.
- `tool_badge_label`: a configured name ("Left") verbatim, else `tool_number_text`. This
  is today's `src/ui/print/print_status_chips.cpp#other_tool_badge` with the style added;
  that function goes away.
- `tool_name_label` and `tool_label_in_form` **replace** their current forms rather
  than overload them, so the compiler lists every caller and each one picks a style.
  `Full` collapses to the code when both halves are equal: Gcode style spells a
  generated tool "T0", never "T0 (T0)". A configured name keeps "Left (T0)" in both
  styles. `fed_lane_tool_label` is unchanged: a lane is not a tool number.
- `active_tool_label()` keeps its signature and passes `active_tool_number_style()`.
- `resolve_tool_number_style`: Auto returns the hint; Physical and Gcode force
  themselves. Pure, so the precedence is tested without a printer.
- `active_tool_number_style`: the per-printer setting and the active backend's
  `tool_number_style()`, Physical with no backend. Lives beside `active_tool_noun()`.

### `include/ams_backend.h`

```cpp
// BackendTraits, after tool_noun:
helix::ui::ToolNumberStyle tool_number_style = helix::ui::ToolNumberStyle::Physical;

// AmsBackend, beside tool_noun():
[[nodiscard]] helix::ui::ToolNumberStyle tool_number_style() const {
    return traits().tool_number_style;
}
```

One byte, no padding, so `BackendTraits::operator==`'s
`has_unique_object_representations_v` guarantee holds (the snippet asserts it).
`AmsBackendMock` copies the persona's traits, so mocks follow for free.

### `include/toolchanger_addon.h` and `AmsBackendToolChanger`

```cpp
namespace helix::toolchanger_addon {
/// True only for a non-empty list where every name has the generated shape ("T0".."T5").
bool tools_named_by_gcode(const std::vector<std::string>& tool_names);
/// Gcode when the printer has klipper-toolchanger (`has_tool_changer()`) and
/// tools_named_by_gcode(hw.tool_names()); Physical otherwise.
helix::ui::ToolNumberStyle tool_number_style(const PrinterDiscovery& hw);
}
```

`AmsBackendToolChanger::set_discovery()` stores
`addon::tool_number_style(discovery)` in a new `tool_number_style_` member, and
`traits()` copies whichever `kTraits*` it returns today and sets the member on the copy,
the way `include/ams_backend_afc.h#AmsBackendAfc/traits` adjusts its base. Set before
`start()`, read-only after, like `mounts_to_feed_`.

Why `has_tool_changer()` matters: discovery invents `T<n>` names from the extruder
count when there are no `[tool]` sections
(`src/printer/printer_discovery_parse.cpp#parse_objects`). A
dual-extruder machine with invented names must stay Physical; only names an owner
wrote count. The Creator 5 publishes no `[tool]` sections (`ff_toolchange`, the Z-Mod
fingerprint) and MedusaHC has no `toolchanger` object, so both stay Physical.

### The setting

A `SettingsManager` row shaped like `settings_toolhead_style`:
`{"settings_tool_number_style", "appearance/tool_number_style", Scope::PerPrinter, ..., 0..2}`,
with `get_tool_number_style_setting()` returning `ToolNumberStyleSetting`. The row sits
in `ui_xml/settings_appearance_overlay.xml` under Toolhead Style, labeled "Tool numbers",
options "Auto", "1, 2, 3", "T0, T1, T2", and is hidden unless
`ToolState::names_mounted_nozzle()`, since no other printer shows a tool number.

## One rule, every badge

**One function decides the text; one existing subject carries the change.**

- Every tool-number surface calls `tool_badge_label`, `tool_number_text` or
  `active_tool_label` under the active style. None formats a number itself.
- `ToolState`'s `tools_version` subject is already what every tool surface re-renders
  on. A language change today re-runs `refresh_display_labels()` (which bumps
  `tools_version`), `PrinterState::refresh_translated_texts()` and a sync of each
  backend (`src/ui/ui_language_refresh.cpp#init_language_refresh`). A style change
  needs exactly that body: extract it into one named function and call it from the
  language observer and from a new observer on the setting's subject.
- A backend's hint changes only at discovery, which runs `init_tools()` after the
  backend exists (`src/printer/printer_discovery.cpp#init_subsystems_from_hardware`),
  so `tools_version` bumps after the hint is in place.
- `tool_badge_text` stays the nozzle badge's subject. Its writer changes from
  `lane_number_text(tool->index)` to `active_tool_badge_label(tool->name, tool->index)`.

## Migration

Surfaces that spell a tool's number today, and what each calls after:

| Surface | Today | After |
|---|---|---|
| Nozzle and heater badges (`nozzle_icon.xml`, `heater_icon.xml`, printer image callouts, status tiles), via `tool_badge_text` | `src/ui/filament/ui_ams_tool_text.cpp#update_tool_badge`: `lane_number_text` | `active_tool_badge_label` (Q2 for configured names) |
| "Other tools" chip (#1818) | `src/ui/print/print_status_chips.cpp#other_tool_badge`, `src/ui/print/ui_print_status_tiles.cpp#other_tool_readings` (`r.number`) | `tool_badge_label` with the active style; `r.number` from `tool_number_text` under the same style |
| "Nozzle 1" label | `src/printer/tool_state.cpp#nozzle_label` | `tool_badge_label`, giving "Nozzle T0" in Gcode style |
| Extruder names | `src/printer/printer_temperature_state.cpp#extruder_display_name` | `tool_number_text(style, tool_label(index), index)`; recomputed by `refresh_translated_texts()` |
| Temp graph sensor names | `src/ui/panel_widgets/temp_graph_widget.cpp#sensor_display_name` | same |
| Print status "show temp for" options and tool label | `src/ui/panel_widgets/print_status_widget.cpp#build_nozzle_tool_options`, `src/ui/panel_widgets/print_status_widget.cpp#update_tool_label` | `tool_number_text` / `active_tool_badge_label` |
| Nozzle temps number rung | `src/ui/panel_widgets/nozzle_temps_widget.cpp#create_extruder_row` | `tool_number_text` |
| Tool switcher | `src/ui/panel_widgets/tool_switcher_widget.cpp#tool_text`, `src/ui/panel_widgets/tool_switcher_widget.cpp#name_text` | unchanged calls; the style arrives through `active_tool_label` and the new `tool_name_label` parameter |
| Filament tool dropdown | `src/ui/filament/ui_panel_filament.cpp#populate_extruder_dropdown` | unchanged: multi-head tools go through `active_tool_label`, lanes through `fed_lane_tool_label` |
| `ToolInfo::display_label` | `src/printer/tool_state.cpp#noun_tool_label` | `tool_name_label(active_tool_noun(), active_tool_number_style(), ...)` |
| Backend prompts (`ams_backend_toolchanger.cpp`, `ams_backend_snapmaker.cpp`, `indx_contract.cpp`) | `tool_label_in_form(noun, ...)` | pass the backend's own `tool_number_style()` |

Not touched, because they are not tool numbers: lane and gate labels, the toolchange
counter "3 / 12", exclude-object numbers, dryer gate labels, `filament_chip_model.cpp`
lane numbers, and every `ToolLabelForm::Code` / `tool_label()` use (the G-code name is
the same in both styles).

Persisted data: none changes. The setting defaults to Auto and is new, so an existing
config reads it as 0. Nothing stores a rendered label.

Docs in the same change: `docs/devel/TOOL_ABSTRACTION.md` § "Names and labels",
`docs/devel/FILAMENT_BACKEND_TOOLCHANGER.md` (the derivation), the nozzle_icon.xml
comment that says the badge is always the 1-based digit, and the settings user doc.
This plan file is deleted in that change.

## Tests

Tags: `[numbering]` (`tests/unit/test_display_numbering.cpp`), `[toolchanger]`,
`[print_status_chips]`, `[tool_state]`, `[ams][traits]`
(`tests/unit/test_ams_backend_traits.cpp`).

| # | Case | Input | Expected |
|---|---|---|---|
| 1 | Physical number | `tool_number_text(Physical, "T0", 0)` | "1" |
| 2 | Gcode number | `tool_number_text(Gcode, "T0", 0)` | "T0" |
| 3 | Gcode keeps a gapped section name | `tool_number_text(Gcode, "T5", 2)` | "T5" |
| 4 | Gcode with a configured name falls back to the index | `tool_number_text(Gcode, "Left", 1)` | "T1" |
| 5 | Negative index | both styles, index -1 | "" |
| 6 | Badge keeps a configured name | `tool_badge_label(Gcode, "Left", 0)`, `(Physical, "Left", 0)` | "Left" both |
| 7 | Badge for a generated name | `tool_badge_label(Physical, "T2", 2)` / `(Gcode, "T2", 2)` | "3" / "T2" |
| 8 | Full, Physical, generated | `tool_label_in_form(Tool, Physical, "T0", 0, Full)` | "Tool 1 (T0)" (today's text) |
| 9 | Full, Gcode, generated: no doubling | `tool_label_in_form(Tool, Gcode, "T0", 0, Full)` | "T0" |
| 10 | Full, Gcode, configured | `tool_label_in_form(Tool, Gcode, "Left", 0, Full)` | "Left (T0)" |
| 11 | Code is style-blind | `Code` form in both styles | "T0" both |
| 12 | U1 noun under Physical | `tool_label_in_form(Toolhead, Physical, "T2", 2, Full)` | "Toolhead 3 (T2)" |
| 13 | Setting precedence | `resolve(Auto, Gcode)`, `resolve(Physical, Gcode)`, `resolve(Gcode, Physical)` | Gcode, Physical, Gcode |
| 14 | Derivation: all `T<n>` | `tools_named_by_gcode({"T0","T1","T2"})` | true |
| 15 | Derivation: one configured name | `{"T0","Left"}` | false |
| 16 | Derivation: empty, and look-alikes | `{}`, `{"T"}`, `{"t0"}`, `{"T0a"}` | false |
| 17 | Invented names stay Physical | discovery with `extruder`, `extruder1`, no `toolchanger` object | `tool_number_style(hw)` Physical |
| 18 | klipper-toolchanger | `toolchanger` + `tool T0..T3` objects | Gcode, and the backend's `traits().tool_number_style` is Gcode |
| 19 | Creator 5 and U1 stay Physical | C5 Z-Mod and U1 mock personas | Physical |
| 20 | Every other backend's default | each `kTraits` in `test_ams_backend_traits.cpp` | Physical |
| 21 | Badge follows the style | toolchanger fixture, T1 active, Gcode | `tool_badge_text` "T1"; flip the setting to Physical: "2" without re-discovery |
| 22 | Other-tools chip agrees with the badge | same fixture, `other_tool_readings()` | badges "T0", "T2", "T3"; `r.number` empty when it equals the badge |
| 23 | Nozzle label | `ToolState::nozzle_label()` under each style | "Nozzle 2" / "Nozzle T1" |
| 24 | Hub printers unaffected | CFS and ACE fixtures under a forced Gcode setting | dropdown still "Slot 1 (T0)" / "1A (T0)"; badge hidden |

Rows 21 and 22 are the ones
that fail if a surface keeps its own formatting; break `update_tool_badge` back to
`lane_number_text` once to see 21 go red.

One screenshot per changed screen at the finish: the print status tiles with the other
tools chip, the home nozzle widget, and Settings > Appearance, on the toolchanger mock
(`--test`, 4 tools) under each style, plus `ctl geom` of the badge at 480x272 to show
"T0" fits where "1" did.

## ESP32 / firmware size

The firmware compiles `display_numbering.cpp`, `tool_state.cpp`, the backends,
`settings_manager.cpp`, `ui_settings_appearance.cpp`, the status tiles and the home
widgets (`firmware/helixscreen-esp32/components/helixapp/app_srcs.txt`);
`ui_ams_tool_text.cpp`, the badge writer, is excluded there. The change adds a one-byte
trait, two small enums, about six short functions, one settings row and three
dropdown strings (two of them digits, so untranslated). Estimate: under 1.5 KB of
flash and no new RAM beyond the setting's one int subject. Measure with the firmware
size report at the finish, not before.

## Open questions for Preston

1. **Badge text in Gcode style: "T0" or "0"?** Recommend "T0": a bare "0" beside a
   nozzle reads as a count or an error, and "T0" is what the dock plate says.
2. **Should the nozzle badge show a configured name ("Left") like the other-tools chip
   already does?** Today the badge says "1" for `[tool Left]` while the chip says
   "Left". Recommend yes, so the two badges for one head agree; it needs a width check
   at 480x272.
3. **IDEX: T0/T1 by default, or Physical?** RatOS speaks T0/T1. Recommend Physical by
   default with the setting available, until an IDEX owner says otherwise.
4. **MedusaHC: are its docks labeled 1-6 or T0-T5?** Recommend Physical until someone
   with one answers.
5. **Happy Hare gates count from 0 in HH itself. Is a matching lane-number style
   wanted?** Out of scope here; the enum is shaped so a lane style could follow.
6. **Is the setting wanted at all**, or is the derivation enough? Recommend keeping it:
   it is the only fix for an owner whose config and dock plates disagree.

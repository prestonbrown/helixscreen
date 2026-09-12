# Lane and tool numbering: 1-based display (prestonbrown/helixscreen#957)

Design for the sweep that removes 0-based numbering from user-facing text.

## Problem

One `int` does two jobs across the UI and nothing distinguishes them. A gcode tool
index and a physical lane position are both stored as bare 0-based ints, formatted
ad hoc at ~59 sites, and the two conventions collide inside single sentences:

- `print_start_checks.cpp#build_empty_lane_message` renders `Tool 0 -> Slot 1`
- `gate_insufficient_lane_weight` renders `Slot 1 has about 200g but tool 0 needs...`
- `ui_filament_mapping_card.cpp#rebuild_compact_view` stacks `T0` over `1` in one chip
- `ams_state.cpp#sync_current_loaded_from_backend` has two branches of one label
  four lines apart, one emitting `Current: Tool 0` and the other `Current: Slot 1`
- `ui_ams_tool_text.cpp#update_tool_badge` writes a bare `0` onto the nozzle icon,
  which reaches the Nozzle Temperature panel widget, the print-status temp card and
  the temp graph overlay

## The rule

**The UI is 1-based.** A physical position counts from 1, because that is what the
hardware is labelled with and what a person counts.

**A string that specifically names a gcode tool stays 0-based and keeps the `T<n>`
spelling.** That is what a user types into a console and what the slicer emitted;
renumbering it would be wrong.

**The noun follows the backend.** Lane for AFC, Gate for Happy Hare, Slot for
AMS/CFS/ACE/QIDI/IFS/U1, Tool for a tool changer. Default: Slot.

| Thing shown | Form | Based |
|---|---|---|
| Physical lane, slot, gate, unit, nozzle | `Slot 1`, `Lane 2`, `Turtle 1 · Slot 2` | 1 |
| Gcode tool | `T0` | 0 |
| A mapping between them | `T0 -> Slot 1` | both, each correct |

Two consequences, stated so they are not surprises later:

- Happy Hare numbers its gates from 0 in its own console and in Mainsail, so our
  `Gate 1` is HH's gate 0. The HH panel already draws 1,2,3,4 over the spools, so
  this aligns the rest of the UI with a choice that panel already made.
- A K2 CFS rack is silk-screened `T1A`..`T4D` and will read `Slot 3` here. Showing
  the silk-screen text was considered and rejected: `CfsMaterialDb::slot_to_tnn` is
  a wire-protocol codec with no display call sites, hard-capped at 16 lanes, and
  returns empty for the bypass lane.

## API

Everything routes through one small header. The central invariant:

> **The `+ 1` that converts a storage index to a display number exists in exactly
> one function.**

Today 17 sites each perform their own `+ 1` correctly. Each is a place the next one
gets written wrong, and no gate can tell a correct one from a missing one.

```cpp
// include/display_numbering.h        namespace helix::ui

enum class LaneNoun { Slot, Lane, Gate, Tool };

// --- Gcode tool: 0-based, always spelled T<n> ---
std::string tool_label(int gcode_tool);          // 0 -> "T0"

// --- Physical position: always 1-based ---
int         lane_number(int index);              // 0 -> 1      (the only + 1)
std::string lane_number_text(int index);         // 0 -> "1"
std::string noun_text(LaneNoun);                 // translated "Slot" / "Lane" / "Gate"

// --- Composed forms ---
std::string lane_label(LaneNoun, int index);     // (Slot, 0) -> "Slot 1";  (Tool, 0) -> "Tool 1"
std::string lane_label(LaneNoun, std::string_view unit, int index);
                                                 // (Slot, "Turtle 1", 1) -> "Turtle 1 · Slot 2"

// --- One resolver, so no caller re-derives it ---
LaneNoun    active_lane_noun();
```

`lane_number_text` and both `lane_label` overloads call `lane_number`, so the `+ 1`
has exactly one home. `LaneNoun::Tool` is an ordinary noun and gets no special
treatment: a tool changer's positions read `Tool 1`..`Tool 4`. The `T<n>` spelling
is produced only by `tool_label`, and only in the gcode domain.

The noun arrives as an enum, never as a backend pointer or a bare string, so the
header stays free of LVGL and of `AmsBackend`, stays unit-testable on its own, and
cannot be defeated by a stringly-typed comparison.

### Existing helpers fold in, they are not forked

`FilamentMapper::format_slot_label` already produces `Slot 2` / `Turtle 1 · Slot 2`,
is 1-based, unit-aware, translated and tested. It stays as the caller-facing entry
point for `AvailableSlot` and is reimplemented on top of `lane_label`, so its
behaviour is preserved and its `+ 1` disappears. `FilamentMapper::mapped_lane_display_number`
becomes a call to `lane_number`. `FilamentMapper` keeps its LVGL-free boundary.

### The noun

```cpp
// include/ams_backend.h
virtual LaneNoun lane_noun() const { return LaneNoun::Slot; }
```

Overridden by `AmsBackendAfc` (`Lane`), `AmsBackendHappyHare` (`Gate`) and
`AmsBackendToolChanger` (`Tool`). Every other backend takes the default.

`active_lane_noun()` is the single resolver every caller uses: the active AMS
backend's `lane_noun()` when there is one, else `LaneNoun::Slot`. A widget never
derives this for itself, which matters because the nozzle badge exists on machines
with no AMS backend at all.

New translation keys needed: `Lane`, `Gate`. `Slot` and `Tool` already exist.

### The tool/position split in the model

`ToolState::ToolInfo::name` keeps `"T0"` as gcode identity. A second field carries
the physical label:

```cpp
struct ToolInfo {
    int         index = 0;
    std::string name = "T0";      // gcode identity, 0-based, T-prefixed
    std::string display_label;    // physical label, 1-based, backend noun
};
```

Five widgets currently reuse `name` as a physical label and pick up `display_label`
instead: `tool_switcher_widget` (pills, compact, picker rows), `nozzle_temps_widget`
(short label), `preheat_widget`, `ui_panel_filament` (extruder dropdown), and the
tool picker buttons. Changing `name` itself was considered and rejected: it would
corrupt the displays where `T0` is genuinely correct.

`ToolTopology::tool_name_prefix` is a latent trap. `ToolState::set_ams_topology`
builds names as `fmt::format("{}{}", prefix, i)`, and a backend supplying `"Lane"`
would silently produce `Lane0`, which no `T{}` search would ever find. The prefix
is removed and `tool_label()` used instead; nothing currently sets it to anything
but `"T"`.

### The nozzle badge

`ui_ams_tool_text.cpp#update_tool_badge` is the one place the two meanings are
genuinely fused: the value is a gcode tool index, but its job is to name which
physical nozzle you are looking at. It is a physical label, so it reads
`lane_number_text(idx)`: `1`, `2`, never a bare `0` and never `T0`.

Matching the silk-screen on a tool changer was considered and rejected, on the
grounds that we cannot know what the silk-screen says. Jubilee and E3D/Duet name
heads `T0`; Prusa XL labels its tools 1 through 5. "Is a tool changer" and "the
hardware says T0" are different predicates and only the first is knowable here, so
branching on machine class would mean guessing what is printed on the user's
machine. 1-based is correct for every vendor that counts from 1, and on the ones
that do not, `T<n>` still appears in the mapping card and the console, where the
user is already thinking in slicer terms.

If a backend ever reports head names we trust, `active_lane_noun()` is the single
place to teach it.

The `ams_current_tool_text` subject keeps carrying a `T<n>` string. Its buffer
comment in `include/ams_state.h` documents that contract and stays accurate.

## Site inventory

Counts from two audits (a `T`-shaped string audit and a widget sweep for 0-based
numbers with any noun).

| Group | Sites | Action |
|---|---|---|
| A. `T<n>` built from a 0-based index | 26 | route through `tool_label` or `lane_label` per the rule |
| B. Word + 0-based number, translated | 8 | re-key (see i18n) |
| C. Backend `"Tool N out of range"`, 8 near-identical copies | 10 | one shared helper |
| D. Firmware text passed through verbatim | 4 | replace with authored copy |
| E. XML defaults and placeholders | 3 | update to match |
| F. Widget sweep, missed by the `T` audit | 6 | see below |
| G. Already correct, own `+ 1` | 17 | route through `lane_number` |

Group F in full, since it is the newest and the reason the scope grew:

| Site | Reads | Fix |
|---|---|---|
| `ui_ams_tool_text.cpp#update_tool_badge` | bare `0` on the nozzle icon | `1` |
| `ui_filament_path_canvas.cpp#fpath::format_tool_badge_label` | `E0` for the first toolhead | 1-based, physical |
| `ams_state.cpp#sync_current_loaded_from_backend` | `Current: Tool 0` | `Current: Tool 1` |
| `print_start_checks.cpp#build_empty_lane_message` (×2) | `Tool 0 -> Slot 1` | `T0 -> Slot 1` |
| `gate_insufficient_lane_weight` | `...but tool 0 needs...` | `...but T0 needs...` |

The `Current:` label deserves a note, because it is the one row where the audit's
"gcode tool" classification is the wrong guide. Both branches of that label answer
the same question, which is what is loaded right now, and the answer is a physical
position in both cases. The sibling branch already renders `Current: Slot 1`, so the
tool-changer branch becomes `Current: Tool 1`. Spelling it `Current: T0` would
reintroduce inside one label exactly the split this issue exists to remove.

Group C collapses to one helper. The string `"Tool " + std::to_string(n) + " out of
range"` is hand-written in `ams_backend_afc.cpp` (×2), `ams_backend_cfs.cpp`,
`ams_backend_happy_hare.cpp` (×2), `ams_backend_toolchanger.cpp` and
`ams_backend_mock.cpp` (×2), with an eighth near-twin in `ams_backend_ad5x_ifs.cpp`
that spells it `"Tool T<n>"`. Two hand-written copies of one rule are a latent bug;
eight is a guarantee. They fold onto one `AmsErrorHelper` call.

## Firmware passthrough (group D)

Four sites render whatever wording the firmware emits, so Snapmaker's
`Filament Sensor e0_filament: Runout Detected` reaches a modal verbatim:

- `ui_resume_dispatch.cpp#show_restart_required_modal`
- `ui_panel_print_status.cpp#recompute_paused_overlay_visibility`
- `ui_panel_ams.cpp#AmsPanel::show_loading_error_modal`
- `recovery_modal_presenter.cpp#RecoveryModalPresenter::present`

These are fixed by **replacing** firmware text with authored copy, not by rewriting
it. `prepare_for_resume` gains a user-facing message on `AmsError`, and the four
sites prefer it, falling back to the firmware string only when none is supplied.

A regex normalizer over `e(\d+)_filament` was considered and rejected: it needs a
per-backend pattern table that rots as firmware wording changes, and it cannot
produce a backend-specific noun.

This touches the resume path, which is print-critical, so it lands as its own phase
with its own tests and can be dropped without blocking the rest.

## i18n

**8 keys embed the zero-based token.** Everything else in groups A, C, E and F
concatenates outside `lv_tr()` and costs nothing.

```
Current: Tool %d
Preheat: T{} + bed set
Switched to T{}
T%d has no filament loaded - this print will run out.
T%d needs filament in slot %d, which is empty - this print will run out.
Tool {} -> Slot {}: no filament loaded.
T{} shares slot with T{}
Slot %d has about %.0fg but tool %d needs about %.0fg. Start anyway?
```

Each exists in all 9 locale YAMLs, so changing the English orphans 8 keys x 8
non-English locales = 64 entries.

Each is re-keyed to take the label as `%s` rather than embedding the number:
`T%d has no filament loaded` becomes `%s has no filament loaded`. The key then
survives any future change to how a tool or lane is spelled, so this bill is paid
once rather than every time the convention moves.

Workflow per `L064`: `make translation-sync`, then `make translations`, then stage
the YAMLs and `ui_xml/translations/*.xml`.

## Lint gate

`tests/shell/test_code_lint.bats` grows a rule forbidding, outside an allowlist:

- a `T` glued to an integer: `"T%d"`, `"T{}"`, `"T" + std::to_string(...)`
- a bare `+ 1` applied to an identifier named `*slot_index`, `*lane*`, `*unit_index`
  inside a formatting call

Allowlist: `display_numbering.cpp`, the gcode emitters in `src/printer/ams_backend_*`
and `gcode_tool_remapper.cpp`, and `filament_slot_override_store.cpp#format_lane_key`
(a persistence key, not a label).

The gate must be proven able to go red before its green is trusted: a meta-test adds
a violating line and asserts the gate fails.

## Tests

No test asserts any of these user-visible strings today, so every one below is new.

| Tag | Covers |
|---|---|
| `[numbering]` | `display_numbering.h` directly: tool 0 -> `T0`, index 0 -> `1`, noun composition, unit form, `LaneNoun::Tool` composing as an ordinary noun (`Tool 1`, never `T0`), negative and out-of-range inputs |
| `[filament_mapper]` | `format_slot_label` output unchanged after reimplementation (the existing six sections are the regression net) |
| `[ams]` | `lane_noun()` per backend, including the default |
| `[ams][slot]` | the slot tool badge renders `T<n>`, not a renumbered value |
| `[ams][numbering]` | `active_lane_noun()` resolves to the backend noun and falls back to `Slot` with no backend; the nozzle badge reads `1` on every machine class including a tool changer |
| `[print_start]` | the three mismatch dialogs and both empty-lane branches render `T0 -> Slot 1` |
| `[tool_state]` | `name` stays `T0`, `display_label` is 1-based, the five widgets read the right one |
| `[lint]` | the gate fires on a violating line |

Per `tests/CLAUDE.md`, a green suite is not evidence. `make mutate-diff` over the
changed hunks, and the commit body names the mutation that went red.

## Phases

Each lands on its own and is independently revertible.

1. `display_numbering.h` plus its tests. No call sites change. Nothing user-visible moves.
2. `FilamentMapper` and the 17 correct sites route through it. Pure refactor; `format_slot_label`'s existing tests are the net.
3. `lane_noun()` virtual plus per-backend overrides plus the `Lane`/`Gate` keys.
4. `ToolInfo::display_label`, the five widgets, and removal of `tool_name_prefix`.
5. Groups A, E and F: the display sweep. The user-visible change lands here.
6. Group B: the 8 translated keys, re-keyed to take `%s`.
7. Group C: the eight `"Tool N out of range"` copies onto one helper.
8. Group D: firmware passthrough via authored copy on `AmsError`.
9. The lint gate, last, so it is written against a tree that already passes.

## Out of scope

- K2 CFS silk-screen labels (`T1A`..`T4D`) as display text. Rejected above.
- Renaming anything in gcode, persistence keys, or `SlotRegistry::backend_name`.
- `docs/` wording. The docs say "slot" throughout; once `lane_noun()` exists they
  are inconsistent with an AFC or HH machine. Worth a follow-up issue, not this sweep.

## Loose end found on the way

`print_status_widget.cpp#build_nozzle_tool_options` and
`temp_graph_widget.cpp#sensor_display_name` label extruder 0 as bare `"Nozzle"`
while its siblings get `"Nozzle 2"`, `"Nozzle 3"`, so it reads as though Nozzle 1
is missing. The authoritative path in `printer_temperature_state.cpp` gates on
`multi` and emits `"Nozzle 1"` correctly. One line each, unrelated to this issue,
cheap to fix in phase 5.

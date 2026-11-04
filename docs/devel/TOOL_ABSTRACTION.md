# Tool Abstraction Layer

Physical tool management for multi-tool printers (tool changers, IDEX). Maps Klipper tool objects to a unified `ToolInfo` model with reactive LVGL subjects.

**Related docs:** [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) (AMS/filament backends), [MULTI_EXTRUDER_TEMPERATURE.md](MULTI_EXTRUDER_TEMPERATURE.md) (per-extruder temps)

---

## Overview

The tool abstraction layer models physical print heads (tools) independently from filament systems. A tool changer printer has multiple tools, each with its own extruder, heater, fan, and gcode offsets. Single-extruder printers get an implicit T0 tool.

```
                    ┌──────────────┐
                    │  ToolState   │  Singleton
                    │ (tool_state) │  Reactive subjects
                    └──────┬───────┘
                           │
              init_tools() │ update_from_status()
                           │
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                   ▼
   ┌─────────┐       ┌─────────┐         ┌─────────┐
   │ToolInfo │       │ToolInfo │   ...   │ToolInfo │
   │  T0     │       │  T1     │         │  TN     │
   │extruder │       │extruder1│         │extruderN│
   │fan      │       │fan1     │         │fanN    │
   └─────────┘       └─────────┘         └─────────┘
        │                  │
        │ backend_index    │ backend_index
        ▼                  ▼
   ┌─────────┐       ┌─────────┐
   │AmsState │       │AmsState │
   │backend 0│       │backend 1│
   └─────────┘       └─────────┘
```

---

## Key Types

### DetectState Enum

Physical presence detection for dockable tools:

```cpp
enum class DetectState {
    PRESENT = 0,      // Tool is on the carriage
    ABSENT = 1,       // Tool is docked / not mounted
    UNAVAILABLE = 2,  // No detection hardware (default)
};
```

### ToolInfo Struct

Represents one physical tool (print head):

```cpp
struct ToolInfo {
    int index = 0;                                    // Tool index (0, 1, 2, ...)
    std::string name = "T0";                          // Klipper tool name ("T0", "T1", or a configured "Left")
    std::string display_label;                        // Physical label, 1-based ("Tool 1", "Toolhead 1" on a U1)
    std::optional<std::string> extruder_name = "extruder"; // Linked extruder ("extruder", "extruder1")
    std::optional<std::string> heater_name;           // Explicit heater override (nullopt = use extruder)
    std::optional<std::string> fan_name;              // Part cooling fan ("fan", "fan_generic fan1")
    std::array<ToolAxisOffset, 3> gcode_offsets{};   // The tool's own X/Y/Z offsets, by axis_index()
                                                     //   each: mm, known, saved_mm, dirty()
    bool active = false;                              // Klipper "active" state
    bool mounted = false;                             // Physically mounted on carriage
    DetectState detect_state = DetectState::UNAVAILABLE; // Physical presence detection
    int backend_index = -1;                           // AMS backend feeding this tool (-1 = direct drive)
    int backend_slot = -1;                            // Fixed slot in that backend (-1 = any/dynamic)
    bool lane_seated = false;                         // This tool's lane is the one seated in its head
};
```

**Names and labels.** `name` is the G-code identity. On klipper-toolchanger it is the
`[tool <name>]` section name from the printer's Klipper config, so `[tool Left]` gives
`"Left"`; everywhere else it is generated as `"T<n>"` (`tool_label()`). HelixScreen has no
setting of its own for it: renaming a tool means renaming its Klipper section.
`display_label` is always generated, from the backend's noun (`active_tool_noun()`).
UI text goes through `include/display_numbering.h` rather than either field directly:
`tool_label_in_form()` renders a generated name as "Tool 1 (T0)" (`Full`) or "T0" (`Code`),
and a configured one as "Left (T0)" or "T0"; `tool_name_label()` gives the friendly half
alone ("Tool 1", "Left"), and `is_generated_tool_name()` tells the two kinds apart.

**`effective_heater()` helper:** Returns the heater responsible for this tool. Prefers explicit `heater_name`, falls back to `extruder_name`, then `"extruder"`.

### ToolState Singleton

Manages tool discovery, status tracking, and reactive subjects:

```cpp
class ToolState {
public:
    static ToolState& instance();

    void init_subjects(bool register_xml = true);
    void deinit_subjects();

    void init_tools(const PrinterDiscovery& hardware);
    void update_from_status(const nlohmann::json& status);

    const std::vector<ToolInfo>& tools() const;
    const ToolInfo* active_tool() const;       // nullptr if index out of range
    int active_tool_index() const;
    int tool_count() const;

    lv_subject_t* get_active_tool_subject();   // Current tool index (int)
    lv_subject_t* get_tool_count_subject();    // Number of tools (int)
    lv_subject_t* get_tools_version_subject(); // Bumped on any tool data change (int)
};
```

**Fixed toolheads:** a machine that ships a set number of toolheads and publishes no
`tool T<n>` objects declares them in one provider table,
`src/printer/fixed_tool_topology.cpp` (`include/fixed_tool_topology.h`): each head's
extruder, heater and part fan. `init_tools()` builds `ToolInfo` from
`helix::tool_topology::fixed_toolheads()` when it is non-empty and never names the machine.
Adding a machine of that kind is one row in that table.

---

## Lifecycle

```
Application startup
        │
        ▼
  init_subjects()          Register active_tool, tool_count, tools_version
        │                  subjects with LVGL XML system
        ▼
  PrinterDiscovery         Klipper objects parsed (tool T*, toolchanger,
  ::parse_objects()        extruder, extruder1, ...)
        │
        ▼
  init_tools(discovery)    Create ToolInfo vector from discovery data
        │                  Set tool_count and tools_version subjects
        ▼
  update_from_status()     Called on every Moonraker status update
        │                  Parses toolchanger + per-tool JSON keys
        │                  Bumps tools_version on any change
        ▼
  deinit_subjects()        Clears tools, resets subjects
```

---

## Discovery

### Tool Changer Printers

When `PrinterDiscovery` finds both a `toolchanger` object and `tool T*` objects in Klipper's object list, `init_tools()` creates one `ToolInfo` per discovered tool name, in Klipper's tool-number order: a tool's index is its tool number (see **Tool order** in `FILAMENT_BACKEND_TOOLCHANGER.md`).

Extruder mapping is by index: discovered extruder names (sorted alphabetically) are paired with tools in order. If there are fewer extruders than tools, extra tools get `extruder_name = nullopt`.

```
Klipper objects:                    ToolInfo result:
  toolchanger                       ┌──────────────────────┐
  tool T0             ───────────>  │ index=0, name="T0"   │
  tool T1                           │ extruder="extruder"  │
  extruder                          ├──────────────────────┤
  extruder1           ───────────>  │ index=1, name="T1"   │
                                    │ extruder="extruder1" │
                                    └──────────────────────┘
```

### Single-Tool Fallback

When no tool changer is detected, a single implicit tool is created:

```cpp
ToolInfo {
    .index = 0,
    .name = "T0",
    .extruder_name = "extruder",
    .fan_name = "fan",
    .active = true,  // Always active on single-tool printers
};
```

---

## Status Updates

`update_from_status()` is called with the Moonraker status JSON on every subscription update. It parses two types of keys:

### Toolchanger Object

```json
{
    "toolchanger": {
        "tool_number": 1
    }
}
```

Updates `active_tool_index_` and the `active_tool` subject.

### The Empty Carriage

Whether `-1` is a real active-tool value depends on the topology. When an AMS
backend owns the tool list, `build_ams_topology()`
(`src/printer/ams_state_tool_mapping.cpp#"build_ams_topology(AmsBackend* backend, int backend_index,"`) fills
`ToolTopology::allows_empty_carriage` from `AmsBackend::carriage_can_be_empty()`
(`include/ams_backend.h#AmsBackend/carriage_can_be_empty`). It defaults to false; a
backend whose heads dock answers true, so the carriage can also hold nothing, as on a tool
changer like the FlashForge Creator 5.
`ToolState::set_ams_topology()` (`src/printer/tool_state.cpp#set_ams_topology`) then
publishes `active_tool` = -1 and the UI shows no head as active. Lane-based systems
(AFC, AMS) leave the flag false and keep the fallback: an out-of-range report maps to
T0, because a lane feeder always has a tool behind it.

The Snapmaker U1 docks every head too: `AmsBackendSnapmaker` answers
`carriage_can_be_empty()` true, because `PARK_EXTRUDER` leaves the carriage empty. The U1's backend owns no tool table, so no
topology is built and `ToolState` keeps the tools it enumerated from the extruders. The
active tool cannot come from the status frame either: `toolhead.extruder` and
`toolchanger.tool_number` keep naming the last head used after it is parked. So
`AmsState::sync_tool_topology()` (`src/printer/ams_state.cpp#sync_tool_topology`)
hands the decision to the backend with `ToolState::set_backend_active_tool()`
(`src/printer/tool_state.cpp#set_backend_active_tool`), passing
`get_current_tool()`. While the backend owns it, `update_from_status()` stops deriving
the active tool from the frame, and an index naming no listed tool reads as -1. A
PARKED head is therefore not the active tool: after Park, `active_tool` reads -1 and the
home tool switcher's face reads "Choose head" ("None" where that does not fit), with its
Park cell disabled. `clear_backend_active_tool()` hands the active tool back to the
frame whenever the backend builds a topology, cannot empty its carriage, or is gone, and
maps a held -1 to T0.

### Parking the Active Tool

`AmsBackend::park_active_tool()` goes through the same gated path as a tool change
(`FilamentOp::ParkTool` in `AmsSubscriptionBackend::run_filament_op`), with a stricter
gate: a paused job refuses it too, on every backend, because a print resumed with its
tool docked prints nothing. The refusal lives in the shared gate so any caller meets it,
and the tool switcher widget (`park_offered()` and `handle_park_requested()`) mirrors it
with `job_holds_machine()` so the action greys out.

A tool change is the opposite case: it is allowed while paused (behind a confirmation,
see [Requesting a Tool Change from the UI](#requesting-a-tool-change-from-the-ui)) unless
`filament_ops_self_home()` is true. While `T<n>` is in flight the
U1 never reads an empty carriage: the old head docks before the new one reports, so the
backend holds the previous `current_tool` while a `T<n>` it sent has not landed (bounded
by `kToolChangeWindow`), a head is in its `activating_move`, or the action is `SELECTING`.

### Per-Tool Objects

Each tool's status is keyed as `"tool <name>"`:

```json
{
    "tool T0": {
        "active": true,
        "mounted": true,
        "detect_state": "present",
        "gcode_x_offset": 0.0,
        "gcode_y_offset": 0.0,
        "gcode_z_offset": 0.0,
        "extruder": "extruder",
        "fan": "fan"
    }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `active` | bool | Klipper's active state for this tool |
| `mounted` | bool | Physically mounted on carriage |
| `detect_state` | string | `"present"`, `"absent"`, or absent key = UNAVAILABLE |
| `gcode_x_offset` | float | Tool X offset (mm) — see below |
| `gcode_y_offset` | float | Tool Y offset (mm) — see below |
| `gcode_z_offset` | float | Tool Z offset (mm) — see below |
| `extruder` | string | Extruder assignment (can change at runtime) |
| `fan` | string | Fan assignment |

Any change bumps `tools_version` subject to trigger UI rebuilds.

The three offsets are **not** parsed in the generic per-tool loop. Which store is
authoritative for a tool's offset is a per-firmware question (`include/tool_offsets.h`):
klipper-toolchanger keeps all three on the `tool T{n}` object and edits them with
`SET_TOOL_PARAMETER`, while a MedusaHC-style machine keeps Z on a macro and never reads the
tool object's copy. `ToolState::init_tools()` resolves that model once from discovery
(`helix::tool_offsets::resolve_reader()`), and `ToolState::update_from_status()` hands the
result to `read_tool_offset_microns()` per tool and per axis - each read a direct key lookup
into the frame, never a scan of it - and only for the axes `supports_axis()` says the
firmware keeps. `ToolInfo::gcode_offset(Axis)` returns the
value with its `known` / `saved_mm` bookkeeping; the per-tool subjects
(`per_tool_{x,y,z}_supported`, `any_tool_offset_dirty`) follow from it, and the values
themselves are read through the `tool_offset_*()` accessors rather than subjects.

---

## Backend Integration

`ToolInfo.backend_index` and `ToolInfo.backend_slot` map tools to AMS filament system backends:

| Field | Value | Meaning |
|-------|-------|---------|
| `backend_index` | `-1` | Direct drive (no filament system) |
| `backend_index` | `0` | Primary AMS backend |
| `backend_index` | `1+` | Secondary AMS backends |
| `backend_slot` | `-1` | Dynamic / any slot |
| `backend_slot` | `0+` | Fixed slot assignment |

This allows a tool changer where T0 feeds from an AFC (backend 0) and T1 feeds from a Happy Hare (backend 1), each with independent slot tracking.

`ToolState::has_multiple_toolheads()` is true for a tool changer or more than one hotend and false for AMS slots feeding one hotend (`physical_heads().size() > 1`). Ask it instead of combining `is_multi_tool()` with `has_multiple_extruders()` and `ams_topology_active()`. Its callers: the toolhead widget's tool label (`src/ui/panel_widgets/toolhead_widget.cpp`) and the belt settling laps' central-half inset (`src/ui/calibration/ui_panel_belt_tension.cpp`, `BeltSettleInputs::tool_count`).

`ToolInfo.lane_seated` comes from `ToolTopology::tool_seated`, which `build_ams_topology()` fills from each lane's `slot_is_actively_loaded()` on every push. Where a hub feeds several lanes into one head, it names the lane the head holds, mounted or parked, and `PhysicalHead::representative_tool()` sends that lane when the head is selected rather than the head's lowest (prestonbrown/helixscreen#1811).

---

## Subject Bindings

| Subject Name | Type | Description |
|--------------|------|-------------|
| `active_tool` | int | Index of the currently active tool; -1 = empty carriage (only where the carriage can be empty, see [The Empty Carriage](#the-empty-carriage)) |
| `tool_count` | int | Total number of tools |
| `tools_version` | int | Monotonically increasing counter, bumped on any tool data change |

**XML binding example:**

```xml
<!-- Show tool badge only on multi-tool printers -->
<lv_obj>
    <bind_flag_if_eq subject="tool_count" flag="hidden" ref_value="1"/>
    <text_small bind_text="active_tool_text"/>
</lv_obj>
```

**C++ access:**

```cpp
auto& ts = helix::ToolState::instance();
const auto* tool = ts.active_tool();
if (tool) {
    spdlog::info("Active: {} (extruder: {})",
        tool->name, tool->extruder_name.value_or("none"));
}
```

---

## UI Patterns

### Active Tool Number (HomePanel)

On multi-tool printers the nozzle icon carries the active tool's number as a bold subscript digit beside the glyph (`ui_xml/components/nozzle_icon.xml#tool_badge`, bound to `tool_badge_text` — the 1-based digit from `lane_number_text()`, never a "T" prefix). Hidden on single-tool printers via `bind_flag_if_eq` on the badge subject.

### Tool-Prefixed Temperatures (PrintStatusPanel)

During a print, each tool's temperature is shown with its tool name prefix (e.g., "T0: 205/210"). The panel observes `tools_version` to rebuild its temperature display when tool configuration changes.

### Tool Selector (TempControlPanel)

The temperature control panel provides a dropdown to select which tool's temperature to control. On single-tool printers, the selector is hidden. The dropdown is rebuilt when `tools_version` changes.

### Requesting a Tool Change from the UI

Every surface that mounts another tool on a tap asks one function, so a tap refused on one
is refused on all of them with the same words: the home tool switcher
(`src/ui/panel_widgets/tool_switcher_widget.cpp#handle_tool_selected`), a toolhead on the
AMS panel's PARALLEL path (`src/ui/filament/ui_panel_ams.cpp#on_path_toolhead_clicked`) and the
filament panel's extruder dropdown (`src/ui/filament/ui_panel_filament.cpp#handle_extruder_changed`).

- **The decision** is the pure `decide_tool_change()` (`include/tool_change_request.h`) over
  `ToolChangeInputs`, which `tool_change_inputs()` reads from live state. In order: the
  mounted tool is `AlreadyActive`; spools on the bed refuse (`spools_on_bed_refusal()`); a
  print that blocks filament ops refuses with the backend's own copy
  (`tool_change_print_refusal()` over `print_blocks_filament_op()`); a backend in
  `AmsAction::PAUSED` refuses with "The filament system needs attention"; any other action
  that holds the system (`ams_action_holds_system()`) is `Busy` and ignored silently; a
  paused job is `ConfirmPaused` whatever the setting; then `ConfirmSetting` or `Immediate`
  from the "Confirm tool changes" setting.
- **The dispatch** is `offer_tool_change()`: refusals raise a warning toast, a confirm
  verdict shows a `modal_confirm`, and the change goes through
  `ToolState::request_tool_change()`. The dialog's primary button decides again from live
  inputs, because a print can start or the filament system can pause while it is open: a
  second verdict that does not dispatch reports itself as the tap would have (the refusal
  toast, or nothing for Busy and AlreadyActive) and runs `on_declined`. A print paused since
  the dialog opened turns `ConfirmSetting` into `ConfirmPaused`: the change is not sent but
  offered again once the dialog has closed, so the user answers the paused warning. `ToolChangeHooks` reports back: `on_success`,
  `on_failed` (after the shared "Tool change failed" toast, possibly off the main thread),
  and `on_declined` for every path that requested nothing, a dialog dismissal included.
  The filament panel uses `on_declined` to put its dropdown back on the mounted tool.
  `owner_token` gates the dialog's callbacks, and `resolve_api` is asked when the change is
  sent, so a dialog left open across a reconnect never holds a stale API.
- **The setting** is `SafetySettingsManager::get_tool_change_require_confirmation()`
  (`/safety/tool_change_require_confirmation`, global, default on, subject
  `settings_tool_change_confirm`). Its Settings > Printing row is hidden while
  `tool_count` is below 2.
- **Labels.** The confirmation names the tool with `active_tool_label()`
  (`include/display_numbering.h`), the same text the switcher and dropdown show
  ("Toolhead 2 (T1)" on a U1).

---

## Key Files

| File | Purpose |
|------|---------|
| `include/tool_state.h` | ToolInfo struct, DetectState enum, ToolState singleton |
| `src/printer/tool_state.cpp` | Discovery, status parsing, subject management |
| `include/printer_discovery.h` | Hardware detection: `tool_names()`, `has_tool_changer()` |
| `include/ams_state.h` | Multi-backend filament system (linked via backend_index) |
| `include/tool_change_request.h`, `src/ui/filament/tool_change_request.cpp` | The one tool-change decision and dispatch every UI surface uses |

---

## See Also

- **[FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)** -- AMS backend architecture, slot management
- **[MULTI_EXTRUDER_TEMPERATURE.md](MULTI_EXTRUDER_TEMPERATURE.md)** -- Per-extruder temperature subjects
- **[MOONRAKER_ARCHITECTURE.md](MOONRAKER_ARCHITECTURE.md)** -- PrinterDiscovery and status subscriptions

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
    std::string name = "T0";                          // Klipper tool name ("T0", "T1", ...)
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
};
```

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

When `PrinterDiscovery` finds both a `toolchanger` object and `tool T*` objects in Klipper's object list, `init_tools()` creates one `ToolInfo` per discovered tool name.

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
tool object's copy. `ToolState::update_from_status()` therefore asks
`helix::tool_offsets::read_tool_offset_microns()` per tool and per axis, and only for the
axes `supports_axis()` says the firmware keeps. `ToolInfo::gcode_offset(Axis)` returns the
value with its `known` / `saved_mm` bookkeeping; the per-axis subjects
(`per_tool_{x,y,z}_supported`, `active_tool_{x,y,z}_offset`, `active_tool_{x,y,z}_offset_valid`,
`any_tool_{x,y,z}_dirty`, `any_tool_offset_dirty`) follow from it.

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

---

## Subject Bindings

| Subject Name | Type | Description |
|--------------|------|-------------|
| `active_tool` | int | Index of the currently active tool |
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

### Active Tool Badge (HomePanel)

The home panel shows a tool badge (e.g., "T1") when multiple tools are present. Hidden on single-tool printers via `bind_flag_if_eq` on `tool_count`.

### Tool-Prefixed Temperatures (PrintStatusPanel)

During a print, each tool's temperature is shown with its tool name prefix (e.g., "T0: 205/210"). The panel observes `tools_version` to rebuild its temperature display when tool configuration changes.

### Tool Selector (TempControlPanel)

The temperature control panel provides a dropdown to select which tool's temperature to control. On single-tool printers, the selector is hidden. The dropdown is rebuilt when `tools_version` changes.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/tool_state.h` | ToolInfo struct, DetectState enum, ToolState singleton |
| `src/printer/tool_state.cpp` | Discovery, status parsing, subject management |
| `include/printer_discovery.h` | Hardware detection: `tool_names()`, `has_tool_changer()` |
| `include/ams_state.h` | Multi-backend filament system (linked via backend_index) |

---

## See Also

- **[FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)** -- AMS backend architecture, slot management
- **[MULTI_EXTRUDER_TEMPERATURE.md](MULTI_EXTRUDER_TEMPERATURE.md)** -- Per-extruder temperature subjects
- **[MOONRAKER_ARCHITECTURE.md](MOONRAKER_ARCHITECTURE.md)** -- PrinterDiscovery and status subscriptions

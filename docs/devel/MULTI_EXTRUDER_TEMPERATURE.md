# Multi-Extruder Temperature System

Dynamic per-extruder temperature tracking with reactive LVGL subjects. Supports arbitrary numbers of extruders discovered at runtime from Klipper heater objects.

**Related docs:** [TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md) (tool-to-extruder mapping), [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) (filament systems)

---

## Overview

`PrinterTemperatureState`, one of `PrinterState`'s domain sub-states, manages all temperature-related LVGL subjects. It supports:

- **Dynamic multi-extruder discovery** from Klipper heater objects
- **Per-extruder subjects** for independent temperature/target binding
- **Active-extruder subjects** (`extruder_temp`, `extruder_target`) that follow the toolhead's current extruder
- **Decidegree precision** (0.1C resolution via integer subjects)
- **Bed and chamber** temperature tracking

```
  Moonraker status notification
          │
          ▼
  helix::dispatch_status_frame()   (src/printer/status_dispatch.cpp)
          │
          ▼
  PrinterState::update_from_status()
          │ delegates to
          ▼
  PrinterTemperatureState::update_from_status()
          │
          ├──> extruders_["extruder"].temp_subject     (per-extruder, heap)
          ├──> extruders_["extruder1"].temp_subject    (per-extruder, heap)
          ├──> active_extruder_temp_  (XML "extruder_temp", follows toolhead.extruder)
          ├──> bed_temp_       (static)
          └──> chamber_temp_   (static)
                  │
                  ▼
              XML bindings / observer callbacks
```

---

## ExtruderInfo Struct

Each discovered extruder gets an `ExtruderInfo` entry in the `extruders_` map (abridged; the full struct, with the latched last non-zero target, pressure advance and reported nozzle, is in `include/printer_temperature_state.h#ExtruderInfo`):

```cpp
struct ExtruderInfo {
    std::string name;         // Klipper name: "extruder", "extruder1", etc.
    std::string display_name; // Human-readable: "Nozzle", "Nozzle 1"
    float temperature = 0.0f; // Raw float for internal tracking
    float target = 0.0f;
    std::unique_ptr<lv_subject_t> temp_subject;   // Decidegrees (value * 10)
    std::unique_ptr<lv_subject_t> target_subject; // Decidegrees
    std::unique_ptr<lv_subject_t> power_subject;  // Duty in whole percent, -1 until reported
    SubjectLifetime temp_lifetime, target_lifetime, power_lifetime;
};
```

### Why Heap-Allocated Subjects

Subjects are stored as `unique_ptr<lv_subject_t>` rather than inline members. This is required because `ExtruderInfo` lives in an `unordered_map`, and map rehash operations move entries. LVGL subjects must have stable addresses because observers hold raw pointers to them. Heap allocation via `unique_ptr` ensures pointer stability across rehash.

### Display Names

Single extruder printers get `display_name = "Nozzle"`. Multi-extruder printers get `"Nozzle"` plus the tool number in the printer's tool number style (`"Nozzle 1"`, or `"Nozzle T0"`), translated. `refresh_display_names()` re-derives them on a language switch without bumping `extruder_version`.

---

## Discovery Flow

Extruders are discovered during printer connection via `init_extruders()`:

```
MoonrakerClient connects
        │
        ▼
PrinterDiscovery::parse_objects()
  Finds: "extruder", "extruder1", "heater_bed", ...
        │
        ▼
PrinterTemperatureState::init_extruders(heaters)
  1. Filter heater list for "extruder" and "extruderN" names
     (rejects "extruder_stepper" and other prefixed names)
  2. Free the entries of extruders no longer reported (lifetimes
     expired first, then the subjects deinit)
  3. Create ExtruderInfo with heap-allocated subjects for each new extruder
  4. Bump extruder_version only if the set changed
```

The filtering logic accepts:
- `"extruder"` -- the default/first extruder
- `"extruder1"`, `"extruder2"`, etc. -- additional extruders (digit immediately after "extruder")

It rejects:
- `"extruder_stepper"` -- not a heater
- Any other `"extruder_*"` prefixed names

`init_extruders()` runs again on every Klippy ready. An extruder still present keeps its subjects and lifetimes, so observers bound to it stay live.

---

## Subject Management

### Static Subjects

These are always available and registered with the LVGL XML system at startup:

| Subject Name | Type | Description |
|--------------|------|-------------|
| `extruder_temp` | int | Active extruder current temp (decidegrees) |
| `extruder_target` | int | Active extruder target temp (decidegrees) |
| `bed_temp` | int | Bed current temp (decidegrees) |
| `bed_target` | int | Bed target temp (decidegrees) |
| `chamber_temp` | int | Chamber current temp (decidegrees) |
| `chamber_effective_target` | int | **Canonical** chamber display target (decidegrees): heater target when Heating, cooling-fan ceiling when Maintaining, 0 when Off |
| `chamber_mode` | int | Chamber control mode (`helix::ChamberMode`: Off=0 / Heating=1 / Maintaining=2) |
| `extruder_version` | int | Bumped when extruder list changes |

> **Chamber target displays bind `chamber_effective_target`, never the raw heater target** (lint-enforced). See [Chamber Heating (M141)](#chamber-heating-m141).

### Dynamic Per-Extruder Subjects

Created by `init_extruders()`. Accessed via name-based lookup (prefer the overloads that also return the `SubjectLifetime` when creating observers):

```cpp
auto& pts = get_printer_state().temperature_state();

// Get per-extruder subjects by Klipper name
lv_subject_t* temp = pts.get_extruder_temp_subject("extruder1");
lv_subject_t* target = pts.get_extruder_target_subject("extruder1");

// Returns nullptr if extruder not found
if (temp) {
    int decidegrees = lv_subject_get_int(temp);
    float degrees = helix::temperature_rules::deci_to_degrees_f(decidegrees);
}
```

### Version Subject

`extruder_version` is an integer counter that increments whenever `init_extruders()` changes the extruder set. UI panels observe this subject to rebuild their temperature displays:

```cpp
// Observer triggers UI rebuild when extruder list changes
add_observer(observe<int>(
    pts.get_extruder_version_subject(),
    this,
    [](TempPanel* self, int32_t version) {
        self->rebuild_extruder_list();
    },
    pts.get_subjects_lifetime()
));
```

---

## Active-Extruder Subjects

The XML subjects `extruder_temp`, `extruder_target`, `extruder_power` and `extruder_pressure_advance` (members `active_extruder_*_`) follow whichever extruder is active, defaulting to `"extruder"`. `PrinterState::update_from_status()` calls `set_active_extruder()` with `toolhead.extruder`, so on a tool changer they switch heads with the tool; `set_active_extruder()` republishes the new head's values at once.

```xml
<text_body bind_text="extruder_temp"/>
<lv_arc bind_value="extruder_target"/>
```

In `update_from_status()`, the active extruder's status key updates both its `extruders_` entry and the active-extruder subjects. C++ reads them through `get_active_extruder_temp_subject()` / `get_active_extruder_target_subject()`.

---

## Temperature Precision

All temperature subjects store **decidegrees** (value multiplied by 10):

| Actual Temp | Subject Value | Calculation |
|-------------|---------------|-------------|
| 205.3 C | 2053 | `205.3 * 10` |
| 60.0 C | 600 | `60.0 * 10` |
| 0.0 C | 0 | `0.0 * 10` |

This provides 0.1C resolution using integer subjects (LVGL subjects don't support float). Conversion uses `helix::units::json_to_decidegrees()` from `unit_conversions.h`.

**Display formatting:** `helix::temperature_rules::deci_to_degrees_f()` (`include/temperature_rules.h`); `helix::ui::temperature::format_temperature()` and `format_temperature_pair()` (`include/ui_temperature_utils.h`).

**Force-notify pattern:** Current-temperature subjects call `lv_subject_notify()` when `lv_subject_set_int()` left the value unchanged, and an extruder missing from a status delta has its last non-zero temperature re-notified. Chart/graph widgets therefore keep receiving samples for time-series rendering. Target subjects are set without a forced notify.

---

## Status Update Flow

`update_from_status()` processes the Moonraker status JSON:

```
Status notification parsed by MoonrakerManager (main thread)
        │
        ▼
dispatch_status_frame() -> PrinterState::update_from_status()
        │
        ▼
PrinterTemperatureState::update_from_status(status)
        │
        ├── For each extruder in extruders_ map:
        │     Check status[extruder_name] for "temperature" and "target"
        │     Convert to decidegrees, set per-extruder subjects
        │     (absent from the delta: re-notify the last temperature)
        │
        ├── status[active_extruder_name_] -> active_extruder_temp_, active_extruder_target_
        │
        ├── status["heater_bed"] -> bed_temp_, bed_target_
        │
        └── status[chamber_sensor_name_] -> chamber_temp_
```

### Chamber Sensor

The chamber temperature sensor name is set through `set_chamber_sensor_name()` by `src/printer/printer_state.cpp#set_hardware`, which resolves the saved chamber sensor assignment with `helix::chamber::resolve_sensor()` (`include/chamber_heater_assignment.h`): `auto` takes the sensor `PrinterDiscovery` picked by name, `none` disables it, and a named sensor counts only while Klipper reports that object, otherwise discovery's pick stands. Consumers read `PrinterTemperatureState::chamber_sensor_name()` and the `printer_has_chamber_sensor` capability, not discovery's own pick. The one exception is the Settings > Devices > Sensors chamber sensor dropdown (`src/ui/settings/ui_settings_sensors.cpp#populate_chamber_assignment`): its "Auto (X)" entry names discovery's pick, because that is the sensor `auto` takes. The status JSON key for chamber varies per printer (e.g., `"temperature_sensor chamber"`, `"temperature_sensor Chamber_Temp"`).

---

## Chamber Heating (M141)

On printers that define an `M141` macro (e.g., the Creality K2), the chamber setpoint is not a single heater target. The `M141` macro splits control across **two Klipper objects** to coordinate a `heater_generic` and a `temperature_fan`:

| Command | Mode | Effect |
|---------|------|--------|
| `M141 S0` | **Off** | Heater target 0; cooling fan reset to its configured resting target |
| `M141 S{≤40}` | **Maintaining** | Sets a cooling-fan *ceiling* via the `temperature_fan`; heater target stays **0** |
| `M141 S{>40}` | **Heating** | Sets the chamber `heater_generic` target |

Because the heater target reads `0` while Maintaining, the raw chamber heater target is **not** a usable display value. The codebase therefore synthesizes a single canonical display target plus a control mode.

### Canonical subjects vs. internal inputs

| Subject | XML-registered? | Role |
|---------|-----------------|------|
| `chamber_temp` | yes | Current chamber temperature (decidegrees) |
| `chamber_effective_target` | **yes** | **Canonical display target**: heater target (Heating), cooling-fan ceiling (Maintaining), or 0 (Off) |
| `chamber_mode` | **yes** | `helix::ChamberMode` int: Off=0 / Heating=1 / Maintaining=2 |
| `chamber_target` | **no (internal)** | Raw heater target (reads 0 in Maintaining); feeds the synthesis only |
| `chamber_fan_target` | **no (internal)** | Raw cooling-fan target; feeds the synthesis only |

**Binding convention (lint-enforced):** UI/XML must bind `chamber_effective_target` (+ `chamber_mode` via `temp_display`'s `bind_mode`) for any chamber target display, **never** the raw `chamber_target`. The raw subjects aren't XML-registered, so they can't be resolved from XML; `scripts/code_lint.sh` additionally fails the build on any `bind_target="chamber_target"`.

`helix::ChamberMode` is declared in `include/printer_temperature_state.h`:

```cpp
enum ChamberMode {
    Off = 0,        // Heater 0 AND (fan 0 OR fan == resting): effective target 0
    Heating = 1,    // Heater target > 0: effective target = heater target
    Maintaining = 2 // Heater 0, fan > 0 and fan != resting: effective target = fan target
};
```

### Single-source helpers

Chamber setpoint synthesis lives in **one place** (`include/temperature_rules.h`, `src/temperature/temperature_rules.cpp`, namespace `helix::temperature_rules`), and the mode word beside the display helpers (`include/ui_temperature_utils.h`, namespace `helix::ui::temperature`):

```cpp
struct ChamberSetpoint { int deci; helix::ChamberMode mode; };

// THE computation of effective target + mode from the two raw M141 targets.
// Used by PrinterTemperatureState to populate chamber_effective_target + chamber_mode.
// A fan target equal to fan_resting_deci means "not a deliberate maintain" -> Off.
ChamberSetpoint chamber_effective_setpoint(int heater_target_deci, int fan_target_deci,
                                           int fan_resting_deci = 0);

// Single mode -> word mapping: "Heating" / "Maintaining" / "Off" (untranslated key).
// TemperatureService::recompute_chamber_target() is its one caller.
const char* chamber_mode_word(helix::ChamberMode mode);
```

The `fan_resting_deci` parameter matters: `M141 S0` parks the cooling fan at its configured resting target (e.g. 35°C on the K2) rather than 0. A fan target equal to that resting value is therefore read as **Off**, not as a deliberate Maintaining set. The resting value is read from `configfile.settings[<fan>].target_temp` at discovery (`set_chamber_fan_resting()`); it defaults to 0 before config fetch, where the `fan != resting` test still distinguishes a real maintain (fan > 0) from off.

### Send routing

Chamber temperature **sends** go through `TemperatureController::set_target(HeaterType::Chamber, ...)` (`include/temperature_controller.h`). That resolves to the discovered chamber heater name and calls `MoonrakerAPI::set_temperature()`, which gates on `chamber_uses_m141()` (chamber heater + an `M141` macro defined) and emits `M141 S{deg}` via `build_heater_gcode(..., use_m141=true)` (`src/api/moonraker_api_controls.cpp`). On printers without an `M141` macro, the same path falls back to the raw heater target (`SET_HEATER_TEMPERATURE` / `SET_TEMPERATURE_FAN_TARGET`). Klipper-side safety-limit validation still bounds the target; `M141` only changes the firmware coordination, not the clamp.

---

## PrinterState Delegation

`PrinterTemperatureState` is a member of `PrinterState`, accessed via:

```cpp
auto& ps = get_printer_state();   // include/app_globals.h

// Active extruder and bed
lv_subject_t* temp = ps.temperature_state().get_active_extruder_temp_subject();
lv_subject_t* bed = ps.temperature_state().get_bed_temp_subject();

// Per-extruder access
lv_subject_t* t1 = ps.temperature_state().get_extruder_temp_subject("extruder1");

// Extruder enumeration
const auto& extruders = ps.temperature_state().extruders();
for (const auto& [name, info] : extruders) {
    spdlog::info("{}: {:.1f}C", info.display_name, info.temperature);
}
```

---

## UI Integration

Every view that shows one entry per head observes `extruder_version` and rebuilds on a bump: the controls panel (`src/ui/controls/ui_panel_controls.cpp`), the temperature graph overlay and its controller (`src/ui/controls/ui_overlay_temp_graph.cpp`, `src/ui/controls/temp_graph_controller.cpp`), the print status tiles (`src/ui/print/ui_print_status_tiles.cpp`), the filament panel and AMS sidebar, and the home widgets `nozzle_temps`, `temp_graph` and `tool_switcher` (`src/ui/panel_widgets/`).

The surfaces that show and set one nozzle (Controls, Filament, the temperature graph) share `helix::ui::TargetTool` (`include/target_tool.h`), which mirrors the target tool's extruder subjects into `target_nozzle_temp` / `target_nozzle_target` and the status pair. Its chooser is a row of heads, not extruders (`ToolState::physical_heads()`), so it is hidden where lanes feed one hotend. See [TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md#the-target-tool-filament-panel-controls-temperature-card-temperature-overlay).

The print status tiles also observe `ToolState`'s tools version and map each tool to its extruder name through `ToolState`, then bind that extruder's per-extruder subjects:

```
  ToolState::tools()           PrinterTemperatureState::extruders()
  ┌──────────────┐             ┌─────────────────────────────┐
  │T0 -> extruder│────────────>│extruder: temp_subject=2050  │
  │T1 -> extruder1│───────────>│extruder1: temp_subject=2103 │
  └──────────────┘             └─────────────────────────────┘
```

---

## Key Files

| File | Purpose |
|------|---------|
| `include/printer_temperature_state.h` | ExtruderInfo struct, PrinterTemperatureState class, `ChamberMode` enum |
| `src/printer/status_dispatch.cpp` | `dispatch_status_frame()`: fans a status frame out to `PrinterState`, `ToolState`, LEDs and sensors |
| `src/printer/printer_temperature_state.cpp` | Discovery, status parsing, subject management, chamber setpoint synthesis |
| `include/temperature_rules.h` / `src/temperature/temperature_rules.cpp` | `chamber_effective_setpoint()`, `build_heater_gcode()` / `chamber_uses_m141()`: the widget-free rules state code shares with the UI |
| `include/ui_temperature_utils.h` / `src/ui/controls/ui_temperature_utils.cpp` | `chamber_mode_word()` and the display helpers |
| `include/temperature_controller.h` / `src/temperature/temperature_controller.cpp` | `TemperatureController::set_target(HeaterType::Chamber, ...)` send path |
| `src/api/moonraker_api_controls.cpp` | `MoonrakerAPI::set_temperature()`: M141 routing gate |
| `include/unit_conversions.h` | `json_to_decidegrees()` conversion helper |
| `include/printer_discovery.h` | Heater discovery: `heaters()` list |
| `include/tool_state.h` | Tool-to-extruder name mapping |

---

## See Also

- **[TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md)** -- Tool state, tool-to-extruder mapping
- **[FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)** -- AMS backend architecture
- **[chapter 05: Printer state & singletons](architecture/05-printer-state.md)** -- Domain decomposition of PrinterState
- **[MOONRAKER_ARCHITECTURE.md](MOONRAKER_ARCHITECTURE.md)** -- Status subscriptions, PrinterDiscovery

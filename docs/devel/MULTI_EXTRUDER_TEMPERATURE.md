# Multi-Extruder Temperature System

Dynamic per-extruder temperature tracking with reactive LVGL subjects. Supports arbitrary numbers of extruders discovered at runtime from Klipper heater objects.

**Related docs:** [TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md) (tool-to-extruder mapping), [FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md) (filament systems)

---

## Overview

`PrinterTemperatureState` manages all temperature-related LVGL subjects. It was extracted from `PrinterState` as part of the god class decomposition. The class supports:

- **Dynamic multi-extruder discovery** from Klipper heater objects
- **Per-extruder subjects** for independent temperature/target binding
- **Legacy compatibility** with single-extruder XML bindings
- **Decidegree precision** (0.1C resolution via integer subjects)
- **Bed and chamber** temperature tracking

```
  Moonraker status JSON
          │
          ▼
  PrinterState::set_*_internal()
          │ delegates to
          ▼
  PrinterTemperatureState::update_from_status()
          │
          ├──> extruders_["extruder"].temp_subject     (per-extruder, heap)
          ├──> extruders_["extruder1"].temp_subject    (per-extruder, heap)
          ├──> extruder_temp_  (legacy static, mirrors "extruder")
          ├──> bed_temp_       (static)
          └──> chamber_temp_   (static)
                  │
                  ▼
              XML bindings / observer callbacks
```

---

## ExtruderInfo Struct

Each discovered extruder gets an `ExtruderInfo` entry in the `extruders_` map:

```cpp
struct ExtruderInfo {
    std::string name;         // Klipper name: "extruder", "extruder1", etc.
    std::string display_name; // Human-readable: "Nozzle", "Nozzle 1"
    float temperature = 0.0f; // Raw float for internal tracking
    float target = 0.0f;
    std::unique_ptr<lv_subject_t> temp_subject;   // Decidegrees (value * 10)
    std::unique_ptr<lv_subject_t> target_subject; // Decidegrees
};
```

### Why Heap-Allocated Subjects

Subjects are stored as `unique_ptr<lv_subject_t>` rather than inline members. This is required because `ExtruderInfo` lives in an `unordered_map`, and map rehash operations move entries. LVGL subjects must have stable addresses because observers hold raw pointers to them. Heap allocation via `unique_ptr` ensures pointer stability across rehash.

### Display Names

Single extruder printers get `display_name = "Nozzle"`. Multi-extruder printers get `"Nozzle 1"`, `"Nozzle 2"`, etc. (1-indexed for user display).

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
  2. Clean up previous ExtruderInfo entries (deinit old subjects)
  3. Create ExtruderInfo for each extruder with heap-allocated subjects
  4. Bump extruder_version subject to trigger UI rebuilds
```

The filtering logic accepts:
- `"extruder"` -- the default/first extruder
- `"extruder1"`, `"extruder2"`, etc. -- additional extruders (digit immediately after "extruder")

It rejects:
- `"extruder_stepper"` -- not a heater
- Any other `"extruder_*"` prefixed names

`init_extruders()` is safe to call multiple times. It cleans up previous subjects before creating new ones.

---

## Subject Management

### Static Subjects (Legacy)

These are always available and registered with the LVGL XML system at startup:

| Subject Name | Type | Description |
|--------------|------|-------------|
| `extruder_temp` | int | First extruder current temp (decidegrees) |
| `extruder_target` | int | First extruder target temp (decidegrees) |
| `bed_temp` | int | Bed current temp (decidegrees) |
| `bed_target` | int | Bed target temp (decidegrees) |
| `chamber_temp` | int | Chamber current temp (decidegrees) |
| `chamber_effective_target` | int | **Canonical** chamber display target (decidegrees): heater target when Heating, cooling-fan ceiling when Maintaining, 0 when Off |
| `chamber_mode` | int | Chamber control mode (`helix::ChamberMode`: Off=0 / Heating=1 / Maintaining=2) |
| `extruder_version` | int | Bumped when extruder list changes |

> **Chamber binding convention (IMPORTANT):** For any chamber *target* display, XML/UI must bind the canonical `chamber_effective_target` (plus `chamber_mode` via `temp_display`'s `bind_mode`) — **never** the raw heater target. The raw `chamber_target` (heater target) and `chamber_fan_target` (cooling-fan target) subjects exist as **internal synthesis inputs only** and are *intentionally NOT XML-registered* — they are not even resolvable from XML. A lint gate (`tests/shell/test_code_lint.bats`) forbids `bind_target="chamber_target"`. See [Chamber Heating (M141)](#chamber-heating-m141) below.

### Dynamic Per-Extruder Subjects

Created by `init_extruders()`. Accessed via name-based lookup:

```cpp
auto& pts = printer_state.temperature();

// Get per-extruder subjects by Klipper name
lv_subject_t* temp = pts.get_extruder_temp_subject("extruder1");
lv_subject_t* target = pts.get_extruder_target_subject("extruder1");

// Returns nullptr if extruder not found
if (temp) {
    int decidegrees = lv_subject_get_int(temp);
    float degrees = helix::ui::temperature::deci_to_degrees_f(decidegrees);
}
```

### Version Subject

`extruder_version` is an integer counter that increments whenever the extruder list changes (via `init_extruders()`). UI panels observe this subject to rebuild their temperature displays:

```cpp
// Observer triggers UI rebuild when extruder list changes
add_observer(observe_int_async<TempPanel>(
    pts.get_extruder_version_subject(),
    this,
    [](TempPanel* self, int32_t version) {
        self->rebuild_extruder_list();
    }
));
```

---

## Legacy Compatibility

The static `extruder_temp` and `extruder_target` subjects always mirror the first extruder (`"extruder"` key in Moonraker status). This preserves backward compatibility with existing XML bindings:

```xml
<!-- These XML bindings continue to work unchanged -->
<text_body bind_text="extruder_temp"/>
<lv_arc bind_value="extruder_target"/>
```

In `update_from_status()`, the `"extruder"` key updates both:
1. The dynamic `extruders_["extruder"]` subjects
2. The legacy static `extruder_temp_` / `extruder_target_` subjects

---

## Temperature Precision

All temperature subjects store **decidegrees** (value multiplied by 10):

| Actual Temp | Subject Value | Calculation |
|-------------|---------------|-------------|
| 205.3 C | 2053 | `205.3 * 10` |
| 60.0 C | 600 | `60.0 * 10` |
| 0.0 C | 0 | `0.0 * 10` |

This provides 0.1C resolution using integer subjects (LVGL subjects don't support float). Conversion uses `helix::units::json_to_decidegrees()` from `unit_conversions.h`.

**Display formatting:** UI code divides by 10 for display: `"{}.{}C", value/10, value%10`.

**Force-notify pattern:** Temperature subjects call `lv_subject_notify()` after `lv_subject_set_int()` even when the value hasn't changed. This ensures chart/graph widgets receive updates for time-series rendering.

---

## Status Update Flow

`update_from_status()` processes the Moonraker status JSON:

```
JSON status arrives (background thread)
        │
        ▼
PrinterState::set_*_internal()
  Posts to LVGL thread via ui_queue_update()
        │
        ▼
PrinterTemperatureState::update_from_status(status)
        │
        ├── For each extruder in extruders_ map:
        │     Check status[extruder_name] for "temperature" and "target"
        │     Convert to decidegrees, set per-extruder subjects
        │
        ├── Legacy: status["extruder"] -> extruder_temp_, extruder_target_
        │
        ├── status["heater_bed"] -> bed_temp_, bed_target_
        │
        └── status[chamber_sensor_name_] -> chamber_temp_
```

### Chamber Sensor

The chamber temperature sensor name is configurable via `set_chamber_sensor_name()`. It's set during discovery when `PrinterDiscovery` detects a `temperature_sensor` with "chamber" in the name. The status JSON key for chamber varies per printer (e.g., `"temperature_sensor chamber"`, `"temperature_sensor Chamber_Temp"`).

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
| `chamber_mode` | **yes** | `helix::ChamberMode` int — Off=0 / Heating=1 / Maintaining=2 |
| `chamber_target` | **no — internal** | Raw heater target (reads 0 in Maintaining); feeds the synthesis only |
| `chamber_fan_target` | **no — internal** | Raw cooling-fan target; feeds the synthesis only |

**Binding convention (lint-enforced):** UI/XML must bind `chamber_effective_target` (+ `chamber_mode` via `temp_display`'s `bind_mode`) for any chamber target display — **never** the raw `chamber_target`. The raw subjects aren't XML-registered, so they can't be resolved from XML; `tests/shell/test_code_lint.bats` additionally fails the build on any `bind_target="chamber_target"`.

`helix::ChamberMode` is declared in `include/printer_temperature_state.h`:

```cpp
enum ChamberMode {
    Off = 0,        // Heater 0 AND (fan 0 OR fan == resting): effective target 0
    Heating = 1,    // Heater target > 0: effective target = heater target
    Maintaining = 2 // Heater 0, fan > 0 and fan != resting: effective target = fan target
};
```

### Single-source helpers

All chamber synthesis and display text live in **one place** (`src/ui/ui_temperature_utils.{h,cpp}`, namespace `helix::ui::temperature`) so the controls panel and the temp-graph overlay can never diverge:

```cpp
struct ChamberSetpoint { int deci; helix::ChamberMode mode; };

// THE computation of effective target + mode from the two raw M141 targets.
// Used by PrinterTemperatureState to populate chamber_effective_target + chamber_mode.
// A fan target equal to fan_resting_deci means "not a deliberate maintain" -> Off.
ChamberSetpoint chamber_effective_setpoint(int heater_target_deci, int fan_target_deci,
                                           int fan_resting_deci = 0);

// Single mode -> word mapping: "Heating" / "Maintaining" / "Off" (untranslated key).
const char* chamber_mode_word(helix::ChamberMode mode);

// THE status line, shared by the controls panel and the temp-graph overlay:
// "Maintaining", "Heating", "Maintaining · Cooling", etc.
std::string chamber_status_text(int current_deci, int target_deci, helix::ChamberMode mode);
```

The `fan_resting_deci` parameter matters: `M141 S0` parks the cooling fan at its configured resting target (e.g. 35°C on the K2) rather than 0. A fan target equal to that resting value is therefore read as **Off**, not as a deliberate Maintaining set. The resting value is read from `configfile.settings[<fan>].target_temp` at discovery (`set_chamber_fan_resting()`); it defaults to 0 before config fetch, where the `fan != resting` test still distinguishes a real maintain (fan > 0) from off.

### Send routing

Chamber temperature **sends** go through `TemperatureController::set_target(HeaterType::Chamber, ...)` (`include/temperature_controller.h`). That resolves to the discovered chamber heater name and calls `MoonrakerAPI::set_temperature()`, which gates on `chamber_uses_m141()` (chamber heater + an `M141` macro defined) and emits `M141 S{deg}` via `build_heater_gcode(..., use_m141=true)` (`src/api/moonraker_api_controls.cpp`). On printers without an `M141` macro, the same path falls back to the raw heater target (`SET_HEATER_TEMPERATURE` / `SET_TEMPERATURE_FAN_TARGET`). Klipper-side safety-limit validation still bounds the target; `M141` only changes the firmware coordination, not the clamp.

---

## PrinterState Delegation

`PrinterTemperatureState` is a member of `PrinterState`, accessed via:

```cpp
auto& ps = PrinterState::instance();

// Legacy accessors (delegate to PrinterTemperatureState)
lv_subject_t* temp = ps.get_extruder_temp_subject();      // Static, first extruder
lv_subject_t* bed = ps.get_bed_temp_subject();

// Per-extruder access
lv_subject_t* t1 = ps.temperature().get_extruder_temp_subject("extruder1");

// Extruder enumeration
const auto& extruders = ps.temperature().extruders();
for (const auto& [name, info] : extruders) {
    spdlog::info("{}: {:.1f}C", info.display_name, info.temperature);
}
```

---

## UI Integration

### HomePanel Temperature Display

Shows the first extruder and bed temperatures using legacy static subjects. On multi-extruder printers, observes `extruder_version` to optionally show a multi-tool temperature summary.

### TempControlPanel Extruder Selector

Provides a dropdown populated from the `extruders()` map. Each entry shows the `display_name`. Selecting an extruder binds the temperature arc and target controls to that extruder's per-extruder subjects. Hidden on single-extruder printers.

### PrintStatusPanel Per-Tool Temps

During a print, shows each tool's temperature with its tool name prefix. Uses `ToolState` to map tools to extruder names, then looks up per-extruder subjects for binding.

```
  ToolState::tools()           PrinterTemperatureState::extruders()
  ┌──────────────┐             ┌─────────────────────────────┐
  │T0 -> extruder│────────────>│extruder: temp_subject=2050  │
  │T1 -> extruder1│───────────>│extruder1: temp_subject=2103 │
  └──────────────┘             └─────────────────────────────┘
         │                                    │
         ▼                                    ▼
  Display: "T0: 205.0/210   T1: 210.3/215"
```

---

## Key Files

| File | Purpose |
|------|---------|
| `include/printer_temperature_state.h` | ExtruderInfo struct, PrinterTemperatureState class, `ChamberMode` enum |
| `src/printer/printer_temperature_state.cpp` | Discovery, status parsing, subject management, chamber setpoint synthesis |
| `include/ui_temperature_utils.h` / `src/ui/ui_temperature_utils.cpp` | `chamber_effective_setpoint()`, `chamber_mode_word()`, `chamber_status_text()` single-source helpers; `build_heater_gcode()` / `chamber_uses_m141()` |
| `include/temperature_controller.h` / `src/ui/temperature_controller.cpp` | `TemperatureController::set_target(HeaterType::Chamber, ...)` send path |
| `src/api/moonraker_api_controls.cpp` | `MoonrakerAPI::set_temperature()` — M141 routing gate |
| `include/unit_conversions.h` | `json_to_decidegrees()` conversion helper |
| `include/printer_discovery.h` | Heater discovery: `heaters()` list |
| `include/tool_state.h` | Tool-to-extruder name mapping |

---

## See Also

- **[TOOL_ABSTRACTION.md](TOOL_ABSTRACTION.md)** -- Tool state, tool-to-extruder mapping
- **[FILAMENT_MANAGEMENT.md](FILAMENT_MANAGEMENT.md)** -- AMS backend architecture
- **[chapter 05 — Printer state & singletons](architecture/05-printer-state.md)** -- Domain decomposition of PrinterState
- **[MOONRAKER_ARCHITECTURE.md](MOONRAKER_ARCHITECTURE.md)** -- Status subscriptions, PrinterDiscovery

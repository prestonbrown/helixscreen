# Standard Macros System - Technical Specification

> **Status: Shipped** — This was the original pre-implementation design spec.
> The feature is now built and in production: the `StandardMacros` class lives at
> `src/printer/standard_macros.cpp` (header `include/standard_macros.h`), wired
> into discovery and the panels described below. The document is retained as
> as-built reference — the macro/alias tables, slot states, resolution order, and
> config schema still describe live behavior. The "Implementation Stages",
> "Files to Create", and "Files to Modify" sections near the end are historical
> planning artifacts kept for provenance; treat the source tree as the source of
> truth for current file locations.

## Overview

The Standard Macros system provides a unified registry that maps semantic operations (Load Filament, Pause, Clean Nozzle, etc.) to printer-specific G-code macros. It supports auto-detection from Moonraker, fallback to HELIX_* helper macros, and user configuration via the Settings UI.

## Goals

1. **Consistent macro handling** - All panels use the same macro resolution logic
2. **Auto-detection** - Automatically find common macros by naming patterns
3. **Fallback support** - Use HELIX_* macros when printer doesn't have its own
4. **Graceful degradation** - Empty slots disable functionality cleanly
5. **User configurability** - Override any slot via Settings overlay

---

## Standard Macro Slots

| Slot | Purpose | Auto-Detect Patterns | HELIX Fallback |
|------|---------|---------------------|----------------|
| `load_filament` | Load filament | LOAD_FILAMENT, M701 | — |
| `unload_filament` | Unload filament | UNLOAD_FILAMENT, M702, **HELIX_UNLOAD_FILAMENT**, QUIT_MATERIAL | HELIX_UNLOAD_FILAMENT |

> **Unload ordering note:** `QUIT_MATERIAL` (Creality K1 family) is matched
> LAST and deliberately below `HELIX_UNLOAD_FILAMENT`: the stock macro purges
> ~100mm forward and retracts only ~62mm — it clears the melt zone for
> manually-cut filament rather than unloading. A printer's own
> `UNLOAD_FILAMENT`/`UNLOAD_MATERIAL`/`M702` always outrank the override.
| `purge` | Purge/prime | PURGE, PURGE_LINE, PRIME_LINE, PURGE_FILAMENT, LINE_PURGE | — |
| `pause` | Pause print | PAUSE, M601 | — |
| `resume` | Resume print | RESUME, M602 | — |
| `cancel` | Cancel print | CANCEL_PRINT | — |
| `bed_mesh` | Bed mesh calibration | BED_MESH_CALIBRATE, G29 | HELIX_BED_MESH_IF_NEEDED |
| `bed_level` | Physical bed leveling | QUAD_GANTRY_LEVEL, QGL, Z_TILT_ADJUST | — |
| `clean_nozzle` | Nozzle cleaning | CLEAN_NOZZLE, NOZZLE_WIPE, WIPE_NOZZLE, CLEAR_NOZZLE | HELIX_CLEAN_NOZZLE |
| `heat_soak` | Chamber/bed soak | HEAT_SOAK, CHAMBER_SOAK, SOAK | — |

### Slot States

Each slot can be in one of four states:
- **Configured**: User explicitly selected a macro via Settings
- **Auto-detected**: System found a matching macro on the printer
- **Fallback**: Using HELIX_* macro (installed by HelixScreen)
- **Empty**: No macro available; functionality is disabled

A **Configured** entry is verified against the printer's macro list at
`init()` (`StandardMacros::validate_configured()`). A name the printer does not
define — a preset that seeded a template machine's macros, or a Klipper config
change that retired one — is **demoted**: the name moves out of
`configured_macro` into `missing_macro`, and the slot answers as though it were
unconfigured. Dispatch then falls through to the detected macro, the HELIX
fallback, or the caller's own fallback path, instead of sending a command
Klipper will reject. The name is kept rather than dropped — `save_to_config()`
round-trips what the user asked for (the printer may just be mid-restart), and
`requested_macro()` still reports it so Settings can show "you configured this
and it is broken" as distinct from "nothing is assigned here".

### Adaptive bed mesh: `ADAPTIVE` parameter forwarding

`HELIX_START_PRINT` (in `assets/config/helix_macros.cfg`) accepts an optional
`ADAPTIVE` parameter and forwards it into its `BED_MESH_CALIBRATE` call:

```gcode
{% set adaptive = params.ADAPTIVE|default(0)|int %}
...
{% if perform_bed_mesh == 1 %}
    {% if adaptive == 1 %}
        BED_MESH_CALIBRATE ADAPTIVE=1
    {% else %}
        BED_MESH_CALIBRATE
    {% endif %}
{% endif %}
```

This is the forwarding contract behind the adaptive bed mesh behavior. It is a
property of the **single** Bed Mesh pre-print toggle — there is no separate
sub-row. When the printer's `pre_print_options.bed_mesh` entry declares an
`adaptive_param`, the firmware exposes `[exclude_object]`, and there is no custom
`calibration.bed_mesh_gcode` template, `PrinterState::apply_dynamic_options()`
sets `PrePrintOption::adaptive_active` on the bed_mesh option. That single flag:

- **relabels** the toggle from "Auto Bed Mesh" to **"Adaptive Bed Mesh"**
  (`PrePrintOptionsRenderer::label_for`), and
- when the toggle is **ENABLED**, makes the print-start emit BOTH the enable
  param and the adaptive token, e.g. `SKIP_LEVELING=0 ADAPTIVE=1`.

The param name is per-printer (`ADAPTIVE`, `ADAPTIVE_MESH`, …; see
`pre_print_option.h` → `PrePrintStrategyMacroParam::adaptive_param`); the
HelixScreen standard macro uses `ADAPTIVE`. Printers whose `START_PRINT` does NOT
forward such a param simply omit `adaptive_param`, so the toggle stays "Auto Bed
Mesh" with unchanged behavior — adaptive can never be a silent no-op.

---

## Architecture

### Core Class: `StandardMacros`

```cpp
// include/standard_macros.h

enum class StandardMacroSlot {
    LoadFilament, UnloadFilament, Purge,
    Pause, Resume, Cancel,
    BedMesh, BedLevel, CleanNozzle, HeatSoak
};

struct StandardMacroInfo {
    std::string slot_name;        // "load_filament"
    std::string display_name;     // "Load Filament"
    std::string configured_macro; // User override (or empty)
    std::string detected_macro;   // Auto-detected (or empty)
    std::string fallback_macro;   // HELIX_* fallback (or empty)

    bool is_empty() const;        // No macro available
    std::string get_macro() const; // Returns first non-empty: configured > detected > fallback
};

class StandardMacros {
public:
    static StandardMacros& instance();

    // Initialize with printer hardware discovery (call after discovery)
    // NOTE: PrinterCapabilities was deleted 2026-01-11, use PrinterDiscovery instead
    void init(const PrinterDiscovery& discovery);

    // Get info for a slot
    const StandardMacroInfo& get(StandardMacroSlot slot) const;

    // Get all slots (for UI listing)
    const std::vector<StandardMacroInfo>& all() const;

    // Configure a slot (user override)
    void set_macro(StandardMacroSlot slot, const std::string& macro);

    // Execute macro for slot (with empty check)
    bool execute(StandardMacroSlot slot, MoonrakerAPI* api,
                 std::function<void()> on_success,
                 std::function<void(const MoonrakerError&)> on_error);

    // Load/save from config
    void load_from_config();
    void save_to_config();

private:
    std::vector<StandardMacroInfo> slots_;
    void auto_detect(const PrinterDiscovery& discovery);
};
```

### Resolution Order

When executing a macro, the system checks in order:
1. **User configured** - Explicit selection in Settings (verified to exist on the printer; a name that fails verification is demoted to `missing_macro` and skipped here)
2. **Auto-detected** - Found on printer via pattern matching
3. **HELIX fallback** - HelixScreen's helper macro (if available)
4. **Empty** - No macro; `execute()` returns false, caller should disable UI

---

## Configuration

### Config Schema

```json
{
  "standard_macros": {
    "quick_button_1": "clean_nozzle",
    "quick_button_2": "bed_level",
    "load_filament": "",
    "unload_filament": "",
    "purge": "",
    "pause": "",
    "resume": "",
    "cancel": "",
    "bed_mesh": "",
    "bed_level": "",
    "clean_nozzle": "",
    "heat_soak": ""
  }
}
```

- **Empty string** (`""`) = Use auto-detection (or disabled if nothing found)
- **Macro name** = Explicit override

### Quick Buttons

The Controls Panel has 2 quick-action macro buttons. These are configured as:
- `quick_button_1`: References a slot name (e.g., `"clean_nozzle"`)
- `quick_button_2`: References a slot name (e.g., `"bed_level"`)

---

## UI Integration

### Settings Overlay

Path: Settings → "Macro Buttons" → Opens overlay panel

Layout:
```
┌─────────────────────────────────────┐
│ ← Macro Buttons                     │
├─────────────────────────────────────┤
│                                     │
│ Quick Button 1     [Dropdown ▼]     │
│ Select macro for first button       │
│                                     │
│ Quick Button 2     [Dropdown ▼]     │
│ Select macro for second button      │
│                                     │
│ ─────────────────────────────────── │
│                                     │
│ Standard Macros                     │
│ Configure macros for operations     │
│                                     │
│ Load Filament      [Dropdown ▼]     │
│ Unload Filament    [Dropdown ▼]     │
│ Purge              [Dropdown ▼]     │
│ ... etc ...                         │
│                                     │
│ ℹ️ Empty slots disable functionality │
└─────────────────────────────────────┘
```

### Dropdown Options

Each standard macro dropdown shows:
1. `(Auto: DETECTED_NAME)` - if auto-detected, shows what was found
2. `(Empty)` - explicitly disable functionality
3. All discovered printer macros (alphabetically sorted)

---

## Panel Integration

### FilamentPanel
```cpp
void FilamentPanel::execute_load() {
    if (!StandardMacros::instance().execute(
            StandardMacroSlot::LoadFilament, api_,
            []() { NOTIFY_SUCCESS("Loading filament..."); },
            [](auto& err) { NOTIFY_ERROR("Load failed: {}", err.user_message()); })) {
        NOTIFY_WARNING("Load filament macro not configured");
    }
}
```

### ControlsPanel
- Quick buttons execute the configured slot
- If slot is empty, button is hidden
- `refresh_macro_buttons()` updates labels after config change

### PrintStatusPanel
- Pause/Resume/Cancel use StandardMacros
- Buttons disabled (greyed) if slot is empty

---

## Implementation Stages

| Stage | Scope | Files | Deliverable |
|-------|-------|-------|-------------|
| 1 | Core class | `standard_macros.h/cpp` | Compiles, unit tests pass |
| 2 | Discovery integration | `moonraker_manager.cpp` | Auto-detection logs on connect |
| 3 | Overlay UI | `macro_buttons_overlay.xml`, `xml_registration.cpp` | UI renders, dropdowns populate |
| 4 | Settings handler | `ui_panel_settings.cpp/h` | Overlay opens, saves to config |
| 5 | Controls integration | `ui_panel_controls.cpp/h` | Quick buttons use StandardMacros |
| 6 | Filament integration | `ui_panel_filament.cpp` | Load/Unload use StandardMacros |
| 7 | Print status integration | `ui_panel_print_status.cpp` | Pause/Resume/Cancel use StandardMacros |
| 8 | Testing & polish | — | Feature complete, all tests pass |

---

## Related Files

### Existing Infrastructure
- `include/printer_discovery.h` - Macro discovery (`has_macro()`, `macros()`)
  - NOTE: `PrinterCapabilities` was deleted 2026-01-11, replaced by `PrinterDiscovery`
  - Access via `MoonrakerAPI::hardware_discovery()`
- `src/printer/macro_manager.cpp` - HELIX_* macro definitions
- `include/config.h` - `MacroConfig` struct, `get_macro()` method
- `ui_xml/settings_display_sound_overlay.xml` - Reference overlay pattern

### Files Created (historical planning list; paths updated to as-built locations)
- `include/standard_macros.h`
- `src/printer/standard_macros.cpp`
- `ui_xml/macro_buttons_overlay.xml`

### Files Modified (historical planning list; paths updated to as-built locations)
- `src/ui/ui_panel_settings.cpp/h` - Add overlay handler
- `ui_xml/settings_panel.xml` - Add action row
- `src/xml_registration.cpp` - Register new component
- `src/application/moonraker_manager.cpp` - Init after discovery
- `src/ui/ui_panel_controls.cpp/h` - Use StandardMacros
- `src/ui/ui_panel_filament.cpp` - Use StandardMacros
- `src/ui/ui_panel_print_status.cpp` - Use StandardMacros

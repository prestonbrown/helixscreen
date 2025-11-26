# Panel Migration Status

This file tracks the migration of function-based panels to the class-based `PanelBase` architecture.

## Reference Implementation

- **TempControlPanel** (`ui_temp_control_panel.h/cpp`) - Complete class-based implementation showing:
  - Constructor dependency injection
  - RAII observer management
  - Static trampolines for LVGL callbacks
  - Two-phase initialization (init_subjects → XML creation → setup)
  - Move semantics for `std::unique_ptr` ownership

## Migration Status

### ✅ Completed (Class-Based)

| Panel | Class Name | Lines | Notes |
|-------|------------|-------|-------|
| `ui_temp_control_panel` | `TempControlPanel` | 790 | Reference implementation |
| `ui_panel_test` | `TestPanel` | 151 | First migrated panel - no subjects, minimal |
| `ui_panel_glyphs` | `GlyphsPanel` | 250 | Display only - no subjects, Phase 2 |
| `ui_panel_step_test` | `StepTestPanel` | 234 | Basic callbacks, static trampolines, Phase 2 |
| `ui_panel_settings` | `SettingsPanel` | 257 | Launcher pattern, lazy overlay creation, Phase 3 |
| `ui_panel_notification_history` | `NotificationHistoryPanel` | 335 | Service DI pattern, overlay panel, Phase 3 |
| `ui_panel_controls` | `ControlsPanel` | 337 | Launcher pattern, manages child panels, Phase 3 |
| `ui_panel_motion` | `MotionPanel` | 474 | 3 subjects, jog_pad widget, Phase 3 |
| `ui_panel_bed_mesh` | `BedMeshPanel` | 356 | TinyGL 3D renderer, non-reactive visual state, Phase 4 |
| `ui_panel_filament` | `FilamentPanel` | 525 | 6 subjects, hybrid reactive/imperative, Phase 4 |
| `ui_panel_home` | `HomePanel` | 596 | 6 subjects, 4 observers, timer lifecycle, Phase 4 |
| `ui_panel_controls_extrusion` | `ExtrusionPanel` | 423 | Cross-panel observer, temperature safety, Phase 4 |
| `ui_panel_print_status` | `PrintStatusPanel` | 555 | 10 subjects, mock simulation, resize callback, Phase 5 |
| `ui_panel_gcode_test` | `GcodeTestPanel` | 666 | File picker overlay, async loading, no subjects, Phase 5 |
| `ui_panel_print_select` | `PrintSelectPanel` | 1087 | 5 subjects, card/list views, sorting, detail overlay, Phase 5 |

### 🔄 In Progress

*None currently - Phase 5 complete!*

### 📋 Phase 2: Simple Panels (Next)

*Phase 2 complete - all simple panels migrated!*

### 📋 Phase 3: Launcher & Subject Patterns

*Phase 3 complete - all launcher/subject panels migrated!*

### 📋 Phase 4: Observer & Complex State

*Phase 4 complete - all observer/complex state panels migrated!*

### 📋 Phase 5: High Complexity

*Phase 5 complete - all high complexity panels migrated!*

| Panel | Target Class | Lines | Key Pattern |
|-------|--------------|-------|-------------|
| ~~`ui_panel_print_status`~~ | ~~`PrintStatusPanel`~~ | ~~468~~ | ✅ Completed |
| ~~`ui_panel_gcode_test`~~ | ~~`GcodeTestPanel`~~ | ~~533~~ | ✅ Completed |
| ~~`ui_panel_print_select`~~ | ~~`PrintSelectPanel`~~ | ~~1167~~ | ✅ Completed - 5 subjects, file browser |

### 📋 Phase 6: Wizard Steps

*Phase 6 complete - all wizard steps migrated!*

| Module | Target Class | Lines | Key Pattern |
|--------|--------------|-------|-------------|
| `ui_wizard_summary` | `WizardSummaryStep` | 345 | 12 subjects, config summary display |
| `ui_wizard_heater_select` | `WizardHeaterSelectStep` | 256 | 2 subjects, hardware dropdown |
| `ui_wizard_fan_select` | `WizardFanSelectStep` | 278 | 2 subjects, hardware dropdown |
| `ui_wizard_led_select` | `WizardLedSelectStep` | 206 | 1 subject, hardware dropdown |
| `ui_wizard_connection` | `WizardConnectionStep` | 558 | 5 subjects, async WebSocket, extern subject |
| `ui_wizard_printer_identify` | `WizardPrinterIdentifyStep` | 500 | 3 subjects, printer auto-detection |
| `ui_wizard_wifi` | `WizardWifiStep` | 912 | 7 subjects, WiFiManager, password modal |

## Deprecated Wrappers Ready for Clean Break

| Panel | Wrapper Function | File |
|-------|-----------------|------|
| `TestPanel` | `ui_panel_test_setup()` | `src/ui_panel_test.cpp` |
| `GlyphsPanel` | `ui_panel_glyphs_create()` | `src/ui_panel_glyphs.cpp` |
| `StepTestPanel` | `ui_panel_step_test_setup()` | `src/ui_panel_step_test.cpp` |
| `SettingsPanel` | `ui_panel_settings_init_subjects()`, `ui_panel_settings_wire_events()` | `src/ui_panel_settings.cpp` |
| `NotificationHistoryPanel` | `ui_panel_notification_history_create()`, `ui_panel_notification_history_refresh()` | `src/ui_panel_notification_history.cpp` |
| `ControlsPanel` | `ui_panel_controls_init_subjects()`, `ui_panel_controls_wire_events()` | `src/ui_panel_controls.cpp` |
| `MotionPanel` | `ui_panel_motion_init_subjects()`, `ui_panel_motion_setup()`, etc. | `src/ui_panel_motion.cpp` |
| `BedMeshPanel` | `ui_panel_bed_mesh_init_subjects()`, `ui_panel_bed_mesh_create()`, etc. | `src/ui_panel_bed_mesh.cpp` |
| `FilamentPanel` | `ui_panel_filament_init_subjects()`, `ui_panel_filament_create()`, `ui_panel_filament_setup()`, etc. | `src/ui_panel_filament.cpp` |
| `HomePanel` | `ui_panel_home_init_subjects()`, `ui_panel_home_setup_observers()`, `ui_panel_home_create()`, etc. | `src/ui_panel_home.cpp` |
| `ExtrusionPanel` | `ui_panel_controls_extrusion_init_subjects()`, `ui_panel_controls_extrusion_setup()`, etc. | `src/ui_panel_controls_extrusion.cpp` |
| `PrintStatusPanel` | `ui_panel_print_status_init_subjects()`, `ui_panel_print_status_setup()`, etc. | `src/ui_panel_print_status.cpp` |
| `GcodeTestPanel` | `ui_panel_gcode_test_create()`, `ui_panel_gcode_test_cleanup()` | `src/ui_panel_gcode_test.cpp` |
| `PrintSelectPanel` | `ui_panel_print_select_init_subjects()`, `ui_panel_print_select_setup()`, etc. | `src/ui_panel_print_select.cpp` |
| `WizardSummaryStep` | `ui_wizard_summary_init_subjects()`, `ui_wizard_summary_create()`, etc. | `src/ui_wizard_summary.cpp` |
| `WizardHeaterSelectStep` | `ui_wizard_heater_select_init_subjects()`, `ui_wizard_heater_select_create()`, etc. | `src/ui_wizard_heater_select.cpp` |
| `WizardFanSelectStep` | `ui_wizard_fan_select_init_subjects()`, `ui_wizard_fan_select_create()`, etc. | `src/ui_wizard_fan_select.cpp` |
| `WizardLedSelectStep` | `ui_wizard_led_select_init_subjects()`, `ui_wizard_led_select_create()`, etc. | `src/ui_wizard_led_select.cpp` |
| `WizardConnectionStep` | `ui_wizard_connection_init_subjects()`, `ui_wizard_connection_create()`, etc. | `src/ui_wizard_connection.cpp` |
| `WizardPrinterIdentifyStep` | `ui_wizard_printer_identify_init_subjects()`, `ui_wizard_printer_identify_create()`, etc. | `src/ui_wizard_printer_identify.cpp` |
| `WizardWifiStep` | `ui_wizard_wifi_init_subjects()`, `ui_wizard_wifi_create()`, `ui_wizard_wifi_init_wifi_manager()`, etc. | `src/ui_wizard_wifi.cpp` |

## Clean Break Checklist

When removing deprecated wrappers from a migrated panel:

1. **Search for old function calls:**
   ```bash
   grep -r "ui_panel_xxx_init_subjects\|ui_panel_xxx_setup" src/
   ```

2. **Update main.cpp** to use class directly:
   ```cpp
   // OLD (deprecated):
   ui_panel_xxx_init_subjects();
   ui_panel_xxx_setup(panel, screen);

   // NEW (class-based):
   auto xxx_panel = std::make_unique<XxxPanel>(get_printer_state(), nullptr);
   xxx_panel->init_subjects();
   xxx_panel->setup(panel, screen);
   ```

3. **Remove wrapper functions** from `.cpp` files

4. **Remove old declarations** from `.h` files

5. **Update tests** using old API

6. **Verify build** with `-Werror`

## Architecture Notes

### PanelBase Hierarchy

```
PanelBase
├── TestPanel
├── GlyphsPanel
├── MotionPanel
├── ...
└── WizardStepBase (future)
    ├── WizardSummaryStep
    ├── WizardWifiStep
    └── ...
```

### Key Patterns

1. **Constructor DI**: All dependencies passed via constructor
2. **RAII Observers**: `register_observer()` for automatic cleanup
3. **Static Trampolines**: `static void xxx_cb()` → instance method
4. **Two-Phase Init**: `init_subjects()` → XML → `setup()`
5. **Move Semantics**: Non-copyable, movable for `unique_ptr`

### Files Created

- `include/ui_panel_base.h` - Abstract base class
- `src/ui_panel_base.cpp` - Base class implementation
- `docs/PANEL_MIGRATION.md` - This tracking file

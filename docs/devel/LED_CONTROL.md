# LED Control System

HelixScreen's LED control system provides unified management of printer LED strips across five backends: native Klipper LEDs, `led_effect` plugin effects, WLED network strips, custom macros, and `output_pin` brightness-only devices. It includes hardware discovery, automatic state-based lighting, a user-facing control overlay, and a settings overlay for configuration.

## Architecture Overview

```
Hardware Discovery (PrinterDiscovery)
    ↓
LedController (singleton, 5 backends)
    ├── NativeBackend      — neopixel, dotstar, led strips via Klipper G-code
    ├── LedEffectBackend   — led_effect plugin animations
    ├── WledBackend        — WLED network strips via Moonraker HTTP bridge
    ├── MacroBackend       — user-configured macro devices
    └── OutputPinBackend   — output_pin brightness-only (PWM) or on/off devices
    ↓
LedAutoState (singleton)           PrinterLedState (domain class)
    │ observes printer state            │ tracks one LED for home panel
    │ applies LED actions               │ provides subjects for XML binding
    ↓                                   ↓
LedControlOverlay (UI)             LedSettingsOverlay (UI)             LedWidget (home panel)
    │ color presets, effects,          │ strip selection, auto-state config,  │ on/off toggle
    │ WLED presets, macro buttons       │ macro device configuration            │ brightness icon
```

## Key Files

### Core

| File | Purpose |
|------|---------|
| `include/led/led_backend.h` | Data types: `LedStripInfo`, `LedEffectInfo`, `LedMacroInfo`, `WledPresetInfo`, enums |
| `include/led/led_controller.h` | `LedController` singleton — orchestrates all 5 backends |
| `src/led/led_controller.cpp` | Discovery, config persistence, toggle_all, startup preference |
| `include/led/led_auto_state.h` | `LedAutoState` singleton — automatic state-to-LED mapping |
| `src/led/led_auto_state.cpp` | Observer-based state tracking, action application, config I/O |

### Domain State

| File | Purpose |
|------|---------|
| `include/printer_led_state.h` | `PrinterLedState` — LVGL subjects for tracked LED (R/G/B/W/brightness/on-off) |
| `src/printer/printer_led_state.cpp` | Updates from Moonraker status JSON, subject lifecycle |

### UI

| File | Purpose |
|------|---------|
| `include/led/ui_led_control_overlay.h` | `LedControlOverlay` — full control overlay (color, effects, WLED, macros) |
| `src/ui/ui_led_control_overlay.cpp` | Section population, action handlers, strip selector |
| `include/ui_settings_led.h` | `LedSettingsOverlay` — settings configuration overlay |
| `src/ui/ui_settings_led.cpp` | Strip selection chips, auto-state editor, macro device editor |
| `include/ui_led_chip_factory.h` | Factory for creating LED action chips in overlay |
| `src/ui/panel_widgets/led_widget.cpp` | `LedWidget` — home panel lightbulb button with version-observer binding |
| `src/ui/panel_widgets/power_widget.cpp` | `PowerWidget` — home panel power toggle (included here as it interacts with LED-adjacent power state) |

### XML Layouts

| File | Purpose |
|------|---------|
| `ui_xml/led_control_overlay.xml` | Control overlay layout (sections, sliders, containers) |
| `ui_xml/led_settings_overlay.xml` | Settings overlay layout (strip chips, toggles, editors) |
| `ui_xml/led_action_chip.xml` | Reusable chip component for LED actions |
| `ui_xml/led_color_swatch.xml` | Color swatch component for preset display |
| `ui_xml/setting_led_chip_row.xml` | Strip selection chip row component |
| `ui_xml/wizard_led_select.xml` | LED selection step in setup wizard |

### Tests

| File | Coverage |
|------|----------|
| `tests/unit/test_led_controller.cpp` | Controller init/deinit, singleton lifecycle, output_pin backend |
| `tests/unit/test_led_config.cpp` | Config persistence: selected strips, color presets, macros |
| `tests/unit/test_led_discovery.cpp` | Hardware discovery from PrinterDiscovery |
| `tests/unit/test_led_auto_state.cpp` | State mapping, evaluate, config round-trip |
| `tests/unit/test_led_native_backend.cpp` | Native strip color control, color cache |
| `tests/unit/test_led_effect_backend.cpp` | Effect activation, target filtering |
| `tests/unit/test_led_wled_backend.cpp` | WLED preset, brightness, toggle, state polling |
| `tests/unit/test_led_macro_backend.cpp` | Macro execution: on/off, toggle, custom actions |
| `tests/unit/test_printer_led_char.cpp` | PrinterLedState subject updates |
| `tests/unit/test_settings_led_char.cpp` | Settings overlay characterization |

## Five Backends

### NativeBackend

Controls Klipper-native LED strips (neopixel, dotstar, led, pca9632) via `SET_LED` G-code commands through `MoonrakerAPI`.

- **Discovery**: From `printer.objects.list` — any object matching `neopixel *`, `dotstar *`, `led *`, `pca9632 *`
- **Color control**: `set_color(strip_id, r, g, b, w)` with 0.0-1.0 RGBW values
- **Color cache**: Tracks current RGBW per strip from Moonraker status updates, with change callbacks
- **Color capability**: `LedStripInfo::supports_color` flag — non-color strips get brightness-only control

### LedEffectBackend

Integrates with the [Klipper LED Effect](https://github.com/julianschill/klipper-led_effect) plugin for animated effects (breathing, fire, rainbow, etc.).

- **Discovery**: From `printer.objects.list` — objects matching `led_effect *`
- **Target filtering**: `effects_for_strip(strip_id)` returns only effects targeting a specific strip
- **Activation**: `activate_effect()` / `stop_all_effects()` via G-code
- **Status tracking**: `update_from_status()` tracks which effects are currently enabled
- **Display helpers**: `display_name_for_effect()` and `icon_hint_for_effect()` for UI

### WledBackend

Controls [WLED](https://kno.wled.ge/) network LED controllers via Moonraker's HTTP bridge.

- **Discovery**: Async via `discover_wled_strips()` — queries Moonraker's `server.config` for WLED entries
- **Controls**: `set_on/off()`, `set_brightness()`, `set_preset()`, `toggle()`
- **Presets**: Fetched directly from WLED device via `fetch_presets_from_device()` (HTTP to `<address>/presets.json`)
- **State polling**: `poll_status()` gets on/off, brightness, active preset from Moonraker
- **Address tracking**: Per-strip IP/hostname from Moonraker server config

### MacroBackend

Executes user-configured Klipper macros for LED control. Three device types:

| Type | Description | Controls |
|------|-------------|----------|
| `ON_OFF` | Separate on/off macros | `execute_on()` / `execute_off()` |
| `TOGGLE` | Single toggle macro | `execute_toggle()` |
| `PRESET` | Named presets, each mapped to a macro | `execute_custom_action()` |

- **Discovery**: Macros with "led" or "light" in the name are auto-discovered as candidates
- **Configuration**: User-configured only — macro devices are created/edited/deleted in LED Settings

### OutputPinBackend

Controls Klipper `[output_pin]` devices used for chamber lights, enclosure LEDs, and other single-channel lighting. These are brightness-only (no color) — either PWM (0-100% brightness slider) or digital on/off.

- **Discovery**: Auto-detected from `printer.objects.list` — `output_pin *` objects with "light", "led", or "lamp" in the name
- **PWM detection**: Checks Klipper config object for `pwm: true` — determines slider vs toggle UI
- **Control**: `SET_PIN PIN=<name> VALUE=<0.0-1.0>` via `MoonrakerAPI`
- **State tracking**: Subscribes to `output_pin <name>` Moonraker status objects; reported `value` (0.0-1.0) maps to brightness percentage
- **Value change callback**: Notifies UI when pin value changes from external sources (macros, other UIs)
- **Strip info flags**: `supports_color = false`, `supports_white = false`, `is_pwm` determines brightness slider vs on/off toggle

## Auto-State Lighting (LedAutoState)

Automatically changes LED behavior based on printer state. Observes `PrinterState` subjects and applies configured actions when state transitions occur.

### Six States

| Key | Triggered When |
|-----|---------------|
| `idle` | Klipper ready, not printing |
| `heating` | Extruder target > 0, not printing |
| `printing` | Print in progress |
| `paused` | Print paused |
| `error` | Klipper in error/shutdown state |
| `complete` | Print just completed |

### Action Types

Each state maps to a `LedStateAction`:

```cpp
struct LedStateAction {
    std::string action_type; // "color", "brightness", "effect", "wled_preset", "macro", "off"
    uint32_t color;          // For "color" action
    int brightness;          // For "color"/"brightness" actions (0-100)
    std::string effect_name; // For "effect" action
    int wled_preset;         // For "wled_preset" action
    std::string macro_gcode; // For "macro" action
};
```

### Observer Pattern

When enabled, `LedAutoState` subscribes to three PrinterState subjects:
- Print state (idle/printing/paused/complete)
- Klippy state (ready/error/shutdown)
- Extruder target temperature (for heating detection)

On any change, `compute_state_key()` determines the current state, and if it differs from `last_applied_key_`, `apply_action()` sends the appropriate LED command.

## Config Persistence

LED configuration is stored in `helixconfig.json` under `/printer/leds/`:

```json
{
    "printer": {
        "leds": {
            "selected_strips": ["neopixel chamber_light", "dotstar status"],
            "last_color": 16777215,
            "last_brightness": 100,
            "color_presets": [16777215, 16766720, 16738101],
            "macro_devices": [
                {
                    "name": "Cabinet Light",
                    "type": "on_off",
                    "on_macro": "LIGHTS_ON",
                    "off_macro": "LIGHTS_OFF"
                }
            ],
            "led_on_at_start": false
        }
    }
}
```

Auto-state mappings are stored under `/printer/leds/auto_state/`:

```json
{
    "printer": {
        "leds": {
            "auto_state": {
                "enabled": true,
                "mappings": {
                    "idle": {"action_type": "brightness", "brightness": 50},
                    "printing": {"action_type": "color", "color": 16777215, "brightness": 100},
                    "error": {"action_type": "color", "color": 16711680, "brightness": 100},
                    "complete": {"action_type": "effect", "effect_name": "rainbow"}
                }
            }
        }
    }
}
```

Config migration from the old `/led/` path is handled automatically on first load.

## UI Components

### LED Control Overlay

Opened via long-press on the home panel lightbulb button. Displays sections for each available backend, auto-hiding unavailable ones via subject-bound visibility flags.

**Sections** (top to bottom):
1. Strip selector — chips for switching between strips
2. Native color — preset swatches, brightness slider, custom color picker (hidden for non-color strips like output_pin)
3. Output pin — brightness slider (PWM) or on/off toggle (non-PWM), shown when an output_pin strip is selected
4. Effects — effect cards with activate/stop-all
5. WLED — toggle, brightness slider, preset buttons
6. Macros — on/off, toggle, and preset action buttons

### LED Settings Overlay

Opened from Settings panel. Configures which strips HelixScreen controls and how automatic lighting works.

**Sections** (top to bottom):
1. LED Selection — multi-select chips for strip selection
2. Startup — "LED on at Start" toggle
3. Automatic LED Control — enable toggle + per-state action editors
4. Macro Devices — add/edit/delete macro device cards

## Threading Model

- **Discovery**: Runs on main thread during printer connection
- **WLED discovery**: Async via Moonraker HTTP — results marshaled to main thread via `ui_async_call()`
- **LED commands**: Sent via `MoonrakerAPI` (through WebSocket, runs on libhv thread)
- **Status updates**: `NativeBackend::update_from_status()`, `OutputPinBackend::update_from_status()`, and `WledBackend::update_strip_state()` called from Moonraker subscription handler (background thread), change callbacks dispatched to main thread
- **UI updates**: All subject updates and widget manipulation on main thread only

## Home Panel Widget Integration

The lightbulb button on the home panel is implemented by `LedWidget` (`src/ui/panel_widgets/led_widget.cpp`). It uses the version-observer self-binding pattern documented in `ARCHITECTURE.md`.

### LedWidget Binding Flow

When `LedWidget::attach()` is called, it immediately subscribes to `LedController::get_led_config_version_subject()`:

```cpp
void LedWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    // ...find light_icon, set user_data...

    // Fires immediately on add (triggering bind_led) — no separate init call needed
    led_version_observer_ = helix::ui::observe_int_sync<LedWidget>(
        led_ctrl.get_led_config_version_subject(), this,
        [weak_alive](LedWidget* self, int /*version*/) {
            if (weak_alive.expired()) return;
            self->bind_led();
        });
}
```

`bind_led()` resets existing per-LED observers, then calls `PrinterState::set_tracked_led()` with the first selected strip, and creates observers for LED state and brightness:

```cpp
void LedWidget::bind_led() {
    led_state_observer_.reset();
    led_brightness_observer_.reset();

    const auto& strips = led_ctrl.selected_strips();
    if (!strips.empty()) {
        // Tell PrinterState which LED to subscribe to for status updates
        printer_state_.set_tracked_led(strips.front());

        // Observe on/off state and brightness from PrinterLedState subjects
        led_state_observer_ = helix::ui::observe_int_sync<LedWidget>(
            printer_state_.get_led_state_subject(), this,
            [weak_alive](LedWidget* self, int state) {
                if (weak_alive.expired()) return;
                self->on_led_state_changed(state);
            });
        led_brightness_observer_ = helix::ui::observe_int_sync<LedWidget>(
            printer_state_.get_led_brightness_subject(), this,
            [weak_alive](LedWidget* self, int /*brightness*/) {
                if (weak_alive.expired()) return;
                self->update_light_icon();
            });
    }
    update_light_icon();
}
```

### Timing: Discovery → Widget Icon

The icon correctly reflects LED state on first render because of an early `set_tracked_led()` call in `printer_discovery.cpp`. The full sequence:

```
1. Hardware discovery runs (PrinterDiscovery)
2. LedController discovers strips, bumps led_config_version_
3. printer_discovery.cpp calls set_tracked_led(first_selected_strip)
   — this starts the Moonraker subscription for that LED before the widget attaches
4. Moonraker subscription response arrives, PrinterLedState subjects are populated
5. Widget attaches (HomePanel::populate_widgets)
6. LedWidget::attach() subscribes to led_config_version_ → fires immediately → bind_led()
7. bind_led() calls set_tracked_led() again (idempotent) and creates state/brightness observers
8. Observers fire with the already-populated subject values → icon renders correctly
```

Without step 3, the subscription would only start after the widget attaches, and the icon would briefly show the wrong state until the first Moonraker status update arrived.

### Icon Rendering

`update_light_icon()` reads brightness and RGBW values from `PrinterLedState` subjects and selects both the icon glyph (empty/dim/full bulb) and its color (muted gray when off, gold for white/warm LEDs, actual color for colored LEDs):

```cpp
void LedWidget::update_light_icon() {
    int brightness = lv_subject_get_int(printer_state_.get_led_brightness_subject());
    const char* icon_name = ui_brightness_to_lightbulb_icon(brightness);
    ui_icon_set_source(light_icon_, icon_name);

    if (brightness == 0) {
        ui_icon_set_color(light_icon_, theme_manager_get_color("light_icon_off"), LV_OPA_COVER);
    } else {
        int r = lv_subject_get_int(printer_state_.get_led_r_subject());
        int g = lv_subject_get_int(printer_state_.get_led_g_subject());
        int b = lv_subject_get_int(printer_state_.get_led_b_subject());
        int w = lv_subject_get_int(printer_state_.get_led_w_subject());
        // White dominant or near-white → gold token; otherwise use actual LED color
        // ...
    }
}
```

### Toggle vs Long-Press

- **Short press** → `led_ctrl.light_toggle()` — toggles all selected strips; updates icon immediately for trackable strips or plays a flash animation for TOGGLE-macro strips
- **Long press** → opens `LedControlOverlay` (lazy-created on first access), pushes it via `NavigationManager`

## Extending the System

### Adding a New Backend

1. Create a new backend class in `include/led/` following the pattern of existing backends
2. Add new `LedBackendType` enum value in `led_backend.h`
3. Add backend member to `LedController` with accessor methods
4. Wire discovery in `LedController::discover_from_hardware()`
5. Add section in `LedControlOverlay::populate_sections()`
6. Add action type support in `LedAutoState::apply_action()`
7. Write unit tests for the new backend

### Adding a New Auto-State Action Type

1. Add the action type string to `LedStateAction::action_type`
2. Handle it in `LedAutoState::apply_action()`
3. Add serialization in `LedAutoState::save_config()` / `load_config()`
4. Add UI controls in `LedSettingsOverlay` for the new action type
5. Add capability filtering (only show if hardware supports it)

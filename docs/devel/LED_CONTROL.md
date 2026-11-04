# LED Control System

HelixScreen's LED control system provides unified management of printer LED strips across five backends: native Klipper LEDs, `led_effect` plugin effects, WLED network strips, custom macros, and `output_pin` brightness-only devices. It includes hardware discovery, automatic state-based lighting, a per-device LEDs overlay, per-instance home-panel light buttons, and a settings overlay for configuration.

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
led/led_devices.h, led/led_device_page.h            LedAutoState (singleton)
    │ pure decisions: chamber light, light targets,      │ observes printer state
    │ device naming, capability classification            │ applies its own device list's actions
    ↓                                                     ↓
LedControlOverlay (UI)         LedSettingsOverlay (UI)         LedWidget / LedControlsWidget
    │ one tab per device,          │ Startup, Automatic LED           (home panel)
    │ a two-column page per         │ Control's own "Applies to"        │ per-instance light button
    │ device's capabilities          │ list, macro devices                │ (device, or All lights)
```

`LedController` owns no shared selection. Every control acts on an explicit list of
device ids the caller supplies — the LEDs overlay's focused device, a light button's
configured target, or Automatic LED Control's own list — never a single shared list that
several features read at once. See [Config Persistence](#config-persistence) for where
each of the old selection's jobs lives now.

## Key Files

### Core

| File | Purpose |
|------|---------|
| `include/led/led_backend.h` | Data types: `LedStripInfo`, `LedEffectInfo`, `LedMacroInfo`, enums |
| `include/led/led_controller.h` | `LedController` singleton — orchestrates all 5 backends, discovery, config persistence, and the id-based device API (`device_state()`, `set_power()`, `light_targets()`, `chamber_light()`, …) |
| `src/led/led_controller.cpp` | Discovery, config persistence, id-based control, startup preference |
| `include/led/led_devices.h`, `src/led/led_devices.cpp` | Pure functions with no LVGL or Moonraker dependency: `resolve_chamber_light()`, `resolve_light_targets()`, `union_light_targets()`, `toggle_target()`, `pick_overlay_focus()`, `device_display_name()`, `macro_device_note()`, `next_power_on()`, `plan_selection_migration()`, `migrate_color_presets()`, the `DeviceState`/`PowerState` pair the overlay and light buttons read |
| `include/led/led_device_page.h`, `src/led/led_device_page.cpp` | `classify_device_page()` — the capability table mapping a device (plus macro type and whether `led_effect` defines anything for it) to the sections its LEDs-overlay page shows; `white_tone()`, `fit_look()`, `ring_for_look()`, `output_rgb()`, `LEVEL_CHIPS` |
| `include/led/led_auto_state.h` | `LedAutoState` singleton — automatic state-to-LED mapping; `stage_light_selection()`, the one-time legacy-selection migration's write path |
| `src/led/led_auto_state.cpp` | Observer-based state tracking, action application, config I/O |
| `include/light_button_config.h`, `src/ui/home/light_button_config.cpp` | Per-instance home light button config: `home_light_button_keys()`, `adopt_pending_light_button()`, `home_light_button_targets()` |

### UI

| File | Purpose |
|------|---------|
| `include/led/ui_led_control_overlay.h` | `LedControlOverlay` — the LEDs overlay: tabs, focus, and the two-column device page |
| `src/ui/controls/ui_led_control_overlay.cpp` | Tab rebuild, page publishing, control handlers |
| `include/ui_settings_led.h` | `LedSettingsOverlay` — settings configuration overlay |
| `src/ui/settings/ui_settings_led.cpp` | Startup section, Automatic LED Control's "Applies to" row and per-state editors, macro device editor |
| `include/ui_led_chip_factory.h` | Chip factory shared by Settings' macro-device editor |
| `src/ui/panel_widgets/led_widget.cpp` | `LedWidget` — home panel light button: per-instance config, device picker, tile bulb |
| `src/ui/panel_widgets/led_controls_widget.cpp` | `LedControlsWidget` — home panel tile that always opens the LEDs overlay |
| `src/ui/widgets/power_device_widget.cpp` | `PowerDeviceWidget` — home panel power toggle (included here as it interacts with LED-adjacent power state) |

### XML Layouts

| File | Purpose |
|------|---------|
| `ui_xml/led_control_overlay.xml` | The LEDs overlay: the tab strip, and the lamp/look two-column device page |
| `ui_xml/led_settings_overlay.xml` | Settings overlay layout: Startup, Automatic LED Control's "Applies to" row and per-state editors, macro device editor |
| `ui_xml/led_action_chip.xml` | Reusable chip component for level chips and macro buttons |
| `ui_xml/led_list_chip.xml` | One effect or preset chip, its label from a subject |
| `ui_xml/led_white_tone.xml` | One White-section tone swatch (Cool/Neutral/Warm) with its caption |
| `ui_xml/components/color_swatch.xml` | The app's one round color swatch — every LED color row, and the filament and theme color pickers, share it |
| `ui_xml/setting_led_chip_row.xml` | The device chip row component behind Automatic LED Control's "Applies to" row |
| `ui_xml/wizard_led_select.xml` | LED selection step in setup wizard |

### Tests

| File | Coverage |
|------|----------|
| `tests/unit/test_led_devices.cpp` | The pure decisions in `led_devices.h`: chamber-light resolution, light-target resolution and union, migration planning, color-preset migration, display naming |
| `tests/unit/test_led_device_page.cpp` | `classify_device_page()` per device kind/macro type, `white_tone()`, `fit_look()`, `ring_for_look()`, `output_rgb()` |
| `tests/unit/test_led_device_state.cpp` | `LedController::device_state()` per backend |
| `tests/unit/test_light_button_config.cpp` | Per-instance light button config round trip, pending adoption, `home_light_button_targets()` |
| `tests/unit/test_led_widget.cpp` | `LedWidget`: default resolution, the device picker, the 2x1 arrow zone, tile rendering |
| `tests/unit/test_led_entry_points.cpp` | Every entry point opens the LEDs overlay on the device it names |
| `tests/unit/test_led_controller.cpp` | Controller init/deinit, singleton lifecycle, output_pin backend |
| `tests/unit/test_led_config.cpp` | Config persistence: legacy selection, color presets, macros |
| `tests/unit/test_led_discovery.cpp` | Hardware discovery from PrinterDiscovery |
| `tests/unit/test_led_auto_state.cpp` | State mapping, evaluate, config round-trip, its own device list |
| `tests/unit/test_led_native_backend.cpp` | Native strip color control, color cache |
| `tests/unit/test_led_effect_backend.cpp` | Effect activation, target filtering |
| `tests/unit/test_led_wled_backend.cpp` | WLED brightness, toggle, state polling |
| `tests/unit/test_led_macro_backend.cpp` | Macro execution: on/off, toggle, custom actions |
| `tests/unit/test_led_control_overlay.cpp` | Tab rebuild, focus, page publishing, control handlers |
| `tests/unit/test_led_settings_overlay.cpp` | Startup, Applies-to row, macro device editor |

## Five Backends

### NativeBackend

Controls Klipper-native LED strips (neopixel, dotstar, led, pca9632) via `SET_LED` G-code commands through `MoonrakerAPI`.

- **Discovery**: From `printer.objects.list` — any object matching `neopixel *`, `dotstar *`, `led *`, `pca9632 *`
- **Color control**: `set_color(strip_id, r, g, b, w)` with 0.0-1.0 RGBW values
- **Color cache**: Tracks current RGBW per strip from Moonraker status updates, with change callbacks
- **Color capability**: `LedStripInfo::supports_color` flag — non-color strips get brightness-only control
- **White capability**: `LedStripInfo::supports_white` comes from the Klipper config. A neopixel is RGBW only when every entry of its `color_order` has a `W` (`src/led/led_controller.cpp#color_order_has_white`); a pin layout known from the config outranks the channel count a status frame reports
- **Brightness 0 is off**: `LedController::set_brightness(ids, 0)` powers the devices off on every backend instead of holding them lit at 0

### LedEffectBackend

Integrates with the [Klipper LED Effect](https://github.com/julianschill/klipper-led_effect) plugin for animated effects (breathing, fire, rainbow, etc.).

- **Discovery**: From `printer.objects.list` — objects matching `led_effect *`
- **Target filtering**: `effects_for_strip(strip_id)` returns only effects targeting a specific strip
- **Activation**: `activate_effect()` / `stop_all_effects()` via G-code
- **Status tracking**: `update_from_status()` tracks which effects are currently enabled
- **Display helpers**: `display_name_for_effect()` and `icon_hint_for_effect()` for UI

### WledBackend

Controls [WLED](https://kno.wled.ge/) network LED controllers via Moonraker's HTTP bridge.

- **Discovery**: Async via `discover_wled_strips()` — queries Moonraker's WLED strip list
- **Controls**: `set_on/off()`, `set_brightness()`, `set_preset()`, `toggle()`
- **State polling**: `poll_status()` gets on/off, brightness, active preset from Moonraker
- **No color, no preset list**: WLED gets power and brightness from the LEDs overlay. Preset IDs are user-defined on the device and HelixScreen cannot list them, so presets are reached only through an Automatic LED Control `wled_preset` action with an explicit ID

### MacroBackend

Executes user-configured Klipper macros for LED control. Three device types:

| Type | Description | Controls |
|------|-------------|----------|
| `ON_OFF` | Separate on/off macros | `execute_on()` / `execute_off()` |
| `TOGGLE` | Single toggle macro | `execute_toggle()` |
| `PRESET` | Named presets, each mapped to a macro | `execute_custom_action()` |

- **Discovery**: Macros with "led" or "light" in the name are auto-discovered as candidates
- **Configuration**: User-configured only — macro devices are created/edited/deleted in LED Settings
- **No readable power state**: a macro device's page never shows a power dot or a filled power button, because HelixScreen cannot read whether it is on. `ON_OFF` gets explicit On/Off buttons where the slider would sit; `TOGGLE` gets one Toggle button; `PRESET` gets only its preset chips.

### OutputPinBackend

Controls Klipper `[output_pin]` devices used for chamber lights, enclosure LEDs, and other single-channel lighting. These are brightness-only (no color) — either PWM (0-100% brightness slider) or digital on/off.

- **Discovery**: Auto-detected from `printer.objects.list` — `output_pin *` objects with "light", "led", or "lamp" in the name (see the safety note below)
- **PWM detection**: Checks Klipper config object for `pwm: true` — determines slider vs toggle UI
- **Control**: `SET_PIN PIN=<name> VALUE=<0.0-1.0>` via `MoonrakerAPI`
- **State tracking**: Subscribes to `output_pin <name>` Moonraker status objects; reported `value` (0.0-1.0) maps to brightness percentage
- **Value change callback**: Notifies UI when pin value changes from external sources (macros, other UIs)
- **Strip info flags**: `supports_color = false`, `supports_white = false`, `is_pwm` determines brightness slider vs on/off toggle

#### ⚠️ The `output_pin` name heuristic is safety logic — do not loosen it

`[output_pin]` is a generic Klipper primitive. Users wire it to whatever they like, and
the object name is the *only* signal we get about what a pin actually does. The full
classifier lives in `PrinterDiscovery::parse_objects()` (`include/printer_discovery.h`)
and is a deliberately narrow allowlist:

| Pin name (upper-cased, after the `output_pin ` prefix) | Classified as |
|---|---|
| starts with `FAN` | fan |
| contains `LIGHT`, `LED`, or `LAMP` | LED |
| contains `BEEPER`, `BUZZER`, or `SPEAKER` | speaker (enables M300) |
| anything else | **ignored entirely** |

Anything we classify becomes writable from the UI via `SET_PIN PIN=<name> VALUE=<v>`.
That is why the default is to ignore: a pin we do not recognise is a pin we never touch.

Real hardware makes the stakes concrete. The Snapmaker U1 defines `output_pin e0_heat_sw`
through `e3_heat_sw` — per-extruder **hotend heater power switches**. They fall through
to "ignored" only because they match none of the patterns above. Widen the LED test to
something like a bare `SW` substring and HelixScreen would list four heater switches as
chamber lights and let a user toggle them from the LEDs overlay.

`tests/unit/test_printer_discovery_real_hardware.cpp` guards this with object lists
captured verbatim from real machines (K1C, Snapmaker U1, Voron V2.4). The U1 case exists
specifically to fail if the heuristic ever stops excluding those heater switches. This
was verified by mutation: adding `SW` to the LED patterns fails 5 assertions in that file.

Synthetic fixtures cannot catch this class of regression, because whoever loosens the
heuristic writes the synthetic fixture too. Re-capture with
`curl -s http://<printer>:7125/printer/objects/list` if you add a machine.

Note that the same `SET_PIN` command drives both categories — on the K1C, `output_pin LED`
is a light while `output_pin fan0/1/2` are fans. Nothing but the name distinguishes them,
which is also why `SET_PIN` carries the generic "change" noun rather than "LED change" in
the busy-queue toast (see `include/gcode_classify.h`).

## The Chamber Light

`resolve_chamber_light()` (`include/led/led_devices.h`) is the one function that knows the
Klipper object-name spellings a chamber light comes under. It matches a switchable
device's id (after its type prefix) against `chamber_light`, `chamber_LED` (Zmod),
`case_light` and `caselight`, case-insensitively and in that order, and falls back to
`LedController::first_available_strip()` when nothing matches.

`LedController::chamber_light()` calls it with `all_selectable_strips()` and the
controller's own `first_available_strip()` as the fallback. Three things use the result,
and nothing else names these spellings:

- A home light button whose `led` config is empty resolves to the chamber light
  (`resolve_light_targets()`, same file).
- The LEDs overlay's fallback focus, when no device was requested and none was last
  focused (`pick_overlay_focus()`).
- The printer-image widget's light chip, which shows only while the chamber light is on
  and opens the LEDs overlay on it.

## Device Naming

`device_display_name()` (`include/led/led_devices.h`) is the one function every LED
surface calls for a device's name: a macro device's configured display name, otherwise
its Klipper or WLED object name prettified (`"neopixel chamber_light"` → `"Chamber
Light"`). It is not LED-specific plumbing — `ui_panel_power.cpp`, `ui_panel_macros.cpp`
and the setup wizard's hardware selector use the same function so a device reads the same
name everywhere it appears.

## Auto-State Lighting (LedAutoState)

Automatically changes LED behavior based on printer state. Observes `PrinterState` subjects and applies configured actions when state transitions occur — on its own device list, entirely separate from what a home light button controls (see [Config Persistence](#config-persistence)).

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

On any change, `compute_state_key()` determines the current state, and if it differs from `last_applied_key_`, `apply_action()` sends the appropriate LED command to `targets()` — its own device list (`strips_`) filtered down to devices still switchable, falling back to the chamber light if that leaves nothing (a device removed since the list was saved, or the list itself empty).

### Lifecycle / Initialization

`LedAutoState` is a dormant singleton until `init(PrinterState&)` is called — `load_config()` runs only inside `init()`, and `evaluate()` / `on_state_changed()` early-return unless `initialized_` is set. The wiring lives in the printer lifecycle:

- **init** — `init_subsystems_from_hardware()` in `src/printer/printer_discovery.cpp` calls `LedAutoState::instance().init(printer_state)` immediately after `LedController::init()` / `discover_from_hardware()`. `init()` is idempotent: it first unsubscribes any stale observers, reloads the per-printer config, and re-subscribes if enabled.
- **deinit** — `Application::tear_down_printer_state()` in `src/application/application.cpp` calls `LedAutoState::instance().deinit()` (step 10b) **before** `LedController::deinit()` (step 11) and well before `StaticSubjectRegistry::deinit_all()`.

**Ordering constraints (both load-bearing):**

1. `LedAutoState` observes PrinterState subjects, so it **must deinit before those subjects are destroyed** (`deinit_all()`). Otherwise the observers outlive their subjects and `lv_subject_deinit()` frees observers that `ObserverGuard` still references → use-after-free.
2. `LedAutoState` drives `LedController` in `apply_action()`, so it **must deinit before `LedController`** to avoid a deferred auto-state callback touching a torn-down controller.

**Per-printer reload on switch.** `load_config()` reads from the active-printer path `<Config::df()>leds/auto_state/...` (i.e. `/printers/<active>/leds/auto_state/enabled`, `.../strips` and `.../mappings`), with a one-time migration from the legacy global `/led/auto_state/` path. Because `switch_printer()` runs teardown → `init_printer_state` → `connect_moonraker` → discovery, the discovery-path `init()` call automatically reloads the new printer's auto-state config on every printer switch — no extra wiring needed.

**Main-thread-only.** `init()` (and thus `subscribe_observers()` via `observe<int>`) runs on the main thread: `init_subsystems_from_hardware()` is invoked inside a `queue_update()` drain. Do not call `init()` / `deinit()` from a background thread.

## Config Persistence

LED configuration is stored in `settings.json` under `/printer/leds/`:

```json
{
    "printer": {
        "leds": {
            "selected_strips": ["neopixel chamber_light", "dotstar status"],
            "last_color": 16777215,
            "last_brightness": 100,
            "color_presets": [16729156, 16739125, 6732650, 48340, 2712319, 10233776, 16728193],
            "macro_devices": [
                {
                    "name": "Cabinet Light",
                    "type": "on_off",
                    "on_macro": "LIGHTS_ON",
                    "off_macro": "LIGHTS_OFF"
                }
            ],
            "led_on_at_start": false,
            "light_button_pending": ""
        }
    }
}
```

- `leds.selected_strips` (and the older single-strip `leds.selected`) **is a legacy key.**
  `load_config()` reads it into `legacy_selection_` for the one-time migration below
  (`LedController::migrate_legacy_selection()`), which runs on the first discovery that
  finds no `leds/auto_state/strips` saved yet. Nothing writes it back and nothing else
  reads it; the key stays on disk as it was.
- `leds.color_presets` defaults to `kDefaultColorPresets` in `include/led/led_devices.h`
  (red, orange, green, cyan, blue, purple, pink). `migrate_color_presets()` recognises the
  untouched pre-1.1 default list and upgrades it silently; a list a user actually edited
  is left alone.
- `leds.light_button_pending` is a one-shot value written by the legacy-selection
  migration (and by the setup wizard's LED-selection step) and consumed the first time a
  home light button adopts it (`adopt_pending_light_button()`,
  `include/light_button_config.h`). It is cleared after that adoption pass runs, even
  when the home layout has no light button yet, so a button added months later still
  falls back to the chamber-light default rather than reviving a stale value.

Where each of the old selection's three jobs actually lives now:

- **Home light buttons**: per widget instance, under that button's own
  `panel_widgets.home` entry (`config.led` — see
  [Home Panel Widget Integration](#home-panel-widget-integration)).
- **Automatic LED Control**: its own device list, `leds/auto_state/strips`, edited from
  the "Applies to" row in LED Settings.
- **LED on at Start**: no list of its own — `home_light_button_targets()` unions every
  home light button's target, falling back to the chamber light when there are no light
  buttons, and that is what `apply_startup_preference()` is called with. A button may
  name a WLED strip that has not been discovered yet, so `settle_light_buttons()`
  (`include/light_button_config.h`) waits while `LedController::wled_discovery_pending()`;
  a discovery settles when strips arrive, none are configured, it fails, or
  `kWledDiscoveryTimeoutMs` (5s) passes, and the app re-runs the settle then.

Auto-state mappings (and now its own device list) are stored **per printer** under `<Config::df()>leds/auto_state/` — i.e. `/printers/<active-printer-id>/leds/auto_state/` (mappings serialize the action under the `action` key with the color as a `#RRGGBB` string; see `LedAutoState::save_config()`):

```json
{
    "printers": {
        "<active-printer-id>": {
            "leds": {
                "auto_state": {
                    "enabled": true,
                    "strips": ["neopixel toolhead_leds"],
                    "mappings": {
                        "idle": {"action": "brightness", "color": "#FFFFFF", "brightness": 50},
                        "printing": {"action": "color", "color": "#FFFFFF", "brightness": 100},
                        "error": {"action": "color", "color": "#FF0000", "brightness": 100},
                        "complete": {"action": "effect", "color": "#FFFFFF", "brightness": 100, "effect_name": "rainbow"}
                    }
                }
            }
        }
    }
}
```

Config migration from the old global `/led/auto_state/` path to the active-printer path is handled automatically on first load (`LedAutoState::load_config()`).

### The one-time selection migration

The first hardware discovery that finds a saved `leds/selected_strips` but no saved
`leds/auto_state/strips` runs `LedController::migrate_legacy_selection()`, which calls
`plan_selection_migration()` (`include/led/led_devices.h`) and applies the result through
`stage_light_selection()` (`include/led/led_auto_state.h`):

| Old `selected_strips` | Light button gets | Auto-state target gets |
|---|---|---|
| Exactly one switchable device | That device (written to `leds/light_button_pending`) | The same one device |
| Every switchable device | `kLightButtonAll` (`leds/light_button_pending`) | Every switchable device |
| Anything else (a subset, or none of the above) | Nothing written — falls back to the chamber-light default | The old selection, unchanged |

The third row is the one case users notice: a partial selection carries over to Automatic
LED Control only, and light buttons start on the chamber-light default. `stage_light_selection()` writes `auto_state/strips`
unconditionally (so a re-run never mistakes a real empty selection for "not yet
migrated") and the pending light-button value only when the plan produced one.

## UI Components

### The LEDs Overlay (LedControlOverlay)

Opened from a home light button's arrow zone, the printer-image light chip, the LED
Controls widget, or the `leds` demo screen — always through
`open_led_control_overlay(parent, device_id)`. An empty `device_id` opens on the overlay's last-focused device this
session, else the chamber light (`pick_overlay_focus()`).

**Tabs.** One tab per device, in `all_selectable_strips()` order, then every named
PRESET macro device. Tabs sit in a single row that scrolls sideways, with a fade at the
right edge once more tabs sit past it. A tab only changes which device is *focused*
(`focused_strip_`, owned by the overlay) — it is never written to any selection,
configuration, or Automatic LED Control target. Each tab carries a state dot: filled in
the device's current color when on, hollow when off, a dimmed ring when the state cannot
be read (macro devices). Tabs rebuild only when the overlay activates, not on a mid-view
device-list change, so a device added while the overlay is open, appears the next time it opens.

**The device page.** Two columns for the focused device: the lamp (power, brightness, or
the macro buttons) on the left, the look (White, Color, Effects/Presets/Level chips) on
the right. `LedControlOverlay::load_page_state()` reads the focused device's current
color, brightness and white level on focus and on activation only; a live Moonraker
status frame updates the tab dots, the power state, and the page color, but never
re-reads the slider or swatch selection out from under a control in progress.
`classify_device_page()` (`include/led/led_device_page.h`) is the single capability table
behind which sections a page shows — LVGL never branches on device kind directly, it
binds to the `DevicePage` enum values the controller publishes as page subjects:

| Device | Lamp (`LampControl`) | White (`WhiteMode`) | Color | List (`ListKind`) |
|---|---|---|---|---|
| Klipper LED, RGBW | `PowerAndBrightness` | `WChannel` | yes | `Effects`, when `led_effect` defines any for the strip |
| Klipper LED, RGB | `PowerAndBrightness` | `Mixed` | yes | `Effects`, when defined |
| Klipper LED, single channel | `PowerAndBrightness` | `None` | no | `LevelChips` |
| WLED | `PowerAndBrightness` | `None` | no | `Presets` from the device |
| Output pin, PWM | `PowerAndBrightness` | `None` | no | `LevelChips` |
| Output pin, on/off | `PowerOnly` | `None` | no | `None` |
| Macro, ON_OFF | `OnOffButtons` | `None` | no | a note naming the macros |
| Macro, TOGGLE | `ToggleButton` | `None` | no | a note naming the macro |
| Macro, PRESET | `None` | `None` | no | `Presets`, full width |

A `LampControl::None` device (a PRESET macro) hides the whole lamp column
(`bind_flag_if_eq subject="led_page_lamp" ref_value="4"` in the XML); `PowerOnly` centers
the single power button where the brightness slider would sit. Color, when present, is
`color_presets()` rendered as `color_swatch` components plus a Custom swatch that opens
the existing color picker; `ring_for_look()` decides which swatch (or which White tone)
rings for the device's current look, scaling both sides of every comparison so their
brightest channel is full before comparing. `fit_look()` fits an arbitrary RGB+W look to
what a device can actually show (brightness-only devices get white; RGB devices with no
W channel fold W into RGB).

**Focus resolution.** `request_focus(device_id)` sets the device the *next* activation
opens on; `focused_device()` reports the current one. Every entry point that knows which
device it means calls `request_focus()` before pushing the overlay, so a light button's
arrow zone and the printer-image chip land where they say they will.

### LED Settings Overlay

Opened from Settings panel, still titled "LED Settings". Configures Automatic LED
Control and macro devices — no strip-selection chip row lives here any more; a home
light button's target is set from that button's own edit-mode gear, not from Settings.

**Sections** (top to bottom):
1. Startup — "LED on at Start" toggle (turns on every device the home light buttons
   control, or the chamber light with none placed) and Startup Brightness
2. Automatic LED Control — enable toggle, its own "Applies to" device chip row
   (`leds/auto_state/strips`), and per-state action editors
3. Macro Devices — add/edit/delete macro device cards

## Threading Model

- **Discovery**: Runs on main thread during printer connection
- **WLED discovery**: Async via Moonraker HTTP — results marshaled to main thread via `ui_queue_update()`
- **LED commands**: Sent via `MoonrakerAPI` (through WebSocket, runs on libhv thread)
- **Status updates**: `NativeBackend::update_from_status()`, `OutputPinBackend::update_from_status()`, and `WledBackend::update_strip_state()` called from Moonraker subscription handler (background thread), change callbacks dispatched to main thread
- **UI updates**: All subject updates and widget manipulation on main thread only
- **Auto-state lifecycle**: `LedAutoState::init()` / `deinit()` run on the main thread only (init via the `init_subsystems_from_hardware()` discovery path inside a `queue_update()` drain; deinit via `Application::tear_down_printer_state()`). Its `observe<int>` subscriptions defer callbacks through the UpdateQueue, so auto-state actions always apply on the main thread

## In-Flight Command Tracking

`led_command_in_flight` (an `lv_subject_t` on `LedController`) is the subject every light button — the home panel widget and a Controls panel Quick Actions light slot — disables on while a toggle command is outstanding.

`LedController::set_power(ids, on)` (`src/led/led_controller.cpp`), called by `toggle_power(ids)` with the id list's next state, builds a per-dispatch `Settle{done, fail, queued}` triple via a `make_settle()` factory: `make_settle()` increments the in-flight counter immediately, and whichever of the three settle callbacks fires later decrements it. Not every backend branch calls `make_settle()`:

- **`NATIVE`** and **`OUTPUT_PIN`** emit discretionary G-code (`SET_LED`, `SET_PIN`) and pass `cbs.queued` — while an external blocking op holds Klipper's gcode lock, the command is queued fire-and-forget and its RPC response is dropped, so `on_queued` is the only settle signal that will ever fire on that path.
- **`WLED`** goes over HTTP and never touches the gcode lock, so it only passes `cbs.done`/`cbs.fail`.
- **`MACRO`** calls user-defined macros, which stay non-discretionary under the default-allow rule in `gcode_classify.h` and always get a real ACK, so it only passes `cbs.done`/`cbs.fail`.
- **`LED_EFFECT`** never touches this counter. Effects are driven separately via `activate`/`stop`.

A new backend branch that emits gcode MUST pass `on_queued` (or route through something that does) — otherwise a command queued behind a blocking op wedges the counter and greys out every light button for the rest of the session (#1129, see `ARCHITECTURE.md` § "Klippy-Volatile Subjects" for the root cause and `MOONRAKER_ARCHITECTURE.md` § "MoonrakerAPI (Domain Logic Layer)" for the `execute_gcode()` contract).

That branch must also **declare `caller_surfaces_errors` honestly**, and capture it before its own wrapper. `make_settle()` produces a `fail` callback for every branch, so by the time the send is issued `on_error` is non-null whether or not anything reaches the user — `activate_effect()` and `stop_all_effects()` therefore compute `caller_surfaces_errors && (on_error != nullptr)` from the *caller's* argument (`src/led/led_controller.cpp#activate_effect`), not from the settle wrapper. A branch whose failure handling only decrements the counter and logs must pass `false`: claiming the report records the rejection for cross-channel dedup and silences `GcodeErrorRouter`'s `!!` copy, which for a `SET_LED` rejection is the only thing that would have told the user. See `RPC_ERROR_OWNERSHIP.md`.

As a last-resort safety net, `LedController` also force-clears the counter (`force_clear_in_flight()`) on a Moonraker disconnect and on any Klippy state leaving `READY`, so a leaked dispatch cannot stay pinned across a reconnect or firmware restart.

## Home Panel Widget Integration

### LedWidget — the light button

`LedWidget` (`src/ui/panel_widgets/led_widget.cpp`) follows the per-instance
configurable-widget pattern `FanWidget` set: `set_config()` reads the widget's own `led`
key from its `panel_widgets.home` entry, `on_edit_configure()` opens a device picker from
the edit-mode gear, and `save_widget_config()` persists the choice. Several `LedWidget`
instances can sit on the home screen at once, each independently configured — the widget
registry's `multi_instance` flag, the same mechanism `fan` and `favorite_macro` use for
their extra copies.

- **The picker** ("This button controls") lists every switchable device
  (`LedController::switchable_ids()` — a PRESET-only macro device, which cannot be
  toggled, is never listed) plus **All lights** (`kLightButtonAll`,
  `include/led/led_devices.h`). Leaving a button unconfigured (`led` empty) resolves to
  the chamber light (`resolve_light_targets("", ...)`).
- **Resolving a tap.** `LedController::light_targets(key)` turns the widget's `led`
  value into the device id list a tap should act on: the empty key resolves through
  `resolve_light_targets()` to the chamber light, `kLightButtonAll` to every switchable
  device, any other key to that one device (falling back to the chamber light if the
  device no longer exists).
- **The tile** shows the target's display name and a bulb in its current color when on.
- **1x1**: a tap toggles the target. **2x1 or wider**: a tap still toggles, and a
  separate arrow zone opens the LEDs overlay focused on the target (the chamber light for
  a button set to All lights — there is no single "All lights" tab).
- **Adoption.** When a light button binds with no `led` value of its own,
  `adopt_pending_light_button()` (`include/light_button_config.h`) copies
  `leds/light_button_pending` into it — this is how a button placed after the one-time
  legacy-selection migration ran still picks up its planned target, and how the button
  the migration itself points at gets it on first bind. The pending value is cleared
  after that adoption pass runs even when no light button exists yet to adopt it (a
  button added later must default to the chamber light, not revive a stale migration
  value).

### LedControlsWidget

`LedControlsWidget` (`src/ui/panel_widgets/led_controls_widget.cpp`) is not
per-instance: it always opens the LEDs overlay, on the overlay's last-focused device or
the chamber light — the same fallback the overlay uses when opened with no device at
all.

The two tiles gate on different subjects. The LED Controls tile shows while
`led_has_devices` is 1 (any LED device at all, PRESET-only macro devices included, so
their presets stay reachable). A light button, home or Quick Actions, shows while
`led_controllable` is 1 (a chamber light resolves, so a tap has something to switch).
`LedController::publish_controllable_state()` sets both.

### Light buttons off the home screen

The Controls panel's Quick Actions light slots (`ControlsPanel::led_widgets_`) and the
print-status light button have no configuration of their own and act on the chamber
light, the same as an unset home light button.

### Entry points and focus

| Opened from | Lands on |
|---|---|
| A light button's arrow zone (2x1 or wider) | that button's target device (the chamber light for a button set to All lights) |
| The printer-image light chip | the chamber light |
| LED Controls widget | the overlay's last-focused device, else the chamber light |

The printer-image light chip shows only while the chamber light is on
(`resolve_chamber_light()` decides which device that is; see [The Chamber
Light](#the-chamber-light)). `PrinterImageWidget::route_callout_click()` stops the tap
bubbling, so the Printer Manager behind the image does not open on top of the overlay;
a long-press still bubbles to the home grid for edit mode.

## Extending the System

### Adding a New Backend

1. Create a new backend class in `include/led/` following the pattern of existing backends
2. Add new `LedBackendType` enum value in `led_backend.h`
3. Add backend member to `LedController` with accessor methods
4. Wire discovery in `LedController::discover_from_hardware()`
5. Add a row to `classify_device_page()`'s capability table (`include/led/led_device_page.h`) for the sections the new backend's devices should show
6. Add a case to `LedController::device_state()` so the overlay and light buttons can read the new backend's power/brightness/color
7. Add action type support in `LedAutoState::apply_action()`
8. Write unit tests for the new backend

### Adding a New Auto-State Action Type

1. Add the action type string to `LedStateAction::action_type`
2. Handle it in `LedAutoState::apply_action()`
3. Add serialization in `LedAutoState::save_config()` / `load_config()`
4. Add UI controls in `LedSettingsOverlay` for the new action type
5. Add capability filtering (only show if hardware supports it)

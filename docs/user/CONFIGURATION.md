# Configuration Reference

Complete reference for HelixScreen configuration options.

---

## Table of Contents

- [Configuration File Location](#configuration-file-location)
- [Configuration Structure](#configuration-structure)
- [General Settings](#general-settings)
- [Theme Settings](#theme-settings)
- [Logging Settings](#logging-settings)
- [Display Settings](#display-settings)
- [Input Settings](#input-settings)
- [Output Settings](#output-settings)
- [Network Settings](#network-settings)
- [Printer Settings](#printer-settings)
- [LED Settings](#led-settings)
- [Moonraker Settings](#moonraker-settings)
- [G-code Viewer Settings](#g-code-viewer-settings)
- [AMS Settings](#ams-settings)
- [Panel Widget Settings](#panel-widget-settings)
- [Cache Settings](#cache-settings)
- [Streaming Settings](#streaming-settings)
- [Safety Settings](#safety-settings)
- [Filament Sensor Settings](#filament-sensor-settings)
- [Plugin Settings](#plugin-settings)
- [Update Settings](#update-settings)
- [Safety Limits](#safety-limits)
- [Capability Overrides](#capability-overrides)
- [Resetting Configuration](#resetting-configuration)
- [Command-Line Options](#command-line-options)
- [Environment Variables](#environment-variables)

---

## Configuration File Location

| Platform | Location |
|----------|----------|
| MainsailOS (Pi) | `~/helixscreen/config/helixconfig.json` (or `/opt/helixscreen/config/` if no Klipper ecosystem) |
| AD5M Forge-X | `/opt/helixscreen/config/helixconfig.json` |
| AD5M Klipper Mod | `/root/printer_software/helixscreen/config/helixconfig.json` |
| K1 Simple AF | `/usr/data/helixscreen/config/helixconfig.json` |
| Development | `./config/helixconfig.json` (in config/ directory) |

> **Note:** On Pi, the installer auto-detects your Klipper ecosystem. If `~/klipper`, `~/moonraker`, or `~/printer_data` exists, HelixScreen installs to `~/helixscreen`. Otherwise it falls back to `/opt/helixscreen`. You can override with `INSTALL_DIR=/path ./install.sh`.

The configuration file is created automatically by the first-run wizard. You can also copy from the template:

```bash
cp config/helixconfig.json.template config/helixconfig.json
```

**Note:** Legacy config locations (`helixconfig.json` in app root or `/opt/helixscreen/helixconfig.json`) are automatically migrated to the new location on startup.

---

## Configuration Structure

The configuration file is JSON format with several top-level sections:

```json
{
  "dark_mode": false,
  "brightness": 50,
  "sounds_enabled": true,
  "completion_alert": 2,
  "wizard_completed": false,
  "wifi_expected": false,
  "language": "en",
  "beta_features": false,
  "log_dest": "auto",
  "log_path": "",
  "log_level": "warn",

  "panel_widgets": { ... },
  "theme": { ... },
  "display": { ... },
  "input": { ... },
  "output": { ... },
  "network": { ... },
  "printer": { ... },
  "gcode_viewer": { ... },
  "ams": { ... },
  "cache": { ... },
  "streaming": { ... },
  "safety": { ... },
  "filament_sensors": { ... },
  "plugins": { ... },
  "update": { ... }
}
```

---

## General Settings

### `dark_mode`
**Type:** boolean
**Default:** `false`
**Description:** Use dark theme (`true`) or light theme (`false`). Can also be set via Settings panel or `--dark`/`--light` CLI flags.

### `brightness`
**Type:** integer
**Default:** `50`
**Range:** `1` - `100`
**Description:** Screen brightness percentage. Adjustable via Settings panel.

### `sounds_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Enable UI sound effects (button clicks, navigation sounds).

### `completion_alert`
**Type:** integer
**Default:** `2`
**Values:** `0` (Off), `1` (Notification), `2` (Alert)
**Description:** How HelixScreen notifies you when a print completes or is cancelled (while you're on a different screen):
- `0` — **Off**: No notification (sound still plays if sounds are enabled)
- `1` — **Notification**: Brief toast message at the top of the screen
- `2` — **Alert**: Full-screen modal with print stats (duration, layers, filament used) and confetti for successful prints

Errors always show the full alert regardless of this setting. To change this in the UI, go to **Settings > Print Complete Alert** and select from the dropdown.

### `wizard_completed`
**Type:** boolean
**Default:** `false`
**Description:** Whether the setup wizard has been completed. Set automatically after first-run wizard. Set to `false` to re-trigger the wizard on next startup.

### `wifi_expected`
**Type:** boolean
**Default:** `false`
**Description:** Whether WiFi connectivity is expected. When `true`, HelixScreen shows connection warnings if WiFi is unavailable. Set during the wizard based on your network configuration choice.

### `language`
**Type:** string
**Default:** `"en"`
**Values:** `"en"` (English)
**Description:** UI language code. Currently only English is supported.

### `beta_features`
**Type:** boolean
**Default:** `false`
**Description:** Enable beta features that are still under testing. Gates several Advanced panel features (Macro Browser, Input Shaping, Z-Offset Calibration, HelixPrint plugin management, PRINT_START configuration, Timelapse), the Plugins section in Settings, and the Update Channel selector. Always enabled automatically when running in `--test` mode. Can also be toggled by tapping the version button 7 times in Settings > About. See the [Beta Features](USER_GUIDE.md#beta-features) section in the User Guide for the full list.

---

## Theme Settings

Located in the `theme` section:

```json
{
  "theme": {
    "preset": 0
  }
}
```

### `preset`
**Type:** integer
**Default:** `0`
**Values:** `0` (Nord)
**Description:** Theme accent color preset. **Requires restart to take effect.**

---

## Logging Settings

### `log_dest`
**Type:** string
**Default:** `"auto"`
**Values:** `"auto"`, `"journal"`, `"syslog"`, `"file"`, `"console"`
**Description:** Log destination:
- `auto` - Detect best option (journal on systemd, console otherwise)
- `journal` - systemd journal (view with `journalctl -u helixscreen`)
- `syslog` - Traditional syslog
- `file` - Write to log file
- `console` - Print to stdout/stderr

### `log_path`
**Type:** string
**Default:** `""`
**Description:** Path for log file when `log_dest` is `"file"`. Empty uses default location:
- `/var/log/helix-screen.log` (if writable)
- `~/.local/share/helix-screen/helix.log` (fallback)

### `log_level`
**Type:** string
**Default:** `"warn"`
**Values:** `"warn"`, `"info"`, `"debug"`, `"trace"`
**Description:** Log verbosity level:
- `warn` - Quiet, only warnings and errors (default)
- `info` - General operational information
- `debug` - Detailed debugging information
- `trace` - Extremely verbose, all internal operations

**Note:** CLI `-v` flags override this setting (`-v`=info, `-vv`=debug, `-vvv`=trace).

---

## Display Settings

Located in the `display` section:

```json
{
  "display": {
    "animations_enabled": true,
    "time_format": 0,
    "rotate": 0,
    "sleep_sec": 1800,
    "dim_sec": 300,
    "dim_brightness": 30,
    "drm_device": "",
    "gcode_render_mode": 2,
    "gcode_3d_enabled": true,
    "bed_mesh_render_mode": 0,
    "bed_mesh_show_zero_plane": true,
    "printer_image": "",
    "calibration": {
      "valid": false
    }
  }
}
```

### `animations_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Enable UI animations and transitions. Disable for better performance on slow devices.

### `time_format`
**Type:** integer
**Default:** `0`
**Values:** `0` (12-hour), `1` (24-hour)
**Description:** Time display format. `0` shows "2:30 PM", `1` shows "14:30".

### `rotate`
**Type:** integer
**Default:** `0`
**Values:** `0`, `90`, `180`, `270`
**Description:** Rotate the entire display by the specified degrees. Touch coordinates are automatically adjusted to match. On first boot, HelixScreen detects the correct rotation automatically — this setting is saved by the detection wizard and can be changed manually if needed.

### `rotation_probed`
**Type:** boolean
**Default:** `false`
**Description:** Set to `true` after the automatic rotation detection wizard runs. Remove this key (along with `rotate`) to re-trigger automatic detection on next startup.

### `sleep_sec`
**Type:** integer
**Default:** `1800`
**Description:** Seconds of inactivity before screen turns OFF. Set to `0` to disable sleep. Default is 30 minutes.

### `dim_sec`
**Type:** integer
**Default:** `300`
**Description:** Seconds of inactivity before screen dims. Set to `0` to disable dimming. Must be less than `sleep_sec`. Default is 5 minutes.

### `dim_brightness`
**Type:** integer
**Default:** `30`
**Range:** `1` - `100`
**Description:** Brightness percentage when screen is dimmed.

### `drm_device`
**Type:** string
**Default:** `""` (auto-detect)
**Example:** `"/dev/dri/card1"`
**Description:** Override DRM device for display output. Leave empty for auto-detection.

**Pi 5 DRM devices:**
- `/dev/dri/card0` - v3d (3D only, no display output)
- `/dev/dri/card1` - DSI touchscreen
- `/dev/dri/card2` - HDMI (vc4)

Auto-detection finds the first device with dumb buffer support and a connected display.

### `gcode_render_mode`
**Type:** integer
**Default:** `2`
**Values:** `0` (Auto/2D), `1` (3D TinyGL), `2` (2D Layer)
**Description:** G-code visualization mode:
- `0` - Auto (currently uses 2D)
- `1` - 3D TinyGL (development only, ~3 FPS)
- `2` - 2D Layer view (default, recommended)

Can also be overridden via `HELIX_GCODE_MODE` env var (`3D` or `2D`).

### `gcode_3d_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Enable 3D G-code preview capability. When `false`, only 2D layer view is available.

### `bed_mesh_render_mode`
**Type:** integer
**Default:** `0`
**Values:** `0` (3D surface), `1` (2D heatmap)
**Description:** Bed mesh visualization mode. 3D surface shows the mesh as a 3D plot, 2D heatmap shows it as a flat color grid.

### `bed_mesh_show_zero_plane`
**Type:** boolean
**Default:** `true`
**Description:** Show translucent reference plane at Z=0 in bed mesh 3D view. Helps visualize where the nozzle touches the bed.

### `printer_image`
**Type:** string
**Default:** `""` (auto-detect)
**Description:** Printer image displayed on the Home Panel and in the Printer Manager. The value determines which image is used:
- `""` (empty string or absent) — **Auto-detect**: HelixScreen selects an image based on the printer type reported by Klipper
- `"shipped:voron-24r2"` — Use a specific shipped image by name (see `assets/images/printers/` for available images)
- `"custom:my-printer"` — Use a custom image that was imported from `config/custom_images/`

Custom images are PNG or JPEG files placed in `config/custom_images/`. They are automatically converted to optimized LVGL binary format (300px and 150px variants) when the Printer Image picker overlay is opened. Maximum file size is 5MB, maximum resolution is 2048x2048 pixels.

This setting can also be changed via the Printer Manager overlay (tap the printer image on the Home Panel).

### `calibration`
**Type:** object
**Default:** `{"valid": false}`
**Description:** Touch calibration coefficients. Set by the calibration wizard or manually. Contains calibration matrix values (`a` through `f`) when valid. If the wizard auto-detects that the touchscreen's X/Y axes are swapped relative to the display, it also saves `"swap_axes": true` — this is applied automatically on startup.

---

## Input Settings

Located in the `input` section:

```json
{
  "input": {
    "scroll_throw": 25,
    "scroll_limit": 10,
    "jitter_threshold": 15,
    "touch_device": "",
    "force_calibration": false
  }
}
```

### `scroll_throw`
**Type:** integer
**Default:** `25`
**Range:** `1` - `99`
**Description:** Scroll momentum decay rate. Higher values = faster decay (less "throw"). Default LVGL is 10; we use 25 for better touchscreen feel.

### `scroll_limit`
**Type:** integer
**Default:** `10`
**Range:** `1` - `50`
**Description:** Pixels of movement required before scrolling starts. Lower = more responsive. Matches LVGL's default of 10.

### `touch_device`
**Type:** string
**Default:** `""` (auto-detect)
**Example:** `"/dev/input/event1"`
**Description:** Override touch/pointer input device. Leave empty for auto-detection. Auto-detection finds touch or pointer capable devices.

### `jitter_threshold`
**Type:** integer
**Default:** `15`
**Range:** `0` - `200`
**Description:** Touch jitter filter dead zone in pixels. Suppresses small coordinate jitter from noisy touch controllers (e.g., Goodix GT9xx) that would cause taps to be misread as swipes. Set to `0` to disable. Can also be overridden with the `HELIX_TOUCH_JITTER` environment variable.

### `force_calibration`
**Type:** boolean
**Default:** `false`
**Description:** Force touch calibration on next startup, even if the device doesn't normally require it. After successful calibration, this flag is automatically cleared. Useful when touch input is inaccurate but HelixScreen doesn't show the calibration option in Settings.

---

## Output Settings

Located in the `output` section:

```json
{
  "output": {
    "led_on_at_start": false
  }
}
```

### `led_on_at_start`
**Type:** boolean
**Default:** `false`
**Description:** Automatically turn on the configured LED strip when Klipper becomes ready. Useful for printers with chamber lights that should always be on. **Deprecated:** This setting has moved to `printer.leds.led_on_at_start`. The legacy location is still read for backward compatibility.

---

## Network Settings

Located in the `network` section:

```json
{
  "network": {
    "connection_type": "None",
    "wifi_ssid": "",
    "eth_ip": ""
  }
}
```

### `connection_type`
**Type:** string
**Default:** `"None"`
**Values:** `"None"`, `"wifi"`, `"ethernet"`
**Description:** Current network connection type.

### `wifi_ssid`
**Type:** string
**Default:** `""`
**Description:** Connected WiFi network SSID.

### `eth_ip`
**Type:** string
**Default:** `""`
**Description:** Ethernet IP address (when connected).

---

## Printer Settings

Located in the `printer` section:

```json
{
  "printer": {
    "name": "Unnamed Printer",
    "type": "Unknown",
    "moonraker_host": "192.168.1.112",
    "moonraker_port": 7125,
    "moonraker_api_key": false,
    "heaters": {
      "bed": "heater_bed",
      "hotend": "extruder"
    },
    "temp_sensors": {
      "bed": "temperature_sensor bed",
      "hotend": "temperature_sensor extruder"
    },
    "fans": {
      "hotend": "heater_fan hotend_fan",
      "part": "fan",
      "chamber": "",
      "exhaust": ""
    },
    "leds": {
      "strip": "",
      "selected_strips": [],
      "led_on_at_start": false,
      "last_color": 16777215,
      "last_brightness": 100,
      "color_presets": [16777215, 16711680, 65280, 255, 16776960, 16711935, 65535],
      "auto_state": { ... },
      "macro_devices": []
    },
    "extra_sensors": {},
    "hardware": {
      "optional": [],
      "expected": [],
      "last_snapshot": {}
    },
    "default_macros": { ... },
    "safety_limits": { ... },
    "capability_overrides": { ... }
  }
}
```

> **Breaking Change (Jan 2026):** The config schema changed from singular keys (`heater`, `sensor`, `fan`, `led`) to plural keys (`heaters`, `temp_sensors`, `fans`, `leds`). If upgrading from an older version, delete your config file and re-run the first-run wizard.

### `name`
**Type:** string
**Default:** `"Unnamed Printer"`
**Description:** Display name for your printer.

### `type`
**Type:** string
**Default:** `"Unknown"`
**Description:** Printer model/type for feature detection (e.g., "Voron 2.4", "AD5M", "K1").

### `heaters.hotend`
**Type:** string
**Default:** `"extruder"`
**Description:** Klipper heater name for hotend.

### `heaters.bed`
**Type:** string
**Default:** `"heater_bed"`
**Description:** Klipper heater name for heated bed.

### `temp_sensors.hotend`
**Type:** string
**Description:** Temperature sensor for hotend (may differ from heater name if using separate sensor).

### `temp_sensors.bed`
**Type:** string
**Description:** Temperature sensor for bed (may differ from heater name if using separate sensor).

### `fans.part`
**Type:** string
**Default:** `"fan"`
**Description:** Klipper fan name for part cooling.

### `fans.hotend`
**Type:** string
**Description:** Klipper fan name for hotend cooling.

### `fans.chamber`
**Type:** string
**Default:** `""` (none)
**Description:** Klipper fan name for chamber fan (e.g., `"fan_generic chamber_fan"`). Leave empty if not available.

### `fans.exhaust`
**Type:** string
**Default:** `""` (none)
**Description:** Klipper fan name for exhaust fan (e.g., `"fan_generic exhaust_fan"`). Leave empty if not available.

---

## LED Settings

Located in the `printer.leds` section. Configured via **Settings > LED Settings**.

### `leds.strip`
**Type:** string
**Default:** `""` (empty)
**Description:** Legacy single LED strip name. Empty string if no controllable LEDs. Superseded by `leds.selected_strips` for multi-strip control.

### `leds.selected_strips`
**Type:** array of strings
**Default:** `[]`
**Description:** Klipper LED strip IDs to control (e.g., `["neopixel caselight", "dotstar toolhead"]`). Supports neopixel, dotstar, led, and WLED strips. Configured via **Settings > LED Settings**.

### `leds.led_on_at_start`
**Type:** boolean
**Default:** `false`
**Description:** Automatically turn on selected LED strips when Klipper becomes ready. Useful for chamber lights that should always be on. This setting has moved from `output.led_on_at_start` to here, though the legacy location is still read for backward compatibility.

### `leds.last_color`
**Type:** string (or integer)
**Default:** `"#FFFFFF"` (white)
**Description:** Last used LED color as a `#RRGGBB` hex string (e.g., `"#FFFFFF"` = white, `"#FF0000"` = red, `"#00FF00"` = green). Legacy integer RGB values (e.g., `16777215`) are also accepted for backward compatibility. Remembered between sessions.

### `leds.last_brightness`
**Type:** integer
**Default:** `100`
**Range:** `0` - `100`
**Description:** Last used brightness percentage. Remembered between sessions.

### `leds.color_presets`
**Type:** array of strings (or integers)
**Default:** `["#FFFFFF", "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"]`
**Description:** Preset colors shown in the color picker as `#RRGGBB` hex strings. Legacy integer RGB values are also accepted. Default presets are white, red, green, blue, yellow, magenta, and cyan.

### `leds.auto_state`
**Type:** object
**Description:** Automatic state-based LED lighting configuration. When enabled, LEDs change automatically based on printer state.

```json
{
  "auto_state": {
    "enabled": false,
    "mappings": {
      "idle": { "action": "brightness", "brightness": 50, "color": "#000000" },
      "heating": { "action": "color", "color": "#FF0000", "brightness": 100 },
      "printing": { "action": "brightness", "brightness": 100, "color": "#000000" },
      "paused": { "action": "effect", "effect_name": "breathing", "color": "#000000", "brightness": 100 },
      "error": { "action": "color", "color": "#FF0000", "brightness": 100 },
      "complete": { "action": "color", "color": "#00FF00", "brightness": 100 }
    }
  }
}
```

- `enabled` — Boolean, enable/disable automatic state-based lighting
- `mappings` — Object mapping printer state keys (`idle`, `heating`, `printing`, `paused`, `error`, `complete`) to actions
- Each mapping has an `action` type: `"off"`, `"brightness"`, `"color"`, `"effect"`, `"wled_preset"`, or `"macro"`
- Additional fields depend on the action: `brightness` (0-100), `color` (`#RRGGBB` hex string or legacy integer RGB), `effect_name` (string), `wled_preset` (integer), `macro` (string)

### `leds.macro_devices`
**Type:** array of objects
**Default:** `[]`
**Description:** Custom LED macro devices shown as cards in the LED control overlay. Each device object:

```json
{
  "name": "Chamber Light",
  "type": "on_off",
  "on_macro": "LIGHTS_ON",
  "off_macro": "LIGHTS_OFF",
  "toggle_macro": "",
  "presets": []
}
```

- `name` — Display name for the device card
- `type` — Device type: `"on_off"` (separate on/off macros), `"toggle"` (single toggle macro), or `"preset"` (multiple named presets)
- `on_macro` / `off_macro` — Macro names for on/off type
- `toggle_macro` — Macro name for toggle type
- `presets` — Array of `{"name": "...", "macro": "..."}` objects for preset type

Configured via **Settings > LED Settings > Macro Devices**.

### `extra_sensors`
**Type:** object
**Default:** `{}`
**Description:** Additional temperature sensors to monitor (beyond hotend/bed). Keys are display names, values are Klipper sensor names.

### `hardware`
**Type:** object
**Description:** Hardware tracking information (managed automatically by the wizard):
- `optional` - List of optional hardware detected
- `expected` - List of expected hardware based on printer type
- `last_snapshot` - Last hardware state snapshot for change detection

### `default_macros`
**Type:** object
**Description:** Custom macros for quick actions:

```json
{
  "default_macros": {
    "cooldown": "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0",
    "load_filament": { "label": "Load", "gcode": "LOAD_FILAMENT" },
    "unload_filament": { "label": "Unload", "gcode": "UNLOAD_FILAMENT" },
    "macro_1": { "label": "Clean Nozzle", "gcode": "HELIX_CLEAN_NOZZLE" },
    "macro_2": { "label": "Bed Level", "gcode": "HELIX_BED_LEVEL_IF_NEEDED" }
  }
}
```

- `cooldown` - G-code string for the cooldown button
- `load_filament` / `unload_filament` - Object with `label` and `gcode` for filament operations
- `macro_1` / `macro_2` - Custom macro buttons with `label` and `gcode`

---

## Moonraker Settings

Connection settings are in the `printer` section:

```json
{
  "printer": {
    "moonraker_host": "192.168.1.112",
    "moonraker_port": 7125,
    "moonraker_api_key": false,
    "moonraker_connection_timeout_ms": 10000,
    "moonraker_request_timeout_ms": 30000,
    "moonraker_keepalive_interval_ms": 10000,
    "moonraker_reconnect_min_delay_ms": 200,
    "moonraker_reconnect_max_delay_ms": 2000,
    "moonraker_timeout_check_interval_ms": 2000
  }
}
```

### `moonraker_host`
**Type:** string
**Default:** `"192.168.1.112"` (template default, usually `"localhost"`)
**Description:** Moonraker hostname or IP address.

### `moonraker_port`
**Type:** integer
**Default:** `7125`
**Description:** Moonraker port number.

### `moonraker_api_key`
**Type:** string or false
**Default:** `false`
**Description:** API key if Moonraker authentication is enabled. Set to `false` if no authentication.

### `moonraker_connection_timeout_ms`
**Type:** integer
**Default:** `10000`
**Description:** Connection timeout in milliseconds.

### `moonraker_request_timeout_ms`
**Type:** integer
**Default:** `30000`
**Description:** Request timeout for Moonraker API calls.

### `moonraker_keepalive_interval_ms`
**Type:** integer
**Default:** `10000`
**Description:** Interval for WebSocket keepalive pings.

### `moonraker_reconnect_min_delay_ms`
**Type:** integer
**Default:** `200`
**Description:** Minimum delay before reconnection attempt.

### `moonraker_reconnect_max_delay_ms`
**Type:** integer
**Default:** `2000`
**Description:** Maximum delay before reconnection attempt (exponential backoff cap).

### `moonraker_timeout_check_interval_ms`
**Type:** integer
**Default:** `2000`
**Description:** Interval for checking request timeouts.

---

## G-code Viewer Settings

Located in the `gcode_viewer` section:

```json
{
  "gcode_viewer": {
    "shading_model": "smooth",
    "tube_sides": 4,
    "streaming_mode": "auto",
    "streaming_threshold_percent": 40,
    "layers_per_frame": 0,
    "adaptive_layer_target_ms": 16
  }
}
```

### `shading_model`
**Type:** string
**Default:** `"smooth"`
**Values:** `"flat"`, `"smooth"`, `"phong"`
**Description:** 3D rendering quality:
- `flat` - Faceted look, lowest GPU cost
- `smooth` - Gouraud shading, good balance (default)
- `phong` - Per-pixel lighting, highest quality

### `tube_sides`
**Type:** integer
**Default:** `4`
**Values:** `4`, `8`, `16`
**Description:** Cross-section detail for filament paths:
- `4` - Diamond shape, fastest rendering
- `8` - Octagonal, balanced quality
- `16` - Circular, matches OrcaSlicer quality

### `streaming_mode`
**Type:** string
**Default:** `"auto"`
**Values:** `"auto"`, `"on"`, `"off"`
**Description:** Large G-code file handling:
- `auto` - Stream files that would use too much RAM
- `on` - Always stream (lowest memory)
- `off` - Always load full file (fastest viewing)

Can be overridden via `HELIX_GCODE_STREAMING` env var.

### `streaming_threshold_percent`
**Type:** integer
**Default:** `40`
**Range:** `1` - `90`
**Description:** Percent of available RAM that triggers streaming mode. Lower values stream smaller files. Only used when `streaming_mode` is `"auto"`.

### `layers_per_frame`
**Type:** integer
**Default:** `0` (auto)
**Range:** `0` - `100`
**Description:** Number of layers to render per frame during progressive 2D visualization:
- `0` - Auto (adaptive based on render time, default)
- `1-100` - Fixed value

Higher values = faster caching, but may cause UI stutter on slow devices.

### `adaptive_layer_target_ms`
**Type:** integer
**Default:** `16`
**Description:** Target render time in milliseconds when using adaptive `layers_per_frame` (only used when `layers_per_frame=0`). Lower = smoother UI, higher = faster caching. Default 16ms targets ~60 FPS.

---

## AMS Settings

Located in the `ams` section:

```json
{
  "ams": {
    "spool_style": "3d"
  }
}
```

### `spool_style`
**Type:** string
**Default:** `"3d"`
**Values:** `"3d"`, `"flat"`
**Description:** Filament spool visualization style:
- `3d` - Bambu-style pseudo-3D canvas with gradients
- `flat` - Simple concentric rings

---

## Panel Widget Settings

Located under the `panel_widgets` key, grouped by panel ID:

```json
{
  "panel_widgets": {
    "home": [
      {"id": "power", "enabled": true},
      {"id": "network", "enabled": false},
      {"id": "firmware_restart", "enabled": false},
      {"id": "ams", "enabled": true},
      {"id": "temperature", "enabled": true},
      {"id": "temp_stack", "enabled": false, "config": {"display_mode": "stack"}},
      {"id": "led", "enabled": true},
      {"id": "humidity", "enabled": true},
      {"id": "width_sensor", "enabled": true},
      {"id": "probe", "enabled": true},
      {"id": "filament", "enabled": true},
      {"id": "fan_stack", "enabled": true, "config": {"display_mode": "stack"}},
      {"id": "thermistor", "enabled": false},
      {"id": "notifications", "enabled": true}
    ]
  }
}
```

> **Migration note:** If your config has the older `home_widgets` key, HelixScreen automatically migrates it to `panel_widgets.home` on first launch.

### `panel_widgets.home`
**Type:** array of objects
**Default:** See defaults in table below
**Description:** Controls which widgets appear on the Home Panel and in what order. Each object has:

- `id` — Widget identifier (see table below)
- `enabled` — Whether the widget is shown (`true`/`false`)
- `config` — (optional) Per-widget settings object. Currently used by `temp_stack` and `fan_stack` for display mode:
  - `display_mode` — `"stack"` (default) or `"carousel"`. Stack shows compact rows; carousel shows swipeable full-size pages. Toggle via long-press on the widget.

**Available widget IDs:**

| ID | Widget | Default | Hardware-Gated |
|----|--------|---------|---------------|
| `power` | Moonraker power device controls | Enabled | Yes (requires power devices) |
| `network` | WiFi/Ethernet status | Disabled | No |
| `firmware_restart` | Klipper firmware restart | Disabled | No |
| `ams` | Multi-material spool status | Enabled | Yes (requires AMS/MMU) |
| `temperature` | Nozzle temperature with heating animation | Enabled | No |
| `temp_stack` | Stacked nozzle, bed, and chamber temps (supports carousel mode) | Disabled | No |
| `led` | LED quick toggle | Enabled | Yes (requires LEDs) |
| `humidity` | Enclosure humidity sensor | Enabled | Yes (requires sensor) |
| `width_sensor` | Filament width sensor | Enabled | Yes (requires sensor) |
| `probe` | Z probe status and offset | Enabled | Yes (requires probe) |
| `filament` | Filament runout detection | Enabled | Yes (requires sensor) |
| `fan_stack` | Part, hotend, and auxiliary fan speeds (supports carousel mode with arc dials) | Enabled | No |
| `thermistor` | Custom temperature sensor (chamber, etc.) | Disabled | Yes (requires sensor) |
| `notifications` | Pending alerts with severity badge | Enabled | No |

**Notes:**
- The array order determines display order on the Home Panel
- Hardware-gated widgets are hidden on the Home Panel if their hardware isn't detected, even when enabled
- New widgets added in future versions are automatically appended with their default enabled state
- Unknown widget IDs (from older versions) are silently ignored

This is best configured via **Settings > Home Widgets** rather than editing the JSON directly.

---

## Cache Settings

Located in the `cache` section:

```json
{
  "cache": {
    "thumbnail_max_mb": 20,
    "disk_critical_mb": 5,
    "disk_low_mb": 20
  }
}
```

### `thumbnail_max_mb`
**Type:** integer
**Default:** `20`
**Description:** Maximum thumbnail cache size in MB. Cache auto-sizes to 5% of available disk, capped at this limit.

### `disk_critical_mb`
**Type:** integer
**Default:** `5`
**Description:** Stop caching when available disk falls below this threshold (MB). Prevents filling filesystem.

### `disk_low_mb`
**Type:** integer
**Default:** `20`
**Description:** Evict cache aggressively when available disk falls below this threshold (MB). Reduces cache to half normal limit.

---

## Streaming Settings

Located in the `streaming` section:

```json
{
  "streaming": {
    "threshold_mb": 0,
    "force_streaming": false
  }
}
```

### `threshold_mb`
**Type:** integer
**Default:** `0` (auto-detect)
**Description:** File size threshold in MB for using streaming (disk-based) operations instead of buffered (in-memory). `0` = auto-detect based on 10% of available RAM.

Can be overridden via `HELIX_FORCE_STREAMING=1` env var to force streaming for all files.

### `force_streaming`
**Type:** boolean
**Default:** `false`
**Description:** Always use streaming operations regardless of file size. Useful for memory-constrained devices or testing. Can also be set via `HELIX_FORCE_STREAMING=1` env var.

---

## Safety Settings

Located in the `safety` section:

```json
{
  "safety": {
    "estop_require_confirmation": false,
    "cancel_escalation_enabled": false,
    "cancel_escalation_timeout_seconds": 30
  }
}
```

### `estop_require_confirmation`
**Type:** boolean
**Default:** `false`
**Description:** Require confirmation dialog before emergency stop. When `false`, E-Stop triggers immediately. Default is `false` for faster emergency response.

### `cancel_escalation_enabled`
**Type:** boolean
**Default:** `false`
**Description:** When enabled, a cancel that doesn't complete within the configured timeout will automatically escalate to an emergency stop (M112). When `false` (the default), cancel waits indefinitely for the printer to finish its cancel routine. Leave this off if your printer has a long cancel macro (e.g., toolchangers that need to park tools).

### `cancel_escalation_timeout_seconds`
**Type:** integer
**Default:** `30`
**Options:** `15`, `30`, `60`, `120`
**Description:** How long to wait (in seconds) after sending a cancel before escalating to emergency stop. Only applies when `cancel_escalation_enabled` is `true`.

---

## Filament Sensor Settings

Located in the `filament_sensors` section:

```json
{
  "filament_sensors": {
    "master_enabled": true,
    "sensors": []
  }
}
```

### `master_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Global toggle to enable/disable all filament sensor monitoring. When `false`, sensor states are ignored and no runout detection occurs.

### `sensors`
**Type:** array
**Default:** `[]`
**Description:** Array of sensor configurations. Sensors are auto-discovered from Moonraker. Each sensor object has:
- `klipper_name` - Full Klipper object name (e.g., `"filament_switch_sensor fsensor"`)
- `role` - Sensor role: `"none"`, `"runout"`, `"toolhead"`, `"entry"`
- `enabled` - Boolean to enable/disable individual sensor

**Example:**
```json
{
  "sensors": [
    {
      "klipper_name": "filament_switch_sensor fsensor",
      "role": "runout",
      "enabled": true
    }
  ]
}
```

---

## Plugin Settings

Located in the `plugins` section:

```json
{
  "plugins": {
    "enabled": []
  }
}
```

### `enabled`
**Type:** array
**Default:** `[]`
**Description:** List of plugin IDs to load. Plugins must be explicitly enabled.

**Example:**
```json
{
  "enabled": ["led-effects", "custom-macros"]
}
```

---

## Update Settings

Located in the `update` section:

```json
{
  "update": {
    "channel": 0,
    "dev_url": "",
    "r2_url": ""
  }
}
```

### `channel`
**Type:** integer
**Default:** `0`
**Values:** `0` (Stable), `1` (Beta), `2` (Dev)
**Description:** Update channel selection:
- `0` - **Stable**: Tries R2 CDN first (`{r2_url}/stable/manifest.json`), falls back to GitHub releases API
- `1` - **Beta**: Tries R2 CDN first (`{r2_url}/beta/manifest.json`), falls back to GitHub pre-releases API
- `2` - **Dev**: Uses `dev_url` if set (backward compat), otherwise uses R2 CDN (`{r2_url}/dev/manifest.json`)

Can also be changed from the Settings panel when `beta_features` is enabled.

### `dev_url`
**Type:** string
**Default:** `""` (empty)
**Example:** `"https://releases.helixscreen.org/dev"`
**Description:** Explicit base URL for the dev update channel. When set and `channel` is `2`, HelixScreen fetches `{dev_url}/manifest.json` directly, bypassing R2. When empty, the dev channel uses the R2 CDN path (`{r2_url}/dev/manifest.json`). Must use `http://` or `https://` scheme. Primarily used for local development servers or self-hosted setups that predate R2 support.

### `r2_url`
**Type:** string
**Default:** `""` (uses built-in `https://releases.helixscreen.org`)
**Example:** `"https://my-cdn.example.com"`
**Description:** Base URL for R2/CDN update manifests. All channels (Stable, Beta, Dev) fetch manifests from `{r2_url}/{channel}/manifest.json`. When empty, uses the compiled-in default (`https://releases.helixscreen.org`). Self-hosters can override this to point to their own CDN or R2 bucket. Trailing slashes are automatically stripped.

---

## Safety Limits

Located in `printer.safety_limits`:

```json
{
  "printer": {
    "safety_limits": {
      "max_temperature_celsius": 400.0,
      "min_temperature_celsius": 0.0,
      "max_fan_speed_percent": 100.0,
      "min_fan_speed_percent": 0.0,
      "max_feedrate_mm_min": 50000.0,
      "min_feedrate_mm_min": 0.0,
      "max_relative_distance_mm": 1000.0,
      "min_relative_distance_mm": -1000.0,
      "max_absolute_position_mm": 1000.0,
      "min_absolute_position_mm": 0.0
    }
  }
}
```

These override auto-detected limits. Useful for:
- High-temp printers (increase `max_temperature_celsius`)
- Very large printers (increase position limits)
- Safety restrictions (decrease maximums)

Leave unset (or remove the section) to use Moonraker auto-detection from printer.cfg.

---

## Capability Overrides

Located in `printer.capability_overrides`:

```json
{
  "printer": {
    "capability_overrides": {
      "bed_mesh": "auto",
      "qgl": "auto",
      "z_tilt": "auto",
      "nozzle_clean": "auto",
      "heat_soak": "auto",
      "chamber": "auto"
    }
  }
}
```

**Values for each setting:**
- `"auto"` - Use Moonraker detection
- `"enable"` - Force feature on
- `"disable"` - Force feature off

**Use cases:**
- Enable `heat_soak` when you have a chamber but no chamber heater (soak macro works without)
- Disable `qgl` on a printer where it's defined but not used
- Enable `bed_mesh` if detection failed

---

## Resetting Configuration

### Full Reset
Delete the config file and restart (use your actual install path):
```bash
# Pi with Klipper ecosystem:
rm ~/helixscreen/config/helixconfig.json
# Pi without ecosystem (or if installed to /opt):
sudo rm /opt/helixscreen/config/helixconfig.json

sudo systemctl restart helixscreen
```

This triggers the first-run wizard.

### Partial Reset
Edit the config file directly:
```bash
nano ~/helixscreen/config/helixconfig.json
```

Or copy fresh from template:
```bash
cp ~/helixscreen/config/helixconfig.json.template ~/helixscreen/config/helixconfig.json
```

---

## Command-Line Options

HelixScreen accepts command-line options for overriding configuration and debugging.

### Display Options

| Option | Description |
|--------|-------------|
| `-s, --size <size>` | Screen size: `tiny` (480×320), `small` (800×480), `medium` (1024×600), `large` (1280×720) |
| `--dpi <n>` | Display DPI (50-500, default: 160) |
| `--dark` | Use dark theme |
| `--light` | Use light theme |
| `--skip-splash` | Skip splash screen on startup |

### Navigation Options

| Option | Description |
|--------|-------------|
| `-p, --panel <panel>` | Start on specific panel (home, controls, filament, settings, advanced, print-select) |
| `-w, --wizard` | Force first-run configuration wizard |

### Connection Options

| Option | Description |
|--------|-------------|
| `--moonraker <url>` | Override Moonraker URL (e.g., `ws://192.168.1.112:7125`) |

### Logging Options

| Option | Description |
|--------|-------------|
| `-v, --verbose` | Increase verbosity (`-v`=info, `-vv`=debug, `-vvv`=trace) |
| `--log-dest <dest>` | Log destination: `auto`, `journal`, `syslog`, `file`, `console` |
| `--log-file <path>` | Log file path (when `--log-dest=file`) |

### Debugging Options

| Option | Description |
|--------|-------------|
| `--debug-touches` | Draw ripple effects at each touch point for diagnosing touch accuracy |
| `--calibrate-touch` | Force touch calibration on startup |

### Utility Options

| Option | Description |
|--------|-------------|
| `--screenshot [sec]` | Take screenshot after delay (default: 2 seconds) |
| `-t, --timeout <sec>` | Auto-quit after specified seconds (1-3600) |
| `-h, --help` | Show help message |
| `-V, --version` | Show version information |

### Examples

```bash
# Start in dark mode on the settings panel
helix-screen --dark --panel settings

# Override Moonraker connection
helix-screen --moonraker ws://192.168.1.50:7125

# Enable debug logging
helix-screen -vv

# Take screenshot after 5 seconds
helix-screen --screenshot 5
```

> **Note:** Test mode options (`--test`, `--real-*`) are for development only and not documented here.

---

## Environment Variables

These can be set in the systemd service file or before running the binary:

| Variable | Description |
|----------|-------------|
| `HELIX_DRM_DEVICE` | Override DRM device path (e.g., `/dev/dri/card1`) |
| `HELIX_TOUCH_DEVICE` | Override touch input device (e.g., `/dev/input/event1`) |
| `HELIX_DISPLAY_BACKEND` | Override display backend (`drm`, `fbdev`, `sdl`) |
| `HELIX_GCODE_MODE` | Override G-code render mode (`3D` or `2D`) |
| `HELIX_GCODE_STREAMING` | Override G-code streaming mode |
| `HELIX_FORCE_STREAMING` | Force streaming for all file operations (`1` to enable) |

**Example in service file:**
```ini
[Service]
Environment="HELIX_DRM_DEVICE=/dev/dri/card1"
Environment="HELIX_TOUCH_DEVICE=/dev/input/event0"
```

> **Note:** Most users won't need environment variables. The config file options are preferred. Environment variables are mainly for debugging when the config file isn't accessible.
>
> For a comprehensive list of all environment variables (including mock/testing, touch calibration, UI automation, and more), see the [Environment Variables Reference](../devel/ENVIRONMENT_VARIABLES.md).

---

## Example Complete Configuration

```json
{
  "dark_mode": true,
  "brightness": 70,
  "sounds_enabled": true,
  "completion_alert": 2,
  "wizard_completed": true,
  "wifi_expected": true,
  "language": "en",
  "log_dest": "journal",
  "log_path": "",
  "log_level": "warn",

  "theme": {
    "preset": 0
  },

  "display": {
    "animations_enabled": true,
    "time_format": 0,
    "rotate": 0,
    "sleep_sec": 1800,
    "dim_sec": 300,
    "dim_brightness": 30,
    "drm_device": "",
    "gcode_render_mode": 2,
    "gcode_3d_enabled": true,
    "bed_mesh_render_mode": 0,
    "bed_mesh_show_zero_plane": true,
    "printer_image": "",
    "calibration": {
      "valid": false,
      "swap_axes": false
    }
  },

  "input": {
    "scroll_throw": 25,
    "scroll_limit": 10,
    "touch_device": "",
    "force_calibration": false
  },

  "output": {
    "led_on_at_start": false
  },

  "network": {
    "connection_type": "wifi",
    "wifi_ssid": "PrinterNetwork",
    "eth_ip": ""
  },

  "printer": {
    "name": "Voron 2.4 350",
    "type": "Voron 2.4",
    "moonraker_host": "localhost",
    "moonraker_port": 7125,
    "moonraker_api_key": false,
    "moonraker_connection_timeout_ms": 10000,
    "moonraker_request_timeout_ms": 30000,
    "moonraker_keepalive_interval_ms": 10000,
    "moonraker_reconnect_min_delay_ms": 200,
    "moonraker_reconnect_max_delay_ms": 2000,
    "moonraker_timeout_check_interval_ms": 2000,
    "heaters": {
      "hotend": "extruder",
      "bed": "heater_bed"
    },
    "temp_sensors": {
      "hotend": "extruder",
      "bed": "heater_bed"
    },
    "fans": {
      "part": "fan",
      "hotend": "heater_fan hotend_fan",
      "chamber": "",
      "exhaust": ""
    },
    "leds": {
      "strip": "",
      "selected_strips": ["neopixel caselight"],
      "led_on_at_start": false,
      "last_color": 16777215,
      "last_brightness": 100,
      "color_presets": [16777215, 16711680, 65280, 255, 16776960, 16711935, 65535],
      "auto_state": {
        "enabled": false,
        "mappings": {
          "idle": { "action": "brightness", "brightness": 50, "color": 0 },
          "heating": { "action": "color", "color": 16711680, "brightness": 100 },
          "printing": { "action": "brightness", "brightness": 100, "color": 0 },
          "paused": { "action": "off" },
          "error": { "action": "color", "color": 16711680, "brightness": 100 },
          "complete": { "action": "color", "color": 65280, "brightness": 100 }
        }
      },
      "macro_devices": []
    },
    "extra_sensors": {},
    "hardware": {
      "optional": [],
      "expected": [],
      "last_snapshot": {}
    },
    "default_macros": {
      "cooldown": "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0",
      "load_filament": { "label": "Load", "gcode": "LOAD_FILAMENT" },
      "unload_filament": { "label": "Unload", "gcode": "UNLOAD_FILAMENT" },
      "macro_1": { "label": "Clean Nozzle", "gcode": "HELIX_CLEAN_NOZZLE" },
      "macro_2": { "label": "Bed Level", "gcode": "HELIX_BED_LEVEL_IF_NEEDED" }
    },
    "safety_limits": {
      "max_temperature_celsius": 400.0,
      "min_temperature_celsius": 0.0,
      "max_fan_speed_percent": 100.0,
      "min_fan_speed_percent": 0.0,
      "max_feedrate_mm_min": 50000.0,
      "min_feedrate_mm_min": 0.0,
      "max_relative_distance_mm": 1000.0,
      "min_relative_distance_mm": -1000.0,
      "max_absolute_position_mm": 1000.0,
      "min_absolute_position_mm": 0.0
    },
    "capability_overrides": {
      "bed_mesh": "auto",
      "qgl": "auto",
      "z_tilt": "auto",
      "nozzle_clean": "auto",
      "heat_soak": "auto",
      "chamber": "auto"
    }
  },

  "gcode_viewer": {
    "shading_model": "smooth",
    "tube_sides": 8,
    "streaming_mode": "auto",
    "streaming_threshold_percent": 40,
    "layers_per_frame": 0,
    "adaptive_layer_target_ms": 16
  },

  "panel_widgets": {
    "home": [
      {"id": "temperature", "enabled": true},
      {"id": "network", "enabled": true},
      {"id": "led", "enabled": true},
      {"id": "ams", "enabled": true},
      {"id": "notifications", "enabled": true},
      {"id": "power", "enabled": true},
      {"id": "firmware_restart", "enabled": false},
      {"id": "humidity", "enabled": false},
      {"id": "width_sensor", "enabled": false},
      {"id": "probe", "enabled": false},
      {"id": "filament", "enabled": false},
      {"id": "temp_stack", "enabled": false},
      {"id": "fan_stack", "enabled": true},
      {"id": "thermistor", "enabled": false}
    ]
  },

  "ams": {
    "spool_style": "3d"
  },

  "cache": {
    "thumbnail_max_mb": 20,
    "disk_critical_mb": 5,
    "disk_low_mb": 20
  },

  "streaming": {
    "threshold_mb": 0,
    "force_streaming": false
  },

  "safety": {
    "estop_require_confirmation": false
  },

  "filament_sensors": {
    "master_enabled": true,
    "sensors": []
  },

  "plugins": {
    "enabled": []
  },

  "update": {
    "channel": 0,
    "dev_url": ""
  }
}
```

---

*Back to: [User Guide](USER_GUIDE.md) | [Troubleshooting](TROUBLESHOOTING.md)*

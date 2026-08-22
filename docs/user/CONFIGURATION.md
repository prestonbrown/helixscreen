# Configuration Reference

Complete reference for HelixScreen configuration options.

---

## Table of Contents

- [Configuration File Location](#configuration-file-location)
- [Configuration Structure](#configuration-structure)
- [Multi-Printer Configuration](#multi-printer-configuration)
- [General Settings](#general-settings)
- [Sound Settings](#sound-settings)
- [Theme Settings](#theme-settings)
- [Logging Settings](#logging-settings)
- [Display Settings](#display-settings)
- [Appearance Settings](#appearance-settings)
- [Input Settings](#input-settings)
- [Output Settings](#output-settings)
- [Network Settings](#network-settings)
- [Printer Settings](#printer-settings)
- [LED Settings](#led-settings)
- [Moonraker Settings](#moonraker-settings)
- [Standard Macros (Quick Action Buttons)](#standard-macros-quick-action-buttons)
- [G-code Viewer Settings](#g-code-viewer-settings)
- [AMS Settings](#ams-settings)
- [Panel Widget Settings](#panel-widget-settings)
- [Cache Settings](#cache-settings)
- [Streaming Settings](#streaming-settings)
- [Safety Settings](#safety-settings)
- [Notification Settings](#notification-settings)
- [Filament Settings](#filament-settings)
- [Filament Sensor Settings](#filament-sensor-settings)
- [Security Settings](#security-settings)
- [Label Printer Settings](#label-printer-settings)
- [Printer Switcher](#printer-switcher)
- [Telemetry Settings](#telemetry-settings)
- [Plugin Settings](#plugin-settings)
- [Update Settings](#update-settings)
- [Upgrade Nudge](#upgrade-nudge)
- [Safety Limits](#safety-limits)
- [Capability Overrides](#capability-overrides)
- [Resetting Configuration](#resetting-configuration)
- [Command-Line Options](#command-line-options)
- [Environment Variables](#environment-variables)

---

## Configuration File Location

| Platform | Location |
|----------|----------|
| MainsailOS (Pi) | `~/helixscreen/config/settings.json` (or `/opt/helixscreen/config/` if no Klipper ecosystem) |
| AD5M Forge-X | `/opt/helixscreen/config/settings.json` |
| AD5M Klipper Mod | `/root/printer_software/helixscreen/config/settings.json` |
| K1 Simple AF | `/usr/data/helixscreen/config/settings.json` |
| Development | `./config/settings.json` (in config/ directory) |

> **Note:** On Pi, the installer auto-detects your Klipper ecosystem. If `~/klipper`, `~/moonraker`, or `~/printer_data` exists, HelixScreen installs to `~/helixscreen`. Otherwise it falls back to `/opt/helixscreen`. You can override with `INSTALL_DIR=/path ./install.sh`.

The configuration file is created automatically by the first-run wizard. You can also copy from the template:

```bash
cp config/settings.json.template config/settings.json
```

**Note:** A `settings.json` sitting in the app root or at `/opt/helixscreen/settings.json` is moved to the location above on startup.

---

## Configuration Structure

The configuration file is JSON format with several top-level sections:

```json
{
  "config_version": 22,
  "dark_mode": false,
  "brightness": 80,
  "sounds_enabled": true,
  "ui_sounds_enabled": true,
  "sound_theme": "default",
  "disable_sound": false,
  "completion_alert": 1,
  "wizard_completed": false,
  "wifi_expected": false,
  "language": "en",
  "beta_features": false,
  "telemetry_enabled": false,
  "log_dest": "auto",
  "log_path": "",
  "log_level": "warn",

  "panel_widgets": { ... },
  "theme": { ... },
  "sounds": { ... },
  "display": { ... },
  "appearance": { ... },
  "input": { ... },
  "output": { ... },
  "network": { ... },
  "printer": { ... },
  "standard_macros": { ... },
  "gcode_viewer": { ... },
  "ams": { ... },
  "cache": { ... },
  "streaming": { ... },
  "safety": { ... },
  "filament": { ... },
  "filament_sensors": { ... },
  "security": { ... },
  "label_printer": { ... },
  "printers": { ... },
  "plugins": { ... },
  "update": { ... }
}
```

---

## Multi-Printer Configuration

When multiple printers are configured, the config file uses a versioned schema with per-printer settings. `config_version` tracks the schema and HelixScreen migrates older files forward on load — leave it alone:

```json
{
  "config_version": 22,
  "active_printer_id": "voron-24",
  "printers": {
    "voron-24": {
      "printer_name": "Voron 2.4",
      "moonraker_host": "192.168.1.100",
      "moonraker_port": 7125,
      "wizard_completed": true,
      "printer_image": "shipped:voron-v2",
      ...per-printer settings...
    },
    "ender-3": {
      "printer_name": "Workshop Ender",
      "moonraker_host": "192.168.1.101",
      "moonraker_port": 7125,
      "wizard_completed": true,
      ...per-printer settings...
    }
  },
  "wifi": { ... },
  "display": { ... }
}
```

Each printer entry contains all printer-specific settings (connection details, hardware selections, LED config, filament sensors, etc.). Device-level settings like WiFi and display preferences remain at the root level and are shared across all printers.

> **Note:** You don't need to edit the config file manually — use the Settings > Hardware & Devices > Printers UI to add and manage printers. The config file is shown here for reference.

---

> **Looking for a walkthrough of each setting?** See the detailed guides:
> [Display & Sound](guide/settings/display-sound.md) · [Printing](guide/settings/printing.md) · [Hardware & Devices](guide/settings/hardware.md) · [Safety & Notifications](guide/settings/safety.md) · [System](guide/settings/system.md) · [LED Settings](guide/settings/led-settings.md) · [Help & About](guide/settings/help-about.md)

## General Settings

### `dark_mode`
**Type:** boolean
**Default:** `false`
**Description:** Use dark theme (`true`) or light theme (`false`). Can also be set via Settings panel or `--dark`/`--light` CLI flags.

### `brightness`
**Type:** integer
**Default:** `80`
**Range:** `1` - `100`
**Description:** Screen brightness percentage. Adjustable via Settings panel.

### `sounds_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Master switch for all sound effects. When `false`, no sounds play (UI or event). This is the "mute" toggle — it silences playback but still initializes the audio backend (see `disable_sound` to prevent initialization entirely).

### `ui_sounds_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Enable UI interaction sounds specifically (button taps, navigation clicks). Independent of event sounds like the print-complete chime, so you can keep alerts while silencing tap feedback. Has no effect when `sounds_enabled` is `false`. Adjustable via **Settings > Display & Sound**.

### `sound_theme`
**Type:** string
**Default:** `"default"`
**Values:** `"default"`, `"minimal"`
**Description:** The active sound theme, loaded from `config/sounds/<name>.json`. `"default"` uses the full set of tones; `"minimal"` uses a sparser, quieter set.

### `completion_alert`
**Type:** integer
**Default:** `1`
**Values:** `0` (Off), `1` (Notification), `2` (Alert)
**Description:** How HelixScreen notifies you when a print completes or is cancelled (while you're on a different screen):
- `0` — **Off**: No notification (sound still plays if sounds are enabled)
- `1` — **Notification**: Brief toast message at the top of the screen
- `2` — **Alert**: Full-screen modal with print stats (duration, layers, filament used) and confetti for successful prints

Errors always show the full alert regardless of this setting. To change this in the UI, go to **Settings > Safety & Notifications > Print Completion Alert** and select from the dropdown.

### `disable_sound`
**Type:** boolean
**Default:** `false`
**Description:** Disable all sound output entirely. Prevents the audio backend from initializing, which avoids CPU overhead on hardware where audio drivers are present but unusable (e.g., Artillery M1 Pro). Also available as the `--no-sound` CLI flag.

This is different from `sounds_enabled` — that toggle mutes playback but still initializes the audio backend. `disable_sound` prevents initialization altogether.

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
**Values:** `"en"`, `"de"`, `"es"`, `"fr"`, `"it"`, `"ja"`, `"pt"`, `"ru"`, `"zh"`
**Description:** UI language code. Nine languages are supported (English, German, Spanish, French, Italian, Japanese, Portuguese, Russian, Chinese). Change via Settings > Display & Sound > Language.

### `beta_features`
**Type:** boolean
**Default:** `false`
**Description:** Enable beta features that are still under testing. Gates several Advanced panel features (Macro Browser, Input Shaping, Z-Offset Calibration, HelixPrint plugin management, PRINT_START configuration, Timelapse), the Plugins section in Settings, and the Update Channel selector. Always enabled automatically when running in `--test` mode. Can also be toggled by tapping the version button 7 times in Settings → About. See the [Beta Features](guide/beta-features.md) guide for the full list.

---

## Sound Settings

Located in the `sounds` section:

```json
{
  "sounds": {
    "volume": 80
  }
}
```

### `sounds.volume`
**Type:** integer
**Default:** `80`
**Range:** `0` - `100`
**Description:** Master playback volume as a percentage. `0` is silent, `100` is full volume. Adjustable via **Settings > Display & Sound**. This scales the level of all sounds; the `sounds_enabled` and `ui_sounds_enabled` toggles decide *whether* sounds play at all.

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
**Description:** Theme accent color preset. **Requires restart to take effect.**

> **Note:** `preset` is a dropdown index, not the effective theme — index `0` maps to Ayu, while the out-of-the-box default is **Nord**. What actually selects the theme is the `/display/theme` string (a theme name). Set that; ignore this.

| Value | Theme |
|-------|-------|
| 0 | Ayu |
| 1 | Catppuccin |
| 2 | ChatGPT |
| 3 | Cupertino |
| 4 | Dracula |
| 5 | Everforest |
| 6 | Gruvbox |
| 7 | Hazard |
| 8 | HelixScreen |
| 9 | Kanagawa |
| 10 | Material Design |
| 11 | Midnight |
| 12 | Nord (default) |
| 13 | One Dark |
| 14 | Rose Pine |
| 15 | Solarized |
| 16 | Tokyo Night |
| 17 | Yami |

> **Tip:** You can also browse and apply themes visually in **Settings > Display & Sound > Theme Colors**.

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

**Note:** This can also be changed at runtime via **Settings > System > Log Level** without restarting. CLI `-v` flags override this setting (`-v`=info, `-vv`=debug, `-vvv`=trace).

---

## Display Settings

Located in the `display` section:

```json
{
  "display": {
    "animations_enabled": true,
    "time_format": 0,
    "timezone": "UTC",
    "theme": "nord",
    "rotate": 0,
    "sleep_sec": 1200,
    "sleep_while_printing": true,
    "dim_sec": 600,
    "dim_brightness": 30,
    "drm_device": "",
    "gcode_render_mode": 2,
    "gcode_3d_enabled": true,
    "bed_mesh_render_mode": 0,
    "bed_mesh_show_zero_plane": true,
    "page_scroll_buttons": false,
    "ui_scale_percent": 0,
    "printer_image": ""
  }
}
```

> **Touch calibration data lives under `input.calibration`, not `display.calibration`.** See the [Input Configuration](#input-settings) section below and the [Touch Calibration Guide](guide/touch-calibration.md). Older configs that placed it under `display` are automatically migrated on first load.

### `animations_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Enable UI animations and transitions. Disable for better performance on slow devices.

### `time_format`
**Type:** integer
**Default:** `0`
**Values:** `0` (12-hour), `1` (24-hour)
**Description:** Time display format. `0` shows "2:30 PM", `1` shows "14:30".

### `timezone`
**Type:** string
**Default:** `"UTC"`
**Example:** `"America/New_York"`, `"Europe/London"`
**Description:** IANA timezone ID used for all displayed clocks and print time estimates. Set this so times shown on screen match your local time instead of UTC. Change via **Settings > Display & Sound > Timezone**.

### `theme`
**Type:** string
**Default:** `"nord"`
**Description:** Active color theme by name (e.g., `"nord"`, `"dracula"`, `"gruvbox"`). This is the string that actually determines the effective theme — the numeric `theme.preset` index is a legacy field. **Requires restart to take effect.** Easiest to change via **Settings > Display & Sound > Theme Colors**, which writes this value for you.

### `layout`
**Type:** string
**Default:** `"auto"`
**Values:** `auto`, `standard`, `ultrawide`, `portrait`, `micro`, `micro-portrait`, `tiny`, `tiny-portrait`
**Description:** Override the auto-detected screen layout. Leave this at `auto` unless you are testing — HelixScreen picks the layout from your display's aspect ratio: wider than about 2.5:1 is `ultrawide`, narrower than about 0.8:1 is `portrait`, and anything in between is `standard`. Displays whose longest side is 480px or less get `micro` (480x272 class) or `tiny` instead, with `-portrait` variants when the screen is taller than wide.

> **Ultrawide and portrait are alpha at best.** Detection, navigation-bar sizing, and grid sizing all work, and both orientations get their own home dashboard layout. Portrait goes further: Print Status, Print Tune, Motion, Bed Mesh, the temperature graph, and the Advanced panel's E-stop bar rearrange for a tall screen as well. Ultrawide has the home dashboard and nothing else. Every other panel falls back to the standard landscape layout, so expect stretched, cramped, or clipped screens outside that set. Neither orientation has been tested much on real hardware. Forcing one of these is useful for contributing layouts, not for daily use.

### `rotate`
**Type:** integer
**Default:** `0`
**Values:** `0`, `90`, `180`, `270`
**Description:** Rotate the entire display by the specified degrees. Touch coordinates are automatically adjusted to match. Change via **Settings > Display & Sound > Screen Rotation** (applies after restart).

**Automatic detection:** On first boot, HelixScreen checks the kernel for panel orientation (e.g., `panel_orientation=upside_down` in the kernel command line). If detected, the rotation is applied immediately and saved here — no manual configuration needed. On framebuffer displays only (e.g., AD5M — **not** Raspberry Pi), an interactive rotation wizard runs instead if no kernel hint is found.

**Performance note (Raspberry Pi / DRM displays):** When rotation is active on DRM-based displays (Pi 4, Pi 5), HelixScreen uses a software rotation approach that redraws the full screen on every frame update instead of only the changed regions. This adds a small overhead (typically <1ms per frame on Pi 5) but is necessary because the LVGL DRM driver does not support hardware rotation. Framebuffer displays (e.g., AD5M) use a more efficient partial-update rotation with no meaningful performance impact.

### `rotation_probed`
**Type:** boolean
**Default:** `false`
**Description:** Set to `true` after automatic rotation detection runs. Remove this key (along with `rotate`) to re-trigger automatic detection on next startup.

### `sleep_sec`
**Type:** integer
**Default:** `1200`
**Description:** Seconds of inactivity before screen turns OFF. Set to `0` to disable sleep. Default is 20 minutes.

### `sleep_while_printing`
**Type:** boolean
**Default:** `true`
**Description:** Whether the screen is allowed to dim and sleep during an active print. When `true`, the normal `dim_sec`/`sleep_sec` timers apply while printing. Set to `false` to keep the display on for the whole print so you can glance at progress without touching the screen. Adjustable via **Settings > Display & Sound**.

### `dim_sec`
**Type:** integer
**Default:** `600`
**Description:** Seconds of inactivity before screen dims. Set to `0` to disable dimming. Must be less than `sleep_sec`. Default is 10 minutes.

### `dim_brightness`
**Type:** integer
**Default:** `30`
**Range:** `1` - `100`
**Description:** Brightness percentage when screen is dimmed.

### `sleep_backlight_off`
**Type:** boolean
**Default:** `true`
**Description:** Whether going to sleep switches the backlight off, rather than only dimming it to zero. Leave this on. It exists for panels where cutting the backlight leaves the screen in a state it cannot wake from; if your display goes dark and stays dark after sleeping, and a touch does not bring it back, set this to `false` and restart.

### `backlight_enable_ioctl`
**Type:** boolean
**Default:** `true`
**Description:** Whether to use the kernel's backlight enable/disable call in addition to setting a brightness level. A few panels wire this signal backwards, so enabling the backlight turns it off. If your screen is inverted — dark when it should be lit, lit when it should be asleep — set this to `false` so brightness alone controls it.

### `screensaver_type`
**Type:** integer
**Default:** `1`
**Values:** `0` = Off, `1` = Flying Toasters, `2` = Starfield, `3` = 3D Pipes
**Description:** Which screensaver plays when the screen has been idle. Choose it in **Settings > Display & Sound**. It starts at the `dim_sec` mark, alongside dimming, and stops when `sleep_sec` takes the display down. On a panel with no backlight control the screensaver is the only idle indication you get.

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
**Default:** `0`
**Values:** `0` (Auto), `1` (3D View), `2` (2D Layers), `3` (Thumbnail Only)
**Description:** G-code visualization mode for the active print:
- `0` - Auto: interactive 3D on hardware with a working GLES renderer, 2D layers everywhere else (also the mode HelixScreen drops to if 3D has to bail mid-print)
- `1` - 3D View: interactive 3D rendering of the toolpath
- `2` - 2D Layers: flat per-layer view, lighter on the GPU than 3D
- `3` - Thumbnail Only: shows just the slicer-embedded thumbnail, no live toolpath rendering - the lightest option

Adjustable from the UI at **Settings > Printing > G-code Preview**.

The mode can be overridden per launch without touching settings. Precedence is command line > `HELIX_GCODE_MODE` env var > this setting:
- `helix-screen --render-2d` forces 2D Layers and `--render-3d` forces 3D View for that session. Launching with `--render-2d` is the quick way to force 2D on hardware that struggles with 3D rendering.
- `HELIX_GCODE_MODE=3D` (or `2D`) - only those exact, case-sensitive values are honored; anything else falls back to 2D, and leaving it unset means Auto.

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

### `page_scroll_buttons`
**Type:** boolean
**Default:** `false`
**Description:** Show up/down scroll buttons on long lists throughout the app. Useful on small screens or displays where drag-to-scroll feels unresponsive. See [Display & Sound Settings](guide/settings/display-sound.md#scroll-buttons) for details.

### `ui_scale_percent`
**Type:** integer
**Default:** `0` (Automatic)
**Values:** `0`, or `100`-`200`
**Description:** Size of the whole interface. `0` means Automatic: HelixScreen works the scale out from the panel's physical pixel density, which leaves every supported printer at exactly `100` and only grows the UI on very high-density screens such as a phone. Any other value is an explicit percentage that overrides the measurement — useful if the interface comes out too small or too large on your display. A value outside the range is ignored and treated as Automatic.

**Takes effect on the next start.** HelixScreen sizes fonts and layout once, while the screen is being set up, so changing this mid-session cannot re-scale what is already drawn. See [Display & Sound Settings](guide/settings/display-sound.md#ui-scale).

### `printer_image`
**Type:** string
**Default:** `""` (auto-detect)
**Description:** Printer image displayed on the Home Panel and in the Printer Manager. The value determines which image is used:
- `""` (empty string or absent) — **Auto-detect**: HelixScreen selects an image based on the printer type reported by Klipper
- `"shipped:voron-v2"` — Use a specific shipped image by name (see `assets/images/printers/` for available images)
- `"custom:my-printer"` — Use a custom image that was imported from `config/custom_images/`

Custom images are PNG or JPEG files placed in `config/custom_images/`. They are automatically converted to optimized LVGL binary format (300px and 150px variants) when the Printer Image picker overlay is opened. Maximum file size is 5MB, maximum resolution is 2048x2048 pixels.

This setting can also be changed via the Printer Manager overlay (tap the printer image on the Home Panel).

### `calibration`
**Type:** object
**Default:** `{"valid": false}`
**Description:** Touch calibration coefficients. Set by the calibration wizard or manually. Contains calibration matrix values (`a` through `f`) when valid. If the wizard detects that the touchscreen's X/Y axes are swapped relative to the display, it bakes that correction directly into the `a`–`f` coefficients — there is no separate setting to configure.

---

## Appearance Settings

Located in the `appearance` section:

```json
{
  "appearance": {
    "toolhead_style": 0,
    "show_widget_labels": false
  }
}
```

### `toolhead_style`
**Type:** integer
**Default:** `0`
**Values:** `0` (Default), `1` (Creality K1), `2` (Creality K2)
**Description:** Which toolhead illustration is drawn on the temperature panel. Normally auto-detected from your printer type, so you rarely need to set it by hand. Override only if the wrong toolhead graphic is shown.

### `show_widget_labels`
**Type:** boolean
**Default:** `false`
**Description:** Show text labels beneath the Home panel widget icons. Leave `false` for a cleaner icon-only look, or set `true` if you prefer captions under each widget. Adjustable via the Home panel's Edit Mode.

---

## Input Settings

Located in the `input` section:

```json
{
  "input": {
    "scroll_throw": 25,
    "scroll_limit": 10,
    "long_press_time": 500,
    "jitter_threshold": 5,
    "scroll_guard": false,
    "scroll_guard_cooldown_ms": 80,
    "home_edit_mode_enabled": true,
    "touch_device": "",
    "device_blacklist": [],
    "force_calibration": false,
    "calibration": {
      "valid": false,
      "a": 1.0,
      "b": 0.0,
      "c": 0.0,
      "d": 1.0,
      "e": 1.0,
      "f": 0.0
    }
  }
}
```

> **Tuning touch feel:** These four settings interact. See **[Touch Feel — Which Setting Do I Tune?](TROUBLESHOOTING.md#touch-feel--which-setting-do-i-tune)** in the troubleshooting guide for a symptom → setting map.
>
> **Touch calibration** (`input.calibration`) is set automatically by the wizard — don't edit the `a`–`f` coefficients by hand. See the [Touch Calibration Guide](guide/touch-calibration.md) for the full reference.

### `scroll_throw`
**Type:** integer
**Default:** `25`
**Range:** `5` - `50` (UI-clamped)
**Description:** Scroll momentum decay rate — how quickly a flicked list coasts to a stop. Higher values = faster decay (less "throw"). LVGL's native default is 10; we use 25 because touchscreens feel sluggish with long coasting. Lower it if lists feel too "sticky" at the end of a flick.

### `scroll_limit`
**Type:** integer
**Default:** `10`
**Range:** `1` - `20` (UI-clamped)
**Description:** Pixels of finger movement required before a gesture is treated as a scroll instead of a tap. Below this threshold LVGL still thinks you're pressing a widget, and releasing will fire a click. Above it, the press is cancelled and scroll engages.

- **Lower** = scroll engages sooner. Fixes phantom clicks that fire when scrolling a list with a short, slow swipe.
- **Higher** = more deliberate gesture required. Reduces accidental scrolls when you meant to tap, but makes short-travel scrolls feel unresponsive.

Matches LVGL's native default of 10.

### `long_press_time`
**Type:** integer
**Default:** `500`
**Range:** `300` - `1500` (UI-clamped)
**Description:** How long (in milliseconds) a finger must hold before a press registers as a long-press. Governs every long-press in the app — home-screen Edit Mode entry, file-card delete, macro edit mode, and others. Raise this if long-press actions trigger when a finger simply rests on the glass (common on a tablet lying flat). Applied live — no restart needed.

### `home_edit_mode_enabled`
**Type:** boolean
**Default:** `true`
**Description:** Whether a long-press on the home grid enters Edit Mode (the drag-and-drop layout editor). When `false`, the long-press is suppressed entirely. Turn off if Edit Mode triggers by accident and you don't need to rearrange widgets, or pair with a higher `long_press_time` to make accidental entry harder while keeping the feature available. Applied live — no restart needed.

### `touch_device`
**Type:** string
**Default:** `""` (auto-detect)
**Example:** `"/dev/input/event1"`
**Description:** Override touch/pointer input device. Leave empty for auto-detection. Auto-detection finds touch or pointer capable devices.

### `device_blacklist`
**Type:** array of strings
**Default:** `[]` (no blacklist)
**Example:** `["002c:261a"]`
**Description:** USB input devices that HelixScreen ignores entirely for keyboard and barcode-scanner input. Each entry is a `"vid:pid"` pair of lowercase 4-digit hex IDs. Use this when a USB barcode scanner enumerates as a plain HID keyboard and HelixScreen keeps claiming it — for example when an external tool like `afc-spool-scan` needs exclusive access to the scanner. A blacklisted device is skipped by both the persistent keyboard binding and the in-app scan overlay, but still appears in the Barcode Scanner settings device list so you can identify it.

**Finding a device's VID:PID:** Open **Settings > Hardware & Devices > Spoolman > Barcode Scanner** — the device list shows each device's VID:PID. Alternatively, run `lsusb` over SSH and read the ID pair after `ID` (e.g. `ID 002c:261a`). See [Sharing a scanner with another tool](guide/barcode-scanner.md#sharing-a-scanner-with-another-tool-device-blacklist) for the full walkthrough.

### `jitter_threshold`
**Type:** integer
**Default:** `5`
**Range:** `0` - `30`
**Description:** Touch jitter filter dead zone in pixels. Capacitive touch controllers (notably Goodix GT9xx on FlashForge displays) report 2–5 px of coordinate drift even with a stationary finger. Without filtering, that drift accumulates past `scroll_limit` and a stationary tap gets cancelled as if it were a scroll. The filter freezes reported coordinates to the initial press point while movement stays within this radius.

- **Raise** if stationary taps are still being misread as swipes or scrolls on a noisy panel (typical fix: 15–25).
- **Lower / 0** if the filter is suppressing intentional short-travel gestures.

Can also be overridden with the `HELIX_TOUCH_JITTER` environment variable.

### `scroll_guard`
**Type:** boolean
**Default:** `false` (overridden to `true` by AD5M/AD5X presets)
**Description:** Suppresses the phantom "clicked" event some capacitive touch controllers generate when the finger lifts at the end of a scroll gesture. Common on FlashForge AD5M and AD5X displays — you scroll a list, lift your finger, and whatever button is now under where your finger was fires. When enabled, HelixScreen ignores taps for the cooldown window (default 80 ms — see `scroll_guard_cooldown_ms`) after a scroll ends. Can also be overridden with the `HELIX_SCROLL_GUARD` environment variable (`1` to enable).

### `scroll_guard_cooldown_ms`
**Type:** integer
**Default:** `80`
**Range:** `20` - `500`
**Description:** How long (in milliseconds) `scroll_guard` suppresses taps after a scroll gesture ends. Only takes effect when `scroll_guard` is enabled. The default handles most capacitive controllers that re-press briefly during lift-off; if you still see phantom clicks right as you lift your finger, try raising to `150` or `200`. Going too high will swallow legitimate taps that closely follow a scroll, so raise gradually. Can also be overridden with the `HELIX_SCROLL_GUARD_COOLDOWN_MS` environment variable.

### `force_calibration`
**Type:** boolean
**Default:** `false`
**Description:** Force the calibration wizard to run on next startup, even if the device doesn't normally require it. After successful calibration, this flag is automatically cleared. Mainly useful when touch is too far off to reach Settings at all — the Settings entry point itself is offered for any touchscreen, so you rarely need this just to find the option.

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
**Description:** Automatically turn on the configured LED strip when Klipper becomes ready. Useful for printers with chamber lights that should always be on. **Set this under [`printer.leds.led_on_at_start`](#ledsled_on_at_start) instead** — that is where HelixScreen writes it and where per-printer settings belong. A value here is still read, but it applies to every printer.

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
    "moonraker_host": "192.168.1.100",
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
    "chamber_heater": "auto",
    "chamber_sensor": "auto",
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

### `chamber_heater`
**Type:** string
**Default:** `"auto"`
**Values:** `"auto"`, `"none"`, or a Klipper object name
**Description:** Which heater warms the enclosure/chamber. `"auto"` lets HelixScreen pick it by name heuristics, `"none"` disables chamber-heater controls, or you can name the Klipper object explicitly (e.g., `"heater_generic chamber"`). Most printers work fine on `"auto"`.

### `chamber_sensor`
**Type:** string
**Default:** `"auto"`
**Values:** `"auto"`, `"none"`, or a Klipper object name
**Description:** Which temperature sensor reports the enclosure/chamber temperature. `"auto"` detects it by name heuristics, `"none"` disables chamber-temperature display, or name the Klipper object explicitly (e.g., `"temperature_sensor enclosure_bme"`). Set this if your chamber temperature reads from the wrong sensor or isn't detected.

---

## LED Settings

Located in the `printer.leds` section. Configured via **Settings > LED Settings**.

### `leds.strip`
**Type:** string
**Default:** `""` (empty)
**Description:** A single LED strip name, empty when there are no controllable LEDs. Use `leds.selected_strips` instead — it is the one that handles more than one strip.

### `leds.selected_strips`
**Type:** array of strings
**Default:** `[]`
**Description:** Klipper LED strip IDs to control (e.g., `["neopixel caselight", "dotstar toolhead"]`). Supports neopixel, dotstar, led, and WLED strips. Configured via **Settings > LED Settings**.

### `leds.led_on_at_start`
**Type:** boolean
**Default:** `false`
**Description:** Automatically turn on selected LED strips when Klipper becomes ready. Useful for chamber lights that should always be on. A copy of this key under `output` is also honoured if you have one; `printer.leds` is where HelixScreen writes it.

### `leds.startup_brightness`
**Type:** integer
**Default:** `80`
**Range:** `0` - `100`
**Description:** Brightness the strips come up at when `leds.led_on_at_start` switches them on. Independent of `leds.last_brightness`, so the lights can start at a fixed level regardless of where you left the slider.

### `leds.last_color`
**Type:** string (or integer)
**Default:** `"#FFFFFF"` (white)
**Description:** Last used LED color as a `#RRGGBB` hex string (e.g., `"#FFFFFF"` = white, `"#FF0000"` = red, `"#00FF00"` = green). Plain integer RGB values (e.g., `16777215`) are also accepted. Remembered between sessions.

### `leds.last_brightness`
**Type:** integer
**Default:** `100`
**Range:** `0` - `100`
**Description:** Last used brightness percentage. Remembered between sessions.

### `leds.color_presets`
**Type:** array of strings (or integers)
**Default:** `["#FFFFFF", "#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF00FF", "#00FFFF"]`
**Description:** Preset colors shown in the color picker as `#RRGGBB` hex strings. Plain integer RGB values are also accepted. Default presets are white, red, green, blue, yellow, magenta, and cyan.

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
- Additional fields depend on the action: `brightness` (0-100), `color` (`#RRGGBB` hex string, or a plain integer RGB), `effect_name` (string), `wled_preset` (integer), `macro` (string)

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
**Description:** G-code macros for quick-action buttons throughout the UI. Each macro can be a plain G-code string or an object with `label` and `gcode` fields.

**Default values:**

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

| Key | Format | Where it's used |
|-----|--------|-----------------|
| `cooldown` | G-code string | Preheat widget (auto-shows "Cool Down" when heaters are on), Filament panel cooldown button |
| `load_filament` | `{ "label", "gcode" }` | Filament panel Load button |
| `unload_filament` | `{ "label", "gcode" }` | Filament panel Unload button |
| `macro_1` | `{ "label", "gcode" }` | Controls panel custom button 1 |
| `macro_2` | `{ "label", "gcode" }` | Controls panel custom button 2 |

**Customizing cooldown for enclosed printers:**

If your printer has a chamber heater, bed fans, or recirculation fans that should turn off during cooldown, override the `cooldown` macro:

```json
{
  "cooldown": "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0\nSET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0\nSET_HEATER_TEMPERATURE HEATER=chamber_heater TARGET=0\nSET_FAN_SPEED FAN=bed_fan SPEED=0"
}
```

Multi-line G-code is separated by `\n`. You can also reference a Klipper macro by name (e.g., `"cooldown": "MY_COOLDOWN_MACRO"`).

Configured via **Settings > Printing > Macro Buttons**, or by editing `settings.json` directly.

---

## Moonraker Settings

Connection settings are in the `printer` section:

```json
{
  "printer": {
    "moonraker_host": "192.168.1.100",
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
**Default:** `"127.0.0.1"` (the value in `config/settings.json.template`)
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

## Standard Macros (Quick Action Buttons)

Located in the `standard_macros` section. These pick which built-in actions appear as the four quick-action buttons on the Controls panel:

```json
{
  "standard_macros": {
    "quick_button_1": "clean_nozzle",
    "quick_button_2": "bed_level",
    "quick_button_3": "",
    "quick_button_4": ""
  }
}
```

### `quick_button_1` … `quick_button_4`
**Type:** string
**Default:** `"clean_nozzle"` (button 1), `"bed_level"` (button 2), `""` (buttons 3 and 4)
**Values:** `"clean_nozzle"`, `"bed_level"`, `"heat_soak"`, `"purge"`, `"bed_mesh"`, or `""` (empty = hide the button)
**Description:** Assigns a built-in action to each of the four Controls-panel quick buttons. An empty string hides that button. The action runs the matching macro on your printer (auto-detected from your Klipper config). Configured most easily via **Settings > Printing > Macro Buttons** rather than by editing JSON.

### `load_filament`, `unload_filament`, `purge`, `pause`, `resume`, `cancel`, `bed_mesh`, `bed_level`, `clean_nozzle`, `heat_soak`
**Type:** string
**Default:** `""` (empty = use auto-detection)
**Description:** Overrides which macro HelixScreen runs for each standard action. An empty
string means "auto-detect from your Klipper config"; a macro name pins that slot to your
choice. Written by **Settings > Printing > Macro Buttons**, which is the easier way to set them
because it lists the macros your printer actually defines.

```json
{
  "standard_macros": {
    "unload_filament": "MY_UNLOAD_ROUTINE"
  }
}
```

On a printer with a multi-filament system, `load_filament` and `unload_filament` also decide
*who* performs the operation. Left empty, the filament system does it. Set to a macro, your
macro does it instead and the filament system's own handling is skipped for that operation —
see [Customizing which macro runs](guide/filament.md#customizing-which-macro-runs).

> **Note:** This is separate from `printer.default_macros`, which customizes the Load/Unload/cooldown/custom-macro buttons elsewhere in the UI. See [Printer Settings › default_macros](#printer-settings).

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

### Per-printer AMS settings

The remaining AMS settings are **per printer**, so they live under the printer's own section rather than the top-level `ams` block:

```json
{
  "printers": {
    "default": {
      "ams": {
        "force_bypass_controls": false,
        "always_show_bypass_spool": false,
        "keep_spool_info_on_eject": true,
        "afc_unload_after_print": false
      }
    }
  }
}
```

All four have UI equivalents in **Settings > Hardware & Devices > Multi-Filament System Management** - edit them there rather than by hand.

#### `force_bypass_controls`
**Type:** boolean
**Default:** `false`
**Description:** Show the bypass controls and the external spool on the filament path even when the firmware reports no bypass position. Applies to Anycubic ACE Pro, Snapmaker U1, tool changers, QIDI Box, and Happy Hare configs where `[mmu_machine] has_bypass` is `0`. The matching UI row is hidden whenever the firmware *does* report a bypass - including the Creality CFS, whose bypass always works.

On Happy Hare, `MMU_SELECT_BYPASS` ignores `has_bypass` and works either way, so this setting makes the bypass usable on `mmu_vendor: Other` setups and on uncalibrated type-A selectors. On the other systems there is no bypass command to send: the Bypass toggle reports that the operation is not supported, and the setting controls only whether the external spool is displayed and tracked.

See [Filament → When Bypass Doesn't Appear](guide/filament.md#when-bypass-doesnt-appear).

#### `always_show_bypass_spool`
**Type:** boolean
**Default:** `false`
**Description:** Keep the external spool visible on the filament path while bypass is disengaged. Applies to AFC systems (Box Turtle, OpenAMS) only, which publish a virtual bypass sensor whether or not one is physically wired; without this, the node is drawn only while bypass is actually engaged.

#### `keep_spool_info_on_eject`
**Type:** boolean
**Default:** `true`
**Description:** Keep a lane's spool details after it empties, so reloading the same spool after maintenance needs no re-selection. Turn it off to start fresh whenever a lane empties. Applies only to spools selected in HelixScreen; a spool assigned elsewhere (such as Mainsail) clears with the lane. To have every assigned spool remembered no matter where it was picked, use the firmware's own retention instead (AFC: `remember_spool` in AFC.cfg) - HelixScreen follows the spool the firmware reports. When that firmware retention covers every lane, it takes precedence and the matching toggle shows as disabled. The toggle (**Keep Spool Info on Eject**, in the AMS Management overlay) is shown only on systems whose firmware tracks spool ids per lane (AFC, Happy Hare); systems that detect spool swaps by tag always refresh on a swap regardless of this setting.

#### `afc_unload_after_print`
**Type:** boolean
**Default:** `false`
**Description:** On AFC systems, retract filament back to its lane when a print finishes.

---

## Panel Widget Settings

Located under the `panel_widgets` key, grouped by panel ID. The Home panel uses a multi-page format with explicit grid positions:

```json
{
  "panel_widgets": {
    "home": {
      "pages": [
        {
          "id": "main",
          "widgets": [
            {"id": "printer_image", "enabled": true, "col": 0, "row": 0, "colspan": 4, "rowspan": 4},
            {"id": "print_status", "enabled": true, "col": 0, "row": 4, "colspan": 8, "rowspan": 4},
            {"id": "temperature", "enabled": true, "col": 4, "row": 0, "colspan": 2, "rowspan": 2},
            {"id": "fan_stack", "enabled": true, "col": 4, "row": 2, "colspan": 2, "rowspan": 2}
          ]
        },
        {
          "id": "page_1",
          "widgets": [
            {"id": "temp_graph", "enabled": true, "col": 0, "row": 0, "colspan": 8, "rowspan": 6},
            {"id": "camera", "enabled": true, "col": 8, "row": 0, "colspan": 8, "rowspan": 6}
          ]
        }
      ],
      "main_page_index": 0,
      "next_page_id": 2
    }
  }
}
```

> **Positions and spans are counted in half cells, not cells.** The grid is laid out in
> half-cell tracks so that a few widgets can sit on half-cell boundaries, and these numbers
> are in those tracks. A one-cell widget is `"colspan": 2, "rowspan": 2`; the two-by-two
> Printer Image above is `4` by `4`. Multiply by two when translating a size you read off
> the Widget Catalog badge or the [Home Panel guide](guide/home-panel.md#available-widgets),
> which are both written in whole cells.

### `panel_widgets.home`
**Type:** object
**Description:** Controls the Home Panel's pages and widgets. Contains:

- `pages` — Array of page objects. Each page has:
  - `id` — Unique page identifier (e.g., `"main"`, `"page_1"`)
  - `widgets` — Array of widget objects on this page (see below)
- `main_page_index` — Which page is the "main" page (shown on first connect and when double-tapping Home). `0` = first page.
- `next_page_id` — Internal counter for generating unique page IDs. Do not modify manually.

Each widget object has:

- `id` — Widget identifier (see table below)
- `enabled` — Whether the widget is shown (`true`/`false`). A widget with `enabled: false` sits in the Widget Catalog waiting for you to add it back
- `col` — Grid column position in half cells (0-based, left to right). `-1` means "no position yet"
- `row` — Grid row position in half cells (0-based, top to bottom). `-1` means "no position yet"
- `colspan` — Width in half cells (`2` = one cell wide)
- `rowspan` — Height in half cells (`2` = one cell tall)
> **What `col: -1` / `row: -1` means.** The widget is switched on but has nowhere to sit right now, usually because the grid was full when HelixScreen last laid out the page. It is *not* disabled: as soon as a cell frees up - you remove another widget, unplug the hardware another widget needed, or view the same layout on a screen with a bigger grid - it places itself again automatically. You do not need to re-add it from the catalog.

- `config` — (optional) Per-widget settings object, written by the gear button in Edit Mode. Which keys apply depends on the widget:

| Key | Widgets | Values |
|-----|---------|--------|
| `display_mode` | `temp_stack`, `fan_stack`, `thermistor` | `"stack"` (default) or `"carousel"` |
| `layout_style` | `print_status` | `"detailed"` for the expanded card, otherwise the compact one |
| `fan` | `fan` | Name of the fan to monitor |
| `sensor` | `thermistor` | Name of the sensor to display |
| `sensors` | `thermistor`, `temp_graph` | Array of sensor names to include |
| `macro`, `color`, `skip_param_prompt` | `favorite_macro` | Macro to run, icon tint, and whether to skip the parameter prompt |
| `device` | `power_device` | Name of the Moonraker power device to bind |
| `icon` | `favorite_macro`, `power_device`, `temp_stack`, `fan_stack`, `tool_switcher` | Icon name override |
| `rotation`, `flip_h`, `flip_v` | `camera` | `0`/`90`/`180`/`270`, and booleans |
| `source`, `danger_threshold` | `clog_detection` | Detection source and danger-zone percentage |
| `source` | `filament` | Which sensor role the tile follows: `"auto"` (default), `"runout"`, `"toolhead"`, or `"entry"` |
| `material_index` | `preheat` | Which material profile the buttons preheat to |

Setting these through Edit Mode is far easier than editing them here, and it is the only way that validates the value against your printer.

**Available widget IDs:**

For what each widget does and how big it can get, see the [Home Panel guide](guide/home-panel.md#available-widgets). This table is just the ID-to-widget mapping you need when editing the JSON.

| ID | Widget | On by default | Hardware-gated |
|----|--------|---------------|----------------|
| `printer_image` | Printer Image | Yes | No |
| `print_status` | Print Status | Yes | No |
| `control_buttons` | Print Controls | No | No |
| `print_stats` | Print Stats | No | No |
| `job_queue` | Job Queue | No | No |
| `camera` | Camera | No | Webcam configured |
| `temperature` | Nozzle Temperature | Yes | No |
| `nozzle_temps` | Nozzle Temperatures | No | No |
| `bed_temperature` | Bed Temperature | Yes | No |
| `chamber_temperature` | Chamber Temperature | No | Chamber sensor or heater |
| `temp_stack` | Temperatures | No | No |
| `thermistor` | Temperature Sensors | No | Extra temperature sensors |
| `temp_graph` | Temperature Graph | No | No |
| `preheat` | Preheat | No | No |
| `fan_stack` | Fan Speeds | Yes | No |
| `fan` | Fan | No | No |
| `ams` | Multi-Filament System Status | No | AMS/MMU detected |
| `active_spool` | Active Spool | No | No |
| `filament` | Filament Sensor | Yes | Filament sensor |
| `humidity` | Humidity | No | Humidity sensor |
| `width_sensor` | Width Sensor | No | Width sensor |
| `clog_detection` | Clog Detection | No | Clog detection hardware |
| `favorite_macro` | Macro Button | No | No |
| `macros` | Macros | No | No |
| `gcode_console` | G-code Console | No | No |
| `motion` | Motion | No | No |
| `tool_switcher` | Tool Switcher | No | No |
| `power_device` | Power | No | Moonraker power device |
| `led` | LED Light | Yes | LEDs configured |
| `led_controls` | LED Controls | No | LEDs configured |
| `network` | Network | No | No |
| `notifications` | Notifications | Yes | No |
| `clock` | Digital Clock | No | No |
| `tips` | Tips | Yes | No |
| `shutdown` | Shutdown/Reboot | No | No |
| `firmware_restart` | Firmware Restart | No | No |
| `lock` | Lock Screen | No | No |

`power_device`, `fan`, `thermistor`, `favorite_macro`, and `temp_graph` can appear more than once. Extra copies get an ID like `favorite_macro:2`.

**Notes:**
- Widget grid positions (`col`, `row`, `colspan`, `rowspan`) determine where each widget appears on its page, in half cells
- Hardware-gated widgets are hidden on the Home Panel if their hardware isn't detected, even when enabled
- New widgets are appended automatically with their default enabled state
- Widget IDs that HelixScreen doesn't recognise are ignored
- Up to 8 pages are supported

This is best configured via **Edit Mode** on the Home Panel (long-press the widget grid) rather than editing the JSON directly. See the [Home Panel guide](guide/home-panel.md) for details on adding pages and arranging widgets.

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
    "estop_require_confirmation": true,
    "cancel_escalation_enabled": false,
    "cancel_escalation_timeout_seconds": 30
  }
}
```

### `estop_require_confirmation`
**Type:** boolean
**Default:** `true`
**Description:** Require confirmation dialog before emergency stop. When `false`, E-Stop triggers immediately. Default is `true` to prevent accidental emergency stops.

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

## Notification Settings

Located in the `notifications` section:

```json
{
  "notifications": {
    "min_toast_severity": 0
  }
}
```

### `min_toast_severity`
**Type:** integer
**Default:** `0`
**Values:** `0` (all toasts), `1` (warnings & errors), `2` (errors only)
**Description:** The lowest notification level allowed to interrupt with a toast. Below-the-line notifications still land in the notification history; full-screen error dialogs always show. Change via **Settings > Safety & Notifications > On-screen Alerts**.

---

## Filament Settings

Located in the `filament` section:

```json
{
  "filament": {
    "auto_cooldown": true,
    "cooldown_delay_seconds": 120
  }
}
```

### `auto_cooldown`
**Type:** boolean
**Default:** `true`
**UI:** Settings > Safety & Notifications > **Cool nozzle after filament ops**
**Description:** Whether HelixScreen turns the extruder heater off after a filament load or unload completes. Turn this off if your filament system runs its own post-operation cooldown — AFC does, in recent versions — so the two aren't both driving the same heater.

### `cooldown_delay_seconds`
**Type:** integer
**Default:** `120`
**Description:** How long to wait, in seconds, after a filament load or unload before automatically turning the extruder heater off. This lets you run several filament operations back-to-back without the nozzle cooling down between them. Default is 120 (2 minutes). Setting this to `0` also disables auto-cooldown, but prefer `auto_cooldown` — it's the one the UI toggle writes.

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

## Security Settings

Located in the `security` section. Controls the optional PIN lock screen:

```json
{
  "security": {
    "pin_hash": "",
    "auto_lock": false
  }
}
```

### `security.pin_hash`
**Type:** string
**Default:** `""` (empty = lock screen disabled)
**Description:** A SHA-256 hash of your lock-screen PIN. HelixScreen stores only this hash, never the PIN itself. **Do not edit this value by hand** — set, change, or clear your PIN through **Settings > System > Security**, which computes and writes the hash for you. An empty string means no PIN and no lock screen.

### `security.auto_lock`
**Type:** boolean
**Default:** `false`
**Description:** When `true`, the screen automatically locks whenever the display sleeps and wakes, requiring your PIN to get back in. Has no effect unless `pin_hash` is set (with no PIN there is nothing to lock). Toggle via **Settings > System > Security**.

---

## Label Printer Settings

Located in the `label_printer` section. Configures the thermal label printer used to print filament spool labels. This is best set up through **Settings > Hardware & Devices > Spoolman > Label Printer** — scanning and selecting a printer fills these fields in for you. The keys are documented here for reference. See the [Label Printing guide](guide/label-printing.md) for the full walkthrough.

```json
{
  "label_printer": {
    "type": "network",
    "address": "",
    "port": 9100,
    "protocol": "raw",
    "label_size": 0,
    "preset": 0,
    "label_count": 1,
    "usb_vid": 0,
    "usb_pid": 0,
    "usb_serial": "",
    "bt_address": "",
    "bt_name": "",
    "bt_transport": "spp"
  }
}
```

### `type`
**Type:** string
**Default:** `"network"`
**Values:** `"network"`, `"usb"`, `"bluetooth"`
**Description:** How the label printer connects — Network (Brother QL over Ethernet/WiFi), USB (Phomemo over cable), or Bluetooth (any supported printer paired over Bluetooth). Bluetooth is only offered when your device has Bluetooth hardware.

### `address`
**Type:** string
**Default:** `""`
**Description:** IP address or hostname of a network label printer. Only used when `type` is `"network"`.

### `port`
**Type:** integer
**Default:** `9100`
**Description:** TCP port for a network label printer. `9100` is the standard RAW/JetDirect port used by Brother QL. Only used when `type` is `"network"`.

### `protocol`
**Type:** string
**Default:** `"raw"`
**Values:** `"raw"`, `"ipp"`
**Description:** Network print protocol. `"raw"` (JetDirect, port 9100) works for most printers; `"ipp"` is used by some networked models.

### `label_size`
**Type:** integer
**Default:** `0`
**Description:** Index of the label/tape size preset for your printer. `0` selects that model's default size. The available sizes depend on the detected printer (a Niimbot D11 offers different sizes than a B21), so pick yours from the size list in the Label Printer settings overlay rather than guessing an index.

### `preset`
**Type:** integer
**Default:** `0`
**Description:** Label content layout preset. `0` = **Standard** (full label: spool name, material, color, temperatures, and QR code); other indices select the **Compact** or **QR Only** layouts. Choose it in the Label Printer settings overlay.

### `label_count`
**Type:** integer
**Default:** `1`
**Description:** Number of copies to print per label job.

### `usb_vid` / `usb_pid` / `usb_serial`
**Type:** integer / integer / string
**Default:** `0` / `0` / `""`
**Description:** USB vendor ID, product ID, and serial number identifying a USB label printer. `0` (and an empty serial) means auto-detect — you normally never set these by hand. Only used when `type` is `"usb"`.

### `bt_address` / `bt_name` / `bt_transport`
**Type:** string / string / string
**Default:** `""` / `""` / `"spp"`
**Description:** Bluetooth MAC address, advertised name, and transport of a Bluetooth label printer. `bt_transport` is `"spp"` (Bluetooth Classic / RFCOMM — used by Brother PT, Phomemo, and MakeID) or `"ble"` (Bluetooth Low Energy — used by Niimbot). These are filled in automatically when you scan and select a printer; you don't normally type the MAC address by hand. Only used when `type` is `"bluetooth"`.

---

## Printer Switcher

Located in the `printers` section:

```json
{
  "printers": {
    "show_printer_switcher": false
  }
}
```

### `printers.show_printer_switcher`
**Type:** boolean
**Default:** `false`
**Description:** Show a printer-switcher button on the Home panel for quickly jumping between configured printers. Off by default since single-printer setups don't need it; turn it on when you manage more than one printer. See [Multi-Printer Configuration](#multi-printer-configuration) above.

---

## Telemetry Settings

### `telemetry_enabled`
**Type:** boolean
**Default:** `false`
**Description:** Enables anonymous usage telemetry. This is a top-level key (not nested in a section). **OFF by default — you must opt in**, either during the setup wizard or via **Settings > System > Share Usage Data**. While `false`, nothing is collected, queued, or transmitted. For a full breakdown of exactly what is and isn't collected, and how the data is anonymized, see the [Telemetry](TELEMETRY.md) documentation.

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

## Upgrade Nudge

Located in the top-level `upgrade_nudge` section. Controls how insistently HelixScreen tells you an update is waiting.

```json
{
  "upgrade_nudge": {
    "intensity": "off",
    "dismissed_version": ""
  }
}
```

### `upgrade_nudge.intensity`
**Type:** string
**Default:** `"off"`
**Values:** `"off"`, `"normal"`, `"aggressive"`
**Description:** How visible the update prompt is.

- `"off"` — nothing. An available update is only visible in **Settings > Help & About**
- `"normal"` — a red dot on the Settings icon
- `"aggressive"` — the red dot, plus a banner across the top of the screen that stays until you dismiss it

No nudge of any kind appears while a print is running.

### `upgrade_nudge.dismissed_version`
**Type:** string
**Default:** `""` (empty)
**Description:** The version whose banner you dismissed. Set for you when you close the banner, and only suppresses that exact version — the next release brings the banner back. Clear it to see the banner again for a version you already dismissed.

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
      "chamber": "auto",
      "speaker": "auto"
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
- Disable `speaker` on a board whose buzzer is detected but not wired, so HelixScreen stops trying to play M300 beeps through it

---

## Resetting Configuration

### Full Reset
Delete the config file and restart (use your actual install path):
```bash
# Pi with Klipper ecosystem:
rm ~/helixscreen/config/settings.json
# Pi without ecosystem (or if installed to /opt):
sudo rm /opt/helixscreen/config/settings.json

sudo systemctl restart helixscreen
```

This triggers the first-run wizard.

### Partial Reset
Edit the config file directly:
```bash
nano ~/helixscreen/config/settings.json
```

Or copy fresh from template:
```bash
cp ~/helixscreen/config/settings.json.template ~/helixscreen/config/settings.json
```

---

## Config Safety & Recovery

HelixScreen protects your configuration against corruption and data loss:

### Atomic Saves
Configuration writes use atomic file operations — data is written to a temporary
file first, then renamed into place. This prevents partial writes from corrupting
your config if power is lost during a save.

### Corruption Detection
If `settings.json` contains invalid JSON (e.g., from manual editing errors),
HelixScreen detects the parse failure and:
1. Renames the corrupt file to `settings.json.corrupt` (preserved for diagnosis)
2. Loads safe defaults
3. Logs the error with details about what went wrong

### Rolling Backups
Every successful config save maintains rolling backups in two locations:
- `/var/lib/helixscreen/settings.json.backup` (primary — survives app reinstalls)
- `~/.helixscreen/settings.json.backup` (fallback)

If `settings.json` is missing at startup (e.g., after a Moonraker update wipe),
HelixScreen automatically restores from the most recent backup.

### Recovery Steps
If your config is lost or corrupted:
1. **Automatic:** HelixScreen restores from rolling backup on next launch
2. **Manual:** Check for `settings.json.corrupt` in your config directory — this
   contains your previous (invalid) config that you can manually fix
3. **Fresh start:** Copy `settings.json.template` to `settings.json` and re-run
   the setup wizard

### If you have a `helixconfig.json`
HelixScreen renames it to `settings.json` on startup — no manual action needed.

---

## Command-Line Options

HelixScreen accepts command-line options for overriding configuration and debugging.

### Display Options

| Option | Description |
|--------|-------------|
| `-s, --size <size>` | Screen size: `tiny` (480×320), `small` (480×400), `medium` (800×480), `large` (1024×600) |
| `--layout <type>` | Override auto-detected layout: `auto`, `standard`, `ultrawide`, `portrait`, `micro`, `micro-portrait`, `tiny`, `tiny-portrait`. **`ultrawide` and all portrait variants are alpha** — see the [`layout`](#layout) setting |
| `--dpi <n>` | Display DPI (50-500, default: 160) |
| `--dark` | Use dark theme |
| `--light` | Use light theme |
| `--skip-splash` | Skip splash screen on startup |
| `--no-sound` | Disable all sound output (prevents audio backend initialization) |

### Navigation Options

| Option | Description |
|--------|-------------|
| `-w, --wizard` | Force first-run configuration wizard |
| `--skip-wizard` | Suppress the first-run wizard (for automation/screenshots) |

> Developers: to drive the UI to a specific panel or overlay, see `docs/devel/HELIXCTL.md` (the `helix-screen ctl` remote-control client). The control server it talks to is auto-enabled in `--test` mode, or opt-in with `--remote` (and `--remote-socket <path>` to override the socket location).

### Connection Options

| Option | Description |
|--------|-------------|
| `--moonraker <url>` | Override Moonraker URL (e.g., `ws://192.168.1.100:7125`) |

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
# Start in dark mode, skipping the splash screen
helix-screen --dark --skip-splash

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

**Display & Input:**

| Variable | Description |
|----------|-------------|
| `HELIX_DRM_DEVICE` | Override DRM device path (e.g., `/dev/dri/card1`) |
| `HELIX_DISPLAY_BACKEND` | Override display backend (`drm`, `fbdev`, `sdl`) |
| `HELIX_DISPLAY_ROTATION` | Override display rotation in degrees (`0`, `90`, `180`, `270`) |
| `HELIX_COLOR_SWAP_RB` | Swap red/blue channels (`1` to enable) — fixes inverted colors on some displays |
| `HELIX_BACKLIGHT_DEVICE` | Force the backlight control method: `sysfs`, `allwinner`, `brightness` (Creality Sonic Pad), or `none` to disable. Fixes a brightness slider that does nothing |
| `HELIX_DPI` | Override display DPI / UI scale (`50`–`500`, default `160`) — lower for oversized UI, higher for cramped UI |
| `HELIX_SCREEN_SIZE` | Force screen size / layout (`micro`, `tiny`, `small`, `medium`, `large`, `xlarge`, `xxlarge`, or `WxH`) — persistent equivalent of `-s` |
| `HELIX_TOUCH_DEVICE` | Override touch input device (e.g., `/dev/input/event1`) |
| `HELIX_TOUCH_SWAP_AXES` | Swap X/Y touch axes (`1` to enable) |
| `HELIX_TOUCH_CALIBRATE` | Force touch calibration on next launch (`1` to enable) |
| `HELIX_MOUSE_DEVICE` | Override USB mouse device (e.g., `/dev/input/event4`) |
| `HELIX_KEYBOARD_DEVICE` | Override USB keyboard device (e.g., `/dev/input/event5`) |
| `HELIX_TOUCH_JITTER` | Override `jitter_threshold` dead zone in pixels (`0`–`30`) |
| `HELIX_SCROLL_GUARD` | Override `scroll_guard` post-scroll tap suppression (`1` to enable) |
| `HELIX_SCROLL_GUARD_COOLDOWN_MS` | Override `scroll_guard_cooldown_ms` window in milliseconds |

**Theme & Rendering:**

| Variable | Description |
|----------|-------------|
| `HELIX_THEME` | Override theme (e.g., `dracula`, `nord`, `gruvbox`) |
| `HELIX_GCODE_MODE` | Override G-code render mode (`3D` or `2D`, exact case-sensitive; unset = Auto, any other value = 2D) |
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
  "config_version": 22,
  "dark_mode": true,
  "brightness": 70,
  "sounds_enabled": true,
  "completion_alert": 1,
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
    "sleep_sec": 1200,
    "dim_sec": 600,
    "dim_brightness": 30,
    "drm_device": "",
    "gcode_render_mode": 2,
    "gcode_3d_enabled": true,
    "bed_mesh_render_mode": 0,
    "bed_mesh_show_zero_plane": true,
    "page_scroll_buttons": false,
    "printer_image": ""
  },

  "input": {
    "scroll_throw": 25,
    "scroll_limit": 10,
    "jitter_threshold": 5,
    "scroll_guard": false,
    "scroll_guard_cooldown_ms": 80,
    "touch_device": "",
    "device_blacklist": [],
    "force_calibration": false,
    "calibration": {
      "valid": false,
      "a": 1.0,
      "b": 0.0,
      "c": 0.0,
      "d": 0.0,
      "e": 1.0,
      "f": 0.0
    }
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
    "home": {
      "pages": [
        {
          "id": "main",
          "widgets": [
            {"id": "printer_image", "enabled": true, "col": 0, "row": 0, "colspan": 2, "rowspan": 2},
            {"id": "print_status", "enabled": true, "col": 0, "row": 2, "colspan": 2, "rowspan": 2},
            {"id": "temperature", "enabled": true, "col": 2, "row": 0, "colspan": 1, "rowspan": 1},
            {"id": "fan_stack", "enabled": true, "col": 3, "row": 0, "colspan": 1, "rowspan": 1}
          ]
        }
      ],
      "main_page_index": 0,
      "next_page_id": 1
    }
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
    "estop_require_confirmation": true
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

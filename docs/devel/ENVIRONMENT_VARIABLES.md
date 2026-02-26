# Environment Variables Reference

This document provides a comprehensive reference for all environment variables used in HelixScreen.

## Quick Reference

| Category | Count | Prefix |
|----------|-------|--------|
| [Display & Backend](#display--backend-configuration) | 10 | `HELIX_` |
| [Touch Calibration](#touch-calibration) | 6 | `HELIX_TOUCH_*` |
| [G-Code Viewer](#g-code-viewer) | 3 | `HELIX_` |
| [Bed Mesh](#bed-mesh) | 1 | `HELIX_` |
| [Mock & Testing](#mock--testing) | 14 | `HELIX_MOCK_*` |
| [UI Automation](#ui-automation) | 3 | `HELIX_AUTO_*` |
| [Calibration](#calibration-auto-start) | 2 | `*_AUTO_START` |
| [Development](#development) | 1 | `HELIX_` |
| [Debugging](#debugging) | 2 | `HELIX_DEBUG_*` |
| [Deployment](#deployment) | 1 | `HELIX_` |
| [Logging & Startup](#logging--startup) | 2 | `HELIX_` |
| [Data Paths](#data-paths) | 3 | `HELIX_` / Standard Unix |

---

## Display & Backend Configuration

These variables control how HelixScreen connects to displays and input devices.

### `HELIX_THEME`

Override the theme loaded from config. Useful for testing themes or taking screenshots without changing settings.

| Property | Value |
|----------|-------|
| **Values** | Theme filename without `.json` extension (e.g., `gruvbox`, `dracula`, `nord`) |
| **Default** | Read from config (`/display/theme`) |
| **File** | `src/ui/theme_manager.cpp` |

```bash
# Test Gruvbox theme
HELIX_THEME=gruvbox ./build/bin/helix-screen --test

# Take screenshot with Dracula theme
HELIX_THEME=dracula ./scripts/screenshot.sh helix-screen dracula-home home
```

**Available themes:** `ayu`, `catppuccin`, `chatgpt`, `chatgpt-classic`, `dracula`, `everforest`, `google-drive`, `google-notebooklm`, `gruvbox`, `kanagawa`, `nord`, `onedark`, `rose-pine`, `solarized`, `tokyonight`

### `HELIX_DISPLAY_BACKEND`

Override the automatic display backend detection.

| Property | Value |
|----------|-------|
| **Values** | `sdl`, `drm`, `fbdev` |
| **Default** | `fbdev` (CPU rendering, maximum compatibility) |
| **File** | `src/display_backend.cpp` |

**Backend comparison:**

| Backend | Rendering | Best for |
|---------|-----------|----------|
| `fbdev` | CPU (software) | Maximum compatibility, all hardware, SPI displays |
| `drm` | GPU-accelerated via DRM+EGL (OpenGL ES) | Pi 3B+, Pi 4, Pi 5, BTT CB1 with HDMI/DSI displays |
| `sdl` | SDL2 (desktop development) | Development on Linux/macOS desktops |

The `drm` backend uses DRM (Direct Rendering Manager) with EGL/OpenGL ES to offload rendering to the GPU. This reduces CPU usage and can improve frame rates, especially on Pi 4 and Pi 5. The `fbdev` backend is the safe default that works everywhere, including SPI displays that lack DRM support.

```bash
# Force SDL backend (useful for debugging on embedded systems)
HELIX_DISPLAY_BACKEND=sdl ./build/bin/helix-screen

# Force DRM backend with GPU acceleration
HELIX_DISPLAY_BACKEND=drm ./build/bin/helix-screen
```

**Enabling in production (systemd service):**
```ini
# /etc/systemd/system/helixscreen.service (or override)
[Service]
Environment="HELIX_DISPLAY_BACKEND=drm"
```

Then reload and restart:
```bash
sudo systemctl daemon-reload
sudo systemctl restart helixscreen
```

**Supported hardware for `drm`:** Raspberry Pi 3B+, Pi 4, Pi 5, BTT CB1 (and other Allwinner H616 boards). Requires a display connected via HDMI or DSI — SPI displays are not supported with `drm`. If the `drm` backend fails to initialize, HelixScreen falls back to `fbdev` automatically.

### `HELIX_DRM_DEVICE`

Specify which DRM device to use when the DRM backend is active. Needed when multiple GPU/display controllers are present (common on Pi 5).

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/dri/card0`, `/dev/dri/card1`) |
| **Default** | `/dev/dri/card0` (auto-detect scans for first device with a connected display) |
| **File** | `src/display_backend_drm.cpp` |

```bash
# Use secondary GPU / display controller
HELIX_DRM_DEVICE=/dev/dri/card1 ./build/bin/helix-screen
```

**Pi 5 DRM device mapping:**
| Device | Controller | Use |
|--------|-----------|-----|
| `/dev/dri/card0` | v3d | 3D rendering only (no display output) |
| `/dev/dri/card1` | DSI | Official Pi touchscreen |
| `/dev/dri/card2` | vc4 (HDMI) | HDMI displays |

On Pi 4 and earlier, `/dev/dri/card0` is typically the only device with display output. Auto-detection finds the first device with dumb buffer support and a connected display, so most users do not need to set this variable.

### `HELIX_TOUCH_DEVICE`

Override automatic touch input device detection.

| Property | Value |
|----------|-------|
| **Values** | Device path (e.g., `/dev/input/event0`) |
| **Default** | Auto-detect |
| **Files** | `src/display_backend_fbdev.cpp`, `src/display_backend_drm.cpp` |

```bash
# Specify touch device explicitly
HELIX_TOUCH_DEVICE=/dev/input/event2 ./build/bin/helix-screen
```

### `HELIX_BACKLIGHT_DEVICE`

Control backlight device path or disable backlight control entirely.

| Property | Value |
|----------|-------|
| **Values** | Device path, or `"none"` to disable |
| **Default** | Auto-detect |
| **File** | `src/backlight_backend.cpp` |

```bash
# Disable backlight control (e.g., for external displays)
HELIX_BACKLIGHT_DEVICE=none ./build/bin/helix-screen

# Use specific backlight device
HELIX_BACKLIGHT_DEVICE=/sys/class/backlight/backlight-lvds ./build/bin/helix-screen
```

### `HELIX_DISPLAY_ROTATION`

Override the display rotation angle. Takes highest priority over config file (`/display/rotate`) and CLI flags (`--rotate`).

| Property | Value |
|----------|-------|
| **Values** | `0`, `90`, `180`, `270` (degrees) |
| **Default** | `0` (no rotation) |
| **Files** | `src/application/display_manager.cpp` |

```bash
# Rotate display 90° (e.g., portrait display mounted landscape)
HELIX_DISPLAY_ROTATION=90 ./build/bin/helix-screen

# Rotate 180° (upside-down mount)
HELIX_DISPLAY_ROTATION=180 ./build/bin/helix-screen
```

**Priority order:**
1. `HELIX_DISPLAY_ROTATION` environment variable (highest)
2. `--rotate <degrees>` CLI flag
3. `/display/rotate` in `helixconfig.json`
4. Default: `0` (no rotation)

**Note:** Software rotation is only supported on embedded backends (fbdev/DRM). On SDL (desktop dev), rotation is logged as a warning and skipped due to LVGL's DIRECT render mode limitation.

**Touch auto-rotation:** On fbdev, touch coordinates are automatically rotated to match the display rotation for non-USB-HID devices (e.g., Goodix, sun4i_ts). USB HID touchscreens (e.g., BTT HDMI) report logical coordinates natively and are not transformed. `HELIX_TOUCH_SWAP_AXES` is still available as a manual override for edge cases.

### `HELIX_FORCE_ROTATION_PROBE`

Force the rotation probe to run on next startup, even if it has already run or a rotation is configured. Useful for testing the probe UI on SDL or re-running on a device.

| Property | Value |
|----------|-------|
| **Values** | `1` (force probe) |
| **Default** | Unset (probe only on first boot, fbdev only, no existing rotation) |
| **Files** | `src/application/application.cpp`, `src/application/display_manager.cpp` |

```bash
# Test the rotation probe on SDL (desktop)
HELIX_FORCE_ROTATION_PROBE=1 ./build/bin/helix-screen --test -vv

# Re-run probe on a device that already has rotation configured
HELIX_FORCE_ROTATION_PROBE=1 ./build/bin/helix-screen
```

**Rotation probe behavior:**
- On first boot with no configured rotation (fbdev only), HelixScreen cycles through 0°, 90°, 180°, 270° showing "Tap anywhere if this text is right-side up" for 5 seconds each
- Two-tap confirmation: after initial tap, shows "Tap again to confirm" (10s timeout) to prevent accidental selection
- Loops continuously until user confirms — does not exit after one cycle
- Saves the confirmed rotation to `/display/rotate` in `helixconfig.json`
- Sets `/display/rotation_probed` flag so it doesn't re-run on subsequent boots
- Skips entirely if: rotation is already configured (config, env var, or CLI), or the probe has already run
- Runs after translations are loaded (Phase 8c) so probe strings are translatable via `lv_tr()`

### `HELIX_DPI`

Override the display DPI (dots per inch). Useful for screens where spacing looks too large or too small at the auto-detected DPI.

| Property | Value |
|----------|-------|
| **Values** | `50` – `500` |
| **Default** | Auto-detected (LVGL default: `130`) |
| **File** | `scripts/helix-launcher.sh`, `src/system/cli_args.cpp` |

```bash
# Force 240 DPI (common for high-density small screens)
HELIX_DPI=240 ./build/bin/helix-screen

# In helixscreen.env (persistent):
HELIX_DPI=240
```

The launcher translates this to `--dpi=<value>` on the CLI. Can also be passed directly: `./build/bin/helix-screen --dpi 240`.

### `HELIX_SDL_DISPLAY`

Select which monitor to use when running with SDL backend on multi-monitor systems.

| Property | Value |
|----------|-------|
| **Values** | Display index (`0`, `1`, `2`, ...) |
| **Default** | `0` (primary display) |
| **File** | `src/main.cpp` |

```bash
# Run on second monitor
HELIX_SDL_DISPLAY=1 ./build/bin/helix-screen
```

### `HELIX_SDL_XPOS` / `HELIX_SDL_YPOS`

Position the SDL window at exact screen coordinates.

| Property | Value |
|----------|-------|
| **Values** | Pixel coordinates (integers) |
| **Default** | Centered on selected display |
| **File** | `src/main.cpp` |

```bash
# Position window at specific coordinates
HELIX_SDL_XPOS=100 HELIX_SDL_YPOS=200 ./build/bin/helix-screen
```

---

## Touch Calibration

### Linear Calibration (env vars)

Simple axis range mapping via LVGL's built-in calibration. Use for devices with known linear offset/scale.

| Variable | Description | Default |
|----------|-------------|---------|
| `HELIX_TOUCH_MIN_X` | Minimum raw X value (maps to screen left) | Auto-detect |
| `HELIX_TOUCH_MAX_X` | Maximum raw X value (maps to screen right) | Auto-detect |
| `HELIX_TOUCH_MIN_Y` | Minimum raw Y value (maps to screen top) | Auto-detect |
| `HELIX_TOUCH_MAX_Y` | Maximum raw Y value (maps to screen bottom) | Auto-detect |
| `HELIX_TOUCH_SWAP_AXES` | Swap X/Y axes (set to "1" to enable). Overrides auto-detection. | Disabled (auto-detected) |

**Usage Notes:**
- All four min/max variables must be set together for calibration to apply
- To invert an axis, swap the min/max values (e.g., `MIN_Y=3200 MAX_Y=900` inverts Y)
- These values override the kernel-reported axis ranges from `EVIOCGABS`
- `HELIX_TOUCH_SWAP_AXES` is now primarily a manual override — the calibration wizard auto-detects swapped axes and saves the flag to config (`/input/calibration/swap_axes`). The env var takes priority over auto-detection if both are set

**Example:**
```bash
# AD5M resistive touchscreen with inverted Y axis
export HELIX_TOUCH_MIN_X=500
export HELIX_TOUCH_MAX_X=3580
export HELIX_TOUCH_MIN_Y=3200  # Higher value = screen top (inverted)
export HELIX_TOUCH_MAX_Y=900
./build/bin/helix-screen
```

### Affine Calibration (config file)

For precise calibration including rotation and skew correction, use the touch calibration wizard. The wizard computes a 6-coefficient affine transform and saves it to the config file at `display.calibration.{a,b,c,d,e,f}`.

**Affine transform formula:**
```
screen_x = a * touch_x + b * touch_y + c
screen_y = d * touch_x + e * touch_y + f
```

The calibration wizard is automatically presented during first-run setup on framebuffer devices. It can also be triggered manually from Settings.

**Note:** There are no environment variable overrides for affine calibration. Edit the config file directly or use the calibration wizard.

### Touch Jitter Filter

Suppresses small coordinate jitter from noisy touch controllers (e.g., Goodix GT9xx) that would otherwise cause stationary taps to be misinterpreted as scroll/swipe gestures.

| Variable | Description | Default |
|----------|-------------|---------|
| `HELIX_TOUCH_JITTER` | Dead-zone threshold in pixels. Coordinate changes within this distance are suppressed. | `15` |

**How it works:** When a finger is pressed, the filter records the initial position. Subsequent coordinate reports within the dead zone are snapped back to the last stable position. Once movement exceeds the threshold, the new position becomes the anchor. On release, the last stable position is reported.

**Config file equivalent:** `/input/jitter_threshold` (integer, default `15`, set to `0` to disable)

**Example:**
```bash
# Increase threshold for a very noisy touchscreen
HELIX_TOUCH_JITTER=25 ./build/bin/helix-screen

# Disable the jitter filter entirely
HELIX_TOUCH_JITTER=0 ./build/bin/helix-screen
```

---

## G-Code Viewer

### `HELIX_GCODE_MODE`

Force the G-code preview rendering mode.

| Property | Value |
|----------|-------|
| **Values** | `2D`, `3D` |
| **Default** | `2D` (TinyGL 3D renderer disabled by default) |
| **Files** | `src/ui_gcode_viewer.cpp`, `src/ui_panel_print_status.cpp`, `src/ui_panel_gcode_test.cpp` |

```bash
# Enable 3D G-code preview (requires TinyGL/OpenGL ES)
HELIX_GCODE_MODE=3D ./build/bin/helix-screen

# Force 2D layer view
HELIX_GCODE_MODE=2D ./build/bin/helix-screen
```

**Note:** 3D mode requires the build to include TinyGL or OpenGL ES support. On platforms without GPU acceleration, 2D mode is recommended for performance.

### `HELIX_FORCE_GCODE_MEMORY_FAIL`

Force the G-code memory safety check to fail, simulating a memory-constrained device like AD5M. Useful for testing thumbnail fallback behavior without deploying to embedded hardware.

| Property | Value |
|----------|-------|
| **Values** | `1` (force failure), unset (normal behavior) |
| **Default** | Unset (normal memory checking) |
| **File** | `src/memory_utils.cpp` |

```bash
# Force memory check to fail - viewer falls back to thumbnail mode
HELIX_FORCE_GCODE_MEMORY_FAIL=1 ./build/bin/helix-screen --test -p print-status -vv
```

**Use case:** Testing that the thumbnail displays immediately when G-code rendering is unavailable, without needing to deploy to memory-constrained hardware.

### `HELIX_GCODE_STREAMING`

Control G-code streaming mode for memory-efficient loading of large files. Streaming loads layers on-demand instead of the entire file at once, enabling 10MB+ G-code files on memory-constrained devices like AD5M.

| Property | Value |
|----------|-------|
| **Values** | `on` (always stream), `off` (always full load), `auto` (calculate based on RAM) |
| **Default** | `auto` |
| **Config** | `gcode_viewer.streaming_mode` in `helixconfig.json` |
| **File** | `src/gcode_streaming_config.cpp` |

**Priority order:**
1. Environment variable (highest) - for testing/debugging
2. Config file setting - for user preference
3. Auto-detection based on available RAM

```bash
# Force streaming mode (useful for testing streaming behavior)
HELIX_GCODE_STREAMING=on ./build/bin/helix-screen --test -p print-status -vv

# Force full load mode (may crash on large files with low RAM!)
HELIX_GCODE_STREAMING=off ./build/bin/helix-screen --test --gcode-file large.gcode -vv

# Use auto-detection (default)
HELIX_GCODE_STREAMING=auto ./build/bin/helix-screen --test -p print-status -vv
```

**Auto-detection thresholds** (at 40% RAM threshold, 15x expansion factor):
| Available RAM | Streaming kicks in at |
|---------------|----------------------|
| 47 MB (AD5M)  | ~1.25 MB |
| 256 MB        | ~6.8 MB |
| 1 GB          | ~27 MB |
| 4 GB          | ~107 MB |

**Related config options:**
- `gcode_viewer.streaming_mode`: `"auto"`, `"on"`, or `"off"`
- `gcode_viewer.streaming_threshold_percent`: 1-90 (default 40)

---

## Bed Mesh

### `HELIX_BED_MESH_2D`

Force the bed mesh visualization to use 2D heatmap mode instead of 3D surface rendering.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Off (3D surface when available) |
| **File** | `src/ui_bed_mesh.cpp` |

```bash
# Force 2D heatmap visualization
HELIX_BED_MESH_2D=1 ./build/bin/helix-screen
```

---

## Mock & Testing

These variables control the mock printer simulation, useful for development and testing without a real printer.

### `HELIX_AMS_GATES`

Set the number of filament gates in the mock AMS (Automatic Material System).

| Property | Value |
|----------|-------|
| **Values** | `1` to `16` |
| **Default** | `4` |
| **File** | `src/main.cpp` |

```bash
# Simulate 8-slot AMS
HELIX_AMS_GATES=8 ./build/bin/helix-screen --test

# Simulate 16-slot MMU
HELIX_AMS_GATES=16 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_AMS`

Select the mock AMS topology/type.

| Property | Value |
|----------|-------|
| **Values** | `afc`, `toolchanger` / `tc`, `mixed`, `multi` |
| **Default** | Happy Hare, LINEAR, 4 slots |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it simulates |
|-------|-------------------|
| *(unset)* | Happy Hare, LINEAR, 4 slots (default constructor) |
| `afc` | AFC Box Turtle, HUB, 4 slots |
| `toolchanger` / `tc` | Tool Changer, PARALLEL topology |
| `mixed` | Box Turtle + 2x OpenAMS, 6 tools |
| `multi` | Box Turtle (4 slots) + Night Owl (2 slots), single toolhead |

```bash
# Simulate AFC Box Turtle
HELIX_MOCK_AMS=afc ./build/bin/helix-screen --test

# Simulate toolchanger
HELIX_MOCK_AMS=toolchanger ./build/bin/helix-screen --test

# Simulate mixed topology (BT + 2x OpenAMS)
HELIX_MOCK_AMS=mixed ./build/bin/helix-screen --test

# Simulate multi-unit (Box Turtle + Night Owl, 6 slots, single toolhead)
HELIX_MOCK_AMS=multi ./build/bin/helix-screen --test
```

**Multi-extruder and tool testing:** Setting `HELIX_MOCK_AMS=toolchanger` also creates multiple tool definitions and extruders in the mock environment. Multiple extruders (extruder, extruder1, etc.) and tools are auto-discovered from Klipper objects at runtime, so no separate env var is needed to control extruder count. The toolchanger mock provides a complete multi-tool, multi-extruder test environment.

### `HELIX_MOCK_AMS_STATE`

Select the mock AMS visual scenario.

| Property | Value |
|----------|-------|
| **Values** | `idle`, `loading`, `error`, `bypass` |
| **Default** | `idle` (slot 0 loaded, slot 3 empty, others available) |
| **File** | `src/printer/ams_backend.cpp` |

| Value | What it shows |
|-------|---------------|
| *(unset)* / `idle` | Default idle state |
| `loading` | Active load in progress with realistic segment animation |
| `error` | Slot errors visible; buffer fault also shown when combined with `afc` mode |
| `bypass` | Bypass mode active |

```bash
# Show error states (slot errors + buffer fault)
HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test

# Show realistic loading animation
HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test

# Show bypass mode
HELIX_MOCK_AMS_STATE=bypass ./build/bin/helix-screen --test

# Combine with topology selection
HELIX_MOCK_AMS=afc HELIX_MOCK_AMS_STATE=error ./build/bin/helix-screen --test
HELIX_MOCK_AMS=mixed HELIX_MOCK_AMS_STATE=loading ./build/bin/helix-screen --test
```


### `HELIX_MOCK_DRYER`

Enable filament dryer simulation in mock mode.

| Property | Value |
|----------|-------|
| **Values** | `1` or `true` |
| **Default** | Disabled |
| **File** | `src/ams_backend.cpp` |

```bash
# Enable mock dryer
HELIX_MOCK_DRYER=1 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_DRYER_SPEED`

Speed multiplier for dryer simulation (for faster testing).

| Property | Value |
|----------|-------|
| **Values** | Integer multiplier (e.g., `2` = 2x speed) |
| **Default** | `1` (real-time) |
| **File** | `src/ams_backend_mock.cpp` |

```bash
# Run dryer simulation at 10x speed
HELIX_MOCK_DRYER=1 HELIX_MOCK_DRYER_SPEED=10 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_SPOOLMAN`

Enable or disable mock Spoolman integration. When disabled, `get_spoolman_status()` reports as disconnected.

| Property | Value |
|----------|-------|
| **Values** | `0` or `off` to disable; any other value keeps enabled |
| **Default** | Enabled (mock Spoolman always connected in test mode) |
| **File** | `src/main.cpp` |

```bash
# Disable mock Spoolman to test "no Spoolman" scenarios
HELIX_MOCK_SPOOLMAN=0 ./build/bin/helix-screen --test
```

### `HELIX_MOCK_FILAMENT_SENSORS`

Configure custom filament sensor configurations for testing.

| Property | Value |
|----------|-------|
| **Values** | Comma-separated `type:name` pairs, or `"none"` |
| **Default** | Single runout switch sensor |
| **File** | `src/moonraker_client_mock.cpp` |

**Sensor Types:**
- `switch` - Simple on/off runout switch
- `motion` - Motion-based encoder sensor

```bash
# Multiple sensors
HELIX_MOCK_FILAMENT_SENSORS="switch:fsensor,motion:encoder" ./build/bin/helix-screen --test

# No sensors
HELIX_MOCK_FILAMENT_SENSORS=none ./build/bin/helix-screen --test
```

### `HELIX_MOCK_FILAMENT_STATE`

Set the initial state of filament sensors.

| Property | Value |
|----------|-------|
| **Values** | `sensor_name:state` (e.g., `fsensor:empty`, `fsensor:detected`) |
| **Default** | Detected |
| **File** | `src/moonraker_client_mock.cpp` |

```bash
# Start with empty filament sensor
HELIX_MOCK_FILAMENT_STATE="fsensor:empty" ./build/bin/helix-screen --test
```

### `HELIX_FORCE_RUNOUT_MODAL`

Force the filament runout guidance modal to appear even when an AMS/MMU system is present. Normally, runout modals are suppressed for AMS systems because filament runout during swaps is expected behavior.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (normal behavior) |
| **Default** | Unset (modal suppressed with AMS) |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Force runout modal with real AMS system
HELIX_FORCE_RUNOUT_MODAL=1 ./build/bin/helix-screen

# In test mode, use --no-ams instead (simpler)
./build/bin/helix-screen --test --no-ams
```

**Note:** In test mode, a mock AMS is created by default (4 gates). Use `--no-ams` flag to disable the mock AMS, which enables runout modal testing without needing this environment variable.

### `MOCK_EMPTY_POWER`

Return an empty power devices list from mock Moonraker API.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Populated power device list |
| **File** | `src/moonraker_api_mock.cpp` |

```bash
# Simulate printer with no controllable power devices
MOCK_EMPTY_POWER=1 ./build/bin/helix-screen --test
```

---

## UI Automation

These variables enable automated testing and CI/CD workflows.

### `HELIX_AUTO_QUIT_MS`

Automatically quit the application after a specified duration.

| Property | Value |
|----------|-------|
| **Values** | `100` to `3600000` (milliseconds) |
| **Default** | No timeout (run indefinitely) |
| **File** | `src/main.cpp` |

```bash
# Quit after 5 seconds
HELIX_AUTO_QUIT_MS=5000 ./build/bin/helix-screen --test

# CI test run (3 second timeout)
HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p home
```

### `HELIX_AUTO_SCREENSHOT`

Automatically capture a screenshot before quitting (use with `HELIX_AUTO_QUIT_MS`).

| Property | Value |
|----------|-------|
| **Values** | `1` (enable) |
| **Default** | Disabled |
| **File** | `src/main.cpp` |

```bash
# Automated screenshot capture
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p motion
```

### `HELIX_BENCHMARK`

Enable frame counting and FPS reporting for performance testing.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/main.cpp` |

```bash
# Run performance benchmark
HELIX_BENCHMARK=1 HELIX_AUTO_QUIT_MS=10000 ./build/bin/helix-screen --test
```

---

## Calibration Auto-Start

These variables auto-start calibration procedures for testing purposes.

### `INPUT_SHAPER_AUTO_START`

Auto-start X-axis input shaper calibration when the panel loads.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/ui_panel_input_shaper.cpp` |

```bash
# Test input shaper panel with auto-start
INPUT_SHAPER_AUTO_START=1 ./build/bin/helix-screen --test -p input-shaper
```

### `SCREWS_AUTO_START`

Auto-start bed screw probing when the screws tilt panel loads.

| Property | Value |
|----------|-------|
| **Values** | Any value (presence enables) |
| **Default** | Disabled |
| **File** | `src/ui_panel_screws_tilt.cpp` |

```bash
# Test screws panel with auto-start
SCREWS_AUTO_START=1 ./build/bin/helix-screen --test -p screws-tilt
```

---

## Development

### `HELIX_HOT_RELOAD`

Enable XML hot reload for live UI editing. When enabled, a background thread polls `ui_xml/` and `ui_xml/components/` every 500ms for file changes. Modified XML components are automatically unregistered and re-registered with LVGL — no restart needed.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled (zero overhead in production) |
| **File** | `src/system/runtime_config.cpp`, `src/application/xml_hot_reloader.cpp` |

```bash
# Enable hot reload during development
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv
```

**How it works:**
1. On startup, scans `ui_xml/` and `ui_xml/components/` and records file modification times
2. Every 500ms, checks all tracked XML files for mtime changes
3. When a change is detected, queues an `lv_xml_component_unregister()` + `lv_xml_register_component_from_file()` on the LVGL main thread
4. Log output confirms the reload: `[HotReload] Reloaded: home_panel (0.4ms)`

**Limitations:**
- **Existing widgets are not rebuilt.** After a reload, navigate away from the current panel and back to see the updated layout. Future versions may add automatic panel refresh.
- **New files are not detected.** Only files present when the app starts are tracked. Adding a new XML file requires a restart.
- **Component re-registration only.** If the XML change requires new subjects, callbacks, or C++ code, a full rebuild + restart is needed.

**Typical workflow:**
```bash
# Terminal 1: Run with hot reload
HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv

# Terminal 2: Edit XML, save, watch the log
vim ui_xml/home_panel.xml
# [HotReload] Reloaded: home_panel (0.3ms)

# Switch panels in the UI to see the new layout
```

---

## Debugging

### `HELIX_DEBUG_SUBJECTS`

Enable verbose subject debugging with stack traces. When LVGL subject type mismatches occur (e.g., trying to read an INT from a STRING subject), this enables detailed diagnostics including the subject name, registration location, and a full stack trace.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled |
| **CLI equivalent** | `--debug-subjects` |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Enable via environment variable
HELIX_DEBUG_SUBJECTS=1 ./build/bin/helix-screen -vv

# Or via command-line flag (equivalent)
./build/bin/helix-screen --debug-subjects -vv
```

**Example output when a type mismatch occurs:**
```
[warning] [LVGL] Subject type is not LV_SUBJECT_TYPE_INT (ptr=0x155008f30, type=7)
[warning]   -> Expected type: LV_SUBJECT_TYPE_INT
[warning]   -> Subject: "print_progress" (STRING) registered at printer_state.cpp:226
[warning]   Stack trace:
[warning]     #0 HomePanel::update_status() + 108
[warning]     #1 HomePanel::on_state_changed(int) + 140
```

**Use cases:**
- Debugging XML binding issues (e.g., `bind_text` on an INT subject)
- Finding subject initialization order problems
- Tracing observer callbacks that fire before subjects are ready

### `HELIX_DEBUG_TOUCHES`

Enable touch point visualization. When enabled, a ripple effect is drawn at each touch point, showing exactly where the system registers touches. Useful for diagnosing touch accuracy issues, verifying calibration, and identifying UI elements that absorb click events.

| Property | Value |
|----------|-------|
| **Values** | `1` (enable), unset (disable) |
| **Default** | Disabled |
| **CLI equivalent** | `--debug-touches` |
| **File** | `src/system/runtime_config.cpp` |

```bash
# Enable via environment variable
HELIX_DEBUG_TOUCHES=1 ./build/bin/helix-screen -vv

# Or via command-line flag (equivalent)
./build/bin/helix-screen --debug-touches -vv

# Combine with test mode for desktop debugging
./build/bin/helix-screen --test --debug-touches -vv
```

**Use cases:**
- Verifying touch calibration accuracy
- Diagnosing buttons or UI elements that don't respond to taps
- Identifying overlapping UI elements that absorb click events
- Confirming extended click areas are working correctly

---

## Deployment

### `HELIX_DATA_DIR`

Override the runtime asset directory. When set, the application `chdir()`s to this path at startup so that all relative asset paths (`ui_xml/`, `assets/`, `config/`) resolve correctly. Use this when assets are installed to a different location than the default (e.g., due to storage constraints on embedded devices, or a custom filesystem layout).

| Property | Value |
|----------|-------|
| **Values** | Absolute path to directory containing `ui_xml/`, `assets/`, `config/` |
| **Default** | Auto-detect from executable path |
| **File** | `src/application/application.cpp` |

```bash
# Assets on a separate partition
HELIX_DATA_DIR=/mnt/data/helixscreen /usr/bin/helix-screen

# FHS-style installation with split layout
HELIX_DATA_DIR=/usr/share/helixscreen /usr/bin/helix-screen

# Systemd service
Environment="HELIX_DATA_DIR=/opt/helixscreen"
```

**Required directory structure:**
```
$HELIX_DATA_DIR/
  ├── ui_xml/          # XML layout files
  ├── assets/          # Images (runtime-loaded only)
  └── config/          # Default configuration
```

**Note:** Fonts compiled into the binary (e.g., `mdi_icons_64`, `noto_sans_*`) work regardless of this setting. Only runtime-loaded files (XML, images) require the data directory.

---

## Logging & Startup

### `HELIX_LOG_LEVEL`

Set the log verbosity level. Preferred over the legacy `HELIX_DEBUG` variable.

| Property | Value |
|----------|-------|
| **Values** | `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` |
| **Default** | `warn` |
| **File** | `scripts/helix-launcher.sh`, `src/system/cli_args.cpp` |

```bash
# In helixscreen.env (persistent, recommended for deployed systems):
HELIX_LOG_LEVEL=debug

# Or inline for one-off debugging:
HELIX_LOG_LEVEL=trace ./build/bin/helix-screen
```

The launcher translates this to `--log-level=<value>` on the CLI. Equivalent CLI flags: `-v` (info), `-vv` (debug), `-vvv` (trace), or `--log-level=<level>`.

**Priority order:**
1. CLI `--log-level` / `-v` flags (highest)
2. `HELIX_LOG_LEVEL` env var
3. `HELIX_DEBUG=1` (legacy, maps to debug level)
4. Config `log_level` in `helixconfig.json`
5. Default: `warn`

### `HELIX_SKIP_SPLASH`

Skip the splash screen on startup. Useful for debugging startup issues or faster iteration.

| Property | Value |
|----------|-------|
| **Values** | `1` (skip) or unset (show splash) |
| **Default** | Unset (splash shown) |
| **File** | `scripts/helix-launcher.sh` |

```bash
# In helixscreen.env:
HELIX_SKIP_SPLASH=1
```

The launcher translates this to `--skip-splash` on the CLI.

---

## Data Paths

### `HELIX_CACHE_DIR`

Override the base directory for all HelixScreen cache/temp files (thumbnails, screenshots, gcode temp files).

| Property | Value |
|----------|-------|
| **Values** | Absolute directory path |
| **Default** | Auto-detected per platform |
| **File** | `src/app_globals.cpp` |

When set, all cache subdirectories are created under `$HELIX_CACHE_DIR/<subdir>`. Platform hooks set this automatically:
- **AD5M**: `/data/helixscreen/cache` (5.8GB ext4 partition)
- **K1/K2**: `/usr/data/helixscreen/cache`

```bash
# Custom cache location
HELIX_CACHE_DIR=/mnt/storage/helix-cache ./build/bin/helix-screen

# AD5M (set automatically by platform hooks)
export HELIX_CACHE_DIR="/data/helixscreen/cache"
```

**Resolution chain** (first match wins):
1. `HELIX_CACHE_DIR` env var
2. Config `/cache/base_directory`
3. Platform-specific compile-time default
4. `XDG_CACHE_HOME/helix/`
5. `$HOME/.cache/helix/`
6. `/var/tmp/helix_`
7. `/tmp/helix_` (last resort)

### `XDG_DATA_HOME`

XDG base directory specification for application data storage.

| Property | Value |
|----------|-------|
| **Values** | Directory path |
| **Default** | `~/.local/share` |
| **File** | `src/logging_init.cpp` |

HelixScreen stores logs and data in `$XDG_DATA_HOME/helixscreen/`.

### `HOME`

User home directory (standard Unix variable).

| Property | Value |
|----------|-------|
| **Values** | Directory path |
| **Default** | (from system) |
| **File** | `src/logging_init.cpp` |

Used as fallback when `XDG_DATA_HOME` is not set.

---

## Build-Time Variables

These are set during compilation via the Makefile system.

| Variable | Purpose | Source |
|----------|---------|--------|
| `HELIX_VERSION` | Version string | `VERSION.txt` |
| `HELIX_VERSION_MAJOR` | Major version number | Parsed from version |
| `HELIX_VERSION_MINOR` | Minor version number | Parsed from version |
| `HELIX_VERSION_PATCH` | Patch version number | Parsed from version |
| `HELIX_GIT_HASH` | Git commit hash (short) | `git describe` |

---

## Preprocessor Flags

These are set at compile time to enable/disable features:

| Flag | Purpose |
|------|---------|
| `HELIX_DISPLAY_SDL` | Enable SDL2 display backend |
| `HELIX_DISPLAY_DRM` | Enable DRM display backend |
| `HELIX_DISPLAY_FBDEV` | Enable framebuffer display backend |
| `HELIX_ENABLE_OPENGLES` | Enable OpenGL ES support for 3D rendering |
| `HELIX_HAS_SYSTEMD` | Enable systemd integration |

---

## Shell Script Variables

### Screenshot Script (`scripts/screenshot.sh`)

| Variable | Purpose | Default |
|----------|---------|---------|
| `HELIX_SCREENSHOT_DISPLAY` | Display number for multi-monitor | `1` |
| `HELIX_SCREENSHOT_OPEN` | Auto-open in Preview (macOS) | Disabled |

```bash
# Take screenshot on display 0 and open it
HELIX_SCREENSHOT_DISPLAY=0 HELIX_SCREENSHOT_OPEN=1 ./scripts/screenshot.sh helix-screen test-output
```

---

## Systemd Service Configuration

When running as a systemd service, environment variables are set in the service file:

```ini
# /etc/systemd/system/helixscreen.service
[Service]
Environment="HELIX_DISPLAY_BACKEND=drm"
# Environment="HELIX_DATA_DIR=/usr/share/helixscreen"
```

See `docs/user/CONFIGURATION.md` for systemd deployment details.

---

## Common Usage Patterns

### Development Testing

```bash
# Basic mock testing
./build/bin/helix-screen --test -vv

# Test specific panel with verbose logging
./build/bin/helix-screen --test -p motion -vv

# Multi-slot AMS testing
HELIX_AMS_GATES=8 ./build/bin/helix-screen --test -p filament
```

### CI/CD Screenshots

```bash
# Automated panel screenshots
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p home
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p motion
HELIX_AUTO_SCREENSHOT=1 HELIX_AUTO_QUIT_MS=3000 ./build/bin/helix-screen --test -p settings
```

### Performance Benchmarking

```bash
# Run 10-second benchmark
HELIX_BENCHMARK=1 HELIX_AUTO_QUIT_MS=10000 ./build/bin/helix-screen --test -p home -v
```

### Hardware Override (Embedded)

```bash
# Override all device paths
HELIX_DISPLAY_BACKEND=drm \
HELIX_DRM_DEVICE=/dev/dri/card1 \
HELIX_TOUCH_DEVICE=/dev/input/event2 \
HELIX_BACKLIGHT_DEVICE=/sys/class/backlight/lcd \
./build/bin/helix-screen
```

---

## See Also

- [Development Guide](DEVELOPMENT.md) - Daily development workflow
- [Build System](BUILD_SYSTEM.md) - Build configuration
- [Testing Guide](TESTING.md) - Test infrastructure
- [User Configuration](user/CONFIGURATION.md) - End-user setup

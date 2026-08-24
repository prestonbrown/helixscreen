# Input Shaper & Calibration System

Developer guide for the input shaper calibration feature, frequency response chart widget, PID tuning panel, and the supporting CSV parser and result cache infrastructure.

**Panels**: Input Shaper (`input_shaper_panel`), PID Calibration (`calibration_pid_panel`)
**Access**: Advanced panel row click

---

## Architecture Overview

The calibration system has four layers:

```
InputShaperPanel (UI overlay, state machine, XML-bound subjects)
  |  Manages user flow: IDLE -> MEASURING -> RESULTS -> Apply/Save
  |
  +-> InputShaperCalibrator (orchestrator, Moonraker API calls)
  |     State machine: IDLE -> CHECKING_ADXL -> TESTING_X/Y -> READY
  |     Coordinates homing, noise check, resonance test, apply, save
  |
  +-> ShaperCsvParser (CSV file reader)
  |     Parses Klipper's calibrate_shaper.py output into frequency bins,
  |     raw PSD data, and per-shaper filtered response curves
  |
  +-> FrequencyResponseChart (LVGL widget, C API)
  |     Line chart with multiple series, peak markers, axis labels,
  |     grid lines, and platform-adaptive downsampling
  |
  +-> InputShaperCache (JSON persistence, 30-day TTL)
        Stores calibration results keyed by printer ID
```

### Data Flow

```
Klipper SHAPER_CALIBRATE
  -> Moonraker WebSocket response (shaper type, freq, metrics)
  -> CSV file on printer filesystem (/tmp/calibration_data_{axis}_{timestamp}.csv)
  -> ShaperCsvParser reads CSV -> ShaperCsvData (frequencies, raw_psd, shaper_curves)
  -> MoonrakerAPI assembles InputShaperResult (all_shapers + freq_response + shaper_curves)
  -> InputShaperPanel populates comparison table + frequency response chart
  -> User taps Apply -> SET_INPUT_SHAPER G-code
  -> User taps Save  -> SAVE_CONFIG (Klipper restart)
```

---

## Key Files

| File | Purpose |
|------|---------|
| `include/calibration_types.h` | Data structures: `InputShaperResult`, `ShaperOption`, `ShaperResponseCurve`, `InputShaperConfig` |
| `include/shaper_csv_parser.h` | CSV parser interface |
| `src/calibration/shaper_csv_parser.cpp` | CSV parser implementation |
| `include/shaper_response.h` | Klipper shaper transfer functions (client-side scoring) |
| `src/calibration/shaper_response.cpp` | Tap definitions + transfer evaluation, pinned against firmware CSV output |
| `include/input_shaper_calibrator.h` | Calibration orchestrator (state machine) |
| `src/calibration/input_shaper_calibrator.cpp` | Orchestrator implementation |
| `include/input_shaper_cache.h` | Result cache with JSON serialization |
| `src/calibration/input_shaper_cache.cpp` | Cache implementation (XDG-compliant paths) |
| `include/ui_frequency_response_chart.h` | Frequency response chart widget (C API) |
| `src/ui/ui_frequency_response_chart.cpp` | Chart implementation with LVGL draw callbacks |
| `include/ui_panel_input_shaper.h` | Input shaper panel (overlay) |
| `src/ui/ui_panel_input_shaper.cpp` | Panel implementation |
| `ui_xml/input_shaper_panel.xml` | XML layout with subject bindings |
| `include/ui_panel_calibration_pid.h` | PID calibration panel |
| `src/ui/ui_panel_calibration_pid.cpp` | PID panel implementation |
| `ui_xml/calibration_pid_panel.xml` | PID panel XML layout |
| `include/platform_capabilities.h` | Hardware tier detection and chart point limits |

### Test Files

| File | Coverage |
|------|----------|
| `tests/unit/test_shaper_csv_parser.cpp` | CSV parsing: realistic data, axis selection, edge cases |
| `tests/unit/test_shaper_response.cpp` | Transfer math: pinned against a K1C firmware CSV column, residual guards |
| `tests/unit/test_frequency_response_chart.cpp` | Chart widget: lifecycle, series, data, downsampling, platform tiers |
| `tests/unit/test_input_shaper_calibrator.cpp` | Calibrator: state machine, callbacks, validation, error handling |
| `tests/unit/test_input_shaper_cache.cpp` | Cache: save/load round-trip, TTL expiry, printer ID matching |
| `tests/unit/test_input_shaper_panel_integration.cpp` | Panel integration tests |
| `tests/unit/test_input_shaper_panel_progress_display.cpp` | Progress display across phases: determinate bar while sweeping, spinner + elapsed-seconds label while analyzing, bar restored on complete |
| `tests/unit/test_input_shaper_chart.cpp` | Chart-specific integration tests |
| `tests/unit/test_moonraker_api_input_shaper.cpp` | Moonraker API shaper endpoints |
| `tests/unit/test_pid_calibrate_collector.cpp` | PID calibration data collection |
| `tests/unit/test_wizard_input_shaper_step.cpp` | Wizard step integration |

---

## CSV Parser (`ShaperCsvParser`)

Parses CSV output from Klipper's calibrate_shaper.py. The CSV is written to `/tmp/calibration_data_{axis}_{timestamp}.csv` on the printer's filesystem.

### Klipper CSV Format

```csv
freq, psd_x, psd_y, psd_z, psd_xyz, zv(59.0), mzv(53.8), ei(56.2), 2hump_ei(71.8), 3hump_ei(89.6)
5.0,  1.234e-03, 2.345e-03, 1.123e-03, 4.702e-03, 0.001, 0.001, 0.001, 0.000, 0.000
10.0, 2.500e-03, 3.100e-03, 1.800e-03, 7.400e-03, 0.002, 0.002, 0.002, 0.001, 0.001
...
```

- Typically ~132 frequency bins from 5-200 Hz
- Raw PSD columns: `psd_x`, `psd_y`, `psd_z`, `psd_xyz`
- Shaper columns: `name(fitted_freq)` format, e.g., `mzv(53.8)`
- Legacy `shapers:` marker column is tolerated but not required

### API

```cpp
namespace helix::calibration {

struct ShaperCsvData {
    std::vector<float> frequencies;                 // Frequency bins (Hz)
    std::vector<float> raw_psd;                     // Raw PSD for requested axis
    std::vector<ShaperResponseCurve> shaper_curves; // Per-shaper filtered responses
};

// Parse CSV for specified axis ('X' or 'Y')
ShaperCsvData parse_shaper_csv(const std::string& csv_path, char axis);
}
```

### How It Works

1. Reads the header line and identifies column indices by name
2. Detects shaper columns by matching `name(freq)` pattern in headers
3. Selects `psd_x` or `psd_y` based on the `axis` parameter
4. Parses each data row, extracting frequency, raw PSD, and all shaper values
5. Returns empty `ShaperCsvData` on any parse failure (missing file, bad format)

### Adding Support for New Shaper Types

The parser auto-discovers shaper columns from the CSV header. Any column matching the `name(frequency)` pattern is parsed as a shaper curve. No code changes needed when Klipper adds new shaper types.

Client-side re-scoring (`shaper_response.h`) is stricter: it ports each shaper's tap definitions, so a genuinely new shaper type needs a new taps function there. Unknown types degrade gracefully — `shaper_transfer_curve()` returns an empty vector and callers hide the verdict row / chart overlay rather than guessing.

---

## Frequency Response Chart Widget

A custom LVGL widget for displaying frequency domain data from accelerometer measurements. Supports multiple data series, peak markers, custom grid/axis draw callbacks, and platform-adaptive rendering.

### C API

```cpp
// Create/destroy
ui_frequency_response_chart_t* ui_frequency_response_chart_create(lv_obj_t* parent);
void ui_frequency_response_chart_destroy(ui_frequency_response_chart_t* chart);

// Series management (max 13 series)
int  ui_frequency_response_chart_add_series(chart, "name", color);
void ui_frequency_response_chart_remove_series(chart, series_id);
void ui_frequency_response_chart_show_series(chart, series_id, visible);

// Data (auto-downsampled if exceeding max_points for current tier)
void ui_frequency_response_chart_set_data(chart, series_id, frequencies, amplitudes, count);
void ui_frequency_response_chart_clear(chart);

// Peak markers (one per series, glow dot effect)
void ui_frequency_response_chart_mark_peak(chart, series_id, peak_freq, peak_amplitude);
void ui_frequency_response_chart_clear_peak(chart, series_id);

// Axis configuration
void ui_frequency_response_chart_set_freq_range(chart, min, max);     // Default: 0-200 Hz
void ui_frequency_response_chart_set_amplitude_range(chart, min, max); // Default: 0-1e9

// Platform adaptation
void   ui_frequency_response_chart_configure_for_platform(chart, tier);
size_t ui_frequency_response_chart_get_max_points(chart);
bool   ui_frequency_response_chart_is_chart_mode(chart);

// LVGL integration
lv_obj_t* ui_frequency_response_chart_get_obj(chart);
```

### Internal Structure

```cpp
struct FrequencySeriesData {
    int id;                      // Series ID (-1 = unused slot)
    char name[32];               // Series name
    lv_color_t color;            // Line color
    bool visible;                // Visibility state
    lv_chart_series_t* lv_series; // LVGL chart series
    bool has_peak;               // Peak marker active
    float peak_freq, peak_amplitude;
    std::vector<float> frequencies, amplitudes; // Stored data
};

struct ui_frequency_response_chart_t {
    lv_obj_t* root;              // Container widget
    lv_obj_t* chart;             // LVGL chart widget
    PlatformTier tier;
    size_t max_points;
    bool chart_mode;
    float freq_min/max, amp_min/max;
    FrequencySeriesData series[MAX_SERIES]; // Up to 13 series
};
```

### Custom Draw Callbacks

The chart uses five LVGL draw event callbacks registered on `LV_EVENT_DRAW_MAIN` and `LV_EVENT_DRAW_POST`:

1. **`draw_freq_grid_lines_cb`** (DRAW_MAIN) -- Subtle grid lines at 25/50/75/100 Hz vertical markers and 4 horizontal amplitude divisions. Color from `elevated_bg` theme token at 15% opacity.

2. **`draw_x_axis_labels_cb`** (DRAW_POST) -- Frequency labels (0 Hz, 50, 100, 150, 200) below the chart content area. Uses `font_small` and `text_muted` theme tokens.

3. **`draw_y_axis_labels_cb`** (DRAW_POST) -- Amplitude labels along the left side at each horizontal division. Auto-formats using scientific notation for large values (e.g., "1e9", "500M").

4. **`draw_peak_dots_cb`** (DRAW_POST) -- For each series with a marked peak, draws a glow circle (10px radius, 30% opacity lighter tint) behind a solid dot (5px radius, series color) at the peak frequency position.

5. **`draw_muted_series_cb`** (DRAW_POST) -- Draws series flagged via `ui_frequency_response_chart_set_series_muted()` as thin (1px, ~66% opacity) polylines from their stored data, instead of through their LVGL chart series (which stays hidden). Used for the live-before "Previous" curve so the fitted curves stay visually dominant. Point placement mirrors the built-in renderer: X from point index, Y from amplitude over the configured range.

### Downsampling

When `set_data()` is called with more points than `max_points` for the current tier, data is automatically downsampled using evenly-spaced point selection. The first and last points are always preserved to maintain frequency range endpoints.

---

## Platform Tiers and Adaptive Complexity

The chart adapts to hardware capabilities detected at runtime via `PlatformCapabilities::detect()`.

### Tier Classification

| Tier | RAM | Cores | Chart Mode | Max Points |
|------|-----|-------|------------|------------|
| **EMBEDDED** | <512 MB or 1 core | Any | Yes (simplified) | 50 |
| **BASIC** | 512 MB - 2 GB or 2-3 cores | 2-3 | Yes (simplified) | 50 |
| **STANDARD** | >=2 GB and 4+ cores | 4+ | Yes (full) | 200 |

### Platform Examples

- **EMBEDDED**: AD5M printer (108 MB RAM) -- chart with 50 downsampled points
- **BASIC**: Raspberry Pi 3 -- chart with 50 points
- **STANDARD**: Raspberry Pi 4/5 (2 GB+), desktop -- full chart with 200 points

### Constants (from `platform_capabilities.h`)

```cpp
static constexpr size_t EMBEDDED_RAM_THRESHOLD_MB = 512;
static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 2048;
static constexpr int    STANDARD_CPU_CORES_MIN = 4;
static constexpr size_t STANDARD_CHART_POINTS = 200;
static constexpr size_t BASIC_CHART_POINTS = 50;
```

Memory impact is approximately 10 KB total including widget overhead. The static 50-132 point chart drawn once is lighter than the temperature graph (1,200 points with live updates), which already runs on EMBEDDED hardware.

---

## Shaper Overlay Chip Toggles

Below each frequency response chart, a row of chip buttons allows toggling individual shaper response curves on and off. This lets users visually compare how different shapers attenuate the resonance peak.

### How Chips Work

Each chip is an LVGL button styled as a pill-shaped toggle:
- **Off state**: transparent background, border outline
- **On state**: primary color background, filled (via `bind_state_if_eq` on `is_{axis}_chip_{n}_active`)
- Label text is dynamically set from the shaper name (e.g., "ZV", "MZV", "EI")

### Chip Subject Bindings

Per chip (11 per axis matching `MAX_SHAPERS`; only chips backed by a CSV curve are shown, via `is_{axis}_num_chips`):
- `is_x_chip_0_label` / `is_y_chip_0_label` -- Shaper name text (string subject)
- `is_x_chip_0_active` / `is_y_chip_0_active` -- Toggle state (int subject, 0=off, 1=on)

### Event Flow

```
User taps chip -> input_shaper_chip_x_2_cb (XML event callback)
  -> InputShaperPanel::handle_chip_x_clicked(2)
    -> toggle_shaper_overlay('X', 2)
      -> flip shaper_visible[2]
      -> ui_frequency_response_chart_show_series(chart, series_id, visible)
      -> lv_subject_set_int(&chips[2].active, visible ? 1 : 0)
      -> update_legend(axis) -- updates legend dot color and label
```

### Shaper Overlay Colors

Colors come from the 11-entry `SHAPER_OVERLAY_COLORS[]` array, shared by chart lines and legend dots. Assignment is positional — each shaper takes the color at its column index in the CSV (`index % 11`), so an 11-shaper Kalico CSV never reuses a color:

| Index | Shaper (stock Klipper) | Color (hex) | Visual |
|-------|------------------------|-------------|--------|
| 0 | ZV | `0x4FC3F7` | Light blue |
| 1 | MZV | `0x66BB6A` | Green |
| 2 | EI | `0xFFA726` | Orange |
| 3 | 2HUMP_EI | `0xAB47BC` | Purple |
| 4 | 3HUMP_EI | `0xEF5350` | Red |
| 5 | — | `0x26C6DA` | Cyan |
| 6 | — | `0xFFEE58` | Yellow |
| 7 | — | `0xEC407A` | Pink |
| 8 | — | `0x7E57C2` | Deep purple |
| 9 | — | `0x8D6E63` | Brown |
| 10 | — | `0x78909C` | Blue-grey |

The Shaper column above is the stock-Klipper mapping (five columns, indices 0-4). Kalico lists its smooth shapers (`smooth_zv` ... `smooth_si`) ahead of the discrete ones, so on a Kalico CSV the names shift down the palette while every column still gets a distinct color.

### Legend

A legend row sits to the right of the chips keying all three curve kinds (a "Relative vibration" caption above the chart itself names the Y quantity):
- Gray dot (0xB0B0B0) + "Measured (shaper off)" label -- the raw PSD series; shaping was not applied when it was captured
- Colored dot + shaper name label (dynamically updated to show the last-toggled-on shaper) -- the fitted shaper chips' predicted residual PSD (transfer curve times the measured PSD)
- Muted dot (`text_muted` token) + "Previous" label -- the live-before setting's predicted residual curve, gated by `is_{axis}_has_delta` so it only appears when a run had a before-config to compare against

---

## Live-Before Delta, Residual Verdict, and Firmware Overwrite Warning

When a calibration run starts, the panel snapshots the currently live `[input_shaper]` config (`snapshot_config_before()`, first axis of a run only) so the results cards can show what actually changed — anchored to live-before rather than to what `SAVE_CONFIG` later writes.

Per axis, when a before-value exists and the result is valid:

- **Delta row** — a single `is_{axis}_delta_text` string ("ei @ 69.8 Hz -> mzv @ 53.8 Hz"), gated by `is_{axis}_has_delta`. The live-before value appears only inside this string (`populate_axis_delta()` formats both halves); there is no separate "was" subject.
- **Verdict row** — "Old setting on today's data: N% residual - now: M%": the old setting's transfer curve re-scored against the freshly measured PSD next to the new fit's residual (`residual_vibration_percent()`, which reproduces klippy's `_estimate_remaining_vibrations()` — thresholded at `psd.max()/20`, shaped spectrum `H*psd` linear — so the verdict is comparable to the vibrations% the comparison table shows). The new side prefers the curve the firmware fitted into the CSV column (the parser shapes it by the raw PSD, so the verdict divides that back out bin by bin); both sides hide via `is_{axis}_has_verdict` when the PSD is missing or a shaper type has no ported taps.
- **Chart overlay** — the old setting's curve is added as a muted series (see `draw_muted_series_cb` above) so its notch is comparable against the fresh fit on the same spectrum.

### Firmware overwrite warning (Creality forks)

Some klippy forks (Creality's K1C build) overwrite the staged X result with the Y-axis values at the end of a Y-axis run and announce it with a console line starting `copy_TestAxis_y_to_x Recommended shaper_type_x = ...`. The collector's marker matcher runs BEFORE the recommendation parser — the marker embeds a real `Recommended shaper_type_x` wording that would otherwise clobber the Y run's own recommendation — and sets `InputShaperResult::x_overwritten_by_firmware` on the Y result. The panel carries that onto the X results card (`fw_overwrite_warn_x`, gated on `is_x_fw_overwrite_warn`) with a warning row: the measured X recommendation shown on screen was discarded by the firmware and the saved config holds Y's values for both axes. A Y-only run has no X card to carry it, so the same warning also renders on the Y card (`fw_overwrite_warn_y`, gated `is_x_fw_overwrite_warn eq 1 and is_results_has_x eq 0`).

The transfer math lives in `src/calibration/shaper_response.cpp` and reproduces the klippy shaper tap definitions plus the damping-aware `_estimate_shaper()` (pessimized over Klipper's `TEST_DAMPING_RATIOS`), which is what the firmware writes into a calibration CSV's fitted columns. `tests/unit/test_shaper_response.cpp` pins it against a real K1C CSV column so the port cannot silently drift from firmware output.

---

## Input Shaper Panel

### State Machine

```
IDLE (0)          Shows instructions, current config, Calibrate X/Y/All buttons
  |
  v
MEASURING (1)     Spinner/progress bar, step labels, Abort button

Both the panel and the first-run wizard step use the same analysis-phase
treatment: the sweep shows a determinate bar; the offline analysis reports no
percent, so the bar hides, a spinner takes over, and a 1 Hz
`helix::ui::ElapsedLabelTimer` (ui_timer_guard.h) refreshes the status label
with "Analyzing data... Ns" on the virtual clock. `PIDCalibrationPanel`'s eta
timer is the same shape and is the intended follow-up consumer of the helper.
  |
  +---> RESULTS (2)   Per-axis result cards with charts, comparison tables, Save button
  |
  +---> ERROR (3)     Error message, troubleshooting tips, Retry/Back buttons
```

State visibility is controlled entirely through XML subject bindings:
```xml
<bind_flag_if_not_eq subject="input_shaper_state" flag="hidden" ref_value="0"/>
```

### Calibrate All Flow

"Calibrate All" runs a sequential two-axis calibration:

1. Pre-flight accelerometer noise check
2. Calibrate X axis (progress: "Step 1 of 2")
3. Store X result, skip pre-flight for Y (accelerometer just verified)
4. Calibrate Y axis (progress: "Step 2 of 2")
5. Show results for both axes

### Abort Behavior

Aborting during calibration sends `M112` (emergency stop) followed by `FIRMWARE_RESTART`. This is necessary because `SHAPER_CALIBRATE` blocks the G-code queue and cannot be cancelled through normal means. Recovery dialog suppression is applied for 15 seconds.

### Klipper Commands

| Command | When |
|---------|------|
| `G28` | Auto-home before calibration if not already homed |
| `MEASURE_AXES_NOISE` | Pre-flight accelerometer check |
| `SHAPER_CALIBRATE AXIS=X` | X axis resonance test |
| `SHAPER_CALIBRATE AXIS=Y` | Y axis resonance test |
| `SET_INPUT_SHAPER SHAPER_TYPE_X=mzv SHAPER_FREQ_X=53.8` | Apply settings |
| `SAVE_CONFIG` | Persist to printer.cfg (restarts Klipper) |

---

## PID Calibration Panel

The PID panel provides an interactive UI for Klipper's `PID_CALIBRATE` command with live temperature graphing.

### State Machine

```
IDLE (0)          Heater selection (extruder/bed), temp adjustment, material presets
  |
  v
CALIBRATING (1)   Live temp graph, progress bar, Abort button
  |
  +---> COMPLETE (3)   Shows Kp/Ki/Kd results with before/after comparison
  |
  +---> SAVING (2)     SAVE_CONFIG in progress
  |
  +---> ERROR (4)      Error message with retry option
```

### Features

- **Heater selection**: Extruder or heated bed
- **Material presets**: PLA (200C), PETG (240C), ABS (250C), PA (260C), TPU (220C) for extruder; PLA (60C), PETG (70C), ABS (100C) for bed
- **Fan control**: Part cooling fan speed slider (extruder only, simulates real print conditions)
- **Live temperature graph**: Reuses the `ui_temp_graph_t` widget to show real-time temperature during calibration
- **Progress tracking**: Works with both standard Klipper (fallback timer) and Kalico (sample callbacks)
- **Before/after comparison**: Fetches old PID values before calibration starts

### Temperature Limits

| Heater | Min | Max | Default |
|--------|-----|-----|---------|
| Extruder | 150C | 280C | 200C |
| Bed | 40C | 110C | 60C |

---

## Interpreting Frequency Response Charts

This section explains what users see in the chart and how to make decisions.

### What the Chart Shows

- **X axis**: Frequency in Hz (0-200 Hz range)
- **Y axis**: Power spectral density (amplitude of vibration at each frequency)
- **Gray line**: Raw measured vibration spectrum from the accelerometer
- **Peak dot**: The dominant resonance frequency (with glow effect)
- **Colored lines**: Filtered response curves showing how each shaper algorithm attenuates vibrations

### Reading the Results

1. **The peak** shows where your printer resonates most. A sharp, narrow peak is typical and easy to compensate. A broad hump or multiple peaks indicates mechanical issues.

2. **Shaper overlays** show the filtered response after each algorithm is applied. The ideal shaper brings the peak down to the noise floor while preserving the rest of the spectrum.

3. **The comparison table** below the chart shows all shaper options with:
   - **Type**: Shaper algorithm (ZV, MZV, EI, 2HUMP_EI, 3HUMP_EI)
   - **Peak Freq**: Fitted frequency for that shaper
   - **Vibration %**: Remaining vibration after filtering (lower is better)
   - **Max Accel**: Maximum recommended acceleration (higher allows faster printing)

### Shaper Type Guide

| Type | Smoothing | Best For |
|------|-----------|----------|
| **ZV** | Minimal | Well-built printers with clean single-peak resonance |
| **MZV** | Moderate | Most printers (recommended default) |
| **EI** | Strong | Printers with moderate vibration issues |
| **2HUMP_EI** | Heavy | Significant vibration problems, multi-peak resonance |
| **3HUMP_EI** | Maximum | Severe mechanical issues (consider fixing hardware first) |

### Quality Levels

| Vibration % | Rating | Meaning |
|-------------|--------|---------|
| < 5% | Excellent | Minimal residual vibration |
| 5-15% | Good | Acceptable vibration level |
| 15-25% | Fair | Mechanical improvements could help |
| > 25% | Poor | Check for mechanical issues |

---

## Result Cache

Calibration results are cached to disk in JSON format to avoid re-running expensive resonance tests.

### Cache Location

Follows XDG Base Directory Specification:
1. `$XDG_CACHE_HOME/helix/input_shaper_cache.json`
2. `$HOME/.cache/helix/input_shaper_cache.json` (fallback)
3. /tmp/helix/input_shaper_cache.json (last resort)

### Cache Format

```json
{
  "version": 1,
  "printer_id": "my-printer-uuid",
  "timestamp": 1707753600,
  "noise_level": 22.5,
  "x_result": {
    "axis": "X",
    "shaper_type": "mzv",
    "shaper_freq": 53.8,
    "max_accel": 4000.0,
    "smoothing": 0.130,
    "vibrations": 1.6,
    "freq_response": [[5.0, 0.00123], [10.0, 0.00250], ...],
    "all_shapers": [
      {"type": "zv", "frequency": 59.0, "vibrations": 5.2, "smoothing": 0.045, "max_accel": 13400}
    ]
  },
  "y_result": { ... }
}
```

### TTL

Cache entries expire after 30 days (`DEFAULT_TTL_DAYS = 30`). Entries are also invalidated if the printer ID does not match.

---

## Demo Mode / Mock Screenshots

Both the Input Shaper panel and PID panel support demo result injection for screenshots and development.

### Input Shaper Demo Mode

```cpp
// Before showing the panel:
auto& panel = get_global_input_shaper_panel();
panel.request_demo_inject();  // Sets pending flag
panel.show();                  // on_activate() calls inject_demo_results()
```

`inject_demo_results()` generates realistic frequency response data matching the mock backend:
- Resonance peak at 53.8 Hz (X) and 48.2 Hz (Y)
- ~50 frequency bins from 5-200 Hz with Lorentzian peak + noise
- All 5 shaper overlay curves with simulated attenuation
- MZV recommended at 53.8 Hz, 1.6% vibration, 4000 mm/s^2 max accel

### PID Demo Mode

```cpp
auto& pid_panel = get_global_pid_cal_panel();
pid_panel.request_demo_inject();
pid_panel.show();
```

### Environment Variable

Set `INPUT_SHAPER_AUTO_START=1` to auto-start X axis calibration when the panel opens (useful for automated testing).

### Screenshot Script

```bash
./scripts/screenshot.sh helix-screen input-shaper input_shaper_panel
```

---

## Developer Guide: Extending Calibration Features

### Adding a New Shaper Type

The CSV parser auto-discovers shaper columns from the header, and the chip toggles and comparison table rows are pre-built for up to 11 shapers (`MAX_SHAPERS` in `ui_panel_input_shaper.h`) — stock Klipper's five plus Kalico's six smooth shapers fit with no code changes. To support more than 11, increase `MAX_SHAPERS` and add the corresponding chip XML elements and callback registrations. The exception is client-side re-scoring: a genuinely new shaper type still needs a new taps function in `shaper_response.h` (see "Adding Support for New Shaper Types" above), or its verdict row and live-before overlays stay hidden.

### Adding a New Calibration Metric

1. Add the field to `ShaperOption` in `calibration_types.h`
2. Update JSON serialization in `input_shaper_cache.cpp` (`shaper_option_to_json` / `shaper_option_from_json`)
3. Add a column to the comparison table XML in `input_shaper_panel.xml`
4. Add a subject binding for the new column in `InputShaperPanel::init_subjects()`
5. Populate the value in `InputShaperPanel::populate_axis_result()`

### Adding a New Calibration Panel

Follow the existing pattern:

1. Create header in `include/ui_panel_calibration_*.h` extending `OverlayBase`
2. Create implementation in `src/ui/ui_panel_calibration_*.cpp`
3. Create XML layout in `ui_xml/calibration_*_panel.xml`
4. Register XML event callbacks at startup (called from `main.cpp` initialization)
5. Register a row click handler for the Advanced panel
6. Use `SubjectManager` for RAII subject cleanup
7. Use `std::shared_ptr<std::atomic<bool>> alive_` pattern for async callback safety ([L012])

### Threading Rules

- All Moonraker API calls happen on background threads (libhv/WebSocket callbacks)
- Never call `lv_subject_set_*()` from background threads
- Use `helix::async::invoke()` (from `async_helpers.h`) to bounce results back to the LVGL thread
- Always capture `alive_` flag in async callbacks and check before accessing panel state

### Testing

Run input shaper tests:
```bash
./build/bin/helix-tests "[shaper_csv]"           # CSV parser tests
./build/bin/helix-tests "[frequency_response_chart]" # Chart widget tests
./build/bin/helix-tests "[calibrator][input_shaper]" # Calibrator tests
./build/bin/helix-tests "[input_shaper]"          # All input shaper tests
```

Test tags:
- `[shaper_csv]` -- CSV parser
- `[shaper_response]` -- Client-side transfer math and residual scoring (pinned against firmware CSV output)
- `[frequency_response_chart]` -- Chart widget lifecycle, series, data, downsampling, platform tiers
- `[calibrator]` -- InputShaperCalibrator state machine
- `[input_shaper]` -- Combined input shaper tests
- `[platform]` -- Platform tier detection and adaptation
- `[downsampling]` -- Data downsampling behavior

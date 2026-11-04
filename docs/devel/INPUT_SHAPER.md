# Input Shaper & Calibration System

Developer guide for the input shaper calibration feature, frequency response chart widget, PID tuning panel, and the supporting CSV parser and result cache infrastructure.

**Panels**: Input Shaper (`input_shaper_panel`), PID Calibration (`calibration_pid_panel`)
**Access**: Advanced panel row click

---

## Architecture Overview

The calibration system has four layers:

```
InputShaperPanel (UI overlay, state machine, XML-bound subjects)
  |  Manages user flow: IDLE -> MEASURING -> RESULTS -> Select/Save
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
        Line chart with multiple series, peak markers, axis labels,
        grid lines, and platform-adaptive downsampling
```

### Data Flow

```
Klipper SHAPER_CALIBRATE
  -> Moonraker WebSocket response (shaper type, freq, metrics)
  -> CSV file on printer filesystem (/tmp/calibration_data_{axis}_{timestamp}.csv)
  -> ShaperCsvParser reads CSV -> ShaperCsvData (frequencies, raw_psd, shaper_curves)
  -> MoonrakerAPI assembles InputShaperResult (all_shapers + freq_response + shaper_curves)
  -> InputShaperPanel populates comparison table + frequency response chart
  -> User taps a shaper chip per axis -> selected_shaper_for('X'/'Y') (radio semantics)
  -> User taps Save  -> shaper_config_edits() builds [input_shaper] ADD_KEY edits
                    -> safe_multi_edit() ends in FIRMWARE_RESTART (no SET_INPUT_SHAPER,
                       no SAVE_CONFIG: the restart applies the config atomically)
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
| `include/ui_frequency_response_chart.h` | Frequency response chart widget (C API) |
| `src/ui/calibration/ui_frequency_response_chart.cpp` | Chart implementation with LVGL draw callbacks |
| `include/ui_panel_input_shaper.h` | Input shaper panel (overlay) |
| `src/ui/calibration/ui_panel_input_shaper.cpp` | Panel implementation |
| `ui_xml/input_shaper_panel.xml` | XML layout with subject bindings |
| `include/ui_shaper_sweep_chart.h`, `src/ui/calibration/ui_shaper_sweep_chart.cpp` | `ShaperSweepChart`: the live chart shared by the panel's MEASURING view and the wizard step |
| `include/live_sweep_preview.h`, `include/sweep_response_estimator.h` | The stream-to-points pipeline behind every live sweep chart (`BELT_TENSION.md` § Live preview) |
| `include/ui_panel_calibration_pid.h` | PID calibration panel |
| `src/ui/calibration/ui_panel_calibration_pid.cpp` | PID panel implementation |
| `ui_xml/calibration_pid_panel.xml` | PID panel XML layout |
| `include/platform_capabilities.h` | Hardware tier detection and chart point limits |

### Test Files

| File | Coverage |
|------|----------|
| `tests/unit/test_shaper_csv_parser.cpp` | CSV parsing: realistic data, axis selection, edge cases |
| `tests/unit/test_shaper_response.cpp` | Transfer math: pinned against a K1C firmware CSV column, residual guards |
| `tests/unit/test_frequency_response_chart.cpp` | Chart widget: lifecycle, series, data, downsampling, platform tiers |
| `tests/unit/test_input_shaper_calibrator.cpp` | Calibrator: state machine, callbacks, validation, error handling |
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

**Axis label gutters.** The chart sizes its own label space from `font_small` instead of fixed pixels (`src/ui/calibration/ui_frequency_response_chart.cpp#apply_axis_pads`). The left pad is `ui_frequency_response_chart_y_gutter_width()`, the widest amplitude label the formatter can emit ("100%", "999k", "0.005", "1e-04", "100e9"), never under 36 px, plus `space_xs`; the bottom pad is `space_sm` plus the font height plus `space_xs`. Both re-run on `LV_EVENT_SIZE_CHANGED`, because a breakpoint change swaps the font under a live chart. Frequency labels are centred on their tick but clamped inside the chart's box (`ui_frequency_response_chart_x_label_center`, half-width from `ui_frequency_response_chart_x_label_half_width`) so "100 Hz" never clips at either edge. Tick spacing follows the range: 5 Hz up to a 40 Hz span, 10 Hz up to 80, so a zoomed belt-tuning zone still shows a few ticks. `ui_frequency_response_chart_get_freq_range()` reads the range back.

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
    FrequencySeriesData series[kMaxSeries]; // Up to 13 series
};
```

### Custom Draw Callbacks

The chart uses five LVGL draw event callbacks registered on `LV_EVENT_DRAW_MAIN` and `LV_EVENT_DRAW_POST`:

1. **`draw_freq_grid_lines_cb`** (DRAW_MAIN) -- Subtle grid lines at 25/50/75/100 Hz vertical markers and 4 horizontal amplitude divisions. Color from `elevated_bg` theme token at 15% opacity.

2. **`draw_x_axis_labels_cb`** (DRAW_POST) -- Frequency labels (0 Hz, 50, 100, 150, 200) below the chart content area. Uses `font_small` and `text_muted` theme tokens.

3. **`draw_y_axis_labels_cb`** (DRAW_POST) -- Amplitude labels along the left side at each horizontal division. Auto-formats using scientific notation for large values (e.g., "1e9", "500M").

4. **`draw_peak_dots_cb`** (DRAW_POST) -- For each series with a marked peak, draws a glow circle (10px radius, 30% opacity lighter tint) behind a solid dot (5px radius, series color) at the peak frequency position.

5. **`draw_muted_series_cb`** (DRAW_POST) -- Draws series flagged via `ui_frequency_response_chart_set_series_muted()` as thin (1px, ~66% opacity) polylines from their stored data, instead of through their LVGL chart series (which stays hidden). Used for the live-before "Previous" curve so the fitted curves stay visually dominant. Point placement mirrors the built-in renderer: X from point index, Y from amplitude over the configured range.

### NaN gaps

A non-finite amplitude is a gap. The built-in LVGL renderer skips it
(`LV_CHART_POINT_NONE`), and the styled and muted passes stroke and fill only the runs
`include/ui_frequency_response_chart.h#fr_chart_finite_runs` returns: every maximal run of
finite values at least two points long. A lone finite point between two gaps draws nothing.
Points are placed by index, so a caller with a partial curve passes the whole frequency grid
with NaN where it has no value (the live preview does exactly this).

### Downsampling

When `set_data()` is called with more points than `max_points` for the current tier, data is automatically downsampled using evenly-spaced point selection. The first and last points are always preserved to maintain frequency range endpoints. Picking
can land on a NaN gap or step over a narrow peak, so the live curve on a BASIC-tier chart is
coarser than the CSV curve that replaces it.

---

## Platform Tiers and Adaptive Complexity

The chart adapts to hardware capabilities detected at runtime via `PlatformCapabilities::detect()`.

### Tier Classification

| Tier | RAM | Cores | Chart Mode | Max Points |
|------|-----|-------|------------|------------|
| **EMBEDDED** | <512 MB or 1 core | Any | Yes (simplified) | 50 |
| **BASIC** | 512-767 MB or 2-3 cores | 2-3 | Yes (simplified) | 50 |
| **STANDARD** | >=768 MB and 4+ cores | 4+ | Yes (full) | 200 |

### Platform Examples

- **EMBEDDED**: AD5M printer (108 MB RAM) -- chart with 50 downsampled points
- **BASIC**: 512 MB or 2-3 core boards -- chart with 50 points
- **STANDARD**: Raspberry Pi 3B (1 GB), Pi 4/5, desktop -- full chart with 200 points

### Constants (from `platform_capabilities.h`)

```cpp
static constexpr size_t EMBEDDED_RAM_THRESHOLD_MB = 512;
static constexpr size_t STANDARD_RAM_THRESHOLD_MB = 768;
static constexpr int    STANDARD_CPU_CORES_MIN = 4;
static constexpr size_t STANDARD_CHART_POINTS = 200;
static constexpr size_t BASIC_CHART_POINTS = 50;
```

Memory impact is approximately 10 KB total including widget overhead. The static 50-132 point chart drawn once is lighter than the temperature graph (1,200 points with live updates), which already runs on EMBEDDED hardware.

---

## Shaper Selection Chips

Below each frequency response chart, a row of chip buttons selects which shaper the results use. One chip per axis is selected: the selection is what **Save** writes, and tapping the selected chip again keeps it selected (`select_shaper_overlay()` implements radio semantics). This lets users compare how different shapers attenuate the resonance peak and commit the one they pick.

### How Chips Work

Each chip is an LVGL button styled as a pill:
- **Unselected**: transparent background, border outline
- **Selected**: primary color background, filled (via `bind_state_if_eq` on `is_{axis}_chip_{n}_active`)
- Label text is dynamically set from the shaper name (e.g., "ZV", "MZV", "EI")

### Chip Subject Bindings

Per chip (11 per axis matching `kMaxShapers`; only chips backed by a CSV curve are shown, via `is_{axis}_num_chips`):
- `is_x_chip_0_label` / `is_y_chip_0_label` -- Shaper name text (string subject)
- `is_x_chip_0_active` / `is_y_chip_0_active` -- Selection state (int subject, 0=unselected, 1=selected)

### Event Flow

```
User taps chip -> input_shaper_chip_x_2_cb (XML event callback)
  -> InputShaperPanel::handle_chip_x_clicked(2)
    -> select_shaper_overlay('X', 2)   (re-click of the selection returns early - kept)
      -> hide previous chip's series + clear its active subject
      -> ui_frequency_response_chart_show_series(chart, series_id, true)
      -> lv_subject_set_int(&chips[2].active, 1)
      -> update_legend(axis) + refresh_axis_display(axis)
```

### Shaper Overlay Colors

Colors come from the 11-entry `kShaperOverlayColors[]` array, shared by chart lines and legend dots. Assignment is positional — each shaper takes the color at its column index in the CSV (`index % 11`), so an 11-shaper Kalico CSV never reuses a color:

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
- Colored dot + shaper name label (dynamically updated to show the selected shaper) -- the fitted shaper chips' predicted residual PSD (transfer curve times the measured PSD)
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

The transfer math lives in `src/calibration/shaper_response.cpp` and reproduces the klippy shaper tap definitions plus the damping-aware `_estimate_shaper()` (pessimized over Klipper's `kTestDampingRatios`), which is what the firmware writes into a calibration CSV's fitted columns. `tests/unit/test_shaper_response.cpp` pins it against a real K1C CSV column so the port cannot silently drift from firmware output.

---

## Input Shaper Panel

### State Machine

```
IDLE (0)          Shows instructions, current config, Calibrate X/Y/All buttons
  |
  v
MEASURING (1)     Progress bar, step labels, Stop button, and the live response chart (§ Live sweep chart)

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

### Live sweep chart

While `SHAPER_CALIBRATE` sweeps, MEASURING draws the measured response as it arrives, in
`chart_container_measuring` (`ui_xml/input_shaper_panel.xml`). The wizard step draws the same
chart in `wizard_input_shaper_chart_host`. Both own a
`include/ui_shaper_sweep_chart.h#ShaperSweepChart`, which wraps a `LiveSweepPreview`
(`BELT_TENSION.md` § Live preview covers the signals, the drive lock, why the console timing
is not drawn, and the threading crossing).

- **Arming.** `InputShaperPanel::begin_axis_preview` and
  `WizardInputShaperStep::begin_axis_preview` build a target with
  `live_sweep_target(preview_facts_, SweepAxis::X or Y)`. The facts (socket path,
  reachability, `[resonance_tester]`) are queried once on entry (`query_preview_facts`). A
  per-axis `accel_chip_x` / `accel_chip_y` picks that axis's chip.
- **Progress.** `ShaperSweepChart::on_progress`: `Sweeping` with a frequency feeds the
  console value (the first one opens the stream), `Analyzing` calls `finish()`, which keeps
  the stream for the last points and then releases the chip before the offline analysis.
- **Column and axis.** The chart plots the axis under test (`SweepColumn::X` or `Y`) on a
  fixed 0 to `kShaperChartMaxHz` (200 Hz) axis shared with the results charts, so a curve
  sits at the same x before and after the swap. The amplitude range follows the tallest
  point with `kShaperChartHeadroom`.
- **Swap.** `show_curve` stops the stream and draws the CSV curve in place. In Calibrate All
  the panel keeps X's CSV curve on screen until Y's first preview points replace it, so the
  chart never blanks between axes. Anywhere else the panel clears the chart on completion.
- **Visibility.** The layout decides whether the chart fits: the XML publishes
  `is_measuring_chart_fits` (panel) and `wizard_input_shaper_chart_fits` (wizard) from the
  breakpoint, and `live_chart_layout_fits` reads them. Where it is hidden nothing arms, so
  nothing streams for a chart nobody sees.
- **Rebuild.** A hot-reload rebuild frees the widget tree without `on_ui_destroyed()`, so
  `InputShaperPanel::on_deactivating` calls it on `DeactivateReason::Rebuild` while the tree
  is still alive: the live chart and both results charts are destroyed with their hosts,
  `create()` makes the results charts again, and the next sweep makes the live chart. The
  wizard step destroys its chart in `cleanup()`, before the wizard deletes the screen.
- **No stream, no chart.** Without a reachable socket, a single chip, or with
  `HELIX_SWEEP_PREVIEW_OFF`, MEASURING shows the progress bar alone.
- **Never a verdict.** Recommendations, the comparison table and the residual scoring read
  the CSV only.

### Stop Behavior

The Stop button (`ui_xml/input_shaper_panel.xml#btn_abort`, the wizard's `ui_xml/wizard_input_shaper.xml#cancel_calibration_btn`) sends `M112` (emergency stop) followed by `FIRMWARE_RESTART`. This is necessary because `SHAPER_CALIBRATE` blocks the G-code queue and cannot be cancelled through normal means. Recovery dialog suppression is applied for 15 seconds. Every control that performs this stop is labelled "Stop" with the `alert_octagon` icon and `variant="danger"`: Input Shaper, the wizard step, PID, Tool Offsets and Belt Tension.

### Klipper Commands

| Command | When |
|---------|------|
| `G28` | Auto-home before calibration if not already homed |
| `MEASURE_AXES_NOISE` | Pre-flight accelerometer check |
| `SHAPER_CALIBRATE AXIS=X` | X axis resonance test |
| `SHAPER_CALIBRATE AXIS=Y` | Y axis resonance test |

Saving never sends `SET_INPUT_SHAPER` or `SAVE_CONFIG`: `handle_save_clicked()` resolves each axis's selection via `selected_shaper_for()`, `shaper_config_edits()` builds `[input_shaper]` `ADD_KEY` edits (`shaper_type_x`, `shaper_freq_x` formatted `%.1f`, same for Y), and the `safe_multi_edit()` call ends in a `FIRMWARE_RESTART` so the written config is applied atomically.

### Where the save writes

`KlipperConfigEditor` (`src/system/klipper_config_editor.cpp`) serves this save, PID/MPC migration (`ui_panel_calibration_pid.cpp`) and the probe overlay's config rows (`safe_edit_value()`). `config_edit_route()` picks one of two writers from `plugin_feature("config_edit", 1)`:

- **Plugin ready:** `plugin_multi_edit()` reads the section with `server.helix.config`, `plan_plugin_config_edits()` turns the edit list into `server.helix.config_edit` edits that each carry the option's current value as `expect`, and the plugin writes them atomically into the file and line that define each option, after a backup under `.helix_backups/`. A revert copies the backups it returned back over their files.
- **Otherwise, and when the plugin path hands back:** the file API. `load_config_files()` downloads the include chain, `resolve_includes()` maps each section to the file of its last definition in Klipper's read order (an include is read where its line stands, so a section an include redefines belongs to the include), `config_edit_target_file()` picks the file, `apply_edits()` edits it in memory, `backup_file()` copies it to `config_backup_path()` (`.helix_backups/<file>.helix_backup`), and the upload replaces it. `restore_backups()` / `cleanup_backups()` find backups by `config_backup_original()`.

The plugin refuses options that live only in the `SAVE_CONFIG` block, so `plan_plugin_config_edits()` routes such a list to the file API. There an `ADD_KEY` of such an option is written into `printer.cfg`, never an include: Klipper reads a regular definition instead of the block's, and its next `SAVE_CONFIG` of that option comments out a duplicate in `printer.cfg` but refuses one in an included file ("conflicts with included value"). A failed `server.helix.config` read also falls back, since nothing is written yet. Both writers end in the same `restart_and_monitor()`: `FIRMWARE_RESTART`, wait for Klipper, and on timeout restore the backups and restart again.

Tests: `[plugin_config_edit]` (`tests/unit/test_config_edit_plugin.cpp`), which also pins `resolve_includes()` against the plugin's reader on the shared fixture tree `tests/fixtures/klipper_config_tree/`.

---

## Probe Preparation (runtime rules)

Some probes need re-zeroing before use (the AD5X load cell's zero drifts when bed screws turn, and its firmware only tares on the print path). Every probing command HelixScreen sends can therefore be wrapped with preparation gcode, driven entirely by runtime config - no rebuild to add a printer (`with_probe_preparation()` in `src/api/moonraker_advanced_api.cpp`, evaluator in `src/printer/probe_preparation.cpp`).

Rules live under a top-level `probe_preparation` array in `assets/config/printer_database.json` (user drop-ins in `printer_database.d/*.json` merge at higher priority, and the writable user path wins over the bundled asset):

```json
"probe_preparation": [
  {
    "id": "zmod_load_cell_tare",
    "when": [{ "type": "macro_match", "field": "macros", "pattern": "LOAD_CELL_TARE" }],
    "operations": ["screws_tilt", "bed_mesh", "probe_accuracy", "z_offset_calibrate"],
    "gcode": ["LOAD_CELL_TARE"],
    "label": "Zeroing load cell",
    "timeout_s": 60
  }
]
```

Invariants:

- `when` is a list evaluated as AND; `when` predicates are the same heuristic types the printer DB already uses (`macro_match`, `object_exists`, `sensor_match`, `hostname_match`). Rules are evaluated in order, first match wins per operation. A rule also lists `skip_if_macro_in`: the resolved macro the operation will actually run, when named there, disables the rule (for firmwares whose own command already prepares the probe).
- Evaluation fails closed: a malformed rule, an unknown predicate type, or a predicate this build cannot evaluate all mean "does not match". Silently ignoring an unrecognized predicate would let a rule fire on the wrong printer.
- A rule is keyed on predicates, not on the printer model name, deliberately: if a firmware renames the macro, the predicate stops matching and the send degrades to the bare command, whereas a name-keyed rule would keep firing a dead macro into the probe script. Two rules with old and new names act as a version detector.
- `timeout_s` is added to the operation's existing budget, never replaces it - a slow preparation cannot eat the probe's own ceiling.
- The wrapped call sites are the four probe entry points: `SCREWS_TILT_CALCULATE`, `BED_MESH_CALIBRATE`, `PROBE_ACCURACY`, and the Z-offset calibration commands (`Z_ENDSTOP_CALIBRATE` / `PROBE_CALIBRATE`).

### Screws-tilt specifics

- **Direction.** The rotation arrows show Klipper's CW/CCW as reported; the Tighten/Loosen wording is derived from the probed corner heights, not from Klipper's direction.
- **Recentering hint.** Klipper reports every screw relative to `screw1`, treating it as a fixed base - but the adjustment vector is only determined up to a uniform translation (adding a constant to every screw moves the bed without changing tilt), so "reference = 0" is an arbitrary choice, not the cheapest one. When all non-reference screws need the same large correction, `suggest_screw_recentering()` offers the one-screw alternative as a hint subject on the panel (`screws_tilt_recenter_hint`).

---

## PID Calibration Panel

The PID panel provides an interactive UI for Klipper's `PID_CALIBRATE` command with live temperature graphing.

### State Machine

```
IDLE (0)          Heater selection (extruder/bed), temp adjustment, material presets
  |
  v
CALIBRATING (1)   Live temp graph, progress bar, Stop button
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

The CSV parser auto-discovers shaper columns from the header, and the chip toggles and comparison table rows are pre-built for up to 11 shapers (`kMaxShapers` in `ui_panel_input_shaper.h`) — stock Klipper's five plus Kalico's six smooth shapers fit with no code changes. To support more than 11, increase `kMaxShapers` and add the corresponding chip XML elements and callback registrations. The exception is client-side re-scoring: a genuinely new shaper type still needs a new taps function in `shaper_response.h` (see "Adding Support for New Shaper Types" above), or its verdict row and live-before overlays stay hidden.

### Adding a New Calibration Metric

1. Add the field to `ShaperOption` in `calibration_types.h`
2. Add a column to the comparison table XML in `input_shaper_panel.xml`
3. Add a subject binding for the new column in `InputShaperPanel::init_subjects()`
4. Populate the value in `InputShaperPanel::populate_axis_result()`

### Adding a New Calibration Panel

Follow the existing pattern:

1. Create header in `include/ui_panel_calibration_*.h` extending `OverlayBase`
2. Create implementation in `src/ui/calibration/ui_panel_calibration_*.cpp`
3. Create XML layout in `ui_xml/calibration_*_panel.xml`
4. Register XML event callbacks at startup (called from `main.cpp` initialization)
5. Register a row click handler for the Advanced panel
6. Use `SubjectManager` for RAII subject cleanup
7. Use `std::shared_ptr<std::atomic<bool>> alive_` pattern for async callback safety ([L012])

### Threading Rules

- All Moonraker API calls happen on background threads (libhv/WebSocket callbacks)
- Never call `lv_subject_set_*()` from background threads
- Use `lifetime_.bg_cb()` (from `async_lifetime_guard.h`) to bounce results back to the LVGL thread
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
- Live sweep chart: `tests/unit/test_shaper_sweep_chart.cpp`, `test_live_sweep_preview.cpp`, `test_sweep_response_estimator.cpp`, `test_mock_accel_batch_source.cpp`; the wizard step's wiring is in `test_wizard_input_shaper_step.cpp`

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_temp_graph.h
 * @brief Real-time temperature graph widget with gradient fills and target lines
 *
 * Usage:
 * @code
 *   // Create and configure
 *   ui_temp_graph_t* graph = ui_temp_graph_create(parent);
 *   int nozzle = ui_temp_graph_add_series(graph, "Nozzle", lv_color_hex(0xFF4444));
 *   int bed = ui_temp_graph_add_series(graph, "Bed", lv_color_hex(0x44FF44));
 *
 *   // Set targets (shows horizontal line)
 *   ui_temp_graph_set_series_target(graph, nozzle, 210.0f, true);
 *
 *   // Update in your temp callback (push mode - one value at a time)
 *   ui_temp_graph_update_series(graph, nozzle, current_temp);
 *
 *   // Cleanup
 *   ui_temp_graph_destroy(graph);
 * @endcode
 *
 * Recommended colors:
 *   - Nozzle:  0xFF4444 (red)
 *   - Bed:     0x44FF44 (green)
 *   - Chamber: 0x4444FF (blue)
 *   - Ambient: 0xFFAA44 (orange)
 *
 * Performance: ~2.4KB per series (300 points × 8 bytes). Update is O(1) circular buffer.
 */

#pragma once

#include "lvgl/lvgl.h"

struct temp_graph_tooltip_t; // owned by src/ui/temp_graph_tooltip.cpp

// Default configuration
#define UI_TEMP_GRAPH_MAX_SERIES 16      // Maximum concurrent temperature series
#define UI_TEMP_GRAPH_DISPLAY_MINUTES 20 // Display period in minutes (primary constant)
// Chart resolution: one point every SAMPLE_INTERVAL_SEC. A 20-min window holds
// far more 1 Hz detail than any panel can resolve (plots are ~330-1024px wide),
// so we store/draw coarser to keep per-frame software-render cost bounded on
// slow 32-bit boards (#979: 1200 points × N series froze the K2 Plus touch UI).
// 20 min / 3 s = 400 points.
#define UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC 3
#define UI_TEMP_GRAPH_DEFAULT_POINTS                                                               \
    (UI_TEMP_GRAPH_DISPLAY_MINUTES * 60 / UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC)
#define UI_TEMP_GRAPH_DISPLAY_MS (UI_TEMP_GRAPH_DISPLAY_MINUTES * 60 * 1000) // Display period in ms
#define UI_TEMP_GRAPH_DEFAULT_MIN_TEMP 0.0f   // Default Y-axis minimum
#define UI_TEMP_GRAPH_DEFAULT_MAX_TEMP 100.0f // Default Y-axis maximum

// Gradient opacity defaults (stock chart style: visible at line, fades to transparent)
#define UI_TEMP_GRAPH_GRADIENT_TOP_OPA 110 // At the line (~43%) — saturated near the curve
#define UI_TEMP_GRAPH_GRADIENT_BOTTOM_OPA LV_OPA_0 // At chart bottom (fully transparent)

/**
 * Feature flags for controlling which chart elements are visible.
 * Used by ui_temp_graph_set_features() / ui_temp_graph_get_features().
 */
enum ui_temp_graph_feature {
    TEMP_GRAPH_FEATURE_LINES = (1 << 0),        // Temperature data lines (always forced on)
    TEMP_GRAPH_FEATURE_TARGET_LINES = (1 << 1), // Target temperature dashed lines (master switch)
    TEMP_GRAPH_FEATURE_LEGEND = (1 << 2),       // Legend chips (color swatch + series name)
    TEMP_GRAPH_FEATURE_Y_AXIS = (1 << 3),       // Y-axis temperature labels
    TEMP_GRAPH_FEATURE_X_AXIS = (1 << 4),       // X-axis time labels
    TEMP_GRAPH_FEATURE_GRADIENTS = (1 << 5),    // Gradient fills under lines
    TEMP_GRAPH_FEATURE_READOUTS = (1 << 6), // Reserved — managed by widget, not by this module
    TEMP_GRAPH_FEATURE_TARGET_HISTORY = (1 << 7), // Sub-flag: time-varying dashed trace vs.
                                                  // legacy horizontal line. Requires TARGET_LINES.
};

#define TEMP_GRAPH_ALL_FEATURES                                                                    \
    (TEMP_GRAPH_FEATURE_LINES | TEMP_GRAPH_FEATURE_TARGET_LINES | TEMP_GRAPH_FEATURE_LEGEND |      \
     TEMP_GRAPH_FEATURE_Y_AXIS | TEMP_GRAPH_FEATURE_X_AXIS | TEMP_GRAPH_FEATURE_GRADIENTS |        \
     TEMP_GRAPH_FEATURE_READOUTS | TEMP_GRAPH_FEATURE_TARGET_HISTORY)

/**
 * Temperature series metadata
 * Stores information about each temperature series (heater/sensor)
 */
struct ui_temp_series_meta_t {
    int id;                          // Series ID (index in series_meta array)
    lv_chart_series_t* chart_series; // LVGL chart series
    lv_color_t color;                // Series color
    char name[32];                   // Series name (e.g., "Nozzle", "Bed")
    bool visible;                    // Show/hide series
    bool show_target;                // Show/hide target temperature line
    float target_temp;               // Latest target temperature (current setpoint).
                                     //   In TARGET_HISTORY mode: drawn as accent tick at right
                                     //   edge; also the value pushed into target_deci_buf on
                                     //   each actuals sample.
                                     //   In legacy mode: drawn as a single horizontal line.
    lv_opa_t gradient_bottom_opa;    // Bottom gradient opacity
    lv_opa_t gradient_top_opa;       // Top gradient opacity
    bool first_value_received;       // True after first real data point (for backfill)

    // Target history (parallel to chart series buffer):
    //   target_deci_buf[i] holds the target × 10 at the time chart sample i was pushed.
    //   0 sentinel = "no target / heater off" (segment break in draw).
    //   Must be (re)allocated to graph->point_count entries by add_series /
    //   set_point_count; freed by remove_series / destroy.
    int16_t* target_deci_buf = nullptr;
    int target_head = 0; // Count of valid samples in [0, target_head).
                         //   Caps at point_count; shift-left on overflow.
};

/**
 * Temperature graph widget
 * Manages an LVGL chart with dynamic series for real-time temperature monitoring
 */
struct ui_temp_graph_t {
    lv_obj_t* chart;                                             // LVGL chart widget
    ui_temp_series_meta_t series_meta[UI_TEMP_GRAPH_MAX_SERIES]; // Series metadata
    int series_count;                                            // Current number of series
    int next_series_id;                                          // Next available series ID
    int point_count;                                             // Number of points per series
    float min_temp;                                              // Y-axis minimum temperature
    float max_temp;                                              // Y-axis maximum temperature

    // X-axis time tracking for rendered labels
    int64_t first_point_time_ms;  // Timestamp of oldest visible point (left edge)
    int64_t latest_point_time_ms; // Timestamp of most recent point (right edge)
    int visible_point_count;      // How many points have actual data

    // Feature flags (controls which chart elements are visible)
    uint32_t features; // Bitmask of ui_temp_graph_feature flags

    // Y-axis label configuration
    float y_axis_increment; // Temperature increment between Y-axis labels (e.g., 80 for
                            // 0°,80°,160°...)
    bool show_y_axis;       // Whether to draw Y-axis labels
    bool show_x_axis;       // Whether to draw X-axis time labels

    // Gradient rendering state (updated when data changes)
    float max_visible_temp; // Maximum temperature currently visible in any series

    // Axis label font (configurable via ui_temp_graph_set_axis_size)
    const lv_font_t* axis_font; // Font for X/Y axis labels (default: font_small)
    int32_t y_axis_width;       // Width reserved for Y-axis labels

    // Theme change observer (re-applies chart colors on theme toggle)
    lv_observer_t* theme_observer;

    // Cached theme colors for draw callbacks (avoid per-frame theme lookups)
    lv_color_t cached_grid_color;
    lv_color_t cached_graph_bg;

    // Gradient cache: render the per-column software gradient once, blit on redraws
    // where nothing changed. See #979 (K2 Plus touch freeze from per-frame gradient).
    lv_obj_t* gradient_canvas;         // hidden lv_canvas used purely as an offscreen render target
    lv_draw_buf_t* gradient_cache_buf; // ARGB8888 buffer backing gradient_canvas
    int32_t gradient_cache_w;          // cached content width  (realloc buf when viewport changes)
    int32_t gradient_cache_h;          // cached content height
    bool gradient_cache_dirty;         // true => recompute on next draw; false => blit cached buf
    // Content signature of the last gradient actually rendered into the buffer.
    // A pushed sample shifts the curve and so dirties the cache, but the RENDERED
    // result is frequently identical - most obviously on an idle printer, where a
    // flat trace shifted by one column is the same picture. Hashing the render's
    // inputs lets an unchanged frame skip the per-column fill entirely. Not valid
    // until a render has happened, and reset whenever the buffer is reallocated
    // (a fresh buffer is uninitialised, so a matching hash must NOT skip it).
    uint64_t gradient_cache_sig = 0;
    bool gradient_cache_sig_valid = false;
    // Per-instance, deliberately NOT static: several graphs are alive at once
    // (the home tile, the print-status card, the overlay), so process-wide
    // counters would blend their timings into a mean that describes none of
    // them. Also what the unit tests assert the skip against.
    uint64_t gradient_render_count = 0;
    uint64_t gradient_skip_count = 0;
    uint64_t gradient_render_us_total = 0;

    // Tap-to-caption state. nullptr = tooltip disabled (the default). Owned and
    // defined by temp_graph_tooltip.cpp so no tooltip state leaks into this struct.
    struct temp_graph_tooltip_t* tooltip = nullptr;
};

/**
 * Core API
 */

/**
 * Create a new temperature graph widget
 *
 * @param parent Parent LVGL object
 * @return Pointer to graph structure (NULL on error)
 */
ui_temp_graph_t* ui_temp_graph_create(lv_obj_t* parent);

/**
 * Destroy the temperature graph widget
 *
 * @param graph Graph instance to destroy
 */
void ui_temp_graph_destroy(ui_temp_graph_t* graph);

/**
 * Get the underlying LVGL chart widget (for custom styling)
 *
 * @param graph Graph instance
 * @return LVGL chart object
 */
lv_obj_t* ui_temp_graph_get_chart(ui_temp_graph_t* graph);

/**
 * Check if a graph is valid and ready for updates
 * Returns false if graph is NULL or the underlying chart widget has been destroyed
 */
static inline bool ui_temp_graph_is_valid(ui_temp_graph_t* graph) {
    return graph && graph->chart;
}

/**
 * Series Management API
 */

/**
 * Add a new temperature series to the graph
 *
 * @param graph Graph instance
 * @param name Series name (max 31 chars)
 * @param color Series color (for line and gradient)
 * @return Series ID (>=0 on success, -1 on error)
 */
int ui_temp_graph_add_series(ui_temp_graph_t* graph, const char* name, lv_color_t color);

/**
 * Remove a temperature series from the graph
 *
 * @param graph Graph instance
 * @param series_id Series ID to remove
 */
void ui_temp_graph_remove_series(ui_temp_graph_t* graph, int series_id);

/**
 * Show or hide a temperature series
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param visible true to show, false to hide
 */
void ui_temp_graph_show_series(ui_temp_graph_t* graph, int series_id, bool visible);

/**
 * Data Update API
 */

/**
 * Add a single temperature point to a series (push mode)
 * Uses circular buffer with shift update mode
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param temp Temperature value
 */
void ui_temp_graph_update_series(ui_temp_graph_t* graph, int series_id, float temp);

/**
 * Add a single temperature point with timestamp (push mode)
 * Uses circular buffer with shift update mode. Timestamp is used for X-axis labels.
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param temp Temperature value
 * @param timestamp_ms Unix timestamp in milliseconds
 */
void ui_temp_graph_update_series_with_time(ui_temp_graph_t* graph, int series_id, float temp,
                                           int64_t timestamp_ms);

/**
 * Replace all data points for a series (array mode)
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param temps Array of temperature values
 * @param count Number of temperatures in array
 */
void ui_temp_graph_set_series_data(ui_temp_graph_t* graph, int series_id, const float* temps,
                                   int count);

/**
 * Clear all data points in the graph (all series)
 */
void ui_temp_graph_clear(ui_temp_graph_t* graph);

/**
 * Clear data points for a specific series
 *
 * @param graph Graph instance
 * @param series_id Series ID
 */
void ui_temp_graph_clear_series(ui_temp_graph_t* graph, int series_id);

/**
 * Target Temperature API
 */

/**
 * Set target temperature and visibility for a series
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param target Target temperature (in same units as data)
 * @param show true to show target line, false to hide
 */
void ui_temp_graph_set_series_target(ui_temp_graph_t* graph, int series_id, float target,
                                     bool show);

/**
 * Show or hide target temperature line for a series
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param show true to show, false to hide
 */
void ui_temp_graph_show_target(ui_temp_graph_t* graph, int series_id, bool show);

/**
 * Update the "current target" for a series WITHOUT pushing into the history buffer.
 *
 * The buffer push happens implicitly on the next actuals sample (via
 * ui_temp_graph_update_series). This setter is what live target observers should
 * call: it stages the new setpoint so the next pushed sample records it.
 *
 * @param graph     Graph instance
 * @param series_id Series ID
 * @param target    Target temperature in degrees
 * @param show      true to show target trace, false to hide
 */
void ui_temp_graph_set_current_target(ui_temp_graph_t* graph, int series_id, float target,
                                      bool show);

/**
 * Replace all data points AND target history for a series in one call (array mode).
 *
 * Mirror of ui_temp_graph_set_series_data but populates both buffers. The two
 * arrays MUST have the same length. Used by backfill paths that have parallel
 * temp+target history (e.g., TempSample replay from TemperatureHistoryManager).
 *
 * @param graph     Graph instance
 * @param series_id Series ID
 * @param temps     Array of temperature values (degrees)
 * @param targets   Array of target values (degrees, 0 = heater off)
 * @param count     Number of entries in BOTH arrays
 */
void ui_temp_graph_set_series_data_with_targets(ui_temp_graph_t* graph, int series_id,
                                                const float* temps, const float* targets,
                                                int count);

/**
 * Set X-axis timestamp tracking fields directly (graph-level, no chart-buffer side
 * effects).
 *
 * Use after a bulk replay via set_series_data / set_series_data_with_targets to
 * populate first_point_time_ms / latest_point_time_ms / visible_point_count without
 * the unwanted side effects of update_series_with_time (which would push another
 * sample and — when first_value_received was false — wipe the chart via
 * lv_chart_set_all_values).
 *
 * @param graph        Graph instance
 * @param first_ts_ms  Timestamp of oldest replayed sample (left edge)
 * @param last_ts_ms   Timestamp of newest replayed sample (right edge)
 * @param count        Number of samples replayed
 */
void ui_temp_graph_set_axis_timestamps(ui_temp_graph_t* graph, int64_t first_ts_ms,
                                       int64_t last_ts_ms, int count);

/**
 * Configuration API
 */

/**
 * Set the Y-axis temperature range
 *
 * @param graph Graph instance
 * @param min Minimum temperature
 * @param max Maximum temperature
 */
void ui_temp_graph_set_temp_range(ui_temp_graph_t* graph, float min, float max);

/**
 * Set the number of data points per series (capacity)
 *
 * @param graph Graph instance
 * @param count Number of points (e.g., 300 for 5 min @ 1s)
 */
void ui_temp_graph_set_point_count(ui_temp_graph_t* graph, int count);

/**
 * Set gradient opacity for a series
 *
 * @param graph Graph instance
 * @param series_id Series ID
 * @param bottom_opa Bottom opacity (0-255, default 60% = LV_OPA_60)
 * @param top_opa Top opacity (0-255, default 10% = LV_OPA_10)
 */
void ui_temp_graph_set_series_gradient(ui_temp_graph_t* graph, int series_id, lv_opa_t bottom_opa,
                                       lv_opa_t top_opa);

/**
 * Set Y-axis label configuration
 *
 * @param graph Graph instance
 * @param increment Temperature increment between labels (e.g., 80 for 0°, 80°, 160°, ...)
 * @param show Whether to show Y-axis labels
 */
void ui_temp_graph_set_y_axis(ui_temp_graph_t* graph, float increment, bool show);

/**
 * Set axis label font size
 *
 * @param graph Graph instance
 * @param size Size name: "xs" (font_xs), "sm" (font_small, default), "md" (font_body), "lg"
 * (font_heading)
 */
void ui_temp_graph_set_axis_size(ui_temp_graph_t* graph, const char* size);

/**
 * Feature Flags API
 */

/**
 * Set which chart features are visible
 * LINES flag is always forced on even if not included in the bitmask.
 *
 * @param graph Graph instance
 * @param features Bitmask of ui_temp_graph_feature flags
 */
void ui_temp_graph_set_features(ui_temp_graph_t* graph, uint32_t features);

/**
 * Get the current feature flags
 *
 * @param graph Graph instance
 * @return Bitmask of ui_temp_graph_feature flags (0 if graph is NULL)
 */
uint32_t ui_temp_graph_get_features(ui_temp_graph_t* graph);

/**
 * Tap-to-Caption API
 */

/**
 * Enable or disable tap-to-caption on this graph. Disabled by default.
 *
 * Enabling makes the chart clickable and installs the press + draw callbacks.
 * Disabling clears any pinned caption. Only the full-screen temperature overlay
 * enables this: the home-panel mini graph uses a tap to open that overlay.
 */
void ui_temp_graph_set_tooltip_enabled(ui_temp_graph_t* graph, bool enabled);

/** @return true if tap-to-caption is enabled on this graph. */
bool ui_temp_graph_tooltip_is_enabled(const ui_temp_graph_t* graph);

/**
 * Gradient cache test/introspection API (#979)
 *
 * The gradient cache recomputes its offscreen buffer only when the data or
 * viewport actually changed, then blits the cached buffer on subsequent redraws.
 * These accessors expose the dirty state so tests can pin the "recompute only
 * when necessary" guarantee without a live draw context.
 */

/**
 * @return true if the gradient cache needs recompute on the next draw
 *         (false => the cached buffer will be blitted). Returns false if graph is NULL.
 */
bool ui_temp_graph_gradient_cache_is_dirty(const ui_temp_graph_t* graph);

/**
 * Mark the gradient cache clean, simulating a completed render. Test seam:
 * the real recompute happens inside the draw callback (needs a draw context),
 * so tests use this to model the post-render state.
 */
void ui_temp_graph_mark_gradient_cache_clean(ui_temp_graph_t* graph);

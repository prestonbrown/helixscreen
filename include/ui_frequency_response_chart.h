// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_frequency_response_chart.h
 * @brief Frequency response chart widget for input shaper calibration visualization
 *
 * Displays frequency domain data from accelerometer measurements during input
 * shaper calibration. Supports multiple data series, peak marking, and automatic
 * hardware adaptation based on platform tier.
 *
 * Usage:
 * @code
 *   // Create and configure
 *   ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(parent);
 *   ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
 *
 *   // Add series for X and Y axis measurements
 *   int x_id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
 *   int y_id = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));
 *
 *   // Set frequency response data
 *   ui_frequency_response_chart_set_data(chart, x_id, frequencies, amplitudes, count);
 *
 *   // Mark detected resonance peak
 *   ui_frequency_response_chart_mark_peak(chart, x_id, 45.0f, 5e8f);
 *
 *   // Cleanup
 *   ui_frequency_response_chart_destroy(chart);
 * @endcode
 *
 * Hardware Adaptation:
 * - EMBEDDED tier: Simplified chart with max 50 data points (same as BASIC)
 * - BASIC tier: Simplified chart with max 50 data points
 * - STANDARD tier: Full chart with max 200 data points and animations
 *
 * When set_data is called with more points than max_points for the current tier,
 * the data is automatically downsampled while preserving frequency range endpoints.
 * Downsampling picks points, so on BASIC a narrow peak can fall between picks.
 *
 * A non-finite amplitude is a gap: the built-in renderer skips it
 * (LV_CHART_POINT_NONE) and the styled and muted passes stroke and fill only
 * fr_chart_finite_runs(). Points are still placed by index, so a caller
 * drawing a partial curve passes the full grid with NaN where it has no value.
 *
 * @see platform_capabilities.h for tier definitions
 * @see docs/devel/INPUT_SHAPER.md for design rationale
 */

#include "lvgl/lvgl.h"
#include "platform_capabilities.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ui_frequency_response_chart_t;

// ============================================================================
// Creation/Destruction
// ============================================================================

/**
 * @brief Create a new frequency response chart widget
 *
 * Creates a chart configured for displaying frequency domain data from input
 * shaper calibration. Until configure_for_platform() it is EMBEDDED in table
 * mode (no lv_chart, every point kept).
 *
 * @param parent Parent LVGL object (must not be NULL)
 * @return Pointer to chart structure, or NULL on error
 */
ui_frequency_response_chart_t* ui_frequency_response_chart_create(lv_obj_t* parent);

/**
 * @brief Destroy the frequency response chart widget
 *
 * Frees all resources including series data, peak markers, and LVGL objects.
 * Safe to call with NULL pointer.
 *
 * @param chart Chart instance to destroy (can be NULL)
 */
void ui_frequency_response_chart_destroy(ui_frequency_response_chart_t* chart);

// ============================================================================
// Series Management
// ============================================================================

/**
 * @brief Add a new data series to the chart
 *
 * Creates a new series for displaying frequency response data. Each series
 * can have its own data, visibility state, and peak marker.
 *
 * @param chart Chart instance
 * @param name Series name for legend/tooltip (max 31 chars, must not be NULL)
 * @param color Series line color
 * @return Series ID (>= 0) on success, -1 on error
 */
int ui_frequency_response_chart_add_series(ui_frequency_response_chart_t* chart, const char* name,
                                           lv_color_t color);

/**
 * @brief Remove a data series from the chart
 *
 * Removes the series and frees its data. Safe to call with invalid series_id.
 *
 * @param chart Chart instance
 * @param series_id Series ID returned from add_series
 */
void ui_frequency_response_chart_remove_series(ui_frequency_response_chart_t* chart, int series_id);

/**
 * @brief Show or hide a data series
 *
 * Controls visibility without removing the series data.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param visible true to show, false to hide
 */
void ui_frequency_response_chart_show_series(ui_frequency_response_chart_t* chart, int series_id,
                                             bool visible);

/**
 * @brief Change a series' color, for callers whose colors are theme tokens
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param color New line color
 */
// NAMESPACE_OK: matches this file's C-style chart API
void ui_frequency_response_chart_set_series_color(ui_frequency_response_chart_t* chart,
                                                  int series_id, lv_color_t color);

/**
 * @brief Report a series' color
 *
 * @return The series color, or black for an unknown series
 */
// NAMESPACE_OK: matches this file's C-style chart API
lv_color_t ui_frequency_response_chart_get_series_color(ui_frequency_response_chart_t* chart,
                                                        int series_id);

/**
 * @brief Render a series as a muted background curve
 *
 * The chart's built-in renderer draws every series at the same line width and
 * opacity, which is right for the fitted curves but wrong for a reference
 * curve (e.g. the live-before shaper setting) that must stay visually
 * secondary to the new fit. A muted series keeps its data, visibility flag,
 * and color, but is drawn by the chart's own draw-post pass as a thin
 * (1px), reduced-opacity line instead of through its LVGL series.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param muted true to draw thin and translucent, false to restore the
 *              built-in rendering
 */
void ui_frequency_response_chart_set_series_muted(ui_frequency_response_chart_t* chart,
                                                  int series_id, bool muted);

/**
 * @brief Report whether a series is currently rendered as muted
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @return true when muted, false when not (or for an unknown series)
 */
bool ui_frequency_response_chart_is_series_muted(ui_frequency_response_chart_t* chart,
                                                 int series_id);

/**
 * @brief Show or hide the amplitude labels and their left gutter
 *
 * Relative comparisons (two curves against each other) read better without
 * absolute PSD numbers; the plot then takes the gutter's width.
 */
// NAMESPACE_OK: matches this file's C-style chart API
void ui_frequency_response_chart_set_y_labels_visible(ui_frequency_response_chart_t* chart,
                                                      bool visible);

/**
 * @brief Width of the amplitude-label gutter for @p font
 *
 * The widest label the gutter draws ("100%", "99.5k", "99.5m", "99.5u",
 * "100e9"), never less than 36 px. The chart adds space_xs on top of it.
 */
// NAMESPACE_OK: matches this file's C-style chart API
int32_t ui_frequency_response_chart_y_gutter_width(const lv_font_t* font);

/**
 * @brief The amplitude gutter's labels, top (amp_max) to bottom (amp_min) (pure)
 *
 * divisions + 1 labels at amp_max - i * (amp_max - amp_min) / divisions.
 * Percent mode prints "%.0f%%". Otherwise one SI suffix for the whole axis,
 * picked from the larger end's magnitude (e9, M, k, none, m, u), and the
 * fewest decimals (0 to 2) that keep every label distinct; an exact 0 reads "0".
 * {0, 0.022}: "22m", "16.5m", "11m", "5.5m", "0".
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::vector<std::string> fr_chart_amplitude_labels(float amp_min, float amp_max,
                                                                 int divisions, bool percent);

/**
 * @brief Half the box each frequency label is centred in, for @p font
 *
 * Room for "100 Hz" on one line, never less than 32 px.
 */
// NAMESPACE_OK: matches this file's C-style chart API
int32_t ui_frequency_response_chart_x_label_half_width(const lv_font_t* font);

/**
 * @brief Centre of a frequency label's box for a tick at @p tick_x
 *
 * The tick itself, moved in just enough that a box of @p half_w either side
 * stays within [@p x_min, @p x_max]; the chart clips anything past its edges.
 */
// NAMESPACE_OK: matches this file's C-style chart API
int32_t ui_frequency_response_chart_x_label_center(int32_t tick_x, int32_t half_w, int32_t x_min,
                                                   int32_t x_max);

/**
 * @brief Label the amplitude gridlines as percentages
 *
 * For callers that normalise their data to 0-100 and set the amplitude range
 * to match: each gridline reads "100%", "75%", ... instead of a raw value.
 */
// NAMESPACE_OK: matches this file's C-style chart API
void ui_frequency_response_chart_set_y_labels_percent(ui_frequency_response_chart_t* chart,
                                                      bool percent);

/**
 * @brief Report whether a series is currently visible
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @return true when visible, false when hidden (or for an unknown series)
 */
// NAMESPACE_OK: matches this file's C-style chart API
bool ui_frequency_response_chart_is_series_visible(ui_frequency_response_chart_t* chart,
                                                   int series_id);

/**
 * @brief Per-series draw style for the custom draw pass
 *
 * A series whose effective style has glow or fill set is drawn entirely by
 * the chart's draw-post pass (its built-in lv_chart series is hidden); a
 * default-styled series keeps the built-in renderer. Muted series ignore
 * style and keep their 1px treatment.
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
struct FrChartSeriesStyle {
    int32_t line_width = 2;
    bool glow = false; ///< two wider, fainter strokes under the line
    bool fill = false; ///< vertical gradient from 35% of the series color to transparent
};

/// The measured curve's look on every chart that draws one, live and final:
/// filled and glowing, so a live preview hands over to its CSV curve seamlessly.
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
inline constexpr FrChartSeriesStyle kFrMeasuredCurveStyle{3, true, true};

/**
 * @brief What a tier may draw (pure)
 *
 * Glow needs STANDARD with animations; every other tier drops it. Fill is
 * kept on every tier.
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] FrChartSeriesStyle fr_chart_effective_style(const FrChartSeriesStyle& requested,
                                                          helix::PlatformTier tier,
                                                          bool supports_animations);

/**
 * @brief Set a series' draw style, gated by the configured tier
 *
 * Stores fr_chart_effective_style(style, tier, supports_animations) where the
 * tier and animation support are those captured at configure_for_platform()
 * time; a later configure_for_platform() re-derives it from the requested
 * style. A series whose effective style has glow or fill switches to the
 * chart's custom draw pass; the default style keeps the built-in LVGL series.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param style Requested style
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
void ui_frequency_response_chart_set_series_style(ui_frequency_response_chart_t* chart,
                                                  int series_id, const FrChartSeriesStyle& style);

/**
 * @brief Get the effective style stored for a series
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @return Effective style, or the default for an unknown series
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] FrChartSeriesStyle
ui_frequency_response_chart_get_series_style(ui_frequency_response_chart_t* chart, int series_id);

/**
 * @brief Report whether a series is drawn by the custom draw pass
 *
 * True for muted series and for series whose effective style has glow or
 * fill; false means the built-in lv_chart series is the renderer.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @return true when the chart's own draw callback renders the series
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
bool ui_frequency_response_chart_series_uses_custom_draw(ui_frequency_response_chart_t* chart,
                                                         int series_id);

/**
 * @brief Show a vertical sweep cursor at freq_hz
 *
 * Tints the swept region [range min, freq_hz] and draws a 2px vertical line
 * in the given color, in the chart's draw-post pass.
 *
 * @param chart Chart instance
 * @param freq_hz Cursor frequency in Hz
 * @param color Cursor color
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
void ui_frequency_response_chart_set_cursor(ui_frequency_response_chart_t* chart, float freq_hz,
                                            lv_color_t color);

/**
 * @brief Remove the sweep cursor
 *
 * @param chart Chart instance
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
void ui_frequency_response_chart_clear_cursor(ui_frequency_response_chart_t* chart);

/**
 * @brief A run of consecutive finite amplitudes, [begin, end)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
struct FrChartRun {
    size_t begin = 0;
    size_t end = 0;
};

/**
 * @brief Where a series can be stroked (pure)
 *
 * Every maximal run of finite values at least two points long, in order. A
 * non-finite amplitude is a gap: no segment or fill touches it, and a lone
 * finite point between two gaps draws nothing.
 * {1, 2, NaN, 3, 4, 5, NaN} -> {[0,2), [3,6)}.
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::vector<FrChartRun> fr_chart_finite_runs(const float* amps, size_t count);

/// Fill opacity right under the curve; it fades linearly to transparent at the
/// plot bottom.
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
inline constexpr lv_opa_t kFrFillTopOpa = 90;

/// Gradient start opacities for one segment's two fill triangles: @c first is
/// (top_a, top_b, bottom under a), whose top row is the segment's higher point;
/// @c second is (top_b, bottom under b, bottom under a), whose top row is top_b.
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
struct FrFillSegmentOpa {
    lv_opa_t first;
    lv_opa_t second;
};

/**
 * @brief Fill gradient for one curve segment, top rows @p top_a_y / @p top_b_y (pure)
 *
 * The tint follows the curve: kFrFillTopOpa at the segment's higher point,
 * however low on the plot that is, fading to 0 at @p bottom_y. LVGL spans a
 * triangle's vertical gradient over that triangle's own bounding box, so the
 * lower-topped triangle starts at the ramp's value for its top row and both
 * triangles shade every row they share alike.
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] FrFillSegmentOpa fr_chart_fill_segment_opa(int32_t top_a_y, int32_t top_b_y,
                                                         int32_t bottom_y);

// ============================================================================
// Data Management
// ============================================================================

/**
 * @brief Set frequency response data for a series
 *
 * Replaces all data points for the specified series. If count exceeds the
 * maximum points for the current platform tier, data is automatically
 * downsampled while preserving frequency range endpoints.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param frequencies Array of frequency values in Hz (must not be NULL)
 * @param amplitudes Array of amplitude values (must not be NULL, same length as frequencies)
 * @param count Number of data points
 */
void ui_frequency_response_chart_set_data(ui_frequency_response_chart_t* chart, int series_id,
                                          const float* frequencies, const float* amplitudes,
                                          size_t count);

/**
 * @brief Clear all data from all series
 *
 * Removes data points but keeps series definitions intact.
 *
 * @param chart Chart instance
 */
void ui_frequency_response_chart_clear(ui_frequency_response_chart_t* chart);

// ============================================================================
// Peak Marking
// ============================================================================

/**
 * @brief Mark a resonance peak on a series
 *
 * Displays a vertical marker and/or annotation at the specified frequency.
 * Each series can have one peak marker; calling again updates the marker.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 * @param peak_freq Frequency of the peak in Hz
 * @param peak_amplitude Amplitude value at the peak
 */
void ui_frequency_response_chart_mark_peak(ui_frequency_response_chart_t* chart, int series_id,
                                           float peak_freq, float peak_amplitude);

/**
 * @brief Clear the peak marker for a series
 *
 * Removes the peak marker if present. Safe to call when no marker exists.
 *
 * @param chart Chart instance
 * @param series_id Series ID
 */
void ui_frequency_response_chart_clear_peak(ui_frequency_response_chart_t* chart, int series_id);

/**
 * @brief One marker on a series' curve
 *
 * number > 0 draws a filled dot in the series color carrying that number (1-9);
 * number == 0 draws a hollow ring. The marker sits on the curve at the data
 * point nearest freq_hz.
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
struct FrChartMarker {
    float freq_hz = 0.0f;
    int number = 0;
};

/**
 * @brief Replace a series' markers (count 0 clears them)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
void ui_frequency_response_chart_set_markers(ui_frequency_response_chart_t* chart, int series_id,
                                             const FrChartMarker* markers, size_t count);

/**
 * @brief Amplitudes the series currently draws, after any downsampling
 *        (empty for an unknown series)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::vector<float>
ui_frequency_response_chart_get_series_amplitudes(ui_frequency_response_chart_t* chart,
                                                  int series_id);

/**
 * @brief Markers currently set on a series (empty for an unknown series)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::vector<FrChartMarker>
ui_frequency_response_chart_get_markers(ui_frequency_response_chart_t* chart, int series_id);

// ============================================================================
// Configuration
// ============================================================================

/**
 * @brief Set the frequency axis range
 *
 * @param chart Chart instance
 * @param min Minimum frequency in Hz (typically 0)
 * @param max Maximum frequency in Hz (typically 200 for input shapers)
 */
void ui_frequency_response_chart_set_freq_range(ui_frequency_response_chart_t* chart, float min,
                                                float max);

/**
 * @brief The frequency axis range last set, as {min, max} ({0, 0} for null)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::pair<float, float>
ui_frequency_response_chart_get_freq_range(ui_frequency_response_chart_t* chart);

/**
 * @brief Set the amplitude axis range
 *
 * @param chart Chart instance
 * @param min Minimum amplitude value
 * @param max Maximum amplitude value
 */
void ui_frequency_response_chart_set_amplitude_range(ui_frequency_response_chart_t* chart,
                                                     float min, float max);

/**
 * @brief The amplitude axis range last set, as {min, max} ({0, 0} for null)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
[[nodiscard]] std::pair<float, float>
ui_frequency_response_chart_get_amplitude_range(ui_frequency_response_chart_t* chart);

/**
 * @brief Get the underlying LVGL object
 *
 * Returns the root LVGL object for custom positioning/styling.
 *
 * @param chart Chart instance
 * @return LVGL object, or NULL if chart is NULL
 */
lv_obj_t* ui_frequency_response_chart_get_obj(ui_frequency_response_chart_t* chart);

// ============================================================================
// Hardware Adaptation
// ============================================================================

/**
 * @brief Configure chart for a specific platform tier
 *
 * Adjusts chart rendering mode and data point limits based on hardware
 * capabilities:
 * - EMBEDDED: Simplified chart, max 50 points (same as BASIC)
 * - BASIC: Simplified chart, max 50 points
 * - STANDARD: Full chart with animations, max 200 points
 *
 * @param chart Chart instance
 * @param tier Platform tier from PlatformCapabilities
 */
void ui_frequency_response_chart_configure_for_platform(ui_frequency_response_chart_t* chart,
                                                        helix::PlatformTier tier);

/**
 * @brief Configure chart for a platform tier with explicit animation support
 *
 * Same as the two-argument form, but the caller supplies the platform's
 * animation support instead of it being read from
 * PlatformCapabilities::detect() (tests control it this way). The value is
 * captured once here and gates series styles set afterwards.
 *
 * @param chart Chart instance
 * @param tier Platform tier from PlatformCapabilities
 * @param supports_animations whether the platform may draw animated effects (glow)
 */
// NAMESPACE_OK: joins this header's global ui_frequency_response_chart_* API
void ui_frequency_response_chart_configure_for_platform(ui_frequency_response_chart_t* chart,
                                                        helix::PlatformTier tier,
                                                        bool supports_animations);

/**
 * @brief Get maximum data points for current configuration
 *
 * Returns the maximum number of chart points based on the configured
 * platform tier. Data beyond this limit will be downsampled.
 *
 * @param chart Chart instance
 * @return Max points (200 for STANDARD, 50 for BASIC/EMBEDDED, 0 before configuring)
 */
size_t ui_frequency_response_chart_get_max_points(ui_frequency_response_chart_t* chart);

/**
 * @brief Check if chart mode is enabled
 *
 * Returns whether the chart is rendering as a graphical chart (true) or
 * as a data table (false). Table mode lasts only until configure_for_platform(),
 * which selects chart mode on every tier.
 *
 * @param chart Chart instance
 * @return true if chart mode, false if table mode or NULL chart
 */
bool ui_frequency_response_chart_is_chart_mode(ui_frequency_response_chart_t* chart);

// ============================================================================
// Axis unit and shaded x-bands
// ============================================================================

/// One shaded stretch of the x axis, in the axis' own unit.
struct FrChartBand {
    float x_lo;
    float x_hi;
};

/// The unit the first x tick carries ("Hz" by default, "mm/s" for a speed sweep).
void ui_frequency_response_chart_set_x_unit(ui_frequency_response_chart_t* chart, const char* unit);
const char* ui_frequency_response_chart_get_x_unit(ui_frequency_response_chart_t* chart);

/// Shade these stretches of the plot behind the series; nullptr or 0 clears them.
void ui_frequency_response_chart_set_bands(ui_frequency_response_chart_t* chart,
                                           const FrChartBand* bands, size_t count);
size_t ui_frequency_response_chart_get_band_count(ui_frequency_response_chart_t* chart);

/// The text of one x tick: the value, and the unit on the first tick only.
std::string ui_frequency_response_chart_format_x_label(float value, bool first, const char* unit);

/// The pixel columns a band covers on a plot spanning [x1, x2] for values
/// [min, max], clipped to the plot; nullopt when it lies wholly outside.
std::optional<std::pair<int32_t, int32_t>>
ui_frequency_response_chart_band_pixels(FrChartBand band, float min, float max, int32_t x1,
                                        int32_t x2);

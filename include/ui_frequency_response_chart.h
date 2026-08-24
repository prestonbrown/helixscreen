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
 *
 * @see platform_capabilities.h for tier definitions
 * @see docs/INPUT_SHAPING_IMPLEMENTATION.md for design rationale
 */

#include "lvgl/lvgl.h"
#include "platform_capabilities.h"

#include <cstddef>

// Forward declaration
struct ui_frequency_response_chart_t;

// ============================================================================
// Creation/Destruction
// ============================================================================

/**
 * @brief Create a new frequency response chart widget
 *
 * Creates a chart configured for displaying frequency domain data from input
 * shaper calibration. Initially in EMBEDDED mode until configured with
 * configure_for_platform().
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
 * @brief Set the amplitude axis range
 *
 * @param chart Chart instance
 * @param min Minimum amplitude value
 * @param max Maximum amplitude value
 */
void ui_frequency_response_chart_set_amplitude_range(ui_frequency_response_chart_t* chart,
                                                     float min, float max);

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
 * @brief Get maximum data points for current configuration
 *
 * Returns the maximum number of chart points based on the configured
 * platform tier. Data beyond this limit will be downsampled.
 *
 * @param chart Chart instance
 * @return Max points (200 for STANDARD, 50 for BASIC/EMBEDDED)
 */
size_t ui_frequency_response_chart_get_max_points(ui_frequency_response_chart_t* chart);

/**
 * @brief Check if chart mode is enabled
 *
 * Returns whether the chart is rendering as a graphical chart (true) or
 * as a data table (false). Table mode is used on EMBEDDED tier hardware.
 *
 * @param chart Chart instance
 * @return true if chart mode, false if table mode or NULL chart
 */
bool ui_frequency_response_chart_is_chart_mode(ui_frequency_response_chart_t* chart);

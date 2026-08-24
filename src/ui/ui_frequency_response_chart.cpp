// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_frequency_response_chart.cpp
 * @brief Frequency response chart widget implementation
 *
 * Displays frequency domain data from accelerometer measurements during input
 * shaper calibration. Supports multiple data series, peak marking, and automatic
 * hardware adaptation based on platform tier.
 */

#include "ui_frequency_response_chart.h"

#include "display_settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

// Maximum number of series supported: raw PSD + every fitted shaper curve the
// panel can overlay (MAX_SHAPERS = 11) + the live-before "was" curve. Fitted
// CSVs from Kalico carry up to 11 columns; too few slots here silently drops
// the last-added series (chips without curves, no "was" overlay).
static constexpr int MAX_SERIES = 13;

// Maximum series name length
static constexpr size_t MAX_NAME_LEN = 32;

/**
 * @brief Internal series data structure
 */
struct FrequencySeriesData {
    int id = -1;                   ///< Series ID (-1 = unused slot)
    char name[MAX_NAME_LEN] = {0}; ///< Series name
    lv_color_t color = {};         ///< Line color
    bool visible = true;           ///< Visibility state
    bool muted = false;            ///< Draw thin/translucent instead of via the LVGL series
    lv_chart_series_t* lv_series = nullptr; ///< LVGL chart series (chart mode only)

    // Peak marker data
    bool has_peak = false;
    float peak_freq = 0.0f;
    float peak_amplitude = 0.0f;
    size_t peak_idx = 0; ///< Cached index into frequencies/amplitudes for draw alignment

    // Stored data (for table mode or re-rendering)
    std::vector<float> frequencies;
    std::vector<float> amplitudes;
};

/**
 * @brief Main chart structure
 */
struct ui_frequency_response_chart_t {
    lv_obj_t* root = nullptr;  ///< Container widget
    lv_obj_t* chart = nullptr; ///< LVGL chart widget (nullptr in table mode)

    helix::PlatformTier tier = helix::PlatformTier::EMBEDDED;
    size_t max_points = 0;
    bool chart_mode = false;

    float freq_min = 0.0f;
    float freq_max = 200.0f;
    float amp_min = 0.0f;
    float amp_max = 1e9f;

    FrequencySeriesData series[MAX_SERIES];
    int next_series_id = 0;

    // Peak dot pulse animation state (0-255 opacity cycle)
    int32_t pulse_phase = 255; ///< Current glow opacity (animated)
    lv_anim_t pulse_anim = {};
    bool pulse_active = false;
};

// ============================================================================
// Internal helpers
// ============================================================================

/**
 * @brief Find series by ID
 * @return Pointer to series data or nullptr if not found
 */
static FrequencySeriesData* find_series(ui_frequency_response_chart_t* chart, int series_id) {
    if (!chart || series_id < 0) {
        return nullptr;
    }

    for (int i = 0; i < MAX_SERIES; i++) {
        if (chart->series[i].id == series_id) {
            return &chart->series[i];
        }
    }
    return nullptr;
}

/**
 * @brief Find first available series slot
 * @return Slot index or -1 if none available
 */
static int find_empty_slot(ui_frequency_response_chart_t* chart) {
    if (!chart) {
        return -1;
    }

    for (int i = 0; i < MAX_SERIES; i++) {
        if (chart->series[i].id == -1) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Downsample data arrays to fit within max_points
 *
 * Uses simple linear interpolation, preserving first and last points
 * to maintain frequency range endpoints.
 */
static void downsample_data(const float* src_freqs, const float* src_amps, size_t src_count,
                            std::vector<float>& dst_freqs, std::vector<float>& dst_amps,
                            size_t max_points) {
    if (src_count == 0 || max_points == 0) {
        dst_freqs.clear();
        dst_amps.clear();
        return;
    }

    if (src_count <= max_points) {
        // No downsampling needed
        dst_freqs.assign(src_freqs, src_freqs + src_count);
        dst_amps.assign(src_amps, src_amps + src_count);
        return;
    }

    // Handle edge case: single output point
    if (max_points == 1) {
        dst_freqs.resize(1);
        dst_amps.resize(1);
        // Take last point to preserve frequency range endpoint
        dst_freqs[0] = src_freqs[src_count - 1];
        dst_amps[0] = src_amps[src_count - 1];
        return;
    }

    // Downsample by selecting evenly spaced points
    dst_freqs.resize(max_points);
    dst_amps.resize(max_points);

    for (size_t i = 0; i < max_points; i++) {
        // Map output index to input index
        size_t src_idx;
        if (i == max_points - 1) {
            // Ensure last point is exactly the last source point
            src_idx = src_count - 1;
        } else {
            src_idx = (i * (src_count - 1)) / (max_points - 1);
        }

        dst_freqs[i] = src_freqs[src_idx];
        dst_amps[i] = src_amps[src_idx];
    }
}

/**
 * @brief Update LVGL chart with series data
 */
static void update_chart_series(ui_frequency_response_chart_t* chart, FrequencySeriesData* series) {
    if (!chart || !series || !chart->chart || !series->lv_series) {
        return;
    }

    // Match chart point count to actual data size so SHIFT mode fills all slots
    size_t count = series->frequencies.size();
    uint32_t current_points = lv_chart_get_point_count(chart->chart);
    if (count > 0 && static_cast<uint32_t>(count) != current_points) {
        lv_chart_set_point_count(chart->chart, static_cast<uint32_t>(count));
    }

    // Clear existing data
    lv_chart_set_all_values(chart->chart, series->lv_series, LV_CHART_POINT_NONE);
    for (size_t i = 0; i < count; i++) {
        // Scale amplitude to chart range (LVGL chart uses int32_t)
        float amp = series->amplitudes[i];
        float amp_range = chart->amp_max - chart->amp_min;
        int32_t scaled;
        if (amp_range > 0) {
            scaled = static_cast<int32_t>(((amp - chart->amp_min) / amp_range) * 1000.0f);
        } else {
            scaled = 0;
        }
        lv_chart_set_next_value(chart->chart, series->lv_series, scaled);
    }

    lv_chart_refresh(chart->chart);
}

// ============================================================================
// Creation/Destruction
// ============================================================================

ui_frequency_response_chart_t* ui_frequency_response_chart_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[FreqChart] NULL parent");
        return nullptr;
    }

    auto chart_ptr = std::make_unique<ui_frequency_response_chart_t>();
    if (!chart_ptr) {
        spdlog::error("[FreqChart] Failed to allocate chart structure");
        return nullptr;
    }

    ui_frequency_response_chart_t* chart = chart_ptr.get();

    // Initialize all series slots to unused
    for (int i = 0; i < MAX_SERIES; i++) {
        chart->series[i].id = -1;
    }

    // Create root container
    chart->root = lv_obj_create(parent);
    if (!chart->root) {
        spdlog::error("[FreqChart] Failed to create root container");
        return nullptr;
    }

    // Basic styling for container
    lv_obj_set_size(chart->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(chart->root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart->root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chart->root, LV_OPA_TRANSP, LV_PART_MAIN);

    // Store pointer in user data for retrieval
    lv_obj_set_user_data(chart->root, chart);

    // Default to EMBEDDED tier (configure_for_platform will set up properly)
    chart->tier = helix::PlatformTier::EMBEDDED;
    chart->max_points = 0;
    chart->chart_mode = false;
    chart->chart = nullptr;

    spdlog::debug("[FreqChart] Created frequency response chart");

    return chart_ptr.release();
}

void ui_frequency_response_chart_destroy(ui_frequency_response_chart_t* chart) {
    if (!chart) {
        return;
    }

    // Stop pulse animation before cleanup
    if (chart->pulse_active) {
        lv_anim_delete(chart, nullptr);
        chart->pulse_active = false;
    }

    // Transfer to RAII wrapper for automatic cleanup
    std::unique_ptr<ui_frequency_response_chart_t> chart_ptr(chart);

    // Remove all series
    for (int i = 0; i < MAX_SERIES; i++) {
        if (chart_ptr->series[i].id != -1 && chart_ptr->series[i].lv_series && chart_ptr->chart) {
            lv_chart_remove_series(chart_ptr->chart, chart_ptr->series[i].lv_series);
        }
        chart_ptr->series[i].frequencies.clear();
        chart_ptr->series[i].amplitudes.clear();
    }

    // Delete root widget (also deletes chart if present)
    if (chart_ptr->root) {
        lv_obj_del(chart_ptr->root);
    }

    spdlog::trace("[FreqChart] Destroyed");
}

// ============================================================================
// Series Management
// ============================================================================

int ui_frequency_response_chart_add_series(ui_frequency_response_chart_t* chart, const char* name,
                                           lv_color_t color) {
    if (!chart) {
        spdlog::error("[FreqChart] NULL chart");
        return -1;
    }

    if (!name) {
        spdlog::error("[FreqChart] NULL name");
        return -1;
    }

    int slot = find_empty_slot(chart);
    if (slot == -1) {
        spdlog::error("[FreqChart] No available series slots");
        return -1;
    }

    FrequencySeriesData* series = &chart->series[slot];
    series->id = chart->next_series_id++;
    series->color = color;
    series->visible = true;
    // A slot previously used by a muted series (the live-before overlay) must
    // not stay muted: the new series would be double-drawn (1px copy over its
    // LVGL series) and could never be hidden by a chip toggle.
    series->muted = false;
    series->has_peak = false;
    series->lv_series = nullptr;
    series->frequencies.clear();
    series->amplitudes.clear();

    // Copy name with truncation
    strncpy(series->name, name, MAX_NAME_LEN - 1);
    series->name[MAX_NAME_LEN - 1] = '\0';

    // Create LVGL series if in chart mode
    if (chart->chart_mode && chart->chart) {
        series->lv_series = lv_chart_add_series(chart->chart, color, LV_CHART_AXIS_PRIMARY_Y);
        if (series->lv_series) {
            lv_chart_set_all_values(chart->chart, series->lv_series, LV_CHART_POINT_NONE);
        }
    }

    spdlog::trace("[FreqChart] Added series {} '{}' (slot {}, color 0x{:06X})", series->id,
                  series->name, slot, lv_color_to_u32(color) & 0xFFFFFF);

    return series->id;
}

void ui_frequency_response_chart_remove_series(ui_frequency_response_chart_t* chart,
                                               int series_id) {
    if (!chart) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    // Remove LVGL series if present
    if (series->lv_series && chart->chart) {
        lv_chart_remove_series(chart->chart, series->lv_series);
    }

    // Clear data
    series->frequencies.clear();
    series->amplitudes.clear();

    // Mark slot as unused
    series->id = -1;
    series->lv_series = nullptr;

    spdlog::debug("[FreqChart] Removed series {}", series_id);
}

void ui_frequency_response_chart_show_series(ui_frequency_response_chart_t* chart, int series_id,
                                             bool visible) {
    if (!chart) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    series->visible = visible;

    // Update LVGL series visibility if in chart mode. Muted series never use
    // their LVGL series (the draw-post pass renders them), so theirs stays
    // hidden regardless of the visibility flag.
    if (chart->chart && series->lv_series && !series->muted) {
        lv_chart_hide_series(chart->chart, series->lv_series, !visible);
        lv_obj_invalidate(chart->chart);
    } else if (chart->chart) {
        lv_obj_invalidate(chart->chart);
    }

    spdlog::debug("[FreqChart] Series {} visibility: {}", series_id, visible);
}

void ui_frequency_response_chart_set_series_muted(ui_frequency_response_chart_t* chart,
                                                  int series_id, bool muted) {
    if (!chart) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    series->muted = muted;

    if (chart->chart && series->lv_series) {
        // Muted series are drawn by draw_muted_series_cb(); keep the built-in
        // LVGL series hidden so the curve is not drawn twice.
        lv_chart_hide_series(chart->chart, series->lv_series, muted ? true : !series->visible);
        lv_obj_invalidate(chart->chart);
    }

    spdlog::debug("[FreqChart] Series {} muted: {}", series_id, muted);
}

bool ui_frequency_response_chart_is_series_muted(ui_frequency_response_chart_t* chart,
                                                 int series_id) {
    const FrequencySeriesData* series = find_series(chart, series_id);
    return series ? series->muted : false;
}

// ============================================================================
// Data Management
// ============================================================================

void ui_frequency_response_chart_set_data(ui_frequency_response_chart_t* chart, int series_id,
                                          const float* frequencies, const float* amplitudes,
                                          size_t count) {
    if (!chart) {
        return;
    }

    if (!frequencies || !amplitudes || count == 0) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    // Store data (possibly downsampled) for chart rendering
    if (chart->max_points > 0 && count > chart->max_points) {
        downsample_data(frequencies, amplitudes, count, series->frequencies, series->amplitudes,
                        chart->max_points);
        spdlog::debug("[FreqChart] Downsampled series {} from {} to {} points", series_id, count,
                      series->frequencies.size());
    } else if (chart->max_points > 0) {
        series->frequencies.assign(frequencies, frequencies + count);
        series->amplitudes.assign(amplitudes, amplitudes + count);
    } else {
        // EMBEDDED mode (max_points = 0): store all data for table view
        series->frequencies.assign(frequencies, frequencies + count);
        series->amplitudes.assign(amplitudes, amplitudes + count);
    }

    // Update LVGL chart if in chart mode
    if (chart->chart_mode && chart->chart && series->lv_series) {
        update_chart_series(chart, series);
    }

    spdlog::debug("[FreqChart] Set data for series {}: {} points", series_id, count);
}

void ui_frequency_response_chart_clear(ui_frequency_response_chart_t* chart) {
    if (!chart) {
        return;
    }

    for (int i = 0; i < MAX_SERIES; i++) {
        if (chart->series[i].id != -1) {
            chart->series[i].frequencies.clear();
            chart->series[i].amplitudes.clear();
            chart->series[i].has_peak = false;

            if (chart->chart && chart->series[i].lv_series) {
                lv_chart_set_all_values(chart->chart, chart->series[i].lv_series,
                                        LV_CHART_POINT_NONE);
            }
        }
    }

    // Stop pulse animation since all data is cleared
    if (chart->pulse_active) {
        lv_anim_delete(chart, nullptr);
        chart->pulse_active = false;
    }

    if (chart->chart) {
        lv_chart_refresh(chart->chart);
    }

    spdlog::debug("[FreqChart] Cleared all data");
}

// ============================================================================
// Peak Marking
// ============================================================================

void ui_frequency_response_chart_mark_peak(ui_frequency_response_chart_t* chart, int series_id,
                                           float peak_freq, float peak_amplitude) {
    if (!chart) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    series->has_peak = true;
    series->peak_freq = peak_freq;
    series->peak_amplitude = peak_amplitude;

    // Cache the data point index closest to peak frequency for draw alignment
    series->peak_idx = 0;
    float best_dist = std::numeric_limits<float>::max();
    for (size_t j = 0; j < series->frequencies.size(); j++) {
        float d = std::abs(series->frequencies[j] - peak_freq);
        if (d < best_dist) {
            best_dist = d;
            series->peak_idx = j;
        }
    }

    // Start pulse animation if animations are enabled
    if (!chart->pulse_active &&
        helix::DisplaySettingsManager::instance().get_animations_enabled()) {
        lv_anim_init(&chart->pulse_anim);
        lv_anim_set_var(&chart->pulse_anim, chart);
        lv_anim_set_values(&chart->pulse_anim, 80, 255);
        lv_anim_set_duration(&chart->pulse_anim, 1200);
        lv_anim_set_playback_duration(&chart->pulse_anim, 1200);
        lv_anim_set_repeat_count(&chart->pulse_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&chart->pulse_anim, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&chart->pulse_anim, [](void* var, int32_t val) {
            auto* c = static_cast<ui_frequency_response_chart_t*>(var);
            c->pulse_phase = val;
            if (c->chart) {
                lv_obj_invalidate(c->chart);
            }
        });
        lv_anim_start(&chart->pulse_anim);
        chart->pulse_active = true;
    }

    // Invalidate chart to redraw with peak marker
    if (chart->chart) {
        lv_obj_invalidate(chart->chart);
    }

    spdlog::debug("[FreqChart] Marked peak for series {}: {:.1f} Hz @ {:.2e}", series_id, peak_freq,
                  peak_amplitude);
}

void ui_frequency_response_chart_clear_peak(ui_frequency_response_chart_t* chart, int series_id) {
    if (!chart) {
        return;
    }

    FrequencySeriesData* series = find_series(chart, series_id);
    if (!series) {
        return;
    }

    series->has_peak = false;

    // Stop pulse animation if no series have peaks remaining
    if (chart->pulse_active) {
        bool any_peak = false;
        for (int j = 0; j < MAX_SERIES; j++) {
            if (chart->series[j].id != -1 && chart->series[j].has_peak) {
                any_peak = true;
                break;
            }
        }
        if (!any_peak) {
            lv_anim_delete(chart, nullptr);
            chart->pulse_active = false;
        }
    }

    if (chart->chart) {
        lv_obj_invalidate(chart->chart);
    }

    spdlog::debug("[FreqChart] Cleared peak for series {}", series_id);
}

// ============================================================================
// Configuration
// ============================================================================

void ui_frequency_response_chart_set_freq_range(ui_frequency_response_chart_t* chart, float min,
                                                float max) {
    if (!chart) {
        return;
    }

    chart->freq_min = min;
    chart->freq_max = max;

    // Update chart X-axis range if in chart mode
    // Note: LVGL charts don't have a direct X-axis range for line charts,
    // but we store it for potential custom drawing/labels

    spdlog::debug("[FreqChart] Frequency range: {:.1f} - {:.1f} Hz", min, max);
}

void ui_frequency_response_chart_set_amplitude_range(ui_frequency_response_chart_t* chart,
                                                     float min, float max) {
    if (!chart) {
        return;
    }

    chart->amp_min = min;
    chart->amp_max = max;

    // Update chart Y-axis range if in chart mode
    if (chart->chart) {
        // Scale to internal range (0-1000) for LVGL chart
        lv_chart_set_axis_range(chart->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    }

    spdlog::debug("[FreqChart] Amplitude range: {:.2e} - {:.2e}", min, max);
}

lv_obj_t* ui_frequency_response_chart_get_obj(ui_frequency_response_chart_t* chart) {
    return chart ? chart->root : nullptr;
}

// ============================================================================
// Draw Callbacks
// ============================================================================

/**
 * @brief Draw subtle grid lines behind chart data
 *
 * Renders horizontal amplitude divisions and vertical frequency markers
 * at round Hz values (25, 50, 75, 100 Hz) within the chart content area.
 */
static void draw_freq_grid_lines_cb(lv_event_t* e) {
    lv_obj_t* chart_obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* chart = static_cast<ui_frequency_response_chart_t*>(lv_event_get_user_data(e));

    if (!layer || !chart) {
        return;
    }

    // Get chart bounds and calculate content area (inside padding)
    lv_area_t coords;
    lv_obj_get_coords(chart_obj, &coords);

    int32_t pad_top = lv_obj_get_style_pad_top(chart_obj, LV_PART_MAIN);
    int32_t pad_left = lv_obj_get_style_pad_left(chart_obj, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart_obj, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart_obj, LV_PART_MAIN);

    int32_t content_x1 = coords.x1 + pad_left;
    int32_t content_x2 = coords.x2 - pad_right;
    int32_t content_y1 = coords.y1 + pad_top;
    int32_t content_y2 = coords.y2 - pad_bottom;
    int32_t content_width = content_x2 - content_x1;
    int32_t content_height = content_y2 - content_y1;

    if (content_width <= 0 || content_height <= 0) {
        return;
    }

    // Subtle grid line style
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = theme_manager_get_color("elevated_bg");
    line_dsc.width = 1;
    line_dsc.opa = static_cast<lv_opa_t>(38); // ~15% opacity

    // Horizontal grid lines: 4 divisions (amplitude)
    constexpr int H_DIVISIONS = 4;
    for (int i = 1; i < H_DIVISIONS; i++) {
        int32_t y = content_y1 + (content_height * i) / H_DIVISIONS;
        line_dsc.p1.x = content_x1;
        line_dsc.p1.y = y;
        line_dsc.p2.x = content_x2;
        line_dsc.p2.y = y;
        lv_draw_line(layer, &line_dsc);
    }

    // Vertical grid lines at round frequency values: 25, 50, 75, 100 Hz
    float freq_range = chart->freq_max - chart->freq_min;
    if (freq_range <= 0.0f) {
        return;
    }

    static constexpr float GRID_FREQS[] = {25.0f, 50.0f, 75.0f, 100.0f};
    for (float freq : GRID_FREQS) {
        if (freq <= chart->freq_min || freq >= chart->freq_max) {
            continue;
        }
        float frac = (freq - chart->freq_min) / freq_range;
        int32_t x = content_x1 + static_cast<int32_t>(frac * content_width);
        line_dsc.p1.x = x;
        line_dsc.p1.y = content_y1;
        line_dsc.p2.x = x;
        line_dsc.p2.y = content_y2;
        lv_draw_line(layer, &line_dsc);
    }
}

/**
 * @brief Draw peak frequency dots with glow effect on top of chart data
 *
 * For each series with a marked peak, draws a semi-transparent glow circle
 * behind a solid filled dot at the peak frequency position.
 */
static void draw_peak_dots_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* chart = static_cast<ui_frequency_response_chart_t*>(lv_event_get_user_data(e));

    if (!layer || !chart || !chart->chart) {
        return;
    }

    // Get chart content area (inside padding) for manual position calculation
    lv_area_t chart_coords;
    lv_obj_get_coords(chart->chart, &chart_coords);

    int32_t pad_top = lv_obj_get_style_pad_top(chart->chart, LV_PART_MAIN);
    int32_t pad_left = lv_obj_get_style_pad_left(chart->chart, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart->chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart->chart, LV_PART_MAIN);

    int32_t content_x1 = chart_coords.x1 + pad_left;
    int32_t content_x2 = chart_coords.x2 - pad_right;
    int32_t content_y1 = chart_coords.y1 + pad_top;
    int32_t content_y2 = chart_coords.y2 - pad_bottom;
    int32_t content_width = content_x2 - content_x1;
    int32_t content_height = content_y2 - content_y1;

    if (content_width <= 0 || content_height <= 0) {
        return;
    }

    float freq_range = chart->freq_max - chart->freq_min;
    float amp_range = chart->amp_max - chart->amp_min;

    if (freq_range <= 0.0f || amp_range <= 0.0f) {
        return;
    }

    for (int i = 0; i < MAX_SERIES; i++) {
        FrequencySeriesData* series = &chart->series[i];
        if (series->id == -1 || !series->has_peak || !series->visible || !series->lv_series) {
            continue;
        }

        // Use cached peak index to align dot with LVGL's evenly-spaced points
        size_t total_pts = series->frequencies.size();
        if (total_pts == 0) {
            continue;
        }

        // X: match LVGL's evenly-spaced point positioning
        float x_frac = (total_pts > 1) ? static_cast<float>(series->peak_idx) /
                                             static_cast<float>(total_pts - 1)
                                       : 0.5f;
        x_frac = std::max(0.0f, std::min(1.0f, x_frac));
        int32_t abs_x = content_x1 + static_cast<int32_t>(x_frac * content_width);

        // Y: use the actual amplitude at the peak point for correct vertical position
        float peak_amp = (series->peak_idx < series->amplitudes.size())
                             ? series->amplitudes[series->peak_idx]
                             : series->peak_amplitude;
        float amp_frac = (peak_amp - chart->amp_min) / amp_range;
        amp_frac = std::max(0.0f, std::min(1.0f, amp_frac));
        int32_t abs_y = content_y2 - static_cast<int32_t>(amp_frac * content_height);

        // Glow circle: larger, semi-transparent, lighter tint, pulsing opacity
        constexpr int32_t GLOW_RADIUS = 10;
        lv_draw_rect_dsc_t glow_dsc;
        lv_draw_rect_dsc_init(&glow_dsc);
        glow_dsc.bg_color = lv_color_mix(series->color, lv_color_white(), LV_OPA_40);
        glow_dsc.bg_opa = static_cast<lv_opa_t>(chart->pulse_phase * 30 / 255);
        glow_dsc.radius = LV_RADIUS_CIRCLE;
        glow_dsc.border_width = 0;

        lv_area_t glow_area;
        glow_area.x1 = abs_x - GLOW_RADIUS;
        glow_area.y1 = abs_y - GLOW_RADIUS;
        glow_area.x2 = abs_x + GLOW_RADIUS;
        glow_area.y2 = abs_y + GLOW_RADIUS;
        lv_draw_rect(layer, &glow_dsc, &glow_area);

        // Solid dot: smaller, fully opaque, series color
        constexpr int32_t DOT_RADIUS = 5;
        lv_draw_rect_dsc_t dot_dsc;
        lv_draw_rect_dsc_init(&dot_dsc);
        dot_dsc.bg_color = series->color;
        dot_dsc.bg_opa = LV_OPA_COVER;
        dot_dsc.radius = LV_RADIUS_CIRCLE;
        dot_dsc.border_width = 0;

        lv_area_t dot_area;
        dot_area.x1 = abs_x - DOT_RADIUS;
        dot_area.y1 = abs_y - DOT_RADIUS;
        dot_area.x2 = abs_x + DOT_RADIUS;
        dot_area.y2 = abs_y + DOT_RADIUS;
        lv_draw_rect(layer, &dot_dsc, &dot_area);

        spdlog::trace("[FreqChart] Drew peak dot for series {} at ({}, {})", series->id, abs_x,
                      abs_y);
    }
}

// ============================================================================
// Muted Series Drawing
// ============================================================================

/**
 * @brief Draw muted series as thin translucent polylines on top of chart data
 *
 * Muted series carry reference curves that must stay visually secondary to
 * the fitted curves, so they bypass their LVGL chart series (kept hidden) and
 * are drawn here at 1px with reduced opacity. Point placement mirrors the
 * built-in renderer and draw_peak_dots_cb(): X from the point index over the
 * content width, Y from the stored amplitude over the amplitude range.
 */
static void draw_muted_series_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* chart = static_cast<ui_frequency_response_chart_t*>(lv_event_get_user_data(e));

    if (!layer || !chart || !chart->chart) {
        return;
    }

    lv_area_t chart_coords;
    lv_obj_get_coords(chart->chart, &chart_coords);

    int32_t pad_top = lv_obj_get_style_pad_top(chart->chart, LV_PART_MAIN);
    int32_t pad_left = lv_obj_get_style_pad_left(chart->chart, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart->chart, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart->chart, LV_PART_MAIN);

    const int32_t content_x1 = chart_coords.x1 + pad_left;
    const int32_t content_x2 = chart_coords.x2 - pad_right;
    const int32_t content_y1 = chart_coords.y1 + pad_top;
    const int32_t content_y2 = chart_coords.y2 - pad_bottom;
    const int32_t content_width = content_x2 - content_x1;
    const int32_t content_height = content_y2 - content_y1;

    if (content_width <= 0 || content_height <= 0) {
        return;
    }

    const float amp_range = chart->amp_max - chart->amp_min;
    if (!(amp_range > 0.0f)) {
        return;
    }

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.width = 1;
    line_dsc.opa = 170; // ~66% - present but behind the 2px fitted curves

    for (int i = 0; i < MAX_SERIES; i++) {
        const FrequencySeriesData* series = &chart->series[i];
        if (series->id == -1 || !series->muted || !series->visible) {
            continue;
        }

        const size_t total_pts = series->amplitudes.size();
        if (total_pts < 2) {
            continue;
        }

        line_dsc.color = series->color;
        for (size_t j = 1; j < total_pts; j++) {
            float x_frac_prev = static_cast<float>(j - 1) / static_cast<float>(total_pts - 1);
            float x_frac = static_cast<float>(j) / static_cast<float>(total_pts - 1);
            float amp_frac_prev = (series->amplitudes[j - 1] - chart->amp_min) / amp_range;
            float amp_frac = (series->amplitudes[j] - chart->amp_min) / amp_range;
            amp_frac_prev = std::max(0.0f, std::min(1.0f, amp_frac_prev));
            amp_frac = std::max(0.0f, std::min(1.0f, amp_frac));

            line_dsc.p1.x = content_x1 + static_cast<int32_t>(x_frac_prev * content_width);
            line_dsc.p1.y = content_y2 - static_cast<int32_t>(amp_frac_prev * content_height);
            line_dsc.p2.x = content_x1 + static_cast<int32_t>(x_frac * content_width);
            line_dsc.p2.y = content_y2 - static_cast<int32_t>(amp_frac * content_height);
            lv_draw_line(layer, &line_dsc);
        }

        spdlog::trace("[FreqChart] Drew muted series '{}' ({} points)", series->name, total_pts);
    }
}

// ============================================================================
// Axis Label Draw Callbacks
// ============================================================================

/**
 * @brief Draw X-axis frequency labels below the chart content area
 *
 * Renders frequency values (0, 50, 100, 150, 200 Hz) at evenly spaced
 * positions below the chart. Uses the same font/color styling pattern
 * as ui_temp_graph.cpp axis labels.
 */
static void draw_x_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart_obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* chart = static_cast<ui_frequency_response_chart_t*>(lv_event_get_user_data(e));

    if (!layer || !chart) {
        return;
    }

    // Get chart bounds and content area
    lv_area_t coords;
    lv_obj_get_coords(chart_obj, &coords);

    int32_t pad_left = lv_obj_get_style_pad_left(chart_obj, LV_PART_MAIN);
    int32_t pad_right = lv_obj_get_style_pad_right(chart_obj, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart_obj, LV_PART_MAIN);

    int32_t content_x1 = coords.x1 + pad_left;
    int32_t content_x2 = coords.x2 - pad_right;
    int32_t content_width = content_x2 - content_x1;

    if (content_width <= 0) {
        return;
    }

    // Label style: small, muted text
    const lv_font_t* label_font = theme_manager_get_font("font_small");
    int32_t label_height = theme_manager_get_font_height(label_font);
    int32_t space_xs = theme_manager_get_spacing("space_xs");

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = theme_manager_get_color("text_muted");
    label_dsc.font = label_font;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.opa = LV_OPA_COVER;

    // Position labels just below the chart content area
    int32_t label_y = coords.y2 - pad_bottom + space_xs;

    // Draw labels at round frequency values
    float freq_range = chart->freq_max - chart->freq_min;
    if (freq_range <= 0.0f) {
        return;
    }

    // Choose frequency tick interval based on range
    float tick_interval = 50.0f;
    if (freq_range <= 100.0f) {
        tick_interval = 25.0f;
    }

    // Persistent label buffers (LVGL may defer drawing)
    static char freq_labels[8][12];
    int label_idx = 0;

    for (float freq = chart->freq_min; freq <= chart->freq_max && label_idx < 8;
         freq += tick_interval) {
        float frac = (freq - chart->freq_min) / freq_range;
        int32_t x = content_x1 + static_cast<int32_t>(frac * content_width);

        // Format label
        char* buf = freq_labels[label_idx++];
        if (freq == 0.0f) {
            snprintf(buf, 12, "0 Hz");
        } else {
            snprintf(buf, 12, "%.0f", freq);
        }

        // Center label on tick position
        lv_area_t label_area;
        label_area.x1 = x - 24;
        label_area.y1 = label_y;
        label_area.x2 = x + 24;
        label_area.y2 = label_y + label_height;

        label_dsc.text = buf;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

/**
 * @brief Draw Y-axis amplitude labels along the left side of the chart
 *
 * Renders amplitude values at horizontal grid division positions. Values
 * are formatted in scientific notation for large amplitudes or as decimals
 * for small values.
 */
static void draw_y_axis_labels_cb(lv_event_t* e) {
    lv_obj_t* chart_obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* chart = static_cast<ui_frequency_response_chart_t*>(lv_event_get_user_data(e));

    if (!layer || !chart) {
        return;
    }

    // Get chart bounds and content area
    lv_area_t coords;
    lv_obj_get_coords(chart_obj, &coords);

    int32_t pad_top = lv_obj_get_style_pad_top(chart_obj, LV_PART_MAIN);
    int32_t pad_bottom = lv_obj_get_style_pad_bottom(chart_obj, LV_PART_MAIN);

    int32_t content_y1 = coords.y1 + pad_top;
    int32_t content_y2 = coords.y2 - pad_bottom;
    int32_t content_height = content_y2 - content_y1;

    if (content_height <= 0) {
        return;
    }

    // Label style: small, muted text, right-aligned to sit left of chart area
    const lv_font_t* label_font = theme_manager_get_font("font_small");
    int32_t label_height = theme_manager_get_font_height(label_font);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = theme_manager_get_color("text_muted");
    label_dsc.font = label_font;
    label_dsc.align = LV_TEXT_ALIGN_RIGHT;
    label_dsc.opa = LV_OPA_COVER;

    // Y-axis label area: left padding area of the chart
    int32_t label_width = lv_obj_get_style_pad_left(chart_obj, LV_PART_MAIN) - 2;
    if (label_width <= 0) {
        return;
    }

    // Draw labels at each horizontal division (matching grid lines)
    constexpr int H_DIVISIONS = 4;
    float amp_range = chart->amp_max - chart->amp_min;

    // Persistent label buffers
    static char amp_labels[8][12];

    for (int i = 0; i <= H_DIVISIONS; i++) {
        int32_t y = content_y1 + (content_height * i) / H_DIVISIONS;

        // Amplitude value at this division (top = max, bottom = min)
        float amp = chart->amp_max - (amp_range * i) / H_DIVISIONS;

        // Format amplitude value compactly
        char* buf = amp_labels[i];
        if (amp == 0.0f) {
            snprintf(buf, 12, "0");
        } else if (amp >= 1e9f) {
            snprintf(buf, 12, "%.0fe9", amp / 1e9f);
        } else if (amp >= 1e6f) {
            snprintf(buf, 12, "%.0fM", amp / 1e6f);
        } else if (amp >= 1e3f) {
            snprintf(buf, 12, "%.0fk", amp / 1e3f);
        } else if (amp >= 1.0f) {
            snprintf(buf, 12, "%.0f", amp);
        } else if (amp >= 0.01f) {
            snprintf(buf, 12, "%.2f", amp);
        } else if (amp >= 0.001f) {
            snprintf(buf, 12, "%.3f", amp);
        } else {
            // Very small values: use scientific-ish notation
            snprintf(buf, 12, "%.0e", amp);
        }

        // Position label centered vertically on the grid line
        lv_area_t label_area;
        label_area.x1 = coords.x1;
        label_area.y1 = y - label_height / 2;
        label_area.x2 = coords.x1 + label_width;
        label_area.y2 = y + label_height / 2;

        label_dsc.text = buf;
        lv_draw_label(layer, &label_dsc, &label_area);
    }
}

// ============================================================================
// Hardware Adaptation
// ============================================================================

void ui_frequency_response_chart_configure_for_platform(ui_frequency_response_chart_t* chart,
                                                        helix::PlatformTier tier) {
    if (!chart) {
        return;
    }

    chart->tier = tier;

    // Determine capabilities based on tier
    switch (tier) {
    case helix::PlatformTier::STANDARD:
        chart->max_points = helix::PlatformCapabilities::STANDARD_CHART_POINTS;
        chart->chart_mode = true;
        break;
    case helix::PlatformTier::BASIC:
        chart->max_points = helix::PlatformCapabilities::BASIC_CHART_POINTS;
        chart->chart_mode = true;
        break;
    case helix::PlatformTier::EMBEDDED:
    default:
        chart->max_points = helix::PlatformCapabilities::BASIC_CHART_POINTS;
        chart->chart_mode = true;
        break;
    }

    // Create or destroy LVGL chart widget based on mode
    if (chart->chart_mode && !chart->chart) {
        // Create LVGL chart
        chart->chart = lv_chart_create(chart->root);
        if (chart->chart) {
            lv_obj_set_size(chart->chart, LV_PCT(100), LV_PCT(100));
            lv_chart_set_type(chart->chart, LV_CHART_TYPE_LINE);
            lv_chart_set_update_mode(chart->chart, LV_CHART_UPDATE_MODE_SHIFT);
            lv_chart_set_point_count(chart->chart, static_cast<uint32_t>(chart->max_points));
            lv_chart_set_axis_range(chart->chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);

            // Padding for axis labels
            const lv_font_t* axis_font = theme_manager_get_font("font_small");
            int32_t axis_label_h = theme_manager_get_font_height(axis_font);
            int32_t space_xs = theme_manager_get_spacing("space_xs");
            int32_t space_sm = theme_manager_get_spacing("space_sm");
            // Left padding: room for Y-axis labels
            lv_obj_set_style_pad_left(chart->chart, 36 + space_xs, LV_PART_MAIN);
            // Bottom padding: room for X-axis labels
            lv_obj_set_style_pad_bottom(chart->chart, space_sm + axis_label_h + space_xs,
                                        LV_PART_MAIN);
            // Small top/right padding for visual breathing room
            lv_obj_set_style_pad_top(chart->chart, space_sm, LV_PART_MAIN);
            lv_obj_set_style_pad_right(chart->chart, space_sm, LV_PART_MAIN);

            // Style
            lv_obj_set_style_bg_opa(chart->chart, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(chart->chart, 0, LV_PART_MAIN);
            lv_obj_set_style_line_width(chart->chart, 2, LV_PART_ITEMS);
            lv_obj_set_style_width(chart->chart, 0, LV_PART_INDICATOR);
            lv_obj_set_style_height(chart->chart, 0, LV_PART_INDICATOR);

            // Register draw callbacks for grid lines, axis labels, peak dots,
            // and muted reference curves
            lv_obj_add_event_cb(chart->chart, draw_freq_grid_lines_cb, LV_EVENT_DRAW_MAIN, chart);
            lv_obj_add_event_cb(chart->chart, draw_x_axis_labels_cb, LV_EVENT_DRAW_POST, chart);
            lv_obj_add_event_cb(chart->chart, draw_y_axis_labels_cb, LV_EVENT_DRAW_POST, chart);
            lv_obj_add_event_cb(chart->chart, draw_peak_dots_cb, LV_EVENT_DRAW_POST, chart);
            lv_obj_add_event_cb(chart->chart, draw_muted_series_cb, LV_EVENT_DRAW_POST, chart);

            // Create LVGL series for existing series data
            for (int i = 0; i < MAX_SERIES; i++) {
                if (chart->series[i].id != -1 && !chart->series[i].lv_series) {
                    chart->series[i].lv_series = lv_chart_add_series(
                        chart->chart, chart->series[i].color, LV_CHART_AXIS_PRIMARY_Y);
                    if (chart->series[i].lv_series) {
                        lv_chart_set_all_values(chart->chart, chart->series[i].lv_series,
                                                LV_CHART_POINT_NONE);
                        if (!chart->series[i].frequencies.empty()) {
                            update_chart_series(chart, &chart->series[i]);
                        }
                    }
                }
            }
        }
    } else if (!chart->chart_mode && chart->chart) {
        // Remove existing chart series
        for (int i = 0; i < MAX_SERIES; i++) {
            if (chart->series[i].lv_series) {
                lv_chart_remove_series(chart->chart, chart->series[i].lv_series);
                chart->series[i].lv_series = nullptr;
            }
        }
        // Delete chart widget
        lv_obj_del(chart->chart);
        chart->chart = nullptr;
    } else if (chart->chart_mode && chart->chart) {
        // Update point count if chart exists and mode is chart
        lv_chart_set_point_count(chart->chart, static_cast<uint32_t>(chart->max_points));
    }

    spdlog::debug("[FreqChart] Configured for {} tier: max_points={}, chart_mode={}",
                  helix::platform_tier_to_string(tier), chart->max_points, chart->chart_mode);
}

size_t ui_frequency_response_chart_get_max_points(ui_frequency_response_chart_t* chart) {
    return chart ? chart->max_points : 0;
}

bool ui_frequency_response_chart_is_chart_mode(ui_frequency_response_chart_t* chart) {
    return chart ? chart->chart_mode : false;
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_frequency_response_chart.cpp
 * @brief Unit tests for FrequencyResponseChart widget
 *
 * Test-first development: These tests are written BEFORE implementation.
 * Tests verify the frequency response chart widget for input shaper calibration
 * data visualization.
 *
 * Test categories:
 * 1. Creation/destruction - Basic lifecycle management
 * 2. Series management - Add, remove, show/hide multiple data series
 * 3. Data management - Setting data with downsampling behavior
 * 4. Peak marking - Highlight resonance peaks
 * 5. Configuration - Frequency/amplitude range settings
 * 6. Platform adaptation - Hardware tier configuration and limits
 *
 * Key behaviors:
 * - EMBEDDED tier: Table mode only (is_chart_mode = false), max 0 chart points
 * - BASIC tier: Simplified chart, max 50 points
 * - STANDARD tier: Full chart, max 200 points
 * - Downsampling preserves frequency range endpoints
 */

#include "ui_fonts.h"

#include "../../include/platform_capabilities.h"
#include "../../include/theme_manager.h"
#include "../../include/ui_frequency_response_chart.h"
#include "../lvgl_test_fixture.h"
#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/misc/lv_text_private.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for frequency response chart tests
 *
 * Provides LVGL initialization and a parent screen for widget creation.
 */
class FrequencyResponseChartTestFixture : public LVGLTestFixture {
  public:
    FrequencyResponseChartTestFixture() {
        // Create a screen object to use as parent
        screen = lv_obj_create(NULL);
    }

    ~FrequencyResponseChartTestFixture() {
        // Cleanup is handled by LVGL
    }

    lv_obj_t* screen;
};

// ============================================================================
// Creation/Destruction Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Create and destroy frequency response chart",
                 "[frequency_response_chart][lifecycle]") {
    SECTION("Create chart with valid parent") {
        ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);

        REQUIRE(chart != nullptr);
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);

        ui_frequency_response_chart_destroy(chart);
    }

    SECTION("Create chart with NULL parent returns NULL") {
        ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(nullptr);
        REQUIRE(chart == nullptr);
    }

    SECTION("Destroy NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_destroy(nullptr));
    }

    SECTION("Get obj from NULL chart returns NULL") {
        lv_obj_t* obj = ui_frequency_response_chart_get_obj(nullptr);
        REQUIRE(obj == nullptr);
    }

    SECTION("Double destroy is safe") {
        ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
        REQUIRE(chart != nullptr);

        // First destroy frees the chart; verifying it doesn't throw
        REQUIRE_NOTHROW(ui_frequency_response_chart_destroy(chart));
        // Second destroy with dangling pointer is undefined behavior - not testable safely
    }
}

// ============================================================================
// Series Management Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Add series returns unique IDs",
                 "[frequency_response_chart][series]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Add single series returns valid ID") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        REQUIRE(id >= 0);
    }

    SECTION("Add multiple series returns unique IDs") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));
        int id3 = ui_frequency_response_chart_add_series(chart, "Z Axis", lv_color_hex(0x4444FF));

        REQUIRE(id1 >= 0);
        REQUIRE(id2 >= 0);
        REQUIRE(id3 >= 0);
        REQUIRE(id1 != id2);
        REQUIRE(id2 != id3);
        REQUIRE(id1 != id3);
    }

    SECTION("Add series with NULL name fails") {
        int id = ui_frequency_response_chart_add_series(chart, nullptr, lv_color_hex(0xFF4444));
        REQUIRE(id == -1);
    }

    SECTION("Add series to NULL chart fails") {
        int id = ui_frequency_response_chart_add_series(nullptr, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id == -1);
    }

    SECTION("Add series with empty name succeeds") {
        // Empty string is valid, just not NULL
        int id = ui_frequency_response_chart_add_series(chart, "", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Remove series cleans up",
                 "[frequency_response_chart][series]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Remove existing series") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        REQUIRE_NOTHROW(ui_frequency_response_chart_remove_series(chart, id));

        // Verify removed series slot is reused: adding a new series should succeed
        int id2 = ui_frequency_response_chart_add_series(chart, "Reused", lv_color_hex(0x00FF00));
        REQUIRE(id2 >= 0);
    }

    SECTION("Reused slot does not inherit the muted flag") {
        // The live-before overlay marks its series muted; after removal the
        // slot must come back clean or a later fitted series would be
        // double-drawn and could never be hidden by a chip toggle.
        int id = ui_frequency_response_chart_add_series(chart, "Was", lv_color_hex(0x888888));
        REQUIRE(id >= 0);
        ui_frequency_response_chart_set_series_muted(chart, id, true);
        REQUIRE(ui_frequency_response_chart_is_series_muted(chart, id));

        ui_frequency_response_chart_remove_series(chart, id);

        int id2 = ui_frequency_response_chart_add_series(chart, "Fitted", lv_color_hex(0x00FF00));
        REQUIRE(id2 >= 0);
        CHECK_FALSE(ui_frequency_response_chart_is_series_muted(chart, id2));
    }

    SECTION("Every series the results card needs fits without exhaustion") {
        // Raw PSD + up to kMaxShapers (11) fitted curves + the live-before
        // overlay: a Kalico CSV with 11 fitted columns must not silently drop
        // the last-added series.
        int last_id = -1;
        for (int i = 0; i < 13; i++) {
            INFO("series index " << i);
            last_id = ui_frequency_response_chart_add_series(chart, "S", lv_color_hex(0x00FF00));
            REQUIRE(last_id >= 0);
        }
        // The 14th exceeds the slot budget and must fail loudly, not wrap.
        CHECK(ui_frequency_response_chart_add_series(chart, "S", lv_color_hex(0x00FF00)) == -1);
    }

    SECTION("Remove series from middle maintains others") {
        int id1 = ui_frequency_response_chart_add_series(chart, "Series1", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Series2", lv_color_hex(0x44FF44));
        int id3 = ui_frequency_response_chart_add_series(chart, "Series3", lv_color_hex(0x4444FF));

        ui_frequency_response_chart_remove_series(chart, id2);

        // Remaining series still accept data without crashing
        float freqs[] = {10.0f, 20.0f, 30.0f};
        float amps[] = {1.0f, 2.0f, 1.5f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id1, freqs, amps, 3));
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id3, freqs, amps, 3));

        // Chart obj is still valid after partial removal
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Remove invalid series ID does nothing") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        REQUIRE_NOTHROW(ui_frequency_response_chart_remove_series(chart, 999));

        // Original series still functional after invalid remove
        float freqs[] = {10.0f};
        float amps[] = {1.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 1));
    }

    SECTION("Remove from NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_remove_series(nullptr, 0));
    }

    SECTION("Remove already removed series is safe") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        ui_frequency_response_chart_remove_series(chart, id);
        REQUIRE_NOTHROW(ui_frequency_response_chart_remove_series(chart, id));
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Show/hide series toggles visibility",
                 "[frequency_response_chart][series]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Hide visible series") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id, false));
    }

    SECTION("Show hidden series") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id, false));
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id, true));
    }

    SECTION("Toggle visibility multiple times") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        for (int i = 0; i < 10; i++) {
            REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id, i % 2 == 0));
        }
    }

    SECTION("Show/hide invalid series ID does nothing") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, 999, false));
    }

    SECTION("Show/hide on NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(nullptr, 0, false));
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Data Management Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Set data with various point counts",
                 "[frequency_response_chart][data]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    // Configure for STANDARD tier (max 200 points) for these tests
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Set data with small array") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        float freqs[] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
        float amps[] = {1.0f, 2.5f, 5.0f, 2.0f, 0.5f};

        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 5));
        // Chart object still valid after data set
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Set data with exact max points") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        size_t max_points = ui_frequency_response_chart_get_max_points(chart);
        std::vector<float> freqs(max_points);
        std::vector<float> amps(max_points);

        for (size_t i = 0; i < max_points; i++) {
            freqs[i] = 10.0f + i * 0.5f;
            amps[i] = 1.0f + (float)(i % 50) * 0.1f;
        }

        REQUIRE_NOTHROW(
            ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), max_points));
        // max_points unchanged after setting exact-limit data
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) == max_points);
    }

    SECTION("Set data with NULL frequencies fails gracefully") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        float amps[] = {1.0f, 2.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, nullptr, amps, 2));
    }

    SECTION("Set data with NULL amplitudes fails gracefully") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        float freqs[] = {10.0f, 20.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, nullptr, 2));
    }

    SECTION("Set data with zero count fails gracefully") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        float freqs[] = {10.0f};
        float amps[] = {1.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 0));
    }

    SECTION("Set data on invalid series ID is safe") {
        float freqs[] = {10.0f};
        float amps[] = {1.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, 999, freqs, amps, 1));
    }

    SECTION("Set data on NULL chart is safe") {
        float freqs[] = {10.0f};
        float amps[] = {1.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(nullptr, 0, freqs, amps, 1));
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture,
                 "Downsampling preserves frequency range endpoints",
                 "[frequency_response_chart][downsampling]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    // Configure for STANDARD tier (max 200 points)
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Data with 500 points on STANDARD tier downsamples to ~200 points") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        const size_t input_count = 500;
        std::vector<float> freqs(input_count);
        std::vector<float> amps(input_count);

        float freq_min = 10.0f;
        float freq_max = 200.0f;

        for (size_t i = 0; i < input_count; i++) {
            freqs[i] = freq_min + (freq_max - freq_min) * i / (input_count - 1);
            amps[i] = 1.0f + (float)(i % 50) * 0.1f;
        }

        ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), input_count);

        // The chart should have downsampled to max_points
        // We verify this indirectly - if get_max_points returns 200, the chart
        // should have limited the data to that
        size_t max_points = ui_frequency_response_chart_get_max_points(chart);
        REQUIRE(max_points == PlatformCapabilities::kStandardChartPoints);
    }

    SECTION("Data with 100 points on STANDARD tier keeps all points (no downsampling)") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        const size_t input_count = 100;
        std::vector<float> freqs(input_count);
        std::vector<float> amps(input_count);

        for (size_t i = 0; i < input_count; i++) {
            freqs[i] = 10.0f + i * 2.0f;
            amps[i] = 1.0f;
        }

        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(),
                                                             input_count));
        // No downsampling needed - 100 < 200; max_points still reflects tier
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
                PlatformCapabilities::kStandardChartPoints);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Clear data removes all points",
                 "[frequency_response_chart][data]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Clear removes data from all series") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        float freqs[] = {10.0f, 20.0f, 30.0f};
        float amps[] = {1.0f, 2.0f, 1.5f};

        ui_frequency_response_chart_set_data(chart, id1, freqs, amps, 3);
        ui_frequency_response_chart_set_data(chart, id2, freqs, amps, 3);

        ui_frequency_response_chart_clear(chart);

        // Series should still exist after clear: can set new data without crashing
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id1, freqs, amps, 3));
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Clear NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear(nullptr));
    }

    SECTION("Clear empty chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear(chart));
    }

    SECTION("Clear chart with no series is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear(chart));
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Peak Marking Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Mark peak creates cursor at frequency",
                 "[frequency_response_chart][peak]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    // Configure for STANDARD tier to have chart mode enabled
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Mark peak on valid series") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        float freqs[] = {10.0f, 30.0f, 50.0f, 70.0f, 100.0f};
        float amps[] = {1.0f, 3.0f, 10.0f, 2.0f, 0.5f};
        ui_frequency_response_chart_set_data(chart, id, freqs, amps, 5);

        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 50.0f, 10.0f));
        // Chart still valid after peak marking
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Mark peak updates existing marker") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 50.0f, 10.0f));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 75.0f, 8.0f));
    }

    SECTION("Mark peak on different series is independent") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id1, 50.0f, 10.0f));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id2, 75.0f, 8.0f));
    }

    SECTION("Mark peak on invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, 999, 50.0f, 10.0f));
    }

    SECTION("Mark peak on NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(nullptr, 0, 50.0f, 10.0f));
    }

    SECTION("Mark peak with zero amplitude is valid") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 50.0f, 0.0f));
    }

    SECTION("Mark peak with negative frequency is handled") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, -10.0f, 5.0f));
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Clear peak removes cursor",
                 "[frequency_response_chart][peak]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Clear peak after marking") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        ui_frequency_response_chart_mark_peak(chart, id, 50.0f, 10.0f);
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(chart, id));
    }

    SECTION("Clear peak when none marked is safe") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(chart, id));
    }

    SECTION("Clear peak on invalid series ID is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(chart, 999));
    }

    SECTION("Clear peak on NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(nullptr, 0));
    }

    SECTION("Clear peak only affects specified series") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        ui_frequency_response_chart_mark_peak(chart, id1, 50.0f, 10.0f);
        ui_frequency_response_chart_mark_peak(chart, id2, 75.0f, 8.0f);

        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(chart, id1));
        // id2's peak should still exist; re-marking is safe
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id2, 80.0f, 9.0f));
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Configuration Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Set frequency range updates axis",
                 "[frequency_response_chart][config]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Set valid frequency range") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 0.0f, 200.0f));
    }

    SECTION("Set custom frequency range") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 10.0f, 150.0f));
    }

    SECTION("Set frequency range with different values") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 5.0f, 500.0f));
    }

    SECTION("Invalid range (min >= max) is rejected or handled") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 100.0f, 50.0f));
    }

    SECTION("Invalid range (min == max) is rejected or handled") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 100.0f, 100.0f));
    }

    SECTION("Set range on NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(nullptr, 0.0f, 200.0f));
    }

    SECTION("Negative frequency values are handled") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, -10.0f, 200.0f));
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Set amplitude range updates axis",
                 "[frequency_response_chart][config]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Set valid amplitude range") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 100.0f));
    }

    SECTION("Set custom amplitude range") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(chart, -20.0f, 40.0f));
    }

    SECTION("Set amplitude range for logarithmic scale") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(chart, -60.0f, 20.0f));
    }

    SECTION("Invalid range (min >= max) is rejected or handled") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(chart, 100.0f, 50.0f));
    }

    SECTION("Set range on NULL chart is safe") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(nullptr, 0.0f, 100.0f));
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Platform Adaptation Tests (Key Feature)
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture,
                 "Configure for STANDARD tier enables chart mode",
                 "[frequency_response_chart][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
            PlatformCapabilities::kStandardChartPoints);

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Configure for BASIC tier limits to 50 points",
                 "[frequency_response_chart][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC);

    REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
            PlatformCapabilities::kBasicChartPoints);

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture,
                 "Configure for EMBEDDED tier enables chart mode with 50 points",
                 "[frequency_response_chart][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::EMBEDDED);

    // EMBEDDED tier now uses chart mode with same limits as BASIC
    REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
            PlatformCapabilities::kBasicChartPoints);

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Get max points returns tier-appropriate value",
                 "[frequency_response_chart][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("STANDARD tier returns 200 points") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) == 200);
    }

    SECTION("BASIC tier returns 50 points") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC);
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) == 50);
    }

    SECTION("EMBEDDED tier returns 50 points (same as BASIC)") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::EMBEDDED);
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
                PlatformCapabilities::kBasicChartPoints);
    }

    SECTION("Get max points from NULL chart returns 0") {
        REQUIRE(ui_frequency_response_chart_get_max_points(nullptr) == 0);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture,
                 "is_chart_mode returns correct value for each tier",
                 "[frequency_response_chart][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("STANDARD tier is chart mode") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    }

    SECTION("BASIC tier is chart mode") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC);
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    }

    SECTION("EMBEDDED tier is chart mode (same as BASIC)") {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::EMBEDDED);
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    }

    SECTION("is_chart_mode from NULL chart returns false") {
        REQUIRE(ui_frequency_response_chart_is_chart_mode(nullptr) == false);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Configure on NULL chart is safe",
                 "[frequency_response_chart][platform]") {
    REQUIRE_NOTHROW(
        ui_frequency_response_chart_configure_for_platform(nullptr, PlatformTier::STANDARD));
}

// ============================================================================
// Downsampling Behavior Tests (Tier-Specific)
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "BASIC tier downsampling",
                 "[frequency_response_chart][downsampling][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC);

    SECTION("Data with 500 points downsamples to ~50 points") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        const size_t input_count = 500;
        std::vector<float> freqs(input_count);
        std::vector<float> amps(input_count);

        for (size_t i = 0; i < input_count; i++) {
            freqs[i] = 10.0f + i * 0.4f;
            amps[i] = 1.0f;
        }

        ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), input_count);

        // Verify max points is limited
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) == 50);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "EMBEDDED tier downsamples like BASIC tier",
                 "[frequency_response_chart][downsampling][platform]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::EMBEDDED);

    SECTION("Data with 500 points downsamples to ~50 points") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        const size_t input_count = 500;
        std::vector<float> freqs(input_count);
        std::vector<float> amps(input_count);

        for (size_t i = 0; i < input_count; i++) {
            freqs[i] = 10.0f + i * 0.4f;
            amps[i] = 1.0f;
        }

        ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), input_count);

        // EMBEDDED tier now uses chart mode with same limits as BASIC
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
                PlatformCapabilities::kBasicChartPoints);
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Multiple Series Independence Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Multiple series work independently",
                 "[frequency_response_chart][series][integration]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Independent data per series") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));
        int id3 = ui_frequency_response_chart_add_series(chart, "Z Axis", lv_color_hex(0x4444FF));
        REQUIRE(id1 >= 0);
        REQUIRE(id2 >= 0);
        REQUIRE(id3 >= 0);

        // Different data for each series
        float freqs1[] = {10.0f, 20.0f, 30.0f};
        float amps1[] = {1.0f, 5.0f, 2.0f};

        float freqs2[] = {15.0f, 25.0f, 35.0f, 45.0f};
        float amps2[] = {2.0f, 8.0f, 4.0f, 1.0f};

        float freqs3[] = {12.0f, 22.0f};
        float amps3[] = {3.0f, 6.0f};

        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id1, freqs1, amps1, 3));
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id2, freqs2, amps2, 4));
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id3, freqs3, amps3, 2));

        // Chart still valid with three independent series
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Independent visibility per series") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id1, false));
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id2, true));
        REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id1, true));
    }

    SECTION("Independent peak markers per series") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id1, 50.0f, 10.0f));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id2, 75.0f, 15.0f));

        // Clear one series peak; id2's peak should remain
        REQUIRE_NOTHROW(ui_frequency_response_chart_clear_peak(chart, id1));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id2, 80.0f, 12.0f));
    }

    SECTION("Remove one series doesn't affect others") {
        int id1 = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));
        int id3 = ui_frequency_response_chart_add_series(chart, "Z Axis", lv_color_hex(0x4444FF));

        float freqs[] = {10.0f, 20.0f, 30.0f};
        float amps[] = {1.0f, 2.0f, 1.5f};

        ui_frequency_response_chart_set_data(chart, id1, freqs, amps, 3);
        ui_frequency_response_chart_set_data(chart, id2, freqs, amps, 3);
        ui_frequency_response_chart_set_data(chart, id3, freqs, amps, 3);

        // Remove middle series
        ui_frequency_response_chart_remove_series(chart, id2);

        // Other series still accept data after middle removal
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id1, freqs, amps, 3));
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id3, freqs, amps, 3));
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Integration/Workflow Tests
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Complete calibration workflow",
                 "[frequency_response_chart][integration]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Typical input shaper calibration display") {
        // Set up frequency range for typical input shaper analysis
        ui_frequency_response_chart_set_freq_range(chart, 0.0f, 200.0f);
        ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 1e9f);

        // Add X and Y axis series
        int x_id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        int y_id = ui_frequency_response_chart_add_series(chart, "Y Axis", lv_color_hex(0x44FF44));

        REQUIRE(x_id >= 0);
        REQUIRE(y_id >= 0);

        // Simulate frequency response data (would come from accelerometer)
        const size_t data_points = 150;
        std::vector<float> freqs(data_points);
        std::vector<float> x_amps(data_points);
        std::vector<float> y_amps(data_points);

        for (size_t i = 0; i < data_points; i++) {
            freqs[i] = (float)i + 10.0f;
            // Simulate resonance peak at ~45 Hz for X, ~52 Hz for Y
            float x_peak = 50.0f * expf(-powf(freqs[i] - 45.0f, 2.0f) / 50.0f);
            float y_peak = 40.0f * expf(-powf(freqs[i] - 52.0f, 2.0f) / 60.0f);
            x_amps[i] = 1e6f + x_peak * 1e8f;
            y_amps[i] = 1e6f + y_peak * 1e8f;
        }

        ui_frequency_response_chart_set_data(chart, x_id, freqs.data(), x_amps.data(), data_points);
        ui_frequency_response_chart_set_data(chart, y_id, freqs.data(), y_amps.data(), data_points);

        // Mark detected peaks
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, x_id, 45.0f, 51e8f));
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, y_id, 52.0f, 41e8f));

        // Full workflow completed: chart still valid and in chart mode
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Update data after initial display") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        // Initial data
        float freqs1[] = {10.0f, 20.0f, 30.0f};
        float amps1[] = {1.0f, 2.0f, 1.0f};
        ui_frequency_response_chart_set_data(chart, id, freqs1, amps1, 3);

        // Updated data (different measurements)
        float freqs2[] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f};
        float amps2[] = {1.5f, 3.0f, 5.0f, 2.0f, 1.0f};
        ui_frequency_response_chart_set_data(chart, id, freqs2, amps2, 5);

        // Update peak marker
        REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 30.0f, 5.0f));
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Clear and restart calibration") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        float freqs[] = {10.0f, 20.0f, 30.0f};
        float amps[] = {1.0f, 2.0f, 1.0f};
        ui_frequency_response_chart_set_data(chart, id, freqs, amps, 3);
        ui_frequency_response_chart_mark_peak(chart, id, 20.0f, 2.0f);

        // Clear for new calibration run
        ui_frequency_response_chart_clear(chart);
        ui_frequency_response_chart_clear_peak(chart, id);

        // New calibration data
        float freqs2[] = {15.0f, 25.0f, 35.0f};
        float amps2[] = {2.0f, 4.0f, 2.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs2, amps2, 3));
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Stress tests",
                 "[frequency_response_chart][stress]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);

    SECTION("Rapid data updates") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        // Simulate rapid updates during calibration
        for (int iteration = 0; iteration < 100; iteration++) {
            const size_t points = 50 + (iteration % 100);
            std::vector<float> freqs(points);
            std::vector<float> amps(points);

            for (size_t i = 0; i < points; i++) {
                freqs[i] = 10.0f + i * 2.0f;
                amps[i] = 1.0f + (float)(iteration % 10);
            }

            REQUIRE_NOTHROW(
                ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), points));
        }

        // Chart survives 100 rapid data updates
        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
    }

    SECTION("Rapid configuration changes") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));

        for (int i = 0; i < 100; i++) {
            REQUIRE_NOTHROW(
                ui_frequency_response_chart_set_freq_range(chart, (float)i, (float)(i + 200)));
            REQUIRE_NOTHROW(
                ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 100.0f + i));
            REQUIRE_NOTHROW(ui_frequency_response_chart_mark_peak(chart, id, 50.0f + i, 10.0f));
            REQUIRE_NOTHROW(ui_frequency_response_chart_show_series(chart, id, i % 2 == 0));
        }

        REQUIRE(ui_frequency_response_chart_get_obj(chart) != nullptr);
    }

    SECTION("Rapid tier switching") {
        for (int i = 0; i < 50; i++) {
            REQUIRE_NOTHROW(
                ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD));
            REQUIRE_NOTHROW(
                ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC));
            REQUIRE_NOTHROW(
                ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::EMBEDDED));
        }

        // After 150 tier switches, final state is EMBEDDED (last configured)
        REQUIRE(ui_frequency_response_chart_is_chart_mode(chart) == true);
        REQUIRE(ui_frequency_response_chart_get_max_points(chart) ==
                PlatformCapabilities::kBasicChartPoints);
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "Edge cases",
                 "[frequency_response_chart][edge]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);

    SECTION("Very large frequency values") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 0.0f, 1e6f));
    }

    SECTION("Very small frequency values") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_freq_range(chart, 0.001f, 1.0f));
    }

    SECTION("Very large amplitude values") {
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 1e12f));
    }

    SECTION("Scientific notation data") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        float freqs[] = {1e1f, 1e2f, 1e3f};
        float amps[] = {1e6f, 1e9f, 1e8f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 3));
    }

    SECTION("Single data point") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        float freqs[] = {50.0f};
        float amps[] = {100.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 1));
    }

    SECTION("Two data points") {
        int id = ui_frequency_response_chart_add_series(chart, "X Axis", lv_color_hex(0xFF4444));
        REQUIRE(id >= 0);

        float freqs[] = {10.0f, 100.0f};
        float amps[] = {1.0f, 10.0f};
        REQUIRE_NOTHROW(ui_frequency_response_chart_set_data(chart, id, freqs, amps, 2));
    }

    SECTION("Very long series name") {
        std::string long_name(256, 'x');
        int id = ui_frequency_response_chart_add_series(chart, long_name.c_str(),
                                                        lv_color_hex(0xFF4444));
        // Should either truncate or handle gracefully
        REQUIRE(id >= 0);
    }

    SECTION("Multiple series with same name") {
        int id1 =
            ui_frequency_response_chart_add_series(chart, "Same Name", lv_color_hex(0xFF4444));
        int id2 =
            ui_frequency_response_chart_add_series(chart, "Same Name", lv_color_hex(0x44FF44));

        // Should still get unique IDs
        REQUIRE(id1 >= 0);
        REQUIRE(id2 >= 0);
        REQUIRE(id1 != id2);
    }

    SECTION("Multiple series with same color") {
        int id1 = ui_frequency_response_chart_add_series(chart, "Series 1", lv_color_hex(0xFF4444));
        int id2 = ui_frequency_response_chart_add_series(chart, "Series 2", lv_color_hex(0xFF4444));

        // Same color is allowed
        REQUIRE(id1 >= 0);
        REQUIRE(id2 >= 0);
    }

    ui_frequency_response_chart_destroy(chart);
}

// ============================================================================
// Axis label boxes follow the label font
// ============================================================================

namespace {

int32_t sample_width(const lv_font_t* font, const char* text) {
    lv_text_attributes_t attributes = {};
    attributes.max_width = LV_COORD_MAX;
    return lv_text_get_width(text, static_cast<uint32_t>(std::strlen(text)), font, &attributes);
}

} // namespace

TEST_CASE("axis label boxes fit the label font", "[frequency_response_chart][axis]") {
    SECTION("a large font widens the gutter past its widest label") {
        const lv_font_t* big = &source_code_pro_24;
        REQUIRE(sample_width(big, "100%") > 36);
        CHECK(ui_frequency_response_chart_y_gutter_width(big) >= sample_width(big, "100%"));
        CHECK(ui_frequency_response_chart_y_gutter_width(big) >= sample_width(big, "999k"));
        REQUIRE(sample_width(big, "100 Hz") > 64);
        CHECK(2 * ui_frequency_response_chart_x_label_half_width(big) >=
              sample_width(big, "100 Hz"));
    }
    SECTION("a small font keeps the sizes the smaller tiers were laid out against") {
        const lv_font_t* small = &source_code_pro_8;
        CHECK(ui_frequency_response_chart_y_gutter_width(small) == 36);
        CHECK(ui_frequency_response_chart_x_label_half_width(small) == 32);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "a configured chart pads its gutter for the theme's label font",
                 "[frequency_response_chart][axis]") {
    ui_frequency_response_chart_t* chart = ui_frequency_response_chart_create(test_screen());
    REQUIRE(chart != nullptr);
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
    ui_frequency_response_chart_set_y_labels_visible(chart, true);

    lv_obj_t* root = ui_frequency_response_chart_get_obj(chart);
    REQUIRE(root != nullptr);
    REQUIRE(lv_obj_get_child_count(root) > 0);
    lv_obj_t* plot = lv_obj_get_child(root, 0);
    REQUIRE(lv_obj_check_type(plot, &lv_chart_class));

    const int32_t gutter =
        ui_frequency_response_chart_y_gutter_width(theme_manager_get_font("font_small"));
    // The theme's font_small at this display is wider than the 36 px floor,
    // so a fixed gutter would read narrower than this.
    REQUIRE(gutter > 36);
    const int32_t want = gutter + theme_manager_get_spacing("space_xs");
    CHECK(lv_obj_get_style_pad_left(plot, LV_PART_MAIN) == want);

    SECTION("a resize re-measures the gutter") {
        lv_obj_set_style_pad_left(plot, 0, LV_PART_MAIN);
        lv_obj_set_size(root, 300, 200);
        lv_obj_update_layout(root);
        CHECK(lv_obj_get_style_pad_left(plot, LV_PART_MAIN) == want);
    }

    ui_frequency_response_chart_destroy(chart);
}

namespace {

/// Renders @p chart and returns the text of every label it drew.
std::vector<std::string> drawn_label_texts(ui_frequency_response_chart_t* chart) {
    lv_obj_t* plot = lv_obj_get_child(ui_frequency_response_chart_get_obj(chart), 0);
    REQUIRE(plot != nullptr);
    std::vector<std::string> texts;
    const auto capture = [](lv_event_t* e) {
        auto* out = static_cast<std::vector<std::string>*>(lv_event_get_user_data(e));
        const auto* dsc = lv_draw_task_get_label_dsc(lv_event_get_draw_task(e));
        if (dsc != nullptr && dsc->text != nullptr) {
            out->emplace_back(dsc->text);
        }
    };
    lv_obj_set_send_draw_task_events(plot, true);
    lv_obj_add_event_cb(plot, capture, LV_EVENT_DRAW_TASK_ADDED, &texts);
    if (lv_draw_buf_t* snap = lv_snapshot_take(plot, LV_COLOR_FORMAT_ARGB8888)) {
        lv_draw_buf_destroy(snap);
    }
    lv_obj_remove_event_cb_with_user_data(plot, capture, &texts);
    lv_obj_set_send_draw_task_events(plot, false);
    return texts;
}

bool drew(const std::vector<std::string>& texts, const char* text) {
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "amplitude labels follow each chart's own range",
                 "[frequency_response_chart][axis]") {
    ui_frequency_response_chart_t* a = ui_frequency_response_chart_create(test_screen());
    ui_frequency_response_chart_t* b = ui_frequency_response_chart_create(test_screen());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    for (auto* chart : {a, b}) {
        ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
        ui_frequency_response_chart_set_y_labels_visible(chart, true);
        lv_obj_set_size(ui_frequency_response_chart_get_obj(chart), 400, 240);
    }
    lv_obj_update_layout(test_screen());

    ui_frequency_response_chart_set_amplitude_range(a, 0.0f, 400.0f);
    ui_frequency_response_chart_set_amplitude_range(b, 0.0f, 8000.0f);
    auto texts = drawn_label_texts(a);
    CHECK(drew(texts, "400"));
    CHECK(drew(texts, "200"));
    texts = drawn_label_texts(b);
    CHECK(drew(texts, "8k"));
    CHECK_FALSE(drew(texts, "400"));
    // Drawing b leaves a's labels as they were.
    texts = drawn_label_texts(a);
    CHECK(drew(texts, "400"));
    CHECK_FALSE(drew(texts, "8k"));

    ui_frequency_response_chart_set_amplitude_range(a, 0.0f, 100.0f);
    texts = drawn_label_texts(a);
    CHECK(drew(texts, "100"));
    CHECK_FALSE(drew(texts, "400"));

    // A sub-unit axis draws the SI labels fr_chart_amplitude_labels() picks.
    ui_frequency_response_chart_set_amplitude_range(a, 0.0f, 0.022f);
    texts = drawn_label_texts(a);
    for (const auto& label : fr_chart_amplitude_labels(0.0f, 0.022f, 4, false)) {
        INFO(label);
        CHECK(drew(texts, label.c_str()));
    }
    ui_frequency_response_chart_set_amplitude_range(a, 0.0f, 100.0f);

    ui_frequency_response_chart_set_y_labels_percent(a, true);
    texts = drawn_label_texts(a);
    CHECK(drew(texts, "100%"));
    CHECK(drew(texts, "50%"));

    ui_frequency_response_chart_destroy(b);
    ui_frequency_response_chart_destroy(a);
}

TEST_CASE("frequency labels stay inside the chart's box", "[frequency_response_chart][axis]") {
    // A 300 px chart from x=100 to x=400 and a 32 px half box.
    CHECK(ui_frequency_response_chart_x_label_center(250, 32, 100, 400) == 250);
    CHECK(ui_frequency_response_chart_x_label_center(392, 32, 100, 400) == 368);
    CHECK(ui_frequency_response_chart_x_label_center(110, 32, 100, 400) == 132);
    // Narrower than one box: nowhere to move it, so it stays on the tick.
    CHECK(ui_frequency_response_chart_x_label_center(120, 32, 100, 140) == 120);
}

// ============================================================================
// NaN gaps (prestonbrown/helixscreen#1812)
// ============================================================================

namespace {

constexpr float kGap = std::numeric_limits<float>::quiet_NaN();

std::vector<std::pair<size_t, size_t>> runs_of(std::vector<float> amps) {
    std::vector<std::pair<size_t, size_t>> out;
    for (const FrChartRun& r : fr_chart_finite_runs(amps.data(), amps.size())) {
        out.emplace_back(r.begin, r.end);
    }
    return out;
}

lv_obj_t* inner_lv_chart(ui_frequency_response_chart_t* chart) {
    lv_obj_t* root = ui_frequency_response_chart_get_obj(chart);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_chart_class)) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("fr_chart_finite_runs splits a series at its gaps", "[frequency_response_chart][gaps]") {
    using Runs = std::vector<std::pair<size_t, size_t>>;
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(runs_of({1, 2, kGap, 3, 4, 5, kGap}) == Runs{{0, 2}, {3, 6}});
    CHECK(runs_of({kGap, 1, kGap, 2}).empty());
    CHECK(runs_of({1, 2, 3}) == Runs{{0, 3}});
    CHECK(fr_chart_finite_runs(nullptr, 0).empty());
    CHECK(runs_of({1, inf, 2, 3}) == Runs{{2, 4}});
    CHECK(runs_of({1}).empty());
    CHECK(runs_of({kGap, kGap}).empty());
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "a NaN amplitude is no point on the chart",
                 "[frequency_response_chart][gaps]") {
    auto* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
    ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 100.0f);
    const int id = ui_frequency_response_chart_add_series(chart, "live", lv_color_hex(0xB0B0B0));
    REQUIRE(id >= 0);

    const float freqs[] = {10, 20, 30, 40};
    const float amps[] = {10, kGap, 30, 40};
    ui_frequency_response_chart_set_data(chart, id, freqs, amps, 4);

    lv_obj_t* lv_chart = inner_lv_chart(chart);
    REQUIRE(lv_chart != nullptr);
    lv_chart_series_t* ser = lv_chart_get_series_next(lv_chart, nullptr);
    REQUIRE(ser != nullptr);
    const int32_t* y = lv_chart_get_series_y_array(lv_chart, ser);
    REQUIRE(lv_chart_get_point_count(lv_chart) == 4);
    const uint32_t start = lv_chart_get_x_start_point(lv_chart, ser);
    std::vector<int32_t> values;
    for (uint32_t i = 0; i < 4; ++i) {
        values.push_back(y[(start + i) % 4]);
    }
    CHECK(values == std::vector<int32_t>{100, LV_CHART_POINT_NONE, 300, 400});

    const auto stored = ui_frequency_response_chart_get_series_amplitudes(chart, id);
    REQUIRE(stored.size() == 4);
    CHECK(std::isnan(stored[1]));

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE_METHOD(FrequencyResponseChartTestFixture, "BASIC downsampling carries gaps through",
                 "[frequency_response_chart][gaps]") {
    auto* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::BASIC);
    const int id = ui_frequency_response_chart_add_series(chart, "live", lv_color_hex(0xB0B0B0));

    std::vector<float> freqs(131);
    std::vector<float> amps(131);
    for (size_t i = 0; i < 131; ++i) {
        freqs[i] = 5.0f + static_cast<float>(i);
        amps[i] = i < 10 ? kGap : static_cast<float>(i);
    }
    ui_frequency_response_chart_set_data(chart, id, freqs.data(), amps.data(), amps.size());
    const auto stored = ui_frequency_response_chart_get_series_amplitudes(chart, id);
    REQUIRE(stored.size() == 50);
    CHECK(std::isnan(stored[0]));
    // Picks land at src index i * 130 / 49: index 4 picks source 10.
    for (size_t i = 4; i < stored.size(); ++i) {
        INFO(i);
        CHECK(std::isfinite(stored[i]));
    }
    ui_frequency_response_chart_destroy(chart);
}

// LVGLTestFixture's display flushes; this file's own fixture makes displays
// with no flush callback, so only that one display is refreshed.
TEST_CASE_METHOD(LVGLTestFixture, "styled, muted and marked series render gaps",
                 "[frequency_response_chart][gaps]") {
    lv_obj_t* screen = test_screen();
    auto* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);
    lv_obj_set_size(ui_frequency_response_chart_get_obj(chart), 600, 300);
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
    ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 100.0f);

    std::vector<float> freqs(40);
    std::vector<float> amps(40);
    std::vector<float> none(40, kGap);
    for (size_t i = 0; i < 40; ++i) {
        freqs[i] = static_cast<float>(i);
        amps[i] = (i % 7 == 3) ? kGap : static_cast<float>(i);
    }
    const int styled =
        ui_frequency_response_chart_add_series(chart, "styled", lv_color_hex(0xFF0000));
    ui_frequency_response_chart_set_series_style(chart, styled, FrChartSeriesStyle{3, true, true});
    ui_frequency_response_chart_set_data(chart, styled, freqs.data(), amps.data(), amps.size());
    const FrChartMarker markers[] = {{3.0f, 1}, {10.0f, 0}};
    ui_frequency_response_chart_set_markers(chart, styled, markers, 2);
    ui_frequency_response_chart_mark_peak(chart, styled, 3.0f, 50.0f);

    const int muted =
        ui_frequency_response_chart_add_series(chart, "muted", lv_color_hex(0x00FF00));
    ui_frequency_response_chart_set_series_muted(chart, muted, true);
    ui_frequency_response_chart_set_data(chart, muted, freqs.data(), amps.data(), amps.size());

    const int empty =
        ui_frequency_response_chart_add_series(chart, "empty", lv_color_hex(0x0000FF));
    ui_frequency_response_chart_set_series_style(chart, empty, FrChartSeriesStyle{3, true, true});
    ui_frequency_response_chart_set_data(chart, empty, freqs.data(), none.data(), none.size());

    lv_obj_invalidate(screen);
    lv_refr_now(lv_obj_get_display(screen));
    CHECK(ui_frequency_response_chart_get_series_amplitudes(chart, styled).size() == 40);
    ui_frequency_response_chart_destroy(chart);
}

namespace {

/// Renders @p plot and counts the pixels in rows [y1, y2] (screen coordinates)
/// whose @p channel (0 = red, 1 = green) leads the other two by a clear margin.
int tinted_pixels(lv_obj_t* plot, int channel, int32_t y1, int32_t y2) {
    lv_draw_buf_t* snap = lv_snapshot_take(plot, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    lv_area_t coords;
    lv_obj_get_coords(plot, &coords);
    const int32_t origin_y = coords.y1 - lv_obj_get_ext_draw_size(plot);
    int count = 0;
    for (int32_t y = std::max(y1, origin_y);
         y <= y2 && y - origin_y < static_cast<int32_t>(snap->header.h); ++y) {
        const uint8_t* row = snap->data + static_cast<size_t>(y - origin_y) * snap->header.stride;
        for (uint32_t x = 0; x < snap->header.w; ++x) {
            // ARGB8888 is stored B, G, R, A.
            const int b = row[x * 4 + 0];
            const int g = row[x * 4 + 1];
            const int r = row[x * 4 + 2];
            const int lead = channel == 0 ? r - std::max(g, b) : g - std::max(r, b);
            if (lead > 40) {
                ++count;
            }
        }
    }
    lv_draw_buf_destroy(snap);
    return count;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "nothing a series draws reaches toward a gap",
                 "[frequency_response_chart][gaps]") {
    lv_obj_t* screen = test_screen();
    auto* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);
    lv_obj_set_size(ui_frequency_response_chart_get_obj(chart), 600, 300);
    ui_frequency_response_chart_configure_for_platform(chart, PlatformTier::STANDARD);
    ui_frequency_response_chart_set_amplitude_range(chart, 0.0f, 100.0f);
    lv_obj_update_layout(screen);

    // Every finite value sits in the bottom 40% of the axis; index 3 is a gap,
    // and the peak and the numbered marker both land on it.
    std::vector<float> freqs(40);
    std::vector<float> amps(40);
    for (size_t i = 0; i < 40; ++i) {
        freqs[i] = static_cast<float>(i);
        amps[i] = (i % 7 == 3) ? kGap : static_cast<float>(i);
    }
    const int styled =
        ui_frequency_response_chart_add_series(chart, "styled", lv_color_hex(0xFF0000));
    ui_frequency_response_chart_set_series_style(chart, styled, FrChartSeriesStyle{3, true, true});
    ui_frequency_response_chart_set_data(chart, styled, freqs.data(), amps.data(), amps.size());
    const FrChartMarker markers[] = {{3.0f, 1}, {10.0f, 2}};
    ui_frequency_response_chart_set_markers(chart, styled, markers, 2);
    ui_frequency_response_chart_mark_peak(chart, styled, 3.0f, 50.0f);

    const int muted =
        ui_frequency_response_chart_add_series(chart, "muted", lv_color_hex(0x00FF00));
    ui_frequency_response_chart_set_series_muted(chart, muted, true);
    ui_frequency_response_chart_set_data(chart, muted, freqs.data(), amps.data(), amps.size());

    lv_obj_t* plot = inner_lv_chart(chart);
    REQUIRE(plot != nullptr);
    lv_area_t content;
    lv_obj_get_content_coords(plot, &content);
    REQUIRE(lv_area_get_height(&content) > 100);
    // The tallest finite value is 39% of the axis, so the top 30% holds no
    // curve, fill or dot; a gap drawn as a point would land at the top.
    const int32_t ceiling = content.y1 + lv_area_get_height(&content) * 3 / 10;

    // Both series do draw, below the ceiling.
    REQUIRE(tinted_pixels(plot, 0, ceiling, content.y2) > 0);
    REQUIRE(tinted_pixels(plot, 1, ceiling, content.y2) > 0);
    CHECK(tinted_pixels(plot, 0, content.y1, ceiling) == 0);
    CHECK(tinted_pixels(plot, 1, content.y1, ceiling) == 0);

    ui_frequency_response_chart_destroy(chart);
}

TEST_CASE("the gutter holds every amplitude label the axis prints",
          "[frequency_response_chart][axis]") {
    // A proportional face, where a label's width depends on its letters.
    const lv_font_t* font = &noto_sans_28;
    const int32_t gutter = ui_frequency_response_chart_y_gutter_width(font);
    // Spans of 22 per decade: their quarter steps need a decimal ("16.5m").
    for (const float top : {0.000022f, 0.00022f, 0.0022f, 0.022f, 0.22f, 2.2f, 22.0f, 220.0f,
                            2200.0f, 22000.0f, 220000.0f, 2.2e6f, 22e6f, 2.2e9f, 22e9f}) {
        for (const bool percent : {false, true}) {
            if (percent && top > 100.0f) {
                continue;
            }
            for (const auto& label : fr_chart_amplitude_labels(0.0f, top, 4, percent)) {
                INFO(label << " on a 0.." << top << " axis");
                CHECK(gutter >= sample_width(font, label.c_str()));
            }
        }
    }
}

TEST_CASE("amplitude labels are distinct and compact", "[frequency_response_chart][axis]") {
    using V = std::vector<std::string>;
    CHECK(fr_chart_amplitude_labels(0.0f, 0.022f, 4, false) ==
          V{"22m", "16.5m", "11m", "5.5m", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 0.003f, 4, false) ==
          V{"3m", "2.25m", "1.5m", "0.75m", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 33000.0f, 4, false) ==
          V{"33k", "24.8k", "16.5k", "8.2k", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 400.0f, 4, false) == V{"400", "300", "200", "100", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 2e9f, 4, false) ==
          V{"2e9", "1.5e9", "1e9", "0.5e9", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 0.0004f, 4, false) ==
          V{"400u", "300u", "200u", "100u", "0"});
    CHECK(fr_chart_amplitude_labels(0.0f, 100.0f, 4, true) == V{"100%", "75%", "50%", "25%", "0%"});
    CHECK(fr_chart_amplitude_labels(0.0f, 1.0f, 0, false).empty());
}

TEST_CASE("the chart fill follows the curve without per-triangle wedges",
          "[frequency_response_chart][fill]") {
    constexpr int32_t kBottom = 220;

    // The tint starts at full strength right under the curve wherever the
    // curve is, low tails and small bumps included.
    for (const auto& [a, b] : {std::pair{30, 60}, std::pair{180, 200}, std::pair{210, 205}}) {
        INFO("tops " << a << ", " << b);
        CHECK(fr_chart_fill_segment_opa(a, b, kBottom).first == kFrFillTopOpa);
    }
    CHECK(fr_chart_fill_segment_opa(kBottom, kBottom, kBottom).first == 0);

    // Each triangle's vertical gradient runs from its own top row's opacity
    // down to 0 at the bottom. The segment's two triangles must give every row
    // they share the same opacity, or a diagonal wedge shows between them.
    auto shade = [&](lv_opa_t top_opa, int32_t tri_top, int32_t row) {
        return static_cast<double>(top_opa) * static_cast<double>(kBottom - row) /
               static_cast<double>(kBottom - tri_top);
    };
    for (const auto& [a, b] : {std::pair{25, 55}, std::pair{150, 40}, std::pair{180, 182}}) {
        const FrFillSegmentOpa opa = fr_chart_fill_segment_opa(a, b, kBottom);
        const int32_t first_top = std::min(a, b);
        for (const int32_t row : {std::max(a, b), 200, 215}) {
            INFO("tops " << a << ", " << b << " row " << row);
            CHECK(std::abs(shade(opa.first, first_top, row) - shade(opa.second, b, row)) <= 1.0);
        }
    }
}

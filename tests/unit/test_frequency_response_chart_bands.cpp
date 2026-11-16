// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/platform_capabilities.h"
#include "../../include/ui_frequency_response_chart.h"
#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_draw_private.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

lv_obj_t* plot_of(ui_frequency_response_chart_t* chart) {
    lv_obj_t* root = ui_frequency_response_chart_get_obj(chart);
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_chart_class)) {
            return child;
        }
    }
    return nullptr;
}

/// ARGB of one rendered pixel of @p plot, screen coordinates.
uint32_t pixel_at(lv_obj_t* plot, int32_t x, int32_t y) {
    lv_draw_buf_t* snap = lv_snapshot_take(plot, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    lv_area_t coords;
    lv_obj_get_coords(plot, &coords);
    const int32_t ox = coords.x1 - lv_obj_get_ext_draw_size(plot);
    const int32_t oy = coords.y1 - lv_obj_get_ext_draw_size(plot);
    uint32_t px = 0;
    std::memcpy(&px, snap->data + static_cast<size_t>(y - oy) * snap->header.stride + (x - ox) * 4,
                4);
    lv_draw_buf_destroy(snap);
    return px;
}

} // namespace

TEST_CASE("x tick labels carry the chart's unit on the first tick only",
          "[frequency_chart][ripple]") {
    CHECK(ui_frequency_response_chart_format_x_label(50.0f, true, "Hz") == "50 Hz");
    CHECK(ui_frequency_response_chart_format_x_label(100.0f, false, "Hz") == "100");
    CHECK(ui_frequency_response_chart_format_x_label(50.0f, true, "mm/s") == "50 mm/s");
}

TEST_CASE("a band covers its stretch of the plot, clipped to the range",
          "[frequency_chart][ripple]") {
    // 0..300 across 600 px from x = 100.
    const auto b = ui_frequency_response_chart_band_pixels({77.0f, 97.0f}, 0.0f, 300.0f, 100, 700);
    REQUIRE(b.has_value());
    CHECK(b->first == 254);
    CHECK(b->second == 294);
    const auto clipped =
        ui_frequency_response_chart_band_pixels({280.0f, 320.0f}, 0.0f, 300.0f, 100, 700);
    REQUIRE(clipped.has_value());
    CHECK(clipped->second == 700);
    CHECK_FALSE(ui_frequency_response_chart_band_pixels({310.0f, 320.0f}, 0.0f, 300.0f, 100, 700));
}

TEST_CASE_METHOD(LVGLTestFixture, "bands shade the plot until cleared, across a range change",
                 "[frequency_chart][ripple]") {
    lv_obj_t* screen = test_screen();
    auto* chart = ui_frequency_response_chart_create(screen);
    REQUIRE(chart != nullptr);
    lv_obj_set_size(ui_frequency_response_chart_get_obj(chart), 600, 300);
    ui_frequency_response_chart_configure_for_platform(chart, helix::PlatformTier::STANDARD);
    ui_frequency_response_chart_set_x_unit(chart, "mm/s");
    CHECK(std::string(ui_frequency_response_chart_get_x_unit(chart)) == "mm/s");
    ui_frequency_response_chart_set_freq_range(chart, 0.0f, 300.0f);
    lv_obj_update_layout(screen);

    lv_obj_t* plot = plot_of(chart);
    REQUIRE(plot != nullptr);
    lv_area_t content;
    lv_obj_get_content_coords(plot, &content);
    const int32_t y = (content.y1 + content.y2) / 2;
    const auto at = [&](float speed) {
        const float frac = speed / 300.0f;
        return content.x1 + static_cast<int32_t>(frac * lv_area_get_width(&content));
    };
    const uint32_t bare = pixel_at(plot, at(87.0f), y);

    const FrChartBand bands[] = {{77.0f, 97.0f}, {154.0f, 179.0f}};
    ui_frequency_response_chart_set_bands(chart, bands, 2);
    CHECK(ui_frequency_response_chart_get_band_count(chart) == 2);
    CHECK(pixel_at(plot, at(87.0f), y) != bare);
    CHECK(pixel_at(plot, at(120.0f), y) == bare);

    ui_frequency_response_chart_set_freq_range(chart, 0.0f, 250.0f);
    CHECK(ui_frequency_response_chart_get_band_count(chart) == 2);

    ui_frequency_response_chart_set_bands(chart, nullptr, 0);
    CHECK(ui_frequency_response_chart_get_band_count(chart) == 0);
    ui_frequency_response_chart_set_freq_range(chart, 0.0f, 300.0f);
    CHECK(pixel_at(plot, at(87.0f), y) == bare);
    ui_frequency_response_chart_destroy(chart);
}

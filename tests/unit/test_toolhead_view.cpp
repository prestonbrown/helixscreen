// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolhead_view.cpp
 * @brief fit_toolhead() and the <toolhead_view> widget: the configured head,
 *        as large as fits its box, centred, for every style.
 */

#include "ui_toolhead_view.h"

#include "../lvgl_ui_test_fixture.h"
#include "settings_manager.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

using helix::ToolheadStyle;
using helix::ui::fit_toolhead;
using helix::ui::toolhead_bounds;

namespace {

/// The glyph draw_toolhead() makes at @p fit, for @p style (A4T is drawn 6/5 up).
helix::ui::GlyphBounds drawn(ToolheadStyle style, const helix::ui::ToolheadFit& fit) {
    const int32_t s = style == ToolheadStyle::A4T ? fit.scale * 6 / 5 : fit.scale;
    return toolhead_bounds(style, fit.cx, fit.nozzle_y, s);
}

bool inside(const helix::ui::GlyphBounds& g, const lv_area_t& box) {
    return g.left >= box.x1 && g.right <= box.x2 && g.top >= box.y1 && g.bottom <= box.y2;
}

} // namespace

TEST_CASE("fit_toolhead fills the box, centred, for every style", "[toolhead_view]") {
    const ToolheadStyle styles[] = {ToolheadStyle::DEFAULT,       ToolheadStyle::A4T,
                                    ToolheadStyle::ANTHEAD,       ToolheadStyle::JABBERWOCKY,
                                    ToolheadStyle::STEALTHBURNER, ToolheadStyle::CREALITY_K1,
                                    ToolheadStyle::CREALITY_K2};
    const lv_area_t boxes[] = {{10, 20, 69, 79}, {0, 0, 199, 59}, {5, 5, 34, 124}};
    for (auto style : styles) {
        for (const auto& box : boxes) {
            CAPTURE(static_cast<int>(style), box.x1, box.y1, box.x2, box.y2);
            const auto fit = fit_toolhead(style, box);
            REQUIRE(fit.scale >= 1);
            const auto g = drawn(style, fit);
            CHECK(inside(g, box));
            // The largest that fits: one step up spills out.
            CHECK_FALSE(inside(drawn(style, {fit.cx, fit.nozzle_y, fit.scale + 1}), box));
            // Centred: the slack on each side differs by at most two pixels.
            CHECK(std::abs((g.left - box.x1) - (box.x2 - g.right)) <= 2);
            CHECK(std::abs((g.top - box.y1) - (box.y2 - g.bottom)) <= 2);
        }
    }
}

TEST_CASE("fit_toolhead honours a scale ceiling and a box too small", "[toolhead_view]") {
    const lv_area_t big{0, 0, 999, 999};
    CHECK(fit_toolhead(ToolheadStyle::DEFAULT, big, 12).scale == 12);
    const lv_area_t tiny{0, 0, 2, 2};
    CHECK(fit_toolhead(ToolheadStyle::DEFAULT, tiny).scale == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "toolhead_view is an XML widget that draws",
                 "[toolhead_view]") {
    const char* attrs[] = {"width", "60", "height", "60", nullptr};
    auto* view = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "toolhead_view", attrs));
    REQUIRE(view != nullptr);
    process_lvgl(30); // a draw pass must not fault
    // A style change redraws it: the widget asks for a redraw on the setting.
    const auto before = helix::SettingsManager::instance().get_toolhead_style();
    helix::SettingsManager::instance().set_toolhead_style(ToolheadStyle::STEALTHBURNER);
    process_lvgl(30);
    helix::SettingsManager::instance().set_toolhead_style(before);
    lv_obj_delete(view);
}

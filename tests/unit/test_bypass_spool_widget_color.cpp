// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_bypass_spool_widget_color.cpp
 * @brief The bypass swatch must paint black, which is the same integer as "unset".
 *
 * K2 Plus, 2026-08-24: the bypass spool was assigned Spoolman spool 86
 * ("Black ASA", color_rgb 0) and the AMS panel drew it WHITE. Everything
 * upstream was correct — settings.json held color_rgb 0, the notification
 * carried it, and the panel re-fired on every change. The paint was skipped:
 * bypass_spool_set_color()'s dedup guard compared against a cached value whose
 * default was also 0, so black read as "unchanged" on the first call and every
 * call after, leaving ui_spool_canvas's own creation-time default (0xE0E0E0,
 * "Default white/light filament") on screen forever.
 *
 * The same black-is-not-unset trap is already handled in
 * AmsState::notify_external_spool_changed() and in the lane_data identity
 * check; this widget was the one that was missed.
 */

#include "ui_bypass_spool_widget.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"

#include "../catch_amalgamated.hpp"

using helix::ui::bypass_spool_create;
using helix::ui::bypass_spool_set_color;
using helix::ui::BypassSpoolWidgets;

namespace {

/// The canvas's own pre-any-set_color() default (ui_spool_canvas.cpp).
constexpr uint32_t CANVAS_DEFAULT = 0xE0E0E0;

uint32_t canvas_rgb(const BypassSpoolWidgets& w) {
    lv_color_t c = ui_spool_canvas_get_color(w.spool_canvas);
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "bypass swatch paints a black spool black",
                 "[ams][bypass][spool_widget]") {
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    REQUIRE(w.spool_canvas != nullptr);

    // Pre-condition: the canvas starts on its own light default, which is what
    // the user actually saw.
    REQUIRE(canvas_rgb(w) == CANVAS_DEFAULT);

    bypass_spool_set_color(w, 0x000000);

    CHECK(canvas_rgb(w) == 0x000000);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass swatch still dedups a repeated colour",
                 "[ams][bypass][spool_widget]") {
    // The guard exists to keep panel refreshes from invalidating the canvas on
    // every tick. Fixing black must not cost that.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    bypass_spool_set_color(w, 0x00AEFF);
    REQUIRE(w.color_painted);
    REQUIRE(w.cached_color_rgb == 0x00AEFF);

    // A second identical set is a no-op; a different one is not.
    bypass_spool_set_color(w, 0x00AEFF);
    CHECK(canvas_rgb(w) == 0x00AEFF);

    bypass_spool_set_color(w, 0x000000);
    CHECK(canvas_rgb(w) == 0x000000);
    CHECK(w.color_painted);

    helix::ui::bypass_spool_destroy(w);
}

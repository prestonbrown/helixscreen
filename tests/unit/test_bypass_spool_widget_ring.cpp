// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_bypass_spool_widget_ring.cpp
 * @brief The bypass node wears the same active ring a lane slot does.
 *
 * A lane slot gets a primary border plus outer glow while it is the node
 * feeding the toolhead (apply_current_slot_highlight). Bypass is a node on the
 * same path and feeds the toolhead the same way, but had no such marker - so an
 * engaged bypass looked identical to an idle one, and the only clue was the
 * filament panel's own controls.
 *
 * The styling lives in ams_draw::set_active_ring() precisely so the two cannot
 * drift; these tests pin the widget's use of it, including the change-detection
 * pair that keeps panel refreshes from restyling on every tick.
 */

#include "ui_ams_lane_spool.h"
#include "ui_bypass_spool_widget.h"

#include "../test_fixtures.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include "../catch_amalgamated.hpp"

using helix::ui::bypass_spool_create;
using helix::ui::bypass_spool_set_active;
using helix::ui::BypassSpoolWidgets;

TEST_CASE_METHOD(LVGLTestFixture, "bypass node draws the active ring when engaged",
                 "[ams][bypass][spool_widget]") {
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    bypass_spool_set_active(w, true);

    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);
    CHECK(lv_obj_get_style_border_opa(w.box, LV_PART_MAIN) == LV_OPA_COVER);
    // The glow is what makes it read as "active" rather than merely outlined.
    CHECK(lv_obj_get_style_shadow_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingGlowWidth);
    CHECK(lv_obj_get_style_shadow_spread(w.box, LV_PART_MAIN) == ams_draw::kActiveRingGlowSpread);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass node clears the ring when disengaged",
                 "[ams][bypass][spool_widget]") {
    // Disengaging must actually remove it. A ring left behind says the printer
    // is feeding from bypass when it is not - worse than never drawing one.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());

    bypass_spool_set_active(w, true);
    REQUIRE(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);

    bypass_spool_set_active(w, false);

    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_shadow_width(w.box, LV_PART_MAIN) == 0);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(LVGLTestFixture, "bypass ring applies on the first call even when inactive",
                 "[ams][bypass][spool_widget]") {
    // The same trap the colour guard fell into: "off" is the natural default,
    // so a bare cached_active could not tell "never applied" from "already
    // off". active_applied is the bit that separates them.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    REQUIRE_FALSE(w.active_applied);

    bypass_spool_set_active(w, false);
    CHECK(w.active_applied);
    CHECK_FALSE(w.cached_active);

    // And the guard still dedups once applied.
    bypass_spool_set_active(w, true);
    CHECK(w.cached_active);
    CHECK(lv_obj_get_style_border_width(w.box, LV_PART_MAIN) == ams_draw::kActiveRingBorderWidth);

    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE_METHOD(XMLTestFixture, "bypass spool is drawn the size of the lane spools beside it",
                 "[ams][bypass][spool_widget]") {
    // The bypass node sits on the same path as the lanes; a spool smaller than
    // theirs reads as a different kind of thing.
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    lv_obj_update_layout(w.box);

    const int32_t lane = helix::ui::ams_slot_spool_size();
    REQUIRE(lane == theme_manager_get_spacing("ams_slot_spool_size"));
    CHECK(lv_obj_get_width(w.spool_canvas) == lane);
    CHECK(lv_obj_get_height(w.spool_canvas) == lane);
    CHECK(lv_obj_get_width(w.box) > lane);

    helix::ui::bypass_spool_destroy(w);
}

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

TEST_CASE_METHOD(XMLTestFixture, "bypass spool set to a size draws its spool and card at it",
                 "[ams][bypass][spool_widget]") {
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    const int32_t lane = helix::ui::ams_slot_spool_size();
    helix::ui::bypass_spool_set_size(w, lane + 40);
    helix::ui::bypass_spool_set_position(w, 200, 300);
    lv_obj_update_layout(w.box);
    CHECK(lv_obj_get_width(w.spool_canvas) == lane + 40);
    CHECK(lv_obj_get_height(w.spool_canvas) == lane + 40);
    CHECK(w.box_size == helix::ui::bypass_spool_box_size(lane + 40));
    CHECK(lv_obj_get_width(w.box) == w.box_size);
    CHECK(lv_obj_get_height(w.box) == w.box_size);
    // It stands centred over the point it is placed at, its spool's bottom where its tube
    // turns along the merge line.
    lv_area_t box, spool;
    lv_obj_get_coords(w.box, &box);
    lv_obj_get_coords(w.spool_canvas, &spool);
    CHECK(box.x1 + w.box_size / 2 == 200);
    CHECK(spool.y2 + 1 == 300 - helix::ui::bypass_spool_bottom_above_merge());
    helix::ui::bypass_spool_destroy(w);
}

TEST_CASE("bypass spool beside a drawn unit takes the unit's spool size as far as there is room",
          "[ams][bypass][spool_widget]") {
    using helix::ui::bypass_spool_box_size;
    using helix::ui::bypass_spool_label_room;
    using helix::ui::bypass_spool_spot;
    using helix::ui::BypassSpoolRoom;
    constexpr int32_t kMin = 64;
    BypassSpoolRoom room;
    room.home_cx = 400;
    room.right = 600;
    room.box_bottom = 500;
    room.top = 0;
    room.gap = 8;
    const auto clear = [](int32_t, int32_t) { return INT32_MIN; };

    SECTION("an outline row just above the card's bottom edge pushes it right") {
        // The unit reaches x=480 in one row only, two above the card's bottom.
        const int32_t row = room.box_bottom - 2;
        const auto unit = [row](int32_t top, int32_t bottom) {
            return top <= row && row <= bottom ? 480 : INT32_MIN;
        };
        const auto spot = bypass_spool_spot(120, kMin, room, unit);
        CHECK(spot.cx - spot.box / 2 >= 480 + room.gap);
    }
    SECTION("with nothing beside it, it is the unit's spool size at its own spot") {
        const auto spot = bypass_spool_spot(120, kMin, room, clear);
        CHECK(spot.spool == 120);
        CHECK(spot.box == bypass_spool_box_size(120));
        CHECK(spot.cx == 400);
    }
    SECTION("a unit whose spools are smaller keeps it at the lane size everywhere else") {
        const auto spot = bypass_spool_spot(50, kMin, room, clear);
        CHECK(spot.spool == kMin);
        CHECK(spot.cx == 400);
    }
    SECTION("it moves right to clear the unit, as far as the room's right edge") {
        const auto unit = [](int32_t, int32_t) { return 380; };
        const auto spot = bypass_spool_spot(120, kMin, room, unit);
        CHECK(spot.spool == 120);
        CHECK(spot.cx - spot.box / 2 >= 380 + room.gap);
        CHECK(spot.cx + (spot.box - spot.box / 2) <= room.right);
    }
    SECTION("with too little room right of the unit it is the largest card that fits") {
        const auto unit = [](int32_t, int32_t) { return 500; };
        const auto spot = bypass_spool_spot(120, kMin, room, unit);
        CHECK(spot.box == room.right - (500 + room.gap));
        CHECK(spot.cx == room.right - (spot.box - spot.box / 2));
    }
    SECTION("only the unit's rows from the card's label down count") {
        // The unit reaches far right only above y=300: a card whose label stays below it is
        // free of it, so the spool is as large as keeps the label under y=300.
        const auto unit = [](int32_t top, int32_t) { return top < 300 ? 590 : 200; };
        const auto spot = bypass_spool_spot(400, kMin, room, unit);
        CHECK(room.box_bottom - spot.box - bypass_spool_label_room() >= 300);
        CHECK(room.box_bottom - bypass_spool_box_size(spot.spool + 1) - bypass_spool_label_room() <
              300);
    }
    SECTION("it rises no higher than the room's top") {
        room.top = 400;
        const auto spot = bypass_spool_spot(200, kMin, room, clear);
        CHECK(room.box_bottom - spot.box - bypass_spool_label_room() >= room.top);
        CHECK(spot.spool > kMin);
    }
    SECTION("when no card larger than the lane size fits, it keeps that size at its own spot") {
        const auto unit = [](int32_t, int32_t) { return 590; };
        const auto spot = bypass_spool_spot(120, kMin, room, unit);
        CHECK(spot.spool == kMin);
        CHECK(spot.box == bypass_spool_box_size(kMin));
        CHECK(spot.cx == 400);
    }
}

TEST_CASE("bypass spool below a drawn unit stands right of the hub, its tube into its side",
          "[ams][bypass][spool_widget]") {
    using helix::ui::bypass_spool_box_size;
    using helix::ui::bypass_spool_label_room;
    using helix::ui::bypass_spool_spot_below;
    using helix::ui::BypassSpoolBelowRoom;
    constexpr int32_t kMin = 64;
    BypassSpoolBelowRoom room;
    room.left = 300;
    room.right = 520;
    room.bottom = 460;
    room.merge_y = 400;
    room.gap = 8;
    // The unit ends at y=330 everywhere right of x=300.
    const auto unit = [](int32_t x) { return x < 430 ? 330 : INT32_MIN; };

    SECTION("it is as large as fits under the unit and its label, over the label below it") {
        const auto spot = bypass_spool_spot_below(200, kMin, room, unit);
        REQUIRE(spot.side_entry);
        CHECK(spot.top >= 330 + room.gap + bypass_spool_label_room());
        CHECK(spot.top + spot.box <= room.bottom);
        // One more pixel would not fit between the unit and the bottom.
        CHECK(bypass_spool_box_size(spot.spool + 1) >
              room.bottom - (330 + room.gap + bypass_spool_label_room()));
        CHECK(spot.cx - spot.box / 2 >= room.left);
        CHECK(spot.cx + (spot.box - spot.box / 2) <= room.right);
        // The tube runs into the card's side, clear of its corners.
        CHECK(spot.top < room.merge_y);
        CHECK(spot.top + spot.box > room.merge_y);
    }
    SECTION("a card narrower than the room stands at its right, away from the unit") {
        room.left = 100;
        const auto spot = bypass_spool_spot_below(80, kMin, room, [](int32_t) { return 200; });
        REQUIRE(spot.spool == 80);
        CHECK(spot.cx + (spot.box - spot.box / 2) == room.right);
    }
    SECTION("a merge line it cannot stand across leaves no lower spot") {
        // The merge line runs just under a unit that spans the whole room.
        room.merge_y = 340;
        const auto spot = bypass_spool_spot_below(200, kMin, room, [](int32_t) { return 330; });
        CHECK_FALSE(spot.side_entry);
        CHECK(spot.spool == kMin);
    }
}

TEST_CASE("bypass spool takes the lower spot only when it is larger than the one beside",
          "[ams][bypass][spool_widget]") {
    using helix::ui::bypass_spool_better;
    using helix::ui::BypassSpoolSpot;
    const BypassSpoolSpot beside_small{67, 75, 450, 0, false};
    const BypassSpoolSpot beside_full{117, 125, 450, 0, false};
    const BypassSpoolSpot below{101, 109, 460, 340, true};

    SECTION("lower spot wins when the beside spot is cramped") {
        const BypassSpoolSpot spot = bypass_spool_better(beside_small, below, 117);
        CHECK(spot.side_entry);
        CHECK(spot.spool == 101);
    }
    SECTION("beside spot wins when it reaches the lane size") {
        const BypassSpoolSpot larger_below{117, 125, 460, 300, true};
        CHECK_FALSE(bypass_spool_better(beside_full, larger_below, 117).side_entry);
    }
    SECTION("beside spot wins a tie and when the lower one is smaller") {
        const BypassSpoolSpot tie{67, 75, 460, 340, true};
        CHECK_FALSE(bypass_spool_better(beside_small, tie, 117).side_entry);
        const BypassSpoolSpot big_beside{110, 118, 450, 0, false};
        CHECK_FALSE(bypass_spool_better(big_beside, below, 117).side_entry);
    }
}

TEST_CASE_METHOD(XMLTestFixture,
                 "bypass spool's card bottom over its merge line is where it stands",
                 "[ams][bypass][spool_widget]") {
    BypassSpoolWidgets w = bypass_spool_create(lv_screen_active(), nullptr, nullptr);
    REQUIRE(w.valid());
    helix::ui::bypass_spool_set_size(w, helix::ui::ams_slot_spool_size() + 30);
    helix::ui::bypass_spool_set_position(w, 200, 300);
    lv_obj_update_layout(w.box);
    lv_area_t box;
    lv_obj_get_coords(w.box, &box);
    CHECK(box.y2 + 1 == helix::ui::bypass_spool_card_bottom_on(300));
    // The card reaches below its spool by its padding: the spool's bottom is not the card's.
    lv_area_t spool;
    lv_obj_get_coords(w.spool_canvas, &spool);
    CHECK(helix::ui::bypass_spool_card_bottom_on(300) > spool.y2 + 1);
    CHECK(spool.y2 + 1 == 300 - helix::ui::bypass_spool_bottom_above_merge());
    helix::ui::bypass_spool_destroy(w);
}

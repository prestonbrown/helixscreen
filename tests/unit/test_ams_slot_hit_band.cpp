// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A row of five or more slots overlaps by half a slot: each slot must answer
// taps on its own spool only, and its number badge must sit on the drawn spool.

#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {
lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}
} // namespace

TEST_CASE("ams_slot hit band: every slot owns one pitch around its spool, no gap, no shared pixel",
          "[ams_slot][hit_band]") {
    // Four slots sit side by side; five and more overlap by half a slot.
    for (int count : {4, 5, 6, 8, 16}) {
        const AmsSlotLayout layout = calculate_ams_slot_layout(470, count);
        REQUIRE((layout.overlap > 0) == (count > 4));
        const int32_t pitch = layout.slot_width - layout.overlap;
        int32_t previous_right = -1;
        for (int i = 0; i < count; ++i) {
            const helix::ui::AmsSlotHitBand band = helix::ui::ams_slot_hit_band(layout, i, count);
            const int32_t left = i * pitch + band.left_inset;
            const int32_t right = i * pitch + layout.slot_width - 1 - band.right_inset;
            const int32_t spool_center = i * pitch + layout.slot_width / 2;
            CAPTURE(count, i, left, right, spool_center);
            CHECK(left == previous_right + 1); // no gap, no shared pixel
            CHECK(left <= spool_center);
            CHECK(spool_center <= right);
            previous_right = right;
        }
        CHECK(previous_right == (count - 1) * pitch + layout.slot_width - 1);
    }
}

namespace {
lv_obj_t* slot_owning(lv_obj_t* hit, lv_obj_t* const* slots, int count) {
    for (lv_obj_t* obj = hit; obj; obj = lv_obj_get_parent(obj)) {
        for (int i = 0; i < count; ++i) {
            if (slots[i] == obj)
                return obj;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_slot: a tap on a spool in an overlapping row opens that slot",
                 "[ui][ams_slot][hit_band]") {
    ui_ams_slot_register();
    constexpr int kCount = 8;

    lv_obj_t* row = lv_obj_create(test_screen());
    lv_obj_set_size(row, 700, 300);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t* slots[kCount] = {};
    for (int i = 0; i < kCount; ++i) {
        slots[i] = create_ams_slot(row, i);
        REQUIRE(slots[i] != nullptr);
        ui_ams_slot_set_layout_info(slots[i], i, kCount);
        // Every child a lane can show, shown: an empty lane's placeholder and a
        // faulted lane's error dot both stand on top of the spool.
        for (const char* name : {"empty_placeholder", "error_indicator"}) {
            lv_obj_t* child = UITest::find_by_name(slots[i], name);
            REQUIRE(child != nullptr);
            lv_obj_set_hidden(child, false);
        }
    }
    lv_obj_update_layout(row);

    // A row as tight as the smallest screens: neighbouring spools, their
    // containers and their placeholders all overlap, as well as the slots.
    lv_obj_t* placeholder = UITest::find_by_name(slots[0], "empty_placeholder");
    const int32_t spool_w = lv_obj_get_width(UITest::find_by_name(slots[0], "spool_container"));
    const int32_t placeholder_w = lv_obj_get_width(placeholder);
    // The error dot stands off the spool's centre, past the half pitch, so it
    // overhangs the neighbour's band as it does on a 480x320 screen of 8 lanes.
    lv_area_t ph_area;
    lv_area_t dot_area0;
    lv_obj_get_coords(placeholder, &ph_area);
    lv_obj_get_coords(UITest::find_by_name(slots[0], "error_indicator"), &dot_area0);
    const int32_t dot_offset = dot_area0.x1 - (ph_area.x1 + ph_area.x2) / 2;
    REQUIRE(dot_offset > 4);
    const int32_t target_pitch = std::min<int32_t>(placeholder_w * 3 / 4, 2 * dot_offset - 6);
    const AmsSlotLayout layout =
        calculate_ams_slot_layout(target_pitch * 2 * (kCount + 1) / 2, kCount);
    const int32_t pitch = layout.slot_width - layout.overlap;
    REQUIRE(layout.overlap > 0);
    REQUIRE(spool_w > pitch + 4);
    REQUIRE(placeholder_w > pitch + 4);
    REQUIRE(dot_offset > pitch / 2 + 1);

    lv_obj_set_style_pad_column(row, -layout.overlap, LV_PART_MAIN);
    for (int i = 0; i < kCount; ++i) {
        lv_obj_set_width(slots[i], layout.slot_width);
        const helix::ui::AmsSlotHitBand band = helix::ui::ams_slot_hit_band(layout, i, kCount);
        helix::ui::ams_slot_set_hit_band(slots[i], band.left_inset, band.right_inset);
    }
    lv_obj_update_layout(row);
    lv_area_t bands[kCount];
    for (int i = 0; i < kCount; ++i) {
        const helix::ui::AmsSlotHitBand band = helix::ui::ams_slot_hit_band(layout, i, kCount);
        lv_obj_get_coords(slots[i], &bands[i]);
        bands[i].x1 += band.left_inset;
        bands[i].x2 -= band.right_inset;
    }

    lv_area_t spool_area;
    lv_area_t dot_area;
    lv_obj_get_coords(placeholder, &spool_area);
    lv_obj_get_coords(UITest::find_by_name(slots[0], "error_indicator"), &dot_area);
    const int32_t rows_y[] = {(spool_area.y1 + spool_area.y2) / 2, (dot_area.y1 + dot_area.y2) / 2};

    // Every pixel across the row, at the spool's middle and at the error dot's
    // height, belongs to the slot whose band holds it.
    for (int32_t y : rows_y) {
        for (int32_t x = bands[0].x1; x <= bands[kCount - 1].x2; ++x) {
            int owner = -1;
            for (int i = 0; i < kCount; ++i) {
                if (x >= bands[i].x1 && x <= bands[i].x2)
                    owner = i;
            }
            lv_point_t p{x, y};
            CAPTURE(x, y, owner);
            REQUIRE(owner >= 0);
            REQUIRE(slot_owning(lv_indev_search_obj(row, &p), slots, kCount) == slots[owner]);
        }
    }
    lv_obj_delete(row);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_slot: a staggered slot's badge sits on the drawn spool's lower corner",
                 "[ui][ams_slot][badge]") {
    ui_ams_slot_register();
    lv_obj_t* slot = create_ams_slot(test_screen(), 0);
    REQUIRE(slot != nullptr);
    ui_ams_slot_set_layout_info(slot, 0, 6);
    lv_obj_set_width(slot, 118);
    lv_obj_update_layout(slot);

    lv_obj_t* label = UITest::find_by_name(slot, "material_label");
    REQUIRE(label != nullptr);
    REQUIRE(lv_obj_is_ignore_layout(label)); // staggered: out of the flow

    lv_obj_t* badge = UITest::find_by_name(slot, "status_badge");
    REQUIRE(badge != nullptr);

    lv_obj_t* layer = lv_obj_create(test_screen());
    lv_obj_set_style_pad_all(layer, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(layer, 0, LV_PART_MAIN);
    lv_obj_set_pos(layer, lv_obj_get_x(slot), lv_obj_get_y(slot));
    lv_obj_set_size(layer, 300, 300);
    lv_obj_update_layout(layer);

    lv_obj_t* graphic = UITest::find_by_name(slot, "spool_graphic");
    REQUIRE(graphic != nullptr);
    lv_area_t slot_area;
    lv_area_t spool_area;
    lv_obj_get_coords(slot, &slot_area);
    lv_obj_get_coords(graphic, &spool_area);
    const int32_t slot_center_x = (slot_area.x2 - slot_area.x1 + 1) / 2;

    ui_ams_slot_move_badge_to_layer(slot, layer, slot_center_x);
    lv_obj_update_layout(layer);
    REQUIRE(lv_obj_get_parent(badge) == layer);

    // Inset by the badge's own 2px translate from the drawn spool's corner.
    lv_area_t on_layer;
    lv_obj_get_coords(badge, &on_layer);
    CHECK(on_layer.y2 == spool_area.y2 - 2);
    CHECK(on_layer.x2 == spool_area.x2 - 2);

    ui_ams_slot_detach_layers(slot);
    lv_obj_delete(layer);
    lv_obj_delete(slot);
}

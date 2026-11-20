// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_temp_graph_swatches.cpp
 * @brief The temperature graph overlay's series chips show their colour as a
 *        swatch-painter circle.
 *
 * Run with: ./build/bin/helix-tests "[temp_graph][swatch]"
 */

#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"

#include <array>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

class TempGraphSwatchFixture : public LVGLUITestFixture {
  protected:
    TempGraphSwatchFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~TempGraphSwatchFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(TempGraphSwatchFixture, "Temp graph series chips paint their dot as a swatch",
                 "[temp_graph][swatch]") {
    auto& overlay = get_global_temp_graph_overlay();
    overlay.open(TempGraphOverlay::Mode::Nozzle, lv_screen_active());
    settle();
    lv_obj_t* root = overlay.get_root();
    REQUIRE(root != nullptr);
    lv_obj_t* chip_row = lv_obj_find_by_name(root, "chip_row");
    REQUIRE(chip_row != nullptr);
    const uint32_t chips = lv_obj_get_child_count(chip_row);
    REQUIRE(chips > 0);
    for (uint32_t i = 0; i < chips; ++i) {
        INFO("chip " << i);
        lv_obj_t* dot = lv_obj_get_child(lv_obj_get_child(chip_row, static_cast<int32_t>(i)), 0);
        REQUIRE(dot != nullptr);
        CHECK(helix::ui::has_swatch_paint(dot));
        CHECK(lv_obj_get_style_radius(dot, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
        // The dot shows its series' colour, as the chip's colour subject holds it.
        lv_subject_t* color =
            lv_xml_get_subject(nullptr, ("temp_graph_chip_color_" + std::to_string(i)).c_str());
        REQUIRE(color != nullptr);
        CHECK(lv_color_eq(helix::ui::swatch_color_of(dot), lv_subject_get_color(color)));
    }
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(TempGraphSwatchFixture, "Temp graph series chip dims while its series is hidden",
                 "[temp_graph][swatch]") {
    auto& overlay = get_global_temp_graph_overlay();
    overlay.open(TempGraphOverlay::Mode::Nozzle, lv_screen_active());
    settle();
    lv_obj_t* chip_row = lv_obj_find_by_name(overlay.get_root(), "chip_row");
    REQUIRE(chip_row != nullptr);
    REQUIRE(lv_obj_get_child_count(chip_row) > 0);
    lv_obj_t* chip = lv_obj_get_child(chip_row, 0);
    const lv_opa_t shown = lv_obj_get_style_opa(chip, LV_PART_MAIN);

    lv_obj_send_event(chip, LV_EVENT_CLICKED, nullptr);
    settle();
    const lv_opa_t after_one = lv_obj_get_style_opa(chip, LV_PART_MAIN);
    lv_obj_send_event(chip, LV_EVENT_CLICKED, nullptr);
    settle();
    const lv_opa_t after_two = lv_obj_get_style_opa(chip, LV_PART_MAIN);

    // One tap flips the series; the dimmed chip is the 40% one either way round.
    CHECK(after_one != shown);
    CHECK(after_two == shown);
    CHECK((shown == LV_OPA_40 || after_one == LV_OPA_40));
    NavigationManager::instance().go_back();
    settle();
}

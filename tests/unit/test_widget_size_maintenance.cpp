// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_maintenance.cpp
 * @brief The maintenance tile draws what its ladder promises at each medium-tier box:
 * the published rung, which parts carry HIDDEN, the square ring, and the total line.
 *
 * The decision is maintenance_tile_layout.h's; these pin its wiring to the real fonts,
 * XML and model.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][maintenance]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/tile_boxes.h"
#include "config.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "panel_widget_manager.h"
#include "src/ui/panel_widgets/maintenance_model.h"
#include "src/ui/panel_widgets/maintenance_widget.h"
#include "theme_manager.h"
#include "xml_registration.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;
using test::Box;
using test::kCell;
using test::kFull;
using test::kStrip;
using test::kWide;

namespace {

struct MaintenanceTileFixture : public LVGLUITestFixture {
    MaintenanceTileFixture() {
        theme_manager_refresh_layout_constants(lv_display_get_default());
        helix::unregister_idle_xml_components();
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        PanelWidgetManager::instance().init_widget_subjects();
        drain();
    }
    ~MaintenanceTileFixture() override {
        StaticSubjectRegistry::instance().deinit_one("MaintenanceModelSubjects");
        drain();
    }
    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return false;
    }
    return true;
}

bool shown(PanelWidgetHarness<MaintenanceWidget>& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return shown(obj);
}

} // namespace

TEST_CASE_METHOD(MaintenanceTileFixture, "maintenance tile: medium-tier boxes land on their rung",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());
    for (const Box& b : {kStrip, kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(subject_int("maintenance_rung") == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(MaintenanceTileFixture, "maintenance tile: each rung shows its own parts",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());

    h.resize(1, 1, kStrip.w, kStrip.h);
    CHECK(shown(h, "mt_strip"));
    CHECK_FALSE(shown(h, "mt_ring"));
    CHECK_FALSE(shown(h, "mt_full"));

    h.resize(1, 1, kCell.w, kCell.h);
    CHECK(shown(h, "mt_ring"));
    CHECK(shown(h, "mt_ring_name"));
    CHECK_FALSE(shown(h, "mt_list"));
    CHECK_FALSE(shown(h, "mt_strip"));

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(shown(h, "mt_ring"));
    CHECK(shown(h, "mt_list"));
    CHECK(shown(h, "mt_list_row_2"));
    CHECK_FALSE(shown(h, "mt_ring_name"));

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(h, "mt_full"));
    CHECK(shown(h, "mt_row_0"));
    CHECK(shown(h, "mt_total"));
    CHECK_FALSE(shown(h, "mt_main"));
}

TEST_CASE_METHOD(MaintenanceTileFixture,
                 "maintenance tile: the ring is a square inside the tile, centred under its text",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());
    for (const Box& b : {kCell, kWide}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        lv_obj_t* dial = h.child("mt_dial");
        lv_obj_t* hours = h.child("mt_ring_hours");
        REQUIRE(dial != nullptr);
        REQUIRE(hours != nullptr);
        const int d = subject_int("maintenance_ring_d");
        CHECK(d > 0);
        CHECK(lv_obj_get_width(dial) == d);
        CHECK(lv_obj_get_height(dial) == d);
        lv_area_t tile{}, ring{}, text{};
        lv_obj_get_coords(h.child("mt_main"), &tile);
        lv_obj_get_coords(dial, &ring);
        lv_obj_get_coords(hours, &text);
        CHECK(ring.y1 >= tile.y1);
        CHECK(ring.y2 <= tile.y2);
        CHECK(std::abs((ring.x1 + ring.x2) / 2 - (text.x1 + text.x2) / 2) <= 1);
    }
}

TEST_CASE_METHOD(MaintenanceTileFixture,
                 "maintenance tile: a meter turned off leaves its rows, not the rung",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());
    h.resize(1, 1, kWide.w, kWide.h);
    maintenance_model::set_interval_choice(static_cast<int>(maintenance::MeterId::Rails), 0);
    drain();
    CHECK_FALSE(shown(h, "mt_list_row_2"));
    CHECK(shown(h, "mt_list_row_0"));
    CHECK(subject_int("maintenance_rung") == static_cast<int>(kWide.rung));
}

TEST_CASE_METHOD(MaintenanceTileFixture,
                 "maintenance tile: a box draws the same parts whatever it was before",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());
    for (int w : {test::kTileCellW, test::kTileWideW}) {
        INFO(w);
        CHECK(h.unsettled_boxes(box_sweep(w, w, 40, 240)) == std::vector<std::string>{});
    }
}

TEST_CASE_METHOD(MaintenanceTileFixture, "maintenance tile: fits_at refuses a box no rung holds",
                 "[widget_size][maintenance]") {
    PanelWidgetHarness<MaintenanceWidget> h(test_screen());
    CHECK(h.widget().fits_at(kStrip.w, kStrip.h));
    CHECK(h.widget().fits_at(kCell.w, kCell.h));
    CHECK_FALSE(h.widget().fits_at(20, 20));
}

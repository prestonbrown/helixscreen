// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AMS panel's dry-box unit as laid out: spools stand in the box, labels sit
// above its lid (or back wall), the readout stands beside the drum, and under
// per-lane lids each lane shows its own humidity behind a droplet.

#include "ui_ams_detail.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "theme_manager.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
namespace tray = helix::ui::tray;

namespace {

void register_ams_xml_once() {
    static bool done = false;
    if (done)
        return;
    ui_spool_canvas_register();
    ui_ams_slot_register();
    ui_filament_path_canvas_register();
    ui_endless_spool_arrows_register();
    helix::ui::AmsOperationSidebar::register_callbacks_static();
    lv_xml_register_component_from_file("A:ui_xml/components/ams_unit_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_loaded_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_environment_indicator.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    done = true;
}

class AmsTrayPanelFixture : public XMLTestFixture {
  public:
    void build(const char* env_mode) {
        auto mock = std::make_unique<AmsBackendMock>(4);
        mock->set_afc_mode(true);
        mock->set_environment_mode(env_mode);
        REQUIRE(mock->start().success());
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        register_ams_xml_once();

        panel_ = std::make_unique<AmsPanel>(state(), &api());
        panel_->init_subjects();
        panel_obj_ = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());
        panel_->on_activate();
        lv_obj_update_layout(test_screen());
        process_lvgl(50);
        panel_->refresh_slots();
        process_lvgl(20);
        lv_obj_update_layout(test_screen());

        container_ = lv_obj_find_by_name(panel_obj_, "slot_container");
        slot_grid_ = lv_obj_find_by_name(panel_obj_, "slot_grid");
        REQUIRE(container_ != nullptr);
        REQUIRE(slot_grid_ != nullptr);
        lv_area_t c;
        lv_obj_get_coords(container_, &c);
        origin_ = {c.x1, c.y1};
        REQUIRE(helix::ui::ams_detail_tray_geometry(box_, lid_, lid_h_, half_));
    }

    ~AmsTrayPanelFixture() override {
        if (panel_) {
            panel_->on_deactivate(DeactivateReason::NavigateAway);
            panel_->clear_panel_reference();
        }
        if (panel_obj_)
            lv_obj_delete(panel_obj_);
        process_lvgl(10);
        AmsState::instance().set_backend(nullptr);
    }

    lv_obj_t* slot(int i) const {
        return lv_obj_get_child(slot_grid_, i);
    }

    float top() const {
        return tray::unit_top_y(box_, lid_h_, lid_ != tray::LidMode::None);
    }

    std::unique_ptr<AmsPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* slot_grid_ = nullptr;
    lv_point_t origin_{};
    tray::TrayBox box_{};
    tray::LidMode lid_ = tray::LidMode::None;
    float lid_h_ = 0;
    float half_ = 0;
};

} // namespace

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit with a reading: spools in the box under one lid",
                 "[ams][tray][ui_integration]") {
    build("passive");
    REQUIRE(lid_ == tray::LidMode::Unit);
    const int32_t space_md = theme_manager_get_spacing("space_md");
    const float skew = tray::DEPTH_SKEW * box_.depth;

    float first_x = 0, last_x = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const float cx = (a.x1 + a.x2 + 1) / 2.0f - origin_.x;
        const float cy = (a.y1 + a.y2 + 1) / 2.0f - origin_.y;
        const float ry = tray::SPOOL_FLANGE_RADIUS * lv_area_get_width(&a);
        const tray::PointF want = tray::spool_center(box_, cx - skew / 2, ry);
        CHECK(std::fabs(cx - want.x) <= 1);
        CHECK(std::fabs(cy - want.y) <= 1);
        if (i == 0)
            first_x = cx - skew / 2;
        last_x = cx - skew / 2;

        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        CHECK(std::fabs((float)(la.y2 - origin_.y) - (top() - space_md)) <= 1);
    }
    // The box ends where the outer lane lids would.
    CHECK(box_.fl == Catch::Approx(first_x - half_).margin(0.01));
    CHECK(box_.fr == Catch::Approx(last_x + half_).margin(0.01));

    lv_obj_t* readout = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(readout != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(readout, LV_OBJ_FLAG_HIDDEN));
    lv_area_t ra;
    lv_obj_get_coords(readout, &ra);
    const tray::PointF br_t = tray::tray_faces(box_).back_wall[1];
    CHECK(std::fabs((float)(ra.x1 - origin_.x) - (br_t.x + space_md)) <= 1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit with no climate data: no lid, labels over the wall and the spools",
                 "[ams][tray][ui_integration]") {
    build("off");
    REQUIRE(lid_ == tray::LidMode::None);
    const int32_t space_md = theme_manager_get_spacing("space_md");
    const float wall_top = box_.ft - box_.back_extra - box_.rise;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(label != nullptr);
        REQUIRE(spool != nullptr);
        lv_area_t la, sa;
        lv_obj_get_coords(label, &la);
        lv_obj_get_coords(spool, &sa);
        const float spool_top = (sa.y1 + sa.y2 + 1) / 2.0f - origin_.y -
                                tray::SPOOL_FLANGE_RADIUS * lv_area_get_width(&sa);
        CHECK((float)(la.y2 - origin_.y) <= wall_top - space_md + 1);
        CHECK((float)(la.y2 - origin_.y) <= spool_top - space_md + 1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS per-lane sensors: each lane's humidity behind a droplet",
                 "[ams][tray][ui_integration]") {
    build("slot");
    REQUIRE(lid_ == tray::LidMode::PerLane);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* row = ui_ams_slot_get_lane_humidity(slot(i));
        REQUIRE(row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
        REQUIRE(lv_obj_get_child_count(row) == 2);
        // The droplet comes first, then the value.
        CHECK(std::strcmp(lv_obj_get_name(lv_obj_get_child(row, 0)), "lane_humidity_icon") == 0);
        const char* value = lv_label_get_text(lv_obj_get_child(row, 1));
        REQUIRE(value != nullptr);
        CHECK(std::strchr(value, '%') != nullptr);

        // Above its material label.
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t ra, la;
        lv_obj_get_coords(row, &ra);
        lv_obj_get_coords(label, &la);
        CHECK(ra.y2 <= la.y1);
    }
}

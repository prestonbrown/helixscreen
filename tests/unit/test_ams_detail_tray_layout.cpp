// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AMS panel's dry-box unit as laid out: spools stand in the box, labels sit
// above its lid (or back wall), the readout is a pill on the box's end, and under
// per-lane lids each lane shows its own humidity behind a droplet.

#include "ui_ams_detail.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_sidebar.h"
#include "ui_ams_slot.h"
#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_humidity_drop.h"
#include "ui_panel_ams.h"
#include "ui_spool_canvas.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/scoped_geometry.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "config.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "src/ui/filament/ui_filament_path_plan.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

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
    lv_xml_register_component_from_file("A:ui_xml/components/ams_env_pill.xml");
    lv_xml_register_component_from_file("A:ui_xml/components/ams_sidebar.xml");
    lv_xml_register_component_from_file("A:ui_xml/ams_panel.xml");
    done = true;
}

class AmsTrayPanelFixture : public XMLTestFixture {
  public:
    /// A tool changer has no housing, and flat spools stand in none, so build()
    /// finds no tray for either.
    void build(const char* env_mode, int gates = 4, bool tool_changer = false) {
        helix::Config::get_instance()->set<std::string>("/ams/spool_style", flat_ ? "flat" : "3d");
        auto mock = std::make_unique<AmsBackendMock>(gates);
        if (tool_changer)
            mock->set_tool_changer_mode(true);
        else
            mock->set_afc_mode(true);
        mock->set_environment_mode(env_mode);
        REQUIRE(mock->start().success());
        AmsState::instance().set_backend(std::move(mock));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();
        register_ams_xml_once();

        panel_ = std::make_unique<AmsPanel>(state(), &api());
        panel_->init_subjects();
        lv_obj_t* parent = host_ ? host_ : test_screen();
        panel_obj_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_panel", nullptr));
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
        REQUIRE(helix::ui::ams_detail_tray_geometry(box_, lid_, lid_h_, half_) !=
                (tool_changer || flat_));
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
        helix::Config::get_instance()->set<std::string>("/ams/spool_style", "3d");
    }

    lv_obj_t* slot(int i) const {
        return lv_obj_get_child(slot_grid_, i);
    }

    float top() const {
        return tray::unit_top_y(box_, lid_h_, lid_ != tray::LidMode::None);
    }

    /// The environment pill stands against the box's right end, as tall as the
    /// box, inside the unit's row, and covers no spool, badge or label.
    void check_env_chip_clear(int gates) {
        lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
        REQUIRE(pill != nullptr);
        REQUIRE_FALSE(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
        lv_obj_update_layout(test_screen());
        lv_area_t pa;
        lv_obj_get_coords(pill, &pa);
        CAPTURE(pa.x1, pa.y1, pa.x2, pa.y2);
        CHECK(std::fabs((float)(pa.x1 - origin_.x) - tray::pill_left_x(box_)) <= 1);
        CHECK(std::fabs((float)(pa.y1 - origin_.y) - top()) <= 1);
        CHECK(std::fabs((float)(pa.y2 - origin_.y) - box_.fb) <= 1);
        lv_area_t row;
        lv_obj_get_coords(lv_obj_get_parent(container_), &row);
        CHECK(pa.x2 <= row.x2);
        std::vector<lv_obj_t*> obstacles;
        for (const char* layer_name : {"badge_layer", "labels_layer"}) {
            lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, layer_name);
            for (uint32_t i = 0; layer && i < lv_obj_get_child_count(layer); ++i)
                obstacles.push_back(lv_obj_get_child(layer, (int32_t)i));
        }
        for (int i = 0; i < gates; ++i)
            for (const char* n : {"material_label", "name_label", "tool_badge"})
                if (lv_obj_t* o = lv_obj_find_by_name(slot(i), n))
                    obstacles.push_back(o);
        int checked = 0;
        for (lv_obj_t* o : obstacles) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
                continue;
            lv_area_t oa;
            lv_obj_get_coords(o, &oa);
            CAPTURE(oa.x1, oa.y1, oa.x2, oa.y2);
            CHECK_FALSE((oa.x1 <= pa.x2 && oa.x2 >= pa.x1 && oa.y1 <= pa.y2 && oa.y2 >= pa.y1));
            ++checked;
        }
        REQUIRE(checked >= 2 * gates);
        // The spools stand inside the box, and the pill outside its right end.
        const tray::TrayFaces f = tray::tray_faces(box_);
        float box_right = -1e9f;
        for (const auto* face : {f.back_wall, f.floor, f.front, f.right_side})
            for (int k = 0; k < 4; ++k)
                box_right = std::max(box_right, face[k].x);
        CHECK((float)(pa.x1 - origin_.x) >= box_right - 1);
    }

    /// Every lane's number badge lies on the box front.
    void check_badges_on_front(int gates) {
        lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
        REQUIRE(badges != nullptr);
        REQUIRE(lv_obj_get_child_count(badges) == (uint32_t)gates);
        for (int i = 0; i < gates; ++i) {
            CAPTURE(i);
            lv_area_t ba;
            lv_obj_get_coords(lv_obj_get_child(badges, i), &ba);
            CHECK((float)(ba.y1 - origin_.y) >= box_.ft - 1);
            CHECK((float)(ba.y2 - origin_.y) <= box_.fb + 1);
        }
    }

    bool flat_ = false;
    std::unique_ptr<AmsPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    /// Where build() puts the panel; the screen when null.
    lv_obj_t* host_ = nullptr;
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
    const float skew = tray::kDepthSkew * box_.depth;

    float first_x = 0, last_x = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const float cx = (a.x1 + a.x2 + 1) / 2.0f - origin_.x;
        const float cy = (a.y1 + a.y2 + 1) / 2.0f - origin_.y;
        const float ry = tray::kSpoolFlangeRadius * lv_area_get_width(&a);
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

    // The readout is a pill against the box's right end.
    check_env_chip_clear(4);
    check_badges_on_front(4);
    // In landscape the strip stays at the top of the sidebar.
    lv_obj_t* strip = lv_obj_find_by_name(panel_obj_, "current_loaded_card");
    REQUIRE(strip != nullptr);
    CHECK(lv_obj_get_parent(strip) == lv_obj_find_by_name(panel_obj_, "ams_operation_sidebar"));
    // The pill takes its width from the box's row.
    lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
    lv_obj_t* unit = lv_obj_get_parent(container_);
    CHECK(lv_obj_get_width(container_) <
          lv_obj_get_content_width(unit) - lv_obj_get_width(pill) + 1);
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
                                tray::kSpoolFlangeRadius * lv_area_get_width(&sa);
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
        // Temperature above, then the droplet and humidity.
        lv_obj_t* temp_row = lv_obj_find_by_name(row, "lane_temp_row");
        lv_obj_t* hum_row = lv_obj_find_by_name(row, "lane_humidity_row");
        REQUIRE(temp_row != nullptr);
        REQUIRE(hum_row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(temp_row, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_index(temp_row) < lv_obj_get_index(hum_row));
        CHECK(std::strcmp(lv_obj_get_name(lv_obj_get_child(temp_row, 0)), "lane_temp_icon") == 0);
        const char* temp = lv_label_get_text(lv_obj_get_child(temp_row, 1));
        REQUIRE(temp != nullptr);
        CHECK(std::strstr(temp, "\xC2\xB0"
                                "C") != nullptr);
        REQUIRE(lv_obj_get_child_count(hum_row) == 2);
        // The droplet comes first, then the value.
        CHECK(std::strcmp(lv_obj_get_name(lv_obj_get_child(hum_row, 0)), "lane_humidity_icon") ==
              0);
        const char* value = lv_label_get_text(lv_obj_get_child(hum_row, 1));
        REQUIRE(value != nullptr);
        CHECK(std::strchr(value, '%') != nullptr);
        // The drop fills to the reading the text states.
        const int drop_x10 = helix::ui::humidity_drop_value(lv_obj_get_child(hum_row, 0));
        CHECK(drop_x10 >= 0);
        CHECK(std::abs((drop_x10 + 5) / 10 - std::atoi(value)) <= 1);

        // Above its material label.
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t ra, la;
        lv_obj_get_coords(row, &ra);
        lv_obj_get_coords(label, &la);
        CHECK(ra.y2 <= la.y1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS tray: a dark-mode switch repaints the box in the new colors",
                 "[ams][tray][theme][ui_integration]") {
    build("off");
    REQUIRE(lid_ == tray::LidMode::None);
    REQUIRE_FALSE(helix::ui::reduced_effects());

    // A point on the back wall just under its top edge, midway along it:
    // between the two middle spools.
    const tray::TrayFaces f = tray::tray_faces(box_);
    const int x = (int)std::lround((f.back_wall[0].x + f.back_wall[1].x) / 2);
    const int y = (int)std::lround((f.back_wall[0].y + f.back_wall[1].y) / 2) + 4;

    auto back_pixel = [&]() {
        lv_draw_buf_t* snap = lv_snapshot_take(container_, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(snap != nullptr);
        const uint8_t* px = snap->data + y * snap->header.stride + x * 4;
        const lv_color_t c = lv_color_make(px[2], px[1], px[0]);
        lv_draw_buf_destroy(snap);
        return c;
    };
    auto back_token = [](bool dark) {
        const char* hex = lv_xml_get_const(lv_xml_component_get_scope("ams_unit_detail"),
                                           dark ? "tray_back_dark" : "tray_back_light");
        REQUIRE(hex != nullptr);
        return theme_manager_parse_hex_color(hex);
    };
    auto near = [](lv_color_t a, lv_color_t b) {
        return std::abs(a.red - b.red) <= 2 && std::abs(a.green - b.green) <= 2 &&
               std::abs(a.blue - b.blue) <= 2;
    };

    const bool dark = theme_manager_is_dark_mode();
    REQUIRE_FALSE(near(back_token(dark), back_token(!dark)));
    REQUIRE(near(back_pixel(), back_token(dark)));

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
    CHECK(near(back_pixel(), back_token(!dark)));

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit detail: the unit is centered and its tubes drop from the spools",
                 "[ams][tray][ui_integration]") {
    const char* mode = GENERATE("passive", "off");
    CAPTURE(mode);
    build(mode);

    // The unit: the box's faces, plus the pill standing against its end.
    const tray::TrayFaces f = tray::tray_faces(box_);
    float lo = 1e9f, hi = -1e9f;
    for (const auto* face : {f.back_wall, f.floor, f.left_wall, f.front, f.right_side}) {
        for (int k = 0; k < 4; k++) {
            lo = std::min(lo, face[k].x);
            hi = std::max(hi, face[k].x);
        }
    }
    int32_t left = origin_.x + (int32_t)std::lround(lo);
    int32_t right = origin_.x + (int32_t)std::lround(hi);
    lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(pill != nullptr);
    if (!lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN)) {
        lv_area_t pa;
        lv_obj_get_coords(pill, &pa);
        right = std::max(right, (int32_t)pa.x2);
    }
    lv_obj_t* row = lv_obj_get_parent(container_);
    lv_area_t rc;
    lv_obj_get_content_coords(row, &rc);
    INFO("unit " << left << ".." << right << " in row " << rc.x1 << ".." << rc.x2);
    CHECK(std::abs((left - rc.x1) - (rc.x2 - right)) <= 4);

    // The path canvas reads the laid-out spools, so each lane sits under its spool.
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    const auto* data = helix::ui::fpath::get_data(canvas);
    REQUIRE(data != nullptr);
    lv_area_t cc;
    lv_obj_get_coords(canvas, &cc);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const int32_t lane_x = cc.x1 + helix::ui::fpath::get_slot_x(data, i, cc.x1);
        CHECK(std::abs(lane_x - (a.x1 + a.x2) / 2) <= 2);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS tool changer: each tube hangs from just below its spool",
                 "[ams][tray][ui_integration][filament-path]") {
    const auto size = GENERATE(std::make_pair(480, 320), std::make_pair(800, 480));
    CAPTURE(size.first, size.second);
    helix::test::ScopedGeometry screen(size.first, size.second);
    build("off", 4, /*tool_changer=*/true);

    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    auto* data = helix::ui::fpath::get_data(canvas);
    REQUIRE(data != nullptr);
    REQUIRE(data->topology == static_cast<int>(PathTopology::PARALLEL));
    const helix::ui::fpath::ParallelRows rows = helix::ui::fpath::parallel_rows(
        *data, helix::ui::fpath::compute_base_geometry(canvas, data));
    const int32_t tube_top = rows.entry_y - (data->theme.tube_gauge + 1) / 2;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_graphic");
        REQUIRE(spool != nullptr);
        lv_area_t a;
        lv_obj_get_coords(spool, &a);
        const float flange_bottom =
            (a.y1 + a.y2 + 1) / 2.0f + tray::kSpoolFlangeRadius * lv_area_get_width(&a);
        CAPTURE(a.y1, a.y2, flange_bottom, tube_top);
        // The tube's cap stands on the flange: clear of the spool, with no gap.
        CHECK((float)tube_top >= flange_bottom - 1);
        CHECK((float)tube_top <= flange_bottom + 1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS flat spool style: no box, the tubes hang from the spools",
                 "[ams][tray][ui_integration][filament-path]") {
    const auto size = GENERATE(std::make_pair(480, 272), std::make_pair(800, 480));
    CAPTURE(size.first, size.second);
    helix::test::ScopedGeometry screen(size.first, size.second);
    flat_ = true;
    build("off");

    lv_obj_t* front = lv_obj_find_by_name(panel_obj_, "slot_tray");
    REQUIRE(front != nullptr);
    CHECK(lv_obj_has_flag(front, LV_OBJ_FLAG_HIDDEN));

    // The row stays compact rather than spreading across the width.
    const int32_t pitch = theme_manager_get_spacing("ams_slot_spool_size") +
                          helix::ui::kAmsLaneSpoolBadgeMarginPx +
                          theme_manager_get_spacing("space_xs");
    CHECK(helix::ui::ams_detail_slot_layout(4000, 4).slot_width == pitch);

    // The lanes leave from under the spools' lower flanges.
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    auto* data = helix::ui::fpath::get_data(canvas);
    REQUIRE(data != nullptr);
    lv_area_t g, a;
    lv_obj_get_coords(slot_grid_, &g);
    lv_obj_get_coords(lv_obj_find_by_name(slot(0), "spool_graphic"), &a);
    const float flange_bottom =
        (a.y1 + a.y2 + 1) / 2.0f + tray::kSpoolFlangeRadius * lv_area_get_width(&a);
    CAPTURE(g.y1, a.y1, a.y2, flange_bottom, data->lane_entry_offset);
    REQUIRE(data->lane_entry_offset != INT32_MIN);
    CHECK(std::fabs((float)(g.y1 + data->lane_entry_offset) - flange_bottom) <= 1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS flat spool style: per-lane humidity still shows above each label",
                 "[ams][tray][ui_integration]") {
    flat_ = true;
    build("slot");
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* row = ui_ams_slot_get_lane_humidity(slot(i));
        REQUIRE(row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t ra, la;
        lv_obj_get_coords(row, &ra);
        lv_obj_get_coords(label, &la);
        CHECK(ra.y2 <= la.y1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS flat spool style: the readout stands beside the spools",
                 "[ams][tray][ui_integration]") {
    const auto size = GENERATE(std::make_pair(480, 272), std::make_pair(800, 480));
    const int gates = GENERATE(1, 4);
    CAPTURE(size.first, size.second, gates);
    helix::test::ScopedGeometry screen(size.first, size.second);
    flat_ = true;
    build("passive", gates);

    lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(pill != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(pill, LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(test_screen());
    lv_area_t pa;
    lv_obj_get_coords(pill, &pa);
    CAPTURE(pa.x1, pa.y1, pa.x2, pa.y2);
    lv_area_t row;
    lv_obj_get_coords(lv_obj_get_parent(container_), &row);
    CHECK(pa.x2 <= row.x2);

    // As tall as the spools, level with them, just past the last lane's spool
    // and its badge as the badge layer shows it.
    int32_t spool_top = INT32_MAX, spool_bottom = INT32_MIN, lanes_right = INT32_MIN;
    for (int i = 0; i < gates; ++i) {
        lv_area_t a;
        lv_obj_get_coords(lv_obj_find_by_name(slot(i), "spool_graphic"), &a);
        spool_top = std::min(spool_top, a.y1);
        spool_bottom = std::max(spool_bottom, a.y2);
        lanes_right = std::max(lanes_right, a.x2);
        lv_obj_get_coords(ui_ams_slot_get_status_badge(slot(i)), &a);
        lanes_right = std::max(lanes_right, a.x2);
    }
    lv_area_t c;
    lv_obj_get_coords(container_, &c);
    lanes_right = std::min(lanes_right, c.x2);
    CAPTURE(spool_top, spool_bottom, lanes_right);
    CHECK(std::abs(pa.y1 - spool_top) <= 1);
    CHECK(std::abs(pa.y2 - spool_bottom) <= 1);
    CHECK(pa.x1 > lanes_right);
    CHECK(pa.x1 - lanes_right <= theme_manager_get_spacing("space_md"));
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS tool changer: every lane's number badge stays inside its row",
                 "[ams][tray][ui_integration]") {
    // Without a tray the badges keep the spool container's corner, which is
    // wider than a narrow slot.
    const auto size = GENERATE(std::make_pair(480, 320), std::make_pair(800, 480));
    CAPTURE(size.first, size.second);
    helix::test::ScopedGeometry screen(size.first, size.second);
    build("passive", 4, /*tool_changer=*/true);

    lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
    REQUIRE(badges != nullptr);
    REQUIRE(lv_obj_get_child_count(badges) == 4);
    lv_obj_update_layout(test_screen());
    lv_area_t layer;
    lv_obj_get_coords(badges, &layer);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_area_t b;
        lv_obj_get_coords(lv_obj_get_child(badges, i), &b);
        CAPTURE(b.x1, b.x2, layer.x1, layer.x2);
        // The layer clips its children: a badge past its edge is cut off.
        CHECK(b.x1 >= layer.x1);
        CHECK(b.x2 <= layer.x2);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: a badge moved onto a layer with no width keeps its corner",
                 "[ams][tray][ui_integration]") {
    build("passive", 4, /*tool_changer=*/true);
    lv_obj_t* wide = lv_obj_create(test_screen());
    lv_obj_set_size(wide, 400, 100);
    lv_obj_t* empty = lv_obj_create(test_screen());
    lv_obj_set_size(empty, 0, 0);
    lv_obj_update_layout(test_screen());

    lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(0));
    REQUIRE(badge != nullptr);
    ui_ams_slot_move_badge_to_layer(slot(0), wide, 100);
    const int32_t corner_x = lv_obj_get_style_x(badge, LV_PART_MAIN);
    REQUIRE(corner_x > 0);
    ui_ams_slot_move_badge_to_layer(slot(0), empty, 100);
    CHECK(lv_obj_get_style_x(badge, LV_PART_MAIN) == corner_x);
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a spool name stacks directly over its material",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    int named = 0;
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        lv_obj_t* material = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(name != nullptr);
        REQUIRE(material != nullptr);
        if (lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN))
            continue;
        ++named;
        lv_area_t na, ma;
        lv_obj_get_coords(name, &na);
        lv_obj_get_coords(material, &ma);
        // The tray lifts the material above the unit; the name rides with it.
        const int32_t gap = ma.y1 - na.y2 - 1;
        CHECK(gap >= 0);
        CHECK(gap <= lv_obj_get_style_pad_row(slot(i), LV_PART_MAIN));
    }
    REQUIRE(named > 0);
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a named slot's badge sits on the box front",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    int named = 0;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        REQUIRE(name != nullptr);
        if (!lv_obj_has_flag(name, LV_OBJ_FLAG_HIDDEN))
            ++named;
    }
    REQUIRE(named > 0);
    check_badges_on_front(4);
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS unit at 5, 7 and 8 lanes: the pill stands on the box end and badges stay on the box",
    "[ams][tray][ui_integration][env_chip]") {
    for (int gates : {5, 7, 8}) {
        CAPTURE(gates);
        build("passive", gates);
        REQUIRE(lid_ == tray::LidMode::Unit);
        check_env_chip_clear(gates);
        check_badges_on_front(gates);
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        panel_->clear_panel_reference();
        lv_obj_delete(panel_obj_);
        panel_obj_ = nullptr;
        panel_.reset();
        process_lvgl(10);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit at 480x272 with 8 lanes: two label rows and the whole toolhead",
                 "[ams][tray][ui_integration][stagger]") {
    helix::test::ScopedGeometry micro(480, 272);
    build("passive", 8);
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    lv_obj_update_layout(test_screen());

    // Labels alternate between two rows, each keeping to its two-lane span.
    lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, "labels_layer");
    REQUIRE(layer != nullptr);
    std::vector<lv_obj_t*> labels;
    for (uint32_t k = 0; k < lv_obj_get_child_count(layer); ++k) {
        lv_obj_t* c = lv_obj_get_child(layer, (int32_t)k);
        const char* name = lv_obj_get_name(c);
        if (name && std::strcmp(name, "material_label") == 0)
            labels.push_back(c);
    }
    REQUIRE(labels.size() == 8);
    const int32_t spacing = lv_obj_get_x(slot(1)) - lv_obj_get_x(slot(0));
    int32_t row_y[2] = {INT32_MIN, INT32_MIN};
    for (int i = 0; i < 8; ++i) {
        CAPTURE(i);
        lv_area_t la;
        lv_obj_get_coords(labels[(size_t)i], &la);
        CHECK(lv_area_get_width(&la) < 2 * spacing);
        int32_t& y = row_y[i % 2];
        if (y == INT32_MIN)
            y = la.y1;
        CHECK(la.y1 == y);
    }
    CHECK(row_y[0] != row_y[1]);

    // The nozzle is drawn inside the canvas, not cut off by its bottom.
    lv_area_t toolhead, cc;
    REQUIRE(ui_filament_path_canvas_get_toolhead_box(canvas, &toolhead));
    lv_obj_get_coords(canvas, &cc);
    CAPTURE(toolhead.y1, toolhead.y2, cc.y1, cc.y2);
    CHECK(toolhead.y2 <= cc.y2);
    CHECK(toolhead.y1 >= cc.y1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel in portrait at 272x480: diagram on top, the sidebar below it",
                 "[ams][tray][ui_integration][portrait]") {
    helix::test::ScopedGeometry portrait(272, 480);
    lv_subject_t* is_portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(is_portrait != nullptr);
    REQUIRE(lv_subject_get_int(is_portrait) == 1);
    // The app's panel area: the screen less the bottom navigation bar.
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, 272, 410);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    build("passive", 8);
    lv_obj_update_layout(test_screen());

    lv_obj_t* content = lv_obj_find_by_name(panel_obj_, "overlay_content");
    lv_obj_t* right = lv_obj_find_by_name(panel_obj_, "right_column");
    lv_obj_t* sidebar = lv_obj_find_by_name(panel_obj_, "ams_operation_sidebar");
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(content != nullptr);
    REQUIRE(right != nullptr);
    REQUIRE(sidebar != nullptr);
    REQUIRE(canvas != nullptr);

    // The sidebar is a full-width bottom section under the diagram.
    lv_area_t ca, ra;
    lv_obj_get_coords(content, &ca);
    lv_obj_get_coords(right, &ra);
    CHECK(ra.y1 >= ca.y2);
    CHECK(lv_area_get_width(&ra) == lv_obj_get_width(panel_obj_));
    // The tube run is capped rather than grown to fill, and the toolhead is drawn
    // whole inside it. The cap's value is resolved when the panel's XML registers,
    // which in this binary may be at another tier, so only the shape is pinned;
    // the no-scroll budget is measured on the running app (ctl geom).
    lv_obj_t* path = lv_obj_find_by_name(panel_obj_, "path_container");
    REQUIRE(path != nullptr);
    CHECK(lv_obj_get_style_flex_grow(path, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_height(path, LV_PART_MAIN) > 0);
    CHECK(lv_obj_get_style_flex_grow(right, LV_PART_MAIN) == 1);
    lv_area_t toolhead, cc;
    REQUIRE(ui_filament_path_canvas_get_toolhead_box(canvas, &toolhead));
    lv_obj_get_coords(canvas, &cc);
    CHECK(toolhead.y2 <= cc.y2);
    // The loaded strip and the status line stand above the diagram, in that order.
    lv_obj_t* strip = lv_obj_find_by_name(panel_obj_, "current_loaded_card");
    lv_obj_t* status = lv_obj_find_by_name(panel_obj_, "status_section");
    lv_obj_t* slot_parent = lv_obj_find_by_name(panel_obj_, "portrait_summary_slot");
    REQUIRE(strip != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(slot_parent != nullptr);
    CHECK(lv_obj_get_parent(strip) == slot_parent);
    CHECK(lv_obj_get_parent(status) == slot_parent);
    lv_area_t sa, sta, ua;
    lv_obj_get_coords(strip, &sa);
    lv_obj_get_coords(status, &sta);
    lv_obj_get_coords(lv_obj_find_by_name(panel_obj_, "ams_unit_card"), &ua);
    CHECK(sa.y2 < sta.y1);
    CHECK(sta.y2 < ua.y1);
    // The strip is one row: its meters stand right of the lines, not under them.
    lv_obj_t* details = lv_obj_find_by_name(panel_obj_, "loaded_details");
    REQUIRE(details != nullptr);
    lv_area_t da;
    lv_obj_get_coords(details, &da);
    int meters_seen = 0;
    for (const char* meter : {"clog_meter", "buffer_mini"}) {
        lv_obj_t* m = lv_obj_find_by_name(panel_obj_, meter);
        REQUIRE(m != nullptr);
        if (lv_obj_has_flag(m, LV_OBJ_FLAG_HIDDEN))
            continue;
        ++meters_seen;
        lv_area_t ma;
        lv_obj_get_coords(m, &ma);
        CAPTURE(meter, ma.x1, ma.y1, ma.y2, da.x2, da.y1, da.y2);
        CHECK(ma.x1 > da.x2);
        CHECK(ma.y1 <= da.y2);
        CHECK(ma.y2 >= da.y1);
    }
    REQUIRE(meters_seen > 0);
    // In a narrow text column the weight ends the position line.
    lv_area_t wa;
    lv_obj_get_coords(lv_obj_find_by_name(panel_obj_, "loaded_weight"), &wa);
    CHECK(wa.x2 == da.x2);
    // Lane badges stay apart at eight lanes.
    lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
    REQUIRE(badges != nullptr);
    REQUIRE(lv_obj_get_child_count(badges) == 8);
    for (int i = 0; i + 1 < 8; ++i) {
        CAPTURE(i);
        lv_area_t a, b;
        lv_obj_get_coords(lv_obj_get_child(badges, i), &a);
        lv_obj_get_coords(lv_obj_get_child(badges, i + 1), &b);
        CHECK(a.x2 < b.x1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel in portrait at 480x800: the weight follows the position",
                 "[ams][tray][ui_integration][portrait]") {
    helix::test::ScopedGeometry portrait(480, 800);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "ui_breakpoint")) >= 3);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, 480, 730);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    build("passive", 8);
    lv_obj_update_layout(test_screen());

    lv_obj_t* details = lv_obj_find_by_name(panel_obj_, "loaded_details");
    lv_obj_t* position = lv_obj_find_by_name(panel_obj_, "loaded_position");
    lv_obj_t* weight = lv_obj_find_by_name(panel_obj_, "loaded_weight");
    REQUIRE(details != nullptr);
    REQUIRE(position != nullptr);
    REQUIRE(weight != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(weight, LV_OBJ_FLAG_HIDDEN));
    lv_area_t da, pa, wa;
    lv_obj_get_coords(details, &da);
    lv_obj_get_coords(position, &pa);
    lv_obj_get_coords(weight, &wa);
    CAPTURE(da.x1, da.x2, pa.x1, pa.x2, wa.x1, wa.x2);
    // The text column is wide, so the weight sits right after the position
    // rather than at the column's far end beside the meters.
    REQUIRE(lv_area_get_width(&da) > 3 * (lv_area_get_width(&pa) + lv_area_get_width(&wa)) / 2);
    CHECK(wa.x1 > pa.x2);
    CHECK(wa.x1 - pa.x2 <= 16);
    CHECK(wa.x2 < da.x2 - lv_area_get_width(&wa));
    CHECK(wa.y1 < pa.y2);
    CHECK(wa.y2 > pa.y1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: one named lane among unnamed ones keeps every spool level",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    auto clear_name = [](int i) {
        lv_subject_t* s = AmsState::instance().get_slot_name_subject(0, i);
        REQUIRE(s != nullptr);
        lv_subject_copy_string(s, "");
    };
    auto spool_tops = [&]() {
        lv_obj_update_layout(test_screen());
        std::vector<int32_t> tops;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t* spool = lv_obj_find_by_name(slot(i), "spool_container");
            REQUIRE(spool != nullptr);
            lv_area_t a;
            lv_obj_get_coords(spool, &a);
            tops.push_back(a.y1);
        }
        return tops;
    };
    auto name_lines_shown = [&]() {
        int shown = 0;
        for (int i = 0; i < 4; ++i)
            if (!lv_obj_has_flag(lv_obj_find_by_name(slot(i), "name_label"), LV_OBJ_FLAG_HIDDEN))
                ++shown;
        return shown;
    };
    const std::vector<int32_t> all_named = spool_tops();
    REQUIRE(name_lines_shown() == 4);

    for (int i : {0, 2, 3})
        clear_name(i);
    process_lvgl(20);
    const std::vector<int32_t> one_named = spool_tops();
    CAPTURE(one_named);
    CHECK(name_lines_shown() == 4);
    for (int i = 1; i < 4; ++i)
        CHECK(one_named[i] == one_named[0]);
    CHECK(one_named[0] == all_named[0]);

    // With no name in the row, no slot keeps the line.
    clear_name(1);
    process_lvgl(20);
    const std::vector<int32_t> none_named = spool_tops();
    CAPTURE(none_named);
    CHECK(name_lines_shown() == 0);
    for (int i = 1; i < 4; ++i)
        CHECK(none_named[i] == none_named[0]);
    CHECK(none_named[0] < all_named[0]);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel: an endless-spool backup between lanes past sixteen is shown",
                 "[ams][slot_ceiling][endless_spool]") {
    constexpr int kLanes = 20;
    build("off", kLanes);
    AmsBackend* backend = AmsState::instance().get_backend();
    REQUIRE(backend != nullptr);
    REQUIRE(backend->get_endless_spool_capabilities().available());
    REQUIRE(backend->set_endless_spool_backup(kLanes - 2, kLanes - 1).success());

    AmsPanelTestAccess::update_endless_arrows(*panel_);
    lv_obj_t* arrows = lv_obj_find_by_name(panel_obj_, "endless_arrows");
    REQUIRE(arrows != nullptr);
    CHECK_FALSE(lv_obj_has_flag(arrows, LV_OBJ_FLAG_HIDDEN));
}

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
#include "ui_unit_art.h"

#include "../test_fixtures.h"
#include "../test_helpers/ams_panel_test_access.h"
#include "../test_helpers/backend_user_edit.h"
#include "../test_helpers/scoped_geometry.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "config.h"
#include "filament_tube_stroker.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "src/ui/filament/ui_ams_detail_tray.h"
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
    void build(const char* env_mode, int gates = 4, bool tool_changer = false, bool ace = false,
               bool openams = false, bool buffer = false, int empty_lane = -1) {
        helix::Config::get_instance()->set<std::string>("/ams/spool_style", flat_ ? "flat" : "3d");
        auto mock = std::make_unique<AmsBackendMock>(gates);
        if (tool_changer)
            mock->set_tool_changer_mode(true);
        else
            mock->set_afc_mode(true);
        mock->set_environment_mode(env_mode);
        // These cases cover the generic box, which a unit with its own drawing does not use.
        mock->rename_unit(0, unit_name_);
        if (ace)
            mock->set_ace_identity();
        if (openams)
            mock->set_openams_identity();
        REQUIRE(mock->start().success());
        if (buffer)
            mock->set_unit_buffer_health(0, BufferHealth{});
        if (empty_lane >= 0) {
            SlotInfo info = mock->get_slot_info(empty_lane);
            info.status = SlotStatus::EMPTY;
            info.material.clear();
            info.color_name.clear();
            REQUIRE(helix::test::apply_edit(*mock, empty_lane, info).success());
            mock->force_slot_status(empty_lane, SlotStatus::EMPTY);
        }
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

    /// Give every lane a brand, as a Spoolman vendor would (the mock's lanes
    /// carry none), and lay the tray out again around the name lines.
    void name_every_lane(int lanes, const char* brand) {
        for (int i = 0; i < lanes; ++i)
            lv_subject_set_string(AmsState::instance().get_slot_name_subject(0, i), brand);
        panel_->refresh_slots();
        process_lvgl(20);
        lv_obj_update_layout(test_screen());
    }

    /// Fill (@p material) or empty (nullptr) lane @p lane as the backend reports it, with no
    /// unit refresh: only the backend's update reaches the panel.
    void set_lane_live(int lane, const char* material) {
        auto* backend = static_cast<AmsBackendMock*>(AmsState::instance().get_backend());
        REQUIRE(backend != nullptr);
        SlotInfo info = backend->get_slot_info(lane);
        const SlotStatus status = material ? SlotStatus::AVAILABLE : SlotStatus::EMPTY;
        info.status = status;
        info.material = material ? material : "";
        if (material)
            info.color_rgb = 0x2E7D32;
        REQUIRE(helix::test::apply_edit(*backend, lane, info).success());
        backend->force_slot_status(lane, status);
        AmsState::instance().sync_from_backend();
        process_lvgl(50);
        lv_obj_update_layout(test_screen());
    }

    /// The material labels standing over the drawn unit, in lane order, shown or not.
    std::vector<lv_obj_t*> over_lane_labels() {
        lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, "labels_layer");
        std::vector<lv_obj_t*> labels;
        for (uint32_t k = 0; layer && k < lv_obj_get_child_count(layer); ++k) {
            lv_obj_t* c = lv_obj_get_child(layer, (int32_t)k);
            const char* name = lv_obj_get_name(c);
            if (name && std::strcmp(name, "material_label") == 0)
                labels.push_back(c);
        }
        return labels;
    }

    /// No two shown lane numbers of the first @p lanes overlap.
    void check_badges_apart(int lanes) {
        lv_obj_update_layout(test_screen());
        std::vector<lv_area_t> badges;
        for (int i = 0; i < lanes; ++i) {
            lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
            if (!badge || lv_obj_is_hidden(badge))
                continue;
            lv_area_t a;
            lv_obj_get_coords(badge, &a);
            badges.push_back(a);
        }
        REQUIRE(badges.size() >= 2);
        for (size_t i = 0; i < badges.size(); ++i)
            for (size_t j = i + 1; j < badges.size(); ++j) {
                const lv_area_t& a = badges[i];
                const lv_area_t& b = badges[j];
                CAPTURE(i, j, a.x1, a.y1, a.x2, a.y2, b.x1, b.y1, b.x2, b.y2);
                CHECK_FALSE((a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1));
            }
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

    /// Each shown label over the drawn unit stands over its own spool, its dotted tick a
    /// straight vertical line from just under it down to the spool top; labels cycle through
    /// at most three rows, no two in one row overlap, each keeps at least three characters
    /// before an ellipsis, and an empty lane shows none.
    void check_labels_over_lanes(const std::vector<lv_obj_t*>& all) {
        namespace at = helix::ui::ams_tray;
        std::vector<lv_obj_t*> labels;
        for (lv_obj_t* l : all)
            if (!lv_obj_is_hidden(l))
                labels.push_back(l);
        const int n = (int)labels.size();
        REQUIRE(at::s_tray.leader_count == n);
        std::vector<lv_area_t> areas((size_t)n);
        std::vector<int32_t> tops;
        std::vector<bool> readable;
        for (int i = 0; i < n; ++i) {
            CAPTURE(i);
            lv_area_t& la = areas[(size_t)i];
            lv_obj_get_coords(labels[(size_t)i], &la);
            const tray::PointF* tick = at::s_tray.leader[i];
            CAPTURE(la.x1, la.x2, la.y2, tick[0].x, tick[0].y, tick[2].x, tick[2].y);
            CHECK(tick[0].x == Catch::Approx(tick[2].x).margin(0.5));
            CHECK(tick[1].x == Catch::Approx(tick[2].x).margin(0.5));
            CHECK(tick[2].y > tick[0].y);
            // Over its own spool: its tick rises into it.
            CHECK((float)(la.x1 - origin_.x) <= tick[2].x);
            CHECK((float)(la.x2 - origin_.x) >= tick[2].x);
            CHECK(std::fabs((float)(la.y2 - origin_.y) - tick[0].y) <= 3.0f);
            if (std::find(tops.begin(), tops.end(), la.y1) == tops.end())
                tops.push_back(la.y1);
            // Three characters and an ellipsis, at the least.
            const char* text = lv_label_get_text(labels[(size_t)i]);
            const lv_font_t* font = lv_obj_get_style_text_font(labels[(size_t)i], LV_PART_MAIN);
            const int32_t whole = helix::ui::text_width(text, font, 0);
            const std::string head = std::string(text).substr(0, 3) + "...";
            readable.push_back(lv_area_get_width(&la) >=
                               std::min(whole, helix::ui::text_width(head.c_str(), font, 0)) - 1);
        }
        const int rows = (int)tops.size();
        CHECK(rows <= 3);
        // Three rows are the most; short of them every label keeps a few characters.
        if (rows < 3)
            for (int i = 0; i < n; ++i) {
                CAPTURE(i);
                CHECK(readable[(size_t)i]);
            }
        for (int i = 0; i + rows < n; ++i) {
            CAPTURE(i, rows);
            CHECK(areas[(size_t)i].y1 == areas[(size_t)(i + rows)].y1);
            CHECK(areas[(size_t)i].x2 < areas[(size_t)(i + rows)].x1);
        }
        // An empty lane shows no label.
        for (int i = 0; i < (int)all.size() && i < 16; ++i) {
            lv_obj_t* spool = lv_obj_find_by_name(slot(i), "lane_spool");
            if (spool && helix::ui::ams_lane_spool_empty(spool))
                CHECK(lv_obj_is_hidden(all[(size_t)i]));
        }
    }

    float top() const {
        return tray::unit_top_y(box_, lid_h_, lid_ != tray::LidMode::None);
    }

    /// The environment pill stands against the box's right end, as tall as the
    /// box, inside the unit's row, and covers no spool, badge or label.
    void check_env_chip_clear(int gates) {
        lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
        REQUIRE(pill != nullptr);
        REQUIRE_FALSE(lv_obj_is_hidden(pill));
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
            if (lv_obj_is_hidden(o))
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
        // The path canvas finds these same badges, to draw them over the lanes leaving below.
        auto* path = helix::ui::fpath::get_data(lv_obj_find_by_name(panel_obj_, "path_canvas"));
        REQUIRE(path != nullptr);
        lv_obj_t* found[helix::ui::fpath::FilamentPathData::kMaxSlots] = {};
        REQUIRE(helix::ui::fpath::lane_badges(*path, found) == gates);
        for (int i = 0; i < gates; ++i)
            CHECK(lv_obj_get_parent(found[i]) == badges);
    }

    bool flat_ = false;
    const char* unit_name_ = "Generic Unit";
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

    float first_x = 0, last_x = 0, size = 0;
    int32_t lowest_label = INT32_MIN;
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
        size = (float)lv_area_get_width(&a);

        // The label stands above the lid, the lowest row space_md over it.
        lv_obj_t* label = ui_ams_slot_get_material_label(slot(i));
        REQUIRE(label != nullptr);
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        CHECK((float)(la.y2 - origin_.y) <= top() - space_md + 1);
        lowest_label = std::max<int32_t>(lowest_label, la.y2);
    }
    CHECK(std::fabs((float)(lowest_label - origin_.y) - (top() - space_md)) <= 1);
    // The spools stand at the real units' pitch, and the box ends clear the outer flanges.
    CHECK(std::fabs((last_x - first_x) / 3 - tray::spool_pitch(size)) <= 1);
    CHECK(box_.fl == Catch::Approx(first_x - tray::end_cap_width(nullptr, size)).margin(0.01));
    CHECK(box_.fr == Catch::Approx(last_x + tray::end_cap_width(nullptr, size)).margin(0.01));

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
        lv_obj_t* label = ui_ams_slot_get_material_label(slot(i));
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
        CHECK_FALSE(lv_obj_is_hidden(row));
        // Temperature above, then the droplet and humidity.
        lv_obj_t* temp_row = lv_obj_find_by_name(row, "lane_temp_row");
        lv_obj_t* hum_row = lv_obj_find_by_name(row, "lane_humidity_row");
        REQUIRE(temp_row != nullptr);
        REQUIRE(hum_row != nullptr);
        CHECK_FALSE(lv_obj_is_hidden(temp_row));
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
        lv_obj_t* label = ui_ams_slot_get_material_label(slot(i));
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

    // A point on the back wall just under its top edge, between the last spool's front
    // flange and the wall's right end.
    const tray::TrayFaces f = tray::tray_faces(box_);
    lv_obj_t* last = lv_obj_find_by_name(slot(3), "spool_graphic");
    REQUIRE(last != nullptr);
    lv_area_t la;
    lv_obj_get_coords(last, &la);
    const float size = (float)lv_area_get_width(&la);
    const float spool_right = (la.x1 + la.x2 + 1) / 2.0f - origin_.x +
                              size * tray::kSpoolWidth / 2 +
                              tray::kDepthSkew * tray::kSpoolFlangeRadius * size;
    const int x = (int)std::lround((spool_right + f.back_wall[1].x) / 2);
    const int y = (int)std::lround(f.back_wall[1].y) + 2;

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
    const lv_color_t seen = back_pixel(), want = back_token(dark);
    CAPTURE(x, y, (int)seen.red, (int)seen.green, (int)seen.blue, (int)want.red, (int)want.green,
            (int)want.blue);
    REQUIRE(near(seen, want));

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
    if (!lv_obj_is_hidden(pill)) {
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
    CHECK(lv_obj_is_hidden(front));

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
        CHECK_FALSE(lv_obj_is_hidden(row));
        lv_obj_t* label = lv_obj_find_by_name(slot(i), "material_label");
        REQUIRE(label != nullptr);
        lv_area_t ra, la;
        lv_obj_get_coords(row, &ra);
        lv_obj_get_coords(label, &la);
        CHECK(ra.y2 <= la.y1);
    }
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS tray: a drawn unit's readout tucks low over its end, clear of the last number",
    "[ams][tray][ui_integration]") {
    // Five lanes: the ACE's own drawing, which has no five-lane art.
    build("dryer", 5, /*tool_changer=*/false, /*ace=*/true);
    lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(pill != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(pill));
    lv_obj_update_layout(test_screen());
    lv_area_t pill_area, box_area;
    lv_obj_get_coords(pill, &pill_area);
    lv_obj_get_coords(container_, &box_area);
    const int32_t tuck = theme_manager_get_spacing("space_sm");
    lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
    REQUIRE((badges != nullptr && lv_obj_get_child_count(badges) > 0));
    lv_area_t last;
    lv_obj_get_coords(lv_obj_get_child(badges, -1), &last);
    CHECK(pill_area.x1 > last.x2);
    CHECK(pill_area.x1 >= box_area.x1 + (int32_t)std::lround(tray::pill_left_x(box_)) - tuck);
    CHECK(pill_area.y2 == box_area.y1 + (int32_t)std::lround(box_.fb) + tuck);
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
    REQUIRE_FALSE(lv_obj_is_hidden(pill));
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

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: a label over a drawn unit reads its material alone",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    name_every_lane(4, "eSUN");
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* name = lv_obj_find_by_name(slot(i), "name_label");
        lv_obj_t* material = ui_ams_slot_get_material_label(slot(i));
        REQUIRE(name != nullptr);
        REQUIRE(material != nullptr);
        CHECK(lv_obj_is_hidden(name));
        const char* text = lv_label_get_text(material);
        REQUIRE(text != nullptr);
        CHECK(std::strstr(text, "eSUN") == nullptr);
        CHECK(std::strlen(text) > 0);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "EMU: a lid per box, spools lifted off its base, numbers right of each lane",
                 "[ams][tray][ui_integration][unit-art]") {
    unit_name_ = "EMU EMU_1";
    build("passive", 2); // a reading for the whole unit, which would put one lid over the row
    CHECK(lid_ == tray::LidMode::PerLane);
    lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
    REQUIRE(badges != nullptr);
    for (int i = 0; i < 2; ++i) {
        CAPTURE(i);
        lv_area_t sa, ba;
        lv_obj_get_coords(lv_obj_find_by_name(slot(i), "lane_spool"), &sa);
        lv_obj_get_coords(lv_obj_get_child(badges, i), &ba);
        const float flange_bottom =
            (sa.y1 + sa.y2) / 2.0f + tray::kSpoolFlangeRadius * lv_area_get_width(&sa) - origin_.y;
        CHECK(box_.fb - flange_bottom > tray::kSpoolFloorGap + 3);
        CHECK(ba.x1 + ba.x2 > sa.x1 + sa.x2);
    }
    lv_area_t g;
    lv_obj_get_coords(slot_grid_, &g);
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    // The drawn box's lanes leave its foot; over the EMU's baked art they leave its edge.
    lv_obj_t* art = lv_obj_find_by_name(container_, "unit_view_art");
    if (!art || lv_obj_is_hidden(art))
        CHECK(std::abs(helix::ui::fpath::get_data(canvas)->lane_entry_offset -
                       (origin_.y + (int32_t)std::lround(box_.fb) - g.y1)) <= 1);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: the path follows the slots rebuilt in its grid before it is told",
                 "[ams][tray][ui_integration]") {
    build("passive");
    auto* path = helix::ui::fpath::get_data(lv_obj_find_by_name(panel_obj_, "path_canvas"));
    REQUIRE(path != nullptr);
    AmsDetailWidgets w = ams_detail_find_widgets(lv_obj_find_by_name(panel_obj_, "unit_detail"));
    helix::ui::WidgetRef slots[kAmsDetailMaxSlots];
    int count = (int)lv_obj_get_child_count(slot_grid_);
    for (int i = 0; i < count; ++i)
        slots[i].reset(slot(i));
    ams_detail_destroy_slots(w, slots, count);
    REQUIRE(
        ams_detail_create_slots(w, slots, kAmsDetailMaxSlots, -1, nullptr, nullptr).slot_count ==
        4);
    // The new row stands where the old one did; shift it so a stale read shows.
    lv_obj_set_style_translate_x(slot_grid_, 7, LV_PART_MAIN);
    lv_obj_update_layout(test_screen());
    for (int i = 0; i < 4; ++i) {
        lv_area_t sa;
        lv_obj_get_coords(ui_ams_slot_get_spool_container(slot(i)), &sa);
        CHECK(helix::ui::fpath::get_slot_x(path, i, 0) == (sa.x1 + sa.x2) / 2);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: a named slot's badge sits on the box front",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    name_every_lane(4, "eSUN");
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
                 "AMS unit at 480x272 with 8 lanes: labels in at most two rows, the whole toolhead",
                 "[ams][tray][ui_integration][stagger]") {
    helix::test::ScopedGeometry micro(480, 272);
    build("passive", 8);
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    lv_obj_update_layout(test_screen());

    // Labels stand in one row or alternate between two, each centered over its own spool
    // with a short straight tick down to it, and no two in a row overlap.
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
    check_labels_over_lanes(labels);

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
    // The sidebar stands at its own height below the diagram, in a panel that scrolls to it.
    CHECK(lv_obj_get_style_flex_grow(right, LV_PART_MAIN) == 0);
    CHECK(lv_obj_is_scrollable(panel_obj_));
    CHECK(lv_obj_get_height(right) > 0);
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
        if (lv_obj_is_hidden(m))
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
    // The hub stands clear of every lane's number.
    {
        auto* cd = helix::ui::fpath::get_data(canvas);
        REQUIRE((cd != nullptr && cd->hits.hub_valid));
        for (int i = 0; i < 8; ++i) {
            lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
            if (!badge || lv_obj_is_hidden(badge))
                continue;
            lv_area_t ba;
            lv_obj_get_coords(badge, &ba);
            const lv_area_t& hub = cd->hits.hub;
            CAPTURE(i, ba.x1, ba.y1, ba.x2, ba.y2, hub.x1, hub.y1, hub.x2, hub.y2);
            CHECK_FALSE((ba.x1 <= hub.x2 && ba.x2 >= hub.x1 && ba.y1 <= hub.y2 && ba.y2 >= hub.y1));
        }
    }
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
    REQUIRE_FALSE(lv_obj_is_hidden(weight));
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

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit: names never move a drawn unit's spools",
                 "[ams][tray][ui_integration][slot_name]") {
    build("passive");
    auto clear_name = [](int i) {
        lv_subject_t* s = AmsState::instance().get_slot_name_subject(0, i);
        REQUIRE(s != nullptr);
        lv_subject_set_string(s, "");
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
            if (!lv_obj_is_hidden(lv_obj_find_by_name(slot(i), "name_label")))
                ++shown;
        return shown;
    };
    // Over a drawn unit a label reads its material alone, so names take no line and
    // move no spool, however many lanes have one.
    name_every_lane(4, "eSUN");
    const std::vector<int32_t> all_named = spool_tops();
    CHECK(name_lines_shown() == 0);
    for (int i = 1; i < 4; ++i)
        CHECK(all_named[i] == all_named[0]);

    for (int i : {0, 2, 3})
        clear_name(i);
    process_lvgl(20);
    CHECK(spool_tops() == all_named);

    clear_name(1);
    process_lvgl(20);
    CHECK(spool_tops() == all_named);
    CHECK(name_lines_shown() == 0);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS panel: an endless-spool backup between lanes past sixteen is shown",
                 "[ams][slot_ceiling][endless_spool]") {
    constexpr int kLanes = 20;
    build("off", kLanes);
    AmsBackend* backend = AmsState::instance().get_backend();
    REQUIRE(backend != nullptr);
    REQUIRE(backend->get_endless_spool_capabilities().available());
    lv_obj_t* arrows = lv_obj_find_by_name(panel_obj_, "endless_arrows");
    REQUIRE(arrows != nullptr);
    // With no backup the row has nothing to draw and gives its space back.
    CHECK(lv_obj_is_hidden(arrows));
    // One backend leaves nothing to select between.
    lv_obj_t* selector = lv_obj_find_by_name(panel_obj_, "backend_selector_row");
    REQUIRE(selector != nullptr);
    CHECK(lv_obj_is_hidden(selector));

    REQUIRE(backend->set_endless_spool_backup(kLanes - 2, kLanes - 1).success());
    AmsPanelTestAccess::update_endless_arrows(*panel_);
    CHECK_FALSE(lv_obj_is_hidden(arrows));
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: a lane that fills or empties shows or drops its label at once",
                 "[ams][tray][ui_integration][over-lane][lane-label-live]") {
    const bool art = GENERATE(false, true);
    CAPTURE(art);
    if (art)
        unit_name_ = "Box Turtle 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/false, /*openams=*/false,
          /*buffer=*/false, /*empty_lane=*/1);
    namespace at = helix::ui::ams_tray;
    lv_obj_t* label = ui_ams_slot_get_material_label(slot(1));
    REQUIRE(label != nullptr);
    REQUIRE(lv_obj_is_hidden(label));
    const int ticks = at::s_tray.leader_count;

    set_lane_live(1, "PETG");
    CHECK_FALSE(lv_obj_is_hidden(label));
    CHECK(at::s_tray.leader_count == ticks + 1);

    // And empty it again.
    set_lane_live(1, nullptr);
    CHECK(lv_obj_is_hidden(label));
    CHECK(at::s_tray.leader_count == ticks);
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS unit: a lane that fills and needs a second label row keeps it in the unit row",
    "[ams][tray][ui_integration][over-lane][lane-label-live]") {
    const bool art = GENERATE(false, true);
    CAPTURE(art);
    if (art)
        unit_name_ = "Box Turtle 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/false, /*openams=*/false,
          /*buffer=*/false, /*empty_lane=*/1);
    for (int lane : {0, 2, 3})
        set_lane_live(lane, "PLA");
    // A full layout around the short names: their labels take one row.
    panel_->refresh_slots();
    process_lvgl(20);
    lv_obj_update_layout(test_screen());
    std::vector<lv_obj_t*> labels = over_lane_labels();
    REQUIRE(labels.size() == 4);
    auto rows = [&] {
        std::vector<int32_t> tops;
        for (lv_obj_t* l : labels) {
            lv_area_t a;
            lv_obj_get_coords(l, &a);
            if (!lv_obj_is_hidden(l) && std::find(tops.begin(), tops.end(), a.y1) == tops.end())
                tops.push_back(a.y1);
        }
        return tops.size();
    };
    REQUIRE(rows() == 1);

    // Lane 1 fills with a name wider than the room between its neighbours: two rows.
    set_lane_live(1, "ABS-GF");
    REQUIRE_FALSE(lv_obj_is_hidden(labels[1]));
    REQUIRE(rows() == 2);
    lv_area_t c;
    lv_obj_get_coords(container_, &c);
    for (int i = 0; i < 4; ++i) {
        lv_area_t a;
        lv_obj_get_coords(labels[(size_t)i], &a);
        CAPTURE(i, a.y1, c.y1);
        CHECK(a.y1 >= c.y1);
    }
    check_labels_over_lanes(labels);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit art: a lane that empties keeps its number clear of its neighbours",
                 "[ams][tray][ui_integration][unit-art][lane-badges][lane-label-live]") {
    const int unit = GENERATE(0, 1, 2); // Box Turtle, ACE, OpenAMS's AMS 2 Pro
    const int size = GENERATE(0, 1, 2, 3);
    const int32_t sizes[4][4] = {
        {320, 480, 320, 410}, {800, 480, 800, 400}, {1024, 600, 1024, 500}, {480, 272, 480, 220}};
    CAPTURE(unit, sizes[size][0], sizes[size][1]);
    helix::test::ScopedGeometry scoped(sizes[size][0], sizes[size][1]);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, sizes[size][2], sizes[size][3]);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    unit_name_ = unit == 0 ? "Box Turtle 1" : unit == 1 ? "ACE 1" : "AMS2";
    // Every lane loaded: the numbers are laid out with no "+" to make room for.
    build("passive", 4, /*tool_changer=*/false, /*ace=*/unit == 1, /*openams=*/unit == 2,
          /*buffer=*/unit == 2);
    lv_obj_t* drawing = lv_obj_find_by_name(container_, "unit_view_art");
    if (!drawing || lv_obj_is_hidden(drawing))
        SKIP("no art for this unit here");
    check_badges_apart(4);
    set_lane_live(0, nullptr);
    set_lane_live(1, nullptr);
    check_badges_apart(4);
    // The numbers stand as a full layout of the unit as it is now places them.
    struct Badge {
        lv_area_t area;
        bool plus;
    };
    auto badges = [&] {
        lv_obj_update_layout(test_screen());
        std::vector<Badge> out;
        for (int i = 0; i < 4; ++i) {
            lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
            REQUIRE(badge != nullptr);
            lv_obj_t* plus = lv_obj_find_by_name(badge, "badge_plus");
            Badge b{};
            lv_obj_get_coords(badge, &b.area);
            b.plus = plus && !lv_obj_is_hidden(plus);
            out.push_back(b);
        }
        return out;
    };
    const std::vector<Badge> live = badges();
    panel_->refresh_slots();
    process_lvgl(20);
    const std::vector<Badge> rebuilt = badges();
    for (int i = 0; i < 4; ++i) {
        const Badge& l = live[(size_t)i];
        const Badge& r = rebuilt[(size_t)i];
        CAPTURE(i, l.area.x1, l.area.y1, l.area.x2, l.plus, r.area.x1, r.area.y1, r.area.x2,
                r.plus);
        CHECK(l.plus == r.plus);
        CHECK(std::abs(l.area.x1 - r.area.x1) <= 1);
        CHECK(std::abs(l.area.y1 - r.area.y1) <= 1);
        CHECK(std::abs(l.area.x2 - r.area.x2) <= 1);
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture, "AMS unit art: no two lane numbers overlap at any size",
                 "[ams][tray][ui_integration][unit-art][lane-badges]") {
    const int unit = GENERATE(0, 1, 2); // Box Turtle, ACE, OpenAMS's AMS 2 Pro
    const int size = GENERATE(0, 1, 2, 3);
    const int32_t sizes[4][4] = {
        {320, 480, 320, 410}, {800, 480, 800, 400}, {1024, 600, 1024, 500}, {480, 272, 480, 220}};
    CAPTURE(unit, sizes[size][0], sizes[size][1]);
    helix::test::ScopedGeometry scoped(sizes[size][0], sizes[size][1]);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, sizes[size][2], sizes[size][3]);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    unit_name_ = unit == 0 ? "Box Turtle 1" : unit == 1 ? "ACE 1" : "AMS2";
    // One lane empty: its number carries the "+", and it shows no label.
    build("passive", 4, /*tool_changer=*/false, /*ace=*/unit == 1, /*openams=*/unit == 2,
          /*buffer=*/unit == 2, /*empty_lane=*/1);
    lv_obj_update_layout(test_screen());
    lv_obj_t* drawing = lv_obj_find_by_name(container_, "unit_view_art");
    if (!drawing || lv_obj_is_hidden(drawing))
        SKIP("no art for this unit here");
    std::vector<lv_area_t> badges;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
        if (!badge || lv_obj_is_hidden(badge))
            continue;
        lv_area_t a;
        lv_obj_get_coords(badge, &a);
        badges.push_back(a);
    }
    REQUIRE(badges.size() >= 2);
    for (size_t i = 0; i < badges.size(); ++i)
        for (size_t j = i + 1; j < badges.size(); ++j) {
            const lv_area_t& a = badges[i];
            const lv_area_t& b = badges[j];
            CAPTURE(i, j, a.x1, a.y1, a.x2, a.y2, b.x1, b.y1, b.x2, b.y2);
            CHECK_FALSE((a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1));
        }
    lv_obj_t* empty = ui_ams_slot_get_material_label(slot(1));
    REQUIRE(empty != nullptr);
    CHECK(lv_obj_is_hidden(empty));
    // Labels, where the drawing shows any, follow the same rules as everywhere.
    lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, "labels_layer");
    std::vector<lv_obj_t*> labels;
    for (uint32_t k = 0; layer && k < lv_obj_get_child_count(layer); ++k) {
        lv_obj_t* c = lv_obj_get_child(layer, (int32_t)k);
        const char* name = lv_obj_get_name(c);
        if (name && std::strcmp(name, "material_label") == 0)
            labels.push_back(c);
    }
    check_labels_over_lanes(labels);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit art: the path below gets the height its stack needs, the head on screen",
                 "[ams][tray][ui_integration][unit-art][art-fit]") {
    // Screens, and the panel area each gives the AMS panel: short enough that the drawing,
    // at the size its width gives it, leaves its path too little room.
    const int size = GENERATE(0, 1, 2, 3);
    const int32_t sizes[4][4] = {
        {320, 480, 320, 410}, {800, 480, 800, 400}, {1024, 600, 1024, 500}, {480, 272, 480, 220}};
    const bool smallest = size == 3;
    CAPTURE(sizes[size][0], sizes[size][1], sizes[size][3]);
    helix::test::ScopedGeometry scoped(sizes[size][0], sizes[size][1]);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, sizes[size][2], sizes[size][3]);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    // OpenAMS's AMS 2 Pro: a tall drawing over a hub with an FPS buffer.
    unit_name_ = "AMS2";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/false, /*openams=*/true,
          /*buffer=*/true);
    lv_obj_update_layout(test_screen());
    lv_obj_t* drawing = lv_obj_find_by_name(container_, "unit_view_art");
    REQUIRE((drawing != nullptr && !lv_obj_is_hidden(drawing)));
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    lv_area_t cc, head, screen;
    lv_obj_get_coords(canvas, &cc);
    lv_obj_get_coords(host_ ? host_ : test_screen(), &screen);
    REQUIRE(ui_filament_path_canvas_get_toolhead_box(canvas, &head));
    const int32_t need = ui_filament_path_canvas_required_height(canvas);
    CAPTURE(cc.y1, cc.y2, need, head.y1, head.y2, screen.y2);
    // The head is on screen, and the unit shrinks until the canvas holds the full-size stack.
    // At 480x272 even a unit at half its size leaves too little, and the head gives up the rest.
    CHECK(head.y2 <= screen.y2);
    CHECK(head.y2 <= cc.y2);
    if (!smallest)
        CHECK(lv_area_get_height(&cc) >= need);
    // The hub stands clear of every lane's number.
    auto* cd = helix::ui::fpath::get_data(canvas);
    REQUIRE((cd != nullptr && cd->hits.hub_valid));
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
        if (!badge || lv_obj_is_hidden(badge))
            continue;
        lv_area_t ba;
        lv_obj_get_coords(badge, &ba);
        CAPTURE(i, ba.y2, cd->hits.hub.y1);
        CHECK(ba.y2 < cd->hits.hub.y1);
    }
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS unit art: a unit cut to fit its path keeps that size whatever was sized since",
    "[ams][tray][ui_integration][unit-art][art-fit]") {
    // At 480x272 the AMS 2 Pro's drawing leaves its path too little room even at half size.
    helix::test::ScopedGeometry scoped(480, 272);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, 480, 220);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    unit_name_ = "AMS2";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/false, /*openams=*/true,
          /*buffer=*/true);
    namespace at = helix::ui::ams_tray;
    REQUIRE(at::s_tray.art);
    lv_obj_t* slot_area = lv_obj_get_parent(slot_grid_);
    lv_obj_update_layout(slot_area);
    const int32_t width = lv_obj_get_content_width(slot_area);
    const auto shown = (int32_t)at::s_tray.spool_size;
    CAPTURE(width, shown);
    REQUIRE(helix::ui::ams_detail_spool_size(width, 4) == shown);
    // A wider row is sized (the overview sizes its hub for its widest unit), and the path is
    // set up again: the unit keeps its size, so its next layout starts there.
    int wider = 5;
    while (wider < 16 && helix::ui::ams_detail_spool_size(width, wider) / 2 >= shown)
        ++wider;
    CAPTURE(wider);
    REQUIRE(helix::ui::ams_detail_spool_size(width, wider) / 2 < shown);
    (void)helix::ui::ams_detail_slot_layout(width, wider);
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    REQUIRE(canvas != nullptr);
    CHECK_FALSE(ams_detail_setup_path_canvas(canvas, slot_grid_, -1));
    CHECK(helix::ui::ams_detail_spool_size(width, 4) == shown);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit art in portrait at 320x480: the hub stands clear of the lane numbers",
                 "[ams][tray][ui_integration][portrait][unit-art]") {
    helix::test::ScopedGeometry portrait(320, 480);
    host_ = lv_obj_create(test_screen());
    lv_obj_set_size(host_, 320, 410);
    lv_obj_set_style_pad_all(host_, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(host_, 0, LV_PART_MAIN);
    unit_name_ = "ACE 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/true);
    lv_obj_update_layout(test_screen());
    lv_obj_t* drawing = lv_obj_find_by_name(container_, "unit_view_art");
    REQUIRE((drawing != nullptr && !lv_obj_is_hidden(drawing)));
    auto* cd = helix::ui::fpath::get_data(lv_obj_find_by_name(panel_obj_, "path_canvas"));
    REQUIRE((cd != nullptr && cd->hits.hub_valid));
    const lv_area_t& hub = cd->hits.hub;
    int checked = 0;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(i));
        if (!badge || lv_obj_is_hidden(badge))
            continue;
        lv_area_t ba;
        lv_obj_get_coords(badge, &ba);
        CAPTURE(i, ba.x1, ba.y1, ba.x2, ba.y2, hub.x1, hub.y1, hub.x2, hub.y2);
        CHECK(ba.y2 < hub.y1);
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: the path fills the height below the unit, the head near its bottom",
                 "[ams][tray][ui_integration][full-height]") {
    const bool art = GENERATE(false, true);
    const int geometry = GENERATE(0, 1, 2);
    const int32_t sizes[3][2] = {{800, 480}, {480, 272}, {1024, 600}};
    CAPTURE(art, sizes[geometry][0], sizes[geometry][1]);
    helix::test::ScopedGeometry scoped(sizes[geometry][0], sizes[geometry][1]);
    if (art)
        unit_name_ = "ACE 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/art);
    lv_obj_update_layout(test_screen());
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    lv_obj_t* path = lv_obj_find_by_name(panel_obj_, "path_container");
    REQUIRE((canvas != nullptr && path != nullptr));
    lv_area_t cc, pc, head;
    lv_obj_get_coords(canvas, &cc);
    lv_obj_get_coords(path, &pc);
    REQUIRE(ui_filament_path_canvas_get_toolhead_box(canvas, &head));
    CAPTURE(cc.y1, cc.y2, pc.y1, pc.y2, head.y1, head.y2);
    // The canvas ends where its container does, however far it reaches up into the unit.
    CHECK(std::abs(cc.y2 - pc.y2) <= 1);
    // The head stands near the canvas's bottom.
    CHECK(head.y2 <= cc.y2);
    CHECK(cc.y2 - head.y2 <= lv_area_get_height(&cc) / 10);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit: each label stands over its own spool on a short vertical tick",
                 "[ams][tray][ui_integration][stagger][over-lane]") {
    const bool art = GENERATE(false, true);
    const int lanes = GENERATE(4, 8);
    const bool small = GENERATE(false, true);
    CAPTURE(art, lanes, small);
    helix::test::ScopedGeometry geometry(small ? 480 : 800, small ? 272 : 480);
    if (art)
        unit_name_ = "ACE 1";
    build("passive", lanes, /*tool_changer=*/false, /*ace=*/art);
    if (art) {
        lv_obj_t* drawing = lv_obj_find_by_name(container_, "unit_view_art");
        if (!drawing || lv_obj_is_hidden(drawing))
            SKIP("no art for this lane count");
    }
    lv_obj_update_layout(test_screen());
    lv_obj_t* layer = lv_obj_find_by_name(panel_obj_, "labels_layer");
    REQUIRE(layer != nullptr);
    std::vector<lv_obj_t*> labels;
    for (uint32_t k = 0; k < lv_obj_get_child_count(layer); ++k) {
        lv_obj_t* c = lv_obj_get_child(layer, (int32_t)k);
        const char* name = lv_obj_get_name(c);
        if (name && std::strcmp(name, "material_label") == 0)
            labels.push_back(c);
    }
    REQUIRE((int)labels.size() == lanes);
    check_labels_over_lanes(labels);
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit with baked art: a tap anywhere down a lane opens that lane's slot",
                 "[ams][tray][ui_integration][unit-art][hit_band]") {
    unit_name_ = "ACE 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/true);
    lv_obj_t* art = lv_obj_find_by_name(container_, "unit_view_art");
    REQUIRE(art != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(art));
    lv_area_t aa;
    lv_obj_get_coords(art, &aa);
    auto owner = [&](lv_point_t p) {
        for (lv_obj_t* o = lv_indev_search_obj(test_screen(), &p); o; o = lv_obj_get_parent(o)) {
            for (int i = 0; i < 4; ++i) {
                if (o == slot(i))
                    return i;
            }
        }
        return -1;
    };
    for (int i = 0; i < 4; ++i) {
        lv_area_t spool;
        lv_point_t outlet;
        REQUIRE(helix::ui::unit_art_lane_place(art, i, spool, outlet));
        // The lanes lean with the drawing: each outlet stands left of its spool's middle.
        const lv_point_t mid{aa.x1 + (spool.x1 + spool.x2) / 2, aa.y1 + (spool.y1 + spool.y2) / 2};
        const lv_point_t out{aa.x1 + outlet.x, aa.y1 + outlet.y};
        const lv_point_t below{out.x, out.y + 8};
        const lv_point_t upper{mid.x, mid.y - (spool.y2 - spool.y1) / 3};
        for (lv_point_t p : {upper, mid, out, below}) {
            CAPTURE(i, p.x, p.y);
            CHECK(owner(p) == i);
        }
    }
}

TEST_CASE_METHOD(AmsTrayPanelFixture,
                 "AMS unit with baked art: the art stands in for the tray, lanes leave its foot",
                 "[ams][tray][ui_integration][unit-art]") {
    unit_name_ = "ACE 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/true);
    lv_obj_t* art = lv_obj_find_by_name(container_, "unit_view_art");
    REQUIRE(art != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(art));
    lv_area_t aa;
    lv_obj_get_coords(art, &aa);
    auto* path = helix::ui::fpath::get_data(lv_obj_find_by_name(panel_obj_, "path_canvas"));
    REQUIRE(path != nullptr);
    lv_area_t pc;
    lv_obj_get_coords(lv_obj_find_by_name(panel_obj_, "path_canvas"), &pc);
    lv_obj_t* badges = lv_obj_find_by_name(panel_obj_, "badge_layer");
    REQUIRE(badges != nullptr);
    int32_t band_minus_start[4] = {};
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_area_t spool;
        lv_point_t outlet;
        REQUIRE(helix::ui::unit_art_lane_place(art, i, spool, outlet));
        // The slot's own spool is not drawn; its tap target stands over the art's spool.
        lv_obj_t* lane_spool = lv_obj_find_by_name(slot(i), "lane_spool");
        REQUIRE(lane_spool != nullptr);
        CHECK(lv_obj_get_style_opa(lane_spool, LV_PART_MAIN) == LV_OPA_TRANSP);
        lv_area_t sa;
        lv_obj_get_coords(slot(i), &sa);
        const int32_t spool_mid = aa.x1 + (spool.x1 + spool.x2) / 2;
        CHECK((sa.x1 <= spool_mid && spool_mid <= sa.x2));
        // The path's lane runs straight down from the lane's outlet.
        CHECK(std::abs(pc.x1 + helix::ui::fpath::get_slot_x(path, i, pc.x1) - (aa.x1 + outlet.x)) <=
              1);
        // The lane's tube starts on the unit's bottom edge, slanted as drawn, where it crosses
        // the lane's column; its band stands on a line parallel to the edge, its number level
        // with the band and right of the column.
        lv_point_t ea, eb;
        REQUIRE(helix::ui::unit_art_bottom_edge(art, ea, eb));
        const float column = (float)(aa.x1 + outlet.x);
        const float edge_y = (float)(aa.y1 + ea.y) + (float)(eb.y - ea.y) *
                                                         (column - (float)(aa.x1 + ea.x)) /
                                                         (float)(eb.x - ea.x);
        int32_t start = 0, band = 0;
        lv_obj_t* canvas_obj = lv_obj_find_by_name(panel_obj_, "path_canvas");
        REQUIRE(ui_filament_path_canvas_get_lane_rows(canvas_obj, i, &start, &band));
        CAPTURE(start, band, edge_y);
        CHECK(std::fabs((float)start - edge_y) <= 1.0f);
        band_minus_start[i] = band - start;
        lv_area_t ba;
        lv_obj_get_coords(ui_ams_slot_get_status_badge(slot(i)), &ba);
        CHECK(ba.x1 > aa.x1 + outlet.x);
        CHECK(std::abs((ba.y1 + ba.y2) / 2 - band) <= 1);
        // The lane's number sits centered in its circle.
        lv_obj_t* number =
            lv_obj_find_by_name(ui_ams_slot_get_status_badge(slot(i)), "slot_badge_label");
        REQUIRE(number != nullptr);
        lv_area_t na;
        lv_obj_get_coords(number, &na);
        CHECK(std::abs((na.x1 + na.x2) - (ba.x1 + ba.x2)) <= 2);
        CHECK(std::abs((na.y1 + na.y2) - (ba.y1 + ba.y2)) <= 2);
    }
    for (int i = 1; i < 4; ++i)
        CHECK(band_minus_start[i] == band_minus_start[0]);
    CHECK(band_minus_start[0] > 0);
    // The canvas reaches up to every lane's start.
    lv_area_t canvas_area;
    lv_obj_get_coords(lv_obj_find_by_name(panel_obj_, "path_canvas"), &canvas_area);
    for (int i = 0; i < 4; ++i) {
        int32_t start = 0;
        REQUIRE(ui_filament_path_canvas_get_lane_rows(
            lv_obj_find_by_name(panel_obj_, "path_canvas"), i, &start, nullptr));
        CHECK(start >= canvas_area.y1);
    }
    // No tube is drawn over the unit: the layer in front of the spools paints nothing.
    lv_obj_t* front = lv_obj_find_by_name(panel_obj_, "slot_tray");
    REQUIRE(front != nullptr);
    lv_draw_buf_t* snap = lv_snapshot_take(front, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    int painted = 0;
    for (uint32_t y = 0; y < snap->header.h; ++y) {
        const auto* row = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(snap, 0, y));
        for (uint32_t x = 0; x < snap->header.w; ++x)
            painted += row[x * 4 + 3] > 0;
    }
    lv_draw_buf_destroy(snap);
    CHECK(painted == 0);
    // The drawing's foot stands on the unit row's bottom edge; the path's lane entry is the
    // lowest of the lanes' starts on the slanted edge.
    lv_area_t drawn, g, cc;
    REQUIRE(helix::ui::unit_art_drawn_area(art, drawn));
    lv_obj_get_coords(slot_grid_, &g);
    lv_obj_get_coords(container_, &cc);
    CHECK(aa.y1 + drawn.y2 == cc.y2);
    int32_t lowest_start = INT32_MIN;
    for (int i = 0; i < 4; ++i) {
        int32_t start = 0;
        REQUIRE(ui_filament_path_canvas_get_lane_rows(
            lv_obj_find_by_name(panel_obj_, "path_canvas"), i, &start, nullptr));
        lowest_start = std::max(lowest_start, start);
    }
    CHECK(path->lane_entry_offset + g.y1 == lowest_start);
    // A number standing below the foot, past its own layer, is still drawn: by the path.
    lv_obj_t* canvas = lv_obj_find_by_name(panel_obj_, "path_canvas");
    // The lowest number, which hangs past the unit row.
    int lowest_lane = 0;
    lv_area_t low;
    lv_obj_get_coords(ui_ams_slot_get_status_badge(slot(0)), &low);
    for (int i = 1; i < 4; ++i) {
        lv_area_t a;
        lv_obj_get_coords(ui_ams_slot_get_status_badge(slot(i)), &a);
        if (a.y2 > low.y2) {
            low = a;
            lowest_lane = i;
        }
    }
    REQUIRE(low.y2 > cc.y2);
    REQUIRE(canvas != nullptr);
    lv_obj_t* screen = lv_obj_get_screen(canvas);
    lv_area_t sc;
    lv_obj_get_coords(screen, &sc);
    auto badge_color_at = [&](bool shown) {
        lv_obj_set_hidden(ui_ams_slot_get_status_badge(slot(lowest_lane)), !shown);
        lv_draw_buf_t* shot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(shot != nullptr);
        const auto* px = static_cast<const uint8_t*>(
            lv_draw_buf_goto_xy(shot, (low.x1 + low.x2) / 2 - sc.x1, low.y2 - 2 - sc.y1));
        const uint32_t rgb = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
        lv_draw_buf_destroy(shot);
        return rgb;
    };
    CHECK(badge_color_at(true) != badge_color_at(false));
    lv_obj_set_hidden(ui_ams_slot_get_status_badge(slot(lowest_lane)), false);
    // The readout tucks low over the drawing's end, clear of the last number.
    lv_obj_t* pill = lv_obj_find_by_name(panel_obj_, "env_indicator");
    REQUIRE(pill != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(pill));
    lv_area_t pa, last;
    lv_obj_get_coords(pill, &pa);
    lv_obj_get_coords(lv_obj_get_child(badges, -1), &last);
    const int32_t tuck = theme_manager_get_spacing("space_sm");
    CHECK(pa.x1 > last.x2);
    CHECK(pa.y2 == aa.y1 + drawn.y2 + tuck);
}

TEST_CASE_METHOD(
    AmsTrayPanelFixture,
    "AMS unit with baked art: an empty lane's badge carries its plus, a missing bay dims",
    "[ams][tray][ui_integration][unit-art]") {
    unit_name_ = "ACE 1";
    build("passive", 4, /*tool_changer=*/false, /*ace=*/true);
    // Each badge carries its lane's "+" exactly when the lane shows as empty.
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        lv_obj_t* spool = lv_obj_find_by_name(slot(i), "lane_spool");
        lv_obj_t* plus = lv_obj_find_by_name(ui_ams_slot_get_status_badge(slot(i)), "badge_plus");
        REQUIRE(plus != nullptr);
        CHECK(lv_obj_is_hidden(plus) == !helix::ui::ams_lane_spool_empty(spool));
    }
    // The badge shows what it is told: the "+" after the number, the dot on its corner.
    lv_obj_t* badge = ui_ams_slot_get_status_badge(slot(2));
    ui_ams_slot_set_badge_marks(slot(2), true, true, SlotError::Severity::ERROR);
    CHECK_FALSE(lv_obj_is_hidden(lv_obj_find_by_name(badge, "badge_plus")));
    lv_obj_t* dot = lv_obj_find_by_name(badge, "badge_error_dot");
    REQUIRE(dot != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(dot));
    lv_obj_update_layout(badge);
    lv_area_t da, ba;
    lv_obj_get_coords(dot, &da);
    lv_obj_get_coords(badge, &ba);
    CHECK(da.x2 > ba.x2);
    CHECK(da.y1 < ba.y1);
    ui_ams_slot_set_badge_marks(slot(2), false, false, SlotError::Severity::INFO);
    CHECK(lv_obj_is_hidden(dot));
    // A bay that is not on the bus takes the disabled state, and the art dims its spool.
    lv_obj_t* art = lv_obj_find_by_name(container_, "unit_view_art");
    REQUIRE(art != nullptr);
    lv_obj_add_state(slot(1), LV_STATE_DISABLED);
    helix::ui::WidgetRef slots[4];
    for (int i = 0; i < 4; ++i)
        slots[i].reset(slot(i));
    AmsSystemInfo info = *AmsState::instance().system_info();
    info.units[0].absent = true;
    helix::ui::ams_detail_sync_slot_states(slots, 4, info);
    CHECK(helix::ui::unit_art_lane_mark(art, 1).dim);
}

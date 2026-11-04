// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_list_connection_dot.cpp
 * @brief The Printers list shows a connection dot only where the state is known: the
 *        connected printer's row, colored by its live connection state.
 */

#include "ui_nav_printer_badge.h"
#include "ui_printer_list_overlay.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/config_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

class ConnectionDotFixture : public XMLTestFixture {
  public:
    ConnectionDotFixture() {
        REQUIRE(register_component("printer_list_item"));
        REQUIRE(register_component("printer_list_overlay"));

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "alpha";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "alpha";
        get_printer_state().init_subjects(false);
    }

    ~ConnectionDotFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
    }

    static lv_obj_t* dot_of(lv_obj_t* root, const char* printer_id) {
        lv_obj_t* row = lv_obj_find_by_name(root, printer_id);
        REQUIRE(row != nullptr);
        lv_obj_t* dot = lv_obj_find_by_name(row, "connection_dot");
        REQUIRE(dot != nullptr);
        return dot;
    }

    helix::Config* cfg_ = nullptr;

  private:
    nlohmann::json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE_METHOD(ConnectionDotFixture,
                 "Printers list: only the connected printer's row has a dot, in its live color",
                 "[multi-printer][printer_list]") {
    helix::ui::PrinterListOverlay overlay;
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    lv_obj_t* alpha_dot = dot_of(root, "alpha");
    CHECK_FALSE(lv_obj_has_flag(alpha_dot, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(dot_of(root, "beta"), LV_OBJ_FLAG_HIDDEN));

    lv_subject_t* state =
        get_printer_state().network_state().get_printer_connection_state_subject();
    lv_subject_set_int(state, 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(alpha_dot, LV_PART_MAIN),
                      helix::ui::connection_dot_color(0)));

    lv_subject_set_int(state, 2);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(alpha_dot, LV_PART_MAIN),
                      helix::ui::connection_dot_color(2)));
}

TEST_CASE_METHOD(ConnectionDotFixture,
                 "Printers list: a printer not yet identified shows its host, not its id",
                 "[multi-printer][printer_list]") {
    helix::ConfigTestAccess::data(*cfg_)["printers"]["printer-2"]["moonraker_host"] = "10.0.0.9";
    helix::ui::PrinterListOverlay overlay;
    lv_obj_t* root = overlay.create(test_screen());
    REQUIRE(root != nullptr);
    overlay.on_activate();
    helix::ui::UpdateQueue::instance().drain_until_empty();

    lv_obj_t* row = lv_obj_find_by_name(root, "printer-2");
    REQUIRE(row != nullptr);
    lv_obj_t* name = lv_obj_find_by_name(row, "printer_name");
    REQUIRE(name != nullptr);
    CHECK(std::string(lv_label_get_text(name)) == "10.0.0.9");
}

// A repopulation clears and rebuilds the rows inside the overlay's own call; queued
// work, such as the go_back a row click schedules, waits for the next tick instead of
// running under it, where it could free the overlay mid-populate.
TEST_CASE_METHOD(ConnectionDotFixture, "Printers list: repopulating runs no queued callback",
                 "[multi-printer][printer_list][drain_reentrancy]") {
    auto overlay = std::make_unique<helix::ui::PrinterListOverlay>();
    lv_obj_t* root = overlay->create(test_screen());
    REQUIRE(root != nullptr);

    bool ran = false;
    helix::ui::UpdateQueue::instance().queue("test::free_printer_list", [&ran, &overlay]() {
        ran = true;
        overlay.reset();
    });
    overlay->on_activate();
    REQUIRE_FALSE(ran);
    CHECK(lv_obj_find_by_name(root, "alpha") != nullptr);
    CHECK(lv_obj_find_by_name(root, "beta") != nullptr);

    helix::ui::UpdateQueue::instance().drain_until_empty();
    CHECK(ran);
    CHECK(overlay == nullptr);
}

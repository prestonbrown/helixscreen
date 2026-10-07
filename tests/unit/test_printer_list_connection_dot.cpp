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
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "printer_state.h"

#include <lvgl.h>

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
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    lv_obj_t* alpha_dot = dot_of(root, "alpha");
    CHECK_FALSE(lv_obj_has_flag(alpha_dot, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(dot_of(root, "beta"), LV_OBJ_FLAG_HIDDEN));

    lv_subject_t* state =
        get_printer_state().network_state().get_printer_connection_state_subject();
    lv_subject_set_int(state, 0);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(alpha_dot, LV_PART_MAIN),
                      helix::ui::connection_dot_color(0)));

    lv_subject_set_int(state, 2);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(alpha_dot, LV_PART_MAIN),
                      helix::ui::connection_dot_color(2)));
}

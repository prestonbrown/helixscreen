// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nav_printer_badge.cpp
 * @brief The navbar printer badge: connection dot and printer switch callbacks
 */

#include "ui_nav_printer_badge.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "connection_state.h"
#include "printer_state.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

using helix::ui::PrinterBadgeMenu;

namespace {

class PrinterBadgeFixture : public LVGLUITestFixture {
  public:
    PrinterBadgeFixture() {
        navbar_ = lv_obj_create(test_screen());
        dot_ = lv_obj_create(navbar_);
        lv_obj_set_name(dot_, "nav_printer_dot");
        conn_ = get_printer_state().network_state().get_printer_connection_state_subject();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    void connect_as(helix::ConnectionState state) {
        lv_subject_set_int(conn_, static_cast<int>(state));
        drain();
    }

    uint32_t dot_color() {
        return lv_color_to_u32(lv_obj_get_style_bg_color(dot_, LV_PART_MAIN));
    }

    lv_obj_t* navbar_ = nullptr;
    lv_obj_t* dot_ = nullptr;
    lv_subject_t* conn_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(PrinterBadgeFixture, "Switch and add callbacks run when triggered",
                 "[navigation][printer_badge]") {
    PrinterBadgeMenu badge;
    std::string switched_to;
    int adds = 0;
    badge.set_callbacks([&](const std::string& id) { switched_to = id; }, [&] { ++adds; });

    badge.trigger_switch("printer_2");
    badge.trigger_add();

    CHECK(switched_to == "printer_2");
    CHECK(adds == 1);
}

TEST_CASE_METHOD(PrinterBadgeFixture, "Shutdown drops the callbacks",
                 "[navigation][printer_badge]") {
    PrinterBadgeMenu badge;
    int calls = 0;
    badge.set_callbacks([&](const std::string&) { ++calls; }, [&] { ++calls; });

    badge.shutdown();
    badge.trigger_switch("printer_2");
    badge.trigger_add();

    CHECK(calls == 0);
}

TEST_CASE_METHOD(PrinterBadgeFixture, "The dot follows the connection state",
                 "[navigation][printer_badge]") {
    PrinterBadgeMenu badge;
    badge.wire(navbar_);

    connect_as(helix::ConnectionState::CONNECTED);
    CHECK(dot_color() == lv_color_to_u32(theme_manager_get_color("success")));

    connect_as(helix::ConnectionState::CONNECTING);
    CHECK(dot_color() == lv_color_to_u32(theme_manager_get_color("warning")));

    connect_as(helix::ConnectionState::DISCONNECTED);
    CHECK(dot_color() == lv_color_to_u32(theme_manager_get_color("danger")));
}

TEST_CASE_METHOD(PrinterBadgeFixture, "Shutdown stops the dot following the connection state",
                 "[navigation][printer_badge]") {
    PrinterBadgeMenu badge;
    badge.wire(navbar_);
    connect_as(helix::ConnectionState::CONNECTED);
    REQUIRE(dot_color() == lv_color_to_u32(theme_manager_get_color("success")));

    badge.shutdown();
    connect_as(helix::ConnectionState::DISCONNECTED);

    CHECK(dot_color() == lv_color_to_u32(theme_manager_get_color("success")));
}

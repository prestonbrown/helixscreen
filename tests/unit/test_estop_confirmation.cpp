// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The E-Stop confirmation dialog comes from the bare Modal::show() factory,
// which never tells the overlay when a backdrop tap or ESC closes it. Every
// later press must still reach the printer (prestonbrown/helixscreen#1795).

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_client.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Counts M112 requests instead of sending them.
class StopCountingMoonrakerAPI : public MoonrakerAPI {
  public:
    using MoonrakerAPI::MoonrakerAPI;
    void emergency_stop(SuccessCallback, ErrorCallback) override {
        ++stops;
    }
    int stops = 0;
};

struct EstopConfirmFixture : public LVGLUITestFixture {
    EstopConfirmFixture() : stop_api_(*client(), state()) {
        auto& estop = EmergencyStopOverlay::instance();
        estop.init_subjects();
        estop.init(state(), &stop_api_);
        estop.set_require_confirmation(true);
    }
    ~EstopConfirmFixture() override {
        EmergencyStopOverlay::instance().set_require_confirmation(false);
        close_all_modals();
    }

    /// Press a button wired to @p callback in XML. The trampolines ignore the event.
    static void press(const char* callback) {
        lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, callback);
        REQUIRE(cb != nullptr);
        cb(nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    static lv_obj_t* open_confirmation() {
        lv_obj_t* top = Modal::get_top();
        return top && lv_obj_get_name(top) &&
                       std::string(lv_obj_get_name(top)) == "estop_confirmation_dialog"
                   ? top
                   : nullptr;
    }

    void close_all_modals() {
        while (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
            process_lvgl(500);
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    StopCountingMoonrakerAPI stop_api_;
};

} // namespace

TEST_CASE_METHOD(EstopConfirmFixture, "E-Stop asks again after its dialog is dismissed by backdrop",
                 "[estop][estop_confirm]") {
    press("emergency_stop_clicked");
    lv_obj_t* first = open_confirmation();
    REQUIRE(first != nullptr);

    // A backdrop tap or ESC closes it through Modal::hide(), not the overlay.
    close_all_modals();
    REQUIRE(open_confirmation() == nullptr);

    press("emergency_stop_clicked");
    CHECK(open_confirmation() != nullptr);
    CHECK(stop_api_.stops == 0);
}

TEST_CASE_METHOD(EstopConfirmFixture, "a second E-Stop press while confirming stops the printer",
                 "[estop][estop_confirm]") {
    press("emergency_stop_clicked");
    REQUIRE(open_confirmation() != nullptr);
    REQUIRE(stop_api_.stops == 0);

    press("emergency_stop_clicked");
    CHECK(stop_api_.stops == 1);
}

TEST_CASE_METHOD(EstopConfirmFixture, "the dialog's STOP button stops the printer once",
                 "[estop][estop_confirm]") {
    press("emergency_stop_clicked");
    REQUIRE(open_confirmation() != nullptr);

    press("estop_dialog_confirm_clicked");
    CHECK(stop_api_.stops == 1);
}

TEST_CASE_METHOD(EstopConfirmFixture, "the lock screen E-Stop stops without a confirmation",
                 "[estop][estop_confirm]") {
    // The lock screen is on lv_layer_top, above any modal: a dialog would open
    // hidden behind it.
    press("lock_screen_estop_clicked");
    CHECK(stop_api_.stops == 1);
    CHECK(open_confirmation() == nullptr);
}

TEST_CASE_METHOD(EstopConfirmFixture,
                 "a second tap on the navbar E-Stop over its own confirmation stops",
                 "[estop][estop_confirm][modal_covers_navbar]") {
    lv_subject_t* visible = lv_xml_get_subject(nullptr, "estop_visible");
    REQUIRE(visible != nullptr);
    const int saved = lv_subject_get_int(visible);
    lv_obj_t* navbar =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "navigation_bar", nullptr));
    REQUIRE(navbar != nullptr);
    NavigationManager::instance().wire_events(navbar);
    lv_obj_t* estop = NavigationManager::instance().navbar_estop();
    REQUIRE(estop != nullptr);
    lv_subject_set_int(visible, 1);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(test_screen());

    lv_area_t button;
    lv_obj_get_coords(estop, &button);
    lv_point_t spot{(button.x1 + button.x2) / 2, (button.y1 + button.y2) / 2};
    // What a finger at the E-Stop's spot reaches, given whatever is on top.
    auto tap = [&] {
        lv_obj_t* hit = lv_indev_search_obj(test_screen(), &spot);
        REQUIRE(hit != nullptr);
        lv_obj_send_event(hit, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
        return hit;
    };

    CHECK(tap() == estop);
    REQUIRE(open_confirmation() != nullptr);
    REQUIRE(stop_api_.stops == 0);

    CHECK(tap() == estop);
    CHECK(stop_api_.stops == 1);

    lv_subject_set_int(visible, saved);
    helix::ui::UpdateQueue::instance().drain();
}

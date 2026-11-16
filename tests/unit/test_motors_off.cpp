// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motors_off.cpp
 * @brief the shared motors-off confirm sends M84 only on confirm.
 *
 * Run with: ./build/bin/helix-tests "[controls][motors]"
 *
 * The helper owns the dialog, the M84 send and the guard's lifecycle; the
 * test drives the real ConfirmationModal buttons over a mock client, so a
 * wording or wiring regression that reaches the wrong button - or no button -
 * shows up as a missing or spurious script in the client's history.
 */

#include "ui_modal.h"
#include "ui_motors_off.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

class MotorsOffFixture : public LVGLUITestFixture {
  public:
    MotorsOffFixture() {
        helix::ui::modal_init_subjects();
        set_moonraker_api(&api);
        client.clear_gcode_script_history();
    }

    ~MotorsOffFixture() override {
        set_moonraker_api(previous_api_);
        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        settle();
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void press(lv_obj_t* dialog, const char* name) {
        lv_obj_t* button = lv_obj_find_by_name(dialog, name);
        REQUIRE(button != nullptr);
        lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    }

    bool sent_m84() const {
        for (const auto& script : client.gcode_script_history()) {
            if (script.find("M84") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api_ = get_moonraker_api();
};

} // namespace

TEST_CASE_METHOD(MotorsOffFixture, "motors off confirm sends M84", "[controls][motors]") {
    helix::ui::WidgetRef guard;
    helix::ui::show_motors_off_confirm(&api, guard);
    REQUIRE(guard.get() != nullptr);

    press(guard.get(), "btn_primary");
    settle();

    CHECK(sent_m84());
    // The confirm path drops the stored handle, so the caller's next entry is
    // not blocked by a dialog that already closed.
    CHECK(guard.get() == nullptr);
}

TEST_CASE_METHOD(MotorsOffFixture, "motors off cancel moves nothing", "[controls][motors]") {
    helix::ui::WidgetRef guard;
    helix::ui::show_motors_off_confirm(&api, guard);
    REQUIRE(guard.get() != nullptr);

    press(guard.get(), "btn_secondary");
    settle();

    CHECK(client.gcode_script_history().empty());
    CHECK(guard.get() == nullptr);
}

TEST_CASE_METHOD(MotorsOffFixture, "motors off refuses a confirm made after a print started",
                 "[controls][motors]") {
    auto& ps = get_printer_state();
    ps.update_from_status({{"print_stats", {{"state", "standby"}}}});
    settle();

    helix::ui::WidgetRef guard;
    helix::ui::show_motors_off_confirm(&api, guard);
    REQUIRE(guard.get() != nullptr);

    // A print starts from elsewhere while the dialog is open.
    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    settle();
    REQUIRE(lv_subject_get_int(ps.print_state().get_machine_motion_blocked_subject()) == 1);

    press(guard.get(), "btn_primary");
    settle();

    CHECK_FALSE(sent_m84());
    CHECK(guard.get() == nullptr);

    ps.update_from_status({{"print_stats", {{"state", "standby"}}}});
    settle();
}

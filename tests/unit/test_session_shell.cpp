// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_session_shell.cpp
 * @brief create_app_layout(): the app shell desktop and the ESP32 firmware both build. The
 *        panels it holds are set up by setup_app_panels(), which needs a whole app.
 *
 * The navbar's printer menu reaches the session only through the callbacks the shell
 * registers, and the navbar's printer icon only moves once its observers are installed.
 */

#include "ui_nav_manager.h"
#include "ui_printer_status_icon.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "session_wiring.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct SessionShellFixture : public LVGLUITestFixture {
    lv_obj_t* shell = nullptr;
    std::string switched_to;
    int adds = 0;

    SessionShellFixture() {
        // A fresh icon: one some earlier case initialised would pass on its own.
        PrinterStatusIcon::instance().deinit_subjects();
        PrinterStatusIcon::instance().init_subjects();
        shell = helix::create_app_layout(
            test_screen(), [this](const std::string& id) { switched_to = id; }, [this] { ++adds; });
    }

    ~SessionShellFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        NavigationManager::instance().set_printer_callbacks(nullptr, nullptr);
        PrinterStatusIcon::instance().deinit_subjects();
        // The navigation holds the layout, the navbar and the widgets wire_events() made;
        // released here, before the shell they point into is deleted.
        NavigationManager::instance().deinit_subjects();
        if (shell) {
            lv_obj_delete(shell);
        }
    }

    static int icon_state() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "printer_icon_state"));
    }
};

} // namespace

TEST_CASE_METHOD(SessionShellFixture, "the shell holds the navbar and the panel container",
                 "[session_wiring][ui_integration]") {
    REQUIRE(shell != nullptr);
    CHECK(lv_obj_find_by_name(shell, "navbar") != nullptr);
    CHECK(lv_obj_find_by_name(shell, "panel_container") != nullptr);
}

TEST_CASE_METHOD(SessionShellFixture, "the navbar's printer menu reaches the session",
                 "[session_wiring][ui_integration]") {
    REQUIRE(shell != nullptr);

    NavigationManager::instance().trigger_printer_switch("bravo");
    CHECK(switched_to == "bravo");

    NavigationManager::instance().trigger_add_printer();
    CHECK(adds == 1);
}

TEST_CASE_METHOD(SessionShellFixture, "the printer icon follows the connection",
                 "[session_wiring][ui_integration]") {
    REQUIRE(shell != nullptr);
    REQUIRE(icon_state() == static_cast<int>(PrinterIconState::DISCONNECTED));

    auto& ps = get_printer_state();
    ps.set_klippy_state_sync(KlippyState::READY);
    lv_subject_set_int(ps.network_state().get_printer_connection_state_subject(),
                       static_cast<int>(ConnectionState::CONNECTED));
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(icon_state() == static_cast<int>(PrinterIconState::READY));
}

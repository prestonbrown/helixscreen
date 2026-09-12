// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_delete_guard.cpp
 * @brief A confirmed delete must never navigate away from the file list
 *
 * The delete-success callback always calls hide_detail_view(), and a delete
 * launched by long-press never opens the detail overlay: on_file_long_pressed()
 * applies the selection state and shows the confirmation modal directly. So
 * hide_detail_view() has to gate on is_visible() itself — PrintSelectDetailView::
 * hide()'s only guard is overlay_root_, which panel setup() creates eagerly, so
 * hide() cannot tell an open overlay from one that was never pushed. Hiding one
 * that was never pushed reaches go_back(), which pops panel_stack_.back(); with
 * only a main panel on the stack that entry is the print-select panel itself,
 * and go_back()'s empty-stack fallback then activates Home.
 *
 * Also pins MoonrakerClientMock's server.files.delete_file: MoonrakerFileAPI::
 * delete_file sends that exact method, so nothing can drive the delete-success
 * path unless the mock answers it.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"
#include "ui_print_select_detail_view.h"

#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "moonraker_client_mock.h"

#include <ctime>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Long-press delete: the detail overlay is never pushed
// ============================================================================

TEST_CASE_METHOD(PrintSelectPanelFixture,
                 "Confirmed long-press delete with the detail view never opened stays on "
                 "print-select",
                 "[print_select][delete][navigation]") {
    PlantedGcode file("delete_guard_longpress.gcode");
    REQUIRE(file.on_disk());

    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));

    // The long-press chain minus its private trampolines: selection state,
    // confirmation modal (which does NOT need the detail view), then the
    // confirm handler's body. The detail overlay is never shown or pushed.
    panel_->set_selected_file(file.name().c_str(), "", "", "1h", "10 g", "--", "--", time(nullptr),
                              "0.2 mm", "PLA");
    panel_->show_delete_confirmation();
    panel_->delete_file();
    drain();

    // Nothing pushed the overlay, so hide_detail_view() must not reach
    // go_back(): the only stack entry is the print-select panel itself, and
    // popping it leaves the empty-stack fallback to choose a panel — Home.
    REQUIRE(NavigationManager::instance().get_active() == PanelId::PrintSelect);
    REQUIRE(NavigationManagerTestAccess::panel_stack(NavigationManager::instance()).size() == 1);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE_FALSE(file.on_disk());
}

// ============================================================================
// The guard must not overcorrect: detail view open, delete from it
// ============================================================================

TEST_CASE_METHOD(PrintSelectPanelFixture,
                 "Confirmed delete from the open detail view closes it and returns to the list",
                 "[print_select][delete][navigation]") {
    PlantedGcode file("delete_guard_detail.gcode");
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    // Open the detail view the way a file tap does — this pushes the overlay.
    REQUIRE(panel_->select_file_by_name(file.name()));
    drain();
    REQUIRE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    REQUIRE(NavigationManagerTestAccess::panel_stack(NavigationManager::instance()).size() == 2);

    // The detail view's delete button + the modal's confirm handler.
    panel_->show_delete_confirmation();
    panel_->delete_file();
    drain();

    REQUIRE(NavigationManager::instance().get_active() == PanelId::PrintSelect);
    REQUIRE(NavigationManagerTestAccess::panel_stack(NavigationManager::instance()).size() == 1);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE_FALSE(file.on_disk());
}

// ============================================================================
// The mock: server.files.delete_file must behave like the other file methods
// ============================================================================

TEST_CASE("MoonrakerClientMock server.files.delete_file removes the file and reports absence",
          "[print_select][delete][mock]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    PlantedGcode file("delete_guard_mock.gcode");
    REQUIRE(file.on_disk());

    bool success = false;
    bool error = false;
    std::string error_msg;
    json params = {{"path", "gcodes/" + file.name()}};

    mock.send_jsonrpc(
        "server.files.delete_file", params, [&](const json&) { success = true; },
        [&](const MoonrakerError& e) {
            error = true;
            error_msg = e.message;
        });

    CHECK(success);
    CHECK_FALSE(error);
    CHECK_FALSE(file.on_disk());

    // The directory listing the panel refreshes from no longer reports it.
    bool still_listed = false;
    mock.send_jsonrpc(
        "server.files.get_directory", json{{"root", "gcodes"}},
        [&](const json& response) {
            for (const auto& item : response.value("result", json::array())) {
                if (item.value("path", "") == file.name()) {
                    still_listed = true;
                }
            }
        },
        [](const MoonrakerError&) {});
    CHECK_FALSE(still_listed);

    // A file that is not there is an error, like real Moonraker's 404 — not a
    // silent success.
    success = false;
    error = false;
    mock.send_jsonrpc(
        "server.files.delete_file", params, [&](const json&) { success = true; },
        [&](const MoonrakerError& e) {
            error = true;
            error_msg = e.message;
        });
    CHECK_FALSE(success);
    CHECK(error);
    CHECK_FALSE(error_msg.empty());

    mock.stop_temperature_simulation();
    mock.disconnect();
}

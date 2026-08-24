// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_delete_guard.cpp
 * @brief A confirmed delete must never navigate away from the file list
 *
 * The delete-success callback always calls hide_detail_view(), but a delete
 * launched by long-press never opened the detail overlay: on_file_long_pressed()
 * applies the selection state and shows the confirmation modal directly.
 * hide_detail_view() called PrintSelectDetailView::hide() unconditionally, and
 * hide()'s only guard is overlay_root_ — which panel setup() creates eagerly.
 * The go_back() inside then popped panel_stack_.back(); with only a main panel
 * on the stack that entry is the print-select panel itself, and go_back()'s
 * empty-stack fallback bounced the user to Home instead of refreshing the
 * list in place.
 *
 * Also pins MoonrakerClientMock's server.files.delete_file: MoonrakerFileAPI::
 * delete_file sends that exact method, and the mock dropped it on the floor
 * ("not implemented - callbacks not invoked"), so no test could drive the
 * delete-success path at all.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"
#include "ui_print_select_detail_view.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A .gcode planted in the mock's virtual gcodes root for one test.
/// MoonrakerClientMock backs the gcodes root with assets/test_gcodes on disk
/// (scan_mock_gcode_files() is a real directory scan), so a delete that
/// mutates the virtual FS removes a real file. Plant one nothing else depends
/// on and take it back out however the test ends.
class PlantedGcode {
  public:
    explicit PlantedGcode(const std::string& name) {
        for (const auto* prefix : {"", "../", "../../"}) {
            std::string dir = std::string(prefix) + "assets/test_gcodes";
            if (std::filesystem::is_directory(dir)) {
                path_ = dir + "/" + name;
                break;
            }
        }
        REQUIRE_FALSE(path_.empty());
        std::ofstream out(path_, std::ios::trunc);
        out << "; planted for the delete-guard tests\nG28\n";
    }

    ~PlantedGcode() {
        std::remove(path_.c_str());
    }

    PlantedGcode(const PlantedGcode&) = delete;
    PlantedGcode& operator=(const PlantedGcode&) = delete;

    bool on_disk() const {
        return std::filesystem::exists(path_);
    }
    std::string name() const {
        return std::filesystem::path(path_).filename().string();
    }

  private:
    std::string path_;
};

/// The real delete flow over the real panel and NavigationManager: mock client
/// connected, MoonrakerAPI on top of it, print_select_panel XML built, and the
/// navigation stack seeded the way the app has it (panel_stack_[0] = the active
/// main panel). Animations off — the deterministic go_back path runs inline in
/// the drain instead of an animation completion tick later.
class PrintSelectDeleteFixture : public LVGLUITestFixture {
  public:
    PrintSelectDeleteFixture()
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24, /*speedup_factor=*/100.0) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        // Connect before the API exists, like IdleRunoutGraceFixture: the
        // initial-state dispatch then has no subscribers to storm, while
        // get_connection_state() still reports CONNECTED for is_ready() checks.
        mock_client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, get_printer_state());

        panel_ = std::make_unique<PrintSelectPanel>(get_printer_state(), api_.get());
        panel_->init_subjects();
        panel_obj_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());

        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        panels[static_cast<int>(PanelId::PrintSelect)] = panel_obj_;
        auto& nav = NavigationManager::instance();
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::PrintSelect, panel_.get());
        nav.set_active(PanelId::PrintSelect);
        drain();
    }

    ~PrintSelectDeleteFixture() override {
        auto& nav = NavigationManager::instance();
        // Drop the panel registration BEFORE the panel dies: NavigationManager
        // keeps raw panel pointers, and the base fixture still walks it.
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        drain();
        panel_.reset();
        api_.reset();
        mock_client_.stop_temperature_simulation();
        mock_client_.disconnect();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// Every hop in the delete chain crosses the UpdateQueue (token.defer,
    /// queue_update, go_back's own queue_update, the refresh's on_files_ready);
    /// drain until fully empty like OverlayActivationFixture does.
    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    MoonrakerClientMock mock_client_;
    std::unique_ptr<MoonrakerAPI> api_;
    std::unique_ptr<PrintSelectPanel> panel_;
    lv_obj_t* panel_obj_ = nullptr;
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

// ============================================================================
// The regression: long-press delete never opened the detail overlay
// ============================================================================

TEST_CASE_METHOD(PrintSelectDeleteFixture,
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

    // Before the is_visible() gate, hide_detail_view() called go_back() with
    // the overlay never pushed: it popped the print-select panel itself and
    // the empty-stack fallback activated Home.
    REQUIRE(NavigationManager::instance().get_active() == PanelId::PrintSelect);
    REQUIRE(NavigationManagerTestAccess::panel_stack(NavigationManager::instance()).size() == 1);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE_FALSE(file.on_disk());
}

// ============================================================================
// The guard must not overcorrect: detail view open, delete from it
// ============================================================================

TEST_CASE_METHOD(PrintSelectDeleteFixture,
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

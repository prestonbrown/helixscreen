// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_filelist_move.cpp
 * @brief The panel's notify_filelist_changed handler must parse source_item.
 *
 * Moonraker builds the notification's `item` from a move's destination and
 * attaches the origin as `source_item`, so a gcode moved out of the gcodes
 * root reports item.root == "config" with source_item.root == "gcodes". The
 * root filter predicate is pinned as a pure function in
 * test_print_select_filelist_filter.cpp; these cases pin the wiring — the
 * registered callback must parse source_item and refresh, while a change
 * confined to foreign roots must not re-fetch.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "thumbnail_processor.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A .gcode planted in the mock's virtual gcodes root. MoonrakerClientMock
/// backs the gcodes root with assets/test_gcodes (a real directory scan), so
/// taking the file off disk changes what the NEXT get_directory reports while
/// the panel's cached list keeps naming it — which is exactly the stale
/// listing a missed refresh presents.
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
        out << "; planted for the filelist move tests\nG28\n";
    }

    ~PlantedGcode() {
        std::remove(path_.c_str());
    }

    PlantedGcode(const PlantedGcode&) = delete;
    PlantedGcode& operator=(const PlantedGcode&) = delete;

    bool on_disk() const {
        return std::filesystem::exists(path_);
    }

    /// The simulated move's effect on the printer's storage. The dtor's
    /// remove() tolerates the file already being gone.
    bool remove_from_disk() {
        return std::remove(path_.c_str()) == 0;
    }

    std::string name() const {
        return std::filesystem::path(path_).filename().string();
    }

  private:
    std::string path_;
};

/// The real notification path over the real panel: mock client connected,
/// MoonrakerAPI on top of it, print_select_panel XML built. The handler the
/// panel registers parses the frame on the calling thread and marshals the
/// refresh to the main thread via async_call, so firing the callbacks from
/// the test thread mirrors a WebSocket delivery.
class PrintSelectFilelistFixture : public LVGLUITestFixture {
  public:
    PrintSelectFilelistFixture()
        : mock_client_(MoonrakerClientMock::PrinterType::VORON_24, /*speedup_factor=*/100.0) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        mock_client_.connect("ws://mock/websocket", []() {}, []() {});
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, get_printer_state());

        panel_ = std::make_unique<PrintSelectPanel>(get_printer_state(), api_.get());
        panel_->init_subjects();
        panel_obj_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "print_select_panel", nullptr));
        REQUIRE(panel_obj_ != nullptr);
        panel_->setup(panel_obj_, test_screen());
        // setup() creates file_provider_; the app then hands the panel its API,
        // which is what registers the notify_filelist_changed handler this
        // file drives (and early-refreshes now that the mock is connected).
        panel_->set_api(api_.get());

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

    ~PrintSelectFilelistFixture() override {
        auto& nav = NavigationManager::instance();
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        drain();
        panel_.reset();
        api_.reset();
        mock_client_.stop_temperature_simulation();
        mock_client_.disconnect();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// The notification's refresh lands on the UpdateQueue, and its
    /// get_directory response feeds the ThumbnailProcessor pool; join both
    /// like the delete-guard fixture does.
    static void drain() {
        auto& processor = helix::ThumbnailProcessor::instance();
        auto& queue = helix::ui::UpdateQueue::instance();
        for (int pass = 0; pass < 4; ++pass) {
            processor.wait_for_completion();
            if (processor.pending_tasks() == 0 &&
                helix::ui::UpdateQueueTestAccess::queue_empty(queue)) {
                break;
            }
            helix::ui::UpdateQueueTestAccess::drain_all(queue);
        }
    }

    /// A notify_filelist_changed frame as the WebSocket dispatch would
    /// deliver it, with caller-chosen item and source_item roots.
    static json move_frame(const std::string& item_root, const char* source_root) {
        json item = {{"root", item_root}, {"path", "spare.cfg"}};
        json payload = {{"action", "move_file"}, {"item", item}};
        if (source_root != nullptr) {
            payload["source_item"] = {{"root", source_root}, {"path", "origin.gcode"}};
        }
        return json{{"jsonrpc", "2.0"},
                    {"method", "notify_filelist_changed"},
                    {"params", json::array({payload})}};
    }

    void fire(const json& frame) {
        MoonrakerClientTestAccess::fire_method_callbacks(mock_client_, "notify_filelist_changed",
                                                         frame);
        drain();
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
// The regression: a move OUT of gcodes reports a foreign item root
// ============================================================================

TEST_CASE_METHOD(PrintSelectFilelistFixture, "A move out of the gcodes root refreshes the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_out.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    // The move already happened on the printer's storage: only the
    // notification can tell the panel to look again. item describes the
    // destination; the gcodes origin lives in source_item.
    REQUIRE(file.remove_from_disk());
    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));

    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(PrintSelectFilelistFixture,
                 "A move confined to foreign roots does not re-fetch the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_foreign.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    fire(PrintSelectFilelistFixture::move_frame("config", "config"));
    // No refresh: the cached list still names a file the directory no longer
    // has, which is the price of filtering — one the config-root storm makes
    // worth paying.
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    // The callback is live, not silently dead: the same panel, with no
    // further refresh_files(true), still refreshes when a frame names gcodes.
    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(PrintSelectFilelistFixture,
                 "A foreign-root frame without source_item does not re-fetch the listing",
                 "[print_select][filelist][1575]") {
    PlantedGcode file("filelist_move_nosrc.gcode");
    REQUIRE(file.on_disk());
    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(file.remove_from_disk());

    // Uploads, creates and deletes carry no source_item at all; one of those
    // shaped like a foreign-root item must not refresh.
    fire(PrintSelectFilelistFixture::move_frame("config", nullptr));
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));

    fire(PrintSelectFilelistFixture::move_frame("config", "gcodes"));
    REQUIRE_FALSE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

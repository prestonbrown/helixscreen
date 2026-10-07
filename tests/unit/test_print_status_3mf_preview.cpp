// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_3mf_preview.cpp
 * @brief A .3mf print's preview streams only G-code QIDI extracted into .temp
 *
 * A .3mf is a zip archive: streamed into the viewer it fails to index and
 * raises an error toast over the thumbnail. These cases open the real print
 * status overlay, start its G-code load for a .3mf, answer the .temp listing,
 * and read what the panel asked the printer for.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "display_settings_manager.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "moonraker_file_api.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A .temp listing the test answers, and a count of size lookups.
class HeldListingFiles : public MoonrakerFileAPI {
  public:
    using MoonrakerFileAPI::MoonrakerFileAPI;

    void list_files(const std::string&, const std::string&, bool, FileListCallback on_success,
                    ErrorCallback on_error) override {
        on_list_ = std::move(on_success);
        on_list_error_ = std::move(on_error);
    }

    void get_file_metadata(const std::string&, FileMetadataCallback, ErrorCallback, bool) override {
        ++metadata_lookups;
    }

    bool pending() const {
        return static_cast<bool>(on_list_);
    }

    void deliver(const std::vector<FileInfo>& files) {
        auto done = std::move(on_list_);
        on_list_ = nullptr;
        done(files);
    }

    void fail() {
        auto failed = std::move(on_list_error_);
        on_list_ = nullptr;
        MoonrakerError err;
        err.message = "root .temp not registered";
        failed(err);
    }

    int metadata_lookups = 0;

  private:
    FileListCallback on_list_;
    ErrorCallback on_list_error_;
};

/// Records each download and completes none.
class RecordingTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file_to_path(const std::string& root, const std::string& path, const std::string&,
                               StringCallback, ErrorCallback, ProgressCallback) override {
        downloads.push_back(root + "/" + path);
    }

    std::vector<std::string> downloads;
};

class Api3mf : public MoonrakerAPIMock {
  public:
    Api3mf(MoonrakerClientMock& client, PrinterState& state)
        : MoonrakerAPIMock(client, state), files_(client), transfers_(client, "") {}

    MoonrakerFileAPI& files() override {
        return files_;
    }
    MoonrakerFileTransferAPI& transfers() override {
        return transfers_;
    }

    HeldListingFiles files_;
    RecordingTransfers transfers_;
};

FileInfo temp_file(const std::string& path, double modified = 0) {
    FileInfo f;
    f.path = path;
    f.size = 10;
    f.modified = modified;
    return f;
}

class PrintStatus3mfFixture : public LVGLUITestFixture {
  public:
    PrintStatus3mfFixture()
        : client_(MoonrakerClientMock::PrinterType::VORON_24), api_(client_, state()) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[UI_PANEL_COUNT] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);

        REQUIRE(PrintStatusPanel::push_overlay(test_screen()));
        drain();
        previous_api_ = PrintStatusPanelTestAccess::api(panel());
        panel().set_api(&api_);
    }

    ~PrintStatus3mfFixture() override {
        panel().set_api(previous_api_);
        NavigationManager::instance().go_back();
        PrintStatusPanel::destroy_cached_overlay(
            helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
        drain();
        process_lvgl(20);
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static PrintStatusPanel& panel() {
        return get_global_print_status_panel();
    }

    void load(const std::string& filename) {
        PrintStatusPanelTestAccess::load_gcode_for_viewing(panel(), filename);
        REQUIRE(api_.files_.pending());
    }

    MoonrakerClientMock client_;
    Api3mf api_;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(PrintStatus3mfFixture, "A .3mf with no G-code in .temp downloads nothing",
                 "[print_status][qidi]") {
    load("Benchy.gcode.3mf");
    api_.files_.deliver({temp_file("unrelated.gcode")});
    drain();

    CHECK(api_.files_.metadata_lookups == 0);
    CHECK(api_.transfers_.downloads.empty());
}

TEST_CASE_METHOD(PrintStatus3mfFixture, "A .3mf whose .temp listing fails downloads nothing",
                 "[print_status][qidi]") {
    load("Benchy.gcode.3mf");
    api_.files_.fail();
    drain();

    CHECK(api_.files_.metadata_lookups == 0);
    CHECK(api_.transfers_.downloads.empty());
}

TEST_CASE_METHOD(PrintStatus3mfFixture, "A .3mf streams its extracted G-code from .temp",
                 "[print_status][qidi]") {
    std::string expected;
    std::vector<FileInfo> files;
    SECTION("a shadow_native_plate file, newest of several") {
        files = {temp_file("shadow_native_plate_1.gcode", 100),
                 temp_file("shadow_native_plate_2.gcode", 200)};
        expected = ".temp/shadow_native_plate_2.gcode";
    }
    SECTION("the plate extracted under the print's own name") {
        files = {temp_file("Benchy.gcode")};
        expected = ".temp/Benchy.gcode";
    }

    load("Benchy.gcode.3mf");
    api_.files_.deliver(files);
    drain();

    CHECK(api_.transfers_.downloads == std::vector<std::string>{expected});
}

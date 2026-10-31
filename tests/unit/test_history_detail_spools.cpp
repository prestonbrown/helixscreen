// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_detail_spools.cpp
 * @brief History's detail card lists the spools a job used, from the
 *        HelixPrint plugin's filament_usage record.
 *
 * Run with: ./build/bin/helix-tests "[plugin_filament][history]"
 */

#include "ui_nav_manager.h"
#include "ui_panel_history_list.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../test_helpers/scoped_env.h"
#include "app_globals.h"
#include "filament_accounting.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix_plugin_features.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_plugin_status_state.h"
#include "printer_state.h"

#include <algorithm>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The mock printer with the plugin, its filament_usage method answering two
/// spools for any job, as the app's globals.
struct SpoolsFx : LVGLUITestFixture {
    ScopedEnv plugin_env{"HELIX_MOCK_HELIX_PLUGIN", "1"};
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::unique_ptr<MoonrakerAPIMock> api;
    IMoonrakerAPI* previous_api = nullptr;
    IMoonrakerClient* previous_client = nullptr;

    SpoolsFx() {
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPIMock>(client, get_printer_state());
        previous_api = get_moonraker_api();
        previous_client = get_moonraker_client();
        set_moonraker_api(api.get());
        set_moonraker_client(&client);
        PluginFeatureEntry entry;
        entry.version = kFilamentAccountingMinVersion;
        entry.available = true;
        entry.enabled = true;
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_installed(true);
        plugin.set_features({{kFilamentAccountingFeature, entry}});
        drain();
    }

    ~SpoolsFx() override {
        auto& plugin = get_printer_state().plugin_status_state();
        plugin.set_features({});
        plugin.set_installed(false);
        drain();
        set_moonraker_client(previous_client);
        set_moonraker_api(previous_api);
        client.stop_temperature_simulation();
        client.disconnect();
    }

    static void drain() {
        for (int i = 0; i < 20; ++i) {
            ui::UpdateQueue::instance().drain();
        }
    }
};

} // namespace

TEST_CASE_METHOD(SpoolsFx, "history's detail card lists the spools the job used",
                 "[plugin_filament][history]") {
    PrintHistoryJob job;
    job.job_id = "00002A";
    job.filename = "a.gcode";
    job.start_time = 1700000000.0;
    job.status = PrintJobStatus::COMPLETED;

    HistoryListPanel panel;
    panel.init_subjects();
    ui::HistoryListPanelTestAccess::show_detail_overlay(panel, test_screen(), job);
    drain();
    process_lvgl(10);
    drain();

    lv_subject_t* spools = lv_xml_get_subject(nullptr, "history_detail_spools");
    REQUIRE(spools != nullptr);
    const std::string text = lv_subject_get_string(spools);
    // One line for each of the record's two spools.
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);

    lv_obj_t* section = lv_obj_find_by_name(test_screen(), "spools_section");
    lv_obj_t* label = lv_obj_find_by_name(test_screen(), "spools_label");
    REQUIRE(section != nullptr);
    REQUIRE(label != nullptr);
    CHECK_FALSE(lv_obj_has_flag(section, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(label)) == text);

    // A tree deleted with its screen is created afresh on the next open.
    lv_obj_t* root = section;
    while (lv_obj_get_parent(root) != test_screen()) {
        root = lv_obj_get_parent(root);
    }
    lv_obj_delete(root);
    drain();
    ui::HistoryListPanelTestAccess::show_detail_overlay(panel, test_screen(), job);
    drain();
    CHECK(lv_obj_find_by_name(test_screen(), "spools_section") != nullptr);

    NavigationManager::instance().go_back();
    drain();
    process_lvgl(10);
}

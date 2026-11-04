// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_view_timelapse.cpp
 * @brief "View Timelapse" on a history record opens the timelapse browser.
 *
 * Run with: ./build/bin/helix-tests "[history][timelapse][overlay]"
 *
 * A toast naming the file is a control that does nothing
 * (prestonbrown/helixscreen#1373), so the button hands the file to
 * the timelapse browser, which owns player detection and playback; on a host
 * without a player the browser is still what opens.
 */

#include "ui_nav_manager.h"
#include "ui_overlay_timelapse_videos.h"
#include "ui_panel_history_list.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../ui_test_utils.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "static_panel_registry.h"
#include "timelapse_state.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HistoryListPanelTestAccess;

namespace {

PrintHistoryJob job(bool with_timelapse) {
    PrintHistoryJob j;
    j.job_id = "id-benchy";
    j.filename = "benchy.gcode";
    j.status = PrintJobStatus::COMPLETED;
    j.has_timelapse = with_timelapse;
    j.timelapse_filename = with_timelapse ? "benchy_20260907.mp4" : "";
    return j;
}

class HistoryTimelapseFixture : public LVGLUITestFixture {
  public:
    HistoryTimelapseFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        // Slot 0 of the navigation stack is the active main panel; an overlay
        // pushed over nothing would sit there and read as the base.
        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(helix::PanelId::Home)] = home_widget_;
        NavigationManager::instance().set_panels(panels);

        // The browser is a real overlay: its XML, the subjects it binds, and
        // the global instance open_timelapse_video() reaches for.
        lv_xml_register_component_from_file("A:ui_xml/overlay_panel.xml");
        lv_xml_register_component_from_file("A:ui_xml/components/progress_bar.xml");
        lv_xml_register_component_from_file("A:ui_xml/timelapse_videos_overlay.xml");
        helix::TimelapseState::instance().init_subjects();
        init_global_timelapse_videos(api());
    }

    ~HistoryTimelapseFixture() override {
        while (NavigationManager::instance().has_open_overlays()) {
            NavigationManager::instance().go_back();
            settle();
        }
        // The global overlay captured this fixture's API at init; the next case
        // builds its own.
        StaticPanelRegistry::instance().destroy_all();
        helix::TimelapseState::instance().deinit_subjects();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void settle() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static bool browser_open() {
        auto names = NavigationManager::instance().overlay_stack_names();
        return std::find(names.begin(), names.end(), "timelapse_videos_overlay") != names.end();
    }

    HistoryListPanel panel;

  private:
    lv_obj_t* home_widget_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(HistoryTimelapseFixture, "View Timelapse opens the timelapse browser",
                 "[history][timelapse][overlay][1373]") {
    REQUIRE(helix::ui::timelapse_viewer_available());
    HistoryListPanelTestAccess::select_job(panel, {job(true)}, 0);
    REQUIRE_FALSE(browser_open());

    HistoryListPanelTestAccess::handle_view_timelapse(panel);
    settle();

    CHECK(browser_open());
}

TEST_CASE_METHOD(HistoryTimelapseFixture, "View Timelapse without a video says so and stays put",
                 "[history][timelapse][overlay][1373]") {
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& message) { warnings.push_back(message); });
    HistoryListPanelTestAccess::select_job(panel, {job(false)}, 0);

    HistoryListPanelTestAccess::handle_view_timelapse(panel);
    settle();
    helix::ui::set_test_notification_warning_hook(nullptr);

    CHECK_FALSE(browser_open());
    CHECK(warnings.size() == 1);
}

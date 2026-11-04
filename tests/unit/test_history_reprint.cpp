// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_reprint.cpp
 * @brief Reprint hands Print Select the job it was asked for, whatever the
 *        navigation it triggers does to the history list.
 *
 * Run with: ./build/bin/helix-tests "[history][reprint]"
 *
 * Reprint queues three go_back()s and then switches to Print Select. Anything
 * that drains the UpdateQueue during that switch (a widget teardown, an overlay
 * destroy) runs those pops inline, and closing the history list clears its
 * search box, which re-filters and replaces filtered_jobs_. The job's filename
 * has to survive that.
 */

#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_panel_base.h"
#include "ui_panel_history_list.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../test_helpers/log_capture.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "static_panel_registry.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::HistoryListPanelTestAccess;

namespace {

// Long enough that std::string keeps it on the heap, so a read through a
// dangling job reads freed memory rather than an inline buffer.
const std::string kReprintFile = "reprint_target_with_a_name_long_enough_to_leave_sso.gcode";

PrintHistoryJob job(const std::string& filename) {
    PrintHistoryJob j;
    j.job_id = "id-" + filename;
    j.filename = filename;
    j.status = PrintJobStatus::COMPLETED;
    j.exists = true;
    return j;
}

/// Print Select stand-in whose activation drains the UpdateQueue, as a panel
/// switch does whenever it tears down a widget that flushes deferred work.
class DrainingPanel : public PanelBase {
  public:
    DrainingPanel() : PanelBase(get_printer_state(), nullptr) {}

    void init_subjects() override {}
    const char* get_name() const override {
        return "DrainingPanel";
    }
    const char* get_xml_component_name() const override {
        return "draining_panel";
    }
    void on_activate() override {
        helix::ui::UpdateQueue::instance().drain();
    }
};

void forward_search_changed(lv_event_t* e) {
    auto* panel = static_cast<HistoryListPanel*>(lv_event_get_user_data(e));
    HistoryListPanelTestAccess::on_search_changed(*panel);
}

class HistoryReprintFixture : public LVGLUITestFixture {
  public:
    HistoryReprintFixture() {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);
        // A fresh Print Select has no file list yet, so select_file_by_name()
        // records the name as the pending selection.
        StaticPanelRegistry::instance().destroy_all();

        auto& nav = NavigationManager::instance();
        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        nav.set_panels(panels);
        nav.register_panel_instance(PanelId::PrintSelect, &print_select_);

        panel.init_subjects();

        // Stack as the user has it: Home, the history list, its detail overlay.
        list_root_ = lv_obj_create(test_screen());
        detail_root_ = lv_obj_create(test_screen());
        nav.register_overlay_instance(list_root_, &panel);
        nav.register_overlay_instance(detail_root_, nullptr);
        nav.push_overlay(list_root_);
        drain();
        HistoryListPanelTestAccess::set_detail_overlay_open(panel, true);
        nav.push_overlay(detail_root_);
        drain();

        search_box_ = lv_textarea_create(list_root_);
        lv_obj_add_event_cb(search_box_, forward_search_changed, LV_EVENT_VALUE_CHANGED, &panel);
        HistoryListPanelTestAccess::set_search_box(panel, search_box_);
    }

    ~HistoryReprintFixture() override {
        auto& nav = NavigationManager::instance();
        nav.register_panel_instance(PanelId::PrintSelect, nullptr);
        nav.unregister_overlay_instance(list_root_);
        nav.unregister_overlay_instance(detail_root_);
        HistoryListPanelTestAccess::set_search_box(panel, nullptr);
        drain();
        StaticPanelRegistry::instance().destroy_all();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    HistoryListPanel panel;

  private:
    DrainingPanel print_select_;
    lv_obj_t* home_widget_ = nullptr;
    lv_obj_t* list_root_ = nullptr;
    lv_obj_t* detail_root_ = nullptr;
    lv_obj_t* search_box_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(HistoryReprintFixture,
                 "Reprint selects the job's file even when navigation re-filters the list",
                 "[history][reprint]") {
    HistoryListPanelTestAccess::set_jobs(panel, {job("other.gcode"), job(kReprintFile)});
    HistoryListPanelTestAccess::select_job(panel, {job("other.gcode"), job(kReprintFile)}, 1);
    const PrintHistoryJob* jobs_before = HistoryListPanelTestAccess::filtered_jobs(panel).data();

    helix::LogCapture logs;
    HistoryListPanelTestAccess::handle_reprint(panel);

    // Premise: the switch closed the list and its search reset replaced the
    // vector the selected job lived in.
    REQUIRE(HistoryListPanelTestAccess::filtered_jobs(panel).data() != jobs_before);

    CHECK(logs.has_line_with({"Set pending file selection: '" + kReprintFile + "'"}));
    CHECK(logs.has_line_with({"Navigated to file details for: " + kReprintFile}));
    drain();
}

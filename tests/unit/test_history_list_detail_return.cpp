// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_list_detail_return.cpp
 * @brief The history list is still there when its detail overlay closes
 *
 * Opening a job's detail covers the list, and NavigationManager deactivates the
 * list for it. Back from the detail reactivates the same list: the rows, the
 * scroll position and the filters the user left must all be on screen.
 *
 * Run with: ./build/bin/helix-tests "[history][detail_return]"
 */

#include "ui_nav.h"
#include "ui_nav_manager.h"
#include "ui_panel_history_list.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "print_history_manager.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::HistoryScope;
using ManagerAccess = helix::PrintHistoryManagerTestAccess;
using ListAccess = helix::ui::HistoryListPanelTestAccess;

namespace {

std::vector<PrintHistoryJob> jobs_newest_first(int count) {
    std::vector<PrintHistoryJob> out;
    for (int i = 0; i < count; ++i) {
        PrintHistoryJob j;
        j.job_id = "id" + std::to_string(i);
        j.filename = "part" + std::to_string(i) + ".gcode";
        j.status = PrintJobStatus::COMPLETED;
        j.start_time = 1'000'000'000.0 - i * 3600.0;
        out.push_back(j);
    }
    return out;
}

int visible_rows(lv_obj_t* rows) {
    int n = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(rows); ++i) {
        lv_obj_t* child = lv_obj_get_child(rows, static_cast<int32_t>(i));
        const char* name = lv_obj_get_name(child);
        if (name && std::strcmp(name, "history_row") == 0 && !lv_obj_is_hidden(child)) {
            ++n;
        }
    }
    return n;
}

class DetailReturnFixture : public LVGLUITestFixture {
  public:
    DetailReturnFixture() : manager(nullptr, nullptr) {
        ManagerAccess::set_loaded_jobs(manager, jobs_newest_first(40), HistoryScope::COMPLETE, 40);
        panel.init_subjects();
        content = lv_obj_create(test_screen());
        lv_obj_set_size(content, 400, 300);
        rows = lv_obj_create(content);
        lv_obj_set_width(rows, LV_PCT(100));
        lv_obj_set_height(rows, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        ListAccess::set_list_containers(panel, content, rows);
        ListAccess::set_history_manager(panel, &manager);

        // A fresh open from the dashboard, as show() runs it.
        ListAccess::before_show(panel);
        panel.on_activate();
        lv_obj_update_layout(content);
    }

    ~DetailReturnFixture() override {
        panel.on_deactivate(DeactivateReason::Shutdown);
        ListAccess::set_history_manager(panel, nullptr);
        lv_obj_delete(content);
    }

    /// What a row tap and NavigationManager's push of the detail do to the list.
    void open_detail() {
        ListAccess::set_detail_overlay_open(panel, true);
        panel.on_deactivate(DeactivateReason::NavigateAway);
    }

    PrintHistoryManager manager;
    HistoryListPanel panel;
    lv_obj_t* content = nullptr;
    lv_obj_t* rows = nullptr;
};

} // namespace

TEST_CASE_METHOD(DetailReturnFixture, "back from a history detail shows the list where it was",
                 "[history][detail_return]") {
    REQUIRE(visible_rows(rows) > 0);
    lv_obj_scroll_to_y(content, 200, LV_ANIM_OFF);
    lv_obj_update_layout(content);
    const int32_t reading_at = lv_obj_get_scroll_y(content);
    REQUIRE(reading_at > 0);

    open_detail();
    panel.on_activate();
    lv_obj_update_layout(content);

    CHECK(visible_rows(rows) > 0);
    CHECK(lv_obj_get_scroll_y(content) == reading_at);
    CHECK(ListAccess::filtered_jobs(panel).size() == 40);
}

TEST_CASE_METHOD(DetailReturnFixture,
                 "history that changes under a detail is on the list it returns to",
                 "[history][detail_return]") {
    open_detail();
    // A job finishes while the user reads the detail.
    ManagerAccess::set_loaded_jobs(manager, jobs_newest_first(41), HistoryScope::COMPLETE, 41);
    REQUIRE(ListAccess::filtered_jobs(panel).size() == 40);

    panel.on_activate();
    lv_obj_update_layout(content);

    CHECK(ListAccess::filtered_jobs(panel).size() == 41);
    CHECK(visible_rows(rows) > 0);
}

TEST_CASE_METHOD(DetailReturnFixture,
                 "a list closed under its detail opens populated from the dashboard",
                 "[history][detail_return]") {
    // The user narrowed, re-sorted and scrolled the list before opening a detail.
    ListAccess::set_view(panel, "part", HistoryStatusFilter::COMPLETED, HistorySortColumn::DATE,
                         HistorySortDirection::ASC);
    lv_obj_update_layout(content);
    lv_obj_scroll_to_y(content, 200, LV_ANIM_OFF);
    lv_obj_update_layout(content);
    REQUIRE(lv_obj_get_scroll_y(content) > 0);
    REQUIRE(ListAccess::filtered_jobs(panel).front().job_id == "id39");

    open_detail();
    // A nav-bar switch tears the stack down with the detail still on top, so
    // the list is never activated by a Back.
    panel.on_deactivate(DeactivateReason::NavigateAway);

    ListAccess::before_show(panel);
    panel.on_activate();
    lv_obj_update_layout(content);

    // A fresh open starts from the defaults, at the top, newest first.
    CHECK(visible_rows(rows) > 0);
    CHECK(lv_obj_get_scroll_y(content) == 0);
    REQUIRE(ListAccess::filtered_jobs(panel).size() == 40);
    CHECK(ListAccess::filtered_jobs(panel).front().job_id == "id0");
}

namespace {

bool holds_job(const std::vector<PrintHistoryJob>& jobs, const std::string& job_id) {
    return std::any_of(jobs.begin(), jobs.end(),
                       [&job_id](const PrintHistoryJob& j) { return j.job_id == job_id; });
}

/// The list and its detail on the real navigation stack, with a mock printer
/// that answers server.history.delete_job, so the detail's Delete runs the
/// same go_back() and re-activation of the list that a user's tap does.
class DeleteFromDetailFixture : public LVGLUITestFixture {
  public:
    DeleteFromDetailFixture() : manager(nullptr, nullptr) {
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(client, get_printer_state());
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());

        ManagerAccess::set_loaded_jobs(manager, jobs_newest_first(40), HistoryScope::COMPLETE, 40);
        panel.init_subjects();

        auto& nav = NavigationManager::instance();
        home_widget_ = lv_obj_create(test_screen());
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_widget_;
        nav.set_panels(panels);

        list_root_ = lv_obj_create(test_screen());
        lv_obj_set_size(list_root_, 400, 300);
        rows_ = lv_obj_create(list_root_);
        lv_obj_set_width(rows_, LV_PCT(100));
        lv_obj_set_height(rows_, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rows_, LV_FLEX_FLOW_COLUMN);
        ListAccess::set_list_containers(panel, list_root_, rows_);
        ListAccess::set_history_manager(panel, &manager);

        detail_root_ = lv_obj_create(test_screen());
        nav.register_overlay_instance(list_root_, &panel);
        nav.register_overlay_instance(detail_root_, nullptr);
        ListAccess::before_show(panel);
        nav.push_overlay(list_root_);
        drain();
        ListAccess::set_detail_overlay_open(panel, true);
        nav.push_overlay(detail_root_);
        drain();
    }

    ~DeleteFromDetailFixture() override {
        auto& nav = NavigationManager::instance();
        nav.go_back();
        drain();
        nav.unregister_overlay_instance(list_root_);
        nav.unregister_overlay_instance(detail_root_);
        panel.on_deactivate(DeactivateReason::Shutdown);
        ListAccess::set_history_manager(panel, nullptr);
        ListAccess::set_list_containers(panel, nullptr, nullptr);
        drain();
        set_moonraker_api(previous_api_);
        api.reset();
        client.stop_temperature_simulation();
        client.disconnect();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Select the row with `job_id`, as tapping it before opening the detail does.
    void select(const std::string& job_id) {
        std::vector<PrintHistoryJob> shown = ListAccess::filtered_jobs(panel);
        auto it = std::find_if(shown.begin(), shown.end(),
                               [&job_id](const PrintHistoryJob& j) { return j.job_id == job_id; });
        REQUIRE(it != shown.end());
        const auto index = static_cast<size_t>(it - shown.begin());
        ListAccess::select_job(panel, std::move(shown), index);
    }

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::unique_ptr<MoonrakerAPI> api;
    PrintHistoryManager manager;
    HistoryListPanel panel;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
    lv_obj_t* home_widget_ = nullptr;
    lv_obj_t* list_root_ = nullptr;
    lv_obj_t* rows_ = nullptr;
    lv_obj_t* detail_root_ = nullptr;
    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(DeleteFromDetailFixture,
                 "a job deleted from its detail stays gone when history changed meanwhile",
                 "[history][detail_return]") {
    // A job finishes while the user reads the detail, so Back takes the full
    // reload from the manager's cache.
    ManagerAccess::set_loaded_jobs(manager, jobs_newest_first(41), HistoryScope::COMPLETE, 41);
    select("id5");

    ListAccess::confirm_delete(panel);
    drain();

    CHECK_FALSE(holds_job(ListAccess::filtered_jobs(panel), "id5"));
    CHECK(ListAccess::filtered_jobs(panel).size() == 40);
    // Nothing that reads the manager's cache later brings it back either.
    CHECK_FALSE(holds_job(manager.get_jobs(), "id5"));
}

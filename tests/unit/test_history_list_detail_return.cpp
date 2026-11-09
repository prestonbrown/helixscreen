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

#include "ui_panel_history_list.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "print_history_manager.h"

#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::HistoryScope;
using helix::PrintHistoryManagerTestAccess;
using helix::ui::HistoryListPanelTestAccess;

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
        if (name && std::strcmp(name, "history_row") == 0 &&
            !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            ++n;
        }
    }
    return n;
}

class DetailReturnFixture : public LVGLUITestFixture {
  public:
    DetailReturnFixture() : manager(nullptr, nullptr) {
        PrintHistoryManagerTestAccess::set_loaded_jobs(manager, jobs_newest_first(40),
                                                       HistoryScope::COMPLETE, 40);
        panel.init_subjects();
        content = lv_obj_create(test_screen());
        lv_obj_set_size(content, 400, 300);
        rows = lv_obj_create(content);
        lv_obj_set_width(rows, LV_PCT(100));
        lv_obj_set_height(rows, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
        HistoryListPanelTestAccess::set_list_containers(panel, content, rows);
        HistoryListPanelTestAccess::set_history_manager(panel, &manager);

        // A fresh open from the dashboard, as show() runs it.
        HistoryListPanelTestAccess::before_show(panel);
        panel.on_activate();
        lv_obj_update_layout(content);
    }

    ~DetailReturnFixture() override {
        panel.on_deactivate(DeactivateReason::Shutdown);
        HistoryListPanelTestAccess::set_history_manager(panel, nullptr);
        lv_obj_delete(content);
    }

    /// What a row tap and NavigationManager's push of the detail do to the list.
    void open_detail() {
        HistoryListPanelTestAccess::set_detail_overlay_open(panel, true);
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
    CHECK(HistoryListPanelTestAccess::filtered_jobs(panel).size() == 40);
}

TEST_CASE_METHOD(DetailReturnFixture,
                 "history that changes under a detail is on the list it returns to",
                 "[history][detail_return]") {
    open_detail();
    // A job finishes while the user reads the detail.
    PrintHistoryManagerTestAccess::set_loaded_jobs(manager, jobs_newest_first(41),
                                                   HistoryScope::COMPLETE, 41);
    REQUIRE(HistoryListPanelTestAccess::filtered_jobs(panel).size() == 40);

    panel.on_activate();
    lv_obj_update_layout(content);

    CHECK(HistoryListPanelTestAccess::filtered_jobs(panel).size() == 41);
    CHECK(visible_rows(rows) > 0);
}

TEST_CASE_METHOD(DetailReturnFixture,
                 "a list closed under its detail opens populated from the dashboard",
                 "[history][detail_return]") {
    open_detail();
    // A nav-bar switch tears the stack down with the detail still on top, so
    // the list is never activated by a Back.
    panel.on_deactivate(DeactivateReason::NavigateAway);

    HistoryListPanelTestAccess::before_show(panel);
    panel.on_activate();
    lv_obj_update_layout(content);

    CHECK(visible_rows(rows) > 0);
    CHECK(HistoryListPanelTestAccess::filtered_jobs(panel).size() == 40);
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_history_list_paging_scroll.cpp
 * @brief A page of older history keeps the list where the user is reading
 *
 * Scrolling to the bottom of the history list pages older jobs into the shared
 * cache, and the cache's observer repopulates the list. That repopulation must
 * keep the scroll position: the user is at the bottom, waiting for more.
 */

#include "ui_panel_history_list.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "../test_helpers/print_history_manager_test_access.h"
#include "core/update_queue.h"
#include "print_history_manager.h"

#include <memory>
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

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a history refresh from the shared cache keeps the scroll",
                 "[history][paging]") {
    PrintHistoryManager manager(nullptr, nullptr);
    PrintHistoryManagerTestAccess::set_loaded_jobs(manager, jobs_newest_first(40),
                                                   HistoryScope::COMPLETE, 40);

    HistoryListPanel panel;
    panel.init_subjects();
    lv_obj_t* content = lv_obj_create(test_screen());
    lv_obj_set_size(content, 400, 300);
    lv_obj_t* rows = lv_obj_create(content);
    lv_obj_set_width(rows, LV_PCT(100));
    lv_obj_set_height(rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    HistoryListPanelTestAccess::set_list_containers(panel, content, rows);
    HistoryListPanelTestAccess::set_history_manager(panel, &manager);

    HistoryListPanelTestAccess::refresh_from_manager(panel);
    lv_obj_update_layout(content);
    lv_obj_scroll_to_y(content, lv_obj_get_scroll_bottom(content), LV_ANIM_OFF);
    lv_obj_update_layout(content);
    const int32_t reading_at = lv_obj_get_scroll_y(content);
    REQUIRE(reading_at > 0);

    // An older page lands in the cache.
    PrintHistoryManagerTestAccess::set_loaded_jobs(manager, jobs_newest_first(90),
                                                   HistoryScope::COMPLETE, 90);
    HistoryListPanelTestAccess::refresh_from_manager(panel);
    lv_obj_update_layout(content);

    CHECK(lv_obj_get_scroll_y(content) == reading_at);

    HistoryListPanelTestAccess::set_history_manager(panel, nullptr);
    lv_obj_delete(content);
}

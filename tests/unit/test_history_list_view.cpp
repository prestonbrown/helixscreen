// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_history_list_view.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// Container-replacement coverage for the #1396 fix as it applies to
// HistoryListView. The mechanism and the reasoning are the twin of
// test_spoolman_list_view.cpp's hot-reload cases; they exist per class because
// the pool members are per class — a fix that lands in one view and not
// another reverts green here.

static std::vector<PrintHistoryJob> make_test_jobs(int count) {
    std::vector<PrintHistoryJob> jobs;
    jobs.reserve(count);
    for (int i = 0; i < count; i++) {
        PrintHistoryJob job;
        job.job_id = "job-" + std::to_string(i);
        job.filename = "part_" + std::to_string(i) + ".gcode";
        job.status = PrintJobStatus::COMPLETED;
        job.print_duration = 600.0 + i;
        job.exists = true;
        jobs.push_back(job);
    }
    return jobs;
}

static lv_obj_t* make_scroll_container(lv_obj_t* screen) {
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 400, 600);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    return container;
}

TEST_CASE_METHOD(LVGLUITestFixture, "HistoryListView - container deletion drops the cached pool",
                 "[history_list_view][ui_integration][hot-reload]") {
    HistoryListView view;
    lv_obj_t* container_a = make_scroll_container(test_screen());
    view.setup(container_a, nullptr, [](size_t) {});

    view.populate(make_test_jobs(10));
    process_lvgl(50);
    REQUIRE(view.is_initialized() == true);
    REQUIRE(lv_obj_get_child_count(container_a) > 0); // rows + spacers

    // PanelBase::rebuild() deletes the tree while the owning panel survives.
    lv_obj_delete(container_a);

    REQUIRE(view.is_initialized() == false);
    REQUIRE(view.container() == nullptr);

    lv_obj_t* container_b = make_scroll_container(test_screen());
    view.setup(container_b, nullptr, [](size_t) {});
    view.populate(make_test_jobs(10));
    process_lvgl(50);
    REQUIRE(view.is_initialized() == true);
    REQUIRE(lv_obj_get_child_count(container_b) > 0);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "HistoryListView - late deletion of a replaced container keeps the new pool",
                 "[history_list_view][ui_integration][hot-reload]") {
    // The rebuild path frees the old subtree DEFERRED (safe_delete_subtree →
    // safe_delete_deferred), so the old container's LV_EVENT_DELETE can land
    // after setup() already re-pointed the view at the replacement.
    HistoryListView view;
    lv_obj_t* container_a = make_scroll_container(test_screen());
    view.setup(container_a, nullptr, [](size_t) {});
    view.populate(make_test_jobs(10));
    process_lvgl(50);

    lv_obj_t* container_b = make_scroll_container(test_screen());
    view.setup(container_b, nullptr, [](size_t) {});
    view.populate(make_test_jobs(10));
    process_lvgl(50);
    REQUIRE(view.is_initialized() == true);
    const uint32_t children_b = lv_obj_get_child_count(container_b);
    REQUIRE(children_b > 0);

    lv_obj_delete(container_a);
    process_lvgl(50);

    REQUIRE(view.is_initialized() == true);
    REQUIRE(view.container() == container_b);
    REQUIRE(lv_obj_get_child_count(container_b) == children_b);
}

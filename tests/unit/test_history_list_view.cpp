// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_history_list_view.h"
#include "ui_nav_manager.h"
#include "ui_panel_history_list.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/history_list_panel_test_access.h"
#include "data_root_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "thumbnail_cache.h"
#include "translation_loader.h"

#include <filesystem>
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

namespace {

// LVGL has no pack-unregister API, so the language goes back to the identity
// locale, whose lookups return the tag itself.
struct ScopedGerman {
    ScopedGerman() {
        helix::ui::ensure_translation_loaded("de");
        lv_translation_set_language("de");
    }
    ~ScopedGerman() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedGerman(const ScopedGerman&) = delete;
    ScopedGerman& operator=(const ScopedGerman&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "HistoryListView - a row's status and filament read in the UI language",
                 "[history_list_view][history][translation]") {
    ScopedGerman german;
    REQUIRE(std::string(lv_tr("Completed")) != "Completed"); // the pack loaded

    HistoryListView view;
    lv_obj_t* container = make_scroll_container(test_screen());
    view.setup(container, nullptr, [](size_t) {});
    view.populate(make_test_jobs(3));
    process_lvgl(50);

    lv_obj_t* status = lv_obj_find_by_name(container, "row_status");
    REQUIRE(status != nullptr);
    REQUIRE(std::string(lv_label_get_text(status)) == lv_tr("Completed"));

    // The test jobs carry no filament type.
    lv_obj_t* filament = lv_obj_find_by_name(container, "row_filament");
    REQUIRE(filament != nullptr);
    REQUIRE(std::string(lv_label_get_text(filament)) == lv_tr("Unknown"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "HistoryListPanel - the detail status reads in the UI language",
                 "[history][translation]") {
    ScopedGerman german;

    HistoryListPanel panel;
    panel.init_subjects();

    PrintHistoryJob job;
    job.filename = "benchy.gcode";
    job.status = PrintJobStatus::ERROR;
    helix::ui::HistoryListPanelTestAccess::update_detail_subjects(panel, job);

    lv_subject_t* status = lv_xml_get_subject(nullptr, "history_detail_status");
    REQUIRE(status != nullptr);
    REQUIRE(std::string(lv_subject_get_string(status)) == lv_tr("Failed"));
    REQUIRE(std::string(lv_tr("Failed")) != "Failed");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "HistoryListPanel - the detail thumbnail path is relative to the gcodes root",
                 "[history][subdir]") {
    const bool in_subdir = GENERATE(false, true);
    PrintHistoryJob job;
    job.filename = in_subdir ? "sub/dir/DetailThumbProbe.gcode" : "DetailThumbProbe.gcode";
    job.status = PrintJobStatus::COMPLETED;
    job.thumbnail_path = ".thumbs/DetailThumbProbe.png";
    const std::string key =
        in_subdir ? "sub/dir/.thumbs/DetailThumbProbe.png" : ".thumbs/DetailThumbProbe.png";

    // A cached PNG under exactly one key: the overlay shows it only when it
    // asks for that key.
    auto& cache = get_thumbnail_cache();
    std::filesystem::remove(cache.get_cache_path(".thumbs/DetailThumbProbe.png"));
    std::filesystem::remove(cache.get_cache_path("sub/dir/.thumbs/DetailThumbProbe.png"));
    const std::string planted = cache.get_cache_path(key);
    std::filesystem::copy_file(helix::asset_path("assets/images/folder.png"), planted,
                               std::filesystem::copy_options::overwrite_existing);
    const std::string expected = cache.get_if_cached(key);
    REQUIRE_FALSE(expected.empty());

    {
        HistoryListPanel panel;
        panel.init_subjects();
        helix::ui::HistoryListPanelTestAccess::show_detail_overlay(panel, test_screen(), job);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
        helix::ui::UpdateQueue::instance().drain();

        lv_obj_t* image = lv_obj_find_by_name(test_screen(), "thumbnail_image");
        REQUIRE(image != nullptr);
        const void* src = lv_image_get_src(image);
        REQUIRE(src != nullptr);
        REQUIRE(std::string(static_cast<const char*>(src)) == expected);

        NavigationManager::instance().go_back();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }
    std::filesystem::remove(planted);
}

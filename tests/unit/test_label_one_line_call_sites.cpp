// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_label_one_line_call_sites.cpp
 * @brief Dotted labels built in C++ hold one line (prestonbrown/helixscreen#1775).
 *
 * Each widget here creates its own lv_label in DOTS mode; without a pinned
 * height the label wraps instead of ellipsizing.
 */

#include "ui_context_menu.h"
#include "ui_step_progress.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/job_queue_subjects_fixture.h"
#include "../test_helpers/one_line_label.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "job_queue_state.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "printer_fan_state.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/fan_widget.h"
#include "src/ui/panel_widgets/job_queue_widget.h"

#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::find_dotted_label;
using helix::test::held_to_one_line;

TEST_CASE_METHOD(LVGLUITestFixture, "job queue tile holds the filename to one line",
                 "[one_line][job_queue]") {
    PanelWidgetManager::instance().init_widget_subjects();
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard;
    jqs.init_subjects();
    set_job_queue_state(&jqs);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_job_queue"), 1);
    JobQueueStatus status;
    status.queue_state = "ready";
    status.queued_jobs.push_back(
        {"job-1", "an_extremely_long_queued_filename_that_cannot_fit_in_any_home_tile_v12.gcode",
         1000.0, 30.0});
    JobQueueStateTestAccess::deliver_status(jqs, status);

    {
        PanelWidgetHarness<JobQueueWidget> h(test_screen());
        h.resize(1, 1, 114, 54);
        process_lvgl(30);
        lv_obj_t* hero = h.child("jq_hero");
        REQUIRE(hero != nullptr);
        CHECK(held_to_one_line(hero));
    }
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_job_queue"), 0);
    set_job_queue_state(nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "step progress caption holds one line",
                 "[one_line][step_caption]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_size(parent, 264, 120);
    std::array<ui_step_t, 2> steps = {
        {{"Heat", StepState::Active},
         {"Purge a very long distance before the swap", StepState::Pending}}};
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    ui_step_progress_set_current_label_only(widget, true);
    lv_obj_t* caption = ui_step_progress_get_caption(widget);
    REQUIRE(caption != nullptr);
    CHECK(lv_label_get_long_mode(caption) == LV_LABEL_LONG_MODE_DOTS);
    CHECK(held_to_one_line(caption));
}

namespace {

/// Replaces the global printer's fans for one test and puts the old set back.
struct ScopedFans {
    std::vector<std::string> saved;
    explicit ScopedFans(const std::vector<std::string>& names) {
        for (const auto& f : get_printer_state().fan_state().get_fans())
            saved.push_back(f.object_name);
        get_printer_state().fan_state().init_fans(names);
    }
    ~ScopedFans() {
        get_printer_state().fan_state().init_fans(saved);
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "fan widget picker rows hold the fan name to one line",
                 "[one_line][fan]") {
    helix::ui::ContextMenu::register_shared_callbacks();
    REQUIRE(register_component("fan_picker"));
    ScopedFans fans({"fan_generic an_extremely_long_auxiliary_cooling_fan_name_for_the_chamber"});

    FanWidget widget("fan");
    lv_obj_t* tile = lv_obj_create(test_screen());
    widget.attach(tile, test_screen());
    widget.on_edit_configure();

    lv_obj_t* card = lv_obj_find_by_name(test_screen(), "context_menu");
    REQUIRE(card != nullptr);
    lv_obj_t* list = lv_obj_find_by_name(card, "fan_list");
    REQUIRE(list != nullptr);
    REQUIRE(lv_obj_get_child_count(list) >= 1);
    lv_obj_t* name = find_dotted_label(lv_obj_get_child(list, 0));
    REQUIRE(name != nullptr);
    CHECK(held_to_one_line(name));

    widget.detach();
    helix::ui::UpdateQueue::instance().drain();
}

// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_count_subject.cpp
 * @brief `job_queue_count` is the only refresh channel the three queue
 * surfaces have, and for a long time nothing registered it (#1349).
 *
 * Four sites resolve the subject by name and attach an observer to it:
 * `job_queue_widget.cpp:95` (home panel widget), `ui_job_queue_modal.cpp:115`
 * (the modal), and `print_status_widget.cpp:328`/`:730` (the print-status
 * widget's "Job Queue" library row). `JobQueueState::init_subjects()`
 * registered only the two *text* subjects, so `lv_xml_get_subject()` returned
 * null at every one of them, no observer was ever attached, and each of those
 * surfaces was frozen:
 *
 *  - `JobQueueWidget::rebuild_job_list()` is reachable ONLY from
 *    `on_size_changed()` and the count observer. `on_activate()` fires an
 *    async `fetch()` with no completion hook of its own, so the queue list
 *    never picked up fetched data until something resized the widget.
 *  - `JobQueueModal::remove_job()` calls `fetch()` and nothing else — its
 *    comment says "count observer will auto-rebuild the list" — so a deleted
 *    job stayed on screen.
 *  - `PrintStatusWidget::update_job_queue_row_visibility()` derives `has_jobs`
 *    from `lv_subject_get_int()` on that same lookup. Null subject means
 *    `has_jobs == false` unconditionally, so the queue row was permanently
 *    hidden no matter what the user configured.
 *
 * There was no substitute path for any of the three, which is why the fix is
 * to register the subject rather than delete the reads.
 *
 * The assertions below go past "the subject exists": after one resize to
 * settle the widget into list-showing mode, every later list rebuild is driven
 * purely by the subject moving. `deliver_status()` goes through
 * `on_queue_fetched()` -> `update_subjects()`, the same path a real Moonraker
 * response takes, so a fix that registered the subject but forgot to publish
 * to it would still fail here.
 *
 * Teardown order matters and is why the guard is declared before the harness:
 * `~PanelWidgetHarness` must detach the widget (releasing its observer) BEFORE
 * `deinit_one()` deinits the subject it observes. The guard also un-registers
 * all three names from helix-xml's global scope — `JobQueueState` is
 * process-lifetime in production and never does, so leaving a local instance's
 * subjects in that table would dangle for the rest of the binary (the same
 * trap documented at length in test_widget_size_print_status.cpp).
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/job_queue_widget.h"
#include "static_subject_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

/// Installs a JobQueueState on the get_job_queue_state() global for the scope
/// of a test — the pointer is a shared static in the test binary
/// (tests/ui_test_utils.cpp), so a leaked pointer to a local would dangle for
/// whatever test runs next.
struct ScopedJobQueueState {
    explicit ScopedJobQueueState(JobQueueState* state) {
        set_job_queue_state(state);
    }
    ~ScopedJobQueueState() {
        set_job_queue_state(nullptr);
    }
    ScopedJobQueueState(const ScopedJobQueueState&) = delete;
    ScopedJobQueueState& operator=(const ScopedJobQueueState&) = delete;
};

/// Runs JobQueueState's registered deinit while the instance is still alive,
/// then drops all three subject names out of helix-xml's global scope so no
/// later test resolves a pointer into this test's stack frame.
///
/// deinit_one() rather than deinit_all(): the test binary's registry also
/// holds entries left by earlier fixtures, some capturing destroyed objects.
struct ScopedJobQueueSubjects {
    ~ScopedJobQueueSubjects() {
        StaticSubjectRegistry::instance().deinit_one("JobQueueState");
        lv_xml_unregister_subject(nullptr, "job_queue_count");
        lv_xml_unregister_subject(nullptr, "job_queue_summary_text");
        lv_xml_unregister_subject(nullptr, "job_queue_state_text");
    }
};

JobQueueStatus status_with(int n) {
    JobQueueStatus s;
    s.queue_state = "ready";
    for (int i = 0; i < n; ++i) {
        s.queued_jobs.push_back({"job-" + std::to_string(i), "file-" + std::to_string(i) + ".gcode",
                                 1000.0 + i, 30.0 * (i + 1)});
    }
    return s;
}

int container_children(lv_obj_t* c) {
    return static_cast<int>(lv_obj_get_child_count(c));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "job_queue_count is registered and drives the queue list rebuild",
                 "[1349][job_queue][subjects]") {
    PanelWidgetManager::instance().init_widget_subjects();

    // JobQueueState guards null api_/client_ in both fetch() (never reached
    // here) and subscribe_to_notifications(), so nullptr/nullptr is safe.
    JobQueueState jqs(nullptr, nullptr);
    ScopedJobQueueSubjects subject_guard; // destroyed AFTER the harness below
    jqs.init_subjects();

    // --- The registration itself. Before #1349 this returned null, which is
    // what left every consumer's observer unattached.
    lv_subject_t* count = lv_xml_get_subject(nullptr, "job_queue_count");
    REQUIRE(count != nullptr);
    CHECK(lv_subject_get_int(count) == 0);

    lv_subject_t* summary = lv_xml_get_subject(nullptr, "job_queue_summary_text");
    REQUIRE(summary != nullptr);

    ScopedJobQueueState scoped_state(&jqs);
    PanelWidgetHarness<JobQueueWidget> h(test_screen());

    lv_obj_t* container = h.child("job_list_container");
    REQUIRE(container != nullptr);

    // One resize, to put the widget in a mode that shows the list at all
    // (mode 1). Every assertion after this point is about the subject driving
    // a rebuild — resize is never called again.
    h.resize(1, 1, w_normal(), h_tall());
    process_lvgl(50);
    REQUIRE(container_children(container) == 0);

    // --- A queue response arrives. update_subjects() must publish the count,
    // and the widget's observer must rebuild off it with no resize.
    JobQueueStateTestAccess::deliver_status(jqs, status_with(3));
    CHECK(lv_subject_get_int(count) == 3);
    CHECK(std::string(lv_subject_get_string(summary)) == "3 jobs queued");

    REQUIRE(wait_until([&] { return container_children(container) == 3; }, 3000));

    // --- A job is removed (JobQueueModal::remove_job's real path: fetch, then
    // let the count observer repopulate). The list must shrink on its own.
    JobQueueStateTestAccess::deliver_status(jqs, status_with(1));
    CHECK(lv_subject_get_int(count) == 1);
    CHECK(std::string(lv_subject_get_string(summary)) == "1 job queued");

    REQUIRE(wait_until([&] { return container_children(container) == 1; }, 3000));

    // --- Emptied queue: back to zero rows, and the empty-state label returns.
    JobQueueStateTestAccess::deliver_status(jqs, status_with(0));
    CHECK(lv_subject_get_int(count) == 0);

    REQUIRE(wait_until([&] { return container_children(container) == 0; }, 3000));

    lv_obj_t* empty_state = h.child("jq_empty_state");
    REQUIRE(empty_state != nullptr);
    CHECK_FALSE(lv_obj_has_flag(empty_state, LV_OBJ_FLAG_HIDDEN));
}

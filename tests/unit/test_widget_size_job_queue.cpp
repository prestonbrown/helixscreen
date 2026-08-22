// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_job_queue.cpp
 * @brief job_queue picks its 3-way mode from physical pixels on two
 * independent axes (width, height), not colspan/rowspan — and, unlike most
 * migrated widgets, `on_size_changed` triggers a *full child rebuild* of
 * `job_list_container` on every call (no memo, no `widget_obj_` guard).
 *
 * The old predicate required a 3-span on *either* axis for "expanded" (mode
 * 2), given both axes already clear the 2-span floor. `w_wide()` is the width
 * analogue of that 3-span floor, but there was no height analogue among the
 * three widget_size.h constants — the measured tier table only
 * derives a rowspan>=2 threshold (`h_tall()`), not rowspan>=3. The smallest
 * measured 3-row height across all eight tiers is 197.5px (Micro
 * 480x272).
 *
 * `h_taller()` (Small rung 197, just below that measured minimum) is the
 * height counterpart to `w_wide()` in panel_widget_size.h, following the same
 * ">= admits the smallest measured extent" rule as the other three bands — the
 * option that preserves the OR-of-two-axes behavior. Reusing `w_wide()` (Small
 * rung 205) for the height bound would drop Micro's 3-row case out of mode 2 —
 * a real regression, not a
 * cosmetic threshold shift. Leaving mode 1 unbounded on height would drop
 * the height-only expansion path entirely — a tall-but-narrow queue that
 * reaches mode 2 today via rowspan alone would be stuck in mode 1.
 *
 * Four cases isolate the 3-way, 2-axis-OR predicate: mode 0 (either axis
 * below floor), mode 1 (both axes in the middle band), mode 2 via width
 * alone (isolates the width term of the OR), mode 2 via height alone
 * (isolates the height term). Each pairs its target pixels with a
 * colspan/rowspan the *old* span-based predicate would resolve to a
 * *different* mode, so an implementation that still reads spans fails here
 * instead of passing by coincidence.
 *
 * Every case also asserts `job_list_container`'s hidden flag (the visible
 * effect XML binds off `jq_size_mode` via `bind_flag_if_eq`,
 * panel_widget_job_queue.xml:32) AND its child count after seeding three
 * queued jobs via `JobQueueStateTestAccess` — proving the rebuild actually
 * runs, not just that the subject moved. A subject-only test would pass
 * even if `rebuild_job_list()` silently no-op'd.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/job_queue_state_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "app_globals.h"
#include "job_queue_state.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/job_queue_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

int jq_size_mode() {
    auto* subject = lv_xml_get_subject(nullptr, "jq_size_mode");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

/// Installs a JobQueueState on the get_job_queue_state() global for the
/// scope of a test, and always clears it back to nullptr on the way out —
/// the pointer is a shared static in the test binary (tests/ui_test_utils.cpp),
/// so a leaked pointer to a local would dangle for whatever test runs next.
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

std::vector<JobQueueEntry> three_jobs() {
    return {
        {"job-1", "first.gcode", 1000.0, 30.0},
        {"job-2", "second.gcode", 1100.0, 90.0},
        {"job-3", "third.gcode", 1200.0, 150.0},
    };
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "job_queue mode follows pixels on both axes, not spans, "
                 "and rebuilds the job list",
                 "[widget_size][job_queue]") {
    // Widget-owned subjects (jq_size_mode) are registered lazily; the harness
    // alone does not trigger it.
    PanelWidgetManager::instance().init_widget_subjects();

    // No live Moonraker connection: JobQueueState guards api_/client_ being
    // null in both fetch() (never called here) and subscribe_to_notifications()
    // (constructor), so nullptr/nullptr is safe — we only need cached job
    // data, injected directly via JobQueueStateTestAccess.
    JobQueueState jqs(nullptr, nullptr);
    JobQueueStateTestAccess::set_jobs(jqs, three_jobs());
    ScopedJobQueueState scoped_state(&jqs);

    PanelWidgetHarness<JobQueueWidget> h(test_screen());

    lv_obj_t* container = h.child("job_list_container");
    REQUIRE(container != nullptr);
    lv_obj_t* empty_state = h.child("jq_empty_state");
    REQUIRE(empty_state != nullptr);

    // --- Mode 0 (compact): both axes below floor. Contradicting span: 4x4
    // (old predicate: colspan>=2 && rowspan>=2, not both <=2 -> mode 2).
    h.resize(4, 4, w_normal() - 1, h_tall() - 1);
    process_lvgl(30);

    CHECK(jq_size_mode() == 0);
    CHECK(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_child_count(container) == 0);

    // --- Mode 1 (normal): both axes at/over their floor but below the wide
    // bands. Contradicting span: 1x1 (old predicate: colspan<2 -> mode 0).
    h.resize(1, 1, w_normal(), h_tall());
    process_lvgl(30);

    CHECK(jq_size_mode() == 1);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_child_count(container) == 3);
    // Mode 1 rows show only the filename — no time-in-queue label.
    CHECK(lv_obj_get_child_count(lv_obj_get_child(container, 0)) == 1);

    // --- Mode 2 via width alone: width at/over w_wide(), height still short of
    // h_taller() (at h_tall() only) — isolates the width term of the OR from the
    // height term. Contradicting span: 1x1 (old predicate -> mode 0).
    h.resize(1, 1, w_wide(), h_tall());
    process_lvgl(30);

    CHECK(jq_size_mode() == 2);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_child_count(container) == 3);
    // Mode 2 rows add the time-in-queue label alongside the filename.
    CHECK(lv_obj_get_child_count(lv_obj_get_child(container, 0)) == 2);

    // --- Mode 2 via height alone: height at/over h_taller(), width still
    // short of w_wide() (at w_normal() only) — isolates the height term.
    // Contradicting span: 1x1 (old predicate -> mode 0).
    h.resize(1, 1, w_normal(), h_taller());
    process_lvgl(30);

    CHECK(jq_size_mode() == 2);
    CHECK_FALSE(lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_child_count(container) == 3);
    CHECK(lv_obj_get_child_count(lv_obj_get_child(container, 0)) == 2);
}

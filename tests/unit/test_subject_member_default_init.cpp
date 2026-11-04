// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_subject_member_default_init.cpp
 * @brief A panel's lv_subject_t members read as LV_SUBJECT_TYPE_INVALID before
 *        init_subjects(), whatever bytes the allocation held (#1423).
 *
 * The panel is constructed into storage pre-filled with 0x02 bytes: read as an
 * lv_subject_t that is type LV_SUBJECT_TYPE_INT with a non-null subscriber
 * list, the garbage lv_subject_set_int() would walk. Value-initialized members
 * overwrite that with zeroes, so a premature publish takes LVGL's type check
 * and returns without touching the list.
 */

#include "ui_panel_print_status.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "core/update_queue.h"

#include <cstdlib>
#include <cstring>
#include <lvgl.h>
#include <new>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrintStatusPanel subjects are INVALID before init_subjects() (#1423)",
                 "[print_status][subject_lifetime]") {
    // Heap storage the constructor cannot see being filled, so the pattern
    // survives to the constructor and only the members' own initializers can
    // clear it.
    void* storage = std::malloc(sizeof(PrintStatusPanel));
    REQUIRE(storage != nullptr);
    std::memset(storage, LV_SUBJECT_TYPE_INT, sizeof(PrintStatusPanel));

    auto* panel = new (storage) PrintStatusPanel(state(), nullptr);
    REQUIRE_FALSE(panel->are_subjects_initialized());

    for (lv_subject_t* subject :
         {PrintStatusPanelTestAccess::print_controls_enabled_subject(*panel),
          PrintStatusPanelTestAccess::gcode_viewer_mode_subject(*panel)}) {
        CHECK(subject->type == LV_SUBJECT_TYPE_INVALID);
        CHECK(lv_ll_get_head(&subject->subs_ll) == nullptr);

        // The premature publish on_print_state_changed() makes: warned no-op.
        lv_subject_set_int(subject, 1);
        lv_subject_copy_string(subject, "x");
        CHECK(subject->type == LV_SUBJECT_TYPE_INVALID);
        CHECK(lv_subject_get_int(subject) == 0);

        // An observer cannot attach to an uninitialized subject either.
        int calls = 0;
        auto* observer = lv_subject_add_observer(
            subject,
            [](lv_observer_t* o, lv_subject_t*) {
                ++*static_cast<int*>(lv_observer_get_user_data(o));
            },
            &calls);
        CHECK(observer == nullptr);
        lv_subject_set_int(subject, 2);
        CHECK(calls == 0);
    }

    UpdateQueue::instance().drain();
    panel->~PrintStatusPanel();
    std::free(storage);
    UpdateQueue::instance().drain();
}

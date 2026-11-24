// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The wizard header's step progress (ui_xml/wizard_frame_header.xml), built from
// the production XML: progress="1" draws one dot per step, progress="2" one
// segment per step, each as many marks as the step count subject says.

#include "../lvgl_ui_test_fixture.h"
#include "core/subject.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

/// Marks drawn under @p node: leaf objects not hidden themselves.
int visible_marks(lv_obj_t* node) {
    const uint32_t n = lv_obj_get_child_count(node);
    if (n == 0) {
        return lv_obj_is_hidden(node) ? 0 : 1;
    }
    int marks = 0;
    for (uint32_t i = 0; i < n; ++i) {
        marks += visible_marks(lv_obj_get_child(node, i));
    }
    return marks;
}

/// Int subjects the header binds by name. Static: the XML registry keeps the
/// pointers after the case.
lv_subject_t* step_subject(const char* name, int value) {
    static helix::Subject count;
    static helix::Subject current;
    helix::Subject& s = std::string(name) == "test_wizard_step_count" ? count : current;
    if (!s.is_live()) {
        s.init_int(value);
        lv_xml_register_subject(nullptr, name, &s);
    }
    lv_subject_set_int(&s, value);
    return &s;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "the wizard header draws one progress mark per step",
                 "[calibration_run][wizard_progress]") {
    const auto [progress, name] = GENERATE(
        table<const char*, const char*>({{"1", "cal_header_dots"}, {"2", "cal_header_segments"}}));
    CAPTURE(progress);
    step_subject("test_wizard_step_count", 4);
    step_subject("test_wizard_step_current", 1);

    const char* attrs[] = {"title",
                           "Calibrate your printer",
                           "progress",
                           progress,
                           "step_count_subject",
                           "test_wizard_step_count",
                           "step_current_subject",
                           "test_wizard_step_current",
                           nullptr};
    auto* header =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "wizard_frame_header", attrs));
    REQUIRE(header != nullptr);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* steps = lv_obj_find_by_name(header, name);
    REQUIRE(steps != nullptr);
    CHECK(visible_marks(steps) == 4);

    // The count follows its subject.
    step_subject("test_wizard_step_count", 6);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(visible_marks(steps) == 6);

    lv_obj_delete(header);
    helix::ui::UpdateQueue::instance().drain();
}

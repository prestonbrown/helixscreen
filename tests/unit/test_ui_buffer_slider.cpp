// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_buffer_slider.cpp
 * @brief UiBufferSlider's lifecycle: the trace timer, and outliving its objects.
 */

#include "ui_buffer_slider.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterStatus;
using helix::ui::UiBufferSlider;

namespace {
lv_obj_t* box(lv_obj_t* parent, int w, int h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    return obj;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider redraws its trace once a second",
                 "[buffer][slider]") {
    AmsState::instance().init_subjects(true);
    UiBufferSlider slider(box(test_screen(), 24, 120), box(test_screen(), 160, 120), -1);
    lv_timer_set_repeat_count(slider.timer_for_test(), 3);
    process_lvgl(2100);
    CHECK(slider.trace_ticks() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider without a trace runs no timer",
                 "[buffer][slider]") {
    UiBufferSlider slider(box(test_screen(), 24, 120));
    process_lvgl(2100);
    CHECK(slider.trace_ticks() == 0);
    CHECK_FALSE(slider.has_trace_timer());
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider outlives the objects it draws into",
                 "[buffer][slider]") {
    AmsState::instance().init_subjects(true);
    lv_obj_t* slider_obj = box(test_screen(), 24, 120);
    lv_obj_t* trace_obj = box(test_screen(), 160, 120);
    auto slider = std::make_unique<UiBufferSlider>(slider_obj, trace_obj, -1);
    REQUIRE(slider->has_trace_timer());

    lv_obj_delete(trace_obj);
    process_lvgl(1100);
    CHECK_FALSE(slider->has_trace_timer());
    CHECK(slider->trace_ticks() == 0);

    lv_obj_delete(slider_obj);
    slider.reset(); // removes no callback from a freed object
    process_lvgl(100);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider follows the system-level reading",
                 "[buffer][slider]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    UiBufferSlider slider(box(test_screen(), 24, 120));
    slider.follow_system_reading();

    AmsSystemInfo info;
    info.sync_feedback_bias = -0.45f;
    AmsStateTestAccess::sync_buffer(ams, info, 0);
    CHECK(slider.bias() == Catch::Approx(-0.45f));
    CHECK(slider.status() == ClogMeterStatus::Warning);

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    CHECK(slider.bias() == Catch::Approx(0.0f));
    CHECK(slider.status() == ClogMeterStatus::Ok);
    AmsStateTestAccess::clear_buffer_traces(ams);
}

// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_buffer_slider.cpp
 * @brief UiBufferSlider's lifecycle: the trace timer, and outliving its objects.
 */

#include "ui_buffer_slider.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"
#include "buffer_reading.h"

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

namespace {
/// Paint the screen so the slider's draw hook runs.
void paint(lv_obj_t* screen) {
    lv_obj_update_layout(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(nullptr);
}

BufferReading pressure(float value, bool loaded = true) {
    return buffer_reading(test::fps_units({value}, 0.5f, -1, true, loaded), -1);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider paints a fill reading as the fill gauge",
                 "[buffer][slider][fill]") {
    UiBufferSlider slider(box(test_screen(), 24, 120));
    slider.set_reading(pressure(0.6f));
    CHECK(slider.gauge() == BufferGauge::Fill);
    CHECK(slider.value_pct() == 60);
    CHECK(slider.target_pct() == 50);
    paint(test_screen());
    REQUIRE(slider.has_painted());
    CHECK(slider.painted_gauge() == BufferGauge::Fill);

    SECTION("a bias reading paints the slider again") {
        slider.set_reading(-0.4f, ClogMeterStatus::Warning);
        CHECK(slider.gauge() == BufferGauge::Bias);
        paint(test_screen());
        CHECK(slider.painted_gauge() == BufferGauge::Bias);
        CHECK(slider.bias() == Catch::Approx(-0.4f));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UiBufferSlider paints an AFC or Happy Hare reading as the slider",
                 "[buffer][slider][fill]") {
    UiBufferSlider slider(box(test_screen(), 24, 120));
    slider.set_reading(buffer_reading(test::fps_units({0.32f}), -1));
    CHECK(slider.gauge() == BufferGauge::Bias);
    CHECK(slider.bias() == Catch::Approx(-0.36f));
    paint(test_screen());
    REQUIRE(slider.has_painted());
    CHECK(slider.painted_gauge() == BufferGauge::Bias);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider follows a system-level fill reading",
                 "[buffer][slider][fill]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    UiBufferSlider slider(box(test_screen(), 24, 120));
    slider.follow_system_reading();

    AmsStateTestAccess::sync_buffer(
        ams, test::fps_units({0.97f}, 0.5f, -1, /*compression_only=*/true, true), 0);
    CHECK(slider.gauge() == BufferGauge::Fill);
    CHECK(slider.value_pct() == 97);
    CHECK(slider.target_pct() == 50);
    CHECK(slider.status() == ClogMeterStatus::Fault);
    CHECK(slider.bias() == 0.0f);

    SECTION("and back to a bias reading") {
        AmsSystemInfo hh;
        hh.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_buffer(ams, hh, 1000);
        CHECK(slider.gauge() == BufferGauge::Bias);
        CHECK(slider.bias() == Catch::Approx(-0.45f));
    }
    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 2000);
    AmsStateTestAccess::clear_buffer_traces(ams);
}

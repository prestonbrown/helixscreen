// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_buffer.cpp
 * @brief AmsState's filament buffer reading: its traces and its subjects.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"
#include "clog_meter_geometry.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The traces and the buffer_* subjects are the singleton's, read by later tests.
struct ResetBuffer {
    AmsState& ams;
    ~ResetBuffer() {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        AmsStateTestAccess::clear_buffer_traces(ams);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState keeps a trace per buffer reading",
                 "[ams][buffer][trace]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetBuffer reset{ams};

    // Unit 1 feeds the toolhead: unit 0 reads +0.8, unit 1 reads -0.4.
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    AmsStateTestAccess::sync_buffer(ams, info, 1000);

    REQUIRE_FALSE(ams.buffer_trace(-1).window(1000).empty());
    CHECK(ams.buffer_trace(-1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(0).window(1000).back().bias == Catch::Approx(0.8f));
    CHECK(ams.buffer_trace(1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(5).size() == 0);

    SECTION("a unit that goes away takes its trace with it") {
        info.units.pop_back();
        info.current_slot = -1;
        AmsStateTestAccess::sync_buffer(ams, info, 2000);
        CHECK(ams.buffer_trace(1).size() == 0);
        CHECK(ams.buffer_trace(-1).window(2000).back().bias == Catch::Approx(0.8f));
    }

    SECTION("clear_backends drops every trace") {
        ams.clear_backends();
        CHECK(ams.buffer_trace(-1).size() == 0);
        CHECK(ams.buffer_trace(0).size() == 0);
    }
}

namespace {
int subject_int(lv_subject_t* s) {
    return lv_subject_get_int(s);
}
std::string text_of(lv_subject_t* s) {
    return lv_subject_get_string(s);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState publishes the system-level buffer reading",
                 "[ams][buffer][subjects]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetBuffer reset{ams};

    SECTION("Happy Hare sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_buffer(ams, info, 0);
        CHECK(subject_int(ams.get_buffer_present_subject()) == 1);
        CHECK(subject_int(ams.get_buffer_slider_subject()) == 1);
        CHECK(subject_int(ams.get_buffer_bias_pct_subject()) == -45);
        CHECK(subject_int(ams.get_buffer_status_subject()) ==
              static_cast<int>(ui::ClogMeterStatus::Warning));
        CHECK(text_of(ams.get_buffer_label_subject()) == "Sync");
        CHECK(text_of(ams.get_buffer_value_text_subject()) == "-45%");
    }

    SECTION("a set point that goes away mid-print leaves text and a gap") {
        AmsSystemInfo info = test::fps_units({0.62f});
        AmsStateTestAccess::sync_buffer(ams, info, 1000);
        REQUIRE(subject_int(ams.get_buffer_slider_subject()) == 1);
        CHECK(text_of(ams.get_buffer_target_text_subject()) == "target 50%");

        info.units[0].buffer_health->fps_set_point = -1.0f;
        info.sync_feedback_bias = info.pressure_sensor_bias();
        AmsStateTestAccess::sync_buffer(ams, info, 2000);
        CHECK(subject_int(ams.get_buffer_present_subject()) == 1);
        CHECK(subject_int(ams.get_buffer_slider_subject()) == 0);
        CHECK(subject_int(ams.get_buffer_bias_pct_subject()) == 0);
        CHECK(subject_int(ams.get_buffer_status_subject()) == 0);
        CHECK(text_of(ams.get_buffer_value_text_subject()) == "Pressure: 62%");
        CHECK(text_of(ams.get_buffer_short_text_subject()) == "62%");
        CHECK(text_of(ams.get_buffer_target_text_subject()).empty());
        const auto w = ams.buffer_trace(-1).window(2000);
        REQUIRE(w.size() == 2);
        CHECK_FALSE(w.back().valid);

        SECTION("and comes back") {
            info.units[0].buffer_health->fps_set_point = 0.5f;
            AmsStateTestAccess::sync_buffer(ams, info, 3000);
            CHECK(subject_int(ams.get_buffer_slider_subject()) == 1);
            CHECK(ams.buffer_trace(-1).window(3000).back().valid);
        }
    }

    SECTION("clear_backends takes the reading away") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_buffer(ams, info, 0);
        ams.clear_backends();
        CHECK(subject_int(ams.get_buffer_present_subject()) == 0);
        CHECK(text_of(ams.get_buffer_label_subject()).empty());
    }
}

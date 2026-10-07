// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_clog_meter_sources.cpp
 * @brief What feeds the clog meter: clog detectors only. A buffer reading is
 *        drawn by the filament buffer surfaces, never by the clog meter.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "clog_meter_geometry.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterMode;

namespace {

int mode(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_mode_subject());
}

AmsSystemInfo fps_info(float pressure) {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    unit.buffer_health = fps;
    info.units.push_back(unit);
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

/// The meter subjects and the override are the singleton's, read by later tests.
struct ResetMeter {
    AmsState& ams;
    ~ResetMeter() {
        ams.set_source_override(0);
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: a buffer reading alone is not clog detection",
                 "[ams][clog][sources]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    SECTION("Happy Hare sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == 0);
    }

    SECTION("a filament pressure sensor") {
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }

    SECTION("beside a detector, the meter shows the detector") {
        AmsSystemInfo info = fps_info(0.9f);
        info.encoder_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));
        CHECK(std::string(lv_subject_get_string(ams.get_clog_meter_mode_text_subject())) != "FPS");
    }

    SECTION("a forced detector this printer lacks leaves the meter empty") {
        ams.set_source_override(2); // Flowguard
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }
}

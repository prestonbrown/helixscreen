// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_reading.cpp
 * @brief buffer_reading(): which sensor a view reads and what it shows.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/buffer_infos.h"
#include "buffer_reading.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterStatus;

TEST_CASE("buffer_reading: a pressure sensor against its set point", "[buffer][reading]") {
    const BufferReading r = buffer_reading(test::fps_units({0.32f}), -1);
    CHECK(r.source == BufferSource::Fps);
    CHECK(r.present());
    CHECK(r.unit == 0);
    CHECK(r.has_slider);
    CHECK(r.value_pct == 32);
    CHECK(r.target_pct == 50);
    CHECK(r.bias == Catch::Approx(-0.36f));
    CHECK(r.status == ClogMeterStatus::Warning);
}

TEST_CASE("buffer_reading: no set point is a reading with nothing to centre on",
          "[buffer][reading]") {
    const BufferReading r = buffer_reading(test::fps_units({0.62f}, -1.0f), -1);
    CHECK(r.source == BufferSource::Fps);
    CHECK_FALSE(r.has_slider);
    CHECK(r.value_pct == 62);
    CHECK(r.target_pct == -1);
    CHECK(r.bias == 0.0f);
    CHECK(r.status == ClogMeterStatus::Ok);
}

TEST_CASE("buffer_reading: Happy Hare sync feedback is one buffer for the system",
          "[buffer][reading]") {
    AmsSystemInfo info;
    info.sync_feedback_bias = -0.45f;
    const BufferReading r = buffer_reading(info, -1);
    CHECK(r.source == BufferSource::Sync);
    CHECK(r.unit == -1);
    CHECK(r.has_slider);
    CHECK(r.value_pct == -45);
    CHECK(r.bias == Catch::Approx(-0.45f));
    CHECK(r.status == ClogMeterStatus::Warning);

    SECTION("a unit with no buffer of its own reads the same one") {
        AmsUnit unit;
        unit.slot_count = 4;
        info.units.push_back(unit);
        CHECK(buffer_reading(info, 0).source == BufferSource::Sync);
    }

    SECTION("no bias, no reading") {
        info.sync_feedback_bias = -2.0f;
        CHECK_FALSE(buffer_reading(info, -1).present());
    }
}

TEST_CASE("buffer_reading: the system follows the lane feeding the toolhead", "[buffer][reading]") {
    // Unit 0 reads 0.9 (bias +0.8), unit 1 reads 0.3 (bias -0.4).
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    CHECK(buffer_reading(info, -1).unit == 1);
    CHECK(buffer_reading(info, -1).bias == Catch::Approx(-0.4f));

    SECTION("several lanes loaded, no single current slot: the first lane with a sensor") {
        info.current_slot = -1;
        const BufferReading r = buffer_reading(info, -1);
        CHECK(r.unit == 0);
        CHECK(r.bias == Catch::Approx(0.8f));
        CHECK(r.status == ClogMeterStatus::Fault);
    }

    SECTION("a unit's own view reads its own lane") {
        CHECK(buffer_reading(info, 0).unit == 0);
        CHECK(buffer_reading(info, 0).bias == Catch::Approx(0.8f));
    }

    SECTION("a unit without a sensor reads the lane feeding the toolhead") {
        info.units[0].buffer_health.reset();
        CHECK(buffer_reading(info, 0).unit == 1);
    }

    SECTION("a unit index past the end reads the system") {
        CHECK(buffer_reading(info, 7).unit == 1);
    }
}

TEST_CASE("buffer_reading: a switched buffer has no reading", "[buffer][reading]") {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth turtleneck;
    turtleneck.fault_detection_enabled = true;
    turtleneck.distance_to_fault = 12.0f;
    unit.buffer_health = turtleneck;
    info.units.push_back(unit);
    // A pressure sensor elsewhere must not be borrowed by the switched unit.
    AmsSystemInfo other = test::fps_units({0.7f});
    info.units.push_back(other.units[0]);
    info.units[1].unit_index = 1;
    info.units[1].first_slot_global_index = 4;

    CHECK_FALSE(buffer_reading(info, 0).present());
    CHECK(buffer_reading(info, 1).present());
}

TEST_CASE("buffer_reading: a sensor past its rails", "[buffer][reading]") {
    const BufferReading high = buffer_reading(test::fps_units({1.2f}), -1);
    CHECK(high.value_pct == 100);
    CHECK(high.bias == Catch::Approx(1.0f));
    CHECK(high.status == ClogMeterStatus::Fault);

    const BufferReading low = buffer_reading(test::fps_units({-0.05f}), -1);
    CHECK(low.value_pct == 0);
    CHECK(low.bias == Catch::Approx(-1.0f));
    CHECK(low.status == ClogMeterStatus::Fault);
}

TEST_CASE("buffer_reading: the bands at their edges", "[buffer][reading]") {
    // bias = (p - 0.5) / 0.5, judged on lround(bias * 100)
    CHECK(buffer_reading(test::fps_units({0.36f}), -1).status == ClogMeterStatus::Ok);      // -28
    CHECK(buffer_reading(test::fps_units({0.35f}), -1).status == ClogMeterStatus::Warning); // -30
    CHECK(buffer_reading(test::fps_units({0.65f}), -1).status == ClogMeterStatus::Warning); // +30
    CHECK(buffer_reading(test::fps_units({0.84f}), -1).status == ClogMeterStatus::Warning); // +68
    CHECK(buffer_reading(test::fps_units({0.85f}), -1).status == ClogMeterStatus::Fault);   // +70
    CHECK(buffer_reading(test::fps_units({0.15f}), -1).status == ClogMeterStatus::Fault);   // -70
}

TEST_CASE_METHOD(LVGLTestFixture, "buffer reading words", "[buffer][reading][text]") {
    SECTION("pressure with a set point") {
        const BufferReading r = buffer_reading(test::fps_units({0.32f}), -1);
        CHECK(std::string(buffer_label(r)) == "FPS");
        CHECK(buffer_value_text(r) == "32%");
        CHECK(buffer_target_text(r) == "target 50%");
        CHECK(std::string(buffer_lean_text(r)) == "Running tight");
    }
    SECTION("pressure without one") {
        const BufferReading r = buffer_reading(test::fps_units({0.32f}, -1.0f), -1);
        CHECK(buffer_value_text(r) == "Pressure: 32%");
        CHECK(buffer_short_text(r) == "32%");
        CHECK(buffer_target_text(r).empty());
        CHECK(std::string(buffer_lean_text(r)).empty());
    }
    SECTION("sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = 0.15f;
        BufferReading r = buffer_reading(info, -1);
        CHECK(std::string(buffer_label(r)) == "Sync");
        CHECK(buffer_value_text(r) == "+15%");
        CHECK(buffer_short_text(r) == "+15%");
        CHECK(buffer_target_text(r).empty());
        CHECK(std::string(buffer_lean_text(r)) == "Running loose");
        info.sync_feedback_bias = 0.0f;
        r = buffer_reading(info, -1);
        CHECK(buffer_value_text(r) == "0%");
        CHECK(std::string(buffer_lean_text(r)) == "Balanced");
    }
    SECTION("nothing to read") {
        const BufferReading r;
        CHECK(std::string(buffer_label(r)).empty());
        CHECK(buffer_value_text(r).empty());
        CHECK(buffer_short_text(r).empty());
    }
}

namespace {
/// One compression-only lane (as OpenAMS reports) at @p pressure.
BufferReading fill_reading(float pressure, float set_point = 0.5f, bool loaded = true) {
    return buffer_reading(test::fps_units({pressure}, set_point, -1, true, loaded), -1);
}
} // namespace

TEST_CASE("buffer_reading: a compression-only sensor is a fill gauge, not a bias",
          "[buffer][reading][fill]") {
    const BufferReading r = fill_reading(0.32f);
    CHECK(r.source == BufferSource::Fps);
    CHECK(r.gauge == BufferGauge::Fill);
    CHECK(r.is_fill());
    CHECK(r.has_slider);
    CHECK(r.value_pct == 32);
    CHECK(r.target_pct == 50);
    CHECK(r.bias == 0.0f);
    // Below the set point is less compression, not tension.
    CHECK(r.status == ClogMeterStatus::Ok);

    SECTION("an AFC FPS_PSF sensor is still a two-ended bias") {
        const BufferReading afc = buffer_reading(test::fps_units({0.32f}), -1);
        CHECK(afc.gauge == BufferGauge::Bias);
        CHECK_FALSE(afc.is_fill());
        CHECK(afc.bias == Catch::Approx(-0.36f));
        CHECK(afc.status == ClogMeterStatus::Warning);
    }
    SECTION("Happy Hare sync feedback is still a two-ended bias") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        CHECK(buffer_reading(info, -1).gauge == BufferGauge::Bias);
    }
}

TEST_CASE("buffer_reading: a fill gauge is only colored near its rails",
          "[buffer][reading][fill]") {
    CHECK(fill_reading(0.84f).status == ClogMeterStatus::Ok);
    CHECK(fill_reading(0.85f).status == ClogMeterStatus::Warning);
    CHECK(fill_reading(0.94f).status == ClogMeterStatus::Warning);
    CHECK(fill_reading(0.95f).status == ClogMeterStatus::Fault);
    CHECK(fill_reading(1.2f).status == ClogMeterStatus::Fault);

    SECTION("regulating around the set point, however far from it, is neutral") {
        CHECK(fill_reading(0.20f, 0.5f).status == ClogMeterStatus::Ok);
        CHECK(fill_reading(0.80f, 0.5f).status == ClogMeterStatus::Ok);
        CHECK(fill_reading(0.50f, 0.5f).status == ClogMeterStatus::Ok);
    }
    SECTION("near empty warns only while filament is loaded") {
        CHECK(fill_reading(0.05f, 0.5f, true).status == ClogMeterStatus::Warning);
        CHECK(fill_reading(0.06f, 0.5f, true).status == ClogMeterStatus::Ok);
        CHECK(fill_reading(0.05f, 0.5f, false).status == ClogMeterStatus::Ok);
        CHECK(fill_reading(0.0f, 0.5f, true).status == ClogMeterStatus::Warning);
    }
    SECTION("the rails do not need a set point") {
        CHECK(fill_reading(0.97f, -1.0f).status == ClogMeterStatus::Fault);
        CHECK(fill_reading(0.03f, -1.0f, true).status == ClogMeterStatus::Warning);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "buffer reading words: a fill gauge against its target",
                 "[buffer][reading][fill][text]") {
    auto lean = [](float pressure) {
        return std::string(buffer_lean_text(fill_reading(pressure)));
    };

    // Target 50: the deadband is +/-5 points, inclusive.
    CHECK(lean(0.50f) == "At target");
    CHECK(lean(0.45f) == "At target");
    CHECK(lean(0.55f) == "At target");
    CHECK(lean(0.44f) == "Below target");
    CHECK(lean(0.56f) == "Above target");
    CHECK(lean(0.10f) == "Below target");
    CHECK(lean(0.90f) == "Above target");

    SECTION("with the number and the target") {
        const BufferReading r = fill_reading(0.53f);
        CHECK(std::string(buffer_label(r)) == "FPS");
        CHECK(buffer_value_text(r) == "53%");
        CHECK(buffer_target_text(r) == "target 50%");
        CHECK_FALSE(r.text_only());
    }
    SECTION("no set point: the number alone, no verdict") {
        const BufferReading r = fill_reading(0.53f, -1.0f);
        CHECK(r.has_slider);
        CHECK(r.target_pct == -1);
        CHECK(r.text_only());
        CHECK(buffer_value_text(r) == "Pressure: 53%");
        CHECK(std::string(buffer_lean_text(r)).empty());
        CHECK(buffer_target_text(r).empty());
    }
    SECTION("the trace caption names the pressure axis") {
        CHECK(std::string(buffer_trace_caption(fill_reading(0.5f))) ==
              "last 60 s · pressure, target dashed");
        CHECK(std::string(buffer_trace_caption(buffer_reading(test::fps_units({0.5f}), -1))) ==
              "last 60 s · LOOSE up, TIGHT down");
    }
}

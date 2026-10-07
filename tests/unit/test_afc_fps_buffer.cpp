// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_fps_buffer.cpp
 * @brief AFC's FPS_PSF filament pressure sensor, mapped onto sync_feedback_bias.
 *
 * An AFC_buffer configured `type: FPS_PSF` (AFCFPSBuffer, AFC v1.2.0+) reads an
 * analog pressure sensor and publishes `fps_value`, `smoothed_fps` and
 * `set_point`. That is the same quantity Happy Hare reports as
 * `sync_feedback_bias`, so mapping it onto the same -1..+1 scale lets one
 * buffer_reading() serve both.
 *
 * NOT VERIFIED ON HARDWARE. The only AFC rig here is a BoxTurtle with a
 * switched TurtleNeck buffer, whose live status carries none of these fields
 * (confirmed against the running printer). Everything below is pinned against
 * a source read of AFC v1.2.0 (a06f14d) `extras/AFC_buffer.py`, so these tests
 * are the contract to re-check when FPS hardware is available.
 *
 * The one thing hardware cannot change is the safety property: a buffer that
 * reports no pressure must be indistinguishable from before this existed.
 */

#include "ams_types.h"
#include "buffer_reading.h"

#include <optional>

#include "../catch_amalgamated.hpp"

namespace {

helix::BufferHealth fps_buffer(float smoothed, float set_point) {
    helix::BufferHealth h;
    h.smoothed_fps = smoothed;
    h.fps_value = smoothed;
    h.fps_set_point = set_point;
    h.fps_reported = true;
    return h;
}

constexpr float kNoData = -1.5f;

} // namespace

TEST_CASE("afc_fps: a switched buffer is untouched", "[ams][afc][fps]") {
    // The BoxTurtle case, and every AFC install before FPS existed. No fields,
    // no bias, and buffer_reading() gates on exactly this
    // sentinel — so nothing about those printers changes.
    helix::BufferHealth switched;
    CHECK_FALSE(switched.has_fps());
    CHECK(switched.fps_to_bias() == kNoData);

    // A half-populated frame is still not FPS data. Moonraker forwards only
    // changed keys, so a status update carrying one of the three is normal.
    helix::BufferHealth partial;
    partial.smoothed_fps = 0.5f;
    partial.fps_reported = true;
    CHECK_FALSE(partial.has_fps()); // set_point never arrived
    CHECK(partial.fps_to_bias() == kNoData);
}

TEST_CASE("afc_fps: the set point is neutral", "[ams][afc][fps]") {
    CHECK(fps_buffer(0.5f, 0.5f).fps_to_bias() == Catch::Approx(0.0f));
    // Including a deliberately off-centre one.
    CHECK(fps_buffer(0.3f, 0.3f).fps_to_bias() == Catch::Approx(0.0f));
}

TEST_CASE("afc_fps: sign follows the existing convention", "[ams][afc][fps]") {
    // Negative is tension (filament pulling tight), positive is compression
    // (filament loose) — the same reading buffer_status_modal describes in
    // words. AFC agrees: low_point is max tension, high_point max compression.
    CHECK(fps_buffer(0.2f, 0.5f).fps_to_bias() < 0.0f);
    CHECK(fps_buffer(0.8f, 0.5f).fps_to_bias() > 0.0f);
}

TEST_CASE("afc_fps: the rails map to full scale", "[ams][afc][fps]") {
    CHECK(fps_buffer(0.0f, 0.5f).fps_to_bias() == Catch::Approx(-1.0f));
    CHECK(fps_buffer(1.0f, 0.5f).fps_to_bias() == Catch::Approx(1.0f));

    // An off-centre set point still reaches both rails, because each side is
    // normalized against its own distance to the rail rather than a shared
    // span. low_point/high_point would have been the better divisor, but
    // AFCFPSBuffer::get_status does not publish them.
    CHECK(fps_buffer(0.0f, 0.8f).fps_to_bias() == Catch::Approx(-1.0f));
    CHECK(fps_buffer(1.0f, 0.8f).fps_to_bias() == Catch::Approx(1.0f));
}

TEST_CASE("afc_fps: readings outside the rail are clamped, not wrapped", "[ams][afc][fps]") {
    // The ADC is nominally 0..1 but this is a voltage divider on real hardware,
    // so a reading can land just outside it. Below the rail is max tension —
    // the most interesting reading there is — and an earlier value-based
    // presence check reported it as "no data" and blanked the meter.
    CHECK(fps_buffer(-0.2f, 0.5f).fps_to_bias() == Catch::Approx(-1.0f));
    CHECK(fps_buffer(1.4f, 0.5f).fps_to_bias() == Catch::Approx(1.0f));
}

TEST_CASE("afc_fps: a degenerate set point cannot divide by zero", "[ams][afc][fps]") {
    // AFC constrains set_point to 0.1..0.9 and validates low < set < high, but
    // this arrives over the wire and a 0 or 1 would otherwise be a divide by
    // zero on one side.
    for (float sp : {0.0f, 1.0f}) {
        auto b = fps_buffer(0.5f, sp);
        if (!b.has_fps()) {
            continue; // 0.0 is rejected outright, which is also fine
        }
        const float bias = b.fps_to_bias();
        INFO("set_point " << sp << " -> " << bias);
        CHECK(bias >= -1.0f);
        CHECK(bias <= 1.0f);
    }
}

static helix::AmsUnit unit_with(int first_slot, std::optional<helix::BufferHealth> buffer) {
    helix::AmsUnit u;
    u.first_slot_global_index = first_slot;
    u.slot_count = 4;
    u.buffer_health = std::move(buffer);
    return u;
}

TEST_CASE("fps: the system bias follows the sensor feeding the toolhead", "[ams][afc][fps]") {
    helix::BufferHealth switched;
    switched.state = "Trailing";
    helix::AmsSystemInfo info;
    info.units = {unit_with(0, switched), unit_with(4, fps_buffer(0.9f, 0.5f)),
                  unit_with(8, fps_buffer(0.3f, 0.5f))};

    SECTION("no current slot: the first unit with a sensor") {
        CHECK(info.pressure_sensor_bias() == Catch::Approx(0.8f));
    }
    SECTION("the current slot's unit when it has a sensor") {
        info.current_slot = 9;
        CHECK(info.pressure_sensor_bias() == Catch::Approx(-0.4f));
    }
    SECTION("a current slot on a switched buffer falls back to the first sensor") {
        info.current_slot = 1;
        CHECK(info.pressure_sensor_bias() == Catch::Approx(0.8f));
    }
    SECTION("no sensor anywhere") {
        info.units.resize(1);
        CHECK(info.pressure_sensor_bias() == kNoData);
    }
}

TEST_CASE("fps: a unit's buffer is drawn from its own sensor", "[ams][afc][fps]") {
    helix::BufferHealth switched;
    helix::AmsSystemInfo info;
    info.sync_feedback_bias = 0.6f;
    info.units = {unit_with(0, std::nullopt), unit_with(4, switched),
                  unit_with(8, fps_buffer(0.3f, 0.5f))};

    // A unit without a sensor reads the sensor feeding the toolhead.
    CHECK(helix::buffer_reading(info, 0).source == helix::BufferSource::Fps);
    CHECK(helix::buffer_reading(info, 0).bias == Catch::Approx(-0.4f));
    // A switched buffer has no proportional reading, whatever another unit says.
    CHECK(!helix::buffer_reading(info, 1).present());
    CHECK(helix::buffer_reading(info, 2).bias == Catch::Approx(-0.4f));

    // With no sensor anywhere, a unit without one reads the system-level bias
    // (Happy Hare's single buffer).
    info.units.pop_back();
    CHECK(helix::buffer_reading(info, 0).source == helix::BufferSource::Sync);
    CHECK(helix::buffer_reading(info, 0).bias == Catch::Approx(0.6f));
}

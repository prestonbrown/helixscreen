// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_detail_buffer_box.cpp
 * @brief What the path canvas's buffer box draws: which reading, its label,
 *        and the severity its tint comes from.
 */

#include "ui_ams_detail.h"

#include "../test_helpers/buffer_infos.h"
#include "buffer_reading.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ams_detail_buffer_box;

TEST_CASE("buffer box: the whole-backend view reads the lane feeding the toolhead",
          "[ams][buffer][path]") {
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    const auto box = ams_detail_buffer_box(info, -1);
    CHECK(box.present);
    CHECK(std::string(box.label) == "FPS");
    CHECK(box.bias == Catch::Approx(-0.4f));
    CHECK(box.fault == 1);

    SECTION("a unit's own view reads its own lane") {
        const auto own = ams_detail_buffer_box(info, 0);
        CHECK(own.bias == Catch::Approx(0.8f));
        CHECK(own.fault == 2);
    }
}

TEST_CASE("buffer box: on target is not a fault", "[ams][buffer][path]") {
    const auto box = ams_detail_buffer_box(test::fps_units({0.5f}), -1);
    CHECK(box.fault == 0);
    CHECK(box.bias == Catch::Approx(0.0f));
}

TEST_CASE("buffer box: no set point draws the FPS box untinted", "[ams][buffer][path]") {
    const auto box = ams_detail_buffer_box(test::fps_units({0.62f}, -1.0f), -1);
    CHECK(box.present);
    CHECK(std::string(box.label) == "FPS");
    CHECK(box.bias == -2.0f);
    CHECK(box.fault == -1);
}

TEST_CASE("buffer box: an AFC fault distance outranks a calm reading", "[ams][buffer][path]") {
    AmsSystemInfo info = test::fps_units({0.5f});
    auto& h = *info.units[0].buffer_health;
    h.fault_detection_enabled = true;
    h.distance_to_fault = 60.0f;
    CHECK(ams_detail_buffer_box(info, -1).fault == 2);
}

TEST_CASE("buffer box: Happy Hare sync feedback", "[ams][buffer][path]") {
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    info.sync_feedback_state = "tension";
    info.sync_feedback_bias = -0.8f;
    const auto box = ams_detail_buffer_box(info, -1);
    CHECK(box.present);
    CHECK(box.state == 2);
    CHECK(std::string(box.label) == "BUF");
    CHECK(box.fault == 2);
}

// A multi-unit AFC whose only sensor is on unit 1: the all-units box takes its
// state from that unit, the same one the modal describes.
static AmsSystemInfo sensor_on_unit_1() {
    AmsSystemInfo info = test::fps_units({0.5f, 0.5f}, 0.5f, /*current_slot=*/5);
    info.type = AmsType::AFC;
    info.units[0].buffer_health.reset();
    info.units[1].buffer_health->state = "Advancing";
    return info;
}

TEST_CASE("buffer box: the all-units view describes the sensor's unit", "[ams][buffer][path]") {
    const AmsSystemInfo info = sensor_on_unit_1();
    REQUIRE(buffer_view_unit(info, -1) == 1);
    CHECK(ams_detail_buffer_box(info, -1).state == ams_detail_buffer_box(info, 1).state);
    CHECK(ams_detail_buffer_box(info, -1).state == 1);
}

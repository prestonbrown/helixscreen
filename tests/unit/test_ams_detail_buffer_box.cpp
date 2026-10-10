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

// distance_to_fault counts DOWN to a fault: small is danger, negative is a
// stopped timer. Values are the HELIX_MOCK_BUFFER_STATE table's.
static int afc_box_fault(float distance, float sensitivity = 7.0f) {
    AmsSystemInfo info = test::fps_units({0.5f});
    auto& h = *info.units[0].buffer_health;
    h.fault_detection_enabled = true;
    h.error_sensitivity = sensitivity;
    h.distance_to_fault = distance;
    return ams_detail_buffer_box(info, -1).fault;
}

TEST_CASE("buffer box: the AFC fault countdown tints like the clog meter", "[ams][buffer][path]") {
    CHECK(afc_box_fault(-100.0f) == 0); // timer stopped
    CHECK(afc_box_fault(40.0f) == 0);   // at the 40mm threshold: 0% danger
    CHECK(afc_box_fault(25.0f) == 0);   // 37.5%, below the meter's danger mark
    CHECK(afc_box_fault(10.0f) == 1);   // 75%: on the danger mark
    CHECK(afc_box_fault(5.0f) == 2);    // 87.5%: fault imminent
    CHECK(afc_box_fault(60.0f) == 0);   // just reset, above the threshold
}

TEST_CASE("buffer box: error_sensitivity moves the AFC threshold", "[ams][buffer][path]") {
    // The threshold is (11 - sensitivity) * 10 mm: 40mm at 7, 100mm at 1.
    CHECK(afc_box_fault(15.0f, 1.0f) == 2); // 100mm threshold: 85% danger
    CHECK(afc_box_fault(15.0f, 7.0f) == 0); // 40mm threshold: 62.5%
}

TEST_CASE("buffer box: AFC fault detection off draws no fault", "[ams][buffer][path]") {
    AmsSystemInfo info = test::fps_units({0.5f});
    auto& h = *info.units[0].buffer_health;
    h.fault_detection_enabled = false;
    h.distance_to_fault = 1.0f;
    CHECK(ams_detail_buffer_box(info, -1).fault == 0);
}

TEST_CASE("buffer box: an AFC fault outranks a calm pressure reading", "[ams][buffer][path]") {
    CHECK(afc_box_fault(2.0f) == 2);
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

TEST_CASE("buffer box: a one-sided pressure passes no bias", "[ams][buffer][path][fill]") {
    auto box_of = [](float pressure, bool loaded = true, float set_point = 0.5f) {
        return ams_detail_buffer_box(
            test::fps_units({pressure}, set_point, -1, /*compression_only=*/true, loaded), -1);
    };

    SECTION("far from the set point, inside the rails: no bias, untinted") {
        for (float pressure : {0.2f, 0.5f, 0.8f}) {
            const auto box = box_of(pressure);
            CHECK(box.present);
            CHECK(std::string(box.label) == "FPS");
            CHECK(box.bias == -2.0f);
            CHECK(box.fault == -1);
        }
    }
    SECTION("the rails tint by status alone") {
        CHECK(box_of(0.90f).bias == -2.0f);
        CHECK(box_of(0.90f).fault == 1);
        CHECK(box_of(0.97f).bias == -2.0f);
        CHECK(box_of(0.97f).fault == 2);
        CHECK(box_of(0.03f).fault == 1);
        CHECK(box_of(0.03f, /*loaded=*/false).fault == -1);
    }
    SECTION("no set point still tints at a rail") {
        CHECK(box_of(0.97f, true, -1.0f).fault == 2);
        CHECK(box_of(0.60f, true, -1.0f).fault == -1);
    }
    SECTION("a two-ended sensor still passes its bias") {
        const auto box = ams_detail_buffer_box(test::fps_units({0.3f}), -1);
        CHECK(box.bias == Catch::Approx(-0.4f));
    }
}

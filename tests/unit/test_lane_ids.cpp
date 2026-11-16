// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Lane ids name one backend's slot, the bypass or a tool; the funnels refuse
// anything else, replace a source whole, and keep lanes apart. A lane colour
// string reads as a value, a clear or nothing.

#include "helix_test_fixture.h"
#include "lane_source_store.h"
#include "lane_translation.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using helix::ams::lane_id_value;
using helix::ams::LaneId;
using helix::ams::Observation;
using helix::ams::ObservationSource;

TEST_CASE("a lane id names one backend's slot, and only assigned ids are lanes", "[lane][ids]") {
    using namespace helix::ams;
    constexpr int kEndOfBlocks = kMaxBackends * kLanesPerBackend;
    const auto plus = [](LaneId lane, int n) { return LaneId{lane_id_value(lane) + n}; };

    CHECK(lane_id_for(0, 3) == LaneId{3});
    CHECK(lane_id_for(1, 0) == LaneId{kLanesPerBackend});
    CHECK(lane_id_for(0, kLanesPerBackend - 1) < lane_id_for(1, 0));
    CHECK(lane_id_for(kMaxBackends - 1, kLanesPerBackend - 1) < kBypassLaneId);
    CHECK(kFirstToolLaneId > kBypassLaneId);

    // A pair naming no lane yields no id, never the nearest real one.
    CHECK(lane_id_for(-1, 0) == kInvalidLaneId);
    CHECK(lane_id_for(kMaxBackends, 0) == kInvalidLaneId);
    CHECK(lane_id_for(0, -1) == kInvalidLaneId);
    CHECK(lane_id_for(0, kLanesPerBackend) == kInvalidLaneId);

    CHECK(is_lane_id(LaneId{0}));
    CHECK(is_lane_id(LaneId{kEndOfBlocks - 1}));
    CHECK(is_lane_id(kBypassLaneId));
    CHECK(is_lane_id(kFirstToolLaneId));
    CHECK(is_lane_id(plus(kEndLaneId, -1)));
    CHECK_FALSE(is_lane_id(kInvalidLaneId));
    CHECK_FALSE(is_lane_id(LaneId{kEndOfBlocks}));
    CHECK_FALSE(is_lane_id(plus(kBypassLaneId, -1)));
    CHECK_FALSE(is_lane_id(plus(kBypassLaneId, 1)));
    CHECK_FALSE(is_lane_id(plus(kFirstToolLaneId, -1)));
    CHECK_FALSE(is_lane_id(kEndLaneId));
}

TEST_CASE_METHOD(HelixTestFixture, "the funnels write only lanes, whole, and lane by lane",
                 "[lane][ids]") {
    helix::ams::reset_lane_sources();
    const auto restore_level = spdlog::default_logger()->level();
    spdlog::set_level(spdlog::level::critical);
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    helix::ams::ingest(helix::ams::kInvalidLaneId, sensed);
    helix::ams::ingest(helix::ams::kEndLaneId, sensed);
    Observation edit(ObservationSource::Pending);
    edit.material = "PETG";
    helix::ams::file_pending(helix::ams::kInvalidLaneId, edit);
    spdlog::set_level(restore_level);
    CHECK(helix::ams::known_lanes().empty());

    // A source replaced whole: a field it stopped stating stops contributing.
    Observation first(ObservationSource::Firmware);
    first.color_rgb = 0xED2C2C;
    first.material = "PETG";
    helix::ams::ingest(LaneId{0}, first);
    Observation second(ObservationSource::Firmware);
    second.color_rgb = 0xED2C2C;
    helix::ams::ingest(LaneId{0}, second);
    Observation other(ObservationSource::Firmware);
    other.material = "ABS";
    helix::ams::ingest(LaneId{1}, other);

    const auto lane0 = helix::ams::lane_sources(LaneId{0});
    REQUIRE(lane0.firmware.has_value());
    CHECK_FALSE(lane0.firmware->material.has_value());
    CHECK(helix::ams::lane_sources(LaneId{1}).firmware->material == "ABS");
    CHECK(helix::ams::known_lanes().size() == 2);
    helix::ams::reset_lane_sources();
}

TEST_CASE("a lane colour string reads as a value, a clear or nothing", "[lane][ids]") {
    using helix::ams::ColorReadingKind;
    using helix::ams::read_lane_color;
    for (const char* spelled : {"#ED2C2C", "ED2C2C", "0xED2C2C", "ed2c2c"}) {
        INFO(spelled);
        CHECK(read_lane_color(spelled).kind == ColorReadingKind::Observed);
        CHECK(read_lane_color(spelled).rgb == 0xED2C2Cu);
    }
    CHECK(read_lane_color("#000000").kind == ColorReadingKind::Observed);
    CHECK(read_lane_color("#F00").rgb == 0xFF0000u);
    CHECK(read_lane_color("#800080FF").rgb == 0x800080u);
    for (const char* cleared : {"", "#", "  ", " # "}) {
        CHECK(read_lane_color(cleared).kind == ColorReadingKind::Cleared);
    }
    for (const char* junk : {"#zzzzzz", "FF0000junk", "-1", "beef", "None"}) {
        INFO(junk);
        CHECK(read_lane_color(junk).kind == ColorReadingKind::NoReading);
    }
}

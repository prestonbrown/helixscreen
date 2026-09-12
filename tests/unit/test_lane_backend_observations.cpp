// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES; a SlotInfo read appears only where one is
// needed to establish a case's precondition.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/registered_backend.h"

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsBackendAd5xIfs;
using helix::ams::lane_sources;
using helix::test::RegisteredBackend;

namespace {
/// A backend with no Moonraker behind it, registered so its lane ids are real.
using Ad5xHarness = RegisteredBackend<AmsBackendAd5xIfs>;
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a registered backend's slots are its own block of lanes",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    CHECK(harness.lane(0) == helix::ams::lane_id_for(0, 0));
    CHECK(harness.lane(3) == helix::ams::lane_id_for(0, 3));
    CHECK(helix::ams::is_lane_id(harness.lane(0)));
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X ingests silk-sensor presence as sensed",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
    Ad5xIfsTestAccess::set_material(*harness, 0, "PETG");
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, false);

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == false);
    // The vendor file still remembers the last spool. That is a cache of a past
    // declaration, so it must not carry presence or identity into the sensor's
    // record.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.vendor_cache->material.has_value());
    CHECK(*lane.vendor_cache->material == "PETG");
    CHECK_FALSE(lane.vendor_cache->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files a sensed record with no reading before Ports is seen",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);

    // Native ZMOD publishes no per-port sensors, so port_presence_ is false for
    // every lane whether or not filament is there. An unread sensor is not a
    // reading of "empty".
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto unread = lane_sources(harness.lane(1));
    // The record must exist: its absence would mean the translation never ran,
    // which is a different fact from "the sensor has said nothing yet" and must
    // not pass for it.
    REQUIRE(unread.sensed.has_value());
    CHECK_FALSE(unread.sensed->present.has_value());

    // The same lane, once the silk mask has been read. This half is what makes
    // the half above a guard rather than a backend that never reports presence.
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto read = lane_sources(harness.lane(1));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X does not cache a colour it has not read",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 2, "");
    Ad5xIfsTestAccess::set_material(*harness, 2, "");
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    const auto lane = lane_sources(harness.lane(2));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X caches pure black as a colour", "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 3, true);
    Ad5xIfsTestAccess::set_color(*harness, 3, "000000");

    const auto lane = lane_sources(harness.lane(3));

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files no colour when the stored one will not parse",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");

    const auto parsed = lane_sources(harness.lane(0));
    REQUIRE(parsed.vendor_cache.has_value());
    REQUIRE(parsed.vendor_cache->color_rgb == 0xED2C2Cu);

    // parse_adventurer_json stores ffmColorN as the printer sent it, so
    // colors_[] can hold a string that is not hex. The colour already on the
    // lane must not stand in for one: the reading is gone, and "no colour
    // observed" is a state this model can express.
    Ad5xIfsTestAccess::set_color(*harness, 0, "notahexvalue");

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AD5X's vendor-cache record",
                 "[lane][ingest][ad5x]") {
    Ad5xHarness harness(nullptr, nullptr);
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);

    // A user colour that disagrees with what the vendor file says. SlotInfo is
    // persistent across frames and apply_overrides rewrites it in place, so
    // entry->info.color_rgb is this value by the time the frame ends. A
    // translation reading it back would file the user's own choice as something
    // the vendor store remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    Ad5xIfsTestAccess::seed_override(*harness, 1, user);

    Ad5xIfsTestAccess::set_color(*harness, 1, "ED2C2C");
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, true);

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot, there is no laundering for the case to catch and
    // the assertion below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);

    // A lane the vendor file says nothing about. Here entry->info.color_rgb
    // holds the override alone, so a read-back files a colour where the vendor
    // store has none.
    Ad5xIfsTestAccess::seed_override(*harness, 2, user);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    REQUIRE(harness->get_slot_info(2).color_rgb == 0x00FF00u);

    const auto blank = lane_sources(harness.lane(2));
    REQUIRE(blank.vendor_cache.has_value());
    CHECK_FALSE(blank.vendor_cache->color_rgb.has_value());
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_test_fixture.h"
#include "lane_source_store.h"

#include "../catch_amalgamated.hpp"

using helix::ams::ingest;
using helix::ams::lane_sources;
using helix::ams::Observation;
using helix::ams::ObservationSource;

// The store treats a lane id as an opaque key, so these cases use bare
// integers. Everything that produces an id goes through lane_id_for().
TEST_CASE_METHOD(HelixTestFixture, "ingest writes one source and leaves the rest alone",
                 "[lane][ingest]") {
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(2, sensed);

    Observation cache(ObservationSource::VendorCache);
    cache.color_rgb = 0xED2C2C;
    cache.material = "PETG";
    ingest(2, cache);

    const auto lane = lane_sources(2);
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == true);
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->color_rgb == 0xED2C2C);
    CHECK_FALSE(lane.spoolman.has_value());
    CHECK_FALSE(lane.local_user.has_value());
    CHECK_FALSE(lane.metered.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "ingest replaces a source's record whole", "[lane][ingest]") {
    Observation first(ObservationSource::VendorCache);
    first.color_rgb = 0xED2C2C;
    first.material = "PETG";
    ingest(0, first);

    // The vendor store stops reporting a material. Whole-record replacement is
    // what makes that stop contributing: a field the source has stopped
    // observing must not keep standing from an earlier frame.
    Observation second(ObservationSource::VendorCache);
    second.color_rgb = 0xED2C2C;
    ingest(0, second);

    const auto lane = lane_sources(0);
    REQUIRE(lane.vendor_cache.has_value());
    CHECK(lane.vendor_cache->color_rgb == 0xED2C2C);
    CHECK_FALSE(lane.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "lanes are independent destinations", "[lane][ingest]") {
    Observation a(ObservationSource::Sensed);
    a.present = true;
    ingest(0, a);

    Observation b(ObservationSource::Sensed);
    b.present = false;
    ingest(1, b);

    const auto lane0 = lane_sources(0);
    const auto lane1 = lane_sources(1);
    REQUIRE(lane0.sensed.has_value());
    REQUIRE(lane1.sensed.has_value());
    CHECK(lane0.sensed->present == true);
    CHECK(lane1.sensed->present == false);
}

TEST_CASE_METHOD(HelixTestFixture, "an unseen lane reads as nothing observed", "[lane][ingest]") {
    const auto lane = lane_sources(7);
    CHECK_FALSE(lane.sensed.has_value());
    CHECK_FALSE(lane.vendor_cache.has_value());
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a lane id names one backend's slot and nothing else", "[lane][ingest]") {
    using helix::ams::lane_id_for;

    // Two backends is the ordinary case, not an exotic one: a tool changer
    // beside a filament system is what makes a bare slot index wrong.
    CHECK(lane_id_for(0, 0) == 0);
    CHECK(lane_id_for(0, 3) == 3);
    CHECK(lane_id_for(1, 0) != lane_id_for(0, 0));
    CHECK(lane_id_for(1, 0) == 16);

    // The last slot of one block never collides with the first of the next.
    CHECK(lane_id_for(0, helix::ams::LANES_PER_BACKEND - 1) < lane_id_for(1, 0));

    // The printer-level ids sit clear of every backend block.
    CHECK(helix::ams::BYPASS_LANE_ID > lane_id_for(60, 15));
    CHECK(helix::ams::FIRST_TOOL_LANE_ID > helix::ams::BYPASS_LANE_ID);

    // The last backend and slot this scheme supports still sits below the
    // bypass id, pinning the boundary MAX_BACKENDS exists to hold.
    CHECK(lane_id_for(helix::ams::MAX_BACKENDS - 1, helix::ams::LANES_PER_BACKEND - 1) <
          helix::ams::BYPASS_LANE_ID);
}

TEST_CASE_METHOD(HelixTestFixture, "known_lanes lists every lane that has been written",
                 "[lane][ingest]") {
    Observation obs(ObservationSource::Sensed);
    obs.present = true;
    ingest(3, obs);
    ingest(0, obs);

    const auto lanes = helix::ams::known_lanes();
    REQUIRE(lanes.size() == 2);
    CHECK(lanes[0] == 0);
    CHECK(lanes[1] == 3);
}

TEST_CASE("every ObservationSource round-trips to its own LaneSources member", "[lane]") {
    const ObservationSource sources[] = {
        ObservationSource::Sensed, ObservationSource::Spoolman, ObservationSource::LocalUser,
        ObservationSource::VendorCache, ObservationSource::Metered};

    for (ObservationSource s : sources) {
        helix::ams::LaneSources lane;
        Observation obs(s);
        obs.present = true;
        lane.apply(obs);

        CHECK(lane.sensed.has_value() == (s == ObservationSource::Sensed));
        CHECK(lane.spoolman.has_value() == (s == ObservationSource::Spoolman));
        CHECK(lane.local_user.has_value() == (s == ObservationSource::LocalUser));
        CHECK(lane.vendor_cache.has_value() == (s == ObservationSource::VendorCache));
        CHECK(lane.metered.has_value() == (s == ObservationSource::Metered));

        lane.drop(s);
        CHECK_FALSE(lane.sensed.has_value());
        CHECK_FALSE(lane.spoolman.has_value());
        CHECK_FALSE(lane.local_user.has_value());
        CHECK_FALSE(lane.vendor_cache.has_value());
        CHECK_FALSE(lane.metered.has_value());
    }
}

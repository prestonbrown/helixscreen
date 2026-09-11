// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_observation.h"
#include "lane_resolver.h"
#include "lane_sources.h"

#include "../catch_amalgamated.hpp"

using helix::ams::Observation;
using helix::ams::ObservationSource;

TEST_CASE("Observation distinguishes an unobserved field from an empty one", "[lane][resolver]") {
    Observation obs;
    obs.source = ObservationSource::Sensed;

    // Nothing observed yet. This is the whole point of the type: "I have no
    // reading" must not be spelled the same as "the value is blank", which is
    // what every sentinel check (!= 0, !empty(), >= 0.0f) conflates today.
    CHECK_FALSE(obs.color_rgb.has_value());
    CHECK_FALSE(obs.material.has_value());
    CHECK_FALSE(obs.present.has_value());

    obs.material = "";
    CHECK(obs.material.has_value());
    CHECK(obs.material->empty());
}

TEST_CASE("LaneSources keeps one record per source", "[lane][resolver]") {
    helix::ams::LaneSources lane;

    Observation spool;
    spool.source = ObservationSource::Spoolman;
    spool.color_rgb = 0xA4B2BC;
    lane.apply(spool);

    Observation sensed;
    sensed.source = ObservationSource::Sensed;
    sensed.present = false;
    lane.apply(sensed);

    // Writing one source leaves every other untouched. There is no shared
    // destination, so no writer can clobber another's value.
    REQUIRE(lane.spoolman.has_value());
    CHECK(lane.spoolman->color_rgb == 0xA4B2BC);
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);

    // Re-applying a source REPLACES that source's record, whole.
    Observation newer;
    newer.source = ObservationSource::Spoolman;
    newer.color_rgb = 0x00FF00;
    lane.apply(newer);
    CHECK(lane.spoolman->color_rgb == 0x00FF00);
    CHECK(lane.sensed->present == false);

    // Dropping one source leaves every other standing. This is the whole
    // "clear" a lane needs: the hand-partitioned clear paths exist only
    // because there is a single shared struct to partition.
    lane.drop(ObservationSource::Spoolman);
    CHECK_FALSE(lane.spoolman.has_value());
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);
}

TEST_CASE("Presence comes from the sensor and nothing else", "[lane][resolver]") {
    helix::ams::LaneSources lane;

    SECTION("identity metadata never implies presence") {
        // A cache that still remembers the last spool is not evidence a spool
        // is there. Vendor stores keep colour across an eject by design.
        Observation cache;
        cache.source = ObservationSource::VendorCache;
        cache.color_rgb = 0x8000FF;
        cache.material = "PLA";
        lane.apply(cache);

        Observation spool;
        spool.source = ObservationSource::Spoolman;
        spool.spoolman_id = 7;
        spool.material = "PETG";
        lane.apply(spool);

        CHECK_FALSE(helix::ams::resolve(lane).present);
    }

    SECTION("the sensor decides, in both directions") {
        Observation sensed;
        sensed.source = ObservationSource::Sensed;
        sensed.present = true;
        lane.apply(sensed);
        CHECK(helix::ams::resolve(lane).present);

        sensed.present = false;
        lane.apply(sensed);
        CHECK_FALSE(helix::ams::resolve(lane).present);
    }
}

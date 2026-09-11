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

TEST_CASE("Identity ranks Spoolman over the user's own record over the vendor cache",
          "[lane][resolver]") {
    helix::ams::LaneSources lane;

    Observation cache;
    cache.source = ObservationSource::VendorCache;
    cache.color_rgb = 0xFFFFFF;
    cache.material = "PETG";
    cache.brand = "";
    lane.apply(cache);

    SECTION("the vendor cache is used when it is all there is") {
        const auto r = helix::ams::resolve(lane);
        CHECK(r.color_rgb == 0xFFFFFF);
        CHECK(r.material == "PETG");
    }

    SECTION("a user record outranks the vendor cache") {
        Observation user;
        user.source = ObservationSource::LocalUser;
        user.color_rgb = 0xBCBCBC;
        lane.apply(user);

        const auto r = helix::ams::resolve(lane);
        CHECK(r.color_rgb == 0xBCBCBC);
        // Material was not observed by the user, so the cache still supplies it.
        CHECK(r.material == "PETG");
    }

    SECTION("a linked spool supplies identity, but not a colour the user picked") {
        Observation user;
        user.source = ObservationSource::LocalUser;
        user.color_rgb = 0xBCBCBC;
        lane.apply(user);

        Observation spool;
        spool.source = ObservationSource::Spoolman;
        spool.spoolman_id = 4;
        spool.color_rgb = 0xA4B2BC;
        spool.brand = "Kingroon";
        lane.apply(spool);

        const auto r = helix::ams::resolve(lane);
        // Brand and the spool link come from the spool: they describe the
        // spool, not the lane.
        CHECK(r.brand == "Kingroon");
        CHECK(r.spoolman_id == 4);
        // Material was observed by neither, so the cache still supplies it.
        CHECK(r.material == "PETG");
        // The colour is the user's, because they chose it for this lane.
        CHECK(r.color_rgb == 0xBCBCBC);
    }
}

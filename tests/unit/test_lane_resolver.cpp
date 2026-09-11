// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_observation.h"
#include "lane_resolver.h"
#include "lane_sources.h"

#include "../catch_amalgamated.hpp"

using helix::ams::Observation;
using helix::ams::ObservationSource;

TEST_CASE("Observation distinguishes an unobserved field from an empty one", "[lane][resolver]") {
    Observation obs(ObservationSource::Sensed);

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

    Observation spool(ObservationSource::Spoolman);
    spool.color_rgb = 0xA4B2BC;
    lane.apply(spool);

    Observation sensed(ObservationSource::Sensed);
    sensed.present = false;
    lane.apply(sensed);

    // Writing one source leaves every other untouched. There is no shared
    // destination, so no writer can clobber another's value.
    REQUIRE(lane.spoolman.has_value());
    CHECK(lane.spoolman->color_rgb == 0xA4B2BC);
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);

    // Re-applying a source REPLACES that source's record, whole.
    Observation newer(ObservationSource::Spoolman);
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
        Observation cache(ObservationSource::VendorCache);
        cache.color_rgb = 0x8000FF;
        cache.material = "PLA";
        lane.apply(cache);

        Observation spool(ObservationSource::Spoolman);
        spool.spoolman_id = 7;
        spool.material = "PETG";
        lane.apply(spool);

        CHECK_FALSE(helix::ams::resolve(lane).present);
    }

    SECTION("the sensor decides, in both directions") {
        Observation sensed(ObservationSource::Sensed);
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

    Observation cache(ObservationSource::VendorCache);
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
        Observation user(ObservationSource::LocalUser);
        user.color_rgb = 0xBCBCBC;
        lane.apply(user);

        const auto r = helix::ams::resolve(lane);
        CHECK(r.color_rgb == 0xBCBCBC);
        // Material was not observed by the user, so the cache still supplies it.
        CHECK(r.material == "PETG");
    }

    SECTION("a linked spool supplies identity, but not a colour the user picked") {
        Observation user(ObservationSource::LocalUser);
        user.color_rgb = 0xBCBCBC;
        lane.apply(user);

        Observation spool(ObservationSource::Spoolman);
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

TEST_CASE("The identity ladder applies sources weakest first", "[lane][resolver]") {
    // Every ranked field is observed by all three sources, so reversing the
    // ladder flips every assertion rather than only one that happens to
    // collide. Colour is deliberately absent: the user-colour override runs
    // after the ladder and would mask an ordering defect on that field alone.
    helix::ams::LaneSources lane;

    Observation cache(ObservationSource::VendorCache);
    cache.color_name = "cache";
    cache.material = "cache";
    cache.brand = "cache";
    cache.spool_name = "cache";
    cache.catalog_id = "cache";
    cache.product_name = "cache";
    cache.spoolman_id = 1;
    cache.spoolman_vendor_id = 1;
    lane.apply(cache);

    Observation user = cache;
    user.source = ObservationSource::LocalUser;
    user.color_name = "user";
    user.material = "user";
    user.brand = "user";
    user.spool_name = "user";
    user.catalog_id = "user";
    user.product_name = "user";
    user.spoolman_id = 2;
    user.spoolman_vendor_id = 2;
    lane.apply(user);

    Observation spool = cache;
    spool.source = ObservationSource::Spoolman;
    spool.color_name = "spool";
    spool.material = "spool";
    spool.brand = "spool";
    spool.spool_name = "spool";
    spool.catalog_id = "spool";
    spool.product_name = "spool";
    spool.spoolman_id = 3;
    spool.spoolman_vendor_id = 3;
    lane.apply(spool);

    const auto r = helix::ams::resolve(lane);
    CHECK(r.color_name == "spool");
    CHECK(r.material == "spool");
    CHECK(r.brand == "spool");
    CHECK(r.spool_name == "spool");
    CHECK(r.catalog_id == "spool");
    CHECK(r.product_name == "spool");
    CHECK(r.spoolman_id == 3);
    CHECK(r.spoolman_vendor_id == 3);
}

TEST_CASE("Weight comes from Spoolman when a spool is linked, the meter otherwise",
          "[lane][resolver]") {
    helix::ams::LaneSources lane;

    Observation metered(ObservationSource::Metered);
    metered.remaining_weight_g = 218.0F;
    metered.total_weight_g = 750.0F;
    lane.apply(metered);

    SECTION("an unlinked lane uses the meter") {
        const auto r = helix::ams::resolve(lane);
        CHECK(r.remaining_weight_g == Catch::Approx(218.0F));
    }

    SECTION("a linked lane uses Spoolman, which owns consumption for it") {
        Observation spool(ObservationSource::Spoolman);
        spool.spoolman_id = 4;
        spool.remaining_weight_g = 71.0F;
        spool.total_weight_g = 1000.0F;
        lane.apply(spool);

        const auto r = helix::ams::resolve(lane);
        CHECK(r.remaining_weight_g == Catch::Approx(71.0F));
        CHECK(r.total_weight_g == Catch::Approx(1000.0F));
    }

    SECTION("a linked spool that reports no weight does not blank the meter's") {
        Observation spool(ObservationSource::Spoolman);
        spool.spoolman_id = 4;
        lane.apply(spool);

        CHECK(helix::ams::resolve(lane).remaining_weight_g == Catch::Approx(218.0F));
    }
}

TEST_CASE("A weight refresh cannot disturb presence or identity", "[lane][resolver]") {
    // A Metered observation reaches only the weight fields of its own record,
    // so no number of weight writes can reach presence or identity. There is no
    // shared destination for them to pass through.
    helix::ams::LaneSources lane;

    Observation sensed(ObservationSource::Sensed);
    sensed.present = false;
    lane.apply(sensed);

    Observation cache(ObservationSource::VendorCache);
    cache.material = "PETG";
    cache.color_rgb = 0xED2C2C;
    lane.apply(cache);

    const auto before = helix::ams::resolve(lane);
    REQUIRE_FALSE(before.present);

    for (int i = 0; i < 100; ++i) {
        Observation weight(ObservationSource::Metered);
        weight.remaining_weight_g = static_cast<float>(200 - i);
        lane.apply(weight);

        const auto after = helix::ams::resolve(lane);
        REQUIRE_FALSE(after.present);
        REQUIRE(after.material == "PETG");
        REQUIRE(after.color_rgb == 0xED2C2C);
    }
}

TEST_CASE("A colour the user picks outranks the one that came with the spool", "[lane][resolver]") {
    // A colour the user picks is its own record, not an edit of the spool's.
    // The binding is untouched, so the spool link and the brand survive a pick
    // that changes only the colour.
    helix::ams::LaneSources lane;

    Observation spool(ObservationSource::Spoolman);
    spool.spoolman_id = 7;
    spool.brand = "Kingroon";
    spool.color_rgb = 0xFFFFFF;
    spool.color_name = "Arctic White";
    lane.apply(spool);

    REQUIRE(helix::ams::resolve(lane).color_rgb == 0xFFFFFF);
    REQUIRE(helix::ams::resolve(lane).color_name == "Arctic White");

    SECTION("a pick with a name uses it") {
        Observation picked(ObservationSource::LocalUser);
        picked.color_rgb = 0xBCBCBC;
        picked.color_name = "Concrete Gray";
        lane.apply(picked);

        const auto r = helix::ams::resolve(lane);
        CHECK(r.spoolman_id == 7);
        CHECK(r.brand == "Kingroon");
        CHECK(r.color_rgb == 0xBCBCBC);
        CHECK(r.color_name == "Concrete Gray");
    }

    SECTION("a pick with no name clears the spool's rather than keeping it") {
        // The swatch changed and the label named on it did not observe a
        // name, so the spool's name would contradict the new swatch.
        Observation picked(ObservationSource::LocalUser);
        picked.color_rgb = 0xBCBCBC;
        lane.apply(picked);

        const auto r = helix::ams::resolve(lane);
        CHECK(r.spoolman_id == 7);
        CHECK(r.brand == "Kingroon");
        CHECK(r.color_rgb == 0xBCBCBC);
        CHECK(r.color_name.empty());
    }
}

TEST_CASE("A sensor that reports no presence reading is not a present lane", "[lane][resolver]") {
    // Three states are distinct and only one of them means occupied: no sensor
    // record at all, a record that observed something other than presence, and
    // a record that observed presence. Collapsing the middle one into "present"
    // is how a lane with a live sensor but no reading resurrects.
    helix::ams::LaneSources lane;

    Observation sensed(ObservationSource::Sensed);
    sensed.color_rgb = 0xED2C2C;
    REQUIRE_FALSE(sensed.present.has_value());
    lane.apply(sensed);

    REQUIRE(lane.sensed.has_value());
    CHECK_FALSE(helix::ams::resolve(lane).present);
}

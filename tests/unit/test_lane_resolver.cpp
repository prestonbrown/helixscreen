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
    spool.material = "PETG";
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

    // Re-applying a source REPLACES that source's record, whole: a field the
    // newer observation says nothing about does not survive from the old one.
    Observation newer(ObservationSource::Spoolman);
    newer.color_rgb = 0x00FF00;
    lane.apply(newer);
    CHECK(lane.spoolman->color_rgb == 0x00FF00);
    CHECK_FALSE(lane.spoolman->material.has_value());
    CHECK(lane.sensed->present == false);

    // Applying a VendorCache observation touches only the vendor_cache slot:
    // there is no shared destination, so no writer can reach another
    // source's field.
    Observation cache(ObservationSource::Firmware);
    cache.color_rgb = 0x112233;
    lane.apply(cache);
    REQUIRE(lane.firmware.has_value());
    CHECK(lane.firmware->color_rgb == 0x112233);
    CHECK(lane.spoolman->color_rgb == 0x00FF00);

    // Dropping one source leaves every other source's record standing: this
    // pins that a lane's sources are independent, not slots into one shared
    // struct a clear could cross-contaminate.
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
        Observation cache(ObservationSource::Firmware);
        cache.color_rgb = 0x8000FF;
        cache.material = "PLA";
        lane.apply(cache);

        Observation spool(ObservationSource::Spoolman);
        spool.spoolman_id = 7;
        spool.material = "PETG";
        lane.apply(spool);

        // No sensor spoke, so there is no presence reading to report. That is
        // a third answer, distinct from both "occupied" and "empty".
        CHECK_FALSE(helix::ams::resolve(lane).present.has_value());
    }

    SECTION("the sensor decides, in both directions") {
        Observation sensed(ObservationSource::Sensed);
        sensed.present = true;
        lane.apply(sensed);
        CHECK(helix::ams::resolve(lane).present == true);

        sensed.present = false;
        lane.apply(sensed);
        // Engaged and false: the sensor looked and found the lane empty, which
        // is a reading and not the absence of one.
        CHECK(helix::ams::resolve(lane).present == false);
    }
}

TEST_CASE("A docked toolhead is a reading of its own, never a filament presence",
          "[lane][resolver]") {
    helix::ams::LaneSources lane;

    // A physical tool changer senses a toolhead in the dock and nothing about
    // the filament in it, so the dock answer must reach a consumer as itself:
    // filing it as presence would make every docked tool read as a loaded bay.
    Observation sensed(ObservationSource::Sensed);
    sensed.tool_docked = true;
    lane.apply(sensed);

    const auto r = helix::ams::resolve(lane);
    CHECK(r.tool_docked == true);
    CHECK_FALSE(r.present.has_value());

    sensed.tool_docked = false;
    lane.apply(sensed);
    CHECK(helix::ams::resolve(lane).tool_docked == false);
}

TEST_CASE("A weight refresh cannot disturb presence or identity", "[lane][resolver]") {
    // A Metered observation reaches only the weight fields of its own record,
    // so no number of weight writes can reach presence or identity. There is no
    // shared destination for them to pass through.
    helix::ams::LaneSources lane;

    Observation sensed(ObservationSource::Sensed);
    sensed.present = false;
    lane.apply(sensed);

    Observation cache(ObservationSource::Firmware);
    cache.material = "PETG";
    cache.color_rgb = 0xED2C2C;
    lane.apply(cache);

    const auto before = helix::ams::resolve(lane);
    // Engaged and false. Spelled against the value rather than the optional,
    // because REQUIRE_FALSE on an optional<bool> asks whether anything was
    // observed and would pass just as happily on a lane nobody sensed.
    REQUIRE(before.present == false);

    for (int i = 0; i < 100; ++i) {
        Observation weight(ObservationSource::Metered);
        weight.remaining_weight_g = static_cast<float>(200 - i);
        lane.apply(weight);

        const auto after = helix::ams::resolve(lane);
        REQUIRE(after.present == false);
        REQUIRE(after.material == "PETG");
        REQUIRE(after.color_rgb == 0xED2C2C);
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
    const auto r = helix::ams::resolve(lane);
    CHECK_FALSE(r.present.has_value());
    // A sensed reading carries no identity: presence is sensed, identity is
    // declared, and this colour never reaches the resolved lane. Unobserved
    // rather than grey, so a reader can leave the backend's colour alone.
    CHECK_FALSE(r.color_rgb.has_value());
}

namespace {

Observation from(ObservationSource source) {
    return Observation(source);
}

} // namespace

TEST_CASE("Identity ranks a pending edit over firmware over the record over Spoolman",
          "[lane][resolver]") {
    helix::ams::LaneSources lane;
    Observation spool = from(ObservationSource::Spoolman);
    spool.spoolman_id = 7;
    spool.material = "SPOOL";
    spool.brand = "SpoolBrand";
    spool.spool_name = "SpoolName";
    spool.color_name = "SpoolColour";
    lane.apply(spool);
    Observation record = from(ObservationSource::Record);
    record.spoolman_id = 7;
    record.material = "RECORD";
    record.brand = "RecordBrand";
    record.spool_name = "RecordName";
    lane.apply(record);
    Observation firmware = from(ObservationSource::Firmware);
    firmware.material = "FIRMWARE";
    firmware.brand = "FirmwareBrand";
    lane.apply(firmware);
    Observation pending = from(ObservationSource::Pending);
    pending.material = "PENDING";
    lane.apply(pending);

    const auto r = helix::ams::resolve(lane);
    CHECK(r.material == "PENDING");
    CHECK(r.brand == "FirmwareBrand");
    CHECK(r.spool_name == "RecordName");
    // Spoolman fills only what nobody else states.
    CHECK(r.color_name == "SpoolColour");

    lane.drop(ObservationSource::Pending);
    CHECK(helix::ams::resolve(lane).material == "FIRMWARE");
    lane.drop(ObservationSource::Firmware);
    CHECK(helix::ams::resolve(lane).material == "RECORD");
    lane.drop(ObservationSource::Record);
    // With nothing linking spool 7, its details describe no spool on the lane.
    CHECK_FALSE(helix::ams::resolve(lane).material.has_value());
}

TEST_CASE("The link is the strongest of pending, firmware and record", "[lane][resolver]") {
    helix::ams::LaneSources lane;
    CHECK(helix::ams::linked_spool_id(lane) == 0);

    Observation record = from(ObservationSource::Record);
    record.spoolman_id = 3;
    lane.apply(record);
    CHECK(helix::ams::linked_spool_id(lane) == 3);

    Observation firmware = from(ObservationSource::Firmware);
    firmware.spoolman_id = 5;
    lane.apply(firmware);
    CHECK(helix::ams::linked_spool_id(lane) == 5);

    // An unlink in flight is a link to nothing, whatever firmware still says.
    Observation unlink = from(ObservationSource::Pending);
    unlink.spoolman_id = 0;
    lane.apply(unlink);
    CHECK(helix::ams::linked_spool_id(lane) == 0);
    CHECK(helix::ams::resolve(lane).spoolman_id == 0);

    // Spoolman's own record never makes a link.
    helix::ams::LaneSources only_spoolman;
    Observation spool = from(ObservationSource::Spoolman);
    spool.spoolman_id = 9;
    only_spoolman.apply(spool);
    CHECK(helix::ams::linked_spool_id(only_spoolman) == 0);
}

TEST_CASE("Spoolman's details count only for the spool the lane links", "[lane][resolver]") {
    helix::ams::LaneSources lane;
    Observation firmware = from(ObservationSource::Firmware);
    firmware.spoolman_id = 42;
    lane.apply(firmware);
    Observation spool = from(ObservationSource::Spoolman);
    spool.spoolman_id = 42;
    spool.brand = "Polymaker";
    spool.remaining_weight_g = 500.0F;
    lane.apply(spool);
    CHECK(helix::ams::resolve(lane).brand == "Polymaker");
    CHECK(helix::ams::resolve(lane).remaining_weight_g == Catch::Approx(500.0F));

    // Firmware moves the lane to another spool: the old details stop filling it.
    firmware.spoolman_id = 43;
    lane.apply(firmware);
    const auto r = helix::ams::resolve(lane);
    CHECK_FALSE(r.brand.has_value());
    CHECK_FALSE(r.remaining_weight_g.has_value());
    CHECK(r.spoolman_id == 43);
}

TEST_CASE("Weight ranks pending, Spoolman, firmware, record, then the meter", "[lane][resolver]") {
    helix::ams::LaneSources lane;
    const auto weight = [&lane]() { return *helix::ams::resolve(lane).remaining_weight_g; };

    Observation metered = from(ObservationSource::Metered);
    metered.remaining_weight_g = 100.0F;
    lane.apply(metered);
    CHECK(weight() == Catch::Approx(100.0F));

    Observation record = from(ObservationSource::Record);
    record.remaining_weight_g = 200.0F;
    record.spoolman_id = 1;
    lane.apply(record);
    CHECK(weight() == Catch::Approx(200.0F));

    Observation firmware = from(ObservationSource::Firmware);
    firmware.remaining_weight_g = 300.0F;
    lane.apply(firmware);
    CHECK(weight() == Catch::Approx(300.0F));

    Observation spool = from(ObservationSource::Spoolman);
    spool.spoolman_id = 1;
    spool.remaining_weight_g = 400.0F;
    lane.apply(spool);
    CHECK(weight() == Catch::Approx(400.0F));

    Observation pending = from(ObservationSource::Pending);
    pending.remaining_weight_g = 500.0F;
    lane.apply(pending);
    CHECK(weight() == Catch::Approx(500.0F));
}

TEST_CASE("A value observed as empty blanks what a weaker source knows", "[lane][resolver]") {
    helix::ams::LaneSources lane;
    Observation record = from(ObservationSource::Record);
    record.brand = "Kingroon";
    record.spool_name = "Kingroon Basic PETG";
    lane.apply(record);
    Observation pending = from(ObservationSource::Pending);
    pending.brand = "";
    lane.apply(pending);
    const auto r = helix::ams::resolve(lane);
    CHECK(r.brand == "");
    CHECK(r.spool_name == "Kingroon Basic PETG");
}

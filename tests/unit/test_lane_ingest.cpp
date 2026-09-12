// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "helix_test_fixture.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "test_helpers/log_capture.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using helix::ams::classify_declaration;
using helix::ams::declared_from_record;
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
    using helix::ams::LANES_PER_BACKEND;

    // Two backends is the ordinary case, not an exotic one: a tool changer
    // beside a filament system is what makes a bare slot index wrong.
    CHECK(lane_id_for(0, 0) == 0);
    CHECK(lane_id_for(0, 3) == 3);
    CHECK(lane_id_for(1, 0) != lane_id_for(0, 0));
    CHECK(lane_id_for(1, 0) == LANES_PER_BACKEND);

    // The last slot of one block never collides with the first of the next.
    CHECK(lane_id_for(0, LANES_PER_BACKEND - 1) < lane_id_for(1, 0));

    // The printer-level ids sit clear of every backend block.
    CHECK(helix::ams::FIRST_TOOL_LANE_ID > helix::ams::BYPASS_LANE_ID);

    // The last backend and slot this scheme supports still sits below the
    // bypass id, pinning the boundary MAX_BACKENDS exists to hold.
    CHECK(lane_id_for(helix::ams::MAX_BACKENDS - 1, LANES_PER_BACKEND - 1) <
          helix::ams::BYPASS_LANE_ID);
}

TEST_CASE("a backend block is sized for hardware, not for the subject array", "[lane][ingest]") {
    using helix::ams::lane_id_for;

    // AFC reports one lane per unit it finds and Happy Hare one per gate, both
    // uncapped by AmsState::MAX_SLOTS, which bounds only how many slots get
    // subjects. A five-unit BoxTurtle and a twenty-gate MMU are the shipped
    // hardware that exceeds it, and both must still address a lane of their own.
    CHECK(helix::ams::LANES_PER_BACKEND > 20);
    CHECK(lane_id_for(0, 20) != helix::ams::INVALID_LANE_ID);
    CHECK(lane_id_for(0, 20) != lane_id_for(1, 0));
}

TEST_CASE("a pair that names no lane yields no id", "[lane][ingest]") {
    using helix::ams::INVALID_LANE_ID;
    using helix::ams::lane_id_for;

    // A backend registration never reached leaves its index at -1, and a slot
    // index past the block is the neighbouring backend's slot. Neither may
    // resolve to an id: the nearest one is a real lane on a real backend, so
    // nothing downstream could tell the record apart from a deliberate write.
    CHECK(lane_id_for(-1, 0) == INVALID_LANE_ID);
    CHECK(lane_id_for(helix::ams::MAX_BACKENDS, 0) == INVALID_LANE_ID);
    CHECK(lane_id_for(0, -1) == INVALID_LANE_ID);
    CHECK(lane_id_for(0, helix::ams::LANES_PER_BACKEND) == INVALID_LANE_ID);

    CHECK_FALSE(helix::ams::is_lane_id(INVALID_LANE_ID));
    CHECK(helix::ams::is_lane_id(0));
}

TEST_CASE("only an id the scheme assigns is a lane", "[lane][ingest]") {
    using helix::ams::BYPASS_LANE_ID;
    using helix::ams::END_LANE_ID;
    using helix::ams::FIRST_TOOL_LANE_ID;
    using helix::ams::is_lane_id;
    using helix::ams::LANES_PER_BACKEND;
    using helix::ams::MAX_BACKENDS;

    constexpr helix::ams::LaneId END_OF_BLOCKS = MAX_BACKENDS * LANES_PER_BACKEND;

    // Both ends of each of the three ranges the scheme assigns.
    CHECK(is_lane_id(0));
    CHECK(is_lane_id(END_OF_BLOCKS - 1));
    CHECK(is_lane_id(BYPASS_LANE_ID));
    CHECK(is_lane_id(FIRST_TOOL_LANE_ID));
    CHECK(is_lane_id(END_LANE_ID - 1));

    // One past each end, both ends of the gap between the last backend block
    // and the bypass, and an arbitrary large integer. A positive value is not
    // a lane merely for being positive: no backend, bypass or tool owns any
    // of these, so a record filed on one would describe nothing at all.
    CHECK_FALSE(is_lane_id(-1));
    CHECK_FALSE(is_lane_id(helix::ams::INVALID_LANE_ID));
    CHECK_FALSE(is_lane_id(END_OF_BLOCKS));
    CHECK_FALSE(is_lane_id(BYPASS_LANE_ID - 1));
    CHECK_FALSE(is_lane_id(BYPASS_LANE_ID + 1));
    CHECK_FALSE(is_lane_id(FIRST_TOOL_LANE_ID - 1));
    CHECK_FALSE(is_lane_id(END_LANE_ID));
    CHECK_FALSE(is_lane_id(1000000));

    // Everything lane_id_for produces is an id this admits, at the far corner.
    CHECK(is_lane_id(helix::ams::lane_id_for(MAX_BACKENDS - 1, LANES_PER_BACKEND - 1)));
}

TEST_CASE_METHOD(HelixTestFixture, "the store cannot grow past the ids the scheme assigns",
                 "[lane][ingest]") {
    Observation obs(ObservationSource::Sensed);
    obs.present = true;

    // Each rejected id logs, and this offers a great many of them.
    const auto restore_level = spdlog::default_logger()->level();
    spdlog::set_level(spdlog::level::critical);

    // Ids no backend, bypass or tool can own. "Bounded by construction" is
    // only true if the funnels refuse these: a backend deriving an id wrongly
    // in a later plan would otherwise grow the map for as long as it polls.
    for (helix::ams::LaneId lane = helix::ams::END_LANE_ID; lane < helix::ams::END_LANE_ID + 200000;
         ++lane) {
        ingest(lane, obs);
    }

    // The gap between the last backend block and the bypass is the same
    // question in the range a miscomputed backend id would land in.
    for (helix::ams::LaneId lane = helix::ams::MAX_BACKENDS * helix::ams::LANES_PER_BACKEND;
         lane < helix::ams::BYPASS_LANE_ID; ++lane) {
        ingest(lane, obs);
    }

    spdlog::set_level(restore_level);

    CHECK(helix::ams::known_lanes().empty());

    // The three ranges that are lanes still write, so the refusal above is
    // selective rather than a funnel that stopped working.
    ingest(0, obs);
    ingest(helix::ams::BYPASS_LANE_ID, obs);
    ingest(helix::ams::FIRST_TOOL_LANE_ID, obs);
    CHECK(helix::ams::known_lanes().size() == 3);
    CHECK(static_cast<int>(helix::ams::known_lanes().size()) <= helix::ams::MAX_LANES);
}

TEST_CASE_METHOD(HelixTestFixture, "a funnel handed no lane writes nothing", "[lane][ingest]") {
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(helix::ams::INVALID_LANE_ID, sensed);

    Observation user(ObservationSource::LocalUser);
    user.color_rgb = 0xBCBCBC;
    helix::ams::commit_slot_edit(helix::ams::INVALID_LANE_ID, user);

    // Not a clamp onto lane 0, and not a record filed under the id itself.
    CHECK(helix::ams::known_lanes().empty());
    CHECK_FALSE(lane_sources(0).sensed.has_value());
    CHECK_FALSE(lane_sources(0).local_user.has_value());
    CHECK_FALSE(lane_sources(helix::ams::INVALID_LANE_ID).sensed.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "a dropped lane is reported once, and again when the id changes",
                 "[lane][ingest]") {
    helix::LogCapture log;

    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;

    // A producer filing through a backend that has no index yet reaches this
    // three times per lane per frame, and the id is the whole content of the
    // message, so the second and third repeat tell a reader nothing.
    ingest(helix::ams::INVALID_LANE_ID, sensed);
    ingest(helix::ams::INVALID_LANE_ID, sensed);
    ingest(helix::ams::INVALID_LANE_ID, sensed);
    CHECK(log.count_containing("names no position") == 1);

    // A different id is a different fact and speaks for itself.
    ingest(helix::ams::END_LANE_ID, sensed);
    CHECK(log.count_containing("names no position") == 2);
}

TEST_CASE("a lane colour string reads as a value, a clear or nothing", "[lane][ingest]") {
    using helix::ams::ColorReadingKind;
    using helix::ams::read_lane_color;

    SECTION("a colour, however the producer spells it") {
        CHECK(read_lane_color("#ED2C2C").kind == ColorReadingKind::Observed);
        CHECK(read_lane_color("#ED2C2C").rgb == 0xED2C2Cu);
        CHECK(read_lane_color("ED2C2C").rgb == 0xED2C2Cu);
        CHECK(read_lane_color("0xED2C2C").rgb == 0xED2C2Cu);
        CHECK(read_lane_color("ed2c2c").rgb == 0xED2C2Cu);
        // Pure black is a colour a spool can be, not a failure.
        CHECK(read_lane_color("#000000").kind == ColorReadingKind::Observed);
        CHECK(read_lane_color("#000000").rgb == 0x000000u);
    }

    SECTION("the short form expands rather than reading as a near-black") {
        CHECK(read_lane_color("#F00").kind == ColorReadingKind::Observed);
        CHECK(read_lane_color("#F00").rgb == 0xFF0000u);
    }

    SECTION("a slicer's 8-digit form drops alpha rather than carrying it") {
        CHECK(read_lane_color("#800080FF").kind == ColorReadingKind::Observed);
        CHECK(read_lane_color("#800080FF").rgb == 0x800080u);
    }

    SECTION("nothing but a prefix is the producer clearing the lane") {
        CHECK(read_lane_color("").kind == ColorReadingKind::Cleared);
        CHECK(read_lane_color("#").kind == ColorReadingKind::Cleared);
        CHECK(read_lane_color("  ").kind == ColorReadingKind::Cleared);
        CHECK(read_lane_color(" # ").kind == ColorReadingKind::Cleared);
    }

    SECTION("a value that is not a colour is no reading, which is not a clear") {
        // Each of these has a reading a bare std::stoul would hand back: a
        // partial parse of the head, or a negation. None of them is what the
        // producer meant.
        CHECK(read_lane_color("#zzzzzz").kind == ColorReadingKind::NoReading);
        CHECK(read_lane_color("FF0000junk").kind == ColorReadingKind::NoReading);
        CHECK(read_lane_color("-1").kind == ColorReadingKind::NoReading);
        CHECK(read_lane_color("beef").kind == ColorReadingKind::NoReading);
        CHECK(read_lane_color("None").kind == ColorReadingKind::NoReading);
    }
}

TEST_CASE("the blocks are adjacent, which is why a slot index is bounded", "[lane][ingest]") {
    using helix::ams::lane_id_for;
    using helix::ams::LANES_PER_BACKEND;

    // No gap between one block's last id and the next block's first. That is
    // what makes lane_id_for's slot_index bound load-bearing rather than
    // defensive: a slot index one past a block is not an unused id, it is the
    // neighbouring backend's slot 0, and every index past that is one of its
    // real slots.
    CHECK(lane_id_for(0, LANES_PER_BACKEND - 1) + 1 == lane_id_for(1, 0));
    CHECK(lane_id_for(3, LANES_PER_BACKEND - 1) + 1 == lane_id_for(4, 0));

    // The id a bounds violation would have produced belongs to a real slot on
    // a real backend, so nothing downstream could tell it apart.
    CHECK(lane_id_for(0, 0) + (6 * LANES_PER_BACKEND + 3) == lane_id_for(6, 3));
}

TEST_CASE_METHOD(HelixTestFixture, "commit_slot_edit refuses a source that is not the user",
                 "[lane][ingest]") {
    Observation cache(ObservationSource::VendorCache);
    cache.color_rgb = 0xED2C2C;
    helix::ams::commit_slot_edit(5, cache);

    // The two funnels take the same arguments and mean opposite things, so the
    // source check is what stops a backend reaching for the amending one and
    // becoming a third writer of a record the user owns. It returns void, so a
    // caller cannot tell a drop from a write; the lane is where that shows.
    const auto lane = lane_sources(5);
    CHECK_FALSE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.local_user.has_value());
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "ingest refuses a LocalUser observation and leaves the user's record alone",
                 "[lane][ingest]") {
    Observation declared(ObservationSource::LocalUser);
    declared.color_rgb = 0xBCBCBC;
    helix::ams::commit_slot_edit(6, declared);

    const auto before = lane_sources(6);
    REQUIRE(before.local_user.has_value());
    CHECK(before.local_user->color_rgb == 0xBCBCBC);

    // ingest() replaces whole-record, so a LocalUser observation reaching it
    // would destroy the user's declaration rather than merely fail to amend
    // it. The colour differs from the one above so a silent pass-through
    // shows up as a changed value, not a coincidental match.
    Observation impostor(ObservationSource::LocalUser);
    impostor.color_rgb = 0x000000;
    ingest(6, impostor);

    const auto after = lane_sources(6);
    REQUIRE(after.local_user.has_value());
    CHECK(after.local_user->color_rgb == 0xBCBCBC);
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

namespace {
/// A lane_data record as it arrives off the wire, so the lock keys are present
/// or absent exactly as a co-author or a legacy write left them.
helix::ams::FilamentSlotOverride record_from(const nlohmann::json& j) {
    auto parsed = helix::ams::from_lane_data_record(j);
    REQUIRE(parsed.has_value());
    return parsed->second;
}
} // namespace

TEST_CASE("a linked record is the server's declaration, locks unread", "[lane][ingest]") {
    const nlohmann::json wire = {
        {"lane", "0"},
        {"color", "#A4B2BC"},
        {"spool_id", 7},
        {"helix_locked_color", true},
        {"helix_locked_material", true},
        {"material", "PETG"},
    };
    const auto rec = record_from(wire);

    CHECK(classify_declaration(rec, wire) == ObservationSource::Spoolman);

    const auto obs = declared_from_record(rec, wire);
    CHECK(obs.source == ObservationSource::Spoolman);
    CHECK(obs.color_rgb == 0xA4B2BC);
    CHECK(obs.spoolman_id == 7);
}

TEST_CASE("an unlinked record with a real lock is the user's declaration", "[lane][ingest]") {
    const nlohmann::json wire = {
        {"lane", "1"},
        {"color", "#BCBCBC"},
        {"helix_locked_color", true},
    };
    const auto rec = record_from(wire);

    CHECK(classify_declaration(rec, wire) == ObservationSource::LocalUser);
    // The observation has to carry the same verdict, not merely the colour: a
    // classifier that files a person's locked colour under VendorCache is the
    // stale-cache-reads-as-a-choice failure this model exists to delete.
    CHECK(declared_from_record(rec, wire).source == ObservationSource::LocalUser);
    CHECK(declared_from_record(rec, wire).color_rgb == 0xBCBCBC);
}

TEST_CASE("an unlinked record with no lock key is a cache, not a user", "[lane][ingest]") {
    // The load default reads a missing helix_locked_color back as color_set,
    // so a legacy record carrying a colour arrives looking locked. Only a key
    // that is actually present is a human's signature.
    const nlohmann::json wire = {
        {"lane", "2"},
        {"color", "#ED2C2C"},
    };
    const auto rec = record_from(wire);
    REQUIRE(rec.user_locked_color); // the load default, not a declaration

    CHECK(classify_declaration(rec, wire) == ObservationSource::VendorCache);
    CHECK(declared_from_record(rec, wire).source == ObservationSource::VendorCache);
}

TEST_CASE("a record with a zero spool id is unlinked", "[lane][ingest]") {
    const nlohmann::json wire = {
        {"lane", "3"},
        {"color", "#000000"},
        {"spool_id", 0},
        {"helix_locked_color", true},
    };
    const auto rec = record_from(wire);

    CHECK(classify_declaration(rec, wire) == ObservationSource::LocalUser);
    // Pure black survives the round trip. It is a colour, not an absent one.
    CHECK(declared_from_record(rec, wire).color_rgb == 0x000000u);
}

TEST_CASE("a record with no colour does not claim one", "[lane][ingest]") {
    const nlohmann::json wire = {
        {"lane", "0"},
        {"material", "PLA"},
    };
    const auto rec = record_from(wire);

    // The record's own default reads user_locked_material as true (material
    // is non-empty), but the wire carries no lock key at all: the classifier
    // must side with the wire, not the struct's legacy-preservation default.
    CHECK(classify_declaration(rec, wire) == ObservationSource::VendorCache);

    const auto obs = declared_from_record(rec, wire);
    CHECK(obs.material == "PLA");
    CHECK_FALSE(obs.color_rgb.has_value());
}

TEST_CASE("a record carrying the default-slot sentinel does not declare a colour",
          "[lane][ingest]") {
    // "#808080" round-trips through from_lane_data_record with color_set true
    // and color_rgb == AMS_DEFAULT_SLOT_COLOR, indistinguishable in the struct
    // from a real grey. The sentinel means "no colour reading" everywhere else
    // it is read, and this is the third place that has to honour that.
    const nlohmann::json wire = {
        {"lane", "4"},
        {"color", "#808080"},
    };
    const auto rec = record_from(wire);
    REQUIRE(rec.color_set);
    REQUIRE(rec.color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);

    const auto obs = declared_from_record(rec, wire);
    CHECK_FALSE(obs.color_rgb.has_value());
}

TEST_CASE("a record carrying only material observes nothing else", "[lane][ingest]") {
    // Every other field on FilamentSlotOverride defaults to something that
    // looks like a value (empty string, 0, -1.0f): an absent field and a
    // field declared empty/zero must read as two different statements, or
    // Observation's whole "nullopt means not observed" contract is void.
    const nlohmann::json wire = {
        {"lane", "6"},
        {"material", "PLA"},
    };
    const auto rec = record_from(wire);
    const auto obs = declared_from_record(rec, wire);

    REQUIRE(obs.material.has_value());
    CHECK(*obs.material == "PLA");
    CHECK_FALSE(obs.color_rgb.has_value());
    CHECK_FALSE(obs.color_name.has_value());
    CHECK_FALSE(obs.brand.has_value());
    CHECK_FALSE(obs.spool_name.has_value());
    CHECK_FALSE(obs.catalog_id.has_value());
    CHECK_FALSE(obs.product_name.has_value());
    CHECK_FALSE(obs.spoolman_id.has_value());
    CHECK_FALSE(obs.spoolman_vendor_id.has_value());
    CHECK_FALSE(obs.remaining_weight_g.has_value());
    CHECK_FALSE(obs.total_weight_g.has_value());
}

TEST_CASE("a fully populated record observes every field it carries", "[lane][ingest]") {
    const nlohmann::json wire = {
        {"lane", "7"},
        {"color", "#112233"},
        {"color_name", "Galaxy Black"},
        {"material", "ABS"},
        {"vendor", "Sunlu"},
        {"spool_name", "Reel 5"},
        {"helix_catalog_id", "cat-42"},
        {"helix_product_name", "ABS Marble"},
        {"spoolman_vendor_id", 3},
        {"remaining_weight_g", 512.0},
        {"total_weight_g", 1000.0},
    };
    const auto rec = record_from(wire);
    const auto obs = declared_from_record(rec, wire);

    REQUIRE(obs.color_rgb.has_value());
    CHECK(*obs.color_rgb == 0x112233u);
    REQUIRE(obs.color_name.has_value());
    CHECK(*obs.color_name == "Galaxy Black");
    REQUIRE(obs.material.has_value());
    CHECK(*obs.material == "ABS");
    REQUIRE(obs.brand.has_value());
    CHECK(*obs.brand == "Sunlu");
    REQUIRE(obs.spool_name.has_value());
    CHECK(*obs.spool_name == "Reel 5");
    REQUIRE(obs.catalog_id.has_value());
    CHECK(*obs.catalog_id == "cat-42");
    REQUIRE(obs.product_name.has_value());
    CHECK(*obs.product_name == "ABS Marble");
    REQUIRE(obs.spoolman_vendor_id.has_value());
    CHECK(*obs.spoolman_vendor_id == 3);
    REQUIRE(obs.remaining_weight_g.has_value());
    CHECK(*obs.remaining_weight_g == Catch::Approx(512.0f));
    REQUIRE(obs.total_weight_g.has_value());
    CHECK(*obs.total_weight_g == Catch::Approx(1000.0f));
}

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES; a SlotInfo read appears only where one is
// needed to establish a case's precondition.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"
#include "test_helpers/ad5x_ifs_test_access.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/registered_backend.h"

#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AfcTestAccess;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendAfc;
using helix::AmsBackendHappyHare;
using helix::HappyHareTestAccess;
using helix::ams::lane_sources;
using helix::test::RegisteredBackend;

namespace {
/// A backend with no Moonraker behind it, registered so its lane ids are real.
using Ad5xHarness = RegisteredBackend<AmsBackendAd5xIfs>;
using AfcHarness = RegisteredBackend<AmsBackendAfc>;
using HappyHareHarness = RegisteredBackend<AmsBackendHappyHare>;

/// Four lanes through AFC's own initialize_slots(), which is what a discovery
/// answer ends in.
void init_afc_lanes(AmsBackendAfc& backend) {
    AfcTestAccess::initialize_slots(backend,
                                    std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
}

/// One AFC lane_data payload, the Moonraker DB snapshot, keyed by lane name.
void feed_afc_lane_data(AmsBackendAfc& backend, const nlohmann::json& lane_data) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(backend));
    AfcTestAccess::parse_lane_data(backend, lane_data);
}

/// One AFC_stepper lane object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Every case here
/// drives the production path rather than the parse alone, because what makes
/// AFC different is that its frames are deltas.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

/// One printer.mmu object, delivered the way Moonraker delivers it: a
/// notify_status_update carrying only the keys that changed. Happy Hare's
/// frames are deltas, so every case here drives the production entry point
/// rather than the parse alone.
void feed_mmu(AmsBackendHappyHare& backend, const nlohmann::json& mmu) {
    nlohmann::json params;
    params["mmu"] = mmu;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    HappyHareTestAccess::handle_status_update(backend, notification);
}
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

TEST_CASE_METHOD(LVGLTestFixture, "AFC splits its sensors, its own store and its own weight",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"load", true},
                   {"tool_loaded", false},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"},
                   {"spool_vendor", "Kingroon"},
                   {"weight", 612.0},
                   {"spool_id", 7},
                   {"initial_weight", 1000.0}});

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);
    // prep and load are real sensors and say nothing about which spool this is.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    // AFC's own store keeps colour, material, filament name and vendor across
    // an eject, so all four are a cache of a past declaration.
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(lane.vendor_cache->present.has_value());

    REQUIRE(lane.metered.has_value());
    REQUIRE(lane.metered->remaining_weight_g.has_value());
    CHECK(*lane.metered->remaining_weight_g == Catch::Approx(612.0F));
    REQUIRE(lane.metered->total_weight_g.has_value());
    CHECK(*lane.metered->total_weight_g == Catch::Approx(1000.0F));
    // A weight tick has no business asserting identity or presence.
    CHECK_FALSE(lane.metered->material.has_value());
    CHECK_FALSE(lane.metered->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial AFC delta narrows nothing it does not mention",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // The commonest frame AFC sends: the lane reached the toolhead and nothing
    // about the spool changed. A record assembled from this frame alone would
    // leave the lane with no identity at all.
    feed_afc_lane(*harness, "lane2", {{"status", "Tooled"}});

    const auto after_status = lane_sources(harness.lane(1));
    REQUIRE(after_status.vendor_cache.has_value());
    REQUIRE(after_status.vendor_cache->color_rgb.has_value());
    CHECK(*after_status.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(after_status.vendor_cache->material == "PETG");
    CHECK(after_status.vendor_cache->spool_name == "Galaxy Black");
    REQUIRE(after_status.sensed.has_value());
    CHECK(after_status.sensed->present == true);

    // A colour change alone must not take the material with it.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}});

    const auto after_color = lane_sources(harness.lane(1));
    REQUIRE(after_color.vendor_cache.has_value());
    REQUIRE(after_color.vendor_cache->color_rgb.has_value());
    CHECK(*after_color.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(after_color.vendor_cache->material == "PETG");
    CHECK(after_color.vendor_cache->spool_name == "Galaxy Black");
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC files no weight reading until a frame carries one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane4", {{"prep", true}, {"status", "Loaded"}});

    const auto lane = lane_sources(harness.lane(3));
    // The record is filed whether or not AFC has weighed anything, so the
    // absence below is a reading AFC has not made rather than a translation
    // that never ran.
    REQUIRE(lane.metered.has_value());
    CHECK_FALSE(lane.metered->remaining_weight_g.has_value());
    CHECK_FALSE(lane.metered->total_weight_g.has_value());

    feed_afc_lane(*harness, "lane4", {{"weight", 612.0}});

    const auto weighed = lane_sources(harness.lane(3));
    REQUIRE(weighed.metered.has_value());
    REQUIRE(weighed.metered->remaining_weight_g.has_value());
    CHECK(*weighed.metered->remaining_weight_g == Catch::Approx(612.0F));
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's eject clears its own store without clearing the sensor",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane3",
                  {{"prep", true},
                   {"load", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});
    REQUIRE(lane_sources(harness.lane(2)).vendor_cache->material == "PETG");

    // clear_values() empties colour, material and filament name on eject. An
    // empty value is a clear, so the cache must stop reporting them rather
    // than standing on the last frame that named them.
    feed_afc_lane(*harness, "lane3",
                  {{"prep", false},
                   {"load", false},
                   {"status", "None"},
                   {"color", ""},
                   {"material", ""},
                   {"filament_name", ""}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.sensed.has_value());
    CHECK(lane.sensed->present == false);
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
    CHECK_FALSE(lane.vendor_cache->spool_name.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AFC's vendor-cache record",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // A user colour and material that disagree with what AFC reports.
    // apply_overrides() rewrites SlotInfo with these on every frame, so a
    // translation reading that struct back would file the user's own choice as
    // something AFC's store remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    user.material = "ABS";
    AfcTestAccess::overrides(*harness)[1] = user;

    feed_afc_lane(
        *harness, "lane2",
        {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}, {"material", "PETG"}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for this case to catch
    // and both assertions below would hold for the wrong reason.
    const auto merged = harness->get_slot_info(1);
    REQUIRE(merged.color_rgb == 0x00FF00u);
    REQUIRE(merged.material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a frame with no sensor key neither sets nor erases AFC presence",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // Weight alone. AFC's sensors have said nothing about this lane, and no
    // sensor reading is not a sensor reading of "empty".
    feed_afc_lane(*harness, "lane1", {{"weight", 612.0}});

    const auto unread = lane_sources(harness.lane(0));
    // Proof the translation ran, so the absence below is about the sensors.
    REQUIRE(unread.metered.has_value());
    CHECK_FALSE(unread.sensed.has_value());

    // The same lane once the sensors do speak. This half is what makes the
    // half above a guard rather than a backend that never reports presence.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}});

    const auto read = lane_sources(harness.lane(0));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == true);

    // Another weight-only frame. AFC's presence authority is per frame rather
    // than a latch, and a record is written whole, so filing an empty one here
    // would erase a live reading.
    feed_afc_lane(*harness, "lane1", {{"weight", 600.0}});

    const auto still = lane_sources(harness.lane(0));
    REQUIRE(still.sensed.has_value());
    REQUIRE(still.sensed->present.has_value());
    CHECK(*still.sensed->present == true);
}

TEST_CASE_METHOD(LVGLTestFixture, "each AFC lane accumulates its own identity",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"filament_name", "Galaxy Black"}});

    // A second lane, saying nothing about any spool. One accumulator for the
    // backend instead of one per lane would hand it lane1's identity.
    feed_afc_lane(*harness, "lane2", {{"prep", false}, {"status", "None"}});

    const auto second = lane_sources(harness.lane(1));
    REQUIRE(second.vendor_cache.has_value());
    CHECK_FALSE(second.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(second.vendor_cache->material.has_value());
    CHECK_FALSE(second.vendor_cache->spool_name.has_value());

    const auto first = lane_sources(harness.lane(0));
    REQUIRE(first.vendor_cache.has_value());
    REQUIRE(first.vendor_cache->color_rgb.has_value());
    CHECK(*first.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(first.vendor_cache->material == "PETG");

    // The second lane's own spool, which must reach its own record and leave
    // the first lane's alone.
    feed_afc_lane(*harness, "lane2", {{"color", "#00AEFF"}, {"material", "PLA"}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a spool id AFC did not state is not filed as one",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    helix::ams::FilamentSlotOverride user;
    user.spoolman_id = 99;
    AfcTestAccess::overrides(*harness)[0] = user;

    // AFC unlinks the lane; the override re-supplies the user's id on the
    // merged slot, which is the precondition rather than the behaviour.
    feed_afc_lane(*harness, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", nullptr}});
    REQUIRE(harness->get_slot_info(0).spoolman_id == 99);
    REQUIRE_FALSE(lane_sources(harness.lane(0)).vendor_cache->spoolman_id.has_value());

    // A spool_id that is neither an integer nor null states nothing. Reading
    // the merged slot here would file the user's 99 as AFC's own word.
    feed_afc_lane(*harness, "lane1", {{"spool_id", "junk"}});

    const auto lane = lane_sources(harness.lane(0));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's DB snapshot files on the same lane as its status frames",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // lane_data alone. No status frame has run, so every value below is this
    // parser's work and nothing else's.
    feed_afc_lane_data(*harness, {{"lane3",
                                   {{"color", "#ED2C2C"},
                                    {"material", "PETG"},
                                    {"name", "Galaxy Black"},
                                    {"vendor_name", "Kingroon"},
                                    {"spool_id", 7}}}});

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
    CHECK(lane.vendor_cache->spool_name == "Galaxy Black");
    CHECK(lane.vendor_cache->brand == "Kingroon");
    CHECK(lane.vendor_cache->spoolman_id == 7);

    // A database record is not a sensor and does not weigh anything.
    CHECK_FALSE(lane.sensed.has_value());
    CHECK_FALSE(lane.metered.has_value());

    // A value the snapshot carries but we cannot read is not the snapshot
    // saying the lane has no colour, so the record keeps the last one it could
    // read. Only an empty value removes it.
    feed_afc_lane_data(*harness, {{"lane3", {{"color", "#zzzzzz"}}}});

    const auto unreadable = lane_sources(harness.lane(2));
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);

    feed_afc_lane_data(*harness, {{"lane3", {{"color", ""}}}});
    CHECK_FALSE(lane_sources(harness.lane(2)).vendor_cache->color_rgb.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AFC's two parsers accumulate into one account of a lane",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    // The subscription carries colour, material and weight on every AFC
    // version; the filament name reaches pre-v1.2.0 firmware only through the
    // DB snapshot. Neither parser may narrow the lane to its own half.
    feed_afc_lane(*harness, "lane1",
                  {{"prep", true},
                   {"status", "Loaded"},
                   {"color", "#ED2C2C"},
                   {"material", "PETG"},
                   {"weight", 612.0}});
    feed_afc_lane_data(*harness, {{"lane1", {{"name", "Galaxy Black"}}}});

    const auto joined = lane_sources(harness.lane(0));
    REQUIRE(joined.vendor_cache.has_value());
    REQUIRE(joined.vendor_cache->color_rgb.has_value());
    CHECK(*joined.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(joined.vendor_cache->material == "PETG");
    CHECK(joined.vendor_cache->spool_name == "Galaxy Black");

    // The snapshot said nothing about sensors or weight, so what the
    // subscription established still stands.
    REQUIRE(joined.sensed.has_value());
    CHECK(joined.sensed->present == true);
    REQUIRE(joined.metered.has_value());
    CHECK(*joined.metered->remaining_weight_g == Catch::Approx(612.0F));

    // An empty name in the snapshot is the clear AFC writes on eject, the same
    // answer the status path gives an empty filament_name.
    feed_afc_lane_data(*harness, {{"lane1", {{"name", ""}}}});

    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->spool_name.has_value());
    CHECK(cleared.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable colour leaves AFC's record standing",
                 "[lane][ingest][afc]") {
    AfcHarness harness(nullptr, nullptr);
    init_afc_lanes(*harness);

    feed_afc_lane(*harness, "lane2", {{"prep", true}, {"status", "Loaded"}, {"color", "#ED2C2C"}});
    REQUIRE(*lane_sources(harness.lane(1)).vendor_cache->color_rgb == 0xED2C2Cu);

    // "AFC published something we cannot read" and "AFC cleared the lane" are
    // different statements, and the record has to tell them apart or the lane
    // loses an identity nobody withdrew. The slot keeps its colour on screen;
    // the record keeps the same one.
    feed_afc_lane(*harness, "lane2", {{"color", "#zzzzzz"}});

    const auto unreadable = lane_sources(harness.lane(1));
    REQUIRE(unreadable.vendor_cache.has_value());
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(1).color_rgb == 0xED2C2Cu);

    // The clear is the other statement, and it does remove the colour.
    feed_afc_lane(*harness, "lane2", {{"color", ""}});

    const auto cleared = lane_sources(harness.lane(1));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(1).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare splits gate status from gate metadata",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "000000", "", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", "", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0, 0})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed.has_value());
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == true);
    // The sensor's record carries presence and nothing else: what the gate map
    // remembers is a cache of a past declaration, not something a gate sensed.
    CHECK_FALSE(gate0.sensed->color_rgb.has_value());
    CHECK_FALSE(gate0.sensed->material.has_value());

    REQUIRE(gate0.vendor_cache.has_value());
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->material == "PETG");
    CHECK(gate0.vendor_cache->spoolman_id == 7);
    CHECK_FALSE(gate0.vendor_cache->present.has_value());
    // The gate map carries no weight at all.
    CHECK_FALSE(gate0.metered.has_value());

    const auto gate1 = lane_sources(harness.lane(1));
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == false);
    // An empty gate still remembers what it last held. That memory is a cache,
    // and pure black in it is a colour like any other.
    REQUIRE(gate1.vendor_cache.has_value());
    REQUIRE(gate1.vendor_cache->color_rgb.has_value());
    CHECK(*gate1.vendor_cache->color_rgb == 0x000000u);
    // Happy Hare writes 0 for a gate with no spool, which is a clear rather
    // than a spool numbered zero.
    CHECK_FALSE(gate1.vendor_cache->spoolman_id.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare records no colour for a gate that reports none",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", ""})}});

    const auto gate1 = lane_sources(harness.lane(1));
    // Proof the path ran: presence came through for the same gate.
    REQUIRE(gate1.sensed.has_value());
    REQUIRE(gate1.sensed->present.has_value());
    CHECK(*gate1.sensed->present == true);
    REQUIRE(gate1.vendor_cache.has_value());
    CHECK_FALSE(gate1.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(gate1.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "a Happy Hare frame without metadata does not blank the cache",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})}});
    REQUIRE(lane_sources(harness.lane(0)).vendor_cache->material == "PETG");

    // A status-only delta states nothing about metadata, so it must not be
    // read as a gate that stopped reporting one.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({0, 1})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.sensed->present.has_value());
    CHECK(*gate0.sensed->present == false);
    CHECK(gate0.vendor_cache->material == "PETG");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "a partial Happy Hare delta narrows nothing it does not mention",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})},
                        {"gate_material", nlohmann::json::array({"PETG", "ABS"})},
                        {"gate_spool_id", nlohmann::json::array({7, 9})}});

    // MMU_GATE_MAP GATE=0 MATERIAL=PLA moves one array. Moonraker names only
    // what changed, so a record built from this frame alone would be a gate
    // whose colour and spool link had just vanished.
    feed_mmu(*harness, {{"gate_material", nlohmann::json::array({"PLA", "ABS"})}});

    const auto gate0 = lane_sources(harness.lane(0));
    REQUIRE(gate0.vendor_cache.has_value());
    CHECK(gate0.vendor_cache->material == "PLA");
    REQUIRE(gate0.vendor_cache->color_rgb.has_value());
    CHECK(*gate0.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(gate0.vendor_cache->spoolman_id == 7);
}

TEST_CASE_METHOD(LVGLTestFixture, "each Happy Hare gate accumulates its own identity",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // Gate 0 alone carries a spool. One accumulator for the backend instead of
    // one per gate would hand its identity to every other gate.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 0, 0})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 0, 0})}});

    const auto silent = lane_sources(harness.lane(1));
    REQUIRE(silent.vendor_cache.has_value());
    CHECK_FALSE(silent.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(silent.vendor_cache->material.has_value());
    CHECK_FALSE(silent.vendor_cache->spoolman_id.has_value());

    // Gate 1 gets its own spool; gate 0's record must not follow it.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"ed2c2c", "00aeff", ""})},
                        {"gate_material", nlohmann::json::array({"PETG", "PLA", ""})},
                        {"gate_spool_id", nlohmann::json::array({7, 12, 0})}});

    const auto changed = lane_sources(harness.lane(1));
    REQUIRE(changed.vendor_cache->color_rgb.has_value());
    CHECK(*changed.vendor_cache->color_rgb == 0x00AEFFu);
    CHECK(changed.vendor_cache->material == "PLA");
    CHECK(changed.vendor_cache->spoolman_id == 12);

    const auto untouched = lane_sources(harness.lane(0));
    REQUIRE(untouched.vendor_cache->color_rgb.has_value());
    CHECK(*untouched.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(untouched.vendor_cache->material == "PETG");
    CHECK(untouched.vendor_cache->spoolman_id == 7);

    const auto quiet = lane_sources(harness.lane(2));
    REQUIRE(quiet.vendor_cache.has_value());
    CHECK_FALSE(quiet.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(quiet.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches Happy Hare's vendor-cache record",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // A user colour and material that disagree with what the gate map says.
    // SlotInfo persists across frames and apply_overrides() rewrites it in
    // place, so a translation reading that struct back would file the user's
    // own choice as something Happy Hare's gate map remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    user.material = "ABS";
    {
        std::lock_guard<std::mutex> lock(HappyHareTestAccess::mutex(*harness));
        HappyHareTestAccess::overrides(*harness)[1] = user;
    }

    // gate_spool_id is what makes apply_overrides() run at all on this path.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "ed2c2c"})},
                        {"gate_material", nlohmann::json::array({"PLA", "PETG"})},
                        {"gate_spool_id", nlohmann::json::array({0, 0})}});

    // Precondition, not the behaviour under test: unless the override actually
    // wins on the merged slot there is no laundering for the case to catch and
    // the assertions below would hold for the wrong reason.
    REQUIRE(harness->get_slot_info(1).color_rgb == 0x00FF00u);
    REQUIRE(harness->get_slot_info(1).material == "ABS");

    const auto lane = lane_sources(harness.lane(1));
    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(lane.vendor_cache->material == "PETG");
}

TEST_CASE_METHOD(LVGLTestFixture, "a gate Happy Hare calls unknown files no presence reading",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});

    const auto unread = lane_sources(harness.lane(0));
    // The record exists, so the translation provably ran; what it holds is the
    // absence of a reading rather than a reading of absence.
    REQUIRE(unread.sensed.has_value());
    CHECK_FALSE(unread.sensed->present.has_value());

    const auto known = lane_sources(harness.lane(1));
    REQUIRE(known.sensed->present.has_value());
    CHECK(*known.sensed->present == true);

    // Positive contrast: the same gate reports a real status and the reading
    // lands, so the arm above is a guard and not a gate that never reports.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})}});
    REQUIRE(lane_sources(harness.lane(0)).sensed->present.has_value());
    CHECK(*lane_sources(harness.lane(0)).sensed->present == true);

    // Happy Hare withdrawing its word is news. Leaving the last reading
    // standing would keep asserting a presence the MMU has stopped claiming.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({-1, 1})}});
    const auto withdrawn = lane_sources(harness.lane(0));
    REQUIRE(withdrawn.sensed.has_value());
    CHECK_FALSE(withdrawn.sensed->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "an unreadable Happy Hare colour leaves the record standing",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color", nlohmann::json::array({"ed2c2c", "a4b2bc"})}});
    REQUIRE(*lane_sources(harness.lane(0)).vendor_cache->color_rgb == 0xED2C2Cu);

    // A value nothing can read is not a gate stating it has no colour, so the
    // last readable word stands in both the record and the slot.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"zzzzzz", "a4b2bc"})}});
    const auto unreadable = lane_sources(harness.lane(0));
    REQUIRE(unreadable.vendor_cache->color_rgb.has_value());
    CHECK(*unreadable.vendor_cache->color_rgb == 0xED2C2Cu);
    CHECK(harness->get_slot_info(0).color_rgb == 0xED2C2Cu);

    // Empty is Happy Hare wiping the gate, which is a statement and clears both.
    feed_mmu(*harness, {{"gate_color", nlohmann::json::array({"", "a4b2bc"})}});
    const auto cleared = lane_sources(harness.lane(0));
    REQUIRE(cleared.vendor_cache.has_value());
    CHECK_FALSE(cleared.vendor_cache->color_rgb.has_value());
    CHECK(harness->get_slot_info(0).color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
}

TEST_CASE_METHOD(LVGLTestFixture, "Happy Hare files the colour its numeric gate map states",
                 "[lane][ingest][happy_hare]") {
    HappyHareHarness harness(nullptr, nullptr);

    // gate_color_rgb wins over the hex strings, in both the shapes Happy Hare
    // and its emulator publish: packed integers and float triplets.
    feed_mmu(*harness, {{"gate_status", nlohmann::json::array({1, 1})},
                        {"gate_color_rgb", nlohmann::json::array({0xED2C2C, {0.0, 1.0, 0.0}})},
                        {"gate_color", nlohmann::json::array({"a4b2bc", "a4b2bc"})}});

    const auto packed = lane_sources(harness.lane(0));
    REQUIRE(packed.vendor_cache.has_value());
    REQUIRE(packed.vendor_cache->color_rgb.has_value());
    CHECK(*packed.vendor_cache->color_rgb == 0xED2C2Cu);

    const auto triplet = lane_sources(harness.lane(1));
    REQUIRE(triplet.vendor_cache->color_rgb.has_value());
    CHECK(*triplet.vendor_cache->color_rgb == 0x00FF00u);
}

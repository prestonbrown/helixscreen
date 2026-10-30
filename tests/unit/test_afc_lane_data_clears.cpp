// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_data_clears.cpp
 * @brief Applying lane_data: clears, overrides, and the registry it never resizes.
 *
 * Reading the records is test_afc_status_parse.cpp's subject; both lane
 * surfaces share one identity reader there. These cases pin what the backend
 * does with a record (#1195).
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "test_helpers/afc_lane_feed.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::feed_lane_data;
using json = nlohmann::json;

namespace {

json lane_record(const json& spool_id) {
    return json{{"color", "#FF0000"}, {"material", "PLA"}, {"spool_id", spool_id}};
}

/// Every case names both lanes, which is what a settled system looks like.
json both_lanes(const json& lane1, const json& lane2 = json::object()) {
    return json{{"lane1", lane1}, {"lane2", lane2}};
}

void seed_spool_id(AmsBackendAfc& afc, int slot_index, int id) {
    std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(afc));
    AfcTestAccess::slots(afc).get_mut(slot_index)->info.spoolman_id = id;
}

} // namespace

TEST_CASE("AFC lane_data null spool_id unlinks one lane and leaves the other", "[ams][afc][1195]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc_reg(nullptr, nullptr);
    AmsBackendAfc& afc = *afc_reg;
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});
    seed_spool_id(afc, 0, 42);
    seed_spool_id(afc, 1, 99);

    feed_lane_data(afc, both_lanes(json{{"material", "PETG"}}));
    CHECK(afc.get_slot_info(0).spoolman_id == 42);

    feed_lane_data(afc, both_lanes(lane_record(nullptr)));
    CHECK(afc.get_slot_info(0).spoolman_id == 0);
    CHECK(afc.get_slot_info(1).spoolman_id == 99);
}

TEST_CASE("AFC lane_data applies the user's slot overrides", "[ams][afc][1195]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc_reg(nullptr, nullptr);
    AmsBackendAfc& afc = *afc_reg;
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    helix::ams::FilamentSlotOverride o;
    o.brand = "Polymaker";
    o.material = "ASA";
    o.spoolman_id = 77;
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(afc));
        AfcTestAccess::overrides(afc)[0] = o;
    }
    helix::test::file_override_as_lane_records(afc, 0, o);

    SECTION("override wins where firmware echoes the same spool") {
        feed_lane_data(afc, both_lanes(json{{"material", "PLA"}, {"spool_id", 77}}));
        CHECK(afc.get_slot_info(0).brand == "Polymaker");
        CHECK(afc.get_slot_info(0).material == "ASA");
    }

    SECTION("override re-supplies identity across the null clear") {
        // Firmware clears on eject, and the override is the only thing that
        // puts the user's spool back.
        feed_lane_data(afc, both_lanes(lane_record(nullptr)));
        CHECK(afc.get_slot_info(0).spoolman_id == 77);
        CHECK(afc.get_slot_info(0).brand == "Polymaker");
    }

    SECTION("a lane with no override takes firmware's word") {
        feed_lane_data(afc,
                       both_lanes(json::object(), json{{"material", "PETG"}, {"spool_id", 5}}));
        CHECK(afc.get_slot_info(1).material == "PETG");
        CHECK(afc.get_slot_info(1).spoolman_id == 5);
        CHECK(afc.get_slot_info(1).brand.empty());
    }
}

TEST_CASE("AFC lane_data names and brands yield to the user's own", "[ams][afc][833]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc_reg(nullptr, nullptr);
    AmsBackendAfc& afc = *afc_reg;
    helix::test::init_afc_lanes(afc, {"lane1", "lane2"});

    feed_lane_data(afc, both_lanes(json{{"name", "Ambrosia Pink"}, {"vendor_name", "Polymaker"}},
                                   json{{"filament_name", "Galaxy Black"}}));
    CHECK(afc.get_slot_info(0).spool_name == "Ambrosia Pink");
    CHECK(afc.get_slot_info(0).brand == "Polymaker");
    CHECK(afc.get_slot_info(1).spool_name == "Galaxy Black");

    SlotInfo edit = afc.get_slot_info(0);
    edit.brand = "Elegoo";
    edit.spool_name = "My Pink Spool";
    helix::test::edit_slot_as_user(afc, 0, edit);

    feed_lane_data(afc, both_lanes(json{{"name", "Ambrosia Pink"}, {"vendor_name", "Polymaker"}}));
    CHECK(afc.get_slot_info(0).spool_name == "My Pink Spool");
    CHECK(afc.get_slot_info(0).brand == "Elegoo");

    // An empty name clears firmware's own, so a lane without a declaration empties.
    feed_lane_data(afc, both_lanes(json::object(), json{{"filament_name", ""}}));
    CHECK(afc.get_slot_info(1).spool_name.empty());
}

// ============================================================================
// Mid-repopulation payloads
//
// AFC empties the whole lane_data namespace at the start of every PREP and
// writes each lane's key back only once that lane's own prep finishes, so the
// namespace sits partially populated for seconds on every boot while
// query_lane_data() is one-shot. Klipper's object list is the authority on
// which lanes exist; the database is a supplement.
// ============================================================================

TEST_CASE("AFC lane_data never resizes a registry that already exists",
          "[ams][afc][lane-data-race]") {
    helix::test::RegisteredBackend<AmsBackendAfc> afc_reg(nullptr, nullptr);
    AmsBackendAfc& afc = *afc_reg;
    helix::test::init_afc_lanes(afc, {"lane1", "lane2", "lane3", "lane4"});
    seed_spool_id(afc, 1, 99);

    feed_lane_data(afc, json{{"lane1", lane_record(127)}});
    CHECK(AfcTestAccess::slots(afc).slot_count() == 4);
    CHECK(AfcTestAccess::slots(afc).name_of(3) == "lane4");
    CHECK(afc.get_slot_info(0).spoolman_id == 127);
    CHECK(afc.get_slot_info(1).spoolman_id == 99);

    // The window opens with the delete, so an even earlier query returns {}.
    feed_lane_data(afc, json::object());
    CHECK(AfcTestAccess::slots(afc).slot_count() == 4);
}

TEST_CASE("AFC lane_data defers to Klipper discovery for the lane set",
          "[ams][afc][lane-data-race]") {
    AmsBackendAfc afc(nullptr, nullptr);
    afc.set_discovered_lanes({"lane1", "lane2", "lane3", "lane4"}, {});

    feed_lane_data(afc, json{{"lane1", lane_record(127)}});

    CHECK(AfcTestAccess::slots(afc).slot_count() == 4);
    CHECK(AfcTestAccess::slots(afc).name_of(3) == "lane4");
    CHECK(afc.get_slot_info(0).spoolman_id == 127);
}

TEST_CASE("AFC lane_data still bootstraps when discovery found no lanes",
          "[ams][afc][lane-data-race]") {
    // Without AFC_stepper/AFC_lane objects to enumerate, the database is the
    // only lane source there is, so deference must not become a refusal.
    AmsBackendAfc afc(nullptr, nullptr);
    REQUIRE(AfcTestAccess::slots(afc).slot_count() == 0);

    feed_lane_data(afc, json{{"lane2", lane_record(2)}, {"lane1", lane_record(1)}});

    CHECK(AfcTestAccess::slots(afc).slot_count() == 2);
    CHECK(AfcTestAccess::slots(afc).name_of(1) == "lane2");
    CHECK(afc.get_slot_info(1).spoolman_id == 2);
}

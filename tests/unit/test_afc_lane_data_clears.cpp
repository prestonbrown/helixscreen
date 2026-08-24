// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_lane_data_clears.cpp
 * @brief parse_lane_data() null-clear and override parity with the status path (#1195).
 *
 * AFC has two lane parsers and they had drifted. parse_afc_stepper() — the live
 * status path — treats a null spool_id as an explicit clear and re-applies the
 * user's overrides afterwards. parse_lane_data() — the Moonraker DB path — did
 * neither: a null fell through its is_number_integer() guard and left the
 * previous id in place, and overrides were never applied at all.
 *
 * Both are reachable now. query_lane_data() was pointing at the wrong namespace
 * and 404'ing until a7671487a, so whichever parser ran last decided whether an
 * ejected lane still looked linked and whether a user's override was visible.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

class AfcLaneDataClearHelper : public AmsBackendAfc {
  public:
    AfcLaneDataClearHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
    }

    /// Start from an arbitrary lane set, or from none at all (empty vector) to
    /// exercise the pre-initialization path.
    explicit AfcLaneDataClearHelper(const std::vector<std::string>& names)
        : AmsBackendAfc(nullptr, nullptr) {
        if (!names.empty()) {
            initialize_slots(names);
        }
    }

    [[nodiscard]] int slot_count() const {
        return slots_.slot_count();
    }

    [[nodiscard]] std::string lane_name(int slot_index) const {
        return slots_.name_of(slot_index);
    }

    void feed_lane_data(const nlohmann::json& lane_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        parse_lane_data(lane_data);
    }

    /// Drive the live status path, so a test can assert the two parsers agree.
    void feed_stepper(const std::string& lane, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_stepper " + lane] = data;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    void set_spool_id(int slot_index, int id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* entry = slots_.get_mut(slot_index);
        if (entry) {
            entry->info.spoolman_id = id;
        }
    }

    [[nodiscard]] int spool_id(int slot_index) const {
        return get_slot_info(slot_index).spoolman_id;
    }

    void set_override(int slot_index, const helix::ams::FilamentSlotOverride& o) {
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_[slot_index] = o;
    }

    [[nodiscard]] std::string brand(int slot_index) const {
        return get_slot_info(slot_index).brand;
    }

    [[nodiscard]] std::string material(int slot_index) const {
        return get_slot_info(slot_index).material;
    }

    [[nodiscard]] std::string spool_name(int slot_index) const {
        return get_slot_info(slot_index).spool_name;
    }
};

namespace {

nlohmann::json lane_record(const nlohmann::json& spool_id) {
    return nlohmann::json{{"color", "#FF0000"}, {"material", "PLA"}, {"spool_id", spool_id}};
}

/// A full two-lane payload.
///
/// Every case here names both lanes so the payload matches the registry, which
/// is what a settled system looks like. Partial payloads are their own subject,
/// covered by the mid-repopulation cases at the bottom of this file.
nlohmann::json both_lanes(const nlohmann::json& lane1,
                          const nlohmann::json& lane2 = nlohmann::json::object()) {
    return nlohmann::json{{"lane1", lane1}, {"lane2", lane2}};
}

} // namespace

TEST_CASE("AFC lane_data treats a null spool_id as an explicit clear", "[ams][afc][1195]") {
    AfcLaneDataClearHelper afc;
    afc.set_spool_id(0, 42);
    REQUIRE(afc.spool_id(0) == 42);

    SECTION("null unlinks the spool, as an eject requires") {
        // AFC writes spool_id=None on eject. Retaining the old id left the lane
        // looking linked, which is what later aimed an edit's Spoolman write at
        // the wrong spool.
        afc.feed_lane_data(both_lanes(lane_record(nullptr)));
        CHECK(afc.spool_id(0) == 0);
    }

    SECTION("an integer still links") {
        afc.feed_lane_data(both_lanes(lane_record(7)));
        CHECK(afc.spool_id(0) == 7);
    }

    SECTION("an ABSENT key means unchanged — these are deltas, not snapshots") {
        afc.feed_lane_data(both_lanes(nlohmann::json{{"color", "#00FF00"}, {"material", "PETG"}}));
        CHECK(afc.spool_id(0) == 42);
    }

    SECTION("a non-integer, non-null value does not corrupt the link") {
        afc.feed_lane_data(both_lanes(lane_record("not-an-id")));
        CHECK(afc.spool_id(0) == 42);
    }

    SECTION("clearing one lane leaves the other alone") {
        afc.set_spool_id(1, 99);
        afc.feed_lane_data(both_lanes(lane_record(nullptr)));
        CHECK(afc.spool_id(0) == 0);
        CHECK(afc.spool_id(1) == 99);
    }
}

TEST_CASE("AFC lane_data applies the user's slot overrides", "[ams][afc][1195]") {
    AfcLaneDataClearHelper afc;

    helix::ams::FilamentSlotOverride o;
    o.brand = "Polymaker";
    o.material = "ASA";
    o.spoolman_id = 77;
    afc.set_override(0, o);

    SECTION("override wins where firmware echoes the same spool") {
        afc.feed_lane_data(both_lanes(
            nlohmann::json{{"material", "PLA"}, {"spool_id", 77}, {"color", "#FF0000"}}));
        CHECK(afc.brand(0) == "Polymaker");
        CHECK(afc.material(0) == "ASA");
        CHECK(afc.spool_id(0) == 77);
    }

    SECTION("a different firmware id is an external re-bind — the record drops (#1281)") {
        // Another writer set spool 5 on this lane; our stale override for 77
        // must not shadow it. Mirrors the status-path test in
        // test_override_rebind.cpp through the DB parser.
        afc.feed_lane_data(
            both_lanes(nlohmann::json{{"material", "PLA"}, {"spool_id", 5}, {"color", "#FF0000"}}));
        CHECK(afc.spool_id(0) == 5);
        CHECK(afc.brand(0).empty());
    }

    SECTION("override re-supplies identity across the null clear") {
        // This is the pairing that matters: firmware clears on eject, and the
        // override is the only thing that puts the user's spool back. Applying
        // the clear without re-applying the override would wipe it.
        afc.feed_lane_data(both_lanes(lane_record(nullptr)));
        CHECK(afc.spool_id(0) == 77);
        CHECK(afc.brand(0) == "Polymaker");
    }

    SECTION("a lane with no override is untouched") {
        afc.feed_lane_data(both_lanes(nlohmann::json::object(),
                                      nlohmann::json{{"material", "PETG"}, {"spool_id", 5}}));
        CHECK(afc.material(1) == "PETG");
        CHECK(afc.spool_id(1) == 5);
        CHECK(afc.brand(1).empty());
    }
}

TEST_CASE("AFC lane_data adopts the filament name, like the status path", "[ams][afc][1195]") {
    // These sections use `filament_name`, the defensive spelling — see the #833
    // case below for `name`, which is what AFC actually publishes into
    // lane_data. Either key reaches the same slot field. The bug being pinned
    // here is that only the status parser read a name at all, so a lane whose
    // data arrived solely through the lane_data path had none, and the loaded
    // card fell back to the algorithmic colour name.
    AfcLaneDataClearHelper afc;

    SECTION("a name in the DB record reaches the slot") {
        afc.feed_lane_data(
            both_lanes(nlohmann::json{{"material", "PLA"}, {"filament_name", "Ambrosia Pink"}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
    }

    SECTION("an empty string is a deliberate clear") {
        // clear_values() writes filament_name="" on eject. Treating that as a
        // parse failure would keep painting the previous spool's name.
        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}}));
        REQUIRE(afc.spool_name(0) == "Ambrosia Pink");

        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", ""}}));
        CHECK(afc.spool_name(0).empty());
    }

    SECTION("an ABSENT key means unchanged — these are deltas, not snapshots") {
        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}}));
        afc.feed_lane_data(both_lanes(nlohmann::json{{"material", "PETG"}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
    }

    SECTION("a non-string value leaves the name alone") {
        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}}));
        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", nullptr}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
    }

    SECTION("a user-entered name still wins over firmware's") {
        // apply_overrides() must run AFTER the parse, same as the status path.
        helix::ams::FilamentSlotOverride o;
        o.spool_name = "My Pink Spool";
        afc.set_override(0, o);

        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}}));
        CHECK(afc.spool_name(0) == "My Pink Spool");
    }

    SECTION("naming one lane leaves the other alone") {
        afc.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}},
                                      nlohmann::json{{"filament_name", "Galaxy Black"}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
        CHECK(afc.spool_name(1) == "Galaxy Black");
    }
}

TEST_CASE("AFC lane_data reads the shared #833 key spellings", "[ams][afc][833]") {
    // AFCProject/AFC-Klipper-Add-On#833 publishes brand and product name into
    // lane_data under the keys Happy Hare established for that shared namespace
    // — `vendor_name` and `name` — NOT under AFCLane's own attribute names.
    // (Its get_status dict does use the attribute names, `spool_vendor` and
    // `filament_name`; the surfaces differ on purpose.) A reader that only knew
    // the attribute spellings would see every #833 lane as unbranded and
    // unnamed, which is the whole point of the upstream change.
    AfcLaneDataClearHelper afc;

    SECTION("`name` reaches the slot's spool name") {
        afc.feed_lane_data(
            both_lanes(nlohmann::json{{"material", "PLA"}, {"name", "Ambrosia Pink"}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
    }

    SECTION("`vendor_name` reaches the slot's brand") {
        afc.feed_lane_data(
            both_lanes(nlohmann::json{{"material", "PLA"}, {"vendor_name", "Polymaker"}}));
        CHECK(afc.brand(0) == "Polymaker");
    }

    SECTION("an empty `name` is a deliberate clear, as with the other spelling") {
        afc.feed_lane_data(both_lanes(nlohmann::json{{"name", "Ambrosia Pink"}}));
        REQUIRE(afc.spool_name(0) == "Ambrosia Pink");

        afc.feed_lane_data(both_lanes(nlohmann::json{{"name", ""}}));
        CHECK(afc.spool_name(0).empty());
    }

    SECTION("`name` wins when a record somehow carries both spellings") {
        afc.feed_lane_data(both_lanes(
            nlohmann::json{{"name", "Ambrosia Pink"}, {"filament_name", "Galaxy Black"}}));
        CHECK(afc.spool_name(0) == "Ambrosia Pink");
    }

    SECTION("a user's override still wins over both") {
        helix::ams::FilamentSlotOverride o;
        o.brand = "Elegoo";
        o.spool_name = "My Pink Spool";
        afc.set_override(0, o);

        afc.feed_lane_data(
            both_lanes(nlohmann::json{{"name", "Ambrosia Pink"}, {"vendor_name", "Polymaker"}}));
        CHECK(afc.spool_name(0) == "My Pink Spool");
        CHECK(afc.brand(0) == "Elegoo");
    }
}

TEST_CASE("AFC lane_data and status paths agree about the filament name", "[ams][afc][1195]") {
    // The bug was an ASYMMETRY, so pin the parity: the same firmware value
    // expressed through either parser must reach the same slot field.
    AfcLaneDataClearHelper via_db;
    AfcLaneDataClearHelper via_status;

    via_db.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", "Ambrosia Pink"}}));
    via_status.feed_stepper("lane1", nlohmann::json{{"filament_name", "Ambrosia Pink"}});

    CHECK(via_db.spool_name(0) == via_status.spool_name(0));
    CHECK(via_db.spool_name(0) == "Ambrosia Pink");

    via_db.feed_lane_data(both_lanes(nlohmann::json{{"filament_name", ""}}));
    via_status.feed_stepper("lane1", nlohmann::json{{"filament_name", ""}});

    CHECK(via_db.spool_name(0) == via_status.spool_name(0));
    CHECK(via_db.spool_name(0).empty());
}

TEST_CASE("AFC lane_data and status paths agree about the null clear", "[ams][afc][1195]") {
    // The bug was a DIVERGENCE, so pin the parity rather than each side alone:
    // the same eject expressed through either parser must reach the same state.
    AfcLaneDataClearHelper via_db;
    AfcLaneDataClearHelper via_status;
    via_db.set_spool_id(0, 42);
    via_status.set_spool_id(0, 42);

    via_db.feed_lane_data(both_lanes(lane_record(nullptr)));
    via_status.feed_stepper("lane1", nlohmann::json{{"spool_id", nullptr}});

    CHECK(via_db.spool_id(0) == via_status.spool_id(0));
    CHECK(via_db.spool_id(0) == 0);
}

// ============================================================================
// Mid-repopulation payloads
//
// AFC empties the whole lane_data namespace at the start of every PREP
// (AFC.py delete_lane_data) and writes each lane's key back only once that
// lane's own prep finishes (AFC_BoxTurtle.py send_lane_data). BoxTurtle prep
// drives each lane motor in turn, so the namespace sits partially populated for
// seconds on every boot. query_lane_data() is one-shot and unretried, so a cold
// boot that lands in that window used to redefine the lane set from whatever
// had been written so far: a 4-lane BoxTurtle pinned to a single slot for the
// rest of the session, with lanes 2-4 invisible because the status handler only
// iterates slots that exist.
//
// Klipper's object list is the authority on which lanes exist. The database is
// a supplement carrying colours, materials and spool ids.
// ============================================================================

TEST_CASE("AFC lane_data never resizes a registry that already exists",
          "[ams][afc][lane-data-race]") {
    AfcLaneDataClearHelper afc(std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
    REQUIRE(afc.slot_count() == 4);

    SECTION("a payload holding only the first lane leaves all four standing") {
        afc.feed_lane_data(nlohmann::json{{"lane1", lane_record(127)}});
        CHECK(afc.slot_count() == 4);
        CHECK(afc.lane_name(3) == "lane4");
    }

    SECTION("the lane the payload does carry is still updated") {
        afc.feed_lane_data(nlohmann::json{{"lane1", lane_record(127)}});
        CHECK(afc.spool_id(0) == 127);
    }

    SECTION("a lane absent from the payload keeps its own data") {
        afc.set_spool_id(1, 99);
        afc.feed_lane_data(nlohmann::json{{"lane1", lane_record(127)}});
        CHECK(afc.spool_id(1) == 99);
    }

    SECTION("an empty namespace does not erase the registry") {
        // The window opens with the delete, so an even earlier query returns {}.
        afc.feed_lane_data(nlohmann::json::object());
        CHECK(afc.slot_count() == 4);
    }
}

TEST_CASE("AFC lane_data defers to Klipper discovery for the lane set",
          "[ams][afc][lane-data-race]") {
    // Ordering inside on_started() is not guaranteed to have built the registry
    // first, so the deference has to hold on the uninitialized path too.
    AfcLaneDataClearHelper afc{std::vector<std::string>{}};
    REQUIRE(afc.slot_count() == 0);
    afc.set_discovered_lanes({"lane1", "lane2", "lane3", "lane4"}, {});

    afc.feed_lane_data(nlohmann::json{{"lane1", lane_record(127)}});

    CHECK(afc.slot_count() == 4);
    CHECK(afc.lane_name(3) == "lane4");
    CHECK(afc.spool_id(0) == 127);
}

TEST_CASE("AFC lane_data still bootstraps when discovery found no lanes",
          "[ams][afc][lane-data-race]") {
    // Without AFC_stepper/AFC_lane objects to enumerate, the database is the
    // only lane source there is, so deference must not become a refusal.
    AfcLaneDataClearHelper afc{std::vector<std::string>{}};
    REQUIRE(afc.slot_count() == 0);

    afc.feed_lane_data(nlohmann::json{{"lane1", lane_record(1)}, {"lane2", lane_record(2)}});

    CHECK(afc.slot_count() == 2);
    CHECK(afc.lane_name(1) == "lane2");
}

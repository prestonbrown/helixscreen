// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_status_field_adoption.cpp
 * @brief AFC status-object fields: the one still worth adopting, and the two
 *        readers that must stay inert until upstream ships them.
 *
 * #1149 — `extruder_temp` is the last unadopted field from the v1.2.0 audit.
 * AFC_lane.get_status() has published it since v1.1.0 (AFC_lane.py:1774) and
 * AFC_spool.py fills it from Spoolman's `settings_extruder_temp`, so it is the
 * spool's single recommended print temperature. Its home is the nozzle pair on
 * SlotInfo, which active_material_provider layers over the filament DB as the
 * tier-2 vendor preset — and `MaterialInfo::nozzle_recommended()` averages that
 * pair, so writing the same value to both ends makes the recommended preheat
 * exactly what the spool asks for. That is the display surface #1149 said was
 * undecided; preheat_widget, temperature_service and ui_panel_filament all read
 * it already.
 *
 * #1175 — `AFC.version` and the lane vendor were requested upstream and are read
 * by code that has never met a printer emitting either. Verified 2026-07-28:
 *   - `version` exists only on upstream DEV (extras/AFC.py `str['version']`),
 *     not in v1.2.0, the latest release, and not on the live BoxTurtle at .112
 *     (AFC v1.1.0-4-g2921371, whose get_status() has no such key).
 *   - no released branch publishes a vendor at all. DEV carries `spool_vendor`
 *     as an internal member (AFC_lane.py:128) that no status or lane_data
 *     payload emitted. #833 is the PR that publishes it — as `spool_vendor` in
 *     get_status (this file's surface) and as `vendor_name` in lane_data (see
 *     test_afc_lane_data_clears.cpp).
 * Neither reader can be exercised against hardware, so the contract to lock is
 * that they stay no-ops: absence must not clobber what we already knew.
 */

#include "active_material_provider.h"
#include "ams_backend_afc.h"
#include "ams_types.h"
#include "filament_database.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using json = nlohmann::json;

/// Drives the real status parser with no client, the way the fixture-contract
/// test does, and exposes the version member the readers under test write.
class AfcStatusFieldHelper : public AmsBackendAfc {
  public:
    AfcStatusFieldHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2"};
        initialize_slots(names);
    }

    void feed_lane(const json& data) {
        json params;
        params["AFC_stepper lane1"] = data;
        json notification;
        notification["params"] = json::array({params, 0.0});
        handle_status_update(notification);
    }

    void feed_afc(const json& data) {
        json params;
        params["AFC"] = data;
        json notification;
        notification["params"] = json::array({params, 0.0});
        handle_status_update(notification);
    }

    bool apply_db_version(const json& response) {
        return apply_afc_version_response(response);
    }

    std::string version() const {
        return afc_version_;
    }
};

namespace {

/// The envelope Moonraker sends for server.database.get_item.
json db_envelope(const json& value) {
    return {{"jsonrpc", "2.0"},
            {"id", 1},
            {"result", {{"namespace", "afc-install"}, {"value", value}}}};
}

/// A clean AFC status object shaped like the live v1.1.0 capture — note it
/// carries no "version" key, because no released AFC emits one.
json pre_807_state() {
    return json{{"current_load", "lane1"},
                {"current_lane", nullptr},
                {"current_state", "Idle"},
                {"error_state", false},
                {"bypass_state", false},
                {"lanes", json::array({"lane1", "lane2"})},
                {"message", {{"message", ""}, {"type", ""}}}};
}

} // namespace

// ============================================================================
// #1149 — extruder_temp
// ============================================================================

TEST_CASE("AFC's per-lane extruder_temp becomes the slot's nozzle temperature",
          "[ams][afc][1149]") {
    AfcStatusFieldHelper afc;
    afc.feed_lane({{"material", "PETG"}, {"extruder_temp", 250.0}});

    const SlotInfo slot = afc.get_slot_info(0);
    CHECK(slot.nozzle_temp_min == 250);
    CHECK(slot.nozzle_temp_max == 250);
}

// The point of adopting it: a single recommended temperature has to survive the
// min/max modelling all the way to the preheat the UI actually targets. PETG's
// DB range is 230-260, so an unadopted field leaves recommended at 245.
TEST_CASE("extruder_temp reaches the recommended preheat, beating the filament DB",
          "[ams][afc][1149]") {
    AfcStatusFieldHelper afc;

    // Guard: the DB value this must displace is not already 250.
    auto db = filament::find_material("PETG");
    REQUIRE(db.has_value());
    REQUIRE(db->nozzle_recommended() != 250);

    afc.feed_lane({{"material", "PETG"}, {"extruder_temp", 250.0}});

    const ActiveMaterial active = build_active_material(afc.get_slot_info(0));
    CHECK(active.material_info.nozzle_min == 250);
    CHECK(active.material_info.nozzle_max == 250);
    CHECK(active.material_info.nozzle_recommended() == 250);
}

TEST_CASE("extruder_temp rounds rather than truncating", "[ams][afc][1149]") {
    AfcStatusFieldHelper afc;
    afc.feed_lane({{"extruder_temp", 244.6}});
    CHECK(afc.get_slot_info(0).nozzle_temp_min == 245);
}

// AFC_spool.clear_values() sets extruder_temp = None on eject. Null is the
// clear, exactly as for bed_temp and spool_id.
TEST_CASE("a null extruder_temp clears the slot's nozzle temperature", "[ams][afc][1149]") {
    AfcStatusFieldHelper afc;
    afc.feed_lane({{"extruder_temp", 250.0}});
    REQUIRE(afc.get_slot_info(0).nozzle_temp_min == 250);

    afc.feed_lane({{"extruder_temp", nullptr}});
    CHECK(afc.get_slot_info(0).nozzle_temp_min == 0);
    CHECK(afc.get_slot_info(0).nozzle_temp_max == 0);
}

// Status updates are deltas, not snapshots — an omitted key means "unchanged".
TEST_CASE("an absent extruder_temp leaves the previous nozzle temperature alone",
          "[ams][afc][1149]") {
    AfcStatusFieldHelper afc;
    afc.feed_lane({{"extruder_temp", 250.0}});
    REQUIRE(afc.get_slot_info(0).nozzle_temp_min == 250);

    afc.feed_lane({{"weight", 812.5}});
    CHECK(afc.get_slot_info(0).nozzle_temp_min == 250);
    CHECK(afc.get_slot_info(0).nozzle_temp_max == 250);
}

// ============================================================================
// #1175 — the readers upstream has not shipped
// ============================================================================

// The checklist item: a pre-#807 printer must still report cleanly. Absence of
// the key must leave afc_version_ at its "unknown" default, not blank it or
// write an empty string into system_info_.
TEST_CASE("a status payload with no version leaves the reader a no-op", "[ams][afc][1175]") {
    AfcStatusFieldHelper afc;
    REQUIRE(afc.version() == "unknown");

    afc.feed_afc(pre_807_state());
    CHECK(afc.version() == "unknown");
}

// The other half: whatever detect_afc_version() managed to pull from the (now
// orphaned) afc-install namespace must survive every subsequent status frame.
TEST_CASE("a status payload with no version does not clobber the detected version",
          "[ams][afc][1175]") {
    AfcStatusFieldHelper afc;
    REQUIRE(afc.apply_db_version(db_envelope({{"version", "1.0.40"}})));
    REQUIRE(afc.version() == "1.0.40");

    afc.feed_afc(pre_807_state());
    CHECK(afc.version() == "1.0.40");

    // An empty string is upstream saying nothing, not saying "".
    json blank = pre_807_state();
    blank["version"] = "";
    afc.feed_afc(blank);
    CHECK(afc.version() == "1.0.40");
}

// Forward path, for when #807 reaches a tagged release: the status object wins
// over the dead database namespace.
TEST_CASE("the status version is adopted once upstream emits it", "[ams][afc][1175]") {
    AfcStatusFieldHelper afc;
    REQUIRE(afc.apply_db_version(db_envelope({{"version", "1.0.40"}})));

    json with_version = pre_807_state();
    with_version["version"] = "1.2.1";
    afc.feed_afc(with_version);
    CHECK(afc.version() == "1.2.1");
}

// #808 is unimplemented on every branch, so the vendor reader only ever sees a
// missing key. It must not touch slot.brand in that case — and must not treat an
// empty value as a clear either, since a wiped brand is unrecoverable on the
// lane_data path.
TEST_CASE("the vendor reader neither invents nor wipes a brand", "[ams][afc][1175]") {
    AfcStatusFieldHelper afc;

    afc.feed_lane({{"material", "PETG"}});
    CHECK(afc.get_slot_info(0).brand.empty());

    // `spool_vendor` is the get_status spelling; lane_data uses `vendor_name`.
    afc.feed_lane({{"spool_vendor", "Polymaker"}});
    REQUIRE(afc.get_slot_info(0).brand == "Polymaker");

    afc.feed_lane({{"spool_vendor", ""}});
    CHECK(afc.get_slot_info(0).brand == "Polymaker");

    afc.feed_lane({{"material", "PLA"}});
    CHECK(afc.get_slot_info(0).brand == "Polymaker");
}

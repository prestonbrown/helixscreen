// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_fixture_contract.cpp
 * @brief Schema contract between AFC's real payloads and our parsers (#1154).
 *
 * tests/fixtures/afc_*.json are captures from the BoxTurtle at 192.168.1.112.
 * They sat orphaned — nothing loaded them, so nothing failed when they drifted
 * from reality. afc_buffer.json had rotted to three keys and was missing every
 * buffer-health field two live tests already required.
 *
 * This file makes them load-bearing, two-sided:
 *
 *  1. FIELD PRESENCE — each fixture must still carry the keys our parsers read.
 *     Refresh a fixture from a newer AFC and this fails the moment upstream
 *     renames or drops something we depend on.
 *  2. PARSE BEHAVIOUR — feeding the real payload through the real backend must
 *     produce the state we claim to derive from it. This is what catches a
 *     parser that quietly stops reading a field the firmware still sends.
 *
 * Complements scripts/afc-test.sh rather than duplicating it: that script
 * validates against a live printer and cannot run in CI or on a machine without
 * AFC hardware. This runs everywhere, against the last known-good capture.
 *
 * REFRESHING: re-dump from a BoxTurtle via Moonraker
 *   /printer/objects/query?<object>, one file per object, keys sorted. If a
 *   refresh makes this file fail, that is the point — reconcile the parser with
 *   the new schema, do not weaken the assertion.
 */

#include "ui_update_queue.h"

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "test_helpers/afc_test_access.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Resolve tests/fixtures/ from __FILE__ so the test does not depend on cwd.
std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

nlohmann::json load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + name;
    std::ifstream f(path);
    INFO("fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j;
}

/// Every key must be present. Reports the missing one by name — a bare
/// contains() assertion tells you a fixture is wrong but not which field.
void require_keys(const nlohmann::json& j, const std::string& what,
                  const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        INFO(what << " is missing required key: " << k);
        REQUIRE(j.contains(k));
    }
}

} // namespace

namespace helix {

class AfcFixtureHelper : public AmsBackendAfc {
  public:
    // A message frame queues its toast decision for the main thread.
    ~AfcFixtureHelper() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    AfcFixtureHelper() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> names{"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::initialize_slots(*this, names);
    }

    void feed(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    void feed_object(const std::string& key, const nlohmann::json& data) {
        nlohmann::json params;
        params[key] = data;
        feed(params);
    }
};
} // namespace helix

// ============================================================================
// Field presence — the keys our parsers read must still exist upstream
// ============================================================================

TEST_CASE("AFC fixtures carry every field our parsers read", "[ams][afc][1154][fixture]") {
    SECTION("AFC global state") {
        auto j = load_fixture("afc_state.json");
        // apply_afc_state reads these; current_load/current_lane drive the
        // aggregate slot pointer, error_state gates current_error() (#1171),
        // message drives fault text and the drain budget (#1186).
        require_keys(j, "AFC",
                     {"current_load", "current_lane", "current_state", "error_state",
                      "bypass_state", "message", "lanes", "units", "extruders", "hubs", "buffers",
                      "current_toolchange", "number_of_toolchanges"});
        REQUIRE(j["message"].is_object());
        require_keys(j["message"], "AFC.message", {"message", "type"});

        // ABSENCE is part of the contract here (#1200). apply_afc_state() still
        // reads AFC.system for num_extruders and per-extruder tool_stn, but
        // get_status() has never published it — the only writers upstream are
        // _webhooks_status() (GET /printer/afc/status, which we do not call) and
        // save_vars(). Toolchanger discovery therefore has to come from the flat
        // `extruders` array, and it silently did not until 6bcfb067b.
        //
        // If a refreshed capture makes this fail, upstream started publishing
        // `system` on the status object: keep the flat-array derivation as the
        // floor, decide deliberately whether the richer block should now win,
        // and promote the [webhook]-tagged tests in
        // test_ams_afc_multi_extruder.cpp to real wire coverage.
        INFO("AFC.system appeared on the status object — see #1200 before relaxing this");
        CHECK_FALSE(j.contains("system"));

        // The array that must carry the toolchanger shape in its place.
        REQUIRE(j["extruders"].is_array());
        CHECK_FALSE(j["extruders"].empty());
    }

    SECTION("AFC_stepper lane") {
        auto j = load_fixture("afc_stepper_lane1.json");
        // tool_loaded is the per-lane load authority (#1194); prep/load are the
        // lane sensors; spool_id/material/color/weight drive slot identity.
        // NB: the wire key is "extruder", which apply_lane_status reads into our
        // slot.extruder_name member (AFC_lane.py: response['extruder']). Assert
        // the key AFC sends, never our C++ field name.
        // extruder_temp is the spool's recommended print temperature (#1149);
        // AFC has published it since v1.1.0 and it is null until a Spoolman
        // spool is linked, so assert the key, not a value.
        require_keys(j, "AFC_stepper",
                     {"tool_loaded", "status", "prep", "load", "loaded_to_hub", "spool_id",
                      "material", "color", "weight", "hub", "lane", "map", "extruder", "unit",
                      "name", "runout_lane", "remember_spool", "extruder_temp"});
    }

    SECTION("AFC_extruder") {
        auto j = load_fixture("afc_extruder.json");
        // lane_loaded attributes the toolhead sensors to a lane (#1194).
        require_keys(j, "AFC_extruder",
                     {"lane_loaded", "tool_start", "tool_start_status", "tool_end",
                      "tool_end_status", "lanes", "tool_stn", "tool_stn_unload"});
    }

    SECTION("AFC_hub") {
        auto j = load_fixture("afc_hub.json");
        require_keys(j, "AFC_hub", {"state", "lanes", "afc_bowden_length", "cut"});
    }

    SECTION("AFC_buffer carries the health fields") {
        // The regression this issue names: the committed fixture had rotted to
        // {state, lanes, enabled} and was missing all three health fields that
        // BufferHealth parsing requires.
        auto j = load_fixture("afc_buffer.json");
        require_keys(j, "AFC_buffer",
                     {"state", "lanes", "enabled", "distance_to_fault", "error_sensitivity",
                      "fault_detection_enabled", "rotation_distance"});
    }

    SECTION("AFC unit object") {
        auto j = load_fixture("afc_unit_boxturtle.json");
        require_keys(j, "AFC_BoxTurtle", {"lanes", "extruders", "hubs", "buffers"});
    }

    SECTION("object list covers every object we subscribe to") {
        auto j = load_fixture("afc_objects_list.json");
        REQUIRE(j.contains("objects"));
        std::vector<std::string> objects = j["objects"].get<std::vector<std::string>>();
        auto has = [&](const std::string& name) {
            return std::find(objects.begin(), objects.end(), name) != objects.end();
        };
        CHECK(has("AFC"));
        CHECK(has("AFC_stepper lane1"));
        CHECK(has("AFC_extruder extruder"));
        CHECK(has("AFC_hub Turtle_1"));
        CHECK(has("AFC_buffer Turtle_1"));
        CHECK(has("AFC_BoxTurtle Turtle_1"));
    }
}

// ============================================================================
// Parse behaviour — the real payload through the real parser
// ============================================================================

TEST_CASE("AFC parses its real captured payloads into the expected state",
          "[ams][afc][1154][fixture]") {
    AfcFixtureHelper afc;

    afc.feed_object("AFC", load_fixture("afc_state.json"));
    for (int i = 1; i <= 4; ++i) {
        const std::string lane = "lane" + std::to_string(i);
        afc.feed_object("AFC_stepper " + lane, load_fixture("afc_stepper_" + lane + ".json"));
    }
    afc.feed_object("AFC_extruder extruder", load_fixture("afc_extruder.json"));

    SECTION("the seated lane is the one the capture has tool_loaded") {
        // The capture was taken with lane1 at the toolhead ("Tooled").
        auto lane1 = load_fixture("afc_stepper_lane1.json");
        REQUIRE(lane1["tool_loaded"].get<bool>()); // guard: capture still shows this
        CHECK(afc.get_slot_info(0).status == SlotStatus::LOADED);
        CHECK(afc.slot_is_actively_loaded(0));
    }

    SECTION("idle lanes holding filament read AVAILABLE, not LOADED or EMPTY") {
        // AFC reports these as status "None" with prep+load true — filament is
        // parked in the lane. Treating "None" as EMPTY would blank three lanes
        // that physically hold spools.
        for (int i = 1; i < 4; ++i) {
            INFO("lane" << (i + 1));
            CHECK(afc.get_slot_info(i).status == SlotStatus::AVAILABLE);
            CHECK_FALSE(afc.slot_is_actively_loaded(i));
        }
    }

    SECTION("toolhead sensors attribute to exactly the lane the extruder names") {
        auto ext = load_fixture("afc_extruder.json");
        REQUIRE(ext["lane_loaded"].is_string()); // guard: capture still names a lane
        CHECK(afc.slot_has_filament_at_toolhead(0));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(1));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(2));
        CHECK_FALSE(afc.slot_has_filament_at_toolhead(3));
    }

    SECTION("spool_id parses as the integer AFC actually emits") {
        // AFC_lane.py emits `int(self.spool_id) if self.spool_id else None` —
        // never a string. apply_lane_status guards on is_number_integer(), so a
        // fixture carrying a string would be silently dropped (#1154).
        auto lane1 = load_fixture("afc_stepper_lane1.json");
        REQUIRE((lane1["spool_id"].is_number_integer() || lane1["spool_id"].is_null()));
        if (lane1["spool_id"].is_number_integer()) {
            CHECK(afc.get_slot_info(0).spoolman_id == lane1["spool_id"].get<int>());
        }
    }

    SECTION("a clean capture reports no fault") {
        auto state = load_fixture("afc_state.json");
        REQUIRE_FALSE(state["error_state"].get<bool>()); // guard: capture is clean
        CHECK_FALSE(afc.current_error().has_value());
    }

    SECTION("topology is derived, not defaulted") {
        CHECK(afc.get_topology() == PathTopology::HUB);
    }

    SECTION("a lane with no Spoolman link contributes no nozzle temperature") {
        // The capture has extruder_temp null on every lane — AFC only fills it
        // from a linked spool's settings_extruder_temp. Null is the clear, so
        // the slot must stay at 0 and let the filament DB supply the preheat
        // rather than inheriting a stale or invented number (#1149).
        auto lane1 = load_fixture("afc_stepper_lane1.json");
        REQUIRE(lane1["extruder_temp"].is_null()); // guard: capture has no link
        CHECK(afc.get_slot_info(0).nozzle_temp_min == 0);
        CHECK(afc.get_slot_info(0).nozzle_temp_max == 0);
    }
}

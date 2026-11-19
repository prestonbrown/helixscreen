// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The generic Lane Status Surface consumer (lane-status-spec RFC 0001): which
// printers it claims, how each spec field lands on the slots, and what it does
// with absent, extra and foreign data. The payloads follow SPEC.md §2.1 and §5.

#include "../helix_test_fixture.h"
#include "ams_backend.h"
#include "ams_backend_lane_status.h"
#include "ams_error.h"
#include "ams_types.h"
#include "core/update_queue.h"
#include "lane_status_parse.h"
#include "printer_discovery.h"
#include "test_helpers/registered_backend.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsBackendLaneStatus;
using helix::AmsResult;
using helix::AmsType;
using helix::SlotStatus;
using json = nlohmann::json;

namespace {

/// The real backend, running, with every command it sends captured.
class LaneStatusHarness : public AmsBackendLaneStatus {
  public:
    LaneStatusHarness() : AmsBackendLaneStatus(nullptr, nullptr) {
        running_ = true;
    }
    ~LaneStatusHarness() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    void feed(const json& lane_status) {
        handle_status_update(
            json{{"method", "notify_status_update"},
                 {"params", json::array({json{{"lane_status", lane_status}}, 0.0})}});
    }

    helix::AmsError execute_gcode(const std::string& gcode) override {
        commands.push_back(gcode);
        return helix::AmsErrorHelper::success();
    }

    std::vector<std::string> commands;
};

/// A first status: every key present, as SPEC.md §2.2 requires.
json controller_status() {
    return {{"spec_version", "0.1"},
            {"producer", {{"name", "acme_lanes"}, {"version", "2.3"}, {"url", nullptr}}},
            {"kind", "controller"},
            {"wraps", nullptr},
            {"capabilities", {"load", "unload"}},
            {"automap", "none"},
            {"compat_profiles", {"slicer"}},
            {"units", {"Acme 4"}},
            {"lanes", {"E1", "E2", "E3"}},
            {"current_lane", "E2"},
            {"state", "Idle"},
            {"error", {{"active", false}, {"message", nullptr}, {"type", nullptr}}}};
}

json record(const char* lane_name, const char* lane, bool loaded) {
    return {{"lss_version", "0.1"},
            {"writer", "acme_lanes"},
            {"lane", lane},
            {"lane_name", lane_name},
            {"loaded", loaded}};
}

helix::PrinterDiscovery discover(const std::vector<std::string>& names) {
    helix::PrinterDiscovery hw;
    hw.parse_objects(json(names));
    return hw;
}

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE("lane_status claims a printer only when no backend of its own does",
          "[ams][lane_status][discovery]") {
    SECTION("alone, it is the filament system") {
        const auto hw = discover({"extruder", "lane_status"});
        CHECK(hw.mmu_type() == AmsType::LANE_STATUS);
        REQUIRE(hw.detected_ams_systems().size() == 1);
        CHECK(hw.detected_ams_systems()[0].type == AmsType::LANE_STATUS);
        // It subscribes the spec's keys by name, and nothing else.
        const json objects = AmsBackendLaneStatus::required_status_objects(hw);
        REQUIRE(objects.contains("lane_status"));
        CHECK(objects["lane_status"].size() == 12);
        CHECK(helix::AmsBackend::required_status_objects(hw).contains("lane_status"));
    }
    SECTION("a vendor backend beside it keeps the printer") {
        CHECK(discover({"extruder", "AFC", "lane_status"}).mmu_type() == AmsType::AFC);
        CHECK(discover({"lane_status", "mmu"}).mmu_type() == AmsType::HAPPY_HARE);
    }
    SECTION("a native fallback keeps a printer a status-only producer wraps") {
        CHECK(discover({"extruder", "filament_detect", "lane_status"}).mmu_type() ==
              AmsType::SNAPMAKER);
    }
    SECTION("without the object nothing claims it, and nothing subscribes it") {
        const auto hw = discover({"extruder"});
        CHECK(hw.mmu_type() == AmsType::NONE);
        CHECK(AmsBackendLaneStatus::required_status_objects(hw).empty());
    }
}

// ============================================================================
// Each field mapped
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "every lane_status key lands on the system",
                 "[ams][lane_status]") {
    LaneStatusHarness b;
    b.feed(controller_status());

    const auto info = b.get_system_info();
    CHECK(info.type == AmsType::LANE_STATUS);
    CHECK(info.type_name == "acme_lanes"); // producer.name
    CHECK(info.version == "2.3");          // producer.version
    CHECK(info.total_slots == 3);          // lanes
    REQUIRE(info.units.size() == 1);
    CHECK(info.units[0].name == "Acme 4"); // units
    CHECK(b.lane_name(0) == "E1");
    CHECK(b.lane_name(2) == "E3");
    CHECK(info.current_slot == 1); // current_lane
    CHECK(info.filament_loaded);
    CHECK(b.get_slot_info(1).status == SlotStatus::LOADED);
    CHECK(info.action == AmsAction::IDLE);

    const auto s = b.status();
    CHECK(s.kind == helix::lane_status::Kind::Controller);
    CHECK(s.automap == "none");
    CHECK(s.compat_profiles == std::vector<std::string>{"slicer"});

    SECTION("state") {
        b.feed({{"state", "Loading"}});
        CHECK(b.get_system_info().action == AmsAction::LOADING);
        b.feed({{"state", "Unloading"}});
        CHECK(b.get_system_info().action == AmsAction::UNLOADING);
    }
    SECTION("error") {
        CHECK_FALSE(b.current_error().has_value());
        b.feed({{"state", "Error"},
                {"error", {{"active", true}, {"message", "Lane E2 jammed"}, {"type", "jam"}}}});
        CHECK(b.get_system_info().action == AmsAction::ERROR);
        CHECK(b.get_system_info().operation_detail == "Lane E2 jammed");
        const auto event = b.current_error();
        REQUIRE(event.has_value());
        CHECK(event->detail == "Lane E2 jammed");
    }
    SECTION("current_lane null") {
        b.feed({{"current_lane", nullptr}});
        CHECK(b.get_system_info().current_slot == -1);
        CHECK_FALSE(b.get_system_info().filament_loaded);
    }
}

TEST_CASE_METHOD(HelixTestFixture, "every lane_data field lands on its lane",
                 "[ams][lane_status]") {
    // Registered, so its lanes have ids the lane sources file under.
    helix::test::RegisteredBackend<LaneStatusHarness> reg;
    LaneStatusHarness& b = *reg;
    b.feed(controller_status());
    json full = record("E1", "0", true);
    full.update({{"material", "PETG"},
                 {"color", "#1A2B3C"},
                 {"nozzle_temp", 245},
                 {"bed_temp", 85},
                 {"filament_id", "OGFG99"},
                 {"vendor_name", "Polymaker"},
                 {"name", "PolyLite PETG"},
                 {"spool_id", 17},
                 {"remaining_weight_g", 640.0},
                 {"extruder_index", 0},
                 {"tool", "T3"}});
    b.adopt_records({{"T3", full}, {"lane3", record("E3", "2", false)}});

    const auto slot = b.get_slot_info(0);
    CHECK(slot.material == "PETG");
    CHECK(slot.color_rgb == 0x1A2B3Cu);
    CHECK(slot.nozzle_temp_recommended == 245);
    CHECK(slot.bed_temp == 85);
    CHECK(slot.brand == "Polymaker");          // vendor_name
    CHECK(slot.spool_name == "PolyLite PETG"); // name
    CHECK(slot.spoolman_id == 17);             // spool_id
    CHECK(slot.remaining_weight_g == 640.0f);
    CHECK(slot.mapped_tool == 3);                          // tool
    CHECK(slot.status == SlotStatus::AVAILABLE);           // loaded: true
    CHECK(b.get_slot_info(2).status == SlotStatus::EMPTY); // loaded: false
}

// ============================================================================
// Absent, extra and foreign data
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "an absent or extra key changes nothing it should not",
                 "[ams][lane_status]") {
    LaneStatusHarness b;
    b.feed(controller_status());

    SECTION("an update carries only what changed") {
        b.feed({{"state", "Loading"}, {"acme_motor_temp", 41}});
        CHECK(b.get_system_info().total_slots == 3);
        CHECK(b.get_system_info().current_slot == 1);
        CHECK(b.get_system_info().type_name == "acme_lanes");
    }
    SECTION("a lane with no record and an unknown presence is unknown") {
        CHECK(b.get_slot_info(0).status == SlotStatus::UNKNOWN);
        CHECK(b.get_slot_info(0).material.empty());
    }
    SECTION("a lane name no G-code could carry, or a repeat, is dropped") {
        b.feed({{"lanes", {"E1", "bad lane", "E1", "E2"}}});
        CHECK(b.get_system_info().total_slots == 2);
        CHECK(b.lane_name(1) == "E2");
    }
    SECTION("a spec major this reader does not know shows no lanes") {
        b.feed({{"spec_version", "1.0"}});
        CHECK(b.get_system_info().total_slots == 0);
        CHECK_FALSE(b.status().offers("load"));
    }
    SECTION("a first status with keys missing reads as their empty values") {
        LaneStatusHarness sparse;
        sparse.feed({{"spec_version", "0.1"}, {"lanes", {"a"}}});
        const auto s = sparse.status();
        CHECK(s.readable);
        CHECK(s.kind == helix::lane_status::Kind::Unknown);
        CHECK(s.capabilities.empty());
        CHECK(sparse.get_system_info().total_slots == 1);
        CHECK(sparse.get_system_info().type_name == "Lane Status");
    }
}

TEST_CASE("lane_data records join by lane_name under the writer rule", "[ams][lane_status]") {
    const std::vector<std::string> lanes = {"E1", "E2", "E3"};
    json e2 = record("E2", "7", true); // lane follows the slicer tool, not display order
    e2["material"] = "ABS";
    json foreign = record("E1", "0", true);
    foreign["writer"] = "someone_else";
    foreign["material"] = "PLA";
    json legacy = {{"lane", "2"}, {"material", "TPU"}};
    json legacy_shadowed = {{"lane", "1"}, {"material", "PC"}};
    json future = record("E3", "2", true);
    future["lss_version"] = "1.0";
    const json doc = {{"T7", e2},
                      {"x", foreign},
                      {"lane3", legacy},
                      {"lane2", legacy_shadowed},
                      {"bad", "not a record"},
                      {"f", future}};

    const auto joined = helix::lane_status::join_lane_records(doc, lanes, "acme_lanes");
    REQUIRE(joined.size() == 3);
    // Another producer's record is not this producer's lane.
    CHECK_FALSE(joined[0].has_value());
    // Joined by lane_name, and the producer's record beats a legacy one.
    REQUIRE(joined[1].has_value());
    CHECK(joined[1]->identity.material == "ABS");
    // A legacy record (no writer) still reads, by its lane index; the unknown
    // lss major beside it is skipped.
    REQUIRE(joined[2].has_value());
    CHECK(joined[2]->identity.material == "TPU");
}

// ============================================================================
// Commands
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "a controller's listed commands are sent; nothing else is",
                 "[ams][lane_status]") {
    LaneStatusHarness b;
    json status = controller_status();
    status["current_lane"] = nullptr;
    b.feed(status);
    b.adopt_records({{"a", record("E3", "2", true)}});

    SECTION("load and unload name the lane") {
        CHECK(b.load_filament(2).success());
        CHECK(b.unload_filament(2).success());
        CHECK(b.commands == std::vector<std::string>{"CHANGE_TOOL LANE=E3", "TOOL_UNLOAD LANE=E3"});
    }
    SECTION("a status-only producer offers neither") {
        b.feed({{"kind", "status_only"}});
        CHECK(b.load_filament(2).result == AmsResult::NOT_SUPPORTED);
        CHECK_FALSE(b.can_unload_from_toolhead(2));
        CHECK(b.commands.empty());
    }
    SECTION("a controller that does not list a command does not get it") {
        b.feed({{"capabilities", {"unload"}}});
        CHECK(b.load_filament(2).result == AmsResult::NOT_SUPPORTED);
        CHECK(b.commands.empty());
    }
    SECTION("the producer owns the records, so an edit is refused") {
        CHECK(b.apply_user_edit(2, b.get_slot_info(2)).result == AmsResult::NOT_SUPPORTED);
        CHECK_FALSE(b.clear_spool_refusal(2).empty());
    }
}

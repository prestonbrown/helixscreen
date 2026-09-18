// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_openams.h"
#include "printer_discovery.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

namespace {

class RecordingOpenAmsBackend : public helix::AmsBackendOpenAms {
  public:
    RecordingOpenAmsBackend() : AmsBackendOpenAms(nullptr, nullptr) {}

    using AmsBackendOpenAms::do_load_filament;
    using AmsBackendOpenAms::do_unload_filament;
    using AmsBackendOpenAms::handle_status_update;

    std::vector<std::string> sent;
    bool send_succeeds = true;

  protected:
    helix::AmsError
    send_operation_gcode(const std::string& gcode, std::function<void()> on_complete,
                         std::function<void(const MoonrakerError&)> on_error) override {
        (void)on_complete;
        (void)on_error;
        sent.push_back(gcode);
        return send_succeeds ? helix::AmsErrorHelper::success()
                             : helix::AmsError(helix::AmsResult::NOT_CONNECTED);
    }
};

json contract(
    const json& lanes = json::array({json{
        {"id", "fps"}, {"state", "loaded"}, {"current_group", "T1"}, {"current_slot", 6}}})) {
    return json{
        {"oams_manager",
         json{{"api_version", 1},
              {"schema", "openams.manager"},
              {"ready", true},
              {"commands", json{{"load", "OPENAMS_LOAD"},
                                {"unload", "OPENAMS_UNLOAD"},
                                {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                                {"reset", "OAMSM_CLEAR_ERRORS"}}},
              {"lanes", lanes},
              {"units",
               json::array(
                   {json{{"id", "1"},
                         {"name", "AMS 1"},
                         {"kind", "oams"},
                         {"topology", "hub"},
                         {"lane", "fps"},
                         {"connected", true},
                         {"slots",
                          json::array(
                              {json{{"id", 4}, {"bay", 0}, {"ready", true}, {"loaded", false}},
                               json{{"id", 6}, {"bay", 1}, {"ready", true}, {"loaded", true}}})}},
                    json{{"id", "follower"},
                         {"name", "Direct feeder"},
                         {"kind", "follower"},
                         {"topology", "linear"},
                         {"lane", "fps-1"},
                         {"connected", true},
                         {"slots",
                          json::array({json{
                              {"id", 9}, {"bay", 0}, {"ready", true}, {"loaded", false}}})}}})},
              {"groups", json::array({json{{"name", "T0"}, {"lane", "fps"}, {"slots", {4}}},
                                      json{{"name", "T1"}, {"lane", "fps"}, {"slots", {6}}},
                                      json{{"name", "T2"}, {"lane", "fps-1"}, {"slots", {9}}}})}}}};
}

} // namespace

TEST_CASE("native OpenAMS discovery does not require AFC objects", "[ams][openams]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"oams_manager", "fps", "oams unit1"}));

    REQUIRE(discovery.has_mmu());
    REQUIRE(discovery.mmu_type() == helix::AmsType::OPENAMS);
    REQUIRE(discovery.detected_ams_systems().size() == 1);
    REQUIRE(discovery.detected_ams_systems()[0].name == "OpenAMS");
}

TEST_CASE("native OpenAMS wins deterministically over legacy AFC objects", "[ams][openams]") {
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(json::array({"oams_manager", "AFC", "AFC_OpenAMS unit1"}));

    REQUIRE(discovery.mmu_type() == helix::AmsType::OPENAMS);
}

TEST_CASE("OpenAMS v1 snapshot maps units slots groups and mixed topology", "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    backend.handle_status_update(contract());

    const auto info = backend.get_system_info();
    REQUIRE(info.type == helix::AmsType::OPENAMS);
    REQUIRE(info.version == "1");
    REQUIRE(info.total_slots == 3);
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].topology == helix::PathTopology::HUB);
    REQUIRE(info.units[1].topology == helix::PathTopology::LINEAR);
    REQUIRE(backend.get_topology() == helix::PathTopology::MIXED);
    REQUIRE(info.current_slot == 1);
    REQUIRE(info.current_tool == 1);
    REQUIRE(info.filament_loaded);
    REQUIRE(info.units[0].slots[1].status == helix::SlotStatus::LOADED);
    REQUIRE(info.units[0].slots[0].mapped_tool == 0);
    REQUIRE(info.units[1].slots[0].mapped_tool == 2);
}

TEST_CASE("OpenAMS load dispatch uses advertised high-level command and remote slot id",
          "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    backend.handle_status_update(contract());

    const auto result = backend.do_load_filament(0);

    REQUIRE(result.success());
    REQUIRE(backend.sent == std::vector<std::string>{"OPENAMS_LOAD GROUP=T0 SLOT=4"});
    REQUIRE(backend.get_current_action() == helix::AmsAction::LOADING);
}

TEST_CASE("OpenAMS clears optimistic action when dispatch fails synchronously", "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    backend.handle_status_update(contract());
    backend.send_succeeds = false;

    const auto result = backend.do_load_filament(0);

    REQUIRE_FALSE(result.success());
    REQUIRE(backend.get_current_action() == helix::AmsAction::IDLE);
    REQUIRE(backend.get_system_info().pending_target_slot == -1);
}

TEST_CASE("OpenAMS retains topology across lane-only subscription deltas", "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    backend.handle_status_update(contract());
    backend.handle_status_update(
        json{{"oams_manager", json{{"lanes", json::array({json{{"id", "fps"},
                                                               {"state", "unloading"},
                                                               {"current_group", "T1"},
                                                               {"current_slot", 6}}})}}}});

    const auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 3);
    REQUIRE(info.action == helix::AmsAction::UNLOADING);
}

TEST_CASE("OpenAMS rejects an unsupported status API version", "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    json bad = contract();
    bad["oams_manager"]["api_version"] = 2;

    backend.handle_status_update(bad);

    const auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 0);
    REQUIRE(info.action == helix::AmsAction::ERROR);
    REQUIRE(info.operation_detail.find("Unsupported") != std::string::npos);
}

TEST_CASE("OpenAMS rejects an unknown generic unit topology", "[ams][openams]") {
    RecordingOpenAmsBackend backend;
    json bad = contract();
    bad["oams_manager"]["units"][0]["topology"] = "new-family-name";

    backend.handle_status_update(bad);

    const auto info = backend.get_system_info();
    REQUIRE(info.total_slots == 0);
    REQUIRE(info.action == helix::AmsAction::ERROR);
    REQUIRE(info.operation_detail.find("topology") != std::string::npos);
}

// SPDX-License-Identifier: GPL-3.0-or-later

// Lane Status Surface mock hardware (HELIX_MOCK_AMS=lane_status): a producer no
// backend here knows, publishing the `lane_status` object and writing its
// `lane_data` records the way lane-status-spec RFC 0001 describes, so real
// discovery builds the generic AmsBackendLaneStatus under --test.
//
// HELIX_MOCK_LANE_STATUS_KIND=status_only makes it a status-only producer, which
// lists no capabilities: the panel then offers no Load or Unload.

#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdlib>
#include <string>

using namespace helix;
using mock_internal::effective_mock_ams_env;

namespace {

constexpr const char* kProducer = "mock_lanes";

struct MockLane {
    const char* name;
    bool loaded;
    const char* material;
    const char* color;
    const char* vendor;
    const char* product;
    int nozzle;
    int bed;
};

constexpr std::array<MockLane, 4> kLanes = {{
    {"lane1", true, "PLA", "#E63A2E", "Polymaker", "PolyLite PLA", 210, 60},
    {"lane2", true, "PETG", "#2E7BE6", "Prusament", "PETG Galaxy Black", 240, 80},
    {"lane3", false, nullptr, nullptr, nullptr, nullptr, 0, 0},
    {"lane4", true, "ABS", "#F2C230", "eSUN", "ABS+", 250, 100},
}};

bool status_only() {
    const char* kind = std::getenv("HELIX_MOCK_LANE_STATUS_KIND");
    return kind != nullptr && std::string(kind) == "status_only";
}

nlohmann::json text_or_null(const char* s) {
    return s ? nlohmann::json(s) : nlohmann::json(nullptr);
}

/// A record's `tool`, in the T<n> wire format the spec defines.
nlohmann::json tool_wire(std::size_t i) {
    return "T" + std::to_string(i); // DISPLAY_NUMBERING_OK: lane_data wire format, not a label
}

nlohmann::json positive_or_null(int v) {
    return v > 0 ? nlohmann::json(v) : nlohmann::json(nullptr);
}

} // namespace

bool MoonrakerClientMock::is_mock_lane_status() const {
    return effective_mock_ams_env() == "lane_status";
}

nlohmann::json MoonrakerClientMock::lane_status_json() const {
    const bool passive = status_only();
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto& lane : kLanes) {
        lanes.push_back(lane.name);
    }
    const int current = lane_status_current_.load();
    // Every key from the first status, null for unknown (SPEC.md §2.2).
    return {
        {"spec_version", "0.1"},
        {"producer", {{"name", kProducer}, {"version", "1.0"}, {"url", nullptr}}},
        {"kind", passive ? "status_only" : "controller"},
        {"wraps", passive ? nlohmann::json("mock_feeder") : nlohmann::json(nullptr)},
        {"capabilities",
         passive ? nlohmann::json::array() : nlohmann::json::array({"load", "unload"})},
        {"automap", "none"},
        {"compat_profiles", nlohmann::json::array({"slicer"})},
        {"units", nlohmann::json::array({"Mock Lanes"})},
        {"lanes", lanes},
        {"current_lane", current >= 0 && current < static_cast<int>(kLanes.size())
                             ? nlohmann::json(kLanes[static_cast<std::size_t>(current)].name)
                             : nlohmann::json(nullptr)},
        {"state", "Idle"},
        {"error", {{"active", false}, {"message", nullptr}, {"type", nullptr}}},
    };
}

void MoonrakerClientMock::seed_lane_status_records() {
    for (std::size_t i = 0; i < kLanes.size(); ++i) {
        const MockLane& lane = kLanes[i];
        // An empty lane keeps its keys, with null values (SPEC.md §5).
        mock_db_set("lane_data", std::string("lane") + std::to_string(i),
                    {{"lss_version", "0.1"},
                     {"writer", kProducer},
                     {"lane", std::to_string(i)},
                     {"lane_name", lane.name},
                     {"loaded", lane.loaded},
                     {"material", text_or_null(lane.material)},
                     {"color", text_or_null(lane.color)},
                     {"nozzle_temp", positive_or_null(lane.nozzle)},
                     {"bed_temp", positive_or_null(lane.bed)},
                     {"filament_id", nullptr},
                     {"vendor_name", text_or_null(lane.vendor)},
                     {"name", text_or_null(lane.product)},
                     {"spool_id", nullptr},
                     {"extruder_index", 0},
                     {"tool", lane.loaded ? tool_wire(i) : nlohmann::json(nullptr)}});
    }
    spdlog::info("[MoonrakerClientMock] lane_status mock: {} lanes, kind={}", kLanes.size(),
                 status_only() ? "status_only" : "controller");
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_lane_status(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    if (status_only() || (cmd != "CHANGE_TOOL" && cmd != "TOOL_UNLOAD")) {
        return std::nullopt;
    }
    if (cmd == "TOOL_UNLOAD") {
        lane_status_current_ = -1;
    } else {
        const size_t pos = gcode.find("LANE=");
        const std::string name =
            pos == std::string::npos ? "" : gcode.substr(pos + 5, gcode.find(' ', pos) - pos - 5);
        for (std::size_t i = 0; i < kLanes.size(); ++i) {
            if (name == kLanes[i].name && kLanes[i].loaded) {
                lane_status_current_ = static_cast<int>(i);
            }
        }
    }
    dispatch_status_update({{"lane_status", lane_status_json()}});
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later

// Native Z-Mod IFS mock hardware on an AD5X: the stock four-port IFS
// (HELIX_MOCK_AMS=ifs) or one whose IFS port runs through an IFS Jacker
// (ifs-jacker), so the firmware drives more than four channels. The zmod_ifs /
// zmod_color frames carry that count as color_limit, the way Z-Mod publishes it,
// and the real AD5X IFS backend runs against them under --test.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

using namespace helix;
using mock_internal::effective_mock_ams_env;

namespace helix {
namespace {

struct JackerLane {
    const char* material;
    const char* hex;
};

// One entry per port Z-Mod can drive; the mock shows the first
// ifs_jacker_ports_ of them.
constexpr std::array<JackerLane, 16> kJackerLanes{{{"PLA", "A03CF7"},
                                                   {"PETG", "00FF00"},
                                                   {"ABS", "FF8800"},
                                                   {"TPU", "FFFFFF"},
                                                   {"PLA", "1E88E5"},
                                                   {"PLA", "E53935"},
                                                   {"PETG", "FDD835"},
                                                   {"PLA", "212121"},
                                                   {"PLA", "8E24AA"},
                                                   {"PETG", "00897B"},
                                                   {"ABS", "6D4C41"},
                                                   {"PLA", "F06292"},
                                                   {"PLA", "C0CA33"},
                                                   {"PETG", "3949AB"},
                                                   {"ABS", "BDBDBD"},
                                                   {"PLA", "FF7043"}}};

// The stock IFS's four spools.
constexpr std::array<JackerLane, 4> kStockLanes{
    {{"PLA", "000000"}, {"PETG", "FFFFFF"}, {"PLA", "8000FF"}, {"ABS", "804000"}}};

/// PRUTOK= / CHANNEL= value of a Z-Mod IFS command line; -1 when absent.
int port_param(const std::string& gcode) {
    for (const char* key : {"PRUTOK=", "CHANNEL="}) {
        const size_t p = gcode.find(key);
        if (p != std::string::npos) {
            return text_io::parse_leading<int>(gcode.substr(p + std::string(key).size()))
                .value_or(-1);
        }
    }
    return -1;
}

} // namespace
} // namespace helix

bool MoonrakerClientMock::is_mock_zmod_ifs_stock() const {
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "ifs" || ams_type == "ad5x" || ams_type == "ad5x_ifs";
}

bool MoonrakerClientMock::is_mock_zmod_ifs() const {
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "ifs-jacker" || ams_type == "ifs_jacker" || is_mock_zmod_ifs_stock();
}

void MoonrakerClientMock::init_ifs_jacker_from_env() {
    if (!is_mock_zmod_ifs()) {
        return;
    }
    if (is_mock_zmod_ifs_stock()) {
        // The stock IFS: four full ports, port 1 in the head.
        ifs_jacker_ports_ = 4;
        ifs_jacker_presence_.store(0xFu);
        ifs_jacker_chan_.store(1);
        ifs_jacker_head_loaded_.store(true);
        return;
    }
    // Ports behind the jacker (HELIX_MOCK_IFS_PORTS, 4..16; default two IFS
    // units). Ports 3 and 7 start empty so the overview shows both states.
    if (const char* ports = std::getenv("HELIX_MOCK_IFS_PORTS")) {
        ifs_jacker_ports_ = std::clamp(text_io::parse_leading<int>(ports).value_or(8), 4, 16);
    }
    const uint32_t all = (1u << ifs_jacker_ports_) - 1u;
    ifs_jacker_presence_.store(all & ~((1u << 2) | (1u << 6)));
}

nlohmann::json MoonrakerClientMock::zmod_ifs_status() const {
    return is_mock_zmod_ifs() ? ifs_jacker_zmod_ifs_json() : json::object();
}

nlohmann::json MoonrakerClientMock::ifs_jacker_zmod_ifs_json() const {
    const uint32_t presence = ifs_jacker_presence_.load();
    json ports = json::array();
    for (int i = 0; i < ifs_jacker_ports_; ++i) {
        ports.push_back((presence & (1u << i)) != 0);
    }
    // Shape of zmod_ifs.get_status(): the IFS_STATUS keys plus color_limit.
    return json{{"available", true}, {"color_limit", ifs_jacker_ports_},
                {"State", 5},        {"Ports", std::move(ports)},
                {"Silk", presence},  {"Chan", ifs_jacker_chan_.load()},
                {"Insert", 0},       {"NeedInsert", false},
                {"Stall", false},    {"stall_state", 0}};
}

nlohmann::json MoonrakerClientMock::ifs_jacker_zmod_color_json() const {
    const uint32_t presence = ifs_jacker_presence_.load();
    json slots = json::array();
    for (int i = 0; i < ifs_jacker_ports_; ++i) {
        const auto& lane = is_mock_zmod_ifs_stock() ? kStockLanes[static_cast<size_t>(i)]
                                                    : kJackerLanes[static_cast<size_t>(i)];
        slots.push_back({{"ID", std::to_string(i + 1)},
                         {"Material", lane.material},
                         {"Color", ""},
                         {"HEX", lane.hex},
                         {"hasFilament", (presence & (1u << i)) != 0}});
    }
    // Shape of zmod_color.get_status() on an AD5X: one IFS-fed extruder.
    return json{{"ifs", true},
                {"display", false},
                {"color_limit", ifs_jacker_ports_},
                {"valid_types", json::array({"PLA", "PETG", "ABS", "TPU", "?"})},
                {"hidden_types", json::array()},
                {"extruder_sensor", ifs_jacker_head_loaded_.load()},
                {"total_tools", 1},
                {"active_tool_id", ifs_jacker_head_loaded_.load() ? 0 : -1},
                {"channel", ifs_jacker_chan_.load()},
                {"slots", std::move(slots)}};
}

bool MoonrakerClientMock::apply_ifs_jacker_gcode(const std::string& cmd, const std::string& gcode) {
    const auto valid = [this](int port) { return port >= 1 && port <= ifs_jacker_ports_; };

    if (cmd == "INSERT_PRUTOK_IFS" || cmd == "A_CHANGE_FILAMENT") {
        const int port = port_param(gcode);
        if (valid(port)) {
            ifs_jacker_chan_.store(port);
            ifs_jacker_head_loaded_.store(true);
            spdlog::info("[MoonrakerClientMock] IFS Jacker: loaded port {}", port);
        }
        return true;
    }
    if (cmd == "_IFS_REMOVE_CURRENT_PRUTOK" || cmd == "REMOVE_PRUTOK_IFS" ||
        cmd == "_REMOVE_PRUTOK_IFS") {
        ifs_jacker_head_loaded_.store(false);
        spdlog::info("[MoonrakerClientMock] IFS Jacker: unloaded port {}", ifs_jacker_chan_.load());
        return true;
    }
    if (cmd == "IFS_F11") {
        // Cold retract of one idle lane: the silk sensor clears.
        const int port = port_param(gcode);
        if (valid(port)) {
            ifs_jacker_presence_.fetch_and(~(1u << (port - 1)));
            ifs_jacker_chan_.store(port);
            spdlog::info("[MoonrakerClientMock] IFS Jacker: ejected port {}", port);
        }
        return true;
    }
    return cmd == "IFS_F24" || cmd == "IFS_F39" || cmd == "SET_EXTRUDER_SLOT";
}

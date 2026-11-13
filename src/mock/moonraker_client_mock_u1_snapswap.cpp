// SPDX-License-Identifier: GPL-3.0-or-later

// A stock Snapmaker U1's SnapSwap (HELIX_MOCK_AMS=snapmaker, the snapmaker_u1
// persona's default): the filament_detect RFID frame, print_task_config, each
// head's park/active pins and the feeder frames, plus the head swap and the
// filament-config command that change them, so real discovery builds the
// production AmsBackendSnapmaker under --test.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

using namespace helix;
using mock_internal::effective_mock_ams_env;

namespace {

/// The value of ` KEY=` on a command line, empty when absent.
std::string param(const std::string& gcode, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + needle.size();
    const size_t end = gcode.find_first_of(" \t", start);
    return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string extruder_name(int head) {
    return head == 0 ? "extruder" : "extruder" + std::to_string(head);
}

} // namespace

bool MoonrakerClientMock::is_mock_u1_snapswap() const {
    const std::string mode = effective_mock_ams_env();
    return mode == "snapmaker" || mode == "snapswap" || mode == "u1";
}

nlohmann::json MoonrakerClientMock::u1_snapswap_status_json() const {
    std::array<U1Spool, kU1Channels> spools;
    int head = 0;
    {
        std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
        spools = u1_spools_;
        head = u1_active_head_;
    }

    nlohmann::json info = nlohmann::json::array();
    nlohmann::json state = nlohmann::json::array();
    nlohmann::json exist = nlohmann::json::array();
    nlohmann::json type = nlohmann::json::array();
    nlohmann::json vendor = nlohmann::json::array();
    nlohmann::json rgba = nlohmann::json::array();
    for (int i = 0; i < kU1Channels; ++i) {
        const U1Spool& s = spools[static_cast<size_t>(i)];
        // A Snapmaker RFID spool's tag, as the reader reports it.
        info.push_back({{"MAIN_TYPE", s.type},
                        {"SUB_TYPE", "Basic"},
                        {"VENDOR", s.vendor},
                        {"MANUFACTURER", s.vendor},
                        {"ARGB_COLOR", 0xFF000000u | s.rgb},
                        {"CARD_UID", {144, 32, 196, i + 1}}});
        // Fed through to the head, the entrance sensor reads 0.
        state.push_back(0);
        // The feeder's port sensor: an unloaded spool leaves the head's record empty.
        exist.push_back(u1_filament_detected_[static_cast<size_t>(i)]);
        type.push_back(s.type);
        vendor.push_back(s.vendor);
        rgba.push_back(fmt::format("{:06X}FF", s.rgb));
    }

    nlohmann::json out;
    out["filament_detect"] = {{"info", info}, {"state", state}};
    out["print_task_config"] = {{"filament_exist", exist},
                                {"filament_type", type},
                                {"filament_vendor", vendor},
                                {"filament_color_rgba", rgba},
                                {"extruder_map_table", {0, 1, 2, 3}},
                                {"extruders_used", {false, false, false, false}}};
    for (int i = 0; i < kU1Channels; ++i) {
        out[extruder_name(i)] = {{"state", i == head ? "ACTIVE" : "PARKED"},
                                 {"park_pin", i != head},
                                 {"active_pin", i == head},
                                 {"activating_move", false}};
    }
    out["filament_feed left"] = u1_feed_side_json(0);
    out["filament_feed right"] = u1_feed_side_json(2);
    return out;
}

nlohmann::json MoonrakerClientMock::mock_ams_hardware_status() const {
    nlohmann::json out = afc_status_json();
    if (is_mock_u1_snapswap()) {
        out.update(u1_snapswap_status_json());
    }
    return out;
}

void MoonrakerClientMock::merge_mock_ams_hardware_status(nlohmann::json& into,
                                                         const nlohmann::json* only) const {
    const nlohmann::json from = mock_ams_hardware_status();
    for (const auto& [name, value] : from.items()) {
        if (only && !only->contains(name)) {
            continue;
        }
        // A heater the caller already filled keeps its temperature fields.
        if (into.contains(name) && into[name].is_object() && value.is_object()) {
            into[name].update(value);
        } else {
            into[name] = value;
        }
    }
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_u1_snapswap(const std::string& gcode) {
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t\r\n"));

    if (cmd.size() == 2 && cmd[0] == 'T' && cmd[1] >= '0' && cmd[1] < '0' + kU1Channels) {
        // The head swap. The toolhead.extruder frame follows from the generic T<n>
        // handler, so the line is not consumed here.
        const int head = cmd[1] - '0';
        {
            std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
            u1_active_head_ = head;
        }
        spdlog::info("[MoonrakerClientMock] U1: head {} picked up", head);
        nlohmann::json frame;
        const nlohmann::json all = u1_snapswap_status_json();
        for (int i = 0; i < kU1Channels; ++i) {
            frame[extruder_name(i)] = all[extruder_name(i)];
        }
        dispatch_status_update(frame);
        return std::nullopt;
    }
    if (cmd == "SET_PRINT_FILAMENT_CONFIG") {
        const int head = text_io::parse_leading<int>(param(gcode, "CONFIG_EXTRUDER")).value_or(-1);
        if (head < 0 || head >= kU1Channels) {
            return std::nullopt;
        }
        {
            std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
            U1Spool& s = u1_spools_[static_cast<size_t>(head)];
            if (const std::string t = param(gcode, "FILAMENT_TYPE"); !t.empty()) {
                s.type = t;
            }
            if (const std::string v = param(gcode, "VENDOR"); !v.empty()) {
                s.vendor = v;
            }
            // RRGGBBAA; the alpha byte is dropped.
            if (const std::string c = param(gcode, "FILAMENT_COLOR_RGBA"); c.size() >= 6) {
                s.rgb = static_cast<uint32_t>(std::strtoul(c.substr(0, 6).c_str(), nullptr, 16));
            }
        }
        dispatch_status_update(
            {{"print_task_config", u1_snapswap_status_json()["print_task_config"]}});
        return 0;
    }
    return std::nullopt;
}

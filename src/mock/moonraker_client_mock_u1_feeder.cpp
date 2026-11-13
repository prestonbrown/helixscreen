// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mock Snapmaker U1's filament feeders: the filament_feed frames, the
// AUTO_FEEDING channel walk, and each head's Auto loading (FEED_AUTO AUTO=).

#include "core/text_io.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

/// The value of `KEY=value` in a command line, the parameter spelled exactly
/// as the backend sends it.
std::optional<std::string> feeder_param(const std::string& gcode, std::string_view key) {
    const std::string needle = " " + std::string(key) + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const size_t start = at + needle.size();
    return gcode.substr(start, gcode.find_first_of(" \t", start) - start);
}

} // namespace

nlohmann::json MoonrakerClientMock::u1_feed_side_json(int ext) const {
    // "filament_feed left" carries extruders 0/1, "right" carries 2/3.
    const int first = (ext < 2) ? 0 : 2;
    nlohmann::json channels = nlohmann::json::object();
    for (int i = first; i < first + 2; ++i) {
        channels["extruder" + std::to_string(i)] = {
            {"channel_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"channel_error", "ok"},
            {"channel_error_state", "none"},
            {"channel_action_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"disable_auto", u1_disable_auto_[static_cast<size_t>(i)]},
            {"filament_detected", u1_filament_detected_[static_cast<size_t>(i)]},
            {"module_exist", true},
        };
    }
    return channels;
}

void MoonrakerClientMock::publish_u1_channel_frame(int ext) {
    const std::string side = (ext < 2) ? "left" : "right";
    dispatch_status_update({{"filament_feed " + side, u1_feed_side_json(ext)}});
}

bool MoonrakerClientMock::apply_u1_feeding_gcode(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    // AUTO_FEEDING and AUTO_FEEDING_BATCH ACTION=DOING both drive a channel.
    // The BATCH START/END lines name no EXTRUDER, so they land in the "feeder
    // command, nothing to walk" branch and change no channel state.
    if (cmd.rfind("AUTO_FEEDING", 0) != 0) {
        return false;
    }
    const size_t e = gcode.find("EXTRUDER=");
    if (e == std::string::npos) {
        return true;
    }
    int ext = -1;
    try {
        ext = std::stoi(gcode.substr(e + 9));
    } catch (...) {
        return true;
    }
    if (ext < 0 || ext >= kU1Channels) {
        return true;
    }
    // "UNLOAD=1" contains "LOAD=1" as a substring; the direction must be read
    // unload-first.
    const bool unload = gcode.find("UNLOAD=1") != std::string::npos;
    if (!unload && u1_disable_auto_[static_cast<size_t>(ext)]) {
        spdlog::info("[MoonrakerClientMock] U1 feeder: extruder {} Auto loading is off; load "
                     "ignored",
                     ext);
        return true;
    }

    int fail_slot = -1;
    if (const char* fail_env = std::getenv("HELIX_MOCK_BATCH_FAIL_SLOT")) {
        try {
            fail_slot = std::stoi(fail_env);
        } catch (...) {
            fail_slot = -1;
        }
    }

    const size_t ext_idx = static_cast<size_t>(ext);
    // The state sequences the firmware's own filament_feed walks: preload then
    // load for a feed, heat then retract for an unload.
    u1_channel_state_[ext_idx] = unload ? "unload_heat_finish" : "preload_finish";
    if (!unload) {
        u1_filament_detected_[ext_idx] = true;
    }
    publish_u1_channel_frame(ext);

    const char* terminal = unload ? "unload_finish" : "load_finish";
    if (ext == fail_slot) {
        terminal = unload ? "unload_fail" : "load_fail";
    }
    u1_channel_state_[ext_idx] = terminal;
    if (unload && ext != fail_slot) {
        u1_filament_detected_[ext_idx] = false;
    }
    publish_u1_channel_frame(ext);
    spdlog::info("[MoonrakerClientMock] U1 feeder: extruder {} {} -> {}", ext,
                 unload ? "unload" : "load", terminal);
    return true;
}

bool MoonrakerClientMock::apply_u1_auto_mode_gcode(const std::string& gcode) {
    if (gcode.rfind("FEED_AUTO ", 0) != 0) {
        return false;
    }
    const auto module = feeder_param(gcode, "MODULE");
    const auto channel = feeder_param(gcode, "CHANNEL");
    const auto mode = feeder_param(gcode, "AUTO");
    if (!module || !channel || !mode) {
        return true;
    }
    // The mock printer's own wiring, as a stock U1's printer.cfg states it in
    // _FILAMENT_FEED_VARIABLE: head n is fed by kWiring[n].
    static constexpr std::array<std::pair<std::string_view, int>, kU1Channels> kWiring = {
        {{"left", 1}, {"left", 0}, {"right", 0}, {"right", 1}}};
    const std::string module_name = helix::text_io::to_lower(*module);
    const int channel_index = helix::text_io::parse_leading<int>(*channel).value_or(-1);
    int head = -1;
    for (int h = 0; h < kU1Channels; ++h) {
        const auto& [m, c] = kWiring[static_cast<size_t>(h)];
        if (m == module_name && c == channel_index) {
            head = h;
        }
    }
    if (head < 0) {
        return true;
    }
    u1_disable_auto_[static_cast<size_t>(head)] = *mode == "0";
    publish_u1_channel_frame(head);
    spdlog::info("[MoonrakerClientMock] U1 feeder: extruder {} Auto loading {}", head,
                 *mode == "0" ? "off" : "on");
    return true;
}

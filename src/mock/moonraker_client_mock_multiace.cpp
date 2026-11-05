// SPDX-License-Identifier: GPL-3.0-or-later

// multiACE mock hardware (HELIX_MOCK_AMS=multiace): the per-unit status frame, the
// head routes and the gcode that changes them.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using namespace helix;
using mock_internal::effective_mock_ams_env;

bool MoonrakerClientMock::is_mock_multiace() const {
    return effective_mock_ams_env() == "multiace";
}

namespace {

/// The mock's per-bay identity table, shared by every unit: bay 1 a tagged red
/// PLA, bay 2 a blue PETG, bay 3 empty, bay 4 a black PLA.
struct MultiaceBaySeed {
    const char* status;
    const char* material;
    const char* brand;
    std::array<int, 3> color;
};

constexpr std::array<MultiaceBaySeed, 4> kMultiaceBays{{
    {"ready", "PLA", "Anycubic", {255, 0, 0}},
    {"ready", "PETG", "Generic", {0, 0, 255}},
    {"empty", "", "", {0, 0, 0}},
    {"ready", "PLA", "Generic", {0, 0, 0}},
}};

/// One integer VAL= token of a gcode line, or @p fallback when absent.
int multiace_token(const std::string& gcode, const char* key, int fallback) {
    const std::string needle = std::string(key) + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return fallback;
    }
    try {
        return std::stoi(gcode.substr(at + needle.size()));
    } catch (...) {
        return fallback;
    }
}

} // namespace

nlohmann::json MoonrakerClientMock::multiace_status_json() const {
    std::lock_guard<std::mutex> lock(multiace_mutex_);
    const bool head_mode = multiace_mode_ != "multi";
    nlohmann::json ace = {
        {"api_version", 1},
        {"mode", head_mode ? "head" : "multi"},
        {"swap_phase", multiace_swap_phase_},
        {"swap_in_progress", multiace_swap_phase_ != "idle" && multiace_swap_phase_ != "done"},
        {"unload_all_active", false},
        {"event_seq", 0},
        {"quad_replenish", false},
        {"quad_first", false},
    };

    nlohmann::json head_ace = nlohmann::json::object();
    nlohmann::json head_feeder = nlohmann::json::object();
    nlohmann::json head_source = nlohmann::json::object();
    nlohmann::json ace_heads = nlohmann::json::array();
    for (int h = 0; h < 4; ++h) {
        const std::string key = std::to_string(h);
        // The engine wires one unit per head; a head with no unit to spare is
        // fed from the side feeder, the state head_mode_2ace_2feeder models.
        const bool ace_fed = !multiace_head_is_feeder(h);
        head_ace[key] = head_mode ? (ace_fed ? h : -1) : 0;
        head_feeder[key] = !ace_fed;
        if (ace_fed) {
            ace_heads.push_back(h);
        }
        const auto& route = multiace_route_[static_cast<size_t>(h)];
        if (route.first < 0) {
            head_source[key] = nullptr;
            continue;
        }
        const MultiaceBaySeed& bay = kMultiaceBays[static_cast<size_t>(route.second)];
        char color_hex[8];
        snprintf(color_hex, sizeof(color_hex), "%02X%02X%02X", bay.color[0], bay.color[1],
                 bay.color[2]);
        head_source[key] = {
            {"ace_index", route.first}, {"slot", route.second}, {"type", bay.material},
            {"color", color_hex},       {"brand", bay.brand},   {"subtype", ""},
            {"load_failed", false},
        };
    }
    ace["head_ace"] = head_ace;
    ace["head_source"] = head_source;
    ace["head_manual"] = {{"0", false}, {"1", false}, {"2", false}, {"3", false}};
    ace["head_feeder"] = head_feeder;
    ace["ace_heads"] = ace_heads;

    nlohmann::json aces = nlohmann::json::array();
    for (int u = 0; u < multiace_units_; ++u) {
        nlohmann::json slots = nlohmann::json::array();
        for (int s = 0; s < 4; ++s) {
            const MultiaceBaySeed& bay = kMultiaceBays[static_cast<size_t>(s)];
            slots.push_back({
                {"index", s},
                {"status", bay.status},
                {"material", bay.material},
                {"brand", bay.brand},
                {"color", json::array({bay.color[0], bay.color[1], bay.color[2]})},
                {"rfid", s == 0 ? 2 : 0},
            });
        }
        aces.push_back({
            {"idx", u},
            {"connected", true},
            {"protocol", "v1"},
            {"model", "ACE Pro"},
            {"firmware", "V1.3.84"},
            {"status", "ready"},
            {"temp", 25},
            {"humidity", u == 0 ? json(31.5) : json(nullptr)},
            {"gate_status", json::array({1, 1, 0, 1})},
            {"slots", slots},
        });
    }
    ace["aces"] = aces;
    return ace;
}

bool MoonrakerClientMock::apply_multiace_gcode(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    const bool load = cmd == "ACE_LOAD_HEAD";
    const bool unload = cmd == "ACE_UNLOAD_HEAD" || cmd == "ACE_UNLOAD_ALL_HEADS";
    if (!load && !unload && cmd != "ACE_DRY" && cmd != "ACE_STOP_DRYING") {
        return false;
    }
    if (!is_mock_multiace()) {
        return false; // an ACE_* line on a printer the mock does not simulate
    }

    if (cmd == "ACE_DRY" || cmd == "ACE_STOP_DRYING") {
        spdlog::info("[MoonrakerClientMock] multiACE mock: {} acknowledged (dryer state is "
                     "static in the mock)",
                     cmd);
        return true;
    }

    std::vector<int> heads;
    if (cmd == "ACE_UNLOAD_ALL_HEADS") {
        // The engine's own loop unloads every head whose motion sensor sees
        // filament, ACE-fed, feeder and manual alike.
        for (int h = 0; h < 4; ++h) {
            if (u1_filament_detected_[static_cast<size_t>(h)]) {
                heads.push_back(h);
            }
        }
    } else {
        heads.push_back(std::clamp(multiace_token(gcode, "HEAD", 0), 0, 3));
    }

    for (int h : heads) {
        const auto i = static_cast<size_t>(h);
        if (load) {
            int ace = multiace_token(gcode, "ACE", -1);
            int slot = multiace_token(gcode, "SLOT", -1);
            {
                std::lock_guard<std::mutex> lock(multiace_mutex_);
                if (multiace_head_is_feeder(h)) {
                    // A feeder head loads from the side port: no bay is routed
                    // and head_source stays null, the engine's own shape.
                    ace = -1;
                } else {
                    if (ace < 0 || slot < 0) {
                        // A bare load takes the unit wired to the head (head
                        // mode) or unit 1, bay 1.
                        ace = (multiace_mode_ != "multi") ? h : 0;
                        slot = 0;
                    }
                    // HEAD is clamped where it is read; an out-of-range ACE or
                    // SLOT must not reach the route table or the bay seeds.
                    ace = std::clamp(ace, 0, multiace_units_ - 1);
                    slot = std::clamp(slot, 0, 3);
                    if (multiace_route_[i].first >= 0) {
                        // The engine feeds nothing into an occupied head.
                        spdlog::warn("[MoonrakerClientMock] multiACE mock: head {} already "
                                     "loaded - load feeds nothing",
                                     h);
                        continue;
                    }
                    multiace_route_[i] = {ace, slot};
                    multiace_swap_phase_ = "done";
                }
            }
            u1_channel_state_[i] = "preload_finish";
            u1_filament_detected_[i] = true;
            publish_u1_channel_frame(h);
            u1_channel_state_[i] = "load_finish";
            publish_u1_channel_frame(h);
        } else {
            bool had_route = false;
            {
                std::lock_guard<std::mutex> lock(multiace_mutex_);
                had_route = multiace_route_[i].first >= 0;
                multiace_route_[i] = {-1, -1};
                multiace_swap_phase_ = "idle";
            }
            if (had_route || u1_filament_detected_[i]) {
                u1_channel_state_[i] = "unload_heat_finish";
                publish_u1_channel_frame(h);
                // multiACE's feed unload parks the channel at preload_finish.
                u1_channel_state_[i] = "preload_finish";
                u1_filament_detected_[i] = false;
                publish_u1_channel_frame(h);
            }
        }
        spdlog::info("[MoonrakerClientMock] multiACE mock: {} head {}", load ? "load" : "unload",
                     h);
    }
    return true;
}

void MoonrakerClientMock::read_multiace_knobs() {
    std::lock_guard<std::mutex> lock(multiace_mutex_);
    if (const char* mode_env = std::getenv("HELIX_MOCK_MULTIACE_MODE")) {
        const std::string mode = helix::text_io::to_lower(mode_env);
        if (mode == "multi" || mode == "head") {
            multiace_mode_ = mode;
        } else {
            spdlog::warn("[MoonrakerClientMock] HELIX_MOCK_MULTIACE_MODE='{}' is not 'head' or "
                         "'multi' - using the default 'head'",
                         mode_env);
        }
    }
    if (const char* units_env = std::getenv("HELIX_MOCK_MULTIACE_UNITS")) {
        try {
            multiace_units_ = std::clamp(std::stoi(units_env), 1, 4);
        } catch (...) {
            spdlog::warn("[MoonrakerClientMock] HELIX_MOCK_MULTIACE_UNITS='{}' is not a number - "
                         "using 1",
                         units_env);
            multiace_units_ = 1;
        }
    }
    // One routed head at rest, so the path view has a chain to draw.
    const bool unrouted = std::all_of(multiace_route_.begin(), multiace_route_.end(),
                                      [](const auto& r) { return r.first < 0; });
    if (unrouted) {
        multiace_route_[0] = {0, 0};
    }
    spdlog::info("[MoonrakerClientMock] multiACE mock: mode={}, units={}", multiace_mode_,
                 multiace_units_);
}

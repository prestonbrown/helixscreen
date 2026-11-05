// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone IFS module mock hardware (HELIX_MOCK_AMS=ifs-module): the module's
// status, materials and variables plus the gcode that changes them, so the real
// backend runs under --test. The bare "ifs" value is AmsBackendMock's simulation.

#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <string>

using namespace helix;
using mock_internal::effective_mock_ams_env;

bool MoonrakerClientMock::is_mock_ifs_module() const {
    // "ifs-module", not "ifs": the bare value (and "ad5x") selects the
    // AmsBackendMock simulation in try_create_mock(); this mode runs the real
    // backend, so the two must not collide.
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "ifs-module" || ams_type == "ifs_module" || ams_type == "ad5x-module";
}

nlohmann::json MoonrakerClientMock::ifs_module_status_json() const {
    const int loaded = ifs_module_loaded_.load();
    const uint8_t presence = ifs_module_presence_.load();
    json loaded_channels = json::array();
    for (int i = 0; i < 4; ++i) {
        if (presence & (1u << i)) {
            loaded_channels.push_back(i + 1);
        }
    }
    // Shape mirrored from the module's ifs.py get_status().
    return json{{"connected", true},
                {"error", nullptr},
                {"channel_count", 4},
                {"version", "mock"},
                {"probed", true},
                {"state", 5},
                {"activity", "ready"},
                {"activity_channel", 0},
                {"active_channel", loaded},
                {"loaded_channels", std::move(loaded_channels)},
                {"moving_channels", json::array()},
                {"pending_insert_channels", json::array()},
                {"params", json::object()}};
}

nlohmann::json MoonrakerClientMock::ifs_module_materials_json() const {
    static const std::map<std::string, double> handling_temps{
        {"PLA", 220.0}, {"PLA-CF", 220.0}, {"SILK", 230.0},   {"TPU", 230.0},
        {"ABS", 250.0}, {"PETG", 250.0},   {"PETG-CF", 250.0}};

    json temps = json::object();
    for (const auto& [name, t] : handling_temps) {
        temps[name] = t;
    }

    json slots = json::object();
    {
        std::lock_guard<std::mutex> lock(ifs_module_materials_mutex_);
        for (const auto& [slot, tm] : ifs_module_materials_) {
            auto temp_it = handling_temps.find(tm.first);
            slots[std::to_string(slot)] = json{
                {"type", tm.first.empty() ? json(nullptr) : json(tm.first)},
                {"color", tm.second.empty() ? json(nullptr) : json("#" + tm.second)},
                {"temp", temp_it != handling_temps.end() ? json(temp_it->second) : json(nullptr)}};
        }
    }

    const int loaded = ifs_module_loaded_.load();
    json loaded_entry = nullptr;
    if (loaded >= 1 && slots.contains(std::to_string(loaded))) {
        loaded_entry = slots[std::to_string(loaded)];
    }
    return json{{"available", true},
                {"channel_count", 4},
                {"enabled", true},
                {"slots", std::move(slots)},
                {"loaded", std::move(loaded_entry)},
                {"purge_first_mm", json::object()},
                {"temperatures", std::move(temps)}};
}

nlohmann::json MoonrakerClientMock::ifs_module_vars_json() const {
    return json{{"variables", json{{"ifs_loaded", ifs_module_loaded_.load()},
                                   {"ifs_at_hub", ifs_module_at_hub_.load()}}}};
}

bool MoonrakerClientMock::apply_ifs_module_gcode(const std::string& cmd, const std::string& gcode) {
    auto slot_param = [&]() -> int {
        const size_t s = gcode.find("SLOT=");
        if (s == std::string::npos) {
            return -1;
        }
        try {
            return std::stoi(gcode.substr(s + 5));
        } catch (...) {
            return -1;
        }
    };
    auto value_param = [&](const char* key) -> std::optional<std::string> {
        const std::string pattern = std::string(key) + "=";
        const size_t p = gcode.find(pattern);
        if (p == std::string::npos) {
            return std::nullopt;
        }
        const size_t start = p + pattern.size();
        const size_t end = gcode.find_first_of(" \t", start);
        return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    if (cmd == "IFS_SET_MATERIAL") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            auto type = value_param("TYPE");
            auto color = value_param("COLOR");
            std::lock_guard<std::mutex> lock(ifs_module_materials_mutex_);
            auto& tm = ifs_module_materials_[slot];
            if (type) {
                tm.first = *type;
            }
            if (color) {
                tm.second = *color;
            }
            spdlog::info("[MoonrakerClientMock] IFS module: slot {} -> {} #{}", slot, tm.first,
                         tm.second);
        }
        return true;
    }
    if (cmd == "IFS_LOAD" || cmd == "IFS_SELECT") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            ifs_module_loaded_.store(slot);
            ifs_module_at_hub_.store(slot);
            ifs_module_head_loaded_.store(true);
            spdlog::info("[MoonrakerClientMock] IFS module: loaded slot {}", slot);
        }
        return true;
    }
    if (cmd == "IFS_UNLOAD") {
        ifs_module_loaded_.store(0);
        ifs_module_at_hub_.store(0);
        ifs_module_head_loaded_.store(false);
        spdlog::info("[MoonrakerClientMock] IFS module: unloaded");
        return true;
    }
    if (cmd == "_IFS_CLEAR_EXTRUDER") {
        // Cut and withdraw the extruder only: no lane moves, no record changes.
        ifs_module_head_loaded_.store(false);
        spdlog::info("[MoonrakerClientMock] IFS module: extruder cleared");
        return true;
    }
    if (cmd == "IFS_EJECT") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            ifs_module_presence_.fetch_and(static_cast<uint8_t>(~(1u << (slot - 1))));
            if (ifs_module_loaded_.load() == slot) {
                ifs_module_loaded_.store(0);
                ifs_module_head_loaded_.store(false);
            }
            if (ifs_module_at_hub_.load() == slot) {
                ifs_module_at_hub_.store(0);
            }
            spdlog::info("[MoonrakerClientMock] IFS module: ejected slot {}", slot);
        }
        return true;
    }
    // Bare T<n>: the module's slicer spelling (T0..T3 -> slots 1..4).
    if (cmd.size() >= 2 && cmd[0] == 'T' &&
        std::all_of(cmd.begin() + 1, cmd.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int tool = std::stoi(cmd.substr(1));
        if (tool >= 0 && tool < 4) {
            ifs_module_loaded_.store(tool + 1);
            ifs_module_at_hub_.store(tool + 1);
            ifs_module_head_loaded_.store(true);
            spdlog::info("[MoonrakerClientMock] IFS module: T{} -> slot {}", tool, tool + 1);
        }
        return true;
    }
    return false;
}

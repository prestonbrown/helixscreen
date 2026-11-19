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
    nlohmann::json subtype = nlohmann::json::array();
    nlohmann::json multi = nlohmann::json::array();
    std::array<int, kU1Channels> map{};
    bool replenish = true;
    {
        std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
        map = u1_map_table_;
        replenish = u1_auto_replenish_;
    }
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
        rgba.push_back(fmt::format("{:06X}{:02X}", s.rgb, s.alpha));
        subtype.push_back(s.subtype);
        multi.push_back(
            {{"nums", s.colors.empty() ? 1 : s.colors.size()},
             {"alpha", s.alpha},
             {"mode", s.multi_mode},
             {"colors", s.colors.empty() ? nlohmann::json::array({fmt::format("{:06X}", s.rgb)})
                                         : nlohmann::json(s.colors)}});
    }

    nlohmann::json out;
    out["filament_detect"] = {{"info", info}, {"state", state}};
    out["print_task_config"] = {{"filament_exist", exist},
                                {"filament_type", type},
                                {"filament_vendor", vendor},
                                {"filament_color_rgba", rgba},
                                {"filament_sub_type", subtype},
                                {"filament_color_multi", multi},
                                {"auto_replenish_filament", replenish},
                                {"extruder_map_table", map},
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
    out.update(happy_hare_status_json());
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
    if (gcode.find('\n') == std::string::npos) {
        return u1_snapswap_line(gcode);
    }
    // A script runs its lines in order, as Klipper does.
    bool handled = false;
    size_t start = 0;
    while (start < gcode.size()) {
        size_t end = gcode.find('\n', start);
        if (end == std::string::npos) {
            end = gcode.size();
        }
        handled |= u1_snapswap_line(gcode.substr(start, end - start)).has_value();
        start = end + 1;
    }
    return handled ? GcodeResult{0} : std::nullopt;
}

std::optional<int> MoonrakerClientMock::u1_snapswap_line(const std::string& gcode) {
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
            if (const std::string st = param(gcode, "FILAMENT_SUBTYPE"); !st.empty()) {
                s.subtype = st;
            }
            // RRGGBBAA, or the multi-colour form's COLORS list.
            if (const std::string c = param(gcode, "FILAMENT_COLOR_RGBA"); c.size() >= 6) {
                s.rgb = static_cast<uint32_t>(std::strtoul(c.substr(0, 6).c_str(), nullptr, 16));
                s.alpha = c.size() >= 8
                              ? static_cast<int>(std::strtoul(c.substr(6, 2).c_str(), nullptr, 16))
                              : 0xFF;
                s.colors.clear();
                s.multi_mode = 0;
            } else if (const std::string list = param(gcode, "COLORS"); !list.empty()) {
                s.colors.clear();
                size_t at = 0;
                while (at <= list.size()) {
                    const size_t comma = list.find(',', at);
                    s.colors.push_back(list.substr(
                        at, comma == std::string::npos ? std::string::npos : comma - at));
                    if (comma == std::string::npos) {
                        break;
                    }
                    at = comma + 1;
                }
                s.rgb = static_cast<uint32_t>(std::strtoul(s.colors.front().c_str(), nullptr, 16));
                s.multi_mode = text_io::parse_leading<int>(param(gcode, "MULTI_MODE")).value_or(0);
                s.alpha = text_io::parse_leading<int>(param(gcode, "ALPHA")).value_or(s.alpha);
            }
        }
        dispatch_status_update(
            {{"print_task_config", u1_snapswap_status_json()["print_task_config"]}});
        return 0;
    }
    if (cmd == "SET_PRINT_PREFERENCES") {
        const std::string v = param(gcode, "AUTO_REPLENISH_FILAMENT");
        if (v.empty()) {
            return std::nullopt;
        }
        {
            std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
            u1_auto_replenish_ = v == "1";
        }
        dispatch_status_update(
            {{"print_task_config", u1_snapswap_status_json()["print_task_config"]}});
        return 0;
    }
    if (cmd == "INNER_AUTO_REPLENISH_FILAMENT") {
        const int head = text_io::parse_leading<int>(param(gcode, "EXTRUDER")).value_or(-1);
        if (head >= 0 && head < kU1Channels) {
            u1_auto_replenish(head);
        }
        return 0;
    }
    return std::nullopt;
}

void MoonrakerClientMock::u1_auto_replenish(int empty_head) {
    // The firmware's search (print_task_config.py cmd_INNER_AUTO_REPLENISH_FILAMENT):
    // the first other head whose vendor, type, subtype and colour record equal
    // the empty head's and whose feeder can supply it. Nozzles are all alike here.
    if (print_phase_.load() != MockPrintPhase::PAUSED) {
        spdlog::info("[MoonrakerClientMock] U1: auto-replenish ignored, print not paused");
        return;
    }
    int replacement = -1;
    {
        std::lock_guard<std::mutex> lock(u1_snapswap_mutex_);
        if (!u1_auto_replenish_ || u1_active_head_ != empty_head) {
            spdlog::info("[MoonrakerClientMock] U1: auto-replenish off or head {} not active",
                         empty_head);
            return;
        }
        // The colour record as the status reports it: one colour unless listed.
        const auto colors_of = [](const U1Spool& s) {
            return s.colors.empty() ? std::vector<std::string>{fmt::format("{:06X}", s.rgb)}
                                    : s.colors;
        };
        const U1Spool& want = u1_spools_[static_cast<size_t>(empty_head)];
        for (int i = 0; i < kU1Channels && replacement < 0; ++i) {
            const U1Spool& s = u1_spools_[static_cast<size_t>(i)];
            const bool feeder_ready = u1_channel_state_[static_cast<size_t>(i)] == "load_finish" ||
                                      (u1_filament_detected_[static_cast<size_t>(i)] &&
                                       !u1_disable_auto_[static_cast<size_t>(i)]);
            if (i != empty_head && feeder_ready && s.vendor == want.vendor && s.type == want.type &&
                s.subtype == want.subtype && colors_of(s) == colors_of(want) &&
                s.alpha == want.alpha && s.multi_mode == want.multi_mode) {
                replacement = i;
            }
        }
        if (replacement < 0) {
            spdlog::info("[MoonrakerClientMock] U1: cannot auto-replenish head {}", empty_head);
            return;
        }
        for (int& h : u1_map_table_) {
            if (h == empty_head) {
                h = replacement;
            }
        }
    }
    spdlog::info("[MoonrakerClientMock] U1: auto-replenish T{} -> T{}", empty_head, replacement);
    dispatch_status_update({{"print_task_config", u1_snapswap_status_json()["print_task_config"]}});
    gcode_script(fmt::format("T{}", replacement));
    resume_print_internal();
}

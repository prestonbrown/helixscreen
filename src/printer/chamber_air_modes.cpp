// SPDX-License-Identifier: GPL-3.0-or-later

#include "chamber_air_modes.h"

#include "enclosure_top.h"
#include "printer_discovery.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "hv/json.hpp"

namespace helix::chamber_air {
namespace {

using nlohmann::json;

constexpr size_t kModeCount = 3;

constexpr std::array<AirMode, kModeCount> kAllModes = {AirMode::Recirculate, AirMode::Exhaust,
                                                       AirMode::Off};

/// One firmware that offers chamber air modes. Everything past `macros` is
/// optional; a provider fills in only what its firmware has.
struct Provider {
    const char* name;
    /// Identifies the firmware from discovered hardware.
    bool (*detect)(const PrinterDiscovery& hw);
    /// Macro per mode, indexed by AirMode; nullptr where it has none. A mode is
    /// offered only while its macro is actually defined. Unused when
    /// `mode_command` is set.
    std::array<const char*, kModeCount> macros;
    /// Builds the command for a mode from the targets; replaces `macros`.
    std::string (*mode_command)(AirMode mode, const ModeTargets& t) = nullptr;
    /// Reads a status frame; nullptr when the provider reports nothing.
    Reading (*read)(const PrinterDiscovery& hw, const json& status) = nullptr;
    /// Status objects the reader needs, as {object: [fields]}.
    json (*subscribe)() = nullptr;
    bool live_presence = false;
    bool mode_readback = false;
    bool targets = false;
    std::vector<OwnedFan> fans{};
    std::string (*fan_command)(FanRole role, int pct) = nullptr;
    const char* filter_reset = nullptr;
    int floor_pct = 0;
};

std::optional<double> number_at(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

/// `obj[outer][inner]` as a number, when both exist with the right types.
std::optional<double> nested_number(const json& obj, const char* outer, const char* inner) {
    auto it = obj.find(outer);
    if (it == obj.end() || !it->is_object()) {
        return std::nullopt;
    }
    return number_at(*it, inner);
}

std::string percent_as_fraction(int pct) {
    return fmt::format("{:.2f}", std::clamp(pct, 0, 100) / 100.0);
}

// --- Z-Mod on the FlashForge Creator 5 Pro -----------------------------------
//
// c5pro_generic.cfg: AIR_CIRCULATION_INTERNAL runs chamber_heat_fan 0.9 and
// chamber_loop_fan 0.3 with the exhaust fans off ("ABS");
// AIR_CIRCULATION_EXTERNAL runs chamber_fan 0.5 and chamber_cool_fan 0.7 with
// heat/loop off ("PLA, TPU"); AIR_CIRCULATION_STOP zeroes all four. The macros
// themselves are the capability, whatever machine defines them.
bool air_circulation_detect(const PrinterDiscovery& hw) {
    return hw.has_macro("AIR_CIRCULATION_INTERNAL") || hw.has_macro("AIR_CIRCULATION_EXTERNAL") ||
           hw.has_macro("AIR_CIRCULATION_STOP");
}

// --- Snapmaker U1 Top Cover --------------------------------------------------
//
// klippy/extras/purifier.py (firmware 2.x). The `purifier` object exists on every
// U1; the cover is its accessory, reported by power_detected. Modes: 0 idle,
// 1 cool chamber (exhaust fan holds the chamber at or below DESIRE_TEMP and
// alarms above ALARM_TEMP), 2 preheat and 3 hot chamber (the inner fan
// recirculates through the filter). Preheat is what slicer gcode sends before
// WAIT_CHAMBER_TEMP; manually, recirculating means hot chamber. With the cover
// off the firmware refuses every non-zero speed and mode, logging only.
// Older firmware reports fewer fields (1.3 has no `mode` and keeps the work time
// under inner_fan.work), so every reading keys on its own field being present.
constexpr double kAlarmMarginC = 1.0; // purifier.py DESIRED_CRITICAL_MIN_TEMP_DIFF

bool top_cover_detect(const PrinterDiscovery& hw) {
    return enclosure_top::firmware_reports_enclosure_top(hw);
}

std::string top_cover_mode_command(AirMode mode, const ModeTargets& t) {
    switch (mode) {
    case AirMode::Exhaust:
        return fmt::format("SET_PURIFIER_MODE MODE=1 FAN_SPEED={} DESIRE_TEMP={} ALARM_TEMP={}",
                           percent_as_fraction(t.exhaust_pct), std::lround(t.target_c),
                           std::lround(t.target_c + kAlarmMarginC));
    case AirMode::Recirculate:
        return "SET_PURIFIER_MODE MODE=3 FAN_SPEED=" + percent_as_fraction(t.recirculate_pct);
    case AirMode::Off:
        return "SET_PURIFIER_MODE MODE=0";
    }
    return "";
}

std::optional<AirMode> top_cover_mode(const json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    switch (value.get<int>()) {
    case 0:
        return AirMode::Off;
    case 1:
        return AirMode::Exhaust;
    case 2:
    case 3:
        return AirMode::Recirculate;
    default:
        return std::nullopt;
    }
}

Reading top_cover_read(const PrinterDiscovery& hw, const json& status) {
    Reading r;
    if (!status.is_object()) {
        return r;
    }
    r.present = enclosure_top::fitted_from_status(hw, status);
    auto obj = status.find("purifier");
    if (obj == status.end() || !obj->is_object()) {
        return r;
    }
    if (auto m = obj->find("mode"); m != obj->end()) {
        r.mode = top_cover_mode(*m);
    }
    r.exhaust_speed = nested_number(*obj, "exhaust_fan", "speed");
    r.recirculate_speed = nested_number(*obj, "inner_fan", "speed");
    r.exhaust_threshold = nested_number(*obj, "exhaust_fan", "speed_threshold");
    r.recirculate_threshold = nested_number(*obj, "inner_fan", "speed_threshold");
    r.critical_temp_c = number_at(*obj, "critical_chamber_temp");
    r.filter_seconds = number_at(*obj, "inner_fan_work_time");
    return r;
}

json top_cover_subscribe() {
    return json{{"purifier",
                 {"power_detected", "mode", "critical_chamber_temp", "inner_fan_work_time",
                  "exhaust_fan", "inner_fan"}}};
}

std::string top_cover_fan_command(FanRole role, int pct) {
    return fmt::format("SET_PURIFIER FAN={} SPEED={}",
                       role == FanRole::Exhaust ? "exhaust" : "inner", percent_as_fraction(pct));
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> kProviders = [] {
        std::vector<Provider> v;
        v.push_back(Provider{
            "Z-Mod AIR_CIRCULATION",
            air_circulation_detect,
            {"AIR_CIRCULATION_INTERNAL", "AIR_CIRCULATION_EXTERNAL", "AIR_CIRCULATION_STOP"}});
        Provider u1{"Snapmaker U1 top cover", top_cover_detect, {nullptr, nullptr, nullptr}};
        u1.mode_command = top_cover_mode_command;
        u1.read = top_cover_read;
        u1.subscribe = top_cover_subscribe;
        u1.live_presence = true;
        u1.mode_readback = true;
        u1.targets = true;
        u1.fans = {
            {FanRole::Exhaust, "chamber_air_exhaust", "fan_generic exhaust_fan"},
            {FanRole::Recirculate, "chamber_air_recirculate", "fan_generic circulation_fan"}};
        u1.fan_command = top_cover_fan_command;
        u1.filter_reset = "SET_PURIFIER FAN=inner WORK=0";
        u1.floor_pct = 10;
        v.push_back(std::move(u1));
        return v;
    }();
    return kProviders;
}

const Provider* match(const PrinterDiscovery& hw) {
    for (const auto& p : providers()) {
        if (p.detect(hw)) {
            return &p;
        }
    }
    return nullptr;
}

bool provider_offers(const PrinterDiscovery& hw, const Provider& p, AirMode mode) {
    if (p.mode_command) {
        return true;
    }
    const char* macro = p.macros[static_cast<size_t>(mode)];
    return macro && hw.has_macro(macro);
}

} // namespace

bool offers(const PrinterDiscovery& hw, AirMode mode) {
    const Provider* p = match(hw);
    return p && provider_offers(hw, *p, mode);
}

std::vector<AirMode> modes(const PrinterDiscovery& hw) {
    std::vector<AirMode> out;
    for (AirMode m : kAllModes) {
        if (offers(hw, m)) {
            out.push_back(m);
        }
    }
    return out;
}

bool supported(const PrinterDiscovery& hw) {
    return !modes(hw).empty();
}

std::string mode_gcode(const PrinterDiscovery& hw, AirMode mode, const ModeTargets& targets) {
    const Provider* p = match(hw);
    if (!p || !provider_offers(hw, *p, mode)) {
        return "";
    }
    if (p->mode_command) {
        return p->mode_command(mode, targets);
    }
    return p->macros[static_cast<size_t>(mode)];
}

bool presence_is_live(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->live_presence;
}

bool reports_mode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->mode_readback;
}

bool takes_targets(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->targets;
}

std::vector<OwnedFan> owned_fans(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->fans : std::vector<OwnedFan>{};
}

std::string fan_speed_gcode(const PrinterDiscovery& hw, FanRole role, int pct) {
    const Provider* p = match(hw);
    if (!p || !p->fan_command) {
        return "";
    }
    const bool owns = std::any_of(p->fans.begin(), p->fans.end(),
                                  [role](const OwnedFan& f) { return f.role == role; });
    return owns ? p->fan_command(role, pct) : "";
}

std::string filter_reset_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->filter_reset ? p->filter_reset : "";
}

int guard_floor_pct(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->floor_pct : 0;
}

nlohmann::json required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->subscribe ? p->subscribe() : json::object();
}

Reading read_status(const PrinterDiscovery& hw, const nlohmann::json& status) {
    const Provider* p = match(hw);
    return p && p->read ? p->read(hw, status) : Reading{};
}

std::optional<FanRole> driven_fan(AirMode mode) {
    switch (mode) {
    case AirMode::Exhaust:
        return FanRole::Exhaust;
    case AirMode::Recirculate:
        return FanRole::Recirculate;
    case AirMode::Off:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<FanRole> disabled_fan(AirMode mode) {
    switch (mode) {
    case AirMode::Exhaust:
        return FanRole::Recirculate;
    case AirMode::Recirculate:
        return FanRole::Exhaust;
    case AirMode::Off:
        return std::nullopt;
    }
    return std::nullopt;
}

FanGuard guard_fan_change(std::optional<AirMode> mode, FanRole role, int requested_pct,
                          int current_pct, std::optional<int> threshold_pct, int floor_pct) {
    if (!mode || *mode == AirMode::Off) {
        return {FanGuardAction::Apply, requested_pct};
    }
    if (disabled_fan(*mode) == role) {
        return requested_pct > 0 ? FanGuard{FanGuardAction::Blocked, current_pct}
                                 : FanGuard{FanGuardAction::Apply, 0};
    }
    int pct = requested_pct;
    if (driven_fan(*mode) == role) {
        pct = std::max(pct, floor_pct);
        if (threshold_pct && pct < *threshold_pct && pct < current_pct) {
            return {FanGuardAction::Confirm, pct};
        }
    }
    return {FanGuardAction::Apply, pct};
}

const char* mode_id(AirMode mode) {
    switch (mode) {
    case AirMode::Recirculate:
        return "recirculate";
    case AirMode::Exhaust:
        return "exhaust";
    case AirMode::Off:
        return "off";
    }
    return "";
}

std::optional<AirMode> parse_mode_id(std::string_view id) {
    for (AirMode m : kAllModes) {
        if (id == mode_id(m)) {
            return m;
        }
    }
    return std::nullopt;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : "";
}

} // namespace helix::chamber_air

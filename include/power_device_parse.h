// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "json_utils.h"
#include "moonraker_types.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace helix {

/// Parse Moonraker's `machine/device_power/devices` reply into PowerDevice records.
///
/// Split out of MoonrakerAPI::get_power_devices() so the shape rules have a
/// single implementation that both the API and the unit tests execute.
///
/// Moonraker returns `result.devices` as an ARRAY of objects; treating it as an
/// object yields array indices where device names belong (#466), so anything but
/// an array parses to nothing. Fields are read through json_util::safe_* because
/// nlohmann's .value() throws type_error.302 on a JSON null, and one null field
/// would otherwise drop the entire device list. Entries with no device name are
/// skipped — an unnamed device cannot be addressed by any later call.
inline std::vector<PowerDevice> parse_power_devices(const nlohmann::json& j) {
    std::vector<PowerDevice> devices;
    const nlohmann::json* device_list = json_util::find_path(j, "result", "devices");
    if (!device_list || !device_list->is_array()) {
        return devices;
    }

    for (const auto& info : *device_list) {
        if (!info.is_object()) {
            continue;
        }
        PowerDevice dev;
        dev.device = json_util::safe_string(info, "device");
        dev.type = json_util::safe_string(info, "type", "unknown");
        dev.status = json_util::safe_string(info, "status", "off");
        dev.locked_while_printing = json_util::safe_bool(info, "locked_while_printing", false);
        if (!dev.device.empty()) {
            devices.push_back(dev);
        }
    }
    return devices;
}

/// Whether @p service names a Klipper instance: `klipper`, or `klipper-1` on a
/// multi-instance host, with or without `.service`. `klipper_mcu` is a separate
/// process that keeps Klipper running when it stops.
inline bool is_klipper_service_name(std::string_view service) {
    constexpr std::string_view kUnitSuffix = ".service";
    if (service.size() > kUnitSuffix.size() &&
        service.substr(service.size() - kUnitSuffix.size()) == kUnitSuffix) {
        service.remove_suffix(kUnitSuffix.size());
    }
    constexpr std::string_view kKlipper = "klipper";
    if (service.substr(0, kKlipper.size()) != kKlipper) {
        return false;
    }
    return service.size() == kKlipper.size() || service[kKlipper.size()] == '-';
}

/// The power devices whose `bound_services` include Klipper, from a `server.config`
/// reply. Moonraker stops those services whenever the device turns off. Foreign
/// input: anything malformed reads as not bound.
inline std::vector<std::string> parse_klipper_bound_power_devices(const nlohmann::json& j) {
    std::vector<std::string> bound;
    const nlohmann::json* config = json_util::find_path(j, "result", "config");
    if (!config || !config->is_object()) {
        return bound;
    }

    constexpr std::string_view kSectionPrefix = "power ";
    for (const auto& [section, options] : config->items()) {
        if (section.size() <= kSectionPrefix.size() ||
            std::string_view(section).substr(0, kSectionPrefix.size()) != kSectionPrefix ||
            !options.is_object()) {
            continue;
        }
        const auto names_klipper = [](const nlohmann::json& service) {
            return service.is_string() &&
                   is_klipper_service_name(service.get_ref<const std::string&>());
        };
        // Moonraker still honours the deprecated single-valued `bound_service`.
        const nlohmann::json* bound_service = json_util::find_member(options, "bound_service");
        bool is_bound = bound_service && names_klipper(*bound_service);
        if (!is_bound) {
            if (const nlohmann::json* services = json_util::find_array(options, "bound_services")) {
                is_bound = std::any_of(services->begin(), services->end(), names_klipper);
            }
        }
        if (is_bound) {
            bound.push_back(section.substr(kSectionPrefix.size()));
        }
    }
    return bound;
}

} // namespace helix

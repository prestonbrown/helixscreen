// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_utils.h"
#include "moonraker_types.h"

#include <vector>

#include "hv/json.hpp"

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
    if (!j.contains("result") || !j["result"].contains("devices") ||
        !j["result"]["devices"].is_array()) {
        return devices;
    }

    for (const auto& info : j["result"]["devices"]) {
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

} // namespace helix

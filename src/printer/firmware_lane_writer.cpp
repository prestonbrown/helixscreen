// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_lane_writer.h"

#include "i_moonraker_api.h"
#include "json_utils.h"
#include "printer_discovery.h"
#include "spdlog/spdlog.h"
#include "version.h"

#include <algorithm>
#include <string>

#include "hv/json.hpp"

namespace helix::lane_writer {
namespace {

/// One firmware that writes lane_data itself from some version on.
struct Provider {
    const char* name;
    /// Status object carrying the version, listed in objects/list.
    const char* object;
    /// Key of that object holding the version, a "major.minor" string.
    const char* version_key;
    /// Version constraint (helix::version) from which the firmware writes the
    /// records and its edit command takes the whole identity.
    const char* writes_from;
    /// The edit command's parameter for each identity field.
    const char* spool_id_param;
    const char* vendor_param;
    const char* name_param;
    const char* filament_id_param;
    const char* bed_temp_param;
};

// Kiln OS (AD5X): kiln_lanes writes lane_data from IFS_SET_MATERIAL's per-slot
// fields, which plugin API 1.12 extends with the identity below
// (djvalic/kiln-os#101, docs/plugin-api.md). Older versions take type, color
// and temperature only, and HelixScreen stays the lane_data writer there.
constexpr Provider kProviders[] = {
    {"Kiln OS", "ad5x_plugins", "api_version", ">=1.12", "SPOOL_ID", "VENDOR", "NAME",
     "FILAMENT_ID", "BED_TEMP"},
};

const Provider* listed_provider(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    for (const Provider& p : kProviders) {
        if (std::find(objects.begin(), objects.end(), p.object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

/// A text field as one quoted parameter value, empty when nothing is left to send.
std::string text_value(const Provider& p, const char* param, const std::string& text) {
    const std::string sent = sendable_text(text);
    if (sent != text) {
        spdlog::warn("[LaneWriter] {} {}= sent as '{}': '{}' holds characters the command cannot "
                     "carry",
                     p.name, param, sent, text);
    }
    return sent.empty() ? std::string() : IMoonrakerAPI::quoted_gcode_param_value(sent);
}

} // namespace

std::string sendable_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (IMoonrakerAPI::is_safe_text_param(std::string(1, c))) {
            out += c;
        }
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    if (const Provider* p = listed_provider(hw)) {
        return {p->object};
    }
    return {};
}

bool firmware_may_write_lane_records(const PrinterDiscovery& hw) {
    return listed_provider(hw) != nullptr;
}

std::optional<bool> firmware_writes_lane_records(const nlohmann::json& status) {
    for (const Provider& p : kProviders) {
        const nlohmann::json* obj = json_util::find_object(status, p.object);
        if (obj == nullptr) {
            continue;
        }
        const auto version = json_util::read_field<std::string>(*obj, p.version_key);
        if (!version) {
            continue;
        }
        return version::check_version_constraint(p.writes_from, *version);
    }
    return std::nullopt;
}

std::string slot_identity_params(const SlotIdentity& identity) {
    // One row today; a second firmware with its own spelling picks its row the
    // way the status reading does.
    const Provider& p = kProviders[0];
    std::string out;
    const auto add = [&out](const char* param, const std::string& value) {
        out += ' ';
        out += param;
        out += '=';
        out += value;
    };
    add(p.spool_id_param, identity.spool_id > 0 ? std::to_string(identity.spool_id) : "");
    add(p.vendor_param, text_value(p, p.vendor_param, identity.vendor));
    add(p.name_param, text_value(p, p.name_param, identity.name));
    add(p.filament_id_param, text_value(p, p.filament_id_param, identity.filament_id));
    add(p.bed_temp_param, identity.bed_temp > 0 ? std::to_string(identity.bed_temp) : "");
    return out;
}

} // namespace helix::lane_writer

// SPDX-License-Identifier: GPL-3.0-or-later

#include "firmware_lane_writer.h"

#include "core/text_io.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "printer_discovery.h"
#include "spdlog/spdlog.h"
#include "version.h"

#include <algorithm>
#include <iterator>
#include <map>
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
    /// Characters the edit command refuses inside a quoted text value, beyond
    /// the ones Klipper's own parser cannot carry.
    const char* refused_text_chars;
    /// The edit command's parameter for each identity field.
    const char* spool_id_param;
    const char* vendor_param;
    const char* name_param;
    const char* filament_id_param;
    const char* bed_temp_param;
    const char* nozzle_temp_param;
    /// The nozzle temperatures the edit command takes; one outside them gets
    /// the whole command refused.
    int nozzle_temp_min;
    int nozzle_temp_max;
    /// Status object and its per-slot dict (keyed "1".."n") stating each
    /// slot's identity, null for an unset field.
    const char* identity_object;
    const char* identity_slots_key;
    /// That slot dict's key for each identity field.
    const char* spool_id_key;
    const char* vendor_key;
    const char* name_key;
    const char* filament_id_key;
    const char* bed_temp_key;
};

// Kiln OS (AD5X): kiln_lanes writes lane_data from IFS_SET_MATERIAL's per-slot
// fields, which plugin API 1.12 extends with the identity below
// (djvalic/kiln-os#101, docs/plugin-api.md), and states them in
// printer.ifs_materials. Older versions take type, color and temperature only,
// and HelixScreen stays the lane_data writer there. A value holding `'` or `%`
// gets the command refused; TEMP= takes 150-350, empty clearing it.
constexpr Provider kProviders[] = {
    {"Kiln OS", "ad5x_plugins", "api_version", ">=1.12", "'%",          "SPOOL_ID", "VENDOR",
     "NAME",    "FILAMENT_ID",  "BED_TEMP",    "TEMP",   150,           350,        "ifs_materials",
     "slots",   "spool_id",     "vendor",      "name",   "filament_id", "bed_temp"},
};

/// The row whose edit command a slot edit is sent through. One row today; a
/// second firmware with its own spelling picks its row the way the status
/// reading does.
const Provider& edit_provider() {
    return kProviders[0];
}

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
    const std::string refused = edit_provider().refused_text_chars;
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (IMoonrakerAPI::is_safe_text_param(std::string(1, c)) &&
            refused.find(c) == std::string::npos) {
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

std::map<int, SlotIdentity> slot_identities(const nlohmann::json& status) {
    std::map<int, SlotIdentity> out;
    const Provider& p = edit_provider();
    const nlohmann::json* slots =
        json_util::find_path(status, p.identity_object, p.identity_slots_key);
    if (slots == nullptr || !slots->is_object()) {
        return out;
    }
    for (auto it = slots->begin(); it != slots->end(); ++it) {
        const auto number = text_io::parse_leading<int>(it.key());
        const nlohmann::json& slot = it.value();
        if (!number || *number < 1 || !slot.is_object()) {
            continue;
        }
        // A slot stating none of the fields is a firmware that does not keep them.
        const char* const keys[] = {p.spool_id_key, p.vendor_key, p.name_key, p.filament_id_key,
                                    p.bed_temp_key};
        if (std::none_of(std::begin(keys), std::end(keys),
                         [&slot](const char* key) { return slot.contains(key); })) {
            continue;
        }
        SlotIdentity id;
        id.spool_id = std::max(json_util::safe_int(slot, p.spool_id_key), 0);
        id.vendor = json_util::safe_string(slot, p.vendor_key);
        id.name = json_util::safe_string(slot, p.name_key);
        id.filament_id = json_util::safe_string(slot, p.filament_id_key);
        id.bed_temp = std::max(json_util::safe_int(slot, p.bed_temp_key), 0);
        out.emplace(*number - 1, std::move(id));
    }
    return out;
}

std::string slot_identity_params(const SlotIdentity& identity) {
    const Provider& p = edit_provider();
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
    std::string nozzle;
    if (identity.nozzle_temp >= p.nozzle_temp_min && identity.nozzle_temp <= p.nozzle_temp_max) {
        nozzle = std::to_string(identity.nozzle_temp);
    } else if (identity.nozzle_temp > 0) {
        spdlog::warn("[LaneWriter] {} {}= sent empty: {} is outside the {}-{} it takes", p.name,
                     p.nozzle_temp_param, identity.nozzle_temp, p.nozzle_temp_min,
                     p.nozzle_temp_max);
    }
    add(p.nozzle_temp_param, nozzle);
    return out;
}

} // namespace helix::lane_writer

// SPDX-License-Identifier: GPL-3.0-or-later

#include "pre_print_preferences.h"

#include "printer_discovery.h"

#include <algorithm>

#include "hv/json.hpp"

namespace helix::preprint_prefs {
namespace {

/// One firmware that stores pre-print option settings itself.
struct Provider {
    const char* name;
    /// Printer object whose presence identifies the firmware.
    const char* detect_object;
    /// Object carrying the stored settings.
    const char* status_object;
    /// Pre-print option id -> the boolean field holding its setting. An option
    /// the firmware does not persist simply has no row here.
    std::vector<std::pair<const char*, const char*>> fields;
};

const std::vector<Provider>& providers() {
    static const std::vector<Provider> kProviders = {
        {"Snapmaker U1",
         "print_task_config",
         "print_task_config",
         {
             {"bed_mesh", "auto_bed_leveling"},
             {"flow_calibrate", "flow_calibrate"},
             {"u1_timelapse", "time_lapse_camera"},
         }},
    };
    return kProviders;
}

/// The provider for this printer, or nullptr.
///
/// An objects list we have not received yet cannot refute anything, so this
/// answers nullptr until discovery reports one rather than concluding the
/// firmware stores nothing.
const Provider* provider_for(const PrinterDiscovery& hw) {
    if (!hw.objects_reported()) {
        return nullptr;
    }
    const auto& objects = hw.printer_objects();
    for (const auto& p : providers()) {
        if (std::find(objects.begin(), objects.end(), p.detect_object) != objects.end()) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = provider_for(hw);
    return p ? std::vector<std::string>{p->status_object} : std::vector<std::string>{};
}

bool firmware_persists_options(const PrinterDiscovery& hw) {
    return provider_for(hw) != nullptr;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = provider_for(hw);
    return p ? p->name : std::string{};
}

std::map<std::string, bool> read_persisted_defaults(const PrinterDiscovery& hw,
                                                    const nlohmann::json& status) {
    std::map<std::string, bool> out;
    const Provider* p = provider_for(hw);
    if (!p || !status.is_object()) {
        return out;
    }
    auto obj = status.find(p->status_object);
    if (obj == status.end() || !obj->is_object()) {
        return out;
    }
    for (const auto& [option_id, field] : p->fields) {
        auto it = obj->find(field);
        if (it == obj->end()) {
            continue; // silence, not an off
        }
        // A firmware storing the flag as a Python bool serialises true/false;
        // one storing it as an int serialises 0/1. Anything else is not a
        // setting we can read, and is left out rather than guessed at.
        if (it->is_boolean()) {
            out[option_id] = it->get<bool>();
        } else if (it->is_number_integer()) {
            out[option_id] = it->get<int>() != 0;
        }
    }
    return out;
}

} // namespace helix::preprint_prefs

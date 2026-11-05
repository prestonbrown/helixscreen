// SPDX-License-Identifier: GPL-3.0-or-later

#include "enclosure_top.h"

#include "printer_discovery.h"

#include <algorithm>
#include <vector>

#include "hv/json.hpp"

namespace helix::enclosure_top {
namespace {

/// One firmware that reports a removable enclosure top.
struct Provider {
    const char* name;
    /// Status object carrying the reading.
    const char* status_object;
    /// Boolean field: true while the top is fitted.
    const char* fitted_field;
    /// Objects only this firmware publishes; at least one must be present
    /// beside status_object, whose name alone is too generic to identify it.
    std::vector<const char*> markers;
};

// The Snapmaker U1's top module carries an air purifier that the main board
// powers through the top's connector, so the board sensing that power is the
// top being on the printer.
const std::vector<Provider>& providers() {
    static const std::vector<Provider> kProviders = {
        {"Snapmaker U1",
         "purifier",
         "power_detected",
         {"fm175xx_reader", "machine_state_manager", "defect_detection",
          "filament_entangle_detect"}},
    };
    return kProviders;
}

const Provider* provider_for(const PrinterDiscovery& hw) {
    const auto& objects = hw.printer_objects();
    auto has = [&objects](const char* name) {
        return std::find(objects.begin(), objects.end(), name) != objects.end();
    };
    for (const auto& p : providers()) {
        if (has(p.status_object) && std::any_of(p.markers.begin(), p.markers.end(), has)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool firmware_reports_enclosure_top(const PrinterDiscovery& hw) {
    return provider_for(hw) != nullptr;
}

nlohmann::json required_status_objects(const PrinterDiscovery& hw) {
    nlohmann::json objects = nlohmann::json::object();
    if (const Provider* p = provider_for(hw)) {
        // Narrowed to the one field: the same object reports fan speeds that
        // would otherwise notify on every tick.
        objects[p->status_object] = nlohmann::json::array({p->fitted_field});
    }
    return objects;
}

std::optional<bool> fitted_from_status(const PrinterDiscovery& hw, const nlohmann::json& status) {
    const Provider* p = provider_for(hw);
    if (!p || !status.is_object()) {
        return std::nullopt;
    }
    auto obj = status.find(p->status_object);
    if (obj == status.end() || !obj->is_object()) {
        return std::nullopt;
    }
    auto field = obj->find(p->fitted_field);
    if (field == obj->end() || !field->is_boolean()) {
        return std::nullopt;
    }
    return field->get<bool>();
}

} // namespace helix::enclosure_top

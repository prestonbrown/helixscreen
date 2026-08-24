// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_persistence.h"

#include "printer_discovery.h"

#include <cmath>

namespace helix::zoffset {
namespace {

/// One firmware that stores the z-offset outside gcode_move.
struct Provider {
    const char* name;
    /// Macro whose presence identifies the firmware.
    const char* detect_macro;
    /// Status objects that carry the stored value.
    std::vector<std::string> status_objects;
    /// One-shot command that turns persistence on, or nullptr.
    const char* enable_gcode;
    /// Pull the stored offset, in microns, out of a status frame.
    std::optional<int> (*read)(const nlohmann::json& status);
};

/// Fetch status.<object>.<member> as an object, or nullptr.
const nlohmann::json* nested_object(const nlohmann::json& status, const char* object,
                                    const char* member) {
    if (!status.is_object()) {
        return nullptr;
    }
    auto outer = status.find(object);
    if (outer == status.end() || !outer->is_object()) {
        return nullptr;
    }
    auto inner = outer->find(member);
    if (inner == outer->end() || !inner->is_object()) {
        return nullptr;
    }
    return &(*inner);
}

// --- ZMOD (AD5M / AD5X and friends) -----------------------------------------
//
// ZMOD's SET_GCODE_OFFSET override writes every offset the user dials in to the
// `gcode_offsets` save-variable, its END_PRINT/CANCEL_PRINT zero the live
// gcode_move offset, and START_PRINT re-applies the stored one via
// LOAD_GCODE_OFFSET. Reloading is off by default, hence the enable command.
std::optional<int> read_zmod(const nlohmann::json& status) {
    const nlohmann::json* variables = nested_object(status, "save_variables", "variables");
    if (!variables) {
        return std::nullopt;
    }
    // ZMOD stores a per-axis dict; LOAD_GCODE_OFFSET iterates it, so sibling
    // axes are legal and only z concerns us.
    auto offsets = variables->find("gcode_offsets");
    if (offsets == variables->end() || !offsets->is_object()) {
        return std::nullopt;
    }
    auto z = offsets->find("z");
    if (z == offsets->end() || !z->is_number()) {
        // Seeded as {'z': None} before the first SET_GCODE_OFFSET.
        return std::nullopt;
    }
    // Round rather than truncate: the stored value is an accumulation of
    // relative Z_ADJUST deltas, so a nominal -0.150 arrives as -0.1499999.
    return static_cast<int>(std::lround(z->get<double>() * 1000.0));
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> table = {
        {"ZMOD", "SAVE_ZMOD_DATA", {"save_variables"}, "SAVE_ZMOD_DATA LOAD_ZOFFSET=1", &read_zmod},
    };
    return table;
}

const Provider* match(const PrinterDiscovery& hw) {
    for (const auto& p : providers()) {
        if (hw.has_macro(p.detect_macro)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    if (const Provider* p = match(hw)) {
        return p->status_objects;
    }
    return {};
}

std::optional<int> read_persisted_offset_microns(const nlohmann::json& status) {
    // Read by schema rather than by detected firmware: this runs on the status
    // path, which has no PrinterDiscovery to hand, and the schemas are distinct
    // enough to identify themselves. A printer without the firmware never
    // carries the keys, so it simply never matches.
    for (const auto& p : providers()) {
        if (auto microns = p.read(status)) {
            return microns;
        }
    }
    return std::nullopt;
}

std::string persistence_enable_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (p && p->enable_gcode) {
        return p->enable_gcode;
    }
    return {};
}

bool firmware_persists_z_offset(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string persistence_provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

} // namespace helix::zoffset

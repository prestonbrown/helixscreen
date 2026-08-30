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
    /// Command clearing a stale probe delta the firmware subtracts before
    /// persisting an adjustment, or nullptr when it has no such variable.
    const char* stale_delta_clear_gcode;
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
//
// The write is `z - _TEST_POINT.temp_z_offset`: mid-print that subtraction is
// the point, excluding the per-print probe delta START_PRINT stashed in the
// variable. Through ZMOD 1.7.2 the variable is zeroed only at
// BED_MESH_CALIBRATE, _MESH_TEST, SAVE_ZMOD_DATA and boot - not at
// END_PRINT/CANCEL_PRINT - so an adjustment sent while idle AFTER a print
// stores the intended value minus the last print's delta (ghzserg/zmod#699).
// ghzserg/z_ad5x@6a0adf3 zeroes it in _COMMON_END_PRINT, which both END_PRINT
// and CANCEL_PRINT call, and no release carries that yet. Callers clear the
// variable right before an idle adjustment: load-bearing below the fix, a
// write of zero over zero above it. The command lives here because only ZMOD
// has the variable.
//
// That same commit also zeroes the live gcode_move offset at print end in the
// alt-screen config, so post-print the live offset reads 0 while the stored
// value stands. Reading the stored one is exactly what this provider is for.
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

// --- Forge-X (FlashForge Adventurer 5M / Pro mod) ----------------------------
//
// The mod's klippy plugin keeps its parameters - z-offset among them - in its
// own INI store, not klipper's save_variables, and mirrors them into the
// `mod_params` status object. Its SET_GCODE_OFFSET override persists every
// offset the user dials in; print start re-applies the stored one only when
// the load_zoffset param is on, hence the enable command.
std::optional<int> read_forge_x(const nlohmann::json& status) {
    const nlohmann::json* variables = nested_object(status, "mod_params", "variables");
    if (!variables) {
        return std::nullopt;
    }
    auto z = variables->find("z_offset");
    if (z == variables->end() || !z->is_number()) {
        // Not written until the firmware's first SET_GCODE_OFFSET Z=.
        return std::nullopt;
    }
    // Same accumulate-and-round treatment as ZMOD: the stored value is the sum
    // of relative Z_ADJUST deltas, so a nominal -0.150 arrives as -0.1499999.
    return static_cast<int>(std::lround(z->get<double>() * 1000.0));
}

// --- Helper-Script save-zoffset (Creality K1/K1C/K1 Max and friends) --------
//
// Guilouz's Helper-Script ships save-zoffset.cfg: it wraps SET_GCODE_OFFSET
// (rename_existing: _SET_GCODE_OFFSET), mirrors every offset into the
// `zoffset` save-variable, and a boot-time delayed_gcode re-applies that value
// 2s after every Klipper start. The module IS a z-offset persistence provider:
// the offset survives restarts without any probe fold.
//
// That is exactly why our "Save Z Offset" is dangerous on these boxes
// (prestonbrown/helixscreen#1401): Z_OFFSET_APPLY_PROBE folds the gcode offset
// into the probe's z_offset, SAVE_CONFIG restarts Klipper, and the boot gcode
// re-applies the SAME offset on top - the probe value grows by the full offset
// on every save cycle (observed 0.060 -> 2.515mm over five cycles, ending in
// nozzle-on-bed). Detection flips the save path to firmware-managed, the same
// treatment ZMOD and Forge-X already get.
std::optional<int> read_helper_script(const nlohmann::json& status) {
    const nlohmann::json* variables = nested_object(status, "save_variables", "variables");
    if (!variables) {
        return std::nullopt;
    }
    auto zoffset = variables->find("zoffset");
    if (zoffset == variables->end() || !zoffset->is_object()) {
        return std::nullopt;
    }
    auto z = zoffset->find("z");
    if (z == zoffset->end() || !z->is_number()) {
        // Seeded as {'z': None} before the first wrapped SET_GCODE_OFFSET.
        return std::nullopt;
    }
    return static_cast<int>(std::lround(z->get<double>() * 1000.0));
}

const std::vector<Provider>& providers() {
    // Row order is match priority: a box exposing two firmwares' macros
    // resolves to the first hit.
    static const std::vector<Provider> table = {
        {"ZMOD",
         "SAVE_ZMOD_DATA",
         {"save_variables"},
         "SAVE_ZMOD_DATA LOAD_ZOFFSET=1",
         "SET_GCODE_VARIABLE MACRO=_TEST_POINT VARIABLE=temp_z_offset VALUE=0",
         &read_zmod},
        {"Forge-X",
         "SET_MOD",
         {"mod_params"},
         "SET_MOD PARAM=\"load_zoffset\" VALUE=1",
         nullptr,
         &read_forge_x},
        // Keyed on the WRAPPER object. Klipper exposes a builtin command as a
        // printer object ONLY when a [gcode_macro] shadows it (and shadowing a
        // builtin requires rename_existing), so `gcode_macro
        // SET_GCODE_OFFSET` in objects/list is exactly "a renaming wrapper
        // exists" - stock Klipper never lists it. The renamed original the
        // wrapper creates is a bare command, not an object; verified against
        // debug bundle 5J49T5RU: 83 macro objects including
        // SET_GCODE_OFFSET, none named _SET_GCODE_OFFSET. Must stay below
        // ZMOD, which also wraps the command.
        {"Helper-Script",
         "SET_GCODE_OFFSET",
         {"save_variables"},
         nullptr,
         nullptr,
         &read_helper_script},
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

std::string stale_probe_delta_clear_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (p && p->stale_delta_clear_gcode) {
        return p->stale_delta_clear_gcode;
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

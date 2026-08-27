// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offsets.h"

#include "printer_discovery.h"

#include <cmath>
#include <cstdio>

namespace helix::tool_offsets {
namespace {

/// One firmware's per-tool z-offset model.
struct Provider {
    const char* name;
    /// Whether this printer uses this model.
    bool (*detect)(const PrinterDiscovery& hw);
    /// Status objects beyond what the tool-changer subscription already asks
    /// for.
    std::vector<std::string> status_objects;
    /// Pull one tool's offset, in microns, out of a status frame.
    std::optional<int> (*read)(const nlohmann::json& status, int tool_index,
                               const std::string& tool_name);
    /// Runtime write - takes effect now, not persisted.
    std::string (*set_gcode)(int tool_index, int microns);
    /// Durable write. Self-sufficient and idempotent after set_gcode().
    std::string (*save_gcode)(int tool_index, int microns);
    /// Whether save_gcode() only stages the change, awaiting SAVE_CONFIG.
    bool persist_needs_save_config;
};

/// Render a double as a bare decimal literal - the form every firmware here
/// parses as a number.
std::string mm_literal(int microns) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(microns) / 1000.0);
    return buf;
}

/// Round rather than truncate: these values round-trip through a float, so a
/// nominal -0.150 arrives as -0.1499999 and truncation would drift by a micron
/// every time the UI echoed the value back.
std::optional<int> to_microns(const nlohmann::json& value) {
    if (!value.is_number()) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(value.get<double>() * 1000.0));
}

/// Fetch status.<object> as an object, or nullptr.
const nlohmann::json* status_object(const nlohmann::json& status, const std::string& object) {
    if (!status.is_object()) {
        return nullptr;
    }
    auto it = status.find(object);
    if (it == status.end() || !it->is_object()) {
        return nullptr;
    }
    return &(*it);
}

// --- MedusaHC-style: a TOOL_OFFSET macro holding every tool's offset ---------
//
// Irbis3D/MedusaHC keeps the runtime offsets as variables on one macro,
// `[gcode_macro TOOL_OFFSET]` with `variable_t<N>_off_z`, and the durable copy
// in save_variables under `t<N>_gcode_z_offset`, loaded into the macro at
// startup. Its own calibration macro writes both:
//
//     SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE={kx} VALUE={x}
//     SAVE_VARIABLE VARIABLE={sx} VALUE={x}
//
// A MedusaHC is ALSO a klipper-toolchanger printer - it ships [toolchanger] and
// [tool T0..T3] - so it matches both rows and must come first. Writing
// klipper-toolchanger's own offset on one of these would set a value its macros
// never read (docs/devel/FILAMENT_BACKEND_MEDUSAHC.md: "klipper-toolchanger's
// own offset model is not the authority").
//
// Detection is the macro itself rather than the machine, so any config using
// this store is covered and the row does not go stale when the hardware is
// rebranded.
bool detect_tool_offset_macro(const PrinterDiscovery& hw) {
    return hw.has_macro("TOOL_OFFSET");
}

std::optional<int> read_tool_offset_macro(const nlohmann::json& status, int tool_index,
                                          const std::string& /*tool_name*/) {
    const nlohmann::json* macro = status_object(status, "gcode_macro TOOL_OFFSET");
    if (!macro || tool_index < 0) {
        return std::nullopt;
    }
    auto it = macro->find("t" + std::to_string(tool_index) + "_off_z");
    if (it == macro->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::string set_tool_offset_macro(int tool_index, int microns) {
    return "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t" + std::to_string(tool_index) +
           "_off_z VALUE=" + mm_literal(microns);
}

std::string save_tool_offset_macro(int tool_index, int microns) {
    // Both stores, so the save stands alone: SAVE_VARIABLE is the durable copy,
    // the macro variable is what the machine actually prints with. SAVE_VARIABLE
    // lands immediately - no SAVE_CONFIG, no restart.
    return "SAVE_VARIABLE VARIABLE=t" + std::to_string(tool_index) +
           "_gcode_z_offset VALUE=" + mm_literal(microns) + "\n" +
           set_tool_offset_macro(tool_index, microns);
}

// --- viesturz/klipper-toolchanger -------------------------------------------
//
// Each `tool T<n>` object publishes gcode_x/y/z_offset, and ToolGcodeTransform
// adds them to every move:
//
//     transformed_pos = [newpos[0] + self.tool.gcode_x_offset,
//                        newpos[1] + self.tool.gcode_y_offset,
//                        newpos[2] + self.tool.gcode_z_offset] + newpos[3:]
//
// Because the transform reads the attribute per move, SET_TOOL_PARAMETER takes
// effect on the next move - no re-select needed and no gcode_move involvement.
// SAVE_TOOL_PARAMETER goes through configfile.set(), i.e. a pending config
// change awaiting SAVE_CONFIG.
//
// Only the offset model from the 2025.12 gcode-transform rework is supported.
// Older builds folded tool offsets into the user's gcode_move offset, where a
// baby-step and a tool change fight each other; we do not detect or paper over
// that.
bool detect_toolchanger(const PrinterDiscovery& hw) {
    return hw.has_tool_changer();
}

std::optional<int> read_toolchanger(const nlohmann::json& status, int /*tool_index*/,
                                    const std::string& tool_name) {
    if (tool_name.empty()) {
        return std::nullopt;
    }
    const nlohmann::json* tool = status_object(status, "tool " + tool_name);
    if (!tool) {
        return std::nullopt;
    }
    auto it = tool->find("gcode_z_offset");
    if (it == tool->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::string set_toolchanger(int tool_index, int microns) {
    return "SET_TOOL_PARAMETER T=" + std::to_string(tool_index) +
           " PARAMETER=gcode_z_offset VALUE=" + mm_literal(microns);
}

std::string save_toolchanger(int tool_index, int microns) {
    // SAVE_TOOL_PARAMETER persists whatever the tool currently holds, so set the
    // value first and the pair is both self-sufficient and idempotent after a
    // set_tool_z_gcode() with the same value.
    return set_toolchanger(tool_index, microns) + "\nSAVE_TOOL_PARAMETER T=" +
           std::to_string(tool_index) + " PARAMETER=gcode_z_offset";
}

const std::vector<Provider>& providers() {
    // Row order is match priority, as in z_offset_persistence.cpp. MedusaHC
    // FIRST: it is a klipper-toolchanger printer too, so it matches both rows,
    // and the second one would write a store its macros never read.
    static const std::vector<Provider> table = {
        {"TOOL_OFFSET macro", &detect_tool_offset_macro, {"gcode_macro TOOL_OFFSET"},
         &read_tool_offset_macro, &set_tool_offset_macro, &save_tool_offset_macro, false},
        {"klipper-toolchanger", &detect_toolchanger, {}, &read_toolchanger, &set_toolchanger,
         &save_toolchanger, true},
    };
    return table;
}

const Provider* match(const PrinterDiscovery& hw) {
    for (const auto& p : providers()) {
        if (p.detect(hw)) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

bool supports_per_tool_z(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    if (const Provider* p = match(hw)) {
        return p->status_objects;
    }
    return {};
}

std::optional<int> read_tool_z_microns(const nlohmann::json& status, int tool_index,
                                       const std::string& tool_name) {
    // Read by schema rather than by detected firmware, for the same reason
    // helix::zoffset does: this runs on the status path, which has no
    // PrinterDiscovery to hand. Table order still decides, so a machine
    // carrying both schemas resolves to the authoritative one.
    for (const auto& p : providers()) {
        if (auto microns = p.read(status, tool_index, tool_name)) {
            return microns;
        }
    }
    return std::nullopt;
}

std::string set_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns) {
    const Provider* p = match(hw);
    if (!p || tool_index < 0) {
        return {};
    }
    return p->set_gcode(tool_index, microns);
}

std::string save_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns) {
    const Provider* p = match(hw);
    if (!p || tool_index < 0) {
        return {};
    }
    return p->save_gcode(tool_index, microns);
}

bool persist_requires_save_config(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->persist_needs_save_config;
}

std::string provider_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string{};
}

} // namespace helix::tool_offsets

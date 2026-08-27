// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offsets.h"

#include "printer_discovery.h"

#include <algorithm>
#include <cctype>
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
    /// for. Takes the printer because an object's name can depend on how the
    /// config spells it.
    std::vector<std::string> (*status_objects)(const PrinterDiscovery& hw);
    /// Pull one tool's offset, in microns, out of a status frame.
    std::optional<int> (*read)(const nlohmann::json& status, int tool_index,
                               const std::string& tool_name);
    /// Runtime write - takes effect now, not persisted.
    std::string (*set_gcode)(const PrinterDiscovery& hw, int tool_index, int microns);
    /// Durable write. Self-sufficient and idempotent after set_gcode().
    std::string (*save_gcode)(const PrinterDiscovery& hw, int tool_index, int microns);
    /// Whether save_gcode() only stages the change, awaiting SAVE_CONFIG.
    bool persist_needs_save_config;
    /// Whether this provider's store appears in the frame at all, regardless of
    /// the tool asked for. Guards the by-schema fallthrough above.
    bool (*store_present)(const nlohmann::json& status);
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

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::toupper(x) == std::toupper(y);
           });
}

/// Fetch the status object of a gcode_macro, whatever case printer.cfg spells
/// it in.
///
/// Klipper keys the object on the config section VERBATIM, so
/// `[gcode_macro Tool_Offset]` publishes "gcode_macro Tool_Offset" while the
/// command it registers is the uppercased alias TOOL_OFFSET. Elsewhere we ask
/// PrinterDiscovery::macro_config_name() for the real spelling, but the read
/// path deliberately has no PrinterDiscovery (see read_tool_z_microns), so it
/// falls back to a scan.
///
/// The scan is unambiguous: two sections differing only in case would register
/// the same command alias and Klipper refuses to start.
const nlohmann::json* status_macro(const nlohmann::json& status, const std::string& macro) {
    const std::string exact = "gcode_macro " + macro;
    if (const nlohmann::json* hit = status_object(status, exact)) {
        return hit;
    }
    if (!status.is_object()) {
        return nullptr;
    }
    for (auto it = status.begin(); it != status.end(); ++it) {
        if (it->is_object() && iequals(it.key(), exact)) {
            return &(*it);
        }
    }
    return nullptr;
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
    // The macro AND more than one toolhead. `TOOL_OFFSET` is a plausible name
    // for a hand-written macro on a single-extruder machine, and matching on it
    // alone put the tool selector on such a printer and pointed its buttons at
    // `t0_off_z` in a macro that has no such variable and a [save_variables]
    // section that need not exist. supports_per_tool_z() promises false on
    // every single-toolhead printer; this is what keeps that promise.
    return hw.has_macro("TOOL_OFFSET") && hw.tool_names().size() > 1;
}

std::vector<std::string> status_objects_tool_offset_macro(const PrinterDiscovery& hw) {
    // The name as printer.cfg spells it, not the uppercased one we matched on:
    // Klipper never resolves a subscription request case-insensitively, and an
    // object it cannot look up is silently absent from every frame rather than
    // an error. Asking for "gcode_macro TOOL_OFFSET" on a machine configured
    // `[gcode_macro Tool_Offset]` therefore reads empty forever.
    const std::string name = hw.macro_config_name("TOOL_OFFSET");
    if (name.empty()) {
        return {};
    }
    return {"gcode_macro " + name};
}

std::optional<int> read_tool_offset_macro(const nlohmann::json& status, int tool_index,
                                          const std::string& /*tool_name*/) {
    const nlohmann::json* macro = status_macro(status, "TOOL_OFFSET");
    if (!macro || tool_index < 0) {
        return std::nullopt;
    }
    auto it = macro->find("t" + std::to_string(tool_index) + "_off_z");
    if (it == macro->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::string set_tool_offset_macro(const PrinterDiscovery& hw, int tool_index, int microns) {
    // MACRO= is a mux key registered on the CONFIG-case name, not the alias
    // (klippy/extras/gcode_macro.py registers `name`, not `self.alias`), so a
    // capitalised MACRO= is rejected outright on a `[gcode_macro Tool_Offset]`
    // machine - the command errors rather than silently missing.
    const std::string name = hw.macro_config_name("TOOL_OFFSET");
    if (name.empty()) {
        return {};
    }
    return "SET_GCODE_VARIABLE MACRO=" + name + " VARIABLE=t" + std::to_string(tool_index) +
           "_off_z VALUE=" + mm_literal(microns);
}

std::string save_tool_offset_macro(const PrinterDiscovery& hw, int tool_index, int microns) {
    // Both stores, so the save stands alone: SAVE_VARIABLE is the durable copy,
    // the macro variable is what the machine actually prints with. SAVE_VARIABLE
    // lands immediately - no SAVE_CONFIG, no restart.
    const std::string runtime = set_tool_offset_macro(hw, tool_index, microns);
    if (runtime.empty()) {
        // No macro to write the runtime half into. Persisting alone would leave
        // the durable store and the value the machine prints with disagreeing
        // until the next restart, so write neither.
        return {};
    }
    return "SAVE_VARIABLE VARIABLE=t" + std::to_string(tool_index) +
           "_gcode_z_offset VALUE=" + mm_literal(microns) + "\n" + runtime;
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
bool tool_offset_macro_present(const nlohmann::json& status) {
    return status_macro(status, "TOOL_OFFSET") != nullptr;
}

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

std::vector<std::string> status_objects_none(const PrinterDiscovery& /*hw*/) {
    return {};
}

std::string set_toolchanger(const PrinterDiscovery& /*hw*/, int tool_index, int microns) {
    return "SET_TOOL_PARAMETER T=" + std::to_string(tool_index) +
           " PARAMETER=gcode_z_offset VALUE=" + mm_literal(microns);
}

std::string save_toolchanger(const PrinterDiscovery& hw, int tool_index, int microns) {
    // SAVE_TOOL_PARAMETER persists whatever the tool currently holds, so set the
    // value first and the pair is both self-sufficient and idempotent after a
    // set_tool_z_gcode() with the same value.
    return set_toolchanger(hw, tool_index, microns) +
           "\nSAVE_TOOL_PARAMETER T=" + std::to_string(tool_index) + " PARAMETER=gcode_z_offset";
}

const std::vector<Provider>& providers() {
    // Row order is match priority, as in z_offset_persistence.cpp. MedusaHC
    // FIRST: it is a klipper-toolchanger printer too, so it matches both rows,
    // and the second one would write a store its macros never read.
    static const std::vector<Provider> table = {
        {"TOOL_OFFSET macro", &detect_tool_offset_macro, &status_objects_tool_offset_macro,
         &read_tool_offset_macro, &set_tool_offset_macro, &save_tool_offset_macro, false,
         &tool_offset_macro_present},
        {"klipper-toolchanger", &detect_toolchanger, &status_objects_none, &read_toolchanger,
         &set_toolchanger, &save_toolchanger, true, nullptr},
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
        return p->status_objects(hw);
    }
    return {};
}

std::optional<int> read_tool_z_microns(const nlohmann::json& status, int tool_index,
                                       const std::string& tool_name) {
    // Read by schema rather than by detected firmware, because this runs on the
    // status path, which has no PrinterDiscovery to hand.
    //
    // Table order alone is NOT enough here. It resolves a frame carrying BOTH
    // schemas — the query seed does — but Moonraker's delta frames carry only
    // what changed, so a MedusaHC frame with just `tool T0` would fall past the
    // authoritative TOOL_OFFSET row and read klipper-toolchanger's copy, the
    // store this module's own table says is not the authority. That value would
    // then overwrite the real one and become the base for the next adjustment.
    //
    // So a lower-priority row is only allowed to answer when no higher-priority
    // row's store is present in the frame AT ALL — absent is "no news", not
    // "ask someone else".
    for (const auto& p : providers()) {
        if (auto microns = p.read(status, tool_index, tool_name)) {
            return microns;
        }
        if (p.store_present && p.store_present(status)) {
            // This provider owns the frame and simply has nothing for this
            // tool. Falling through would answer from the wrong store.
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string set_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns) {
    const Provider* p = match(hw);
    if (!p || tool_index < 0) {
        return {};
    }
    return p->set_gcode(hw, tool_index, microns);
}

std::string save_tool_z_gcode(const PrinterDiscovery& hw, int tool_index, int microns) {
    const Provider* p = match(hw);
    if (!p || tool_index < 0) {
        return {};
    }
    return p->save_gcode(hw, tool_index, microns);
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

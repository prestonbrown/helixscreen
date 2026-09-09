// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offsets.h"

#include "printer_discovery.h"

#include <cmath>
#include <cstdio>

namespace helix::tool_offsets {

/// One firmware's per-tool offset model. The header declares it only as an
/// opaque type: an OffsetReader points at one row of the table below.
struct Provider {
    const char* name;
    /// Whether this printer uses this model.
    bool (*detect)(const PrinterDiscovery& hw);
    /// Which axes this model keeps. A false here makes read/set/save answer
    /// nothing for that axis, so a caller can loop kAllAxes blindly.
    bool (*supports_axis)(Axis axis);
    /// Status objects beyond what the tool-changer subscription already asks
    /// for. Takes the printer because an object's name can depend on how the
    /// config spells it.
    std::vector<std::string> (*status_objects)(const PrinterDiscovery& hw);
    /// Pull one axis of one tool's offset, in microns, out of a status frame.
    /// @p store_object is OffsetReader::store_object - the exact status key of
    /// a single-object store, resolved from the config-case section name once
    /// at discovery, so no read has to scan the frame for it.
    std::optional<int> (*read)(const nlohmann::json& status, const std::string& store_object,
                               Axis axis, int tool_index, const std::string& tool_name);
    /// Runtime write - takes effect now, not persisted.
    std::string (*set_gcode)(const PrinterDiscovery& hw, Axis axis, int tool_index, int microns);
    /// Durable write. Self-sufficient and idempotent after set_gcode().
    std::string (*save_gcode)(const PrinterDiscovery& hw, Axis axis, int tool_index, int microns);
    /// Whether save_gcode() only stages the change, awaiting SAVE_CONFIG.
    bool persist_needs_save_config;
};

namespace {

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
    // The macro AND more than one toolhead. `TOOL_OFFSET` is a plausible name
    // for a hand-written macro on a single-extruder machine, and matching on it
    // alone put the tool selector on such a printer and pointed its buttons at
    // `t0_off_z` in a macro that has no such variable and a [save_variables]
    // section that need not exist. supports_per_tool_offsets() promises false on
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

// Z only - NOT because the machine cannot do X/Y. Only the Z variable names
// (t<n>_off_z / t<n>_gcode_z_offset) are confirmed against a machine; MedusaHC's
// docs say the durable store holds t<n>_gcode_{x,y,z}_offset, so an X/Y pair
// very likely exists in the same shape. A guessed runtime name would write a
// value nothing reads, so the axes stay declined until someone checks them.
// Extending this to X/Y is a matter of confirming those names and flipping this
// predicate; nothing else in the row is Z-specific by design.
bool supports_axis_tool_offset_macro(Axis axis) {
    return axis == Axis::Z;
}

std::optional<int> read_tool_offset_macro(const nlohmann::json& status,
                                          const std::string& store_object, Axis axis,
                                          int tool_index, const std::string& /*tool_name*/) {
    if (!supports_axis_tool_offset_macro(axis) || tool_index < 0) {
        return std::nullopt;
    }
    // store_object is "gcode_macro <section as printer.cfg spells it>", the
    // key Klipper actually publishes - a direct find, whatever the casing.
    const nlohmann::json* macro = status_object(status, store_object);
    if (!macro) {
        return std::nullopt;
    }
    auto it = macro->find("t" + std::to_string(tool_index) + "_off_z");
    if (it == macro->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::string set_tool_offset_macro(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                  int microns) {
    if (!supports_axis_tool_offset_macro(axis)) {
        return {};
    }
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

std::string save_tool_offset_macro(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                   int microns) {
    // Both stores, so the save stands alone: SAVE_VARIABLE is the durable copy,
    // the macro variable is what the machine actually prints with. SAVE_VARIABLE
    // lands immediately - no SAVE_CONFIG, no restart.
    const std::string runtime = set_tool_offset_macro(hw, axis, tool_index, microns);
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
// adds all three to every move:
//
//     transformed_pos = [newpos[0] + self.tool.gcode_x_offset,
//                        newpos[1] + self.tool.gcode_y_offset,
//                        newpos[2] + self.tool.gcode_z_offset] + newpos[3:]
//
// Because the transform reads the attribute per move, SET_TOOL_PARAMETER takes
// effect on the next move - no re-select needed and no gcode_move involvement.
// It is absolute and one parameter per command, so X, Y and Z are three
// separate writes. SAVE_TOOL_PARAMETER goes through configfile.set(), i.e. a
// pending config change awaiting SAVE_CONFIG, and persists whatever the tool
// currently holds - it must follow a SET_TOOL_PARAMETER for the same name in
// the same Klipper session or the firmware rejects it.
//
// Only the offset model from the 2025.12 gcode-transform rework is supported.
// Older builds folded tool offsets into the user's gcode_move offset, where a
// baby-step and a tool change fight each other; we do not detect or paper over
// that.
bool detect_toolchanger(const PrinterDiscovery& hw) {
    return hw.has_tool_changer();
}

bool supports_axis_toolchanger(Axis /*axis*/) {
    return true;
}

/// The tool parameter (and status field - they are the same name) for an axis.
const char* toolchanger_param(Axis axis) {
    static constexpr const char* names[] = {"gcode_x_offset", "gcode_y_offset", "gcode_z_offset"};
    return names[axis_index(axis)];
}

std::optional<int> read_toolchanger(const nlohmann::json& status,
                                    const std::string& /*store_object*/, Axis axis,
                                    int /*tool_index*/, const std::string& tool_name) {
    if (tool_name.empty()) {
        return std::nullopt;
    }
    const nlohmann::json* tool = status_object(status, "tool " + tool_name);
    if (!tool) {
        return std::nullopt;
    }
    auto it = tool->find(toolchanger_param(axis));
    if (it == tool->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::vector<std::string> status_objects_none(const PrinterDiscovery& /*hw*/) {
    return {};
}

std::string set_toolchanger(const PrinterDiscovery& /*hw*/, Axis axis, int tool_index,
                            int microns) {
    return "SET_TOOL_PARAMETER T=" + std::to_string(tool_index) +
           " PARAMETER=" + toolchanger_param(axis) + " VALUE=" + mm_literal(microns);
}

std::string save_toolchanger(const PrinterDiscovery& hw, Axis axis, int tool_index, int microns) {
    // SAVE_TOOL_PARAMETER persists whatever the tool currently holds, so set the
    // value first and the pair is both self-sufficient and idempotent after a
    // set_tool_offset_gcode() with the same value.
    return set_toolchanger(hw, axis, tool_index, microns) +
           "\nSAVE_TOOL_PARAMETER T=" + std::to_string(tool_index) +
           " PARAMETER=" + toolchanger_param(axis);
}

const std::vector<Provider>& providers() {
    // Row order is match priority, as in z_offset_persistence.cpp. MedusaHC
    // FIRST: it is a klipper-toolchanger printer too, so it matches both rows,
    // and the second one would write a store its macros never read.
    static const std::vector<Provider> table = {
        {"TOOL_OFFSET macro", &detect_tool_offset_macro, &supports_axis_tool_offset_macro,
         &status_objects_tool_offset_macro, &read_tool_offset_macro, &set_tool_offset_macro,
         &save_tool_offset_macro, false},
        {"klipper-toolchanger", &detect_toolchanger, &supports_axis_toolchanger,
         &status_objects_none, &read_toolchanger, &set_toolchanger, &save_toolchanger, true},
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

bool supports_per_tool_offsets(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

bool supports_axis(const PrinterDiscovery& hw, Axis axis) {
    const Provider* p = match(hw);
    return p != nullptr && p->supports_axis(axis);
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    if (const Provider* p = match(hw)) {
        return p->status_objects(hw);
    }
    return {};
}

OffsetReader resolve_reader(const PrinterDiscovery& hw) {
    OffsetReader reader;
    reader.provider = match(hw);
    if (reader.provider) {
        // A single-object store is exactly the extra subscription the row
        // asks for, already spelled the way Klipper publishes it.
        const auto objects = reader.provider->status_objects(hw);
        if (!objects.empty()) {
            reader.store_object = objects.front();
        }
    }
    return reader;
}

std::optional<int> read_tool_offset_microns(const OffsetReader& reader,
                                            const nlohmann::json& status, Axis axis, int tool_index,
                                            const std::string& tool_name) {
    // By the resolved model, never by the frame's schema: a MedusaHC delta
    // frame carrying only `tool T0` must not be answered from
    // klipper-toolchanger's copy, and an axis the model does not keep is
    // "no news" even when some other object in the frame has a value for it.
    const Provider* p = reader.provider;
    if (!p || !p->supports_axis(axis)) {
        return std::nullopt;
    }
    return p->read(status, reader.store_object, axis, tool_index, tool_name);
}

std::string set_tool_offset_gcode(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                  int microns) {
    const Provider* p = match(hw);
    if (!p || !p->supports_axis(axis) || tool_index < 0) {
        return {};
    }
    return p->set_gcode(hw, axis, tool_index, microns);
}

std::string save_tool_offset_gcode(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                   int microns) {
    const Provider* p = match(hw);
    if (!p || !p->supports_axis(axis) || tool_index < 0) {
        return {};
    }
    return p->save_gcode(hw, axis, tool_index, microns);
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

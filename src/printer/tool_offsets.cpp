// SPDX-License-Identifier: GPL-3.0-or-later

#include "tool_offsets.h"

#include "indx_contract.h"
#include "printer_discovery.h"

#include <cmath>
#include <cstdio>

#include "hv/json.hpp"

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
    /// The offset added to every tool's own; nullptr when the model keeps none.
    std::optional<int> (*read_global_z)(const nlohmann::json& status);
    /// Runtime write - takes effect now, not persisted. nullptr for a
    /// read-only model, one the printer measures and writes itself.
    std::string (*set_gcode)(const PrinterDiscovery& hw, Axis axis, int tool_index, int microns);
    /// Durable write. Self-sufficient and idempotent after set_gcode().
    std::string (*save_gcode)(const PrinterDiscovery& hw, Axis axis, int tool_index, int microns);
    /// Whether save_gcode() only stages the change, awaiting SAVE_CONFIG.
    bool persist_needs_save_config;
    /// Gcode that applies a MOUNTED tool's stored offset to the head now, for a
    /// model whose writes otherwise wait for the next tool change. nullptr when
    /// a write already takes effect on the next move.
    std::string (*activate_gcode)(const PrinterDiscovery& hw, int tool_index);
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
// `[gcode_macro _TOOL_OFFSET]` with `variable_t<N>_off_{x,y,z}`, and the
// durable copy in save_variables under `t<N>_gcode_{x,y,z}_offset`, which its
// controller loads into the macro at startup. The controller resolves the macro
// as `_TOOL_OFFSET` first and the unprefixed `TOOL_OFFSET` second, and so does
// this row, so either spelling is the store the machine prints with.
//
// The controller applies the mounted tool's X/Y/Z absolutely with
// SET_GCODE_OFFSET on every swap (`MHC_TOOL_OFFSET T=<n>` does the same on
// demand), so writing a variable changes nothing on the head until the next
// swap or that command. activate_gcode below is the "now" half.
//
// A MedusaHC can ALSO run klipper-toolchanger beside its controller, so it can
// match both rows and must come first. Writing klipper-toolchanger's own offset
// there would set a value the controller never reads.
//
// Detection is the macro itself rather than the machine, so any config using
// this store is covered and the row does not go stale when the hardware is
// rebranded.

/// The offset macro as printer.cfg spells it: the `_`-prefixed name when the
/// printer has it, else the plain one, else empty.
std::string tool_offset_macro_name(const PrinterDiscovery& hw) {
    for (const char* candidate : {"_TOOL_OFFSET", "TOOL_OFFSET"}) {
        std::string name = hw.macro_config_name(candidate);
        if (!name.empty()) {
            return name;
        }
    }
    return {};
}

bool detect_tool_offset_macro(const PrinterDiscovery& hw) {
    // The macro AND more than one toolhead. `TOOL_OFFSET` is a plausible name
    // for a hand-written macro on a single-extruder machine, and matching on it
    // alone would put the tool selector on such a printer and point its buttons
    // at `t0_off_z` in a macro that has no such variable and a [save_variables]
    // section that need not exist. supports_per_tool_offsets() promises false on
    // every single-toolhead printer; this is what keeps that promise.
    return !tool_offset_macro_name(hw).empty() && hw.tool_names().size() > 1;
}

std::vector<std::string> status_objects_tool_offset_macro(const PrinterDiscovery& hw) {
    // The name as printer.cfg spells it, not the uppercased one we matched on:
    // Klipper never resolves a subscription request case-insensitively, and an
    // object it cannot look up is silently absent from every frame rather than
    // an error. Asking for "gcode_macro TOOL_OFFSET" on a machine configured
    // `[gcode_macro Tool_Offset]` therefore reads empty forever.
    const std::string name = tool_offset_macro_name(hw);
    if (name.empty()) {
        return {};
    }
    return {"gcode_macro " + name};
}

/// `t<n>_off_<axis>`, the runtime variable on the offset macro.
std::string tool_offset_variable(int tool_index, Axis axis) {
    return "t" + std::to_string(tool_index) + "_off_" + axis_letter(axis);
}

std::optional<int> read_tool_offset_macro(const nlohmann::json& status,
                                          const std::string& store_object, Axis axis,
                                          int tool_index, const std::string& /*tool_name*/) {
    if (tool_index < 0) {
        return std::nullopt;
    }
    // store_object is "gcode_macro <section as printer.cfg spells it>", the
    // key Klipper actually publishes - a direct find, whatever the casing.
    const nlohmann::json* macro = status_object(status, store_object);
    if (!macro) {
        return std::nullopt;
    }
    auto it = macro->find(tool_offset_variable(tool_index, axis));
    if (it == macro->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::string set_tool_offset_macro(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                  int microns) {
    // MACRO= is a mux key registered on the CONFIG-case name, not the alias
    // (klippy/extras/gcode_macro.py registers `name`, not `self.alias`), so a
    // capitalised MACRO= is rejected outright on a `[gcode_macro Tool_Offset]`
    // machine - the command errors rather than silently missing.
    const std::string name = tool_offset_macro_name(hw);
    if (name.empty()) {
        return {};
    }
    return "SET_GCODE_VARIABLE MACRO=" + name +
           " VARIABLE=" + tool_offset_variable(tool_index, axis) + " VALUE=" + mm_literal(microns);
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
    return "SAVE_VARIABLE VARIABLE=t" + std::to_string(tool_index) + "_gcode_" + axis_letter(axis) +
           "_offset VALUE=" + mm_literal(microns) + "\n" + runtime;
}

std::string activate_tool_offset_macro(const PrinterDiscovery& hw, int tool_index) {
    // MHC_TOOL_OFFSET is the [medusahc] controller's own command; MOVE=0
    // shifts the coordinate system without moving the head over the print.
    if (!hw.has_medusahc()) {
        return {};
    }
    return "MHC_TOOL_OFFSET T=" + std::to_string(tool_index) + " MOVE=0";
}

// --- Bondtech INDX: save_variables, written by its own calibration ---------
//
// INDX's calibration macros measure each tool against tool 0 and keep the
// result in save_variables as t<n>_offset_x|y|z, plus a global_z_offset added
// to every tool. Its tool-change macros apply them; nothing else writes them,
// so the row is read-only.
bool supports_axis_all(Axis /*axis*/) {
    return true;
}

/// save_variables.variables, or nullptr.
const nlohmann::json* save_variables(const nlohmann::json& status) {
    const nlohmann::json* sv = status_object(status, "save_variables");
    if (!sv) {
        return nullptr;
    }
    auto it = sv->find("variables");
    return it != sv->end() && it->is_object() ? &(*it) : nullptr;
}

std::optional<int> read_indx(const nlohmann::json& status, const std::string& /*store_object*/,
                             Axis axis, int tool_index, const std::string& /*tool_name*/) {
    const nlohmann::json* vars = save_variables(status);
    if (!vars || tool_index < 0) {
        return std::nullopt;
    }
    const std::string key = "t" + std::to_string(tool_index) + "_offset_" + axis_letter(axis);
    auto it = vars->find(key);
    if (it == vars->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::optional<int> read_global_z_indx(const nlohmann::json& status) {
    const nlohmann::json* vars = save_variables(status);
    if (!vars) {
        return std::nullopt;
    }
    auto it = vars->find("global_z_offset");
    if (it == vars->end()) {
        return std::nullopt;
    }
    return to_microns(*it);
}

std::vector<std::string> status_objects_none(const PrinterDiscovery& /*hw*/);

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
    // before klipper-toolchanger: it can run klipper-toolchanger too, so it
    // can match both rows, and the later one would write a store its
    // controller never reads.
    // INDX's subscription already carries save_variables, so its row asks for
    // nothing more.
    static const std::vector<Provider> table = {
        {"INDX save_variables", &helix::indx::present, &supports_axis_all, &status_objects_none,
         &read_indx, &read_global_z_indx, nullptr, nullptr, false, nullptr},
        {"TOOL_OFFSET macro", &detect_tool_offset_macro, &supports_axis_all,
         &status_objects_tool_offset_macro, &read_tool_offset_macro, nullptr,
         &set_tool_offset_macro, &save_tool_offset_macro, false, &activate_tool_offset_macro},
        {"klipper-toolchanger", &detect_toolchanger, &supports_axis_toolchanger,
         &status_objects_none, &read_toolchanger, nullptr, &set_toolchanger, &save_toolchanger,
         true, nullptr},
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

/// The matched row when HelixScreen may write it, else nullptr.
const Provider* match_writable(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p != nullptr && p->set_gcode != nullptr ? p : nullptr;
}

} // namespace

bool supports_per_tool_offsets(const PrinterDiscovery& hw) {
    return match_writable(hw) != nullptr;
}

bool supports_axis(const PrinterDiscovery& hw, Axis axis) {
    const Provider* p = match_writable(hw);
    return p != nullptr && p->supports_axis(axis);
}

bool reads_axis(const PrinterDiscovery& hw, Axis axis) {
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

std::optional<int> read_global_z_offset_microns(const OffsetReader& reader,
                                                const nlohmann::json& status) {
    const Provider* p = reader.provider;
    if (!p || !p->read_global_z) {
        return std::nullopt;
    }
    return p->read_global_z(status);
}

std::string set_tool_offset_gcode(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                  int microns) {
    const Provider* p = match_writable(hw);
    if (!p || !p->supports_axis(axis) || tool_index < 0) {
        return {};
    }
    return p->set_gcode(hw, axis, tool_index, microns);
}

std::string save_tool_offset_gcode(const PrinterDiscovery& hw, Axis axis, int tool_index,
                                   int microns) {
    const Provider* p = match_writable(hw);
    if (!p || !p->supports_axis(axis) || tool_index < 0) {
        return {};
    }
    return p->save_gcode(hw, axis, tool_index, microns);
}

std::string activate_mounted_offset_gcode(const PrinterDiscovery& hw, int tool_index,
                                          int mounted_tool) {
    const Provider* p = match_writable(hw);
    if (!p || !p->activate_gcode || tool_index < 0 || tool_index != mounted_tool) {
        return {};
    }
    return p->activate_gcode(hw, tool_index);
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

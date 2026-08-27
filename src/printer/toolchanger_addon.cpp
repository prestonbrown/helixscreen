// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolchanger_addon.h"

#include "printer_discovery.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace helix::toolchanger_addon {
namespace {

constexpr const char* kMedusaObject = "medusahc";

/// Sanity bound on a dock index parsed out of a status key, so a malformed
/// "t9999999" cannot size a vector off a network payload.
constexpr int kMaxDockIndex = 63;

/// One machine bolted onto klipper-toolchanger.
struct Provider {
    const char* name;
    /// Identifies the machine from discovered hardware.
    bool (*detect)(const PrinterDiscovery& hw);
    /// Status objects carrying its sensor state.
    std::vector<std::string> (*status_objects)(const PrinterDiscovery& hw);
    /// Feeder gcode, or nullptr for a machine without one.
    std::string (*open_gcode)(const PrinterDiscovery& hw);
    std::string (*close_gcode)(const PrinterDiscovery& hw);
    /// What drives a swap when klipper-toolchanger is not installed to do it.
    /// Prefixed to the tool number; nullptr when the machine has no such
    /// command of its own.
    const char* select_prefix;
    /// Unmounts the tool on the head, or nullptr when there is no such command.
    const char* unselect_gcode;
};

// --- MedusaHC ---------------------------------------------------------------
//
// A budget hotend changer (Irbis3D/MedusaHC). klipper-toolchanger performs the
// swap; MedusaHC's own pin_watch.py extra reads the dock sensors. Both signals
// are required to claim the machine: pin_watch alone is just the extra, and
// [toolchanger] alone is any of the many klipper-toolchanger builds.
//
// There are three shipping configurations and this has to serve all of them:
//
//   (a) Irbis3D stock - [pin_watch io] + toolchanger.cfg, OPEN/CLOSE macros,
//       no [medusahc] object. pin_watch.get_status() is {"current_tool": int}.
//   (b) Irbis3D MedusaHC-Python-Controller - adds [medusahc], whose get_status()
//       carries operation / current_tool / target_tool / last_error /
//       feeder_open / layer / sensor_error / tool_count / sensors. Registers
//       MHC_* commands and ships legacy OPEN/CLOSE aliases forwarding to them.
//   (c) third-party forks - also register [medusahc], but with a DIFFERENT
//       schema: state instead of operation, error instead of last_error, flat
//       toolN_docked keys instead of a sensors dict.
//
// Object presence cannot separate (b) from (c), so read_medusahc() discriminates
// on FIELD NAMES and reads the Irbis3D names first. The fork's names are a
// fallback, never a default.

bool medusa_detect(const PrinterDiscovery& hw) {
    // The reference shape is klipper-toolchanger plus the pin_watch extra, and
    // neither half alone is enough: pin_watch is just the extra, and
    // [toolchanger] is any of the many klipper-toolchanger builds.
    //
    // The second clause is a compatibility path, not a second mainline. It
    // catches a machine carrying [medusahc] and nothing else, which today means
    // exactly one fork (bundle 6QWNVZY5) - and, eventually, upstream itself if
    // Sergei follows through on dropping the klipper-toolchanger dependency.
    // Nothing else in Klipper registers [medusahc], so it needs no second half.
    return (hw.has_pin_watch() && hw.has_tool_changer()) || hw.has_medusahc();
}

std::vector<std::string> medusa_status_objects(const PrinterDiscovery& hw) {
    std::vector<std::string> objects;
    // The [medusahc] object may not exist (config (a)); subscribing to an absent
    // object is harmless and it appears the moment the user migrates.
    objects.emplace_back(hw.has_medusahc() ? hw.medusahc_object_name() : kMedusaObject);
    if (!hw.pin_watch_object_name().empty()) {
        objects.push_back(hw.pin_watch_object_name());
    }
    return objects;
}

/// Prefer the controller's native command when the printer actually has it.
/// The official build also ships OPEN/CLOSE aliases forwarding to MHC_*, so
/// either works there; picking what is present keeps config (a) working too.
std::string medusa_open_gcode(const PrinterDiscovery& hw) {
    return hw.has_macro("MHC_OPEN") ? "MHC_OPEN" : "OPEN";
}

std::string medusa_close_gcode(const PrinterDiscovery& hw) {
    return hw.has_macro("MHC_CLOSE") ? "MHC_CLOSE" : "CLOSE";
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> table = {
        // T<n> and DROP_TOOL are what the extra registers when it runs the swap
        // itself, which is the fork case only - on the reference config
        // klipper-toolchanger is present and SELECT_TOOL/UNSELECT_TOOL stay in
        // use. DROP_TOOL is a bare gcode command, not a [gcode_macro], so it
        // never appears in printer.objects.list: it cannot be capability-checked
        // the way the feeder macros are, and naming it here is the whole point of
        // this table.
        {"MedusaHC", medusa_detect, medusa_status_objects, medusa_open_gcode, medusa_close_gcode,
         "T", "DROP_TOOL"},
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

/// Fetch an int field, tolerating the string form some Klipper extras emit.
std::optional<int> int_field(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> bool_field(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return std::nullopt;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    return std::nullopt;
}

/// Tool index out of a dock key: "t3" -> 3, "tool3_docked" -> 3. nullopt when
/// the key is not one.
std::optional<int> dock_index(const std::string& key) {
    std::string digits;
    if (key.size() > 1 && key[0] == 't' && key.rfind("tool", 0) != 0) {
        digits = key.substr(1);
    } else if (key.rfind("tool", 0) == 0 && key.size() > 4) {
        const auto suffix = key.find("_docked");
        if (suffix == std::string::npos || suffix <= 4) {
            return std::nullopt;
        }
        digits = key.substr(4, suffix - 4);
    } else {
        return std::nullopt;
    }
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                       [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    try {
        return std::stoi(digits);
    } catch (...) {
        return std::nullopt;
    }
}

/// 1/0 and true/false both mean the same thing to these controllers.
std::optional<bool> as_bool(const nlohmann::json& v) {
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number_integer()) {
        return v.get<int>() != 0;
    }
    return std::nullopt;
}

/// Record dock `index` without disturbing the ones this frame did not mention.
void set_dock(std::vector<std::optional<bool>>& docks, int index, bool seated) {
    if (index < 0 || index > kMaxDockIndex) {
        return;
    }
    if (static_cast<size_t>(index) >= docks.size()) {
        docks.resize(static_cast<size_t>(index) + 1);
    }
    docks[static_cast<size_t>(index)] = seated;
}

/// Read the [medusahc] object. Irbis3D field names first, fork names as fallback.
std::optional<ToolReading> read_medusahc(const nlohmann::json& obj) {
    ToolReading r;
    bool saw_anything = false;

    if (auto tool = int_field(obj, "current_tool")) {
        r.current_tool = *tool;
        saw_anything = true;
    }
    if (auto count = int_field(obj, "tool_count")) {
        r.tool_count = *count;
        saw_anything = true;
    }

    // Irbis3D: "operation" (idle/picking/dropping). topi314: "state"
    // (uninitialized/ready/changing/error). Read in that order, upstream first.
    // These are NOT interchangeable spellings - only `operation` names the swap
    // direction - so which key answered is recorded alongside the value.
    for (const char* key : {"operation", "state"}) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_string()) {
            r.operation = it->get<std::string>();
            r.phase_names_direction = (std::string_view(key) == "operation");
            saw_anything = true;
            break;
        }
    }

    // Irbis3D publishes sensor_error outright; both schemas encode it as -2.
    if (auto flag = bool_field(obj, "sensor_error")) {
        r.sensor_error = *flag;
        saw_anything = true;
    }
    if (r.current_tool == -2) {
        r.sensor_error = true;
    }

    // Per-dock occupancy. Irbis3D publishes a sensors dict keyed "e" for the
    // toolhead and "t<n>" per dock; the fork flattens it to tool<n>_docked plus
    // head_loaded. Sergei, 2026-08-24: "e: 1 means that a tool is installed on
    // the toolhead ... tN: 1 means that the corresponding tool is seated in its
    // dock."
    auto sensors = obj.find("sensors");
    if (sensors != obj.end() && sensors->is_object()) {
        for (const auto& [key, value] : sensors->items()) {
            auto flag = as_bool(value);
            if (!flag) {
                continue;
            }
            if (key == "e") {
                r.head_loaded = *flag;
                saw_anything = true;
            } else if (auto index = dock_index(key)) {
                set_dock(r.docks, *index, *flag);
                saw_anything = true;
            }
        }
    }
    for (const auto& [key, value] : obj.items()) {
        if (key.rfind("tool", 0) != 0) {
            continue;
        }
        if (auto index = dock_index(key)) {
            if (auto flag = as_bool(value)) {
                set_dock(r.docks, *index, *flag);
                saw_anything = true;
            }
        }
    }
    if (auto loaded = bool_field(obj, "head_loaded")) {
        r.head_loaded = *loaded;
        saw_anything = true;
    }
    // Irbis3D-only. Left nullopt everywhere else on purpose: a machine that never
    // reports the gripper is not a machine whose gripper is closed.
    if (auto feeder = bool_field(obj, "feeder_open")) {
        r.feeder_open = *feeder;
        saw_anything = true;
    }

    return saw_anything ? std::optional<ToolReading>(r) : std::nullopt;
}

/// Read a pin_watch object. Its whole status is {"current_tool": int}, which is
/// the sensor truth on config (a) and the source [medusahc] itself reads.
std::optional<ToolReading> read_pin_watch(const nlohmann::json& obj) {
    auto tool = int_field(obj, "current_tool");
    if (!tool) {
        return std::nullopt;
    }
    ToolReading r;
    r.current_tool = *tool;
    r.sensor_error = (*tool == -2);
    return r;
}

} // namespace

bool present(const PrinterDiscovery& hw) {
    return match(hw) != nullptr;
}

std::string machine_name(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->name : std::string();
}

ToolSensor resolve_tool_sensor(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p) {
        return {};
    }
    return ToolSensor{true, p->name};
}

ToolCommands resolve_tool_commands(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    // klipper-toolchanger present means SELECT_TOOL/UNSELECT_TOOL exist and are
    // the machine's own answer. Only a changer running without it needs to name
    // its commands here.
    if (!p || hw.has_tool_changer()) {
        return {};
    }
    ToolCommands c;
    c.present = true;
    c.provider_name = p->name;
    c.select_prefix = p->select_prefix ? p->select_prefix : "";
    c.unselect = p->unselect_gcode ? p->unselect_gcode : "";
    return c;
}

std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw);

Feeder resolve_feeder(const PrinterDiscovery& hw, const std::string& open_override,
                      const std::string& close_override) {
    const Provider* p = match(hw);
    if (!p || !p->open_gcode) {
        return {};
    }
    auto pick = [](const std::string& override_value, const std::string& detected) {
        return (override_value.empty() || override_value == kAutoMacro) ? detected : override_value;
    };
    Feeder f;
    f.present = true;
    f.provider_name = p->name;
    f.detected_open = p->open_gcode(hw);
    f.detected_close = p->close_gcode(hw);
    f.open_gcode = pick(open_override, f.detected_open);
    f.close_gcode = pick(close_override, f.detected_close);
    f.open_choice = open_override.empty() ? kAutoMacro : open_override;
    f.close_choice = close_override.empty() ? kAutoMacro : close_override;

    auto candidates = feeder_macro_candidates(hw);
    if (!candidates.empty()) {
        f.macro_options.emplace_back(kAutoMacro);
        f.macro_options.insert(f.macro_options.end(), candidates.begin(), candidates.end());
    }
    return f;
}

std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw) {
    std::vector<std::string> out;
    if (!match(hw)) {
        return out;
    }
    // A feeder macro is one of the controller's own MHC_* commands, or a macro
    // whose name says what it does. Everything else on the printer would be
    // noise in a picker.
    for (const auto& macro : hw.macros()) {
        const bool native = macro.rfind("MHC_", 0) == 0;
        const bool named =
            macro.find("OPEN") != std::string::npos || macro.find("CLOSE") != std::string::npos ||
            macro.find("FEEDER") != std::string::npos || macro.find("GRIP") != std::string::npos;
        if (native || named) {
            out.push_back(macro);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> required_status_objects(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p ? p->status_objects(hw) : std::vector<std::string>{};
}

std::optional<ToolReading> read_tool(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    // [medusahc] wins when present: it reads pin_watch itself and adds the
    // operation phase and the explicit sensor_error flag on top.
    auto medusa = status.find(kMedusaObject);
    if (medusa != status.end() && medusa->is_object()) {
        if (auto r = read_medusahc(*medusa)) {
            return r;
        }
    }
    for (const auto& [key, value] : status.items()) {
        if ((key == "pin_watch" || key.rfind("pin_watch ", 0) == 0) && value.is_object()) {
            if (auto r = read_pin_watch(value)) {
                return r;
            }
        }
    }
    return std::nullopt;
}

} // namespace helix::toolchanger_addon

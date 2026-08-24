// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolchanger_addon.h"

#include "printer_discovery.h"

#include <algorithm>
#include <cctype>

namespace helix::toolchanger_addon {
namespace {

constexpr const char* kMedusaObject = "medusahc";

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
    return hw.has_pin_watch() && hw.has_tool_changer();
}

std::vector<std::string> medusa_status_objects(const PrinterDiscovery& hw) {
    std::vector<std::string> objects;
    // The [medusahc] object may not exist (config (a)); subscribing to an absent
    // object is harmless and it appears the moment the user migrates.
    objects.emplace_back(kMedusaObject);
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
        {"MedusaHC", medusa_detect, medusa_status_objects, medusa_open_gcode, medusa_close_gcode},
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

    // Irbis3D: "operation" (idle/picking/dropping). Fork: "state".
    for (const char* key : {"operation", "state"}) {
        auto it = obj.find(key);
        if (it != obj.end() && it->is_string()) {
            r.operation = it->get<std::string>();
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

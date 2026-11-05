// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolchanger_addon.h"

#include "color_utils.h"
#include "core/text_io.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"
#include "zmod_color_status.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "hv/json.hpp"

namespace helix::toolchanger_addon {
namespace {

constexpr const char* kMedusaObject = "medusahc";

/// Sanity bound on a dock index parsed out of a status key, so a malformed
/// "t9999999" cannot size a vector off a network payload.
constexpr int kMaxDockIndex = 63;

/// One machine this module knows the dialect of.
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
    std::string (*unselect_gcode)(const PrinterDiscovery& hw);
    /// The machine's own commands drive the swap even beside klipper-toolchanger,
    /// whose SELECT_TOOL/UNSELECT_TOOL do not move this hardware.
    bool owns_swap;
    /// Stores a slot's material and colour in firmware, or nullptr when the
    /// firmware keeps no such record.
    std::string (*material_write_gcode)(int slot_index, const std::string& type, std::uint32_t rgb);
    /// Feeds filament into one head, or nullptr when the firmware has no such
    /// command. Returns empty when this printer does not offer it for @p head
    /// (the shim that owns it is absent, or published no lane for the head).
    std::string (*feed_gcode)(const PrinterDiscovery& hw, int head);
    /// One extruder drive serves every tool, so filament moves only through
    /// the tool on the carriage: a load or unload mounts it first.
    bool feeds_through_mount;
    /// The target tool's nozzle must be above this before the controller's
    /// resume_command picks it to continue a paused print; 0 for no such rule.
    int resume_min_nozzle_c;
    /// Brushes the mounted nozzle clean; nullptr, or empty for this printer,
    /// when there is no such command.
    std::string (*clean_gcode)(const PrinterDiscovery& hw);
    /// The controller command a RESUME macro calls to get the resume rule, or
    /// nullptr.
    const char* resume_command;
};

// --- MedusaHC ---------------------------------------------------------------
//
// A budget hotend changer (Irbis3D/MedusaHC). Its pin_watch extra reads the
// dock sensors, and the swap is always the machine's own: `T<n>` macros
// running SET, and DROP_TOOL to park. The configurations this serves:
//
//   (a) Irbis3D stock - [medusahc] + [pin_watch io], no klipper-toolchanger.
//       [medusahc].get_status() carries operation / current_tool / target_tool
//       / last_error / feeder_open / layer / sensor_error / tool_count /
//       sensors, and the controller registers the MHC_* commands that
//       MHC_macros.cfg wraps (SET, DROP_TOOL, OPEN, CLOSE, CLEAN).
//   (b) The same plus klipper-toolchanger (pin_watch sync_toolchanger: 1),
//       which only mirrors the detected tool: its existing T<n> macros are kept
//       and its UNSELECT_TOOL runs an empty dropoff_gcode, so it never moves
//       this hardware.
//   (c) The frozen legacy-macros config: [pin_watch io] + toolchanger.cfg and
//       the same T<n> / DROP_TOOL macros, no [medusahc].

bool medusa_detect(const PrinterDiscovery& hw) {
    // pin_watch is required, and alone is just the extra. Beside it,
    // [toolchanger] (any of the many klipper-toolchanger builds) or [medusahc]
    // (the Python controller, which owns the swap without klipper-toolchanger)
    // makes the machine a MedusaHC.
    return hw.has_pin_watch() && (hw.has_tool_changer() || hw.has_medusahc());
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

/// A failed swap mid-print pauses it (MHC_ERROR). MHC_RESUME, which the
/// upstream example RESUME calls, picks the target tool again, priming it, and
/// refuses while that tool's nozzle is at or below this.
constexpr int kMedusaResumeMinNozzleC = 190;
constexpr const char* kMedusaResumeCommand = "MHC_RESUME";

/// DROP_TOOL is a [gcode_macro] on every shipped configuration (it also closes
/// the feeder). A config that dropped it still has the controller's own pair.
std::string medusa_unselect_gcode(const PrinterDiscovery& hw) {
    if (!hw.has_macro("DROP_TOOL") && hw.has_medusahc()) {
        return "MHC_DROP\nMHC_CLOSE";
    }
    return "DROP_TOOL";
}

/// CLEAN is MHC_macros.cfg's [gcode_macro] wrapping the controller's MHC_CLEAN,
/// which brushes the mounted nozzle and does nothing with the carriage empty.
std::string medusa_clean_gcode(const PrinterDiscovery& hw) {
    return hw.has_macro("CLEAN") ? "CLEAN" : "";
}

/// The plain OPEN/CLOSE names: every configuration has them (the controller's
/// own MHC_* commands are what its OPEN/CLOSE forward to), and they work on a
/// parked tool.
std::string medusa_open_gcode(const PrinterDiscovery& /*hw*/) {
    return "OPEN";
}

std::string medusa_close_gcode(const PrinterDiscovery& /*hw*/) {
    return "CLOSE";
}

// --- Z-Mod on the FlashForge Creator 5 Pro -----------------------------------
//
// Z-Mod runs FlashForge's own Klipper, which has no klipper-toolchanger. Its
// zmod_color extra mounts a head with `_T_IN T=<n>`, parks it with `_T_OUT`, and
// reads gcode_button extruder_pos1..4 (dock) and extruder_grab1..4 (carriage)
// into zmod_color.active_tool_id: 0..3 mounted, -1 nothing on the carriage, -2
// the buttons disagree. The AD5X Z-Mod publishes zmod_color too but has no
// carriage buttons, which is what keeps it out of this row.

constexpr const char* kZmodColorObject = "zmod_color";

bool printer_has_object(const PrinterDiscovery& hw, const char* name) {
    const auto& objects = hw.printer_objects();
    return std::find(objects.begin(), objects.end(), name) != objects.end();
}

bool zmod_c5_detect(const PrinterDiscovery& hw) {
    return printer_has_object(hw, kZmodColorObject) &&
           printer_has_object(hw, "gcode_button extruder_grab1");
}

std::string zmod_c5_unselect_gcode(const PrinterDiscovery& /*hw*/) {
    return "_T_OUT";
}

std::vector<std::string> zmod_c5_status_objects(const PrinterDiscovery& hw) {
    // The afclite shim carries the firmware's own fault flag and activity word
    // on zmod 1.7.4+; subscribing it on a build that does not publish the
    // object is harmless (the frame simply never arrives).
    std::vector<std::string> objects = {kZmodColorObject};
    if (hw.has_zmod_afclite()) {
        objects.emplace_back("zmod_afclite");
        // Each lane's spool_id: a tool that binds a spool to a head from
        // outside (SET_SPOOL_ID) restates nothing zmod_color shows when the
        // new spool's colour matches the old one's.
        for (const auto& lane : hw.afc_lane_names()) {
            objects.emplace_back("AFC_lane " + lane);
        }
    }
    return objects;
}

/// HEX and TYPE together write without opening a prompt; SILENT=1 keeps the
/// GET_ZCOLOR that CHANGE_ZCOLOR runs afterwards from opening one in
/// Mainsail/Fluidd.
std::string zmod_c5_material_write(int slot_index, const std::string& type, std::uint32_t rgb) {
    return fmt::format("CHANGE_ZCOLOR SLOT={} HEX={:06X} TYPE={} SILENT=1",
                       slot_index + 1, // DISPLAY_NUMBERING_OK: gcode wire, not a label
                       rgb & 0xFFFFFFu, type);
}

/// Z-Mod's afclite shim runs `CHANGE_TOOL LANE=<lane>` as the same load the
/// COLOR dialog starts: it heats only that head and feeds it. The lane is named
/// by the shim's own `AFC_lane` object, which is the only thing sent, so
/// `_T_IN_ZCOLOR` and its SLOT/NAPR arguments stay the shim's business. The
/// head-to-lane relation is the lane name: head n is lane `E<n>`, and a lane
/// the shim did not publish yields no command.
std::string zmod_c5_feed_gcode(const PrinterDiscovery& hw, int head) {
    if (!hw.has_zmod_afclite() || head < 0) {
        return {};
    }
    const std::string lane = fmt::format("E{}", head);
    const auto& lanes = hw.afc_lane_names();
    if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) {
        return {};
    }
    return "CHANGE_TOOL LANE=" + lane;
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> table = {
        // T<n> and DROP_TOOL swap tools on every MedusaHC, with or without
        // klipper-toolchanger beside the controller.
        {"MedusaHC", medusa_detect, medusa_status_objects, medusa_open_gcode, medusa_close_gcode,
         "T", medusa_unselect_gcode, /*owns_swap=*/true, nullptr, nullptr,
         /*feeds_through_mount=*/true, kMedusaResumeMinNozzleC, medusa_clean_gcode,
         kMedusaResumeCommand},
        {kZmodC5ProviderName, zmod_c5_detect, zmod_c5_status_objects, nullptr, nullptr,
         "_T_IN T=", zmod_c5_unselect_gcode, /*owns_swap=*/true, zmod_c5_material_write,
         zmod_c5_feed_gcode, /*feeds_through_mount=*/false, /*resume_min_nozzle_c=*/0, nullptr,
         /*resume_command=*/nullptr},
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
        return helix::text_io::parse_leading<int>(it->get<std::string>());
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

/// Tool index out of a dock key: "t3" -> 3. nullopt when the key is not one.
std::optional<int> dock_index(const std::string& key) {
    if (key.size() < 2 || key[0] != 't') {
        return std::nullopt;
    }
    const std::string digits = key.substr(1);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                       [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }
    // All digits, but an arbitrarily long run still overflows int.
    return helix::text_io::parse_leading<int>(digits);
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

/// Read the [medusahc] object.
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

    // idle / changing / dropping / picking: the words name the swap direction.
    if (auto it = obj.find("operation"); it != obj.end() && it->is_string()) {
        r.operation = it->get<std::string>();
        r.phase_names_direction = true;
        saw_anything = true;
    }

    if (auto it = obj.find("last_error"); it != obj.end() && it->is_string()) {
        r.last_error = it->get<std::string>();
        saw_anything = true;
    }
    if (auto target = int_field(obj, "target_tool")) {
        r.target_tool = *target;
        saw_anything = true;
    }

    // sensor_error is published outright, and current_tool encodes it as -2.
    if (auto flag = bool_field(obj, "sensor_error")) {
        r.sensor_error = *flag;
        r.sensor_error_reported = *flag;
        r.sensor_error_named = true;
        saw_anything = true;
    }
    if (r.current_tool == -2) {
        r.sensor_error = true;
    }

    // Per-dock occupancy: a sensors dict keyed "e" for the toolhead and "t<n>"
    // per dock. Sergei, 2026-08-24: "e: 1 means that a tool is installed on
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
    // Left nullopt when absent on purpose: a machine that never reports the
    // gripper is not a machine whose gripper is closed.
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

/// The firmware's own -2 is reported as a sensor error, not derived here.
std::optional<ToolReading> read_zmod_color(const nlohmann::json& obj) {
    auto tool = int_field(obj, "active_tool_id");
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

bool zmod_c5_present(const PrinterDiscovery& hw) {
    return zmod_c5_detect(hw);
}

bool firmware_picks_lanes_at_print(const PrinterDiscovery& hw) {
    return hw.has_zmod_afclite();
}

std::optional<ToolReading> parse_zmod_marker(const std::string& line) {
    // Both spellings are respond_info output, so on the wire each arrives with
    // whatever console framing the host adds; the tokens themselves are unique
    // to the swap macros. A change runs the dropping markers first (the parked
    // head's echoes) and the picking markers second, which is the order the
    // step bar wants.
    ToolReading r;
    r.phase_names_direction = true;
    if (line.find("Grab attempt for T") != std::string::npos ||
        line.find("_T_IN") != std::string::npos) {
        r.operation = "picking";
        return r;
    }
    if (line.find("Unload attempt for T") != std::string::npos ||
        line.find("_T_OUT") != std::string::npos) {
        r.operation = "dropping";
        return r;
    }
    return std::nullopt;
}

std::optional<ToolReading> parse_zmod_tinfo_line(const std::string& line, bool& block_ended_out) {
    block_ended_out = false;
    // T_INFO's rows are `T<n>: home|HEAD|?|ERROR (both)`; the trailing block is
    // Door/Top/Offset, and `Offset:` is documented last, so it is the block
    // end the "no HEAD anywhere" answer hangs off.
    std::string_view view = line;
    while (!view.empty() && (view.front() == ' ' || view.front() == '/')) {
        view.remove_prefix(1);
    }
    if (view.rfind("Offset:", 0) == 0) {
        block_ended_out = true;
        return std::nullopt;
    }
    if (view.empty() || view.front() != 'T') {
        return std::nullopt;
    }
    const std::size_t colon = view.find(':');
    if (colon == std::string_view::npos || colon < 2) {
        return std::nullopt;
    }
    const auto index = helix::text_io::parse_leading<int>(std::string(view.substr(1, colon - 1)));
    if (!index || *index < 0) {
        return std::nullopt;
    }
    const std::size_t word = view.find_first_not_of(" \t", colon + 1);
    if (word == std::string_view::npos) {
        return std::nullopt;
    }
    ToolReading r;
    if (view.compare(word, 4, "HEAD") == 0) {
        r.current_tool = *index;
    } else if (view.compare(word, 4, "home") == 0) {
        r.docks.resize(static_cast<size_t>(*index) + 1);
        r.docks[static_cast<size_t>(*index)] = true;
    } else if (view.compare(word, 5, "ERROR") == 0) {
        // "ERROR (both)": the tool reads home AND on the head, which is the
        // same disagreeing-sensors fault a -2 active_tool_id is.
        r.sensor_error = true;
    } else {
        // "?" (undefined) says nothing for this tool; anything unrecognised is
        // treated the same way rather than guessed at.
        return std::nullopt;
    }
    return r;
}

std::optional<AfcStateReading> read_afc_state(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    // The extra registers itself as `zmod_afclite` and aliases `AFC`; either
    // can carry a delta.
    const auto it = status.find("zmod_afclite");
    const auto alias = status.find("AFC");
    const nlohmann::json* obj = nullptr;
    if (it != status.end() && it->is_object()) {
        obj = &*it;
    } else if (alias != status.end() && alias->is_object()) {
        obj = &*alias;
    }
    if (obj == nullptr) {
        return std::nullopt;
    }
    AfcStateReading r;
    if (auto state = obj->find("current_state"); state != obj->end() && state->is_string()) {
        r.current_state = state->get<std::string>();
    }
    if (auto err = obj->find("error_state"); err != obj->end() && err->is_boolean()) {
        r.error_state = err->get<bool>();
    }
    if (auto msg = obj->find("message"); msg != obj->end() && msg->is_string()) {
        r.message = msg->get<std::string>();
    }
    if (auto lanes = obj->find("lanes"); lanes != obj->end() && lanes->is_array()) {
        for (const auto& lane_name : *lanes) {
            if (!lane_name.is_string()) {
                continue;
            }
            const std::string key = "AFC_lane " + lane_name.get<std::string>();
            const auto lane = status.find(key);
            if (lane == status.end() || !lane->is_object()) {
                continue;
            }
            const auto map = lane->find("map");
            if (map != lane->end() && map->is_string()) {
                r.lane_maps.push_back(map->get<std::string>());
            }
        }
    }
    return r;
}

std::optional<std::vector<AfcLaneSpool>> read_afc_lane_spools(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    constexpr std::string_view kPrefix = "AFC_lane ";
    std::vector<AfcLaneSpool> spools;
    for (auto it = status.begin(); it != status.end(); ++it) {
        if (it.key().rfind(kPrefix, 0) != 0 || !it.value().is_object()) {
            continue;
        }
        const auto id = it.value().find("spool_id");
        if (id == it.value().end()) {
            continue;
        }
        AfcLaneSpool spool;
        spool.lane = it.key().substr(kPrefix.size());
        if (id->is_number_integer()) {
            spool.spool_id = std::max(0, id->get<int>());
        }
        spools.push_back(std::move(spool));
    }
    if (spools.empty()) {
        return std::nullopt;
    }
    return spools;
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

std::string clean_nozzle_gcode(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->clean_gcode ? p->clean_gcode(hw) : std::string();
}

ResumeRule resume_rule(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p || p->resume_min_nozzle_c <= 0 || p->resume_command == nullptr) {
        return {};
    }
    return ResumeRule{p->resume_min_nozzle_c, p->resume_command};
}

bool resume_rule_in_force(const ResumeRule& rule) {
    return rule.min_nozzle_c > 0 && !rule.command.empty() &&
           MacroParamCache::instance().macro_runs("RESUME", rule.command);
}

ToolCommands resolve_tool_commands(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    // klipper-toolchanger present means SELECT_TOOL/UNSELECT_TOOL exist and are
    // the machine's own answer, unless the provider swaps the hardware itself.
    if (hw.has_tool_changer() && !(p && p->owns_swap)) {
        return {};
    }
    // Otherwise T<n> is what every multi-tool machine answers to: Klipper maps
    // it to ACTIVATE_EXTRUDER, and a changer extra registers its own T<n> over
    // that. So the table only has to name what DIFFERS, which is the unmount -
    // a machine whose tools are plain extruders has none, and its tool can only
    // be swapped for another.
    ToolCommands c;
    c.present = true;
    c.provider_name = p ? p->name : std::string();
    c.select_prefix = (p && p->select_prefix) ? p->select_prefix : "T";
    c.unselect = (p && p->unselect_gcode) ? p->unselect_gcode(hw) : "";
    c.initialize_available = hw.has_tool_changer();
    return c;
}

std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw);

Feeder resolve_feeder(const PrinterDiscovery& hw, const std::string& open_override,
                      const std::string& close_override) {
    const Provider* p = match(hw);
    if (!p || !p->open_gcode) {
        return {};
    }
    Feeder f;
    f.present = true;
    f.provider_name = p->name;
    f.detected_open = p->open_gcode(hw);
    f.detected_close = p->close_gcode(hw);
    f.open_gcode = resolve_macro_choice(open_override, f.detected_open);
    f.close_gcode = resolve_macro_choice(close_override, f.detected_close);
    f.open_choice = macro_choice_or_auto(open_override);
    f.close_choice = macro_choice_or_auto(close_override);

    auto candidates = feeder_macro_candidates(hw);
    if (!candidates.empty()) {
        f.macro_options.emplace_back(kAutoMacro);
        f.macro_options.insert(f.macro_options.end(), candidates.begin(), candidates.end());
    }
    return f;
}

std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw) {
    std::vector<std::string> out;
    const Provider* p = match(hw);
    if (!p || !p->open_gcode) {
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

ToolFeed resolve_tool_feed(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p || !p->feed_gcode) {
        return {};
    }
    ToolFeed feed;
    feed.provider_name = p->name;
    const int heads = static_cast<int>(hw.afc_lane_names().size());
    for (int head = 0; head < heads; ++head) {
        feed.head_gcode.push_back(p->feed_gcode(hw, head));
    }
    feed.present = std::any_of(feed.head_gcode.begin(), feed.head_gcode.end(),
                               [](const std::string& g) { return !g.empty(); });
    return feed;
}

bool feeds_through_mount(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    return p && p->feeds_through_mount;
}

MaterialSource resolve_material_source(const PrinterDiscovery& hw) {
    const Provider* p = match(hw);
    if (!p || !p->material_write_gcode) {
        return {};
    }
    MaterialSource s;
    s.present = true;
    s.provider_name = p->name;
    s.write_gcode = p->material_write_gcode;
    return s;
}

std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    auto it = status.find(kZmodColorObject);
    if (it == status.end() || !it->is_object()) {
        return std::nullopt;
    }
    MaterialReading r;
    if (auto slots = zmod_color::parse_slots(*it, max_slots)) {
        std::vector<std::optional<SlotMaterial>> out(slots->size());
        for (size_t i = 0; i < slots->size(); ++i) {
            if (!(*slots)[i]) {
                continue;
            }
            SlotMaterial m;
            m.material = (*slots)[i]->material;
            m.rgb = parse_hex_color((*slots)[i]->hex);
            m.filament_present = (*slots)[i]->filament_present;
            out[i] = std::move(m);
        }
        r.slots = std::move(out);
    }
    r.valid_types = zmod_color::parse_valid_types(*it);
    r.palette = zmod_color::parse_palette(*it);
    if (!r.slots && !r.valid_types && !r.palette) {
        return std::nullopt;
    }
    return r;
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
    auto zmod = status.find(kZmodColorObject);
    if (zmod != status.end() && zmod->is_object()) {
        if (auto r = read_zmod_color(*zmod)) {
            return r;
        }
    }
    return std::nullopt;
}

bool sensor_error_is_fault(const ToolReading& reading, bool swap_in_flight) {
    if (!reading.sensor_error) {
        return false;
    }
    if (reading.sensor_error_reported) {
        const bool carriage_known = reading.current_tool && *reading.current_tool >= -1;
        return swap_in_flight || !carriage_known;
    }
    return !swap_in_flight;
}

} // namespace helix::toolchanger_addon

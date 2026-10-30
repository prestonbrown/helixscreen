// src/printer/ad5x_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ad5x_status_parse.h"

#include "helix_regex.h"
#include "json_utils.h"
#include "text_io.h"
#include "zmod_color_status.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace helix::ad5x {

using json = nlohmann::json;

ZColorSilentResult parse_zcolor_silent(const std::vector<std::string>& lines) {
    ZColorSilentResult result;

    // Summary: "// Extruder: None (N) | IFS: True"
    //   or:    "// Extruder: N: MATERIAL/HEX | IFS: True"
    static const helix::Regex summary_re(
        R"(^//\s*Extruder:\s*(.+?)\s*\|\s*IFS:\s*(True|False)\s*$)");
    // Slot: "// N: MATERIAL/HEX" or "// N: MATERIAL/NAME/HEX" or old "// N: MATERIAL"
    static const helix::Regex slot_re(R"(^//\s*([1-9])\s*:\s*(.+?)\s*$)");
    // Extruder detail inside summary text: "N: MATERIAL/..."
    static const helix::Regex extruder_slot_re(R"(^([1-9])\s*:)");
    // current channel: "None (N)" or bare "N" form — look for "(N)" paren form
    static const helix::Regex channel_paren_re(R"(\((\d+)\))");

    // First pass: classify the response and pull the IFS_STATUS JSON line.
    //
    // IFS_STATUS (zmod cmd_IFS_STATUS -> respond_info(json.dumps(get_values())))
    // arrives as one `// `-prefixed clean-JSON object containing "Chan" (1-based
    // seated port, 0 = none). It shares the response buffer with GET_ZCOLOR but
    // is emitted via respond_info, NOT a prompt dialog — so on old zmod where
    // GET_ZCOLOR degrades to an action:prompt_ dialog, the IFS_STATUS data is
    // still present and must NOT be discarded by the prompt early-out. We scan
    // every line here (no early return) and let apply_zcolor_result honor
    // ifs_chan even when is_prompt_fallback is set.
    for (const auto& raw : lines) {
        if (raw.find("action:prompt_") != std::string::npos) {
            result.is_prompt_fallback = true;
            continue;
        }
        if (result.ifs_chan.has_value() || raw.find("\"Chan\"") == std::string::npos) {
            continue;
        }
        // Strip the gcode-console "// " prefix before JSON parsing.
        std::string body = raw;
        const size_t slashes = body.find("//");
        if (slashes != std::string::npos) {
            body = body.substr(slashes + 2);
        }
        json obj = json::parse(body, nullptr, false);
        // Not valid JSON (malformed line, partial buffer) — ignore. Never
        // throw on a bad gcode-response line.
        if (!obj.is_discarded() && obj.is_object()) {
            read_ifs_status_object(obj, result);
            if (result.ifs_chan.has_value()) {
                // safe_int: a line like {"Chan":1,"State":null} must not throw.
                result.ifs_state = helix::json_util::safe_int(obj, "State", -1);
            }
        }
    }

    int slot_lines_seen = 0;
    int slot_lines_with_hex = 0;
    helix::RegexMatch m;

    for (const auto& line : lines) {
        if (helix::regex_match(line, m, summary_re)) {
            result.saw_valid_response = true;
            result.saw_silent_content = true;   // genuine GET_ZCOLOR summary line
            result.saw_extruder_summary = true; // carries the "Extruder:" head field
            result.ifs_active = (m[2].str() == "True");
            const std::string extruder_part = m[1].str();

            helix::RegexMatch em;
            if (helix::regex_search(extruder_part, em, extruder_slot_re)) {
                const auto n = helix::text_io::parse_leading<int>(em[1].str());
                if (n && *n >= 1 && *n <= kNumPorts) {
                    result.extruder_slot = *n - 1; // 0-based
                }
            }
            helix::RegexMatch cm;
            if (helix::regex_search(extruder_part, cm, channel_paren_re)) {
                const auto chan = helix::text_io::parse_leading<int>(cm[1].str());
                if (chan) {
                    result.current_channel = *chan;
                }
            }
            continue;
        }

        if (helix::regex_match(line, m, slot_re)) {
            const auto parsed_n = helix::text_io::parse_leading<int>(m[1].str());
            if (!parsed_n) {
                continue;
            }
            const int n = *parsed_n;
            if (n < 1 || n > kNumPorts) {
                continue; // slot number out of range — skip (e.g. "// 99: nonsense")
            }

            result.saw_valid_response = true;
            result.saw_silent_content = true; // genuine GET_ZCOLOR slot line
            slot_lines_seen++;

            // Slot line body is "MATERIAL" or "MATERIAL/HEX" or "MATERIAL/NAME/HEX".
            // Rule: material is everything before the first '/', hex is everything
            // after the LAST '/'. Anything between is a COLOR_MAPPING alias we ignore.
            const std::string rest = m[2].str();
            const size_t first_slash = rest.find('/');
            const size_t last_slash = rest.rfind('/');

            ZColorSlot slot;
            if (first_slash == std::string::npos) {
                // Old format: just material, no /HEX.
                slot.material = rest;
            } else {
                slot.material = rest.substr(0, first_slash);
                slot.hex = rest.substr(last_slash + 1);
                slot_lines_with_hex++;
            }
            result.slots[static_cast<size_t>(n - 1)] = std::move(slot);
        }
    }

    // Old format detection: we saw slot lines but none had a /HEX segment.
    if (slot_lines_seen > 0 && slot_lines_with_hex == 0) {
        result.is_old_format = true;
    }

    return result;
}

void read_ifs_status_object(const json& obj, ZColorSilentResult& result) {
    // Chan and Ports are read independently. The gcode path only ever calls
    // this on a line that already contains "Chan", but a subscription delivers
    // Moonraker's diff: a frame where only the silk sensors moved carries Ports
    // and no Chan, and requiring Chan would drop the presence update on the
    // floor.
    if (auto chan_it = obj.find("Chan"); chan_it != obj.end() && chan_it->is_number_integer()) {
        result.ifs_chan = chan_it->get<int>();
        result.saw_valid_response = true;
    }

    // Per-port presence: the RS-485 silk switch for port i+1, and the presence
    // authority in apply_zcolor_result — which is what makes empty-channel
    // resurrection from a stale ffmColor impossible (#981). All or nothing: one
    // non-boolean entry discards the array rather than inventing a value for
    // that port.
    auto ports_it = obj.find("Ports");
    if (ports_it == obj.end() || !ports_it->is_array() || ports_it->size() != kNumPorts) {
        return;
    }
    std::array<bool, kNumPorts> ports{};
    for (int p = 0; p < kNumPorts; ++p) {
        const auto& v = (*ports_it)[static_cast<size_t>(p)];
        if (!v.is_boolean()) {
            return;
        }
        ports[static_cast<size_t>(p)] = v.get<bool>();
    }
    result.ifs_ports = ports;
    result.saw_valid_response = true;
}

bool read_zmod_color_object(const json& obj, ZColorSilentResult& result,
                            std::optional<int>& channel) {
    // The bool behind the "IFS: True" field of the GET_ZCOLOR summary line.
    if (auto ifs_it = obj.find("ifs"); ifs_it != obj.end() && ifs_it->is_boolean()) {
        result.ifs_active = ifs_it->get<bool>();
        result.saw_valid_response = true;
    }
    // FFMInfo.channel, the same value parse_adventurer_json() reads. Handed
    // back rather than applied: adopting it as the seated lane is head-gated
    // (#1065 Bug 3) and that gate lives in the file path.
    if (auto chan_it = obj.find("channel"); chan_it != obj.end() && chan_it->is_number_integer()) {
        channel = chan_it->get<int>();
    }

    auto slots = helix::zmod_color::parse_slots(obj, kNumPorts);
    if (!slots) {
        return false;
    }
    bool saw_slot = false;
    for (size_t i = 0; i < slots->size(); ++i) {
        if (!(*slots)[i]) {
            continue;
        }
        ZColorSlot slot;
        slot.material = (*slots)[i]->material;
        slot.hex = (*slots)[i]->hex;
        result.slots[i] = std::move(slot);
        saw_slot = true;
    }
    if (saw_slot) {
        result.saw_valid_response = true;
        // Counts as silent content: a firmware that publishes these objects at
        // all is far past the prompt-only zmod the demotion in #981 exists for,
        // so a status frame must not leave GET_ZCOLOR marked unsupported.
        result.saw_silent_content = true;
    }
    return saw_slot;
}

std::string normalize_module_color_hex(const std::string& value) {
    // The module stores and reports colours '#'-prefixed ("#A03CF7", or the
    // "#RGB" short form the stock UI writes); colors_[] holds bare upper-case
    // 6-digit hex. Empty return = "not a colour" (null / empty / malformed),
    // which the apply path treats as no reading rather than as black.
    std::string digits = value;
    if (!digits.empty() && digits.front() == '#') {
        digits.erase(0, 1);
    }
    if (digits.size() == 3) {
        std::string expanded;
        expanded.reserve(6);
        for (char c : digits) {
            expanded.push_back(c);
            expanded.push_back(c);
        }
        digits = std::move(expanded);
    }
    if (digits.size() != 6) {
        return {};
    }
    for (char c : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return {};
        }
    }
    return helix::text_io::to_upper(digits);
}

bool read_ifs_materials_object(const json& obj, ZColorSilentResult& result) {
    auto slots_it = obj.find("slots");
    if (slots_it == obj.end() || !slots_it->is_object()) {
        return false;
    }
    bool saw_slot = false;
    for (auto slot_it = slots_it->begin(); slot_it != slots_it->end(); ++slot_it) {
        // Moonraker serialises dict keys as strings, so "1".."4" - but accept
        // an integer key too, same tolerance helix::zmod_color::parse_slots
        // gives the ID field (it is somebody else's serializer either way).
        const auto id_number = helix::text_io::parse_leading<int>(slot_it.key());
        if (!id_number) {
            continue;
        }
        const int id = *id_number;
        if (id < 1 || id > kNumPorts || !slot_it.value().is_object()) {
            continue;
        }
        const auto& entry = slot_it.value();
        ZColorSlot slot;
        if (auto type = entry.find("type"); type != entry.end() && type->is_string()) {
            slot.material = type->get<std::string>();
            // The stock-UI unset sentinel, same one the zmod_color reader and
            // parse_adventurer_json normalise - a live "?" must never render.
            if (slot.material == "?") {
                slot.material.clear();
            }
        }
        if (auto color = entry.find("color"); color != entry.end() && color->is_string()) {
            slot.hex = normalize_module_color_hex(color->get<std::string>());
        }
        result.slots[static_cast<size_t>(id - 1)] = std::move(slot);
        saw_slot = true;
    }
    if (saw_slot) {
        result.saw_valid_response = true;
        // Same reasoning as the zmod_color reader: a firmware publishing these
        // objects has no prompt-only degradation path to protect against.
        result.saw_silent_content = true;
    }
    return saw_slot;
}

IfsModuleObject parse_ifs_module_object(const json& obj) {
    IfsModuleObject out;
    // loaded_channels: 1-based channel ints decoded from the same RS-485 silk
    // bitmask the per-lane sensors and zmod's Ports array carry. All or
    // nothing across the array - one non-integer entry discards the presence
    // view rather than inventing values.
    if (auto lc = obj.find("loaded_channels"); lc != obj.end() && lc->is_array()) {
        std::array<bool, kNumPorts> ports{};
        bool usable = true;
        for (const auto& ch : *lc) {
            if (!ch.is_number_integer()) {
                usable = false;
                break;
            }
            const int c = ch.get<int>();
            if (c >= 1 && c <= kNumPorts) {
                ports[static_cast<size_t>(c - 1)] = true;
            }
        }
        if (usable) {
            out.loaded_ports = ports;
        }
    }
    if (auto ac = obj.find("active_channel"); ac != obj.end() && ac->is_number_integer()) {
        out.active_channel = ac->get<int>();
    }
    if (auto a = obj.find("activity"); a != obj.end() && a->is_string()) {
        out.activity = a->get<std::string>();
    }
    if (auto e = obj.find("error"); e != obj.end() && e->is_string()) {
        out.error = e->get<std::string>();
    }
    return out;
}

namespace {

std::optional<bool> filament_detected(const json& status, const char* key) {
    const auto it = status.find(key);
    if (it == status.end() || !it->is_object()) {
        return std::nullopt;
    }
    const auto fd = it->find("filament_detected");
    if (fd == it->end() || !fd->is_boolean()) {
        return std::nullopt;
    }
    return fd->get<bool>();
}

std::optional<bool> first_reading(const json& status, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (auto v = filament_detected(status, key)) {
            return v;
        }
    }
    return std::nullopt;
}

} // namespace

SensorFrame parse_sensor_frame(const json& status) {
    SensorFrame f;
    // Leading space in sensor name is intentional — Klipper object naming convention
    for (int port = 1; port <= kNumPorts; ++port) {
        const std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port);
        f.port_detected[static_cast<size_t>(port - 1)] = filament_detected(status, key.c_str());
    }
    f.motion_detected = first_reading(status, {"filament_motion_sensor ifs_motion_sensor",
                                               "zmod_ifs_motion_sensor ifs_motion_sensor"});
    f.head_detected = first_reading(status, {"filament_switch_sensor head_switch_sensor",
                                             "zmod_ifs_switch_sensor head_switch_sensor",
                                             "filament_switch_sensor toolhead"});
    if (const auto extr = status.find("extruder"); extr != status.end()) {
        f.has_extruder = true;
        if (extr->is_object()) {
            if (auto t = extr->find("target"); t != extr->end() && t->is_number()) {
                f.extruder_target = t->get<double>();
            }
            if (auto t = extr->find("temperature"); t != extr->end() && t->is_number()) {
                f.extruder_temperature = t->get<double>();
            }
        }
    }
    return f;
}

SaveVariables parse_save_variables(const json& vars, const std::string& known_prefix) {
    SaveVariables out;
    if (vars.contains("bambufy_colors") || vars.contains("bambufy_tools")) {
        out.prefix = "bambufy";
    } else if (vars.contains("less_waste_colors") || vars.contains("less_waste_tools")) {
        out.prefix = "less_waste";
    }

    if (auto it = vars.find("bambufy_custom_types"); it != vars.end() && it->is_array()) {
        std::vector<std::string> names;
        for (const auto& t : *it) {
            if (t.is_string() && !t.get_ref<const std::string&>().empty()) {
                names.push_back(t.get<std::string>());
            }
        }
        out.custom_types = std::move(names);
    }

    if (auto it = vars.find("ifs_at_hub"); it != vars.end() && it->is_number_integer()) {
        out.module_at_hub = it->get<int>();
    }
    if (auto it = vars.find("ifs_loaded"); it != vars.end() && it->is_number_integer()) {
        const int loaded = it->get<int>();
        out.module_loaded = (loaded >= 1 && loaded <= kNumPorts) ? loaded : 0;
    }

    const std::string p = out.prefix.value_or(known_prefix);
    const std::string other_p = (p == "bambufy") ? "less_waste" : "bambufy";
    const auto self_tools = vars.find(p + "_tools");
    const auto other_tools = vars.find(other_p + "_tools");
    const bool have_self_tools = self_tools != vars.end() && self_tools->is_array();
    const bool have_other_tools = other_tools != vars.end() && other_tools->is_array();
    out.tools_conflict = have_self_tools && have_other_tools && *self_tools != *other_tools;

    if (have_self_tools) {
        const size_t n = std::min(self_tools->size(), static_cast<size_t>(kToolMapSize));
        for (size_t i = 0; i < n; ++i) {
            if ((*self_tools)[i].is_number_integer()) {
                out.tools[i] = (*self_tools)[i].get<int>();
            }
        }
    }

    // lessWaste keeps these kToolMapSize-entry and tool-indexed; bambufy's
    // legitimately hold 4 entries, and a non-array row is not ours to judge.
    if (p == "less_waste") {
        for (const std::string key : {p + "_colors", p + "_types"}) {
            const auto it = vars.find(key);
            if (it != vars.end() && it->is_array() &&
                it->size() < static_cast<size_t>(kToolMapSize)) {
                out.short_tool_list = std::make_pair(key, it->size());
                break;
            }
        }
    }

    if (auto it = vars.find(p + "_current_tool"); it != vars.end() && it->is_number_integer()) {
        out.current_tool = it->get<int>();
    }
    if (auto it = vars.find(p + "_external"); it != vars.end() && it->is_number_integer()) {
        out.external = it->get<int>() != 0;
    }
    return out;
}

std::optional<IfsVarsMacro> parse_ifs_vars_macro(const json& macro_status) {
    if (!macro_status.is_object() || macro_status.empty()) {
        return std::nullopt;
    }
    const auto boolish = [&](const char* key) -> std::optional<bool> {
        const auto it = macro_status.find(key);
        if (it == macro_status.end()) {
            return std::nullopt;
        }
        if (it->is_boolean()) {
            return it->get<bool>();
        }
        if (it->is_number_integer()) {
            return it->get<int>() != 0;
        }
        return std::nullopt;
    };
    IfsVarsMacro out;
    out.backup = boolish("variable_backup");
    out.unlock_after_boot = boolish("variable_ifs_unlock_after_boot");
    return out;
}

std::optional<FilamentEjectTable> parse_filament_json(const std::string& content) {
    // filament.json is a JSON object keyed by filament TYPE name plus a
    // "default" entry. Each value carries filament_tube_length (the unload
    // retract distance, our IFS_F11 LEN) and filament_ifs_speed (IFS_F11
    // SPEED), among many fields we don't use.
    json root = json::parse(content, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return std::nullopt;
    }

    // int field reader with a fallback, tolerant of numeric-as-string values.
    auto read_int = [](const json& obj, const char* key, int fallback) -> int {
        auto it = obj.find(key);
        if (it == obj.end())
            return fallback;
        if (it->is_number_integer() || it->is_number_unsigned())
            return it->get<int>();
        if (it->is_number_float())
            return static_cast<int>(it->get<double>());
        if (it->is_string()) {
            return helix::text_io::parse_leading<int>(it->get_ref<const std::string&>())
                .value_or(fallback);
        }
        return fallback;
    };

    FilamentEjectTable table;
    auto& def = table.default_params;
    if (auto def_it = root.find("default"); def_it != root.end() && def_it->is_object()) {
        def = {read_int(*def_it, "filament_tube_length", 1000),
               read_int(*def_it, "filament_ifs_speed", 1200)};
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() == "default" || !it.value().is_object())
            continue;
        table.materials[it.key()] = {read_int(it.value(), "filament_tube_length", def.first),
                                     read_int(it.value(), "filament_ifs_speed", def.second)};
    }
    return table;
}

std::optional<AdventurerFile> parse_adventurer_json(const std::string& content) {
    json doc = json::parse(content, nullptr, false);
    if (doc.is_discarded()) {
        return std::nullopt;
    }
    AdventurerFile file;
    auto ffm_it = doc.find("FFMInfo");
    if (ffm_it == doc.end() || !ffm_it->is_object()) {
        return file;
    }
    const auto& ffm = *ffm_it;
    file.has_ffm = true;

    for (int port = 1; port <= kNumPorts; ++port) {
        auto color_it = ffm.find("ffmColor" + std::to_string(port));
        auto type_it = ffm.find("ffmType" + std::to_string(port));
        if (color_it == ffm.end() && type_it == ffm.end())
            continue;

        FfmPort entry;
        if (color_it != ffm.end() && color_it->is_string()) {
            entry.hex = color_it->get<std::string>();
        }
        if (!entry.hex.empty() && entry.hex[0] == '#') {
            entry.hex = entry.hex.substr(1);
        }
        entry.has_color = !entry.hex.empty();
        if (entry.hex.empty()) {
            entry.hex = "808080";
        }
        for (auto& c : entry.hex)
            c = static_cast<char>(toupper(c));

        if (type_it != ffm.end() && type_it->is_string()) {
            entry.type = type_it->get<std::string>();
        }
        // Firmware-native unset sentinel: live stock ZMOD writes ffmType='?'
        // for an entry with no assigned material.
        if (entry.type == "?")
            entry.type.clear();
        file.ports[static_cast<size_t>(port - 1)] = std::move(entry);
    }

    if (auto chan_it = ffm.find("channel"); chan_it != ffm.end() && chan_it->is_number_integer()) {
        file.channel = chan_it->get<int>();
    }
    return file;
}

std::vector<std::string> parse_user_cfg_filament_types(const std::string& body) {
    // zmod docs: https://wiki.zmod.link/AD5X/#7-add-custom-filament-types
    //
    //   [zmod_ifs]
    //   filament_NEWTYPE: 300
    //
    // Section is Klipper-style INI. Comments start with '#' or ';'. Values
    // are decimal temperatures we don't currently use — we only collect the
    // NAME tokens (uppercased convention, but preserve user case as written).
    // Any whitespace before the section header or key is tolerated; we don't
    // try to fully reimplement Klipper's INI parser, just match the lines we
    // care about within the [zmod_ifs] section.
    std::vector<std::string> out;
    bool in_section = false;
    static const helix::Regex section_re(R"(^\s*\[\s*([^\]\s]+)\s*\]\s*$)");
    static const helix::Regex filament_re(R"(^\s*filament_([A-Za-z0-9_+\-]+)\s*[:=].*$)");
    for (std::string_view line_view : helix::text_io::lines(body)) {
        std::string line(line_view);
        // Strip trailing CR for files saved with CRLF.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Strip inline comments — Klipper accepts both '#' and ';'.
        for (char ch : {'#', ';'}) {
            auto pos = line.find(ch);
            if (pos != std::string::npos) {
                line.erase(pos);
                break;
            }
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        helix::RegexMatch m;
        if (helix::regex_match(line, m, section_re)) {
            in_section = (m[1].str() == "zmod_ifs");
            continue;
        }
        if (in_section && helix::regex_match(line, m, filament_re)) {
            out.push_back(m[1].str());
        }
    }
    return out;
}

} // namespace helix::ad5x

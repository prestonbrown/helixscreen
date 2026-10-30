// src/printer/qidi_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qidi_status_parse.h"

#include "color_utils.h"
#include "lane_translation.h"
#include "text_io.h"

#include <algorithm>
#include <cctype>

namespace helix::qidi {

namespace tio = helix::text_io;
using json = nlohmann::json;

namespace {

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// Parse `"slot<N>"` into N when valid and within [0, slot_count).
// Returns nullopt for the box_extras.py sentinel `"slot-1"` (nothing
// loaded) and for any other malformed input. Used to decode the
// `value_t<T>` and `last_load_slot` save_variables, both of which carry
// slot references in this format.
std::optional<int> parse_slot_name(const std::string& val, int slot_count) {
    if (val.rfind("slot", 0) != 0) {
        return std::nullopt;
    }
    // A malformed slot suffix reads as no slot at all.
    const auto idx = tio::parse_leading<int>(val.substr(4));
    if (idx && *idx >= 0 && *idx < slot_count) {
        return idx;
    }
    return std::nullopt;
}

// Read a slot's palette row id (filament_slot<N> / color_slot<N> /
// vendor_slot<N>) from a save_variables value. The firmware and HelixScreen
// write bare ints, but a SAVE_VARIABLE spelled with a quoted VALUE - Qidi
// Studio emits VALUE="5" - is stored as a string, because save_variables.py
// evaluates the literal before saving it. Accept a non-negative integer, or a
// string holding nothing but a non-negative integer (surrounding whitespace
// tolerated) - zero IS an id: it is the Box stating the field is unset.
std::optional<int> read_slot_id(const json& val) {
    if (val.is_number_integer()) {
        const int id = val.get<int>();
        // A negative id has no palette row; the digits-only string branch
        // already refuses one, so the two spellings of the same write agree.
        if (id >= 0) {
            return id;
        }
    }
    if (val.is_string()) {
        const std::string s = trim(val.get<std::string>());
        if (is_all_digits(s)) {
            // More digits than an int holds - no palette row is that big.
            if (const auto id = tio::parse_leading<int>(s)) {
                return id;
            }
        }
    }
    return std::nullopt;
}

// A color_slot<N> colour list (`['228332']`, bare RGB hex). True when @p val
// is a list at all; @p out is its first entry's RGB, or empty for an empty
// list or one whose first entry is not a colour.
bool read_color_list(const json& val, std::optional<std::uint32_t>& out) {
    if (!val.is_array()) {
        return false;
    }
    out.reset();
    if (!val.empty() && val.front().is_string()) {
        out = helix::parse_hex_color(val.front().get<std::string>());
    }
    return true;
}

} // namespace

bool is_all_digits(std::string_view s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::optional<std::string> read_filament_code(const json& val) {
    if (!val.is_string()) {
        return std::nullopt;
    }
    const std::string s = trim(val.get<std::string>());
    const auto first_digit =
        std::find_if(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    const auto letters = static_cast<size_t>(first_digit - s.begin());
    const size_t digits = s.size() - letters;
    const bool letters_ok =
        letters >= 2 && letters <= 4 &&
        std::all_of(s.begin(), first_digit, [](unsigned char c) { return std::isupper(c) != 0; });
    const bool digits_ok =
        digits >= 2 && digits <= 3 && is_all_digits(std::string_view(s).substr(letters));
    if (!letters_ok || !digits_ok) {
        return std::nullopt;
    }
    return s;
}

SaveVariables parse_save_variables(const json& variables, int known_slot_count) {
    SaveVariables out;
    if (!variables.is_object()) {
        return out;
    }
    if (auto it = variables.find("box_count"); it != variables.end() && it->is_number_integer()) {
        const int box_count = it->get<int>();
        if (box_count >= 0 && box_count <= kMaxBoxes) {
            out.box_count = box_count;
        }
    }
    if (auto it = variables.find("enable_box"); it != variables.end() && it->is_number_integer()) {
        out.enable_box = it->get<int>() != 0;
    }

    const int slot_count =
        std::min(out.box_count ? *out.box_count * kSlotsPerBox : known_slot_count, kMaxSlots);

    // value_t<N> = "slot<M>" — tool N prints from global slot M.
    for (int t = 0; t < slot_count; ++t) {
        const auto it = variables.find("value_t" + std::to_string(t));
        if (it == variables.end() || !it->is_string()) {
            continue;
        }
        if (auto idx = parse_slot_name(it->get<std::string>(), slot_count)) {
            out.tool_slots.emplace_back(t, *idx);
        }
    }

    const auto tool_change_it = variables.find("is_tool_change");
    out.has_slot_or_action_key = tool_change_it != variables.end();
    out.tool_change = tool_change_it != variables.end() && tool_change_it->is_number_integer() &&
                      tool_change_it->get<int>() != 0;

    for (int i = 0; i < slot_count; ++i) {
        const std::string suffix = std::to_string(i);
        const auto slot_it = variables.find("slot" + suffix);
        if (slot_it != variables.end()) {
            out.has_slot_or_action_key = true;
            if (slot_it->is_number_integer()) {
                out.slot_state[static_cast<size_t>(i)] = slot_it->get<int>();
            }
        }

        auto& ids = out.ids[static_cast<size_t>(i)];
        const auto read_id = [&](const std::string& key) -> std::optional<int> {
            const auto it = variables.find(key);
            if (it == variables.end()) {
                return std::nullopt;
            }
            auto id = read_slot_id(*it);
            if (!id) {
                out.ignored_id_keys.push_back(key);
            }
            return id;
        };
        if (const auto it = variables.find("filament_slot" + suffix); it != variables.end()) {
            if (auto code = read_filament_code(*it)) {
                ids.filament_code = std::move(*code);
            } else {
                ids.filament_id = read_id("filament_slot" + suffix);
            }
        }
        if (const auto it = variables.find("color_slot" + suffix); it != variables.end()) {
            if (read_color_list(*it, ids.color_rgb)) {
                ids.color_list = true;
            } else {
                ids.color_id = read_id("color_slot" + suffix);
            }
        }
        ids.vendor_id = read_id("vendor_slot" + suffix);
    }

    if (auto it = variables.find("last_load_slot"); it != variables.end() && it->is_string()) {
        const std::string val = it->get<std::string>();
        if (val == "slot-1") {
            out.last_load_slot = -1;
        } else if (auto idx = parse_slot_name(val, slot_count)) {
            out.last_load_slot = *idx;
        }
    }
    return out;
}

std::map<int, ControllerSlotDelta> parse_controller_status(const json& controller) {
    std::map<int, ControllerSlotDelta> out;
    const auto box_it = controller.find("box");
    if (box_it == controller.end() || !box_it->is_object()) {
        return out;
    }
    constexpr std::string_view kInfo = "_filament_info_";
    for (auto it = box_it->begin(); it != box_it->end(); ++it) {
        // box<B>_filament_info_<I>_<field>: Box B+1, slot I within it.
        const std::string& key = it.key();
        if (key.rfind("box", 0) != 0) {
            continue;
        }
        const size_t info_pos = key.find(kInfo);
        if (info_pos == std::string::npos) {
            continue;
        }
        const std::string_view box_digits(key.data() + 3, info_pos - 3);
        const size_t slot_pos = info_pos + kInfo.size();
        const size_t field_sep = key.find('_', slot_pos);
        if (field_sep == std::string::npos || !is_all_digits(box_digits)) {
            continue;
        }
        const std::string_view slot_digits(key.data() + slot_pos, field_sep - slot_pos);
        if (!is_all_digits(slot_digits)) {
            continue;
        }
        const int box = tio::parse_leading<int>(std::string(box_digits)).value_or(-1);
        const int slot_in_box = tio::parse_leading<int>(std::string(slot_digits)).value_or(-1);
        if (box < 0 || box >= kMaxBoxes || slot_in_box < 0 || slot_in_box >= kSlotsPerBox) {
            continue;
        }
        ControllerSlotDelta& d = out[box * kSlotsPerBox + slot_in_box];
        const std::string_view field(key.data() + field_sep + 1, key.size() - field_sep - 1);
        const auto& val = it.value();
        if (field == "filament_id" && val.is_string()) {
            d.filament_id = read_filament_code(val).value_or(std::string{});
        } else if (field == "filament_type" && val.is_string()) {
            d.filament_type = val.get<std::string>();
        } else if (field == "vendor_str" && val.is_string()) {
            d.vendor = val.get<std::string>();
        } else if (field == "min_temp" && val.is_number()) {
            d.min_temp = static_cast<int>(val.get<double>());
        } else if (field == "max_temp" && val.is_number()) {
            d.max_temp = static_cast<int>(val.get<double>());
        }
    }
    return out;
}

std::optional<int> parse_box_unit_index(const std::string& key) {
    const size_t pos = key.rfind("box");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto box = tio::parse_leading<int>(key.substr(pos + 3));
    if (box && *box >= 1 && *box <= kMaxBoxes) {
        return *box - 1;
    }
    return std::nullopt;
}

std::optional<std::vector<std::pair<int, std::int64_t>>>
parse_box_drying_state(const json& box_extras) {
    auto ds_it = box_extras.find("box_drying_state");
    if (ds_it == box_extras.end() || !ds_it->is_object()) {
        return std::nullopt;
    }
    std::vector<std::pair<int, std::int64_t>> out;
    for (auto it = ds_it->begin(); it != ds_it->end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const auto unit_index = parse_box_unit_index(it.key()); // "box1" -> 0
        if (!unit_index) {
            continue;
        }
        std::int64_t end = 0;
        if (auto et = it->find("end_time"); et != it->end() && et->is_number()) {
            end = et->get<std::int64_t>();
        }
        out.emplace_back(*unit_index, end);
    }
    return out;
}

ConfigSettings parse_config_settings(const json& settings) {
    ConfigSettings out;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        const std::string& key = it.key();
        // Box-controller dialect marker: a "[multi_color_controller]" section
        // (bare or instanced, e.g. "multi_color_controller box0"). The Max 4 ships
        // it, and so does Q2 firmware 01.01.02.x; Q2 1.1.x and the Plus 4 do not. #1083
        if (key == "multi_color_controller" || key.rfind("multi_color_controller ", 0) == 0) {
            out.has_multi_color = true;
        }
        if (!it->is_object()) {
            continue;
        }
        if (key.rfind("heater_generic heater_box", 0) == 0) {
            if (auto m = it->find("max_temp"); m != it->end() && m->is_number()) {
                out.dryer_max_temp = m->get<float>();
            }
        } else if (key.rfind("box_config box", 0) == 0) {
            if (auto m = it->find("target_max_temp_heater_generic");
                m != it->end() && m->is_number()) {
                out.dryer_max_temp = m->get<float>();
            }
        }
    }
    if (auto fm = settings.find("force_move"); fm != settings.end() && fm->is_object()) {
        if (auto en = fm->find("enable_force_move"); en != fm->end() && en->is_boolean()) {
            out.force_move = en->get<bool>();
        }
    }
    return out;
}

std::array<BoxReading, kMaxBoxes> parse_heater_status(const json& notification) {
    constexpr std::string_view kHeaterPrefix = "heater_generic heater_box";
    constexpr std::string_view kAht20Prefix = "aht20_f heater_box";
    constexpr std::string_view kBoxTempSensorPrefix = "temperature_sensor heater_temp_";

    std::array<BoxReading, kMaxBoxes> readings;
    const auto raise = [](std::optional<float>& slot, const json& obj, const char* field) {
        if (auto f = obj.find(field); f != obj.end() && f->is_number()) {
            const float v = f->get<float>();
            if (!slot || v > *slot) {
                slot = v;
            }
        }
    };
    for (auto it = notification.begin(); it != notification.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const std::string& key = it.key();
        const bool is_heater = key.rfind(kHeaterPrefix, 0) == 0;
        const bool is_aht = key.rfind(kAht20Prefix, 0) == 0;
        const bool is_box_temp_sensor =
            key.rfind(kBoxTempSensorPrefix, 0) == 0 && key.find("_box") != std::string::npos;
        if (!is_heater && !is_aht && !is_box_temp_sensor) {
            continue;
        }
        const auto unit_index = parse_box_unit_index(key);
        if (!unit_index) {
            continue;
        }
        auto& reading = readings[static_cast<size_t>(*unit_index)];
        if (is_heater || is_aht) {
            raise(reading.temp, *it, "temperature");
        }
        if (is_heater) {
            raise(reading.target, *it, "target");
        }
        raise(reading.humidity, *it, "humidity");
    }
    return readings;
}

FilasList parse_filas_list(const std::string& content) {
    // Minimal ConfigParser-compatible INI: `[section]` headers, `key = value`
    // lines, `#` or `;` comments. Three section kinds are honoured:
    //   [fila<N>]     → FilaProfile (name, type, nozzle + box temps)
    //   [colordict]   → integer id → 0xRRGGBB
    //   [vendor_list] → integer id → vendor name
    // Everything else is silently ignored. Trailing `#`/`;` tail content on a
    // value is dropped.
    //
    // Drop inline `;` / `#` tail then trim. ConfigParser keeps `#` inside a
    // value unless preceded by whitespace, but the stock file never relies on
    // that, so the simpler "strip at first comment char" rule is fine here.
    auto strip_tail = [](const std::string& v) {
        std::string body = v;
        for (char ch : {';', '#'}) {
            // A leading '#' is a color literal, not a comment — only strip a
            // '#' that is not at the very start of the trimmed value.
            auto pos = body.find(ch);
            if (ch == '#' && pos == 0) {
                continue;
            }
            if (pos != std::string::npos) {
                body.erase(pos);
            }
        }
        return trim(body);
    };
    auto parse_int_field = [&](const std::string& v, int& out) {
        if (const auto parsed = tio::parse_leading<int>(strip_tail(v))) {
            out = *parsed;
        }
    };
    enum class Section { None, Fila, Color, Vendor };

    FilasList out;
    Section section = Section::None;
    std::optional<int> current_id;
    FilaProfile current;

    auto flush_fila = [&]() {
        if (section == Section::Fila && current_id) {
            out.profiles[*current_id] = current;
        }
        current_id.reset();
        current = FilaProfile{};
    };

    for (std::string_view sv : tio::lines(content)) {
        std::string t = trim(std::string(sv));
        if (t.empty() || t.front() == '#' || t.front() == ';') {
            continue;
        }
        if (t.front() == '[' && t.back() == ']') {
            flush_fila();
            std::string name = trim(t.substr(1, t.size() - 2));
            if (name == "colordict") {
                section = Section::Color;
            } else if (name == "vendor_list") {
                section = Section::Vendor;
            } else if (name.rfind("fila", 0) == 0) {
                section = Section::Fila;
                const auto id = tio::parse_leading<int>(name.substr(4));
                if (!id) {
                    // Malformed `fila<N>` — treat as no current section.
                    section = Section::None;
                } else if (*id > 0) {
                    current_id = *id;
                }
            } else {
                section = Section::None;
            }
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string val = t.substr(eq + 1);

        switch (section) {
        case Section::Fila: {
            if (!current_id) {
                break;
            }
            if (key == "filament") {
                current.name = strip_tail(val);
            } else if (key == "type") {
                current.type = strip_tail(val);
            } else if (key == "min_temp") {
                parse_int_field(val, current.nozzle_min);
            } else if (key == "max_temp") {
                parse_int_field(val, current.nozzle_max);
            } else if (key == "box_min_temp") {
                parse_int_field(val, current.box_min);
            } else if (key == "box_max_temp") {
                parse_int_field(val, current.box_max);
            }
            break;
        }
        case Section::Color: {
            // A non-integer key in [colordict] names no row to store.
            if (const auto id = tio::parse_leading<int>(trim(key))) {
                // A colordict row is a lane-shaped colour value like any other
                // producer's, so the tree's one hex grammar reads it. Only a row
                // stating a colour becomes a palette entry; a blank row states
                // that the id names none.
                const auto reading = helix::ams::read_lane_color(val);
                if (reading.kind == helix::ams::ColorReadingKind::Observed) {
                    out.colors[*id] = reading.rgb;
                }
            }
            break;
        }
        case Section::Vendor: {
            // A non-integer key in [vendor_list] names no row to store.
            if (const auto id = tio::parse_leading<int>(trim(key))) {
                out.vendors[*id] = strip_tail(val);
            }
            break;
        }
        case Section::None:
            break;
        }
    }
    flush_fila();
    return out;
}

} // namespace helix::qidi

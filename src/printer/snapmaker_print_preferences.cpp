// src/printer/snapmaker_print_preferences.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_print_preferences.h"

#include "hv/json.hpp"

namespace helix::snapmaker {
namespace {

/// A firmware storing a flag as a Python bool serialises true/false; one
/// storing it as an int serialises 0/1. Anything else is not a setting we can
/// read, and is left unset rather than guessed at.
std::optional<bool> read_flag(const nlohmann::json& obj, const char* key) {
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

/// A per-toolhead list of flags, in either spelling; empty when absent.
std::vector<bool> read_flag_list(const nlohmann::json& ptc, const char* key) {
    std::vector<bool> out;
    if (auto it = ptc.find(key); it != ptc.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_boolean()) {
                out.push_back(v.get<bool>());
            } else if (v.is_number_integer()) {
                out.push_back(v.get<int>() != 0);
            }
        }
    }
    return out;
}

void append(std::string& out, const char* name, const std::string& value) {
    out += (out.empty() ? "SET_PRINT_PREFERENCES " : " ");
    out += name;
    out += '=';
    out += value;
}

} // namespace

bool PrintPreferences::empty() const {
    return !auto_replenish && !replenish_ignore_color && !filament_entangle_detect &&
           !end_led_turn_off && !filament_entangle_sen && end_unload_filament.empty();
}

void PrintPreferences::merge(const PrintPreferences& incoming) {
    if (incoming.auto_replenish) {
        auto_replenish = incoming.auto_replenish;
    }
    if (incoming.replenish_ignore_color) {
        replenish_ignore_color = incoming.replenish_ignore_color;
    }
    if (incoming.filament_entangle_detect) {
        filament_entangle_detect = incoming.filament_entangle_detect;
    }
    if (incoming.end_led_turn_off) {
        end_led_turn_off = incoming.end_led_turn_off;
    }
    if (incoming.filament_entangle_sen) {
        filament_entangle_sen = incoming.filament_entangle_sen;
    }
    if (!incoming.end_unload_filament.empty()) {
        end_unload_filament = incoming.end_unload_filament;
    }
    if (!incoming.filament_soft.empty()) {
        filament_soft = incoming.filament_soft;
    }
}

PrintPreferences read_print_preferences(const nlohmann::json& status) {
    PrintPreferences p;
    if (!status.is_object()) {
        return p;
    }
    auto ptc = status.find("print_task_config");
    if (ptc == status.end() || !ptc->is_object()) {
        return p;
    }
    p.auto_replenish = read_flag(*ptc, "auto_replenish_filament");
    p.replenish_ignore_color = read_flag(*ptc, "replenish_ignore_color");
    p.filament_entangle_detect = read_flag(*ptc, "filament_entangle_detect");
    p.end_led_turn_off = read_flag(*ptc, "end_led_turn_off");

    if (auto it = ptc->find("filament_entangle_sen"); it != ptc->end() && it->is_string()) {
        p.filament_entangle_sen = it->get<std::string>();
    }
    p.end_unload_filament = read_flag_list(*ptc, "end_unload_filament");
    p.filament_soft = read_flag_list(*ptc, "filament_soft");
    return p;
}

std::string write_print_preferences_gcode(const PrintPreferences& changes) {
    std::string out;
    if (changes.auto_replenish) {
        append(out, "AUTO_REPLENISH_FILAMENT", *changes.auto_replenish ? "1" : "0");
    }
    if (changes.replenish_ignore_color) {
        append(out, "REPLENISH_IGNORE_COLOR", *changes.replenish_ignore_color ? "1" : "0");
    }
    if (changes.filament_entangle_detect) {
        append(out, "FILAMENT_ENTANGLE_DETECT", *changes.filament_entangle_detect ? "1" : "0");
    }
    if (changes.end_led_turn_off) {
        append(out, "END_LED_TURN_OFF", *changes.end_led_turn_off ? "1" : "0");
    }
    if (changes.filament_entangle_sen) {
        append(out, "FILAMENT_ENTANGLE_SEN", *changes.filament_entangle_sen);
    }
    if (!changes.end_unload_filament.empty()) {
        // A Python list literal: the firmware parses the value with
        // ast.literal_eval and rejects anything that is not a list, and
        // Klipper splits gcode parameters on whitespace, so the value
        // carries no spaces.
        std::string list_literal = "[";
        for (size_t i = 0; i < changes.end_unload_filament.size(); ++i) {
            list_literal += (i ? "," : "");
            list_literal += changes.end_unload_filament[i] ? "1" : "0";
        }
        list_literal += "]";
        append(out, "END_UNLOAD_FILAMENT", list_literal);
    }
    if (changes.force && !out.empty()) {
        append(out, "FORCE", "1");
    }
    return out;
}

} // namespace helix::snapmaker

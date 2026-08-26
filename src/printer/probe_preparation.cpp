// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe_preparation.h"

#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace helix::probe_prep {

namespace {

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

/**
 * Evaluate one `when` entry. Returns false for anything this build does not
 * understand — an unrecognised predicate must never read as "no objection",
 * because a rule firing on the wrong printer sends g-code to real hardware.
 */
bool predicate_holds(const nlohmann::json& pred,
                     const std::unordered_set<std::string>& macros_upper) {
    if (!pred.is_object()) {
        return false;
    }
    const std::string type = pred.value("type", "");
    const std::string pattern = pred.value("pattern", "");
    if (pattern.empty()) {
        return false;
    }

    if (type == "macro_match") {
        return macros_upper.count(to_upper(pattern)) > 0;
    }

    spdlog::debug("[ProbePrep] Unknown predicate type '{}' — rule cannot match", type);
    return false;
}

/// Read `gcode` as either a string or an array of strings. Empty on any other
/// shape, which makes the rule a non-match.
std::string read_gcode(const nlohmann::json& rule) {
    const auto it = rule.find("gcode");
    if (it == rule.end()) {
        return "";
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (!it->is_array()) {
        return "";
    }
    std::string joined;
    for (const auto& line : *it) {
        if (!line.is_string()) {
            return "";
        }
        if (!joined.empty()) {
            joined += "\n";
        }
        joined += line.get<std::string>();
    }
    return joined;
}

bool lists_operation(const nlohmann::json& rule, Operation op) {
    const auto it = rule.find("operations");
    if (it == rule.end() || !it->is_array()) {
        return false;
    }
    const std::string key = operation_key(op);
    return std::any_of(it->begin(), it->end(), [&](const nlohmann::json& entry) {
        return entry.is_string() && entry.get<std::string>() == key;
    });
}

bool suppressed_by_resolved_macro(const nlohmann::json& rule, const std::string& resolved_macro) {
    if (resolved_macro.empty()) {
        return false;
    }
    const auto it = rule.find("skip_if_macro_in");
    if (it == rule.end() || !it->is_array()) {
        return false;
    }
    const std::string wanted = to_upper(resolved_macro);
    return std::any_of(it->begin(), it->end(), [&](const nlohmann::json& entry) {
        return entry.is_string() && to_upper(entry.get<std::string>()) == wanted;
    });
}

} // namespace

const char* operation_key(Operation op) {
    switch (op) {
    case Operation::ScrewsTilt:
        return "screws_tilt";
    case Operation::BedMesh:
        return "bed_mesh";
    case Operation::ProbeAccuracy:
        return "probe_accuracy";
    case Operation::ZOffsetCalibrate:
        return "z_offset_calibrate";
    }
    return "";
}

Preparation resolve_from_rules(const nlohmann::json& rules,
                               const std::unordered_set<std::string>& macros_upper, Operation op,
                               const std::string& resolved_macro) {
    if (!rules.is_array()) {
        return {};
    }

    for (const auto& rule : rules) {
        if (!rule.is_object()) {
            continue;
        }
        if (!rule.value("enabled", true)) {
            continue;
        }
        if (!lists_operation(rule, op)) {
            continue;
        }

        // Fail closed: a rule with no predicates does not match everything.
        const auto when = rule.find("when");
        if (when == rule.end() || !when->is_array() || when->empty()) {
            spdlog::debug("[ProbePrep] Rule '{}' has no usable 'when' — skipping",
                          rule.value("id", "?"));
            continue;
        }
        const bool all_hold = std::all_of(when->begin(), when->end(), [&](const nlohmann::json& p) {
            return predicate_holds(p, macros_upper);
        });
        if (!all_hold) {
            continue;
        }

        if (suppressed_by_resolved_macro(rule, resolved_macro)) {
            spdlog::debug("[ProbePrep] Rule '{}' stands down: '{}' prepares itself",
                          rule.value("id", "?"), resolved_macro);
            continue;
        }

        Preparation prep;
        prep.gcode = read_gcode(rule);
        if (prep.gcode.empty()) {
            spdlog::warn("[ProbePrep] Rule '{}' matched but has no usable 'gcode' — skipping",
                         rule.value("id", "?"));
            continue;
        }
        prep.label = rule.value("label", "");
        prep.rule_id = rule.value("id", "");
        const auto timeout = rule.find("timeout_s");
        if (timeout != rule.end() && timeout->is_number()) {
            const double seconds = timeout->get<double>();
            if (seconds > 0.0) {
                prep.extra_timeout_ms = static_cast<uint32_t>(seconds * 1000.0);
            }
        }

        spdlog::info("[ProbePrep] {} prepared by rule '{}'", operation_key(op), prep.rule_id);
        return prep;
    }

    return {};
}

Preparation resolve(const PrinterDiscovery& hw, Operation op, const std::string& resolved_macro) {
    return resolve_from_rules(database_rules(), hw.get_macros(), op, resolved_macro);
}

uint32_t append_preparation(std::string& script, Operation op, const std::string& resolved_macro) {
    const Preparation prep = resolve(get_printer_state().get_discovery(), op, resolved_macro);
    if (prep.empty()) {
        return 0;
    }
    script += prep.gcode;
    script += "\n";
    spdlog::info("[ProbePrep] {} prepared by rule '{}' (+{} ms budget)", operation_key(op),
                 prep.rule_id, prep.extra_timeout_ms);
    return prep.extra_timeout_ms;
}

} // namespace helix::probe_prep

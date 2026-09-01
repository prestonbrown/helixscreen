// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

#include "data_root_resolver.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include "hv/json.hpp"

using namespace helix;

using json = nlohmann::json;

// ============================================================================
// STATIC HELPER: Case-insensitive string comparison
// ============================================================================

static std::string to_upper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

// ============================================================================
// STATIC HELPERS: Status-signal predicate evaluation
// ============================================================================

/// Resolve a dot-path into an object's status frame as a double, optionally
/// selecting one array element. False when any segment is missing, the path
/// lands on a non-number, or the index is out of range — a rule watching a
/// field the frame does not carry simply does not hold.
static bool resolve_numeric_field(const json& object_status, const std::string& path, int index,
                                  double& out) {
    if (!object_status.is_object() || path.empty()) {
        return false;
    }

    const json* node = &object_status;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const std::string segment =
            path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (segment.empty()) {
            return false;
        }
        const auto child = node->find(segment);
        if (child == node->end()) {
            return false;
        }
        node = &*child;
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }

    if (index >= 0) {
        if (!node->is_array() || static_cast<size_t>(index) >= node->size()) {
            return false;
        }
        node = &(*node)[static_cast<size_t>(index)];
    }

    if (!node->is_number()) {
        return false;
    }
    out = node->get<double>();
    return true;
}

/// Apply one comparison op. `tolerance` is only read by NEAR.
static bool compare_values(PrintStartProfile::StatusPredicate::Op op, double lhs, double rhs,
                           double tolerance) {
    switch (op) {
    case PrintStartProfile::StatusPredicate::Op::EQ:
        return lhs == rhs;
    case PrintStartProfile::StatusPredicate::Op::NE:
        return lhs != rhs;
    case PrintStartProfile::StatusPredicate::Op::GT:
        return lhs > rhs;
    case PrintStartProfile::StatusPredicate::Op::LT:
        return lhs < rhs;
    case PrintStartProfile::StatusPredicate::Op::NEAR:
        return std::fabs(lhs - rhs) < tolerance;
    }
    return false;
}

// ============================================================================
// FACTORY METHODS
// ============================================================================

std::shared_ptr<PrintStartProfile> PrintStartProfile::load(const std::string& profile_name) {
    std::string path = helix::find_readable("print_start_profiles/" + profile_name + ".json");

    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::warn("[PrintStartProfile] Could not open '{}', falling back to default", path);
        return load_default();
    }

    json j;
    try {
        j = json::parse(file);
    } catch (const json::parse_error& e) {
        spdlog::warn("[PrintStartProfile] JSON parse error in '{}': {}", path, e.what());
        return load_default();
    }

    auto profile = std::make_shared<PrintStartProfile>();
    if (!profile->parse_json(j, path)) {
        spdlog::warn("[PrintStartProfile] Failed to parse '{}', falling back to default", path);
        return load_default();
    }

    spdlog::info("[PrintStartProfile] Loaded profile '{}' from {}", profile->name(), path);
    return profile;
}

std::shared_ptr<PrintStartProfile> PrintStartProfile::load_default() {
    std::string path = helix::find_readable("print_start_profiles/default.json");

    // Try to load from JSON file first
    do {
        std::ifstream file(path);
        if (!file.is_open()) {
            break;
        }

        json j;
        try {
            j = json::parse(file);
        } catch (const json::parse_error& e) {
            spdlog::warn("[PrintStartProfile] JSON parse error in default.json: {}", e.what());
            break;
        }

        auto profile = std::make_shared<PrintStartProfile>();
        if (profile->parse_json(j, path)) {
            profile->is_default_ = true;
            spdlog::debug("[PrintStartProfile] Loaded default profile from JSON");
            return profile;
        }
        spdlog::warn("[PrintStartProfile] Failed to parse default.json, using built-in fallback");
    } while (false);
    // Built-in fallback: same patterns as currently hardcoded in PrintStartCollector
    auto profile = std::make_shared<PrintStartProfile>();
    profile->name_ = "Generic (built-in)";
    profile->is_default_ = true;
    profile->description_ = "Built-in fallback patterns matching PrintStartCollector defaults";
    profile->progress_mode_ = ProgressMode::WEIGHTED;

    // Response patterns matching the hardcoded patterns in print_start_collector.cpp
    struct PatternDef {
        const char* pattern;
        PrintStartPhase phase;
        const char* message;
        int weight;
    };

    // clang-format off
    const PatternDef builtin_patterns[] = {
        {"G28|Homing|Home All Axes|homing",
         PrintStartPhase::HOMING, lv_tr("Homing..."), 10},
        {"M190|M140\\s+S[1-9]|Heating bed|Heat Bed|BED_TEMP|bed.*heat",
         PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."), 20},
        {"M109|M104\\s+S[1-9]|Heating (nozzle|hotend|extruder)|EXTRUDER_TEMP",
         PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."), 20},
        {"QUAD_GANTRY_LEVEL|quad.?gantry.?level|QGL",
         PrintStartPhase::QGL, lv_tr("Leveling Gantry..."), 15},
        {"Z_TILT_ADJUST|z.?tilt.?adjust",
         PrintStartPhase::Z_TILT, lv_tr("Z Tilt Adjust..."), 15},
        {"BED_MESH_CALIBRATE|BED_MESH_PROFILE\\s+LOAD=|Loading bed mesh|mesh.*load",
         PrintStartPhase::BED_MESH, lv_tr("Loading Bed Mesh..."), 10},
        {"CLEAN_NOZZLE|NOZZLE_CLEAN|WIPE_NOZZLE|nozzle.?wipe|clean.?nozzle",
         PrintStartPhase::CLEANING, lv_tr("Cleaning Nozzle..."), 5},
        {"VORON_PURGE|LINE_PURGE|PURGE_LINE|Prime.?Line|Priming|KAMP_.*PURGE|purge.?line",
         PrintStartPhase::PURGING, lv_tr("Purging..."), 5},
    };
    // clang-format on

    for (const auto& def : builtin_patterns) {
        try {
            ResponsePattern rp;
            rp.pattern = std::regex(def.pattern, std::regex::icase);
            rp.phase = def.phase;
            rp.message_template = def.message;
            rp.weight = def.weight;
            profile->response_patterns_.push_back(std::move(rp));
        } catch (const std::regex_error& e) {
            spdlog::error("[PrintStartProfile] Built-in regex error for '{}': {}", def.pattern,
                          e.what());
        }
    }

    // Phase weights matching the hardcoded values
    profile->phase_weights_ = {
        {PrintStartPhase::HOMING, 10},         {PrintStartPhase::HEATING_BED, 20},
        {PrintStartPhase::HEATING_NOZZLE, 20}, {PrintStartPhase::QGL, 15},
        {PrintStartPhase::Z_TILT, 15},         {PrintStartPhase::BED_MESH, 10},
        {PrintStartPhase::CLEANING, 5},        {PrintStartPhase::PURGING, 5},
    };

    spdlog::debug("[PrintStartProfile] Using built-in fallback profile");
    return profile;
}

// ============================================================================
// MATCHING METHODS
// ============================================================================

bool PrintStartProfile::try_match_signal(const std::string& line, MatchResult& result) const {
    for (const auto& fmt : signal_formats_) {
        // Find the prefix anywhere in the line (not just at start)
        // The AD5M wraps state signals as "// State: HOMING..." etc.
        size_t pos = line.find(fmt.prefix);
        if (pos == std::string::npos) {
            continue;
        }

        // Extract the value after the prefix
        std::string value = line.substr(pos + fmt.prefix.size());

        // Trim trailing whitespace
        size_t end = value.find_last_not_of(" \t\n\r");
        if (end != std::string::npos) {
            value = value.substr(0, end + 1);
        }

        // Look up in mappings
        auto it = fmt.mappings.find(value);
        if (it != fmt.mappings.end()) {
            result = it->second;
            spdlog::debug("[PrintStartProfile] Signal match: '{}' -> phase={}, msg='{}'", value,
                          static_cast<int>(result.phase), result.message);
            return true;
        }

        // No match for this prefix's value, but prefix was found
        spdlog::trace("[PrintStartProfile] Prefix '{}' found but value '{}' not in mappings",
                      fmt.prefix, value);
    }
    return false;
}

bool PrintStartProfile::try_match_pattern(const std::string& line, MatchResult& result) const {
    return match_pattern_list(response_patterns_, line, result);
}

bool PrintStartProfile::try_match_state(const std::string& state, MatchResult& result) const {
    return match_pattern_list(state_patterns_, state, result);
}

bool PrintStartProfile::match_pattern_list(const std::vector<ResponsePattern>& patterns,
                                           const std::string& text, MatchResult& result) const {
    for (const auto& rp : patterns) {
        std::smatch match;
        if (std::regex_search(text, match, rp.pattern)) {
            result.phase = rp.phase;
            // Translate the TEMPLATE, then substitute captures into the
            // translated text. Doing it the other way round looks up the
            // substituted string ("Heating bed to 60°C...") as the key, which
            // never matches — the keys are the literal $1 templates — so
            // capture-bearing messages would stay English in every locale.
            result.message =
                substitute_captures(std::string(lv_tr(rp.message_template.c_str())), match);
            result.progress = rp.weight; // Caller interprets based on progress_mode
            spdlog::trace("[PrintStartProfile] Pattern match: '{}' -> phase={}, msg='{}'", text,
                          static_cast<int>(result.phase), result.message);
            return true;
        }
    }
    return false;
}

std::vector<std::string> PrintStartProfile::required_status_objects() const {
    std::vector<std::string> objects;
    if (has_phase_object()) {
        objects.push_back(phase_object_name_);
    }
    for (const auto& rule : status_signals_) {
        if (std::find(objects.begin(), objects.end(), rule.object) == objects.end()) {
            objects.push_back(rule.object);
        }
    }
    return objects;
}

bool PrintStartProfile::evaluate_status_signal(const json& object_status,
                                               const StatusSignalRule& rule,
                                               MatchResult& result) const {
    for (const auto& predicate : rule.when) {
        double lhs = 0.0;
        if (!resolve_numeric_field(object_status, predicate.field, predicate.index, lhs)) {
            return false;
        }
        double rhs = predicate.value;
        if (!predicate.ref_field.empty()) {
            double referenced = 0.0;
            if (!resolve_numeric_field(object_status, predicate.ref_field, -1, referenced)) {
                return false;
            }
            rhs = referenced;
        }
        if (!compare_values(predicate.op, lhs, rhs + predicate.offset, predicate.tolerance)) {
            return false;
        }
    }

    result.phase = rule.phase;
    // Translate the message like a pattern template (no capture groups here).
    result.message = lv_tr(rule.message.c_str());
    result.progress = rule.weight; // Caller interprets based on progress_mode
    return true;
}

// ============================================================================
// PROGRESS
// ============================================================================

int PrintStartProfile::get_phase_weight(PrintStartPhase phase) const {
    auto it = phase_weights_.find(phase);
    return (it != phase_weights_.end()) ? it->second : 0;
}

// ============================================================================
// JSON PARSING
// ============================================================================

bool PrintStartProfile::parse_json(const json& j, const std::string& source_path) {
    // Name (required)
    if (!j.contains("name") || !j["name"].is_string()) {
        spdlog::warn("[PrintStartProfile] Missing or invalid 'name' in {}", source_path);
        return false;
    }
    name_ = j["name"].get<std::string>();

    // Description (optional)
    if (j.contains("description") && j["description"].is_string()) {
        description_ = j["description"].get<std::string>();
    }

    // Adaptive bed-mesh probing (optional, defaults to false)
    if (j.contains("adaptive_meshing") && j["adaptive_meshing"].is_boolean()) {
        adaptive_meshing_ = j["adaptive_meshing"].get<bool>();
    }

    // Creality tag stream in gcode_response (optional, defaults to false)
    if (j.contains("cfs_signals") && j["cfs_signals"].is_boolean()) {
        cfs_signals_ = j["cfs_signals"].get<bool>();
    }

    // Silent-window position inference (optional, defaults to false)
    if (j.contains("position_signals") && j["position_signals"].is_boolean()) {
        position_signals_ = j["position_signals"].get<bool>();
    }

    // Progress mode (optional, defaults to weighted)
    if (j.contains("progress_mode") && j["progress_mode"].is_string()) {
        std::string mode_str = to_upper(j["progress_mode"].get<std::string>());
        if (mode_str == "WEIGHTED") {
            progress_mode_ = ProgressMode::WEIGHTED;
        } else if (mode_str == "SEQUENTIAL") {
            progress_mode_ = ProgressMode::SEQUENTIAL;
        } else {
            spdlog::warn("[PrintStartProfile] Unknown progress_mode '{}' in {}, defaulting to "
                         "weighted",
                         j["progress_mode"].get<std::string>(), source_path);
        }
    }

    // Signal formats (optional)
    if (j.contains("signal_formats") && j["signal_formats"].is_array()) {
        for (const auto& sf_json : j["signal_formats"]) {
            if (!sf_json.is_object()) {
                spdlog::warn("[PrintStartProfile] Skipping non-object signal_format in {}",
                             source_path);
                continue;
            }

            if (!sf_json.contains("prefix") || !sf_json["prefix"].is_string()) {
                spdlog::warn("[PrintStartProfile] Signal format missing 'prefix' in {}",
                             source_path);
                continue;
            }

            SignalFormat fmt;
            fmt.prefix = sf_json["prefix"].get<std::string>();

            if (sf_json.contains("mappings") && sf_json["mappings"].is_object()) {
                for (auto it = sf_json["mappings"].begin(); it != sf_json["mappings"].end(); ++it) {
                    const auto& mapping = it.value();
                    if (!mapping.is_object()) {
                        spdlog::warn("[PrintStartProfile] Skipping non-object mapping '{}' in {}",
                                     it.key(), source_path);
                        continue;
                    }

                    MatchResult mr;

                    // Parse phase (required)
                    if (!mapping.contains("phase") || !mapping["phase"].is_string()) {
                        spdlog::warn("[PrintStartProfile] Mapping '{}' missing 'phase' in {}",
                                     it.key(), source_path);
                        continue;
                    }
                    mr.phase = parse_phase_name(mapping["phase"].get<std::string>());

                    // Parse message (optional, default to key name)
                    if (mapping.contains("message") && mapping["message"].is_string()) {
                        mr.message = mapping["message"].get<std::string>();
                    } else {
                        mr.message = it.key();
                    }

                    // Parse progress (optional, default 0)
                    if (mapping.contains("progress") && mapping["progress"].is_number()) {
                        mr.progress = mapping["progress"].get<int>();
                    } else {
                        mr.progress = 0;
                    }

                    fmt.mappings[it.key()] = std::move(mr);
                }
            }

            signal_formats_.push_back(std::move(fmt));
        }
    }

    // Response patterns (optional)
    if (j.contains("response_patterns") && j["response_patterns"].is_array()) {
        parse_pattern_array(j["response_patterns"], response_patterns_, "response_pattern",
                            source_path);
    }

    // Phase object (optional) — a status object whose field carries structured
    // phase state. Its state strings map to phases through state_patterns and
    // feed the same matching pipeline console lines take. Malformed entries
    // warn and leave the profile without a phase object, which is exactly the
    // behavior of a profile that never declared one.
    if (j.contains("phase_object") && j["phase_object"].is_object()) {
        const auto& po = j["phase_object"];
        const bool has_object = po.contains("object") && po["object"].is_string();
        const bool has_field = po.contains("field") && po["field"].is_string();
        if (has_object && has_field) {
            phase_object_name_ = po["object"].get<std::string>();
            phase_object_field_ = po["field"].get<std::string>();
            if (phase_object_name_.empty() || phase_object_field_.empty()) {
                spdlog::warn("[PrintStartProfile] Empty phase_object name/field in {} - ignored",
                             source_path);
                phase_object_name_.clear();
                phase_object_field_.clear();
            }
        } else {
            spdlog::warn(
                "[PrintStartProfile] phase_object needs 'object' and 'field' strings in {} - "
                "ignored",
                source_path);
        }
    }

    // State patterns (optional) — matched against the phase object's state
    // string. Same entry shape as response_patterns.
    if (j.contains("state_patterns") && j["state_patterns"].is_array()) {
        parse_pattern_array(j["state_patterns"], state_patterns_, "state_pattern", source_path);
    }

    // Status signals (optional) — physical phase-inference rules over status
    // objects. Malformed entries warn and are skipped whole, exactly like the
    // other blocks.
    if (j.contains("status_signals") && j["status_signals"].is_array()) {
        parse_status_signals(j["status_signals"], source_path);
    }

    // Silent-phase progression (optional) — time-based phase advancement
    // for firmwares that run cleaning/purge as silent macros (no gcode
    // response between heat-complete and first layer).
    if (j.contains("silent_progression") && j["silent_progression"].is_array()) {
        for (const auto& entry_json : j["silent_progression"]) {
            if (!entry_json.is_object()) {
                spdlog::warn(
                    "[PrintStartProfile] Skipping non-object silent_progression entry in {}",
                    source_path);
                continue;
            }
            if (!entry_json.contains("phase") || !entry_json["phase"].is_string()) {
                spdlog::warn("[PrintStartProfile] silent_progression entry missing 'phase' in {}",
                             source_path);
                continue;
            }
            SilentPhaseEntry e;
            e.phase = parse_phase_name(entry_json["phase"].get<std::string>());
            if (entry_json.contains("after_temps_ready_seconds") &&
                entry_json["after_temps_ready_seconds"].is_number()) {
                e.after_temps_ready_seconds = entry_json["after_temps_ready_seconds"].get<int>();
            }
            if (entry_json.contains("message") && entry_json["message"].is_string()) {
                e.message = entry_json["message"].get<std::string>();
            }
            silent_progression_.push_back(std::move(e));
        }
        std::sort(silent_progression_.begin(), silent_progression_.end(),
                  [](const SilentPhaseEntry& a, const SilentPhaseEntry& b) {
                      return a.after_temps_ready_seconds < b.after_temps_ready_seconds;
                  });
    }

    // Phase weights (optional)
    if (j.contains("phase_weights") && j["phase_weights"].is_object()) {
        for (auto it = j["phase_weights"].begin(); it != j["phase_weights"].end(); ++it) {
            if (!it.value().is_number()) {
                spdlog::warn("[PrintStartProfile] Non-numeric phase_weight for '{}' in {}",
                             it.key(), source_path);
                continue;
            }
            PrintStartPhase phase = parse_phase_name(it.key());
            phase_weights_[phase] = it.value().get<int>();
        }
    }

    spdlog::debug("[PrintStartProfile] Parsed '{}': {} signal_formats, {} response_patterns, "
                  "{} state_patterns, {} phase_weights, {} silent_progression, {} status_signals",
                  name_, signal_formats_.size(), response_patterns_.size(), state_patterns_.size(),
                  phase_weights_.size(), silent_progression_.size(), status_signals_.size());
    return true;
}

void PrintStartProfile::parse_pattern_array(const nlohmann::json& array,
                                            std::vector<ResponsePattern>& out, const char* kind,
                                            const std::string& source_path) {
    for (const auto& rp_json : array) {
        if (!rp_json.is_object()) {
            spdlog::warn("[PrintStartProfile] Skipping non-object {} in {}", kind, source_path);
            continue;
        }

        if (!rp_json.contains("pattern") || !rp_json["pattern"].is_string()) {
            spdlog::warn("[PrintStartProfile] {} missing 'pattern' in {}", kind, source_path);
            continue;
        }

        ResponsePattern rp;

        // Compile regex with case-insensitive flag
        std::string pattern_str = rp_json["pattern"].get<std::string>();
        try {
            rp.pattern = std::regex(pattern_str, std::regex::icase);
        } catch (const std::regex_error& e) {
            spdlog::warn("[PrintStartProfile] Invalid regex '{}' in {}: {}", pattern_str,
                         source_path, e.what());
            continue;
        }

        // Parse phase (required)
        if (!rp_json.contains("phase") || !rp_json["phase"].is_string()) {
            spdlog::warn("[PrintStartProfile] {} missing 'phase' for regex '{}' in {}", kind,
                         pattern_str, source_path);
            continue;
        }
        rp.phase = parse_phase_name(rp_json["phase"].get<std::string>());

        // Parse message template (optional)
        if (rp_json.contains("message") && rp_json["message"].is_string()) {
            rp.message_template = rp_json["message"].get<std::string>();
        }

        // Parse weight (optional, default 0)
        if (rp_json.contains("weight") && rp_json["weight"].is_number()) {
            rp.weight = rp_json["weight"].get<int>();
        } else {
            rp.weight = 0;
        }

        out.push_back(std::move(rp));
    }
}

void PrintStartProfile::parse_status_signals(const json& array, const std::string& source_path) {
    auto op_from_name = [](const std::string& name, StatusPredicate::Op& op) -> bool {
        if (name == "eq")
            op = StatusPredicate::Op::EQ;
        else if (name == "ne")
            op = StatusPredicate::Op::NE;
        else if (name == "gt")
            op = StatusPredicate::Op::GT;
        else if (name == "lt")
            op = StatusPredicate::Op::LT;
        else if (name == "near")
            op = StatusPredicate::Op::NEAR;
        else
            return false;
        return true;
    };

    for (const auto& rule_json : array) {
        if (!rule_json.is_object()) {
            spdlog::warn("[PrintStartProfile] Skipping non-object status_signal in {}",
                         source_path);
            continue;
        }
        if (!rule_json.contains("name") || !rule_json["name"].is_string() ||
            rule_json["name"].get<std::string>().empty()) {
            spdlog::warn("[PrintStartProfile] status_signal missing 'name' in {}", source_path);
            continue;
        }
        const std::string name = rule_json["name"].get<std::string>();
        const auto skip = [&](const char* why) {
            spdlog::warn("[PrintStartProfile] Skipping status_signal '{}' - {} in {}", name, why,
                         source_path);
        };

        if (!rule_json.contains("object") || !rule_json["object"].is_string() ||
            rule_json["object"].get<std::string>().empty()) {
            skip("missing 'object'");
            continue;
        }
        if (!rule_json.contains("when") || !rule_json["when"].is_array() ||
            rule_json["when"].empty()) {
            skip("'when' must be a non-empty array");
            continue;
        }
        if (!rule_json.contains("phase") || !rule_json["phase"].is_string()) {
            skip("missing 'phase'");
            continue;
        }
        // A rule targeting IDLE has nothing to say — the phase stream starts
        // at INITIALIZING — so it is a malformed target, not a mapping.
        const std::string phase_name = rule_json["phase"].get<std::string>();
        if (to_upper(phase_name) == "IDLE") {
            skip("phase may not be IDLE");
            continue;
        }
        if (std::any_of(
                status_signals_.begin(), status_signals_.end(),
                [&name](const StatusSignalRule& existing) { return existing.name == name; })) {
            skip("duplicate name (the name is the edge-trigger latch key)");
            continue;
        }

        StatusSignalRule rule;
        rule.name = name;
        rule.object = rule_json["object"].get<std::string>();
        rule.phase = parse_phase_name(phase_name);

        bool predicates_ok = true;
        for (const auto& predicate_json : rule_json["when"]) {
            if (!predicate_json.is_object()) {
                skip("'when' contains a non-object predicate");
                predicates_ok = false;
                break;
            }
            StatusPredicate predicate;
            if (!predicate_json.contains("field") || !predicate_json["field"].is_string() ||
                predicate_json["field"].get<std::string>().empty()) {
                skip("predicate missing 'field'");
                predicates_ok = false;
                break;
            }
            predicate.field = predicate_json["field"].get<std::string>();

            if (predicate_json.contains("index")) {
                if (!predicate_json["index"].is_number_integer()) {
                    skip("predicate 'index' must be an integer");
                    predicates_ok = false;
                    break;
                }
                predicate.index = predicate_json["index"].get<int>();
                if (predicate.index < 0) {
                    skip("predicate 'index' must be >= 0");
                    predicates_ok = false;
                    break;
                }
            }

            if (!predicate_json.contains("op") || !predicate_json["op"].is_string() ||
                !op_from_name(predicate_json["op"].get<std::string>(), predicate.op)) {
                skip("predicate 'op' must be one of eq/ne/gt/lt/near");
                predicates_ok = false;
                break;
            }

            const bool has_value =
                predicate_json.contains("value") && predicate_json["value"].is_number();
            const bool has_ref = predicate_json.contains("ref_field") &&
                                 predicate_json["ref_field"].is_string() &&
                                 !predicate_json["ref_field"].get<std::string>().empty();
            if (has_value && has_ref) {
                skip("predicate has both 'value' and 'ref_field'");
                predicates_ok = false;
                break;
            }
            if (!has_value && !has_ref) {
                skip("predicate needs a 'value' or a 'ref_field'");
                predicates_ok = false;
                break;
            }
            if (has_value) {
                predicate.value = predicate_json["value"].get<double>();
            } else {
                predicate.ref_field = predicate_json["ref_field"].get<std::string>();
            }

            if (predicate_json.contains("offset")) {
                if (!predicate_json["offset"].is_number()) {
                    skip("predicate 'offset' must be a number");
                    predicates_ok = false;
                    break;
                }
                predicate.offset = predicate_json["offset"].get<double>();
            }

            if (predicate.op == StatusPredicate::Op::NEAR) {
                if (!predicate_json.contains("tolerance") ||
                    !predicate_json["tolerance"].is_number() ||
                    predicate_json["tolerance"].get<double>() <= 0.0) {
                    skip("'near' needs a positive 'tolerance'");
                    predicates_ok = false;
                    break;
                }
                predicate.tolerance = predicate_json["tolerance"].get<double>();
            }

            rule.when.push_back(std::move(predicate));
        }
        if (!predicates_ok || rule.when.empty()) {
            continue;
        }

        if (rule_json.contains("message") && rule_json["message"].is_string()) {
            rule.message = rule_json["message"].get<std::string>();
        }
        if (rule_json.contains("weight") && rule_json["weight"].is_number()) {
            rule.weight = rule_json["weight"].get<int>();
        }

        status_signals_.push_back(std::move(rule));
    }
}

PrintStartPhase PrintStartProfile::parse_phase_name(const std::string& name) {
    std::string upper = to_upper(name);

    if (upper == "IDLE")
        return PrintStartPhase::IDLE;
    if (upper == "INITIALIZING")
        return PrintStartPhase::INITIALIZING;
    if (upper == "HOMING")
        return PrintStartPhase::HOMING;
    if (upper == "HEATING_BED")
        return PrintStartPhase::HEATING_BED;
    if (upper == "HEATING_NOZZLE")
        return PrintStartPhase::HEATING_NOZZLE;
    if (upper == "QGL")
        return PrintStartPhase::QGL;
    if (upper == "Z_TILT")
        return PrintStartPhase::Z_TILT;
    if (upper == "BED_MESH")
        return PrintStartPhase::BED_MESH;
    if (upper == "CLEANING")
        return PrintStartPhase::CLEANING;
    if (upper == "PURGING")
        return PrintStartPhase::PURGING;
    if (upper == "COMPLETE")
        return PrintStartPhase::COMPLETE;

    spdlog::warn("[PrintStartProfile] Unknown phase name: '{}'", name);
    return PrintStartPhase::IDLE;
}

std::string PrintStartProfile::substitute_captures(const std::string& tmpl,
                                                   const std::smatch& match) {
    std::string result;
    result.reserve(tmpl.size() + 32);

    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '$' && (i + 1) < tmpl.size() && std::isdigit(tmpl[i + 1])) {
            // Parse the group number (supports multi-digit: $1, $2, ..., $12, etc.)
            size_t start = i + 1;
            size_t end = start;
            while (end < tmpl.size() && std::isdigit(tmpl[end])) {
                ++end;
            }
            int group = std::stoi(tmpl.substr(start, end - start));

            if (group >= 0 && static_cast<size_t>(group) < match.size()) {
                result += match[group].str();
            }
            // Skip past the digits
            i = end - 1;
        } else {
            result += tmpl[i];
        }
    }

    return result;
}

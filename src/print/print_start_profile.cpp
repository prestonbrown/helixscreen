// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
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
// FACTORY METHODS
// ============================================================================

std::shared_ptr<PrintStartProfile> PrintStartProfile::load(const std::string& profile_name) {
    std::string path = "config/print_start_profiles/" + profile_name + ".json";

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
    std::string path = "config/print_start_profiles/default.json";

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
            spdlog::debug("[PrintStartProfile] Loaded default profile from JSON");
            return profile;
        }
        spdlog::warn("[PrintStartProfile] Failed to parse default.json, using built-in fallback");
    } while (false);
    // Built-in fallback: same patterns as currently hardcoded in PrintStartCollector
    auto profile = std::make_shared<PrintStartProfile>();
    profile->name_ = "Generic (built-in)";
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
    for (const auto& rp : response_patterns_) {
        std::smatch match;
        if (std::regex_search(line, match, rp.pattern)) {
            result.phase = rp.phase;
            result.message = substitute_captures(rp.message_template, match);
            result.progress = rp.weight; // Caller interprets based on progress_mode
            spdlog::trace("[PrintStartProfile] Pattern match: '{}' -> phase={}, msg='{}'", line,
                          static_cast<int>(result.phase), result.message);
            return true;
        }
    }
    return false;
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
        for (const auto& rp_json : j["response_patterns"]) {
            if (!rp_json.is_object()) {
                spdlog::warn("[PrintStartProfile] Skipping non-object response_pattern in {}",
                             source_path);
                continue;
            }

            if (!rp_json.contains("pattern") || !rp_json["pattern"].is_string()) {
                spdlog::warn("[PrintStartProfile] Response pattern missing 'pattern' in {}",
                             source_path);
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
                spdlog::warn(
                    "[PrintStartProfile] Response pattern missing 'phase' for regex '{}' in {}",
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

            response_patterns_.push_back(std::move(rp));
        }
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
                  "{} phase_weights",
                  name_, signal_formats_.size(), response_patterns_.size(), phase_weights_.size());
    return true;
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

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

#include "data_root_resolver.h"
#include "lvgl.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "hv/json.hpp"

using namespace helix;

using json = nlohmann::json;

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

    auto text = helix::text_io::read_file(path);
    if (!text) {
        spdlog::warn("[PrintStartProfile] Could not open '{}', falling back to default", path);
        return load_default();
    }

    json j = json::parse(*text, nullptr, false);
    if (j.is_discarded()) {
        spdlog::warn("[PrintStartProfile] JSON parse error in '{}'", path);
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
        auto text = helix::text_io::read_file(path);
        if (!text) {
            break;
        }

        json j = json::parse(*text, nullptr, false);
        if (j.is_discarded()) {
            spdlog::warn("[PrintStartProfile] JSON parse error in default.json");
            break;
        }

        auto profile = std::make_shared<PrintStartProfile>();
        if (profile->parse_json(j, path)) {
            profile->is_default_ = true;
            spdlog::debug("[PrintStartProfile] Loaded default profile from JSON");
            return profile;
        }
        spdlog::warn("[PrintStartProfile] Failed to parse default.json");
    } while (false);

    // Only a broken install lands here. The engine's own signals (HELIX:PHASE,
    // RESPOND, macro variables, heater inference, timeouts) need no patterns.
    spdlog::error(
        "[PrintStartProfile] No readable default.json at '{}', phase text matching is off", path);
    auto profile = std::make_shared<PrintStartProfile>();
    profile->name_ = "Generic";
    profile->is_default_ = true;
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

bool PrintStartProfile::start_block_step_owns(const std::string& command,
                                              const std::string& console_line) const {
    const auto it = start_block_owned_lines_.find(command);
    if (it == start_block_owned_lines_.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(), [&](const std::string& text) {
        return console_line.find(text) != std::string::npos;
    });
}

bool PrintStartProfile::try_match_pattern(const std::string& line, MatchResult& result) const {
    return match_pattern_list(response_patterns_, line, result);
}

bool PrintStartProfile::try_match_tag(const std::string& line, MatchResult& result) const {
    return match_pattern_list(tag_patterns_, line, result);
}

int PrintStartProfile::percent_from_report(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        return -1;
    }
    const float v = static_cast<float>(value);
    // Bounded before the cast; the result clamps to 100 anyway.
    const float scaled = (v <= 1.5f) ? v * 100.0f : std::min(v, 1000.0f);
    return std::clamp(static_cast<int>(scaled + 0.5f), 0, 100);
}

bool PrintStartProfile::try_match_state(const std::string& state, MatchResult& result) const {
    // A profile that declares a phase object but no state patterns matches its
    // state text with the response patterns instead: both feeds carry the same
    // phase vocabulary, so one list serves both and the generic profile needs
    // no second hand-maintained copy of the same regexes.
    const auto& patterns = state_patterns_.empty() ? response_patterns_ : state_patterns_;
    return match_pattern_list(patterns, state, result);
}

bool PrintStartProfile::match_pattern_list(const std::vector<ResponsePattern>& patterns,
                                           const std::string& text, MatchResult& result) const {
    for (const auto& rp : patterns) {
        helix::RegexMatch match;
        if (helix::regex_search(text, match, rp.pattern)) {
            result.phase = rp.phase;
            // Translate the TEMPLATE, then substitute captures into the
            // translated text. Doing it the other way round looks up the
            // substituted string ("Heating bed to 60°C...") as the key, which
            // never matches — the keys are the literal $1 templates — so
            // capture-bearing messages would stay English in every locale.
            result.message =
                substitute_captures(std::string(lv_tr(rp.message_template.c_str())), match);
            result.progress = rp.weight; // Caller interprets based on progress_mode
            result.hold_seconds = 0;
            result.narrates = rp.narrates;
            result.counts_probes = rp.counts_probes;
            result.reported_percent = -1;
            result.owns = rp.owns;
            if (rp.progress_group > 0 && static_cast<size_t>(rp.progress_group) < match.size()) {
                if (auto value = helix::text_io::parse_double(match[rp.progress_group].view)) {
                    result.reported_percent = percent_from_report(*value);
                }
            }
            if (rp.hold_minutes_group > 0 &&
                static_cast<size_t>(rp.hold_minutes_group) < match.size()) {
                // Firmware prints the number with a '.' whatever the UI locale.
                // A day bounds the seconds well inside an int.
                constexpr double kMaxHoldMinutes = 24.0 * 60.0;
                auto minutes = helix::text_io::parse_double(match[rp.hold_minutes_group].view);
                if (minutes && std::isfinite(*minutes) && *minutes > 0.0) {
                    result.hold_seconds =
                        static_cast<int>(std::lround(std::min(*minutes, kMaxHoldMinutes) * 60.0));
                }
            }
            spdlog::trace("[PrintStartProfile] Pattern match: '{}' -> phase={}, msg='{}'", text,
                          static_cast<int>(result.phase), result.message);
            return true;
        }
    }
    return false;
}

std::string PrintStartProfile::innermost_context(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_array() && !value.empty() && value.back().is_string()) {
        return value.back().get<std::string>();
    }
    return {};
}

std::string PrintStartProfile::qualify_state(const std::string& context, const std::string& state) {
    return context + "/" + state;
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
    result.hold_seconds = 0;
    return true;
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

    // Start sequence inlined in the print file (optional, defaults to false)
    if (j.contains("scripted_start") && j["scripted_start"].is_boolean()) {
        scripted_start_ = j["scripted_start"].get<bool>();
    }

    // Console lines a start-block command prints while it runs (optional)
    if (j.contains("start_block_owned_lines") && j["start_block_owned_lines"].is_object()) {
        for (const auto& [command, lines] : j["start_block_owned_lines"].items()) {
            if (!lines.is_array()) {
                continue;
            }
            auto& owned = start_block_owned_lines_[helix::text_io::to_upper(command)];
            for (const auto& text : lines) {
                if (text.is_string() && !text.get_ref<const std::string&>().empty()) {
                    owned.push_back(text.get<std::string>());
                }
            }
        }
    }

    // Silent-window position inference (optional, defaults to false)
    if (j.contains("position_signals") && j["position_signals"].is_boolean()) {
        position_signals_ = j["position_signals"].get<bool>();
    }

    // Progress mode (optional, defaults to weighted)
    if (j.contains("progress_mode") && j["progress_mode"].is_string()) {
        std::string mode_str = helix::text_io::to_upper(j["progress_mode"].get<std::string>());
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

    // Tag patterns (optional) — lines that announce a step outright, matched
    // ahead of the response patterns. Same entry shape.
    if (j.contains("tag_patterns") && j["tag_patterns"].is_array()) {
        parse_pattern_array(j["tag_patterns"], tag_patterns_, "tag_pattern", source_path);
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
            // A bad context qualifier drops the whole phase object: its
            // state_patterns are written against "<context>/<state>" keys, so
            // matching bare states against them would only ever miss.
            if (po.contains("context_field")) {
                const auto& cf = po["context_field"];
                if (cf.is_string() && !cf.get_ref<const std::string&>().empty()) {
                    phase_object_context_field_ = cf.get<std::string>();
                } else {
                    spdlog::warn("[PrintStartProfile] phase_object context_field must be a "
                                 "non-empty string in {} - phase_object ignored",
                                 source_path);
                    phase_object_name_.clear();
                    phase_object_field_.clear();
                }
            }
            if (phase_object_name_.empty()) {
                phase_object_context_field_.clear();
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

    spdlog::debug("[PrintStartProfile] Parsed '{}': {} signal_formats, {} response_patterns, "
                  "{} state_patterns, {} silent_progression, {} status_signals",
                  name_, signal_formats_.size(), response_patterns_.size(), state_patterns_.size(),
                  silent_progression_.size(), status_signals_.size());
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

        // Case-insensitive unless the entry asks otherwise
        std::string pattern_str = rp_json["pattern"].get<std::string>();
        bool case_sensitive = false;
        if (rp_json.contains("case_sensitive")) {
            if (rp_json["case_sensitive"].is_boolean()) {
                case_sensitive = rp_json["case_sensitive"].get<bool>();
            } else {
                spdlog::warn(
                    "[PrintStartProfile] Ignoring non-boolean case_sensitive for {} '{}' in {}",
                    kind, pattern_str, source_path);
            }
        }
        rp.pattern =
            helix::Regex(pattern_str, case_sensitive ? helix::Regex::None : helix::Regex::ICase);
        if (!rp.pattern.ok()) {
            spdlog::warn("[PrintStartProfile] Invalid regex '{}' in {}: {}", pattern_str,
                         source_path, rp.pattern.error());
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

        if (rp_json.contains("narrates")) {
            if (rp_json["narrates"].is_boolean()) {
                rp.narrates = rp_json["narrates"].get<bool>();
            } else {
                spdlog::warn("[PrintStartProfile] Ignoring non-boolean narrates for {} '{}' in {}",
                             kind, pattern_str, source_path);
            }
        }

        if (rp_json.contains("counts_probes")) {
            if (rp_json["counts_probes"].is_boolean()) {
                rp.counts_probes = rp_json["counts_probes"].get<bool>();
            } else {
                spdlog::warn(
                    "[PrintStartProfile] Ignoring non-boolean counts_probes for {} '{}' in {}",
                    kind, pattern_str, source_path);
            }
        }

        // Silent minutes the text announces, from a capture group (optional)
        if (rp_json.contains("hold_minutes_group")) {
            const auto& group = rp_json["hold_minutes_group"];
            if (group.is_number_integer() && group.get<int>() >= 1 &&
                static_cast<unsigned>(group.get<int>()) <= rp.pattern.mark_count()) {
                rp.hold_minutes_group = group.get<int>();
            } else {
                spdlog::warn("[PrintStartProfile] Ignoring hold_minutes_group for {} '{}' in {}: "
                             "not a capture group of the pattern",
                             kind, pattern_str, source_path);
            }
        }

        // Progress the text reports, from a capture group (optional)
        if (rp_json.contains("progress_group")) {
            const auto& group = rp_json["progress_group"];
            if (group.is_number_integer() && group.get<int>() >= 1 &&
                static_cast<unsigned>(group.get<int>()) <= rp.pattern.mark_count()) {
                rp.progress_group = group.get<int>();
            } else {
                spdlog::warn("[PrintStartProfile] Ignoring progress_group for {} '{}' in {}: "
                             "not a capture group of the pattern",
                             kind, pattern_str, source_path);
            }
        }

        // Phases the step this text begins emits signals for (optional)
        if (rp_json.contains("owns")) {
            const auto& owns = rp_json["owns"];
            if (!owns.is_array()) {
                spdlog::warn("[PrintStartProfile] Ignoring non-array owns for {} '{}' in {}", kind,
                             pattern_str, source_path);
            } else {
                for (const auto& name : owns) {
                    std::optional<PrintStartPhase> phase;
                    if (name.is_string()) {
                        phase = helix::print_start_phase_from_name(
                            helix::text_io::to_upper(name.get<std::string>()));
                    }
                    // A step cannot own itself, nothing, or the print starting.
                    if (!phase || *phase == rp.phase || *phase == PrintStartPhase::IDLE ||
                        *phase == PrintStartPhase::COMPLETE) {
                        spdlog::warn("[PrintStartProfile] Ignoring owns entry {} for {} '{}' in {}",
                                     name.dump(), kind, pattern_str, source_path);
                        continue;
                    }
                    if (std::find(rp.owns.begin(), rp.owns.end(), *phase) == rp.owns.end()) {
                        rp.owns.push_back(*phase);
                    }
                }
            }
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
        if (helix::text_io::to_upper(phase_name) == "IDLE") {
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
        if (rule_json.contains("after_heat")) {
            if (rule_json["after_heat"].is_boolean()) {
                rule.after_heat = rule_json["after_heat"].get<bool>();
            } else {
                spdlog::warn("[PrintStartProfile] Ignoring non-boolean after_heat for status "
                             "signal '{}' in {}",
                             rule.name, source_path);
            }
        }

        status_signals_.push_back(std::move(rule));
    }
}

PrintStartPhase PrintStartProfile::parse_phase_name(const std::string& name) {
    // Profiles are hand-written JSON, so the spelling is case-folded before the
    // canonical lookup; the lookup itself is exact.
    if (const auto phase = helix::print_start_phase_from_name(helix::text_io::to_upper(name))) {
        return *phase;
    }

    spdlog::warn("[PrintStartProfile] Unknown phase name: '{}'", name);
    return PrintStartPhase::IDLE;
}

std::string PrintStartProfile::substitute_captures(const std::string& tmpl,
                                                   const helix::RegexMatch& match) {
    std::string result;
    result.reserve(tmpl.size() + 32);

    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '$' && (i + 1) < tmpl.size() &&
            std::isdigit(static_cast<unsigned char>(tmpl[i + 1]))) {
            // Parse the group number (supports multi-digit: $1, $2, ..., $12, etc.)
            size_t start = i + 1;
            size_t end = start;
            while (end < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[end]))) {
                ++end;
            }
            int group =
                helix::text_io::parse_leading<int>(tmpl.substr(start, end - start)).value_or(-1);

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

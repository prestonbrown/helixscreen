// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_param_cache.h"

#include "core/text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "hv/json.hpp"

namespace helix {

namespace {
/// Convert a string to lowercase in-place and return a reference.
std::string& to_lower_inplace(std::string& s) {
    s = helix::text_io::to_lower(s);
    return s;
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

size_t skip_space(std::string_view s, size_t pos) {
    while (pos < s.size() && is_space(s[pos])) {
        ++pos;
    }
    return pos;
}
} // namespace

PrinterStopCall find_printer_stop_call(std::string_view gcode_template) {
    static constexpr std::string_view NAME = "action_emergency_stop";
    PrinterStopCall call;
    for (size_t at = gcode_template.find(NAME); at != std::string_view::npos;
         at = gcode_template.find(NAME, at + NAME.size())) {
        if (at > 0 && is_identifier_char(gcode_template[at - 1])) {
            continue; // part of a longer name
        }
        size_t pos = skip_space(gcode_template, at + NAME.size());
        if (pos >= gcode_template.size() || gcode_template[pos] != '(') {
            continue; // named, not called
        }
        call.calls = true;

        pos = skip_space(gcode_template, pos + 1);
        if (pos >= gcode_template.size()) {
            return call;
        }
        const char quote = gcode_template[pos];
        if (quote != '"' && quote != '\'') {
            return call;
        }
        const size_t close = gcode_template.find(quote, pos + 1);
        if (close == std::string_view::npos) {
            return call;
        }
        const std::string_view literal = gcode_template.substr(pos + 1, close - pos - 1);
        if (literal.find('\\') != std::string_view::npos) {
            return call; // an escape is not a plain literal
        }
        const size_t after = skip_space(gcode_template, close + 1);
        if (after < gcode_template.size() && gcode_template[after] == ')') {
            call.message = std::string(literal);
        }
        return call;
    }
    return call;
}

bool template_sets_active_spool(std::string_view gcode_template) {
    const std::string upper = helix::text_io::to_upper(gcode_template);
    const auto is_word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    for (const std::string_view word : {"SET_ACTIVE_SPOOL", "SPOOLMAN_SET_ACTIVE_SPOOL"}) {
        for (size_t pos = upper.find(word); pos != std::string::npos;
             pos = upper.find(word, pos + 1)) {
            const size_t end = pos + word.size();
            const bool starts = pos == 0 || !is_word(upper[pos - 1]);
            const bool ends = end >= upper.size() || !is_word(upper[end]);
            if (starts && ends) {
                return true;
            }
        }
    }
    return false;
}

namespace {

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// Drop leading whitespace and `{% ... %}` / `{# ... #}` blocks.
std::string_view skip_jinja_prefix(std::string_view line) {
    for (;;) {
        const size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) {
            return {};
        }
        line.remove_prefix(start);
        if (line.size() >= 2 && line[0] == '{' && (line[1] == '%' || line[1] == '#')) {
            const char closer[3] = {line[1], '}', '\0'};
            const size_t end = line.find(closer, 2);
            if (end == std::string_view::npos) {
                return {};
            }
            line.remove_prefix(end + 2);
            continue;
        }
        return line;
    }
}

/// The value of `key=` in an upper-cased command line, or empty.
std::string_view param_value(std::string_view upper_line, std::string_view key) {
    for (size_t pos = upper_line.find(key); pos != std::string_view::npos;
         pos = upper_line.find(key, pos + 1)) {
        if (pos > 0 && is_word_char(upper_line[pos - 1])) {
            continue;
        }
        const size_t value_start = pos + key.size();
        const size_t value_end = upper_line.find_first_of(" \t\r", value_start);
        return upper_line.substr(value_start, value_end == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : value_end - value_start);
    }
    return {};
}

} // namespace

TemplateTemperatureUse template_temperature_use(std::string_view gcode_template) {
    TemplateTemperatureUse use;
    const std::string upper = helix::text_io::to_upper(gcode_template);
    std::string_view rest = upper;
    while (!rest.empty()) {
        const size_t nl = rest.find('\n');
        std::string_view line = skip_jinja_prefix(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);

        size_t word_end = 0;
        while (word_end < line.size() && is_word_char(line[word_end])) {
            ++word_end;
        }
        if (word_end == 0) {
            continue;
        }
        const std::string_view command = line.substr(0, word_end);
        use.commands.emplace_back(command);

        const char* kind = nullptr;
        std::string_view value;
        if (command == "SET_HEATER_TEMPERATURE") {
            kind = "heater ";
            value = param_value(line, "HEATER=");
        } else if (command == "SET_TEMPERATURE_FAN_TARGET") {
            kind = "temperature_fan ";
            value = param_value(line, "TEMPERATURE_FAN=");
        }
        if (kind && !value.empty()) {
            std::string name = value.find('{') != std::string_view::npos ? "*" : std::string(value);
            to_lower_inplace(name);
            use.targets.push_back(kind + name);
        }
    }
    return use;
}

MacroParamCache& MacroParamCache::instance() {
    static MacroParamCache cache;
    return cache;
}

void MacroParamCache::populate_from_configfile(
    const nlohmann::json& config, const std::unordered_set<std::string>& known_macros) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    populated_ = false;
    ++generation_;

    if (!config.is_object()) {
        spdlog::warn("[MacroParamCache] configfile.config is not an object");
        return;
    }

    static constexpr std::string_view PREFIX = "gcode_macro ";

    size_t config_count = 0;
    for (auto it = config.begin(); it != config.end(); ++it) {
        const auto& key = it.key();

        if (key.size() <= PREFIX.size() || key.compare(0, PREFIX.size(), PREFIX) != 0) {
            continue;
        }

        std::string macro_name_lower = key.substr(PREFIX.size());
        to_lower_inplace(macro_name_lower);

        // Extract gcode template for param parsing
        std::string gcode_template;
        if (it.value().is_object() && it.value().contains("gcode") &&
            it.value()["gcode"].is_string()) {
            gcode_template = it.value()["gcode"].get<std::string>();
        }

        // Extract description if present
        std::string description;
        if (it.value().is_object() && it.value().contains("description") &&
            it.value()["description"].is_string()) {
            description = it.value()["description"].get<std::string>();
        }

        CachedMacroInfo info;
        info.description = std::move(description);
        PrinterStopCall stop = find_printer_stop_call(gcode_template);
        info.stops_printer = stop.calls;
        info.stop_message = std::move(stop.message);
        info.sets_active_spool = template_sets_active_spool(gcode_template);
        info.has_template = !gcode_template.empty();
        info.temperature_use = template_temperature_use(gcode_template);
        if (it.value().is_object()) {
            static constexpr std::string_view kVariablePrefix = "variable_";
            for (const auto& [field, value] : it.value().items()) {
                if (field.size() > kVariablePrefix.size() &&
                    field.compare(0, kVariablePrefix.size(), kVariablePrefix) == 0 &&
                    value.is_string()) {
                    info.variables[field.substr(kVariablePrefix.size())] = value.get<std::string>();
                }
            }
        }
        auto params = parse_macro_params(gcode_template);

        // Note: variable_* keys (Klipper SET_GCODE_VARIABLE state) are
        // intentionally NOT included. They are internal macro state, not
        // user-facing parameters. User params are already extracted from
        // the gcode template (params.NAME references) above.

        if (params.empty()) {
            info.knowledge = MacroParamKnowledge::KNOWN_NO_PARAMS;
        } else {
            info.knowledge = MacroParamKnowledge::KNOWN_PARAMS;
            info.params = std::move(params);
        }

        cache_[macro_name_lower] = std::move(info);
        ++config_count;
    }

    // Mark known macros not found in configfile as UNKNOWN
    size_t unknown_count = 0;
    for (const auto& macro : known_macros) {
        std::string lower = macro;
        to_lower_inplace(lower);
        if (cache_.find(lower) == cache_.end()) {
            cache_[lower] = CachedMacroInfo{MacroParamKnowledge::UNKNOWN, {}};
            ++unknown_count;
        }
    }

    populated_ = true;
    spdlog::info("[MacroParamCache] Populated: {} entries from configfile, {} unknown macros",
                 config_count, unknown_count);
}

namespace {

/// `heater_generic chamber` and a bare `chamber` are both what HEATER=chamber sets.
std::string target_key(const std::string& full_name) {
    static constexpr std::string_view kFan = "temperature_fan ";
    static constexpr std::string_view kGeneric = "heater_generic ";
    std::string key;
    if (full_name.rfind(kFan, 0) == 0) {
        key = "temperature_fan " + full_name.substr(kFan.size());
    } else if (full_name.rfind(kGeneric, 0) == 0) {
        key = "heater " + full_name.substr(kGeneric.size());
    } else {
        key = "heater " + full_name;
    }
    to_lower_inplace(key);
    return key;
}

constexpr int kMaxMacroCallDepth = 4;

std::optional<bool> sets_target(const std::unordered_map<std::string, CachedMacroInfo>& cache,
                                const std::string& macro, const std::string& key,
                                const std::string& any_key, int depth,
                                std::unordered_set<std::string>& visited) {
    const auto it = cache.find(macro);
    if (it == cache.end() || !it->second.has_template) {
        return std::nullopt;
    }
    const auto& use = it->second.temperature_use;
    for (const auto& target : use.targets) {
        if (target == key || target == any_key) {
            return true;
        }
    }
    bool unknown = false;
    if (depth < kMaxMacroCallDepth) {
        for (const auto& command : use.commands) {
            std::string callee = command;
            to_lower_inplace(callee);
            if (cache.find(callee) == cache.end() || !visited.insert(callee).second) {
                continue; // a Klipper command, or a macro already on this path
            }
            const auto r = sets_target(cache, callee, key, any_key, depth + 1, visited);
            if (r == std::optional<bool>(true)) {
                return true;
            }
            unknown = unknown || !r.has_value();
        }
    }
    return unknown ? std::nullopt : std::optional<bool>(false);
}

bool runs_command(const std::unordered_map<std::string, CachedMacroInfo>& cache,
                  const std::string& macro, const std::string& command, int depth,
                  std::unordered_set<std::string>& visited) {
    const auto it = cache.find(macro);
    if (it == cache.end()) {
        return false;
    }
    for (const auto& line_command : it->second.temperature_use.commands) {
        if (line_command == command) {
            return true;
        }
    }
    if (depth >= kMaxMacroCallDepth) {
        return false;
    }
    for (const auto& line_command : it->second.temperature_use.commands) {
        std::string callee = line_command;
        to_lower_inplace(callee);
        if (cache.find(callee) != cache.end() && visited.insert(callee).second &&
            runs_command(cache, callee, command, depth + 1, visited)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool MacroParamCache::macro_runs(const std::string& macro_name, const std::string& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string macro = macro_name;
    to_lower_inplace(macro);
    std::unordered_set<std::string> visited{macro};
    return runs_command(cache_, macro, helix::text_io::to_upper(command), 0, visited);
}

std::optional<bool> MacroParamCache::macro_sets_target(const std::string& macro_name,
                                                       const std::string& target_full_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string macro = macro_name;
    to_lower_inplace(macro);
    const std::string key = target_key(target_full_name);
    const std::string any_key = key.substr(0, key.find(' ') + 1) + "*";
    std::unordered_set<std::string> visited{macro};
    return sets_target(cache_, macro, key, any_key, 0, visited);
}

std::map<std::string, std::string> MacroParamCache::printer_stop_commands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::string> commands;
    for (const auto& [name, info] : cache_) {
        if (!info.stops_printer) {
            continue;
        }
        std::string upper = name;
        upper = helix::text_io::to_upper(upper);
        commands.emplace(std::move(upper), info.stop_message);
    }
    return commands;
}

bool MacroParamCache::is_populated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return populated_;
}

uint64_t MacroParamCache::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

CachedMacroInfo MacroParamCache::get(const std::string& macro_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string lower = macro_name;
    to_lower_inplace(lower);

    auto it = cache_.find(lower);
    if (it != cache_.end()) {
        return it->second;
    }
    return CachedMacroInfo{MacroParamKnowledge::UNKNOWN, {}};
}

bool MacroParamCache::has_macro(const std::string& macro_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string lower = macro_name;
    to_lower_inplace(lower);
    // Every entry in cache_ comes from either configfile or known_macros —
    // both signal the macro is registered with Klipper. The UNKNOWN tag means
    // "no params parsed"; it still indicates the macro exists.
    return cache_.find(lower) != cache_.end();
}

void MacroParamCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    populated_ = false;
    ++generation_;
    spdlog::debug("[MacroParamCache] Cache cleared");
}

} // namespace helix

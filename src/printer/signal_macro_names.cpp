// SPDX-License-Identifier: GPL-3.0-or-later

#include "signal_macro_names.h"

#include "core/text_io.h"

#include <algorithm>
#include <cctype>

#include "hv/json.hpp"

namespace helix::signal_macros {

namespace {

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// `name` appears in `upper` with no word character on either side.
bool has_standalone(const std::string& upper, std::string_view name) {
    for (size_t pos = upper.find(name); pos != std::string::npos; pos = upper.find(name, pos + 1)) {
        const size_t end = pos + name.size();
        if ((pos == 0 || !is_word_char(upper[pos - 1])) &&
            (end == upper.size() || !is_word_char(upper[end]))) {
            return true;
        }
    }
    return false;
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

/// The configfile.settings key of a gcode_macro: Klipper lowercases section names.
std::string macro_key(const std::string& name) {
    return "gcode_macro " + text_io::to_lower(name);
}

enum class Definer { Alias, Pack, User };

/// Who wrote a [gcode_macro <visible signal name>] section, read from its
/// description: configfile.settings does not say which file a section came from.
Definer classify(const nlohmann::json& section, size_t index) {
    if (!section.is_object()) {
        return Definer::User;
    }
    auto desc = section.find("description");
    if (desc == section.end() || !desc->is_string()) {
        return Definer::User;
    }
    const std::string& text = desc->get_ref<const std::string&>();
    if (text == std::string(kAliasDescriptionPrefix) + hidden_name(kVisibleNames[index])) {
        return Definer::Alias;
    }
    if (text == kPackDescriptions[index]) {
        return Definer::Pack;
    }
    return Definer::User;
}

/// Pack sections and our forwarders call only what HelixScreen wrote; a user's
/// own macro under a signal name is theirs and is read like any other.
bool is_scanned(const std::string& section, const std::vector<std::string>& pack_sections,
                const nlohmann::json& config_settings) {
    if (std::find(pack_sections.begin(), pack_sections.end(), section) == pack_sections.end()) {
        return true;
    }
    for (size_t i = 0; i < kVisibleNames.size(); ++i) {
        if (section == macro_key(std::string(kVisibleNames[i]))) {
            return classify(config_settings[section], i) == Definer::User;
        }
    }
    return false;
}

} // namespace

std::string hidden_name(std::string_view visible) {
    return "_" + std::string(visible);
}

std::string alias_block(const std::vector<std::string>& visible_names) {
    std::string out;
    for (auto visible : kVisibleNames) {
        if (std::find(visible_names.begin(), visible_names.end(), visible) == visible_names.end()) {
            continue;
        }
        const std::string hidden = hidden_name(visible);
        if (!out.empty()) {
            out += '\n';
        }
        out += "[gcode_macro ";
        out += visible;
        out += "]\ndescription: ";
        out += kAliasDescriptionPrefix;
        out += hidden + "\ngcode:\n    " + hidden + "\n";
    }
    return out;
}

bool SignalCaller::operator==(const SignalCaller& o) const {
    return section == o.section && option == o.option && name == o.name;
}

SignalMacroScan scan(const nlohmann::json& config_settings,
                     const std::vector<std::string>& pack_sections) {
    SignalMacroScan result;
    if (!config_settings.is_object()) {
        return result;
    }

    std::array<bool, kVisibleNames.size()> alias{};
    std::array<bool, kVisibleNames.size()> foreign{};
    bool all_hidden = true;
    for (size_t i = 0; i < kVisibleNames.size(); ++i) {
        const std::string visible(kVisibleNames[i]);
        all_hidden = all_hidden && config_settings.contains(macro_key(hidden_name(visible)));
        auto it = config_settings.find(macro_key(visible));
        if (it == config_settings.end()) {
            continue;
        }
        switch (classify(*it, i)) {
        case Definer::Alias:
            alias[i] = true;
            result.visible_loaded = true;
            break;
        case Definer::User:
            foreign[i] = true;
            break;
        case Definer::Pack:
            result.visible_loaded = true;
            break;
        }
    }
    result.hidden_loaded = all_hidden;

    for (const auto& [section, options] : config_settings.items()) {
        if (!options.is_object() || !is_scanned(section, pack_sections, config_settings)) {
            continue;
        }
        for (const auto& [option, value] : options.items()) {
            if (!value.is_string()) {
                continue;
            }
            const std::string upper = text_io::to_upper(value.get<std::string>());
            for (auto visible : kVisibleNames) {
                if (has_standalone(upper, visible)) {
                    result.callers.push_back({section, option, std::string(visible)});
                }
            }
        }
    }

    for (size_t i = 0; i < kVisibleNames.size(); ++i) {
        if (alias[i]) {
            result.aliases.emplace_back(kVisibleNames[i]);
        }
        if (foreign[i]) {
            result.foreign.emplace_back(kVisibleNames[i]);
        }
    }
    return result;
}

std::vector<std::string> names_to_alias(const SignalMacroScan& scan,
                                        const std::vector<std::string>& extra_names) {
    // A sliced file can call any signal name, and nothing in the config shows
    // which, so a printer with any visible name in play keeps all of them.
    const bool extra_signal =
        std::any_of(kVisibleNames.begin(), kVisibleNames.end(),
                    [&](std::string_view v) { return contains(extra_names, v); });
    std::vector<std::string> names;
    if (!scan.visible_loaded && scan.callers.empty() && !extra_signal) {
        return names;
    }
    for (auto visible : kVisibleNames) {
        if (!contains(scan.foreign, visible)) {
            names.emplace_back(visible);
        }
    }
    return names;
}

SignalNameDrift drift(const SignalMacroScan& scan, const std::vector<std::string>& extra_names) {
    if (!scan.hidden_loaded) {
        return SignalNameDrift::MoveToHidden;
    }
    const auto wanted = names_to_alias(scan, extra_names);
    for (const auto& name : wanted) {
        if (!contains(scan.aliases, name)) {
            return SignalNameDrift::AddAliases;
        }
    }
    for (const auto& name : scan.aliases) {
        if (!contains(wanted, name)) {
            return SignalNameDrift::DropAliases;
        }
    }
    return SignalNameDrift::None;
}

} // namespace helix::signal_macros

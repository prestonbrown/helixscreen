// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_params.h"

#include "core/text_io.h"
#include "helix_regex.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string_view>

using namespace helix;

namespace {

/// The argument of a `|default(...)` filter that directly opens @p rest, read to
/// its matching parenthesis so nested calls and quoted parentheses stay whole.
/// nullopt when no such filter follows, or its parenthesis never closes.
std::optional<std::string_view> default_filter_argument(std::string_view rest) {
    static constexpr std::string_view KEYWORD = "default";

    rest = helix::text_io::trim(rest);
    if (rest.empty() || rest.front() != '|') {
        return std::nullopt;
    }
    rest = helix::text_io::trim(rest.substr(1));
    if (rest.substr(0, KEYWORD.size()) != KEYWORD) {
        return std::nullopt;
    }
    rest = helix::text_io::trim(rest.substr(KEYWORD.size()));
    if (rest.empty() || rest.front() != '(') {
        return std::nullopt;
    }
    rest.remove_prefix(1);

    int depth = 0;
    char quote = '\0';
    for (size_t i = 0; i < rest.size(); ++i) {
        const char c = rest[i];
        if (quote != '\0') {
            if (c == '\\') {
                ++i; // the escaped character cannot close the string
            } else if (c == quote) {
                quote = '\0';
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {
                return rest.substr(0, i);
            }
            --depth;
        }
    }
    return std::nullopt;
}

/// One quoted string: opens and closes with the same quote, never closing early.
bool is_quoted_string(std::string_view text) {
    if (text.size() < 2) {
        return false;
    }
    const char quote = text.front();
    if ((quote != '\'' && quote != '"') || text.back() != quote) {
        return false;
    }
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] == '\\') {
            if (i + 2 == text.size()) {
                return false; // the closing quote is escaped
            }
            ++i;
        } else if (text[i] == quote) {
            return false;
        }
    }
    return true;
}

MacroDefaultKind classify_default(std::string_view text) {
    static const helix::Regex number_re(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
    static constexpr std::string_view kKeywords[] = {"true",  "True", "false",
                                                     "False", "none", "None"};

    if (text.empty() || is_quoted_string(text) || helix::regex_match(text, number_re)) {
        return MacroDefaultKind::Literal;
    }
    for (std::string_view keyword : kKeywords) {
        if (text == keyword) {
            return MacroDefaultKind::Literal;
        }
    }
    return MacroDefaultKind::Expression;
}

} // namespace

// ============================================================================
// parse_macro_params — extract Klipper gcode_macro parameters from template
// ============================================================================

std::vector<MacroParam> helix::parse_macro_params(const std::string& gcode_template) {
    std::vector<MacroParam> result;
    std::set<std::string> seen;

    // Match params.NAME, params['NAME'], params["NAME"]
    // Optional trailing |default(VALUE) or | default(VALUE)
    helix::Regex param_re(
        R"RE(params\.([A-Za-z_][A-Za-z0-9_]*)|params\['([A-Za-z_][A-Za-z0-9_]*)'\]|params\["([A-Za-z_][A-Za-z0-9_]*)"\])RE");

    auto it = helix::RegexIterator(gcode_template, param_re);
    auto end = helix::RegexIterator();

    for (; it != end; ++it) {
        const auto& match = *it;

        // Extract name from whichever group matched
        std::string name;
        if (match[1].matched)
            name = match[1].str();
        else if (match[2].matched)
            name = match[2].str();
        else if (match[3].matched)
            name = match[3].str();

        // Normalize to uppercase
        name = helix::text_io::to_upper(name);

        // Skip duplicates
        if (seen.count(name)) {
            continue;
        }
        seen.insert(name);

        MacroParam param;
        param.name = name;
        const std::string_view rest = std::string_view(gcode_template).substr(match.end());
        if (auto argument = default_filter_argument(rest)) {
            std::string_view text = helix::text_io::trim(*argument);
            param.default_kind = classify_default(text);
            if (param.default_kind == MacroDefaultKind::Literal && is_quoted_string(text)) {
                text = text.substr(1, text.size() - 2);
            }
            param.default_value = std::string(text);
        }

        result.push_back(std::move(param));
    }

    // Second pass: catch {% if 'NAME' in params %} / {% if "NAME" in params %}
    // Also matches 'not in params'. Skips names already found by dot/bracket access.
    helix::Regex in_params_re(
        R"RE((?:'([A-Za-z_][A-Za-z0-9_]*)'|"([A-Za-z_][A-Za-z0-9_]*)")\s+(?:not\s+)?in\s+params)RE");

    auto it2 = helix::RegexIterator(gcode_template, in_params_re);
    for (; it2 != end; ++it2) {
        const auto& match = *it2;

        std::string name;
        if (match[1].matched)
            name = match[1].str();
        else if (match[2].matched)
            name = match[2].str();

        name = helix::text_io::to_upper(name);

        if (seen.count(name)) {
            continue;
        }
        seen.insert(name);

        // No default value extractable from conditional checks
        result.push_back({name, ""});
    }

    return result;
}

std::map<std::string, std::string>
helix::parse_raw_macro_params(const std::string& raw_text, std::vector<std::string>* rejected) {
    std::map<std::string, std::string> result;
    size_t pos = 0;
    while (pos < raw_text.size()) {
        while (pos < raw_text.size() && std::isspace(static_cast<unsigned char>(raw_text[pos])))
            ++pos;
        if (pos >= raw_text.size())
            break;
        size_t end = raw_text.find_first_of(" \t\n\r", pos);
        if (end == std::string::npos)
            end = raw_text.size();
        std::string token = raw_text.substr(pos, end - pos);
        pos = end;
        auto eq = token.find('=');
        if (eq == std::string::npos || eq == 0) {
            if (rejected)
                rejected->push_back(token);
            continue;
        }
        std::string key = token.substr(0, eq);
        key = helix::text_io::to_upper(key);
        result[key] = token.substr(eq + 1);
    }
    return result;
}

std::string helix::format_raw_macro_params(const std::map<std::string, std::string>& values) {
    std::string text;
    for (const auto& [key, value] : values) {
        if (!text.empty())
            text += ' ';
        text += key;
        text += '=';
        text += value;
    }
    return text;
}

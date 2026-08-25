// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace helix::ui {

/**
 * @file console_filter_engine.h
 * @brief Match-and-suppress engine for the G-code console panel.
 *
 * Holds a list of patterns and exposes a single fast `should_filter(line)` query
 * used by ConsolePanel to drop firmware-internal noise from the displayed
 * stream. Patterns are serialized as `<type>:<text>`:
 *
 *   - `prefix:...`     — line starts with text (cheapest)
 *   - `substring:...`  — line contains text
 *   - `regex:...`      — line matches ECMAScript regex (opt-in; pulls regex runtime)
 *
 * Presets ship per-printer in `assets/config/printer_database.json`. Users may
 * add or remove patterns via SettingsManager.
 */
class ConsoleFilterEngine {
  public:
    enum class Type { Prefix, Substring, Regex };

    /**
     * @brief Add one pattern from its serialized spec ("prefix:foo" / "substring:bar" /
     * "regex:^baz").
     * @return true on success; false on malformed spec, unknown type, or invalid regex.
     */
    bool add(std::string_view spec);

    /// Add multiple patterns; malformed entries are logged and skipped.
    void add_all(const std::vector<std::string>& specs);

    /// Remove every entry whose (type, text) matches a serialized spec. No-op if absent.
    void remove(std::string_view spec);

    /// True if any pattern matches `line`.
    [[nodiscard]] bool should_filter(std::string_view line) const;

    [[nodiscard]] std::size_t size() const {
        return patterns_.size();
    }

    /// Matcher types in the order should_filter() evaluates them. Exposed so the
    /// cheapest-first ordering can be asserted rather than assumed.
    [[nodiscard]] std::vector<Type> types_in_order() const {
        std::vector<Type> out;
        out.reserve(patterns_.size());
        for (const auto& p : patterns_) {
            out.push_back(p.type);
        }
        return out;
    }
    [[nodiscard]] bool empty() const {
        return patterns_.empty();
    }
    void clear() {
        patterns_.clear();
    }

    /// Parse `<type>:<text>` into its components. Returns false on malformed input.
    static bool parse(std::string_view spec, Type& type, std::string& text);

  private:
    struct Pattern {
        Type type{Type::Prefix};
        std::string text;
        std::regex compiled; ///< Populated only when type == Regex
    };

    std::vector<Pattern> patterns_;
};

} // namespace helix::ui

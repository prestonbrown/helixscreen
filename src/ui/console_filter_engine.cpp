// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "console_filter_engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace helix::ui {

bool ConsoleFilterEngine::parse(std::string_view spec, Type& type, std::string& text) {
    if (spec.empty()) {
        return false;
    }
    const auto colon = spec.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const auto kind = spec.substr(0, colon);
    const auto body = spec.substr(colon + 1);
    if (body.empty()) {
        return false;
    }
    if (kind == "prefix") {
        type = Type::Prefix;
    } else if (kind == "substring") {
        type = Type::Substring;
    } else if (kind == "regex") {
        type = Type::Regex;
    } else {
        return false;
    }
    text.assign(body.data(), body.size());
    return true;
}

bool ConsoleFilterEngine::add(std::string_view spec) {
    Pattern p;
    if (!parse(spec, p.type, p.text)) {
        spdlog::warn("[ConsoleFilter] Malformed pattern: '{}'", spec);
        return false;
    }
    if (p.type == Type::Regex) {
        try {
            p.compiled = std::regex(p.text, std::regex::ECMAScript | std::regex::optimize);
        } catch (const std::regex_error& e) {
            spdlog::warn("[ConsoleFilter] Invalid regex '{}': {}", p.text, e.what());
            return false;
        }
    }
    // Keep the list ordered Prefix < Substring < Regex so should_filter() always
    // spends the cheapest matcher first. Measured over a 1220-line capture from a
    // real K1C and K2 Plus (16 patterns, x86 -O2): prefix 17us/1000 lines,
    // substring 88us, std::regex 3796us. Insertion order is otherwise arbitrary —
    // user patterns append after the presets — and a single regex landing ahead of
    // the prefixes would make every console line pay the 200x cost before reaching
    // them. This runs on the LVGL main thread, so that matters.
    const auto at = std::find_if(patterns_.begin(), patterns_.end(),
                                 [&](const Pattern& e) { return e.type > p.type; });
    patterns_.insert(at, std::move(p));
    return true;
}

void ConsoleFilterEngine::add_all(const std::vector<std::string>& specs) {
    for (const auto& s : specs) {
        add(s);
    }
}

void ConsoleFilterEngine::remove(std::string_view spec) {
    Type type;
    std::string text;
    if (!parse(spec, type, text)) {
        return;
    }
    patterns_.erase(
        std::remove_if(patterns_.begin(), patterns_.end(),
                       [&](const Pattern& p) { return p.type == type && p.text == text; }),
        patterns_.end());
}

bool ConsoleFilterEngine::should_filter(std::string_view line) const {
    for (const auto& p : patterns_) {
        switch (p.type) {
        case Type::Prefix:
            if (line.size() >= p.text.size() && line.compare(0, p.text.size(), p.text) == 0) {
                return true;
            }
            break;
        case Type::Substring:
            if (line.find(p.text) != std::string_view::npos) {
                return true;
            }
            break;
        case Type::Regex:
            if (std::regex_search(std::string(line), p.compiled)) {
                return true;
            }
            break;
        }
    }
    return false;
}

} // namespace helix::ui

// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "afc_fault_position.h"

#include "core/text_io.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace helix::afc {

namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// A needle only counts when it sits on word boundaries. AFC prefixes the lane
/// name (`lane1 filament failed to trigger …`) and Klipper may prefix `!!`, so the
/// needle is never anchored at position 0 — but neither may it match mid-word.
bool contains_word(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool hit = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != needle[j]) {
                hit = false;
                break;
            }
        }
        if (!hit)
            continue;
        const bool left_ok = (i == 0) || !is_word(haystack[i - 1]);
        const size_t after = i + needle.size();
        const bool right_ok = (after >= haystack.size()) || !is_word(haystack[after]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

struct Rule {
    std::string_view needle; ///< lowercase; matched case-insensitively on word boundaries
    PathSegment segment;
};

/// The five AFC error sites that ship a position diagram, mapped to how far the
/// filament actually got. `post` precedes `pre` only for defensiveness — the two
/// needles are not substrings of one another.
constexpr std::array<Rule, 5> kRules{{
    // AFC.py:1469 — 'Current lane not loaded, LOAD TRIGGER NOT TRIGGERED'
    {"load trigger not triggered", PathSegment::SPOOL},
    // AFC_BoxTurtle.py:527 — ' FAILED TO LOAD, CHECK FILAMENT AT TRIGGER'
    {"check filament at trigger", PathSegment::SPOOL},
    // AFC.py:1294 — 'filament did not trigger hub sensor, CHECK FILAMENT PATH'
    {"did not trigger hub sensor", PathSegment::HUB},
    // AFC.py:1370 — 'filament failed to trigger post extruder gear toolhead sensor'
    {"post extruder gear toolhead sensor", PathSegment::TOOLHEAD},
    // AFC.py:1345 — 'filament failed to trigger pre extruder gear toolhead sensor'
    {"pre extruder gear toolhead sensor", PathSegment::OUTPUT},
}};

/// The bar row: `||=====||==>--||-----||`. Only pipes, fill, the filament head and
/// spaces, and it must actually contain pipes — otherwise a plain `---` separator
/// line in some future message would qualify.
bool is_diagram_row(std::string_view line) {
    line = helix::text_io::trim(line);
    if (line.empty())
        return false;
    bool has_pipe = false;
    for (char c : line) {
        if (c == '|')
            has_pipe = true;
        else if (c != '=' && c != '-' && c != '>' && c != ' ')
            return false;
    }
    return has_pipe;
}

/// The label row: `TRG   LOAD   HUB   TOOL` (AFC.py:1294 ends it `TOOL.`). Pinned on
/// all four labels in order so a sentence merely mentioning HUB cannot be eaten.
bool is_label_row(std::string_view line) {
    static constexpr std::array<std::string_view, 4> kLabels{"trg", "load", "hub", "tool"};

    size_t label = 0;
    size_t i = 0;
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };

    while (i < line.size()) {
        while (i < line.size() && is_space(line[i]))
            ++i;
        if (i >= line.size())
            break;
        size_t start = i;
        while (i < line.size() && !is_space(line[i]))
            ++i;
        std::string_view token = line.substr(start, i - start);
        // AFC.py:1294 writes the last label as `TOOL.`
        if (!token.empty() && token.back() == '.')
            token.remove_suffix(1);
        if (label >= kLabels.size() || token.size() != kLabels[label].size())
            return false;
        for (size_t j = 0; j < token.size(); ++j) {
            if (lower(token[j]) != kLabels[label][j])
                return false;
        }
        ++label;
    }
    return label == kLabels.size();
}

} // namespace

std::optional<PathSegment> afc_fault_position(std::string_view message) {
    for (const auto& rule : kRules) {
        if (contains_word(message, rule.needle))
            return rule.segment;
    }
    return std::nullopt;
}

std::string afc_strip_position_diagram(std::string_view message) {
    // Recognition gates the edit: anything we did not understand comes back byte-for-byte.
    if (!afc_fault_position(message))
        return std::string(message);

    std::string out;
    out.reserve(message.size());

    size_t pos = 0;
    bool first = true;
    while (pos <= message.size()) {
        const size_t nl = message.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? message.size() : nl;
        std::string_view line = message.substr(pos, end - pos);

        // Tolerate CRLF when deciding, but keep surviving lines exactly as they came.
        std::string_view probe = line;
        if (!probe.empty() && probe.back() == '\r')
            probe.remove_suffix(1);

        if (!is_diagram_row(probe) && !is_label_row(probe)) {
            if (!first)
                out += '\n';
            out.append(line);
            first = false;
        }

        if (nl == std::string_view::npos)
            break;
        pos = nl + 1;
    }
    return out;
}

} // namespace helix::afc

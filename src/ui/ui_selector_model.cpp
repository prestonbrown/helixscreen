// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_selector_model.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace helix::ui {

namespace {

std::string to_lower(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (unsigned char c : s) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower;
}

std::string trimmed(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\n\r\f\v");
    if (begin == std::string::npos) {
        return "";
    }
    return s.substr(begin, s.find_last_not_of(" \t\n\r\f\v") - begin + 1);
}

} // namespace

std::string selector_bucket_of(const SelectorEntry& entry) {
    return entry.group.empty() ? entry.label : entry.group;
}

bool selector_entry_matches(const SelectorEntry& entry, const std::string& query) {
    const std::string normalized = to_lower(trimmed(query));
    if (normalized.empty()) {
        return true;
    }
    return to_lower(entry.label).find(normalized) != std::string::npos ||
           to_lower(entry.group).find(normalized) != std::string::npos;
}

std::vector<const SelectorEntry*> filter_selector_entries(const std::vector<SelectorEntry>& entries,
                                                          const std::string& query) {
    std::vector<const SelectorEntry*> matches;
    matches.reserve(entries.size());
    for (const auto& entry : entries) {
        if (selector_entry_matches(entry, query)) {
            matches.push_back(&entry);
        }
    }
    return matches;
}

std::vector<SelectorGroup> group_selector_entries(const std::vector<SelectorEntry>& entries) {
    // Ordered map: buckets come out sorted by name, matching the alphabetical
    // tile grid the wizard renders.
    std::map<std::string, std::vector<const SelectorEntry*>> buckets;
    for (const auto& entry : entries) {
        buckets[selector_bucket_of(entry)].push_back(&entry);
    }

    std::vector<SelectorGroup> groups;
    groups.reserve(buckets.size());
    for (auto& [name, members] : buckets) {
        groups.push_back(SelectorGroup{name, std::move(members)});
    }
    return groups;
}

std::string selector_group_name(const std::vector<SelectorEntry>& entries,
                                const std::string& label) {
    for (const auto& entry : entries) {
        if (entry.label == label) {
            return selector_bucket_of(entry);
        }
    }
    return "";
}

} // namespace helix::ui

// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_selector_model.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace helix::ui {

std::string selector_bucket_of(const SelectorEntry& entry) {
    return entry.group.empty() ? entry.label : entry.group;
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

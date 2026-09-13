// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_selector_model.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::filter_selector_entries;
using helix::ui::group_selector_entries;
using helix::ui::selector_entry_matches;
using helix::ui::selector_group_name;
using helix::ui::SelectorEntry;
using helix::ui::SelectorGroup;

namespace {

// A slice of the real printer list shape: real machines carry a manufacturer,
// the pseudo-machines ("Custom/Other", "Unknown") carry none. "PFA Micron"
// stands in for the vendors whose model names do not contain the vendor
// string (PrintersForAnts, Positron3D) — label-only matching misses those.
std::vector<SelectorEntry> sample_entries() {
    return {
        {"Anycubic Kobra 2", "Anycubic", 0},
        {"Creality K1 Max", "Creality", 1},
        {"Creality K2 Plus", "Creality", 2},
        {"FlashForge Adventurer 5M", "FlashForge", 3},
        {"PFA Micron", "PrintersForAnts", 4},
        {"Qidi X-Max 3", "Qidi", 5},
        {"Custom/Other", "", 6},
        {"Unknown", "", 7},
    };
}

std::vector<std::string> labels_of(const std::vector<const SelectorEntry*>& entries) {
    std::vector<std::string> labels;
    labels.reserve(entries.size());
    for (const auto* e : entries) {
        labels.push_back(e->label);
    }
    return labels;
}

const SelectorGroup* find_group(const std::vector<SelectorGroup>& groups, const std::string& name) {
    auto it = std::find_if(groups.begin(), groups.end(),
                           [&](const SelectorGroup& g) { return g.name == name; });
    return it == groups.end() ? nullptr : &*it;
}

} // namespace

// ============================================================================
// Matching
// ============================================================================

TEST_CASE("Selector: empty query matches every entry", "[selector]") {
    const auto entries = sample_entries();
    CHECK(filter_selector_entries(entries, "").size() == entries.size());
    CHECK(filter_selector_entries(entries, "   ").size() == entries.size());
}

TEST_CASE("Selector: query matches the model name case-insensitively", "[selector]") {
    const auto entries = sample_entries();
    CHECK(labels_of(filter_selector_entries(entries, "kobra")) ==
          std::vector<std::string>{"Anycubic Kobra 2"});
    CHECK(labels_of(filter_selector_entries(entries, "K2 PLUS")) ==
          std::vector<std::string>{"Creality K2 Plus"});
    // Substring, not prefix: mid-name model codes match too.
    CHECK(labels_of(filter_selector_entries(entries, "5m")) ==
          std::vector<std::string>{"FlashForge Adventurer 5M"});
}

TEST_CASE("Selector: query matches the vendor and surfaces all its machines", "[selector]") {
    const auto entries = sample_entries();
    const auto hits = filter_selector_entries(entries, "creality");
    CHECK(labels_of(hits) == std::vector<std::string>{"Creality K1 Max", "Creality K2 Plus"});
}

TEST_CASE("Selector: query matching only the vendor, not any model name, still hits",
          "[selector]") {
    const auto entries = sample_entries();
    // "ants" appears in no model label — only in the PrintersForAnts bucket.
    CHECK(labels_of(filter_selector_entries(entries, "ants")) ==
          std::vector<std::string>{"PFA Micron"});
    CHECK(labels_of(filter_selector_entries(entries, "printersforants")) ==
          std::vector<std::string>{"PFA Micron"});
}

TEST_CASE("Selector: pseudo-machines match on their own label", "[selector]") {
    const auto entries = sample_entries();
    CHECK(labels_of(filter_selector_entries(entries, "custom")) ==
          std::vector<std::string>{"Custom/Other"});
    CHECK(labels_of(filter_selector_entries(entries, "unknown")) ==
          std::vector<std::string>{"Unknown"});
}

TEST_CASE("Selector: query matching nothing yields an empty list", "[selector]") {
    const auto entries = sample_entries();
    CHECK(filter_selector_entries(entries, "prusa xl").empty());
    CHECK(filter_selector_entries(entries, "zzz").empty());
}

TEST_CASE("Selector: filter_selector_entries is exactly the entries the predicate accepts",
          "[selector]") {
    const auto entries = sample_entries();
    for (const char* query : {"", "k1", "CREALITY", "5m", "zzz"}) {
        INFO("query=" << query);
        std::vector<std::string> expected;
        for (const auto& e : entries) {
            if (selector_entry_matches(e, query)) {
                expected.push_back(e.label);
            }
        }
        CHECK(labels_of(filter_selector_entries(entries, query)) == expected);
    }
}

// ============================================================================
// Grouping
// ============================================================================

TEST_CASE("Selector: grouping puts every entry in exactly one bucket", "[selector]") {
    const auto entries = sample_entries();
    const auto groups = group_selector_entries(entries);

    // Every entry appears once across all buckets...
    size_t total = 0;
    std::vector<std::string> seen;
    for (const auto& g : groups) {
        for (const auto* e : g.entries) {
            seen.push_back(e->label);
            ++total;
        }
    }
    CHECK(total == entries.size());
    std::sort(seen.begin(), seen.end());
    std::vector<std::string> expected;
    for (const auto& e : entries) {
        expected.push_back(e.label);
    }
    std::sort(expected.begin(), expected.end());
    CHECK(seen == expected);

    // ...and no label is claimed by two buckets.
    const auto unique_end = std::unique(seen.begin(), seen.end());
    CHECK(unique_end == seen.end());
}

TEST_CASE("Selector: buckets are named for the vendor and ordered alphabetically", "[selector]") {
    const auto groups = group_selector_entries(sample_entries());
    std::vector<std::string> names;
    for (const auto& g : groups) {
        names.push_back(g.name);
    }
    CHECK(names == std::vector<std::string>{"Anycubic", "Creality", "Custom/Other", "FlashForge",
                                            "PrintersForAnts", "Qidi", "Unknown"});
}

TEST_CASE("Selector: a vendor bucket holds exactly that vendor's machines, in order",
          "[selector]") {
    const auto entries = sample_entries();
    const auto groups = group_selector_entries(entries);

    const auto* creality = find_group(groups, "Creality");
    REQUIRE(creality != nullptr);
    CHECK(labels_of(creality->entries) ==
          std::vector<std::string>{"Creality K1 Max", "Creality K2 Plus"});

    const auto* anycubic = find_group(groups, "Anycubic");
    REQUIRE(anycubic != nullptr);
    CHECK(labels_of(anycubic->entries) == std::vector<std::string>{"Anycubic Kobra 2"});
}

TEST_CASE("Selector: entries with no group become singleton buckets named for themselves",
          "[selector]") {
    const auto groups = group_selector_entries(sample_entries());

    const auto* custom = find_group(groups, "Custom/Other");
    REQUIRE(custom != nullptr);
    CHECK(labels_of(custom->entries) == std::vector<std::string>{"Custom/Other"});

    const auto* unknown = find_group(groups, "Unknown");
    REQUIRE(unknown != nullptr);
    CHECK(labels_of(unknown->entries) == std::vector<std::string>{"Unknown"});
}

TEST_CASE("Selector: an empty entry list groups to nothing", "[selector]") {
    CHECK(group_selector_entries({}).empty());
}

// ============================================================================
// Prior-selection drill-in target
// ============================================================================

TEST_CASE("Selector: a real machine's group resolves to its vendor", "[selector]") {
    const auto entries = sample_entries();
    CHECK(selector_group_name(entries, "Creality K2 Plus") == "Creality");
    CHECK(selector_group_name(entries, "FlashForge Adventurer 5M") == "FlashForge");
}

TEST_CASE("Selector: a pseudo-machine resolves to its own bucket, not a vendor's", "[selector]") {
    const auto entries = sample_entries();
    CHECK(selector_group_name(entries, "Custom/Other") == "Custom/Other");
    CHECK(selector_group_name(entries, "Unknown") == "Unknown");
}

TEST_CASE("Selector: the group a label resolves to exists and contains the label", "[selector]") {
    const auto entries = sample_entries();
    const auto groups = group_selector_entries(entries);
    for (const auto& e : entries) {
        const auto group = selector_group_name(entries, e.label);
        INFO("label=" << e.label << " group=" << group);
        REQUIRE(!group.empty());
        const auto* bucket = find_group(groups, group);
        REQUIRE(bucket != nullptr);
        CHECK(std::find_if(bucket->entries.begin(), bucket->entries.end(),
                           [&](const SelectorEntry* hit) { return hit->label == e.label; }) !=
              bucket->entries.end());
    }
}

TEST_CASE("Selector: an absent label resolves to no group", "[selector]") {
    CHECK(selector_group_name(sample_entries(), "Bambu X1C").empty());
    CHECK(selector_group_name({}, "Creality K1").empty());
}

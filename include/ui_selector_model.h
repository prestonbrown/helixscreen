// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/text_io.h"

#include <cctype>
#include <string>
#include <vector>

namespace helix::ui {

/**
 * @file ui_selector_model.h
 * @brief Pure selection-model helpers for long machine/widget lists.
 *
 * A "selector" screen offers the user one pick out of a long list that is too
 * large to scroll comfortably on small panels: the wizard's printer models
 * (~105 entries), the widget catalog (#1016), any future capability picker.
 * The interaction is always the same two moves — type-to-filter across every
 * entry, and drill into one group at a time — so the classification lives here
 * as pure functions over a plain entry list and every caller maps the result to
 * its own widgets.
 *
 * Nothing in this header touches LVGL: the callers own rendering.
 */

/**
 * @brief One choosable item in a selector list.
 *
 * Entries are sourced by the caller from its own domain data. For the wizard,
 * `label` is the printer model name, `group` the manufacturer and `id` the
 * index into PrinterDetector's kinematics-filtered list. The widget catalog
 * (#1016) additionally fills `description` so search reaches the one-line
 * blurb shown under each widget's name.
 */
struct SelectorEntry {
    /// Displayed and searched text (the model / widget name).
    std::string label;
    /// Bucket the entry drills into (vendor / category). Empty entries become
    /// singleton buckets named after their own label.
    std::string group;
    /// Caller's handle delivered back on selection.
    int id = 0;
    /// Secondary searched text (the entry's description). Empty = label and
    /// group only, which is every wizard entry.
    std::string description;
};

/**
 * @brief One drill-in bucket.
 */
struct SelectorGroup {
    std::string name;
    /// Entries in input order.
    std::vector<const SelectorEntry*> entries;
};

/**
 * @brief The bucket an entry drills into: its group, or its own label when it
 * has none (pseudo-machines).
 *
 * This is the one authority for bucket membership — grouping, drill-in
 * targeting and row filtering all answer "entry X belongs to bucket
 * selector_bucket_of(X)".
 */
std::string selector_bucket_of(const SelectorEntry& entry);

/**
 * @brief True when a query is empty or whitespace-only — the "show everything"
 * state. Callers use it to decide between the grouped browse view and the
 * filtered list; selector_entry_matches() treats such a query as matching all.
 */
inline bool selector_query_is_blank(const std::string& query) {
    return helix::text_io::trim(query).empty();
}

/**
 * @brief Case-insensitive substring match of a query against an entry.
 *
 * Matches the label, the group, or the description, so typing a vendor name
 * surfaces every machine it makes and typing a trait ("webcam") surfaces the
 * widget whose description carries it. An empty (or whitespace-only) query
 * matches everything.
 */
inline bool selector_entry_matches(const SelectorEntry& entry, const std::string& query) {
    const std::string normalized =
        helix::text_io::to_lower(std::string(helix::text_io::trim(query)));
    if (normalized.empty()) {
        return true;
    }
    return helix::text_io::to_lower(entry.label).find(normalized) != std::string::npos ||
           helix::text_io::to_lower(entry.group).find(normalized) != std::string::npos ||
           helix::text_io::to_lower(entry.description).find(normalized) != std::string::npos;
}

/**
 * @brief Entries matching a query, in input order. Empty query = all entries.
 *
 * The result borrows from @p entries, so @p entries must outlive it. Passing a
 * temporary leaves every returned pointer dangling.
 */
std::vector<const SelectorEntry*> filter_selector_entries(const std::vector<SelectorEntry>& entries,
                                                          const std::string& query);

/**
 * @brief Partition entries into drill-in buckets, ordered by bucket name.
 *
 * Every input entry lands in exactly one bucket. Entries whose group is empty
 * (the wizard's "Custom/Other" and "Unknown" pseudo-machines) each get their
 * own singleton bucket named after the entry's label, so they stay reachable
 * from the tile grid without inventing an "Other" bucket that would compete
 * with real vendors.
 *
 * Each bucket borrows from @p entries, so @p entries must outlive the returned
 * groups. Passing a temporary leaves every SelectorGroup::entries pointer
 * dangling.
 */
std::vector<SelectorGroup> group_selector_entries(const std::vector<SelectorEntry>& entries);

/**
 * @brief The bucket name a label belongs to, or "" when no entry has it.
 *
 * This is the drill-in target for an already-selected machine: the wizard
 * auto-opens this bucket and scrolls to the entry inside it.
 */
std::string selector_group_name(const std::vector<SelectorEntry>& entries,
                                const std::string& label);

} // namespace helix::ui

// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_selection_state.h"

#include <functional>

namespace helix::gcode {

namespace {

/// splitmix64 finalizer. Scrambles a std::hash result so that SUMMING the
/// scrambled values gives a commutative combine with decent distribution --
/// commutative is the requirement, because two equal sets must hash equally no
/// matter what order they happen to iterate in.
inline size_t mix64(size_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

size_t hash_name_set(const std::unordered_set<std::string>& names) {
    size_t acc = 0;
    for (const auto& n : names) {
        acc += mix64(std::hash<std::string>{}(n));
    }
    // Fold in the count so an empty set and a set that happens to sum to zero
    // cannot collide, then scramble once more.
    return mix64(acc ^ names.size());
}

} // namespace

InvalidationScope SelectionState::set_excluded(const std::unordered_set<std::string>& names) {
    if (names == excluded_) {
        return InvalidationScope::None;
    }
    excluded_ = names;
    refresh_flags();
    // The ghost pass dims excluded objects, so its cache is stale too.
    return InvalidationScope::SolidAndGhost;
}

InvalidationScope SelectionState::set_highlighted(const std::unordered_set<std::string>& names) {
    if (names == highlighted_) {
        return InvalidationScope::None;
    }
    highlighted_ = names;
    highlighted_hash_ = hash_name_set(highlighted_);
    refresh_flags();
    // The ghost pass draws the selection halo too - it is what is visible for
    // most of a print - so its cache is stale as well. Measured at 7ms for 219
    // layers / 128k segments, so the earlier "multi-second re-render" worry that
    // justified returning SolidCache here was inherited from a comment, not
    // measured.
    return InvalidationScope::SolidAndGhost;
}

void SelectionState::rebuild_index_map(const std::vector<std::string>& name_table) {
    name_table_ = name_table;
    refresh_flags();
}

void SelectionState::refresh_flags() {
    flags_.assign(name_table_.size(), 0);
    if (excluded_.empty() && highlighted_.empty()) {
        return;
    }
    for (size_t i = 0; i < name_table_.size(); ++i) {
        const std::string& name = name_table_[i];
        if (name.empty()) {
            continue;
        }
        uint8_t f = 0;
        if (excluded_.count(name) > 0) {
            f |= kExcludedBit;
        }
        if (highlighted_.count(name) > 0) {
            f |= kHighlightedBit;
        }
        flags_[i] = f;
    }
}

} // namespace helix::gcode

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file gcode_selection_state.h
 * @brief Which objects are excluded or selected, and what a change to that costs.
 *
 * Two jobs, done here once so every renderer does them the same way.
 *
 * 1. Per-segment classification, on the INTERNED INDEX rather than the name.
 *    On the isometric view's hot Bresenham cache path,
 *    GCodeLayerRenderer::resolve_object_name() returns std::string BY VALUE, so
 *    classifying by name allocates a string for every segment of every layer.
 *
 *    That signature cannot simply be changed to return a reference:
 *    GCodeStreamingController::get_object_name() copies under name_table_mutex_
 *    because remap_object_name_indices() mutates merged_object_name_table_ from
 *    another thread, so a reference into it could dangle. Classifying on the
 *    index sidesteps the problem entirely: the per-segment test becomes an array
 *    lookup, with no string and no lock.
 *
 * 2. Invalidation scope. Any real change invalidates both caches: the ghost
 *    pass dims excluded objects and draws the selection tag and rim, and the
 *    ghost is what shows for the unprinted part of a print. An unchanged set
 *    invalidates nothing, so repeated calls do not restart the ghost build.
 *
 * Thread safety: the index map is a SNAPSHOT of the name table taken on the main
 * thread. It is never a view into a table another thread can mutate, which is
 * what makes classify() safe to call from the background ghost thread.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace helix::gcode {

/// How much cached work a selection change invalidates.
enum class InvalidationScope {
    /// Not `None`: X11 defines that as a macro (`#define None 0L`), so any build
    /// that reaches an X11 header - the Debian and Raspberry Pi desktop targets -
    /// preprocesses `InvalidationScope::None` into a numeric constant and fails to
    /// compile. The printer targets never include X11, so only the desktop
    /// targets hit it.
    Nothing,       ///< contents unchanged; do not touch either cache
    SolidAndGhost, ///< both: the ghost pass dims exclusions and draws the selection
};

/// Result of classifying one segment's object.
struct SelectionFlags {
    bool excluded = false;
    bool highlighted = false;
};

class SelectionState {
  public:
    /// Replace the excluded set. Returns what the caller must invalidate.
    InvalidationScope set_excluded(const std::unordered_set<std::string>& names);

    /// Replace the highlighted (selected) set. Returns what to invalidate.
    InvalidationScope set_highlighted(const std::unordered_set<std::string>& names);

    /**
     * @brief Snapshot an object-name table and rebuild the index map from it.
     *
     * Call when geometry loads, and again in streaming mode whenever the merged
     * name table has grown. Cheap enough to call per layer batch; it is O(names),
     * not O(segments).
     */
    void rebuild_index_map(const std::vector<std::string>& name_table);

    /**
     * @brief Classify one segment by its interned object index.
     *
     * Hot path: one bounds check and one byte load. An index of -1 (a segment
     * outside any EXCLUDE_OBJECT block -- purge lines, prime blobs, wipe towers)
     * or an index past the snapshot reports neither. The latter happens in
     * streaming mode when a layer loads an object the map has not seen yet;
     * reporting neither for one frame is correct, because the next rebuild picks
     * it up.
     */
    SelectionFlags classify(int16_t index) const {
        if (index < 0 || static_cast<size_t>(index) >= flags_.size()) {
            return {};
        }
        const uint8_t f = flags_[static_cast<size_t>(index)];
        return SelectionFlags{(f & kExcludedBit) != 0, (f & kHighlightedBit) != 0};
    }

    /// Gate for the halo and shell passes, which is what makes an unselected
    /// plate cost nothing.
    bool any_highlighted() const {
        return !highlighted_.empty();
    }
    bool any_excluded() const {
        return !excluded_.empty();
    }

    /**
     * @brief Order-independent hash of the highlighted set.
     *
     * GCodeGLESRenderer's frame-skip compares this to decide whether it can blit
     * the previous frame instead of re-rendering. Must be stable for equal sets
     * regardless of insertion order: the previous inline version folded the
     * running accumulator into each step, so it depended on unordered_set
     * iteration order and could report a spurious change.
     */
    size_t highlighted_hash() const {
        return highlighted_hash_;
    }

    const std::unordered_set<std::string>& excluded() const {
        return excluded_;
    }
    const std::unordered_set<std::string>& highlighted() const {
        return highlighted_;
    }

  private:
    static constexpr uint8_t kExcludedBit = 1u << 0;
    static constexpr uint8_t kHighlightedBit = 1u << 1;

    void refresh_flags();

    std::unordered_set<std::string> excluded_;
    std::unordered_set<std::string> highlighted_;

    /// Snapshot of the name table, retained so a selection change arriving after
    /// the map was built can refresh it without waiting for another rebuild.
    std::vector<std::string> name_table_;

    /// Parallel to name_table_: kExcludedBit | kHighlightedBit per index.
    std::vector<uint8_t> flags_;

    size_t highlighted_hash_ = 0;
};

} // namespace helix::gcode

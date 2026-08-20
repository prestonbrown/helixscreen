// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/gcode_selection_state.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {
const std::vector<std::string> kTable = {"cube_1", "cube_2", "calicat", "stand_s"};
}

// ---------------------------------------------------------------------------
// Classification is by INTERNED INDEX, not by name.
//
// The renderers used to resolve an index to a std::string per segment per frame
// and then do `!name.empty() && set.count(name) > 0` — eight near-identical
// copies of that test across three files. On the isometric view's hot Bresenham
// cache path, GCodeLayerRenderer::resolve_object_name() returns by value, so
// every segment of every layer allocated a string just to ask whether its object
// was selected.
//
// It cannot simply be changed to return a reference: the streaming controller's
// get_object_name() copies under name_table_mutex_ because
// remap_object_name_indices() mutates merged_object_name_table_ from another
// thread, so a reference into it could dangle. Classifying on the index sidesteps
// the whole problem: the per-segment test becomes an array lookup with no string
// and no lock.
// ---------------------------------------------------------------------------

TEST_CASE("classify resolves excluded and highlighted state by index", "[gcode_selection_state]") {
    SelectionState s;
    s.set_excluded({"cube_1"});
    s.set_highlighted({"calicat"});
    s.rebuild_index_map(kTable);

    REQUIRE(s.classify(0).excluded == true);
    REQUIRE(s.classify(0).highlighted == false);
    REQUIRE(s.classify(2).highlighted == true);
    REQUIRE(s.classify(2).excluded == false);
    REQUIRE(s.classify(1).excluded == false);
    REQUIRE(s.classify(1).highlighted == false);
}

TEST_CASE("a segment with no object classifies as neither", "[gcode_selection_state]") {
    // object_name_index is -1 for segments emitted outside any
    // EXCLUDE_OBJECT_START/END block (purge lines, prime blobs, wipe towers).
    SelectionState s;
    s.set_excluded({"cube_1"});
    s.rebuild_index_map(kTable);
    REQUIRE(s.classify(-1).excluded == false);
    REQUIRE(s.classify(-1).highlighted == false);
}

TEST_CASE("an index past the mapped table is safe", "[gcode_selection_state]") {
    // In streaming mode the merged name table grows as layers load, so a segment
    // can carry an index the map has not seen yet. This must not read out of
    // bounds; reporting "neither" for one frame is correct, because the next
    // rebuild picks it up.
    SelectionState s;
    s.set_excluded({"cube_1"});
    s.rebuild_index_map(kTable);
    REQUIRE(s.classify(99).excluded == false);
    REQUIRE(s.classify(99).highlighted == false);
}

TEST_CASE("classification survives a table that grows", "[gcode_selection_state]") {
    SelectionState s;
    s.set_highlighted({"late_arrival"});
    s.rebuild_index_map(kTable);
    REQUIRE(s.classify(4).highlighted == false); // not in the table yet

    std::vector<std::string> grown = kTable;
    grown.push_back("late_arrival");
    s.rebuild_index_map(grown);
    REQUIRE(s.classify(4).highlighted == true);
}

TEST_CASE("selection applied after the index map is built still classifies",
          "[gcode_selection_state]") {
    // Order independence: a tap changes the selection long after geometry loaded,
    // so the setters must refresh the index map themselves rather than relying on
    // a later rebuild_index_map() call that may never come.
    SelectionState s;
    s.rebuild_index_map(kTable);
    s.set_highlighted({"stand_s"});
    REQUIRE(s.classify(3).highlighted == true);
}

// ---------------------------------------------------------------------------
// Invalidation scope. A highlight change currently calls invalidate_cache(),
// which also clears the ghost cache and resets ghost_rendered_up_to_, restarting
// a multi-second background ghost render — even though the ghost pass never
// renders highlight at all (it handles exclusion only). Exclusion must still
// invalidate ghost, because the ghost pass does dim excluded objects.
// ---------------------------------------------------------------------------

TEST_CASE("an unchanged selection requires no invalidation", "[gcode_selection_state]") {
    SelectionState s;
    s.set_highlighted({"cube_1"});
    REQUIRE(s.set_highlighted({"cube_1"}) == InvalidationScope::None);
    s.set_excluded({"cube_2"});
    REQUIRE(s.set_excluded({"cube_2"}) == InvalidationScope::None);
}

TEST_CASE("a highlight change does not invalidate the ghost cache", "[gcode_selection_state]") {
    SelectionState s;
    REQUIRE(s.set_highlighted({"cube_1"}) == InvalidationScope::SolidCache);
}

TEST_CASE("an exclusion change invalidates the ghost cache too", "[gcode_selection_state]") {
    SelectionState s;
    REQUIRE(s.set_excluded({"cube_1"}) == InvalidationScope::SolidAndGhost);
}

TEST_CASE("clearing a selection is a change", "[gcode_selection_state]") {
    SelectionState s;
    s.set_highlighted({"cube_1"});
    REQUIRE(s.set_highlighted({}) == InvalidationScope::SolidAndGhost);
}

// ---------------------------------------------------------------------------
// Hash memoization. GCodeGLESRenderer's frame-skip compares highlight_set_hash
// to decide whether it can blit the previous frame instead of re-rendering, so
// the hash must be stable for equal sets regardless of insertion order, and must
// change when the set changes. Get this wrong and the 3D view either renders
// every frame (perf) or never updates the selection (correctness).
// ---------------------------------------------------------------------------

TEST_CASE("equal highlight sets hash equally regardless of order", "[gcode_selection_state]") {
    SelectionState a, b;
    a.set_highlighted({"cube_1", "cube_2", "calicat"});
    b.set_highlighted({"calicat", "cube_1", "cube_2"});
    REQUIRE(a.highlighted_hash() == b.highlighted_hash());
}

TEST_CASE("a different highlight set hashes differently", "[gcode_selection_state]") {
    SelectionState a, b;
    a.set_highlighted({"cube_1"});
    b.set_highlighted({"cube_2"});
    REQUIRE(a.highlighted_hash() != b.highlighted_hash());
}

TEST_CASE("an emptied highlight set reports no selection", "[gcode_selection_state]") {
    // The halo and shell passes are both gated on this, so it is what makes the
    // unselected case cost nothing.
    SelectionState s;
    s.set_highlighted({"cube_1"});
    REQUIRE(s.any_highlighted() == true);
    s.set_highlighted({});
    REQUIRE(s.any_highlighted() == false);
}

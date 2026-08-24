#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_series_meta_indexing.py — the series_meta
# slot-vs-handle gate.
#
# ui_temp_graph_t::series_meta is indexed by SLOT (first free entry of a
# fixed 16-slot array). meta->id is a monotonically increasing handle
# (next_series_id++) that is never reused. ui_temp_graph_remove_series frees
# a slot without lowering next_series_id, so after one remove-then-add the
# same number means two different things. Indexing series_meta[] with a
# TempGraphHit::series_id therefore renders the wrong series, and past 16
# add/remove cycles reads off the end of the array. That shipped once in
# temp_graph_tooltip_draw_cb and was caught in review rather than by a test,
# because the only symptom is drawn pixels and this repo has no draw-pass
# readback. Hence a gate.
#
# The gate takes --scan-dir, so these tests point it at a fixture tree rather
# than copying the script. Both halves of the contract are pinned: the shapes
# it must catch, and the idioms it must stay quiet about. The silent cases
# matter as much as the loud ones — a gate that fires on legitimate code gets
# switched off, and then it protects nothing.

GATE="scripts/check_series_meta_indexing.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/series_meta"
    mkdir -p "$FIXTURE_DIR/src/ui"
}

# Write $1 as the fixture source file, then run the real gate against it.
run_gate() {
    printf '%s\n' "$1" > "$FIXTURE_DIR/src/ui/fixture.cpp"
    run python3 "$GATE" --scan-dir "$FIXTURE_DIR"
}

# ---------------------------------------------------------------- must catch

@test "catches indexing by a pin's series_id" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    const ui_temp_series_meta_t* meta = &graph->series_meta[pin->series_id];
    (void)meta;
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"index series_meta[] by a series id"* ]]
}

@test "catches indexing by a by-value hit's series_id" {
    run_gate 'void draw(ui_temp_graph_t* graph, TempGraphHit hit) {
    auto* meta = &graph->series_meta[hit.series_id];
    (void)meta;
}'
    [ "$status" -eq 1 ]
}

@test "catches it through whitespace" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    auto* meta = &graph->series_meta [ pin->series_id ];
    (void)meta;
}'
    [ "$status" -eq 1 ]
}

@test "reports the offending file and line" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    auto* meta = &graph->series_meta[pin->series_id];
    (void)meta;
}'
    [ "$status" -eq 1 ]
    [[ "$output" == *"src/ui/fixture.cpp:2"* ]]
}

# --------------------------------------------------------------- must ignore

@test "allows iterating slots by index" {
    run_gate 'void scan(ui_temp_graph_t* graph) {
    for (int i = 0; i < UI_TEMP_GRAPH_MAX_SERIES; i++) {
        auto* meta = &graph->series_meta[i];
        (void)meta;
    }
}'
    [ "$status" -eq 0 ]
}

@test "allows a literal slot index" {
    run_gate 'void first(ui_temp_graph_t* graph) {
    auto* meta = &graph->series_meta[0];
    (void)meta;
}'
    [ "$status" -eq 0 ]
}

@test "allows a named slot variable" {
    run_gate 'void at_slot(ui_temp_graph_t* graph, int slot) {
    auto* meta = &graph->series_meta[slot];
    (void)meta;
}'
    [ "$status" -eq 0 ]
}

@test "allows the correct resolver call" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    const ui_temp_series_meta_t* meta = find_meta_by_id(graph, pin->series_id);
    (void)meta;
}'
    [ "$status" -eq 0 ]
}

@test "ignores the pattern inside a line comment" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    // The bug this replaces was series_meta[pin->series_id], which indexed
    // the array by a handle instead of a slot.
    auto* meta = find_meta_by_id(graph, pin->series_id);
    (void)meta;
}'
    [ "$status" -eq 0 ]
}

@test "ignores the pattern inside a block comment" {
    run_gate 'void draw(ui_temp_graph_t* graph, const TempGraphHit* pin) {
    /* Do NOT write series_meta[pin->series_id] here: series_meta is
       slot-indexed and series_id is a handle. */
    auto* meta = find_meta_by_id(graph, pin->series_id);
    (void)meta;
}'
    [ "$status" -eq 0 ]
}

@test "passes on a tree with no series_meta at all" {
    run_gate 'int unrelated() { return 42; }'
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK:"* ]]
}

# ------------------------------------------------------------- real codebase

@test "the real tree passes the gate" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

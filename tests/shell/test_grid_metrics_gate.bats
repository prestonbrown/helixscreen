#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for the grid-metrics-single-source rule of
# scripts/check_source_rules.py: the grid cell-metrics single-source-of-truth
# rule.
#
# GridEditMode::current_metrics() is the one place allowed to call
# GridLayout::get_cols()/get_rows()/get_dimensions() directly; every other
# grid_edit_mode.cpp path takes a CellMetrics from it instead of recomputing,
# so gutter handling and int-vs-float rounding can't drift between call
# sites. The rule counts matching LINES in src/ui/home/grid_edit_mode.cpp and
# fails once that count exceeds GRID_METRICS_LIMIT (2 — one get_cols + one
# get_rows, both inside current_metrics()), so a regrown duplicate stays caught.
#
# The rule reads a fixed path under --root, so these tests stand up a
# miniature repo layout under a tmpdir; a fixture grid_edit_mode.cpp stands in
# for the real one without touching the tree.

GATE="scripts/check_source_rules.py --rule grid-metrics-single-source"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    load helpers.bash
    FIXTURE_DIR="${BATS_TEST_TMPDIR:-$(mktemp -d)}/grid_metrics"
    mkdir -p "$FIXTURE_DIR/src/ui/home"
}

# Write $1 into the fixture's src/ui/home/grid_edit_mode.cpp and run the rule on the fixture.
run_gate() {
    printf '%s\n' "$1" > "$FIXTURE_DIR/src/ui/home/grid_edit_mode.cpp"
    run python3 $GATE --root "$FIXTURE_DIR"
}

# ---------------------------------------------------------------- must catch

@test "fails on a fixture with three or more call sites" {
    run_gate 'int f(UiBreakpoint bp) {
    int cols = GridLayout::get_cols(bp);
    int rows = GridLayout::get_rows(bp);
    return cols + rows;
}
int g(UiBreakpoint bp) {
    return GridLayout::get_cols(bp);
}'
    [ "$status" -eq 1 ]
    contains "3 grid-dimension call sites" "$output"
    [[ "$output" == *"case.cpp"* || "$output" == *"grid_edit_mode.cpp"* ]]
}

@test "fails on get_dimensions() piling onto an already-full budget" {
    run_gate 'int f(UiBreakpoint bp) {
    int cols = GridLayout::get_cols(bp);
    int rows = GridLayout::get_rows(bp);
    auto dims = GridLayout::get_dimensions(bp);
    return cols + rows + dims.first;
}'
    [ "$status" -eq 1 ]
}

@test "fails rather than silently passing when the target file is missing" {
    # No run_gate call — the fixture's src/ui/home/grid_edit_mode.cpp never gets
    # written, so the rule must report the miss, not exit 0.
    run python3 $GATE --root "$FIXTURE_DIR"
    [ "$status" -eq 1 ]
    [[ "$output" == *"not found"* ]]
}

# ---------------------------------------------------------------- must allow

@test "passes with exactly the two call sites current_metrics() makes" {
    run_gate 'int f(UiBreakpoint bp) {
    int cols = GridLayout::get_cols(bp);
    int rows = GridLayout::get_rows(bp);
    return cols + rows;
}'
    [ "$status" -eq 0 ]
}

@test "two calls folded onto one line count as a single hit (documented gap)" {
    # This is exactly the shape the grid-metrics-single-source rule's
    # comment warns about: counting is per line, so squeezing both calls
    # onto one line only spends one of LIMIT's two slots. Pinning it here
    # means a future rewrite to token-level counting is a deliberate choice,
    # not an accidental behavior change.
    run_gate 'int f(UiBreakpoint bp) {
    int cols = GridLayout::get_cols(bp), rows = GridLayout::get_rows(bp);
    return cols + rows;
}'
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the real tree

@test "the committed grid_edit_mode.cpp is at exactly the two allowed call sites" {
    run python3 $GATE
    [ "$status" -eq 0 ]
}

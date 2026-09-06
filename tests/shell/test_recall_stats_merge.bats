#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/recall_stats_merge.py — the git merge driver for
# .claude-recall/*.json lesson usage counters.
#
# These counters were gitignored for months precisely because a text merge
# cannot combine them: two worktrees that each cited a lesson produce
# conflicting scalars, and resolving by hand means picking a side and silently
# throwing away the other's citations. The driver exists to make that merge
# additive, so the load-bearing tests here are the end-to-end git merge and its
# negative control — a driver that computes the right number but never gets
# invoked by git is worth nothing, and that wiring lives in three separate
# places (.gitattributes, merge.recall-stats.driver, the script itself).
#
# The delete and malformed-input cases matter for a different reason: this
# script runs during a merge, with the merge half-applied. Corrupting the file
# there is far worse than failing, because git will happily commit whatever it
# writes. Exiting non-zero hands the conflict back to the user instead.

load helpers

SCRIPT="scripts/recall_stats_merge.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO="$PWD"
    DRIVER="$REPO/$SCRIPT"
    TEST_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "$TEST_DIR"
}

# Read one field out of a merged file.
field() {
    python3 -c "
import json,sys
print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])" "$1" "$2" "$3"
}

# ---------------------------------------------------------------------------
# Script hygiene
# ---------------------------------------------------------------------------

@test "recall_stats_merge.py exists and is valid python" {
    [ -f "$SCRIPT" ]
    run python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$SCRIPT"
    [ "$status" -eq 0 ]
}

@test "wiring: .gitattributes routes the counter files to this driver" {
    run git check-attr merge -- .claude-recall/stats.json
    [ "$status" -eq 0 ]
    contains "merge: recall-stats" "$output"

    run git check-attr merge -- .claude-recall/injection-stats.json
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge: recall-stats"* ]]
}

@test "wiring: LESSONS.md is NOT routed to the counter driver" {
    # Prose merges as text. Sending it here would mangle it.
    run git check-attr merge -- .claude-recall/LESSONS.md
    [ "$status" -eq 0 ]
    [[ "$output" == *"merge: unspecified"* ]]
}

# ---------------------------------------------------------------------------
# Merge arithmetic
# ---------------------------------------------------------------------------

@test "counters add each side's delta from the ancestor" {
    echo '{"L1":{"uses":7}}'  > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":10}}' > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":9}}'  > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    # 7 + 3 + 2, not 10 and not 9.
    [ "$(field "$TEST_DIR/a.json" L1 uses)" -eq 12 ]
}

@test "a side that did not change contributes nothing" {
    echo '{"L1":{"uses":5}}' > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":5}}' > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":8}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 uses)" -eq 8 ]
}

@test "injections and citations are additive too" {
    echo '{"L1":{"injections":4,"citations":0}}' > "$TEST_DIR/o.json"
    echo '{"L1":{"injections":6,"citations":1}}' > "$TEST_DIR/a.json"
    echo '{"L1":{"injections":5,"citations":2}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 injections)" -eq 7 ]
    [ "$(field "$TEST_DIR/a.json" L1 citations)" -eq 3 ]
}

@test "velocity takes the max, never the sum" {
    # velocity is a decayed metric recall recomputes, not a counter. Summing
    # deltas would inflate it every merge until the rating is meaningless.
    echo '{"L1":{"uses":1,"velocity":4}}'  > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":1,"velocity":9}}'  > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":1,"velocity":6}}'  > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 velocity)" = "9" ]
}

@test "last-used takes the later ISO date" {
    echo '{"L1":{"uses":1,"last":"2026-01-01"}}' > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":1,"last":"2026-08-20"}}' > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":1,"last":"2026-08-15"}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 last)" = "2026-08-20" ]
}

@test "counters never merge to a negative number" {
    # A reset on one side must not drag the total below zero.
    echo '{"L1":{"uses":10}}' > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":0}}'  > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":0}}'  > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 uses)" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Which lessons survive
# ---------------------------------------------------------------------------

@test "a lesson added on either side is kept" {
    echo '{"L1":{"uses":1}}'                     > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":1},"L2":{"uses":3}}'     > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":1},"L3":{"uses":4}}'     > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L2 uses)" -eq 3 ]
    [ "$(field "$TEST_DIR/a.json" L3 uses)" -eq 4 ]
}

@test "a lesson deleted on one side stays deleted" {
    # `recall delete` is deliberate; resurrecting the entry every merge would
    # make deletion impossible while any branch still carried the old file.
    echo '{"L1":{"uses":1},"L2":{"uses":5}}' > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":1},"L2":{"uses":5}}' > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":1}}'                 > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    run python3 -c "
import json,sys; print('L2' in json.load(open(sys.argv[1])))" "$TEST_DIR/a.json"
    [ "$output" = "False" ]
}

@test "an absent ancestor merges as a plain union" {
    # Both sides created the file independently: git hands us an empty %O.
    : > "$TEST_DIR/o.json"
    echo '{"L1":{"uses":2}}' > "$TEST_DIR/a.json"
    echo '{"L2":{"uses":3}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(field "$TEST_DIR/a.json" L1 uses)" -eq 2 ]
    [ "$(field "$TEST_DIR/a.json" L2 uses)" -eq 3 ]
}

# ---------------------------------------------------------------------------
# Failure behavior — a conflict is recoverable, a corrupt counter file is not
# ---------------------------------------------------------------------------

@test "malformed input exits non-zero instead of writing a partial file" {
    echo '{"L1":{"uses":1}}' > "$TEST_DIR/o.json"
    echo 'this is not json'  > "$TEST_DIR/a.json"
    echo '{"L1":{"uses":2}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -ne 0 ]
    # The unparseable side must be left exactly as it was for the user to fix.
    [ "$(cat "$TEST_DIR/a.json")" = "this is not json" ]
}

@test "too few arguments exits non-zero" {
    run python3 "$DRIVER" "$TEST_DIR/o.json"
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# Formatting — recall writes the two files in different styles
# ---------------------------------------------------------------------------

@test "indented input stays indented, with its trailing newline" {
    printf '{\n  "L1": {\n    "uses": 1\n  }\n}\n' > "$TEST_DIR/o.json"
    printf '{\n  "L1": {\n    "uses": 3\n  }\n}\n' > "$TEST_DIR/a.json"
    printf '{\n  "L1": {\n    "uses": 2\n  }\n}\n' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    grep -q '^  "L1"' "$TEST_DIR/a.json"
    [ -n "$(tail -c 1 "$TEST_DIR/a.json")" ] || true   # trailing newline present
    [ "$(tail -c 1 "$TEST_DIR/a.json" | xxd -p)" = "0a" ]
}

@test "compact input stays compact, with no trailing newline" {
    printf '{"L1":{"uses":1}}' > "$TEST_DIR/o.json"
    printf '{"L1":{"uses":3}}' > "$TEST_DIR/a.json"
    printf '{"L1":{"uses":2}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    [ "$(cat "$TEST_DIR/a.json")" = '{"L1":{"uses":4}}' ]
}

@test "field order is preserved so a merge does not reflow the file" {
    printf '{"L1":{"uses":1,"velocity":1,"last":"2026-01-01"}}' > "$TEST_DIR/o.json"
    printf '{"L1":{"uses":3,"velocity":2,"last":"2026-02-01"}}' > "$TEST_DIR/a.json"
    printf '{"L1":{"uses":2,"velocity":1,"last":"2026-01-15"}}' > "$TEST_DIR/b.json"

    run python3 "$DRIVER" "$TEST_DIR/o.json" "$TEST_DIR/a.json" "$TEST_DIR/b.json"
    [ "$status" -eq 0 ]
    run python3 -c "
import json,sys; print(','.join(json.load(open(sys.argv[1]))['L1']))" "$TEST_DIR/a.json"
    [ "$output" = "uses,velocity,last" ]
}

# ---------------------------------------------------------------------------
# End to end — does git actually call it
# ---------------------------------------------------------------------------

# Build a repo with two branches that each incremented the same counter.
make_diverged_repo() {
    cd "$TEST_DIR" || return 1
    git init -q .
    git config user.email t@t
    git config user.name t
    mkdir -p .claude-recall
    printf '.claude-recall/*.json merge=recall-stats\n' > .gitattributes
    printf '{"L1":{"uses":7}}\n' > .claude-recall/stats.json
    git add -f .gitattributes .claude-recall/stats.json
    git commit -qm base

    git checkout -qb branchA
    printf '{"L1":{"uses":10}}\n' > .claude-recall/stats.json
    git commit -qam "A cites three times"

    git checkout -q main 2>/dev/null || git checkout -q master
    git checkout -qb branchB
    printf '{"L1":{"uses":9}}\n' > .claude-recall/stats.json
    git commit -qam "B cites twice"
}

@test "end to end: a real git merge combines both sides' citations" {
    make_diverged_repo
    git config merge.recall-stats.name "recall lesson-stats union merge"
    git config merge.recall-stats.driver "python3 $DRIVER %O %A %B"

    run git merge branchA -m merged
    [ "$status" -eq 0 ]
    lacks "CONFLICT" "$output"
    [ "$(field .claude-recall/stats.json L1 uses)" -eq 12 ]
}

@test "negative control: without the driver the same merge conflicts" {
    # Proves the assertion above is testing the driver and not git being
    # clever on its own. If this ever passes clean, the test above is vacuous.
    make_diverged_repo
    run git merge branchA -m merged
    [ "$status" -ne 0 ]
    [[ "$output" == *"CONFLICT"* ]]
}

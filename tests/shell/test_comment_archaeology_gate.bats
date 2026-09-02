#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_comment_archaeology.py — the ratchet keeping commit
# SHAs out of comments.
#
# Comments explain the code as it is; how it got here belongs in the commit message,
# where git blame surfaces it on demand. A SHA in a comment also rots, because the
# commit it names can be squashed or rebased away.
#
# Only the objective half of that rule is gated. Phrasing is judgment and stays with
# the reviewer; a SHA either resolves in this repository or it does not.
#
# The quiet half is the harder half here: hex appears throughout this tree in colour
# literals, content hashes and identifiers, and a gate that flagged those would be
# noise and would get switched off. Precision comes from asking git to resolve the
# token rather than pattern-matching it.

GATE="scripts/check_comment_archaeology.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    ROOT="${BATS_TEST_TMPDIR:-$(mktemp -d)}/tree"
    mkdir -p "$ROOT/src"
    # A real commit in this repository, and a hex string that is not one.
    REAL_SHA="$(git rev-parse --short=9 HEAD)"
    printf '0 src/clean.cpp\n' > "$ROOT/base.txt"
    printf 'int clean() { return 0; } // nothing to see\n' > "$ROOT/src/clean.cpp"
}

run_gate() {
    run python3 "$GATE" --repo-root "$ROOT" --baseline "$ROOT/base.txt"
}

# ------------------------------------------------------------- the quiet half

@test "passes on a tree with no citations" {
    run_gate
    [ "$status" -eq 0 ]
}

@test "a hex colour literal is not a citation" {
    printf 'auto c = 0xE0E0E0; // card background, matches #card_bg\n' \
        > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a hex-looking word that is not a commit is not a citation" {
    printf '// canary value deadbeef marks an uninitialised slot\n' \
        > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a short issue reference is not a citation" {
    # Issue refs are explicitly welcome; only SHAs are gated.
    printf '// A wrapper existing does not prove it persists (prestonbrown/helixscreen#1401)\n' \
        > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a SHA in running code rather than a comment is left alone" {
    printf 'const char* pinned = "%s";\n' "$REAL_SHA" > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------- the catch half

@test "flags a real commit SHA in a comment" {
    printf '// Guard added by %s, do not remove.\nint x;\n' "$REAL_SHA" \
        > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"cites a commit SHA"* ]]
    [[ "$output" == *"clean.cpp"* ]]
}

@test "flags a SHA in a block comment and in a hash comment" {
    printf '/* see %s for why */\n' "$REAL_SHA" > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 1 ]

    mkdir -p "$ROOT/scripts"
    printf '0 scripts/gate.py\n0 src/clean.cpp\n' > "$ROOT/base.txt"
    printf 'int clean() { return 0; }\n' > "$ROOT/src/clean.cpp"
    printf '# ported in %s\n' "$REAL_SHA" > "$ROOT/scripts/gate.py"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"gate.py"* ]]
}

# ------------------------------------------------------------------ the ratchet

@test "existing debt at its baseline passes; the same debt plus one fails" {
    printf '// grandfathered %s\nint x;\n' "$REAL_SHA" > "$ROOT/src/clean.cpp"
    printf '1 src/clean.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ]

    printf '// grandfathered %s\n// and another %s\nint x;\n' "$REAL_SHA" "$REAL_SHA" \
        > "$ROOT/src/clean.cpp"
    run_gate
    [ "$status" -eq 1 ]
}

@test "removing a citation is reported as an improvement, not a failure" {
    printf 'int x;\n' > "$ROOT/src/clean.cpp"
    printf '1 src/clean.cpp\n' > "$ROOT/base.txt"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"improved"* ]]
}

# ------------------------------------------------------------------- the real tree

@test "the checked-in tree is within its committed baseline" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

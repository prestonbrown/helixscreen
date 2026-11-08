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

load helpers

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
    contains "cites a commit SHA" "$output"
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

# ------------------------------------------------------- history phrases, added lines
#
# The phrase check reads only lines a change adds, in a scratch repository: a staged
# edit for the commit hook, a committed range for the full run. Every phrase on the
# list was measured against the tree for prose that is present tense; the quiet cases
# below are the shapes that measurement turned up.

phrase_repo() {
    unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE GIT_PREFIX
    REPO="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
    mkdir -p "$REPO/src" "$REPO/scripts"
    git -C "$REPO" init -q
    git -C "$REPO" config user.email t@example.com
    git -C "$REPO" config user.name t
    printf 'int f() { return 0; }\n' > "$REPO/src/a.cpp"
    printf '#!/bin/sh\necho hi\n' > "$REPO/scripts/a.sh"
    git -C "$REPO" add -A
    git -C "$REPO" commit -qm base
}

# Appends $1 to src/a.cpp, stages it, runs the staged check.
stage_cpp() {
    printf '%s\n' "$1" >> "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
}

@test "phrases: each history phrase on an added comment line fails" {
    phrase_repo
    local line
    for line in \
        '// It used to scan the whole cache.' \
        '// This used to be gated on the flag.' \
        '// Pre-fix, the drain ran over freed widgets.' \
        '// The old code returned "default" here.' \
        '// Before the fix this path matched nothing.' \
        '// before this fix the tooltip stayed armed' \
        '// This fixes the bug where the panel froze.' \
        '// The timeout was changed to 5s.' \
        '// Every BoxTurtle as of 2026-07-29 falls through.'; do
        git -C "$REPO" reset -q --hard
        stage_cpp "$line"
        [ "$status" -eq 1 ] || fail "not flagged: $line ($output)"
        contains "src/a.cpp:2" "$output"
    done
}

@test "phrases: a line inside a block comment and a hash comment are read" {
    phrase_repo
    printf '/*\n * The old code leaked here.\n */\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 1 ]
    contains "src/a.cpp:3" "$output"

    git -C "$REPO" reset -q --hard
    printf '# this used to call make twice\n' >> "$REPO/scripts/a.sh"
    git -C "$REPO" add scripts/a.sh
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 1 ]
    contains "scripts/a.sh:3" "$output"
}

@test "phrases: committed work since a base is checked" {
    phrase_repo
    local base
    base="$(git -C "$REPO" rev-parse HEAD)"
    printf '// The old code dropped this.\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" commit -qam change
    run python3 "$GATE" --repo-root "$REPO" --base "$base"
    [ "$status" -eq 1 ]
    contains "src/a.cpp:2" "$output"
}

@test "phrases: present-tense prose the list leaves alone passes" {
    phrase_repo
    local line
    for line in \
        '// We are no longer waiting for the reconnect.' \
        '// A name no longer than 32 bytes.' \
        '// Restore the previously selected item.' \
        '// Use current_target, not the originally clicked child.' \
        '// File descriptor pair used to wake sd_bus_wait().' \
        '// The lane keeps the colour of whatever used to be in it.' \
        '// Only apply this fix for 32bpp displays.' \
        '// This prevents the bug where icons are added but fonts are not.' \
        '// 2026-06-15 12:00:00 UTC is 08:00 EDT.' \
        '// Verbatim from klippy.log of the 2026-08-16 print.' \
        '//   debug-bundle.sh --list --until 2026-02-26' \
        '// Set by the code used to parse the header.' \
        '// Until 2027-01-01 the API key stays valid.'; do
        git -C "$REPO" reset -q --hard
        stage_cpp "$line"
        [ "$status" -eq 0 ] || fail "flagged: $line ($output)"
    done
}

@test "phrases: existing lines, code and string literals pass" {
    phrase_repo
    # Already in the base commit: only added lines are read.
    printf '// The old code dropped this.\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" commit -qam grandfathered
    printf 'int g() { return 1; }\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 0 ]

    git -C "$REPO" reset -q --hard
    stage_cpp 'const char* kMsg = "// the old code returned this";'
    [ "$status" -eq 0 ]

    git -C "$REPO" reset -q --hard
    printf 'echo "# this used to fail" >&2\n' >> "$REPO/scripts/a.sh"
    git -C "$REPO" add scripts/a.sh
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 0 ]
}

@test "phrases: a base that does not resolve is an error, not a pass" {
    phrase_repo
    run python3 "$GATE" --repo-root "$REPO" --base no-such-ref
    [ "$status" -eq 2 ]
    contains "no-such-ref" "$output"
    [[ "$output" != *"OK:"* ]] || fail "reported OK: $output"
}

@test "phrases: a grandfathered comment moved or reflowed passes; a new phrase in the same change fails" {
    phrase_repo
    printf '// The old code dropped this.\nint g() { return 1; }\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" commit -qam grandfathered
    # Moved below g() and re-indented.
    printf 'int f() { return 0; }\nint g() { return 1; }\n    //   The old   code dropped this.\n' \
        > "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 0 ] || fail "a move was flagged: $output"

    printf '// It used to scan the whole cache.\n' >> "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 1 ]
    contains "src/a.cpp:4" "$output"
    [[ "$output" != *"src/a.cpp:3"* ]] || fail "the moved line was flagged: $output"

    # One removed line excuses one added copy, not two.
    git -C "$REPO" reset -q --hard
    printf 'int f() { return 0; }\nint g() { return 1; }\n// The old code dropped this.\n// The old code dropped this.\n' \
        > "$REPO/src/a.cpp"
    git -C "$REPO" add src/a.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 1 ]
}

@test "phrases: a renamed file is read, and the comments it carries over pass" {
    phrase_repo
    # Enough unchanged lines that git pairs b.cpp with c.cpp as a rename.
    { printf '// The old code dropped this.\n'; printf 'int g%d();\n' 1 2 3 4 5 6 7 8 9; } \
        > "$REPO/src/b.cpp"
    git -C "$REPO" add src/b.cpp
    git -C "$REPO" commit -qm b
    local base
    base="$(git -C "$REPO" rev-parse HEAD)"
    git -C "$REPO" mv src/b.cpp src/c.cpp
    printf 'int h();\n' >> "$REPO/src/c.cpp"
    git -C "$REPO" commit -qam moved
    run python3 "$GATE" --repo-root "$REPO" --base "$base"
    [ "$status" -eq 0 ] || fail "a moved file was flagged: $output"

    # A renamed file is still read: a phrase it gains fails.
    printf '// This used to be gated on the flag.\n' >> "$REPO/src/c.cpp"
    git -C "$REPO" commit -qam gained
    run python3 "$GATE" --repo-root "$REPO" --base "$base"
    [ "$status" -eq 1 ]
    contains "src/c.cpp:12" "$output"
}

@test "phrases: ARCHAEOLOGY_OK on the line or the one above passes" {
    phrase_repo
    stage_cpp '// The old code is kept for reference. ARCHAEOLOGY_OK: quotes the vendor doc'
    [ "$status" -eq 0 ]

    git -C "$REPO" reset -q --hard
    printf '// ARCHAEOLOGY_OK: the migration names the shape it repairs\n' >> "$REPO/src/a.cpp"
    stage_cpp '// Records written before the fix carry no colour.'
    [ "$status" -eq 0 ]
}

@test "phrases: a file outside the scanned tree is not read" {
    phrase_repo
    mkdir -p "$REPO/docs"
    printf '<!-- the old code -->\n// the old code\n' > "$REPO/docs/notes.cpp"
    git -C "$REPO" add docs/notes.cpp
    run python3 "$GATE" --repo-root "$REPO" --staged
    [ "$status" -eq 0 ]
}

@test "phrases: the commit hook runs the phrase check on staged sources" {
    eval "$(grep '^QC_ALL=' scripts/quality-checks.sh)"
    eval "$(grep '^QC_PUSH_ONLY=' scripts/quality-checks.sh)"
    [[ " $QC_ALL " == *" qc_comment_archaeology "* ]] || fail "not in QC_ALL"
    [[ " $QC_PUSH_ONLY " != *" qc_comment_archaeology "* ]] || fail "deferred to push"
    local re
    re="$(qc_trigger comment_archaeology)"
    [[ "src/ui/panel.cpp" =~ $re ]] || fail "src/ui/panel.cpp does not trigger it"
    [[ "scripts/qc/x.sh" =~ $re ]] || fail "scripts/qc/x.sh does not trigger it"
    [[ "docs/devel/X.md" =~ $re ]] && fail "docs trigger it"
    grep -q 'check_comment_archaeology.py $mode' scripts/qc/comment_archaeology.sh
}

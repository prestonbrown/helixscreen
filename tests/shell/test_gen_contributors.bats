#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/gen-contributors.sh
# Guards against the shallow-clone regression where CI produced a
# contributors.h containing only the last committer.

load helpers

SCRIPT="scripts/gen-contributors.sh"

setup() {
    TEST_DIR="$(mktemp -d)"
    WORK_DIR="$(mktemp -d)"
    # Copy the script into an isolated work dir so we control CONTRIBUTORS.txt
    # and the presence/absence of a git repo independently of the real checkout.
    mkdir -p "$WORK_DIR/scripts"
    cp "$SCRIPT" "$WORK_DIR/scripts/gen-contributors.sh"
    chmod +x "$WORK_DIR/scripts/gen-contributors.sh"
}

teardown() {
    rm -rf "$TEST_DIR" "$WORK_DIR"
}

@test "gen-contributors.sh exists and is executable" {
    [ -f "$SCRIPT" ]
    [ -x "$SCRIPT" ]
}

@test "gen-contributors.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "CONTRIBUTORS.txt exists and has more than one contributor" {
    [ -f CONTRIBUTORS.txt ]
    count=$(grep -cvE '^[[:space:]]*(#|$)' CONTRIBUTORS.txt)
    [ "$count" -gt 1 ]
}

@test "generates header from CONTRIBUTORS.txt" {
    cd "$WORK_DIR"
    cat > CONTRIBUTORS.txt <<'EOF'
Alice Example
Bob Example
Carol Example
EOF
    BUILD_DIR=build ./scripts/gen-contributors.sh
    [ -f build/generated/contributors.h ]
    grep -q '"Alice Example"' build/generated/contributors.h
    grep -q '"Bob Example"' build/generated/contributors.h
    grep -q '"Carol Example"' build/generated/contributors.h
}

@test "generated header contains more than one contributor" {
    cd "$WORK_DIR"
    cat > CONTRIBUTORS.txt <<'EOF'
Alice Example
Bob Example
Carol Example
EOF
    BUILD_DIR=build ./scripts/gen-contributors.sh
    count=$(grep -cE '^    "' build/generated/contributors.h)
    [ "$count" -gt 1 ]
}

@test "real CONTRIBUTORS.txt produces header with more than one contributor" {
    # This is the regression guard — the shallow-clone bug caused this count to be 1.
    BUILD_DIR="$TEST_DIR" ./scripts/gen-contributors.sh
    [ -f "$TEST_DIR/generated/contributors.h" ]
    count=$(grep -cE '^    "' "$TEST_DIR/generated/contributors.h")
    [ "$count" -gt 1 ]
}

@test "skips comment and blank lines in CONTRIBUTORS.txt" {
    cd "$WORK_DIR"
    cat > CONTRIBUTORS.txt <<'EOF'
# This is a comment
Alice Example

# Another comment
Bob Example
EOF
    BUILD_DIR=build ./scripts/gen-contributors.sh
    refute grep -q 'comment' build/generated/contributors.h
    count=$(grep -cE '^    "' build/generated/contributors.h)
    [ "$count" -eq 2 ]
}

@test "escapes double quotes in names" {
    cd "$WORK_DIR"
    cat > CONTRIBUTORS.txt <<'EOF'
Alice "Ace" Example
EOF
    BUILD_DIR=build ./scripts/gen-contributors.sh
    grep -q 'Alice \\"Ace\\" Example' build/generated/contributors.h
}

@test "generated header declares CONTRIBUTOR_COUNT" {
    cd "$WORK_DIR"
    echo "Alice Example" > CONTRIBUTORS.txt
    BUILD_DIR=build ./scripts/gen-contributors.sh
    grep -q 'CONTRIBUTOR_COUNT' build/generated/contributors.h
    grep -q 'CONTRIBUTORS\[\]' build/generated/contributors.h
}

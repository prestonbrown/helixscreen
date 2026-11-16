#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# $(OBJ_DIR)/.build-flags is a prerequisite of every object, so its mtime is
# what decides whether a flag change rebuilds the tree. It must move only when
# the flag text changes: a rewrite of identical text, or a dry run parsing with
# someone else's flags, rebuilds every object for nothing. Each test points
# OBJ_DIR at a temp dir, so the tree's own stamp is never touched.

load helpers

setup() {
    cd "$(scratch_tree)" || return 1
    OBJ="${BATS_TEST_TMPDIR:-$(mktemp -d)}/obj"
    STAMP="$OBJ/.build-flags"
}

parse() {
    make --no-print-directory OBJ_DIR="$OBJ" "$@" print-var-CC >/dev/null 2>&1
}

age_stamp() {
    touch -d '2000-01-01 00:00:00' "$STAMP"
}

@test "a parse with unchanged flags leaves the stamp's mtime alone" {
    parse
    [ -s "$STAMP" ]
    age_stamp
    parse
    [ "$(stat -c %Y "$STAMP")" = "$(date -d '2000-01-01 00:00:00' +%s)" ]
}

@test "a parse with changed flags rewrites the stamp" {
    parse
    age_stamp
    parse ENABLE_REMOTE_CONTROL=no
    [ "$(stat -c %Y "$STAMP")" != "$(date -d '2000-01-01 00:00:00' +%s)" ]
}

@test "a dry run with other flags writes nothing to the stamp" {
    parse
    cp "$STAMP" "$OBJ/before"
    age_stamp
    parse -n -p STRIP_BINARY=yes CROSS_COMPILE=fake-
    parse -q ENABLE_REMOTE_CONTROL=no || true   # -q exits 1: print-var-CC is never up to date
    cmp "$STAMP" "$OBJ/before"
    [ "$(stat -c %Y "$STAMP")" = "$(date -d '2000-01-01 00:00:00' +%s)" ]
}

@test "a dry run with other flags still reports the objects as out of date" {
    parse
    run make --no-print-directory -n OBJ_DIR="$OBJ" ENABLE_REMOTE_CONTROL=no -p print-var-CC
    [ "$status" -eq 0 ]
    grep -qx '\.PHONY: .*'"$STAMP"'.*' <<<"$output"
}

@test "a parse writes its own temp file, never one a concurrent parse is comparing" {
    parse
    age_stamp
    # Another make in the same tree, mid-write of a different flag set.
    echo "another parse's flags" >"$STAMP.new"
    parse
    [ "$(cat "$STAMP.new")" = "another parse's flags" ]
    [ "$(stat -c %Y "$STAMP")" = "$(date -d '2000-01-01 00:00:00' +%s)" ]
    # Its own temp is gone, whichever way the compare went.
    parse ENABLE_REMOTE_CONTROL=no
    [ "$(stat -c %Y "$STAMP")" != "$(date -d '2000-01-01 00:00:00' +%s)" ]
    [ -z "$(find "$OBJ" -maxdepth 1 -name '.build-flags.new.*')" ]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/merge_compile_commands.py — the post-build step every make
# target runs (mk/rules.mk). Its exit code becomes the target's, so a
# successful link-only build must not read as failure: .ccj fragments are a
# byproduct of COMPILING, and none are written when every object came from the
# cache. A target that returns failure on success teaches callers to stop
# reading its exit code.

load helpers

SCRIPT="scripts/merge_compile_commands.py"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TMP_DIR="$BATS_TMPDIR/merge-ccj-$$"
    mkdir -p "$TMP_DIR/build"
}

teardown() {
    rm -rf "$TMP_DIR"
}

# A fragment in the shape emit-compile-command writes, naming $2 as its source.
write_fragment() { # $1 fragment path, $2 source file
    printf '%s\n' \
        '{"directory": "'"$PWD"'", "file": "'"$2"'", "arguments": ["clang++", "'"$2"'"]}' > "$1"
}

@test "an empty fragment set is success, and the database is left alone" {
    printf 'sentinel\n' > "$TMP_DIR/db.json"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
    [ "$(cat "$TMP_DIR/db.json")" = "sentinel" ]
    # The note says what happened, on stdout, even without --quiet.
    [[ "$output" == *"nothing recompiled"* ]]
}

@test "an empty fragment set under --quiet is silent success" {
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json" --quiet
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

@test "fragments naming live sources merge into the database" {
    write_fragment "$TMP_DIR/build/src.ccj" scripts/version-compare.sh
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
    run python3 -c 'import json,sys; e=json.load(open(sys.argv[1])); assert len(e) == 1' \
        "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
}

@test "fragments whose sources are all gone fail rather than wipe the database" {
    write_fragment "$TMP_DIR/build/gone.ccj" "$TMP_DIR/no-such-file.cpp"
    printf 'sentinel\n' > "$TMP_DIR/db.json"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -ne 0 ]
    [ "$(cat "$TMP_DIR/db.json")" = "sentinel" ]
}

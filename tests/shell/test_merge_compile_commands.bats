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
    [ "$status" -eq 1 ]
    [ "$(cat "$TMP_DIR/db.json")" = "sentinel" ]
    # The exit code alone is also what an unhandled exception produces, so name
    # the branch: this has to be the refusal, not a traceback that reached the
    # same 1 by a different route.
    contains "name no existing source" "$output"
    lacks "Traceback" "$output"
}

@test "a write that fails leaves the previous database in place" {
    write_fragment "$TMP_DIR/build/live.ccj" scripts/version-compare.sh
    printf 'sentinel\n' > "$TMP_DIR/db.json"
    # The database is written through a sibling and renamed over, so the real
    # file is never open while the content is incomplete. Occupying the sibling
    # is the deterministic way to fail the write after the entries are chosen;
    # an in-place open() would have truncated db.json before reaching here.
    mkdir "$TMP_DIR/db.json.tmp"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 1 ]
    [ "$(cat "$TMP_DIR/db.json")" = "sentinel" ]
    contains "cannot write" "$output"
}

@test "no temporary file survives a successful merge" {
    write_fragment "$TMP_DIR/build/live.ccj" scripts/version-compare.sh
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
    [ ! -e "$TMP_DIR/db.json.tmp" ]
}

# ------------------------------------------------------- unreadable fragments
#
# `emit-compile-command` writes a fragment with a shell redirect, so one can be
# read half-written: a build killed mid-compile leaves its in-flight fragments
# truncated, and so does catching a peer's build in the act. A skipped fragment
# is a source file with no entry in the database, which surfaces only as clangd
# and syntax_check.py answering about that file with no flags - nothing points
# back here. So the count is carried, never swallowed.

# A fragment the writer had not finished.
write_truncated() { # $1 fragment path
    printf '{"directory": "/x", "file": "src/half' > "$1"
}

@test "a corrupt majority is refused, not written as a truncated database" {
    write_fragment "$TMP_DIR/build/live.ccj" scripts/version-compare.sh
    write_truncated "$TMP_DIR/build/cut1.ccj"
    write_truncated "$TMP_DIR/build/cut2.ccj"
    write_truncated "$TMP_DIR/build/cut3.ccj"
    printf 'sentinel\n' > "$TMP_DIR/db.json"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 1 ]
    [ "$(cat "$TMP_DIR/db.json")" = "sentinel" ]
    contains "3 of 4" "$output"
    contains "unreadable" "$output"
}

@test "a lone unreadable fragment warns and still merges the rest" {
    # A build running right now holds a few fragments mid-write beside the
    # thousands it already finished. Refusing on any one of them would fail a
    # build that succeeded, so below a majority this merges and says what it lost.
    write_fragment "$TMP_DIR/build/one.ccj" scripts/version-compare.sh
    write_fragment "$TMP_DIR/build/two.ccj" scripts/quality-checks.sh
    write_truncated "$TMP_DIR/build/cut.ccj"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
    contains "1 of 3" "$output"
    run python3 -c 'import json,sys; e=json.load(open(sys.argv[1])); assert len(e) == 2' \
        "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
}

@test "an all-corrupt fragment set is not reported as nothing recompiled" {
    # Zero USABLE fragments is not zero fragments. Saying "nothing recompiled"
    # with corrupt .ccj files sitting in the tree is a success-shaped answer to
    # a tree that needs rebuilding.
    write_truncated "$TMP_DIR/build/cut.ccj"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 1 ]
    lacks "nothing recompiled" "$output"
    contains "unreadable" "$output"
}

@test "a fragment that is valid JSON but not an object is skipped, not a crash" {
    # null, a number and a bare list all parse, so nothing raises until the
    # write-time tag tries item assignment on them. Through the make recipe that
    # traceback is exit 1 on a build that succeeded.
    write_fragment "$TMP_DIR/build/one.ccj" scripts/version-compare.sh
    write_fragment "$TMP_DIR/build/two.ccj" scripts/quality-checks.sh
    printf 'null\n' > "$TMP_DIR/build/scalar.ccj"
    run python3 "$SCRIPT" --build-dir "$TMP_DIR/build" --output "$TMP_DIR/db.json"
    [ "$status" -eq 0 ]
    lacks "Traceback" "$output"
    contains "1 of 3" "$output"
}

# --------------------------------------------------------- the make callers
#
# Both recipes in mk/rules.mk capture the merge's stdout into a summary line, so
# stderr is the only place a refusal can be read. A green build with no
# compile database and no message is the shape this whole step is here to avoid.

@test "no make caller discards the merge's stderr" {
    run grep -n 'merge_compile_commands.py --build-dir' mk/rules.mk
    [ "$status" -eq 0 ]
    lacks "2>/dev/null" "$output"
}

@test "the compile_commands target reports the merge's own summary" {
    # The summary already says which of the two things happened, including
    # "nothing recompiled"; a fixed "generated" in front of it contradicts that
    # on the run where nothing was. compile_commands_full is a different target
    # and its own claim is true, so this names the summary-carrying line.
    run grep -F 'compile_commands.json: $$SUMMARY' mk/rules.mk
    [ "$status" -eq 0 ]
    run grep -F 'compile_commands.json generated ($$SUMMARY)' mk/rules.mk
    [ "$status" -ne 0 ]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# On a bats too old to make BATS_TEST_TMPDIR itself, helpers.bash makes one per
# test and nothing else ever removes it. These run that path in a child shell,
# with BATS_TMPDIR pointed at a test-owned directory.

HELPERS="$BATS_TEST_DIRNAME/helpers.bash"

setup() {
    load helpers
    ROOT="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$ROOT"
}

# Source the helpers as an old bats would, from a child shell whose pid is the owner.
# $1 = commands to run before sourcing. Prints the child pid, then BATS_TEST_TMPDIR.
_old_bats() {
    env -u BATS_TEST_TMPDIR -u HELIX_BATS_TMPDIR_FILLED -u HELIX_BATS_TMPDIR_SWEPT \
        -u HELIX_BATS_TMPDIR_TEST -u BATS_TEST_NAME BATS_TMPDIR="$ROOT" \
        bash -c "$1"'
            source "'"$HELPERS"'"
            echo "$$"
            echo "$BATS_TEST_TMPDIR"'
}

_reaped_pid() {
    sh -c 'echo $$'
}

@test "a dir the helpers make is named for its owner" {
    run _old_bats ""
    [ "$status" -eq 0 ]
    owner=${lines[0]}
    dir=${lines[1]}
    contains "$ROOT/bats-test-$owner-" "$dir"
    [ -d "$dir" ]
}

@test "the first fill sweeps the dirs of owners that are gone, and only those" {
    dead=$(_reaped_pid)
    mkdir -p "$ROOT/bats-test-$dead-abcdef/sub" "$ROOT/bats-test-$$-abcdef" "$ROOT/other-dir"
    run _old_bats ""
    [ "$status" -eq 0 ]
    [ ! -e "$ROOT/bats-test-$dead-abcdef" ]
    [ -d "$ROOT/bats-test-$$-abcdef" ]
    [ -d "$ROOT/other-dir" ]
}

@test "the next test's fill removes the previous test's dir" {
    run _old_bats '
        source "'"$HELPERS"'"
        first=$BATS_TEST_TMPDIR
        unset BATS_TEST_NAME
        source "'"$HELPERS"'"
        [ ! -e "$first" ] || exit 9
        [ "$first" != "$BATS_TEST_TMPDIR" ] || exit 8'
    [ "$status" -eq 0 ]
}

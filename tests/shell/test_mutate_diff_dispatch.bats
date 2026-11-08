#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/mutate-diff.sh (make mutate-diff) runs the mutation gate locally only
# where mutate_diff.py says parallel workers can run (--probe-workers). Anywhere
# else it hands the run to the test host, which mutates the PUSHED HEAD, so it
# refuses when that would judge something other than the tree here.

load helpers

setup() {
    REPO="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$REPO/scripts/lib" "$REPO/src"
    cp "$BATS_TEST_DIRNAME/../../scripts/mutate-diff.sh" "$REPO/scripts/"
    cp "$BATS_TEST_DIRNAME/../../scripts/lib/build_hosts.sh" "$REPO/scripts/lib/"
    printf '#!/bin/bash\nprintf "TEST-HOST-RUN:"; printf " <%%s>" "$@"; echo\n' > "$REPO/scripts/test-host-run.sh"
    chmod +x "$REPO/scripts/test-host-run.sh" "$REPO/scripts/mutate-diff.sh"
    echo 'int f() { return 1; }' > "$REPO/src/a.cpp"
    git -C "$REPO" init -q -b main
    git -C "$REPO" -c user.email=t@t -c user.name=t add .
    git -C "$REPO" -c user.email=t@t -c user.name=t commit -qm base
    # No build-hosts file of the user's: only what each case exports.
    export XDG_CONFIG_HOME="$BATS_TEST_TMPDIR/config"
    unset HELIX_TEST_HOST
    cd "$REPO" || return 1
}

# python3 stands in for mutate_diff.py: --probe-workers answers the parallel
# question, any other call is the run itself.
no_reflink() { mock_command_script python3 'case "$*" in *--probe-workers*) echo "no reflink copies"; exit 1 ;; esac; echo "LOCAL: $*"'; }
reflink() { mock_command_script python3 'case "$*" in *--probe-workers*) exit 0 ;; esac; echo "LOCAL: $*"'; }
pushed() { git update-ref refs/remotes/origin/main HEAD; }

@test "a host where workers run in parallel runs the gate here, in parallel" {
    reflink
    run scripts/mutate-diff.sh --tests '[x]'
    [ "$status" -eq 0 ]
    contains "LOCAL: scripts/mutate_diff.py --workers auto --pool --tests [x]" "$output"
    lacks "TEST-HOST-RUN" "$output"
}

@test "without reflink copies, a pushed clean tree goes to the test host" {
    no_reflink
    pushed
    export HELIX_TEST_HOST=zeus.invalid
    run scripts/mutate-diff.sh --tests '[x]' --tests-for 'src/a b.cpp=[one case]'
    [ "$status" -eq 0 ]
    # Each argument arrives whole, spaces included.
    contains "TEST-HOST-RUN: <mutate> <--pool> <--tests> <[x]> <--tests-for> <src/a b.cpp=[one case]>" "$output"
    lacks "LOCAL:" "$output"
}

@test "without reflink copies or a test host, it refuses and names MUTATE_LOCAL" {
    no_reflink
    pushed
    run scripts/mutate-diff.sh
    [ "$status" -eq 2 ]
    contains "no test host is configured" "$output"
    contains "MUTATE_LOCAL=1" "$output"
    lacks "LOCAL:" "$output"
}

@test "uncommitted code is refused: the test host would not see it" {
    no_reflink
    pushed
    export HELIX_TEST_HOST=zeus.invalid
    echo 'int f() { return 2; }' > src/a.cpp
    run scripts/mutate-diff.sh
    [ "$status" -eq 2 ]
    contains "src/a.cpp" "$output"
    lacks "TEST-HOST-RUN" "$output"
}

@test "an uncommitted test is refused too: the survivor it kills would not be seen" {
    no_reflink
    pushed
    export HELIX_TEST_HOST=zeus.invalid
    mkdir -p tests/unit
    echo 'TEST_CASE("x") {}' > tests/unit/test_new.cpp
    run scripts/mutate-diff.sh
    [ "$status" -eq 2 ]
    contains "tests/unit/test_new.cpp" "$output"
    lacks "TEST-HOST-RUN" "$output"
}

@test "an unpushed HEAD is refused: the test host mutates the pushed one" {
    no_reflink
    export HELIX_TEST_HOST=zeus.invalid
    run scripts/mutate-diff.sh
    [ "$status" -eq 2 ]
    contains "is not pushed" "$output"
    lacks "TEST-HOST-RUN" "$output"
}

@test "MUTATE_LOCAL=1 runs here whatever the host" {
    no_reflink
    export MUTATE_LOCAL=1
    run scripts/mutate-diff.sh --tests '[x]'
    [ "$status" -eq 0 ]
    contains "LOCAL: scripts/mutate_diff.py --tests [x]" "$output"
}

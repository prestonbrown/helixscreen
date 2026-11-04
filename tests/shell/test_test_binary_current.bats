#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_test_binary_current.sh, the predicate deciding
# whether the hidden test set is cheap enough to run on a commit.
#
# The predicate it replaces was `make -q`, which cannot ever succeed in this
# tree: src/system/helix_version.cpp is regenerated on every invocation for the
# git hash. The gate behind it printed "Test binary is stale - skipping" on
# every commit and ran nothing, which is how the [.] set rots unobserved.
#
# A stubbed make on PATH drives the plan, so these need no build and run on a
# bare checkout.

load helpers

GATE="scripts/check_test_binary_current.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    STUB_BIN="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$STUB_BIN"
}

# A make whose -q always reports work outstanding and whose -n plan compiles
# exactly the sources named here.
stub_make() {
    {
        echo '#!/usr/bin/env bash'
        echo 'for a in "$@"; do [ "$a" = "-q" ] && exit 1; done'
        for src in "$@"; do
            echo "echo 'ccache clang++ -std=c++17 -c $src -o build/obj/x.o'"
        done
        echo 'exit 0'
    } > "$STUB_BIN/make"
    chmod +x "$STUB_BIN/make"
}

@test "current when the only stale source is the git-hash file" {
    stub_make src/system/helix_version.cpp
    PATH="$STUB_BIN:$PATH" run "$GATE"
    [ "$status" -eq 0 ]
}

@test "stale when a real source is out of date" {
    stub_make src/system/helix_version.cpp src/ui/home/ui_panel_home.cpp
    PATH="$STUB_BIN:$PATH" run "$GATE"
    [ "$status" -eq 1 ]
    contains "ui_panel_home.cpp" "$output"
}

@test "the git-hash file is not named as a reason when something else is stale" {
    stub_make src/system/helix_version.cpp src/ui/home/ui_panel_home.cpp
    PATH="$STUB_BIN:$PATH" run "$GATE"
    lacks "helix_version.cpp" "$output"
}

@test "current when nothing at all is queued" {
    stub_make
    PATH="$STUB_BIN:$PATH" run "$GATE"
    [ "$status" -eq 0 ]
}

@test "a make -q that succeeds short-circuits to current" {
    {
        echo '#!/usr/bin/env bash'
        echo 'exit 0'
    } > "$STUB_BIN/make"
    chmod +x "$STUB_BIN/make"
    PATH="$STUB_BIN:$PATH" run "$GATE"
    [ "$status" -eq 0 ]
}

# ------------------------------------------------------------------- wiring
#
# A predicate nobody consults is a file.

@test "quality-checks.sh consults the predicate" {
    contains "check_test_binary_current.sh" "$(cat scripts/qc/hidden_tests.sh)"
}

@test "quality-checks.sh no longer gates the hidden set on a bare make -q" {
    local block
    block=$(awk '/^qc_hidden_tests\(\)/{f=1} f&&/^}/{print;exit} f' scripts/qc/hidden_tests.sh)
    lacks "make -q _PARALLEL_GUARD=1 build/bin/helix-tests" "$block"
}

@test "the hidden block runs the binary rather than a building make target" {
    local block
    block=$(awk '/^qc_hidden_tests\(\)/{f=1} f&&/^}/{print;exit} f' scripts/qc/hidden_tests.sh)
    contains 'build/bin/helix-tests "[.]"' "$block"
    lacks "if make test-hidden" "$block"
}

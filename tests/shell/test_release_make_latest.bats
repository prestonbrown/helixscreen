#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/release-make-latest.sh, which decides whether a release
# becomes GitHub's "latest". Moonraker's stable channel installs whatever
# releases/latest names, downgrades included (helixscreen#2147), so "latest"
# must never move backwards.

load helpers

SCRIPT="scripts/release-make-latest.sh"
WORKFLOW=".github/workflows/release.yml"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

@test "release-make-latest.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "a release newer than latest becomes latest" {
    run "$SCRIPT" v1.1.0 false v1.0.10
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
}

@test "a release on an older line does not take latest back" {
    run "$SCRIPT" v1.0.11 false v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "false" ]
}

@test "a release equal to latest does not reassert it" {
    run "$SCRIPT" v1.1.0 false v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "false" ]
}

@test "with no latest release yet, a stable release becomes latest" {
    run "$SCRIPT" v1.1.0 false ""
    [ "$status" -eq 0 ]
    [ "$output" = "true" ]
    run "$SCRIPT" v1.1.0 false
    [ "$output" = "true" ]
}

@test "a prerelease never becomes latest, even above the current one" {
    run "$SCRIPT" v1.2.0-beta.1 true v1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "false" ]
    run "$SCRIPT" v1.2.0-beta.1 true ""
    [ "$output" = "false" ]
}

@test "precedence is semver, not string order" {
    # String order ranks 1.0.9 above 1.0.10.
    run "$SCRIPT" v1.0.10 false v1.0.9
    [ "$output" = "true" ]
    # A stable release outranks its own prerelease.
    run "$SCRIPT" v1.1.0 false v1.1.0-rc.4
    [ "$output" = "true" ]
}

@test "a latest tag that cannot be ranked is an error, not a verdict" {
    run "$SCRIPT" v1.1.0 false nightly
    [ "$status" -ne 0 ]
    [[ "$output" != "true" && "$output" != "false" ]]
}

@test "an unrecognised prerelease value is an error" {
    run "$SCRIPT" v1.1.0 yes v1.0.10
    [ "$status" -eq 2 ]
}

@test "the release step takes make_latest from the decision" {
    grep -q 'scripts/release-make-latest.sh' "$WORKFLOW"
    grep -qE '^ +make_latest: \$\{\{ steps\.latest\.outputs\.make_latest \}\}' "$WORKFLOW"
}

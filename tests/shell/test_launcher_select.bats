#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs tests/test_launcher_select.sh, which exercises select_binary() in
# scripts/helix-launcher.sh — the three-rung choice between helix-screen-egl,
# helix-screen and helix-screen-fbdev.
#
# The harness there extracts the real functions out of the launcher with sed
# and drives them against mock binaries, so it needs a shell rather than bats
# assertions. This wrapper is what puts it in `make test-shell`; without it the
# suite runs and the launcher's selection logic is covered by nothing.

@test "launcher binary selection suite passes" {
    run bash tests/test_launcher_select.sh
    if [ "$status" -ne 0 ]; then
        echo "$output"
    fi
    [ "$status" -eq 0 ]
}

@test "launcher selection suite actually asserts something" {
    # A harness that silently stopped finding the functions it extracts would
    # exit 0 having checked nothing, and the wrapper above would call that a
    # pass. Pin the count so a vanished test is a red, not a quieter green.
    run bash tests/test_launcher_select.sh
    [ "$status" -eq 0 ]
    [[ "$output" == *"Total tests: 11"* ]]
}

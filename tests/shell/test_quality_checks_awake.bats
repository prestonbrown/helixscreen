#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A full quality sweep re-runs itself under caffeinate where the host has it, so
# a laptop does not sleep mid-sweep; a commit's staged sweep does not.

load helpers

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    unset HELIX_QC_AWAKE
    mock_command_script caffeinate 'echo "caffeinate: $*"'
}

hold() { # <STAGED_ONLY>: what qc_hold_awake does with the sweep's own arguments
    run bash -c 'STAGED_ONLY=$1; eval "$(sed -n "/^qc_hold_awake() {/,/^}/p" scripts/quality-checks.sh)"
        qc_hold_awake ./scripts/quality-checks.sh --x; echo "ran on"' _ "$1"
}

@test "a full sweep re-runs itself under caffeinate" {
    hold false
    contains "caffeinate: -i -s ./scripts/quality-checks.sh --x" "$output"
    lacks "ran on" "$output"
}

@test "a staged sweep, or one already held awake, runs as it is" {
    hold true
    contains "ran on" "$output"
    lacks "caffeinate:" "$output"
    HELIX_QC_AWAKE=1 hold false
    contains "ran on" "$output"
    lacks "caffeinate:" "$output"
}

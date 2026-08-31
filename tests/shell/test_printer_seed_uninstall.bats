#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests undo_seeded_settings() in uninstall.sh (#986): the install→uninstall
# acknowledgement for the per-printer settings-seed mechanism. Uninstall must
# log which printer's defaults were seeded, leave settings.json UNTOUCHED (the
# seeded values can't be safely un-merged from the user's own keys), and remove
# the .seeded_settings marker file. Absent marker must be a graceful no-op.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export SEED_STATE_FILE="$INSTALL_DIR/config/.seeded_settings"
    export SETTINGS_FILE="$INSTALL_DIR/config/settings.json"

    SUDO=""
    export SUDO

    # Stub functions referenced by uninstall.sh but not under test.
    kill_process_by_name() { :; }
    export -f kill_process_by_name

    unset _HELIX_UNINSTALL_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
}

@test "undo_seeded: removes marker and leaves settings.json untouched" {
    # A seed was recorded; settings.json holds the (merged) result.
    printf '%s\n' "settings:sovol_sv06_ace" > "$SEED_STATE_FILE"
    printf '%s\n' '{"input":{"calibration":{"valid":true,"a":1.66}}}' > "$SETTINGS_FILE"

    run undo_seeded_settings
    [ "$status" -eq 0 ]

    # Marker removed...
    [ ! -f "$SEED_STATE_FILE" ]
    # ...but settings.json is byte-for-byte unchanged (values remain by design).
    python3 -c "import json;d=json.load(open('$SETTINGS_FILE'));assert d['input']['calibration']['valid'] is True;assert d['input']['calibration']['a']==1.66, d"
}

@test "undo_seeded: logs which printer's defaults were seeded" {
    # Re-stub log_info to capture output for this test only.
    local log="$BATS_TEST_TMPDIR/log_info.log"
    log_info() { echo "$*" >> "$log"; }
    export -f log_info

    printf '%s\n' "settings:sovol_sv06_ace" > "$SEED_STATE_FILE"
    : > "$SETTINGS_FILE"

    run undo_seeded_settings
    [ "$status" -eq 0 ]

    grep -q "sovol_sv06_ace" "$log"
}

@test "undo_seeded: missing marker file is a graceful no-op" {
    rm -f "$SEED_STATE_FILE"
    run undo_seeded_settings
    [ "$status" -eq 0 ]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The standalone installer must refuse to run on a device where HelixScreen is
# managed by the firmware (e.g. PAXX's Snapmaker U1 firmware). Detection markers:
#   - <root>/oem/apps/helixscreen  (directory), OR
#   - <root>/etc/hooks/lmd.d/30-helixscreen.sh  (file)
# HELIX_FIRMWARE_MANAGED_MARKER overrides the marker root for testing.
# HELIX_IGNORE_FIRMWARE_MANAGED=1 forces past the guard.

load helpers

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    unset _HELIX_MAIN_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    export HELIX_FIRMWARE_MANAGED_MARKER="$BATS_TEST_TMPDIR/root"
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER"
    unset HELIX_IGNORE_FIRMWARE_MANAGED
}

@test "no markers present: guard passes" {
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "oem/apps/helixscreen dir present: guard aborts non-zero" {
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER/oem/apps/helixscreen"
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    contains "managed by your firmware" "$output"
    [[ "$output" == *"HELIX_IGNORE_FIRMWARE_MANAGED=1"* ]]
}

@test "lmd hook file present: guard aborts non-zero" {
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER/etc/hooks/lmd.d"
    : > "$HELIX_FIRMWARE_MANAGED_MARKER/etc/hooks/lmd.d/30-helixscreen.sh"
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    [[ "$output" == *"managed by your firmware"* ]]
}

@test "HELIX_IGNORE_FIRMWARE_MANAGED=1 bypasses the guard" {
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER/oem/apps/helixscreen"
    export HELIX_IGNORE_FIRMWARE_MANAGED=1
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "detected marker path is reported in the abort message" {
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER/oem/apps/helixscreen"
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    [[ "$output" == *"$HELIX_FIRMWARE_MANAGED_MARKER/oem/apps/helixscreen"* ]]
}

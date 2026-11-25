#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The standalone installer must refuse to run where the firmware runs HelixScreen
# itself: PAXX's Snapmaker U1 firmware, when its lmd hook
# (<root>/etc/hooks/lmd.d/30-helixscreen.sh) binds its package: extended2.cfg's
# `[components] gui` selects helixscreen AND oem/apps/helixscreen/latest/bin/helix-screen
# is executable. PAXX ships the hook whatever gui says.
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

@test "stock firmware, no hook: guard passes" {
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

paxx_hook() {
    mkdir -p "$HELIX_FIRMWARE_MANAGED_MARKER/etc/hooks/lmd.d"
    : > "$HELIX_FIRMWARE_MANAGED_MARKER/etc/hooks/lmd.d/30-helixscreen.sh"
}

paxx_package() {
    local bin="$HELIX_FIRMWARE_MANAGED_MARKER/oem/apps/helixscreen/latest/bin"
    mkdir -p "$bin"
    : > "$bin/helix-screen"
    chmod +x "$bin/helix-screen"
}

paxx_gui() {
    local dir="$HELIX_FIRMWARE_MANAGED_MARKER/oem/printer_data/config/extended"
    mkdir -p "$dir"
    printf '[web]\nfrontend: fluidd\n\n[components]\nrfid: external\ngui: %s\n' "$1" > "$dir/extended2.cfg"
}

@test "PAXX runs its package (hook, gui: helixscreen, package): guard aborts non-zero" {
    paxx_hook
    paxx_gui helixscreen
    paxx_package
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    contains "managed by your firmware" "$output"
    [[ "$output" == *"HELIX_IGNORE_FIRMWARE_MANAGED=1"* ]]
}

@test "lmd hook with gui: snapmaker and no package: guard passes" {
    paxx_hook
    paxx_gui snapmaker
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "lmd hook without extended2.cfg: guard passes, as the hook binds nothing" {
    paxx_hook
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "gui: helixscreen with no package to bind: guard passes" {
    paxx_hook
    paxx_gui helixscreen
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "PAXX package beside our install with gui: snapmaker: guard passes" {
    paxx_hook
    paxx_gui snapmaker
    paxx_package
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "gui is read the way configparser reads it" {
    local cfg="$BATS_TEST_TMPDIR/x.cfg"
    printf '[components]\n  GUI = helixscreen  \r\n[other]\ngui: snapmaker\n' > "$cfg"
    run _paxx_selected_gui "$cfg"
    [ "$output" = "helixscreen" ]
    printf '[other]\ngui: helixscreen\n' > "$cfg"
    run _paxx_selected_gui "$cfg"
    [ -z "$output" ]
}

@test "HELIX_IGNORE_FIRMWARE_MANAGED=1 bypasses the guard" {
    paxx_hook
    paxx_gui helixscreen
    paxx_package
    export HELIX_IGNORE_FIRMWARE_MANAGED=1
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "detected hook path is reported in the abort message" {
    paxx_hook
    paxx_gui helixscreen
    paxx_package
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    [[ "$output" == *"$HELIX_FIRMWARE_MANAGED_MARKER/etc/hooks/lmd.d/30-helixscreen.sh"* ]]
}

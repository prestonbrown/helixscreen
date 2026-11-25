#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The standalone installer must refuse to run where the firmware runs HelixScreen
# itself: PAXX's Snapmaker U1 firmware, when its lmd hook
# (<root>/etc/hooks/lmd.d/30-helixscreen.sh) binds its package: extended2.cfg's
# `[components] gui` selects helixscreen AND oem/apps/helixscreen/latest/bin/helix-screen
# is executable. PAXX ships the hook whatever gui says. With the hook and the
# package present, a config that cannot be read or parsed is refused.
# HELIX_FIRMWARE_MANAGED_MARKER overrides the marker root for testing.
# HELIX_IGNORE_FIRMWARE_MANAGED=1 forces past the guard.

load helpers

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    unset _HELIX_MAIN_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    # main.sh installs the installer's ERR and EXIT handlers over bats's own,
    # which would end the run on a failing assertion instead of reporting it.
    local bats_traps
    bats_traps=$(trap -p ERR EXIT)
    . "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    eval "$bats_traps"
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

@test "hook and package with no extended2.cfg: guard passes, gui defaults to snapmaker" {
    paxx_hook
    paxx_package
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "a config only under /home/lava/printer_data is not the one the hook reads" {
    paxx_hook
    paxx_package
    local dir="$HELIX_FIRMWARE_MANAGED_MARKER/home/lava/printer_data/config/extended"
    mkdir -p "$dir"
    printf '[components]\ngui: helixscreen\n' > "$dir/extended2.cfg"
    run _refuse_if_firmware_managed
    [ "$status" -eq 0 ]
}

@test "hook and package with an unparsable config: guard aborts (fails closed)" {
    paxx_hook
    paxx_package
    local dir="$HELIX_FIRMWARE_MANAGED_MARKER/oem/printer_data/config/extended"
    mkdir -p "$dir"
    printf 'gui: snapmaker\n' > "$dir/extended2.cfg"
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
    [[ "$output" == *"gui: unreadable"* ]]
}

@test "hook and package with an unreadable config: guard aborts (fails closed)" {
    [ "$(id -u)" -ne 0 ] || skip "root reads a mode-000 file"
    paxx_hook
    paxx_package
    paxx_gui snapmaker
    chmod 000 "$HELIX_FIRMWARE_MANAGED_MARKER/oem/printer_data/config/extended/extended2.cfg"
    run _refuse_if_firmware_managed
    [ "$status" -ne 0 ]
}

# The installer's awk reader and the plugin's configparser reader
# (moonraker-plugin/helix_signal_migration.py#paxx_selected_gui) give the same
# answer for every fixture: the value, "snapmaker" when gui is absent, or
# UNREADABLE when the file does not parse.
@test "the awk gui reader matches configparser" {
    local dir="$BATS_TEST_TMPDIR/fixtures"
    mkdir -p "$dir"
    printf '[components]\nrfid: external\n  gui: helixscreen\n' > "$dir/indented-key-after-key"
    printf '[components]\ngui: snapmaker\n[web]\nx: 1\n[components]\ngui: helixscreen\n' > "$dir/duplicate-section"
    printf '[components]\ngui: helixscreen\ngui: snapmaker\n' > "$dir/duplicate-key"
    printf '[components] # the UIs\n# gui: helixscreen\n; also a comment\ngui: snapmaker\n' > "$dir/comments"
    printf '[components]\r\ngui: helixscreen\r\n' > "$dir/crlf"
    printf 'gui: helixscreen\n[components]\n' > "$dir/no-section-header"
    printf '[components]\ngui: helixscreen\n  extra\n' > "$dir/continued-value"
    printf '[components]\ngui: a\n\n  b\n' > "$dir/blank-then-continued"
    printf '[components]\nGUI = helixscreen\n' > "$dir/upper-key-equals"
    printf '[DEFAULT]\ngui: helixscreen\n[components]\nrfid: x\n' > "$dir/default-section"
    printf '[components]\nnot an option\ngui: helixscreen\n' > "$dir/no-delimiter"
    printf '[components]\ngui: a\n  [web]\n' > "$dir/indented-header"
    printf '[web]\nfrontend: fluidd\n' > "$dir/no-components"
    local f awk_out py_out
    for f in "$dir"/*; do
        run _paxx_selected_gui "$f"
        awk_out=$output
        case "$status" in 1) awk_out=snapmaker ;; 2) awk_out=UNREADABLE ;; esac
        py_out=$(python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import helix_signal_migration as m; v = m.paxx_selected_gui(sys.argv[2]); sys.stdout.write("UNREADABLE" if v is None else v)' "$WORKTREE_ROOT/moonraker-plugin" "$f")
        [ "$awk_out" = "$py_out" ] || { echo "$(basename "$f"): awk [$awk_out] configparser [$py_out]"; return 1; }
    done
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

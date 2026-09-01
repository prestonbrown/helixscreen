#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests undo_klipper_includes() in uninstall.sh (#986): the install→uninstall
# round-trip for the per-printer Klipper include mechanism. Uninstall must
# strip the marker-guarded [include ...] line from printer.cfg and remove the
# copied snippet, leaving the user's original printer.cfg content intact.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    export KLIPPER_HOME="$BATS_TEST_TMPDIR/home/sovol"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    export PRINTER_CFG="$KLIPPER_HOME/printer_data/config/printer.cfg"
    printf '%s\n' '[mcu]' 'serial: /dev/ttyS0' > "$PRINTER_CFG"

    export HELIX_KLIPPER_CFG_DIR="$BATS_TEST_TMPDIR/klipper_includes"
    mkdir -p "$HELIX_KLIPPER_CFG_DIR"
    printf '%s\n' "[output_pin demo]" "pin: gpio1" > "$HELIX_KLIPPER_CFG_DIR/demo.cfg"

    SUDO=""
    export SUDO

    # Stub functions referenced by uninstall.sh but not under test.
    kill_process_by_name() { :; }
    export -f kill_process_by_name

    unset _HELIX_KLIPPER_INCLUDE_SOURCED _HELIX_UNINSTALL_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/klipper_include.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
}

@test "undo: round-trip removes include line and snippet" {
    install_klipper_include_for_printer "demo"
    grep -qF "[include helixscreen/demo.cfg]" "$PRINTER_CFG"
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/demo.cfg" ]

    undo_klipper_includes

    # Include line + marker comment stripped; snippet removed.
    refute grep -q "include helixscreen/demo.cfg" "$PRINTER_CFG"
    refute grep -q "Added by HelixScreen installer" "$PRINTER_CFG"
    [ ! -f "$KLIPPER_HOME/printer_data/config/helixscreen/demo.cfg" ]

    # Original user content preserved.
    grep -q "serial: /dev/ttyS0" "$PRINTER_CFG"
    grep -q "\[mcu\]" "$PRINTER_CFG"
}

@test "undo: missing state file is a graceful no-op" {
    run undo_klipper_includes
    [ "$status" -eq 0 ]
}

@test "undo: leaves unrelated includes untouched" {
    # A pre-existing user include must survive the undo.
    printf '%s\n' "[include my_macros.cfg]" >> "$PRINTER_CFG"
    install_klipper_include_for_printer "demo"

    undo_klipper_includes

    grep -qF "[include my_macros.cfg]" "$PRINTER_CFG"
    ! grep -q "include helixscreen/demo.cfg" "$PRINTER_CFG"
}

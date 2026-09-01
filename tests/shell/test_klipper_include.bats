#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the generic per-printer Klipper-config include mechanism (#986).
# Exercises install_klipper_include_for_printer() in klipper_include.sh:
#   - copies a bundled cfg to printer_data/config/helixscreen/<id>.cfg
#   - adds a marker-guarded [include helixscreen/<id>.cfg] line to printer.cfg
#   - is idempotent (running twice does not duplicate the include line)
#   - a missing bundled cfg is a graceful no-op success
#   - records both actions to the state file for uninstall

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    export INCLUDE_STATE_FILE="$INSTALL_DIR/config/.klipper_includes"

    # Fake Klipper home with printer_data/config layout.
    export KLIPPER_HOME="$BATS_TEST_TMPDIR/home/sovol"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    export PRINTER_CFG="$KLIPPER_HOME/printer_data/config/printer.cfg"
    printf '%s\n' '[mcu]' 'serial: /dev/ttyS0' > "$PRINTER_CFG"

    # Test-controlled bundled-cfg dir.
    export HELIX_KLIPPER_CFG_DIR="$BATS_TEST_TMPDIR/klipper_includes"
    mkdir -p "$HELIX_KLIPPER_CFG_DIR"

    SUDO=""
    export SUDO

    unset _HELIX_KLIPPER_INCLUDE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/klipper_include.sh"
}

write_cfg() {
    local id="$1"
    printf '%s\n' "[output_pin demo]" "pin: gpio1" > "$HELIX_KLIPPER_CFG_DIR/${id}.cfg"
}

@test "include: copies cfg and adds include line" {
    write_cfg "demo"

    run install_klipper_include_for_printer "demo"
    [ "$status" -eq 0 ]

    # cfg copied into printer_data/config/helixscreen/
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/demo.cfg" ]
    grep -q "output_pin demo" "$KLIPPER_HOME/printer_data/config/helixscreen/demo.cfg"

    # include line added to printer.cfg
    grep -qF "[include helixscreen/demo.cfg]" "$PRINTER_CFG"
}

@test "include: idempotent — running twice does not duplicate include line" {
    write_cfg "demo"

    install_klipper_include_for_printer "demo"
    install_klipper_include_for_printer "demo"

    local count
    count=$(grep -cF "[include helixscreen/demo.cfg]" "$PRINTER_CFG")
    [ "$count" -eq 1 ]
}

@test "include: missing bundled cfg is a graceful no-op success" {
    run install_klipper_include_for_printer "no_such_printer"
    [ "$status" -eq 0 ]

    # printer.cfg untouched (no include line, original lines intact).
    refute grep -q "include helixscreen" "$PRINTER_CFG"
    grep -q "serial: /dev/ttyS0" "$PRINTER_CFG"
}

@test "include: records both actions to state file" {
    write_cfg "demo"
    run install_klipper_include_for_printer "demo"
    [ "$status" -eq 0 ]

    [ -f "$INCLUDE_STATE_FILE" ]
    # Records the copied cfg and the modified printer.cfg for uninstall undo.
    grep -qF "cfg:$KLIPPER_HOME/printer_data/config/helixscreen/demo.cfg" "$INCLUDE_STATE_FILE"
    grep -qF "include:$PRINTER_CFG:helixscreen/demo.cfg" "$INCLUDE_STATE_FILE"
}

@test "include: missing printer.cfg warns and skips without failing" {
    write_cfg "demo"
    rm -f "$PRINTER_CFG"

    run install_klipper_include_for_printer "demo"
    [ "$status" -eq 0 ]
}

@test "include: idempotent across the state file too (no dup records)" {
    write_cfg "demo"
    install_klipper_include_for_printer "demo"
    install_klipper_include_for_printer "demo"

    local c
    c=$(grep -cF "include:$PRINTER_CFG:helixscreen/demo.cfg" "$INCLUDE_STATE_FILE")
    [ "$c" -eq 1 ]
}

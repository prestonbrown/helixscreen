#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Guards on the two user-settable paths the installer destroys:
#
#   TMP_DIR      rm -rf "$TMP_DIR"                        (common.sh)
#   INSTALL_DIR  mv aside + rm -rf + config/ sibling sweep (release.sh, uninstall.sh)
#
# and on --clean's confirmation, which must not treat a non-TTY stdin as
# consent (the documented invocation is `curl … | sh -s -- --clean`, whose
# stdin is the pipe carrying the script).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    # _safe_remove_tmp_dir's mountpoint probe uses `stat -c '%d'`.
    install_gnu_stat_shim

    unset _HELIX_COMMON_SOURCED _HELIX_PLATFORM_SOURCED
    export SUDO=""
    export TMP_DIR=""
    export INSTALL_DIR=""

    # common.sh owns the guards; platform.sh is their only caller. Production
    # always has both (bundle-installer.sh emits common.sh first), so tests
    # must load both or they exercise a shape that never ships.
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    # Capture log output so refusal messages are assertable. Must come AFTER
    # common.sh, which defines the real (stderr + ANSI) implementations.
    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_error log_success
}

# ===========================================================================
# validate_tmp_dir
# ===========================================================================

@test "validate_tmp_dir: accepts the installer's own scratch names" {
    for d in /tmp/helixscreen-install \
             /var/tmp/helixscreen-install \
             /home/pi/.helixscreen-install \
             /user-resource/helixscreen-install \
             /opt/helixscreen-install/; do
        run validate_tmp_dir "$d"
        [ "$status" -eq 0 ] || fail "rejected legitimate TMP_DIR: $d ($output)"
    done
}

@test "validate_tmp_dir: accepts the in-app updater's staging dir handoff" {
    # update_checker.cpp STAGING_NAME — install.sh is exec'd with this TMP_DIR.
    run validate_tmp_dir "/home/pi/.helix-update-staging"
    [ "$status" -eq 0 ]
}

@test "validate_tmp_dir: refuses a mount root (the /mnt/UDISK wipe)" {
    run validate_tmp_dir "/mnt/UDISK"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refusing to use TMP_DIR='/mnt/UDISK'"* ]]
}

@test "validate_tmp_dir: refuses a home directory" {
    run validate_tmp_dir "/home/pi"
    [ "$status" -ne 0 ]
    [[ "$output" == *"helixscreen-install"* ]]
}

@test "validate_tmp_dir: refuses the filesystem root, relative paths, and traversal" {
    for d in / /data /usr/data relative/helixscreen-install /a/helixscreen-install/../..; do
        run validate_tmp_dir "$d"
        [ "$status" -ne 0 ] || fail "accepted unsafe TMP_DIR: $d"
    done
}

# ===========================================================================
# validate_install_dir
# ===========================================================================

@test "validate_install_dir: accepts every auto-detected platform path" {
    for d in /opt/helixscreen \
             /srv/helixscreen \
             /usr/data/helixscreen \
             /userdata/helixscreen \
             /user-resource/helixscreen \
             /root/printer_software/helixscreen \
             /home/biqu/helixscreen; do
        run validate_install_dir "$d"
        [ "$status" -eq 0 ] || fail "rejected legitimate INSTALL_DIR: $d ($output)"
    done
}

@test "validate_install_dir: refuses a bare data directory" {
    for d in /home/pi /mnt/UDISK /usr/data /; do
        run validate_install_dir "$d"
        [ "$status" -ne 0 ] || fail "accepted unsafe INSTALL_DIR: $d"
    done
}

@test "validate_install_dir: refusal names the offending value and the rule" {
    run validate_install_dir "/home/pi"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refusing to use INSTALL_DIR='/home/pi'"* ]]
    [[ "$output" == *"must contain 'helixscreen'"* ]]
}

# ===========================================================================
# _safe_remove_tmp_dir — last line of defence on the actual rm -rf
# ===========================================================================

@test "_safe_remove_tmp_dir: removes a properly named scratch dir" {
    local d="$BATS_TEST_TMPDIR/helixscreen-install"
    mkdir -p "$d/extract"
    touch "$d/extract/payload"

    _safe_remove_tmp_dir "$d"
    [ ! -d "$d" ]
}

@test "_safe_remove_tmp_dir: refuses to rm -rf a directory that is not ours" {
    local d="$BATS_TEST_TMPDIR/printer_data"
    mkdir -p "$d/config"
    echo "user gcode macros" > "$d/config/printer.cfg"

    run _safe_remove_tmp_dir "$d"
    [ "$status" -eq 0 ]
    [ -f "$d/config/printer.cfg" ]
    [[ "$output" == *"Refusing to rm -rf"* ]]
}

# ===========================================================================
# Wiring: the guards actually gate the code paths that honour the overrides
# ===========================================================================

@test "detect_tmp_dir: aborts on an unsafe user override instead of using it" {
    export TMP_DIR="/home/pi"
    run detect_tmp_dir
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refusing to use TMP_DIR"* ]]
}

@test "detect_tmp_dir: still honours a safe user override verbatim" {
    export TMP_DIR="/home/pi/helixscreen-install"
    detect_tmp_dir
    [ "$TMP_DIR" = "/home/pi/helixscreen-install" ]
}

@test "detect_pi_install_dir: aborts on an unsafe user override" {
    _USER_INSTALL_DIR="/home/pi"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    run detect_pi_install_dir
    [ "$status" -ne 0 ]
    [[ "$output" == *"Refusing to use INSTALL_DIR"* ]]
}

@test "detect_pi_install_dir: still honours a safe user override" {
    _USER_INSTALL_DIR="/custom/path/helixscreen"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/custom/path/helixscreen" ]
}

# ===========================================================================
# --clean confirmation (uninstall.sh)
# ===========================================================================

load_uninstall_module() {
    unset _HELIX_UNINSTALL_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
}

@test "confirm_clean_install: refuses when stdin is a pipe and --yes was not given" {
    load_uninstall_module
    ASSUME_YES=false

    # bats gives the test body a non-TTY stdin, which is exactly the
    # `curl … | sh -s -- --clean` shape.
    run confirm_clean_install
    [ "$status" -eq 1 ]
    [[ "$output" == *"Refusing to run --clean without confirmation"* ]]
    [[ "$output" == *"--yes"* ]]
}

@test "confirm_clean_install: proceeds when --yes was given" {
    load_uninstall_module
    ASSUME_YES=true

    run confirm_clean_install
    [ "$status" -eq 0 ]
    [[ "$output" == *"proceeding without confirmation"* ]]
}

@test "installer parses --yes and hands it to the clean confirmation" {
    # main.sh's parser and uninstall.sh's gate must agree on the variable name;
    # a rename on either side silently restores the skip-the-prompt behavior.
    grep -q 'ASSUME_YES=true' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -q -- '--yes|-y|--force' "$WORKTREE_ROOT/scripts/lib/installer/main.sh"
    grep -q 'ASSUME_YES:-false' "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
}

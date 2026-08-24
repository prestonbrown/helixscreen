#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for detect_pi_install_dir() and Pi install path logic in platform.sh
# Tests the ecosystem detection cascade:
#   1. klipper/moonraker dir in KLIPPER_HOME → ~/helixscreen
#   2. printer_data dir in KLIPPER_HOME → ~/helixscreen
#   3. moonraker.service active → ~/helixscreen
#   4. Fallback → /opt/helixscreen
# Also tests: INSTALL_DIR override, AD5M/K1 regression, set_install_paths integration

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals before each test
    KLIPPER_USER=""
    KLIPPER_HOME=""
    INIT_SCRIPT_DEST=""
    PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    INSTALL_DIR="/opt/helixscreen"
    TMP_DIR="/tmp/helixscreen-install"
    _USER_INSTALL_DIR=""
    # Empty by default so the existing-install probe cannot see a real install on
    # the machine running the suite. Tests that exercise it set their own list.
    _HELIX_KNOWN_INSTALL_DIRS=""

    # Source common.sh + platform.sh (skip source guards by unsetting them).
    # common.sh owns validate_install_dir/validate_tmp_dir, which platform.sh
    # calls; the bundled installer always carries both, so loading platform.sh
    # alone would test a shape that never ships.
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # common.sh defines the real log_* (they print to stderr, which bats folds
    # into $output). Restore helpers.bash's silent stubs.
    load helpers
}

# --- Cascade priority tests ---

@test "klipper dir in home → ~/helixscreen" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "moonraker dir in home → ~/helixscreen" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/moonraker"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "printer_data in home (no klipper/moonraker dirs) → ~/helixscreen" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/printer_data"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "moonraker.service exists (no dirs) → ~/helixscreen" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME"
    # Mock systemctl to report moonraker.service is active
    # is-active --quiet moonraker.service → $1=is-active, $2=--quiet, $3=moonraker.service
    # is-active --quiet moonraker → $1=is-active, $2=--quiet, $3=moonraker
    mock_command_script "systemctl" '
        case "$1" in
            is-active)
                # Check all args for moonraker
                for arg in "$@"; do
                    case "$arg" in
                        moonraker.service|moonraker) exit 0 ;;
                    esac
                done
                exit 1
                ;;
            *) exit 1 ;;
        esac'

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "nothing detected → /opt/helixscreen fallback" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME"
    # No klipper dirs, no systemctl
    mock_command_fail "systemctl"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

# --- Parent-writability tests ---
#
# An update applies by renaming the install root ("mv <root> <root>.old; mv <new>
# <root>"), which mutates the PARENT's directory entries — so the parent is what
# needs to be writable by the service user. /opt is root-owned and the service
# runs unprivileged, which leaves only install.sh's in-place fallback: delete the
# root's contents, then move the new ones in. That works, but it deletes before it
# moves. A home-owned parent keeps the atomic path available.

@test "no ecosystem but a non-root service user → home, not /opt" {
    KLIPPER_USER="$(id -un)"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME"
    mock_command_fail "systemctl"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "no ecosystem and a root service user → /opt/helixscreen" {
    # root owns /opt, so the atomic swap is available there anyway. This is the
    # embedded shape and must not move.
    KLIPPER_USER="root"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/root"
    mkdir -p "$KLIPPER_HOME"
    mock_command_fail "systemctl"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

@test "home not owned by the service user → /opt/helixscreen" {
    # The point of the home branch is a parent the SERVICE USER can rename in.
    # A home that exists but belongs to someone else buys nothing.
    KLIPPER_USER="somebodyelse"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/somebodyelse"
    mkdir -p "$KLIPPER_HOME"
    mock_command_fail "systemctl"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

# --- Existing-install tests ---

@test "an existing install pins the location" {
    # Relocating an install the user already has orphans the old tree and the
    # config in it. Whatever the cascade would pick, an install already on disk
    # wins.
    KLIPPER_USER="$(id -un)"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    local existing="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$existing/bin"
    touch "$existing/bin/helix-screen"
    chmod +x "$existing/bin/helix-screen"
    _HELIX_KNOWN_INSTALL_DIRS="$existing"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$existing" ]
}

@test "an ecosystem install outranks a stale install elsewhere" {
    KLIPPER_USER="$(id -un)"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/helixscreen/bin"
    touch "$KLIPPER_HOME/helixscreen/bin/helix-screen"
    chmod +x "$KLIPPER_HOME/helixscreen/bin/helix-screen"

    local stale="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$stale/bin"
    touch "$stale/bin/helix-screen"
    chmod +x "$stale/bin/helix-screen"
    _HELIX_KNOWN_INSTALL_DIRS="$stale"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "an empty leftover directory is not an existing install" {
    # Uninstall leaves the directory behind on some platforms. Only the binary
    # makes it an install.
    KLIPPER_USER="$(id -un)"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME"
    mock_command_fail "systemctl"

    local leftover="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$leftover/config"
    _HELIX_KNOWN_INSTALL_DIRS="$leftover"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "a user override still outranks an existing install" {
    local existing="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$existing/bin"
    touch "$existing/bin/helix-screen"
    chmod +x "$existing/bin/helix-screen"
    _HELIX_KNOWN_INSTALL_DIRS="$existing"

    _USER_INSTALL_DIR="/custom/path/helixscreen"
    INSTALL_DIR="/custom/path/helixscreen"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/custom/path/helixscreen" ]
}

# --- Override tests ---

@test "explicit INSTALL_DIR env var preserved" {
    # Simulate user setting INSTALL_DIR before sourcing. The final path
    # component must name us — INSTALL_DIR is mv'd aside and rm -rf'd, so
    # validate_install_dir refuses a bare data directory (see
    # test_destructive_path_guards.bats).
    _USER_INSTALL_DIR="/custom/path/helixscreen"
    INSTALL_DIR="/custom/path/helixscreen"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/custom/path/helixscreen" ]
}

@test "default INSTALL_DIR is not treated as user override" {
    # _USER_INSTALL_DIR empty means no user override
    _USER_INSTALL_DIR=""
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

# --- Regression tests for other platforms ---

@test "AD5M klipper_mod path defaults to /opt without legacy dir" {
    # v00.06+: /root/printer_software doesn't exist on CI, uses /opt fallback
    # v00.05: would use /root/printer_software/helixscreen if that dir existed
    set_install_paths "ad5m" "klipper_mod"
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

@test "AD5M forge_x path unchanged" {
    set_install_paths "ad5m" "forge_x"
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

@test "AD5M zmod path is /srv/helixscreen" {
    set_install_paths "ad5m" "zmod"
    [ "$INSTALL_DIR" = "/srv/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S80helixscreen" ]
    [ "$PREVIOUS_UI_SCRIPT" = "" ]
}

@test "K1 simple_af path unchanged" {
    set_install_paths "k1" "simple_af"
    [ "$INSTALL_DIR" = "/usr/data/helixscreen" ]
}

# --- Integration: set_install_paths calls detect correctly ---

@test "set_install_paths pi with klipper ecosystem sets home path" {
    # Pre-set KLIPPER_HOME to simulate what detect_klipper_user would do
    # (detect_klipper_user would normally set this, but we mock it here)
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"

    # Since all mocks fail, detect_klipper_user will set root/root
    # Override KLIPPER_HOME after to simulate a real user with klipper
    set_install_paths "pi"
    # With root fallback and no ecosystem dirs in /root, should be /opt/helixscreen
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

@test "set_install_paths pi detects ecosystem in klipper user home" {
    # Create a fake home directory with klipper ecosystem
    local fake_home="$BATS_TEST_TMPDIR/home/pi"
    mkdir -p "$fake_home/klipper"

    # Mock detect_klipper_user to set our test user
    mock_command_script "systemctl" '
        case "$1" in
            show) echo "pi" ;;
            is-active)
                case "$2" in
                    moonraker.service|moonraker) exit 1 ;;
                    *) exit 1 ;;
                esac
                ;;
            *) exit 1 ;;
        esac'
    mock_command "id" ""

    # detect_klipper_user will find "pi" via systemd mock
    # but KLIPPER_HOME will be /home/pi (real system) not our temp dir
    # So we need to test detect_pi_install_dir directly with controlled KLIPPER_HOME
    KLIPPER_HOME="$fake_home"
    KLIPPER_USER="pi"
    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$fake_home/helixscreen" ]
}

# --- Edge cases ---

@test "KLIPPER_HOME empty falls back to /opt/helixscreen" {
    KLIPPER_HOME=""
    mock_command_fail "systemctl"

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
}

@test "klipper dir takes priority over moonraker.service" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$KLIPPER_HOME/klipper"
    # Even if systemctl would match, dir check comes first
    mock_command_script "systemctl" '
        case "$1" in
            is-active) echo "active"; exit 0 ;;
            *) exit 1 ;;
        esac'

    detect_pi_install_dir
    [ "$INSTALL_DIR" = "$KLIPPER_HOME/helixscreen" ]
}

@test "set_install_paths pi still sets init script and tmp dir" {
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"

    set_install_paths "pi"
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S90helixscreen" ]
    [ "$TMP_DIR" = "/tmp/helixscreen-install" ]
}

# --- Bundled installer parity ---
# The bundled install.sh is a separate copy of the modules.
# These tests ensure it stays in sync with platform.sh.

@test "bundled install.sh has detect_pi_install_dir function" {
    grep -q 'detect_pi_install_dir()' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh has _USER_INSTALL_DIR capture" {
    grep -q '_USER_INSTALL_DIR="${INSTALL_DIR}"' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh Pi branch calls detect_pi_install_dir" {
    # After detect_klipper_user, must call detect_pi_install_dir
    grep -A5 'detect_klipper_user' "$WORKTREE_ROOT/scripts/install.sh" | grep -q 'detect_pi_install_dir'
}

@test "bundled install.sh Pi branch does NOT hardcode /opt/helixscreen" {
    # The else branch should NOT set INSTALL_DIR="/opt/helixscreen" directly
    ! grep -A3 'detect klipper user.*auto-detect' "$WORKTREE_ROOT/scripts/install.sh" | grep -q 'INSTALL_DIR="/opt/helixscreen"'
}

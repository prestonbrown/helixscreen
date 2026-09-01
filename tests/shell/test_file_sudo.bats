#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for file_sudo() helper and minimal-privilege file operations.
# Verifies that installer operations don't unnecessarily escalate to sudo,
# preventing root-owned config files in user-writable directories.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_MOONRAKER_SOURCED _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    # host_profile.sh: the mod-ownership guard the moonraker.sh writers below call
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"

    export SUDO="sudo"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export KLIPPER_HOME=""
    export PLATFORM=""

    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"
}

# =============================================================================
# file_sudo() — core logic
# =============================================================================

@test "file_sudo: returns empty for user-writable existing file" {
    local testfile="$BATS_TEST_TMPDIR/writable_file"
    echo "content" > "$testfile"

    local result
    result=$(file_sudo "$testfile")
    [ "$result" = "" ]
}

@test "file_sudo: returns SUDO for non-writable existing file" {
    local testfile="$BATS_TEST_TMPDIR/readonly_file"
    echo "content" > "$testfile"
    chmod 444 "$testfile"

    local result
    result=$(file_sudo "$testfile")
    [ "$result" = "sudo" ]
}

@test "file_sudo: returns empty for new file in writable directory" {
    local testfile="$BATS_TEST_TMPDIR/new_file_that_doesnt_exist"
    # Ensure the file doesn't exist but parent dir is writable
    rm -f "$testfile"

    local result
    result=$(file_sudo "$testfile")
    [ "$result" = "" ]
}

@test "file_sudo: returns SUDO for new file in non-writable directory" {
    local testdir="$BATS_TEST_TMPDIR/readonly_dir"
    mkdir -p "$testdir"
    chmod 555 "$testdir"

    local result
    result=$(file_sudo "$testdir/new_file")
    [ "$result" = "sudo" ]

    # Cleanup
    chmod 755 "$testdir"
}

@test "file_sudo: returns empty when SUDO is empty (already root)" {
    SUDO=""
    local testfile="$BATS_TEST_TMPDIR/readonly_file"
    echo "content" > "$testfile"
    chmod 444 "$testfile"

    local result
    result=$(file_sudo "$testfile")
    # Even though file isn't writable, SUDO="" means we're root, so no escalation
    [ "$result" = "" ]
}

# =============================================================================
# file_sudo() used in moonraker.sh operations
# =============================================================================

@test "moonraker add_update_manager_section: uses file_sudo for user-writable conf" {
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    local conf_dir="$BATS_TEST_TMPDIR/home/testuser/printer_data/config"
    mkdir -p "$conf_dir"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    local conf="$conf_dir/moonraker.conf"

    # Create a user-writable moonraker.conf
    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0
port: 7125
CONF

    # SUDO is set but file is writable — should NOT need sudo
    # If it tries to use sudo, it will fail (sudo not mocked)
    mock_command_fail "sudo"

    add_update_manager_section "$conf"

    # Section was added without needing sudo
    grep -q '^\[update_manager helixscreen\]' "$conf"
}

@test "moonraker add_update_manager_section: backup is created without sudo" {
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    local conf_dir="$BATS_TEST_TMPDIR/home/testuser/printer_data/config"
    mkdir -p "$conf_dir"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    local conf="$conf_dir/moonraker.conf"

    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0
CONF

    mock_command_fail "sudo"

    add_update_manager_section "$conf"

    [ -f "${conf}.bak.helixscreen" ]
}

@test "moonraker ensure_moonraker_asvc: uses file_sudo for user-writable asvc" {
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    local conf_dir="$BATS_TEST_TMPDIR/home/testuser/printer_data/config"
    local printer_data="$BATS_TEST_TMPDIR/home/testuser/printer_data"
    mkdir -p "$conf_dir"
    local conf="$conf_dir/moonraker.conf"
    echo "[server]" > "$conf"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"

    mock_command_fail "sudo"

    ensure_moonraker_asvc "$conf"

    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
}

@test "moonraker remove_update_manager_section: works without sudo on user-writable conf" {
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    local conf_dir="$BATS_TEST_TMPDIR/home/testuser/printer_data/config"
    mkdir -p "$conf_dir"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    local conf="$conf_dir/moonraker.conf"
    MOONRAKER_CONF_PATHS="$conf"

    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0

# HelixScreen Update Manager
# Added by HelixScreen installer - enables one-click updates from Mainsail/Fluidd
[update_manager helixscreen]
type: zip
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
managed_services: helixscreen
CONF

    # macOS sed workaround (same as test_moonraker_config.bats)
    if [ "$(uname)" = "Darwin" ]; then
        mkdir -p "$BATS_TEST_TMPDIR/sedbin"
        cat > "$BATS_TEST_TMPDIR/sedbin/sed" << 'SEDWRAP'
#!/bin/sh
if [ "$1" = "-i" ] && [ -n "$2" ] && [ "$2" != "" ]; then
    case "$2" in
        s\|*|s/*|/*) exit 1 ;;
        '') exec /usr/bin/sed "$@" ;;
    esac
fi
exec /usr/bin/sed "$@"
SEDWRAP
        chmod +x "$BATS_TEST_TMPDIR/sedbin/sed"
        export PATH="$BATS_TEST_TMPDIR/sedbin:$PATH"
    fi

    mock_command_fail "sudo"

    remove_update_manager_section

    refute grep -q '^\[update_manager helixscreen\]' "$conf"
    grep -q '^\[server\]' "$conf"
}

# =============================================================================
# file_sudo() used in platform.sh symlink operations
# =============================================================================

@test "setup_config_symlink: creates config directory without sudo in user-writable dir" {
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"

    mock_command_fail "sudo"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    # New layout creates a real directory, not a symlink
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

@test "setup_config_symlink: migrates old symlink to directory layout without sudo" {
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Old layout used a directory symlink
    ln -s "/old/wrong/path" "$KLIPPER_HOME/printer_data/config/helixscreen"

    mock_command_fail "sudo"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    # Old symlink should be replaced with a real directory
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

# =============================================================================
# Bundled installer parity
# =============================================================================

@test "bundled install.sh contains file_sudo function" {
    grep -q 'file_sudo()' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh uses file_sudo for moonraker.conf operations" {
    grep -q 'file_sudo.*conf' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled uninstall.sh contains file_sudo function" {
    grep -q 'file_sudo()' "$WORKTREE_ROOT/scripts/uninstall.sh"
}

@test "bundled install.sh does NOT use bare SUDO for moonraker.conf backup" {
    # The bundled moonraker module section should use $fs (file_sudo result), not $SUDO
    # Look for the old pattern: $SUDO cp ... bak.helixscreen
    ! grep -q '\$SUDO cp.*bak\.helixscreen' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh does NOT use bare SUDO for tee -a moonraker.conf" {
    # The old pattern: | $SUDO tee -a ... moonraker.conf
    # Should now use file_sudo or $fs
    ! grep -q '\$SUDO tee -a.*conf' "$WORKTREE_ROOT/scripts/install.sh"
}

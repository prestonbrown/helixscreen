#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for moonraker.conf path detection
# Tests find_moonraker_conf() with and without KLIPPER_HOME

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals
    KLIPPER_HOME=""
    INSTALL_DIR="/opt/helixscreen"

    # Source moonraker module
    unset _HELIX_MOONRAKER_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"
}

# --- Dynamic detection (KLIPPER_HOME set) ---

@test "finds moonraker.conf via KLIPPER_HOME when file exists" {
    local test_home="$BATS_TEST_TMPDIR/home/biqu"
    mkdir -p "$test_home/printer_data/config"
    touch "$test_home/printer_data/config/moonraker.conf"
    KLIPPER_HOME="$test_home"

    result=$(find_moonraker_conf)
    [ "$result" = "$test_home/printer_data/config/moonraker.conf" ]
}

@test "KLIPPER_HOME path checked before static paths" {
    # Create both a KLIPPER_HOME path and a static path
    local test_home="$BATS_TEST_TMPDIR/home/testuser"
    mkdir -p "$test_home/printer_data/config"
    touch "$test_home/printer_data/config/moonraker.conf"
    KLIPPER_HOME="$test_home"

    # Override static paths to also have a match
    local static_home="$BATS_TEST_TMPDIR/home/pi"
    mkdir -p "$static_home/printer_data/config"
    touch "$static_home/printer_data/config/moonraker.conf"

    result=$(find_moonraker_conf)
    # Should match KLIPPER_HOME first, not static
    [ "$result" = "$test_home/printer_data/config/moonraker.conf" ]
}

@test "KLIPPER_HOME set but no file there: falls through to static" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/nobody"
    # No file created at KLIPPER_HOME path

    # Static paths won't match either in test env
    result=$(find_moonraker_conf)
    [ -z "$result" ]
}

# --- Static fallback paths ---

@test "static paths include /home/biqu/ entry" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "/home/biqu/"
}

@test "static paths include /home/pi/ entry (regression)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "/home/pi/"
}

@test "static paths include /home/mks/ entry (regression)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "/home/mks/"
}

@test "first matching static path wins" {
    # We can't easily create files at /home/pi etc in tests,
    # but we can verify the function returns the first match
    # by temporarily overriding MOONRAKER_CONF_PATHS
    local dir1="$BATS_TEST_TMPDIR/first"
    local dir2="$BATS_TEST_TMPDIR/second"
    mkdir -p "$dir1" "$dir2"
    touch "$dir1/moonraker.conf" "$dir2/moonraker.conf"

    MOONRAKER_CONF_PATHS="$dir1/moonraker.conf $dir2/moonraker.conf"
    KLIPPER_HOME=""

    result=$(find_moonraker_conf)
    [ "$result" = "$dir1/moonraker.conf" ]
}

# --- No match ---

@test "returns empty string when no moonraker.conf found" {
    KLIPPER_HOME=""
    MOONRAKER_CONF_PATHS="/nonexistent/path/moonraker.conf"

    result=$(find_moonraker_conf)
    [ -z "$result" ]
}

# --- generate_update_manager_config (type: web) ---

@test "generate_update_manager_config emits type: web" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    echo "$config" | grep -q "type: web"
}

@test "generate_update_manager_config has correct repo" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    echo "$config" | grep -q "repo: prestonbrown/helixscreen"
}

@test "generate_update_manager_config path equals INSTALL_DIR" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    echo "$config" | grep -q "path: /opt/helixscreen"
}

@test "generate_update_manager_config has persistent_files" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    echo "$config" | grep -q "persistent_files:"
    echo "$config" | grep -q "config/helixconfig.json"
}

@test "generate_update_manager_config does NOT have install_script" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    ! echo "$config" | grep -q "install_script"
}

# --- has_old_git_repo_section ---

@test "has_old_git_repo_section detects old git_repo format" {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0

[update_manager helixscreen]
type: git_repo
channel: stable
path: ~/helixscreen-repo
origin: https://github.com/prestonbrown/helixscreen.git
primary_branch: main
managed_services: helixscreen
install_script: scripts/install.sh
CONF
    has_old_git_repo_section "$conf"
}

@test "has_old_git_repo_section returns false for type: zip" {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0

[update_manager helixscreen]
type: zip
channel: stable
repo: prestonbrown/helixscreen
path: ~/helixscreen
managed_services: helixscreen
CONF
    ! has_old_git_repo_section "$conf"
}

@test "has_old_git_repo_section returns false when no section" {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0

[update_manager mainsail]
type: web
CONF
    ! has_old_git_repo_section "$conf"
}

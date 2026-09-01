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
    KLIPPER_CONFIG_DIR=""
    INSTALL_DIR="/opt/helixscreen"
    SUDO=""

    # common.sh supplies klipper_config_dir() and file_sudo(), which
    # find_moonraker_conf() and ensure_moonraker_asvc() call. host_profile.sh
    # rides along in the bundle's module order and carries the mod-ownership
    # guard find_moonraker_conf() consults. The bundled installer always
    # sources all three ahead of moonraker.sh.
    unset _HELIX_MOONRAKER_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"
    # common.sh defines the real log_* (stderr); restore helpers.bash's stubs.
    load helpers
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

@test "static paths include /home/qidi/ entry (QIDI 01.01.02 user rename, #1047)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "/home/qidi/"
}

@test "static paths include AD5X ZMOD /opt/config/printer_data path (#938)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "^/opt/config/printer_data/config/moonraker.conf$"
}

@test "static paths include AD5X ZMOD /usr/data/config/printer_data path (#938)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "^/usr/data/config/printer_data/config/moonraker.conf$"
}

@test "static paths include COSMOS /etc/klipper/config path (Elegoo CC1)" {
    echo "$MOONRAKER_CONF_PATHS" | grep -q "^/etc/klipper/config/moonraker\.conf$"
}

@test "no static path assumes printer_data for the COSMOS entry" {
    # Guards against someone 'normalising' the COSMOS entry into a
    # printer_data shape. There is no printer_data anywhere on a CC1.
    ! echo "$MOONRAKER_CONF_PATHS" | grep -q "^/etc/klipper/.*printer_data"
}

# --- COSMOS / CC1: no printer_data anywhere on the device ---

@test "klipper_config_dir derives printer_data/config from KLIPPER_HOME by default" {
    KLIPPER_HOME="/home/pi"
    KLIPPER_CONFIG_DIR=""
    [ "$(klipper_config_dir)" = "/home/pi/printer_data/config" ]
}

@test "klipper_config_dir honours an explicit KLIPPER_CONFIG_DIR" {
    KLIPPER_HOME="/root"
    KLIPPER_CONFIG_DIR="/etc/klipper/config"
    [ "$(klipper_config_dir)" = "/etc/klipper/config" ]
}

@test "klipper_config_dir is empty when nothing is known" {
    KLIPPER_HOME=""
    KLIPPER_CONFIG_DIR=""
    [ -z "$(klipper_config_dir)" ]
}

@test "find_moonraker_conf finds the COSMOS layout via KLIPPER_CONFIG_DIR" {
    # Mirrors a real CC1: /etc/klipper/config/moonraker.conf, and KLIPPER_HOME
    # is /root with NO printer_data anywhere.
    local cosmos="$BATS_TEST_TMPDIR/etc/klipper/config"
    mkdir -p "$cosmos" "$cosmos/moonraker-readonly"
    touch "$cosmos/moonraker.conf" "$cosmos/printer.cfg"
    touch "$cosmos/moonraker-readonly/moonraker.conf"

    KLIPPER_HOME="$BATS_TEST_TMPDIR/root"
    KLIPPER_CONFIG_DIR="$cosmos"
    MOONRAKER_CONF_PATHS="/nonexistent/moonraker.conf"

    result=$(find_moonraker_conf)
    [ "$result" = "$cosmos/moonraker.conf" ]
}

@test "KLIPPER_CONFIG_DIR beats the KLIPPER_HOME printer_data derivation" {
    local cosmos="$BATS_TEST_TMPDIR/etc/klipper/config"
    mkdir -p "$cosmos"
    touch "$cosmos/moonraker.conf"

    # A printer_data tree also exists — the explicit dir must still win.
    local home="$BATS_TEST_TMPDIR/root"
    mkdir -p "$home/printer_data/config"
    touch "$home/printer_data/config/moonraker.conf"

    KLIPPER_HOME="$home"
    KLIPPER_CONFIG_DIR="$cosmos"

    result=$(find_moonraker_conf)
    [ "$result" = "$cosmos/moonraker.conf" ]
}

@test "CC1 platform branch points KLIPPER_CONFIG_DIR at /etc/klipper/config" {
    unset _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    load helpers

    set_install_paths cc1 >/dev/null 2>&1

    [ "$KLIPPER_CONFIG_DIR" = "/etc/klipper/config" ]
    [ "$(klipper_config_dir)" = "/etc/klipper/config" ]
}

@test "non-CC1 platform leaves KLIPPER_CONFIG_DIR derived, not stale" {
    unset _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    load helpers

    set_install_paths cc1 >/dev/null 2>&1
    set_install_paths k1 stock_klipper >/dev/null 2>&1

    [ -z "$KLIPPER_CONFIG_DIR" ]
    [ "$(klipper_config_dir)" = "/usr/data/printer_data/config" ]
}

# --- ensure_moonraker_asvc path derivation ---

@test "ensure_moonraker_asvc derives the asvc one level above the config dir" {
    local pd="$BATS_TEST_TMPDIR/home/pi/printer_data"
    mkdir -p "$pd/config"
    touch "$pd/config/moonraker.conf"
    printf 'klipper\nmoonraker\n' > "$pd/moonraker.asvc"

    ensure_moonraker_asvc "$pd/config/moonraker.conf"
    grep -q '^helixscreen$' "$pd/moonraker.asvc"
}

@test "ensure_moonraker_asvc finds the COSMOS asvc at /etc/klipper/moonraker.asvc" {
    # Verified on a real CC1: the allowlist sits beside config/, not under it.
    local klipper="$BATS_TEST_TMPDIR/etc/klipper"
    mkdir -p "$klipper/config"
    touch "$klipper/config/moonraker.conf"
    printf 'klipper\nmoonraker\n' > "$klipper/moonraker.asvc"

    ensure_moonraker_asvc "$klipper/config/moonraker.conf"
    grep -q '^helixscreen$' "$klipper/moonraker.asvc"
    # Nothing invented inside config/
    [ ! -e "$klipper/config/moonraker.asvc" ]
}

@test "ensure_moonraker_asvc is idempotent" {
    local klipper="$BATS_TEST_TMPDIR/etc/klipper"
    mkdir -p "$klipper/config"
    touch "$klipper/config/moonraker.conf"
    printf 'klipper\nmoonraker\n' > "$klipper/moonraker.asvc"

    ensure_moonraker_asvc "$klipper/config/moonraker.conf"
    ensure_moonraker_asvc "$klipper/config/moonraker.conf"
    [ "$(grep -c '^helixscreen$' "$klipper/moonraker.asvc")" -eq 1 ]
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

# --- Mod hosts: the mod's own moonraker.conf is never ours to edit ----------
#
# On a Forge-X/Z-Mod host the mod's moonraker.conf is a git-tracked file in
# the mod's repo: writing our stanza there dirties their checkout and their
# OTA (git clean in Moonraker's git_repo flow) drops it again. The sanctioned
# include point is user.moonraker.conf in the mod_data sibling, which the
# mod's conf already includes. find_moonraker_conf must return that, and must
# never return any path host_path_is_mod_owned accepts -- including a conf at
# a plain location symlinked INTO the mod tree.

@test "mod host: returns the mod's user conf even when the mod's own conf is discoverable" {
    SANDBOX="$BATS_TEST_TMPDIR/modhost"
    mkdir -p "$SANDBOX/config/mod" "$SANDBOX/config/mod_data"
    touch "$SANDBOX/config/mod/moonraker.conf"

    HOST_MOD_ROOT="$SANDBOX/config/mod"
    HOST_MOONRAKER_USER_CONF="$SANDBOX/config/mod_data/user.moonraker.conf"
    KLIPPER_HOME=""
    # The mod's own conf is the one static discovery would find first.
    MOONRAKER_CONF_PATHS="$SANDBOX/config/mod/moonraker.conf"

    result=$(find_moonraker_conf)
    [ "$result" = "$HOST_MOONRAKER_USER_CONF" ] || fail "returned '$result'"
}

@test "mod host: a moonraker.conf symlinked into the mod tree is never returned" {
    # Probe found no tree (no marker), but the tree still counts as mod-owned
    # and host_path_is_mod_owned resolves symlinks: a conf at a plain location
    # pointing into the mod tree must be skipped, not handed to the stanza
    # writer. With no user conf known either, the honest answer is empty.
    SANDBOX="$BATS_TEST_TMPDIR/symlinkhost"
    mkdir -p "$SANDBOX/config/mod" "$SANDBOX/plain/config"
    touch "$SANDBOX/config/mod/moonraker.conf"
    ln -s "$SANDBOX/config/mod/moonraker.conf" "$SANDBOX/plain/config/moonraker.conf"

    HOST_MOD_ROOT="$SANDBOX/config/mod"
    HOST_MOONRAKER_USER_CONF=""
    KLIPPER_HOME=""
    MOONRAKER_CONF_PATHS="$SANDBOX/plain/config/moonraker.conf"

    result=$(find_moonraker_conf)
    [ -z "$result" ] || fail "returned the symlinked mod-owned conf: '$result'"
}

@test "mod host: discovery falls past a mod-owned conf to a plain one" {
    # Same shape as above, but a legitimate conf also exists: skipping the
    # mod-owned candidate must not lose the real one.
    SANDBOX="$BATS_TEST_TMPDIR/mixedhost"
    mkdir -p "$SANDBOX/config/mod" "$SANDBOX/plain/config" "$SANDBOX/real/config"
    touch "$SANDBOX/config/mod/moonraker.conf" "$SANDBOX/real/config/moonraker.conf"
    ln -s "$SANDBOX/config/mod/moonraker.conf" "$SANDBOX/plain/config/moonraker.conf"

    HOST_MOD_ROOT="$SANDBOX/config/mod"
    HOST_MOONRAKER_USER_CONF=""
    KLIPPER_HOME=""
    MOONRAKER_CONF_PATHS="$SANDBOX/plain/config/moonraker.conf $SANDBOX/real/config/moonraker.conf"

    result=$(find_moonraker_conf)
    [ "$result" = "$SANDBOX/real/config/moonraker.conf" ] || fail "returned '$result'"
}

@test "mod host: the user conf wins over a KLIPPER_HOME-derived conf" {
    # The dynamic probe runs before the static list; on a mod host even that
    # must not beat the user conf.
    SANDBOX="$BATS_TEST_TMPDIR/dynamichost"
    mkdir -p "$SANDBOX/config/mod" "$SANDBOX/config/mod_data" "$SANDBOX/home/root/printer_data/config"
    touch "$SANDBOX/config/mod/moonraker.conf" "$SANDBOX/home/root/printer_data/config/moonraker.conf"

    HOST_MOD_ROOT="$SANDBOX/config/mod"
    HOST_MOONRAKER_USER_CONF="$SANDBOX/config/mod_data/user.moonraker.conf"
    KLIPPER_HOME="$SANDBOX/home/root"
    MOONRAKER_CONF_PATHS="$SANDBOX/config/mod/moonraker.conf"

    result=$(find_moonraker_conf)
    [ "$result" = "$HOST_MOONRAKER_USER_CONF" ] || fail "returned '$result'"
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

@test "generate_update_manager_config does NOT have persistent_files" {
    INSTALL_DIR="/opt/helixscreen"
    local config
    config=$(generate_update_manager_config)
    refute grep -q "persistent_files:" <<<"$config"
    refute grep -q "config/settings.json" <<<"$config"
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

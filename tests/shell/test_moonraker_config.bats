#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for moonraker configuration mutations (moonraker.sh)
# Covers add/migrate/write/configure/restart/remove for update_manager sections.
# (Path detection tests are in test_moonraker_paths.bats)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_MOONRAKER_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    # Set required globals
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export KLIPPER_HOME=""
    export PLATFORM=""

    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"

    # macOS sed workaround: GNU-style 'sed -i EXPR FILE' hangs on macOS because
    # it interprets EXPR as backup suffix and reads stdin. Wrap sed to detect
    # this pattern and fail fast, so the '||' fallback to BSD-style works.
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
}

# Helper: create a moonraker.conf with basic content
create_moonraker_conf() {
    local conf="$1"
    mkdir -p "$(dirname "$conf")"
    cat > "$conf" << 'CONF'
[server]
host: 0.0.0.0
port: 7125

[authorization]
trusted_clients:
    127.0.0.1

[update_manager mainsail]
type: web
channel: stable
repo: mainsail-crew/mainsail
path: ~/mainsail
CONF
}

# Helper: create a moonraker.conf with existing helixscreen web section
create_moonraker_conf_with_helix() {
    local conf="$1"
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

# HelixScreen Update Manager
# Added by HelixScreen installer - enables one-click updates from Mainsail/Fluidd
[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
persistent_files:
    config/helixconfig.json
    config/.disabled_services
CONF
}

# Helper: create a moonraker.conf with old git_repo section
create_moonraker_conf_with_git_repo() {
    local conf="$1"
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: git_repo
channel: stable
path: ~/helixscreen-repo
origin: https://github.com/prestonbrown/helixscreen.git
primary_branch: main
managed_services: helixscreen
install_script: scripts/install.sh
CONF
}

# Helper: point find_moonraker_conf at our test directory
setup_moonraker_home() {
    local conf_dir="$BATS_TEST_TMPDIR/home/testuser/printer_data/config"
    mkdir -p "$conf_dir"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/testuser"
    export KLIPPER_HOME
    echo "$conf_dir/moonraker.conf"
}

# =============================================================================
# add_update_manager_section
# =============================================================================

@test "add_update_manager_section: appends section to existing conf" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    add_update_manager_section "$conf"

    grep -q '^\[update_manager helixscreen\]' "$conf"
}

@test "add_update_manager_section: creates .bak.helixscreen backup" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    add_update_manager_section "$conf"

    [ -f "${conf}.bak.helixscreen" ]
}

@test "add_update_manager_section: appended section has type: web" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    add_update_manager_section "$conf"

    # Extract the helixscreen section and check type
    awk '/^\[update_manager helixscreen\]/{found=1} found' "$conf" | grep -q "type: web"
}

@test "add_update_manager_section: preserves existing content" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    add_update_manager_section "$conf"

    # Original sections still present
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
}

# =============================================================================
# migrate_to_web_type
# =============================================================================

@test "migrate_to_web_type: removes old section and adds web section" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_git_repo "$conf"
    # Override MOONRAKER_CONF_PATHS so find_moonraker_conf finds our test file
    MOONRAKER_CONF_PATHS="$conf"

    migrate_to_web_type "$conf"

    # Old git_repo type should be gone
    ! awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'git_repo'
    # New web section should exist
    grep -q '^\[update_manager helixscreen\]' "$conf"
    awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'web'
}

@test "migrate_to_web_type: cleans up old -repo directory" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_git_repo "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    # Create old repo directory
    mkdir -p "${INSTALL_DIR}-repo/.git"

    migrate_to_web_type "$conf"

    [ ! -d "${INSTALL_DIR}-repo" ]
}

@test "migrate_to_web_type: no old repo dir does not error" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_git_repo "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    # Ensure no -repo dir exists
    rm -rf "${INSTALL_DIR}-repo"

    # Don't use `run` here — sh -c inside remove_update_manager_section
    # causes fd issues with bats run subshell on macOS
    migrate_to_web_type "$conf"
}

# =============================================================================
# write_release_info
# =============================================================================

@test "write_release_info: already exists is a no-op" {
    echo '{"version":"v1.0.0"}' > "$INSTALL_DIR/release_info.json"

    run write_release_info
    [ "$status" -eq 0 ]
    # Content unchanged
    grep -q 'v1.0.0' "$INSTALL_DIR/release_info.json"
}

@test "write_release_info: binary not found returns 0" {
    rm -f "$INSTALL_DIR/release_info.json"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    run write_release_info
    [ "$status" -eq 0 ]
    # release_info.json should NOT have been created (no version detected)
    [ ! -f "$INSTALL_DIR/release_info.json" ]
}

@test "write_release_info: creates json when binary reports version" {
    rm -f "$INSTALL_DIR/release_info.json"
    # Create a fake helix-screen binary that reports a version
    cat > "$INSTALL_DIR/bin/helix-screen" << 'BINEOF'
#!/bin/sh
echo "helix-screen v1.2.3-rc1"
BINEOF
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    PLATFORM="pi"

    write_release_info

    [ -f "$INSTALL_DIR/release_info.json" ]
    grep -q '"v1.2.3-rc1"' "$INSTALL_DIR/release_info.json"
    grep -q '"helixscreen-pi.zip"' "$INSTALL_DIR/release_info.json"
}

@test "write_release_info: install dir missing does not crash" {
    rm -rf "$INSTALL_DIR"

    run write_release_info
    [ "$status" -eq 0 ]
}

# =============================================================================
# configure_moonraker_updates
# =============================================================================

@test "configure_moonraker_updates: ad5m platform skips entirely" {
    run configure_moonraker_updates "ad5m"
    [ "$status" -eq 0 ]
}

@test "configure_moonraker_updates: no moonraker.conf found warns but no crash" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nonexistent"
    MOONRAKER_CONF_PATHS="/nonexistent/moonraker.conf"
    # Ensure no binary so write_release_info is a no-op
    rm -f "$INSTALL_DIR/bin/helix-screen"

    run configure_moonraker_updates "pi"
    [ "$status" -eq 0 ]
}

@test "configure_moonraker_updates: section already exists is a no-op" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    local original_content
    original_content=$(cat "$conf")

    configure_moonraker_updates "pi"

    # Content should be unchanged
    [ "$(cat "$conf")" = "$original_content" ]
}

@test "configure_moonraker_updates: old git_repo section triggers migration" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_git_repo "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    # Mock systemctl so restart_moonraker doesn't fail
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi"

    # Should now have web type, not git_repo
    awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf" | grep -q 'web'
}

# =============================================================================
# restart_moonraker
# =============================================================================

@test "restart_moonraker: systemctl available and moonraker active restarts" {
    local restart_called="$BATS_TEST_TMPDIR/restart_called"
    mock_command_script "systemctl" '
        case "$*" in
            *is-active*moonraker*) exit 0 ;;
            *restart*moonraker*) touch "'"$restart_called"'"; exit 0 ;;
            *) exit 0 ;;
        esac
    '

    restart_moonraker

    [ -f "$restart_called" ]
}

@test "restart_moonraker: no systemctl, SysV init script exists uses init script" {
    # Make systemctl fail on is-active so it falls to the elif for SysV
    mock_command_script "systemctl" '
        case "$*" in
            *is-active*) exit 1 ;;
            *) exit 1 ;;
        esac
    '

    local init_script="$BATS_TEST_TMPDIR/etc/init.d/S56moonraker_service"
    local restart_called="$BATS_TEST_TMPDIR/moonraker_sysv_restart"
    mkdir -p "$(dirname "$init_script")"
    cat > "$init_script" << MOONEOF
#!/bin/sh
case "\$1" in
    restart) touch "$restart_called" ;;
esac
MOONEOF
    chmod +x "$init_script"

    # The function checks /etc/init.d/S56moonraker_service which is a fixed path.
    # We can only test this if we can write to /etc/init.d.
    if [ ! -d "/etc/init.d" ] || [ ! -w "/etc/init.d" ]; then
        skip "Cannot create /etc/init.d/S56moonraker_service (hardcoded path, need writable /etc/init.d)"
    fi

    restart_moonraker
    # If we got here, check the call was made
    [ -f "$restart_called" ]
}

# =============================================================================
# remove_update_manager_section (edge cases)
# =============================================================================

@test "remove_update_manager_section: conf does not exist returns 0" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nonexistent"
    MOONRAKER_CONF_PATHS="/nonexistent/moonraker.conf"

    run remove_update_manager_section
    [ "$status" -eq 0 ]
}

@test "remove_update_manager_section: section at end of file is cleanly removed" {
    local conf
    conf=$(setup_moonraker_home)
    # Create conf with helixscreen section at the very end (no following section)
    create_moonraker_conf_with_helix "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    remove_update_manager_section

    # helixscreen section should be gone
    ! grep -q '^\[update_manager helixscreen\]' "$conf"
    # Other sections still present
    grep -q '^\[server\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
}

@test "remove_update_manager_section: only helixscreen removed, others preserved" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    # Add helixscreen section between other sections
    cat >> "$conf" << 'CONF'

# HelixScreen Update Manager
# Added by HelixScreen installer - enables one-click updates from Mainsail/Fluidd
[update_manager helixscreen]
type: zip
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
managed_services: helixscreen

[update_manager klipper]
type: git_repo
channel: dev
path: ~/klipper
CONF
    MOONRAKER_CONF_PATHS="$conf"

    remove_update_manager_section

    # helixscreen section should be gone
    ! grep -q '^\[update_manager helixscreen\]' "$conf"
    # All other sections intact
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
    grep -q '^\[update_manager klipper\]' "$conf"
    # Comment lines also removed
    ! grep -q '# HelixScreen Update Manager' "$conf"
    ! grep -q '# Added by HelixScreen installer' "$conf"
}

# =============================================================================
# ensure_moonraker_asvc
# =============================================================================

@test "ensure_moonraker_asvc: adds helixscreen to existing asvc file" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    # Create moonraker.asvc in printer_data (two levels up from config/moonraker.conf)
    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"

    ensure_moonraker_asvc "$conf"

    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
}

@test "ensure_moonraker_asvc: skips if helixscreen already present" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\nhelixscreen\n" > "$printer_data/moonraker.asvc"

    local before
    before=$(cat "$printer_data/moonraker.asvc")

    ensure_moonraker_asvc "$conf"

    # Content should be unchanged — no duplicate entry
    [ "$(cat "$printer_data/moonraker.asvc")" = "$before" ]
}

@test "ensure_moonraker_asvc: no asvc file returns 0" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    rm -f "$printer_data/moonraker.asvc"

    run ensure_moonraker_asvc "$conf"
    [ "$status" -eq 0 ]
}

@test "ensure_moonraker_asvc: does not match partial names" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\nhelixscreen-old\n" > "$printer_data/moonraker.asvc"

    ensure_moonraker_asvc "$conf"

    # Should have added helixscreen (helixscreen-old is not an exact match)
    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
    # Original entry still there
    grep -q '^helixscreen-old$' "$printer_data/moonraker.asvc"
}

# =============================================================================
# configure_moonraker_updates + asvc integration
# =============================================================================

@test "configure_moonraker_updates: adds helixscreen to asvc when adding section" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"

    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi"

    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
}

@test "configure_moonraker_updates: adds to asvc even when section already exists" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"

    configure_moonraker_updates "pi"

    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
}

# =============================================================================
# ensure_persistent_files
# =============================================================================

@test "ensure_persistent_files: adds persistent_files to section without it" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Add helixscreen section WITHOUT persistent_files (simulates old install)
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen

[update_manager klipper]
type: git_repo
CONF

    ensure_persistent_files "$conf"

    # persistent_files should now be present
    awk '/^\[update_manager helixscreen\]/{found=1} found && /^\[update_manager klipper\]/{exit} found' "$conf" | grep -q 'persistent_files:'
    awk '/^\[update_manager helixscreen\]/{found=1} found && /^\[update_manager klipper\]/{exit} found' "$conf" | grep -q 'config/helixconfig.json'
    awk '/^\[update_manager helixscreen\]/{found=1} found && /^\[update_manager klipper\]/{exit} found' "$conf" | grep -q 'config/helixscreen.env'
}

@test "ensure_persistent_files: no-op when persistent_files already present" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"

    local before
    before=$(cat "$conf")

    ensure_persistent_files "$conf"

    # Content should be unchanged
    [ "$(cat "$conf")" = "$before" ]
}

@test "ensure_persistent_files: preserves other sections" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen

[update_manager klipper]
type: git_repo
channel: dev
path: ~/klipper
CONF

    ensure_persistent_files "$conf"

    # Other sections preserved
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
    grep -q '^\[update_manager klipper\]' "$conf"
    grep -q 'path: ~/klipper' "$conf"
}

@test "ensure_persistent_files: inserted after path: line" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen
CONF

    ensure_persistent_files "$conf"

    # persistent_files should appear after path: line
    local path_line persistent_line
    path_line=$(grep -n '^path: /usr/data/helixscreen' "$conf" | head -1 | cut -d: -f1)
    persistent_line=$(grep -n '^persistent_files:' "$conf" | head -1 | cut -d: -f1)
    [ "$persistent_line" -gt "$path_line" ]
}

@test "configure_moonraker_updates: existing section without persistent_files gets it added" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Add section WITHOUT persistent_files (old install scenario)
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen
CONF
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    configure_moonraker_updates "k1"

    # persistent_files should have been added
    awk '/^\[update_manager helixscreen\]/{found=1} found' "$conf" | grep -q 'persistent_files:'
    awk '/^\[update_manager helixscreen\]/{found=1} found' "$conf" | grep -q 'config/helixconfig.json'
}

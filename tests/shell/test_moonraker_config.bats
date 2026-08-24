#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for moonraker configuration mutations (moonraker.sh)
# Covers add/migrate/write/configure/restart/remove for update_manager sections.
# (Path detection tests are in test_moonraker_paths.bats)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load).
    # platform.sh provides helix_self_update_asset(), which write_release_info()
    # calls to resolve the Moonraker self-update asset name.
    unset _HELIX_COMMON_SOURCED _HELIX_MOONRAKER_SOURCED _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    # Set required globals
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export KLIPPER_HOME=""
    export PLATFORM=""

    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"

    # No Moonraker source tree by default, so moonraker_asset_name_support()
    # reports "undetermined" and configure_moonraker_updates() keeps its
    # pre-gate behaviour. Tests that exercise the gate set this explicitly via
    # fake_moonraker_src(). Without the override the probe would find whatever
    # Moonraker happens to be installed on the machine running the suite.
    export MOONRAKER_SRC_PATHS=""

    # moonraker.sh edits conf files with GNU-style `sed -i EXPR FILE`, which BSD
    # sed misreads as a backup suffix. The shim makes the GNU form work here, so
    # macOS exercises the same branch the Linux devices take.
    install_gnu_sed_shim
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
    config/settings.json
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

# Helper: build a fake Moonraker source tree and point MOONRAKER_SRC_PATHS at it.
# Args: $1 = update_manager module filename (net_deploy.py / zip_deploy.py / ...)
#       $2 = module body (optional; include "asset_name" to signal support)
#       $3 = layout: "checkout" (default, <root>/moonraker/components/...),
#            "package" (<root>/components/...), or
#            "nested"  (<root>/moonraker/moonraker/components/...)
# Echoes the source root.
fake_moonraker_src() {
    local module="$1"
    local body="${2:-}"
    local layout="${3:-checkout}"
    local root="$BATS_TEST_TMPDIR/moonraker-src"
    local um

    if [ "$layout" = "package" ]; then
        um="$root/components/update_manager"
    elif [ "$layout" = "nested" ]; then
        um="$root/moonraker/moonraker/components/update_manager"
    else
        um="$root/moonraker/components/update_manager"
    fi

    mkdir -p "$um"
    # Every real layout has these siblings; include them so a probe that keys
    # off "some file exists" rather than the specific module would be caught.
    : > "$um/__init__.py"
    : > "$um/update_manager.py"
    : > "$um/app_deploy.py"
    : > "$um/git_deploy.py"
    printf '%s\n' "$body" > "$um/$module"

    MOONRAKER_SRC_PATHS="$root"
    export MOONRAKER_SRC_PATHS
    echo "$root"
}

# Negative assertions here use refute_grep/refute from tests/shell/helpers.bash,
# never inline `! grep -q …`: the `!` reserved word suppresses errexit, so a
# mid-test negative assertion is silently swallowed and only the LAST command
# decides the result.

# Body of a modern net_deploy.py — the decisive token is asset_name.
NET_DEPLOY_MODERN='class NetDeploy(AppDeploy):
    async def _get_remote_version(self):
        asset_name = self.release_info.get("asset_name")
        release_asset = assets[0]'

# Body of a net_deploy.py from a fork that stripped asset_name handling.
NET_DEPLOY_NO_ASSET='class NetDeploy(AppDeploy):
    async def _get_remote_version(self):
        release_asset = result.get("assets", [{}])[0]'

# =============================================================================
# find_moonraker_update_manager_dir / moonraker_asset_name_support
# =============================================================================

@test "find_moonraker_update_manager_dir: finds Creality's nested repo layout" {
    # Measured on a K1C (192.168.30.182, Moonraker v0.10.0-10): the install dir
    # is /usr/data/moonraker, the git repo is cloned to
    # /usr/data/moonraker/moonraker, and the python package sits one level below
    # that, so update_manager lives at
    #   /usr/data/moonraker/moonraker/moonraker/components/update_manager
    # Neither the plain-checkout nor the package form reaches it. Before this
    # layout was covered the probe returned "undetermined" on every K1/K2 --
    # the platforms the gate exists to protect.
    # Discard the helper's stdout rather than capturing it: command
    # substitution would run it in a subshell and its `export
    # MOONRAKER_SRC_PATHS` would never reach us. The root is deterministic.
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_MODERN" "nested" >/dev/null
    local root="$BATS_TEST_TMPDIR/moonraker-src"

    run find_moonraker_update_manager_dir
    [ "$status" -eq 0 ]
    [ "$output" = "$root/moonraker/moonraker/components/update_manager" ]
}

@test "moonraker_asset_name_support: Creality nested layout resolves, not undetermined" {
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_MODERN" "nested" >/dev/null

    run moonraker_asset_name_support
    [ "$status" -eq 0 ]
    [ "$output" = "supported" ]
}

@test "moonraker_asset_name_support: nested layout without asset_name is unsupported" {
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_NO_ASSET" "nested" >/dev/null

    run moonraker_asset_name_support
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "moonraker_asset_name_support: net_deploy.py containing asset_name is supported" {
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_MODERN" >/dev/null

    run moonraker_asset_name_support
    [ "$status" -eq 0 ]
    [ "$output" = "supported" ]
}

@test "moonraker_asset_name_support: net_deploy.py without asset_name is unsupported" {
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_NO_ASSET" >/dev/null

    run moonraker_asset_name_support
    [ "$output" = "unsupported" ]
}

@test "moonraker_asset_name_support: zip_deploy.py only is unsupported" {
    # Pre-530f1c2016 Moonraker (Snapmaker U1 ships exactly this today).
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null

    run moonraker_asset_name_support
    [ "$output" = "unsupported" ]
}

@test "moonraker_asset_name_support: web_deploy.py only is unsupported" {
    fake_moonraker_src "web_deploy.py" "class WebClientDeploy(BaseDeploy): pass" >/dev/null

    run moonraker_asset_name_support
    [ "$output" = "unsupported" ]
}

@test "moonraker_asset_name_support: no Moonraker source found is undetermined" {
    MOONRAKER_SRC_PATHS="/nonexistent/moonraker"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nonexistent"

    run moonraker_asset_name_support
    [ "$output" = "undetermined" ]
}

@test "moonraker_asset_name_support: unknown module set is undetermined, not a guess" {
    # update_manager package present but neither net_/zip_/web_deploy.py.
    fake_moonraker_src "future_deploy.py" "asset_name = 1" >/dev/null

    run moonraker_asset_name_support
    [ "$output" = "undetermined" ]
}

@test "moonraker_asset_name_support: package layout (<root>/components) is probed too" {
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_MODERN" "package" >/dev/null

    run moonraker_asset_name_support
    [ "$output" = "supported" ]
}

@test "find_moonraker_update_manager_dir: KLIPPER_HOME wins over the static list" {
    # Static list points at an old zip_deploy tree...
    fake_moonraker_src "zip_deploy.py" "old" >/dev/null

    # ...but the detected Klipper user's home has a modern one.
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/klipperuser"
    local um="$KLIPPER_HOME/moonraker/moonraker/components/update_manager"
    mkdir -p "$um"
    printf '%s\n' "$NET_DEPLOY_MODERN" > "$um/net_deploy.py"

    run find_moonraker_update_manager_dir
    [ "$output" = "$um" ]

    run moonraker_asset_name_support
    [ "$output" = "supported" ]
}

@test "find_moonraker_update_manager_dir: nothing found echoes empty string" {
    MOONRAKER_SRC_PATHS="/nonexistent/moonraker"
    KLIPPER_HOME="$BATS_TEST_TMPDIR/nonexistent"

    run find_moonraker_update_manager_dir
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# =============================================================================
# configure_moonraker_updates: asset_name capability gate (#993)
# =============================================================================

@test "configure_moonraker_updates: supported Moonraker gets the stanza" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_MODERN" >/dev/null
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi"

    grep -q '^\[update_manager helixscreen\]' "$conf"
}

@test "configure_moonraker_updates: unsupported Moonraker does NOT get the stanza" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    mock_command_script "systemctl" 'exit 0'

    # Capture stderr to a file rather than using `run` — remove_update_manager_section
    # shells out via `sh -c`, which breaks bats' run subshell on macOS.
    configure_moonraker_updates "pi" 2>"$BATS_TEST_TMPDIR/gate.log"

    refute_grep '^\[update_manager helixscreen\]' "$conf"
    # And the user is told why, plus what to do instead.
    grep -q 'predates release_info.json asset_name support' "$BATS_TEST_TMPDIR/gate.log"
    grep -q 'v0.10.0' "$BATS_TEST_TMPDIR/gate.log"
    grep -q 'Settings -> Updates' "$BATS_TEST_TMPDIR/gate.log"
    # Unrelated sections are left alone.
    grep -q '^\[update_manager mainsail\]' "$conf"
}

@test "configure_moonraker_updates: net_deploy.py without asset_name is also gated" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "net_deploy.py" "$NET_DEPLOY_NO_ASSET" >/dev/null
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi" 2>"$BATS_TEST_TMPDIR/gate.log"

    refute_grep '^\[update_manager helixscreen\]' "$conf"
}

@test "configure_moonraker_updates: undetermined still writes the stanza, with a warning" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    MOONRAKER_SRC_PATHS="/nonexistent/moonraker"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi" 2>"$BATS_TEST_TMPDIR/gate.log"

    # Don't regress installs we can't reason about.
    grep -q '^\[update_manager helixscreen\]' "$conf"
    grep -q 'Could not locate the Moonraker source' "$BATS_TEST_TMPDIR/gate.log"
}

@test "configure_moonraker_updates: unsupported removes a pre-existing stanza" {
    local conf
    conf=$(setup_moonraker_home)
    # Previous install (before this gate existed) already armed the updater.
    create_moonraker_conf_with_helix "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    mock_command_script "systemctl" 'exit 0'

    grep -q '^\[update_manager helixscreen\]' "$conf"

    configure_moonraker_updates "pi" 2>"$BATS_TEST_TMPDIR/gate.log"

    # Gun unloaded.
    refute_grep '^\[update_manager helixscreen\]' "$conf"
    refute_grep 'repo: prestonbrown/helixscreen' "$conf"
    grep -q 'Removing the existing' "$BATS_TEST_TMPDIR/gate.log"
    # Everything else survives.
    grep -q '^\[server\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
}

@test "configure_moonraker_updates: unsupported does not migrate an old git_repo stanza" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_git_repo "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi" 2>"$BATS_TEST_TMPDIR/gate.log"

    # The gate runs before the migration branch — no type: web section appears.
    refute_grep '^\[update_manager helixscreen\]' "$conf"
}

@test "configure_moonraker_updates: unsupported leaves a conf with no stanza untouched" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    fake_non_buildroot_os_release
    # system_updates_unavailable() is os-release OR no-package-manager, so the
    # package-manager probe has to be pinned too — otherwise a host without apt
    # (macOS, a slim CI container) takes the second term and edits the conf.
    fake_os_package_manager_present
    mock_command_script "systemctl" 'exit 0'

    local before
    before=$(cat "$conf")

    configure_moonraker_updates "pi" 2>/dev/null

    [ "$(cat "$conf")" = "$before" ]
}

@test "configure_moonraker_updates: unsupported still writes the asvc allowlist entry" {
    # The service allowlist is orthogonal to the updater — skipping the stanza
    # must not cost the user the ability to restart HelixScreen from Mainsail.
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    fake_non_buildroot_os_release
    mock_command_script "systemctl" 'exit 0'

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"

    configure_moonraker_updates "pi" 2>/dev/null

    grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
    refute_grep '^\[update_manager helixscreen\]' "$conf"
}

@test "configure_moonraker_updates: unsupported on buildroot still disables system updates" {
    # The System Update Provider warning is triggered by the mainsail/fluidd
    # update_manager sections, not ours — suppress it either way.
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_moonraker_src "zip_deploy.py" "class ZipDeploy(AppDeploy): pass" >/dev/null
    fake_buildroot_os_release
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "snapmaker-u1" 2>/dev/null

    refute_grep '^\[update_manager helixscreen\]' "$conf"
    grep -q '^enable_system_updates: False' "$conf"
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
    local hs_type
    hs_type=$(awk '/^\[update_manager helixscreen\]/{found=1; next} found && /^\[/{exit} found && /^type:/{print; exit}' "$conf")
    refute grep -q 'git_repo' <<<"$hs_type"
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

@test "configure_moonraker_updates: section already exists removes persistent_files" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"

    # Fixture has persistent_files — ensure_persistent_files should remove them
    grep -q 'persistent_files:' "$conf"

    configure_moonraker_updates "pi"

    # persistent_files should have been removed (config now lives outside managed path)
    refute grep -q 'persistent_files:' "$conf"
    refute grep -q 'config/settings.json' "$conf"
    # Section header still present
    grep -q '^\[update_manager helixscreen\]' "$conf"
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

# Helper: drop a fake Moonraker init script named $1 into an overridable
# /etc/init.d and point HELIX_INITD_DIR at it. Sets STAGED_MOONRAKER_MARKER to
# the path the script touches when asked to restart.
#
# Call this directly, NOT via $(...) — a command substitution runs it in a
# subshell, so the export would be discarded and the test would silently
# exercise the real /etc/init.d instead.
stage_sysv_moonraker() {
    local name="$1"
    local initd="$BATS_TEST_TMPDIR/etc/init.d"

    STAGED_MOONRAKER_MARKER="$BATS_TEST_TMPDIR/moonraker_restarted_${name}"

    mkdir -p "$initd"
    cat > "${initd}/${name}" << MOONEOF
#!/bin/sh
case "\$1" in
    restart) touch "$STAGED_MOONRAKER_MARKER" ;;
esac
MOONEOF
    chmod +x "${initd}/${name}"
    export HELIX_INITD_DIR="$initd"
}

@test "restart_moonraker: no systemctl, SysV S56 script is used (K1/Simple AF)" {
    mock_command_script "systemctl" 'exit 1'
    stage_sysv_moonraker "S56moonraker_service"

    restart_moonraker

    [ -f "$STAGED_MOONRAKER_MARKER" ]
}

@test "restart_moonraker: no systemctl, plain init.d/moonraker is used (CC1/COSMOS)" {
    # The CC1 has no systemctl and no S56moonraker_service — its script is
    # /etc/init.d/moonraker. Before this was handled, restart_moonraker fell off
    # the end silently, so the installer's moonraker.conf edit did not take
    # effect until the user happened to reboot.
    mock_command_script "systemctl" 'exit 1'
    stage_sysv_moonraker "moonraker"

    restart_moonraker

    [ -f "$STAGED_MOONRAKER_MARKER" ]
}

@test "restart_moonraker: warns and succeeds when no restart mechanism exists" {
    mock_command_script "systemctl" 'exit 1'
    export HELIX_INITD_DIR="$BATS_TEST_TMPDIR/etc/init.d-empty"
    mkdir -p "$HELIX_INITD_DIR"

    run restart_moonraker

    # Must not abort the install, and must tell the user to restart by hand
    [ "$status" -eq 0 ]
    [[ "$output" == *"manual"* || "$output" == *"manually"* ]]
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
    refute grep -q '^\[update_manager helixscreen\]' "$conf"
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
    refute grep -q '^\[update_manager helixscreen\]' "$conf"
    # All other sections intact
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
    grep -q '^\[update_manager klipper\]' "$conf"
    # Comment lines also removed
    refute grep -q '# HelixScreen Update Manager' "$conf"
    ! grep -q '# Added by HelixScreen installer' "$conf"
}

# The removal used to delete two comment lines by literal pattern while the
# generator wrote five, so every install/uninstall cycle orphaned the other
# three. Verified on a CC1: after install, uninstall, install, the
# mainsail#2444 line appeared twice in moonraker.conf.
@test "remove_update_manager_section: removes the whole generated comment block" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    add_update_manager_section "$conf"
    remove_update_manager_section

    # Every line the generator emits must be gone, not just the two that
    # were named explicitly.
    refute grep -q 'HelixScreen Update Manager' "$conf"
    refute grep -q 'Added by HelixScreen installer' "$conf"
    refute grep -q 'type: web is used instead of type: zip' "$conf"
    refute grep -q 'mainsail#2444' "$conf"
    refute grep -q 'A systemd path unit handles service restart' "$conf"
}

# The real-world symptom: residue accumulates across cycles.
@test "remove_update_manager_section: add/remove round-trip restores the file exactly" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    cp "$conf" "$BATS_TEST_TMPDIR/original.conf"

    # Three full cycles: any per-cycle residue compounds and shows up here.
    add_update_manager_section "$conf"
    remove_update_manager_section
    add_update_manager_section "$conf"
    remove_update_manager_section
    add_update_manager_section "$conf"
    remove_update_manager_section

    diff -u "$BATS_TEST_TMPDIR/original.conf" "$conf"
}

@test "remove_update_manager_section: leaves an unrelated preceding comment alone" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"

    printf '\n# operator note: do not delete this\n' >> "$conf"
    add_update_manager_section "$conf"
    remove_update_manager_section

    grep -q '# operator note: do not delete this' "$conf"
    refute grep -q '^\[update_manager helixscreen\]' "$conf"
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
# remove_moonraker_asvc
#
# ensure_moonraker_asvc had no removal counterpart, so uninstall left
# helixscreen in the allowlist forever (observed on a CC1 running COSMOS).
# =============================================================================

@test "remove_moonraker_asvc: removes the helixscreen entry" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\nhelixscreen\n" > "$printer_data/moonraker.asvc"

    remove_moonraker_asvc "$conf"

    refute grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
    # Everything else survives
    grep -q '^klipper$' "$printer_data/moonraker.asvc"
    grep -q '^moonraker$' "$printer_data/moonraker.asvc"
}

@test "remove_moonraker_asvc: add/remove round-trip restores the file exactly" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\nKlipperScreen\n" > "$printer_data/moonraker.asvc"
    cp "$printer_data/moonraker.asvc" "$BATS_TEST_TMPDIR/original.asvc"

    ensure_moonraker_asvc "$conf"
    remove_moonraker_asvc "$conf"

    diff -u "$BATS_TEST_TMPDIR/original.asvc" "$printer_data/moonraker.asvc"
}

@test "remove_moonraker_asvc: does not touch partial-name entries" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nhelixscreen-old\nhelixscreen\n" > "$printer_data/moonraker.asvc"

    remove_moonraker_asvc "$conf"

    refute grep -q '^helixscreen$' "$printer_data/moonraker.asvc"
    grep -q '^helixscreen-old$' "$printer_data/moonraker.asvc"
}

@test "remove_moonraker_asvc: no asvc file returns 0" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    run remove_moonraker_asvc "$conf"
    [ "$status" -eq 0 ]
}

@test "remove_moonraker_asvc: entry absent is a no-op returning 0" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"

    local printer_data
    printer_data="$(dirname "$(dirname "$conf")")"
    printf "klipper\nmoonraker\n" > "$printer_data/moonraker.asvc"
    cp "$printer_data/moonraker.asvc" "$BATS_TEST_TMPDIR/original.asvc"

    run remove_moonraker_asvc "$conf"
    [ "$status" -eq 0 ]
    diff -u "$BATS_TEST_TMPDIR/original.asvc" "$printer_data/moonraker.asvc"
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
# cleanup_unsupported_options
# =============================================================================

@test "cleanup_unsupported_options: removes persistent_files from section that has it" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Add helixscreen section WITH persistent_files (simulates old install)
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
persistent_files:
    config/settings.json
    config/helixscreen.env

[update_manager klipper]
type: git_repo
CONF

    cleanup_unsupported_options "$conf"

    # persistent_files should have been removed
    local hs_section
    hs_section=$(awk '/^\[update_manager helixscreen\]/{found=1} found && /^\[update_manager klipper\]/{exit} found' "$conf")
    refute grep -q 'persistent_files:' <<<"$hs_section"
    refute grep -q 'config/settings.json' <<<"$hs_section"
    refute grep -q 'config/helixscreen.env' <<<"$hs_section"
}

@test "cleanup_unsupported_options: no-op when persistent_files already absent" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Add helixscreen section WITHOUT persistent_files (already clean)
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /opt/helixscreen
CONF

    local before
    before=$(cat "$conf")

    cleanup_unsupported_options "$conf"

    # Content should be unchanged — nothing to remove
    [ "$(cat "$conf")" = "$before" ]
}

@test "cleanup_unsupported_options: preserves other sections when removing" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen
persistent_files:
    config/settings.json
    config/.disabled_services

[update_manager klipper]
type: git_repo
channel: dev
path: ~/klipper
CONF

    cleanup_unsupported_options "$conf"

    # persistent_files removed
    refute grep -q 'persistent_files:' "$conf"
    # Other sections preserved
    grep -q '^\[server\]' "$conf"
    grep -q '^\[authorization\]' "$conf"
    grep -q '^\[update_manager mainsail\]' "$conf"
    grep -q '^\[update_manager klipper\]' "$conf"
    grep -q 'path: ~/klipper' "$conf"
}

@test "cleanup_unsupported_options: removes persistent_files between other config lines" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen
persistent_files:
    config/settings.json
    config/.disabled_services
CONF

    cleanup_unsupported_options "$conf"

    # persistent_files and its continuation lines should be gone
    refute grep -q 'persistent_files:' "$conf"
    refute grep -q 'config/settings.json' "$conf"
    # path: line should still be present
    grep -q 'path: /usr/data/helixscreen' "$conf"
    # type: line should still be present
    grep -q 'type: web' "$conf"
}

# =============================================================================
# disable_system_updates_on_buildroot
# =============================================================================

# Helper: make is_buildroot_distro() return true by pointing OS_RELEASE_FILE
# at a fixture os-release that contains "buildroot".
fake_buildroot_os_release() {
    local osr="$BATS_TEST_TMPDIR/etc/os-release"
    mkdir -p "$(dirname "$osr")"
    cat > "$osr" << 'OSR'
NAME=Buildroot
VERSION=2021.02
ID=buildroot
OSR
    export OS_RELEASE_FILE="$osr"
}

# Helper: make is_buildroot_distro() return false (non-buildroot distro).
fake_non_buildroot_os_release() {
    local osr="$BATS_TEST_TMPDIR/etc/os-release"
    mkdir -p "$(dirname "$osr")"
    cat > "$osr" << 'OSR'
NAME="Debian GNU/Linux"
VERSION="12 (bookworm)"
ID=debian
OSR
    export OS_RELEASE_FILE="$osr"
}

# Helper: os-release with an arbitrary ID that is neither buildroot nor debian.
# COSMOS (CC1) has no os-release at all; the K2 Plus reports ID="openwrt".
fake_os_release_with_id() {
    local osr="$BATS_TEST_TMPDIR/etc/os-release"
    mkdir -p "$(dirname "$osr")"
    printf 'ID="%s"\n' "$1" > "$osr"
    export OS_RELEASE_FILE="$osr"
}

# Helper: no /etc/os-release on disk at all. This is the real CC1/COSMOS
# (Yocto/poky) shape — the file is absent, so any grep-based distro probe
# fails on the [ -f ] test before it ever looks at the contents.
fake_absent_os_release() {
    export OS_RELEASE_FILE="$BATS_TEST_TMPDIR/etc/os-release-does-not-exist"
    rm -f "$OS_RELEASE_FILE"
}

# Helper: firmware with no OS package manager (CC1, K1, K2, U1 — none have apt).
fake_no_os_package_manager() {
    export OS_PACKAGE_MANAGER_CMDS="helix-no-such-package-manager"
}

# Helper: a distro that does have a working package manager (Pi/Debian/Armbian).
# Pinned explicitly rather than relying on the machine running the suite having
# apt — CI containers may not, and that would silently flip these tests.
fake_os_package_manager_present() {
    export OS_PACKAGE_MANAGER_CMDS="sh"
}

@test "disable_system_updates_on_buildroot: adds bare section + key on buildroot" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_buildroot_os_release

    disable_system_updates_on_buildroot "$conf"

    # A bare [update_manager] section now exists (exact match, no name)
    grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    # enable_system_updates: False is present under it
    awk '
        /^\[update_manager\][[:space:]]*$/ { in_section=1; next }
        in_section && /^\[/ { in_section=0 }
        in_section && /enable_system_updates:[[:space:]]*False/ { found=1 }
        END { exit (found ? 0 : 1) }
    ' "$conf"
    # The helixscreen section is still intact
    grep -q '^\[update_manager helixscreen\]' "$conf"
    # Backup was created
    [ -f "${conf}.bak.helixscreen" ]
}

@test "disable_system_updates_on_buildroot: idempotent — no duplicate section or key" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_buildroot_os_release

    disable_system_updates_on_buildroot "$conf"
    disable_system_updates_on_buildroot "$conf"

    # Exactly one bare [update_manager] section
    [ "$(grep -cE '^\[update_manager\][[:space:]]*$' "$conf")" -eq 1 ]
    # Exactly one enable_system_updates key
    [ "$(grep -cE '^[[:space:]]*enable_system_updates:' "$conf")" -eq 1 ]
}

@test "disable_system_updates_on_buildroot: merges key into existing bare section" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Pre-existing bare [update_manager] with another key (common on K1/K2)
    cat >> "$conf" << 'CONF'

[update_manager]
channel: dev
CONF
    fake_buildroot_os_release

    disable_system_updates_on_buildroot "$conf"

    # Still exactly one bare section (merged, not duplicated)
    [ "$(grep -cE '^\[update_manager\][[:space:]]*$' "$conf")" -eq 1 ]
    # The original key is preserved
    grep -q '^channel: dev' "$conf"
    # The new key was added under the bare section
    awk '
        /^\[update_manager\][[:space:]]*$/ { in_section=1; next }
        in_section && /^\[/ { in_section=0 }
        in_section && /enable_system_updates:[[:space:]]*False/ { found=1 }
        END { exit (found ? 0 : 1) }
    ' "$conf"
}

@test "disable_system_updates_on_buildroot: respects existing user value" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # User already set it to True — we must not override
    cat >> "$conf" << 'CONF'

[update_manager]
enable_system_updates: True
CONF
    fake_buildroot_os_release

    disable_system_updates_on_buildroot "$conf"

    # User's True value is untouched
    grep -q '^enable_system_updates: True' "$conf"
    refute grep -q '^enable_system_updates: False' "$conf"
    # No backup needed (early return, no edit)
    [ ! -f "${conf}.bak.helixscreen" ]
}

@test "disable_system_updates_on_buildroot: appends bare section when no helixscreen block" {
    local conf
    conf=$(setup_moonraker_home)
    # Plain conf, no helixscreen section at all
    create_moonraker_conf "$conf"
    fake_buildroot_os_release

    disable_system_updates_on_buildroot "$conf"

    grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    grep -q '^enable_system_updates: False' "$conf"
}

@test "disable_system_updates_on_buildroot: non-buildroot is a no-op" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_non_buildroot_os_release
    # Not buildroot is only half the predicate: system_updates_unavailable()
    # also fires when no package manager is on PATH, so pin that too rather
    # than inheriting whatever the host running the suite happens to have.
    fake_os_package_manager_present

    local before
    before=$(cat "$conf")

    disable_system_updates_on_buildroot "$conf"

    # Nothing added, nothing changed
    [ "$(cat "$conf")" = "$before" ]
    refute grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    refute grep -q 'enable_system_updates' "$conf"
    [ ! -f "${conf}.bak.helixscreen" ]
}

@test "disable_system_updates_on_buildroot: missing conf does not crash" {
    fake_buildroot_os_release
    run disable_system_updates_on_buildroot "/nonexistent/moonraker.conf"
    [ "$status" -eq 0 ]
}

# -----------------------------------------------------------------------------
# Firmware with no OS package manager, but not identifying as buildroot.
#
# Moonraker's PackageDeploy tries PackageKit over DBus, then falls back to the
# apt CLI and nothing else (system_deploy.py _get_fallback_provider: "Currently
# only the API Fallback provider is available"). With neither, it emits four
# permanent warnings into Mainsail/Fluidd: three "Unable to find DBus PolKit
# Interface" plus "Unable to initialize System Update Provider for
# distribution". Verified on a live CC1.
#
# The buildroot string check misses two shipped platforms:
#   CC1  (COSMOS/Yocto) — no /etc/os-release at all
#   K2+  (OpenWrt)      — ID="openwrt"
# -----------------------------------------------------------------------------

@test "disable_system_updates: fires when os-release is absent (CC1/COSMOS)" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_absent_os_release
    fake_no_os_package_manager

    disable_system_updates_on_buildroot "$conf"

    grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    grep -q '^enable_system_updates: False' "$conf"
    # Our one-click updater section must survive untouched
    grep -q '^\[update_manager helixscreen\]' "$conf"
}

@test "disable_system_updates: fires on OpenWrt with no package manager (K2 Plus)" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_os_release_with_id "openwrt"
    fake_no_os_package_manager

    disable_system_updates_on_buildroot "$conf"

    grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    grep -q '^enable_system_updates: False' "$conf"
}

@test "disable_system_updates: fires on Yocto/poky with no package manager" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_os_release_with_id "poky"
    fake_no_os_package_manager

    disable_system_updates_on_buildroot "$conf"

    grep -q '^enable_system_updates: False' "$conf"
}

@test "disable_system_updates: no-op on Debian where apt actually works" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_non_buildroot_os_release
    fake_os_package_manager_present

    local before
    before=$(cat "$conf")

    disable_system_updates_on_buildroot "$conf"

    # OS updates work on a Pi/Debian host — leave them enabled
    [ "$(cat "$conf")" = "$before" ]
    refute grep -q 'enable_system_updates' "$conf"
    [ ! -f "${conf}.bak.helixscreen" ]
}

@test "disable_system_updates: still fires on buildroot even if apt exists" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf_with_helix "$conf"
    fake_buildroot_os_release
    fake_os_package_manager_present

    disable_system_updates_on_buildroot "$conf"

    # Preserves the pre-existing buildroot behaviour regardless of the new probe
    grep -q '^enable_system_updates: False' "$conf"
}

@test "configure_moonraker_updates: buildroot adds enable_system_updates: False" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_buildroot_os_release
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "k1"

    # Our helixscreen section was added
    grep -q '^\[update_manager helixscreen\]' "$conf"
    # And the buildroot warning suppression key is present
    grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    grep -q '^enable_system_updates: False' "$conf"
}

@test "configure_moonraker_updates: non-buildroot does NOT add enable_system_updates" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    fake_non_buildroot_os_release
    # The no-package-manager term of system_updates_unavailable() would fire on
    # its own on a host without apt, so pin the probe as well as the os-release.
    fake_os_package_manager_present
    mock_command_script "systemctl" 'exit 0'

    configure_moonraker_updates "pi"

    grep -q '^\[update_manager helixscreen\]' "$conf"
    refute grep -qE '^\[update_manager\][[:space:]]*$' "$conf"
    ! grep -q 'enable_system_updates' "$conf"
}

@test "configure_moonraker_updates: existing section without persistent_files is a no-op" {
    local conf
    conf=$(setup_moonraker_home)
    create_moonraker_conf "$conf"
    # Add section WITHOUT persistent_files (already clean — nothing to remove)
    cat >> "$conf" << 'CONF'

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: /usr/data/helixscreen
CONF
    MOONRAKER_CONF_PATHS="$conf"
    rm -f "$INSTALL_DIR/bin/helix-screen"
    # "Unchanged" only holds where system_updates_unavailable() is false, which
    # needs BOTH a non-buildroot os-release and a package manager on PATH. Left
    # to the ambient host this passes on Debian CI and fails anywhere without apt.
    fake_non_buildroot_os_release
    fake_os_package_manager_present

    local original_content
    original_content=$(cat "$conf")

    configure_moonraker_updates "k1"

    # No persistent_files to remove, so content should be unchanged
    [ "$(cat "$conf")" = "$original_content" ]
}

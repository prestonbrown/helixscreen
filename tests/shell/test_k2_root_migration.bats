#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Relocating a platform's payload root.
#
# A platform declares `previous_root` in assets/config/platforms.json when its
# payload belongs somewhere else. An install found there is migrated rather than
# adopted, which is the one case where INSTALL_DIR is not where the tree already
# is - so the operator's config has to be carried, the init script has to be
# repointed, and the tree left behind has to be swept.
#
# The invariant these cases exist to hold: the boot path never names a directory
# without a complete install in it. Everything is ordered around that.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
PLATFORM_SH="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
MAIN_SH="$WORKTREE_ROOT/scripts/lib/installer/main.sh"
SERVICE_SH="$WORKTREE_ROOT/scripts/lib/installer/service.sh"

setup() {
    load helpers

    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    unset INSTALL_DIR _USER_INSTALL_DIR HOST_INSTALL_ROOT STANDALONE_INSTALL
    export SUDO=""
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }

    FAKE_ROOT="$BATS_TEST_TMPDIR/root"
    mkdir -p "$FAKE_ROOT"
}

seed_install() {
    local dir="$FAKE_ROOT$1"
    mkdir -p "$dir/bin" "$dir/config"
    printf '#!/bin/sh\nexit 0\n' > "$dir/bin/helix-screen"
    chmod +x "$dir/bin/helix-screen"
    printf '{"seeded":true}\n' > "$dir/config/settings.json"
    printf '%s\n' "$dir"
}

# Point the probe at the sandbox; the real list holds absolute device paths.
use_sandbox_dirs() {
    _HELIX_KNOWN_INSTALL_DIRS=""
    for d in /opt/helixscreen /srv/helixscreen /usr/data/helixscreen \
             /user-resource/helixscreen /userdata/helixscreen \
             /mnt/UDISK/helixscreen; do
        _HELIX_KNOWN_INSTALL_DIRS="$_HELIX_KNOWN_INSTALL_DIRS $FAKE_ROOT$d"
    done
}

# ---------------------------------------------------------------------------
# Choosing to migrate
# ---------------------------------------------------------------------------

@test "an install at the superseded root is migrated, not adopted" {
    use_sandbox_dirs
    local old
    old="$(seed_install /opt/helixscreen)"

    set_install_paths k2

    [ "$MIGRATE_FROM_DIR" = "$old" ] || fail "MIGRATE_FROM_DIR is '$MIGRATE_FROM_DIR'"
    [ "$INSTALL_DIR" = "/mnt/UDISK/helixscreen" ] || \
        fail "adoption pinned the old root: '$INSTALL_DIR'"
}

@test "an install already at the new root is not a migration" {
    use_sandbox_dirs
    seed_install /mnt/UDISK/helixscreen >/dev/null

    set_install_paths k2

    [ -z "$MIGRATE_FROM_DIR" ] || fail "flagged a migration for '$MIGRATE_FROM_DIR'"
}

@test "a half-finished migration is resumed, not abandoned" {
    # Both trees present: the run that extracted the new payload died before it
    # could sweep the old one. The old tree still has to be cleaned up.
    use_sandbox_dirs
    local old
    old="$(seed_install /opt/helixscreen)"
    seed_install /mnt/UDISK/helixscreen >/dev/null

    set_install_paths k2

    [ "$MIGRATE_FROM_DIR" = "$old" ] || \
        fail "the tree left behind was forgotten: '$MIGRATE_FROM_DIR'"
}

@test "an explicit INSTALL_DIR outranks the migration" {
    use_sandbox_dirs
    seed_install /opt/helixscreen >/dev/null
    _USER_INSTALL_DIR="$FAKE_ROOT/somewhere/helixscreen"
    INSTALL_DIR="$_USER_INSTALL_DIR"

    set_install_paths k2

    [ -z "$MIGRATE_FROM_DIR" ] || fail "overrode a deliberate INSTALL_DIR"
}

@test "a platform declaring no superseded root never migrates" {
    use_sandbox_dirs
    seed_install /usr/data/helixscreen >/dev/null

    set_install_paths k1

    [ -z "${MIGRATE_FROM_DIR:-}" ] || \
        fail "k1 declares no previous_root but flagged '$MIGRATE_FROM_DIR'"
}

@test "an empty superseded directory is not an install" {
    use_sandbox_dirs
    mkdir -p "$FAKE_ROOT/opt/helixscreen/bin"

    set_install_paths k2

    [ -z "$MIGRATE_FROM_DIR" ] || fail "a husk was treated as an install"
}

# ---------------------------------------------------------------------------
# Ordering: the boot path never names an incomplete install
# ---------------------------------------------------------------------------

_step_line() {
    grep -n "^[[:space:]]*$1\b" "$MAIN_SH" | head -1 | cut -d: -f1
}

@test "state moves out before anything extracts over it" {
    local move extract
    move=$(_step_line migrate_previous_state_dir)
    extract=$(_step_line extract_release)
    [ -n "$move" ] && [ -n "$extract" ] || fail "step not found: move=$move extract=$extract"
    [ "$move" -lt "$extract" ] || \
        fail "extract_release (line $extract) runs before the state move (line $move)"
}

@test "the init script is repointed only after the payload is complete" {
    local extract service
    extract=$(_step_line extract_release)
    service=$(_step_line install_service)
    [ -n "$extract" ] && [ -n "$service" ] || fail "step not found"
    [ "$extract" -lt "$service" ] || \
        fail "install_service (line $service) runs before extract_release (line $extract)"
}

@test "the old tree is swept only after the service is up" {
    local start sweep
    start=$(_step_line start_service)
    sweep=$(_step_line cleanup_migrated_install)
    [ -n "$start" ] && [ -n "$sweep" ] || fail "step not found"
    [ "$start" -lt "$sweep" ] || \
        fail "cleanup_migrated_install (line $sweep) runs before start_service (line $start)"
}

# ---------------------------------------------------------------------------
# The self-update escape hatch
# ---------------------------------------------------------------------------

@test "a migrating self-update repoints DAEMON_DIR without copying the script" {
    . "$SERVICE_SH"
    local init="$BATS_TEST_TMPDIR/S99helixscreen"
    printf 'DAEMON_DIR="/opt/helixscreen"\n# LOCAL CUSTOMIZATION\n' > "$init"

    HELIX_SELF_UPDATE=1
    INIT_SCRIPT_DEST="$init"
    INSTALL_DIR="/mnt/UDISK/helixscreen"
    MIGRATE_FROM_DIR="/opt/helixscreen"

    run install_service_sysv
    [ "$status" -eq 0 ]

    grep -q 'DAEMON_DIR="/mnt/UDISK/helixscreen"' "$init" || \
        fail "DAEMON_DIR still names the old tree: $(cat "$init")"
    # #314: the script is edited in place, never replaced, so platform
    # customizations survive the update.
    grep -q 'LOCAL CUSTOMIZATION' "$init" || fail "the init script was overwritten"
}

@test "a non-migrating self-update leaves DAEMON_DIR alone" {
    . "$SERVICE_SH"
    local init="$BATS_TEST_TMPDIR/S99helixscreen"
    printf 'DAEMON_DIR="/opt/helixscreen"\n' > "$init"

    HELIX_SELF_UPDATE=1
    INIT_SCRIPT_DEST="$init"
    INSTALL_DIR="/mnt/UDISK/helixscreen"
    MIGRATE_FROM_DIR=""

    run install_service_sysv
    [ "$status" -eq 0 ]

    grep -q 'DAEMON_DIR="/opt/helixscreen"' "$init" || \
        fail "rewrote DAEMON_DIR on an ordinary self-update: $(cat "$init")"
}

# ---------------------------------------------------------------------------
# Carrying the operator's files off the old tree
# ---------------------------------------------------------------------------

_load_release() {
    export GITHUB_REPO="prestonbrown/helixscreen"
    . "$WORKTREE_ROOT/scripts/lib/installer/release.sh"
    TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$TMP_DIR"
}

@test "config is read from the tree being migrated away from" {
    _load_release
    local old="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$old/config"
    printf '{"moonraker_host":"kept"}\n' > "$old/config/settings.json"
    printf 'HELIX_LOG_LEVEL=debug\n' > "$old/config/helixscreen.env"

    backup_existing_config "$old"

    [ -n "$BACKUP_CONFIG" ] || fail "no config was backed up"
    grep -q 'kept' "$BACKUP_CONFIG" || fail "the operator's settings were not carried"
    grep -q 'HELIX_LOG_LEVEL=debug' "$BACKUP_ENV" || fail "helixscreen.env was not carried"
}

@test "the config source is the old tree while a migration is in flight" {
    _load_release
    MIGRATE_FROM_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    mkdir -p "$MIGRATE_FROM_DIR" "$INSTALL_DIR"

    run _config_source_dir
    [ "$output" = "$MIGRATE_FROM_DIR" ] || \
        fail "would have read config from '$output', losing the operator's settings"
}

@test "the config source is the install root when nothing is migrating" {
    _load_release
    MIGRATE_FROM_DIR=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR"

    run _config_source_dir
    [ "$output" = "$INSTALL_DIR" ] || fail "read config from '$output'"
}

@test "an empty new root yields no config, which is what would be lost" {
    # Reading from INSTALL_DIR during a migration finds nothing. The case exists
    # so the wiring above is provably load-bearing and not a coincidence.
    _load_release
    local fresh="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    mkdir -p "$fresh"

    backup_existing_config "$fresh"

    [ -z "${BACKUP_CONFIG:-}" ] || fail "invented a config from an empty tree"
}

# ---------------------------------------------------------------------------
# Clearing the payload's directory of state
# ---------------------------------------------------------------------------

@test "every subdirectory moves, not just cache and logs" {
    _load_release
    PREVIOUS_STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state"
    INSTALL_DIR="$PREVIOUS_STATE_DIR"
    mkdir -p "$PREVIOUS_STATE_DIR/cache/helix_thumbs" \
             "$PREVIOUS_STATE_DIR/logs" \
             "$PREVIOUS_STATE_DIR/something_else"
    printf 'x\n' > "$PREVIOUS_STATE_DIR/logs/helix.log"

    migrate_previous_state_dir

    [ -d "$STATE_DIR/cache/helix_thumbs" ] || fail "cache did not move"
    [ -f "$STATE_DIR/logs/helix.log" ]     || fail "logs did not move"
    [ -d "$STATE_DIR/something_else" ]     || fail "an unnamed subdir was left behind"
    [ -z "$(ls -A "$PREVIOUS_STATE_DIR")" ] || \
        fail "the payload's directory still holds: $(ls -A "$PREVIOUS_STATE_DIR")"
}

@test "a directory holding a payload is not treated as state" {
    _load_release
    PREVIOUS_STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state"
    INSTALL_DIR="$PREVIOUS_STATE_DIR"
    mkdir -p "$PREVIOUS_STATE_DIR/bin" "$PREVIOUS_STATE_DIR/assets"
    printf '#!/bin/sh\n' > "$PREVIOUS_STATE_DIR/bin/helix-screen"
    chmod +x "$PREVIOUS_STATE_DIR/bin/helix-screen"

    migrate_previous_state_dir

    [ -d "$PREVIOUS_STATE_DIR/assets" ] || fail "folded an install into the state root"
}

@test "a name already at the target is kept, never clobbered" {
    _load_release
    PREVIOUS_STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    STATE_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state"
    INSTALL_DIR="$PREVIOUS_STATE_DIR"
    mkdir -p "$PREVIOUS_STATE_DIR/cache" "$STATE_DIR/cache"
    printf 'older\n' > "$PREVIOUS_STATE_DIR/cache/marker"
    printf 'newer\n' > "$STATE_DIR/cache/marker"

    migrate_previous_state_dir

    grep -q newer "$STATE_DIR/cache/marker"          || fail "clobbered the newer copy"
    grep -q older "$STATE_DIR/cache.previous/marker" || fail "lost the older copy"
}

# ---------------------------------------------------------------------------
# Sweeping the tree left behind
# ---------------------------------------------------------------------------

_stage_sweep() {
    _load_release
    MIGRATE_FROM_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"
    mkdir -p "$MIGRATE_FROM_DIR" "$INSTALL_DIR/bin" "$INSTALL_DIR/config"
    printf '#!/bin/sh\n' > "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    printf '{}\n' > "$INSTALL_DIR/config/settings.json"
}

@test "the old tree goes once the new one is complete" {
    _stage_sweep
    cleanup_migrated_install
    [ ! -d "$MIGRATE_FROM_DIR" ] || fail "the tree left behind was not swept"
}

@test "the old tree stays when the new root has no runnable binary" {
    _stage_sweep
    rm -f "$INSTALL_DIR/bin/helix-screen"
    cleanup_migrated_install
    [ -d "$MIGRATE_FROM_DIR" ] || \
        fail "removed the only working install on the device"
}

@test "the old tree stays when the config was not carried" {
    _stage_sweep
    rm -f "$INSTALL_DIR/config/settings.json"
    cleanup_migrated_install
    [ -d "$MIGRATE_FROM_DIR" ] || fail "removed the only copy of the operator's config"
}

@test "a path that is not an install root is refused" {
    _stage_sweep
    MIGRATE_FROM_DIR="$BATS_TEST_TMPDIR/mnt/UDISK"
    mkdir -p "$MIGRATE_FROM_DIR/printer_data"
    run cleanup_migrated_install
    [ -d "$MIGRATE_FROM_DIR/printer_data" ] || fail "removed a mount root"
    echo "$output" | grep -qi 'refus' || fail "swept a bare mount root silently"
}

# ---------------------------------------------------------------------------
# Moonraker's frozen path:
# ---------------------------------------------------------------------------

_load_moonraker() {
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"
    host_refuse_mod_owned() { :; }
}

_conf_with_path() {
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    cat > "$conf" <<CONF
[server]
host: 0.0.0.0

[update_manager helixscreen]
type: web
channel: stable
repo: prestonbrown/helixscreen
path: $1

[update_manager klipper]
type: git_repo
path: ~/klipper
CONF
    printf '%s\n' "$conf"
}

@test "a stale path: is repointed at the install root" {
    _load_moonraker
    local conf; conf=$(_conf_with_path /opt/helixscreen)
    INSTALL_DIR="/mnt/UDISK/helixscreen"

    sync_update_manager_path "$conf"

    grep -q '^path: /mnt/UDISK/helixscreen$' "$conf" || \
        fail "path: was not repointed: $(grep '^path:' "$conf")"
}

@test "a foreign update_manager section is left alone" {
    _load_moonraker
    local conf; conf=$(_conf_with_path /opt/helixscreen)
    INSTALL_DIR="/mnt/UDISK/helixscreen"

    sync_update_manager_path "$conf"

    grep -q '^path: ~/klipper$' "$conf" || fail "rewrote somebody else's section"
}

@test "a path: that already matches is not rewritten" {
    _load_moonraker
    local conf; conf=$(_conf_with_path /mnt/UDISK/helixscreen)
    INSTALL_DIR="/mnt/UDISK/helixscreen"
    local before; before=$(cat "$conf")

    sync_update_manager_path "$conf"

    [ "$(cat "$conf")" = "$before" ] || fail "rewrote a conf that was already correct"
    [ ! -f "${conf}.bak.helixscreen" ] || fail "took a backup for a no-op"
}

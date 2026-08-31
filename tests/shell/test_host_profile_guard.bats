#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mod-owned-path guard (lib/installer/host_profile.sh).
#
# On a host whose firmware is a mod that owns a tree of its own (Forge-X /
# Z-Mod: git checkout at /usr/data/config/mod, chroots under /usr/data/.mod),
# the installer must never mv, rm -rf, or arm Moonraker's NetDeploy against
# anything under that tree. Those paths are managed by the mod; tearing into
# them from the standalone installer leaves a printer that cannot boot its UI.
# The one exception is --mod-payload (HELIX_MOD_PAYLOAD=1), whose contract is
# an in-place payload update inside the mod's own layout.
#
# The probe's candidate roots are env-overridable for the same reason
# clean_helix_state_dirs' are (HELIX_STATE_VAR_LIB): tests redirect them at a
# sandbox instead of touching the real /usr/data.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_stat_shim

    SANDBOX="$BATS_TEST_TMPDIR/root"
    mkdir -p "$SANDBOX/usr/data/config/mod/.shell" "$SANDBOX/usr/data/.mod/.forge-x"
    touch "$SANDBOX/usr/data/config/mod/.shell/platform.sh"

    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED \
          _HELIX_RELEASE_SOURCED _HELIX_MOONRAKER_SOURCED
    export SUDO=""
    export HELIX_MOD_PAYLOAD=""

    # Production module order (bundle-installer.sh): common.sh first (logging),
    # then host_profile.sh, then the consumers under test.
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/release.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"

    # Capture log output so refusal messages are assertable. Must come AFTER
    # common.sh, which defines the real (stderr + ANSI) implementations.
    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_error log_success
}

# Point the probe's candidate lists at the sandbox.
sandbox_candidates() {
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/usr/data/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/usr/data/.mod/.forge-x"
}

# ===========================================================================
# host_profile_probe
# ===========================================================================

@test "host_profile_probe: finds the mod tree and answers outside the chroot" {
    sandbox_candidates
    mkdir -p "$SANDBOX/usr/data/.mod/.forge-x/usr/bin"
    host_profile_probe

    [ "$HOST_MOD_ROOT" = "$SANDBOX/usr/data/config/mod" ] \
        || fail "HOST_MOD_ROOT='$HOST_MOD_ROOT'"
    [ "$HOST_MOD_CHROOT" = "$SANDBOX/usr/data/.mod/.forge-x" ] \
        || fail "HOST_MOD_CHROOT='$HOST_MOD_CHROOT'"
    # The test runs on the host, not inside the chroot, so the state must be
    # outside:<path> — never "inside" (that would send the AD5X chroot guard
    # the wrong way) and never "none" (the chroot exists).
    [ "$HOST_CHROOT_STATE" = "outside:$SANDBOX/usr/data/.mod/.forge-x" ] \
        || fail "HOST_CHROOT_STATE='$HOST_CHROOT_STATE'"
    [ "$HOST_SERVICE_MECHANISM" = "mod-managed" ] \
        || fail "HOST_SERVICE_MECHANISM='$HOST_SERVICE_MECHANISM'"
    [ "$HOST_INSTALL_ROOT" = "$SANDBOX/usr/data/config/mod/.bin/helixscreen" ] \
        || fail "HOST_INSTALL_ROOT='$HOST_INSTALL_ROOT'"
    [ "$HOST_OWNS_COMPETING_UIS" = "1" ] \
        || fail "HOST_OWNS_COMPETING_UIS='$HOST_OWNS_COMPETING_UIS'"
}

@test "host_profile_probe: a host without the mod stays on the plain-host defaults" {
    # Candidates point at paths that do not exist in this sandbox.
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/no/mod/here"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/no/chroot/here"
    host_profile_probe

    [ -z "$HOST_MOD_ROOT" ] || fail "HOST_MOD_ROOT='$HOST_MOD_ROOT'"
    [ -z "$HOST_MOD_CHROOT" ] || fail "HOST_MOD_CHROOT='$HOST_MOD_CHROOT'"
    [ "$HOST_CHROOT_STATE" = "none" ] || fail "HOST_CHROOT_STATE='$HOST_CHROOT_STATE'"
    [ "$HOST_SERVICE_MECHANISM" = "systemd" ] \
        || fail "HOST_SERVICE_MECHANISM='$HOST_SERVICE_MECHANISM'"
    [ "$HOST_OWNS_COMPETING_UIS" = "0" ] \
        || fail "HOST_OWNS_COMPETING_UIS='$HOST_OWNS_COMPETING_UIS'"
}

# ===========================================================================
# host_path_is_mod_owned
# ===========================================================================

@test "host_path_is_mod_owned: true under the mod tree and the .mod chroot, false elsewhere" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    HOST_MOD_CHROOT="$SANDBOX/usr/data/.mod/.forge-x"

    run host_path_is_mod_owned "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ] || fail "mod tree not owned: $output"

    # Inside the detected chroot (what the guard protects on a real box).
    run host_path_is_mod_owned "$SANDBOX/usr/data/.mod/.forge-x/anything"
    [ "$status" -eq 0 ] || fail "chroot not owned: $output"

    # The canonical chroot root counts even without a probe (symlink-resolved).
    run host_path_is_mod_owned "/usr/data/.mod/.zmod/srv/helixscreen"
    [ "$status" -eq 0 ] || fail "canonical .mod root not owned: $output"

    # Ordinary install locations are not mod-owned.
    run host_path_is_mod_owned "$SANDBOX/srv/helixscreen"
    [ "$status" -ne 0 ] || fail "plain install dir reported mod-owned"
    run host_path_is_mod_owned "$SANDBOX/opt/helixscreen"
    [ "$status" -ne 0 ] || fail "plain install dir reported mod-owned"
}

@test "host_path_is_mod_owned: a symlink into the mod tree resolves before matching" {
    # The ownership test must see through a symlink pointing into the mod tree,
    # or an INSTALL_DIR reached via a link sails past the guard.
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    mkdir -p "$SANDBOX/usr/data/config/mod/.bin/helixscreen" "$SANDBOX/opt"
    ln -s "$SANDBOX/usr/data/config/mod/.bin/helixscreen" "$SANDBOX/opt/helixscreen"

    run host_path_is_mod_owned "$SANDBOX/opt/helixscreen"
    [ "$status" -eq 0 ] || fail "symlink into the mod tree not resolved: $output"
}

@test "host_path_is_mod_owned: with no mod on the host nothing is owned" {
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""
    run host_path_is_mod_owned "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -ne 0 ]
    run host_path_is_mod_owned "/usr/data/.mod/anything"
    [ "$status" -eq 0 ] || fail "canonical .mod root must still count without a probe"
}

@test "canonical mod trees are owned even when the probe found no marker" {
    # Half-uninstall / mod-refactor scenario: .shell/platform.sh is gone, the
    # probe leaves HOST_MOD_ROOT empty, and the payload still sits in the
    # tree. The git trees are the mod's namespace whether or not a marker
    # vouched for them — an INSTALL_DIR pointed there must still be refused.
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""

    run host_path_is_mod_owned "/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ] || fail "/usr/data/config/mod not owned without marker: $output"
    run host_path_is_mod_owned "/opt/config/mod/.bin/exec/logged-real"
    [ "$status" -eq 0 ] || fail "/opt/config/mod not owned without marker: $output"

    # The refusal must reach the entry gate, not just the predicate.
    run validate_install_dir "/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]

    # Same shape, different tree: not the mod's namespace, not owned.
    run host_path_is_mod_owned "/usr/data/config/other-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /usr/data/config path reported owned"
    run host_path_is_mod_owned "/opt/config/not-a-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /opt/config path reported owned"
}

# ===========================================================================
# validate_install_dir (common.sh) — the install/update entry gate
# ===========================================================================

@test "validate_install_dir refuses a mod-owned INSTALL_DIR outside --mod-payload" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    HELIX_MOD_PAYLOAD=""
    run validate_install_dir "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    [[ "$output" == *"--mod-payload"* ]]

    # --mod-payload's contract is an in-place update inside the mod layout:
    # the guard must stand down for it.
    HELIX_MOD_PAYLOAD=1
    run validate_install_dir "$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ] || fail "refused a --mod-payload install: $output"
}

# ===========================================================================
# release.sh — the update-path destructive sites
# ===========================================================================

@test "release.sh update backup refuses to mv a mod-owned root" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    INSTALL_DIR="$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    mkdir -p "$INSTALL_DIR/bin"
    echo old > "$INSTALL_DIR/bin/helix-screen"

    run backup_install_dir_for_update
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    [ -d "$INSTALL_DIR" ]              # untouched
    [ ! -d "${INSTALL_DIR}.old" ]      # nothing moved aside
}

@test "backup_install_dir_for_update still swaps a non-mod-owned install aside" {
    # The extraction from extract_release() must not change the swap itself:
    # same-fs mv to INSTALL_DIR.old, stale .old cleared first.
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    INSTALL_DIR="$SANDBOX/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/bin" "${INSTALL_DIR}.old"
    echo stale > "${INSTALL_DIR}.old/stale-marker"
    echo old > "$INSTALL_DIR/bin/helix-screen"

    backup_install_dir_for_update
    [ "$INSTALL_BACKUP" = "${INSTALL_DIR}.old" ] || fail "INSTALL_BACKUP='$INSTALL_BACKUP'"
    [ -f "${INSTALL_DIR}.old/bin/helix-screen" ] # moved aside, not merged
    [ ! -d "$INSTALL_DIR" ]                      # original gone
    [ ! -f "${INSTALL_DIR}.old/stale-marker" ]   # stale .old was cleared
}

# ===========================================================================
# moonraker.sh — the NetDeploy-arming stanza write
# ===========================================================================

@test "add_update_manager_section refuses to arm NetDeploy at a mod-owned install" {
    # [update_manager helixscreen] type: web makes Moonraker's NetDeploy own
    # `path:` — its update flow rmtree()s the path before extraction. Pointing
    # it into the mod tree hands the mod's own payload to that rmtree.
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    INSTALL_DIR="$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    printf '[server]\n' > "$conf"

    run add_update_manager_section "$conf"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    refute_grep 'update_manager helixscreen' "$conf"
    [ ! -f "${conf}.bak.helixscreen" ]   # not even the backup write happened
}

@test "add_update_manager_section writes the stanza for a --mod-payload install" {
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    HELIX_MOD_PAYLOAD=1
    INSTALL_DIR="$SANDBOX/usr/data/config/mod/.bin/helixscreen"
    local conf="$BATS_TEST_TMPDIR/moonraker.conf"
    printf '[server]\n' > "$conf"

    add_update_manager_section "$conf"
    grep -q '^\[update_manager helixscreen\]' "$conf"
    grep -q "^path: $INSTALL_DIR\$" "$conf"
}

# ===========================================================================
# uninstall.sh sweeps — mod-owned entries are skipped, not rm -rf'd
# ===========================================================================

@test "every HELIX_INSTALL_DIRS sweep skips mod-owned paths" {
    # The sweeps live inline inside uninstall() / clean_old_installation() and
    # the uninstaller bundle's generated remove_installation() — too entangled
    # to drive whole, so the skip is pinned by shape: each loop over
    # HELIX_INSTALL_DIRS must route its rm -rf behind the blocked test.
    # The test functions themselves are covered behaviorally above.
    local n

    n=$(grep -c 'host_mod_destruct_blocked "$install_dir"' \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh")
    [ "$n" -eq 2 ] || fail "uninstall.sh carries $n skip guards (want 2: uninstall + clean_old_installation)"

    n=$(grep -c 'host_mod_destruct_blocked "$install_dir"' \
        "$WORKTREE_ROOT/scripts/bundle-uninstaller.sh")
    [ "$n" -eq 1 ] || fail "bundle-uninstaller.sh carries $n skip guards (want 1: remove_installation)"
}

@test "release.sh guards every INSTALL_DIR-destructive site in the update path" {
    # Three host_refuse_mod_owned calls: the read-only-parent in-place loop,
    # the off-partition relocation mv, and backup_install_dir_for_update.
    local n
    n=$(grep -c 'host_refuse_mod_owned' "$WORKTREE_ROOT/scripts/lib/installer/release.sh")
    [ "$n" -eq 3 ] || fail "release.sh carries $n guards (want 3)"
}

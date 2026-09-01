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
          _HELIX_PLATFORM_SOURCED _HELIX_RELEASE_SOURCED _HELIX_MOONRAKER_SOURCED
    export SUDO=""
    export HELIX_MOD_PAYLOAD=""

    # Production module order (bundle-installer.sh): common.sh first (logging),
    # then host_profile.sh, then the consumers under test. platform.sh carries
    # set_install_paths/detect_tmp_dir, whose gates own the mod-owned refusal
    # since it was hoisted out of common.sh's validators.
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
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

@test "host_profile_probe: a mod tree without the mod chroot is recognized but not payload-managed" {
    # The AD5M Forge-X layout (A1): the mod's tree exists, but there is no
    # Buildroot chroot beside it. The tree is still recognized - flavor
    # detection, the forgex takeover paths, the mod-ownership guard - but the
    # payload contract's answers stay OFF: nothing on that host shape is
    # verified to manage a payload, and claiming it would silently relocate a
    # population whose installs live at the platform root.
    sandbox_candidates
    # No chroot directory: the chroot candidates point at nothing.
    host_profile_probe

    [ "$HOST_MOD_ROOT" = "$SANDBOX/usr/data/config/mod" ] \
        || fail "HOST_MOD_ROOT='$HOST_MOD_ROOT' - the tree must still be recognized"
    [ "$HOST_CHROOT_STATE" = "none" ] \
        || fail "HOST_CHROOT_STATE='$HOST_CHROOT_STATE' - fixture is not the chroot-less shape"
    [ -z "$HOST_INSTALL_ROOT" ] \
        || fail "HOST_INSTALL_ROOT set on a chroot-less host - the probe claimed a payload root"
    [ "$HOST_SERVICE_MECHANISM" = "systemd" ] \
        || fail "HOST_SERVICE_MECHANISM='$HOST_SERVICE_MECHANISM' - the mod does not own the service here"
    [ "$HOST_OWNS_COMPETING_UIS" = "0" ] \
        || fail "HOST_OWNS_COMPETING_UIS set on a chroot-less host"
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

    # The refusal must reach the gate too, not just the predicate - see
    # "set_install_paths' gate refuses a canonical mod path even with no
    # probe" below.

    # Same shape, different tree: not the mod's namespace, not owned.
    run host_path_is_mod_owned "/usr/data/config/other-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /usr/data/config path reported owned"
    run host_path_is_mod_owned "/opt/config/not-a-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /opt/config path reported owned"
}

# ===========================================================================
# The install-dir gate - set_install_paths' final validate (platform.sh)
# ===========================================================================
#
# The mod-owned refusal lives at set_install_paths' final gate, one layer
# above common.sh's name validator: common.sh is the bundle's first module
# and must stay free of later-module calls (the arch review's S2 hoist).
# These tests drive the real flow, not the validator in isolation.

@test "set_install_paths' gate refuses a mod-owned INSTALL_DIR outside --mod-payload" {
    sandbox_candidates
    # The gate's scenario is the payload-capable host shape: the mod tree WITH
    # its chroot (a chroot-less tree is the AD5M Forge-X layout, which the
    # probe no longer claims a payload root for - see A1).
    mkdir -p "$SANDBOX/usr/data/.mod/.forge-x/usr/bin"
    host_profile_probe
    HELIX_MOD_PAYLOAD=""
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    run set_install_paths "ad5x" "forge_x"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    [[ "$output" == *"--payload-root"* ]]   # the current lever, not the retired --mod-payload

    # --mod-payload's contract is an in-place update inside the mod layout:
    # the guard must stand down for it.
    HELIX_MOD_PAYLOAD=1
    set_install_paths "ad5x" "forge_x"
    [ "$INSTALL_DIR" = "$SANDBOX/usr/data/config/mod/.bin/helixscreen" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR'"
}

@test "set_install_paths' gate refuses a canonical mod path even with no probe" {
    # Half-uninstall / mod-refactor scenario: the probe found no marker, so
    # only the canonical mod roots vouch for the tree. The pi branch is the
    # route an operator's INSTALL_DIR override takes on such a host - the
    # refusal must still meet it at the gate.
    mock_command_fail "systemctl"
    mock_command "ps" ""
    mock_command_fail "id"
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""
    HOST_INSTALL_ROOT=""
    HELIX_MOD_PAYLOAD=""
    _USER_INSTALL_DIR="/usr/data/config/mod/.bin/helixscreen"
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    run set_install_paths "pi"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    [[ "$output" == *"--payload-root"* ]]   # the current lever, not the retired --mod-payload
}

@test "detect_tmp_dir refuses a mod-owned user TMP_DIR outside --mod-payload" {
    # TMP_DIR is rm -rf'd on both the success and the failure path. Its name
    # guard ('*helixscreen-install*') is satisfied by a scratch dir INSIDE the
    # mod's git tree too, so ownership -- not just the name -- must gate it:
    # staging inside their repo leaves untracked files their OTA's git clean
    # then removes, and the rm -rf tears through the mod's namespace. The
    # refusal rides detect_tmp_dir's override branch (the only route a
    # user-set TMP_DIR takes), not the name validator.
    HOST_MOD_ROOT="$SANDBOX/usr/data/config/mod"
    HELIX_MOD_PAYLOAD=""
    TMP_DIR="$SANDBOX/usr/data/config/mod/helixscreen-install"
    run detect_tmp_dir
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
    [[ "$output" == *"--payload-root"* ]]   # the current lever, not the retired --mod-payload

    HELIX_MOD_PAYLOAD=1
    detect_tmp_dir
    [ "$TMP_DIR" = "$SANDBOX/usr/data/config/mod/helixscreen-install" ] \
        || fail "refused a --mod-payload scratch dir (TMP_DIR='$TMP_DIR')"
}

# ===========================================================================
# Module separation - common.sh stays free of later-module calls (S2)
# ===========================================================================

@test "common.sh never reaches into host_profile.sh (bundle position 1 stays host-free)" {
    # Structural pin (the arch review's S2, resolved by the guard hoist):
    # common.sh is the bundle's FIRST module and host_profile.sh its second,
    # so any common -> host_profile call is an earlier-to-later edge the
    # bundle order forbids - and a cycle, since host_profile needs common's
    # log_error. Grep IS the behavior here: the invariant is about source
    # structure, and the two guards this file's earlier tests exercise must
    # live one layer up (platform.sh) for it to hold.
    local common="$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    local profile="$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"

    # Half 1: no host_* symbol of any kind (call, variable, or comment).
    if grep -qi 'host_' "$common"; then  # -i: HOST_* globals are the same edge as host_* calls
        grep -n 'host_' "$common"
        fail "common.sh references host_profile symbols; the bundle's first module must stay host-free"
    fi

    # Half 2: no reference to ANY function host_profile defines - the
    # module's non-prefixed helpers (resolve_payload_root & co.) are the
    # same edge the host_* names are. The list is derived from
    # host_profile.sh itself so a new export cannot slip past the pin.
    local fn
    while IFS= read -r fn; do
        [ -n "$fn" ] || continue
        if grep -qw "$fn" "$common"; then
            grep -nw "$fn" "$common"
            fail "common.sh references '$fn', defined in host_profile.sh"
        fi
    done < <(sed -n 's/^\([a-z_][a-z0-9_]*\)().*/\1/p' "$profile")
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

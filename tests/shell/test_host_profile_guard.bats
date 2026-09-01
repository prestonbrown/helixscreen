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
    # A HALF-INSTALLED mod: the tree exists, but its Buildroot chroot is not
    # beside it. Both real Forge-X layouts carry the chroot (each board's own
    # DATA_MNT; see the genuinely-AD5M test below), so a tree without one is a
    # mod mid-install or half-removed. The tree is still recognized - flavor
    # detection, the forgex takeover paths, the mod-ownership guard - but the
    # payload contract's answers stay OFF: nothing verified that host shape,
    # and arming it would point a payload install at a tree the mod cannot
    # yet run it from.
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

# The genuinely-AD5M Forge-X sandbox: the AD5M block of the mod's own
# descriptor (ff5m .shell/platform.sh) puts MOD_ROOT at /opt/config/mod and
# DATA_MNT at /data, so the mod's Buildroot chroot - one derivation off
# DATA_MNT, the same rule the AD5X's /usr/data/.mod/.forge-x follows - is
# /data/.mod/.forge-x. The descriptor selects its block by uname (mips ->
# AD5X, everything else -> AD5M), which is why these tests shadow uname: the
# probe answers must key on the rig's architecture, not on the test host's.
ad5m_sandbox() {
    mkdir -p "$SANDBOX/opt/config/mod/.shell" "$SANDBOX/data/.mod/.forge-x/usr/bin"
    touch "$SANDBOX/opt/config/mod/.shell/platform.sh"
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/opt/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/data/.mod/.forge-x"
}

@test "host_profile_probe: a genuinely-AD5M Forge-X host is payload-managed with the ad5m key" {
    # ff5m is ONE mod with one contract shape on both Adventurer boards; the
    # AD5M/AD5X split in A1 was our verification boundary, not a real one. The
    # probe must arm the payload answers for the AD5M layout exactly as it
    # does for the AD5X rig's - mod_data derived as a sibling of the tree
    # (/opt/config/mod_data), hook key naming the AD5M payload layout.
    ad5m_sandbox
    uname() { echo armv7l; }
    export -f uname
    host_profile_probe

    [ "$HOST_MOD_ROOT" = "$SANDBOX/opt/config/mod" ] \
        || fail "HOST_MOD_ROOT='$HOST_MOD_ROOT'"
    [ "$HOST_MOD_CHROOT" = "$SANDBOX/data/.mod/.forge-x" ] \
        || fail "HOST_MOD_CHROOT='$HOST_MOD_CHROOT' - the AD5M chroot was not probed"
    [ "$HOST_SERVICE_MECHANISM" = "mod-managed" ] \
        || fail "HOST_SERVICE_MECHANISM='$HOST_SERVICE_MECHANISM'"
    [ "$HOST_INSTALL_ROOT" = "$SANDBOX/opt/config/mod/.bin/helixscreen" ] \
        || fail "HOST_INSTALL_ROOT='$HOST_INSTALL_ROOT'"
    # mod_data is a sibling of the mod tree: /opt/config/mod_data on the AD5M,
    # their .shell/helixscreen.sh DATA_ROOT=/opt/config/mod_data/helixscreen.
    [ "$HOST_CONFIG_DIR" = "$SANDBOX/opt/config/mod_data/helixscreen/config" ] \
        || fail "HOST_CONFIG_DIR='$HOST_CONFIG_DIR'"
    [ "$HOST_PLATFORM_HOOK_KEY" = "ad5m-forgex" ] \
        || fail "HOST_PLATFORM_HOOK_KEY='$HOST_PLATFORM_HOOK_KEY' - an AD5M rig must name its own layout"
}

@test "host_profile_probe: the hook key stays ad5x-forgex on a mips (AD5X) rig" {
    # Control for the uname split: the AD5X rig's payload layout key must not
    # become ad5m-forgex when the key turns platform-aware.
    sandbox_candidates
    mkdir -p "$SANDBOX/usr/data/.mod/.forge-x/usr/bin"
    uname() { echo mips; }
    export -f uname
    host_profile_probe

    [ "$HOST_PLATFORM_HOOK_KEY" = "ad5x-forgex" ] \
        || fail "HOST_PLATFORM_HOOK_KEY='$HOST_PLATFORM_HOOK_KEY'"
}

@test "host_profile_probe: an AD5M rig carrying a legacy standalone install answers where" {
    # Before the payload contract, our own installer put ad5m+forge_x hosts at
    # /opt/helixscreen with an S90helixscreen service (set_install_paths'
    # platform root). The payload install cannot offer to adopt a root it
    # never noticed, and a rig without one must answer empty or every install
    # stops to warn about nothing. Both halves of the answer are pinned here.
    ad5m_sandbox
    uname() { echo armv7l; }
    export -f uname
    export HELIX_LEGACY_INSTALL_ROOT="$SANDBOX/opt/helixscreen"
    export HELIX_LEGACY_INIT_SCRIPT="$SANDBOX/etc/init.d/S90helixscreen"

    host_profile_probe
    [ -z "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "claimed a legacy install with no root on disk"

    mkdir -p "$SANDBOX/opt/helixscreen"
    host_profile_probe
    [ "$HOST_LEGACY_INSTALL_ROOT" = "$SANDBOX/opt/helixscreen" ] \
        || fail "HOST_LEGACY_INSTALL_ROOT='$HOST_LEGACY_INSTALL_ROOT'"
    [ "$HOST_LEGACY_INIT_SCRIPT" = "$SANDBOX/etc/init.d/S90helixscreen" ] \
        || fail "HOST_LEGACY_INIT_SCRIPT='$HOST_LEGACY_INIT_SCRIPT'"
}

@test "host_profile_probe: an AD5X rig never answers a legacy standalone root" {
    # The legacy population is AD5M-specific, and on the AD5X /opt is the
    # bind of /usr/data (ff5m's _ensure_bind), so a /opt/helixscreen there is
    # a data-partition path, not a standalone install to adopt. The legacy
    # answer must key on the platform, not on the directory's existence.
    sandbox_candidates
    mkdir -p "$SANDBOX/usr/data/.mod/.forge-x/usr/bin" "$SANDBOX/opt/helixscreen"
    uname() { echo mips; }
    export -f uname
    export HELIX_LEGACY_INSTALL_ROOT="$SANDBOX/opt/helixscreen"

    host_profile_probe

    [ "$HOST_PLATFORM_HOOK_KEY" = "ad5x-forgex" ] \
        || fail "setup: fixture did not probe as the AD5X rig"
    [ -z "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "HOST_LEGACY_INSTALL_ROOT='$HOST_LEGACY_INSTALL_ROOT' on an AD5X rig"
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
    # /data is the AD5M's DATA_MNT (ff5m's descriptor): its .mod namespace is
    # the mod's exactly as /usr/data/.mod is on the AD5X.
    run host_path_is_mod_owned "/data/.mod/.forge-x/usr/bin/bash"
    [ "$status" -eq 0 ] || fail "/data/.mod not owned without marker: $output"

    # The refusal must reach the gate too, not just the predicate - see
    # "set_install_paths' gate refuses a canonical mod path even with no
    # probe" below.

    # Same shape, different tree: not the mod's namespace, not owned.
    run host_path_is_mod_owned "/usr/data/config/other-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /usr/data/config path reported owned"
    run host_path_is_mod_owned "/opt/config/not-a-mod/.bin/helixscreen"
    [ "$status" -ne 0 ] || fail "unrelated /opt/config path reported owned"
}

@test "the AD5M chroot location is in BOTH lists: probe default and canonical literal" {
    # The BOTH-PLACES RULE (host_profile.sh): a mod location appears in the
    # env-overridable probe candidates AND in the canonical literals, or it is
    # either a path the guard does not recognize or one the probe can never
    # find without help. The AD5M's chroot is /data/.mod/.forge-x - one
    # derivation off its DATA_MNT, the same rule the AD5X's
    # /usr/data/.mod/.forge-x follows. The sandbox tests above override the
    # candidates, so the PRODUCTION default list is pinned here by shape, the
    # same way the mode tests pin their dispatch arms.
    local profile="$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"

    # The probe default (production runs leave the env unset).
    grep -q 'usr/data/.mod/.forge-x /usr/data/.mod/.zmod /data/.mod/.forge-x' "$profile" \
        || fail "the AD5M chroot is missing from the probe's default candidate list"
    # The canonical literal (canonical /data/.mod ownership is pinned
    # behaviorally in the test above; this catches the arm being re-scoped).
    grep -q '/data/.mod|/data/.mod/\*' "$profile" \
        || fail "the canonical mod-owned case is missing its /data/.mod arm"
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

@test "upstream 1.4.2 AD5M tree (common.sh, no platform.sh) is probed and owned" {
    # Found on the real AD5M rig: upstream ff5m 1.4.2 ships .shell/common.sh;
    # platform.sh exists only on the AD5X port fork. Keying the probe on the
    # fork's spelling left every upstream AD5M install unprobed - no payload
    # contract, no guard, on a live mod host.
    local sandbox="$BATS_TEST_TMPDIR/upstream-ad5m"
    mkdir -p "$sandbox/opt/config/mod/.shell" "$sandbox/data/.mod/.forge-x/usr/bin"
    touch "$sandbox/opt/config/mod/.shell/common.sh"   # upstream descriptor
    export HELIX_MOD_TREE_CANDIDATES="$sandbox/opt/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$sandbox/data/.mod/.forge-x"
    unset _HELIX_HOST_PROFILE_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    host_profile_probe
    [ "$HOST_MOD_ROOT" = "$sandbox/opt/config/mod" ]
    [ "$HOST_MOD_CHROOT" = "$sandbox/data/.mod/.forge-x" ]
    [ "$HOST_SERVICE_MECHANISM" = "mod-managed" ]
    run host_path_is_mod_owned "$sandbox/opt/config/mod/.bin/helixscreen"
    [ "$status" -eq 0 ]
}

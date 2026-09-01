#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Payload mode end to end (lib/installer).
#
# On a host whose firmware is a mod with its own git tree (Forge-X / Z-Mod),
# the payload contract is the DEFAULT (auto-detected 2026-08-31): the mod owns
# the UI service and the OTA, so a bare install replaces the payload root's
# CONTENTS in place (never mv/rm -rf of the root), preserves config/ and
# platform/, writes no service files, and leaves every Moonraker conf alone
# unless --auto-update opted in to a stanza in the mod's user.moonraker.conf.
# The flags are overrides: --standalone escapes to a self-managed install,
# --payload-root overrides where. The old --mod-payload spellings stay
# accepted as deprecated aliases / a compat no-op.
#
# Covers the brief's five mode properties (a)-(e) plus the six carry-items
# routed here from earlier task reviews:
#   1. start_service short-circuits on HOST_SERVICE_MECHANISM, not a flag
#   2. a standalone install on a mod host warns it will not be started
#   3. HELIX_MOD_PAYLOAD is settable only by the probe/flags, never the env
#   4. detect_tmp_dir never auto-stages inside the mod tree
#   5. the LOG_LEVEL migration is keyed on the host capability, not on
#      PAYLOAD_ENV_PRESERVED
#   6. the user.moonraker.conf stanza write is gated on --auto-update

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim
    install_gnu_stat_shim

    # The uninstall/clean paths below call systemctl whenever the HOST has
    # systemd (detect_init_system), and the helixscreen-update.* sweep is not
    # even gated on INIT_SYSTEM. Unstubbed, every such call raises a polkit
    # prompt on a dev desktop - one per systemctl, since "|| true" swallows
    # the error but not the auth dialog. Same pattern as test_uninstall.bats.
    mock_command_script "systemctl" 'exit 0'

    SANDBOX="$BATS_TEST_TMPDIR/root"
    MOD_ROOT="$SANDBOX/usr/data/config/mod"
    mkdir -p "$MOD_ROOT/.shell"
    touch "$MOD_ROOT/.shell/platform.sh"

    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED \
          _HELIX_PLATFORM_SOURCED _HELIX_REQUIREMENTS_SOURCED \
          _HELIX_FORGEX_SOURCED _HELIX_RELEASE_SOURCED \
          _HELIX_SERVICE_SOURCED _HELIX_MOONRAKER_SOURCED \
          _HELIX_UNINSTALL_SOURCED _HELIX_MAIN_SOURCED
    export SUDO=""

    # Production module order (bundle-installer.sh), minus the modules this
    # suite has no call path into (their uninstall-time callers are type-
    # guarded, so their absence is exercised, not hidden).
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/requirements.sh"
    source_forgex_patched
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/release.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/moonraker.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
    source_main_patched

    # Capture log output so mode messages are assertable. Must come AFTER
    # common.sh, which defines the real (stderr + ANSI) implementations.
    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_error log_success

    # Tests never run under NoNewPrivileges.
    _has_no_new_privs() { return 1; }
    export -f _has_no_new_privs

    HOST_MOD_ROOT="$MOD_ROOT"
    HOST_MOD_CHROOT="$SANDBOX/usr/data/.mod/.forge-x"
    HOST_CHROOT_STATE="outside:$HOST_MOD_CHROOT"
    HOST_SERVICE_MECHANISM="mod-managed"
    HOST_OWNS_COMPETING_UIS=1
    HOST_INSTALL_ROOT="$MOD_ROOT/.bin/helixscreen"
    HOST_CONFIG_DIR="$SANDBOX/usr/data/config/mod_data/helixscreen/config"
    HOST_MOONRAKER_USER_CONF="$SANDBOX/usr/data/config/mod_data/user.moonraker.conf"
    HOST_PLATFORM_HOOK_KEY="ad5x-forgex"
    HOST_LEGACY_INSTALL_ROOT=""
    HOST_LEGACY_INIT_SCRIPT=""

    HELIX_MOD_PAYLOAD=""
    HELIX_MOD_PAYLOAD_UPDATES=""
    MOD_PAYLOAD_ROOT=""

    # Keep every state sweep inside the sandbox.
    export HELIX_STATE_VAR_LIB="$SANDBOX/var/lib/helixscreen"
    export HELIX_STATE_ROOT_HOME="$SANDBOX/root/.helixscreen"
    export KLIPPER_HOME="$SANDBOX/root"

    TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    mkdir -p "$TMP_DIR"
    INSTALL_DIR="$HOST_INSTALL_ROOT"
}

# forgex.sh sourced PRISTINE: the module derives its mod tree from the probe
# (HOST_MOD_ROOT, set in this setup), so no path rewriting is needed. A sed
# rewrite of its /opt/config literals is exactly what masked the AD5X
# host-side layout derivation from this suite.
source_forgex_patched() {
    unset _HELIX_FORGEX_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh"
}

# main.sh defines mod_payload_mode_block + parse_installer_args. Its
# source-time traps are stripped: an ERR trap left armed inside a bats test
# fires on the first failing assertion into an undefined error_handler.
source_main_patched() {
    local patched="$BATS_TEST_TMPDIR/main.sh"
    sed -e "/^trap /d" \
        "$WORKTREE_ROOT/scripts/lib/installer/main.sh" > "$patched"
    unset _HELIX_MAIN_SOURCED
    # shellcheck disable=SC1090
    . "$patched"
}

# A payload root as the rig has it: our tree inside the mod's git tree, with
# operator-authored config, deployed platform hooks, and stale payload
# entries a real update must replace.
seed_payload_root() {
    mkdir -p "$INSTALL_DIR/bin" "$INSTALL_DIR/config" "$INSTALL_DIR/platform" \
             "$INSTALL_DIR/ui_xml" "$INSTALL_DIR/assets"
    create_fake_mips_elf "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    printf 'OLD\n' > "$INSTALL_DIR/bin/old-lib.so"
    printf 'OLD\n' > "$INSTALL_DIR/ui_xml/old.xml"
    printf 'OLD\n' > "$INSTALL_DIR/assets/STALE-ASSET.bin"
    printf 'HELIX_CONFIG_DIR=/opt/config/mod_data/helixscreen/config\nHELIX_LOG_LEVEL=info\n' \
        > "$INSTALL_DIR/config/helixscreen.env"
    cp "$INSTALL_DIR/config/helixscreen.env" "$BATS_TEST_TMPDIR/env.operator"
    printf '{"operator":true}\n' > "$INSTALL_DIR/config/settings.json"
    printf '#!/bin/sh\n# deployed platform hooks\n' > "$INSTALL_DIR/platform/hooks.sh"
    chmod +x "$INSTALL_DIR/platform/hooks.sh"
    cp "$INSTALL_DIR/platform/hooks.sh" "$BATS_TEST_TMPDIR/hooks.operator"
}

# Release tarball for an ad5x payload (MIPS binary; ad5x validates mipsel).
# Every entry is distinguishable from the seeded root's so each half of the
# replace/preserve contract has its own marker.
create_payload_tarball() {
    local want_env="${1:-yes}"
    local staging="$BATS_TEST_TMPDIR/staging"
    rm -rf "$staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets/fonts"
    create_fake_mips_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    printf 'NEW\n' > "$staging/helixscreen/bin/helix-new-lib.so"
    printf 'NEW\n' > "$staging/helixscreen/ui_xml/new.xml"
    printf 'NEW\n' > "$staging/helixscreen/assets/fonts/new-font.bin"
    printf '{"bundled":true}\n' > "$staging/helixscreen/config/settings.json"
    printf '{"new":true}\n' > "$staging/helixscreen/config/new-default.json"
    if [ "$want_env" = "yes" ]; then
        printf '# Bundled default\nHELIX_LOG_LEVEL=info\n' \
            > "$staging/helixscreen/config/helixscreen.env"
    fi
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

# ============================================================================
# (a) In-place contents replacement: config/ and platform/ survive, bin/ goes
# ============================================================================

@test "payload install: replaces contents in place, config/ and platform/ preserved" {
    HELIX_MOD_PAYLOAD=1
    seed_payload_root
    create_payload_tarball
    local inode_before
    inode_before=$(stat -c %i "$INSTALL_DIR")

    extract_release "ad5x"

    # The root itself is never moved or removed - same inode.
    [ "$(stat -c %i "$INSTALL_DIR")" = "$inode_before" ] \
        || fail "the payload root was replaced instead of updated in place"

    # Payload contents ARE replaced.
    [ ! -e "$INSTALL_DIR/bin/old-lib.so" ]
    [ -f "$INSTALL_DIR/bin/helix-new-lib.so" ]
    [ -x "$INSTALL_DIR/bin/helix-screen" ]
    [ ! -e "$INSTALL_DIR/ui_xml/old.xml" ]
    [ -f "$INSTALL_DIR/ui_xml/new.xml" ]
    [ ! -e "$INSTALL_DIR/assets/STALE-ASSET.bin" ]
    [ -f "$INSTALL_DIR/assets/fonts/new-font.bin" ]

    # config/ preserved: the operator's files win, new defaults land.
    cmp -s "$BATS_TEST_TMPDIR/env.operator" "$INSTALL_DIR/config/helixscreen.env" \
        || fail "operator helixscreen.env was modified"
    grep -q '^HELIX_LOG_LEVEL=info$' "$INSTALL_DIR/config/helixscreen.env"
    [ -f "$INSTALL_DIR/config/helixscreen.env.new" ]
    grep -q '"operator":true' "$INSTALL_DIR/config/settings.json"
    [ -f "$INSTALL_DIR/config/new-default.json" ]

    # platform/ preserved byte-for-byte.
    cmp -s "$BATS_TEST_TMPDIR/hooks.operator" "$INSTALL_DIR/platform/hooks.sh" \
        || fail "deployed platform hooks were modified"
    [ -x "$INSTALL_DIR/platform/hooks.sh" ]
}

@test "payload install: a fresh payload root (--payload-root outside the tree) is populated" {
    HELIX_MOD_PAYLOAD=1
    INSTALL_DIR="$SANDBOX/usr/data/helixscreen"
    create_payload_tarball

    extract_release "ad5x"

    [ -x "$INSTALL_DIR/bin/helix-screen" ]
    grep -q '# Bundled default' "$INSTALL_DIR/config/helixscreen.env"
    [ ! -f "$INSTALL_DIR/config/helixscreen.env.new" ]
    [ -f "$INSTALL_DIR/config/new-default.json" ]
}

# ============================================================================
# (b)+(c) Moonraker confs: the mod's stays byte-identical, the user conf gets
# a stanza only with --mod-payload-updates
# ============================================================================

@test "payload install: the mod's git-tracked moonraker.conf stays byte-identical" {
    mkdir -p "$SANDBOX/usr/data/config/mod_data" "$INSTALL_DIR"
    printf '[server]\n[update_manager forge-x]\ntype: git_repo\n' > "$MOD_ROOT/moonraker.conf"
    cp "$MOD_ROOT/moonraker.conf" "$BATS_TEST_TMPDIR/mod-conf.original"
    printf '[authorization]\n' > "$HOST_MOONRAKER_USER_CONF"
    HELIX_MOD_PAYLOAD=1

    configure_moonraker_updates "ad5x"

    cmp -s "$BATS_TEST_TMPDIR/mod-conf.original" "$MOD_ROOT/moonraker.conf" \
        || fail "the mod's own moonraker.conf was modified"
    # No stanza anywhere without the opt-in flag.
    refute_grep 'update_manager helixscreen' "$HOST_MOONRAKER_USER_CONF"
}

@test "payload install: --auto-update writes the stanza into user.moonraker.conf only" {
    # Positive control for the A4 refusal: the stanza lands when the payload
    # root is OUTSIDE the mod's tree (the durable shape --payload-root
    # provides). At a mod-owned root the option is refused instead - see the
    # "--auto-update is refused" test below.
    local durable="$SANDBOX/usr/data/helixscreen"
    mkdir -p "$SANDBOX/usr/data/config/mod_data" "$durable/bin"
    create_fake_mips_elf "$durable/bin/helix-screen"
    chmod +x "$durable/bin/helix-screen"
    printf '[server]\n' > "$MOD_ROOT/moonraker.conf"
    cp "$MOD_ROOT/moonraker.conf" "$BATS_TEST_TMPDIR/mod-conf.original"
    printf '[authorization]\n' > "$HOST_MOONRAKER_USER_CONF"
    HELIX_MOD_PAYLOAD=1
    HELIX_MOD_PAYLOAD_UPDATES=1
    INSTALL_DIR="$durable"

    configure_moonraker_updates "ad5x"

    grep -q '^\[update_manager helixscreen\]' "$HOST_MOONRAKER_USER_CONF" \
        || fail "the opted-in stanza did not land in the mod's user conf"
    cmp -s "$BATS_TEST_TMPDIR/mod-conf.original" "$MOD_ROOT/moonraker.conf"
}

# ============================================================================
# (d) Uninstall: exactly the payload subtree, display mode restored first,
# stanza dropped, nothing else under the mod tree touched
# ============================================================================

@test "payload uninstall: removes exactly the payload subtree, restores display mode, drops the stanza" {
    HELIX_MOD_PAYLOAD=1
    seed_payload_root
    # Mod-tree furniture that must survive untouched.
    printf '# mod bootstrap\n' > "$MOD_ROOT/.shell/helixscreen.sh"
    mkdir -p "$MOD_ROOT/.root"
    printf '#!/bin/sh\n' > "$MOD_ROOT/.root/S80guppyscreen"
    # Display mode taken over at install time, record parked in mod_data.
    mkdir -p "$SANDBOX/usr/data/config/mod_data"
    printf "[Variables]\ndisplay = 'HEADLESS'\n" \
        > "$SANDBOX/usr/data/config/mod_data/variables.cfg"
    printf 'GUPPY\n' \
        > "$SANDBOX/usr/data/config/mod_data/helixscreen_prev_display"
    # The stanza this install wrote into the user conf.
    printf '[authorization]\n[update_manager helixscreen]\ntype: web\n' \
        > "$HOST_MOONRAKER_USER_CONF"

    AD5M_FIRMWARE="forge_x"
    # Keep the sysv sweeps off the real host /etc.
    HELIX_INIT_SCRIPTS="$SANDBOX/nonexistent/helixscreen-init"
    detect_init_system() { INIT_SYSTEM="sysv"; }
    export -f detect_init_system
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    find "$MOD_ROOT" | sort > "$BATS_TEST_TMPDIR/modtree.before"

    run uninstall "ad5x"
    [ "$status" -eq 0 ]

    find "$MOD_ROOT" | sort > "$BATS_TEST_TMPDIR/modtree.after"

    # Nothing was added anywhere under the mod tree.
    [ -z "$(comm -13 "$BATS_TEST_TMPDIR/modtree.before" "$BATS_TEST_TMPDIR/modtree.after")" ] \
        || fail "uninstall left new files under the mod tree"

    # The only removals are the payload subtree itself.
    local removed path
    removed=$(comm -23 "$BATS_TEST_TMPDIR/modtree.before" "$BATS_TEST_TMPDIR/modtree.after")
    [ -n "$removed" ] || fail "the payload root was not removed"
    while IFS= read -r path; do
        case "$path" in
            "$INSTALL_DIR"|"$INSTALL_DIR"/*) ;;
            *) fail "uninstall removed a non-payload path under the mod tree: $path" ;;
        esac
    done <<< "$removed"

    # Display mode restored from the install-time record.
    grep -q "display = 'GUPPY'" "$SANDBOX/usr/data/config/mod_data/variables.cfg" \
        || fail "the display mode was not restored"
    # The stanza is gone from the user conf.
    refute_grep 'update_manager helixscreen' "$HOST_MOONRAKER_USER_CONF"
}

@test "payload uninstall: honors the run's --payload-root, not the probed mod root" {
    # The uninstall sweep list must carry the run's ACTUAL INSTALL_DIR. A
    # custom root outside the fixed HELIX_INSTALL_DIRS six must be removed by
    # the run that targeted it, and a stale in-tree root that this run did NOT
    # target must survive (the mod's, not ours to take).
    local custom_root="$SANDBOX/payload-root/helixscreen"
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT="$custom_root"

    # The payload this run owns: seeded at the custom root.
    INSTALL_DIR="$custom_root"
    seed_payload_root
    # A stale payload at the probed in-tree root from an older layout.
    mkdir -p "$HOST_INSTALL_ROOT/stale-bin"
    printf 'STALE\n' > "$HOST_INSTALL_ROOT/stale-bin/helix-screen"

    # What set_install_paths left behind before the mode block ran.
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    AD5M_FIRMWARE="forge_x"
    HELIX_INIT_SCRIPTS="$SANDBOX/nonexistent/helixscreen-init"
    detect_init_system() { INIT_SYSTEM="sysv"; }
    export -f detect_init_system
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    # Apply the root override the way main() does (direct call: the override
    # must land in THIS shell's INSTALL_DIR).
    mod_payload_mode_block > /dev/null 2>&1
    [ "$INSTALL_DIR" = "$custom_root" ] \
        || fail "setup: INSTALL_DIR='$INSTALL_DIR' (the override did not land)"

    run uninstall "ad5x"
    [ "$status" -eq 0 ]

    [ ! -d "$custom_root" ] \
        || fail "the run's --mod-payload-root payload was not removed (false uninstalled)"
    [ -f "$HOST_INSTALL_ROOT/stale-bin/helix-screen" ] \
        || fail "a stale in-tree root this run did not target was removed"
}

@test "payload uninstall: the display-mode restore runs before the payload removal" {
    # Ordering pin, scoped to uninstall()'s own body so a file-wide grep cannot
    # be satisfied by restore_previous_ui_platform's separate call.
    local body
    body=$(awk '/^uninstall\(\) \{/{c=1} c{print} c&&/^\}$/{exit}' \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" | sed 's/#.*//')
    [ -n "$body" ] || fail "could not extract uninstall()'s body"
    local forgex_line sweep_line
    forgex_line=$(grep -n 'uninstall_forgex' <<< "$body" | head -1 | cut -d: -f1)
    sweep_line=$(grep -n 'helix_install_dirs_for_run' <<< "$body" | head -1 | cut -d: -f1)
    [ -n "$forgex_line" ] || fail "uninstall() never restores the display mode"
    [ -n "$sweep_line" ] || fail "uninstall() does not sweep the run's install dirs"
    [ "$forgex_line" -lt "$sweep_line" ]
}

@test "standalone uninstall: a mod-owned payload root stays skipped, not removed" {
    # Coherence with Task 1's skip-not-exit sweeps: a run that is NOT in the
    # payload contract (--standalone, or any HELIX_MOD_PAYLOAD-unarmed run)
    # never removes the mod's payload root. A bare uninstall on a mod host
    # auto-arms and removes it instead - covered by the test above.
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=1
    seed_payload_root
    AD5M_FIRMWARE=""
    HELIX_INIT_SCRIPTS="$SANDBOX/nonexistent/helixscreen-init"
    detect_init_system() { INIT_SYSTEM="sysv"; }
    export -f detect_init_system
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    run uninstall "ad5x"
    [ "$status" -eq 0 ]
    [ -d "$INSTALL_DIR" ] || fail "a standalone uninstall removed the mod's payload root"
}

# ============================================================================
# (e) Disk check survives a root whose parent walk ends at "/"
# ============================================================================

@test "payload install: disk check passes when the root's parent walk ends at /" {
    HELIX_MOD_PAYLOAD=1
    # A --mod-payload-root whose ancestors do not exist: check_disk_space's
    # walk runs out at "/", and the data-mount fallback (Task 3) must carry
    # the check instead of measuring a full "/" (same shape as
    # test_requirements.bats' stock-AD5X case, pinned here under the mode).
    INSTALL_DIR="/no-such-mount-point/helixscreen"
    local data="$SANDBOX/usr/data"
    mkdir -p "$data"
    export HELIX_DATA_MOUNT_CANDIDATES="$data"
    # df answers "full" for "/" and roomy for everything else: if the check
    # df'd "/", it would refuse; measuring the data mount passes.
    mock_command_script "df" '
case "$*" in
  */) echo "/dev/mmcblk0p5 12800 12800 0 100% /" ;;
  *)  echo "/dev/mmcblk0p7 4831838 0 4831838 0% $*" ;;
esac
'
    INIT_SCRIPT_DEST="$SANDBOX/etc/init.d/S90helixscreen"
    mkdir -p "$(dirname "$INIT_SCRIPT_DEST")"

    run check_disk_space "ad5x"
    [ "$status" -eq 0 ] || fail "disk check refused a payload root on a walk to /: $output"
    [[ "$output" == *"${data}"* ]]
    [[ "$output" != *"Insufficient"* ]]
}

# ============================================================================
# Mode block: root precedence + the OTA-clean warning (Open Decision 1)
# ============================================================================

@test "mode block: --payload-root outranks the probed mod root, no OTA warning outside the tree" {
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT="$SANDBOX/usr/data/helixscreen"
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    # Called with output redirected, not via run or $(): the root override
    # must land in THIS shell's INSTALL_DIR, and both of those spawn a
    # subshell that would swallow it.
    mod_payload_mode_block > "$BATS_TEST_TMPDIR/mode-block.out" 2>&1
    [ "$INSTALL_DIR" = "$SANDBOX/usr/data/helixscreen" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR' (the mod root was not overridden)"
    ! grep -q "OTA" "$BATS_TEST_TMPDIR/mode-block.out"
}

@test "mode block: warns that a payload root inside the mod tree does not survive a Forge-X OTA" {
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    run mod_payload_mode_block
    [ "$status" -eq 0 ]
    [ "$INSTALL_DIR" = "$HOST_INSTALL_ROOT" ]
    [[ "$output" == *"inside the firmware mod's git tree"* ]]
    [[ "$output" == *"OTA"* ]]
    # The suggested durable root is derived from the rig's own data mount
    # (M2): this fixture is the AD5X shape, whose mount is the sandbox's
    # /usr/data - the parent of the probed .mod namespace.
    [[ "$output" == *"--payload-root $SANDBOX/usr/data/helixscreen"* ]] \
        || fail "the AD5X shape must suggest its own data mount, not a pinned literal"
}

@test "mode block: the OTA escape-hatch example is the rig's own data mount, per shape" {
    # M2: the hard-coded /usr/data/helixscreen example sent AD5M operators at
    # a partition their rig does not have (their DATA_MNT is /data), landing
    # the payload on the root filesystem if followed. The example derives
    # from the probe - the parent of the mod's .mod namespace, which is each
    # board's data mount: /usr/data on the AD5X, /data on the AD5M.
    mkdir -p "$SANDBOX/opt/config/mod/.shell" "$SANDBOX/data/.mod/.forge-x/usr/bin"
    touch "$SANDBOX/opt/config/mod/.shell/platform.sh"
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/opt/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/data/.mod/.forge-x"
    uname() { echo armv7l; }
    export -f uname
    # Keep this about the OD1 copy alone: no legacy root to adopt or warn
    # about on this fixture.
    export HELIX_LEGACY_INSTALL_ROOT="$SANDBOX/no-legacy-here"
    unset HOST_MOD_ROOT HOST_MOD_CHROOT HOST_CHROOT_STATE HOST_SERVICE_MECHANISM \
          HOST_INSTALL_ROOT HOST_CONFIG_DIR HOST_MOONRAKER_USER_CONF \
          HOST_PLATFORM_HOOK_KEY HOST_OWNS_COMPETING_UIS \
          HOST_LEGACY_INSTALL_ROOT HOST_LEGACY_INIT_SCRIPT
    host_profile_probe
    [ "$HOST_MOD_CHROOT" = "$SANDBOX/data/.mod/.forge-x" ] \
        || fail "setup: the AD5M chroot was not probed"

    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    run mod_payload_mode_block
    [ "$status" -eq 0 ]
    [[ "$output" == *"--payload-root $SANDBOX/data/helixscreen"* ]] \
        || fail "the AD5M shape must suggest its own data mount (/data), not the AD5X's"
    [[ "$output" != *"usr/data/helixscreen"* ]] \
        || fail "suggested the AD5X-only path on an AD5M rig"
}

# ============================================================================
# Carry-item 1: start_service short-circuits on the host capability
# ============================================================================

@test "start_service stands down on a mod-managed host without the flag (operator INSTALL_DIR seam)" {
    HOST_SERVICE_MECHANISM="mod-managed"
    HELIX_MOD_PAYLOAD=""
    INIT_SYSTEM="sysv"
    # Deliberately absent: the old failure was the whole install running and
    # then dying here with a misleading "not executable" error.
    INIT_SCRIPT_DEST="$SANDBOX/etc/init.d/S90helixscreen"

    run start_service "ad5x"
    [ "$status" -eq 0 ] \
        || fail "start_service did not stand down on a mod-managed host: $output"
}

@test "start_service: the flag alone does not stand down on a non-mod host" {
    HOST_SERVICE_MECHANISM="systemd"
    HELIX_MOD_PAYLOAD=1
    INIT_SYSTEM="sysv"
    INIT_SCRIPT_DEST="$SANDBOX/etc/init.d/S90helixscreen"

    run start_service "ad5x"
    [ "$status" -ne 0 ] || fail "the payload flag alone skipped the service start"
    [[ "$output" == *"not executable"* ]]
}

# ============================================================================
# Carry-item 2: a normal install on a mod host warns
# ============================================================================

@test "an INSTALL_DIR-seam install on a mod host warns the mod owns the UI service" {
    HELIX_MOD_PAYLOAD=""
    INSTALL_DIR="$SANDBOX/usr/data/helixscreen"

    run mod_payload_mode_block
    [ "$status" -eq 0 ]
    [[ "$output" == *"owns the UI service"* ]]
    [[ "$output" == *"not be started automatically"* ]]
    [[ "$output" == *"picks the root, not the contract"* ]]
    # It is a warning, not a refusal: the install proceeds.
    [[ "$output" != *"Refusing"* ]]
}

# ============================================================================
# Carry-item 3: the flag is the only setter of HELIX_MOD_PAYLOAD
# ============================================================================

@test "a stale HELIX_MOD_PAYLOAD in the environment never arms the destruct exemption" {
    export WORKTREE_ROOT
    run env HELIX_MOD_PAYLOAD=1 bash -c '
        unset _HELIX_HOST_PROFILE_SOURCED
        cd "$WORKTREE_ROOT" || exit 9
        . scripts/lib/installer/host_profile.sh
        [ -z "$HELIX_MOD_PAYLOAD" ]'
    [ "$status" -eq 0 ] \
        || fail "an environment HELIX_MOD_PAYLOAD=1 survived the source-time scrub"
}

# ============================================================================
# Auto-detect steer (2026-08-31, Preston): a bare install on a mod host IS a
# payload install. The flags are overrides: --standalone escapes back to a
# self-managed install, --payload-root overrides where, --auto-update opts
# into the Moonraker stanza. The old --mod-payload spellings stay accepted.
# ============================================================================

@test "autodetect: a bare run on a mod host arms the payload contract" {
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT=""
    mod_payload_autodetect
    [ "$HELIX_MOD_PAYLOAD" = "1" ] \
        || fail "a bare run on a mod host did not become a payload install"
}

@test "autodetect: --standalone keeps a mod host on the self-managed install" {
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=1
    mod_payload_autodetect
    [ -z "$HELIX_MOD_PAYLOAD" ] \
        || fail "--standalone still armed the payload contract"

    # And the self-managed install lands on the platform default, not the
    # mod's payload tree (set_install_paths must stand down too). No env
    # INSTALL_DIR: platform.sh captured one at source time from setup().
    HELIX_MOD_PAYLOAD=""
    HOST_INSTALL_ROOT="$MOD_ROOT/.bin/helixscreen"
    _USER_INSTALL_DIR=""
    detect_tmp_dir() { TMP_DIR="$BATS_TEST_TMPDIR/tmp"; }
    set_install_paths "ad5x" "forge_x"
    [ "$INSTALL_DIR" = "/srv/helixscreen" ] \
        || fail "standalone INSTALL_DIR='$INSTALL_DIR' (want the platform default)"
}

@test "autodetect: a ZMOD-shape host (no probed mod tree) never arms" {
    # The probe does not recognize ZMOD trees, so HOST_MOD_ROOT stays empty
    # there and the pre-Task-5 flow must be unchanged: no payload mode, and
    # the mod-owned guard still refuses a direct hit on a canonical mod path.
    HOST_MOD_ROOT=""
    HOST_SERVICE_MECHANISM="systemd"
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=""
    mod_payload_autodetect
    [ -z "$HELIX_MOD_PAYLOAD" ] || fail "armed the payload contract without a probed mod"

    # The refusal rides set_install_paths' install-dir gate (the operator's
    # INSTALL_DIR override takes the pi branch on an unprobed host); the
    # canonical /usr/data/.mod root counts even without a probe.
    HOST_MOD_ROOT=""
    HOST_MOD_CHROOT=""
    HOST_INSTALL_ROOT=""
    _USER_INSTALL_DIR="/usr/data/.mod/.zmod/srv/helixscreen"
    detect_tmp_dir() { TMP_DIR="$BATS_TEST_TMPDIR/tmp"; }
    run set_install_paths "pi"
    [ "$status" -ne 0 ]
    [[ "$output" == *"refusing"* ]]
}

@test "a bare install on a mod host behaves as mod-payload" {
    # End to end through the mode's own seams: autodetect arms, the extract
    # takes the in-place path, no stanza lands anywhere, the OTA warning
    # prints - all with NO flag given.
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT=""
    mod_payload_autodetect
    [ "$HELIX_MOD_PAYLOAD" = "1" ] || fail "setup: autodetect did not arm"

    seed_payload_root
    create_payload_tarball
    local inode_before
    inode_before=$(stat -c %i "$INSTALL_DIR")
    extract_release "ad5x"
    [ "$(stat -c %i "$INSTALL_DIR")" = "$inode_before" ] \
        || fail "the payload root was replaced instead of updated in place"
    cmp -s "$BATS_TEST_TMPDIR/hooks.operator" "$INSTALL_DIR/platform/hooks.sh" \
        || fail "deployed platform hooks were modified"

    mkdir -p "$SANDBOX/usr/data/config/mod_data" "$INSTALL_DIR/bin"
    printf '[server]\n' > "$MOD_ROOT/moonraker.conf"
    cp "$MOD_ROOT/moonraker.conf" "$BATS_TEST_TMPDIR/mod-conf.original"
    printf '[authorization]\n' > "$HOST_MOONRAKER_USER_CONF"
    configure_moonraker_updates "ad5x"
    cmp -s "$BATS_TEST_TMPDIR/mod-conf.original" "$MOD_ROOT/moonraker.conf" \
        || fail "the mod's own moonraker.conf was modified"
    refute_grep 'update_manager helixscreen' "$HOST_MOONRAKER_USER_CONF"

    mod_payload_mode_block > "$BATS_TEST_TMPDIR/mode-block.out" 2>&1
    grep -q "OTA" "$BATS_TEST_TMPDIR/mode-block.out" \
        || fail "the OTA warning did not print on a bare mod-host install"
}

@test "--standalone on a mod host does a normal install with the warning" {
    HELIX_MOD_PAYLOAD=""
    parse_installer_args --standalone
    mod_payload_autodetect
    [ -z "$HELIX_MOD_PAYLOAD" ] || fail "--standalone armed the payload contract"
    INSTALL_DIR="$SANDBOX/usr/data/helixscreen"

    run mod_payload_mode_block
    [ "$status" -eq 0 ]
    [[ "$output" == *"owns the UI service"* ]]
    [[ "$output" == *"not be started automatically"* ]]
    [[ "$output" == *"--standalone"* ]]
    [[ "$output" != *"OTA"* ]]
}

@test "the name gate still refuses the mod tree itself as a payload root" {
    # Blast-radius re-check under auto-arming: pointing --payload-root at the
    # mod's git tree root (not our payload dir inside it) must be refused by
    # the name gate - the exemption never licenses an unnamed directory.
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT="$MOD_ROOT"
    mod_payload_autodetect
    [ "$HELIX_MOD_PAYLOAD" = "1" ] || fail "setup: autodetect did not arm"
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    run mod_payload_mode_block
    [ "$status" -ne 0 ] || fail "the mod tree itself was accepted as a payload root"
    [[ "$output" == *"Refusing"* ]]
}

@test "parse_installer_args: the override flags and their deprecated aliases" {
    parse_installer_args --standalone --auto-update
    [ "$STANDALONE_INSTALL" = "1" ] || fail "--standalone not captured"
    [ "$HELIX_MOD_PAYLOAD_UPDATES" = "1" ] || fail "--auto-update not captured"

    parse_installer_args --payload-root /usr/data/helixscreen
    [ "$MOD_PAYLOAD_ROOT" = "/usr/data/helixscreen" ] \
        || fail "--payload-root not captured: '$MOD_PAYLOAD_ROOT'"

    # Old spellings still parse (courtesy aliases, pre-release surface) and
    # say so. Direct calls with redirected output - run/$() would swallow the
    # variable side effects the assertions read.
    parse_installer_args --no-mod-payload > "$BATS_TEST_TMPDIR/parse1.out" 2>&1
    [ "$STANDALONE_INSTALL" = "1" ] || fail "--no-mod-payload alias not applied"
    grep -q "deprecated" "$BATS_TEST_TMPDIR/parse1.out"

    parse_installer_args --mod-payload-root /usr/data/helixscreen --mod-payload-updates \
        > "$BATS_TEST_TMPDIR/parse2.out" 2>&1
    [ "$MOD_PAYLOAD_ROOT" = "/usr/data/helixscreen" ] || fail "--mod-payload-root alias not applied"
    [ "$HELIX_MOD_PAYLOAD_UPDATES" = "1" ] || fail "--mod-payload-updates alias not applied"
    grep -q "deprecated" "$BATS_TEST_TMPDIR/parse2.out"

    # The old opt-in flag is a compat no-op: accepted, never an error.
    run parse_installer_args --mod-payload
    [ "$status" -eq 0 ]
}

@test "parse_installer_args: --payload-root cannot be combined with --standalone" {
    run parse_installer_args --standalone --payload-root /usr/data/helixscreen
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot be combined with --standalone"* ]]
}

@test "the bundled install.sh carries the override arms and the env scrub" {
    # The generated bundle is what users curl|sh; regeneration must carry the
    # parser arms (new spellings and their aliases) and the source-time scrub.
    local bundle="$WORKTREE_ROOT/scripts/install.sh"
    grep -q -- '--standalone)' "$bundle"
    grep -q -- '--payload-root)' "$bundle"
    grep -q -- '--auto-update)' "$bundle"
    grep -q -- '--no-mod-payload)' "$bundle"
    grep -q '^HELIX_MOD_PAYLOAD=""$' "$bundle"
}

# ============================================================================
# Carry-item 4: detect_tmp_dir never auto-stages inside the mod tree
# ============================================================================

@test "detect_tmp_dir: the install-sibling candidate never lands inside the mod tree" {
    HELIX_MOD_PAYLOAD=1
    INSTALL_DIR="$HOST_INSTALL_ROOT"
    mkdir -p "$MOD_ROOT/.bin"
    export HOME="$BATS_TEST_TMPDIR/home"
    mkdir -p "$HOME"
    TMP_DIR=""

    detect_tmp_dir

    [ -n "$TMP_DIR" ] || fail "no scratch dir was selected"
    case "$TMP_DIR" in
        "$MOD_ROOT"|"$MOD_ROOT"/*)
            fail "staged the scratch dir inside the mod tree: $TMP_DIR" ;;
    esac
}

@test "detect_tmp_dir: a mod-owned TMP_DIR_PREFERRED is dropped, not staged into" {
    HELIX_MOD_PAYLOAD=1
    TMP_DIR_PREFERRED="$MOD_ROOT/helixscreen-install"
    export HOME="$BATS_TEST_TMPDIR/home"
    mkdir -p "$HOME" "$SANDBOX/opt"
    INSTALL_DIR="$SANDBOX/opt/helixscreen"
    TMP_DIR=""

    detect_tmp_dir

    [ -n "$TMP_DIR" ] || fail "no scratch dir was selected"
    [ "$TMP_DIR" != "$TMP_DIR_PREFERRED" ] \
        || fail "staged into the mod-owned TMP_DIR_PREFERRED"
    case "$TMP_DIR" in
        "$MOD_ROOT"|"$MOD_ROOT"/*)
            fail "staged the scratch dir inside the mod tree: $TMP_DIR" ;;
    esac
}

# ============================================================================
# Carry-item 5: the LOG_LEVEL migration is keyed on the host capability
# ============================================================================

@test "a mod-host operator env is never LOG_LEVEL-migrated, even when the payload ships no env" {
    # The normal-path extract at an operator-chosen root on a mod host: the
    # archive ships no env, so PAYLOAD_ENV_PRESERVED stays 0 - the migration
    # must still not fire, because the HOST is mod-managed.
    HOST_SERVICE_MECHANISM="mod-managed"
    HELIX_MOD_PAYLOAD=""
    INSTALL_DIR="$SANDBOX/usr/data/helixscreen"
    mkdir -p "$INSTALL_DIR/bin" "$INSTALL_DIR/config"
    create_fake_mips_elf "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    printf 'HELIX_LOG_LEVEL=info\n' > "$INSTALL_DIR/config/helixscreen.env"
    create_payload_tarball no-env

    extract_release "ad5x"

    grep -q '^HELIX_LOG_LEVEL=info$' "$INSTALL_DIR/config/helixscreen.env" \
        || fail "the one-time migration rewrote the operator env on a mod host"
}

# ============================================================================
# Controls: the plain-host behavior each gate must not disturb
# ============================================================================

@test "plain host: extract_release keeps the atomic swap contract" {
    HOST_MOD_ROOT=""
    HOST_SERVICE_MECHANISM="systemd"
    HELIX_MOD_PAYLOAD=""
    INSTALL_DIR="$SANDBOX/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/bin"
    create_fake_mips_elf "$INSTALL_DIR/bin/helix-screen"
    create_payload_tarball

    extract_release "ad5x"

    # The swap path replaced the root wholesale - the control for (a): on a
    # plain host nothing preserves an unlisted platform/ dir.
    [ -x "$INSTALL_DIR/bin/helix-screen" ]
    [ -d "${INSTALL_DIR}.old" ] || fail "the atomic swap did not run"
}

@test "plain host: the LOG_LEVEL migration still applies" {
    HOST_MOD_ROOT=""
    HOST_SERVICE_MECHANISM="systemd"
    HELIX_MOD_PAYLOAD=""
    INSTALL_DIR="$SANDBOX/opt/helixscreen"
    mkdir -p "$INSTALL_DIR/bin" "$INSTALL_DIR/config"
    create_fake_mips_elf "$INSTALL_DIR/bin/helix-screen"
    printf 'HELIX_LOG_LEVEL=info\n' > "$INSTALL_DIR/config/helixscreen.env"
    create_payload_tarball

    extract_release "ad5x"

    grep -q '^#HELIX_LOG_LEVEL=info' "$INSTALL_DIR/config/helixscreen.env" \
        || fail "the migration no longer applies on a plain host"
}

# --- the payload root is recorded for the uninstaller ---
#
# --payload-root can land the payload outside the probed default (the
# OTA-durable seam), so the install must leave a note of where it actually
# went -- otherwise an armed uninstall later removes the probed default while
# the real payload sits where the operator put it.

@test "payload install records its root beside the display-mode record" {
    HELIX_MOD_PAYLOAD=1
    INSTALL_DIR="$MOD_ROOT/.bin/helixscreen"
    mkdir -p "$SANDBOX/usr/data/config/mod_data"   # the mod's data dir, present on any real host

    mod_payload_mode_block >/dev/null 2>&1

    local record="$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root"
    [ -f "$record" ] || fail "no payload-root record was written"
    [ "$(cat "$record")" = "$INSTALL_DIR" ] \
        || fail "record says $(cat "$record"), install went to $INSTALL_DIR"
}

@test "a self-managed install writes no payload-root record" {
    # Only the payload contract claims a payload root; a standalone install
    # on the same host must not leave a stale pointer for a later armed
    # uninstall to chase.
    HELIX_MOD_PAYLOAD=""
    STANDALONE_INSTALL=1
    INSTALL_DIR="$SANDBOX/opt/helixscreen"

    mod_payload_mode_block >/dev/null 2>&1

    [ ! -e "$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root" ] \
        || fail "non-payload install wrote a payload-root record"
}

# --- R2a/R2b: every uninstall entry point resolves the payload root the same way ---
#
# install.sh --uninstall auto-arms on a mod host and swept via
# helix_install_dirs_for_run, which resolved from probe/flag only — a
# custom-root install removed the probed default there while the recorded
# root (and its -repo) survived, and the record went stale. The sweep and the
# standalone arm now share one resolver (flag > recorded root > probed
# default) whose name gate refuses anything that is not recognisably ours.

@test "the run's sweep list carries the recorded payload root, not the probed default" {
    HELIX_MOD_PAYLOAD=1
    local custom="$SANDBOX/usr/data/helixscreen"
    mkdir -p "$custom" "$SANDBOX/usr/data/config/mod_data"
    printf '%s\n' "$custom" > "$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root"

    local dirs
    dirs=$(helix_install_dirs_for_run)

    case " $dirs " in
        *" $custom "*) ;;
        *) fail "the recorded payload root is not in the sweep list";;
    esac
    case " $dirs " in
        *" $INSTALL_DIR "*) fail "the probed default is swept instead of the recorded root";;
    esac
}

@test "a payload root that fails the name gate never enters the sweep list" {
    # The sweep's rm -rf must never see a root that is not recognisably ours,
    # whichever of the three tiers produced it.
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT="$SANDBOX/usr/data"
    mkdir -p "$SANDBOX/usr/data"

    local dirs
    dirs=$(helix_install_dirs_for_run 2>/dev/null)

    case " $dirs " in
        *" $SANDBOX/usr/data "*) fail "an ungated root entered the sweep list";;
    esac
    [ -d "$SANDBOX/usr/data" ]
}

@test "uninstall() resolves the payload root before sweeping and consumes the record after" {
    local body
    body=$(awk '/^uninstall\(\) \{/{c=1} c{print} c&&/^\}/{exit}' \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" | sed 's/#.*//')
    [ -n "$body" ] || fail "uninstall() not found"

    local resolve_line sweep_line consume_line
    resolve_line=$(grep -n 'resolve_payload_root' <<< "$body" | head -1 | cut -d: -f1)
    sweep_line=$(grep -n 'helix_install_dirs_for_run' <<< "$body" | head -1 | cut -d: -f1)
    consume_line=$(grep -n 'host_payload_root_record' <<< "$body" | head -1 | cut -d: -f1)
    [ -n "$resolve_line" ] || fail "uninstall() never resolves the payload root"
    [ -n "$sweep_line" ]  || fail "uninstall() never sweeps"
    [ -n "$consume_line" ] || fail "uninstall() never consumes the payload-root record"
    [ "$resolve_line" -lt "$sweep_line" ] \
        || fail "resolution must precede the sweep (a refusal cannot come after removals)"
    [ "$sweep_line" -lt "$consume_line" ] \
        || fail "the record must be consumed after the sweep removed the root it named"
}

@test "uninstall()'s forgex restore is flavor-gated like the standalone arm" {
    # Coherence across entry points: the takeover ran for forge_x only, so a
    # Z-Mod payload install must be untouched by uninstall() here too, not
    # just by the bundle's arm.
    local body
    body=$(awk '/^uninstall\(\) \{/{c=1} c{print} c&&/^\}/{exit}' \
        "$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh" | sed 's/#.*//')
    [ -n "$body" ] || fail "uninstall() not found"
    grep -q 'uninstall_forgex' <<< "$body" || fail "uninstall() never restores the display"
    grep -q '"${AD5M_FIRMWARE:-}" = "forge_x"' <<< "$body" \
        || fail "the forgex restore is not flavor-gated"
}

# --- R3: --clean is not a terminating removal - it must not orphan the record ---
#
# install.sh --clean runs mod_payload_mode_block FIRST (which records the
# resolved payload root), then clean_old_installation sweeps, then CONTINUES
# into a fresh install that never re-records. Consuming the record on the
# clean step (round 2's addition) left the fresh payload unrecorded, so a
# later FLAGLESS armed uninstall swept the probed default instead - R2a's
# exact shape via a different path. The record is consumed only by
# terminating removals: uninstall() and the standalone arm.

@test "an armed --clean --payload-root does not orphan the record a later flagless uninstall needs" {

    # An rm-neutral SUDO shim: clean_old_installation also rm -f's real /etc
    # paths (polkit/udev rules) a test user cannot touch and that are
    # irrelevant to the record lifecycle under test; everything else passes
    # through so the sandbox writes happen for real.
    SUDO="$BATS_TEST_TMPDIR/sudo-rm-neutral"
    cat > "$SUDO" <<SHIM
#!/bin/sh
if [ "\$1" = "rm" ]; then
    case " \$* " in
        *"$SANDBOX"/*) exec rm "\$@" ;;
        *) exit 0 ;;
    esac
fi
exec "\$@"
SHIM
    chmod +x "$SUDO"
    export SUDO

    ASSUME_YES=true
    HELIX_MOD_PAYLOAD=1
    STANDALONE_INSTALL=""
    local custom="$SANDBOX/usr/data/helixscreen-custom"
    mkdir -p "$custom/bin" "$SANDBOX/usr/data/config/mod_data"
    printf '#!/bin/sh\n' > "$custom/bin/helix-screen"
    MOD_PAYLOAD_ROOT="$custom"
    INSTALL_DIR="$custom"

    # install.sh main() order: the mode block records, then the clean runs.
    mod_payload_mode_block >/dev/null 2>&1
    [ "$(cat "$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root")" = "$custom" ] \
        || fail "setup: the mode block did not record the payload root"

    clean_old_installation ad5x >/dev/null 2>&1

    # --clean continues into a fresh install at the same root (no re-record).
    mkdir -p "$custom/bin"
    printf '#!/bin/sh\n' > "$custom/bin/helix-screen"

    # Later, a FLAGLESS armed run: set_install_paths resolved the probed
    # default, so only the record can still answer CUSTOM.
    MOD_PAYLOAD_ROOT=""
    HOST_PAYLOAD_ROOT=""
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    local dirs
    dirs=$(helix_install_dirs_for_run)

    case " $dirs " in
        *" $custom "*) ;;
        *) fail "the recorded root is not resolved after an armed --clean";;
    esac
    case " $dirs " in
        *" $HOST_INSTALL_ROOT "*) fail "the sweep fell back to the probed default";;
    esac
}

@test "a plain armed --clean leaves the record naming the root the fresh install populates" {

    # An rm-neutral SUDO shim: clean_old_installation also rm -f's real /etc
    # paths (polkit/udev rules) a test user cannot touch and that are
    # irrelevant to the record lifecycle under test; everything else passes
    # through so the sandbox writes happen for real.
    SUDO="$BATS_TEST_TMPDIR/sudo-rm-neutral"
    cat > "$SUDO" <<SHIM
#!/bin/sh
if [ "\$1" = "rm" ]; then
    case " \$* " in
        *"$SANDBOX"/*) exec rm "\$@" ;;
        *) exit 0 ;;
    esac
fi
exec "\$@"
SHIM
    chmod +x "$SUDO"
    export SUDO

    ASSUME_YES=true
    HELIX_MOD_PAYLOAD=1
    mkdir -p "$SANDBOX/usr/data/config/mod_data"
    INSTALL_DIR="$HOST_INSTALL_ROOT"
    mkdir -p "$INSTALL_DIR"

    mod_payload_mode_block >/dev/null 2>&1
    clean_old_installation ad5x >/dev/null 2>&1

    local record="$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root"
    [ -f "$record" ] || fail "the clean step consumed the record the fresh install relies on"
    [ "$(cat "$record")" = "$INSTALL_DIR" ] \
        || fail "the record names $(cat "$record"), the fresh install populates $INSTALL_DIR"
}

# --- A1: the payload auto-arm is scoped to the verified host shape ---
#
# Both Forge-X layouts carry the mod's Buildroot chroot beside its tree (each
# board's own DATA_MNT: /usr/data on the AD5X, /data on the AD5M), so the arm
# gate - tree AND chroot - now covers both rigs. What it still excludes is a
# tree WITHOUT its chroot: a mod mid-install or half-removed, whose payload
# root nothing can run. That shape keeps its pre-payload contract: no
# auto-arm, no relocation to the mod tree, the platform's own root and
# service stand. The payload contract there is explicit-only (--payload-root).

@test "a half-installed mod host (tree, no chroot) does not auto-arm the payload contract" {
    export HELIX_MOD_TREE_CANDIDATES="$MOD_ROOT"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/usr/data/.mod/.forge-x"   # never created
    # Drop the setup's hand-pinned answers so the assertions read the PROBE's
    # output, not stale pins (the probe owns these answers).
    unset HOST_MOD_ROOT HOST_MOD_CHROOT HOST_CHROOT_STATE HOST_SERVICE_MECHANISM \
          HOST_INSTALL_ROOT HOST_CONFIG_DIR HOST_MOONRAKER_USER_CONF \
          HOST_PLATFORM_HOOK_KEY HOST_OWNS_COMPETING_UIS
    host_profile_probe
    [ -n "$HOST_MOD_ROOT" ] || fail "setup: the mod tree was not recognized"
    [ "$HOST_CHROOT_STATE" = "none" ] || fail "setup: fixture is not the chroot-less shape"

    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT=""
    mod_payload_autodetect

    [ -z "$HELIX_MOD_PAYLOAD" ] \
        || fail "auto-armed the payload contract on a chroot-less mod host"
    [ -z "$HOST_INSTALL_ROOT" ] \
        || fail "the probe claimed a payload root - a bare install would relocate to it"
}

@test "a genuinely-AD5M Forge-X host auto-arms the payload contract" {
    # The AD5M block of the mod's descriptor: MOD_ROOT=/opt/config/mod,
    # chroot /data/.mod/.forge-x, armv7l. A bare install there is a payload
    # install exactly as on the AD5X rig (Task 10): ff5m is one mod, the
    # AD5M/AD5X split was our verification boundary, not a real one.
    mkdir -p "$SANDBOX/opt/config/mod/.shell" "$SANDBOX/data/.mod/.forge-x/usr/bin"
    touch "$SANDBOX/opt/config/mod/.shell/platform.sh"
    export HELIX_MOD_TREE_CANDIDATES="$SANDBOX/opt/config/mod"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/data/.mod/.forge-x"
    uname() { echo armv7l; }
    export -f uname
    unset HOST_MOD_ROOT HOST_MOD_CHROOT HOST_CHROOT_STATE HOST_SERVICE_MECHANISM \
          HOST_INSTALL_ROOT HOST_CONFIG_DIR HOST_MOONRAKER_USER_CONF \
          HOST_PLATFORM_HOOK_KEY HOST_OWNS_COMPETING_UIS
    host_profile_probe
    [ -n "$HOST_MOD_ROOT" ] || fail "setup: the AD5M mod tree was not recognized"
    [ "$HOST_MOD_ROOT" = "$SANDBOX/opt/config/mod" ] \
        || fail "setup: HOST_MOD_ROOT='$HOST_MOD_ROOT'"
    [ "$HOST_PLATFORM_HOOK_KEY" = "ad5m-forgex" ] \
        || fail "setup: an AD5M rig must carry the ad5m payload key"

    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT=""
    mod_payload_autodetect

    [ "$HELIX_MOD_PAYLOAD" = "1" ] \
        || fail "did not auto-arm on the AD5M Forge-X shape"
}

# --- Task 10: the legacy AD5M population gets adopt-or-warn, never silence ---
#
# Before the payload contract, our installer put ad5m+forge_x hosts at
# /opt/helixscreen with an S90helixscreen service. A payload install that
# silently relocated to the mod's default root would strand that install
# (A1's no-silent-conversion rule), so the armed install offers to ADOPT the
# legacy root - a payload root outside the mod's git tree, which is also the
# OTA-durable answer - and a declined or unanswerable offer proceeds at the
# mod default with the exact manual migration commands. The installer never
# deletes the legacy root or touches its service: the commands are printed
# for the operator.

# The legacy half of the genuinely-AD5M rig state, pinned like the setup's
# other HOST_* answers (the probe owns the real ones; these tests exercise
# the mode block's policy on top of them).
seed_legacy_install() {
    HOST_LEGACY_INSTALL_ROOT="$SANDBOX/opt/helixscreen"
    HOST_LEGACY_INIT_SCRIPT="$SANDBOX/etc/init.d/S90helixscreen"
    mkdir -p "$HOST_LEGACY_INSTALL_ROOT/bin" "$SANDBOX/etc/init.d"
    printf '#!/bin/sh\nexec /opt/helixscreen/bin/helix-launcher.sh\n' \
        > "$HOST_LEGACY_INIT_SCRIPT"
    chmod +x "$HOST_LEGACY_INIT_SCRIPT"
    printf 'LEGACY\n' > "$HOST_LEGACY_INSTALL_ROOT/bin/helix-screen"
}

@test "legacy adopt: an accepted offer lands the payload at the legacy root, service untouched" {
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    STANDALONE_INSTALL=""
    uninstall_mode=false
    seed_legacy_install
    # The operator answers the offer with y. The prompt's contract is exactly
    # "return 0 = adopt"; pinning it here drives the adopt arm without a pty
    # (the decline arm below drives the REAL prompt over bats' piped stdin).
    payload_legacy_prompt_adopt() { return 0; }

    mod_payload_mode_block

    [ "$INSTALL_DIR" = "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR' - the adopt did not take"
    # The adopt is recorded: a later armed uninstall resolves this root, and
    # the next install resumes it instead of re-offering.
    [ "$(cat "$(host_payload_root_record)")" = "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "the record names '$(cat "$(host_payload_root_record)")', not the adopted root"
    # The standalone service is the operator's to remove: the command is
    # printed, never executed - and never even de-exec'd by us.
    [ -x "$HOST_LEGACY_INIT_SCRIPT" ] \
        || fail "the installer disabled the standalone service itself"
    [ -f "$HOST_LEGACY_INSTALL_ROOT/bin/helix-screen" ] \
        || fail "the installer deleted the legacy root itself"
    # The adopt arm's own copy: reset the run state (the first call moved
    # INSTALL_DIR and wrote the record; either alone would steer the re-run
    # away from the offer) so this is the fresh offer again, not the resume.
    INSTALL_DIR="$HOST_INSTALL_ROOT"
    rm -f "$(host_payload_root_record)"
    run mod_payload_mode_block
    # The service is KEPT: the mod's own service starts only the mod's tree
    # (their .shell/helixscreen.sh: HELIX_ROOT=$MOD_ROOT/.bin/helixscreen)
    # and the payload contract installs none, so the legacy S90 script is the
    # ONE boot path an adopted root has (M1). Telling the operator to rm it
    # here would kill the UI at the next boot.
    grep -q "Keeping the standalone service $HOST_LEGACY_INIT_SCRIPT" <<< "$output" \
        || fail "the adopt does not say the service is kept"
    ! grep -q "rm $HOST_LEGACY_INIT_SCRIPT" <<< "$output" \
        || fail "the adopt still tells the operator to remove the boot path"
}

@test "legacy decline: the payload stays at the mod default and the warning names both roots" {
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    STANDALONE_INSTALL=""
    uninstall_mode=false
    seed_legacy_install
    # No prompt override: bats runs with piped stdin, so the REAL prompt must
    # decline - the curl|sh path this offer has to survive.

    mod_payload_mode_block

    [ "$INSTALL_DIR" = "$HOST_INSTALL_ROOT" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR' - a declined offer must keep the mod default"
    [ -d "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "the legacy root was deleted on the way past"
    [ -x "$HOST_LEGACY_INIT_SCRIPT" ] \
        || fail "the standalone service was touched without an adopt"

    run mod_payload_mode_block
    grep -q "$HOST_LEGACY_INSTALL_ROOT" <<< "$output" \
        || fail "the warning does not name the legacy root"
    grep -q "$HOST_INSTALL_ROOT" <<< "$output" \
        || fail "the warning does not name the mod default root"
    grep -q "rm $HOST_LEGACY_INIT_SCRIPT" <<< "$output" \
        || fail "no command to remove the standalone service"
    grep -q "rm -rf $HOST_LEGACY_INSTALL_ROOT" <<< "$output" \
        || fail "no command to remove the legacy root"
    grep -q -- "--payload-root $HOST_LEGACY_INSTALL_ROOT" <<< "$output" \
        || fail "no explicit-adopt escape hatch named"
}

@test "legacy resume: a recorded adopt re-lands at the legacy root without asking" {
    # An adopt is a choice the record persists. Re-offering on every update
    # would nag an operator who already decided; re-locating silently is what
    # A1 forbids. Resuming the recorded root is neither.
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    STANDALONE_INSTALL=""
    uninstall_mode=false
    seed_legacy_install
    mkdir -p "$(host_mod_data)"
    printf '%s\n' "$HOST_LEGACY_INSTALL_ROOT" > "$(host_payload_root_record)"
    # The prompt would decline; the record must bypass the offer entirely.
    payload_legacy_prompt_adopt() { return 1; }

    mod_payload_mode_block

    [ "$INSTALL_DIR" = "$HOST_LEGACY_INSTALL_ROOT" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR' - the recorded adopt was not resumed"
}

@test "legacy adopt: the armed uninstall names the now-stale standalone service" {
    # Adoption KEEPS the legacy service because it is the adopted root's only
    # boot path (M1) - so the armed uninstall that removes that root is where
    # the service genuinely becomes stale, and the uninstall says so, naming
    # the exact script. Removal stays the operator's: we never touch the
    # service ourselves in either direction.
    HELIX_MOD_PAYLOAD=1
    seed_legacy_install
    INSTALL_DIR="$HOST_LEGACY_INSTALL_ROOT"
    mkdir -p "$INSTALL_DIR/bin" "$(host_mod_data)"
    printf '%s\n' "$HOST_LEGACY_INSTALL_ROOT" > "$(host_payload_root_record)"

    run uninstall_mod_payload

    [ "$status" -eq 0 ] || fail "the armed uninstall failed: $output"
    [ ! -d "$INSTALL_DIR" ] \
        || fail "the adopted root survived its armed uninstall"
    grep -q "rm $HOST_LEGACY_INIT_SCRIPT" <<< "$output" \
        || fail "the uninstall does not name the now-stale service for removal"
    [ -x "$HOST_LEGACY_INIT_SCRIPT" ] \
        || fail "the uninstall removed the service itself"
}

@test "legacy control: an armed uninstall at the mod default leaves the service unmentioned" {
    # The mod's own service boots the mod-default payload root, and the legacy
    # service still boots the legacy install - neither is stale, so neither is
    # named. The note belongs to the adopted root's uninstall alone.
    HELIX_MOD_PAYLOAD=1
    seed_legacy_install
    INSTALL_DIR="$HOST_INSTALL_ROOT"
    mkdir -p "$INSTALL_DIR/bin" "$(host_mod_data)"
    printf '%s\n' "$HOST_INSTALL_ROOT" > "$(host_payload_root_record)"

    run uninstall_mod_payload

    [ "$status" -eq 0 ] || fail "the armed uninstall failed: $output"
    ! grep -q "$HOST_LEGACY_INIT_SCRIPT" <<< "$output" \
        || fail "named the legacy service where nothing made it stale"
    [ -x "$HOST_LEGACY_INIT_SCRIPT" ]
}

@test "no legacy install: the payload run keeps today's behavior" {
    # Control: with no legacy root probed, the mode block neither warns nor
    # strays from the mod default - the AD5X rig's plain path, unchanged.
    HELIX_MOD_PAYLOAD=1
    MOD_PAYLOAD_ROOT=""
    STANDALONE_INSTALL=""
    uninstall_mode=false
    HOST_LEGACY_INSTALL_ROOT=""
    HOST_LEGACY_INIT_SCRIPT=""

    mod_payload_mode_block
    [ "$INSTALL_DIR" = "$HOST_INSTALL_ROOT" ] \
        || fail "INSTALL_DIR='$INSTALL_DIR' - moved without a legacy root to move to"

    run mod_payload_mode_block
    ! grep -q "older standalone" <<< "$output" \
        || fail "warned about a legacy install that is not there"
}

@test "an AD5X-shape mod host (tree with chroot) still auto-arms" {
    export HELIX_MOD_TREE_CANDIDATES="$MOD_ROOT"
    export HELIX_MOD_CHROOT_CANDIDATES="$SANDBOX/usr/data/.mod/.forge-x"
    mkdir -p "$SANDBOX/usr/data/.mod/.forge-x/usr/bin"
    unset HOST_MOD_ROOT HOST_MOD_CHROOT HOST_CHROOT_STATE HOST_SERVICE_MECHANISM \
          HOST_INSTALL_ROOT HOST_CONFIG_DIR HOST_MOONRAKER_USER_CONF \
          HOST_PLATFORM_HOOK_KEY HOST_OWNS_COMPETING_UIS
    host_profile_probe
    [ "$HOST_CHROOT_STATE" != "none" ] || fail "setup: the chroot was not found"

    STANDALONE_INSTALL=""
    MOD_PAYLOAD_ROOT=""
    mod_payload_autodetect

    [ "$HELIX_MOD_PAYLOAD" = "1" ] \
        || fail "did not auto-arm on the verified host shape"
}

# --- A3: --clean must sweep the RECORDED root, not just this run's ---
#
# The mode block re-records THIS run's root before the clean sweep resolves,
# so a custom root recorded by the previous install was never swept - the
# full payload (and its --auto-update stanza) survived a mode whose contract
# is "remove old installation completely".

@test "--clean sweeps the previously recorded custom root, not just the re-recorded default" {
    SUDO="$BATS_TEST_TMPDIR/sudo-rm-neutral"
    cat > "$SUDO" <<SHIM
#!/bin/sh
if [ "\$1" = "rm" ]; then
    case " \$* " in
        *"$SANDBOX"/*) exec rm "\$@" ;;
        *) exit 0 ;;
    esac
fi
exec "\$@"
SHIM
    chmod +x "$SUDO"
    export SUDO
    ASSUME_YES=true
    HELIX_MOD_PAYLOAD=1
    local custom="$SANDBOX/usr/data/helixscreen-custom"
    mkdir -p "$custom/bin" "$custom-repo" \
             "$SANDBOX/usr/data/config/mod_data" "$HOST_INSTALL_ROOT"
    printf '#!/bin/sh\n' > "$custom/bin/helix-screen"
    printf 'clone\n' > "$custom-repo/HEAD"
    printf '%s\n' "$custom" > "$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root"
    printf '[authorization]\n[update_manager helixscreen]\ntype: web\n' \
        > "$HOST_MOONRAKER_USER_CONF"
    INSTALL_DIR="$HOST_INSTALL_ROOT"

    mod_payload_mode_block >/dev/null 2>&1
    [ "$(cat "$SANDBOX/usr/data/config/mod_data/helixscreen_payload_root")" = "$HOST_INSTALL_ROOT" ] \
        || fail "setup: the mode block did not re-record this run's root"

    clean_old_installation ad5x >/dev/null 2>&1

    [ ! -d "$custom" ] \
        || fail "the previously recorded custom root survived --clean"
    [ ! -d "$custom-repo" ] \
        || fail "the previously recorded root's updater clone survived --clean"
    ! grep -q 'update_manager helixscreen' "$HOST_MOONRAKER_USER_CONF" \
        || fail "the custom root's --auto-update stanza survived --clean"
}

# --- A4 ruling: --auto-update never arms at a mod-owned payload root ---
#
# Moonraker's type:web updater REPLACES the whole root on update, which would
# destroy the config//platform preservation the payload contract exists to
# provide. Refused loudly with the root named and the durable-root escape
# pointed at; a payload root OUTSIDE the mod tree (OD1's durable shape) still
# gets the stanza (the positive control is the reworked test above).

@test "--auto-update is refused while the payload root is inside the mod's tree" {
    mkdir -p "$INSTALL_DIR/bin" "$SANDBOX/usr/data/config/mod_data"
    printf '[authorization]\n' > "$HOST_MOONRAKER_USER_CONF"
    HELIX_MOD_PAYLOAD=1
    HELIX_MOD_PAYLOAD_UPDATES=1

    run configure_moonraker_updates "ad5x"
    [ "$status" -eq 0 ] || fail "the refusal aborted the install: $output"

    ! grep -q 'update_manager helixscreen' "$HOST_MOONRAKER_USER_CONF" \
        || fail "the stanza was armed at a mod-owned root"
    case "$output" in
        *"$INSTALL_DIR"*) ;;
        *) fail "the refusal does not name the payload root";;
    esac
    case "$output" in
        *--payload-root*) ;;
        *) fail "the refusal does not point at the durable-root escape";;
    esac
}

# --- The --help copy states the AD5M truth ---
#
# Task 10 arms the AD5M Forge-X shape, so the line that told operators "On an
# AD5M Forge-X host the payload contract is not auto-detected" becomes a lie
# the moment it lands. The copy must say both rigs auto-detect and that a
# legacy standalone install is offered adoption, not silent relocation.

@test "--help: the payload root copy covers both rigs and the adopt offer" {
    run usage

    [ "$status" -eq 0 ] || fail "usage failed: $output"
    case "$output" in
        *"AD5X and AD5M alike"*) ;;
        *) fail "--help does not say the payload contract covers both rigs";;
    esac
    case "$output" in
        *"not auto-detected"*)
            fail "--help still claims the contract is not auto-detected on the AD5M";;
    esac
}

@test "payload-mode success epilogue does not coach a service we did not install" {
    # Found on hardware (AD5X cycle 2026-09-01): the generic epilogue printed
    # /etc/init.d/S80helixscreen restart on a payload install, where no service
    # exists by design. The epilogue must name the mod's lifecycle instead.
    seed_payload_root
    run print_post_install_commands "mod-managed"
    [ "$status" -eq 0 ]
    [[ "$output" == *"firmware mod starts the UI"* ]]
    [[ "$output" != *"/etc/init.d/"* ]]
    [[ "$output" == *"${INSTALL_DIR}/logs/launcher.log"* ]]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# --mod-payload mode end to end (lib/installer).
#
# On a host whose firmware is a mod with its own git tree (Forge-X / Z-Mod),
# --mod-payload is the only install contract: the mod owns the UI service and
# the OTA, so the installer replaces the payload root's CONTENTS in place
# (never mv/rm -rf of the root), preserves config/ and platform/, writes no
# service files, and leaves every Moonraker conf alone unless
# --mod-payload-updates opted in to a stanza in the mod's user.moonraker.conf.
#
# Covers the brief's five mode properties (a)-(e) plus the six carry-items
# routed here from earlier task reviews:
#   1. start_service short-circuits on HOST_SERVICE_MECHANISM, not the flag
#   2. a normal install on a mod host warns it will not be started
#   3. HELIX_MOD_PAYLOAD is settable only by the flag, never the environment
#   4. detect_tmp_dir never auto-stages inside the mod tree
#   5. the LOG_LEVEL migration is keyed on the host capability, not on
#      PAYLOAD_ENV_PRESERVED
#   6. the user.moonraker.conf stanza write is gated on --mod-payload-updates

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim
    install_gnu_stat_shim

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

# forgex.sh with its /opt/config layout rewritten onto the AD5X host-side
# layout the probe answers for (same rewrite pattern as
# test_forgex_display_modes.bats). Nothing under test may reach the real
# /opt/config tree.
source_forgex_patched() {
    local patched="$BATS_TEST_TMPDIR/forgex.sh"
    sed -e "s|/opt/config/|$SANDBOX/usr/data/config/|g" \
        "$WORKTREE_ROOT/scripts/lib/installer/forgex.sh" > "$patched"
    refute_sh "grep '/opt/config' '$patched'"
    unset _HELIX_FORGEX_SOURCED
    # shellcheck disable=SC1090
    . "$patched"
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

@test "payload install: a fresh payload root (--mod-payload-root outside the tree) is populated" {
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

@test "payload install: --mod-payload-updates writes the stanza into user.moonraker.conf only" {
    mkdir -p "$SANDBOX/usr/data/config/mod_data" "$INSTALL_DIR/bin"
    create_fake_mips_elf "$INSTALL_DIR/bin/helix-screen"
    chmod +x "$INSTALL_DIR/bin/helix-screen"
    printf '[server]\n' > "$MOD_ROOT/moonraker.conf"
    cp "$MOD_ROOT/moonraker.conf" "$BATS_TEST_TMPDIR/mod-conf.original"
    printf '[authorization]\n' > "$HOST_MOONRAKER_USER_CONF"
    HELIX_MOD_PAYLOAD=1
    HELIX_MOD_PAYLOAD_UPDATES=1

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

@test "payload uninstall: honors the run's --mod-payload-root, not the probed mod root" {
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

@test "plain uninstall: a mod-owned payload root without the flag is skipped, not removed" {
    # Coherence with Task 1's skip-not-exit sweeps: only the flag-armed run
    # may remove the payload root.
    HELIX_MOD_PAYLOAD=""
    seed_payload_root
    AD5M_FIRMWARE=""
    HELIX_INIT_SCRIPTS="$SANDBOX/nonexistent/helixscreen-init"
    detect_init_system() { INIT_SYSTEM="sysv"; }
    export -f detect_init_system
    kill_process_by_name() { return 1; }
    export -f kill_process_by_name

    run uninstall "ad5x"
    [ "$status" -eq 0 ]
    [ -d "$INSTALL_DIR" ] || fail "a plain uninstall removed the mod's payload root"
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

@test "mode block: --mod-payload-root outranks the probed mod root, no OTA warning outside the tree" {
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
    [[ "$output" == *"--mod-payload-root"* ]]
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

@test "a normal install on a mod host warns the mod owns the UI service" {
    HELIX_MOD_PAYLOAD=""
    INSTALL_DIR="$SANDBOX/usr/data/helixscreen"

    run mod_payload_mode_block
    [ "$status" -eq 0 ]
    [[ "$output" == *"owns the UI service"* ]]
    [[ "$output" == *"not be started automatically"* ]]
    [[ "$output" == *"--mod-payload"* ]]
    # It is a warning, not a refusal: the install proceeds.
    [[ "$output" != *"refusing"* ]]
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

@test "parse_installer_args: --mod-payload arms the mode and captures its sub-flags" {
    parse_installer_args --mod-payload
    [ "$HELIX_MOD_PAYLOAD" = "1" ] || fail "flag did not arm the mode"

    parse_installer_args --mod-payload --mod-payload-root /usr/data/helixscreen --mod-payload-updates
    [ "$MOD_PAYLOAD_ROOT" = "/usr/data/helixscreen" ] \
        || fail "MOD_PAYLOAD_ROOT='$MOD_PAYLOAD_ROOT'"
    [ "$HELIX_MOD_PAYLOAD_UPDATES" = "1" ] \
        || fail "HELIX_MOD_PAYLOAD_UPDATES='$HELIX_MOD_PAYLOAD_UPDATES'"
}

@test "parse_installer_args: the payload sub-flags require --mod-payload" {
    run parse_installer_args --mod-payload-root /usr/data/helixscreen
    [ "$status" -ne 0 ]
    [[ "$output" == *"requires --mod-payload"* ]]

    run parse_installer_args --mod-payload-updates
    [ "$status" -ne 0 ]
    [[ "$output" == *"requires --mod-payload"* ]]
}

@test "the bundled install.sh carries the --mod-payload arms and the env scrub" {
    # The generated bundle is what users curl|sh; regeneration must carry the
    # parser arms and the source-time scrub forward.
    local bundle="$WORKTREE_ROOT/scripts/install.sh"
    grep -q -- '--mod-payload)' "$bundle"
    grep -q -- '--mod-payload-root)' "$bundle"
    grep -q -- '--mod-payload-updates)' "$bundle"
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

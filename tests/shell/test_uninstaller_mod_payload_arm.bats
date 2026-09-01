#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The STANDALONE uninstaller's --mod-payload arm.
#
# bundle-uninstaller.sh's main() used to have no way to undo a payload-contract
# install: its sweeps refuse mod-owned paths by design, so an operator who
# installed with the (auto-detected) payload contract could not uninstall --
# the shipped uninstaller exited at the mod-owned refusal and left the payload
# subtree, the display takeover and the optional user.moonraker.conf stanza all
# in place. The arm makes that removal an explicit opt-in:
#   sh uninstall.sh --mod-payload
# arms the same mod-owned exemption install.sh uses and runs the payload-root
# uninstall: display-mode restore FIRST (while the payload is still in place),
# then the stanza, then the payload subtree.
#
# The tests drive the GENERATED scripts/uninstall.sh, because the arm's value
# is reachability: a lib function nothing calls is the exact dead-code shape
# test_uninstall_platform_restore_wiring.bats was written to catch.
#
# No `set +e` here, deliberately: bats detects a failing assertion through its
# ERR discipline, and disarming it makes every test in the file vacuously
# green (a mutation that restored the display mode under an unarmed run once
# sailed through exactly that way). The bundle ships `set -e`, which is safe
# because every call below is run-wrapped or || fail-guarded.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    # Source the bundled uninstaller (its trailing case guard skips main).
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED \
          _HELIX_PLATFORM_SOURCED _HELIX_PERMISSIONS_SOURCED \
          _HELIX_REQUIREMENTS_SOURCED _HELIX_FORGEX_SOURCED \
          _HELIX_SERVICE_SOURCED _HELIX_MOONRAKER_SOURCED \
          _HELIX_CAMERA_SOURCED _HELIX_UNINSTALL_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/uninstall.sh"

    # An AD5X-shaped mod host, post payload install:
    #   - payload root (.bin/helixscreen) inside the mod tree
    #   - display mode taken over (HEADLESS) with the arrival recorded (STOCK)
    #   - an --auto-update stanza in the mod's user.moonraker.conf
    #
    # HOST_* are set AFTER sourcing: host_profile.sh declares them blank at
    # source time, so a pre-source assignment is silently wiped.
    SANDBOX="$BATS_TEST_TMPDIR/root"
    MOD_ROOT="$SANDBOX/usr/data/config/mod"
    MOD_DATA="$SANDBOX/usr/data/config/mod_data"
    PAYLOAD_ROOT="$MOD_ROOT/.bin/helixscreen"
    USER_CONF="$MOD_DATA/user.moonraker.conf"

    mkdir -p "$MOD_ROOT/.shell" \
             "$MOD_ROOT/.root" \
             "$PAYLOAD_ROOT/bin" \
             "$PAYLOAD_ROOT/config" \
             "$MOD_DATA"
    touch "$MOD_ROOT/.shell/platform.sh"
    printf '#!/bin/sh\n' > "$PAYLOAD_ROOT/bin/helix-screen"
    printf '{}\n' > "$PAYLOAD_ROOT/config/settings.json"

    printf "[Variables]\ndisplay = 'HEADLESS'\n" > "$MOD_DATA/variables.cfg"
    printf 'STOCK\n' > "$MOD_DATA/helixscreen_prev_display"

    cat > "$USER_CONF" <<'EOF'
# Managed by the mod
[include /usr/data/config/mod/moonraker.conf]

# HelixScreen auto-update stanza (written by the installer's --auto-update)
[update_manager helixscreen]
type: web
path: /usr/data/config/mod/.bin/helixscreen

[server]
host: 0.0.0.0
EOF

    SUDO=""
    export SUDO
    HOST_MOD_ROOT="$MOD_ROOT"
    HOST_MOONRAKER_USER_CONF="$USER_CONF"
    INSTALL_DIR="$PAYLOAD_ROOT"
    HELIX_MOD_PAYLOAD=""
    # The fixture is a forge-x mod host: the takeover ran under that flavor,
    # and the arm's display restore is flavor-gated to match.
    AD5M_FIRMWARE=forge_x
}

@test "the uninstaller defines the payload uninstall" {
    type uninstall_mod_payload >/dev/null 2>&1 \
        || fail "scripts/uninstall.sh has no uninstall_mod_payload"
}

@test "armed run restores the mod display mode and removes the payload root" {
    HELIX_MOD_PAYLOAD=1

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "armed run failed: $output"

    # The display mode went back to the recorded arrival mode...
    grep -q "^display = 'STOCK'$" "$MOD_DATA/variables.cfg" \
        || fail "display mode not restored to STOCK"
    # ...and the record is consumed.
    [ ! -e "$MOD_DATA/helixscreen_prev_display" ] \
        || fail "restore record survived the armed run"
    # The payload subtree itself is gone.
    [ ! -d "$PAYLOAD_ROOT" ] || fail "payload root still present"
}

@test "armed run drops the auto-update stanza but keeps the rest of the user conf" {
    HELIX_MOD_PAYLOAD=1

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "armed run failed: $output"

    ! grep -q 'update_manager helixscreen' "$USER_CONF" \
        || fail "stanza survived the payload uninstall"
    grep -q '^\[include /usr/data/config/mod/moonraker.conf\]$' "$USER_CONF" \
        || fail "the mod's own include was collateral damage"
    grep -q '^\[server\]$' "$USER_CONF" \
        || fail "an unrelated section was dropped"
}

@test "unarmed run leaves the mod tree alone" {
    # Without the arm the mod-owned refusal must stand: nothing the arm does
    # may happen as a side effect of a plain uninstall run.
    HELIX_MOD_PAYLOAD=""

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "unarmed run should no-op cleanly, got: $output"

    [ -d "$PAYLOAD_ROOT" ] || fail "payload root removed by an unarmed run"
    grep -q "^display = 'HEADLESS'$" "$MOD_DATA/variables.cfg" \
        || fail "display mode touched by an unarmed run"
    grep -q 'update_manager helixscreen' "$USER_CONF" \
        || fail "user conf touched by an unarmed run"
}

# A $SUDO shim that records its arguments and runs the real command, so a
# test can count how many times a privileged write actually ran.
make_sudo_shim() {
    SUDO="$BATS_TEST_TMPDIR/sudo-shim"
    cat > "$SUDO" <<EOF
#!/bin/sh
echo "\$*" >> "$BATS_TEST_TMPDIR/sudo.log"
exec "\$@"
EOF
    chmod +x "$SUDO"
    export SUDO
}

# --- F1: the display restore runs exactly once across the stacked callers ---
#
# The armed uninstall stacks THREE uninstall_forgex callers on a forge_x host:
# the payload arm, restore_previous_ui_platform, and reenable_previous_ui's
# own forge_x branch. The second call used to find the restore record already
# consumed, fall back to GUPPY, and rewrite a still-HEADLESS rig to GUPPY --
# the HEADLESS-arrival defect reintroduced on the armed path.

@test "an armed uninstall restores the display exactly once across the stacked callers" {
    # HEADLESS arrival: record says HEADLESS, cfg says HEADLESS. The damaging
    # shape -- run 1 "restores" HEADLESS->HEADLESS (cfg still reads HEADLESS),
    # so run 2's GUPPY fallback fires and rewrites it.
    printf 'HEADLESS\n' > "$MOD_DATA/helixscreen_prev_display"
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=forge_x
    PREVIOUS_UI_SCRIPT=""
    PREVIOUS_UIS=""
    platform=ad5x
    make_sudo_shim

    uninstall_mod_payload
    reenable_previous_ui

    # The rig keeps the mode it arrived on...
    grep -q "^display = 'HEADLESS'$" "$MOD_DATA/variables.cfg" \
        || fail "stacked callers rewrote a HEADLESS rig to $(sed -n "s/^display = '\(.*\)'/\1/p" "$MOD_DATA/variables.cfg")"
    # ...and the restore itself ran exactly once (one sed on variables.cfg).
    [ "$(grep -c "sed -i .*variables.cfg" "$BATS_TEST_TMPDIR/sudo.log")" -eq 1 ] \
        || fail "display restore ran $(grep -c "sed -i .*variables.cfg" "$BATS_TEST_TMPDIR/sudo.log") times"
    [ ! -d "$PAYLOAD_ROOT" ] || fail "payload root survived the armed run"
    case "${restored_ui:-}" in
        *GuppyScreen*) fail "a HEADLESS arrival was reported as GuppyScreen restored";;
    esac
}

@test "an armed uninstall honors the recorded mode across the stacked callers" {
    # STOCK arrival: run 1 restores STOCK; the stacked runs 2 and 3 must not
    # touch it again (they cannot turn it into GUPPY today, but the honor the
    # record invariant is the contract this pins).
    grep -q "^display = 'HEADLESS'$" "$MOD_DATA/variables.cfg" \
        || fail "fixture setup failed: cfg should still be HEADLESS"
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=forge_x
    PREVIOUS_UI_SCRIPT=""
    PREVIOUS_UIS=""
    platform=ad5x
    make_sudo_shim

    uninstall_mod_payload
    reenable_previous_ui

    grep -q "^display = 'STOCK'$" "$MOD_DATA/variables.cfg" \
        || fail "recorded STOCK was not honored across the stacked callers"
    [ "$(grep -c "sed -i .*variables.cfg" "$BATS_TEST_TMPDIR/sudo.log")" -eq 1 ]
}

@test "the arm restores the display only for the flavor the takeover targeted" {
    # Gating coherence: the takeover (configure_platform) runs the forgex
    # display takeover only for forge_x, so the arm's restore must key on the
    # same flavor -- not on the module merely being present. A Z-Mod payload
    # install never took the display over.
    printf 'STOCK\n' > "$MOD_DATA/helixscreen_prev_display"
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=zmod

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "arm failed: $output"

    grep -q "^display = 'HEADLESS'$" "$MOD_DATA/variables.cfg" \
        || fail "display mode touched on a non-forge_x flavor"
    [ -e "$MOD_DATA/helixscreen_prev_display" ] \
        || fail "restore record consumed on a non-forge_x flavor"
    [ ! -d "$PAYLOAD_ROOT" ] || fail "payload root not removed"
}

# --- F2: the payload root is where the operator put it ---
#
# --payload-root lets an install land outside the probed default (the
# OTA-durable seam), so an armed uninstall must remove THAT root: the flag on
# this run, else the root the install recorded in mod_data, else the probed
# default.

make_custom_root() {
    CUSTOM_ROOT="$SANDBOX/usr/data/helixscreen"
    mkdir -p "$CUSTOM_ROOT/bin" "$CUSTOM_ROOT-repo"
    printf '#!/bin/sh\n' > "$CUSTOM_ROOT/bin/helix-screen"
    printf 'clone\n' > "$CUSTOM_ROOT-repo/HEAD"
}

@test "an armed uninstall with --payload-root removes exactly the custom root" {
    make_custom_root
    MOD_PAYLOAD_ROOT="$CUSTOM_ROOT"
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=forge_x

    # Direct call: the arm repoints INSTALL_DIR at the resolved root, and a
    # run-wrapped call would mutate only the subshell's copy.
    uninstall_mod_payload

    [ ! -d "$CUSTOM_ROOT" ] || fail "the named payload root survived"
    [ ! -d "$CUSTOM_ROOT-repo" ] || fail "the named root's updater clone survived"
    [ -d "$PAYLOAD_ROOT" ] || fail "the probed default root was removed instead"
    # The resolved root becomes this run's one install target, so the generic
    # sweeps that follow the arm agree with it.
    [ "$INSTALL_DIR" = "$CUSTOM_ROOT" ]
}

@test "an armed uninstall without the flag removes the install-time recorded root" {
    make_custom_root
    printf '%s\n' "$CUSTOM_ROOT" > "$MOD_DATA/helixscreen_payload_root"
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=forge_x

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "armed run failed: $output"

    [ ! -d "$CUSTOM_ROOT" ] || fail "the recorded payload root survived"
    [ -d "$PAYLOAD_ROOT" ] || fail "the probed default root was removed instead"
    # The record is consumed with the root it named.
    [ ! -e "$MOD_DATA/helixscreen_payload_root" ] \
        || fail "payload-root record survived the removal it directed"
}

@test "an armed uninstall with neither flag nor record falls back to the probed default" {
    # The original contract, now the third tier of the resolution.
    make_custom_root   # a decoy root nothing points at
    HELIX_MOD_PAYLOAD=1
    AD5M_FIRMWARE=forge_x

    run uninstall_mod_payload
    [ "$status" -eq 0 ] || fail "armed run failed: $output"

    [ ! -d "$PAYLOAD_ROOT" ] || fail "probed default not removed"
    [ -d "$CUSTOM_ROOT" ] || fail "an unrecorded decoy root was removed"
}

@test "the armed run restores the display mode before removing the payload" {
    # Ordering contract: the restore reads the record and the mod's files    # while they still exist. A run that removed the payload first would find
    # no record and strand the printer on HEADLESS -- no UI at all after an
    # uninstall. The record file is the witness: only a restore-first run can
    # consume it. Direct call (not run): the stub must set a variable in this
    # shell, which a run subshell would discard.
    HELIX_MOD_PAYLOAD=1
    _consumed_record_during_run=""

    uninstall_forgex() {
        if [ -f "$MOD_DATA/helixscreen_prev_display" ]; then
            _consumed_record_during_run=yes
        fi
        return 0
    }

    uninstall_mod_payload

    [ "${_consumed_record_during_run:-}" = "yes" ] \
        || fail "uninstall_forgex ran after the payload root was removed (or not at all)"
}

# --- bundle main() wiring: the arm must be reachable from the shipped CLI ---

_bundle_main_body() {
    # main()'s own body with comments stripped, so a comment naming the arm
    # cannot satisfy the grep (the same trap the platform-restore wiring hit).
    awk '/^main\(\) \{/{c=1} c{print} c&&/^\}/{exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh" | sed 's/#.*//'
}

@test "bundle main() accepts a --mod-payload flag" {
    local body
    body=$(_bundle_main_body)
    [ -n "$body" ] || fail "main() not found in the bundle"
    grep -q -- '--mod-payload' <<< "$body" \
        || fail "main() does not parse --mod-payload"
}

@test "bundle main() accepts a --payload-root path" {
    # The uninstall-side half of the same flag the installer takes: an
    # operator who installed to a custom root needs to name it again.
    local body
    body=$(_bundle_main_body)
    [ -n "$body" ] || fail "main() not found in the bundle"
    grep -q -- '--payload-root' <<< "$body" \
        || fail "main() does not parse --payload-root"
}

@test "bundle main() detects the mod flavor for ad5x, like the installer does" {
    # main.sh runs the unified flavor detector for BOTH Adventurer platforms
    # (a Forge-X AD5X is no longer misread as the ZMOD layout). The bundle
    # used to run it for ad5m only, which left AD5M_FIRMWARE empty on the
    # AD5X rig -- every forge_x branch in the uninstaller, including the
    # payload arm's display restore, was dead there.
    local body
    body=$(_bundle_main_body)
    [ -n "$body" ] || fail "main() not found in the bundle"
    grep -q 'detect_mod_flavor' <<< "$body" \
        || fail "main() does not run the unified mod-flavor detector"
}

@test "bundle main() routes the arm through uninstall_mod_payload" {
    local body
    body=$(_bundle_main_body)
    [ -n "$body" ] || fail "main() not found in the bundle"
    grep -q 'uninstall_mod_payload' <<< "$body" \
        || fail "main() never calls uninstall_mod_payload -- the arm is dead code"
}

@test "the bundle help text documents the arm" {
    grep -q -- '--mod-payload' "$WORKTREE_ROOT/scripts/uninstall.sh"
}

# --- R2b: a root that is not recognisably ours is refused, never removed ---
#
# The arm's resolved root used to get none of install's name gating, so
# --payload-root /usr/data (or a corrupted record — the tee write is
# non-atomic) could rm -rf an arbitrary existing directory. Every tier of the
# resolution now passes the same last-component-helixscreen gate install
# roots pass; a refusal names the offending source and touches nothing.

@test "--payload-root naming a directory that is not ours refuses the uninstall" {
    mkdir -p "$SANDBOX/usr/data"
    MOD_PAYLOAD_ROOT="$SANDBOX/usr/data"
    HELIX_MOD_PAYLOAD=1

    run uninstall_mod_payload
    [ "$status" -ne 0 ] || fail "acted on a --payload-root that is not ours"
    case "$output" in
        *payload-root*) ;;
        *) fail "the refusal does not name the flag as the source";;
    esac
    [ -d "$SANDBOX/usr/data" ] || fail "the named directory was removed"
    [ -d "$PAYLOAD_ROOT" ] || fail "a refused run still removed the probed default"
    grep -q "^display = 'HEADLESS'$" "$MOD_DATA/variables.cfg" \
        || fail "a refused run still touched the display mode"
}

@test "a corrupted record naming a directory that is not ours refuses and names the record" {
    mkdir -p "$SANDBOX/usr/data"
    printf '%s\n' "$SANDBOX/usr/data" > "$MOD_DATA/helixscreen_payload_root"
    HELIX_MOD_PAYLOAD=1

    run uninstall_mod_payload
    [ "$status" -ne 0 ] || fail "acted on a corrupted payload-root record"
    case "$output" in
        *helixscreen_payload_root*) ;;
        *) fail "the refusal does not name the record file";;
    esac
    [ -d "$SANDBOX/usr/data" ] || fail "the named directory was removed"
    [ -d "$PAYLOAD_ROOT" ] || fail "a refused run still removed the probed default"
}

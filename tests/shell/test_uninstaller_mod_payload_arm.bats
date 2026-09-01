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

@test "the armed run restores the display mode before removing the payload" {
    # Ordering contract: the restore reads the record and the mod's files
    # while they still exist. A run that removed the payload first would find
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

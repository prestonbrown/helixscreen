#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The platform-specific restores must be REACHABLE from the shipped uninstaller.
#
# The two uninstall entry points had diverged. `install.sh --uninstall` reaches them
# fine — main.sh calls uninstall "$platform". But the STANDALONE uninstall.sh, which
# is what ships into the install dir and what users actually run, is built by
# bundle-uninstaller.sh, which appends its own reenable_previous_ui() and a main()
# calling stop_helixscreen / remove_service / remove_installation /
# reenable_previous_ui — never uninstall(). So every platform branch living only in
# uninstall() was unreachable from the uninstaller users invoke.
#
# test_cc1_uninstall.bats covers whether the COSMOS restore block does the right
# thing, by extracting that block textually and sourcing it. That proves the block's
# logic and nothing about whether anything calls it, so it stayed green throughout.
#
# Observed on a real Snapmaker U1 (PAXX 1.4.1-paxx12-19): the shipped uninstall.sh
# --force printed "Uninstall Complete!" while leaving /usr/bin/gui non-executable,
# both .stock launchers unrestored, and /oem/.debug in place — no bootable stock UI
# and the firmware's overlay-wipe-on-boot permanently disabled.
#
# So this test drives the GENERATED scripts/uninstall.sh through the same entry
# point main() uses. Re-orphaning the platform restores fails it.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    export MOCK_ROOT="$BATS_TEST_TMPDIR/u1"
    mkdir -p "$MOCK_ROOT/etc/init.d" "$MOCK_ROOT/usr/bin" "$MOCK_ROOT/oem"

    # Post-install U1 state, as snapmaker-u1-setup-autostart.sh leaves it:
    # debug flag set, stock UI binary neutralized, launchers taken over with the
    # originals parked alongside as .stock.
    touch "$MOCK_ROOT/oem/.debug"
    printf '#!/bin/sh\necho stock gui\n' > "$MOCK_ROOT/usr/bin/gui"
    chmod a-x "$MOCK_ROOT/usr/bin/gui"
    printf '#!/bin/sh\n# HelixScreen delegate\n' > "$MOCK_ROOT/etc/init.d/S99fb-http"
    chmod +x "$MOCK_ROOT/etc/init.d/S99fb-http"
    printf '#!/bin/sh\n# STOCK fb-http\n' > "$MOCK_ROOT/etc/init.d/S99fb-http.stock"
    printf '#!/bin/sh\n# STOCK input-event-daemon\n' > "$MOCK_ROOT/etc/init.d/S99input-event-daemon.stock"

    # Redirect the bundle's absolute host paths into MOCK_ROOT, and drop the
    # trailing `case "${0##*/}" in uninstall.sh) main "$@" ;;` dispatch so sourcing
    # only defines functions.
    BUNDLE="$BATS_TEST_TMPDIR/bundle_under_test.sh"
    sed -e "s|/etc/init\.d|$MOCK_ROOT/etc/init.d|g" \
        -e "s|/usr/bin/gui|$MOCK_ROOT/usr/bin/gui|g" \
        -e "s|/oem/\.debug|$MOCK_ROOT/oem/.debug|g" \
        "$WORKTREE_ROOT/scripts/uninstall.sh" \
        | sed '/^case "\${0##\*\/}" in$/,+2d' > "$BUNDLE"
    export BUNDLE

    # Safety net: nothing under test may touch the real host paths.
    ! grep -qE '(^|[^A-Za-z0-9_.-])/oem/\.debug' "$BUNDLE"
    ! grep -qE '(^|[^A-Za-z0-9_.-])/usr/bin/gui' "$BUNDLE"
}

# Drive the shipped entry point, not an extracted copy.
_run_reenable() {
    bash -c '
        log_info(){ echo "INFO: $*"; }
        log_warn(){ echo "WARN: $*"; }
        log_success(){ echo "OK: $*"; }
        log_error(){ echo "ERR: $*"; }
        . "$BUNDLE" 2>/dev/null
        platform=snapmaker-u1
        SUDO=""; AD5M_FIRMWARE=""; PREVIOUS_UI_SCRIPT=""; PREVIOUS_UIS=""
        reenable_previous_ui
    '
}

@test "uninstall wiring: reenable_previous_ui reaches the platform restores" {
    # The regression itself. Scoped to reenable_previous_ui's own body on purpose:
    # a file-wide grep also matches the function DEFINITION, so it would stay green
    # against exactly the dead-code state this exists to catch.
    # Comment lines are stripped before matching: the block carries a comment
    # naming restore_previous_ui_platform(), which otherwise satisfies the grep
    # even when the actual call is gone (verified — it did).
    local body
    body=$(awk '/^reenable_previous_ui\(\) \{/{c=1} c{print} c&&/^\}/{exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh" | sed 's/#.*//')
    [ -n "$body" ]
    grep -q 'restore_previous_ui_platform' <<< "$body"

    # main() must call reenable_previous_ui — the other half of the chain.
    awk '/^main\(\) \{/{c=1} c{print} c&&/^\}/{exit}' \
        "$WORKTREE_ROOT/scripts/uninstall.sh" | grep -q 'reenable_previous_ui'

    run _run_reenable
    [ "$status" -eq 0 ]
}

@test "uninstall u1: re-enables the stock UI binary" {
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
    run _run_reenable
    [ "$status" -eq 0 ]
    # Without this the printer has no bootable UI at all after an uninstall.
    [ -x "$MOCK_ROOT/usr/bin/gui" ]
}

@test "uninstall u1: removes /oem/.debug so the overlay wipe resumes" {
    [ -f "$MOCK_ROOT/oem/.debug" ]
    run _run_reenable
    [ "$status" -eq 0 ]
    [ ! -f "$MOCK_ROOT/oem/.debug" ]
}

@test "uninstall u1: restores both stock boot launchers from .stock" {
    run _run_reenable
    [ "$status" -eq 0 ]
    for l in S99fb-http S99input-event-daemon; do
        [ -f "$MOCK_ROOT/etc/init.d/$l" ]
        [ ! -f "$MOCK_ROOT/etc/init.d/$l.stock" ]
        grep -q "STOCK" "$MOCK_ROOT/etc/init.d/$l"
    done
    refute grep -q "HelixScreen delegate" "$MOCK_ROOT/etc/init.d/S99fb-http"
}

@test "uninstall u1: reports the restored UI to the caller" {
    run _run_reenable
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Snapmaker stock UI"
    # A restore must suppress the misleading "no previous UI" message that the
    # real device printed while restoring nothing.
    refute grep -q "No previous screen UI found" <<< "$output"
}

@test "uninstall: a platform with no specific restore still falls through to scanning" {
    # The platform restore must not swallow the generic path for platforms it
    # does not know — otherwise fixing U1 breaks GuppyScreen/KlipperScreen boxes.
    run bash -c '
        log_info(){ echo "INFO: $*"; }
        log_warn(){ echo "WARN: $*"; }
        log_success(){ echo "OK: $*"; }
        log_error(){ echo "ERR: $*"; }
        . "$BUNDLE" 2>/dev/null
        platform=pi
        SUDO=""; AD5M_FIRMWARE=""; PREVIOUS_UI_SCRIPT=""; PREVIOUS_UIS=""
        reenable_previous_ui
    '
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "No previous screen UI found"
    # And it must leave the U1 fixtures untouched.
    [ -f "$MOCK_ROOT/oem/.debug" ]
    [ ! -x "$MOCK_ROOT/usr/bin/gui" ]
}

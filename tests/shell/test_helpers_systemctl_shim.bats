#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The default systemctl shadow in helpers.bash (the comment block on
# install_systemctl_shim states why it must be automatic, not opt-in).
# These pin its contract: shadowing lands on load, the shim is inert, a
# test's own mock still overrides it, and the escape hatch restores the
# host binary. The host systemctl itself is never executed here - each
# invocation is guarded by proving the shim is what resolves first.

@test "loading helpers shadows systemctl on PATH" {
    load helpers
    [ "$(command -v systemctl)" = "$BATS_TEST_TMPDIR/bin/systemctl" ] \
        || fail "systemctl resolves to $(command -v systemctl), not the shim"
}

@test "the default shim is inert: systemctl calls succeed without effect" {
    load helpers
    # Guard BEFORE invoking: if the shadow is broken, running systemctl here
    # would be the one place a test interrogates the host's polkit.
    [ "$(command -v systemctl)" = "$BATS_TEST_TMPDIR/bin/systemctl" ] \
        || fail "shim not on PATH; refusing to invoke the real systemctl"
    run systemctl stop helixscreen-update.path
    [ "$status" -eq 0 ]
    run systemctl disable helixscreen-update.path
    [ "$status" -eq 0 ]
    run systemctl daemon-reload
    [ "$status" -eq 0 ]
}

@test "a test's own mock overrides the default shim" {
    load helpers
    mock_command_fail "systemctl"
    run systemctl stop helixscreen-update.path
    [ "$status" -ne 0 ] || fail "mock_command_fail lost to the default shim"
}

@test "HELIX_TEST_REAL_SYSTEMCTL=1 restores the host binary" {
    # load helpers for fail(); the hermetic subshells below bypass its PATH
    # changes via env -i, so loading it here costs nothing and keeps this
    # test's own failure messages real.
    load helpers
    # This shell already carries the shim on PATH from setup, so the opt-out
    # is proven from a hermetic shell with a controlled PATH.
    #
    # sandbox-gap-reviewed: the probes below ask where systemctl RESOLVES and
    # never run it, and a hermetic environment is the thing being tested, so
    # they are outside the suite sandbox on purpose.
    local probe='command -v systemctl'
    local source_and_probe='. "$1" >/dev/null 2>&1; command -v systemctl'
    local real resolved
    real=$(env -i PATH=/usr/bin:/bin bash -c "$probe" 2>/dev/null || true)
    [ -n "$real" ] || skip "no host systemctl to restore"

    resolved=$(env -i PATH=/usr/bin:/bin HELIX_TEST_REAL_SYSTEMCTL=1 \
        bash -c "$source_and_probe" _ "$BATS_TEST_DIRNAME/helpers.bash")
    [ "$resolved" = "$real" ] \
        || fail "opt-out resolved to $resolved, want the host $real"

    # Without the opt-out, the same hermetic shell gets the shim.
    resolved=$(env -i PATH=/usr/bin:/bin \
        bash -c "$source_and_probe" _ "$BATS_TEST_DIRNAME/helpers.bash")
    case "$resolved" in
        */bin/systemctl)
            [ "$resolved" != "$real" ] \
                || fail "default left the host systemctl on PATH" ;;
        *) fail "default resolved to $resolved, want the shim" ;;
    esac
}

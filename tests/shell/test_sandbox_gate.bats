#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Proves the host sandbox in setup_suite.bash is active and catches an escape
# across each boundary a test can cross.
#
# Every escape here is deliberate, so each one redirects the ledger to a private
# file. A blocked call recorded in the suite ledger fails the run, and these
# tests must not do that.

SANDBOX_COMMANDS_DOC="killall pkill pidof reboot shutdown halt poweroff telinit launchctl crontab mount umount diskutil mkfs addr2line"

setup() {
    load helpers
    PRIVATE_LEDGER="$BATS_TEST_TMPDIR/ledger.tsv"
    : > "$PRIVATE_LEDGER"
}

# Run a deliberate escape without touching the suite ledger.
escape() {
    HELIX_TEST_SANDBOX_LEDGER="$PRIVATE_LEDGER" "$@"
}

@test "sandbox: the suite installed a shim directory and a ledger" {
    [ -n "$HELIX_TEST_SANDBOX_BIN" ]
    [ -d "$HELIX_TEST_SANDBOX_BIN" ]
    [ -n "$HELIX_TEST_SANDBOX_LEDGER" ]
    case ":$PATH:" in
        *":$HELIX_TEST_SANDBOX_BIN:"*) ;;
        *) fail "sandbox bin is not on PATH: $PATH" ;;
    esac
}

@test "sandbox: every documented command has a shim on PATH" {
    local cmd
    for cmd in $SANDBOX_COMMANDS_DOC; do
        [ -x "$HELIX_TEST_SANDBOX_BIN/$cmd" ] || fail "no shim for $cmd"
    done
}

@test "sandbox: the shim list in setup_suite matches this test's copy" {
    local declared
    declared=$(grep '^HELIX_SANDBOX_COMMANDS=' "$BATS_TEST_DIRNAME/setup_suite.bash" \
        | cut -d'"' -f2)
    [ "$declared" = "$SANDBOX_COMMANDS_DOC" ] \
        || fail "setup_suite lists [$declared], this test lists [$SANDBOX_COMMANDS_DOC]"
}

# --- the deliberate escapes -------------------------------------------------

@test "sandbox: a bare killall is blocked and recorded" {
    run escape killall helix-screen
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape from a non-bash callee is blocked by the PATH shim" {
    # sh does not import bash's exported functions, so only PATH covers this.
    cat > "$BATS_TEST_TMPDIR/escape.sh" <<'ESC'
killall helix-screen
ESC
    run escape sh "$BATS_TEST_TMPDIR/escape.sh"
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: an escape survives a callee that REPLACES PATH" {
    # The exported function layer is what holds here: the shim directory is gone
    # from PATH, which is what a script hardening PATH with the stock system
    # directories first also does.
    run escape env PATH="/usr/bin:/bin" bash -c 'killall helix-screen'
    [ "$status" -ne 0 ]
    grep -q 'killall helix-screen' "$PRIVATE_LEDGER"
}

@test "sandbox: pidof is blocked, so name-based process lookup finds nothing" {
    run escape pidof helix-screen
    [ "$status" -ne 0 ]
    grep -q 'pidof helix-screen' "$PRIVATE_LEDGER"
}

# --- the sandbox must not get in a legitimate test's way --------------------

@test "sandbox: a test's own mock wins and is never recorded" {
    mock_command_script "killall" 'exit 0'
    run escape killall helix-screen
    [ "$status" -eq 0 ]
    [ ! -s "$PRIVATE_LEDGER" ]
}

@test "sandbox: kill on a pid the test owns is not intercepted" {
    sleep 30 &
    local pid=$!
    run kill "$pid"
    [ "$status" -eq 0 ]
    wait "$pid" 2>/dev/null || true
    [ ! -s "$PRIVATE_LEDGER" ]
}

# --- the residual gap, pinned shut ------------------------------------------

@test "sandbox: nothing lands in the gap between the two layers" {
    # The PATH layer is lost at a callee that REPLACES PATH instead of
    # prepending to it. The function layer is lost where bash functions do not
    # travel: a non-bash callee, or a scrubbed environment. A line that does
    # both at once is the one shape neither layer covers.
    #
    # A reviewed exception writes the marker below inside the @test that needs
    # it, which exempts that test and nothing else.
    local offenders
    offenders=$(awk '
        FNR == 1 || /^@test/ { reviewed = 0 }
        /sandbox-gap-reviewed/ { reviewed = 1 }
        /(^|[^A-Za-z_])PATH=/ {
            if ($0 ~ /^[[:space:]]*#/) next
            if ($0 ~ /\$\{?PATH|HELIX_TEST_SANDBOX_BIN/) next
            if ($0 !~ /env -i|(^|[^-A-Za-z_\/])(sh|dash|busybox)[[:space:]]/) next
            if (reviewed) next
            printf "%s:%d:%s\n", FILENAME, FNR, $0
        }
    ' "$BATS_TEST_DIRNAME"/*.bats "$BATS_TEST_DIRNAME/helpers.bash")
    [ -z "$offenders" ] || fail "PATH replaced for a callee that also loses the function layer:
$offenders"
}

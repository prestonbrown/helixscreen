# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host sandbox for the whole bats suite.
#
# bats loads this file automatically for a directory run, before any test file.
# Nothing in a .bats file has to opt in, so a new test is sandboxed on the day
# it is written and a test that forgets to `load helpers` is covered anyway.
#
# What it blocks: commands that address the HOST by name rather than by a handle
# the test owns. `killall helix-screen` and `pidof dropbear` name a process this
# test did not start, so they reach whatever else is running on the machine -
# another bats file's app, or the developer's own. `kill "$pid"` is not in the
# list: a pid is a handle the test already holds, so it is scoped by
# construction. That distinction is the whole rule.
#
# A blocked call is recorded in the ledger with the test that made it and then
# fails; teardown_suite fails the run and prints the ledger. A test that needs
# one of these commands mocks it explicitly - mock_command* writes into
# $BATS_TEST_TMPDIR/bin, which is prepended later and therefore wins, and a
# shell function beats both. Mocked calls never reach the shim, so they never
# appear in the ledger.
#
# Two layers, because neither survives every boundary a test crosses:
#
#   PATH shims       survive a non-bash callee (installer scripts are #!/bin/sh,
#                    and sh does not import bash's exported functions), and are
#                    lost when a callee REPLACES PATH rather than prepending to
#                    it - `env PATH="$bin" ...`, or a script that hardens PATH
#                    by putting the stock system directories first.
#   Exported shell   survive PATH replacement and PATH hardening, and are lost
#   functions        at a non-bash callee.
#
# The union covers every boundary this suite actually crosses. The residual gap
# is a callee that is BOTH non-bash AND replaces PATH; test_sandbox_gate.bats
# pins that gap closed by failing any PATH assignment in tests/shell that drops
# the sandbox directory.

# Commands that address the host by name. Keep this list and the gate test's
# copy in step - the gate fails if they diverge.
HELIX_SANDBOX_COMMANDS="killall pkill pidof reboot shutdown halt poweroff telinit launchctl crontab mount umount diskutil mkfs addr2line"

setup_suite() {
    export HELIX_TEST_SANDBOX_BIN="$BATS_SUITE_TMPDIR/sandbox-bin"
    export HELIX_TEST_SANDBOX_LEDGER="$BATS_SUITE_TMPDIR/escapes.tsv"
    # "enforce" fails the blocked call; "permissive" lets it return success.
    # Neither ever runs the real command.
    export HELIX_TEST_SANDBOX_MODE="${HELIX_TEST_SANDBOX_MODE:-enforce}"

    mkdir -p "$HELIX_TEST_SANDBOX_BIN"
    : > "$HELIX_TEST_SANDBOX_LEDGER"

    local cmd
    for cmd in $HELIX_SANDBOX_COMMANDS; do
        # A single short printf append is atomic, which matters because bats
        # runs files in parallel and they share one ledger.
        cat > "$HELIX_TEST_SANDBOX_BIN/$cmd" <<SHIM
#!/bin/sh
printf '%s\t%s\t%s %s\n' \
    "\${BATS_TEST_FILENAME##*/}" "\${BATS_TEST_NAME:-(file setup)}" "$cmd" "\$*" \
    >> "\$HELIX_TEST_SANDBOX_LEDGER"
printf 'helix sandbox: blocked host-reaching command: %s %s\n' "$cmd" "\$*" >&2
[ "\$HELIX_TEST_SANDBOX_MODE" = permissive ] && exit 0
exit 1
SHIM
        chmod +x "$HELIX_TEST_SANDBOX_BIN/$cmd"
    done

    export PATH="$HELIX_TEST_SANDBOX_BIN:$PATH"

    # The function layer, which exists for the case where PATH no longer holds
    # the shim. bash resolves a function before PATH, so it would otherwise
    # outrank a test's own mock; the dispatcher hands control back whenever PATH
    # resolves to something under the bats run directory, which is where every
    # mock in this suite is written and nowhere a system binary can be.
    _helix_sandbox_dispatch() {
        local cmd="$1"; shift
        local resolved
        resolved=$(type -P "$cmd" 2>/dev/null) || resolved=""
        if [ -n "$resolved" ] && [ "$resolved" != "$HELIX_TEST_SANDBOX_BIN/$cmd" ]; then
            case "$resolved" in
                "$BATS_RUN_TMPDIR"/*) "$resolved" "$@"; return $? ;;
            esac
        fi
        "$HELIX_TEST_SANDBOX_BIN/$cmd" "$@"
    }
    export -f _helix_sandbox_dispatch

    for cmd in $HELIX_SANDBOX_COMMANDS; do
        eval "$cmd() { _helix_sandbox_dispatch $cmd \"\$@\"; }"
        # shellcheck disable=SC2163  # the name is the loop variable, by design
        export -f "$cmd"
    done

    # A runaway tool that chews the machine is the other way a test does damage,
    # and no command shim catches it. addr2line is in the list above because
    # resolving one address at a time against an unstripped binary exhausts
    # memory; a CPU-time cap is the portable backstop for the rest. An address
    # space cap is deliberately not set: allocators and sanitizers reserve far
    # more virtual memory than they use, so it fails builds without bounding
    # real usage.
    ulimit -t 900 2>/dev/null || true
}

teardown_suite() {
    [ -s "${HELIX_TEST_SANDBOX_LEDGER:-/nonexistent}" ] || return 0

    {
        echo "Tests reached outside the sandbox for a host-reaching command."
        echo "Each line is: test file, test name, blocked command."
        echo
        sort -u "$HELIX_TEST_SANDBOX_LEDGER" | sed 's/^/    /'
        echo
        echo "Mock the command in that test's setup() - mock_command_script"
        echo "\"killall\" 'exit 0' - or call it on a pid the test owns instead."
    } >&2

    [ "$HELIX_TEST_SANDBOX_MODE" = permissive ] && return 0
    return 1
}

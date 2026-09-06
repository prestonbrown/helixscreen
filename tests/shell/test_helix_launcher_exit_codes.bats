#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helix-launcher.sh exit-code handling and log timestamps.
#
# Two behaviours are covered:
#
#  1. Exit code 42 is helix-watchdog's RESTART_LOOP_EXIT_CODE — a deliberate
#     "stop, this will not fix itself" signal. _is_crash_exit() used to accept
#     any 1..127, so on dual-binary Pi builds a 42 triggered a whole second
#     watchdog run against the fbdev binary: the exact opposite of the
#     constant's intent.
#
#  2. Every launcher log line carries a wall-clock timestamp. On platforms
#     where /var/log is tmpfs, launcher.log is often the ONLY surviving record
#     after a reboot; without a timestamp its lines cannot be correlated to a
#     Klipper macro, a print, or anything else on the machine.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers

    mock_command_script "systemctl" 'exit 0'
    mock_command_script "killall" 'exit 0'
    mock_command_script "setterm" 'exit 0'

    export MOCK_INSTALL="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$MOCK_INSTALL/bin" "$MOCK_INSTALL/config"

    cp "$LAUNCHER" "$MOCK_INSTALL/bin/helix-launcher.sh"
}

# Extract _is_crash_exit() from the launcher and expose it to a POSIX sh probe,
# so the predicate is tested against the real source, not a copy.
_probe_is_crash_exit() {
    local code="$1"
    local probe="$BATS_TEST_TMPDIR/is_crash_probe.sh"
    {
        echo '#!/bin/sh'
        # sed range: the WATCHDOG_GIVE_UP_EXIT_CODE constant the predicate reads,
        # through the function's closing brace.
        sed -n '/^WATCHDOG_GIVE_UP_EXIT_CODE=/,/^}/p' "$LAUNCHER"
        echo 'if _is_crash_exit "$1"; then echo CRASH; else echo NOTCRASH; fi'
    } > "$probe"
    sh "$probe" "$code"
}

# Build a dual-binary (Pi-style) install: primary helix-screen + fbdev fallback
# + a watchdog. Watchdog exits with $1; each binary run appends to a log.
_install_dual_binary() {
    local watchdog_exit="$1"

    cat > "$MOCK_INSTALL/bin/helix-watchdog" <<EOF
#!/bin/sh
echo "watchdog \$*" >> "$MOCK_INSTALL/runs.txt"
exit ${watchdog_exit}
EOF
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_INSTALL/bin/helix-screen"
    printf '#!/bin/sh\nexit 0\n' > "$MOCK_INSTALL/bin/helix-screen-fbdev"
    chmod +x "$MOCK_INSTALL/bin/helix-watchdog" \
             "$MOCK_INSTALL/bin/helix-screen" \
             "$MOCK_INSTALL/bin/helix-screen-fbdev"
    rm -f "$MOCK_INSTALL/runs.txt"
}

_watchdog_runs() {
    [ -f "$MOCK_INSTALL/runs.txt" ] && wc -l < "$MOCK_INSTALL/runs.txt" | tr -d ' ' || echo 0
}

@test "launcher has valid sh syntax" {
    sh -n "$LAUNCHER"
}

@test "_is_crash_exit: 42 (watchdog restart-loop surrender) is NOT retryable" {
    run _probe_is_crash_exit 42
    [ "$output" = "NOTCRASH" ]
}

@test "_is_crash_exit: real crash signals still retryable" {
    for code in 134 136 138 139; do
        run _probe_is_crash_exit "$code"
        [ "$output" = "CRASH" ]
    done
}

@test "_is_crash_exit: ordinary non-signal failures still retryable" {
    for code in 1 3 127; do
        run _probe_is_crash_exit "$code"
        [ "$output" = "CRASH" ]
    done
}

@test "_is_crash_exit: clean and signal-terminated exits are not retryable" {
    # 0 = clean, 129/130/137/143 = HUP/INT/KILL/TERM
    for code in 0 129 130 137 143; do
        run _probe_is_crash_exit "$code"
        [ "$output" = "NOTCRASH" ]
    done
}

@test "watchdog exit 42 does not trigger the fbdev fallback run" {
    _install_dual_binary 42

    run env MOCK_INSTALL="$MOCK_INSTALL" sh "$MOCK_INSTALL/bin/helix-launcher.sh"

    # Exactly one watchdog invocation: the fallback must not have been tried.
    [ "$(_watchdog_runs)" -eq 1 ]
    # And 42 propagates out unchanged so the service manager sees it.
    [ "$status" -eq 42 ]
}

@test "watchdog exit 139 (SEGV) still triggers the fbdev fallback run" {
    # Guard against over-correcting: the fallback path must survive.
    _install_dual_binary 139

    run env MOCK_INSTALL="$MOCK_INSTALL" sh "$MOCK_INSTALL/bin/helix-launcher.sh"

    [ "$(_watchdog_runs)" -eq 2 ]
}

@test "every launcher log line carries a timestamp" {
    _install_dual_binary 0

    run env MOCK_INSTALL="$MOCK_INSTALL" sh "$MOCK_INSTALL/bin/helix-launcher.sh"

    # The launcher logs to stderr; bats folds it into $output.
    local launcher_lines
    launcher_lines=$(printf '%s\n' "$output" | grep 'helix-launcher' || true)
    [ -n "$launcher_lines" ]

    # Every one of them must start with a YYYY-MM-DD HH:MM:SS stamp.
    local untimestamped
    untimestamped=$(printf '%s\n' "$launcher_lines" \
        | grep -vE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[helix-launcher\]' \
        || true)
    if [ -n "$untimestamped" ]; then
        echo "untimestamped launcher log lines:" >&2
        printf '%s\n' "$untimestamped" >&2
        return 1
    fi
}

@test "log timestamp uses a BusyBox-compatible date format (no GNU-only flags)" {
    # `date -Iseconds`, `date --rfc-3339` and `date +%N` are absent from the
    # BusyBox date shipped on AD5M/K1/CC1/SonicPad. Check the executable body
    # of log() only — the surrounding comment names those flags on purpose.
    local body="$BATS_TEST_TMPDIR/log_body.sh"
    sed -n '/^log() {/,/^}/p' "$LAUNCHER" | grep -v '^[[:space:]]*#' > "$body"

    [ -s "$body" ]
    refute_grep 'date -I' "$body"
    refute_grep 'rfc-3339' "$body"
    refute_grep '%N' "$body"
    # And it must actually call date, not just claim to.
    grep -q 'date ' "$body"
}

@test "log() degrades gracefully when date is unavailable" {
    # Some minimal initramfs environments have no date binary at all; the
    # launcher must still log rather than die under `set -e`.
    local probe="$BATS_TEST_TMPDIR/log_probe.sh"
    {
        echo '#!/bin/sh'
        echo 'set -e'
        echo 'date() { return 127; }'
        sed -n '/^log() {/,/^}/p' "$LAUNCHER"
        echo 'log "hello world"'
        echo 'echo SURVIVED'
    } > "$probe"

    run sh "$probe"
    [ "$status" -eq 0 ]
    contains "hello world" "$output"
    [[ "$output" == *"SURVIVED"* ]]
}

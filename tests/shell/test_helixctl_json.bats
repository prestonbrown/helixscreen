#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CLI-level smoke coverage for `helix-screen ctl --json`.

bats_require_minimum_version 1.5.0

BIN="./build/bin/helix-screen"

setup_file() {
    # Mirrors require_binary() in test_headless_display.bats. The Shell Tests
    # (BATS) job checks out without submodules and never builds, so a hard
    # failure here would only report "the CI job doesn't compile C++", not
    # anything about ctl --json. Skipping keeps the file honest where a binary
    # exists (every dev machine, `make pi-test`) and quiet where one cannot.
    [ -x "$BIN" ] || skip "helix-screen not built (run 'make -j')"

    export SOCK="${BATS_FILE_TMPDIR}/helix-ctl.sock"
    export APP_LOG="${BATS_FILE_TMPDIR}/app.log"
    # Private config dir, mirroring tests/ui/helix/app.py. bats runs FILES in
    # parallel (--jobs), and the single-instance lock lives in the config dir —
    # so sharing the default one makes this file and test_headless_display.bats
    # race for it, and the loser dies with "Another instance is already running".
    export HELIX_CONFIG_DIR="${BATS_FILE_TMPDIR}/config"
    mkdir -p "$HELIX_CONFIG_DIR"
    # Headless: these cases assert on `ctl --json` output only, so nothing here
    # needs pixels — and any real video driver maps a "HelixScreen" window on
    # the developer's desktop for the whole file (wayland under a Wayland
    # session, x11 otherwise; the CI Shell Tests job never builds the binary,
    # so only desktop runs ever saw it). test_headless_display.bats drives the
    # same ctl surface under the dummy driver. An explicitly exported
    # SDL_VIDEODRIVER still wins.
    if [ -z "$SDL_VIDEODRIVER" ]; then
        export SDL_VIDEODRIVER=dummy
    fi
    "$BIN" --test --skip-wizard --skip-splash --remote --remote-socket "$SOCK" \
        >"$APP_LOG" 2>&1 &
    echo $! >"${BATS_FILE_TMPDIR}/app.pid"
    for _ in $(seq 1 100); do
        [ -S "$SOCK" ] && "$BIN" ctl -s "$SOCK" ping >/dev/null 2>&1 && return 0
        sleep 0.2
    done
    echo "app never became responsive; log:" >&2
    tail -20 "$APP_LOG" >&2
    kill "$(cat "${BATS_FILE_TMPDIR}/app.pid")" 2>/dev/null || true
    return 1
}

teardown_file() {
    "$BIN" ctl -s "$SOCK" shutdown >/dev/null 2>&1 || true
    local pid
    pid=$(cat "${BATS_FILE_TMPDIR}/app.pid" 2>/dev/null) || return 0
    for _ in $(seq 1 25); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.2
    done
    kill "$pid" 2>/dev/null || true
}

@test "--json emits the raw result and nothing else" {
    run "$BIN" ctl -s "$SOCK" --json ping
    [ "$status" -eq 0 ]
    [ "$output" = '"pong"' ]
}

@test "--json result is parseable and structured for object results" {
    run "$BIN" ctl -s "$SOCK" --json current
    [ "$status" -eq 0 ]
    echo "$output" | jq -e '.panel' >/dev/null
}

@test "--json emits one line, not pretty-printed" {
    run "$BIN" ctl -s "$SOCK" --json current
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | wc -l)" -eq 1 ]
}

@test "--json reports server errors as a JSON error object on stderr" {
    run --separate-stderr "$BIN" ctl -s "$SOCK" --json get nonexistent_subject_xyz
    [ "$status" -ne 0 ]
    echo "$stderr" | jq -e '.message' >/dev/null
    [ -z "$output" ]
}

@test "log stays line-oriented even under --json" {
    # The raw result is an object with a "lines" array; --json must not
    # pretty-print it, but it must still be valid JSON.
    run "$BIN" ctl -s "$SOCK" --json log -n 5
    [ "$status" -eq 0 ]
    echo "$output" | jq -e '.lines | type == "array"' >/dev/null
}

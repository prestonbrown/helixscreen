#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Headless smoke test: boot the real binary with no display server, drive a few
# panels through `helix-screen ctl`, capture a screenshot, and shut down cleanly.
#
# CI compiled the UI but never ran it — the whole runtime check was
# `helix-screen --help || true`, which cannot fail. A segfault on startup shipped
# green. This script is the missing check: it exercises the actual application
# lifecycle (display init, XML registration, subject wiring, panel construction,
# shutdown) and fails loudly on a crash.
#
# Usage: scripts/smoke-headless.sh [path-to-helix-screen]

set -uo pipefail

BIN="${1:-build/bin/helix-screen}"
LOG="$(mktemp -t helix-smoke-XXXXXX.log)"
SOCK="/tmp/helix-smoke-$$.sock"          # short path: unix sockets cap at ~108 bytes
SHOT="$(mktemp -t helix-smoke-XXXXXX.png)"
BOOT_TIMEOUT="${HELIX_SMOKE_BOOT_TIMEOUT:-60}"
APP_PID=""
FAILED=0

red()   { printf '\033[0;31m%s\033[0m\n' "$*"; }
green() { printf '\033[0;32m%s\033[0m\n' "$*"; }
info()  { printf '\033[0;36m%s\033[0m\n' "$*"; }

fail() {
    red "✗ $*"
    FAILED=1
}

cleanup() {
    if [ -n "$APP_PID" ] && kill -0 "$APP_PID" 2>/dev/null; then
        kill -KILL "$APP_PID" 2>/dev/null
        wait "$APP_PID" 2>/dev/null
    fi
    rm -f "$SOCK" "$SHOT"
}
trap cleanup EXIT

[ -x "$BIN" ] || { red "✗ Binary not found or not executable: $BIN"; exit 1; }
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

# No window system needed. The SDL backend falls back to the software renderer
# on its own when no accelerated one is available. Audio is silenced the same
# way automatically — silence_audio_if_headless() in main() forces
# SDL_AUDIODRIVER=dummy under SDL_VIDEODRIVER=dummy, so the smoke run won't
# beep through the CI runner's speakers (if it even has any).
export SDL_VIDEODRIVER=dummy

info "Booting $BIN headless (socket $SOCK)"
"$BIN" --test -vv --remote-socket "$SOCK" > "$LOG" 2>&1 &
APP_PID=$!

# Wait for the control socket rather than sleeping a fixed amount — CI runners
# are slower and more variable than a dev box.
for _ in $(seq 1 "$BOOT_TIMEOUT"); do
    [ -S "$SOCK" ] && break
    if ! kill -0 "$APP_PID" 2>/dev/null; then
        red "✗ Process died during boot"
        tail -40 "$LOG"
        exit 1
    fi
    sleep 1
done

if [ ! -S "$SOCK" ]; then
    red "✗ Control socket never appeared after ${BOOT_TIMEOUT}s"
    tail -40 "$LOG"
    exit 1
fi
green "✓ Booted and serving on the control socket"

if ! grep -q "SDL display created" "$LOG"; then
    fail "No display was created"
fi

# Build a panel or two. This is where a broken XML component, a missing subject
# registration or a null-deref in a panel constructor actually surfaces.
for panel in settings filament print_select; do
    if "$BIN" ctl --socket "$SOCK" navigate "$panel" > /dev/null 2>&1; then
        green "✓ navigate $panel"
    else
        fail "navigate $panel failed"
    fi
done

# Screenshots go through lv_snapshot_take(), which re-renders the object tree —
# a non-trivial PNG proves the widget tree is real, not just that we didn't crash.
if "$BIN" ctl --socket "$SOCK" screenshot "$SHOT" > /dev/null 2>&1 && [ -s "$SHOT" ]; then
    green "✓ screenshot captured ($(wc -c < "$SHOT") bytes)"
else
    fail "screenshot failed"
fi

info "Requesting shutdown"
"$BIN" ctl --socket "$SOCK" shutdown > /dev/null 2>&1

for _ in $(seq 1 30); do
    kill -0 "$APP_PID" 2>/dev/null || break
    sleep 1
done

if kill -0 "$APP_PID" 2>/dev/null; then
    fail "Did not exit within 30s of shutdown request"
    kill -KILL "$APP_PID" 2>/dev/null
    wait "$APP_PID" 2>/dev/null
    EXIT_STATUS=""
else
    wait "$APP_PID"
    EXIT_STATUS=$?
    APP_PID=""
fi

# 139 = SIGSEGV, 134 = SIGABRT. These are the ones worth naming explicitly:
# a crash on the way out is just as much a bug as a crash on the way in.
case "${EXIT_STATUS:-}" in
    0)   green "✓ Clean exit" ;;
    139) fail "SEGFAULT on shutdown (exit 139)" ;;
    134) fail "ABORT on shutdown (exit 134)" ;;
    "")  ;;  # already reported above
    *)   fail "Unclean exit (status $EXIT_STATUS)" ;;
esac

if grep -qE "Segmentation fault|AddressSanitizer|std::terminate" "$LOG"; then
    fail "Crash signature found in log"
fi

if [ "$FAILED" -ne 0 ]; then
    red "─── last 60 log lines ───"
    tail -60 "$LOG"
    red "✗ Headless smoke test FAILED (full log: $LOG)"
    exit 1
fi

rm -f "$LOG"
green "✓ Headless smoke test passed"

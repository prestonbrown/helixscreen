#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for helix-launcher.sh co-host detection and the two resource decisions
# gated on it:
# - renice +10, so Klipper's control loop keeps CPU headroom.
# - HELIX_OOM_SCORE_ADJ, exported for helix-screen to apply to itself so the
#   kernel kills the UI (which helix-watchdog restarts) rather than Klipper
#   (which cannot be restarted mid-print).
#
# Both are skipped on a standalone display (remote-display SonicPad, dev
# workstation, kiosk pointed at a network printer).

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LAUNCHER="$WORKTREE_ROOT/scripts/helix-launcher.sh"

setup() {
    load helpers

    # Extract the helix_klipper_co_hosted function from the launcher into a
    # standalone snippet we can source. Range: from the function header to
    # the next line starting with '}'.
    awk '/^helix_klipper_co_hosted\(\)/,/^}/' "$LAUNCHER" \
        > "$BATS_TEST_TMPDIR/co_hosted.sh"

    # Sanity: the snippet must contain the function body, not be empty.
    [ -s "$BATS_TEST_TMPDIR/co_hosted.sh" ]
    grep -q '^}' "$BATS_TEST_TMPDIR/co_hosted.sh"

    FAKE_PROC="$BATS_TEST_TMPDIR/proc"
    mkdir -p "$FAKE_PROC"
}

# Create a fake /proc/<pid>/cmdline with NUL-separated argv, like the kernel.
# Usage: make_proc_entry <pid> <argv0> [argv1 ...]
make_proc_entry() {
    local pid="$1"
    shift
    mkdir -p "$FAKE_PROC/$pid"
    printf '%s\0' "$@" > "$FAKE_PROC/$pid/cmdline"
}

# Run the extracted predicate against the fake proc tree.
#
# A never-matching pgrep is put FIRST on PATH rather than emptying PATH: the
# scan legitimately needs `tr` (a BusyBox applet, present on every target), but
# a regression back to pgrep must not be able to pass by finding the
# developer's own running Klipper via the real /proc.
run_co_hosted() {
    local stubs="$BATS_TEST_TMPDIR/stubs"
    mkdir -p "$stubs"
    printf '#!/bin/sh\nexit 1\n' > "$stubs/pgrep"
    chmod +x "$stubs/pgrep"
    run env "PATH=$stubs:$PATH" "HELIX_PROC_ROOT=$FAKE_PROC" "$(command -v sh)" -c \
        ". '$BATS_TEST_TMPDIR/co_hosted.sh' && helix_klipper_co_hosted"
}

# Real Klipper/Moonraker on the test host would defeat the "no co-host"
# assertions via the socket fallback. Skip those cases when sockets are present.
skip_if_real_klipper_sockets() {
    if [ -S /tmp/klippy_uds ] || [ -S /tmp/moonraker.sock ] || [ -S /tmp/uds ]; then
        skip "real Klipper/Moonraker sockets present on this host"
    fi
}

# =============================================================================
# Static structure: the launcher contains the expected logic
# =============================================================================

@test "launcher defines helix_klipper_co_hosted function" {
    grep -q '^helix_klipper_co_hosted()' "$LAUNCHER"
}

@test "co-host detection scans /proc cmdlines, not pgrep" {
    # pgrep is absent entirely on some BusyBox rootfs (Forge-X on the AD5M),
    # which is what made the previous probe silently return false there.
    grep -q 'HELIX_PROC_ROOT' "$LAUNCHER"
    grep -qE '\$\{?HELIX_PROC_ROOT\}?"?/\[0-9\]\*/cmdline' "$LAUNCHER"
    # No CODE line may invoke pgrep. Comments explaining why we avoid it
    # are fine, so strip comment lines before looking.
    run sh -c "grep -v '^[[:space:]]*#' '$LAUNCHER' | grep -c pgrep"
    [ "$(last_line)" = "0" ]
}

@test "HELIX_PROC_ROOT defaults to /proc" {
    grep -qE '^: "\$\{HELIX_PROC_ROOT:=/proc\}"' "$LAUNCHER"
}

@test "co-host detection has socket fallback for klippy_uds" {
    grep -q '/tmp/klippy_uds' "$LAUNCHER"
}

@test "co-host detection has socket fallback for moonraker.sock" {
    grep -q '/tmp/moonraker.sock' "$LAUNCHER"
}

@test "co-host detection has socket fallback for Forge-X /tmp/uds" {
    grep -qE '\[ -S /tmp/uds \]' "$LAUNCHER"
}

@test "renice is gated on helix_klipper_co_hosted" {
    # The renice call must live inside an `if helix_klipper_co_hosted; then`
    # block — never unconditional.
    awk '
        /^if helix_klipper_co_hosted/ { inside = 1 }
        inside && /renice /            { found = 1 }
        inside && /^fi$/               { inside = 0 }
        END { exit found ? 0 : 1 }
    ' "$LAUNCHER"
}

@test "default nice level is +10" {
    grep -qE 'HELIX_NICE:-10' "$LAUNCHER"
}

@test "HELIX_NICE=0 disables niceness" {
    grep -qE '"\$\{?_helix_nice\}?"[[:space:]]*!=[[:space:]]*"0"' "$LAUNCHER" \
        || grep -qE '"\$\{?HELIX_NICE\}?"[[:space:]]*!=[[:space:]]*"0"' "$LAUNCHER"
}

@test "renice happens AFTER platform_pre_start hook (post-wait positioning)" {
    pre_start_line=$(grep -n 'platform_pre_start' "$LAUNCHER" | tail -1 | cut -d: -f1)
    renice_line=$(grep -n '^if helix_klipper_co_hosted' "$LAUNCHER" | tail -1 | cut -d: -f1)
    [ -n "$pre_start_line" ]
    [ -n "$renice_line" ]
    [ "$renice_line" -gt "$pre_start_line" ]
}

# =============================================================================
# Static structure: OOM score handoff
# =============================================================================

@test "HELIX_OOM_SCORE_ADJ export is gated on helix_klipper_co_hosted" {
    awk '
        /^if helix_klipper_co_hosted/         { inside = 1 }
        inside && /export HELIX_OOM_SCORE_ADJ/ { found = 1 }
        inside && /^fi$/                       { inside = 0 }
        END { exit found ? 0 : 1 }
    ' "$LAUNCHER"
}

@test "default oom_score_adj is +300" {
    grep -qE 'HELIX_OOM_SCORE_ADJ:-300' "$LAUNCHER"
}

@test "HELIX_OOM_SCORE_ADJ=0 disables the handoff" {
    grep -qE '"\$\{?_helix_oom\}?"[[:space:]]*!=[[:space:]]*"0"' "$LAUNCHER"
}

@test "launcher never writes oom_score_adj itself" {
    # oom_score_adj is inherited across fork and preserved across exec. Applying
    # it in the launcher would also mark this shell and helix-watchdog, and
    # killing the watchdog is what stops helix-screen from coming back.
    # helix-screen must apply it to /proc/self instead.
    ! grep -qE '>[[:space:]]*/proc/(self|\$\$)/oom_score_adj' "$LAUNCHER"
}

# =============================================================================
# Behavioral: the extracted predicate against a fake /proc
# =============================================================================

@test "co-host detection: finds klippy.py with no pgrep on PATH" {
    make_proc_entry 101 /opt/Python-3.7.11/bin/python3.7 /opt/klipper/klippy/klippy.py \
        /opt/config/printer.cfg -a /tmp/uds
    run_co_hosted
    [ "$status" -eq 0 ]
}

@test "co-host detection: finds Moonraker started from a venv path" {
    # AD5M / CB1 / Pi shape: /root/moonraker-env/bin/python3 .../moonraker.py
    make_proc_entry 102 /root/moonraker-env/bin/python3 \
        /root/moonraker-env/moonraker/moonraker.py -d /root/printer_data
    run_co_hosted
    [ "$status" -eq 0 ]
}

@test "co-host detection: finds Moonraker started as a module" {
    # CC1 shape: python3 -X no_debug_ranges -m moonraker.moonraker -d ...
    make_proc_entry 103 /usr/bin/python3 -X no_debug_ranges -m moonraker.moonraker \
        -d /etc/klipper
    run_co_hosted
    [ "$status" -eq 0 ]
}

@test "co-host detection: ignores unrelated processes" {
    skip_if_real_klipper_sockets
    make_proc_entry 201 /sbin/init
    make_proc_entry 202 /usr/sbin/dropbear -F -p 22
    make_proc_entry 203 httpd -p 80 -f -h /root/www
    run_co_hosted
    [ "$status" -ne 0 ]
}

@test "co-host detection: a --moonraker URL flag is NOT a co-host" {
    # Regression guard. A standalone kiosk pointed at a network printer runs
    # `helix-screen --moonraker ws://host:7125`. A loose *moonraker* pattern
    # would match that and nice down the very case this must leave alone.
    skip_if_real_klipper_sockets
    make_proc_entry 301 /opt/helixscreen/bin/helix-screen --moonraker ws://192.168.1.50:7125
    run_co_hosted
    [ "$status" -ne 0 ]
}

@test "co-host detection: empty proc tree is not a co-host" {
    skip_if_real_klipper_sockets
    run_co_hosted
    [ "$status" -ne 0 ]
}

@test "co-host detection: survives a cmdline that vanishes mid-scan" {
    # Processes exit while we walk /proc. An unreadable entry must not abort
    # the function under `set -e`.
    skip_if_real_klipper_sockets
    mkdir -p "$FAKE_PROC/401"
    : > "$FAKE_PROC/401/cmdline"
    chmod 000 "$FAKE_PROC/401/cmdline" 2>/dev/null || true
    make_proc_entry 402 /sbin/init
    run_co_hosted
    [ "$status" -ne 0 ]
    chmod 644 "$FAKE_PROC/401/cmdline" 2>/dev/null || true
}

@test "co-host detection: an empty cmdline (kernel thread) is skipped" {
    skip_if_real_klipper_sockets
    mkdir -p "$FAKE_PROC/501"
    : > "$FAKE_PROC/501/cmdline"
    run_co_hosted
    [ "$status" -ne 0 ]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The installer's scratch dir must be removed even when the run does not reach
# its success path.
#
# Cleanup was wired only to cleanup_on_success (normal end) and error_handler,
# which hangs off `trap ... ERR`. ERR is a bash extension: main.sh installs it
# with `2>/dev/null || true`, so on the ash/dash shells every embedded platform
# actually runs it silently never arms. Any interrupted or non-zero exit
# therefore left the full download behind — a K2 was found holding a 60MB
# helixscreen.zip from four months earlier, on a 240MB overlay partition.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
MAIN_SH="$WORKTREE_ROOT/scripts/lib/installer/main.sh"

setup() {
    load helpers
}

@test "main.sh arms cleanup on EXIT/INT/TERM, not only ERR" {
    # ERR alone is not enough: it is a no-op under ash/dash.
    grep -qE "^trap .*(EXIT|INT|TERM)" "$MAIN_SH"
}

# Drive a real POSIX shell through an abnormal exit and assert the scratch dir
# is gone. Uses dash when available, because that is the shell class where the
# ERR trap silently does nothing.
_run_abnormal_exit() {
    local shell_bin="$1" exit_mode="$2" tmpdir="$3"
    mkdir -p "$tmpdir"
    echo payload > "$tmpdir/helixscreen.zip"

    "$shell_bin" -c '
        TMP_DIR="$1"
        # Minimal stand-ins for the installer helpers the trap path touches.
        log_info() { :; }
        log_warn() { :; }
        _safe_remove_tmp_dir() { rm -rf "$1"; }
        cleanup_on_success() { [ -d "$TMP_DIR" ] && _safe_remove_tmp_dir "$TMP_DIR"; }

        trap "cleanup_on_success" EXIT INT TERM
        trap "error_handler" ERR 2>/dev/null || true

        if [ "$2" = "fail" ]; then exit 1; fi
        kill -TERM $$
    ' _ "$tmpdir" "$exit_mode"
}

@test "scratch dir is removed on a non-zero exit under dash" {
    command -v dash >/dev/null 2>&1 || skip "dash not installed"
    local tmpdir="$BATS_TEST_TMPDIR/helixscreen-install"

    _run_abnormal_exit dash fail "$tmpdir" || true

    [ ! -d "$tmpdir" ]
}

@test "scratch dir is removed on SIGTERM under dash" {
    command -v dash >/dev/null 2>&1 || skip "dash not installed"
    local tmpdir="$BATS_TEST_TMPDIR/helixscreen-install-term"

    _run_abnormal_exit dash term "$tmpdir" || true

    [ ! -d "$tmpdir" ]
}

@test "without the EXIT trap the scratch dir survives (proves the test bites)" {
    command -v dash >/dev/null 2>&1 || skip "dash not installed"
    local tmpdir="$BATS_TEST_TMPDIR/helixscreen-install-leak"
    mkdir -p "$tmpdir"
    echo payload > "$tmpdir/helixscreen.zip"

    # Same shape, ERR trap only — the pre-fix behaviour.
    dash -c '
        TMP_DIR="$1"
        cleanup_on_success() { rm -rf "$TMP_DIR"; }
        trap "cleanup_on_success" ERR 2>/dev/null || true
        exit 1
    ' _ "$tmpdir" || true

    [ -d "$tmpdir" ]
}

@test "cleanup_on_success still routes through the destructive-path guard" {
    # The trap must not become a second, unguarded rm -rf: TMP_DIR=/mnt/UDISK
    # once wiped a K2's entire user partition, and _safe_remove_tmp_dir is what
    # refuses that shape.
    grep -q '_safe_remove_tmp_dir' "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    # The trap fires cleanup_on_success, never a bare rm.
    refute_grep 'trap .*rm -rf' "$MAIN_SH"
}

@test "the uninstall sentinel trap does not disarm scratch cleanup" {
    # A trap REPLACES the handler for a signal, and install.sh bundles both the
    # main and uninstall modules — so the uninstall path's EXIT trap must run
    # cleanup_on_success too, or --uninstall silently leaks the scratch dir.
    local uninstall_sh="$WORKTREE_ROOT/scripts/lib/installer/uninstall.sh"
    run grep -E "^\s*trap .*_sweep_uninstalling_sentinel.* EXIT INT TERM" "$uninstall_sh"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'cleanup_on_success'
}

@test "chained uninstall trap runs both handlers under dash" {
    command -v dash >/dev/null 2>&1 || skip "dash not installed"
    local tmpdir="$BATS_TEST_TMPDIR/chained-helixscreen-install"
    local marker="$BATS_TEST_TMPDIR/sentinel-swept"
    mkdir -p "$tmpdir"

    dash -c '
        TMP_DIR="$1"
        MARKER="$2"
        _sweep_uninstalling_sentinel() { touch "$MARKER"; }
        cleanup_on_success() { rm -rf "$TMP_DIR"; }
        trap "_sweep_uninstalling_sentinel; type cleanup_on_success >/dev/null 2>&1 && cleanup_on_success" EXIT INT TERM
        exit 1
    ' _ "$tmpdir" "$marker" || true

    [ -f "$marker" ]
    [ ! -d "$tmpdir" ]
}

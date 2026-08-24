#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The install swap must know whether it is renaming or copying.
#
# `mv` within one filesystem is a rename and needs no free space. Across
# filesystems it is a copy-then-delete needing the whole tree's worth — and the
# staging dir routinely lives on a different partition now (the K2 stages on
# /mnt/UDISK and installs to /opt, because /opt is a 240MB overlay). Phase 4
# sizes this before the old install is moved aside; its tight path then
# relocates that old install off-partition and assumes the freed space is
# enough, which is false when the new tree is much larger than the old one.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
RELEASE_SH="$WORKTREE_ROOT/scripts/lib/installer/release.sh"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED
    export SUDO=""
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$RELEASE_SH"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
}

# ---------------------------------------------------------------------------
# _fs_id — same-filesystem detection
# ---------------------------------------------------------------------------

@test "_fs_id: two dirs on the same filesystem report the same id" {
    mkdir -p "$BATS_TEST_TMPDIR/a" "$BATS_TEST_TMPDIR/b"
    [ "$(_fs_id "$BATS_TEST_TMPDIR/a")" = "$(_fs_id "$BATS_TEST_TMPDIR/b")" ]
}

@test "_fs_id: returns a non-empty identity for a real dir" {
    [ -n "$(_fs_id "$BATS_TEST_TMPDIR")" ]
}

@test "_fs_id never returns a bare number (df -P guards the wrapped-name trap)" {
    # Without -P, df wraps a long device name onto its own line and
    # `tail -1 | awk '{print $1}'` yields a BLOCK COUNT instead of a device.
    # Two filesystems then compare unequal by accident, so a rename is
    # mistaken for a copy — or worse, two different ones compare equal and the
    # space check is skipped entirely.
    local id
    id=$(_fs_id "$BATS_TEST_TMPDIR")
    [ -n "$id" ]
    # A device identity is never purely digits.
    echo "$id" | refute_grep '^[0-9][0-9]*$'
}

@test "the df helpers pass -P so output is one line per filesystem" {
    grep -q 'df -kP "\$1" 2>/dev/null | tail -1 | awk .{print \$1}' "$RELEASE_SH" ||
        grep -qE '_fs_id\(\)' "$RELEASE_SH"
    # Both helpers must carry -P; a bare `df -k` is the wrapped-name bug.
    local helpers
    helpers=$(awk '/^_fs_id\(\)/,/^}/' "$RELEASE_SH"; awk '/^_fs_free_mb\(\)/,/^}/' "$RELEASE_SH")
    [ -n "$helpers" ]
    echo "$helpers" | grep -c 'df -kP' | grep -q '^2$'
    echo "$helpers" | refute_grep 'df -k "'
}

@test "_fs_id: distinguishes genuinely different filesystems" {
    # /dev/shm is a separate tmpfs mount on Linux; skip where it is not.
    [ -d /dev/shm ] || skip "/dev/shm not present"
    local a b
    a=$(_fs_id "$BATS_TEST_TMPDIR")
    b=$(_fs_id /dev/shm)
    [ -n "$a" ] && [ -n "$b" ]
    [ "$a" != "$b" ]
}

# ---------------------------------------------------------------------------
# _tree_size_mb — must never fabricate a zero
# ---------------------------------------------------------------------------

@test "_tree_size_mb: measures a real tree" {
    local d="$BATS_TEST_TMPDIR/tree"
    mkdir -p "$d"
    dd if=/dev/zero of="$d/blob" bs=1M count=6 2>/dev/null
    local mb
    mb=$(_tree_size_mb "$d")
    [ -n "$mb" ]
    [ "$mb" -ge 5 ]
}

@test "_tree_size_mb: echoes NOTHING (not 0) when the path does not exist" {
    # A fabricated zero turns the guard into `free < 10`, which passes on any
    # healthy filesystem — the exact shape that lets an install die mid-copy
    # having reported no problem.
    local out
    out=$(_tree_size_mb "$BATS_TEST_TMPDIR/definitely/absent")
    [ -z "$out" ]
}

@test "_fs_free_mb: reports a plausible figure for a real filesystem" {
    local mb
    mb=$(_fs_free_mb "$BATS_TEST_TMPDIR")
    [ -n "$mb" ]
    [ "$mb" -ge 0 ]
}

@test "_fs_free_mb: echoes nothing for a nonexistent path" {
    [ -z "$(_fs_free_mb "$BATS_TEST_TMPDIR/no/such/dir")" ]
}

# ---------------------------------------------------------------------------
# _check_swap_space — the decision itself
# ---------------------------------------------------------------------------

@test "_check_swap_space: same filesystem proceeds without measuring" {
    # A rename consumes nothing, so the size/free helpers must not even be
    # consulted — stub them to fail loudly if they are.
    _tree_size_mb() { echo "MEASURED" >&2; return 1; }
    _fs_free_mb()   { echo "MEASURED" >&2; return 1; }

    mkdir -p "$BATS_TEST_TMPDIR/src" "$BATS_TEST_TMPDIR/dst"
    run _check_swap_space "$BATS_TEST_TMPDIR/src" "$BATS_TEST_TMPDIR/dst"

    [ "$status" -eq 0 ]
    echo "$output" | refute_grep 'MEASURED'
}

@test "_check_swap_space: cross-filesystem with room proceeds" {
    _fs_id()        { echo "fs-$1"; }          # every path its own filesystem
    _tree_size_mb() { echo 50; }
    _fs_free_mb()   { echo 500; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Cross-filesystem install swap'
}

@test "_check_swap_space: cross-filesystem WITHOUT room refuses" {
    # The case that matters: new tree bigger than the destination can hold.
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo 100; }
    _fs_free_mb()   { echo 40; }

    run _check_swap_space /src /dst
    [ "$status" -eq 1 ]
    echo "$output" | grep -q 'Not enough space to install'
    echo "$output" | grep -q '100MB'
    echo "$output" | grep -q '40MB'
}

@test "_check_swap_space: refuses when free space only just misses the margin" {
    # need+10 is the bar; 109 free for a 100MB tree must still refuse.
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo 100; }
    _fs_free_mb()   { echo 109; }

    run _check_swap_space /src /dst
    [ "$status" -eq 1 ]
}

@test "_check_swap_space: accepts exactly need+margin" {
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo 100; }
    _fs_free_mb()   { echo 110; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
}

@test "_check_swap_space: unmeasurable size warns but does not block" {
    # Refusing on an unknown would brick updates on any platform whose du is
    # odd; the contract is "say so, then proceed".
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo ""; }
    _fs_free_mb()   { echo 500; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Could not measure free space'
}

@test "_check_swap_space: unmeasurable free space warns but does not block" {
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo 50; }
    _fs_free_mb()   { echo ""; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Could not measure free space'
}

@test "_check_swap_space: an unidentifiable filesystem still gets measured" {
    # If df cannot name the filesystem we cannot prove it is a rename, so the
    # size check must still run rather than being skipped as same-fs.
    _fs_id()        { echo ""; }
    _tree_size_mb() { echo 100; }
    _fs_free_mb()   { echo 20; }

    run _check_swap_space /src /dst
    [ "$status" -eq 1 ]
    echo "$output" | grep -q 'Not enough space to install'
}

@test "the swap refuses BEFORE the mv and restores the backup" {
    # Between "old install moved aside" and "new install in place" the box has
    # no install; any bail in that window must put the old one back.
    local phase5 check_line mv_line
    phase5=$(awk '/Phase 5: Move new install into place/,/Phase 6/' "$RELEASE_SH")
    [ -n "$phase5" ]
    echo "$phase5" | grep -q '_restore_install_backup "space check"'
    check_line=$(echo "$phase5" | grep -n '_check_swap_space' | head -1 | cut -d: -f1)
    mv_line=$(echo "$phase5" | grep -n 'mv "${new_install}"' | head -1 | cut -d: -f1)
    [ -n "$check_line" ] && [ -n "$mv_line" ]
    [ "$check_line" -lt "$mv_line" ]
}

# ---------------------------------------------------------------------------
# _restore_install_backup
# ---------------------------------------------------------------------------

@test "_restore_install_backup: puts the previous install back" {
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export INSTALL_BACKUP="$BATS_TEST_TMPDIR/opt/helixscreen.old"
    mkdir -p "$INSTALL_BACKUP/bin"
    echo old > "$INSTALL_BACKUP/bin/helix-screen"

    _restore_install_backup "test"

    [ -d "$INSTALL_DIR" ]
    [ "$(cat "$INSTALL_DIR/bin/helix-screen")" = "old" ]
    [ ! -d "$INSTALL_BACKUP" ]
}

@test "_restore_install_backup: clears a partial new install first" {
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export INSTALL_BACKUP="$BATS_TEST_TMPDIR/opt/helixscreen.old"
    mkdir -p "$INSTALL_BACKUP/bin" "$INSTALL_DIR/bin"
    echo old > "$INSTALL_BACKUP/bin/helix-screen"
    echo partial > "$INSTALL_DIR/bin/helix-screen"

    _restore_install_backup "test"

    # The half-written tree must be gone, replaced by the backup.
    [ "$(cat "$INSTALL_DIR/bin/helix-screen")" = "old" ]
}

@test "_restore_install_backup: no backup is a silent no-op" {
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export INSTALL_BACKUP="$BATS_TEST_TMPDIR/opt/nothing-here"

    run _restore_install_backup "test"

    [ "$status" -eq 0 ]
}

@test "_restore_install_backup: only removes a helixscreen-rollback offsite dir" {
    # The guard that exists because TMP_DIR=/mnt/UDISK once wiped a K2's whole
    # user partition. A mount root named anything else must survive.
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export HELIX_OFFSITE_ROLLBACK_DIR="$BATS_TEST_TMPDIR/mnt/UDISK"
    export INSTALL_BACKUP="$HELIX_OFFSITE_ROLLBACK_DIR/helixscreen"
    # Phase 5 mkdir -p's the install parent before any rollback can run.
    mkdir -p "$(dirname "$INSTALL_DIR")"
    mkdir -p "$INSTALL_BACKUP/bin" "$HELIX_OFFSITE_ROLLBACK_DIR/printer_data"
    echo old > "$INSTALL_BACKUP/bin/helix-screen"
    echo cfg > "$HELIX_OFFSITE_ROLLBACK_DIR/printer_data/printer.cfg"

    _restore_install_backup "test"

    [ -d "$HELIX_OFFSITE_ROLLBACK_DIR" ]
    [ -f "$HELIX_OFFSITE_ROLLBACK_DIR/printer_data/printer.cfg" ]
}

# ---------------------------------------------------------------------------
# Numeric hardening
#
# The installer runs under `set -eu`. dash aborts on `$(( garbage + 10 ))`
# ("Illegal number"), which would kill the run in the one window where the old
# install is already moved aside and the new one is not yet in place — leaving
# the printer with nothing. An empty value fails the other way: `$(( "" + 10 ))`
# is 10, so the guard silently degrades to `free < 10`.
# ---------------------------------------------------------------------------

@test "_check_swap_space: a non-numeric size cannot abort the run" {
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo "du: cannot access"; }
    _fs_free_mb()   { echo 500; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Could not measure free space'
    echo "$output" | refute_grep 'Illegal number'
}

@test "_check_swap_space: a non-numeric free figure cannot abort the run" {
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo 50; }
    _fs_free_mb()   { echo "N/A"; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Could not measure free space'
}

@test "_check_swap_space: survives non-numeric input under a real 'set -eu' dash" {
    # The abort only reproduces in a POSIX shell with -e -u, which is what
    # install-dev.sh actually sets. bats runs bash, so drive dash directly.
    command -v dash >/dev/null 2>&1 || skip "dash not installed"

    cat > "$BATS_TEST_TMPDIR/probe.sh" << 'PROBE'
set -eu
log_warn()  { echo "WARN: $*"; }
log_error() { echo "ERROR: $*"; }
log_info()  { echo "INFO: $*"; }
. "$1"
_fs_id()        { echo "fs-$1"; }
_tree_size_mb() { echo "du: cannot access"; }
_fs_free_mb()   { echo 500; }
_check_swap_space /src /dst
echo "SURVIVED rc=$?"
PROBE

    run dash "$BATS_TEST_TMPDIR/probe.sh" "$WORKTREE_ROOT/scripts/lib/installer/release.sh"
    echo "$output" | grep -q 'SURVIVED'
    echo "$output" | refute_grep 'Illegal number'
}

@test "a decimal size is rejected rather than fed to the arithmetic" {
    # `du -m` on some systems can emit a decimal; dash cannot do float math.
    _fs_id()        { echo "fs-$1"; }
    _tree_size_mb() { echo "12.5"; }
    _fs_free_mb()   { echo 500; }

    run _check_swap_space /src /dst
    [ "$status" -eq 0 ]
    echo "$output" | grep -q 'Could not measure free space'
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for path_sudo() vs file_sudo() in common.sh.
#
# The two answer different questions and are not interchangeable:
#
#   file_sudo <p>  can I WRITE INTO p?      → tests p itself when it exists
#   path_sudo <p>  can I RENAME or REMOVE p? → always tests p's PARENT
#
# rename(2) and unlink(2) mutate the parent's directory entries, not the target,
# so a user-owned directory inside a root-owned parent is writable and still
# cannot be moved or deleted. That is the /opt/helixscreen layout exactly: the
# unit's ExecStartPre chowns the install dir to the service user while /opt stays
# root-owned. Asking file_sudo about the install dir there returns "" and the
# swap runs bare, failing with:
#
#   mv: cannot move '/opt/helixscreen' to '/opt/helixscreen.old': Permission denied

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    load helpers
    # AFTER the last `load helpers` — helpers.bash exports SUDO="" as a no-op
    # stub, so setting it any earlier is silently undone and every case below
    # would assert against an empty escalation prefix.
    SUDO="sudo"

    # Without this the cases asserting an empty result pass against a MISSING
    # path_sudo: $(undefined_command) is also "".
    declare -F path_sudo >/dev/null || {
        echo "path_sudo is not defined in common.sh" >&2
        return 1
    }

    # A writable target inside a NON-writable parent — the shape that breaks.
    PARENT="$BATS_TEST_TMPDIR/ro-parent"
    TARGET="$PARENT/helixscreen"
    mkdir -p "$TARGET"
    chmod 0500 "$PARENT"
}

teardown() {
    chmod 0700 "$PARENT" 2>/dev/null || true
}

# --- The bug this helper exists for ---

@test "path_sudo escalates for a writable target inside a read-only parent" {
    if [ "$(id -u)" = "0" ]; then
        SKIP "running as root: -w ignores permission bits"
    fi
    [ "$(path_sudo "$TARGET")" = "sudo" ]
}

@test "file_sudo does NOT escalate for the same path — that is why it is wrong here" {
    # Pinning the documented behavior, not endorsing it for renames. If this ever
    # flips, path_sudo is redundant and the two should be merged deliberately
    # rather than by accident.
    if [ "$(id -u)" = "0" ]; then
        SKIP "running as root: -w ignores permission bits"
    fi
    [ "$(file_sudo "$TARGET")" = "" ]
}

# --- Agreement where both are right ---

@test "path_sudo does not escalate when the parent is writable" {
    chmod 0700 "$PARENT"
    [ "$(path_sudo "$TARGET")" = "" ]
}

@test "path_sudo checks the parent for a path that does not exist" {
    # Same answer either way: removing a missing file is a parent operation too.
    if [ "$(id -u)" = "0" ]; then
        SKIP "running as root: -w ignores permission bits"
    fi
    [ "$(path_sudo "$PARENT/not-there")" = "sudo" ]
}

@test "path_sudo escalates for a read-only target whose parent is writable" {
    # The target's own mode is irrelevant to rename/unlink — only the parent
    # decides. A 0500 target inside a 0700 parent is removable.
    chmod 0700 "$PARENT"
    chmod 0500 "$TARGET"
    [ "$(path_sudo "$TARGET")" = "" ]
    chmod 0700 "$TARGET"
}

@test "path_sudo honours an empty SUDO" {
    # check_permissions() leaves SUDO empty when already root or when sudo is
    # unavailable. path_sudo must echo that, not the literal string "sudo".
    if [ "$(id -u)" = "0" ]; then
        SKIP "running as root: -w ignores permission bits"
    fi
    SUDO=""
    [ "$(path_sudo "$TARGET")" = "" ]
}

# --- Call-site coverage ---

@test "the install-root swap uses path_sudo, not file_sudo" {
    # mv "${INSTALL_DIR}" "$INSTALL_BACKUP" renames the install root. Both the
    # roomy (same-fs) and off-partition branches do it.
    local release="$WORKTREE_ROOT/scripts/lib/installer/release.sh"
    ! grep -E 'file_sudo "\$\{INSTALL_DIR\}"\) mv "\$\{INSTALL_DIR\}"' "$release"
    [ "$(grep -cE 'path_sudo "\$\{INSTALL_DIR\}"\) mv "\$\{INSTALL_DIR\}"' "$release")" = "2" ]
}

@test "bundled install.sh carries path_sudo" {
    grep -q '^path_sudo()' "$WORKTREE_ROOT/scripts/install.sh"
    ! grep -E 'file_sudo "\$\{INSTALL_DIR\}"\) mv "\$\{INSTALL_DIR\}"' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled uninstall.sh carries path_sudo" {
    grep -q '^path_sudo()' "$WORKTREE_ROOT/scripts/uninstall.sh"
}

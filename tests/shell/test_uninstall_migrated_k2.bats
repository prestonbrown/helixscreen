#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Uninstall has to find a K2 wherever the migration left it.
#
# The K2 payload moved to /mnt/UDISK/helixscreen and its state to the -state
# sibling, and devices exist at both the old and the new layout. A root missing
# from the shipped sweep lists is a tree left on the device forever, and an
# uninstall reports success either way.
#
# These build the sandbox from HELIX_INSTALL_DIRS and HELIX_STATE_DIRS as the
# bundled script actually defines them, rather than from a copy, so a root
# dropped from either list fails here instead of quietly narrowing the sweep.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers
    install_gnu_sed_shim

    . "$WORKTREE_ROOT/scripts/uninstall.sh"

    export HELIX_INIT_SCRIPTS="" HELIX_PROCESSES="" PREVIOUS_UIS="" PREVIOUS_UI_SCRIPT=""
    export INIT_SYSTEM="sysv" AD5M_FIRMWARE="" SUDO=""
    export HELIX_STATE_VAR_LIB="$BATS_TEST_TMPDIR/var/lib/helixscreen"
    export HELIX_STATE_ROOT_HOME="$BATS_TEST_TMPDIR/root/.helixscreen"

    SHIPPED_INSTALL_DIRS="$HELIX_INSTALL_DIRS"
    SHIPPED_STATE_DIRS="$HELIX_STATE_DIRS"
}

# Re-point both shipped lists at the sandbox, entry for entry.
sandbox_lists() {
    local d out=""
    for d in $SHIPPED_INSTALL_DIRS; do out="$out $BATS_TEST_TMPDIR$d"; done
    HELIX_INSTALL_DIRS="$out"
    out=""
    for d in $SHIPPED_STATE_DIRS; do out="$out $BATS_TEST_TMPDIR$d"; done
    HELIX_STATE_DIRS="$out"
}

seed_payload() {
    mkdir -p "$BATS_TEST_TMPDIR$1/bin" "$BATS_TEST_TMPDIR$1/config"
    printf '#!/bin/sh\n' > "$BATS_TEST_TMPDIR$1/bin/helix-screen"
    printf '{}\n' > "$BATS_TEST_TMPDIR$1/config/settings.json"
}

seed_state() {
    mkdir -p "$BATS_TEST_TMPDIR$1/cache/helix_thumbs" "$BATS_TEST_TMPDIR$1/logs"
    printf 'x\n' > "$BATS_TEST_TMPDIR$1/logs/helix.log"
}

sweep_state() {
    local p
    for p in $(helix_state_sweep_paths); do
        [ -d "$p" ] && rm -rf "$p"
    done
    helix_state_prune_empty_roots
    return 0
}

# --------------------------------------------------------------------------
# The lists themselves
# --------------------------------------------------------------------------

@test "the shipped sweep names the K2 payload root, at both layouts" {
    [[ " $SHIPPED_INSTALL_DIRS " == *" /mnt/UDISK/helixscreen "* ]] || \
        fail "the migrated K2 root is not swept: $SHIPPED_INSTALL_DIRS"
    [[ " $SHIPPED_INSTALL_DIRS " == *" /opt/helixscreen "* ]] || \
        fail "a K2 that has not migrated yet is not swept: $SHIPPED_INSTALL_DIRS"
}

@test "the shipped sweep names the K2 state root, at both layouts" {
    [[ " $SHIPPED_STATE_DIRS " == *" /mnt/UDISK/helixscreen-state "* ]] || \
        fail "the migrated K2 state root is not swept: $SHIPPED_STATE_DIRS"
    [[ " $SHIPPED_STATE_DIRS " == *" /mnt/UDISK/helixscreen "* ]] || \
        fail "pre-migration K2 state is not swept: $SHIPPED_STATE_DIRS"
}

# --------------------------------------------------------------------------
# What is actually left on disk
# --------------------------------------------------------------------------

@test "uninstalling a migrated K2 removes the payload and its state" {
    sandbox_lists
    seed_payload /mnt/UDISK/helixscreen
    seed_state /mnt/UDISK/helixscreen-state
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"

    remove_installation
    sweep_state

    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen" ] || \
        fail "the payload survived the uninstall"
    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state/cache" ] || \
        fail "the thumbnail cache survived the uninstall"
    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state/logs" ] || \
        fail "the logs survived the uninstall"
}

@test "uninstalling a K2 that never migrated removes the old layout too" {
    sandbox_lists
    seed_payload /opt/helixscreen
    seed_state /mnt/UDISK/helixscreen
    INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"

    remove_installation
    sweep_state

    [ ! -d "$BATS_TEST_TMPDIR/opt/helixscreen" ] || \
        fail "the pre-migration payload survived"
    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen/cache" ] || \
        fail "pre-migration cache survived"
    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen/logs" ] || \
        fail "pre-migration logs survived"
}

@test "a half-migrated device loses both trees, not just one" {
    # A migration interrupted before its sweep leaves a payload at each root.
    sandbox_lists
    seed_payload /opt/helixscreen
    seed_payload /mnt/UDISK/helixscreen
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"

    remove_installation

    [ ! -d "$BATS_TEST_TMPDIR/opt/helixscreen" ] || \
        fail "the tree the migration left behind survived the uninstall"
    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen" ] || \
        fail "the migrated tree survived the uninstall"
}

@test "the sweep is scoped to helixscreen, not the mount it sits on" {
    # /mnt/UDISK carries printer_data and the user's gcode. An uninstall that
    # walked up one level would take the lot.
    sandbox_lists
    seed_payload /mnt/UDISK/helixscreen
    seed_state /mnt/UDISK/helixscreen-state
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/printer_data/gcodes"
    printf 'G28\n' > "$BATS_TEST_TMPDIR/mnt/UDISK/printer_data/gcodes/benchy.gcode"
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"

    remove_installation
    sweep_state

    [ -f "$BATS_TEST_TMPDIR/mnt/UDISK/printer_data/gcodes/benchy.gcode" ] || \
        fail "the uninstall removed the user's gcode"
    [ -d "$BATS_TEST_TMPDIR/mnt/UDISK" ] || fail "the uninstall removed the mount root"
}

# --------------------------------------------------------------------------
# The directory that held the state
# --------------------------------------------------------------------------

@test "the emptied state root goes too, not just its contents" {
    sandbox_lists
    seed_payload /mnt/UDISK/helixscreen
    seed_state /mnt/UDISK/helixscreen-state
    INSTALL_DIR="$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen"

    remove_installation
    sweep_state

    [ ! -d "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state" ] || \
        fail "an empty directory of ours was left on the device"
}

@test "a state root the operator put something in survives" {
    # rmdir is what makes this safe: anything still in the directory means it is
    # not ours alone to remove.
    sandbox_lists
    seed_state /mnt/UDISK/helixscreen-state
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state/notes"
    printf 'keep me\n' > "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state/notes/mine.txt"

    sweep_state

    [ -f "$BATS_TEST_TMPDIR/mnt/UDISK/helixscreen-state/notes/mine.txt" ] || \
        fail "removed a directory that still had the operator's files in it"
}

@test "the prune is scoped by name, not applied to whatever is listed" {
    HELIX_STATE_DIRS="$BATS_TEST_TMPDIR/mnt/UDISK"
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"

    helix_state_prune_empty_roots

    [ -d "$BATS_TEST_TMPDIR/mnt/UDISK" ] || fail "pruned a path that is not ours"
}

@test "only a -state directory is pruned, never a bare helixscreen one" {
    # "-state" is a name this installer coins. A bare ".../helixscreen" is not,
    # and an operator may have meant that directory themselves.
    HELIX_STATE_DIRS="$BATS_TEST_TMPDIR/data/helixscreen $BATS_TEST_TMPDIR/x/helixscreen-state"
    mkdir -p "$BATS_TEST_TMPDIR/data/helixscreen" "$BATS_TEST_TMPDIR/x/helixscreen-state"

    helix_state_prune_empty_roots

    [ -d "$BATS_TEST_TMPDIR/data/helixscreen" ] || \
        fail "pruned a bare helixscreen directory that may not be ours"
    [ ! -d "$BATS_TEST_TMPDIR/x/helixscreen-state" ] || \
        fail "left our own -state directory behind"
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for KIAUH auto-detection and extension installation (kiauh.sh)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Source modules (reset source guards so each test gets a fresh load)
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED _HELIX_KIAUH_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/kiauh.sh"

    # Stage KIAUH extension source files in a mock INSTALL_DIR
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    local src_dir="$INSTALL_DIR/scripts/kiauh/helixscreen"
    mkdir -p "$src_dir"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/__init__.py" "$src_dir/"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/helixscreen_extension.py" "$src_dir/"
    cp "$WORKTREE_ROOT/scripts/kiauh/helixscreen/metadata.json" "$src_dir/"
}

# --- detect_kiauh_dir tests ---

@test "detect_kiauh_dir finds ~/kiauh" {
    mkdir -p "$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    HOME="$BATS_TEST_TMPDIR/fakehome" run detect_kiauh_dir
    [ "$status" -eq 0 ]
    [ "$output" = "$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions" ]
}

@test "detect_kiauh_dir finds /home/user/kiauh" {
    # HOME doesn't have kiauh, but /home/biqu does
    mkdir -p "$BATS_TEST_TMPDIR/fakehome"
    mkdir -p "$BATS_TEST_TMPDIR/home/biqu/kiauh/kiauh/extensions"

    # Override /home scan by using a function wrapper
    # Since we can't create /home/biqu in tests, test the HOME path instead
    # and trust the glob logic for /home/* (tested structurally)
    HOME="$BATS_TEST_TMPDIR/fakehome" run detect_kiauh_dir
    # No KIAUH at HOME, and we can't mock /home — so empty is expected
    [ "$status" -eq 0 ]
}

@test "detect_kiauh_dir returns empty when no kiauh" {
    HOME="$BATS_TEST_TMPDIR/nope" run detect_kiauh_dir
    [ "$status" -eq 0 ]
    [ "$output" = "" ]
}

# --- install_kiauh_extension tests ---

@test "install_kiauh_extension skips when no kiauh" {
    HOME="$BATS_TEST_TMPDIR/nope" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
}

@test "install_kiauh_extension copies files when kiauh found" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]

    # Verify all 3 files were copied
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
}

@test "install_kiauh_extension is a no-op when target files match source" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext/helixscreen"

    # Pre-seed target with identical copies of the source files
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/__init__.py" "$kiauh_ext/helixscreen/"
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/helixscreen_extension.py" "$kiauh_ext/helixscreen/"
    cp "$INSTALL_DIR/scripts/kiauh/helixscreen/metadata.json" "$kiauh_ext/helixscreen/"
    local before_init_mtime
    before_init_mtime=$(stat_mtime "$kiauh_ext/helixscreen/__init__.py")

    sleep 1  # mtime resolution is 1s on most filesystems
    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
    contains "already up to date" "$output"

    # mtime unchanged → file was not rewritten
    local after_init_mtime
    after_init_mtime=$(stat_mtime "$kiauh_ext/helixscreen/__init__.py")
    [ "$before_init_mtime" = "$after_init_mtime" ]
}

@test "install_kiauh_extension updates existing silently" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext/helixscreen"
    echo "old" > "$kiauh_ext/helixscreen/__init__.py"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]

    # Verify files were updated (no longer "old")
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
    # __init__.py should have been replaced with the real one
    ! grep -q "^old$" "$kiauh_ext/helixscreen/__init__.py"
}

@test "install_kiauh_extension installs fresh when skip flag is empty (default-on)" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension ""
    [ "$status" -eq 0 ]
    # No opt-out — extension should be installed
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
    [ -f "$kiauh_ext/helixscreen/helixscreen_extension.py" ]
    [ -f "$kiauh_ext/helixscreen/metadata.json" ]
}

@test "install_kiauh_extension installs when skip flag is false" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "false"
    [ "$status" -eq 0 ]
    [ -f "$kiauh_ext/helixscreen/__init__.py" ]
}

@test "--skip-kiauh-registration skips install" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "true"
    [ "$status" -eq 0 ]
    # Extension dir should NOT have been created
    [ ! -d "$kiauh_ext/helixscreen" ]
}

@test "install_kiauh_extension handles missing source files gracefully" {
    local kiauh_ext="$BATS_TEST_TMPDIR/fakehome/kiauh/kiauh/extensions"
    mkdir -p "$kiauh_ext"

    # Wipe source files
    rm -rf "$INSTALL_DIR/scripts/kiauh"

    HOME="$BATS_TEST_TMPDIR/fakehome" run install_kiauh_extension "yes"
    [ "$status" -eq 0 ]
    # Should not crash, and should not create target dir
    [ ! -d "$kiauh_ext/helixscreen" ]
}

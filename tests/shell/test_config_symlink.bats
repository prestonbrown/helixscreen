#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for setup_config_symlink() and remove_config_symlink() in platform.sh
#
# New layout: real directory in printer_data/config/helixscreen/ with per-file
# symlinks from install_dir/config/ pointing into printer_data.
# Old layout (upgrade path): directory symlink printer_data/config/helixscreen → install_dir/config

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals before each test
    KLIPPER_USER=""
    KLIPPER_HOME=""
    INIT_SCRIPT_DEST=""
    PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    INSTALL_DIR="/opt/helixscreen"
    TMP_DIR="/tmp/helixscreen-install"
    _USER_INSTALL_DIR=""
    SUDO=""

    # Source common.sh + platform.sh (skip source guards by unsetting them).
    # platform.sh calls common.sh's validate_install_dir/validate_tmp_dir; the
    # bundled installer always carries both.
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # common.sh defines the real log_* (they print to stderr, which bats folds
    # into $output). Restore helpers.bash's silent stubs.
    load helpers
}

# --- Happy path: fresh install ---

@test "creates real directory and per-file symlinks on fresh install" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Create a user config file in install dir (as if just installed)
    echo '{"dark_mode":true}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # printer_data/config/helixscreen/ should be a real directory
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    # File should have been moved to printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json")" = '{"dark_mode":true}' ]

    # Install dir should have a symlink pointing to printer_data
    [ -L "$INSTALL_DIR/config/settings.json" ]
    [ "$(readlink "$INSTALL_DIR/config/settings.json")" = "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
}

@test "creates symlinks for all user config files" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Create all user-editable files
    echo '{}' > "$INSTALL_DIR/config/settings.json"
    echo 'ENV=test' > "$INSTALL_DIR/config/helixscreen.env"
    echo 'svc:foo' > "$INSTALL_DIR/config/.disabled_services"
    echo '[]' > "$INSTALL_DIR/config/tool_spools.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # All files should be in printer_data
    for f in settings.json helixscreen.env .disabled_services tool_spools.json; do
        [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/$f" ]
        [ -L "$INSTALL_DIR/config/$f" ]
    done
}

@test "does not create dangling symlinks for files absent from both install dir and printer_data" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Only settings.json exists — simulates runtime-created files (tool_spools.json,
    # .disabled_services) that are in HELIX_USER_CONFIG_FILES but not yet written
    echo '{}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # settings.json moved and symlinked
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
    [ -L "$INSTALL_DIR/config/settings.json" ]

    # Files absent from both install dir and printer_data must NOT get dangling symlinks.
    # A dangling symlink is worse than no symlink: it causes confusing ENOENT errors and
    # prevents the app from writing the file to the install dir as a fallback.
    [ ! -e "$INSTALL_DIR/config/helixscreen.env" ]
    [ ! -e "$INSTALL_DIR/config/tool_spools.json" ]
    [ ! -e "$INSTALL_DIR/config/.disabled_services" ]
}

@test "creates symlink when file already exists in printer_data but not install dir" {
    # Simulates idempotent re-run after runtime-created file was written to printer_data
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    # Simulate tool_spools.json that was written by the app on first run
    echo '[]' > "$KLIPPER_HOME/printer_data/config/helixscreen/tool_spools.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Symlink should now be wired up (pd_file exists)
    [ -L "$INSTALL_DIR/config/tool_spools.json" ]
    [ "$(readlink "$INSTALL_DIR/config/tool_spools.json")" = "$KLIPPER_HOME/printer_data/config/helixscreen/tool_spools.json" ]
}

@test "does not delete config from install dir when cp to printer_data fails" {
    # Bug: silent cp failure followed by unconditional rm destroyed user config.
    # Fix: check cp return code; skip rm and symlink on failure.
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"important":true}' > "$INSTALL_DIR/config/settings.json"

    # Make pd_helix directory read-only so cp into it fails
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    chmod 555 "$KLIPPER_HOME/printer_data/config/helixscreen"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Original must still be intact — must not have been deleted
    [ -f "$INSTALL_DIR/config/settings.json" ]
    [ "$(cat "$INSTALL_DIR/config/settings.json")" = '{"important":true}' ]

    # Dangling symlink must not have been created
    [ ! -L "$INSTALL_DIR/config/settings.json" ]

    chmod 755 "$KLIPPER_HOME/printer_data/config/helixscreen"
}

# --- Graceful skip conditions ---

@test "skips when KLIPPER_HOME is empty" {
    KLIPPER_HOME=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
}

@test "skips when INSTALL_DIR is empty" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR=""
    mkdir -p "$KLIPPER_HOME/printer_data/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
}

@test "skips when printer_data/config does not exist" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME"
    mkdir -p "$INSTALL_DIR/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    [ ! -e "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

@test "skips when install config directory does not exist" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    [ ! -e "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

# --- Upgrade from old layout (directory symlink) ---

@test "migrates from old directory symlink layout" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"upgraded":true}' > "$INSTALL_DIR/config/settings.json"
    echo 'LOG_LEVEL=debug' > "$INSTALL_DIR/config/helixscreen.env"

    # Create old-style directory symlink
    ln -s "$INSTALL_DIR/config" "$KLIPPER_HOME/printer_data/config/helixscreen"
    [ -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Old symlink should be gone, replaced by real directory
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    # Files should be migrated to printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json")" = '{"upgraded":true}' ]

    # Install dir should have symlinks pointing back
    [ -L "$INSTALL_DIR/config/settings.json" ]
}

# --- Idempotency ---

@test "no-op when symlinks already correct" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Set up the correct layout manually
    echo '{}' > "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json"
    ln -s "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Should still be correct
    [ -L "$INSTALL_DIR/config/settings.json" ]
    [ "$(readlink "$INSTALL_DIR/config/settings.json")" = "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
}

@test "does not overwrite existing file in printer_data during migration" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # File already in printer_data (user edited it via Fluidd)
    echo '{"from_fluidd":true}' > "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json"
    # Old file still in install dir
    echo '{"from_install":true}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # printer_data file should NOT be overwritten
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json")" = '{"from_fluidd":true}' ]

    # Install dir file should be replaced with symlink
    [ -L "$INSTALL_DIR/config/settings.json" ]
}

# --- remove_config_symlink ---

@test "remove_config_symlink removes per-file symlinks from install dir" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Set up per-file symlinks
    echo '{}' > "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json"
    ln -s "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" "$INSTALL_DIR/config/settings.json"

    run remove_config_symlink
    [ "$status" -eq 0 ]

    # Symlink removed
    [ ! -L "$INSTALL_DIR/config/settings.json" ]

    # User files preserved in printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json" ]
}

@test "remove_config_symlink handles old directory symlink" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"

    # Old-style directory symlink
    ln -s "$INSTALL_DIR/config" "$KLIPPER_HOME/printer_data/config/helixscreen"

    run remove_config_symlink
    [ "$status" -eq 0 ]

    # Old symlink removed
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

@test "remove_config_symlink is safe when nothing exists" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    run remove_config_symlink
    [ "$status" -eq 0 ]
}

# --- Fresh-install marker interaction ---
#
# extract_release() leaves config/.helix-fresh-install when it kept the packaged
# config because no user config existed to restore. On Pi that verdict can be
# wrong: printer_data outlives the install dir, so a settings.json can still be
# sitting there when INSTALL_DIR is brand new.

@test "clears fresh-install marker when printer_data already holds a settings.json" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"
    # The user's real config, survivor of a previous install
    echo '{"user":true,"config_version":21}' \
        > "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json"
    # What extract_release just laid down on a fresh install dir
    echo '{"preset":"k1"}' > "$INSTALL_DIR/config/settings.json"
    touch "$INSTALL_DIR/config/.helix-fresh-install"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # The user's config wins, so the marker's premise was false and it must go.
    [ -L "$INSTALL_DIR/config/settings.json" ]
    grep -q '"user"' "$INSTALL_DIR/config/settings.json"
    [ ! -e "$INSTALL_DIR/config/.helix-fresh-install" ]
}

@test "keeps fresh-install marker when the packaged config is the one migrated" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Nothing in printer_data - a genuine first install on this machine
    echo '{"preset":"k1"}' > "$INSTALL_DIR/config/settings.json"
    touch "$INSTALL_DIR/config/.helix-fresh-install"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # The packaged config is what got migrated out, so the verdict still holds.
    grep -q '"preset"' "$KLIPPER_HOME/printer_data/config/helixscreen/settings.json"
    [ -f "$INSTALL_DIR/config/.helix-fresh-install" ]
}

@test "marker stays a real file in the install dir, never relocated or symlinked" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"preset":"k1"}' > "$INSTALL_DIR/config/settings.json"
    touch "$INSTALL_DIR/config/.helix-fresh-install"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # It is not in HELIX_USER_CONFIG_FILES, so it must not follow settings.json
    # out to printer_data. Living inside INSTALL_DIR is what makes Moonraker's
    # rmtree() take it with them.
    [ ! -L "$INSTALL_DIR/config/.helix-fresh-install" ]
    [ ! -e "$KLIPPER_HOME/printer_data/config/helixscreen/.helix-fresh-install" ]
}

@test "marker is not listed as a user config file" {
    # HELIX_USER_CONFIG_FILES entries get moved out to printer_data and replaced
    # by symlinks. The marker must never join them.
    for f in $HELIX_USER_CONFIG_FILES; do
        [ "$f" != ".helix-fresh-install" ]
    done
}

# --- Bundled installer parity ---

@test "bundled install.sh has setup_config_symlink function" {
    grep -q 'setup_config_symlink()' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh calls setup_config_symlink in main flow" {
    grep -q 'setup_config_symlink' "$WORKTREE_ROOT/scripts/install.sh"
}

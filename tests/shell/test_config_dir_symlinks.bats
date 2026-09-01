#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Protection for the user-writable config DIRECTORIES (#1164).
#
# A Mainsail one-click update makes Moonraker shutil.rmtree() the whole install
# dir before extracting the new ZIP. Anything that is a real directory under
# INSTALL_DIR/config/ is therefore destroyed — which is how users permanently
# lost config/custom_images/, their themes, and config/printer_database.d/.
#
# The fix extends the existing per-file symlink scheme to directories, because
# rmtree UNLINKS a symlink it meets as a child entry instead of descending
# through it. "rmtree unlinks a symlinked directory" is the load-bearing
# assumption of the whole design, so it is asserted here directly rather than
# taken on faith.
#
# Covers both halves of the mechanism:
#   setup_config_symlink()      scripts/lib/installer/platform.sh   (installer)
#   restore_config_symlinks()   config/refresh-service-units.sh     (post-update)

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

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

    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh"
    # shellcheck disable=SC1090
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    # common.sh defines the real log_* (stderr + ANSI). Restore the silent stubs.
    load helpers
}

# Lay out a Pi-shaped tree. Sets KLIPPER_HOME / INSTALL_DIR / PD_HELIX.
mk_tree() {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    PD_HELIX="$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
}

# Contents of a freshly extracted release ZIP's config/ directory.
extract_release_zip() {
    mkdir -p "$INSTALL_DIR/config/themes"
    mkdir -p "$INSTALL_DIR/config/printer_database.d"
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo '{"name":"helixscreen"}' > "$INSTALL_DIR/config/themes/helixscreen.json"
    echo '{"name":"nord"}'        > "$INSTALL_DIR/config/themes/nord.json"
    echo '# extensions'           > "$INSTALL_DIR/config/printer_database.d/README.md"
    echo '{}'                     > "$INSTALL_DIR/config/settings.json"
}

# ===========================================================================
# The load-bearing assumption
# ===========================================================================

@test "shutil.rmtree unlinks a symlinked directory instead of recursing into it" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    mk_tree

    mkdir -p "$PD_HELIX/custom_images"
    echo "user photo" > "$PD_HELIX/custom_images/my_printer.png"
    echo "[]"         > "$PD_HELIX/crash_history.json"
    ln -s "$PD_HELIX/custom_images"      "$INSTALL_DIR/config/custom_images"
    ln -s "$PD_HELIX/crash_history.json" "$INSTALL_DIR/config/crash_history.json"

    # Exactly what Moonraker's update_manager does to a type:web path.
    python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$INSTALL_DIR"

    [ ! -e "$INSTALL_DIR" ]
    # The far side of both symlinks is untouched.
    [ -f "$PD_HELIX/custom_images/my_printer.png" ]
    [ "$(cat "$PD_HELIX/custom_images/my_printer.png")" = "user photo" ]
    [ -f "$PD_HELIX/crash_history.json" ]
}

@test "a real directory under config/ is what rmtree actually destroys" {
    # The control case: without the fix, the same data is gone. If this ever
    # passes with the data intact, the bug being fixed does not exist.
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    mk_tree

    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "user photo" > "$INSTALL_DIR/config/custom_images/my_printer.png"

    python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$INSTALL_DIR"

    [ ! -e "$INSTALL_DIR/config/custom_images/my_printer.png" ]
}

# ===========================================================================
# setup_config_symlink — fresh install
# ===========================================================================

@test "fresh install symlinks every user config directory into printer_data" {
    mk_tree
    extract_release_zip

    run setup_config_symlink
    [ "$status" -eq 0 ]

    for d in custom_images themes printer_database.d; do
        [ -L "$INSTALL_DIR/config/$d" ]
        [ "$(readlink "$INSTALL_DIR/config/$d")" = "$PD_HELIX/$d" ]
        [ -d "$PD_HELIX/$d" ]
        [ ! -L "$PD_HELIX/$d" ]
    done
}

@test "fresh install carries the shipped directory contents into printer_data" {
    mk_tree
    extract_release_zip

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -f "$PD_HELIX/themes/helixscreen.json" ]
    [ -f "$PD_HELIX/themes/nord.json" ]
    [ -f "$PD_HELIX/printer_database.d/README.md" ]
    # Reachable through the install dir, which is the path the app uses.
    grep -q '"nord"' "$INSTALL_DIR/config/themes/nord.json"
}

@test "creates the directory symlink even when the release ships nothing for it" {
    # custom_images/ is empty in the ZIP; a link to an existing empty directory
    # is not dangling, so unlike files there is no reason to skip it.
    mk_tree
    echo '{}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/custom_images" ]
    [ -d "$INSTALL_DIR/config/custom_images" ]   # resolves — not dangling
}

# ===========================================================================
# setup_config_symlink — migrating an existing install that has real data
# ===========================================================================

@test "migration moves every existing file out of the install dir without loss" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images"
    mkdir -p "$INSTALL_DIR/config/themes"
    mkdir -p "$INSTALL_DIR/config/printer_database.d"
    echo "PNG1" > "$INSTALL_DIR/config/custom_images/voron.png"
    echo "PNG2" > "$INSTALL_DIR/config/custom_images/ender.png"
    echo "MY"   > "$INSTALL_DIR/config/themes/my-theme.json"
    echo "DB"   > "$INSTALL_DIR/config/printer_database.d/my-printer.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/custom_images/voron.png")" = "PNG1" ]
    [ "$(cat "$PD_HELIX/custom_images/ender.png")" = "PNG2" ]
    [ "$(cat "$PD_HELIX/themes/my-theme.json")" = "MY" ]
    [ "$(cat "$PD_HELIX/printer_database.d/my-printer.json")" = "DB" ]

    for d in custom_images themes printer_database.d; do
        [ -L "$INSTALL_DIR/config/$d" ]
    done
    # Still readable at the original path, through the link.
    [ "$(cat "$INSTALL_DIR/config/custom_images/voron.png")" = "PNG1" ]
}

@test "migration preserves nested subdirectories" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images/vendor/sub"
    echo "DEEP" > "$INSTALL_DIR/config/custom_images/vendor/sub/deep.png"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/custom_images/vendor/sub/deep.png")" = "DEEP" ]
    [ -L "$INSTALL_DIR/config/custom_images" ]
}

@test "migration never overwrites a file the user already has in printer_data" {
    mk_tree
    mkdir -p "$PD_HELIX/themes"
    echo "USER EDIT" > "$PD_HELIX/themes/nord.json"
    mkdir -p "$INSTALL_DIR/config/themes"
    echo "SHIPPED"   > "$INSTALL_DIR/config/themes/nord.json"
    echo "SHIPPED2"  > "$INSTALL_DIR/config/themes/helixscreen.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/themes/nord.json")" = "USER EDIT" ]
    # ...while a file the user does NOT have is still folded in.
    [ "$(cat "$PD_HELIX/themes/helixscreen.json")" = "SHIPPED2" ]
}

@test "migration completes a partially-migrated tree left by an interrupted run" {
    mk_tree
    # Half the files already made it across; the install dir still has the rest.
    mkdir -p "$PD_HELIX/custom_images"
    echo "A" > "$PD_HELIX/custom_images/a.png"
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "A" > "$INSTALL_DIR/config/custom_images/a.png"
    echo "B" > "$INSTALL_DIR/config/custom_images/b.png"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/custom_images/a.png")" = "A" ]
    [ "$(cat "$PD_HELIX/custom_images/b.png")" = "B" ]
    [ -L "$INSTALL_DIR/config/custom_images" ]
}

@test "migration is idempotent across repeated runs" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "KEEP" > "$INSTALL_DIR/config/custom_images/keep.png"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    run setup_config_symlink
    [ "$status" -eq 0 ]
    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/custom_images/keep.png")" = "KEEP" ]
    [ -L "$INSTALL_DIR/config/custom_images" ]
    [ "$(readlink "$INSTALL_DIR/config/custom_images")" = "$PD_HELIX/custom_images" ]
    # Exactly one copy — the migration did not duplicate anything.
    [ "$(find "$PD_HELIX/custom_images" -name 'keep.png' | wc -l)" -eq 1 ]
}

@test "a re-run recreates a target directory that vanished under a correct symlink" {
    mk_tree
    mkdir -p "$PD_HELIX"
    ln -s "$PD_HELIX/themes" "$INSTALL_DIR/config/themes"
    [ ! -d "$INSTALL_DIR/config/themes" ]   # dangling right now

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/themes" ]
    [ -d "$INSTALL_DIR/config/themes" ]     # resolves again
}

@test "a symlink pointing at a stale location is repointed at printer_data" {
    mk_tree
    ln -s "/old/install/config/themes" "$INSTALL_DIR/config/themes"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(readlink "$INSTALL_DIR/config/themes")" = "$PD_HELIX/themes" ]
}

@test "a plain file where a directory is expected is left alone" {
    mk_tree
    echo "not a directory" > "$INSTALL_DIR/config/themes"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/themes" ]
    [ ! -L "$INSTALL_DIR/config/themes" ]
    [ "$(cat "$INSTALL_DIR/config/themes")" = "not a directory" ]
}

# ===========================================================================
# setup_config_symlink — failure must never destroy data
# ===========================================================================

@test "keeps the real directory when printer_data cannot be written" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "IRREPLACEABLE" > "$INSTALL_DIR/config/custom_images/photo.png"

    mkdir -p "$PD_HELIX"
    chmod 555 "$PD_HELIX"

    run setup_config_symlink
    status_saved="$status"
    chmod 755 "$PD_HELIX"
    [ "$status_saved" -eq 0 ]

    # The original is still there and still readable — the pre-fix status quo,
    # which is strictly better than a half-migrated delete.
    [ -d "$INSTALL_DIR/config/custom_images" ]
    [ ! -L "$INSTALL_DIR/config/custom_images" ]
    [ "$(cat "$INSTALL_DIR/config/custom_images/photo.png")" = "IRREPLACEABLE" ]
}

@test "_safe_remove_migrated_config_dir refuses when a file did not reach printer_data" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images" "$PD_HELIX/custom_images"
    echo "A" > "$INSTALL_DIR/config/custom_images/a.png"
    echo "B" > "$INSTALL_DIR/config/custom_images/b.png"
    echo "A" > "$PD_HELIX/custom_images/a.png"          # b.png never copied

    run _safe_remove_migrated_config_dir "$INSTALL_DIR/config" custom_images "$PD_HELIX/custom_images"
    [ "$status" -ne 0 ]
    [ -f "$INSTALL_DIR/config/custom_images/b.png" ]
}

@test "_safe_remove_migrated_config_dir refuses a directory it does not manage" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/klipper_includes" "$PD_HELIX/klipper_includes"
    echo "X" > "$INSTALL_DIR/config/klipper_includes/macros.cfg"

    run _safe_remove_migrated_config_dir "$INSTALL_DIR/config" klipper_includes "$PD_HELIX/klipper_includes"
    [ "$status" -ne 0 ]
    [ -f "$INSTALL_DIR/config/klipper_includes/macros.cfg" ]
}

@test "_safe_remove_migrated_config_dir refuses a path whose parent is not config/" {
    mk_tree
    mkdir -p "$INSTALL_DIR/assets/themes" "$PD_HELIX/themes"
    echo "X" > "$INSTALL_DIR/assets/themes/t.json"
    echo "X" > "$PD_HELIX/themes/t.json"

    run _safe_remove_migrated_config_dir "$INSTALL_DIR/assets" themes "$PD_HELIX/themes"
    [ "$status" -ne 0 ]
    [ -f "$INSTALL_DIR/assets/themes/t.json" ]
}

@test "_safe_remove_migrated_config_dir refuses when the directory walk comes back empty" {
    # The walk decides whether everything was copied. On a stripped BusyBox a
    # missing find feature would report nothing — which must read as "I don't
    # know", never as "nothing left to copy, safe to delete".
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images" "$PD_HELIX/custom_images"
    echo "ONLY COPY" > "$INSTALL_DIR/config/custom_images/photo.png"
    mock_command_script find 'exit 1'

    run _safe_remove_migrated_config_dir "$INSTALL_DIR/config" custom_images "$PD_HELIX/custom_images"
    [ "$status" -ne 0 ]
    [ "$(cat "$INSTALL_DIR/config/custom_images/photo.png")" = "ONLY COPY" ]
}

@test "migration carries dotfiles across" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/printer_database.d"
    echo "HIDDEN" > "$INSTALL_DIR/config/printer_database.d/.local-overrides.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ "$(cat "$PD_HELIX/printer_database.d/.local-overrides.json")" = "HIDDEN" ]
    [ -L "$INSTALL_DIR/config/printer_database.d" ]
}

@test "_safe_remove_migrated_config_dir refuses a relative or traversing path" {
    mk_tree
    run _safe_remove_migrated_config_dir "relative/config" themes "$PD_HELIX/themes"
    [ "$status" -ne 0 ]
    run _safe_remove_migrated_config_dir "$INSTALL_DIR/../config" themes "$PD_HELIX/themes"
    [ "$status" -ne 0 ]
}

# ===========================================================================
# crash_history.json — a runtime-only file, so it needs a seed
# ===========================================================================

@test "migrates an existing crash_history.json out of the install dir" {
    mk_tree
    echo '[{"fingerprint":"abc"}]' > "$INSTALL_DIR/config/crash_history.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/crash_history.json" ]
    grep -q 'abc' "$PD_HELIX/crash_history.json"
}

@test "seeds crash_history.json so the first update cannot destroy it" {
    # The app only writes this file after install, so without a seed there is
    # nothing to link on install day and the next rmtree takes the real file.
    mk_tree
    echo '{}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/crash_history.json" ]
    [ -f "$PD_HELIX/crash_history.json" ]
    [ "$(cat "$PD_HELIX/crash_history.json")" = "[]" ]
}

@test "seeding does not resurrect the dangling-symlink behaviour for other files" {
    mk_tree
    echo '{}' > "$INSTALL_DIR/config/settings.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    [ ! -e "$INSTALL_DIR/config/tool_spools.json" ]
    [ ! -e "$INSTALL_DIR/config/.disabled_services" ]
}

# ===========================================================================
# remove_config_symlink (uninstall)
# ===========================================================================

@test "uninstall unlinks the directory symlinks and leaves the data in printer_data" {
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "PHOTO" > "$INSTALL_DIR/config/custom_images/p.png"
    setup_config_symlink

    run remove_config_symlink
    [ "$status" -eq 0 ]

    [ ! -L "$INSTALL_DIR/config/custom_images" ]
    [ "$(cat "$PD_HELIX/custom_images/p.png")" = "PHOTO" ]
}

# ===========================================================================
# restore_config_symlinks — the real function from refresh-service-units.sh
# ===========================================================================

# Load the real function bodies out of the shipped script. The script is not
# sourceable as a whole (set -e, systemctl, a 90s settle loop), so pull out the
# block between the config lists and the bare invocation at the bottom. Nothing
# is retyped here: a change to the shipped logic is a change to what runs.
load_restore_functions() {
    local extracted="$BATS_TEST_TMPDIR/restore_extract.sh"
    awk '/^HELIX_USER_CONFIG_FILES=/{on=1} /^restore_config_symlinks$/{on=0} on' \
        "$WORKTREE_ROOT/config/refresh-service-units.sh" > "$extracted"
    # Sanity: the markers must still exist, or we would be testing an empty file.
    grep -q 'restore_config_symlinks()' "$extracted"
    grep -q 'HELIX_USER_CONFIG_DIRS=' "$extracted"
    # shellcheck disable=SC1090
    . "$extracted"

    # Discovery scans absolute system paths; point it at the test tree instead.
    eval "discover_pd_helix() { printf '%s' '$1'; }"
}

@test "moonraker wipe then restore leaves every user directory intact" {
    # THE BUG. Full sequence: installed + migrated, Moonraker rmtree()s the
    # install dir, the new ZIP extracts, refresh-service-units restores.
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    mk_tree

    mkdir -p "$INSTALL_DIR/config/custom_images" "$INSTALL_DIR/config/printer_database.d"
    echo "MY PRINTER PHOTO" > "$INSTALL_DIR/config/custom_images/mine.png"
    echo '{"id":"mine"}'    > "$INSTALL_DIR/config/printer_database.d/mine.json"
    mkdir -p "$INSTALL_DIR/config/themes"
    echo '{"user":true}'    > "$INSTALL_DIR/config/themes/my-theme.json"
    echo '[{"crash":1}]'    > "$INSTALL_DIR/config/crash_history.json"
    echo '{"lang":"de"}'    > "$INSTALL_DIR/config/settings.json"

    setup_config_symlink

    # --- Moonraker one-click update ---
    python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$INSTALL_DIR"
    extract_release_zip

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    run restore_config_symlinks
    [ "$status" -eq 0 ]

    # Every payload the issue lists is still there, and reachable at the path
    # the app actually reads.
    [ "$(cat "$INSTALL_DIR/config/custom_images/mine.png")" = "MY PRINTER PHOTO" ]
    [ "$(cat "$INSTALL_DIR/config/printer_database.d/mine.json")" = '{"id":"mine"}' ]
    [ "$(cat "$INSTALL_DIR/config/themes/my-theme.json")" = '{"user":true}' ]
    [ "$(cat "$INSTALL_DIR/config/crash_history.json")" = '[{"crash":1}]' ]
    [ "$(cat "$INSTALL_DIR/config/settings.json")" = '{"lang":"de"}' ]

    for d in custom_images themes printer_database.d; do
        [ -L "$INSTALL_DIR/config/$d" ]
    done
}

@test "a second moonraker wipe after a restore is also survivable" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"
    mk_tree
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "PHOTO" > "$INSTALL_DIR/config/custom_images/mine.png"
    setup_config_symlink

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    for _ in 1 2 3; do
        python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1])" "$INSTALL_DIR"
        extract_release_zip
        restore_config_symlinks
    done

    [ "$(cat "$INSTALL_DIR/config/custom_images/mine.png")" = "PHOTO" ]
    [ -L "$INSTALL_DIR/config/custom_images" ]
}

@test "restore folds a newly shipped theme into printer_data before linking" {
    # A future release adding a bundled theme must still reach users whose
    # themes/ is now a symlink.
    mk_tree
    mkdir -p "$PD_HELIX/themes"
    echo '{"user":true}' > "$PD_HELIX/themes/my-theme.json"
    mkdir -p "$INSTALL_DIR/config/themes"
    echo '{"new":true}'  > "$INSTALL_DIR/config/themes/brand-new.json"

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    run restore_config_symlinks
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/themes" ]
    [ "$(cat "$PD_HELIX/themes/brand-new.json")" = '{"new":true}' ]
    [ "$(cat "$PD_HELIX/themes/my-theme.json")" = '{"user":true}' ]
}

@test "restore does not overwrite a user-edited copy with the shipped default" {
    mk_tree
    mkdir -p "$PD_HELIX/themes"
    echo '{"edited":true}' > "$PD_HELIX/themes/nord.json"
    mkdir -p "$INSTALL_DIR/config/themes"
    echo '{"shipped":true}' > "$INSTALL_DIR/config/themes/nord.json"

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    run restore_config_symlinks
    [ "$status" -eq 0 ]

    [ "$(cat "$INSTALL_DIR/config/themes/nord.json")" = '{"edited":true}' ]
}

@test "restore is idempotent" {
    mk_tree
    mkdir -p "$PD_HELIX/custom_images"
    echo "P" > "$PD_HELIX/custom_images/p.png"
    extract_release_zip

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    restore_config_symlinks
    restore_config_symlinks
    run restore_config_symlinks
    [ "$status" -eq 0 ]

    [ -L "$INSTALL_DIR/config/custom_images" ]
    [ "$(cat "$INSTALL_DIR/config/custom_images/p.png")" = "P" ]
    [ "$(readlink "$INSTALL_DIR/config/custom_images")" = "$PD_HELIX/custom_images" ]
}

@test "restore keeps the shipped directory when nothing can be written to printer_data" {
    [ "$(id -u)" -ne 0 ] || skip "running as root ignores the read-only mode"
    mk_tree
    mkdir -p "$PD_HELIX"
    extract_release_zip
    chmod 555 "$PD_HELIX"

    IDIR="$INSTALL_DIR"
    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    run restore_config_symlinks
    status_saved="$status"
    chmod 755 "$PD_HELIX"
    [ "$status_saved" -eq 0 ]

    [ -d "$INSTALL_DIR/config/themes" ]
    [ ! -L "$INSTALL_DIR/config/themes" ]
    [ -f "$INSTALL_DIR/config/themes/nord.json" ]
}

@test "restore refuses to delete under an install path that is not a helixscreen config dir" {
    # install_config_path_ok is the only guard on the rm -rf; a refusal must
    # leave the directory (and its contents) alone.
    mk_tree
    IDIR="$BATS_TEST_TMPDIR/printer_data"
    mkdir -p "$IDIR/config/themes"
    echo "USER MACRO" > "$IDIR/config/themes/keep.cfg"
    mkdir -p "$PD_HELIX"

    USER_VAL="pi"
    load_restore_functions "$PD_HELIX"

    run restore_config_symlinks
    [ "$status" -eq 0 ]

    [ -d "$IDIR/config/themes" ]
    [ ! -L "$IDIR/config/themes" ]
    [ "$(cat "$IDIR/config/themes/keep.cfg")" = "USER MACRO" ]
}

# ===========================================================================
# The two lists must not drift apart
# ===========================================================================

@test "installer and refresh script agree on the directory list" {
    local from_platform from_refresh
    from_platform=$(grep -m1 '^HELIX_USER_CONFIG_DIRS=' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh")
    from_refresh=$(grep -m1 '^HELIX_USER_CONFIG_DIRS=' "$WORKTREE_ROOT/config/refresh-service-units.sh")
    [ -n "$from_platform" ]
    [ "$from_platform" = "$from_refresh" ]
}

@test "installer and refresh script agree on the file list" {
    local from_platform from_refresh
    from_platform=$(grep -m1 '^HELIX_USER_CONFIG_FILES=' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh")
    from_refresh=$(grep -m1 '^HELIX_USER_CONFIG_FILES=' "$WORKTREE_ROOT/config/refresh-service-units.sh")
    [ -n "$from_platform" ]
    [ "$from_platform" = "$from_refresh" ]
}

@test "the bundled installer carries the directory protection" {
    # scripts/install.sh is generated from lib/installer/; a stale bundle ships
    # the old behaviour to everyone using curl | sh.
    grep -q '^HELIX_USER_CONFIG_DIRS=' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q '_safe_remove_migrated_config_dir()' "$WORKTREE_ROOT/scripts/install.sh"
    grep -q 'crash_history.json' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "the bundled uninstaller carries the directory protection" {
    grep -q '^HELIX_USER_CONFIG_DIRS=' "$WORKTREE_ROOT/scripts/uninstall.sh"
    grep -q '_safe_remove_migrated_config_dir()' "$WORKTREE_ROOT/scripts/uninstall.sh"
}

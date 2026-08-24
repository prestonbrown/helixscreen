#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for extract_release() safe rollback in scripts/lib/installer/release.sh

RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"

    # Source common.sh for file_sudo() which release.sh calls
    unset _HELIX_COMMON_SOURCED
    source scripts/lib/installer/common.sh

    # Stub _has_no_new_privs (defined in service.sh) — tests never run under
    # systemd's NoNewPrivileges, so always return false
    _has_no_new_privs() { return 1; }

    source "$RELEASE_SH"

    # Set up isolated test environment
    export TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export BACKUP_CONFIG=""
    export ORIGINAL_INSTALL_EXISTS=""

    mkdir -p "$TMP_DIR"
}

# Helper: create a valid test tarball containing a fake ELF binary
# The tarball extracts to helixscreen/ (relative)
#
# Must include every top-level entry that extract_release()'s Phase 2
# validation requires — currently bin/helix-screen, ui_xml/, assets/. Missing
# any of these is treated as a corrupt archive and the install aborts.
create_test_tarball() {
    local platform=${1:-ad5m}
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin"
    mkdir -p "$staging/helixscreen/config"
    mkdir -p "$staging/helixscreen/ui_xml"
    mkdir -p "$staging/helixscreen/assets"

    # Create appropriate fake ELF for the platform
    case "$platform" in
        ad5m|k1|pi32)
            create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
            ;;
        pi)
            create_fake_aarch64_elf "$staging/helixscreen/bin/helix-screen"
            ;;
    esac
    chmod +x "$staging/helixscreen/bin/helix-screen"

    # Create tarball
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

# Helper: create a tarball missing a specific top-level entry (e.g. "ui_xml",
# "assets"). Otherwise the same shape as create_test_tarball, so failures are
# attributable to the missing entry rather than other defects.
create_tarball_missing() {
    local platform=${1:-ad5m}
    local omit=$2
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin"
    mkdir -p "$staging/helixscreen/config"
    [ "$omit" = "ui_xml" ] || mkdir -p "$staging/helixscreen/ui_xml"
    [ "$omit" = "assets" ] || mkdir -p "$staging/helixscreen/assets"

    case "$platform" in
        ad5m|k1|pi32)
            create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
            ;;
        pi)
            create_fake_aarch64_elf "$staging/helixscreen/bin/helix-screen"
            ;;
    esac
    chmod +x "$staging/helixscreen/bin/helix-screen"

    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

# Helper: create a tarball with wrong architecture
create_wrong_arch_tarball() {
    local platform=${1:-ad5m}
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin"

    # Create wrong arch: if platform expects ARM32, give it AARCH64
    case "$platform" in
        ad5m|k1|pi32)
            create_fake_aarch64_elf "$staging/helixscreen/bin/helix-screen"
            ;;
        pi)
            create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
            ;;
    esac
    chmod +x "$staging/helixscreen/bin/helix-screen"

    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

# Helper: create a tarball without the binary
create_tarball_no_binary() {
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/config"
    echo '{}' > "$staging/helixscreen/config/settings.json"

    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

# Helper: build a PATH dir that mirrors all of /bin, /usr/bin, /usr/local/bin
# EXCEPT unzip — so extract_release() must fall through to the python zipfile
# extraction path. Every other command (df, awk, tar, gunzip, cp, mv, rm,
# python3, ...) stays available.
nounzip_path() {
    local b="$BATS_TEST_TMPDIR/nounzip"
    mkdir -p "$b"
    for d in /bin /usr/bin /usr/local/bin; do
        [ -d "$d" ] || continue
        for f in "$d"/*; do
            local n
            n=$(basename "$f")
            [ "$n" = unzip ] && continue
            [ -e "$b/$n" ] || ln -sf "$f" "$b/$n" 2>/dev/null || true
        done
    done
    echo "$b"
}

# Helper: build a FLAT zip (no top-level helixscreen/ dir) containing a real
# ARM32 ELF at bin/helix-screen, stored 0644 to exercise the bin/ exec-forcing
# logic in _py_unzip_extract. Uses python's zipfile so no `zip` binary needed.
make_flat_zip_with_elf() {
    local zip=$1
    local elf="$BATS_TEST_TMPDIR/elf_staging/helix-screen"
    mkdir -p "$(dirname "$elf")"
    create_fake_arm32_elf "$elf"
    python3 - "$zip" "$elf" <<'PY'
import sys, zipfile
zipname, elfpath = sys.argv[1], sys.argv[2]
with open(elfpath, "rb") as fh:
    elf = fh.read()
z = zipfile.ZipFile(zipname, "w")
def add(name, data, mode):
    zi = zipfile.ZipInfo(name)
    zi.external_attr = (mode & 0o7777) << 16
    z.writestr(zi, data)
add("bin/helix-screen", elf, 0o644)          # stored NON-executable on purpose
add("config/settings.json", b"{}", 0o644)
# extract_release validates a full release tree (#970): bin/helix-screen +
# ui_xml/ + assets/ must all be present or it rejects the archive.
add("ui_xml/main.xml", b"<view/>", 0o644)
add("assets/placeholder.txt", b"x", 0o644)
z.close()
PY
}

# Helper: set up a fake existing installation
setup_existing_install() {
    mkdir -p "$INSTALL_DIR/bin"
    mkdir -p "$INSTALL_DIR/config"
    echo "old binary" > "$INSTALL_DIR/bin/helix-screen"
    echo '{"old": true}' > "$INSTALL_DIR/config/settings.json"
}

# --- Fresh install tests ---

@test "extract_release: fresh install with correct arch succeeds" {
    create_test_tarball "ad5m"
    run extract_release "ad5m"
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
}

@test "extract_release: fresh install for pi with aarch64 binary succeeds" {
    create_test_tarball "pi"
    run extract_release "pi"
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
}

# --- Update with existing install ---

@test "extract_release: update replaces old install" {
    setup_existing_install
    create_test_tarball "ad5m"
    extract_release "ad5m"
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # Old install should be in .old
    [ -d "${INSTALL_DIR}.old" ]
}

@test "extract_release: config is preserved during update" {
    setup_existing_install
    create_test_tarball "ad5m"
    extract_release "ad5m"
    [ -f "$INSTALL_DIR/config/settings.json" ]
    # Config should contain old content
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
}

# --- Architecture mismatch with rollback ---

@test "extract_release: wrong arch preserves old install" {
    setup_existing_install
    create_wrong_arch_tarball "ad5m"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    # Old installation should still be in place (validation happens before swap)
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    [ -f "$INSTALL_DIR/config/settings.json" ]
}

@test "extract_release: wrong arch cleans up extract dir" {
    setup_existing_install
    create_wrong_arch_tarball "ad5m"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    # Extract dir should be cleaned up
    [ ! -d "$TMP_DIR/extract" ]
}

# --- Missing binary in tarball ---

@test "extract_release: missing binary in tarball preserves old install" {
    setup_existing_install
    create_tarball_no_binary
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    # Old installation should still be intact
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
}

# --- Incomplete archive (missing top-level dirs) ---
#
# Regression coverage for prestonbrown/helixscreen#970: an update that landed a
# tree without ui_xml/ wedged the device into a dead-loop with "Could not find
# HelixScreen data root". Extraction must reject such archives BEFORE touching
# the existing install, not after.

@test "extract_release: missing ui_xml in tarball preserves old install" {
    setup_existing_install
    # Seed the existing install with the dirs the new tree would replace, so
    # a regression (rm-then-discover-missing) would leave a bin/config-only
    # shell visible to the assertions.
    mkdir -p "$INSTALL_DIR/ui_xml" "$INSTALL_DIR/assets"
    touch "$INSTALL_DIR/ui_xml/sentinel" "$INSTALL_DIR/assets/sentinel"

    create_tarball_missing "ad5m" "ui_xml"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    # Old installation must remain entirely intact — bin, ui_xml, assets, config
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    [ -f "$INSTALL_DIR/ui_xml/sentinel" ]
    [ -f "$INSTALL_DIR/assets/sentinel" ]
    [ -f "$INSTALL_DIR/config/settings.json" ]
}

@test "extract_release: missing assets in tarball preserves old install" {
    setup_existing_install
    mkdir -p "$INSTALL_DIR/ui_xml" "$INSTALL_DIR/assets"
    touch "$INSTALL_DIR/ui_xml/sentinel" "$INSTALL_DIR/assets/sentinel"

    create_tarball_missing "ad5m" "assets"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    [ -f "$INSTALL_DIR/ui_xml/sentinel" ]
    [ -f "$INSTALL_DIR/assets/sentinel" ]
    [ -f "$INSTALL_DIR/config/settings.json" ]
}

@test "extract_release: missing ui_xml error message names the missing entry" {
    create_tarball_missing "ad5m" "ui_xml"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    echo "$output" | grep -q "ui_xml"
    echo "$output" | grep -q "incomplete"
}

@test "extract_release: incomplete archive cleans up extract dir" {
    create_tarball_missing "ad5m" "assets"
    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    [ ! -d "$TMP_DIR/extract" ]
}

@test "install.sh (bundled) has incomplete-archive guard" {
    grep -q 'Extracted archive is incomplete' \
        "$BATS_TEST_DIRNAME/../../scripts/install.sh"
}

# --- cleanup_old_install ---

@test "cleanup_old_install: removes .old directory when config restored" {
    mkdir -p "$INSTALL_DIR/config"
    echo '{"user": true}' > "$INSTALL_DIR/config/settings.json"
    mkdir -p "${INSTALL_DIR}.old/bin"
    echo "old" > "${INSTALL_DIR}.old/bin/helix-screen"
    ORIGINAL_INSTALL_EXISTS=true
    cleanup_old_install
    [ ! -d "${INSTALL_DIR}.old" ]
}

@test "cleanup_old_install: keeps .old when config missing after upgrade" {
    mkdir -p "$INSTALL_DIR/config"
    # No settings.json — config restore failed
    mkdir -p "${INSTALL_DIR}.old/config"
    echo '{"user": true}' > "${INSTALL_DIR}.old/config/settings.json"
    ORIGINAL_INSTALL_EXISTS=true
    cleanup_old_install
    # .old must be preserved as last-resort recovery
    [ -d "${INSTALL_DIR}.old" ]
    [ -f "${INSTALL_DIR}.old/config/settings.json" ]
}

@test "cleanup_old_install: no-op when .old does not exist" {
    run cleanup_old_install
    [ "$status" -eq 0 ]
}

# --- First install (no existing dir) ---

@test "extract_release: first install works with no existing dir" {
    [ ! -d "$INSTALL_DIR" ]
    create_test_tarball "ad5m"
    run extract_release "ad5m"
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # No .old should exist
    [ ! -d "${INSTALL_DIR}.old" ]
}

# --- Disk space pre-flight ---

@test "extract_release: fails gracefully when TMP_DIR has insufficient space" {
    # Override log stubs to write to stdout (bats 'run' only captures stdout)
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }

    create_test_tarball "ad5m"

    # Mock df to report very low space on TMP_DIR's filesystem
    mock_command_script "df" '
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
echo "tmpfs       51200     48640  2560       95% /tmp"
'

    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"Not enough space"* ]]
}

@test "extract_release: succeeds when TMP_DIR has adequate space" {
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }

    create_test_tarball "pi"

    # Mock df to report plenty of space
    mock_command_script "df" '
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
echo "/dev/sda1   1048576   0     1048576    0% /"
'

    run extract_release "pi"
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
}

@test "extract_release: error message suggests TMP_DIR override on space failure" {
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }

    create_test_tarball "ad5m"

    mock_command_script "df" '
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
echo "tmpfs       51200     48640  2560       95% /tmp"
'

    run extract_release "ad5m"
    [ "$status" -ne 0 ]
    [[ "$output" == *"TMP_DIR="* ]]
}

# --- Legacy config migration ---

# --- Non-root user with SUDO set (issue #34) ---

@test "extract_release: extracted files are user-owned not root-owned" {
    # Simulates non-root user scenario where SUDO would be set.
    # The key invariant: tar runs WITHOUT $SUDO, so extracted files
    # belong to the current user and can be read/deleted without elevation.
    create_test_tarball "pi"

    # Verify tar in extract_release does NOT use $SUDO by checking
    # extracted files are owned by current user
    extract_release "pi"

    # If extraction used $SUDO tar, files would be root-owned and this
    # user couldn't read them. The binary must be readable for arch validation.
    [ -r "$INSTALL_DIR/bin/helix-screen" ]
}

@test "extract_release: cleanup succeeds without sudo after arch mismatch" {
    # Regression test for issue #34: rm -rf cleanup must work without $SUDO
    # because extracted files should be user-owned (tar runs without $SUDO)
    create_wrong_arch_tarball "pi"

    run extract_release "pi"
    [ "$status" -ne 0 ]

    # Extract dir must be fully cleaned up — no leftover root-owned files
    [ ! -d "$TMP_DIR/extract" ]
}

@test "extract_release: cleanup succeeds without sudo after missing binary" {
    create_tarball_no_binary

    run extract_release "ad5m"
    [ "$status" -ne 0 ]

    # Extract dir must be fully cleaned up
    [ ! -d "$TMP_DIR/extract" ]
}

@test "extract_release: binary readable for architecture validation" {
    # The validate_binary_architecture function uses dd to read the binary.
    # If tar extracted as root, dd would fail with permission denied.
    create_test_tarball "pi32"

    run extract_release "pi32"
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
}

# --- User data restoration during upgrade ---

@test "extract_release: restores custom_images from previous install" {
    setup_existing_install
    mkdir -p "$INSTALL_DIR/config/custom_images"
    echo "my_printer.png" > "$INSTALL_DIR/config/custom_images/my_printer.png"

    create_test_tarball "ad5m"
    extract_release "ad5m"

    [ -d "$INSTALL_DIR/config/custom_images" ]
    [ -f "$INSTALL_DIR/config/custom_images/my_printer.png" ]
    grep -q "my_printer.png" "$INSTALL_DIR/config/custom_images/my_printer.png"
}

@test "extract_release: restores printer_database.d from previous install" {
    setup_existing_install
    mkdir -p "$INSTALL_DIR/config/printer_database.d"
    echo '{"custom": true}' > "$INSTALL_DIR/config/printer_database.d/my_printer.json"

    create_test_tarball "ad5m"
    extract_release "ad5m"

    [ -d "$INSTALL_DIR/config/printer_database.d" ]
    [ -f "$INSTALL_DIR/config/printer_database.d/my_printer.json" ]
    grep -q '"custom"' "$INSTALL_DIR/config/printer_database.d/my_printer.json"
}

@test "extract_release: does not overwrite new bundled config files with old ones" {
    setup_existing_install
    # Simulate a bundled file that exists in both old and new install
    echo '{"old_bundled": true}' > "$INSTALL_DIR/config/printer_database.json"

    # Create tarball that ships a new printer_database.json
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    echo '{"new_bundled": true}' > "$staging/helixscreen/config/printer_database.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"

    extract_release "ad5m"

    # New bundled file should win — not overwritten by old
    grep -q '"new_bundled"' "$INSTALL_DIR/config/printer_database.json"
}

@test "extract_release: restores multiple user data directories" {
    setup_existing_install
    mkdir -p "$INSTALL_DIR/config/custom_images"
    mkdir -p "$INSTALL_DIR/config/printer_database.d"
    mkdir -p "$INSTALL_DIR/config/themes"
    echo "img" > "$INSTALL_DIR/config/custom_images/test.png"
    echo '{}' > "$INSTALL_DIR/config/printer_database.d/custom.json"
    echo "theme" > "$INSTALL_DIR/config/themes/dark.json"

    create_test_tarball "ad5m"
    extract_release "ad5m"

    [ -f "$INSTALL_DIR/config/custom_images/test.png" ]
    [ -f "$INSTALL_DIR/config/printer_database.d/custom.json" ]
    [ -f "$INSTALL_DIR/config/themes/dark.json" ]
}

@test "extract_release: user data restored on first install is no-op" {
    # First install (no existing dir) — nothing to restore
    [ ! -d "$INSTALL_DIR" ]
    create_test_tarball "ad5m"
    run extract_release "ad5m"
    [ "$status" -eq 0 ]
    # Should succeed without errors, no user data dirs expected
    [ ! -d "$INSTALL_DIR/config/custom_images" ]
}

# --- Tarball default removal (config loss prevention) ---

@test "extract_release: tarball default settings.json removed before restore on upgrade" {
    setup_existing_install
    # Create a tarball that ships a default settings.json
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    echo '{"default_preset": true}' > "$staging/helixscreen/config/settings.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"

    extract_release "ad5m"

    # User's config should win, not the tarball default
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
    ! grep -q '"default_preset"' "$INSTALL_DIR/config/settings.json"
}

@test "extract_release: config missing after failed restore triggers recovery" {
    setup_existing_install
    # Create a tarball with a default settings.json
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    echo '{"default_preset": true}' > "$staging/helixscreen/config/settings.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"

    # Sabotage Phase 4 backup so Phase 6 restore has nothing to restore from
    # (simulates TMP_DIR backup failure)
    BACKUP_CONFIG=""

    extract_release "ad5m"

    # With the fix: tarball default is removed + restore fails = file is MISSING
    # This allows Config::init()'s restore_from_backup() to kick in.
    # The .old backup restore should still succeed from INSTALL_BACKUP though.
    # If .old is also gone (e.g. cleanup_old_install ran), file would be missing.
    # Test that the tarball default is NOT what's left behind:
    if [ -f "$INSTALL_DIR/config/settings.json" ]; then
        # If file exists, it must be from .old backup, not the tarball default
        refute grep -q '"default_preset"' "$INSTALL_DIR/config/settings.json"
    fi
}

@test "extract_release: fresh install keeps tarball default settings.json" {
    # On fresh install (no existing install), tarball defaults should be kept
    [ ! -d "$INSTALL_DIR" ]

    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    echo '{"default_preset": true}' > "$staging/helixscreen/config/settings.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    # Fresh install should keep the tarball defaults
    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"default_preset"' "$INSTALL_DIR/config/settings.json"
}

# --- Packaged preset survival (embedded targets) ---
#
# Platform release archives ship config/settings.json AS a platform preset
# (display rotation, hardware mapping) - see the release-k1/k2/cc1/ad5m/ad5x/
# snapmaker_u1 rules in mk/cross.mk. Embedded targets keep logs and cache under
# the install dir, so the directory already exists before a first install and
# its existence alone must not be read as "a user config is here to restore".

# Helper: an install dir that exists but holds no config at all - the K1/K2
# shape, where /usr/data/helixscreen/{logs,cache} predate the install.
setup_install_dir_without_config() {
    mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/cache"
    echo "boot log" > "$INSTALL_DIR/logs/helix.log"
}

# Helper: a tarball whose config/settings.json is a platform preset.
create_preset_tarball() {
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_arm32_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    printf '%s\n' '{"preset": "k1", "wizard_completed": false, "display": {"rotate": 270}}' \
        > "$staging/helixscreen/config/settings.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"
}

@test "extract_release: install dir exists without config keeps packaged preset" {
    setup_install_dir_without_config
    create_preset_tarball

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    # There was never a user config to restore, so the preset must survive.
    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"preset": "k1"' "$INSTALL_DIR/config/settings.json"
    grep -q '"rotate": 270' "$INSTALL_DIR/config/settings.json"
}

@test "extract_release: install dir exists without config leaves fresh-install marker" {
    setup_install_dir_without_config
    create_preset_tarball

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    # Tells Config::init() the packaged config is here on purpose rather than
    # because Moonraker's type:web rmtree() wiped the user's copy.
    [ -f "$INSTALL_DIR/config/.helix-fresh-install" ]
}

@test "extract_release: prior settings.json still wins over packaged preset" {
    setup_existing_install
    create_preset_tarball

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
    ! grep -q '"preset": "k1"' "$INSTALL_DIR/config/settings.json"
}

@test "extract_release: restored user config leaves no fresh-install marker" {
    setup_existing_install
    create_preset_tarball

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    # A restored user config is already versioned, and marking it fresh would
    # suppress backup recovery on a later Moonraker clobber.
    [ ! -f "$INSTALL_DIR/config/.helix-fresh-install" ]
}

@test "extract_release: fresh install marks packaged preset as deliberate" {
    [ ! -d "$INSTALL_DIR" ]
    create_preset_tarball

    run extract_release "ad5m"
    [ "$status" -eq 0 ]

    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"preset": "k1"' "$INSTALL_DIR/config/settings.json"
    [ -f "$INSTALL_DIR/config/.helix-fresh-install" ]
}

@test "install.sh (bundled) guards packaged-config removal on a restore candidate" {
    grep -q '_have_restore_candidate' \
        "$BATS_TEST_DIRNAME/../../scripts/install.sh"
    grep -q '.helix-fresh-install' \
        "$BATS_TEST_DIRNAME/../../scripts/install.sh"
}

# --- Legacy config migration ---

@test "extract_release: preserves legacy config location" {
    mkdir -p "$INSTALL_DIR/bin"
    echo "old" > "$INSTALL_DIR/bin/helix-screen"
    echo '{"legacy": true}' > "$INSTALL_DIR/settings.json"

    create_test_tarball "ad5m"
    extract_release "ad5m"
    # Config should be migrated to new location
    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"legacy"' "$INSTALL_DIR/config/settings.json"
}

# --- Zip extraction via python fallback when unzip absent ---

@test "extract_release: uses python zip fallback when unzip is absent" {
    command -v python3 >/dev/null 2>&1 || skip "python3 not available"

    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }

    make_flat_zip_with_elf "$TMP_DIR/helixscreen.zip"

    # Run extract_release with unzip hidden from PATH; the zip branch must fall
    # through to _py_unzip_extract. Re-source modules inside the subshell so the
    # restricted PATH (no unzip) is what they observe.
    run env PATH="$(nounzip_path)" /bin/bash -c "
        source tests/shell/helpers.bash
        log_error()   { echo \"ERROR: \$*\"; }
        log_info()    { echo \"INFO: \$*\"; }
        log_success() { echo \"OK: \$*\"; }
        log_warn()    { echo \"WARN: \$*\"; }
        unset _HELIX_COMMON_SOURCED _HELIX_RELEASE_SOURCED _PY_BIN _PY_PROBED
        source scripts/lib/installer/common.sh
        _has_no_new_privs() { return 1; }
        source scripts/lib/installer/release.sh
        export TMP_DIR='$TMP_DIR'
        export INSTALL_DIR='$INSTALL_DIR'
        export SUDO=''
        export _ARCHIVE_FORMAT=zip
        extract_release ad5m
    "
    [ "$status" -eq 0 ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    [ -x "$INSTALL_DIR/bin/helix-screen" ]
}

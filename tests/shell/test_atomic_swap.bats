#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for atomic swap vs in-place update during self-update.
# Verifies that extract_release() uses atomic swap when the parent dir is
# writable (new service file with ReadWritePaths for parent), and falls back
# to in-place content replacement when it's not.

RELEASE_SH="scripts/lib/installer/release.sh"

setup() {
    source tests/shell/helpers.bash
    export GITHUB_REPO="prestonbrown/helixscreen"

    # Source common.sh for file_sudo() which release.sh calls, and
    # host_profile.sh for the mod-ownership guard its update paths call
    # (production always ships both — bundle-installer.sh emits them in that
    # order).
    unset _HELIX_COMMON_SOURCED _HELIX_HOST_PROFILE_SOURCED
    source scripts/lib/installer/common.sh
    source scripts/lib/installer/host_profile.sh

    # Stub _has_no_new_privs (defined in service.sh) — tests never run under
    # systemd's NoNewPrivileges, so always return false
    _has_no_new_privs() { return 1; }

    source "$RELEASE_SH"

    export TMP_DIR="$BATS_TEST_TMPDIR/tmp"
    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    export SUDO=""
    export BACKUP_CONFIG=""
    export ORIGINAL_INSTALL_EXISTS=""

    mkdir -p "$TMP_DIR"

    # Override log stubs to write to stdout (bats 'run' captures stdout)
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }
}

# Helper: create a valid test tarball
create_test_tarball() {
    local platform=${1:-pi}
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin"
    mkdir -p "$staging/helixscreen/config"
    mkdir -p "$staging/helixscreen/ui_xml"
    mkdir -p "$staging/helixscreen/assets"
    echo "new content" > "$staging/helixscreen/ui_xml/test.xml"

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

# Helper: set up a fake existing installation
setup_existing_install() {
    mkdir -p "$INSTALL_DIR/bin"
    mkdir -p "$INSTALL_DIR/config"
    mkdir -p "$INSTALL_DIR/ui_xml"
    echo "old binary" > "$INSTALL_DIR/bin/helix-screen"
    echo '{"old": true}' > "$INSTALL_DIR/config/settings.json"
    echo "old xml" > "$INSTALL_DIR/ui_xml/old.xml"
}

# Mock _has_no_new_privs to simulate running under systemd NoNewPrivileges
mock_no_new_privs() {
    _has_no_new_privs() { return 0; }
}

# Mock _has_no_new_privs to simulate running without NoNewPrivileges (SSH)
mock_has_privs() {
    _has_no_new_privs() { return 1; }
}

# =============================================================================
# Atomic swap when parent dir is writable
# =============================================================================

@test "self-update: uses atomic swap when parent dir is writable" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    run extract_release "pi"
    [ "$status" -eq 0 ]

    # Atomic swap creates .old backup
    [ -d "${INSTALL_DIR}.old" ]
    # New binary is installed
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # Log indicates atomic swap was chosen
    [[ "$output" == *"Parent dir writable"* ]]
}

@test "self-update: atomic swap preserves config" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    extract_release "pi"

    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
}

@test "self-update: atomic swap replaces non-config contents" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    extract_release "pi"

    # New content should be present
    [ -f "$INSTALL_DIR/ui_xml/test.xml" ]
    grep -q "new content" "$INSTALL_DIR/ui_xml/test.xml"
    # Old non-config content should be in .old
    [ -f "${INSTALL_DIR}.old/ui_xml/old.xml" ]
}

# =============================================================================
# In-place fallback when parent dir is read-only
# =============================================================================

@test "self-update: falls back to in-place when parent dir is read-only" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    # Make parent dir read-only to force in-place path
    chmod a-w "$BATS_TEST_TMPDIR/opt"

    run extract_release "pi"

    # Restore permissions for cleanup
    chmod u+w "$BATS_TEST_TMPDIR/opt"

    [ "$status" -eq 0 ]
    # No .old directory (in-place doesn't create one)
    [ ! -d "${INSTALL_DIR}.old" ]
    # New binary is installed
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # Log indicates in-place path was used
    [[ "$output" == *"parent read-only"* ]]
}

@test "self-update: in-place preserves config directory" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    chmod a-w "$BATS_TEST_TMPDIR/opt"
    extract_release "pi"
    chmod u+w "$BATS_TEST_TMPDIR/opt"

    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
}

@test "self-update: in-place replaces non-config contents" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    chmod a-w "$BATS_TEST_TMPDIR/opt"
    extract_release "pi"
    chmod u+w "$BATS_TEST_TMPDIR/opt"

    [ -f "$INSTALL_DIR/ui_xml/test.xml" ]
    grep -q "new content" "$INSTALL_DIR/ui_xml/test.xml"
    # Old non-config file should be gone (deleted in-place)
    [ ! -f "$INSTALL_DIR/ui_xml/old.xml" ]
}

# =============================================================================
# In-place: new config defaults are merged
# =============================================================================

@test "self-update: in-place merges new config defaults" {
    setup_existing_install
    mock_no_new_privs

    # Create tarball with a new config file that doesn't exist in old install
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin" "$staging/helixscreen/config" \
             "$staging/helixscreen/ui_xml" "$staging/helixscreen/assets"
    create_fake_aarch64_elf "$staging/helixscreen/bin/helix-screen"
    chmod +x "$staging/helixscreen/bin/helix-screen"
    echo '{"new_default": true}' > "$staging/helixscreen/config/new_feature.json"
    tar -czf "$TMP_DIR/helixscreen.tar.gz" -C "$staging" helixscreen
    rm -rf "$staging"

    chmod a-w "$BATS_TEST_TMPDIR/opt"
    extract_release "pi"
    chmod u+w "$BATS_TEST_TMPDIR/opt"

    # New config default should be merged
    [ -f "$INSTALL_DIR/config/new_feature.json" ]
    grep -q '"new_default"' "$INSTALL_DIR/config/new_feature.json"
    # Existing config should be untouched
    [ -f "$INSTALL_DIR/config/settings.json" ]
    grep -q '"old"' "$INSTALL_DIR/config/settings.json"
}

# =============================================================================
# INSTALL_DIR not writable under NoNewPrivileges
# =============================================================================

@test "self-update: fails gracefully when INSTALL_DIR is read-only" {
    setup_existing_install
    create_test_tarball "pi"
    mock_no_new_privs

    # Make INSTALL_DIR itself read-only
    chmod a-w "$INSTALL_DIR"

    run extract_release "pi"

    # Restore for cleanup
    chmod u+w "$INSTALL_DIR"

    [ "$status" -ne 0 ]
    [[ "$output" == *"Cannot write to"* ]]
}

# =============================================================================
# Without NoNewPrivileges (SSH install) — always uses atomic swap
# =============================================================================

@test "SSH install: uses atomic swap regardless of parent writability" {
    setup_existing_install
    create_test_tarball "pi"
    mock_has_privs

    run extract_release "pi"
    [ "$status" -eq 0 ]

    # Atomic swap creates .old backup
    [ -d "${INSTALL_DIR}.old" ]
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # Should NOT mention parent dir test (skips NoNewPrivileges block entirely)
    lacks "Parent dir writable" "$output"
    [[ "$output" != *"parent read-only"* ]]
}

# =============================================================================
# Service template substitution for @@INSTALL_PARENT@@
# =============================================================================

@test "install_service_systemd: substitutes @@INSTALL_PARENT@@" {
    # Source service.sh module
    unset _HELIX_COMMON_SOURCED _HELIX_SERVICE_SOURCED
    WORKTREE_ROOT="$(cd "$(dirname "$BATS_TEST_DIRNAME")/.." && pwd)"
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/host_profile.sh" 2>/dev/null || true
    . "$WORKTREE_ROOT/scripts/lib/installer/service.sh"

    export INIT_SYSTEM="systemd"
    export SERVICE_NAME="helixscreen"
    export KLIPPER_USER="biqu"

    # Ensure config dir exists (setup() only creates TMP_DIR)
    mkdir -p "$INSTALL_DIR/config" "$INSTALL_DIR/bin"

    # Create service template with @@INSTALL_PARENT@@
    cat > "$INSTALL_DIR/config/helixscreen.service" << 'EOF'
[Service]
User=@@HELIX_USER@@
ReadWritePaths=@@INSTALL_DIR@@ @@INSTALL_PARENT@@ /var/log
EOF

    local fake_systemd="$BATS_TEST_TMPDIR/etc/systemd/system"
    mkdir -p "$fake_systemd"

    # Redirect SUDO to write to tmpdir instead of /etc/systemd/system
    local wrapper="$BATS_TEST_TMPDIR/bin/sudo_redirect"
    mkdir -p "$(dirname "$wrapper")"
    cat > "$wrapper" << 'SUDOEOF'
#!/bin/sh
new_args=""
for arg in "$@"; do
    case "$arg" in
        /etc/systemd/system/*)
            basename="${arg##*/}"
            arg="${FAKE_SYSTEMD_DIR}/${basename}"
            ;;
    esac
    new_args="$new_args \"$arg\""
done
eval $new_args
SUDOEOF
    chmod +x "$wrapper"
    SUDO="$wrapper"
    export FAKE_SYSTEMD_DIR="$fake_systemd"

    mock_command_script "systemctl" 'exit 0'

    install_service_systemd

    local dest="$fake_systemd/helixscreen.service"
    [ -f "$dest" ]
    # @@INSTALL_PARENT@@ should be replaced with dirname of INSTALL_DIR
    local expected_parent
    expected_parent="$(dirname "$INSTALL_DIR")"
    grep -q "ReadWritePaths=$INSTALL_DIR $expected_parent /var/log" "$dest"
    # No un-substituted @@INSTALL_PARENT@@ should remain
    ! grep -q '@@INSTALL_PARENT@@' "$dest"
}

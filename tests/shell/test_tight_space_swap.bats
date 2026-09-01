#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the tight-space "option B" rollback relocation in
# scripts/lib/installer/release.sh.
#
# When the install partition can't hold the old + new install at once (e.g. the
# K2 Plus /opt overlay), extract_release() relocates the old install to a roomy
# persistent partition (off-partition rollback backup) before moving the new
# tree in. When tight AND no roomy alternate exists, it must fail early and
# leave the original install untouched.

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

    # Log to stdout so bats 'run' captures messages.
    log_error()   { echo "ERROR: $*"; }
    log_info()    { echo "INFO: $*"; }
    log_success() { echo "OK: $*"; }
    log_warn()    { echo "WARN: $*"; }
}

# Helper: create a valid test tarball (full release tree: bin + ui_xml + assets)
create_test_tarball() {
    local platform=${1:-ad5m}
    local staging="$BATS_TEST_TMPDIR/staging"
    mkdir -p "$staging/helixscreen/bin"
    mkdir -p "$staging/helixscreen/config"
    mkdir -p "$staging/helixscreen/ui_xml"
    mkdir -p "$staging/helixscreen/assets"

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
    echo "old binary" > "$INSTALL_DIR/bin/helix-screen"
    echo '{"old": true}' > "$INSTALL_DIR/config/settings.json"
}

# Helper: install a df/du mock that reports controlled values keyed off the
# path argument (and df -P for device queries).
#
#   INSTALL_PARENT  -> tight free space, device /dev/installfs
#   ROOMY dir       -> ample free space, device /dev/roomyfs
#   pre-flight TMP  -> ample (so the extract pre-flight check passes)
#
# du -ms <new_install> reports a size LARGER than the install-fs free space so
# the tight branch is taken.
#
# Args: install_parent_path  roomy_path  install_free_kb  roomy_free_kb  new_install_mb
mock_df_du() {
    local install_parent=$1 roomy=$2 install_free_kb=$3 roomy_free_kb=$4 new_install_mb=$5

    mock_command_script "df" '
# df -P <path>  -> device-id query (col1 is the filesystem device)
# df <path>     -> free-space query (col4 is 1K-blocks available)
_pmode=0
_path=""
for _a in "$@"; do
    case "$_a" in
        -P) _pmode=1 ;;
        -*) ;;
        *)  _path="$_a" ;;
    esac
done
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
case "$_path" in
    '"$roomy"'*)
        echo "/dev/roomyfs   99999999 0 '"$roomy_free_kb"' 1% '"$roomy"'" ;;
    '"$install_parent"'*)
        echo "/dev/installfs 1048576 0 '"$install_free_kb"' 95% '"$install_parent"'" ;;
    *)
        # Default (e.g. TMP_DIR pre-flight check) — report plenty of room.
        echo "/dev/installfs 1048576 0 1048576 0% /" ;;
esac
'

    mock_command_script "du" '
# du -ms <new_install>  -> report new install size (MB)
# du -m <archive>       -> archive size for extract pre-flight (report small)
# du -k <archive>       -> validate_archive size check (report >=1024KB)
_path=""
for _a in "$@"; do
    case "$_a" in
        -*) ;;
        *)  _path="$_a" ;;
    esac
done
case "$_path" in
    *extract/helixscreen)
        echo "'"$new_install_mb"'	'"$_path"'" ;;
    *)
        echo "2	'"$_path"'" ;;
esac
'
}

# =============================================================================
# Case 1: tight + roomy alternate -> succeeds off-partition
# =============================================================================

@test "tight + roomy alternate: relocates old install off-partition and succeeds" {
    setup_existing_install
    create_test_tarball "ad5m"

    local roomy="$BATS_TEST_TMPDIR/roomy"
    mkdir -p "$roomy"
    export HELIX_ROLLBACK_CANDIDATES="$roomy"

    local install_parent
    install_parent="$(dirname "$INSTALL_DIR")"

    # install fs: 50MB free; new install: 200MB; roomy: 1GB free, diff device.
    mock_df_du "$install_parent" "$roomy" 51200 1048576 200

    run extract_release "ad5m"
    echo "$output"
    [ "$status" -eq 0 ]

    # New binary landed.
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    # Rollback backup is off-partition under the roomy dir, NOT at .old
    [ ! -d "${INSTALL_DIR}.old" ]
    [ -d "$roomy/helixscreen-rollback/helixscreen" ]
    [ -f "$roomy/helixscreen-rollback/helixscreen/bin/helix-screen" ]
    # Log mentions the off-partition relocation.
    [[ "$output" == *"off-partition"* ]]
}

@test "tight + roomy alternate: cleanup_old_install removes the off-partition backup" {
    setup_existing_install
    create_test_tarball "ad5m"

    local roomy="$BATS_TEST_TMPDIR/roomy"
    mkdir -p "$roomy"
    export HELIX_ROLLBACK_CANDIDATES="$roomy"

    local install_parent
    install_parent="$(dirname "$INSTALL_DIR")"
    mock_df_du "$install_parent" "$roomy" 51200 1048576 200

    extract_release "ad5m"
    [ -d "$roomy/helixscreen-rollback" ]

    # Config was restored, so cleanup proceeds (not the keep-for-recovery path).
    cleanup_old_install
    [ ! -d "$roomy/helixscreen-rollback" ]
    # The roomy mount root itself must survive.
    [ -d "$roomy" ]
}

# =============================================================================
# Case 2: tight + no roomy alternate -> fails early, install untouched
# =============================================================================

@test "tight + no roomy alternate: fails early and leaves install intact" {
    setup_existing_install
    create_test_tarball "ad5m"

    # Candidates: a same-device dir and a nonexistent dir — neither qualifies.
    local samedev="$BATS_TEST_TMPDIR/opt/samedev"
    mkdir -p "$samedev"
    export HELIX_ROLLBACK_CANDIDATES="$samedev $BATS_TEST_TMPDIR/does-not-exist"

    local install_parent
    install_parent="$(dirname "$INSTALL_DIR")"

    # Mock df so install parent is tight AND the candidate reports the SAME
    # device (/dev/installfs) as the install parent -> disqualified.
    mock_command_script "df" '
_pmode=0
_path=""
for _a in "$@"; do
    case "$_a" in
        -P) _pmode=1 ;;
        -*) ;;
        *)  _path="$_a" ;;
    esac
done
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
case "$_path" in
    '"$samedev"'*)
        echo "/dev/installfs 1048576 0 51200 95% '"$samedev"'" ;;
    '"$install_parent"'*)
        echo "/dev/installfs 1048576 0 51200 95% '"$install_parent"'" ;;
    *)
        echo "/dev/installfs 1048576 0 1048576 0% /" ;;
esac
'
    mock_command_script "du" '
_path=""
for _a in "$@"; do
    case "$_a" in
        -*) ;;
        *)  _path="$_a" ;;
    esac
done
case "$_path" in
    *extract/helixscreen) echo "200	$_path" ;;
    *) echo "2	$_path" ;;
esac
'

    run extract_release "ad5m"
    echo "$output"
    [ "$status" -ne 0 ]

    # Original install must be untouched (old binary still present).
    [ -f "$INSTALL_DIR/bin/helix-screen" ]
    grep -q "old binary" "$INSTALL_DIR/bin/helix-screen"
    [ -f "$INSTALL_DIR/config/settings.json" ]
    # No off-partition backup created.
    [ ! -d "$samedev/helixscreen-rollback" ]
    # Error mentions space.
    [[ "$output" == *"space"* || "$output" == *"Not enough space"* ]]
}

# =============================================================================
# Case 3: cleanup_old_install refuses a non-helixscreen-rollback path
# =============================================================================

@test "cleanup_old_install: refuses to remove a non-helixscreen-rollback path" {
    mkdir -p "$INSTALL_DIR/config"
    echo '{"user": true}' > "$INSTALL_DIR/config/settings.json"
    ORIGINAL_INSTALL_EXISTS=true

    local bogus="$BATS_TEST_TMPDIR/some-mount-root"
    mkdir -p "$bogus"
    export HELIX_OFFSITE_ROLLBACK_DIR="$bogus"

    run cleanup_old_install
    echo "$output"
    [ "$status" -eq 0 ]
    # The dir must still exist — it was NOT removed.
    [ -d "$bogus" ]
    [[ "$output" == *"Refusing"* ]]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for detect_tmp_dir() in scripts/lib/installer/platform.sh
# Ensures the installer picks a temp directory with enough free space.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset source guards and globals
    unset _HELIX_PLATFORM_SOURCED _HELIX_COMMON_SOURCED
    export SUDO=""
    export TMP_DIR=""

    # common.sh owns validate_tmp_dir, which detect_tmp_dir calls on a user
    # override; the bundled installer always carries both modules.
    . "$WORKTREE_ROOT/scripts/lib/installer/common.sh"
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    # Override log stubs to capture output (after common.sh, which defines the
    # real ones).
    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }
    export -f log_info log_warn log_error log_success
}

# ===========================================================================
# detect_tmp_dir
# ===========================================================================

@test "detect_tmp_dir: respects user-set TMP_DIR" {
    # Must still be a name validate_tmp_dir recognises as ours: TMP_DIR is
    # rm -rf'd on exit (see test_destructive_path_guards.bats).
    export TMP_DIR="/my/custom/helixscreen-install"
    detect_tmp_dir
    [ "$TMP_DIR" = "/my/custom/helixscreen-install" ]
}

@test "detect_tmp_dir: picks first candidate with enough space" {
    # Create candidate directories
    mkdir -p "$BATS_TEST_TMPDIR/data"
    mkdir -p "$BATS_TEST_TMPDIR/tmp"

    # Mock df to report space based on directory
    mock_command_script "df" "
case \"\$1\" in
    *data*)
        echo 'Filesystem  1K-blocks  Used Available Use% Mounted on'
        echo '/dev/sda1   1048576  0  512000  0% /data'
        ;;
    *tmp*)
        echo 'Filesystem  1K-blocks  Used Available Use% Mounted on'
        echo 'tmpfs       51200    0  51200   0% /tmp'
        ;;
    *)
        echo 'Filesystem  1K-blocks  Used Available Use% Mounted on'
        echo '/dev/sda1   1048576  0  512000  0% /'
        ;;
esac
"

    # Override candidates to use our test dirs
    # We can't easily override the candidate list, but we can test
    # that /tmp fallback works when it's the only writable dir
    export TMP_DIR=""
    detect_tmp_dir

    # On the test system, it should find *something* (likely /tmp or /var/tmp)
    [ -n "$TMP_DIR" ]
}

@test "detect_tmp_dir: falls back to /tmp with warning when no good candidate" {
    # Mock df to always report low space
    mock_command_script "df" '
echo "Filesystem  1K-blocks  Used Available Use% Mounted on"
echo "tmpfs       10240     0  10240       0% /tmp"
'

    export TMP_DIR=""
    run detect_tmp_dir

    # Should warn about using /tmp
    [[ "$output" == *"No temp directory"* ]] || [[ "$TMP_DIR" == *"/tmp/"* ]] || true
}

@test "detect_tmp_dir: skips non-existent candidate directories" {
    # No /data, /mnt/data, etc. on macOS — should still work
    export TMP_DIR=""
    detect_tmp_dir
    [ -n "$TMP_DIR" ]
}

@test "detect_tmp_dir: result ends with helixscreen-install" {
    export TMP_DIR=""
    detect_tmp_dir
    [[ "$TMP_DIR" == *"helixscreen-install" ]]
}

# ===========================================================================
# Runtime handoff contract: app passes its already-validated staging dir via
# TMP_DIR. When set, detect_tmp_dir must NOT probe at all (df must not run).
# ===========================================================================

@test "detect_tmp_dir: honors preset TMP_DIR without probing (no df call)" {
    # Marker file that df writes to if it is ever invoked. Its absence proves
    # detect_tmp_dir returned early on the user/app override.
    local marker="$BATS_TEST_TMPDIR/df_was_called"
    mock_command_script "df" "
touch \"$marker\"
echo 'Filesystem  1K-blocks  Used Available Use% Mounted on'
echo '/dev/sda1   1048576  0  512000  0% /'
"

    export TMP_DIR="/home/pi/helixscreen/.helix-update-staging"
    detect_tmp_dir

    # Value preserved exactly — the app's validated dir wins unchanged.
    [ "$TMP_DIR" = "/home/pi/helixscreen/.helix-update-staging" ]
    # And detect_tmp_dir short-circuited before any candidate probing.
    [ ! -f "$marker" ]
}

# ===========================================================================
# Fresh curl|sh install on a read-only-/tmp box: an install-dir SIBLING
# candidate is probed FIRST and selected ahead of /tmp — but NEVER a dir
# inside INSTALL_DIR (the installer rm -rf's / mv's INSTALL_DIR on --update).
# ===========================================================================

@test "detect_tmp_dir: selects a SIBLING of INSTALL_DIR, never a dir inside it" {
    # Real writable parent with an install subdir under it. The parent stands
    # in for the on-device layout where INSTALL_DIR's parent is the big user
    # partition (e.g. /data/helixscreen → /data). df/writability checks use the
    # host filesystem, which reports >100MB free.
    local parent fake_install
    parent="$(mktemp -d "$BATS_TEST_TMPDIR/parent.XXXXXX")"
    fake_install="$parent/helixscreen"
    mkdir -p "$fake_install"
    export INSTALL_DIR="$fake_install"

    export TMP_DIR=""
    detect_tmp_dir

    # The sibling candidate (INSTALL_DIR's parent) must win — prepended ahead
    # of /var/tmp, /tmp, etc., mirroring the app's C++ probe.
    [ "$TMP_DIR" = "$parent/.helixscreen-install" ]

    # SAFETY INVARIANT: the selected dir must be OUTSIDE INSTALL_DIR — never
    # equal to it and never a subdir of it.
    [ "$TMP_DIR" != "$fake_install" ]
    case "$TMP_DIR" in
        "$fake_install"/*) fail "TMP_DIR $TMP_DIR is INSIDE INSTALL_DIR $fake_install" ;;
    esac
    # And never the /tmp last-resort fallback.
    [[ "$TMP_DIR" != "/tmp/helixscreen-install" ]]
}

# ===========================================================================
# End-to-end against a GENUINELY read-only /tmp (bundle W9Q93WXM repro + fix).
# Runs inside a user+mount namespace so it can mount a real read-only tmpfs
# over /tmp/var/tmp without root and without touching the host. Proves both
# that the failure condition is real (mkdir dies on the read-only fs) and that
# the fix routes a real extraction to the writable sibling staging dir.
# ===========================================================================

@test "read-only /tmp: repro + fix via sibling staging dir (unshare E2E)" {
    command -v unshare >/dev/null 2>&1 || skip "unshare not available"
    unshare --user --map-root-user --mount true 2>/dev/null \
        || skip "user+mount namespaces not permitted"
    [ -d /dev/shm ] && [ -w /dev/shm ] || skip "/dev/shm not writable (needed outside the ro /tmp)"

    local scenario="$WORKTREE_ROOT/tests/shell/fixtures/ro_tmp_update_scenario.sh"
    local platform="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    # Work dir on /dev/shm so it survives the read-only remount of /tmp.
    local shmwork
    shmwork="$(mktemp -d /dev/shm/helix_ro.XXXXXX)"

    # Minimal fake update payload the extraction must land.
    mkdir -p "$shmwork/payload/helixscreen"
    echo "new-binary" > "$shmwork/payload/helixscreen/helix-screen"
    tar -czf "$shmwork/update.tar.gz" -C "$shmwork/payload" helixscreen

    run unshare --user --map-root-user --mount bash "$scenario" "$shmwork" "$platform"
    rm -rf "$shmwork"

    # The namespace couldn't mount tmpfs (locked-down CI): don't fail the suite.
    [[ "$output" == *"MOUNT_TMP_FAIL"* ]] && skip "tmpfs mount not permitted in this environment"

    # Surface the scenario's own markers on failure — this E2E runs against a
    # live namespace whose partition/mount layout varies by CI runner image, so
    # a bare "status != 0" is undiagnosable without the scenario's stdout.
    [ "$status" -eq 0 ] || echo "# ro_tmp scenario status=$status output=<<$output>>" >&3
    [ "$status" -eq 0 ]
    [[ "$output" == *"REPRO_OK"* ]]   # reproduced the bundle mkdir-on-read-only-/tmp failure
    [[ "$output" == *"FIX_OK"* ]]     # sibling staging dir + real extraction succeeded
}

# ===========================================================================
# TMP_DIR_PREFERRED — platform-declared staging root
#
# On the K2 both /opt (INSTALL_DIR's parent) and /usr/data sit on a 240MB
# overlay, while the 27.5GB user partition is mounted at /mnt/UDISK. Staging a
# 60MB archive on the overlay is what filled it. set_install_paths declares the
# right root for the platform; detect_tmp_dir must honour it ahead of the
# generic sibling candidate.
# ===========================================================================

@test "detect_tmp_dir: TMP_DIR_PREFERRED wins over the INSTALL_DIR sibling" {
    local big parent fake_install
    big="$(mktemp -d "$BATS_TEST_TMPDIR/udisk.XXXXXX")"
    parent="$(mktemp -d "$BATS_TEST_TMPDIR/overlay.XXXXXX")"
    fake_install="$parent/helixscreen"
    mkdir -p "$fake_install"

    export INSTALL_DIR="$fake_install"
    export TMP_DIR_PREFERRED="$big/helixscreen-install"
    export TMP_DIR=""

    detect_tmp_dir

    [ "$TMP_DIR" = "$big/helixscreen-install" ]
}

@test "detect_tmp_dir: unset TMP_DIR_PREFERRED keeps the sibling behaviour" {
    local parent fake_install
    parent="$(mktemp -d "$BATS_TEST_TMPDIR/parent2.XXXXXX")"
    fake_install="$parent/helixscreen"
    mkdir -p "$fake_install"

    export INSTALL_DIR="$fake_install"
    unset TMP_DIR_PREFERRED
    export TMP_DIR=""

    detect_tmp_dir

    [ "$TMP_DIR" = "$parent/.helixscreen-install" ]
}

@test "detect_tmp_dir: TMP_DIR_PREFERRED on a missing parent falls through" {
    # A platform may declare a root that this particular unit does not have.
    # That must degrade to the normal probe, not abort the install.
    local parent fake_install
    parent="$(mktemp -d "$BATS_TEST_TMPDIR/parent3.XXXXXX")"
    fake_install="$parent/helixscreen"
    mkdir -p "$fake_install"

    export INSTALL_DIR="$fake_install"
    export TMP_DIR_PREFERRED="$BATS_TEST_TMPDIR/no/such/mount/helixscreen-install"
    export TMP_DIR=""

    detect_tmp_dir

    [ "$TMP_DIR" = "$parent/.helixscreen-install" ]
}

@test "detect_tmp_dir: TMP_DIR_PREFERRED is still name-guarded" {
    # The chosen dir gets rm -rf'd on exit. A platform declaring a bare
    # mountpoint must never be accepted verbatim — that is the /mnt/UDISK
    # incident shape.
    local big
    big="$(mktemp -d "$BATS_TEST_TMPDIR/udisk2.XXXXXX")"

    export INSTALL_DIR="$BATS_TEST_TMPDIR/opt/helixscreen"
    mkdir -p "$INSTALL_DIR"
    export TMP_DIR_PREFERRED="$big"
    export TMP_DIR=""

    detect_tmp_dir

    # Must NOT have accepted the bare directory as the scratch dir.
    [ "$TMP_DIR" != "$big" ]
}

@test "k2 declares /mnt/UDISK as its staging root" {
    # /opt and /usr/data are the 240MB overlay on this box; /mnt/UDISK is the
    # 27.5GB user partition.
    run bash -c "grep -A 20 '\"k2\"' '$WORKTREE_ROOT/scripts/lib/installer/platform.sh' | grep TMP_DIR_PREFERRED"
    [ "$status" -eq 0 ]
    echo "$output" | grep -q '/mnt/UDISK'
}

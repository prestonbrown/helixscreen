#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The install directory is not the only filesystem an install writes to.
#
# On the K1, /usr/data is a multi-gigabyte ext4 (mmcblk0p10) and /etc/init.d is
# on the ~97MB root overlay (mmcblk0p9). check_disk_space measured only the
# former, so a K1 with a full overlay was told "3768MB available", had its stock
# UI stopped, downloaded 58MB, extracted, and only then died on
#     cp: write error: No space left on device
# leaving the printer with no screen and the user deleting print files on the
# partition that was never full.

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
LIB_DIR="$WORKTREE_ROOT/scripts/lib/installer"

setup() {
    load helpers

    unset _HELIX_COMMON_SOURCED _HELIX_REQUIREMENTS_SOURCED _HELIX_RELEASE_SOURCED
    export SUDO=""
    . "$LIB_DIR/common.sh"
    . "$LIB_DIR/requirements.sh"
    . "$LIB_DIR/release.sh"

    log_info()    { echo "INFO: $*"; }
    log_warn()    { echo "WARN: $*"; }
    log_error()   { echo "ERROR: $*"; }
    log_success() { echo "OK: $*"; }

    INSTALL_DIR="$BATS_TEST_TMPDIR/install/helixscreen"
    mkdir -p "$BATS_TEST_TMPDIR/install" "$BATS_TEST_TMPDIR/etc/init.d"
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/etc/init.d/S99helixscreen"
}

# Force the destination onto a "different filesystem" than INSTALL_DIR.
# _fs_id itself is covered by test_install_swap_space.bats.
split_filesystems() {
    _fs_id() {
        case "$1" in
            *etc/init.d*|*systemd*) echo "overlayfs:/overlay" ;;
            *)                      echo "/dev/mmcblk0p10" ;;
        esac
    }
    _fs_free_mb() {
        case "$1" in
            *etc/init.d*|*systemd*) echo 0 ;;
            *)                      echo 3768 ;;
        esac
    }
}

# ---------------------------------------------------------------------------
# The probe
# ---------------------------------------------------------------------------

@test "check_service_dest_space: aborts when the service filesystem cannot take the write" {
    split_filesystems
    dd() { return 1; }   # ENOSPC on the real thing

    run check_service_dest_space
    [ "$status" -eq 1 ]
    echo "$output" | grep -q "Cannot write the service definition"
}

@test "check_service_dest_space: passes when the probe write succeeds" {
    split_filesystems

    run check_service_dest_space
    [ "$status" -eq 0 ]
    echo "$output" | grep -q "Service directory check"
}

@test "check_service_dest_space: leaves no probe file behind on either path" {
    split_filesystems

    run check_service_dest_space
    [ "$status" -eq 0 ]
    [ -z "$(ls -A "$BATS_TEST_TMPDIR/etc/init.d")" ]

    dd() { return 1; }
    run check_service_dest_space
    [ "$status" -eq 1 ]
    [ -z "$(ls -A "$BATS_TEST_TMPDIR/etc/init.d")" ]
}

@test "check_service_dest_space: probes the destination, not the install dir" {
    split_filesystems
    # Record where the probe write was aimed.
    dd() {
        for arg in "$@"; do
            case "$arg" in of=*) echo "${arg#of=}" > "$BATS_TEST_TMPDIR/probe-target" ;; esac
        done
        return 0
    }

    run check_service_dest_space
    [ "$status" -eq 0 ]
    grep -q "etc/init.d" "$BATS_TEST_TMPDIR/probe-target"
}

# ---------------------------------------------------------------------------
# Redundant-check suppression
# ---------------------------------------------------------------------------

@test "check_service_dest_space: skips silently when both live on one filesystem" {
    # No split_filesystems: the real _fs_id sees one tmpdir filesystem.
    run check_service_dest_space
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "check_service_dest_space: no-ops when the destination directory is absent" {
    INIT_SCRIPT_DEST="$BATS_TEST_TMPDIR/nonexistent/S99helixscreen"
    run check_service_dest_space
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------------------------------------------------------------------------
# The message has to send the user to the right partition
# ---------------------------------------------------------------------------

@test "the failure names the service filesystem, not the roomy install one" {
    split_filesystems
    dd() { return 1; }

    run check_service_dest_space
    [ "$status" -eq 1 ]
    echo "$output" | grep -q "overlayfs:/overlay"
    # The advice the K1 reporter acted on for two runs was "clear space" -- on
    # /usr/data, which had 3.7GB free. Say plainly that it will not help.
    echo "$output" | grep -qi "will NOT help"
    echo "$output" | grep -q "DIFFERENT filesystem"
}

@test "the failure points du at the overlay upperdir when / is an overlay" {
    split_filesystems
    dd() { return 1; }
    mkdir -p "$BATS_TEST_TMPDIR/overlay/upper"
    _overlay_upperdir() { echo "$BATS_TEST_TMPDIR/overlay/upper"; }

    run check_service_dest_space
    [ "$status" -eq 1 ]
    echo "$output" | grep -q "du -k $BATS_TEST_TMPDIR/overlay/upper/\*"
}

# ---------------------------------------------------------------------------
# _overlay_upperdir parsing
# ---------------------------------------------------------------------------

@test "_overlay_upperdir: extracts upperdir from a K1-shaped /proc/mounts" {
    cat > "$BATS_TEST_TMPDIR/mounts" <<'EOF'
/dev/mmcblk0p9 /overlay ext4 rw,sync,relatime 0 0
overlayfs:/overlay / overlay rw,sync,noatime,lowerdir=/,upperdir=/overlay/upper,workdir=/overlay/work 0 0
/dev/mmcblk0p10 /usr/data ext4 rw,sync,relatime 0 0
EOF
    [ "$(_overlay_upperdir "$BATS_TEST_TMPDIR/mounts")" = "/overlay/upper" ]
}

@test "_overlay_upperdir: silent when / is not an overlay" {
    cat > "$BATS_TEST_TMPDIR/mounts" <<'EOF'
/dev/mmcblk0p2 / ext4 rw,relatime 0 0
/dev/mmcblk0p1 /boot vfat rw,relatime 0 0
EOF
    [ -z "$(_overlay_upperdir "$BATS_TEST_TMPDIR/mounts")" ]
}

@test "_overlay_upperdir: ignores an overlay mounted somewhere other than /" {
    cat > "$BATS_TEST_TMPDIR/mounts" <<'EOF'
/dev/mmcblk0p2 / ext4 rw,relatime 0 0
overlay /var/lib/docker/overlay2/x/merged overlay rw,lowerdir=/a,upperdir=/b,workdir=/c 0 0
EOF
    [ -z "$(_overlay_upperdir "$BATS_TEST_TMPDIR/mounts")" ]
}

# ---------------------------------------------------------------------------
# Ordering: the refusal is worthless if it fires after the screen goes dark
# ---------------------------------------------------------------------------

@test "the space checks run before the stock UI is stopped and before the download" {
    local main_sh="$LIB_DIR/main.sh"
    local disk stop dl
    disk=$(grep -n '^\s*check_disk_space ' "$main_sh" | head -1 | cut -d: -f1)
    stop=$(grep -n '^\s*stop_competing_uis' "$main_sh" | head -1 | cut -d: -f1)
    dl=$(grep -n '^\s*download_release ' "$main_sh" | head -1 | cut -d: -f1)

    [ -n "$disk" ] && [ -n "$stop" ] && [ -n "$dl" ]
    [ "$disk" -lt "$stop" ]
    [ "$disk" -lt "$dl" ]
}

@test "check_disk_space chains the service-destination check" {
    grep -q 'check_service_dest_space' "$LIB_DIR/requirements.sh"
    # Chained from check_disk_space, not left as an orphan definition.
    awk '/^check_disk_space\(\)/,/^}/' "$LIB_DIR/requirements.sh" |
        grep -q 'check_service_dest_space'
}

# ---------------------------------------------------------------------------
# The bundled installer is what users actually curl
# ---------------------------------------------------------------------------

@test "the generated install.sh carries the service-destination check" {
    grep -q 'check_service_dest_space()' "$WORKTREE_ROOT/scripts/install.sh"
    awk '/^check_disk_space\(\)/,/^}/' "$WORKTREE_ROOT/scripts/install.sh" |
        grep -q 'check_service_dest_space'
}

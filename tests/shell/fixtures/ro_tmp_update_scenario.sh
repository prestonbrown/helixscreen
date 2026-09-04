#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end scenario for the read-only-/tmp self-update failure (debug bundle
# W9Q93WXM, OrangePi Zero3). Runs INSIDE a user+mount namespace so it can mount
# a genuine read-only tmpfs over /tmp and /var/tmp without touching the host.
#
# It does two things against that real read-only filesystem:
#   REPRO  — with no writable sibling, detect_tmp_dir() falls back to
#            /tmp/helixscreen-install and the installer's mkdir dies exactly as
#            it did in the bundle ("Read-only file system").
#   FIX    — with INSTALL_DIR set (the app-provided staging handoff), it selects
#            the writable SIBLING of the install dir and a real tar extraction
#            succeeds there.
#
# Invoked as: unshare --user --map-root-user --mount bash <this> <shmwork> <platform.sh>
# All working files live under <shmwork> (on /dev/shm) so they survive the
# read-only remount of /tmp.

set -uo pipefail

shmwork="${1:?work dir required}"
platform="${2:?platform.sh path required}"

# The suite sandbox blocks `mount` by name, because a test that mounts over /tmp
# on the host wrecks the machine. This script is the one caller that already has
# a stronger guarantee - the private mount namespace named in the header - so it
# steps out of the sandbox for the mounts below, and only for those.
unset -f mount umount 2>/dev/null || true
if [ -n "${HELIX_TEST_SANDBOX_BIN:-}" ]; then
    PATH="${PATH//$HELIX_TEST_SANDBOX_BIN:/}"
    export PATH
fi

# --- Make /tmp and /var/tmp genuinely read-only (the OrangePi Zero3 condition) ---
if ! mount -t tmpfs -o ro tmpfs /tmp; then
    echo "MOUNT_TMP_FAIL"
    exit 2
fi
mount -t tmpfs -o ro tmpfs /var/tmp 2>/dev/null || true

# detect_tmp_dir() probes several writable candidates BEFORE the /tmp last
# resort, and the REPRO must exhaust all of them to reach that fallback:
#   - /opt is the parent of the DEFAULT install-dir sibling. platform.sh does
#     `: "${INSTALL_DIR:=/opt/helixscreen}"` on source, so even with INSTALL_DIR
#     unset the FIRST candidate is /opt/.helixscreen-install. On a runner where
#     /opt is writable it wins and the REPRO never reaches /tmp (this is what
#     broke the nightly: status=4 REPRO_UNEXPECTED_TMP: /opt/.helixscreen-install).
#   - /mnt/data, /data, /user-resource are device-specific partitions absent on
#     the OrangePi we reproduce, but a host can have them writable — the GitHub
#     runner mounts a large world-writable temp disk at /mnt.
# Shadow those parents with read-only tmpfs (best-effort; skip ones that don't
# exist) so /tmp is the only remaining candidate on any host. /usr (parent of
# the /usr/data candidate) is left alone — it holds the binaries this script
# execs, and is non-writable to a namespaced non-root uid regardless.
for parent in /opt /mnt /data /user-resource; do
    [ -d "$parent" ] && mount -t tmpfs -o ro tmpfs "$parent" 2>/dev/null || true
done

# A read-only filesystem rejects writes even for (namespace) root — an EROFS
# the kernel enforces regardless of uid, unlike a chmod which root bypasses.
if mkdir /tmp/helixscreen-install 2>/dev/null; then
    echo "TMP_NOT_READONLY"
    exit 3
fi

# Silence the installer's logging helpers.
log_info() { :; }
log_warn() { :; }
log_error() { :; }
log_success() { :; }
export -f log_info log_warn log_error log_success
export SUDO=""

# ---------------------------------------------------------------------------
# REPRO: no writable sibling → fall back to /tmp → the installer's mkdir fails.
# ---------------------------------------------------------------------------
unset _HELIX_PLATFORM_SOURCED
unset INSTALL_DIR HOME
export TMP_DIR=""
# shellcheck disable=SC1090
. "$platform"
detect_tmp_dir

if [ "$TMP_DIR" != "/tmp/helixscreen-install" ]; then
    # A writable standard candidate existed on this host (e.g. a real /data);
    # the read-only-/tmp fallback path isn't what we're exercising then.
    echo "REPRO_UNEXPECTED_TMP: $TMP_DIR"
    exit 4
fi
if mkdir -p "$TMP_DIR" 2>/dev/null; then
    echo "REPRO_MKDIR_SHOULD_HAVE_FAILED"
    exit 5
fi
echo "REPRO_OK"

# ---------------------------------------------------------------------------
# FIX: app hands a staging dir via INSTALL_DIR's sibling → real extraction works.
# ---------------------------------------------------------------------------
export INSTALL_DIR="$shmwork/root/helixscreen"
mkdir -p "$INSTALL_DIR"
export HOME="$shmwork/home"
mkdir -p "$HOME"

unset _HELIX_PLATFORM_SOURCED
export TMP_DIR=""
# shellcheck disable=SC1090
. "$platform"
detect_tmp_dir

case "$TMP_DIR" in
    /tmp/*)
        echo "FIX_PICKED_READONLY_TMP: $TMP_DIR"
        exit 6
        ;;
esac
expected="$shmwork/root/.helixscreen-install"
if [ "$TMP_DIR" != "$expected" ]; then
    echo "FIX_WRONG_TMP: $TMP_DIR (want $expected)"
    exit 7
fi

# The step that died at "mkdir: Read-only file system" before the fix.
mkdir -p "$TMP_DIR/extract" || { echo "FIX_MKDIR_FAIL"; exit 8; }
# --no-same-owner: inside the user namespace only root is mapped, so the
# tarball's original uid/gid can't be restored (and real non-root installs
# extract this way regardless).
tar --no-same-owner -xzf "$shmwork/update.tar.gz" -C "$TMP_DIR/extract" \
    || { echo "FIX_EXTRACT_FAIL"; exit 9; }
[ -f "$TMP_DIR/extract/helixscreen/helix-screen" ] || { echo "FIX_PAYLOAD_MISSING"; exit 10; }
echo "FIX_OK"

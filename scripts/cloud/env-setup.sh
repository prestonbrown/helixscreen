#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Cloud environment setup script — runs ONCE, as root, before the repo is
# cloned onto a Claude Code cloud session VM, and the whole filesystem is then
# snapshotted as the starting point of every later session on that
# environment. The platform requires this script to finish inside a few
# minutes and to exit 0 unconditionally, so every step here is best-effort:
# a step that fails leaves the corresponding piece of state absent rather
# than aborting the ones after it, and the session-start hook
# (scripts/cloud/session-start.sh) tolerates that absence.
#
# `--deps-only` restricts this to the apt install step and propagates its
# real exit status, so a CI caller (the build-cache workflow, via `sudo`) can
# wrap it in a retry and actually observe failure.
#
# No repo checkout exists while this runs — every path here is either a
# system path or an explicit clone/fetch of prestonbrown/helixscreen.
set -uo pipefail

LOG_FILE="/var/log/helix-env-setup.log"
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
touch "$LOG_FILE" 2>/dev/null || LOG_FILE="/tmp/helix-env-setup.log"
exec > >(tee -a "$LOG_FILE") 2>&1

log() {
    printf '[env-setup] %s\n' "$*"
}

DEPS_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --deps-only) DEPS_ONLY=1 ;;
    esac
done

# Single source of truth for the packages the native desktop build needs.
# Compare with the "Install dependencies" step in .github/workflows/build.yml
# and scripts/check-deps.sh before changing this list — both name the same
# build's requirements from a different angle.
APT_PACKAGES=(
    libsdl2-dev
    clang
    make
    python3
    python3-venv
    npm
    libssl-dev
    pkg-config
    libfmt-dev
    libcairo2-dev
    libpango1.0-dev
    libpng-dev
    libjpeg-dev
    librsvg2-dev
    libnl-3-dev
    libnl-genl-3-dev
    libxml2-utils
    libusb-1.0-0-dev
    libsystemd-dev
    libbluetooth-dev
    libasound2-dev
    shellcheck
    bats
    zstd
    ccache
    git
    curl
)

# Launchpad PPAs are unreachable through the platform's network policy, and
# the base image ships sources that point there: apt-get update aborts on the
# first 403 and nothing installs. Those sources are set aside under $1 (kept,
# not deleted) before the first update; Ubuntu's own sources stay.
# HELIX_APT_SOURCES_DIR lets a test point this at a scratch directory.
disable_blocked_apt_sources() {
    local kept="$1" dir="${HELIX_APT_SOURCES_DIR:-/etc/apt/sources.list.d}" f
    mkdir -p "$kept" 2>/dev/null || true
    for f in "$dir"/*.sources "$dir"/*.list; do
        [ -f "$f" ] || continue
        if grep -qiE 'launchpad(content)?\.net' "$f"; then
            log "apt: setting aside $(basename "$f") (Launchpad PPA, unreachable here)"
            mv "$f" "$kept/" 2>/dev/null || rm -f "$f"
        fi
    done
}

install_apt_packages() {
    disable_blocked_apt_sources "${STATE_DIR:-/opt/helix-cloud-env}/disabled-apt-sources"
    # A failed update still leaves the image's package lists usable.
    apt-get update || log "apt-get update failed; installing from the lists already on disk"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q "${APT_PACKAGES[@]}"
}

if [ "$DEPS_ONLY" = "1" ]; then
    log "Installing apt dependencies (--deps-only)..."
    install_apt_packages
    status=$?
    if [ "$status" -eq 0 ]; then
        log "apt install succeeded"
    else
        log "apt install failed (exit $status)"
    fi
    exit "$status"
fi

STATE_DIR="/opt/helix-cloud-env"
mkdir -p "$STATE_DIR" 2>/dev/null || true

APT_STATUS_FILE="/tmp/.helix-env-setup-apt-status"
SEED_STATUS_FILE="/tmp/.helix-env-setup-seed-status"
VENV_STATUS_FILE="/tmp/.helix-env-setup-venv-status"

# Block until Step A writes its status file, or the timeout elapses.
#
# Steps B and D need packages Step A installs (zstd, python3-venv) and each
# runs in its own backgrounded subshell — a subshell can only `wait` on its
# own children, never a sibling subshell's pid, so polling for the status
# file Step A writes is what stands in for that wait here.
wait_for_apt() {
    local deadline=$(( $(date +%s) + 180 ))
    while [ ! -f "$APT_STATUS_FILE" ]; do
        [ "$(date +%s)" -ge "$deadline" ] && return 1
        sleep 2
    done
    return 0
}

# --- Step A: apt packages (background) --------------------------------
step_apt() {
    log "Step A: installing apt packages..."
    if install_apt_packages; then
        echo 0 > "$APT_STATUS_FILE"
        log "Step A done: apt install succeeded"
    else
        echo "$?" > "$APT_STATUS_FILE"
        log "Step A done: apt install failed"
    fi
}

# --- Step C: seed clone for submodule alternates (background) ----------
step_seed() {
    log "Step C: seeding a full clone for submodule alternates..."
    rm -rf /opt/helixscreen-seed
    if timeout 240 git clone --recurse-submodules -j8 \
        https://github.com/prestonbrown/helixscreen /opt/helixscreen-seed; then
        echo "ok" > "$SEED_STATUS_FILE"
        log "Step C done: seed clone ready"
    else
        rm -rf /opt/helixscreen-seed
        echo "failed" > "$SEED_STATUS_FILE"
        log "Step C done: seed clone failed or timed out"
    fi
}

# --- Step D: prebuild the Python venv (background) ----------------------
step_venv() {
    log "Step D: prebuilding the Python venv..."
    wait_for_apt || log "Step D: apt install did not finish in time, trying anyway"
    if curl -fsSL --retry 3 \
        https://raw.githubusercontent.com/prestonbrown/helixscreen/main/requirements.txt \
        -o /tmp/helix-requirements.txt \
        && python3 -m venv /opt/helix-venv \
        && /opt/helix-venv/bin/pip install -r /tmp/helix-requirements.txt; then
        echo "ok" > "$VENV_STATUS_FILE"
        log "Step D done: venv ready"
    else
        echo "failed" > "$VENV_STATUS_FILE"
        log "Step D done: venv prebuild failed"
    fi
}

# ccache.conf: base_dir strips the repo checkout prefix from every source
# path so a build under a different absolute path (a CI runner, a later
# worktree) still hits objects this warmed. HELIX_CCACHE_BASE_DIR overrides it
# for a caller whose checkout does not live under /home/user (the build-cache
# workflow computes its own value for the runner's checkout instead).
mkdir -p /root/.config/ccache
cat > /root/.config/ccache/ccache.conf <<EOF
cache_dir = /root/.cache/ccache
base_dir = ${HELIX_CCACHE_BASE_DIR:-/home/user}
hash_dir = false
sloppiness = pch_defines,time_macros
compiler_check = content
max_size = 5G
EOF

step_apt &
PID_APT=$!
step_seed &
PID_SEED=$!
step_venv &
PID_VENV=$!

wait "$PID_APT" "$PID_SEED" "$PID_VENV" 2>/dev/null || true

APT_EXIT_CODE=$(cat "$APT_STATUS_FILE" 2>/dev/null || echo "unknown")
SEED_RESULT=$(cat "$SEED_STATUS_FILE" 2>/dev/null || echo "unknown")
VENV_RESULT=$(cat "$VENV_STATUS_FILE" 2>/dev/null || echo "unknown")

SEED_PRESENT="no"
[ -d /opt/helixscreen-seed/.git ] && SEED_PRESENT="yes"
VENV_PRESENT="no"
[ -x /opt/helix-venv/bin/python ] && VENV_PRESENT="yes"

CCACHE_STATS="ccache not installed"
if command -v ccache >/dev/null 2>&1; then
    CCACHE_STATS=$(ccache -s 2>/dev/null || echo "ccache -s failed")
fi

{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "apt_exit_code: $APT_EXIT_CODE"
    echo "ccache_tarball: not used (see BUILD_SYSTEM.md)"
    echo "seed_present: $SEED_PRESENT ($SEED_RESULT)"
    echo "venv_present: $VENV_PRESENT ($VENV_RESULT)"
    echo "---- ccache -s ----"
    echo "$CCACHE_STATS"
} > "$STATE_DIR/READY"

log "================ helix cloud env setup summary ================"
log "apt exit code:  $APT_EXIT_CODE"
log "ccache: installed and configured; no prebuilt cache is fetched"
log "seed clone:     $SEED_PRESENT ($SEED_RESULT)"
log "python venv:    $VENV_PRESENT ($VENV_RESULT)"
log "================================================================"

exit 0

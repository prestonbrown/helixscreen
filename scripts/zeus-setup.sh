#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# HelixScreen - zeus sanitizer host provisioning (see also scripts/zeus-run.sh)
#
# Paths come from $HOME, so there is nothing host-specific to edit; -j12 is zeus's
# ceiling because ZFS hands almost all its RAM to the ARC (see the comment below).
#
# Bootstrapping a fresh box: clone the repo to $HOME/helix-tsan/helixscreen, then
# run these from it. zeus-setup.sh also clones if the checkout is missing, so a
# standalone copy works too.
#
# Phase 1: repo on the host, image from the repo's Dockerfile, container from that image.
set -u
WORK=$HOME/helix-tsan
LOG=$WORK/setup.log
REPO=$WORK/helixscreen
IMAGE=helixscreen/sanitizer
exec >> "$LOG" 2>&1
echo "=== $(date -Is) setup start ==="

echo "--- can zeus reach the repo? ---"
git ls-remote https://github.com/prestonbrown/helixscreen HEAD 2>&1 | head -2 && echo "REMOTE_OK" || echo "REMOTE_FAIL"

# The clone comes first because the image is defined by a file inside it, so on a
# fresh box it has to happen host-side — there is no container yet to do it in.
#
# Updating it afterwards does NOT happen host-side. The container runs as root and
# writes into this bind mount, so parts of .git end up root-owned and a fetch as the
# host user dies on `cannot open '.git/FETCH_HEAD': Permission denied`. Root inside
# the container can write regardless of who owns what, so that is where the fetch
# goes. (scripts/zeus-run.sh fetches and hard-resets in-container on every run too,
# which is what actually keeps this checkout current.)
echo "--- clone / fetch ---"
if [ ! -d "$REPO/.git" ]; then
  git clone --recurse-submodules -q https://github.com/prestonbrown/helixscreen.git \
    "$REPO" && echo CLONE_OK || echo CLONE_FAIL
elif sudo -n docker inspect helix-tsan >/dev/null 2>&1; then
  sudo -n docker exec -w /work/helixscreen helix-tsan git fetch -q --all \
    && echo FETCH_OK || echo FETCH_FAIL
else
  # First run after a `docker rm`: nothing can fetch yet, so the image is built from
  # whatever Dockerfile is on disk. Re-running setup.sh once the container exists
  # picks up a newer one.
  echo "FETCH_SKIPPED (no container yet — image builds from the checkout as-is)"
fi

# One definition of the toolchain: docker/Dockerfile.sanitizer, in the repo. Installing
# packages into a bare ubuntu container instead puts the list in a second place, and the
# two drift — which is how this container came to link with the jammy mold 1.0.3 and
# serialize JSON against the ELF header (prestonbrown/helixscreen#1584).
echo "--- image ---"
sudo -n docker build -t "$IMAGE" -f "$REPO/docker/Dockerfile.sanitizer" "$REPO/docker" \
  && echo IMAGE_OK || echo IMAGE_FAIL

echo "--- container ---"
if ! sudo -n docker inspect helix-tsan >/dev/null 2>&1; then
  # CMD in the image is `sleep infinity`.
  sudo -n docker run -d --name helix-tsan -v "$WORK":/work -w /work "$IMAGE"
fi
echo "container: $(sudo -n docker ps --filter name=helix-tsan --format '{{.Status}}')"

# Ask the compiler that does the linking, not PATH: clang resolves ld.mold from its own
# program directory first, so a stale /usr/bin/ld.mold wins the link even with a newer
# one installed. A silent wrong answer here is the whole of #1584, so it is checked at
# provisioning time rather than discovered from a sanitizer abort weeks later.
echo "--- linker that will actually do the link ---"
sudo -n docker exec helix-tsan bash -lc '
  echo "int main(){return 0;}" > /tmp/t.cpp
  clang++ /tmp/t.cpp -o /tmp/t -fuse-ld=mold || exit 1
  readelf -p .comment /tmp/t | grep -i mold
  readelf -p .comment /tmp/t | grep -qi "mold 1\." && { echo "REFUSING: clang linked with mold 1.x"; exit 1; }
  exit 0
' && echo LINKER_OK || echo LINKER_FAIL

echo "=== $(date -Is) setup done ==="

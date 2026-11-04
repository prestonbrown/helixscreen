#!/usr/bin/env bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand up a test host for scripts/test-host-run.sh: any Linux box with Docker
# that you can ssh to with a key and run `sudo -n docker` on. Run this on the
# machine you develop on. It reads the same build-hosts file as test-host-run.sh
# (scripts/lib/build_hosts.sh) and does every step over ssh, so the host needs
# no clone of the repo.
#
#   scripts/test-host-setup.sh                    # image, container, checks
#   scripts/test-host-setup.sh --recreate         # replace the container with one from the new image
#   scripts/test-host-setup.sh --commit-checkout  # also clone HELIX_TEST_WORKDIR for --commit runs
#
# Safe to re-run. The image is built from this tree's docker/Dockerfile.sanitizer
# every time, so there is one definition of the toolchain; the container is
# created only when it is missing (or with --recreate).
#
# The container mounts one host directory at one container directory: the
# parents of HELIX_TEST_TREES_HOST and HELIX_TEST_TREES. Mirrors, the ccache and
# the --commit checkout all live under it, so the container holds nothing worth
# keeping and recreating it loses nothing.
#
# The container gets a memory limit with no swap: HELIX_TEST_MEMORY (a docker
# size such as 96g), else 60% of the host's memory. A build that outgrows it is
# killed inside the container rather than taking down the host's own services,
# and test-host-run.sh sizes -j from that limit. Every run applies it, so an
# existing container picks up a changed value without --recreate.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"
# shellcheck source-path=SCRIPTDIR source=lib/ccache_shared.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/ccache_shared.sh"
require_build_host HELIX_TEST_HOST || exit 2

RECREATE=0 CLONE=0
for a in "$@"; do
    case "$a" in
        --recreate) RECREATE=1 ;;
        --commit-checkout) CLONE=1 ;;
        *) sed -n '5,22p' "$0" | sed 's/^# \?//'; exit 2 ;;
    esac
done

HOST=$HELIX_TEST_HOST
CONTAINER="${HELIX_TEST_CONTAINER:-helix-test}"
TREES_HOST="${HELIX_TEST_TREES_HOST:-helix-test/trees}"
TREES="${HELIX_TEST_TREES:-/work/trees}"
CCACHE="${HELIX_TEST_CCACHE:-/work/ccache}"
WORKDIR="${HELIX_TEST_WORKDIR:-/work/helixscreen}"
LOCK_DIR="${HELIX_TEST_LOCK_DIR:-/tmp}"
IMAGE=helixscreen/sanitizer
WORK_HOST=$(dirname "$TREES_HOST")
WORK=$(dirname "$TREES")

step() { printf '\033[36m→\033[0m %s\n' "$*"; }
die()  { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST" "$@"; }

# One host directory is the container's one work directory, so the mirror root
# has the same name on both sides and everything the container keeps sits under
# it. A mirror root with no parent of its own would mount the whole home.
if [ "$WORK_HOST" = . ] || [ "$WORK_HOST" = / ]; then
    die "HELIX_TEST_TREES_HOST=$TREES_HOST needs a parent directory of its own (e.g. helix-test/trees)"
fi
[ "$(basename "$TREES_HOST")" = "$(basename "$TREES")" ] ||
    die "HELIX_TEST_TREES_HOST ($TREES_HOST) and HELIX_TEST_TREES ($TREES) must end in the same directory name"
for p in "$CCACHE" "$WORKDIR"; do
    case "$p" in "$WORK"/*) ;; *) die "$p is not under $WORK, the container's mounted directory" ;; esac
done

# Everything a run does on the host: docker and rsync under sudo -n (mirrors
# are root's), flock for the locks, comm and sort with -z for the stale-file pass.
step "$HOST: docker, sudo -n for it and for rsync, flock, comm -z"
on_host 'sudo -n docker info >/dev/null' ||
    die "$HOST needs Docker and passwordless sudo for docker (sudo -n docker info failed)"
on_host 'sudo -n rsync --version >/dev/null && sudo -n true' ||
    die "$HOST needs rsync and passwordless sudo for it, mv and rm (a run writes mirrors as root)"
on_host 'command -v flock >/dev/null && printf "a\0" | sort -z | comm -z -23 - /dev/null >/dev/null' ||
    die "$HOST needs flock and GNU sort/comm with -z"

# The build context goes over the ssh connection as a tar, so the host builds
# exactly this tree's Dockerfile. It is unpacked into a directory there first:
# buildx refuses a stdin context named together with -f ("ambiguous Dockerfile
# source").
step "image $IMAGE from docker/Dockerfile.sanitizer"
COPYFILE_DISABLE=1 tar -C "$(dirname "${BASH_SOURCE[0]}")/../docker" -cf - . |
    on_host "set -e; d=\$(mktemp -d); trap 'rm -rf \"\$d\"' EXIT
tar -xf - -C \"\$d\"
sudo -n docker build -q -t $IMAGE -f \"\$d/Dockerfile.sanitizer\" \"\$d\"" >/dev/null ||
    die "image build failed on $HOST"

step "container $CONTAINER: $HOST:$WORK_HOST mounted at $WORK"
on_host bash -se <<REMOTE || die "could not start container $CONTAINER on $HOST"
set -euo pipefail
mkdir -p "$TREES_HOST"
abs=\$(cd "$WORK_HOST" && pwd -P)
if [ "$RECREATE" = 1 ]; then
    # A held run lock is a job in that container; removing it kills the job.
    for lock in "$LOCK_DIR"/helix-test-host-run*.lock "$LOCK_DIR"/helix-zeus-run-*.lock; do
        [ -e "\$lock" ] || continue
        flock -n "\$lock" true || { echo "✗ a run holds \$lock: \$(tail -n 1 "\$lock")" >&2; exit 1; }
    done
    sudo -n docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
fi
if sudo -n docker inspect "$CONTAINER" >/dev/null 2>&1; then
    # A container made for another layout would take mirrors it cannot see.
    sudo -n docker inspect -f '{{range .Mounts}}{{.Source}}:{{.Destination}} {{end}}' "$CONTAINER" |
        grep -qF "\$abs:$WORK" || {
            echo "✗ $CONTAINER does not mount \$abs at $WORK; --recreate replaces it" >&2; exit 1; }
    # Without an init, every process docker exec leaves behind stays a zombie, and
    # anything that walks /proc (the wpa backend's tests do) slows by orders of magnitude.
    [ "\$(sudo -n docker inspect -f '{{.HostConfig.Init}}' "$CONTAINER")" = true ] ||
        echo "⚠ $CONTAINER runs without --init and accumulates zombies; --recreate replaces it" >&2
fi
mem="${HELIX_TEST_MEMORY:-}"
[ -n "\$mem" ] || mem="\$(awk '/^MemTotal:/ {printf "%dk", \$2 * 0.6}' /proc/meminfo)"
if ! sudo -n docker inspect "$CONTAINER" >/dev/null 2>&1; then
    # CMD in the image is sleep infinity, which reaps nothing: --init does.
    sudo -n docker run -d --init --name "$CONTAINER" -v "\$abs:$WORK" -w "$WORK" \
        --memory "\$mem" --memory-swap "\$mem" --pids-limit 16384 "$IMAGE" >/dev/null
else
    sudo -n docker update --memory "\$mem" --memory-swap "\$mem" --pids-limit 16384 "$CONTAINER" >/dev/null ||
        echo "⚠ could not set $CONTAINER's memory limit to \$mem" >&2
fi
echo "→ $CONTAINER memory limit \$mem, no swap"
sudo -n docker start "$CONTAINER" >/dev/null
REMOTE

if [ "$CLONE" = 1 ]; then
    url=$(git remote get-url origin)
    step "--commit checkout $WORKDIR from $url"
    on_host "sudo -n docker exec $CONTAINER bash -lc '[ -d $WORKDIR/.git ] || git clone -q --recurse-submodules $url $WORKDIR'" ||
        die "clone failed: the container needs to reach $url without credentials"
fi

step "ccache at $CCACHE shared by every tree under $WORK"
on_host "$(ccache_shared_cmd "$CONTAINER" "$CCACHE" apply "$WORK" "$HELIX_TEST_CCACHE_SIZE")" ||
    die "could not apply the shared ccache settings in $CONTAINER"

# Ask the compiler that does the linking, not PATH: clang resolves ld.mold from
# its own program directory first, so a stale /usr/bin/ld.mold wins the link
# even with a newer one installed, and mold 1.x serializes JSON against the ELF
# header (prestonbrown/helixscreen#1584).
step "the linker clang actually uses"
on_host "sudo -n docker exec $CONTAINER bash -lc '
    echo \"int main(){return 0;}\" > /tmp/t.cpp && clang++ /tmp/t.cpp -o /tmp/t -fuse-ld=mold || exit 1
    ! readelf -p .comment /tmp/t | grep -qi \"mold 1\\.\"'" ||
    die "clang links with mold 1.x (or cannot link at all) in $CONTAINER"

# The timezone test copies real TZif files into a fixture; without tzdata that
# one case fails, and one red case stops the mutation gate from establishing a
# baseline at all.
step "zoneinfo the suite copies from"
on_host "sudo -n docker exec $CONTAINER test -f /usr/share/zoneinfo/America/New_York" ||
    die "/usr/share/zoneinfo is missing in $CONTAINER (tzdata)"

step "ready: scripts/test-host-run.sh test '[netd]' builds and runs one tag there"

#!/usr/bin/env bash
# Run the mutation gate on zeus instead of on the box you are typing on.
#
# mutate_diff.py reverts one changed hunk at a time and rebuilds and re-runs the
# suite for each, so it is both the most expensive thing in the loop and the
# least interactive. thelio is routinely at load 30+ with several sessions
# building; zeus has 72 cores and is usually idle.
#
#   scripts/zeus-mutate.sh --base main --tests '[1543]'
#   scripts/zeus-mutate.sh --base main --only src/ui --limit 5
#
# Every argument after the script name goes to mutate_diff.py unchanged.
#
# The commit under test has to be pushed: the container fetches from GitHub, it
# does not take your working tree. That is the point - a mutation verdict about
# an unpushed tree is a verdict nobody else can reproduce.
set -euo pipefail

HOST="${ZEUS_HOST:-zeus}"
CONTAINER="${ZEUS_CONTAINER:-helix-tsan}"
WORKDIR="${ZEUS_WORKDIR:-/work/helixscreen}"
# 72 cores, but the job count is bounded by MEMORY, not cores: TrueNAS gives
# most of zeus's 251 GB to the ZFS ARC, leaving ~14 GB actually available, and
# ARC does not shrink fast enough for a burst of compilers. A -j48 build there
# dies with no error text at all - three compiles in, killed. Twelve fits.
JOBS="${ZEUS_JOBS:-12}"

SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)

if ! git branch -r --contains "$SHA" 2>/dev/null | grep -q .; then
    echo "✗ $SHORT is not on any remote branch — push it first, or zeus cannot fetch it" >&2
    exit 1
fi

LOG="${TMPDIR:-/tmp}/zeus-mutate-$SHORT.log"
echo "→ $HOST:$CONTAINER $WORKDIR @ $SHORT, make -j$JOBS, log $LOG"

# The heredoc runs on zeus. docker needs sudo -n there (pbrown is deliberately
# not in the docker group), and git inside the container looks at a host-owned
# checkout, hence safe.directory.
# shellcheck disable=SC2087  # client-side expansion is the point: the sha, the
# container name and the caller's mutate_diff arguments are resolved HERE, and
# the one value that has to stay server-side ($1 in D) is escaped.
ssh "$HOST" bash -se <<REMOTE | tee "$LOG"
set -euo pipefail
D() { sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache "$CONTAINER" bash -lc "\$1"; }

D 'git config --global --add safe.directory "*"' >/dev/null
D 'git fetch --quiet --all --recurse-submodules=on-demand'
D 'git reset --hard --quiet $SHA && git submodule update --init --recursive --quiet'
D 'git log --oneline -1'
D "python3 scripts/mutate_diff.py --jobs $JOBS $* 2>&1"
REMOTE

#!/usr/bin/env bash
# Run the expensive, non-interactive gates on zeus instead of the box you are
# typing on.
#
#   scripts/zeus-run.sh mutate --tests '[1543]'     # the mutation gate
#   scripts/zeus-run.sh asan '[1543]'               # AddressSanitizer, one tag
#   scripts/zeus-run.sh asan                        # AddressSanitizer, full suite
#   scripts/zeus-run.sh test '[netd]'               # plain suite, one tag
#
# Why these two in particular:
#
#   mutate  mutate_diff.py rebuilds and re-runs the suite once per changed hunk,
#           so it is the most expensive and least interactive thing in the loop.
#
#   asan    thelio's /etc/ld.so.preload holds libinput-config.so, so ASAN's
#           runtime loads second there and the binary produces NO test output and
#           exits 0 - a pass that ran nothing. The container has no
#           /etc/ld.so.preload, so ASAN works there with no workaround, and its
#           image matches CI's, which is why sanitizer findings reproduce.
#
# The commit under test has to be pushed: the container fetches it, it does not
# take your working tree. A verdict about an unpushed tree is one nobody else can
# reproduce.
set -euo pipefail

HOST="${ZEUS_HOST:-zeus}"
CONTAINER="${ZEUS_CONTAINER:-helix-tsan}"
WORKDIR="${ZEUS_WORKDIR:-/work/helixscreen}"

# Bounded by MEMORY, not cores. zeus reports 72 cores and 251 GB, and both
# mislead: TrueNAS gives almost all of that RAM to the ZFS ARC, leaving ~14 GB,
# and ARC does not release fast enough for a burst of compilers. A -j48 build
# there dies three compiles in with no error text. ASAN objects are larger
# again, so it gets less.
JOBS="${ZEUS_JOBS:-12}"
ASAN_JOBS="${ZEUS_ASAN_JOBS:-8}"

WHAT="${1:-}"
[ -n "$WHAT" ] || { sed -n '2,24p' "$0" | sed 's/^# \?//'; exit 2; }
shift

SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)
if ! git branch -r --contains "$SHA" 2>/dev/null | grep -q .; then
    echo "✗ $SHORT is not on any remote branch — push it first, or $HOST cannot fetch it" >&2
    exit 1
fi

case "$WHAT" in
    mutate) CMD="python3 scripts/mutate_diff.py --jobs $JOBS $*" ; JN=$JOBS ;;
    asan)   CMD="make test-asan-one TEST=\"${1:-}\" -j$ASAN_JOBS" ; JN=$ASAN_JOBS ;;
    test)   CMD="make test -j$JOBS && ./build/bin/helix-tests \"${1:-}\"" ; JN=$JOBS ;;
    *)      echo "✗ unknown job '$WHAT' (mutate | asan | test)" >&2; exit 2 ;;
esac

LOG="${TMPDIR:-/tmp}/zeus-$WHAT-$SHORT.log"
echo "→ $HOST:$CONTAINER $WORKDIR @ $SHORT, -j$JN, log $LOG"

# The heredoc runs on zeus. docker needs sudo -n there (pbrown is deliberately
# not in the docker group), and git inside the container looks at a host-owned
# checkout, hence safe.directory.
# shellcheck disable=SC2087  # client-side expansion is the point: the sha, the
# container name and the caller's arguments are resolved HERE, and the one value
# that has to stay server-side ($1 in D) is escaped.
ssh "$HOST" bash -se <<REMOTE | tee "$LOG"
set -euo pipefail
D() { sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache "$CONTAINER" bash -lc "\$1"; }

D 'git config --global --add safe.directory "*"' >/dev/null
D 'git fetch --quiet --all --recurse-submodules=on-demand'
D 'git reset --hard --quiet $SHA && git submodule update --init --recursive --quiet'
D 'git log --oneline -1'
D '$CMD 2>&1'
REMOTE

# A sanitizer run that produced no Catch2 summary ran nothing, whatever its exit
# code said. Refusing to call that a pass is the whole point of checking.
if [ "$WHAT" = asan ] && ! grep -qE 'All tests passed|test cases:|assertions:' "$LOG"; then
    echo ""
    echo "✗ no Catch2 summary in $LOG — the suite did not run, so this is not a clean ASAN result" >&2
    exit 1
fi

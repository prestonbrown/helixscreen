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

# zeus reports 72 cores and 251 GB and both mislead: TrueNAS hands almost all of
# that RAM to the ZFS ARC, which leaves ~13 GB for everything else and does not
# evict fast enough for a burst of compilers starting at once. There is no swap,
# so the overshoot goes straight to the OOM killer and a compile dies with no
# error text.
#
# So the run caps zfs_arc_max for its duration and restores it afterwards. ARC
# returns the memory in under ten seconds (219 GB -> 64 GB frees ~155 GB), which
# costs the pools their cache while the job runs and gives the compilers room.
# The job count is then derived from what is actually available rather than
# guessed, so a run that could NOT cap (no sudo, no ZFS) still gets a safe small
# number instead of an OOM.
# A compile peaks near 400 MB here, ASAN included, so no single process is the
# problem - the total is. At 1 GB per job the formula lands on ~72 with the cap
# applied and on ~13 without it, which is the number this ran at before the cap
# existed, so a run that cannot cap is no worse off than it was.
ARC_CAP_GB="${ZEUS_ARC_CAP_GB:-64}"     # 0 disables the cap entirely
GB_PER_JOB="${ZEUS_GB_PER_JOB:-1}"      # asan overrides to 1.5 below

WHAT="${1:-}"
[ -n "$WHAT" ] || { sed -n '2,24p' "$0" | sed 's/^# \?//'; exit 2; }
shift

SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)
if ! git branch -r --contains "$SHA" 2>/dev/null | grep -q .; then
    echo "✗ $SHORT is not on any remote branch — push it first, or $HOST cannot fetch it" >&2
    exit 1
fi

# $HELIX_J is resolved on zeus, after the cap, from the memory that is then free.
# shellcheck disable=SC2016  # $HELIX_J must reach zeus unexpanded
case "$WHAT" in
    mutate) CMD='python3 scripts/mutate_diff.py --jobs $HELIX_J '"$*" ;;
    asan)   CMD='make test-asan-one TEST="'"${1:-}"'" -j$HELIX_J' ; GB_PER_JOB=1.5 ;;
    test)   CMD='make test -j$HELIX_J && ./build/bin/helix-tests "'"${1:-}"'"' ;;
    *)      echo "✗ unknown job '$WHAT' (mutate | asan | test)" >&2; exit 2 ;;
esac

LOG="${TMPDIR:-/tmp}/zeus-$WHAT-$SHORT.log"
echo "→ $HOST:$CONTAINER $WORKDIR @ $SHORT, ARC cap ${ARC_CAP_GB}GB, log $LOG"

# The heredoc runs on zeus. docker needs sudo -n there (pbrown is deliberately
# not in the docker group), and git inside the container looks at a host-owned
# checkout, hence safe.directory.
# shellcheck disable=SC2087  # client-side expansion is the point: the sha, the
# container name and the caller's arguments are resolved HERE, and the one value
# that has to stay server-side ($1 in D) is escaped.
ssh "$HOST" bash -se <<REMOTE | tee "$LOG"
set -euo pipefail

# --- ZFS ARC: borrow the RAM for the duration, hand it back on any exit -------
# The marker records "<pid> <value to restore>". Liveness is derived from that
# pid, never asserted: a run that died without restoring leaves a marker whose
# pid is gone, and the next run recovers from it. Asserting instead would let one
# crash cap this NAS permanently, since every later run would read the CAPPED
# value as the original.
ARC_PARAM=/sys/module/zfs/parameters/zfs_arc_max
ARC_MARK=/tmp/.helix-zeus-arc-orig
ARC_HELD=no

arc_write() { sudo -n sh -c "echo \$1 > \$ARC_PARAM" 2>/dev/null; }

arc_restore() {
    [ "\$ARC_HELD" = yes ] || return 0
    _orig=\$(awk '{print \$2}' "\$ARC_MARK" 2>/dev/null || echo 0)
    arc_write "\${_orig:-0}" || true
    sudo -n rm -f "\$ARC_MARK" 2>/dev/null || true
    echo "→ zfs_arc_max restored to \${_orig:-0}"
}
trap arc_restore EXIT INT TERM HUP

if [ -e "\$ARC_MARK" ]; then
    _mpid=\$(awk '{print \$1}' "\$ARC_MARK" 2>/dev/null)
    if [ -n "\$_mpid" ] && kill -0 "\$_mpid" 2>/dev/null; then
        echo "→ zeus-run pid \$_mpid already holds the ARC cap; leaving it alone"
        ARC_CAP_GB=0
    else
        _stale=\$(awk '{print \$2}' "\$ARC_MARK" 2>/dev/null)
        echo "→ recovering ARC cap abandoned by dead pid \${_mpid:-?}; restoring \${_stale:-0}"
        arc_write "\${_stale:-0}" || true
        sudo -n rm -f "\$ARC_MARK" 2>/dev/null || true
    fi
fi

if [ "\${ARC_CAP_GB:-$ARC_CAP_GB}" -gt 0 ] && [ -r "\$ARC_PARAM" ]; then
    _orig=\$(cat "\$ARC_PARAM")
    if sudo -n sh -c "echo '\$\$ \$_orig' > \$ARC_MARK" 2>/dev/null &&
       arc_write "\$(( $ARC_CAP_GB * 1024 * 1024 * 1024 ))"; then
        ARC_HELD=yes
        sleep 10   # ARC evicts to the new ceiling in well under this
        echo "→ zfs_arc_max \$_orig -> ${ARC_CAP_GB}GB for this run"
    else
        echo "→ could not cap zfs_arc_max; sizing jobs for memory as-is" >&2
        sudo -n rm -f "\$ARC_MARK" 2>/dev/null || true
    fi
fi

# Derive the job count from what is free NOW, bounded by cores. A run that could
# not cap lands on a small number here rather than OOMing at -j48.
HELIX_J=\$(awk -v per=$GB_PER_JOB -v cpus="\$(nproc)" '
    /^MemAvailable/ { j = int((\$2/1048576) / per); if (j > cpus) j = cpus; if (j < 4) j = 4; print j }
' /proc/meminfo)
echo "→ MemAvailable \$(awk '/^MemAvailable/{printf "%.0fGB", \$2/1048576}' /proc/meminfo), using -j\$HELIX_J"

D() { sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache -e HELIX_J="\$HELIX_J" "$CONTAINER" bash -lc "\$1"; }

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

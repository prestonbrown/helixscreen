#!/usr/bin/env bash
# Run the expensive, non-interactive gates on zeus instead of the box you are
# typing on.
#
#   scripts/zeus-run.sh mutate --tests '[1543]'     # the mutation gate
#   scripts/zeus-run.sh asan '[1543]'               # AddressSanitizer, one tag
#   scripts/zeus-run.sh asan                        # AddressSanitizer, full suite, sharded as CI runs it
#   scripts/zeus-run.sh tsan '[ams]'                # ThreadSanitizer, one tag
#   scripts/zeus-run.sh tsan                        # ThreadSanitizer, full suite, sharded
#   scripts/zeus-run.sh test '[netd]'               # plain suite, one tag
#   scripts/zeus-run.sh sweep                       # make unit-sweep, sharded
#   scripts/zeus-run.sh asan-app help-qr --repeat 50  # the APP under ASAN
#   scripts/zeus-run.sh tsan-app help-qr --repeat 50  # the APP under TSan
#
# The app modes are here for the same reason asan is: the run is long and
# non-interactive, and the container's image (SDL, no ld.so.preload) is the
# only place an instrumented desktop app runs cleanly.
#
# A whole-suite C++ verdict comes from sweep, which shards the way CI and the
# local gate do. test with no tag runs the suite in one process, where
# cross-test contamination fails cases no branch touched, so it is not a gate.
# bats is not run here: the container is root with no shellcheck, so the shell
# suite fails on its environment; run `make test-shell` on thelio instead.
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

HOST="${ZEUS_HOST:-zeus.local}"   # bare `zeus` does not resolve from thelio
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
[ -n "$WHAT" ] || { sed -n '2,35p' "$0" | sed 's/^# \?//'; exit 2; }
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
    asan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            # Trailing args become make overrides, so ASAN_RUN_OPTIONS can be
            # tuned per run (quarantine_size_mb keeps freed blocks poisoned, which
            # turns a recycled-memory SEGV into a heap-use-after-free report).
            # No tag is the nightly's own sharded run, leak ratchet included.
            if [ -z "$_tag" ]; then
                CMD='make test-asan -j$HELIX_J '"$*"
            else
                CMD='make test-asan-one TEST="'"$_tag"'" -j$HELIX_J '"$*"
            fi
            GB_PER_JOB=1.5 ;;
    tsan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            if [ -z "$_tag" ]; then
                CMD='make test-tsan -j$HELIX_J '"$*"
            else
                CMD='make test-tsan-one TEST="'"$_tag"'" -j$HELIX_J '"$*"
            fi
            GB_PER_JOB=1.5 ;;
    test)   CMD='make test -j$HELIX_J && ./build/bin/helix-tests "'"${1:-}"'"' ;;
    # Trailing args become make overrides, e.g. SHARD_CONCURRENCY=24.
    # NPROCS pins the shard count to thelio's 96. The count decides which tests
    # share a process, so zeus's own 216 would judge a grouping nobody runs
    # locally; a trailing NPROCS= still overrides it.
    sweep)  CMD='make unit-sweep NPROCS=96 -j$HELIX_J '"$*" ;;
    asan-app|tsan-app)
        # RECIPE is the positional argument; --repeat N (default 25 in the
        # make target) widens the drive. Both map onto the make target's
        # RECIPE/REPEAT variables.
        _recipe=""; _repeat=""
        while [ $# -gt 0 ]; do
            case "$1" in
                --repeat)
                    [ $# -ge 2 ] || { echo "✗ --repeat needs a value" >&2; exit 2; }
                    _repeat="$2"; shift 2 ;;
                --recipe)
                    [ $# -ge 2 ] || { echo "✗ --recipe needs a value" >&2; exit 2; }
                    _recipe="$2"; shift 2 ;;
                *)
                    if [ -n "$_recipe" ]; then
                        echo "✗ unexpected argument '$1' (usage: $WHAT [RECIPE] --repeat N)" >&2
                        exit 2
                    fi
                    _recipe="$1"; shift ;;
            esac
        done
        _vars=""
        if [ -n "$_recipe" ]; then _vars="RECIPE=$_recipe"; fi
        if [ -n "$_repeat" ]; then _vars="$_vars REPEAT=$_repeat"; fi
        CMD="make $WHAT $_vars"' -j$HELIX_J'
        EXPECTED_REPEAT="${_repeat:-25}"
        GB_PER_JOB=1.5 ;;
    *)      echo "✗ unknown job '$WHAT' (mutate | asan | tsan | test | sweep | asan-app | tsan-app)" >&2; exit 2 ;;
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

# --- One job in the workdir at a time -----------------------------------------
# The job resets the checkout and rebuilds in $WORKDIR, so a second run in the
# same tree builds against files the first is replacing. Jobs queue rather
# than share: -j is sized from MemAvailable at start, which is only sound
# while this run is the only one allocating. The lock lives on the host
# because this script runs there; the workdir path only exists inside the
# container, so the lock name is derived from it.
LOCK="${ZEUS_LOCK_DIR:-/tmp}/helix-zeus-run-$(basename "$WORKDIR")".lock
exec 9>>"\$LOCK"
if ! flock -n 9; then
    echo "→ $WORKDIR busy: \$(tail -n 1 "\$LOCK" 2>/dev/null || echo another zeus-run job); waiting"
    flock 9
    echo "→ $WORKDIR free; continuing"
fi
# The lock is held here, so the file can be rewritten in place: it stays one
# line no matter how many jobs pass through it.
: > "\$LOCK"
printf 'held by pid %s: %s %s since %s\n' "\$\$" "$WHAT" "$SHORT" "\$(date '+%F %T')" >&9

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

# The container is long-lived but has no restart policy, so it is stopped after
# every NAS reboot and `docker exec` fails with a message about the container
# not running, several steps before anything explains why. Starting it is
# idempotent and costs nothing when it is already up.
if ! sudo -n docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "→ container $CONTAINER is not running; starting it"
    sudo -n docker start "$CONTAINER" >/dev/null || {
        echo "✗ could not start container $CONTAINER on \$(hostname)" >&2
        exit 1
    }
fi

D() { sudo -n docker exec -w "$WORKDIR" -e CCACHE_DIR=/work/ccache -e HELIX_J="\$HELIX_J" "$CONTAINER" bash -lc "\$1"; }

# A run whose ssh side died leaves its build running in the container while
# the lock is already released; resetting the tree under that build is the
# corruption the lock exists to prevent. Wait any make out before touching
# git. The poll interval is the only knob: long enough not to spam the log
# of a live box, overridable so tests can spin it fast. Zombies are excluded:
# an interrupted run's make is reparented to the container's PID 1, which
# never reaps it, so a bare pgrep -x make would wait on it forever.
while D 'pgrep -x -r R,S,D,T,t make >/dev/null'; do
    echo "→ orphaned build still running in $CONTAINER; waiting"
    sleep "${ZEUS_ORPHAN_POLL_SECS:-30}"
done

D 'git config --global --add safe.directory "*"' >/dev/null
# Submodules are fetched by the update below, for $SHA's pins only. Recursing
# here fetches the pin of every new superproject commit, and one pin to a
# submodule commit that was rebased away before pushing fails the whole fetch.
D 'git fetch --quiet --all --recurse-submodules=no'
# mutate_diff.py's default base is the nearest fork point among origin/main and the
# local main. A local main left behind by an earlier job puts that fork point
# before everything since, so the run is handed foreign hunks and refuses. Bring
# it level with the remote before the reset below.
D 'git fetch --quiet origin main && git update-ref refs/heads/main FETCH_HEAD'
D 'git reset --hard --quiet $SHA && git submodule update --init --recursive --quiet'
# A submodule already at its pin keeps the patches an earlier job applied, so a
# commit that edits a patch in patches/ meets the old revision and the build's
# drift check refuses. Reapply against this commit's patches/ every run.
D 'make reapply-patches >/dev/null'
D 'git log --oneline -1'
D '$CMD 2>&1'
REMOTE

# A sanitizer run that produced no Catch2 summary ran nothing, whatever its exit
# code said. Refusing to call that a pass is the whole point of checking.
if { [ "$WHAT" = asan ] || [ "$WHAT" = tsan ]; } && ! grep -qE 'All tests passed|test cases:|assertions:' "$LOG"; then
    echo ""
    echo "✗ no Catch2 summary in $LOG — the suite did not run, so this is not a clean ${WHAT^^} result" >&2
    exit 1
fi

# An app run has no Catch2 summary to check; its evidence is the verdict line
# the make target prints and the per-pass lines the drive prints. The remote
# make already enforces both; this re-checks the local log so an exit 0 that
# somehow carried no verdict cannot be read as clean either.
if [ "$WHAT" = asan-app ] || [ "$WHAT" = tsan-app ]; then
    if ! grep -q 'clean — no sanitizer reports' "$LOG"; then
        echo ""
        echo "✗ no clean-verdict line in $LOG — the sanitizer verdict never ran" >&2
        exit 1
    fi
    passes=$(grep -cE '^\[screenshot\] recipe pass [0-9]+/[0-9]+' "$LOG" || true)
    if [ "$passes" -ne "$EXPECTED_REPEAT" ]; then
        echo ""
        echo "✗ expected $EXPECTED_REPEAT recipe passes in $LOG, found $passes — the drive did not complete" >&2
        exit 1
    fi
fi

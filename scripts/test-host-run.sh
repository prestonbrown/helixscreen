#!/usr/bin/env bash
# Run the expensive, non-interactive gates on a test host instead of the box you
# are typing on: any Linux machine with Docker, set up once by
# scripts/test-host-setup.sh and named in the build-hosts file (below).
#
#   scripts/test-host-run.sh mutate --tests '[1543]'     # the mutation gate
#   scripts/test-host-run.sh asan '[1543]'               # AddressSanitizer, one tag
#   scripts/test-host-run.sh asan                        # AddressSanitizer, full suite, sharded as CI runs it
#   scripts/test-host-run.sh tsan '[ams],[spoolman]'     # ThreadSanitizer, tags as ONE argument
#   scripts/test-host-run.sh tsan                        # ThreadSanitizer, full suite, sharded
#   scripts/test-host-run.sh test '[netd]'               # plain suite, one tag
#   scripts/test-host-run.sh test -# '[#test_foo]'        # every argument reaches helix-tests
#   scripts/test-host-run.sh exec -- bash ./x.sh          # any command in the synced mirror
#   scripts/test-host-run.sh exec --build -- ./build/bin/helix-tests --list-tags  # after make test
#   scripts/test-host-run.sh sweep                       # make unit-sweep, sharded
#   scripts/test-host-run.sh asan-app help-qr --repeat 50  # the APP under ASAN
#   scripts/test-host-run.sh tsan-app help-qr --repeat 50  # the APP under TSan
#   scripts/test-host-run.sh plugin-host --layouts plain   # HelixPrint plugin matrix
#
# plugin-host runs scripts/plugin-host-matrix.sh on the test host itself, not
# in the test container: it builds and starts a container per Moonraker
# vintage and printer layout, so it needs the host's docker. Its results come
# back to build/plugin-host/<timestamp>/. It is a mirror run only (no --commit).
#
# test and exec hand their arguments over word for word: each one is quoted
# here (printf %q) and arrives in the container as one argument, so a space,
# a quote or a semicolon in it is data, never shell.
#
# The app modes are here for the same reason asan is: the run is long and
# non-interactive, and the container's image (SDL, no ld.so.preload) is the
# only place an instrumented desktop app runs cleanly.
#
# A whole-suite C++ verdict comes from sweep, which shards the way CI and the
# local gate do. test with no tag runs the suite in one process, where
# cross-test contamination fails cases no branch touched, so it is not a gate.
# bats is not run here: the container is root with no shellcheck, so the shell
# suite fails on its environment; run `make test-shell` locally instead.
#
# Why these two in particular:
#
#   mutate  mutate_diff.py rebuilds and re-runs the suite once per changed hunk,
#           so it is the most expensive and least interactive thing in the loop.
#
#   asan    a desktop whose /etc/ld.so.preload holds a library (libinput-config,
#           say) loads ASAN's runtime second, so the binary produces NO test
#           output and exits 0 - a pass that ran nothing. The container has no
#           /etc/ld.so.preload, so ASAN works there with no workaround, and its
#           image matches CI's, which is why sanitizer findings reproduce.
#
# What runs is the working tree as it is on disk: uncommitted edits and
# untracked files included, ignored files excluded. It is copied to a mirror on
# the test host, one per tree (<HELIX_TEST_TREES>/<tree> in the container),
# whose build/ persists, so a run rebuilds only what changed. Each mirror has its
# own lock: two trees run at once, two runs of one tree queue. Every run prints
# and logs the HEAD it started from and whether the tree was dirty, with a hash
# of the difference, since a dirty verdict is not one anybody else can reproduce.
#
# A mirror's file times are the test host's own: the sync compares content
# (rsync -c) and does not carry the local times, so a file is rewritten, and
# stamped with the host's clock, exactly when its bytes change. That is what
# lets make trust a build/ it did not write.
#
# A tree's first run seeds its mirror from the warm mirror nearest in history
# (fewest commits between the two HEADs; each mirror records the HEAD it last
# ran), copied with cp --reflink=auto, which on a ZFS or btrfs host shares the
# blocks instead of copying them. The sync then makes the files exact and make
# rebuilds what differs, instead of building the whole tree cold.
# HELIX_TEST_SEED=0 skips it.
#
#   scripts/test-host-run.sh --commit sweep    # the pushed HEAD, in the shared checkout
#   scripts/test-host-run.sh --prune [DAYS]    # remove mirrors and mutate checkouts unused for DAYS (14)
#   scripts/test-host-run.sh --drop TREE       # remove one tree's mirror and mutate checkout
#   scripts/test-host-run.sh --probe           # exit 0 when offloading to the test host pays
#
# A run that creates a mirror or a mutate checkout also prunes those unused for
# HELIX_TEST_PRUNE_DAYS (14; 0 never), so the copies do not accumulate.
#
# --commit runs a pushed commit in the shared checkout (HELIX_TEST_WORKDIR)
# instead, for a verdict someone else must be able to reproduce. mutate always
# runs the pushed commit too, since mutate_diff.py reads git history and a
# mirror is files only, but in a checkout of this tree's own
# (<parent of HELIX_TEST_TREES>/mutate/<tree>), so two trees mutate at once.
# Its first run copies the most recently used checkout, build/ included.
#
# The patched submodules are mirrored as they are on disk, patches applied,
# not pristine: `make reapply-patches` resets them with git, which a mirror
# has none of, and the build's marker check confirms each patch's effect by
# text search, which works on a plain file tree.
#
# Configuration comes from the build-hosts file (scripts/lib/build_hosts.sh),
# environment first. HELIX_TEST_HOST has no default: unset, every command
# refuses and names the file to put it in.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=lib/build_hosts.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/build_hosts.sh"
# shellcheck source-path=SCRIPTDIR source=lib/ccache_shared.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/ccache_shared.sh"

HOST="${HELIX_TEST_HOST:-}"
CONTAINER="${HELIX_TEST_CONTAINER:-helix-test}"
# safe.directory for every git in a container exec, set without writing config.
GIT_SAFE_ENV="-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e 'GIT_CONFIG_VALUE_0=*'"
WORKDIR="${HELIX_TEST_WORKDIR:-/work/helixscreen}"       # the --commit checkout
# On the test host, relative to the ssh user's home unless absolute; it is the
# same directory the container sees as TREES.
TREES_HOST="${HELIX_TEST_TREES_HOST:-helix-test/trees}"
TREES="${HELIX_TEST_TREES:-/work/trees}"                 # TREES_HOST, in the container
CCACHE="${HELIX_TEST_CCACHE:-/work/ccache}"              # in the container
LOCK_DIR="${HELIX_TEST_LOCK_DIR:-/tmp}"                  # on the test host
# Locks in LOCK_DIR: <prefix>.tree-<tree>.lock (a mirror), <prefix>.mutate-<tree>.lock
# (a mutate checkout) and <prefix>.global.lock (no jobpool). --commit takes
# COMMIT_LOCK_PREFIX-<checkout>.lock instead, the name zeus-run.sh (still in
# older worktrees) takes for the same checkout: both reset it, so both must take
# one lock.
LOCK_PREFIX=helix-test-host-run
COMMIT_LOCK_PREFIX=helix-zeus-run
MANIFEST=.helix-mirror-files                             # in each mirror: what the last sync sent
HEAD_FILE=.helix-mirror-head                             # in each mirror: the HEAD its last run synced
SEED="${HELIX_TEST_SEED:-1}"                             # 0: a new mirror or mutate checkout starts cold
PRUNE_DAYS="${HELIX_TEST_PRUNE_DAYS:-14}"                # 0: creating one never prunes the others
# The container mounts the parent of TREES_HOST at the parent of TREES
# (test-host-setup.sh), so a container path under WORK has a host path under
# WORK_HOST. Mutate checkouts sit beside the mirrors.
WORK_HOST=$(dirname "$TREES_HOST")
WORK=$(dirname "$TREES")
MUTATE_HOST=$WORK_HOST/mutate
MUTATE=$WORK/mutate
RSYNC_PATH="sudo -n rsync"                               # the container writes mirrors as root
SHARD_ARTIFACTS=build/shard-artifacts                    # failing shard logs, mirror-relative
# BatchMode: never a password prompt. The keepalives bound a link that dies
# mid-run, at a minute: a cross-site link to a host saturated by a cold build
# can go unanswered for well over ten seconds and still be alive, and dropping
# it throws away the run's verdict. ConnectTimeout bounds one that never answers.
SSH_OPTS=(-o ConnectTimeout=3 -o BatchMode=yes -o ServerAliveInterval=5 -o ServerAliveCountMax=12)
ORPHAN_POLL_SECS="${HELIX_TEST_ORPHAN_POLL_SECS:-30}"    # how often to re-check an orphaned build

# --probe: offloading pays when the test host is on the LAN, or when the bytes
# this run would send cross the link in under PROBE_MAX_SYNC_SECS.
PROBE_LAN_RTT_MS=5
PROBE_MAX_SYNC_SECS=15
PROBE_PAYLOAD_BYTES=1048576                              # what --probe times to measure the link

# The sweep's shard count is pinned to the local gate's 96: the count decides
# which tests share a process, so a host's own core count would judge a
# grouping nobody runs locally.
SWEEP_NPROCS=96

# Submodules whose untracked files are source: lvgl, libhv and lua hold the
# files their patches create, helix-xml is edited directly. Untracked files in
# the others are the local toolchain's build output.
SOURCE_SUBMODULES="lib/lvgl lib/libhv lib/lua lib/helix-xml"
# The one submodule edited directly; dirt in the others derives from patches/.
EDITED_SUBMODULE=lib/helix-xml

# On a ZFS host the ARC holds most of the RAM and gives it back through gradual
# kernel reclaim, which a burst of compilers can outrun; with no swap the
# overshoot goes straight to the OOM killer and a compile dies with no error
# text. The zfs_arc_sys_free module parameter makes ZFS hold that much RAM free
# ahead of time, so a build starts into memory that is already free. The run
# warns when the parameter is below SYS_FREE_FLOOR_GB; a host without ZFS has
# no parameter and gets no warning.
#
# The job count comes from MemAvailable, or from the host's jobpool, which
# sizes itself from MemAvailable too. A compile peaks near 400 MB, ASAN
# included, so no single process is the problem - the total is.
SYS_FREE_FLOOR_GB=32
GB_PER_JOB="${HELIX_TEST_GB_PER_JOB:-1}"      # asan overrides to 1.5 below

# When jobpool is installed on the test host, the container's make draws from
# the host's machine pool instead of a -j of its own, so two runs (or a run and
# anything else pooled there) share the cores. The pool's state dir has to sit
# inside a directory the container already mounts (STATE_DIR in the host's
# jobpool conf). Without it the run sizes -j from memory. Resolved on the host.
JOBPOOL_BIN="${HELIX_TEST_JOBPOOL:-\$HOME/.local/bin/jobpool}"   # the host's $HOME

# mutate is background work and yields to everything else on the test host. A
# gate, sweep or tagged run blocks someone; a mutation run only has to finish
# eventually. Its container shell lowers itself to SCHED_IDLE, nice 19 and idle
# I/O before the command starts, and every compiler and test process inherits
# that, so the scheduler gives it only the cores nothing else wants. It stays
# out of the jobpool too: a starved process holding a pool token would keep
# that token from the gate it is yielding to. docker exec starts its process
# from the container runtime, not as a child of the host's shell, so a nice or
# chrt on the host side never reaches it; this runs inside. Each of the three
# is tried on its own, so one that is missing or refused leaves the others in
# force. No single quote: it is spliced into a single-quoted remote command.
YIELD_PRIORITY='_unlowered=""; chrt -i -p 0 $$ >/dev/null 2>&1 || _unlowered="$_unlowered chrt"; renice -n 19 -p $$ >/dev/null 2>&1 || _unlowered="$_unlowered renice"; ionice -c 3 -p $$ >/dev/null 2>&1 || _unlowered="$_unlowered ionice"; [ -z "$_unlowered" ] || echo "→ could not lower this background job with:$_unlowered; it competes with gates more than it should"; '
YIELD=0

usage() { awk 'NR > 1 && /^set -euo/ { exit } NR > 1' "$0" | sed 's/^# \?//'; exit 2; }

# A tree name is spliced into scripts that run on the test host, so it is held to a
# plain name: no quoting characters, no glob, no path.
valid_tree_name() { # <name>
    [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]] && [ "$1" != . ] && [ "$1" != .. ]
}

# The NUL-separated list of files a mirror holds, relative to the tree root:
# tracked files through every submodule, plus untracked files that are not
# ignored in the superproject and in SOURCE_SUBMODULES. build/, .worktrees/
# and every other ignored path stay home.
mirror_file_list() {
    git ls-files -z --recurse-submodules
    git ls-files -z -o --exclude-standard
    local sub f
    for sub in $SOURCE_SUBMODULES; do
        [ -d "$sub" ] || continue
        git -C "$sub" ls-files -z -o --exclude-standard | while IFS= read -r -d '' f; do
            printf '%s/%s\0' "$sub" "$f"
        done
    done
}

# rsync the listed files into this tree's mirror. A worktree's shared lib/
# entries are symlinks into the main tree; the listed paths run through them,
# and rsync creates those leading directories as real directories. Tracked
# symlinks are relative and inside the tree, so they stay links.
# --delete-missing-args because a tracked file deleted but not yet committed
# is still listed. The container writes the mirror as root, so the far side
# runs under sudo.
# --force lets a directory give way to a file of the same name. Exit 24 is
# files that vanished mid-transfer, which a tree being edited always risks: the
# next sync settles them, so it is not a failure.
# -c --no-times: a file is compared by content and, when it differs, written
# with the test host's time, never the local one. A local time can be older
# than the mirror's object for that file (a seeded mirror, a checkout that
# restored an old time, two clocks), and make would then keep the stale object.
# Checksumming costs a few seconds a run; a missed rebuild costs a wrong verdict.
sync_files() { # <list file> [rsync args...]
    local list=$1 rc=0; shift
    rsync -a --no-times -c --mkpath --force --from0 --files-from="$list" --delete-missing-args \
        -e "ssh ${SSH_OPTS[*]}" --rsync-path="$RSYNC_PATH" "$@" ./ "$HOST:$TREES_HOST/$TREE/" || rc=$?
    [ "$rc" -eq 24 ] && return 0
    return "$rc"
}

# The remote snippet that starts the container when a NAS reboot left it
# stopped. Starting is idempotent and costs nothing when it is already up.
# The host command that prints, from inside the container, what it may still
# allocate in kB (lib/mem_available.sh). The script travels in the command, as
# ccache_shared_cmd's does, so it is this tree's copy whatever the container has.
container_mem_cmd() {
    local b64
    b64=$( { cat "$(dirname "${BASH_SOURCE[0]}")/lib/mem_available.sh"; echo helix_mem_available_kb; } |
        base64 | tr -d '\n') || return 1
    printf 'sudo -n docker exec %q bash -lc %q' "$CONTAINER" "echo $b64 | base64 -d | sh"
}

container_up() {
    cat <<SNIPPET
if ! sudo -n docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    echo "→ container $CONTAINER is not running; starting it"
    sudo -n docker start "$CONTAINER" >/dev/null || {
        echo "✗ could not start container $CONTAINER on \$(hostname)" >&2
        exit 1
    }
fi
SNIPPET
}

# The remote snippet that applies scripts/ccache-shared-config.sh to the
# container's cache, then checks it covers <dir>, the directory this run builds
# in. Every mirror, mutate checkout and the --commit checkout share CCACHE, and
# base_dir=WORK is what lets them reuse each other's compiles. It runs on every
# run, so a host set up before the settings existed corrects itself and none
# can drift.
ccache_shared() { # <container dir>
    cat <<SNIPPET
$(ccache_shared_cmd "$CONTAINER" "$CCACHE" apply "$WORK" "$HELIX_TEST_CCACHE_SIZE") | sed -n 's/^ *\(.*(was .*\)/→ ccache \1/p' ||
    echo "⚠ could not apply the shared ccache settings in $CONTAINER" >&2
$(ccache_shared_cmd "$CONTAINER" "$CCACHE" check "$1") || true
SNIPPET
}

# The remote command that succeeds while a make in the container has its cwd
# in <dir>. <dir> is spliced as remote shell text inside double quotes, so it
# may name a variable of the remote script.
live_make_in() { # <container dir>
    printf '%s' "sudo -n docker exec -w / \"$CONTAINER\" bash -lc 'for p in \$(pgrep -x -r R,S,D,T,t make); do case \"\$(readlink /proc/\$p/cwd)\" in '\"$1\"'|'\"$1\"'/*) exit 0 ;; esac; done; exit 1'"
}

# The remote snippet that waits for a seeding copy into <tree> to finish. The
# copying shell holds <prefix>.seed-<kind>-<tree>.lock for its whole life, and it
# outlives a client that was killed mid-copy, so whatever touches that tree
# next waits here instead of racing its cp and mv.
seed_lock_wait() { # <kind: tree|mutate> <tree>
    cat <<SNIPPET
exec 6>>"$LOCK_DIR/$LOCK_PREFIX.seed-$1-$2.lock"
if ! flock -n 6; then
    echo "→ a copy into $2 is still running on \$(hostname); waiting for it" >&2
    flock 6
fi
SNIPPET
}

# The remote snippet that waits out any make whose cwd is in RUNDIR. A run
# whose ssh side died leaves its build running in the container while the lock
# is already released, and changing files under that build is the corruption
# the lock exists to prevent; another tree's build is not ours to wait for.
# Zombies are excluded: an interrupted run's make is reparented to the
# container's PID 1, which never reaps it, so counting it would wait forever.
orphan_wait() {
    cat <<SNIPPET
while $(live_make_in "$RUNDIR"); do
    echo "→ orphaned build still running in $CONTAINER; waiting"
    sleep $ORPHAN_POLL_SECS
done
SNIPPET
}

# Everything that changes the mirror before the sync, under the tree lock: the
# orphan wait, then the delete of what the last sync sent and this one will not,
# then the new list replaces the old. Stale paths go before rsync runs so a path
# that changed kind (file to directory) is free for it. One that is already a
# directory (a sync interrupted mid-way) fails its rm and is left for rsync's
# --force. Interrupted anywhere, the list on the test host still names every file a sync
# put there.
prepare_mirror() { # <list file>
    rsync -a -e "ssh ${SSH_OPTS[*]}" --mkpath --rsync-path="$RSYNC_PATH" "$1" "$HOST:$TREES_HOST/$TREE/$MANIFEST.new"
    # shellcheck disable=SC2087  # the paths and snippets resolve here
    ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE
set -euo pipefail
$(container_up)
$(seed_lock_wait tree "$TREE")
$(orphan_wait)
cd "$TREES_HOST/$TREE"
# A mirror with no HEAD_FILE was synced with local file times, which can be
# older than its objects for files that changed, and a content-compared sync
# never rewrites a file whose bytes already match. Stamping every synced file
# now costs one rebuild (ccache serves most of it) and makes the build exact.
if [ -d build ] && [ ! -e $HEAD_FILE ] && [ -f $MANIFEST ]; then
    echo "→ $TREE's mirror predates content-compared syncs: rebuilding it once" >&2
    sudo -n xargs -0 -r touch -c -h -- < $MANIFEST || true
fi
if [ -f $MANIFEST ]; then
    comm -z -23 <(sort -z $MANIFEST) <(sort -z $MANIFEST.new) | sudo -n xargs -0 -r rm -f -- || true
fi
sudo -n mv $MANIFEST.new $MANIFEST
REMOTE
}

# Removes mirrors under TREES_HOST and mutate checkouts under MUTATE_HOST: those
# named, or with no names, those whose last run is older than DAYS (a mirror's
# sync rewrites its manifest, a mutate run rewrites the checkout's index). One
# whose lock is held is in use and stays.
remove_mirrors() { # <days|-> [tree...]
    local days=$1 n; shift
    for n in "$@"; do valid_tree_name "$n" || { echo "✗ '$n' is not a tree name" >&2; return 2; }; done
    # shellcheck disable=SC2087  # DAYS, the names and the paths resolve here
    ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE
set -uo pipefail
rc=0
sweep() { # <root> <lock kind> <stamp, relative> <what> [name...]
    local root=\$1 kind=\$2 stamp_rel=\$3 what=\$4 d name stamp lock
    shift 4
    [ \$# -gt 0 ] || set -- "\$root"/*/
    for d in "\$@"; do
        name=\$(basename "\$d")
        d="\$root/\$name"
        [ -d "\$d" ] || { [ -z "$*" ] || [ "\$kind" != tree ] || echo "→ no \$what \$name"; continue; }
        # <tree>+seeding is a copy in progress, or one a dead run left behind.
        case "\$name" in *+*) [ -n "\$(find "\$d" -maxdepth 0 -mmin +1440)" ] || continue ;; esac
        if [ "$days" != - ]; then
            stamp="\$d/\$stamp_rel"; [ -e "\$stamp" ] || stamp="\$d"
            [ -n "\$(find "\$stamp" -maxdepth 0 -mmin +$(( ${days/-/0} * 1440 )))" ] || continue
        fi
        lock="$LOCK_DIR/$LOCK_PREFIX.\$kind-\$name.lock"
        exec 9>>"\$lock"
        if ! flock -n 9; then
            echo "→ kept \$what \$name: in use (\$(tail -n 1 "\$lock"))"
            [ -z "$*" ] || rc=1
        elif sudo -n rm -rf -- "\$d"; then
            echo "→ removed \$what \$name"
        else
            echo "✗ could not remove \$d" >&2; rc=1
        fi
        exec 9>&-
    done
}
sweep "$TREES_HOST" tree $MANIFEST mirror $*
sweep "$MUTATE_HOST" mutate .git/index "mutate checkout" $*
exit \$rc
REMOTE
}

# The root half of seeding: copy <src> to <dst> through <dst>+seeding, so an
# interrupted copy never looks like a mirror, and point every text file in its
# build/ (dependency files, compile-command fragments) at <dst>'s own path
# in the container. A build file naming the source's path would hand make the
# source's dependency graph. '+' is not in a tree name, so the temporary name
# cannot collide with a real tree.
seed_copy_snippet() {
    cat <<'SNIPPET'
seed_copy() { # <src> <dst> <src in the container> <dst in the container>
    set -e
    local tmp="$2+seeding" re f t
    rm -rf -- "$tmp" "$2"
    mkdir -p -- "$(dirname "$2")"
    cp -a --reflink=auto -- "$1" "$tmp"
    # The copy's age is now, not the source's last run, for a prune that
    # meets it mid-copy or once it is in place.
    touch -c -- "$tmp/.helix-mirror-files" "$tmp/.git/index"
    re=$(printf '%s' "$3" | sed 's/[][\.*^$#]/\\&/g')
    {
        [ ! -d "$tmp/build" ] || grep -rIlZF -- "$3" "$tmp/build" || true
        [ ! -f "$tmp/compile_commands.json" ] || printf '%s\0' "$tmp/compile_commands.json"
    } | while IFS= read -r -d '' f; do
        t=$(stat -c %y -- "$f")
        sed -i "s#$re\([^A-Za-z0-9._+-]\|\$\)#$4\1#g" -- "$f"
        touch -d "$t" -- "$f"
    done
    mv -T -- "$tmp" "$2"
}
SNIPPET
}

# Orders seed candidates, best first: those whose HEAD this repository knows,
# by the commits between it and ours (both sides of the fork), then the rest by
# their last run, newest first.
rank_seeds() { # stdin: "<name> <head|-> <epoch>" lines
    local name head stamp d
    while read -r name head stamp; do
        d=""
        if [ "$head" != - ] && git cat-file -e "$head^{commit}" 2>/dev/null; then
            d=$(git rev-list --count "HEAD...$head" 2>/dev/null) || d=""
        fi
        if [ -n "$d" ]; then echo "0 $d $(( -stamp )) $name"; else echo "1 0 $(( -stamp )) $name"; fi
    done | sort -k1,1n -k2,2n -k3,3n | awk '{ print $4 }'
}

# A tree with no build/ in its mirror copies the nearest warm one first. A
# candidate has a linked helix-tests (a build that got that far) and is not
# mid-run (its lock is free); a busy one is passed over rather than waited for.
# With none, the run builds cold, as it would have anyway.
seed_mirror() {
    local cands order t0 out
    # shellcheck disable=SC2087  # the paths resolve here
    cands=$(ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE
set -uo pipefail
$(seed_lock_wait tree "$TREE")
cd "$TREES_HOST" 2>/dev/null || exit 0
[ ! -d "$TREE/build" ] || { echo WARM; exit 0; }
[ "$SEED" != 0 ] || { echo OFF; exit 0; }
for d in */; do
    n=\${d%/}
    case "\$n" in "$TREE"|*+*) continue ;; esac
    # A mirror without HEAD_FILE has not yet had its rebuild under
    # content-compared syncs (prepare_mirror), so its objects are not trusted.
    [ -e "\$n/build/bin/helix-tests" ] && [ -e "\$n/$HEAD_FILE" ] || continue
    printf '%s %s %s\n' "\$n" "\$(head -c 40 "\$n/$HEAD_FILE" 2>/dev/null || echo -)" "\$(stat -c %Y "\$n/$MANIFEST" 2>/dev/null || echo 0)"
done
REMOTE
) || return 0
    case "$cands" in WARM*|OFF*) return 0 ;; "") echo "→ no warm mirror on $HOST to seed $TREE from: building cold"; return 0 ;; esac
    order=$(printf '%s\n' "$cands" | rank_seeds | tr '\n' ' ')
    t0=$(date +%s)
    # shellcheck disable=SC2087  # the names and the paths resolve here
    out=$(ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE
set -uo pipefail
$(seed_copy_snippet)
$(seed_lock_wait tree "$TREE")
dst="$TREES_HOST/$TREE"
[ ! -d "\$dst/build" ] || exit 0
for n in $order; do
    exec 7>>"$LOCK_DIR/$LOCK_PREFIX.tree-\$n.lock"
    if ! flock -n 7; then echo "→ seed candidate \$n is busy; trying the next" >&2; exec 7>&-; continue; fi
    # A free lock is not an idle tree: a run whose ssh died leaves its make
    # writing there, and a copy would take its half-written files.
    if $(live_make_in "$TREES/\$n"); then echo "→ seed candidate \$n has an orphaned build running; trying the next" >&2; exec 7>&-; continue; fi
    if sudo -n bash -c "\$(declare -f seed_copy); "'seed_copy "\$@"' _ "$TREES_HOST/\$n" "\$dst" "$TREES/\$n" "$TREES/$TREE"; then
        echo "SEEDED \$n"; exit 0
    fi
    echo "✗ copying mirror \$n failed; trying the next" >&2
    exec 7>&-
done
REMOTE
) || true
    case "$out" in
        *"SEEDED "*) echo "→ seeded $TREE's mirror from ${out##*SEEDED } in $(( $(date +%s) - t0 ))s"
                     prune_after_create ;;
        *)           echo "→ no idle warm mirror to seed $TREE from: building cold" ;;
    esac
}

# Creating a copy is when disk use grows, so that is when the stale ones go.
prune_after_create() {
    [ "$PRUNE_DAYS" != 0 ] || return 0
    remove_mirrors "$PRUNE_DAYS" | sed 's/^→ removed /→ pruned (unused for '"$PRUNE_DAYS"' days) /' | grep -v '^→ kept ' || true
}

# A mutate checkout that does not exist yet copies the most recently used one
# (another tree's, or the shared --commit checkout), whose build/ is warm and
# whose index git refreshes rather than rewriting every file. Busy ones are
# passed over; when every one is busy the run waits for the newest.
seed_mutate_checkout() {
    local t0 out
    t0=$(date +%s)
    # shellcheck disable=SC2087  # the names and the paths resolve here
    out=$(ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE
set -uo pipefail
$(seed_copy_snippet)
$(seed_lock_wait mutate "$TREE")
dst="$MUTATE_HOST/$TREE"
[ ! -d "\$dst/.git" ] || exit 0
shared="$WORK_HOST${WORKDIR#"$WORK"}"
lock_of() { # <host path>
    if [ "\$1" = "\$shared" ]; then echo "$LOCK_DIR/$COMMIT_LOCK_PREFIX-$(basename "$WORKDIR").lock"
    else echo "$LOCK_DIR/$LOCK_PREFIX.mutate-\$(basename "\$1").lock"; fi
}
ctr_of() { # <host path>
    if [ "\$1" = "\$shared" ]; then echo "$WORKDIR"; else echo "$MUTATE/\$(basename "\$1")"; fi
}
cands=()
while IFS= read -r g; do
    c=\${g%/.git}
    [ "\$c" != "\$dst" ] && [ "\$c" = "\${c%+seeding}" ] && cands+=("\$c")
done < <(ls -1dt "$MUTATE_HOST"/*/.git "\$shared/.git" 2>/dev/null)
[ \${#cands[@]} -gt 0 ] || { echo "NONE"; exit 0; }
pick=""
for c in "\${cands[@]}"; do
    exec 7>>"\$(lock_of "\$c")"
    if flock -n 7; then
        _c=\$(ctr_of "\$c")
        if ! $(live_make_in "\$_c"); then pick=\$c; break; fi
        echo "→ \$(basename "\$c") has an orphaned build running; trying the next" >&2
    fi
    exec 7>&-
done
if [ -z "\$pick" ]; then
    pick=\${cands[0]}
    echo "→ every mutate checkout is busy; waiting for \$(basename "\$pick") to copy it" >&2
    exec 7>>"\$(lock_of "\$pick")"
    flock 7
fi
sudo -n bash -c "\$(declare -f seed_copy); "'seed_copy "\$@"' _ "\$pick" "\$dst" "\$(ctr_of "\$pick")" "$RUNDIR" &&
    echo "SEEDED \$(basename "\$pick")"
REMOTE
) || true
    case "$out" in
        *"SEEDED "*) echo "→ seeded $RUNDIR from ${out##*SEEDED } in $(( $(date +%s) - t0 ))s"
                     prune_after_create ;;
        *NONE*)      echo "✗ no checkout on $HOST to copy for $RUNDIR: run scripts/test-host-setup.sh --commit-checkout" >&2
                     return 1 ;;
        "")          ;;
        *)           echo "✗ could not create $RUNDIR on $HOST" >&2; return 1 ;;
    esac
}

# Takes the lock named <name> on the test host and holds it until this script exits:
# the holder is a remote shell that waits for its stdin to close, which happens
# when this process ends, however it ends. The lock has to cover the sync and
# the job both, and they are two connections, so neither can hold it alone.
take_lock() { # <lock file name> <what it guards> <holder note>
    coproc LOCKER { ssh "${SSH_OPTS[@]}" "$HOST" bash -s 2>&1; }
    cat >&"${LOCKER[1]}" <<LOCK
set -eu
mkdir -p "$LOCK_DIR"
LOCK="$LOCK_DIR/$1"
exec 9>>"\$LOCK"
if ! flock -n 9; then
    echo "→ $2 busy: \$(tail -n 1 "\$LOCK" 2>/dev/null || echo another test-host-run job); waiting"
    flock 9
    echo "→ $2 free; continuing"
fi
# The lock is held here, so the file can be rewritten in place: it stays one
# line no matter how many jobs pass through it.
: > "\$LOCK"
printf 'held by pid %s: %s since %s\n' "\$\$" "$3" "\$(date '+%F %T')" >&9
echo LOCKED
exec cat >/dev/null
LOCK
    local line
    while IFS= read -r line <&"${LOCKER[0]}"; do
        [ "$line" = LOCKED ] && return 0
        printf '%s\n' "$line"
    done
    echo "✗ could not take the $2 lock on $HOST" >&2
    return 1
}

# Nanoseconds since the epoch. BSD date (a Mac) has no %N and prints it
# literally, which is not a number.
now_ns() {
    local t
    t=$(date +%s%N)
    case "$t" in
    *[!0-9]*) python3 -c 'import time; print(time.time_ns())' ;;
    *) echo "$t" ;;
    esac
}

probe() {
    if ! ssh "${SSH_OPTS[@]}" "$HOST" true 2>/dev/null; then
        echo "→ $HOST unreachable: sweep runs here"
        return 1
    fi
    local rtt
    rtt=$(ping -c 1 -W 1 "$HOST" 2>/dev/null | sed -n 's/.*time=\([0-9.]*\) ms.*/\1/p')
    if [ -n "$rtt" ] && awk -v r="$rtt" -v m="$PROBE_LAN_RTT_MS" 'BEGIN { exit !(r < m) }'; then
        echo "→ $HOST rtt $rtt ms, under ${PROBE_LAN_RTT_MS} ms: sweep runs on $HOST"
        return 0
    fi
    # Not on the LAN: price the sync this run would do against what the link
    # carries. A warm mirror sends a few KB; a cold one over a slow link does not pay.
    TREE=$(basename "$(git rev-parse --show-toplevel)")
    cd "$(git rev-parse --show-toplevel)"
    local list bytes rate t0 ns est
    list=$(mktemp); mirror_file_list > "$list"
    bytes=$(sync_files "$list" --dry-run --stats | sed -n 's/^Total transferred file size: \([0-9,]*\).*/\1/p' | tr -d ,)
    rm -f "$list"
    rate=${HELIX_TEST_PROBE_BYTES_PER_SEC:-}
    if [ -z "$rate" ]; then
        t0=$(now_ns)
        head -c "$PROBE_PAYLOAD_BYTES" /dev/urandom | ssh "${SSH_OPTS[@]}" "$HOST" 'cat > /dev/null'
        ns=$(( $(now_ns) - t0 ))
        rate=$(( PROBE_PAYLOAD_BYTES * 1000000000 / (ns > 0 ? ns : 1) ))
    fi
    est=$(( ${bytes:-0} / (rate > 0 ? rate : 1) ))
    if [ "$est" -lt "$PROBE_MAX_SYNC_SECS" ]; then
        echo "→ $HOST rtt ${rtt:-unknown} ms; ${bytes:-0} bytes at $rate B/s: estimated sync ${est}s, under ${PROBE_MAX_SYNC_SECS}s: sweep runs on $HOST"
        return 0
    fi
    echo "→ $HOST rtt ${rtt:-unknown} ms; ${bytes:-0} bytes at $rate B/s: estimated sync ${est}s, over ${PROBE_MAX_SYNC_SECS}s: sweep runs here"
    return 1
}

MODE=mirror
# --commit is a mode, not a make override, wherever it is written.
_args=()
for _a in "$@"; do if [ "$_a" = --commit ]; then MODE=commit; else _args+=("$_a"); fi; done
set -- ${_args[@]+"${_args[@]}"}
case "${1:-}" in ""|-h|--help) usage ;; esac
require_build_host HELIX_TEST_HOST || exit 2
HOST=$HELIX_TEST_HOST
case "${1:-}" in
    --prune)  case "${2:-14}" in *[!0-9]*) echo "✗ --prune takes a number of days" >&2; exit 2 ;; esac
              remove_mirrors "${2:-14}"; exit ;;
    --drop)   valid_tree_name "${2:-}" || { echo "✗ --drop needs a tree name (letters, digits, . _ -)" >&2; exit 2; }
              remove_mirrors - "$2"; exit ;;
    --probe)  probe; exit ;;
    -*)       usage ;;
esac

WHAT="${1:-}"
[ -n "$WHAT" ] || usage
shift

if [ "$WHAT" = mutate ] && [ "$MODE" = mirror ]; then
    echo "→ mutate reads git history, which a mirror does not carry: running the pushed commit in this tree's mutate checkout"
    MODE=commit
fi

SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short HEAD)
if [ "$MODE" = commit ]; then
    if ! git branch -r --contains "$SHA" 2>/dev/null | grep -q .; then
        echo "✗ $SHORT is not on any remote branch — push it first, or $HOST cannot fetch it" >&2
        exit 1
    fi
    if [ "$WHAT" = mutate ]; then
        # A checkout per tree, so one tree's mutate never queues behind another's.
        TREE=$(basename "$(git rev-parse --show-toplevel)")
        valid_tree_name "$TREE" || { echo "✗ tree directory '$TREE' is not a plain name (letters, digits, . _ -)" >&2; exit 2; }
        RUNDIR=$MUTATE/$TREE
        LOCK_FILE="$LOCK_PREFIX.mutate-$TREE.lock"
        LABEL="$TREE-$SHORT"
    else
        RUNDIR=$WORKDIR
        TREE=""
        LOCK_FILE="$COMMIT_LOCK_PREFIX-$(basename "$WORKDIR").lock"
        LABEL=$SHORT
    fi
    PROVENANCE="$SHA (pushed)"
else
    TOP=$(git rev-parse --show-toplevel)
    cd "$TOP"
    TREE=$(basename "$TOP")
    valid_tree_name "$TREE" || { echo "✗ tree directory '$TREE' is not a plain name (letters, digits, . _ -)" >&2; exit 2; }
    RUNDIR=$TREES/$TREE
    LOCK_FILE="$LOCK_PREFIX.tree-$TREE.lock"
    # Untracked files: the superproject's, and the edited submodule's (both are
    # mirrored). A file that vanishes before it is hashed is not an error.
    untracked() {
        git ls-files -z -o --exclude-standard
        [ ! -d "$EDITED_SUBMODULE" ] || git -C "$EDITED_SUBMODULE" ls-files -z -o --exclude-standard |
            while IFS= read -r -d '' f; do printf '%s/%s\0' "$EDITED_SUBMODULE" "$f"; done
    }
    UNTRACKED=$(untracked | tr -cd '\0' | wc -c)
    if git diff HEAD --quiet --ignore-submodules=dirty &&
       { [ ! -d "$EDITED_SUBMODULE" ] || git -C "$EDITED_SUBMODULE" diff HEAD --quiet; } &&
       [ "$UNTRACKED" -eq 0 ]; then
        LABEL="$TREE-$SHORT"
        PROVENANCE="HEAD $SHA + clean"
    else
        DIRTY=$( {
            git diff HEAD --binary --ignore-submodules=dirty
            [ ! -d "$EDITED_SUBMODULE" ] || git -C "$EDITED_SUBMODULE" diff HEAD --binary
            untracked | xargs -0 -r sha1sum 2>/dev/null || true
        } | sha1sum | cut -c1-10)
        LABEL="$TREE-$SHORT-dirty-$DIRTY"
        PROVENANCE="HEAD $SHA + dirty $DIRTY, $UNTRACKED untracked"
    fi
fi

# $HELIX_J is resolved on the test host from the memory that is free when the job starts.
# $HELIX_JFLAG is -j$HELIX_J, or empty when the container joins the host's jobpool:
# make leaves an inherited jobserver when its command line has any -j.
# shellcheck disable=SC2016  # $HELIX_J must reach the host unexpanded
case "$WHAT" in
    # The mutate checkout is a standalone clone on a reflink filesystem, which
    # is what --workers needs; a later --workers in the caller's arguments wins.
    # Script hunks are judged by bats AND pytest, and the image has no pytest:
    # without the checkout's own venv (requirements.txt) every one of them is
    # NOT COVERED, however narrowly --shell-tests scopes them.
    mutate) CMD='{ [ -x .venv/bin/python3 ] && .venv/bin/python3 -c "import pytest" 2>/dev/null || make venv-setup >/dev/null; }; '"$YIELD_PRIORITY"'python3 scripts/mutate_diff.py --jobs $HELIX_J --workers auto '"$(printf '%q ' "$@")"
            YIELD=1 ;;
    asan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            # Trailing args become make overrides, so ASAN_RUN_OPTIONS can be
            # tuned per run (quarantine_size_mb keeps freed blocks poisoned, which
            # turns a recycled-memory SEGV into a heap-use-after-free report).
            # No tag is the nightly's own sharded run, leak ratchet included.
            if [ -z "$_tag" ]; then
                CMD='make test-asan $HELIX_JFLAG '"$*"
            else
                CMD='make test-asan-one TEST="'"$_tag"'" $HELIX_JFLAG '"$*"
            fi
            GB_PER_JOB=1.5 ;;
    tsan)   _tag="${1:-}"; [ $# -gt 0 ] && shift
            # Same shape as asan: trailing args become make overrides, and no
            # tag is the sharded full-suite run.
            if [ -z "$_tag" ]; then
                CMD='make test-tsan $HELIX_JFLAG '"$*"
            else
                CMD='make test-tsan-one TEST="'"$_tag"'" $HELIX_JFLAG '"$*"
            fi
            TSAN_TAG="$_tag"
            GB_PER_JOB=1.5 ;;
    test)   CMD='make test $HELIX_JFLAG && ./build/bin/helix-tests'
            [ $# -eq 0 ] || CMD="$CMD $(printf '%q ' "$@")" ;;
    exec)   _build=0
            while [ $# -gt 0 ]; do
                case "$1" in
                    --build) _build=1; shift ;;
                    --) shift; break ;;
                    *) echo "✗ exec takes [--build] -- COMMAND [ARG...], not '$1'" >&2; exit 2 ;;
                esac
            done
            [ $# -gt 0 ] || { echo "✗ exec needs a command after --" >&2; exit 2; }
            CMD=$(printf '%q ' "$@")
            [ "$_build" = 0 ] || CMD='make test $HELIX_JFLAG && '"$CMD" ;;
    # Trailing args become make overrides, e.g. SHARD_CONCURRENCY=24.
    # NPROCS pins the shard count (SWEEP_NPROCS); a trailing NPROCS= overrides it.
    # Failing shards' logs land in the mirror's build/, which outlives the
    # container's /tmp, so fetch_shard_artifacts can bring them back.
    sweep)  CMD="rm -rf $SHARD_ARTIFACTS && SHARD_ARTIFACT_ROOT=$SHARD_ARTIFACTS make unit-sweep NPROCS=$SWEEP_NPROCS"' $HELIX_JFLAG '"$*" ;;
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
        CMD="make $WHAT $_vars"' $HELIX_JFLAG'
        EXPECTED_REPEAT="${_repeat:-25}"
        GB_PER_JOB=1.5 ;;
    plugin-host)
        [ "$MODE" = mirror ] || { echo "✗ plugin-host runs the working tree; it takes no --commit" >&2; exit 2; }
        CMD="bash scripts/plugin-host-matrix.sh"
        [ $# -eq 0 ] || CMD="$CMD $(printf '%q ' "$@")" ;;
    *)      echo "✗ unknown job '$WHAT' (mutate | asan | tsan | test | exec | sweep | asan-app | tsan-app | plugin-host)" >&2; exit 2 ;;
esac

LOG="${TMPDIR:-/tmp}/test-host-$WHAT-$LABEL.log"

# The remote shell reads CMD inside single quotes; each ' in it (from a %q-quoted
# argument) closes and reopens them around an escaped quote.
CMD_SQ=${CMD//\'/\'\\\'\'}

# A failed mirror sweep keeps each failing shard's log (its last test case,
# its timeout output) on the test host; copy them here, where whoever reads
# the failure is. A hung shard otherwise names no test.
fetch_shard_artifacts() {
    [ "$WHAT" = sweep ] && [ "$MODE" = mirror ] || return 0
    mkdir -p "$SHARD_ARTIFACTS" || return 0
    rsync -a -e "ssh ${SSH_OPTS[*]}" --rsync-path="$RSYNC_PATH" \
        "$HOST:$TREES_HOST/$TREE/$SHARD_ARTIFACTS/" "$SHARD_ARTIFACTS/" 2>/dev/null || return 0
    local d
    for d in "$SHARD_ARTIFACTS"/helix-shards-*; do
        [ -d "$d" ] && echo "→ failing shards' logs copied here: $d" >&2
    done
    return 0
}
echo "→ $HOST:$CONTAINER $RUNDIR @ $PROVENANCE, log $LOG"

# One job per tree at a time: a second run would sync or reset files under the
# first one's build. The lock covers the sync too, so it is taken first.
take_lock "$LOCK_FILE" "$RUNDIR" "$WHAT $LABEL"

if [ "$MODE" = mirror ]; then
    LIST="${TMPDIR:-/tmp}/test-host-mirror-$TREE.$$"
    # shellcheck disable=SC2154  # _rc is assigned inside the trap string
    trap '_rc=$?; rm -f "$LIST"; [ "$_rc" -eq 0 ] || fetch_shard_artifacts' EXIT
    mirror_file_list > "$LIST"
    seed_mirror
    _t0=$(date +%s)
    prepare_mirror "$LIST"
    sync_files "$LIST"
    echo "→ synced $(tr -cd '\0' < "$LIST" | wc -c) files to $HOST:$TREES_HOST/$TREE in $(( $(date +%s) - _t0 ))s"
elif [ "$WHAT" = mutate ]; then
    seed_mutate_checkout || exit 1
fi

# The plugin matrix drives the host's docker from the mirror on the host, and
# writes its results outside the mirror, which the container owns.
if [ "$WHAT" = plugin-host ]; then
    PH_OUT="${HELIX_TEST_PLUGIN_HOST_OUT:-/tmp}/helix-plugin-host-$TREE"
    rc=0
    # shellcheck disable=SC2087  # the paths and the command resolve here
    ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE | tee "$LOG" || rc=$?
set -uo pipefail
echo "→ $PROVENANCE"
rm -rf "$PH_OUT"
cd "$TREES_HOST/$TREE"
DOCKER="sudo -n docker" $CMD --out "$PH_OUT"
REMOTE
    _local_out="build/plugin-host/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$_local_out"
    rsync -a -e "ssh ${SSH_OPTS[*]}" "$HOST:$PH_OUT/" "$_local_out/" && echo "→ results in $_local_out"
    exit "$rc"
fi

# The heredoc runs on the test host. docker needs sudo -n there (the ssh user
# need not be in the docker group), and git inside the container looks at a
# host-owned checkout, hence safe.directory. Every exec passes it in the
# environment (GIT_CONFIG_*), which reaches git under make and submodule too:
# concurrent runs share one container, and a `git config --global` write from
# each races on /root/.gitconfig.lock and fails one of them.
# shellcheck disable=SC2087  # client-side expansion is the point: the sha, the
# container name and the caller's arguments are resolved HERE, and the one value
# that has to stay server-side ($1 in D) is escaped.
ssh "${SSH_OPTS[@]}" "$HOST" bash -se <<REMOTE | tee "$LOG"
set -euo pipefail
echo "→ $PROVENANCE"
# What a later tree's first run reads to pick the nearest mirror to seed from.
[ "$MODE" != mirror ] || printf '%s\n' "$SHA" | sudo -n tee "$TREES_HOST/$TREE/$HEAD_FILE" >/dev/null

# --- jobpool: join the host's machine pool when it is installed ---------
# The container sees the FIFO through whichever bind
# mount covers the pool's state dir; no such mount means no pool.
JP=$JOBPOOL_BIN
POOL_ENV=""
if [ ! -x "\$JP" ]; then
    :
elif ! _fifo=\$("\$JP" ensure 2>/dev/null); then
    echo "→ jobpool ensure failed on \$(hostname); sizing -j from memory"
else
    _state=\${_fifo%/fifo}
    while read -r _src _dst; do
        [ -n "\$_src" ] || continue
        case "\$_state/" in
            "\$_src"/*) POOL_ENV=\$("\$JP" container-env "\$_dst\${_state#"\$_src"}") && break ;;
        esac
    done <<< "\$(sudo -n docker inspect -f '{{range .Mounts}}{{.Source}} {{.Destination}}{{println}}{{end}}' "$CONTAINER" 2>/dev/null)"
    [ -n "\$POOL_ENV" ] || echo "→ jobpool state \$_state is not mounted in $CONTAINER; sizing -j from memory"
fi
# A background job beside a pool runs on idle cores with its own -j, and does
# not wait for the global lock either: pooled runs never take it.
if [ "$YIELD" = 1 ] && [ -n "\$POOL_ENV" ]; then
    POOL_ENV=""
    YIELD_UNPOOLED=1
    echo "→ background job: SCHED_IDLE, outside the jobpool, so pooled runs keep every token and core they want"
else
    YIELD_UNPOOLED=0
fi

# Without a pool, -j comes from MemAvailable at start, which only holds while
# this is the only run allocating: runs of different trees take turns then.
if [ -z "\$POOL_ENV" ] && [ "\$YIELD_UNPOOLED" = 0 ]; then
    exec 8>>"$LOCK_DIR/$LOCK_PREFIX.global.lock"
    if ! flock -n 8; then
        echo "→ no jobpool on \$(hostname): waiting for the run ahead, since -j is sized for one"
        flock 8
    fi
fi

# --- ZFS: headroom check, and heal a zfs_arc_max cap left behind ------------
# A marker "<pid> <bytes>" records a zfs_arc_max cap that a test-host-run set and did
# not undo. Once its pid is gone, the recorded bytes go back and the marker goes.
# 0 or garbage is not a value to write, so that marker stays and is named.
ARC_PARAM=${HELIX_TEST_ARC_PARAM:-/sys/module/zfs/parameters/zfs_arc_max}
ARC_SYS_FREE=${HELIX_TEST_ARC_SYS_FREE:-/sys/module/zfs/parameters/zfs_arc_sys_free}
ARC_MARK=${HELIX_TEST_ARC_MARK:-/tmp/.helix-test-host-arc-orig}
if [ -e "\$ARC_MARK" ]; then
    read -r _mpid _orig < "\$ARC_MARK" || true
    if [ -n "\${_mpid:-}" ] && kill -0 "\$_mpid" 2>/dev/null; then
        :   # its run is still live and restores the value itself
    elif [ "\${_orig:-0}" -gt 0 ] 2>/dev/null && sudo -n sh -c "echo \$_orig > \$ARC_PARAM" 2>/dev/null; then
        sudo -n rm -f "\$ARC_MARK" 2>/dev/null || true
        echo "→ restored zfs_arc_max to \$_orig from leftover cap marker \$ARC_MARK"
    else
        echo "✗ leftover cap marker \$ARC_MARK ('\$(cat "\$ARC_MARK" 2>/dev/null)') holds no bytes to restore; check zfs_arc_max by hand, then delete it" >&2
    fi
fi
if _sf=\$(cat "\$ARC_SYS_FREE" 2>/dev/null) && [ "\${_sf:-0}" -lt $((SYS_FREE_FLOOR_GB << 30)) ] 2>/dev/null; then
    echo "⚠ zfs_arc_sys_free is \$_sf bytes, under ${SYS_FREE_FLOOR_GB} GiB: a burst of compilers can outrun ARC reclaim (see the test-host-run.sh header)" >&2
fi

# The container has no restart policy, so a NAS reboot leaves it stopped and
# docker exec fails several steps before anything explains why.
$(container_up)

# Derive the job count from what is free NOW, bounded by cores. The container
# answers, so a memory limit on it bounds -j: the host's MemAvailable alone
# would size past the limit and the kernel would kill compilers inside it.
_avail_kb=\$($(container_mem_cmd) 2>/dev/null)
case "\$_avail_kb" in '' | *[!0-9]*) _avail_kb=\$(awk '/^MemAvailable/{print \$2}' /proc/meminfo) ;; esac
HELIX_J=\$(awk -v per=$GB_PER_JOB -v cpus="\$(nproc)" -v kb="\$_avail_kb" '
    BEGIN { j = int((kb/1048576) / per); if (j > cpus) j = cpus; if (j < 4) j = 4; print j }')
# A background job outside the pool and the lock sized its -j from the same
# free memory a gate or a second background job may be sizing from now, so it
# takes half.
if [ "\$YIELD_UNPOOLED" = 1 ]; then
    _full_j=\$HELIX_J
    HELIX_J=\$(( _full_j / 2 > 1 ? _full_j / 2 : 1 ))
    echo "→ background job: half of the -j\$_full_j free memory allows"
fi
if [ -n "\$POOL_ENV" ]; then
    HELIX_JFLAG=""
    echo "→ \$(( _avail_kb / 1048576 ))GB available to $CONTAINER, joining jobpool: \$("\$JP" status --json 2>/dev/null | sed -n 's/.*"target":\([0-9]*\).*/target \1/p')"
else
    HELIX_JFLAG="-j\$HELIX_J"
    echo "→ \$(( _avail_kb / 1048576 ))GB available to $CONTAINER, using -j\$HELIX_J"
fi

D() { sudo -n docker exec -w "$RUNDIR" $GIT_SAFE_ENV -e CCACHE_DIR="$CCACHE" -e HELIX_J="\$HELIX_J" -e HELIX_JFLAG="\$HELIX_JFLAG" "$CONTAINER" bash -lc "\$1"; }

$(ccache_shared "$RUNDIR")

# Before git touches the --commit checkout. A mirror run waited before its sync;
# this second wait costs one docker exec.
$(orphan_wait)

if [ "$MODE" = commit ]; then
    # Submodules are fetched by the update below, for $SHA's pins only. Recursing
    # here fetches the pin of every new superproject commit, and one pin to a
    # submodule commit that was rebased away before pushing fails the whole fetch.
    D 'git fetch --quiet --all --recurse-submodules=no'
    # mutate_diff.py's default base is the nearest fork point among origin/main and the
    # local main. A local main left behind by an earlier job puts that fork point
    # before everything since, so the run is handed foreign hunks and refuses. Bring
    # it level with the remote before the reset below.
    D 'git fetch --quiet origin main && git update-ref refs/heads/main FETCH_HEAD'
    # A copied checkout's index holds the source's inode and ctime for every
    # file. Refreshed first, git compares content and rewrites only what
    # differs; unrefreshed, the reset rewrites every file and make rebuilds all.
    D 'git update-index -q --refresh >/dev/null; git submodule foreach --quiet --recursive "git update-index -q --refresh >/dev/null; true"'
    D 'git reset --hard --quiet $SHA && git submodule update --init --recursive --quiet'
    # A submodule already at its pin keeps the patches an earlier job applied, so a
    # commit that edits a patch in patches/ meets the old revision and the build's
    # drift check refuses. Reapply against this commit's patches/ every run.
    D 'make reapply-patches >/dev/null'
    D 'git log --oneline -1'
fi
if [ -n "\$POOL_ENV" ]; then
    # jobpool exec on the host keeps the pool's consumer live for the whole
    # build; inside, the container opens the FIFO and exports MAKEFLAGS. The
    # FIFO is mode 600, so a container uid that is not root (or is a remapped
    # root) cannot open it; that run takes its own -j instead.
    "\$JP" exec -- sudo -n docker exec -w "$RUNDIR" $GIT_SAFE_ENV -e CCACHE_DIR="$CCACHE" -e HELIX_J="\$HELIX_J" -e HELIX_JFLAG= "$CONTAINER" bash -lc \
        "if { \$POOL_ENV; } 2>/dev/null; then :; else "'echo "→ uid \$(id -u) cannot open the jobpool FIFO; using -j\$HELIX_J"; HELIX_JFLAG=-j\$HELIX_J; fi; $CMD_SQ 2>&1'
else
    D '$CMD_SQ 2>&1'
fi
REMOTE

# A sanitizer run that produced no Catch2 summary ran nothing, whatever its exit
# code said. Refusing to call that a pass is the whole point of checking.
if [ "$WHAT" = asan ] && ! grep -qE 'All tests passed|test cases:|assertions:' "$LOG"; then
    echo ""
    echo "✗ no Catch2 summary in $LOG — the suite did not run, so this is not a clean ASAN result" >&2
    exit 1
fi
# A tagged TSan run tees the binary's own output, so it must carry a Catch2
# summary. The sharded full run keeps shard logs to itself and prints only the
# verdict, so that is what it must carry.
if [ "$WHAT" = tsan ]; then
    if [ -n "${TSAN_TAG:-}" ]; then _want='All tests passed|test cases:|assertions:'
    else _want='TSAN clean'; fi
    if ! grep -qE "$_want" "$LOG"; then
        echo ""
        echo "✗ no '$_want' in $LOG — the suite did not run, so this is not a clean TSAN result" >&2
        exit 1
    fi
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

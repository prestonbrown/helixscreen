#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test-host-run.sh mirrors the working tree to the test host (scripts/test-host-run.sh).
#
# A plain run copies the tree as it is on disk - uncommitted edits and
# untracked files included, ignored files and build output excluded - into a
# per-tree mirror on the test host, and runs the job there. Each mirror has its own
# lock, so two trees run at once while two runs of one tree queue.
#
# ssh is stubbed so its command runs here: rsync's remote half, the lock
# holder and the job heredoc all execute against a sandbox directory that
# stands in for the test host's mirror root. docker is stubbed to record, so no build
# runs. git is real: each test builds a small repository to sync.

TEST_HOST_RUN="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/test-host-run.sh"

setup() {
    load helpers
    export HELIX_TEST_HOST=testhost.invalid
    export HELIX_TEST_LOCK_DIR="$BATS_TEST_TMPDIR/locks"
    export HELIX_TEST_TREES_HOST="$BATS_TEST_TMPDIR/host-trees"
    export TMPDIR="$BATS_TEST_TMPDIR"
    export MOCK_DOCKER_LOG="$BATS_TEST_TMPDIR/docker.log"
    export HELIX_TEST_JOBPOOL="$BATS_TEST_TMPDIR/no-jobpool"
    export HELIX_TEST_ARC_PARAM="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_max"
    export HELIX_TEST_ARC_SYS_FREE="$BATS_TEST_TMPDIR/no-zfs/zfs_arc_sys_free"
    export HELIX_TEST_ARC_MARK="$BATS_TEST_TMPDIR/arc-mark"
    export HELIX_TEST_TREES=/work/trees
    export HELIX_TEST_ORPHAN_POLL_SECS=0
    mkdir -p "$HELIX_TEST_LOCK_DIR"
    REAL_RSYNC=$(command -v rsync)
    export REAL_RSYNC

    # ssh [-o opt]... <host> <command...>: run the command here through a
    # shell, the way sshd hands it to the remote login shell.
    mock_command_script ssh '
while [ $# -gt 0 ]; do
    case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift
exec bash -c "$*"'
    mock_command_script sudo 'shift; exec "$@"'
    mock_command_script docker '
case "$1" in
    ps) echo helix-test ;;
    start) exit 0 ;;
    inspect) exit 0 ;;
    exec)
        case "$*" in
            *pgrep*)
                # MOCK_LIVE_MAKE_DIR: a make (an orphan) runs in that tree.
                if [ -n "${MOCK_LIVE_MAKE_DIR:-}" ]; then
                    case "$*" in *"$MOCK_LIVE_MAKE_DIR|"*) exit 0 ;; esac
                fi
                # MOCK_PGREP_HITS: the first poll sees a live make, and
                # records what the mirror held at that moment.
                if [ -n "${MOCK_PGREP_HITS:-}" ]; then
                    n=$(cat "$MOCK_PGREP_HITS" 2>/dev/null || echo 0)
                    n=$((n + 1)); echo "$n" > "$MOCK_PGREP_HITS"
                    if [ "$n" -eq 1 ]; then
                        cat "$MOCK_MIRROR_PROBE" > "$MOCK_PGREP_SAW" 2>/dev/null || echo absent > "$MOCK_PGREP_SAW"
                        if [ -n "${MOCK_MIRROR_STALE:-}" ] && [ -e "$MOCK_MIRROR_STALE" ]; then
                            echo stale-present >> "$MOCK_PGREP_SAW"
                        fi
                        exit 0
                    fi
                fi
                # MOCK_RUN_PROBE: run the probe script itself, here.
                if [ -n "${MOCK_RUN_PROBE:-}" ]; then
                    shift; while [ "$1" != bash ]; do shift; done
                    exec bash -c "$3"
                fi
                exit 1 ;;
            *"base64 -d"*)
                # MOCK_CCACHE_DIR: run the ccache settings command here, against
                # that cache; its last argument is the bash -c script.
                echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG"
                if [ -n "${MOCK_CCACHE_DIR:-}" ]; then
                    for a in "$@"; do last=$a; done
                    CCACHE_DIR=$MOCK_CCACHE_DIR exec bash -c "$last"
                fi ;;
            *) echo "[docker-exec] $*" >> "$MOCK_DOCKER_LOG"
               # MOCK_EXEC_DIR: run the job command there, with make logged to
               # MOCK_EXEC_LOG instead of run; its last argument is the bash -c script.
               if [ -n "${MOCK_EXEC_DIR:-}" ]; then
                   for a in "$@"; do last=$a; done
                   cd "$MOCK_EXEC_DIR" || exit 1
                   exec bash -c "make() { echo \"make \$*\" >> \"\$MOCK_EXEC_LOG\"; }; $last"
               fi
               # MOCK_SWEEP_FAILS=<mirror>: the sweep fails and leaves a
               # failing shard log in that mirror, as the real one does.
               if [ -n "${MOCK_SWEEP_FAILS:-}" ]; then
                   mkdir -p "$MOCK_SWEEP_FAILS/build/shard-artifacts/helix-shards-T1"
                   echo "hung in: a teardown case" > "$MOCK_SWEEP_FAILS/build/shard-artifacts/helix-shards-T1/92.log"
                   exit 2
               fi ;;
        esac ;;
    *) echo "unhandled docker call: $*" >&2; exit 1 ;;
esac
exit 0'
}

# A repository named <name> under the sandbox, with one committed file, an
# ignore rule for build/ and *.ignored, and a commit identity.
make_tree() { # <name>
    local t="$BATS_TEST_TMPDIR/$1"
    mkdir -p "$t"
    git -C "$t" init -q -b main
    git -C "$t" config user.email t@example.com
    git -C "$t" config user.name tester
    printf 'build/\n*.ignored\n' > "$t/.gitignore"
    echo one > "$t/tracked.txt"
    git -C "$t" add .gitignore tracked.txt
    git -C "$t" commit -qm base --no-verify
    echo "$t"
}

mirror_of() { echo "$HELIX_TEST_TREES_HOST/$1"; }

@test "an uncommitted edit and an untracked file reach the mirror; ignored files and build/ do not" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo edited > "$t/tracked.txt"
    echo new > "$t/untracked.txt"
    echo secret > "$t/local.ignored"
    mkdir -p "$t/build"; echo obj > "$t/build/thing.o"

    cd "$t"
    run "$TEST_HOST_RUN" test '[x]'
    [ "$status" -eq 0 ]
    [ "$(cat "$m/tracked.txt")" = edited ]
    [ "$(cat "$m/untracked.txt")" = new ]
    [ ! -e "$m/local.ignored" ]
    [ ! -e "$m/build" ]
    # The job runs in the mirror, not in the shared commit checkout.
    grep -qF -- "-w /work/trees/tree-a " "$MOCK_DOCKER_LOG"
    refute_grep "git reset" "$MOCK_DOCKER_LOG"
}

@test "every run applies the shared ccache settings in the container and checks they cover the mirror" {
    command -v ccache >/dev/null || skip "ccache is not installed"
    local t
    t=$(make_tree tree-a)
    export MOCK_CCACHE_DIR="$BATS_TEST_TMPDIR/ccache"
    mkdir -p "$MOCK_CCACHE_DIR"
    cd "$t"
    run "$TEST_HOST_RUN" test '[x]'
    [ "$status" -eq 0 ] || { echo "$output" >&2; return 1; }
    contains "→ ccache base_dir: /work (was unset)" "$output"
    lacks "is not a prefix" "$output"
    grep -qx 'base_dir = /work' "$MOCK_CCACHE_DIR/ccache.conf"
    grep -qx 'hash_dir = false' "$MOCK_CCACHE_DIR/ccache.conf"
    grep -qx 'max_size = 200G' "$MOCK_CCACHE_DIR/ccache.conf"
    grep -qF -- "check /work/trees/tree-a" "$MOCK_DOCKER_LOG"

    # A cache whose base_dir drifted is put back on the next run.
    CCACHE_DIR=$MOCK_CCACHE_DIR ccache --set-config base_dir=/elsewhere
    run "$TEST_HOST_RUN" test '[x]'
    [ "$status" -eq 0 ]
    contains "→ ccache base_dir: /work (was /elsewhere)" "$output"
}

@test "a file deleted locally is gone from the mirror after the next sync" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo a > "$t/committed-delete.txt"
    echo b > "$t/worktree-delete.txt"
    echo c > "$t/untracked-delete.txt"
    git -C "$t" add committed-delete.txt worktree-delete.txt
    git -C "$t" commit -qm more --no-verify
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ -e "$m/committed-delete.txt" ] && [ -e "$m/worktree-delete.txt" ] && [ -e "$m/untracked-delete.txt" ]
    # Build output the job wrote is the mirror's own, never in the file list.
    mkdir -p "$m/build"; echo obj > "$m/build/keep.o"

    git -C "$t" rm -q committed-delete.txt
    git -C "$t" commit -qm drop --no-verify
    rm "$t/worktree-delete.txt" "$t/untracked-delete.txt"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ ! -e "$m/committed-delete.txt" ]
    [ ! -e "$m/worktree-delete.txt" ]
    [ ! -e "$m/untracked-delete.txt" ]
    [ -e "$m/tracked.txt" ]
    [ -e "$m/build/keep.o" ]
}

# A submodule at lib/<name> whose checkout lives elsewhere, reached through a
# symlink the way setup-worktree.sh shares lib/ with the main tree.
add_symlinked_submodule() { # <tree> <name> [private]
    local t="$1" name="$2" src="$BATS_TEST_TMPDIR/src-$2"
    git init -q -b main "$src"
    echo "$name source" > "$src/source.c"
    git -C "$src" add source.c
    git -C "$src" -c user.email=t@example.com -c user.name=t commit -qm sub --no-verify
    git -C "$t" -c protocol.file.allow=always submodule add -q "$src" "lib/$name"
    git -C "$t" commit -qm "add $name" --no-verify
    # A private checkout, as setup-worktree.sh gives lvgl, libhv, lua and helix-xml.
    [ "${3:-}" != private ] || return 0
    rm -rf "${t:?}/lib/$name"
    git clone -q "$src" "$BATS_TEST_TMPDIR/shared-$name"
    ln -s "$BATS_TEST_TMPDIR/shared-$name" "$t/lib/$name"
}

@test "a symlinked lib/ submodule syncs as the target's files, not a link" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    add_symlinked_submodule "$t" lvgl
    add_symlinked_submodule "$t" wpa_supplicant
    # A patch-created file in a patched submodule is part of the source; build
    # output in a shared one belongs to the local toolchain.
    echo created > "$BATS_TEST_TMPDIR/shared-lvgl/patch-created.h"
    echo arm > "$BATS_TEST_TMPDIR/shared-wpa_supplicant/libwpa_client.a"
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ -d "$m/lib/lvgl" ] && [ ! -L "$m/lib/lvgl" ]
    [ "$(cat "$m/lib/lvgl/source.c")" = "lvgl source" ]
    [ "$(cat "$m/lib/lvgl/patch-created.h")" = created ]
    [ -f "$m/lib/wpa_supplicant/source.c" ]
    [ ! -e "$m/lib/wpa_supplicant/libwpa_client.a" ]
}

wait_for_file() { # <path>
    local _
    for _ in $(seq 1 200); do [ -e "$1" ] && return 0; sleep 0.05; done
    return 1
}

wait_for_line() { # <substring> <file>
    local _
    for _ in $(seq 1 200); do grep -qF "$1" "$2" 2>/dev/null && return 0; sleep 0.05; done
    return 1
}

hold_lock() { # <lock file> <ready marker>: prints the holder pid
    (
        exec 9>>"$1"
        flock 9
        printf 'held by pid %s: sweep 0123456 since 2026-10-08 12:00:00\n' "$$" >&9
        touch "$2"
        sleep 30 9>&-
    ) >/dev/null 2>&1 &
    echo $!
}

@test "two trees hold different locks and both proceed; two runs of one tree queue" {
    local a b holder out runner
    a=$(make_tree tree-a)
    b=$(make_tree tree-b)
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.tree-tree-a.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"

    # tree-a's lock is held: tree-b runs straight through.
    cd "$b"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    lacks "busy:" "$output"
    grep -q "held by pid [0-9]*: test " "$HELIX_TEST_LOCK_DIR/helix-test-host-run.tree-tree-b.lock"

    # A run of tree-a waits for it, names the holder, and has synced nothing.
    out="$BATS_TEST_TMPDIR/waiting.log"
    cd "$a"
    "$TEST_HOST_RUN" test >"$out" 2>&1 &
    runner=$!
    wait_for_line "busy:" "$out"
    grep -q "busy: held by pid.*: sweep 0123456 since " "$out"
    [ ! -e "$(mirror_of tree-a)/tracked.txt" ]

    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "free; continuing" "$out"
    [ -e "$(mirror_of tree-a)/tracked.txt" ]
}

@test "with no jobpool on the test host, runs of different trees take turns" {
    # -j is sized from MemAvailable at start, which only holds for one run.
    local b holder out runner
    b=$(make_tree tree-b)
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.global.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    out="$BATS_TEST_TMPDIR/waiting.log"
    cd "$b"
    "$TEST_HOST_RUN" test >"$out" 2>&1 &
    runner=$!
    wait_for_line "no jobpool on" "$out"
    refute_grep "available to" "$out"
    kill "$holder"
    wait "$holder" 2>/dev/null || true
    wait "$runner"
    grep -q "available to" "$out"
}

@test "the provenance line and log name carry -dirty and the diff hash only when dirty" {
    local t sha short h1 h2
    t=$(make_tree tree-a)
    cd "$t"
    sha=$(git rev-parse HEAD)
    short=$(git rev-parse --short HEAD)

    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "HEAD $sha + clean" "$output"
    [ -f "$TMPDIR/test-host-test-tree-a-$short.log" ]
    lacks "dirty" "$output"

    echo edited > tracked.txt
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    h1=$(printf '%s\n' "$output" | sed -n 's/.*HEAD [0-9a-f]* + dirty \([0-9a-f]*\), 0 untracked.*/\1/p' | head -1)
    [ -n "$h1" ]
    [ -f "$TMPDIR/test-host-test-tree-a-$short-dirty-$h1.log" ]

    # Another edit is another hash; an untracked file is counted and hashed.
    echo new > untracked.txt
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    h2=$(printf '%s\n' "$output" | sed -n 's/.*HEAD [0-9a-f]* + dirty \([0-9a-f]*\), 1 untracked.*/\1/p' | head -1)
    [ -n "$h2" ]
    [ "$h1" != "$h2" ]
}

@test "--commit on an unpushed commit still refuses" {
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" --commit test
    [ "$status" -eq 1 ]
    contains "is not on any remote branch" "$output"
    [ ! -e "$MOCK_DOCKER_LOG" ]
    [ ! -e "$(mirror_of tree-a)" ]
}

make_mirror() { # <name> <age in days>
    mkdir -p "$HELIX_TEST_TREES_HOST/$1"
    touch -d "$2 days ago" "$HELIX_TEST_TREES_HOST/$1/.helix-mirror-files"
}

@test "--prune removes an old mirror, keeps a fresh one and one in use" {
    make_mirror old 20
    make_mirror fresh 1
    make_mirror busy 20
    local holder
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.tree-busy.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"

    run "$TEST_HOST_RUN" --prune
    kill "$holder"; wait "$holder" 2>/dev/null || true
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_TEST_TREES_HOST/old" ]
    [ -d "$HELIX_TEST_TREES_HOST/fresh" ]
    [ -d "$HELIX_TEST_TREES_HOST/busy" ]
    contains "removed mirror old" "$output"

    # DAYS is honoured: a 1-day-old mirror goes at --prune 0.
    run "$TEST_HOST_RUN" --prune 0
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_TEST_TREES_HOST/fresh" ]
}

@test "--drop removes one tree's mirror" {
    make_mirror gone 0
    make_mirror stays 0
    run "$TEST_HOST_RUN" --drop gone
    [ "$status" -eq 0 ]
    [ ! -e "$HELIX_TEST_TREES_HOST/gone" ]
    [ -d "$HELIX_TEST_TREES_HOST/stays" ]
}

# --- --probe: the automatic full-test-run default ---------------------------

@test "--probe: an unreachable test host means local" {
    mock_command_fail ssh
    run "$TEST_HOST_RUN" --probe
    [ "$status" -eq 1 ]
    contains "unreachable" "$output"
}

@test "--probe: a LAN round trip means the test host without measuring the sync" {
    mock_command ping "64 bytes from testhost: icmp_seq=1 ttl=64 time=0.137 ms"
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" --probe
    [ "$status" -eq 0 ]
    contains "rtt 0.137 ms" "$output"
    contains "sweep runs on testhost.invalid" "$output"
    lacks "estimated sync" "$output"
}

@test "--probe: a slow link with a cold mirror means local, a warm one means the test host" {
    mock_command ping "64 bytes from testhost: icmp_seq=1 ttl=64 time=40.5 ms"
    export HELIX_TEST_PROBE_BYTES_PER_SEC=1000   # pins the measured throughput
    local t
    t=$(make_tree tree-a)
    head -c 50000 /dev/urandom > "$t/big.bin"
    cd "$t"
    run "$TEST_HOST_RUN" --probe
    [ "$status" -eq 1 ]
    contains "estimated sync" "$output"

    # Warm: the mirror already holds every byte, so almost nothing would move.
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    run "$TEST_HOST_RUN" --probe
    [ "$status" -eq 0 ]
    contains "estimated sync" "$output"
}

@test "--probe: measures the link where date has no nanoseconds (BSD date)" {
    mock_command ping "64 bytes from testhost: icmp_seq=1 ttl=64 time=40.5 ms"
    # BSD date prints %N literally.
    mock_command_script date 'echo "1793107829N"'
    unset HELIX_TEST_PROBE_BYTES_PER_SEC
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" --probe
    lacks "value too great" "$output"
    lacks "unknown job" "$output"
    contains "estimated sync" "$output"
}

# --- the orphan wait, and what may happen before it --------------------------

@test "mirror mode waits out an orphaned build before syncing or deleting anything" {
    # A run interrupted by Ctrl-C releases its lock while its make keeps
    # building in the mirror; the next run must not change files under it.
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo doomed > "$t/doomed.txt"
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    echo second-sync > tracked.txt
    rm doomed.txt
    export MOCK_PGREP_HITS="$BATS_TEST_TMPDIR/hits" MOCK_PGREP_SAW="$BATS_TEST_TMPDIR/saw"
    export MOCK_MIRROR_PROBE="$m/tracked.txt" MOCK_MIRROR_STALE="$m/doomed.txt"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "orphaned build still running" "$output"
    # At the first poll the mirror still held the previous sync: neither the
    # new content nor the stale-file delete had happened yet.
    [ "$(cat "$MOCK_PGREP_SAW")" = "$(printf 'one\nstale-present')" ]
    [ "$(cat "$m/tracked.txt")" = second-sync ]
    [ ! -e "$m/doomed.txt" ]
}

@test "the orphan wait counts only makes whose cwd is inside this tree's mirror" {
    local t rundir other pid
    export HELIX_TEST_TREES="$BATS_TEST_TMPDIR/container-trees"
    export MOCK_RUN_PROBE=1
    rundir="$HELIX_TEST_TREES/tree-a"
    other="$HELIX_TEST_TREES/tree-ab"
    mkdir -p "$rundir/lib" "$other"
    # pgrep reports one make; its cwd decides whether it is ours.
    mock_command_script pgrep 'cat "$BATS_TEST_TMPDIR/make.pid"'
    t=$(make_tree tree-a)
    cd "$t"

    # Another tree's build, in a sibling whose name extends ours: not ours.
    (cd "$other" && exec sleep 30) & pid=$!
    echo "$pid" > "$BATS_TEST_TMPDIR/make.pid"
    run timeout 20 "$TEST_HOST_RUN" test
    kill "$pid"; wait "$pid" 2>/dev/null || true
    [ "$status" -eq 0 ]
    lacks "orphaned build" "$output"

    # A sub-make inside our mirror: waited out until it is gone.
    (cd "$rundir/lib" && exec sleep 30) & pid=$!
    echo "$pid" > "$BATS_TEST_TMPDIR/make.pid"
    "$TEST_HOST_RUN" test > "$BATS_TEST_TMPDIR/out" 2>&1 &
    local runner=$!
    for _ in $(seq 1 200); do grep -q "orphaned build" "$BATS_TEST_TMPDIR/out" && break; sleep 0.05; done
    grep -q "orphaned build" "$BATS_TEST_TMPDIR/out"
    kill "$pid"; wait "$pid" 2>/dev/null || true
    wait "$runner"
}

@test "a path that changes between file and directory syncs without wedging the mirror" {
    local t m
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    echo x > "$t/thing"; mkdir -p "$t/dir"; echo y > "$t/dir/a"
    git -C "$t" add thing dir/a; git -C "$t" commit -qm kinds --no-verify
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    # Output the job wrote there: the directory is not empty when it goes.
    echo obj > "$m/dir/out.o"

    git rm -q thing dir/a
    mkdir thing; echo y > thing/a
    echo z > dir
    git add thing/a dir; git commit -qm swapped --no-verify
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ "$(cat "$m/thing/a")" = y ]
    [ "$(cat "$m/dir")" = z ]
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]

    # A stale path that is a directory in the mirror, as an interrupted sync
    # leaves one, does not stop the run either.
    rm -rf "${m:?}/dir"; mkdir -p "$m/dir/sub"
    git rm -q dir; git commit -qm gone --no-verify
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
}

@test "a tree named global runs with no jobpool on the test host" {
    # The per-tree lock and the no-pool global lock are different files.
    local t
    t=$(make_tree global)
    cd "$t"
    run timeout 20 "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
}

@test "an untracked file vanishing while the tree is hashed does not kill the run" {
    local t real
    t=$(make_tree tree-a)
    echo gone > "$t/vanishing.txt"
    real=$(command -v sha1sum)
    # sha1sum of a file that is gone by then: an error, as from a real race.
    mock_command_script sha1sum '
for a in "$@"; do case "$a" in *vanishing.txt) echo "sha1sum: $a: No such file" >&2; exit 1 ;; esac; done
exec '"$real"' "$@"'
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "+ dirty " "$output"
}

@test "rsync's vanished-files exit is a sync, any other failure is not" {
    local t
    t=$(make_tree tree-a)
    # The local rsync only: the server half runs with --server.
    mock_command_script rsync '
"$REAL_RSYNC" "$@" || exit $?
case " $* " in *" --server "*|*" --dry-run "*) exit 0 ;; esac
case " $* " in *" --files-from="*) exit "${MOCK_RSYNC_RC:-0}" ;; esac'
    cd "$t"
    MOCK_RSYNC_RC=24 run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    MOCK_RSYNC_RC=23 run "$TEST_HOST_RUN" test
    [ "$status" -ne 0 ]
}

@test "an untracked file in helix-xml makes the tree dirty" {
    local t
    t=$(make_tree tree-a)
    add_symlinked_submodule "$t" helix-xml private
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "+ clean" "$output"
    echo new > lib/helix-xml/new_widget.c
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "+ dirty " "$output"
    contains "1 untracked" "$output"
}

@test "a tree or --drop name that is not a plain name is refused" {
    mkdir -p "$HELIX_TEST_TREES_HOST/keep"
    local bad
    for bad in '*' '..' '.' 'a b' 'x$(touch pwned)' 'a/b'; do
        run "$TEST_HOST_RUN" --drop "$bad"
        [ "$status" -eq 2 ]
    done
    [ -d "$HELIX_TEST_TREES_HOST/keep" ]
    [ ! -e pwned ] && [ ! -e "$HELIX_TEST_TREES_HOST/pwned" ]
    local t
    t=$(make_tree 'tree$x')
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 2 ]
    [ ! -e "$MOCK_DOCKER_LOG" ]
}

@test "--commit is recognised after the job name too" {
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" test '[x]' --commit
    [ "$status" -eq 1 ]
    contains "is not on any remote branch" "$output"
}

@test "with no test host configured every command refuses in one line and reaches nothing" {
    unset HELIX_TEST_HOST
    local t cmd
    t=$(make_tree tree-a)
    cd "$t"

    # The build-hosts file alone names one.
    echo "HELIX_TEST_HOST=testhost.invalid" > "$HELIX_BUILD_HOSTS_FILE"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "testhost.invalid:helix-test" "$output"

    rm "$HELIX_BUILD_HOSTS_FILE"
    mock_command_script ssh 'touch "$BATS_TEST_TMPDIR/ssh-called"; exit 255'
    for cmd in "test" "--commit sweep" "--prune" "--drop tree-a" "--probe"; do
        # shellcheck disable=SC2086  # one argument per word
        run "$TEST_HOST_RUN" $cmd
        [ "$status" -eq 2 ]
        [ "${#lines[@]}" -eq 1 ]
        contains "HELIX_TEST_HOST is not set" "$output"
        contains "$HELIX_BUILD_HOSTS_FILE" "$output"
    done
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "a candidate-list test host runs on the first host that answers, never the list itself" {
    export HELIX_TEST_HOST=dead.local,good.lan
    local t hosts="$BATS_TEST_TMPDIR/ssh-hosts"
    t=$(make_tree tree-a)
    cd "$t"
    # The setup mock, recording each host it is asked for; dead.local refuses.
    mock_command_script ssh '
while [ $# -gt 0 ]; do
    case "$1" in -o) shift 2 ;; -*) shift ;; *) break ;; esac
done
echo "$1" >> "'"$hosts"'"
case "$1" in
    dead.local) echo "ssh: connect to host dead.local port 22: Connection refused" >&2; exit 255 ;;
esac
shift
exec bash -c "$*"'

    run "$TEST_HOST_RUN" test '[x]'
    [ "$status" -eq 0 ]
    contains "HELIX_TEST_HOST=good.lan (not answering: dead.local: refused)" "$output"
    contains "good.lan:helix-test" "$output"
    # Past the one refused probe, every ssh goes to good.lan.
    [ "$(head -1 "$hosts")" = dead.local ]
    [ "$(tail -n +2 "$hosts" | sort -u)" = good.lan ]
}

# --- content-compared sync, seeding a new mirror, per-tree mutate checkouts ---

mtime_of() { stat -c %Y "$1"; }

# Gives a mirror a build that counts as warm: a linked helix-tests and a
# dependency file naming the mirror's own container path, both an hour old.
warm_build() { # <tree name>
    local m="$HELIX_TEST_TREES_HOST/$1"
    mkdir -p "$m/build/bin" "$m/build/obj"
    echo bin > "$m/build/bin/helix-tests"
    printf '/work/trees/%s/build/obj/x.o: src/x.c /work/trees/%s-1.0/keep.h\n' "$1" "$1" > "$m/build/obj/x.d"
    printf "{'d':'/work/trees/%s','i':\"-I/work/trees/%s:x\"}\n" "$1" "$1" > "$m/build/obj/x.ccj"
    touch -d '1 hour ago' "$m/build/bin/helix-tests" "$m/build/obj/x.d"
}

@test "the sync compares content: a new local time alone rewrites nothing, a change with an old time is rewritten" {
    local t m before
    t=$(make_tree tree-a)
    m=$(mirror_of tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    touch -d '2 hours ago' "$m/tracked.txt"
    before=$(mtime_of "$m/tracked.txt")

    touch "$t/tracked.txt"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ "$(mtime_of "$m/tracked.txt")" -eq "$before" ]

    # An edit whose local time is older than any object the mirror built.
    echo changed > "$t/tracked.txt"
    touch -d '2001-01-01' "$t/tracked.txt"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ "$(cat "$m/tracked.txt")" = changed ]
    [ "$(mtime_of "$m/tracked.txt")" -gt "$before" ]
}

@test "a run records the HEAD it synced in the mirror" {
    local t
    t=$(make_tree tree-a)
    cd "$t"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    [ "$(cat "$(mirror_of tree-a)/.helix-mirror-head")" = "$(git rev-parse HEAD)" ]
}

@test "a new tree's mirror is seeded from the nearest warm mirror, and the sync makes its files exact" {
    local a b c ma mb
    a=$(make_tree tree-a)
    echo gone > "$a/only-in-a.txt"
    echo same > "$a/same.txt"
    git -C "$a" add only-in-a.txt same.txt
    git -C "$a" commit -qm a --no-verify
    # tree-c: three commits away from a's fork point; tree-b: one commit past a.
    git -C "$a" worktree add -q -b c "$BATS_TEST_TMPDIR/tree-c" HEAD~1
    c=$BATS_TEST_TMPDIR/tree-c
    for i in 1 2 3; do echo "$i" > "$c/c$i.txt"; git -C "$c" add "c$i.txt"; git -C "$c" commit -qm "c$i" --no-verify; done
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    git -C "$b" rm -q only-in-a.txt
    echo b-edit > "$b/tracked.txt"
    git -C "$b" commit -qam b --no-verify

    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    (cd "$c" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    warm_build tree-c
    ma=$(mirror_of tree-a); mb=$(mirror_of tree-b)
    touch -d '3 hours ago' "$ma/same.txt" "$ma/tracked.txt"
    # The far mirror ran last: history decides, not recency.
    touch -d '2 days ago' "$ma/.helix-mirror-files"

    cd "$b"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "seeded tree-b's mirror from tree-a" "$output"
    [ -e "$mb/build/bin/helix-tests" ]
    # The seed's dependency file names this mirror's path; a sibling's path that
    # merely starts the same way is left alone, and its time is kept.
    [ "$(cat "$mb/build/obj/x.d")" = "/work/trees/tree-b/build/obj/x.o: src/x.c /work/trees/tree-a-1.0/keep.h" ]
    [ "$(mtime_of "$mb/build/obj/x.d")" -eq "$(mtime_of "$ma/build/obj/x.d")" ]
    [ "$(cat "$mb/build/obj/x.ccj")" = "{'d':'/work/trees/tree-b','i':\"-I/work/trees/tree-b:x\"}" ]
    # The files are exact: b's edit, a's extra file gone, an unchanged file
    # keeps the seed's time while a changed one is newer than the seed's build.
    [ "$(cat "$mb/tracked.txt")" = b-edit ]
    [ ! -e "$mb/only-in-a.txt" ]
    [ "$(mtime_of "$mb/same.txt")" -eq "$(mtime_of "$ma/same.txt")" ]
    [ "$(mtime_of "$mb/tracked.txt")" -gt "$(mtime_of "$mb/build/bin/helix-tests")" ]
    [ "$(cat "$mb/.helix-mirror-head")" = "$(git -C "$b" rev-parse HEAD)" ]
    # The seed itself is untouched, and nothing temporary is left behind.
    [ "$(cat "$ma/build/obj/x.d")" = "/work/trees/tree-a/build/obj/x.o: src/x.c /work/trees/tree-a-1.0/keep.h" ]
    [ ! -e "$mb+seeding" ]

    # A mirror that has a build/ is never seeded again, nor looks for a seed.
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    lacks "seed" "$output"
}

@test "a busy seed is passed over, and with no idle one the run builds cold" {
    local a b holder
    a=$(make_tree tree-a)
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.tree-tree-a.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    cd "$b"
    run "$TEST_HOST_RUN" test
    kill "$holder"; wait "$holder" 2>/dev/null || true
    [ "$status" -eq 0 ]
    contains "seed candidate tree-a is busy" "$output"
    contains "building cold" "$output"
    [ ! -e "$(mirror_of tree-b)/build" ]
}

@test "HELIX_TEST_SEED=0 starts a new mirror cold" {
    local a b
    a=$(make_tree tree-a)
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    cd "$b"
    HELIX_TEST_SEED=0 run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    lacks "seed" "$output"
    [ ! -e "$(mirror_of tree-b)/build" ]
}

@test "seeding a mirror prunes mirrors unused for HELIX_TEST_PRUNE_DAYS" {
    local a b
    a=$(make_tree tree-a)
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    make_mirror ancient 30
    cd "$b"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "pruned (unused for 14 days) mirror ancient" "$output"
    [ ! -e "$HELIX_TEST_TREES_HOST/ancient" ]
    [ -d "$(mirror_of tree-a)" ]
}

# A pushed tree for mutate: the remote-tracking ref is what the push check reads.
pushed_tree() { # <name>
    local t
    t=$(make_tree "$1")
    git -C "$t" update-ref refs/remotes/origin/main HEAD
    echo "$t"
}

# The shared --commit checkout, as the host sees it: beside the mirror root.
shared_checkout() {
    local s="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$s/.git" "$s/build/obj"
    touch "$s/.git/index"
    printf '/work/helixscreen/build/obj/x.o: src/x.c\n' > "$s/build/obj/x.d"
    echo "$s"
}

@test "mutate runs in this tree's own checkout, copied from the shared one, under a lock of its own" {
    local t m
    t=$(pushed_tree tree-a)
    shared_checkout >/dev/null
    m=$BATS_TEST_TMPDIR/mutate/tree-a
    cd "$t"
    run "$TEST_HOST_RUN" mutate --tests '[x]'
    [ "$status" -eq 0 ]
    contains "seeded /work/mutate/tree-a from helixscreen" "$output"
    [ -d "$m/.git" ]
    [ "$(cat "$m/build/obj/x.d")" = "/work/mutate/tree-a/build/obj/x.o: src/x.c" ]
    grep -qF -- "-w /work/mutate/tree-a " "$MOCK_DOCKER_LOG"
    refute_grep "-w /work/helixscreen " "$MOCK_DOCKER_LOG"
    grep -q "held by pid .*: mutate tree-a-" "$HELIX_TEST_LOCK_DIR/helix-test-host-run.mutate-tree-a.lock"
    # The copied index is refreshed before the reset, so git rewrites only what differs.
    local refresh reset
    refresh=$(grep -nF "git update-index -q --refresh" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    reset=$(grep -nF "git reset --hard" "$MOCK_DOCKER_LOG" | head -1 | cut -d: -f1)
    [ -n "$refresh" ] && [ "$refresh" -lt "$reset" ]
}

@test "two trees mutate at once: one tree's checkout and the shared one being busy do not stop another" {
    local a b h1 h2
    a=$(pushed_tree tree-a)
    b=$(pushed_tree tree-b)
    shared_checkout >/dev/null
    mkdir -p "$BATS_TEST_TMPDIR/mutate/tree-b/.git"
    h1=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.mutate-tree-a.lock" "$BATS_TEST_TMPDIR/ready1")
    h2=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-zeus-run-helixscreen.lock" "$BATS_TEST_TMPDIR/ready2")
    wait_for_file "$BATS_TEST_TMPDIR/ready1"; wait_for_file "$BATS_TEST_TMPDIR/ready2"
    cd "$b"
    run timeout 20 "$TEST_HOST_RUN" mutate
    kill "$h1" "$h2"; wait "$h1" "$h2" 2>/dev/null || true
    [ "$status" -eq 0 ]
    lacks "busy" "$output"
    grep -qF -- "-w /work/mutate/tree-b " "$MOCK_DOCKER_LOG"
}

@test "a new mutate checkout copies the newest idle one, and waits when every one is busy" {
    local t holder out runner
    t=$(pushed_tree tree-c)
    shared_checkout >/dev/null
    mkdir -p "$BATS_TEST_TMPDIR/mutate/tree-a/.git"
    echo from-a > "$BATS_TEST_TMPDIR/mutate/tree-a/marker"
    touch -d '1 day ago' "$BATS_TEST_TMPDIR/helixscreen/.git"
    cd "$t"
    run "$TEST_HOST_RUN" mutate
    [ "$status" -eq 0 ]
    contains "seeded /work/mutate/tree-c from tree-a" "$output"
    [ "$(cat "$BATS_TEST_TMPDIR/mutate/tree-c/marker")" = from-a ]

    rm -rf "$BATS_TEST_TMPDIR/mutate/tree-c"
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.mutate-tree-a.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    local h2
    h2=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-zeus-run-helixscreen.lock" "$BATS_TEST_TMPDIR/ready2")
    wait_for_file "$BATS_TEST_TMPDIR/ready2"
    out="$BATS_TEST_TMPDIR/waiting.log"
    "$TEST_HOST_RUN" mutate >"$out" 2>&1 &
    runner=$!
    wait_for_line "every mutate checkout is busy" "$out"
    [ ! -e "$BATS_TEST_TMPDIR/mutate/tree-c" ]
    kill "$holder"; wait "$holder" 2>/dev/null || true
    wait "$runner"
    kill "$h2"; wait "$h2" 2>/dev/null || true
    [ -d "$BATS_TEST_TMPDIR/mutate/tree-c/.git" ]
}

@test "mutate on an unpushed commit still refuses, before any checkout is made" {
    local t
    t=$(make_tree tree-a)
    shared_checkout >/dev/null
    cd "$t"
    run "$TEST_HOST_RUN" mutate
    [ "$status" -ne 0 ]
    contains "push it first" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/mutate/tree-a" ]
}

@test "--prune and --drop cover mutate checkouts too" {
    mkdir -p "$BATS_TEST_TMPDIR/mutate/old/.git" "$BATS_TEST_TMPDIR/mutate/fresh/.git" "$BATS_TEST_TMPDIR/mutate/gone/.git"
    touch -d '20 days ago' "$BATS_TEST_TMPDIR/mutate/old/.git/index"
    touch "$BATS_TEST_TMPDIR/mutate/fresh/.git/index" "$BATS_TEST_TMPDIR/mutate/gone/.git/index"
    make_mirror gone 0
    run "$TEST_HOST_RUN" --prune
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/mutate/old" ]
    [ -d "$BATS_TEST_TMPDIR/mutate/fresh" ]
    contains "removed mutate checkout old" "$output"
    run "$TEST_HOST_RUN" --drop gone
    [ "$status" -eq 0 ]
    [ ! -e "$BATS_TEST_TMPDIR/mutate/gone" ]
    [ ! -e "$HELIX_TEST_TREES_HOST/gone" ]
    [ -d "$BATS_TEST_TMPDIR/mutate/fresh" ]
}

@test "a seed candidate with an orphaned build running in it is passed over" {
    local a b
    a=$(make_tree tree-a)
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    cd "$b"
    MOCK_LIVE_MAKE_DIR=/work/trees/tree-a run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "seed candidate tree-a has an orphaned build running" "$output"
    [ ! -e "$(mirror_of tree-b)/build" ]
}

@test "a mirror without a recorded HEAD is no seed, and its next run stamps its files once" {
    local a b ma
    a=$(make_tree tree-a)
    git -C "$a" worktree add -q -b b "$BATS_TEST_TMPDIR/tree-b"
    b=$BATS_TEST_TMPDIR/tree-b
    (cd "$a" && "$TEST_HOST_RUN" test >/dev/null)
    warm_build tree-a
    ma=$(mirror_of tree-a)
    rm "$ma/.helix-mirror-head"
    touch -d '3 hours ago' "$ma/tracked.txt"
    (cd "$b" && run "$TEST_HOST_RUN" test && [ "$status" -eq 0 ] && lacks "seeded" "$output")
    [ ! -e "$(mirror_of tree-b)/build" ]

    cd "$a"
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    contains "predates content-compared syncs" "$output"
    [ "$(mtime_of "$ma/tracked.txt")" -gt "$(mtime_of "$ma/build/bin/helix-tests")" ]
    [ -e "$ma/.helix-mirror-head" ]
    # Once only: the HEAD it now records ends it.
    touch -d '3 hours ago' "$ma/tracked.txt"
    run "$TEST_HOST_RUN" test
    lacks "predates" "$output"
    [ "$(mtime_of "$ma/tracked.txt")" -lt "$(mtime_of "$ma/build/bin/helix-tests")" ]
}

@test "a run waits for a copy into its mirror that a killed run left going" {
    local t out runner holder
    t=$(make_tree tree-a)
    holder=$(hold_lock "$HELIX_TEST_LOCK_DIR/helix-test-host-run.seed-tree-tree-a.lock" "$BATS_TEST_TMPDIR/ready")
    wait_for_file "$BATS_TEST_TMPDIR/ready"
    out="$BATS_TEST_TMPDIR/waiting.log"
    (cd "$t" && HELIX_TEST_SEED=0 "$TEST_HOST_RUN" test >"$out" 2>&1) &
    runner=$!
    wait_for_line "a copy into tree-a is still running" "$out"
    sleep 0.5
    [ ! -e "$(mirror_of tree-a)/tracked.txt" ]
    kill "$holder"; wait "$holder" 2>/dev/null || true
    wait "$runner"
    [ -e "$(mirror_of tree-a)/tracked.txt" ]
}

@test "prune leaves a copy in progress alone and collects one a dead run left a day ago" {
    mkdir -p "$HELIX_TEST_TREES_HOST/new+seeding" "$HELIX_TEST_TREES_HOST/dead+seeding"
    touch -d '30 days ago' "$HELIX_TEST_TREES_HOST/new+seeding/.helix-mirror-files"
    touch -d '2 days ago' "$HELIX_TEST_TREES_HOST/dead+seeding"
    run "$TEST_HOST_RUN" --prune 0
    [ "$status" -eq 0 ]
    [ -d "$HELIX_TEST_TREES_HOST/new+seeding" ]
    [ ! -e "$HELIX_TEST_TREES_HOST/dead+seeding" ]
}

@test "a failed sweep brings its failing shards' logs back to the local tree" {
    local t m
    t=$(make_tree tree-s)
    m=$(mirror_of tree-s)
    cd "$t"
    MOCK_SWEEP_FAILS="$m" run "$TEST_HOST_RUN" sweep
    [ "$status" -ne 0 ]
    [ "$(cat "$t/build/shard-artifacts/helix-shards-T1/92.log")" = "hung in: a teardown case" ]
    contains "failing shards' logs copied here: build/shard-artifacts/helix-shards-T1" "$output"
    # The sweep keeps them inside the mirror, where they outlive the container.
    grep -qF "SHARD_ARTIFACT_ROOT=build/shard-artifacts" "$MOCK_DOCKER_LOG"
}

@test "a passing sweep copies nothing back" {
    local t
    t=$(make_tree tree-p)
    cd "$t"
    run "$TEST_HOST_RUN" sweep
    [ "$status" -eq 0 ]
    [ ! -e "$t/build/shard-artifacts" ]
}

# --- test and exec: arguments arrive word for word -----------------------------

# A mirror run whose job command runs here, in a directory holding a stand-in
# helix-tests that logs each argument it receives on its own line.
job_runner() {
    export MOCK_EXEC_DIR="$BATS_TEST_TMPDIR/container" MOCK_EXEC_LOG="$BATS_TEST_TMPDIR/exec.log"
    mkdir -p "$MOCK_EXEC_DIR/build/bin"
    cat > "$MOCK_EXEC_DIR/build/bin/helix-tests" <<'EOF2'
#!/usr/bin/env bash
echo "helix-tests argc=$#" >> "$MOCK_EXEC_LOG"
for a in "$@"; do printf 'arg[%s]\n' "$a" >> "$MOCK_EXEC_LOG"; done
EOF2
    chmod +x "$MOCK_EXEC_DIR/build/bin/helix-tests"
    : > "$MOCK_EXEC_LOG"
    cd "$(make_tree tree-args)" || return 1
}

@test "test forwards every argument to helix-tests as its own word" {
    job_runner
    run "$TEST_HOST_RUN" test -# '[#test_x]'
    [ "$status" -eq 0 ]
    grep -qxF "helix-tests argc=2" "$MOCK_EXEC_LOG"
    grep -qxF "arg[-#]" "$MOCK_EXEC_LOG"
    grep -qxF "arg[[#test_x]]" "$MOCK_EXEC_LOG"
    grep -qF "make test" "$MOCK_EXEC_LOG"
}

@test "test with no arguments runs the whole suite: helix-tests gets none" {
    job_runner
    run "$TEST_HOST_RUN" test
    [ "$status" -eq 0 ]
    grep -qxF "helix-tests argc=0" "$MOCK_EXEC_LOG"
}

@test "an argument with spaces, quotes or a semicolon arrives intact and is never executed" {
    job_runner
    run "$TEST_HOST_RUN" test 'two words' 'x" ; touch PWNED ; echo "' "it's"
    [ "$status" -eq 0 ]
    grep -qxF "helix-tests argc=3" "$MOCK_EXEC_LOG"
    grep -qxF "arg[two words]" "$MOCK_EXEC_LOG"
    grep -qxF 'arg[x" ; touch PWNED ; echo "]' "$MOCK_EXEC_LOG"
    grep -qxF "arg[it's]" "$MOCK_EXEC_LOG"
    [ ! -e "$MOCK_EXEC_DIR/PWNED" ]
}

@test "exec runs a command in the mirror without building; --build runs make test first" {
    job_runner
    printf 'printf "x.sh[%%s]\\n" "$@" >> "$MOCK_EXEC_LOG"\n' > "$MOCK_EXEC_DIR/x.sh"
    run "$TEST_HOST_RUN" exec -- bash ./x.sh 'a b' '"; touch PWNED; "'
    [ "$status" -eq 0 ]
    grep -qxF "x.sh[a b]" "$MOCK_EXEC_LOG"
    grep -qxF 'x.sh["; touch PWNED; "]' "$MOCK_EXEC_LOG"
    refute_grep "make test" "$MOCK_EXEC_LOG"
    [ ! -e "$MOCK_EXEC_DIR/PWNED" ]
    : > "$MOCK_EXEC_LOG"
    run "$TEST_HOST_RUN" exec --build -- bash ./x.sh c
    [ "$status" -eq 0 ]
    head -1 "$MOCK_EXEC_LOG" | grep -q "^make test"
    grep -qxF "x.sh[c]" "$MOCK_EXEC_LOG"
}

@test "exec refuses a missing command or an unknown option before reaching the host" {
    job_runner
    run "$TEST_HOST_RUN" exec --
    [ "$status" -eq 2 ]
    contains "exec needs a command" "$output"
    run "$TEST_HOST_RUN" exec bash ./x.sh
    [ "$status" -eq 2 ]
    contains "exec takes [--build] -- COMMAND" "$output"
    [ ! -s "$MOCK_DOCKER_LOG" ]
}

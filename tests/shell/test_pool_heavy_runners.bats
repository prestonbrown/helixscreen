#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything heavy shares the jobpool's one budget: the bats suite follows a
# growing hold's slot file, container builds size themselves from
# JOBPOOL_SLOTS (`helix-claim hold`, scripts/pool-docker.sh), and a
# container's make, or idf.py's ninja 1.13, joins the pool itself. Without
# jobpool each runs exactly as it would with no pool at all.
#
# The pool is always a fake named through HELIX_JOBPOOL; no test reaches the
# real one, and no test fetches a ninja: the cache is private and the
# download URL leads nowhere unless a test says otherwise.

bats_require_minimum_version 1.5.0
load helpers

REPO="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
CLAIM="$REPO/scripts/helix-claim"
POOL_DOCKER="$REPO/scripts/pool-docker.sh"

setup() {
    # The suite itself runs under a hold; nothing here may inherit it.
    unset JOBPOOL JOBPOOL_SLOTS MAKEFLAGS MFLAGS MAKELEVEL HELIX_JOBS_FILE
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    FAKE="$BATS_TEST_TMPDIR/fake-jobpool"
    export FAKE_LOG="$BATS_TEST_TMPDIR/fake-jobpool.log"
    BIN="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$BIN"
    export HELIX_NINJA_CACHE="$BATS_TEST_TMPDIR/ninja-cache"
    export HELIX_NINJA_URL="file://$BATS_TEST_TMPDIR/no-such-ninja.zip"
    # A docker that prints its argv one per line, then the env it was given.
    cat > "$BIN/docker" <<'EOF'
#!/bin/sh
printf '%s\n' "$@"
echo "ENV JOBPOOL_SLOTS=${JOBPOOL_SLOTS-unset}"
EOF
    chmod +x "$BIN/docker"
}

# A jobpool that holds 5 slots, mounts /pool and joins with a recognisable line.
# Its hold has --grow, writing the slot count to FILE, unless FAKE_NO_GROW is
# set; its usage line says which.
fake_pool() {
    export HELIX_JOBPOOL="$FAKE"
    cat > "$FAKE" <<'EOF'
#!/bin/sh
case "$1" in
    '') if [ -n "${FAKE_NO_GROW-}" ]; then echo "usage: jobpool hold [-n N] -- CMD..." >&2
        else echo "usage: jobpool hold [-n N] [--grow FILE] -- CMD..." >&2; fi
        exit 2 ;;
esac
echo "$*" >> "$FAKE_LOG"
case "$1" in
    target) echo 7 ;;
    hold) shift; g=
        while [ "$1" != -- ]; do [ "$1" = --grow ] && { g=$2; shift; }; shift; done; shift
        [ -z "$g" ] || echo "${FAKE_SLOTS:-5}" > "$g"
        JOBPOOL_SLOTS=${FAKE_SLOTS:-5} exec "$@" ;;
    exec) echo "exec MAKEFLAGS=[${MAKEFLAGS-}]" >> "$FAKE_LOG"; shift 2; exec "$@" ;;
    docker-args) echo "--volume=/pool:$2" ;;
    container-env) echo "exec 3<>$2/fifo 4<>$2/fifo && export MAKEFLAGS=-j\\ --jobserver-auth=3,4" ;;
    *) exit 2 ;;
esac
EOF
    chmod +x "$FAKE"
}

no_pool() {
    unset HELIX_JOBPOOL
    unset -f jobpool 2>/dev/null || true
    PATH="$BIN:$(path_without_jobpool)"
    export PATH
}

# ---------------------------------------------------------------------------
# helix-claim hold
# ---------------------------------------------------------------------------

@test "hold runs the command under jobpool hold when jobpool is installed" {
    fake_pool
    run "$CLAIM" hold -n 3 --min 2 --min-wait 9 -- sh -c 'echo "slots=$JOBPOOL_SLOTS"'
    [ "$status" -eq 0 ]
    [ "$output" = "slots=5" ]
    [ "$(cat "$FAKE_LOG")" = "hold -n 3 --min 2 --min-wait 9 -- sh -c echo \"slots=\$JOBPOOL_SLOTS\"" ]
}

@test "hold without jobpool, or with JOBPOOL=0, sizes from -n or the cores" {
    no_pool
    run "$CLAIM" hold -- sh -c 'echo "slots=$JOBPOOL_SLOTS"'
    [ "$output" = "slots=$(nproc)" ]
    run "$CLAIM" hold -n 3 -- sh -c 'echo "slots=$JOBPOOL_SLOTS"'
    [ "$output" = "slots=3" ]
    fake_pool
    JOBPOOL=0 run "$CLAIM" hold -n 2 -- sh -c 'echo "slots=$JOBPOOL_SLOTS"'
    [ "$output" = "slots=2" ]
    [ ! -e "$FAKE_LOG" ]
}

# ---------------------------------------------------------------------------
# make test-shell: the bats suite's -j is the hold
# ---------------------------------------------------------------------------

# make test-shell with bats and GNU parallel replaced by echoes; the fake
# bats also reports the slot file it was handed. Temp files land in
# $BATS_TEST_TMPDIR/tmp, which must be empty again afterwards.
test_shell() {
    printf '#!/bin/sh\necho "BATS $*"\n[ ! -f "${HELIX_JOBS_FILE-}" ] || echo "FILE $(cat "$HELIX_JOBS_FILE")"\n' > "$BIN/bats"
    printf '#!/bin/sh\n' > "$BIN/parallel"
    chmod +x "$BIN/bats" "$BIN/parallel"
    mkdir -p "$BATS_TEST_TMPDIR/tmp"
    (cd "$(scratch_tree)" && env TMPDIR="$BATS_TEST_TMPDIR/tmp" PATH="$BIN:$(path_without_jobpool)" make -s --no-print-directory test-shell 2>&1)
}

@test "make test-shell runs bats on the pool's growing slot file" {
    fake_pool
    run test_shell
    [ "$status" -eq 0 ]
    contains "BATS --jobs 2 --parallel-binary-name $(scratch_tree)/scripts/parallel-jobs-file.sh --no-parallelize-within-files tests/shell/" "$output"
    contains "FILE 5" "$output"
    # A third of the fake pool's target of 7, and a slot file to grow.
    grep -q '^hold --min 2 --grow .*/slots -- ' "$FAKE_LOG"
    [ -z "$(ls -A "$BATS_TEST_TMPDIR/tmp")" ]
    # One slot still runs in parallel: the file can grow past it.
    FAKE_SLOTS=1 run test_shell
    contains "BATS --jobs 2 --parallel-binary-name" "$output"
    contains "FILE 1" "$output"
}

@test "make test-shell with a jobpool that cannot grow runs bats at the fixed count" {
    fake_pool
    export FAKE_NO_GROW=1
    run test_shell
    [ "$status" -eq 0 ]
    contains "BATS --jobs 5 --no-parallelize-within-files tests/shell/" "$output"
    ! contains "FILE" "$output"
    grep -q '^hold --min 2 -- ' "$FAKE_LOG"
    [ -z "$(ls -A "$BATS_TEST_TMPDIR/tmp")" ]
}

@test "make test-shell on a single fixed slot runs bats serially" {
    fake_pool
    export FAKE_NO_GROW=1
    FAKE_SLOTS=1 run test_shell
    [ "$status" -eq 0 ]
    contains "BATS tests/shell/" "$output"
    ! contains "--jobs" "$output"
}

@test "make test-shell without jobpool runs bats on every core, as before" {
    no_pool
    run test_shell
    [ "$status" -eq 0 ]
    contains "BATS --jobs $(nproc) --no-parallelize-within-files tests/shell/" "$output"
    ! contains "FILE" "$output"
}

# make test-shell with the fake bats on a PATH from which GNU parallel is
# gone: each directory that holds one is replaced by a copy of its links
# without it.
test_shell_without_parallel() {
    printf '#!/bin/sh\necho "BATS $*"\n' > "$BIN/bats"
    chmod +x "$BIN/bats"
    local d p="" n=0 shadow IFS=:
    for d in $(path_without_jobpool); do
        if [ -e "$d/parallel" ]; then
            n=$((n + 1)); shadow="$BATS_TEST_TMPDIR/no-parallel-$n"
            [ -d "$shadow" ] || { mkdir "$shadow" &&
                ls "$d" | grep -vx parallel | sed "s|^|$d/|" | xargs sh -c 'ln -s "$@" "$0"' "$shadow"; }
            d=$shadow
        fi
        p=${p:+$p:}$d
    done
    unset IFS
    (cd "$(scratch_tree)" && env PATH="$BIN:$p" make -s --no-print-directory test-shell "$@" 2>&1)
}

@test "make test-shell without GNU parallel refuses to run serially unless allowed" {
    no_pool
    run test_shell_without_parallel
    [ "$status" -ne 0 ]
    contains "GNU parallel not found" "$output"
    contains "apt install parallel" "$output"
    contains "BATS_ALLOW_SERIAL=1" "$output"
    ! contains "BATS " "$output"
    run test_shell_without_parallel BATS_ALLOW_SERIAL=1
    [ "$status" -eq 0 ]
    contains "BATS tests/shell/" "$output"
    # A gate never runs it serially, allowed or not.
    run test_shell_without_parallel BATS_ALLOW_SERIAL=1 BATS_REQUIRED=1
    [ "$status" -ne 0 ]
    contains "GNU parallel not found" "$output"
    ! contains "BATS " "$output"
}

# ---------------------------------------------------------------------------
# parallel-jobs-file.sh: bats's parallel, reading the slot file
# ---------------------------------------------------------------------------

@test "parallel-jobs-file hands GNU parallel the slot file, and only its --jobs" {
    printf '#!/bin/sh\nprintf "%%s\\n" "$@"\n' > "$BIN/parallel"
    chmod +x "$BIN/parallel"
    echo 6 > "$BATS_TEST_TMPDIR/slots"
    HELIX_JOBS_FILE="$BATS_TEST_TMPDIR/slots" PATH="$BIN:$PATH" run "$REPO/scripts/parallel-jobs-file.sh" \
        --keep-order --jobs 2 -- bats-exec-file -j 2
    [ "$output" = "$(printf '%s\n' --keep-order --jobs "$BATS_TEST_TMPDIR/slots" -- bats-exec-file -j 2)" ]
}

@test "parallel-jobs-file without a slot file passes everything through" {
    printf '#!/bin/sh\nprintf "%%s\\n" "$@"\n' > "$BIN/parallel"
    chmod +x "$BIN/parallel"
    PATH="$BIN:$PATH" run "$REPO/scripts/parallel-jobs-file.sh" --keep-order --jobs 2 -- x
    [ "$output" = "$(printf '%s\n' --keep-order --jobs 2 -- x)" ]
    HELIX_JOBS_FILE="$BATS_TEST_TMPDIR/missing" PATH="$BIN:$PATH" run "$REPO/scripts/parallel-jobs-file.sh" --jobs 2 -- x
    [ "$output" = "$(printf '%s\n' --jobs 2 -- x)" ]
}

# The suite's sandbox reaches every test as exported bash functions.
@test "parallel-jobs-file keeps exported bash functions for the tests below it" {
    printf '#!/usr/bin/env bash\nhelix_probe_fn\n' > "$BIN/parallel"
    chmod +x "$BIN/parallel"
    helix_probe_fn() { echo "function arrived"; }
    export -f helix_probe_fn
    PATH="$BIN:$PATH" run "$REPO/scripts/parallel-jobs-file.sh" --jobs 2 -- x
    [ "$output" = "function arrived" ]
}

# Real bats over real GNU parallel: six files whose tests each count how many
# run at once. bats is told --jobs 2; the slot file says 6.
@test "bats on parallel-jobs-file runs as many files at once as the slot file says" {
    command -v parallel >/dev/null 2>&1 || skip "GNU parallel not installed"
    local suite="$BATS_TEST_TMPDIR/suite" i
    mkdir -p "$suite"
    echo 0 > "$suite/cur"
    echo 0 > "$suite/max"
    cat > "$suite/probe" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
flock "$d/lock" sh -c 'c=$(($(cat "$1/cur") + 1)); echo $c > "$1/cur"; [ $c -le $(cat "$1/max") ] || echo $c > "$1/max"' sh "$d"
sleep 1.5
flock "$d/lock" sh -c 'echo $(($(cat "$1/cur") - 1)) > "$1/cur"' sh "$d"
EOF
    chmod +x "$suite/probe"
    # printf, not a heredoc: bats rewrites any line that opens with a test header.
    for i in 1 2 3 4 5 6; do
        printf '@test "t%s" {\n    "%s/probe"\n}\n' "$i" "$suite" > "$suite/f$i.bats"
    done
    echo 6 > "$suite/slots"
    HELIX_JOBS_FILE="$suite/slots" run bats --jobs 2 --parallel-binary-name "$REPO/scripts/parallel-jobs-file.sh" \
        --no-parallelize-within-files "$suite"
    [ "$status" -eq 0 ]
    [ "$(cat "$suite/max")" -gt 2 ]
}

# ---------------------------------------------------------------------------
# pool-docker.sh
# ---------------------------------------------------------------------------

@test "pool-docker: a container make joins the pool and loses its -j" {
    fake_pool
    export PATH="$BIN:$PATH"
    run "$POOL_DOCKER" docker run --rm -v /a:/b img make PLATFORM_TARGET=pi -j8 all
    [ "$status" -eq 0 ]
    [ "${lines[0]}" = run ]
    [ "${lines[1]}" = "--volume=/pool:/run/jobpool" ]
    [ "${lines[2]}" = --rm ]
    [ "${lines[5]}" = img ]
    [ "${lines[6]}" = sh ]
    [ "${lines[7]}" = -c ]
    [ "${lines[9]}" = make ]
    [ "${lines[10]}" = PLATFORM_TARGET=pi ]
    [ "${lines[11]}" = all ]
    grep -qx 'exec -- docker run --volume=/pool:/run/jobpool --rm -v /a:/b img sh -c .*' "$FAKE_LOG"
}

@test "pool-docker: under a make's private jobserver the run still registers with the pool" {
    fake_pool
    export PATH="$BIN:$PATH"
    MAKEFLAGS=" -j4 --jobserver-auth=3,4" run "$POOL_DOCKER" docker run img make all
    [ "$status" -eq 0 ]
    grep -qx 'exec MAKEFLAGS=\[\]' "$FAKE_LOG"
}

# The in-container line pool-docker.sh writes, run here with the pool dir
# moved to $1 and make replaced by an echo.
inner() {
    local script
    script=$(fake_pool; PATH="$BIN:$PATH" "$POOL_DOCKER" docker run img make X=1 -j8 all | awk 'f { print; exit } $0 == "-c" { f = 1 }')
    mkdir -p "$BATS_TEST_TMPDIR/inbin"
    printf '#!/bin/sh\necho "MAKE $* MAKEFLAGS=${MAKEFLAGS-}"\n' > "$BATS_TEST_TMPDIR/inbin/make"
    chmod +x "$BATS_TEST_TMPDIR/inbin/make"
    PATH="$BATS_TEST_TMPDIR/inbin:$PATH" sh -c "${script//\/run\/jobpool/$1}" make X=1 all
}

@test "pool-docker: inside, make joins the FIFO, or keeps its -j when it cannot open it" {
    mkdir "$BATS_TEST_TMPDIR/pool"
    mkfifo "$BATS_TEST_TMPDIR/pool/fifo"
    run inner "$BATS_TEST_TMPDIR/pool"
    [ "$output" = "MAKE X=1 all MAKEFLAGS=-j --jobserver-auth=3,4" ]
    run inner "$BATS_TEST_TMPDIR/nowhere"
    [ "$output" = "MAKE X=1 all -j8 MAKEFLAGS=" ]
}

# A ninja in the private cache that reports version $1.
cached_ninja() {
    mkdir -p "$HELIX_NINJA_CACHE"
    printf '#!/bin/sh\necho %s\n' "$1" > "$HELIX_NINJA_CACHE/ninja"
    chmod +x "$HELIX_NINJA_CACHE/ninja"
}

@test "pool-docker: idf.py joins the pool through ninja 1.13's jobserver, with no -j" {
    fake_pool
    cached_ninja 1.13.2
    export PATH="$BIN:$PATH"
    run "$POOL_DOCKER" docker run --rm -v /a:/b idf bash -c '. export.sh && idf.py -B b build'
    [ "$status" -eq 0 ]
    [ "$output" = "$(printf '%s\n' run --volume=/pool:/run/jobpool "--volume=$HELIX_NINJA_CACHE/ninja:/usr/bin/ninja:ro" \
        -e "MAKEFLAGS= -j --jobserver-auth=fifo:/run/jobpool/fifo" -e IDF_PY_BUILD_JOBS= \
        --rm -v /a:/b idf bash -c '. export.sh && idf.py -B b build' "ENV JOBPOOL_SLOTS=unset")" ]
    # Registered with the pool as a consumer, and no fixed hold.
    grep -qx 'exec MAKEFLAGS=\[\]' "$FAKE_LOG"
    ! grep -q '^hold' "$FAKE_LOG"
    # A bare ninja command line joins the same way.
    run "$POOL_DOCKER" docker run img ninja -C build
    contains "MAKEFLAGS= -j --jobserver-auth=fifo:/run/jobpool/fifo" "$output"
}

@test "pool-docker: idf.py with a ninja older than 1.13, or a pool that will not mount, holds slots" {
    fake_pool
    cached_ninja 1.11.1
    export PATH="$BIN:$PATH"
    run --separate-stderr "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$status" -eq 0 ]
    [ "$(printf '%s\n' "${lines[@]:0:5}")" = "$(printf '%s\n' run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS=5)" ]
    cached_ninja 1.13.2
    sed -i 's/^    docker-args) .*/    docker-args) exit 1 ;;/' "$FAKE"
    run --separate-stderr "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$(printf '%s\n' "${lines[@]:0:5}")" = "$(printf '%s\n' run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS=5)" ]
}

@test "pool-docker: a command that is not idf.py or ninja holds slots even with ninja 1.13 cached" {
    fake_pool
    cached_ninja 1.13.2
    export PATH="$BIN:$PATH"
    run "$POOL_DOCKER" docker run --rm -v /src/ninja-x:/n img ./build.sh ninja-ish
    [ "$(printf '%s\n' "${lines[@]:0:5}")" = "$(printf '%s\n' run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS=5)" ]
    ! contains "jobserver-auth=fifo" "$output"
}

@test "pool-docker: idf.py with no ninja 1.13 to be had holds slots and gets IDF_PY_BUILD_JOBS" {
    fake_pool
    export PATH="$BIN:$PATH"
    run --separate-stderr "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$status" -eq 0 ]
    contains "cannot fetch" "$stderr"
    [ "$(printf '%s\n' "${lines[@]:0:5}")" = "$(printf '%s\n' run -e JOBPOOL_SLOTS -e IDF_PY_BUILD_JOBS=5)" ]
    contains "ENV JOBPOOL_SLOTS=5" "$output"
    # A third of the fake pool's target of 7: an ESP32 build on a busy pool
    # waits for a floor instead of running on one slot.
    grep -q '^hold --min 2 -- ' "$FAKE_LOG"
}

@test "hold defaults its floor to a third of the pool, capped by -n, unless --min is given" {
    fake_pool
    run "$CLAIM" hold -- true
    [ "$status" -eq 0 ]
    run "$CLAIM" hold -n 1 -- true
    run "$CLAIM" hold --min 5 -- true
    [ "$(cat "$FAKE_LOG" | grep '^hold')" = "$(printf '%s\n' 'hold --min 2 -- true' 'hold -n 1 --min 1 -- true' 'hold --min 5 -- true')" ]
}

@test "pool-docker: without jobpool, with JOBPOOL=0 or when the pool will not start, the command is untouched" {
    local want
    want=$(printf '%s\n' run --rm img make -j8 all "ENV JOBPOOL_SLOTS=unset")
    no_pool
    run "$POOL_DOCKER" docker run --rm img make -j8 all
    [ "$output" = "$want" ]
    fake_pool
    JOBPOOL=0 run "$POOL_DOCKER" docker run --rm img make -j8 all
    [ "$output" = "$want" ]
    JOBPOOL=0 run "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$output" = "$(printf '%s\n' run --rm idf bash -c 'idf.py build' "ENV JOBPOOL_SLOTS=unset")" ]
    no_pool
    run "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$output" = "$(printf '%s\n' run --rm idf bash -c 'idf.py build' "ENV JOBPOOL_SLOTS=unset")" ]
    # No pool, no ninja fetch.
    [ ! -e "$HELIX_NINJA_CACHE" ]
    fake_pool
    sed -i 's/^    docker-args) .*/    docker-args) exit 1 ;;/' "$FAKE"
    run "$POOL_DOCKER" docker run --rm img make -j8 all
    [ "$output" = "$want" ]
}

# ---------------------------------------------------------------------------
# Native cross builds: the sub-make stays in the pool
# ---------------------------------------------------------------------------

cross_submake() {
    (cd "$(scratch_tree)" && env PATH="$(path_without_jobpool)" make -n --no-print-directory MAKE='echo SUBMAKE' "$@" 2>&1)
}

@test "a native cross sub-make inherits a live pool instead of taking -j" {
    fake_pool
    run cross_submake -j2 pi
    contains "SUBMAKE PLATFORM_TARGET=pi  all" "$output"
    no_pool
    run cross_submake -j2 pi
    contains "SUBMAKE PLATFORM_TARGET=pi -j" "$output"
    run cross_submake pi
    contains "SUBMAKE PLATFORM_TARGET=pi -j" "$output"
}

# ---------------------------------------------------------------------------
# helix-claim resources names heavy runners outside the pool
# ---------------------------------------------------------------------------

# A runner reparented away from this suite (which runs under a hold), so only
# its own environment decides; prints its pid. ninja is a binary, so a copy of
# sleep stands in; bats is an env-bash script, whose comm reads "bash".
orphan() {
    local kind=$1 args=(60); shift
    if [ "$kind" = ninja ]; then
        cp "$(command -v sleep)" "$BIN/ninja"
    else
        args=(--jobs 60)
        printf '#!/usr/bin/env bash\nsleep "$2"\n' > "$BIN/bats"
        chmod +x "$BIN/bats"
    fi
    ( env -u JOBPOOL_SLOTS -u MAKEFLAGS "$@" "$BIN/$kind" "${args[@]}" </dev/null >/dev/null 2>&1 3>&- &
      echo $! )
}

@test "resources lists a heavy runner outside the pool, and not one under a hold" {
    fake_pool
    ninja=$(orphan ninja)
    bats=$(orphan bats)
    held=$(orphan ninja JOBPOOL_SLOTS=4)
    run "$CLAIM" resources --no-test-host
    kill "$ninja" "$bats" "$held" 2>/dev/null || true
    [ "$status" -eq 0 ]
    contains "unpooled heavy runners" "$output"
    grep -qE "^  pid $ninja .*ninja 60" <<< "$output"
    grep -qE "^  pid $bats .*bats --jobs 60" <<< "$output"
    ! grep -qE "^  pid $held " <<< "$output"
}

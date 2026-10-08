#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything heavy shares the jobpool's one budget: the bats suite, container
# builds and idf.py size themselves from JOBPOOL_SLOTS (`helix-claim hold`,
# scripts/pool-docker.sh), and a container's make joins the pool. Without
# jobpool each runs exactly as it would with no pool at all.
#
# The pool is always a fake named through HELIX_JOBPOOL; no test reaches the
# real one.

bats_require_minimum_version 1.5.0
load helpers

REPO="$(cd "${BATS_TEST_DIRNAME}/../.." && pwd)"
CLAIM="$REPO/scripts/helix-claim"
POOL_DOCKER="$REPO/scripts/pool-docker.sh"

setup() {
    # The suite itself runs under a hold; nothing here may inherit it.
    unset JOBPOOL JOBPOOL_SLOTS MAKEFLAGS MFLAGS MAKELEVEL
    export HELIX_CLAIM_DIR="$BATS_TEST_TMPDIR/claims"
    FAKE="$BATS_TEST_TMPDIR/fake-jobpool"
    export FAKE_LOG="$BATS_TEST_TMPDIR/fake-jobpool.log"
    BIN="$BATS_TEST_TMPDIR/bin"
    mkdir -p "$BIN"
    # A docker that prints its argv one per line, then the env it was given.
    cat > "$BIN/docker" <<'EOF'
#!/bin/sh
printf '%s\n' "$@"
echo "ENV JOBPOOL_SLOTS=${JOBPOOL_SLOTS-unset}"
EOF
    chmod +x "$BIN/docker"
}

# A jobpool that holds 5 slots, mounts /pool and joins with a recognisable line.
fake_pool() {
    export HELIX_JOBPOOL="$FAKE"
    cat > "$FAKE" <<'EOF'
#!/bin/sh
echo "$*" >> "$FAKE_LOG"
case "$1" in
    target) echo 7 ;;
    hold) shift; while [ "$1" != -- ]; do shift; done; shift; JOBPOOL_SLOTS=${FAKE_SLOTS:-5} exec "$@" ;;
    exec) echo "exec MAKEFLAGS=[${MAKEFLAGS-}]" >> "$FAKE_LOG"; shift 2; exec "$@" ;;
    docker-args) echo "--volume=/pool:$2" ;;
    container-env) echo "exec 3<>$2/fifo 4<>$2/fifo && export MAKEFLAGS=-j\\ --jobserver-auth=3,4" ;;
    *) exit 2 ;;
esac
EOF
    chmod +x "$FAKE"
}

path_without_jobpool() {
    local d p=""
    local IFS=:
    for d in $PATH; do
        [ -e "$d/jobpool" ] && continue
        p=${p:+$p:}$d
    done
    printf '%s' "$p"
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

# make test-shell with bats and GNU parallel replaced by echoes.
test_shell() {
    printf '#!/bin/sh\necho "BATS $*"\n' > "$BIN/bats"
    printf '#!/bin/sh\n' > "$BIN/parallel"
    chmod +x "$BIN/bats" "$BIN/parallel"
    (cd "$REPO" && env PATH="$BIN:$(path_without_jobpool)" make -s --no-print-directory test-shell 2>&1)
}

@test "make test-shell runs bats with the pool's slots" {
    fake_pool
    run test_shell
    [ "$status" -eq 0 ]
    contains "BATS --jobs 5 --no-parallelize-within-files tests/shell/" "$output"
    # A third of the fake pool's target of 7.
    grep -q '^hold --min 2 -- ' "$FAKE_LOG"
}

@test "make test-shell on a single slot runs bats serially" {
    fake_pool
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

@test "pool-docker: any other container command holds slots and gets IDF_PY_BUILD_JOBS" {
    fake_pool
    export PATH="$BIN:$PATH"
    run "$POOL_DOCKER" docker run --rm idf bash -c 'idf.py build'
    [ "$status" -eq 0 ]
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
    sed -i 's/^    docker-args) .*/    docker-args) exit 1 ;;/' "$FAKE"
    run "$POOL_DOCKER" docker run --rm img make -j8 all
    [ "$output" = "$want" ]
}

# ---------------------------------------------------------------------------
# Native cross builds: the sub-make stays in the pool
# ---------------------------------------------------------------------------

cross_submake() {
    (cd "$REPO" && env PATH="$(path_without_jobpool)" make -n --no-print-directory MAKE='echo SUBMAKE' "$@" 2>&1)
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

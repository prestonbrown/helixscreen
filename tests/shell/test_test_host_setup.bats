#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/test-host-setup.sh refuses a layout the container could not use,
# before it reaches the host: the container mounts the parent of the mirror
# root, so the root needs a parent of its own, the same name on both sides,
# and everything else the container keeps must sit under that mount.

SETUP="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)/scripts/test-host-setup.sh"

setup() {
    load helpers
    export HELIX_TEST_HOST=testhost.invalid
    mock_command_script ssh 'touch "$BATS_TEST_TMPDIR/ssh-called"; exit 255'
}

refused() { # <expected text>
    [ "$status" -ne 0 ]
    contains "$1" "$output"
    [ ! -e "$BATS_TEST_TMPDIR/ssh-called" ]
}

@test "a mirror root with no parent of its own is refused: it would mount the whole home" {
    HELIX_TEST_TREES_HOST=trees run "$SETUP"
    refused "needs a parent directory of its own"
}

@test "a mirror root named differently on the host and in the container is refused" {
    HELIX_TEST_TREES_HOST=/srv/mirrors HELIX_TEST_TREES=/work/trees run "$SETUP"
    refused "must end in the same directory name"
}

@test "a ccache or checkout outside the mounted directory is refused" {
    HELIX_TEST_CCACHE=/var/ccache run "$SETUP"
    refused "/var/ccache is not under /work"
    HELIX_TEST_WORKDIR=/src/helixscreen run "$SETUP"
    refused "/src/helixscreen is not under /work"
}

@test "with the default layout it goes on to ask the host" {
    run "$SETUP"
    [ "$status" -ne 0 ]
    [ -e "$BATS_TEST_TMPDIR/ssh-called" ]
    contains "needs Docker and passwordless sudo" "$output"
}

# Runs setup up to the container step, then that step's remote script here,
# against a sudo that records each docker call. $1: "new" (no container yet) or
# "existing" (one already mounting the work directory).
run_container_step() {
    local kind="$1"
    mock_command_script ssh 'case "$*" in
    *"bash -se"*) cat > "$BATS_TEST_TMPDIR/remote.sh"; exit 1 ;;
    *"docker build"*) cat > /dev/null ;;
esac
exit 0'
    cd "$BATS_TEST_TMPDIR"
    run "$SETUP" < /dev/null
    [ -s "$BATS_TEST_TMPDIR/remote.sh" ] || fail "no container script reached the host: $output"
    mock_command_script sudo 'shift; echo "$*" >> "$BATS_TEST_TMPDIR/docker.log"
case "$*" in
    *"inspect -f {{range .Mounts}}"*) echo "$MOCK_MOUNT" ;;
    *"inspect -f {{.HostConfig.Init}}"*) echo true ;;
    "docker inspect "*) [ "$MOCK_KIND" = existing ] ;;
esac'
    mkdir -p helix-test/trees
    MOCK_KIND="$kind" MOCK_MOUNT="$(cd helix-test && pwd -P):/work" run bash "$BATS_TEST_TMPDIR/remote.sh" < /dev/null
}

@test "a new test container gets its memory limit with no swap" {
    HELIX_TEST_MEMORY=96g run_container_step new
    [ "$status" -eq 0 ] || fail "remote script failed: $output"
    grep -E '^docker run .*--memory 96g --memory-swap 96g --pids-limit 16384 ' "$BATS_TEST_TMPDIR/docker.log" ||
        fail "$(cat "$BATS_TEST_TMPDIR/docker.log")"
}

@test "an existing test container takes a changed memory limit without being recreated" {
    HELIX_TEST_MEMORY=64g run_container_step existing
    [ "$status" -eq 0 ] || fail "remote script failed: $output"
    grep -qx 'docker update --memory 64g --memory-swap 64g --pids-limit 16384 helix-test' "$BATS_TEST_TMPDIR/docker.log" ||
        fail "$(cat "$BATS_TEST_TMPDIR/docker.log")"
    ! grep -q '^docker run' "$BATS_TEST_TMPDIR/docker.log" || fail "recreated it"
}

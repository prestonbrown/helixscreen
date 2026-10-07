#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/esp32_build_debug_heap.sh run from a git worktree, with a fake docker that
# records its arguments and writes what an IDF build would leave behind.

load helpers

setup() {
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    MAIN="$BATS_TEST_TMPDIR/main"
    WT="$BATS_TEST_TMPDIR/wt"
    git init -q "$MAIN"
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init
    git -C "$MAIN" worktree add -q "$WT" 2>/dev/null
    mkdir -p "$WT/scripts" "$WT/firmware/helixscreen-esp32/build"
    cp "$REPO_ROOT/scripts/esp32_build_debug_heap.sh" "$WT/scripts/"
    FW="$WT/firmware/helixscreen-esp32"

    export HELIX_CRASH_ELF_DIR="$BATS_TEST_TMPDIR/elves"
    export DOCKER_ARGS_FILE="$BATS_TEST_TMPDIR/docker.args"
    export FAKE_FW="$FW"
    FAKE_DOCKER="$BATS_TEST_TMPDIR/fakedocker"
    cat > "$FAKE_DOCKER" <<'FAKE'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$DOCKER_ARGS_FILE"
b="$FAKE_FW/build-debug-heap"
echo elf > "$b/helixscreen_esp32.elf"
echo "0123456789abcdef  helixscreen_esp32.elf" > "$b/helixscreen_esp32.elf.sha256"
echo "CONFIG_HEAP_POISONING_COMPREHENSIVE=y" > "$b/sdkconfig"
FAKE
    chmod +x "$FAKE_DOCKER"
    export DOCKER="$FAKE_DOCKER"
}

@test "a worktree build mounts the main tree its lib/ symlinks point into" {
    echo image > "$FW/build/storage_frogfs.bin"
    cd "$WT"
    run bash scripts/esp32_build_debug_heap.sh
    [ "$status" -eq 0 ] || fail "$output"
    grep -qx "$WT:$WT" "$DOCKER_ARGS_FILE" || fail "worktree not mounted: $(cat "$DOCKER_ARGS_FILE")"
    grep -qx "$MAIN:$MAIN:ro" "$DOCKER_ARGS_FILE" || fail "main tree not mounted: $(cat "$DOCKER_ARGS_FILE")"
}

@test "the packed asset image is copied into the debug build directory" {
    echo image > "$FW/build/storage_frogfs.bin"
    cd "$WT"
    run bash scripts/esp32_build_debug_heap.sh
    [ "$status" -eq 0 ] || fail "$output"
    [ "$(cat "$FW/build-debug-heap/storage_frogfs.bin")" = image ] || fail "image not staged"
    [ -f "$HELIX_CRASH_ELF_DIR"/012345678-*-debug-heap.elf ] || fail "ELF not archived: $output"
}

@test "a missing asset image fails before docker, naming the commands that make it" {
    cd "$WT"
    run bash scripts/esp32_build_debug_heap.sh
    [ "$status" -ne 0 ] || fail "built without an asset image: $output"
    [[ "$output" == *"esp32_pack_assets.py"* ]] || fail "$output"
    [ ! -f "$DOCKER_ARGS_FILE" ] || fail "docker ran without an asset image"
}

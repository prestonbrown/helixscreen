#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/release-gate.sh in a scratch repo, with a fake make that records its
# arguments and leaves behind what a cross build would, and a pass-through helix-claim.

load helpers

setup() {
    REPO_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"
    T="$BATS_TEST_TMPDIR/repo"
    mkdir -p "$T/scripts"
    cp "$REPO_ROOT/scripts/release-gate.sh" "$REPO_ROOT/scripts/generate-whatsnew.sh" \
        "$REPO_ROOT/scripts/android-version-code.sh" "$REPO_ROOT/scripts/changelog-section.sh" \
        "$REPO_ROOT/scripts/mock_symbol_pattern.txt" \
        "$T/scripts/"
    cat >"$T/scripts/helix-claim" <<'CLAIM'
#!/usr/bin/env bash
while [ "$1" != "--" ]; do shift; done
shift
exec "$@"
CLAIM
    chmod +x "$T/scripts/helix-claim"
    printf '#!/usr/bin/env bash\necho "release-health ran"\n' >"$T/scripts/release-health.sh"
    chmod +x "$T/scripts/release-health.sh"
    git -C "$T" init -q
    git -C "$T" -c user.email=t@t -c user.name=t commit -q --allow-empty -m init
    echo "2.0.0" >"$T/VERSION.txt"
    printf '## [2.0.0] - 2026-10-07\n\n[2.0.0]: https://example/compare/v1.9.0...v2.0.0\n' >"$T/CHANGELOG.md"
    mkdir -p "$T/android/whatsnew"
    echo "Two point oh." >"$T/android/whatsnew/2.0.0.txt"

    export MAKE_ARGS_FILE="$BATS_TEST_TMPDIR/make.args"
    export FAKE_BUILD=ok
    export MAKE="$BATS_TEST_TMPDIR/fakemake"
    cat >"$MAKE" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >>"$MAKE_ARGS_FILE"
target=$2
case "$target" in
    *-docker)
        [ "$FAKE_BUILD" = linkfail ] && { echo "undefined reference to \`__atomic_fetch_add_8'"; exit 2; }
        plat=${target%-docker}; plat=${plat%-all}
        bin=build/$plat/bin; mkdir -p "$bin"
        for b in helix-screen helix-splash helix-watchdog; do echo elf >"$bin/$b"; chmod +x "$bin/$b"; done
        [ "$FAKE_BUILD" = nowatchdog ] && rm "$bin/helix-watchdog"
        echo "00400000 T helix::Application::run()" >"$bin/helix-screen.sym"
        [ "$FAKE_BUILD" = mocks ] && echo "00400100 T MoonrakerClientMock::connect()" >>"$bin/helix-screen.sym"
        ;;
esac
exit 0
FAKE
    chmod +x "$MAKE"
    cd "$T" || return 1
}

@test "a clean packaged build passes every step" {
    run scripts/release-gate.sh
    [ "$status" -eq 0 ] || fail "$output"
    [[ "$output" == *"RELEASE GATE: PASS"* ]] || fail "$output"
    for s in version installer release-health mips:build mips:package mips:binaries mips:no-mocks; do
        echo "$output" | grep -qE "^  PASS  $s " || fail "no PASS row for $s: $output"
    done
    grep -qx -- "--no-print-directory mips-docker HELIX_PACKAGING=1" "$MAKE_ARGS_FILE" || fail "$(cat "$MAKE_ARGS_FILE")"
    grep -qx -- "--no-print-directory release-mips" "$MAKE_ARGS_FILE" || fail "$(cat "$MAKE_ARGS_FILE")"
}

@test "a link failure fails the gate and skips the rest of that target" {
    FAKE_BUILD=linkfail run scripts/release-gate.sh heavy
    [ "$status" -ne 0 ] || fail "passed a failed link: $output"
    echo "$output" | grep -qE "^  FAIL  mips:build " || fail "$output"
    echo "$output" | grep -qE "^  SKIP  mips:package " || fail "$output"
    [[ "$output" == *"__atomic_fetch_add_8"* ]] || fail "failure output not shown: $output"
    ! grep -q release-mips "$MAKE_ARGS_FILE" || fail "packaged a failed build"
}

@test "mock symbols in a packaged binary fail the gate" {
    FAKE_BUILD=mocks run scripts/release-gate.sh heavy
    [ "$status" -ne 0 ] || fail "$output"
    echo "$output" | grep -qE "^  FAIL  mips:no-mocks " || fail "$output"
}

@test "a splash without its watchdog fails the gate" {
    FAKE_BUILD=nowatchdog run scripts/release-gate.sh heavy
    [ "$status" -ne 0 ] || fail "$output"
    echo "$output" | grep -qE "^  FAIL  mips:binaries " || fail "$output"
}

@test "each target builds through the docker target Release builds it with" {
    RELEASE_GATE_TARGETS="pi k2" run scripts/release-gate.sh heavy
    [ "$status" -eq 0 ] || fail "$output"
    grep -qx -- "--no-print-directory pi-all-docker HELIX_PACKAGING=1" "$MAKE_ARGS_FILE" || fail "$(cat "$MAKE_ARGS_FILE")"
    grep -qx -- "--no-print-directory k2-docker HELIX_PACKAGING=1" "$MAKE_ARGS_FILE" || fail "$(cat "$MAKE_ARGS_FILE")"
}

@test "a version with no changelog entry fails" {
    echo "2.0.1" >VERSION.txt
    run scripts/release-gate.sh fast
    [ "$status" -ne 0 ] || fail "$output"
    [[ "$output" == *"no '## [2.0.1] - <date>' entry"* ]] || fail "$output"
}

@test "a stable version with no Play Store text fails before the tag" {
    rm android/whatsnew/2.0.0.txt
    run scripts/release-gate.sh fast
    [ "$status" -ne 0 ] || fail "$output"
    echo "$output" | grep -qE "^  FAIL  whatsnew " || fail "$output"
    grep -q "android/whatsnew/2.0.0.txt is missing" build/release-gate/whatsnew.log || fail "$(cat build/release-gate/whatsnew.log)"
}

@test "a version that is already tagged fails" {
    git tag v2.0.0
    run scripts/release-gate.sh fast
    [ "$status" -ne 0 ] || fail "$output"
    [[ "$output" == *"tag v2.0.0 already exists"* ]] || fail "$output"
}

@test "an unknown tier is refused" {
    run scripts/release-gate.sh medium
    [ "$status" -eq 2 ] || fail "$output"
}

@test "the fast tier runs release-health and keeps its output in the step log" {
    run scripts/release-gate.sh fast
    [ "$status" -eq 0 ] || fail "$output"
    echo "$output" | grep -qE "^  PASS  release-health " || fail "$output"
    grep -q "release-health ran" build/release-gate/release-health.log || fail "no step log"
}

@test "RELEASE_HEALTH=0 skips release-health" {
    RELEASE_HEALTH=0 run scripts/release-gate.sh fast
    [ "$status" -eq 0 ] || fail "$output"
    echo "$output" | grep -qE "^  SKIP  release-health " || fail "$output"
    [ ! -e build/release-gate/release-health.log ] || fail "ran anyway"
}

@test "release-health warnings show on its row, which still passes" {
    printf '#!/usr/bin/env bash\necho "WARN a"\necho "WARN b"\n' >scripts/release-health.sh
    run scripts/release-gate.sh fast
    [ "$status" -eq 0 ] || fail "$output"
    echo "$output" | grep -qE "^  PASS  release-health \(2 warnings\) " || fail "$output"
}

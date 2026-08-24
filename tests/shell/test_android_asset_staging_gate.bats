#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Meta-tests for scripts/check_android_asset_staging.py — the gate asserting
# that android/app/src/main/assets/ stays a Gradle-owned build output.
#
# The copyAssets task in android/app/build.gradle deletes that tree and re-copies
# it from ui_xml/, assets/ and config/ on every build, so the APK is never stale.
# What rots is the copy left on disk between builds: the tree is ignored wholesale
# by android/.gitignore, so an old snapshot is indistinguishable from source to
# anything walking the repo. One went 4 months and 248-of-287 XML files stale, and
# four separate lint gates each grew a hand-written exclusion for it before anyone
# worked out the files were not real. mk/filaments.mk meanwhile cp'd one file into
# it after every regen, refreshing 1 of 592 and making a dead tree look alive.
#
# These tests pin both halves of the contract. The catch half: a tracked file
# under the staged tree, and a Makefile/mk/script rule writing into it. The quiet
# half matters just as much — the four lint gates that legitimately NAME this path
# in order to exclude it must not trip the gate, or it becomes noise on every
# commit and gets switched off. That distinction (naming the path vs. writing to
# it) is the whole design, so it is what these tests guard.

GATE="scripts/check_android_asset_staging.py"
STAGED="android/app/src/main/assets"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    REPO_ROOT="$PWD"
    WORK="${BATS_TEST_TMPDIR:-$(mktemp -d)}/repo"
}

# Build a throwaway repo carrying just enough shape for the gate to run: a git
# checkout (so `git ls-files` works), the source trees it compares mtimes
# against, and a copy of the gate itself.
make_fixture() {
    rm -rf "$WORK"
    mkdir -p "$WORK/scripts" "$WORK/mk" "$WORK/ui_xml" "$WORK/assets" "$WORK/config"
    cp "$REPO_ROOT/$GATE" "$WORK/scripts/"
    echo "x" > "$WORK/ui_xml/a.xml"
    echo "x" > "$WORK/assets/a.json"
    echo "x" > "$WORK/config/a.json"
    git -C "$WORK" init -q
    git -C "$WORK" config user.email t@t
    git -C "$WORK" config user.name t
    git -C "$WORK" add -A
    git -C "$WORK" commit -qm init
}

run_gate() {
    run python3 "$WORK/scripts/check_android_asset_staging.py" --repo "$WORK"
}

# --- The quiet half: legitimate shapes must not fire -----------------------

@test "passes on the real repo as it stands" {
    run python3 "$GATE"
    [ "$status" -eq 0 ]
}

@test "a staged tree that merely exists and differs is not a failure" {
    make_fixture
    mkdir -p "$WORK/$STAGED/ui_xml"
    echo "stale" > "$WORK/$STAGED/ui_xml/a.xml"
    run_gate
    [ "$status" -eq 0 ]
}

@test "lint scripts that NAME the staged path to exclude it stay quiet" {
    # This is the shape of check_overlay_width.py et al. Flagging it would make
    # the gate fire on four existing scripts and get it switched off.
    make_fixture
    cat > "$WORK/scripts/check_thing.py" <<'EOF'
# android/app/src/main/assets/ui_xml/ is a gradle copy of ui_xml/, not a source
EXCLUDE = ["android/app/src/main/assets"]
if p.startswith("android/app/src/main/assets"):
    continue
EOF
    run_gate
    [ "$status" -eq 0 ]
}

@test "reading FROM the staged tree is allowed" {
    make_fixture
    printf 'stage:\n\tcat android/app/src/main/assets/MANIFEST.txt\n' > "$WORK/mk/x.mk"
    run_gate
    [ "$status" -eq 0 ]
}

@test "an annotated write is allowed via the opt-out" {
    make_fixture
    printf 'stage:\n\tcp a.json android/app/src/main/assets/a.json # ANDROID_STAGING_OK: test\n' \
        > "$WORK/mk/x.mk"
    run_gate
    [ "$status" -eq 0 ]
}

# --- The catch half: the two real regressions -----------------------------

@test "catches a tracked file committed under the staged tree" {
    make_fixture
    mkdir -p "$WORK/$STAGED/ui_xml"
    echo "oops" > "$WORK/$STAGED/ui_xml/nozzle_icon.xml"
    git -C "$WORK" add -f "$STAGED/ui_xml/nozzle_icon.xml"
    git -C "$WORK" commit -qm staged
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"tracked file"* ]]
}

@test "catches a make rule cp'ing into the staged tree (the mk/filaments.mk shape)" {
    make_fixture
    printf 'regen:\n\t@mkdir -p android/app/src/main/assets/assets\n\t@cp -a assets/filaments.json android/app/src/main/assets/assets/filaments.json\n' \
        > "$WORK/mk/filaments.mk"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"write into"* ]]
    [[ "$output" == *"filaments.mk"* ]]
}

@test "catches a shell redirect into the staged tree" {
    make_fixture
    printf '#!/bin/sh\necho hi > android/app/src/main/assets/BUILD_STAMP\n' > "$WORK/scripts/x.sh"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"write into"* ]]
}

# --- Wiring: the gate must actually be reachable ---------------------------

@test "gate is wired into quality-checks.sh" {
    run grep -q "check_android_asset_staging.py" scripts/quality-checks.sh
    [ "$status" -eq 0 ]
}

@test "mk/filaments.mk no longer writes into the staged tree" {
    # The regression that motivated the gate. Pinned directly so a revert is
    # caught even if the gate itself is later weakened.
    run grep -q "android/app/src/main/assets" mk/filaments.mk
    [ "$status" -eq 1 ]
}

@test "the staged tree is still ignored wholesale" {
    # The gate's tracked-file rule assumes this. If the ignore rule is dropped,
    # every build output becomes an untracked-file storm instead.
    run git check-ignore -q "$STAGED/ui_xml/nozzle_icon.xml"
    [ "$status" -eq 0 ]
}

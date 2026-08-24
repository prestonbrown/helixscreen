#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/android-version-code.sh — the single definition of the
# Android versionCode packing.
#
# The packing used to be written out three times (android/app/build.gradle,
# scripts/generate-whatsnew.sh, .github/workflows/release.yml) and the three
# copies diverged the moment the lanes were widened from 100 to 1000: two were
# updated, release.yml kept `$1*10000 + $2*100 + $3`. The visible symptom was
# not a wrong versionCode — the APK was fine, because Gradle owned that number.
# It was a filename miss. generate-whatsnew.sh wrote changelogs/99114.txt while
# the workflow looked for changelogs/10014.txt, the `if [ -f "$SRC" ]` fell
# through to a `::warning::`, `if-no-files-found: ignore` skipped the upload,
# and the release-android-whatsnew artifact silently did not exist. Nothing
# went red because publish-android is inert without PLAY_SERVICE_ACCOUNT_JSON;
# the day that secret lands, its download-artifact step hard-fails.
#
# So the divergence gate at the bottom is the load-bearing test here. The value
# tests below only prove one copy is right.

SCRIPT="scripts/android-version-code.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TEST_DIR="$(mktemp -d)"
}

teardown() {
    rm -rf "$TEST_DIR"
}

# ---------------------------------------------------------------------------
# Script hygiene
# ---------------------------------------------------------------------------

@test "android-version-code.sh exists and is executable" {
    [ -f "$SCRIPT" ]
    [ -x "$SCRIPT" ]
}

@test "android-version-code.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "android-version-code.sh passes shellcheck" {
    # scripts/ is gated at -S warning minus SC3043/SC1091 (see
    # tests/shell/test_shellcheck_gate.bats). A new script must be clean at the
    # default severity too — it has no business being added to the baseline.
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "android-version-code.sh carries an SPDX header" {
    run head -3 "$SCRIPT"
    [[ "$output" == *"SPDX-License-Identifier: GPL-3.0-or-later"* ]]
}

# ---------------------------------------------------------------------------
# Known values
# ---------------------------------------------------------------------------

@test "0.99.114 packs to 99114" {
    run "$SCRIPT" 0.99.114
    [ "$status" -eq 0 ]
    [ "$output" = "99114" ]
}

@test "0.99.113 packs to 99113" {
    run "$SCRIPT" 0.99.113
    [ "$status" -eq 0 ]
    [ "$output" = "99113" ]
}

@test "1.0.0 packs to 1000000" {
    run "$SCRIPT" 1.0.0
    [ "$status" -eq 0 ]
    [ "$output" = "1000000" ]
}

@test "1.2.3 packs to 1002003" {
    # Each field lands in its own lane with no carry between them.
    run "$SCRIPT" 1.2.3
    [ "$status" -eq 0 ]
    [ "$output" = "1002003" ]
}

@test "the script prints the versionCode and nothing else" {
    # Gradle and the workflow both consume stdout directly. A stray progress
    # line would be parsed as part of the number.
    run "$SCRIPT" 1.2.3
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [[ "$output" =~ ^[0-9]+$ ]]
}

# ---------------------------------------------------------------------------
# The ordering invariant the widening exists to hold
# ---------------------------------------------------------------------------

@test "versionCode(0.99.113) < versionCode(1.0.0)" {
    # This is why the lanes are 1000 wide. Under the old 100-wide minor lane,
    # 0.99.113 packed to 10013 and 1.0.0 packed to 10000 — so the 1.0 release
    # would have been refused as a downgrade by the Play Store and by every
    # sideloaded Android install.
    local old new
    old="$("$SCRIPT" 0.99.113)"
    new="$("$SCRIPT" 1.0.0)"
    [ "$old" -lt "$new" ]
}

@test "the whole 0.99.x line stays below 1.0.0" {
    local last new
    last="$("$SCRIPT" 0.99.999)"
    new="$("$SCRIPT" 1.0.0)"
    [ "$last" -lt "$new" ]
}

@test "a patch bump strictly increases the versionCode" {
    local a b
    a="$("$SCRIPT" 0.99.113)"
    b="$("$SCRIPT" 0.99.114)"
    [ "$a" -lt "$b" ]
}

# ---------------------------------------------------------------------------
# Suffix stripping
# ---------------------------------------------------------------------------

@test "a prerelease suffix is stripped from the patch field" {
    run "$SCRIPT" 1.0.0-beta
    [ "$status" -eq 0 ]
    [ "$output" = "1000000" ]
}

@test "a dotted prerelease suffix is stripped too" {
    run "$SCRIPT" 1.2.3-rc.1
    [ "$status" -eq 0 ]
    [ "$output" = "1002003" ]
}

# ---------------------------------------------------------------------------
# Overflow rejection
# ---------------------------------------------------------------------------

@test "a minor field of 1000 is rejected, not silently carried" {
    # 0.1000.0 would pack to 1000000 — indistinguishable from 1.0.0.
    run "$SCRIPT" 0.1000.0
    [ "$status" -ne 0 ]
    [[ "$output" == *"overflow"* ]]
}

@test "a patch field of 1000 is rejected" {
    run "$SCRIPT" 0.1.1000
    [ "$status" -ne 0 ]
    [[ "$output" == *"overflow"* ]]
}

@test "999 is still accepted in both lanes" {
    # The boundary must be >= 1000, not > 999 off by one.
    run "$SCRIPT" 0.999.999
    [ "$status" -eq 0 ]
    [ "$output" = "999999" ]
}

@test "the overflow message names the script to widen" {
    run "$SCRIPT" 0.1000.0
    [ "$status" -ne 0 ]
    [[ "$output" == *"android-version-code.sh"* ]]
}

# ---------------------------------------------------------------------------
# Malformed input
# ---------------------------------------------------------------------------

@test "a two-field version is rejected" {
    run "$SCRIPT" 1.2
    [ "$status" -ne 0 ]
    [[ "$output" == *"1.2"* ]]
}

@test "a non-numeric version is rejected" {
    run "$SCRIPT" abc
    [ "$status" -ne 0 ]
}

@test "an empty version string is rejected" {
    run "$SCRIPT" ""
    [ "$status" -ne 0 ]
}

@test "a four-field version is rejected" {
    run "$SCRIPT" 1.2.3.4
    [ "$status" -ne 0 ]
}

@test "a non-numeric patch field is rejected" {
    run "$SCRIPT" 1.2.x
    [ "$status" -ne 0 ]
}

# ---------------------------------------------------------------------------
# VERSION.txt default
# ---------------------------------------------------------------------------

@test "with no argument it reads the repo's VERSION.txt" {
    local expected
    expected="$("$SCRIPT" "$(tr -d '[:space:]' < VERSION.txt)")"
    run "$SCRIPT"
    [ "$status" -eq 0 ]
    [ "$output" = "$expected" ]
}

@test "the repo root is resolved from the script, not the caller's cwd" {
    # Gradle invokes this with the working directory set to android/, and CI
    # runs it from the repo root. A $PWD-relative VERSION.txt would work in one
    # and not the other.
    local root from_root from_elsewhere
    root="$PWD"
    from_root="$("$SCRIPT")"
    from_elsewhere="$(cd "$TEST_DIR" && "$root/$SCRIPT")"
    [ "$from_root" = "$from_elsewhere" ]
}

@test "the repo's current VERSION.txt packs without error" {
    # A version that overflows the lanes must be caught here, not at release
    # time after a 2h build.
    run "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" =~ ^[0-9]+$ ]]
}

# ---------------------------------------------------------------------------
# Divergence gate — the packing must exist in exactly one place
# ---------------------------------------------------------------------------
#
# Detection is deliberately narrow: a *multiplication* by one of the lane
# constants. Comments explaining the packing (which several of these files
# carry, and should) are stripped first, so prose about "1000-wide lanes" or
# "0.99.113 -> 99113" is fine. A live computation is not.

CONSUMERS="android/app/build.gradle
scripts/generate-whatsnew.sh
.github/workflows/release.yml"

# Strip comments, then look for `* <lane constant>`.
packing_hits() {
    local f="$1"
    case "$f" in
        *.gradle) sed 's,//.*,,' "$f" ;;
        *)        sed 's/#.*//' "$f" ;;
    esac | grep -nE '\*[[:space:]]*(1000000|10000|1000|100)([^0-9]|$)' || true
}

@test "no consumer re-derives the versionCode packing" {
    local offenders=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        [ -f "$f" ] || { echo "consumer file missing: $f" >&2; return 1; }
        local hits
        hits="$(packing_hits "$f")"
        if [ -n "$hits" ]; then
            offenders="$offenders
$f:
$hits"
        fi
    done <<< "$CONSUMERS"

    if [ -n "$offenders" ]; then
        echo "these files compute the versionCode themselves instead of calling" >&2
        echo "$SCRIPT — the packing must have exactly one definition:$offenders" >&2
        return 1
    fi
}

@test "the divergence gate actually catches a reintroduced formula" {
    # Mutation check. Without this the gate could be silently inert (a broken
    # regex, a sed that eats the whole file) and every run would pass.
    local mutant="$TEST_DIR/build.gradle"
    printf 'def vCode = vMajor * 1000000 + vMinor * 1000 + vPatch\n' > "$mutant"
    [ -n "$(packing_hits "$mutant")" ]

    local mutant_sh="$TEST_DIR/mutant.sh"
    printf 'code=$(( major * 10000 + minor * 100 + patch ))\n' > "$mutant_sh"
    [ -n "$(packing_hits "$mutant_sh")" ]
}

@test "the divergence gate does not fire on an explanatory comment" {
    local commented="$TEST_DIR/commented.sh"
    cat > "$commented" <<'EOF'
# versionCode packs as major * 1000000 + minor * 1000 + patch.
# The old packing was major * 10000 + minor * 100 + patch.
version_code=$(scripts/android-version-code.sh)
EOF
    [ -z "$(packing_hits "$commented")" ]

    local commented_gradle="$TEST_DIR/commented.gradle"
    cat > "$commented_gradle" <<'EOF'
// versionCode packs as vMajor * 1000000 + vMinor * 1000 + vPatch.
def vCode = versionCodeFromScript()
EOF
    [ -z "$(packing_hits "$commented_gradle")" ]
}

@test "every consumer calls the shared script" {
    # The other half of the gate: deleting the formula is not enough if the
    # file then hardcodes a number or drops the versionCode entirely.
    local missing=""
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        grep -q 'android-version-code\.sh' "$f" || missing="$missing $f"
    done <<< "$CONSUMERS"

    if [ -n "$missing" ]; then
        echo "these files no longer call $SCRIPT:$missing" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# The consumers agree with the script
# ---------------------------------------------------------------------------

@test "generate-whatsnew.sh writes the filename the workflow looks for" {
    # The exact mismatch that motivated this file. generate-whatsnew.sh names
    # its output <versionCode>.txt; release.yml then reads that path back.
    local code out
    code="$("$SCRIPT")"
    out="$TEST_DIR/whatsnew.txt"
    run bash scripts/generate-whatsnew.sh "$out"
    [ "$status" -eq 0 ]
    # The reported versionCode in its summary line must be the shared one.
    [[ "$output" == *"versionCode $code"* ]]
}

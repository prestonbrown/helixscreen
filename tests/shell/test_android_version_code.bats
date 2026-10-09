#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/android-version-code.sh — the single definition of the
# Android versionCode packing.
#
# Two things are being held here, and the second is the load-bearing one.
#
# The value tests pin the packing itself: three fixed-width lanes for
# major/minor/patch and a two-digit ordinal for the prerelease suffix, ordered
# so that every prerelease sorts above the last and below the release it leads
# to. Android will not install a lower versionCode than the one on the device,
# and the Play Store will not accept one it has already seen, so a collision
# between two versions is an unpublishable build.
#
# The divergence gate at the bottom pins the packing to ONE definition. A
# second copy in a consumer fails quietly rather than loudly: Gradle owns the
# number that reaches the APK, so the APK stays right while the changelog
# filename generate-whatsnew.sh writes and the one release.yml reads back stop
# matching. The workflow's `if [ -f "$SRC" ]` then falls through and the Play
# whatsnew artifact is never produced, with nothing red anywhere.

SCRIPT="scripts/android-version-code.sh"

# Prereleases in tests/fixtures/version_precedence.txt that this packing
# deliberately refuses. A bare-numeric identifier (`-1`) and a
# dotted-alphanumeric one (`-alpha.beta`) are both valid semver with a defined
# precedence, but neither has a row in the alpha/beta/rc ordinal table, and an
# invented ordinal is how two versions end up sharing a versionCode.
UNENCODABLE_FIXTURE_LINES="1.1.0-1
1.1.0-11
1.1.0-alpha.beta"

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

@test "0.99.114 packs to 9911499" {
    run "$SCRIPT" 0.99.114
    [ "$status" -eq 0 ]
    [ "$output" = "9911499" ]
}

@test "0.99.113 packs to 9911399" {
    run "$SCRIPT" 0.99.113
    [ "$status" -eq 0 ]
    [ "$output" = "9911399" ]
}

@test "1.0.0 packs to 100000099" {
    run "$SCRIPT" 1.0.0
    [ "$status" -eq 0 ]
    [ "$output" = "100000099" ]
}

@test "1.2.3 packs to 100200399" {
    # Each field lands in its own lane with no carry between them, and a
    # release takes the top ordinal.
    run "$SCRIPT" 1.2.3
    [ "$status" -eq 0 ]
    [ "$output" = "100200399" ]
}

@test "the script prints the versionCode and nothing else" {
    # Gradle and the workflow both consume stdout directly. A stray progress
    # line would be parsed as part of the number.
    run "$SCRIPT" 1.2.3
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [[ "$output" =~ ^[0-9]+$ ]]
}

@test "a prerelease prints the versionCode and nothing else too" {
    run "$SCRIPT" 1.1.0-rc.2
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [[ "$output" =~ ^[0-9]+$ ]]
}

# ---------------------------------------------------------------------------
# The ordinal table, row by row
# ---------------------------------------------------------------------------
#
# 1.1.0 packs its triple to 1001000, so every ordinal below reads off the last
# two digits of 100100NN.

@test "-alpha.N takes ordinal N" {
    local one twentynine
    one="$("$SCRIPT" 1.1.0-alpha.1)"
    twentynine="$("$SCRIPT" 1.1.0-alpha.29)"
    [ "$one" = "100100001" ]
    [ "$twentynine" = "100100029" ]
}

@test "-beta.N takes ordinal 30 + N" {
    local one twentynine
    one="$("$SCRIPT" 1.1.0-beta.1)"
    twentynine="$("$SCRIPT" 1.1.0-beta.29)"
    [ "$one" = "100100031" ]
    [ "$twentynine" = "100100059" ]
}

@test "-rc.N takes ordinal 60 + N" {
    local one twentynine
    one="$("$SCRIPT" 1.1.0-rc.1)"
    twentynine="$("$SCRIPT" 1.1.0-rc.29)"
    [ "$one" = "100100061" ]
    [ "$twentynine" = "100100089" ]
}

@test "no suffix takes ordinal 99" {
    run "$SCRIPT" 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "100100099" ]
}

@test "a bare -alpha / -beta / -rc takes N=0" {
    # Semver precedence rule 4: a prerelease with fewer identifiers sorts below
    # one that shares its prefix, so -alpha is below -alpha.1.
    local alpha beta rc
    alpha="$("$SCRIPT" 1.1.0-alpha)"
    beta="$("$SCRIPT" 1.1.0-beta)"
    rc="$("$SCRIPT" 1.1.0-rc)"
    [ "$alpha" = "100100000" ]
    [ "$beta" = "100100030" ]
    [ "$rc" = "100100060" ]
}

@test "a bare stage sorts below its own .1" {
    local bare dotted
    bare="$("$SCRIPT" 1.1.0-beta)"
    dotted="$("$SCRIPT" 1.1.0-beta.1)"
    [ "$bare" -lt "$dotted" ]
}

@test "the ordinal is the only thing a prerelease changes" {
    # The triple's contribution is identical; a prerelease is the release's
    # code with a lower ordinal, never a different lane.
    local pre rel
    pre="$("$SCRIPT" 1.2.3-alpha.1)"
    rel="$("$SCRIPT" 1.2.3)"
    [ "$((pre / 100))" -eq "$((rel / 100))" ]
}

# ---------------------------------------------------------------------------
# Prerelease ordering at one fixed triple
# ---------------------------------------------------------------------------

@test "alpha.N < beta.N < rc.N < release at the same triple" {
    local a b r rel
    a="$("$SCRIPT" 1.1.0-alpha.5)"
    b="$("$SCRIPT" 1.1.0-beta.5)"
    r="$("$SCRIPT" 1.1.0-rc.5)"
    rel="$("$SCRIPT" 1.1.0)"
    [ "$a" -lt "$b" ]
    [ "$b" -lt "$r" ]
    [ "$r" -lt "$rel" ]
}

@test "the highest prerelease of a triple stays below its release" {
    # The boundary that makes a release always installable over its own last
    # release candidate: 60 + 29 = 89, and a release is 99.
    local top rel
    top="$("$SCRIPT" 1.1.0-rc.29)"
    rel="$("$SCRIPT" 1.1.0)"
    [ "$top" -lt "$rel" ]
}

@test "a release stays below the next patch's earliest prerelease" {
    local rel next
    rel="$("$SCRIPT" 1.1.0)"
    next="$("$SCRIPT" 1.1.1-alpha)"
    [ "$rel" -lt "$next" ]
}

# ---------------------------------------------------------------------------
# The ordering invariant the lanes exist to hold
# ---------------------------------------------------------------------------

@test "versionCode(0.99.113) < versionCode(1.0.0)" {
    # This is why the minor and patch lanes are 1000 wide: a 100-wide minor
    # lane packs 0.99.113 above 1.0.0, and the 1.0 release is then refused as a
    # downgrade by the Play Store and by every sideloaded install.
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
# Every version in the precedence fixture, in file order
# ---------------------------------------------------------------------------
#
# tests/fixtures/version_precedence.txt is the shared ladder the two semver
# comparators assert against (tests/unit/test_version.cpp and
# tests/shell/test_version_compare.bats). The packing has to agree with it
# wherever it encodes a version at all, or the updater and the Play Store
# disagree about which build is newer.

fixture_versions() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' tests/fixtures/version_precedence.txt |
        grep -v '^$'
}

is_unencodable() {
    local candidate="$1" line
    while IFS= read -r line; do
        [ "$line" = "$candidate" ] && return 0
    done <<< "$UNENCODABLE_FIXTURE_LINES"
    return 1
}

@test "the precedence fixture packs to strictly increasing codes in file order" {
    local prev=0 v code checked=0
    while IFS= read -r v; do
        if is_unencodable "$v"; then
            continue
        fi
        code="$("$SCRIPT" "$v")"
        if [ "$code" -le "$prev" ]; then
            echo "$v packs to $code, not above the previous line's $prev" >&2
            return 1
        fi
        prev="$code"
        checked=$((checked + 1))
    done <<< "$(fixture_versions)"

    # A fixture that stopped being read, or a sed that ate every line, would
    # otherwise pass having compared nothing.
    [ "$checked" -ge 15 ]
}

@test "the fixture's unencodable prereleases are rejected, not guessed" {
    local v
    while IFS= read -r v; do
        run "$SCRIPT" "$v"
        if [ "$status" -eq 0 ]; then
            echo "$v packed to $output; it has no ordinal and must be refused" >&2
            return 1
        fi
    done <<< "$UNENCODABLE_FIXTURE_LINES"

    # And each one really is a line of the fixture, so this list cannot drift
    # into naming versions nobody uses.
    local present
    present="$(fixture_versions | grep -c '^1\.1\.0-\(1\|11\|alpha\.beta\)$')"
    [ "$present" -eq 3 ]
}

# ---------------------------------------------------------------------------
# Unrecognised and out-of-range suffixes
# ---------------------------------------------------------------------------

@test "an unrecognised suffix is a hard error, not a guessed ordinal" {
    run "$SCRIPT" 1.1.0-gamma.1
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "no versionCode"
}

@test "a bare numeric prerelease is rejected" {
    # Valid semver, and its precedence is defined, but the ordinal table has no
    # row for it.
    run "$SCRIPT" 1.1.0-1
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "no versionCode"
}

@test "a stage with a non-numeric second identifier is rejected" {
    run "$SCRIPT" 1.1.0-alpha.beta
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "no versionCode"
}

@test "a stage with no separating dot is rejected" {
    run "$SCRIPT" 1.1.0-rc1
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "no versionCode"
}

@test "the unrecognised-suffix error names the suffixes that do work" {
    run "$SCRIPT" 1.1.0-snapshot
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q -- "-alpha"
    printf '%s\n' "$output" | grep -q -- "-beta"
    printf '%s\n' "$output" | grep -q -- "-rc"
}

@test "an error goes to stderr and leaves stdout empty" {
    # Gradle reads stdout into an integer. A diagnostic on the wrong stream is
    # a NumberFormatException instead of the reason for the failure.
    local out
    out="$("$SCRIPT" 1.1.0-gamma.1 2>/dev/null || true)"
    [ -z "$out" ]
    local err
    err="$("$SCRIPT" 1.1.0-gamma.1 2>&1 >/dev/null || true)"
    printf '%s\n' "$err" | grep -q "error:"
}

@test "a prerelease number of 0 is rejected" {
    # .0 would collide with the bare stage, which already holds ordinal 0.
    run "$SCRIPT" 1.1.0-alpha.0
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "out of range"
}

@test "a prerelease number above 29 is rejected in every stage" {
    local v
    for v in 1.1.0-alpha.30 1.1.0-beta.30 1.1.0-rc.30; do
        run "$SCRIPT" "$v"
        if [ "$status" -eq 0 ]; then
            echo "$v packed to $output; 30 is past the end of its band" >&2
            return 1
        fi
    done
    printf '%s\n' "$output" | grep -q "out of range"
}

@test "29 is still accepted in every stage" {
    # The band boundary must be > 29, not > 28 off by one.
    local v
    for v in 1.1.0-alpha.29 1.1.0-beta.29 1.1.0-rc.29; do
        run "$SCRIPT" "$v"
        if [ "$status" -ne 0 ]; then
            echo "$v was rejected: $output" >&2
            return 1
        fi
    done
}

@test "a three-digit prerelease number is rejected" {
    run "$SCRIPT" 1.1.0-rc.100
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "out of range"
}

@test "a leading zero in the prerelease number is rejected" {
    # .01 is not a valid semver numeric identifier, and reading it as 1 would
    # hand two distinct version strings the same versionCode.
    run "$SCRIPT" 1.1.0-alpha.01
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "leading zero"
}

# ---------------------------------------------------------------------------
# Overflow rejection
# ---------------------------------------------------------------------------

@test "a minor field of 1000 is rejected, not silently carried" {
    # 0.1000.0 would pack its triple to 1000000 — indistinguishable from 1.0.0.
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
    [ "$output" = "99999999" ]
}

@test "the overflow message names the script to widen" {
    run "$SCRIPT" 0.1000.0
    [ "$status" -ne 0 ]
    [[ "$output" == *"android-version-code.sh"* ]]
}

@test "a major field of 21 is rejected" {
    # The ordinal lane is 100 wide, so the major lane runs out at 20 rather
    # than at Android's 2100000000 divided by 1000000.
    run "$SCRIPT" 21.0.0
    [ "$status" -ne 0 ]
    printf '%s\n' "$output" | grep -q "2100000000"
}

@test "the largest packable version stays inside Android's cap" {
    local code
    code="$("$SCRIPT" 20.999.999)"
    [ "$code" -le 2100000000 ]
    [ "$code" = "2099999999" ]
}

@test "a major field of 20 is accepted" {
    run "$SCRIPT" 20.0.0
    [ "$status" -eq 0 ]
    [ "$output" = "2000000099" ]
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
    # A version that overflows the lanes, or carries a suffix with no ordinal,
    # must be caught here and not at release time after a 2h build.
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
# "0.99.113 -> 9911399" is fine. A live computation is not.

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
    printf 'def vCode = (vMajor * 1000000 + vMinor * 1000 + vPatch) * 100 + ordinal\n' > "$mutant"
    [ -n "$(packing_hits "$mutant")" ]

    local mutant_sh="$TEST_DIR/mutant.sh"
    printf 'code=$(( major * 10000 + minor * 100 + patch ))\n' > "$mutant_sh"
    [ -n "$(packing_hits "$mutant_sh")" ]

    # The ordinal multiplier on its own is enough to catch.
    local mutant_ordinal="$TEST_DIR/ordinal.sh"
    printf 'code=$(( base * 100 + ordinal ))\n' > "$mutant_ordinal"
    [ -n "$(packing_hits "$mutant_ordinal")" ]
}

@test "the divergence gate does not fire on an explanatory comment" {
    local commented="$TEST_DIR/commented.sh"
    cat > "$commented" <<'EOF'
# versionCode packs as (major * 1000000 + minor * 1000 + patch) * 100 + ordinal.
version_code=$(scripts/android-version-code.sh)
EOF
    [ -z "$(packing_hits "$commented")" ]

    local commented_gradle="$TEST_DIR/commented.gradle"
    cat > "$commented_gradle" <<'EOF'
// versionCode packs as (vMajor * 1000000 + vMinor * 1000 + vPatch) * 100 + ordinal.
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
    # generate-whatsnew.sh names its output <versionCode>.txt and release.yml
    # reads that path back, so the two must resolve the same number.
    local code out
    code="$("$SCRIPT")"
    out="$TEST_DIR/whatsnew.txt"
    run bash scripts/generate-whatsnew.sh "$out"
    [ "$status" -eq 0 ]
    # The reported versionCode in its summary line must be the shared one.
    [[ "$output" == *"versionCode $code"* ]]
}

#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/version-compare.sh — the semver precedence CI consults
# before it moves a channel manifest forward.
#
# The ordering cases are not written here. They live in
# tests/fixtures/version_precedence.txt, in ascending precedence order, and this
# file asserts the whole i<j matrix over that corpus; tests/unit/test_version.cpp
# asserts the same corpus for the in-app comparator. A case added to the corpus
# therefore fails whichever of the two disagrees, which is the only thing keeping
# one rule written in two languages from drifting.
#
# What is written here is everything a list of versions cannot express: equality
# with itself, build metadata being ignored, and a malformed version producing an
# error rather than a verdict. That last one is the trap the exit contract exists
# for — a caller reads 0 as "the same release, safe to republish".

load helpers

SCRIPT="scripts/version-compare.sh"
CORPUS="tests/fixtures/version_precedence.txt"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
}

# The corpus, comments and blank lines stripped, in ascending precedence order.
corpus() {
    sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$CORPUS" \
        | grep -v '^$'
}

# ------------------------------------------------------------------ mechanics

@test "version-compare.sh passes shellcheck" {
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck "$SCRIPT"
}

@test "version-compare.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "version-compare.sh is executable" {
    # The release workflow invokes it by path, not through `bash`.
    [ -x "$SCRIPT" ]
}

@test "--help prints the precedence rules and exits 0" {
    run "$SCRIPT" --help
    [ "$status" -eq 0 ]
    contains "Semantic Versioning 2.0.0" "$output"
    contains "build metadata" "$output"
}

@test "a verdict is one line on stdout and exit 0" {
    # Exit status carries success, never the ordering: a caller that read the
    # status as the answer would see every comparison as "equal".
    run "$SCRIPT" 1.1.0-beta.1 1.1.0
    [ "$status" -eq 0 ]
    [ "${#lines[@]}" -eq 1 ]
    [ "$output" = "-1" ]
}

# --------------------------------------------------------------- the corpus

@test "the corpus is non-empty and exercises prereleases" {
    # Guards every corpus-driven test below: an empty corpus, or one carrying no
    # prerelease, would let them pass while proving nothing about rule 2.
    local n pre
    n=$(corpus | wc -l | tr -d ' ')
    [ "$n" -ge 10 ] || fail "corpus holds $n versions; expected the full ladder"
    pre=$(corpus | grep -c -- '-' || true)
    [ "$pre" -ge 4 ] || fail "corpus holds $pre prereleases; expected several"
}

@test "the corpus lists no version twice" {
    # Two identical lines would demand -1 from a pair that ranks 0, so the matrix
    # test below would fail with a confusing message instead of this one.
    local n uniq
    n=$(corpus | wc -l | tr -d ' ')
    uniq=$(corpus | sort -u | wc -l | tr -d ' ')
    [ "$n" = "$uniq" ] || fail "corpus has $n lines but only $uniq distinct versions"
}

@test "every corpus version parses and ranks equal to itself" {
    # A version the comparator cannot parse is a failure here, not a skip: an
    # unparseable corpus line would silently drop a row and a column from the
    # matrix below.
    local v out problems=""
    while IFS= read -r v; do
        [ -n "$v" ] || continue
        if ! out=$("$SCRIPT" "$v" "$v" 2>&1); then
            problems="$problems
$v: comparator failed: $out"
        elif [ "$out" != "0" ]; then
            problems="$problems
$v vs itself: want 0, got $out"
        fi
    done <<< "$(corpus)"

    [ -z "$problems" ] || fail "corpus versions that are not equal to themselves:$problems"
}

@test "every ordered pair of corpus versions ranks as the corpus says" {
    local -a v
    # A version never contains whitespace, so plain word splitting is the split.
    v=( $(corpus) )
    local n=${#v[@]}
    [ "$n" -ge 10 ] || fail "corpus holds $n versions; expected the full ladder"

    local i=0 j want got problems=""
    while [ "$i" -lt "$n" ]; do
        j=0
        while [ "$j" -lt "$n" ]; do
            want=0
            if [ "$i" -lt "$j" ]; then want=-1; fi
            if [ "$i" -gt "$j" ]; then want=1; fi

            if ! got=$("$SCRIPT" "${v[$i]}" "${v[$j]}" 2>&1); then
                problems="$problems
${v[$i]} vs ${v[$j]}: comparator failed: $got"
            elif [ "$got" != "$want" ]; then
                problems="$problems
${v[$i]} vs ${v[$j]}: want $want, got $got"
            fi
            j=$((j + 1))
        done
        i=$((i + 1))
    done

    [ -z "$problems" ] || fail "pairs that disagree with $CORPUS:$problems"
}

# ------------------------------------------------------- named precedence rules

@test "a prerelease ranks below its own release" {
    # Rule 2, and the reason this script exists rather than a `sort -V` pipeline:
    # GNU version sort puts 1.1.0 first, which inverts the channel guard's
    # verdict on every channel that has served a prerelease.
    run "$SCRIPT" 1.1.0-beta.1 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]

    run "$SCRIPT" 1.1.0 1.1.0-beta.1
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]
}

@test "a trunk beta outranks every hotfix on the older line" {
    # The whole point of the scheme: main ships 1.1.0-beta.N while release/1.0
    # keeps shipping 1.0.x, and the beta must stay above all of them.
    run "$SCRIPT" 1.1.0-beta.1 1.0.99
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]

    run "$SCRIPT" 1.1.0-beta.1 1.0.100
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]
}

@test "numeric prerelease identifiers compare numerically, not as strings" {
    # Rule 3: beta.2 < beta.11, where a lexical compare would say the opposite.
    run "$SCRIPT" 1.1.0-beta.2 1.1.0-beta.11
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]
}

@test "a numeric identifier ranks below an alphanumeric one" {
    run "$SCRIPT" 1.1.0-11 1.1.0-alpha
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]

    run "$SCRIPT" 1.1.0-alpha.1 1.1.0-alpha.beta
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]
}

@test "more identifiers wins when every shared one is equal" {
    # Rule 4.
    run "$SCRIPT" 1.1.0-beta 1.1.0-beta.1
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]

    run "$SCRIPT" 1.1.0-beta.1 1.1.0-beta
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]
}

@test "an identifier too long for shell arithmetic still compares" {
    # Numeric identifiers are unbounded in semver. Comparing them with $(( ))
    # would overflow intmax_t and answer confidently.
    run "$SCRIPT" 1.1.0-1 1.1.0-99999999999999999999999
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]

    run "$SCRIPT" 1.1.0-99999999999999999999999 1.1.0-99999999999999999999998
    [ "$status" -eq 0 ]
    [ "$output" = "1" ]
}

# -------------------------------------------------------------- build metadata

@test "build metadata does not affect precedence" {
    # Rule 5. These cases cannot live in the corpus, which is a strict ordering.
    run "$SCRIPT" 1.1.0+abc 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]

    run "$SCRIPT" 1.1.0 1.1.0+abc
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]

    run "$SCRIPT" 1.1.0+abc 1.1.0+def
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]

    run "$SCRIPT" 1.1.0-beta.1+g1234567 1.1.0-beta.1
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]
}

@test "build metadata is ignored without swallowing the prerelease" {
    # The '+' is stripped first, so a hyphen inside build metadata must not be
    # mistaken for the start of a prerelease.
    run "$SCRIPT" 1.1.0+build-7 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]

    run "$SCRIPT" 1.1.0-beta.1+build-7 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]
}

@test "a hyphen inside a prerelease identifier is legal" {
    run "$SCRIPT" 1.1.0-x-y-z 1.1.0
    [ "$status" -eq 0 ]
    [ "$output" = "-1" ]

    run "$SCRIPT" 1.1.0-x-y-z 1.1.0-x-y-z
    [ "$status" -eq 0 ]
    [ "$output" = "0" ]
}

# ----------------------------------------------------------- malformed input

@test "a malformed version exits 1 with the reason on stderr and no verdict" {
    run bash -c '"$1" 1.0.x 1.1.0 2>/dev/null' _ "$SCRIPT"
    [ "$status" -eq 1 ]
    [ -z "$output" ] || fail "stdout carried a verdict for a malformed version: $output"

    run bash -c '"$1" 1.0.x 1.1.0 2>&1 >/dev/null' _ "$SCRIPT"
    [ "$status" -eq 1 ]
    contains "error:" "$output"
}

@test "an empty version is an error, not an implicit 0.0.0" {
    run "$SCRIPT" "" 1.1.0
    [ "$status" -eq 1 ]
    contains "error:" "$output"

    run "$SCRIPT" 1.1.0 ""
    [ "$status" -eq 1 ]
    contains "error:" "$output"
}

@test "every malformed spelling is rejected in both argument positions" {
    # A comparator that validates only its first argument answers confidently
    # about a string it never parsed.
    local bad out problems=""
    for bad in 1.1 1.1.0.0 v1.1.0 1.1.0- 1.1.0-beta. 1.1.0+ 01.1.0 1.1.0-beta.01 \
               1.1.0-beta+ 1.1.0+be+ta 1.0.x abc 1..0; do
        if out=$("$SCRIPT" "$bad" 1.1.0 2>&1); then
            problems="$problems
first='$bad' answered '$out'"
        fi
        if out=$("$SCRIPT" 1.1.0 "$bad" 2>&1); then
            problems="$problems
second='$bad' answered '$out'"
        fi
    done

    [ -z "$problems" ] || fail "malformed versions that produced a verdict:$problems"
}

@test "a usage error exits 2, distinct from a malformed version" {
    # The workflow distinguishes them: 2 means the step is calling it wrong.
    run "$SCRIPT"
    [ "$status" -eq 2 ]

    run "$SCRIPT" 1.1.0
    [ "$status" -eq 2 ]

    run "$SCRIPT" 1.1.0 1.1.1 1.1.2
    [ "$status" -eq 2 ]

    run "$SCRIPT" --nope 1.1.0 1.1.1
    [ "$status" -eq 2 ]
    contains "unknown option" "$output"
}

# --------------------------------------------------------------- CI wiring

@test "the release workflow's channel guard calls the comparator" {
    # A guard that reached for `sort -V` would report a prerelease as a downgrade
    # of its own release and block the publish, or the reverse.
    local guard code
    guard=$(guard_script)
    [ -n "$guard" ] || fail "could not extract the guard step from .github/workflows/release.yml"
    contains "version-compare.sh" "$guard"

    # Comment lines are dropped before the check: the step's own comment names
    # `sort -V` to say why it is not used.
    code=$(printf '%s\n' "$guard" | grep -v '^[[:space:]]*#' || true)
    [ -n "$code" ] || fail "guard step extracted no executable lines"
    lacks "sort -V" "$code"
}

# ------------------------------------------------- the guard step's behaviour
#
# The guard lives in the workflow, so it is extracted and run here rather than
# restated: a copy of its logic in a test would agree with the step by
# convention until it silently did not. curl is stubbed to serve a chosen
# manifest version; everything else is the step's own shell.

guard_script() {
    awk '
        /^    - name: Guard against channel downgrade$/ { step = 1 }
        step && /^      run: \|$/ { body = 1; next }
        body && /^    - name: / { exit }
        body { sub(/^        /, ""); print }
    ' .github/workflows/release.yml
}

# Put a curl on PATH that serves {"version": "$1"}, or fails like a 404 when $1
# is empty.
stub_manifest() {
    mkdir -p "$BATS_TEST_TMPDIR/bin"
    if [ -n "$1" ]; then
        printf '{"version":"%s"}\n' "$1" > "$BATS_TEST_TMPDIR/manifest.json"
        cat > "$BATS_TEST_TMPDIR/bin/curl" <<EOF
#!/bin/sh
cat "$BATS_TEST_TMPDIR/manifest.json"
EOF
    else
        cat > "$BATS_TEST_TMPDIR/bin/curl" <<'EOF'
#!/bin/sh
exit 22
EOF
    fi
    chmod +x "$BATS_TEST_TMPDIR/bin/curl"
    export PATH="$BATS_TEST_TMPDIR/bin:$PATH"
}

# $1 = version the channel serves ("" for no manifest), $2 = version being
# published, $3 = ALLOW_CHANNEL_DOWNGRADE, $4 = channels (default "beta").
run_guard() {
    local script="$BATS_TEST_TMPDIR/guard.sh"
    guard_script > "$script"
    [ -s "$script" ] || fail "could not extract the guard step from .github/workflows/release.yml"
    stub_manifest "$1"
    run env RELEASE_VERSION="$2" UPLOAD_CHANNELS="${4:-beta}" \
        R2_PUBLIC_URL="https://example.invalid" ALLOW_DOWNGRADE="$3" \
        bash -e "$script"
}

require_jq() {
    if ! command -v jq &>/dev/null; then
        skip "jq not installed"
    fi
}

@test "the guard step body is extractable" {
    # Guards every guard-behaviour test below: an empty extraction would make a
    # `bash -e` run of nothing exit 0 and read as "allowed".
    run guard_script
    [ "$status" -eq 0 ]
    contains "UPLOAD_CHANNELS" "$output"
    contains "version-compare.sh" "$output"
}

@test "the guard blocks a hotfix that would strand a channel on a newer beta" {
    require_jq
    run_guard "1.1.0-beta.2" "1.0.99" ""
    [ "$status" -eq 1 ]
    contains "BACKWARD" "$output"
}

@test "the guard allows the real release over a served prerelease" {
    # The verdict `sort -V` gets wrong: it ranks 1.1.0 below 1.1.0-beta.1, so it
    # would refuse to publish the release the beta was a beta of.
    require_jq
    run_guard "1.1.0-beta.1" "1.1.0" ""
    [ "$status" -eq 0 ]
    contains "forward" "$output"
}

@test "the guard allows the next beta on the same line" {
    require_jq
    run_guard "1.1.0-beta.2" "1.1.0-beta.11" ""
    [ "$status" -eq 0 ]
    contains "forward" "$output"
}

@test "the guard allows republishing the same version" {
    # How a partially-failed upload is retried.
    require_jq
    run_guard "1.1.0-beta.1" "1.1.0-beta.1" ""
    [ "$status" -eq 0 ]
    contains "republishing the same version" "$output"
}

@test "an absent manifest is nothing to downgrade" {
    require_jq
    run_guard "" "1.1.0-beta.1" ""
    [ "$status" -eq 0 ]
    contains "nothing to downgrade" "$output"
}

@test "ALLOW_CHANNEL_DOWNGRADE overrides a backward move" {
    require_jq
    run_guard "1.1.0-beta.2" "1.0.99" "true"
    [ "$status" -eq 0 ]
    contains "ALLOW_CHANNEL_DOWNGRADE=true" "$output"
}

@test "every channel in UPLOAD_CHANNELS is checked" {
    require_jq
    run_guard "1.1.0-beta.1" "1.1.0-beta.2" "" "beta dev"
    [ "$status" -eq 0 ]
    contains "beta: " "$output"
    contains "dev: " "$output"
}

@test "an unparseable served version fails the guard rather than passing it" {
    require_jq
    run_guard "1.1.0.beta.1" "1.1.0-beta.2" ""
    [ "$status" -eq 1 ]
    contains "cannot order" "$output"
}

# ---------------------------------------------------------------------------
# --sort
#
# The ordering cases still are not written here. A shuffle of the corpus has to
# come back in the corpus's own order, so --sort is asserted against the same
# ladder as the pairwise mode and cannot drift from it.
# ---------------------------------------------------------------------------

corpus_versions() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$CORPUS" | grep -v '^$'
}

@test "--sort returns a shuffled corpus to its own order" {
    local expected shuffled got seed
    expected="$(corpus_versions)"

    for seed in 1 2 3 4 5; do
        shuffled="$(echo "$expected" | shuf --random-source=<(yes "$seed"))"
        got="$(echo "$shuffled" | bash "$SCRIPT" --sort)"
        [ "$got" = "$expected" ] || {
            echo "seed $seed produced:"; echo "$got"
            fail "--sort did not reproduce the corpus order"
        }
    done
}

@test "--sort ranks a release above its own prereleases" {
    # The trap a numeric field sort falls into: 1.1.0 must come last here.
    run bash -c "printf '1.1.0\n1.1.0-rc.1\n1.1.0-beta.2\n' | bash '$SCRIPT' --sort"
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | tail -1)" = "1.1.0" ] || fail "release did not sort last: $output"
}

@test "--sort orders numeric prerelease identifiers numerically" {
    run bash -c "printf '1.1.0-beta.11\n1.1.0-beta.2\n' | bash '$SCRIPT' --sort"
    [ "$status" -eq 0 ]
    [ "$(echo "$output" | head -1)" = "1.1.0-beta.2" ] || fail "got: $output"
}

@test "--sort refuses the whole list when one version is unrankable" {
    run bash -c "printf '1.1.0\nnot-a-version\n1.0.0\n' | bash '$SCRIPT' --sort"
    [ "$status" -eq 1 ]
    # No partial ordering may reach stdout: callers prune with head -n -N.
    [[ "$output" != *"1.0.0"$'\n'* ]] || fail "emitted a partial order: $output"
}

@test "--sort refuses a lone unrankable version" {
    # One entry means the insertion sort makes no comparisons at all, so the
    # up-front validation is the only thing standing between a caller and an
    # exit 0 handing back a list it believes is ordered.
    run bash -c "printf 'not-a-version\n' | bash '$SCRIPT' --sort"
    [ "$status" -eq 1 ]
    [[ "$output" != *"not-a-version"$'\n'* ]] || fail "echoed the input back: $output"
}

@test "--sort accepts empty input" {
    run bash -c "printf '' | bash '$SCRIPT' --sort"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--sort takes no positional arguments" {
    run bash -c "printf '1.0.0\n' | bash '$SCRIPT' --sort 1.0.0"
    [ "$status" -eq 2 ]
}

@test "no workflow orders versions with a numeric field sort" {
    # sort -t. -k3,3n reads the patch of 1.1.0-beta.1 as 0 and ranks the
    # release below its own prereleases, so retention deletes the release.
    run grep -rn 'sort -t\. -k1,1n' .github/workflows/
    [ "$status" -ne 0 ] || fail "a workflow orders versions by field sort:"$'\n'"$output"
}

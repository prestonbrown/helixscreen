#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/generate-whatsnew.sh, the Play Store "What's new" text, and
# scripts/changelog-section.sh, which it reads its fallback through.
#
# Every fixture builds a throwaway repo root: the script resolves VERSION.txt,
# android/whatsnew/ and CHANGELOG.md relative to its own BASH_SOURCE, so copying
# it (plus the two scripts it shells out to) into a temp dir is the only way to
# drive it without touching the real ones. VERSION.txt defaults to a prerelease,
# which takes the changelog fallback; `stable` switches a test to 1.2.3.

load helpers
bats_require_minimum_version 1.5.0

SCRIPT="scripts/generate-whatsnew.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TEST_DIR="$(mktemp -d)"
    REPO="$TEST_DIR/repo"
    OUT="$TEST_DIR/whatsnew.txt"
    mkdir -p "$REPO/scripts"
    cp "$SCRIPT" "$REPO/scripts/"
    cp scripts/android-version-code.sh scripts/changelog-section.sh "$REPO/scripts/"
    printf '1.2.3-beta.1\n' > "$REPO/VERSION.txt"
}

stable() {
    printf '1.2.3\n' > "$REPO/VERSION.txt"
}

# Write android/whatsnew/<VERSION.txt>.txt from stdin.
whatsnew_file() {
    mkdir -p "$REPO/android/whatsnew"
    cat > "$REPO/android/whatsnew/$(cat "$REPO/VERSION.txt").txt"
}

teardown() {
    rm -rf "$TEST_DIR"
}

gen() {
    run bash "$REPO/scripts/generate-whatsnew.sh" "$@"
}

# ---------------------------------------------------------------------------
# Script hygiene
# ---------------------------------------------------------------------------

@test "generate-whatsnew.sh has valid bash syntax" {
    bash -n "$SCRIPT"
}

@test "generate-whatsnew.sh carries an SPDX header" {
    run head -3 "$SCRIPT"
    [[ "$output" == *"SPDX-License-Identifier: GPL-3.0-or-later"* ]]
}

@test "the cap counts UTF-16 code units, not bash characters" {
    if ! command -v iconv &>/dev/null; then
        skip "iconv not installed; the script falls back to character counting"
    fi
    # Play counts UTF-16 code units. An astral emoji is 1 bash character and 2
    # UTF-16 units, so 250 of them are 250 "chars" — comfortably under a naive
    # check — and exactly 500 units, already at the cap. One more character
    # must therefore be rejected. A `${#s}` check would emit 251 and pass, and
    # the upload would be refused by Play instead.
    local emoji_block="" i
    for i in $(seq 1 250); do emoji_block+="🎉"; done

    stable
    printf '%sX\n' "$emoji_block" | whatsnew_file

    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    contains "501 chars" "$output"
    refute test -e "$OUT"
}

@test "generate-whatsnew.sh passes the repo's shellcheck gate" {
    # Same severity and exclusions as tests/shell/test_shellcheck_gate.bats.
    if ! command -v shellcheck &>/dev/null; then
        skip "shellcheck not installed"
    fi
    shellcheck -S warning -e SC3043,SC1091 "$SCRIPT"
}

# ---------------------------------------------------------------------------
# android/whatsnew/<version>.txt
# ---------------------------------------------------------------------------

@test "the whatsnew file is emitted verbatim and the changelog is not read" {
    stable
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

This paragraph is written for somebody reading the repository and has no
business appearing in the Play Store listing at all.
EOF
    whatsnew_file <<'EOF'
Filament runout recovery now resumes the print by itself.
Bed mesh calibration shows live probe progress.
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    contains "android/whatsnew/1.2.3.txt" "$output"

    local expected
    expected="Filament runout recovery now resumes the print by itself.
Bed mesh calibration shows live probe progress."
    [ "$(cat "$OUT")" = "$expected" ]
    refute_grep 'repository' "$OUT"
}

@test "a stable release needs no CHANGELOG.md at all when its file exists" {
    stable
    printf 'One short line.\n' | whatsnew_file
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "One short line." ]
}

@test "a multi-line file keeps its order and interior blank lines" {
    stable
    whatsnew_file <<'EOF'
- Alpha first
- Bravo second

- Charlie third
- Delta fourth
- Echo fifth
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    if ! cmp -s "$OUT" "$REPO/android/whatsnew/1.2.3.txt"; then
        echo "expected:" >&2; cat "$REPO/android/whatsnew/1.2.3.txt" >&2
        echo "actual:" >&2; cat "$OUT" >&2
        return 1
    fi
}

@test "a prerelease with its own file uses it over the changelog" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

Prose line that would otherwise be summarized into the listing.
EOF
    printf 'One short line.\n' | whatsnew_file
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "One short line." ]
    lacks "note:" "$output"
}

@test "an over-length file is a hard error naming the actual length" {
    stable
    local i
    for i in $(seq 1 12); do
        printf -- '- Bullet number %d padded out with plenty of words to exceed the cap\n' "$i"
    done | whatsnew_file

    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    # The message must carry the measured length, not just "too long": the
    # author needs to know how much to cut.
    contains "818 chars" "$output"
    contains "limit of 500" "$output"
    contains "android/whatsnew/1.2.3.txt" "$output"
    # And it must not have written a truncated file behind the error.
    refute test -e "$OUT"
}

@test "a file of exactly 500 chars is accepted" {
    # The limit is >500, not >=500. An off-by-one here rejects text that fits
    # and sends the release author chasing a phantom.
    stable
    head -c 500 < /dev/zero | tr '\0' 'x' | whatsnew_file
    gen "$OUT"
    [ "$status" -eq 0 ]
    # 500 chars + the trailing newline printf adds = 501 bytes on disk.
    [ "$(wc -c < "$OUT")" -eq 501 ]
}

@test "a file of 501 chars is rejected" {
    stable
    head -c 501 < /dev/zero | tr '\0' 'x' | whatsnew_file
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    contains "501 chars" "$output"
    refute test -e "$OUT"
}

@test "an empty file is an error, not an empty listing" {
    stable
    printf '\n\n' | whatsnew_file
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    contains "android/whatsnew/1.2.3.txt is empty" "$output"
    refute test -e "$OUT"
}

@test "a stable release with no file fails naming the file, even with a changelog entry" {
    stable
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

- A perfectly good changelog entry that must not be distilled for Play.
EOF
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    contains "android/whatsnew/1.2.3.txt is missing" "$output"
    refute test -e "$OUT"
}

@test "a prerelease with no file falls back to the changelog with a note" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

Filament runout recovery resumes the print by itself now.
EOF
    run --separate-stderr bash "$REPO/scripts/generate-whatsnew.sh" "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "Filament runout recovery resumes the print by itself now." ]
    contains "note: no android/whatsnew/1.2.3-beta.1.txt" "$stderr"
    lacks "note:" "$output"
}

@test "every prerelease kind takes the fallback" {
    local v
    for v in 1.2.3-alpha.2 1.2.3-beta.1 1.2.3-rc.3; do
        printf '%s\n' "$v" > "$REPO/VERSION.txt"
        printf '# Changelog\n\n## [%s] - 2026-08-15\n\n- Built.\n' "$v" > "$REPO/CHANGELOG.md"
        gen "$OUT"
        [ "$status" -eq 0 ] || fail "$v: $output"
        [ "$(cat "$OUT")" = "- Built." ] || fail "$v: $(cat "$OUT")"
    done
}

@test "the fallback drops HTML comments, including an old whatsnew block" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

<!-- whatsnew
Text meant for an old pipeline.
-->

The beta's own prose. <!-- an editor's aside --> It carries on.

## [1.2.2] - 2026-08-01

- old
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "The beta's own prose.  It carries on." ]
    refute_grep '<!--' "$OUT"
    refute_grep -- '-->' "$OUT"
    refute_grep 'old pipeline' "$OUT"
    refute_grep 'aside' "$OUT"
}

# ---------------------------------------------------------------------------
# Fallback: markdown-stripped section, truncated on a boundary
# ---------------------------------------------------------------------------

@test "wrapped prose is cut at a sentence end, not mid-clause" {
    # Bugs 1 and 3 together. In this fixture the last period inside the first
    # 500 chars sits at 413 and the last newline at 492, so:
    #   - the awk-based scan (bug 1) never reaches either and word-chops with
    #     a trailing "…";
    #   - longest-wins (bug 3) takes the 492 newline and ends on "exceeds the".
    # Only the current logic ends on "the trap."
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

The release notes open with a long hard-wrapped paragraph of prose that
was written for somebody reading the repository, not for somebody
standing in the Play Store with a five hundred character budget, and it
runs on well past that budget before it reaches anything resembling a
natural stopping point. The wrapping falls wherever the column ran out
rather than where a sentence ended, which is exactly the trap. Several
more clauses follow here so that the paragraph comfortably exceeds the
limit and the truncation logic has to make a real choice about where to
stop cutting the text off.

## [1.2.2] - 2026-08-01

- old
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]

    local expected
    expected="The release notes open with a long hard-wrapped paragraph of prose that
was written for somebody reading the repository, not for somebody
standing in the Play Store with a five hundred character budget, and it
runs on well past that budget before it reaches anything resembling a
natural stopping point. The wrapping falls wherever the column ran out
rather than where a sentence ended, which is exactly the trap."
    if [ "$(cat "$OUT")" != "$expected" ]; then
        echo "expected:" >&2; printf '%s\n' "$expected" >&2
        echo "actual:" >&2; cat "$OUT" >&2
        return 1
    fi

    # Restated as the properties that matter, so a failure says which one broke.
    [[ "$(cat "$OUT")" == *. ]] || fail "summary does not end in a full stop"
    refute_grep 'exceeds the$' "$OUT"   # the mid-sentence line break (bug 3)
    refute_grep '…' "$OUT"              # the word-chop fallback (bug 1)
    [ "$(wc -c < "$OUT")" -le 501 ]     # content + the one trailing newline
}

@test "a bullet list with no periods is cut at an item boundary" {
    # Periods are scarce in a bullet list, so the line boundary is the right
    # answer here — but it must land between items, never inside one.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

- Filament runout detection now recovers without dropping the print job
- Bed mesh calibration reports its progress while the probe is running
- The macro panel keeps its scroll position across a panel switch again
- Temperature presets survive a restart on every supported printer model
- Camera streams reconnect on their own after the network drops briefly
- Print history shows the filament used per job instead of per session
- Touch input no longer misses the first tap after the screen dims out
- WiFi setup lists hidden networks when the SSID is entered by hand now

## [1.2.2] - 2026-08-01

- old
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]

    local expected
    expected="- Filament runout detection now recovers without dropping the print job
- Bed mesh calibration reports its progress while the probe is running
- The macro panel keeps its scroll position across a panel switch again
- Temperature presets survive a restart on every supported printer model
- Camera streams reconnect on their own after the network drops briefly
- Print history shows the filament used per job instead of per session"
    if [ "$(cat "$OUT")" != "$expected" ]; then
        echo "expected:" >&2; printf '%s\n' "$expected" >&2
        echo "actual:" >&2; cat "$OUT" >&2
        return 1
    fi

    # Every emitted line is a whole bullet: none is a prefix of the next one
    # in the source, and nothing was word-chopped.
    local line
    while IFS= read -r line; do
        [[ "$line" == -\ * ]] || fail "not a whole bullet: $line"
        grep -qxF -- "$line" "$REPO/CHANGELOG.md" || fail "truncated bullet: $line"
    done < "$OUT"
    refute_grep '…' "$OUT"
    [ "$(wc -c < "$OUT")" -le 501 ]
}

@test "a section under the limit passes through untruncated" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

Filament runout recovery resumes the print by itself now.

## [1.2.2] - 2026-08-01

- old
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "Filament runout recovery resumes the print by itself now." ]
    refute_grep '…' "$OUT"
}

@test "the fallback strips markdown emphasis, code spans and links" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

### Fixed

- **Bold** and *italic* and `code` and [a link](https://example.com/x) all flatten.
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    contains "Bold and italic and code and a link all flatten." "$(cat "$OUT")"
    refute_grep '\*\*' "$OUT"
    refute_grep 'https://' "$OUT"
    refute_grep '###' "$OUT"
}

@test "the fallback preserves bullet order with nothing concatenated" {
    # The same `printf '%s' | tac` shape as bug 2, on the OTHER trim pipeline —
    # the one that trims the markdown-stripped section (the `cleaned=` line).
    # Fixing only the explicit-block trim leaves this path folding the last two
    # bullets together and swapping them.
    #
    # If this test is red: the fix is the same one-character change already
    # applied to the explicit-block trim — `printf '%s' "$cleaned"` must become
    # `printf '%s\n' "$cleaned"` in scripts/generate-whatsnew.sh.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

### Fixed

- First bullet stays first
- Second bullet stays second
- Third bullet stays third
- Fourth bullet stays fourth
- Fifth bullet stays fifth
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]

    local expected
    expected="Fixed:

- First bullet stays first
- Second bullet stays second
- Third bullet stays third
- Fourth bullet stays fourth
- Fifth bullet stays fifth"
    if [ "$(cat "$OUT")" != "$expected" ]; then
        echo "expected:" >&2; printf '%s\n' "$expected" >&2
        echo "actual:" >&2; cat "$OUT" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Section selection
# ---------------------------------------------------------------------------

@test "a missing CHANGELOG entry for the version exits non-zero" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [9.0.0] - 2026-08-15

- not our version
EOF
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    contains "no CHANGELOG.md entry for version 1.2.3-beta.1" "$output"
    refute test -e "$OUT"
}

@test "an entry present but empty exits non-zero" {
    # A heading with nothing under it must not produce an empty What's new
    # file that the Play Store would then reject at upload time.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

## [1.2.2] - 2026-08-01

- old
EOF
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    refute test -e "$OUT"
}

@test "only the current version's section is read" {
    # The scan must stop at the next "## [" heading, not run to end of file.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.3.0] - 2026-09-01

- unreleased future work

## [1.2.3-beta.1] - 2026-08-15

- ours and only ours

## [1.2.2] - 2026-08-01

- ancient history
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "- ours and only ours" ]
    refute_grep 'future work' "$OUT"
    refute_grep 'ancient history' "$OUT"
}

# ---------------------------------------------------------------------------
# Output path
# ---------------------------------------------------------------------------

@test "the positional argument sets the output path" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

- Something happened.
EOF
    local custom="$TEST_DIR/nested/dir/custom.txt"
    gen "$custom"
    [ "$status" -eq 0 ]
    [ -f "$custom" ]
    [ "$(cat "$custom")" = "- Something happened." ]
    [[ "$output" == *"$custom"* ]]
}

@test "with no argument it writes the versionCode-derived fastlane path" {
    # release.yml reads this exact path back to upload it; a rename here is
    # invisible until the Play publish step cannot find the file.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

- Something happened.
EOF
    local code expected
    code="$("$REPO/scripts/android-version-code.sh" 1.2.3-beta.1)"
    expected="$REPO/android/fastlane/metadata/android/en-US/changelogs/$code.txt"
    gen
    [ "$status" -eq 0 ]
    [ -f "$expected" ]
    [ "$(cat "$expected")" = "- Something happened." ]
    [[ "$output" == *"versionCode $code"* ]]
}

@test "the output path's parent directory is created if absent" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

- Something happened.
EOF
    refute test -d "$TEST_DIR/a"
    gen "$TEST_DIR/a/b/c/whatsnew.txt"
    [ "$status" -eq 0 ]
    [ -f "$TEST_DIR/a/b/c/whatsnew.txt" ]
}

@test "the repo root is resolved from the script, not the caller's cwd" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3-beta.1] - 2026-08-15

- Resolved from BASH_SOURCE.
EOF
    run bash -c "cd '$TEST_DIR' && bash '$REPO/scripts/generate-whatsnew.sh' '$OUT'"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "- Resolved from BASH_SOURCE." ]
}

# ---------------------------------------------------------------------------
# The real repo
# ---------------------------------------------------------------------------

@test "the repo's own CHANGELOG.md generates without error" {
    # The release runs this for real. A section that trips the over-length
    # guard or the missing-entry guard must be caught here, not at publish
    # time. Writes to a temp path so the tracked fastlane file is untouched.
    run bash "$SCRIPT" "$OUT"
    [ "$status" -eq 0 ]
    [ -s "$OUT" ]
    [ "$(wc -c < "$OUT")" -le 501 ]
}

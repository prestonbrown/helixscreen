#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for scripts/generate-whatsnew.sh — the Play Store "What's new" text.
#
# Nothing tested this script, and three separate bugs shipped in it silently:
#
#   1. The sentence-boundary search ran inside awk. awk is line-oriented, so a
#      backward scan for "." over `$0` only ever saw the FIRST line of the
#      truncated text, and `head -1` took that line's answer. The index landed
#      below the 100-char floor every time, so the sentence branch never once
#      fired and every release fell through to a crude word-chop plus an
#      ellipsis. The 1.0 release candidate's What's New ended mid-clause on
#      "...because the wrong…".
#
#   2. A trim pipeline fed `printf '%s'` (no trailing newline) into `tac`, which
#      folds an unterminated final line into its neighbour — silently reordering
#      AND concatenating the last two bullets. `printf 'a\nb\nc' | tac` is
#      "cb\na\n", not "c\nb\na\n".
#
#   3. The truncation preferred whichever of (last period, last newline) was
#      LONGER. Hard-wrapped prose breaks wherever the column ran out, so the
#      longest-wins rule picks a mid-sentence line break over a real sentence
#      end sitting just behind it.
#
# Every fixture below builds a throwaway repo root — the script resolves
# VERSION.txt and CHANGELOG.md relative to its own BASH_SOURCE, so copying it
# (plus android-version-code.sh, which it shells out to) into a temp dir is the
# only way to drive it without touching the real ones.

load helpers

SCRIPT="scripts/generate-whatsnew.sh"

setup() {
    cd "$BATS_TEST_DIRNAME/../.." || return 1
    TEST_DIR="$(mktemp -d)"
    REPO="$TEST_DIR/repo"
    OUT="$TEST_DIR/whatsnew.txt"
    mkdir -p "$REPO/scripts"
    cp "$SCRIPT" "$REPO/scripts/"
    cp scripts/android-version-code.sh "$REPO/scripts/"
    printf '1.2.3\n' > "$REPO/VERSION.txt"
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

@test "generate-whatsnew.sh exists and is executable" {
    [ -f "$SCRIPT" ]
    [ -x "$SCRIPT" ]
}

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

    {
        printf '# Changelog\n\n## [1.2.3] - 2026-08-15\n\n<!-- whatsnew\n'
        printf '%sX\n' "$emoji_block"
        printf -- '-->\n\n## [1.2.2] - 2026-08-01\n\n- old\n'
    } > "$REPO/CHANGELOG.md"

    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"501 chars"* ]]
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
# The explicit <!-- whatsnew --> block
# ---------------------------------------------------------------------------

@test "an explicit whatsnew block is emitted verbatim and the prose is not" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

### Added

<!-- whatsnew
Filament runout recovery now resumes the print by itself.
Bed mesh calibration shows live probe progress.
-->

This paragraph is written for somebody reading the repository and has no
business appearing in the Play Store listing at all.

## [1.2.2] - 2026-08-01

- old stuff
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"explicit block"* ]]

    local expected
    expected="Filament runout recovery now resumes the print by itself.
Bed mesh calibration shows live probe progress."
    [ "$(cat "$OUT")" = "$expected" ]

    # The surrounding prose, the subheading, and the comment delimiters must
    # all be absent — the block is the whole output, not a prefix of it.
    refute_grep 'repository' "$OUT"
    refute_grep 'Added' "$OUT"
    refute_grep 'whatsnew' "$OUT"
    refute_grep 'old stuff' "$OUT"
}

@test "a five-bullet block keeps its order with nothing concatenated" {
    # Bug 2. `tac` on an unterminated final line folds the last two lines
    # together, so this must assert the EXACT multi-line output — a substring
    # check for "- Alpha first" passes happily on a reordered result.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

<!-- whatsnew
- Alpha first
- Bravo second
- Charlie third
- Delta fourth
- Echo fifth
-->

## [1.2.2] - 2026-08-01

- old
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]

    local expected
    expected="- Alpha first
- Bravo second
- Charlie third
- Delta fourth
- Echo fifth"
    if [ "$(cat "$OUT")" != "$expected" ]; then
        echo "expected:" >&2; printf '%s\n' "$expected" >&2
        echo "actual:" >&2; cat "$OUT" >&2
        return 1
    fi

    # Belt and braces: five lines, each a whole bullet.
    [ "$(wc -l < "$OUT")" -eq 5 ]
    refute_grep 'fifth- Delta' "$OUT"
}

@test "blank lines around the block are trimmed but interior ones are kept" {
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

<!-- whatsnew

- First

- Second

-->
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]

    local expected
    expected="- First

- Second"
    [ "$(cat "$OUT")" = "$expected" ]
}

@test "an over-length block is a hard error naming the actual length" {
    local i
    {
        printf '# Changelog\n\n## [1.2.3] - 2026-08-15\n\n<!-- whatsnew\n'
        for i in $(seq 1 12); do
            printf -- '- Bullet number %d padded out with plenty of words to exceed the cap\n' "$i"
        done
        printf -- '-->\n\n## [1.2.2] - 2026-08-01\n\n- old\n'
    } > "$REPO/CHANGELOG.md"

    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    # The message must carry the measured length, not just "too long" — the
    # author needs to know how much to cut.
    [[ "$output" == *"818 chars"* ]]
    [[ "$output" == *"limit of 500"* ]]
    # And it must not have written a truncated file behind the error.
    refute test -e "$OUT"
}

@test "a block of exactly 500 chars is accepted" {
    # The limit is >500, not >=500. An off-by-one here rejects a block that
    # fits and sends the release author chasing a phantom.
    local body
    body="$(head -c 499 < /dev/zero | tr '\0' 'x')"
    printf '# Changelog\n\n## [1.2.3] - 2026-08-15\n\n<!-- whatsnew\n%s\n-->\n' "$body" \
        > "$REPO/CHANGELOG.md"
    gen "$OUT"
    [ "$status" -eq 0 ]
    # 499 body chars + the trailing newline printf adds = 500 bytes on disk.
    [ "$(wc -c < "$OUT")" -eq 500 ]
}

@test "a block of 501 chars is rejected" {
    local body
    body="$(head -c 501 < /dev/zero | tr '\0' 'x')"
    printf '# Changelog\n\n## [1.2.3] - 2026-08-15\n\n<!-- whatsnew\n%s\n-->\n' "$body" \
        > "$REPO/CHANGELOG.md"
    rm -f "$OUT"
    gen "$OUT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"501 chars"* ]]
    refute test -e "$OUT"
}

@test "the block wins even when the section prose is far longer" {
    # The point of the explicit path: the fallback must not get a look in.
    {
        printf '# Changelog\n\n## [1.2.3] - 2026-08-15\n\n'
        printf 'Prose line %d that would otherwise be summarized into the listing.\n' 1 2 3 4 5 6 7 8 9 10
        printf '\n<!-- whatsnew\nOne short line.\n-->\n'
    } > "$REPO/CHANGELOG.md"
    gen "$OUT"
    [ "$status" -eq 0 ]
    [ "$(cat "$OUT")" = "One short line." ]
    refute_grep 'Prose line' "$OUT"
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

## [1.2.3] - 2026-08-15

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
    [[ "$(cat "$OUT")" == *. ]]
    refute_grep 'exceeds the$' "$OUT"   # the mid-sentence line break (bug 3)
    refute_grep '…' "$OUT"              # the word-chop fallback (bug 1)
    [ "$(wc -c < "$OUT")" -le 501 ]     # content + the one trailing newline
}

@test "a bullet list with no periods is cut at an item boundary" {
    # Periods are scarce in a bullet list, so the line boundary is the right
    # answer here — but it must land between items, never inside one.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

### Fixed

- **Bold** and *italic* and `code` and [a link](https://example.com/x) all flatten.
EOF
    gen "$OUT"
    [ "$status" -eq 0 ]
    [[ "$(cat "$OUT")" == *"Bold and italic and code and a link all flatten."* ]]
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

## [1.2.3] - 2026-08-15

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
    [[ "$output" == *"no CHANGELOG.md entry for version 1.2.3"* ]]
    refute test -e "$OUT"
}

@test "an entry present but empty exits non-zero" {
    # A heading with nothing under it must not produce an empty What's new
    # file that the Play Store would then reject at upload time.
    cat > "$REPO/CHANGELOG.md" <<'EOF'
# Changelog

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

- Something happened.
EOF
    local code expected
    code="$("$REPO/scripts/android-version-code.sh" 1.2.3)"
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

## [1.2.3] - 2026-08-15

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

## [1.2.3] - 2026-08-15

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

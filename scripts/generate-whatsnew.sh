#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Extract the current version's CHANGELOG section into a Play Store "What's new"
# file: android/fastlane/metadata/android/en-US/changelogs/<versionCode>.txt
#
# The Play Store "What's new" field is per-versionCode and capped at 500 chars
# (UTF-16 code units).
#
# Two sources, in order of preference:
#
#   1. An explicit `<!-- whatsnew ... -->` block inside the version's CHANGELOG
#      section. Used verbatim. This is the intended path: CHANGELOG prose is
#      user-facing (docs/devel/CHANGELOG_STYLE.md), and What's New is that same
#      audience at a 500-char budget - a hand-picked distillation, because a
#      full release section does not fit and mechanical summarization is lossy
#      by construction. An over-length block is a hard error rather than a
#      silent truncation: hand-authored text that does not fit should be
#      edited by its author, not chopped by a script.
#
#   2. Failing that, the section body with markdown stripped, truncated on a
#      sentence boundary. A fallback, not the design: it produces whatever the
#      changelog happens to open with, which for a release whose section starts
#      with release-process prose is not what a Play Store user wants to read.
#
# Usage:
#   scripts/generate-whatsnew.sh                    # writes to default path
#   scripts/generate-whatsnew.sh /tmp/whatsnew.txt  # writes to given path
#
# Exit 0 on success, non-zero if CHANGELOG has no entry for the version.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "$repo_root/VERSION.txt")"

# The versionCode names the output file, and release.yml reads that same path
# back to upload it. Both sides resolve it through android-version-code.sh so
# the packing has one definition; a local copy here is what let the two drift.
# Its overflow / malformed-version errors surface on stderr and abort (set -e).
version_code="$("$repo_root/scripts/android-version-code.sh" "$version")"

out_path="${1:-$repo_root/android/fastlane/metadata/android/en-US/changelogs/$version_code.txt}"
mkdir -p "$(dirname "$out_path")"

# Extract the section for this version from CHANGELOG.md.
# Matches a "## [version]" heading and prints lines until the next "## [" heading.
section="$(awk -v ver="$version" '
    /^## \[/ {
        if (found) exit
        if (index($0, "[" ver "]")) { found=1; next }
        next
    }
    found { print }
' "$repo_root/CHANGELOG.md")"

if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "error: no CHANGELOG.md entry for version $version" >&2
    exit 1
fi

limit=500

# Length in UTF-16 code units, which is what the Play Store actually counts.
# `${#s}` counts bash characters, and the two disagree on anything outside the
# BMP: one astral emoji is 1 character and 2 UTF-16 units, so a 499-"char" blurb
# with two emoji passes a naive check here and is rejected at upload.
# iconv is not guaranteed present; falling back to the character count keeps the
# script working and is the same (safe, never over-permissive) answer for the
# all-BMP text this repo actually ships.
utf16_len() {
    if command -v iconv >/dev/null 2>&1; then
        printf '%s' "$1" | iconv -f UTF-8 -t UTF-16LE 2>/dev/null | wc -c | awk '{print int($1/2)}'
    else
        printf '%s' "${#1}"
    fi
}

# Preferred source: an explicit `<!-- whatsnew ... -->` block in the section.
# Taken verbatim, so what lands in the Play Store is exactly what was reviewed
# in the release diff.
explicit="$(printf '%s\n' "$section" | awk '
    /<!--[[:space:]]*whatsnew/ { inblock=1; next }
    inblock && /-->/          { exit }
    inblock                   { print }
')"
# Trim leading/trailing blank lines. The `\n` matters: `tac` reverses whole
# lines, and an unterminated final line gets folded into its neighbour, which
# silently reorders and concatenates the last two bullets.
explicit="$(printf '%s\n' "$explicit" | sed -e '/./,$!d' | tac | sed -e '/./,$!d' | tac)"

if [ -n "$(printf '%s' "$explicit" | tr -d '[:space:]')" ]; then
    explicit_len="$(utf16_len "$explicit")"
    if [ "$explicit_len" -gt "$limit" ]; then
        echo "error: the <!-- whatsnew --> block for $version is $explicit_len chars," \
             "over the Play Store limit of $limit." >&2
        echo "       Edit the block in CHANGELOG.md — it is used verbatim and is" \
             "deliberately not truncated." >&2
        exit 1
    fi
    printf '%s\n' "$explicit" > "$out_path"
    echo "Wrote $(wc -c < "$out_path") bytes to $out_path" \
         "(version $version, versionCode $version_code, explicit block)"
    exit 0
fi

# Strip markdown: drop "### Added/Fixed/Changed/..." subheadings, flatten
# bullets to "- ", collapse blank-line runs, remove bold/italic/link markup.
cleaned="$(printf '%s' "$section" | awk '
    /^### / { section=substr($0, 5); print ""; print section ":"; next }
    /^[[:space:]]*$/ { print ""; next }
    /^[[:space:]]*-[[:space:]]/ { print; next }
    { print }
' | sed -E '
    s/\*\*([^*]+)\*\*/\1/g
    s/\*([^*]+)\*/\1/g
    s/`([^`]+)`/\1/g
    s/\[([^]]+)\]\([^)]+\)/\1/g
' | awk 'BEGIN{blank=0} /^[[:space:]]*$/ { if (blank) next; blank=1; print; next } { blank=0; print }')"

# Trim leading/trailing blank lines.
# Same `tac` trap as the explicit-block trim above: `printf '%s'` leaves the
# final line unterminated, so `tac` folds it into its neighbour and the last two
# bullets come out swapped and concatenated. Here it hid for longer, because a
# section long enough to reach the truncation branch usually loses the folded
# line to the 500-char cut anyway — but any section under the cap shipped it.
cleaned="$(printf '%s\n' "$cleaned" | sed -e '/./,$!d' | tac | sed -e '/./,$!d' | tac)"

# Truncate to the cap on a sentence or line boundary.
#
# The boundary search runs in bash parameter expansion rather than awk. awk is
# line-oriented — `$0` holds one line, so a backward scan for "." inside an awk
# body only ever searched the FIRST line of the truncated text, and the
# `head -1` took that line's answer. The resulting index was almost always
# below the 100-char floor, so the sentence-boundary branch never once fired
# and every release fell through to the word-chop below. That is how the 1.0
# release candidate's What's New ended mid-clause on "...because the wrong…".
if [ "$(utf16_len "$cleaned")" -gt "$limit" ]; then
    # Slice by character, then correct for the UTF-16 overshoot. Each astral
    # character costs one extra unit, so subtracting the excess converges in a
    # single pass; the loop is a backstop, not the expected path.
    slice="$limit"
    while [ "$slice" -gt 0 ] && [ "$(utf16_len "${cleaned:0:$slice}")" -gt "$limit" ]; do
        over="$(( $(utf16_len "${cleaned:0:$slice}") - limit ))"
        slice="$(( slice - (over > 0 ? over : 1) ))"
    done
    truncated="${cleaned:0:$slice}"

    # Longest prefix ending at the last "." (kept) or the last newline
    # (dropped). `%` strips the shortest matching suffix, so each of these
    # backs up to the LAST occurrence in the whole string, not the first line.
    by_period="${truncated%.*}"
    if [ "$by_period" = "$truncated" ]; then by_period=""; else by_period="${by_period}."; fi

    by_line="${truncated%$'\n'*}"
    if [ "$by_line" = "$truncated" ]; then by_line=""; fi

    # Prefer the period by QUALITY, not by length. In hard-wrapped prose a
    # newline falls wherever the column ran out, so the longest-wins rule picks
    # a mid-sentence line break over a real sentence end sitting just behind it
    # ("...against 1.0. Several turned"). A line boundary is only worth having
    # when there is no usable sentence end — a bullet list, where periods are
    # scarce but every line break is a clean item boundary.
    #
    # A boundary so early it would throw away most of the budget is worse than
    # a clean word-chop, so the floor applies to each candidate in turn.
    if [ "${#by_period}" -gt 100 ]; then
        cleaned="$by_period"
    elif [ "${#by_line}" -gt 100 ]; then
        cleaned="$by_line"
    else
        cleaned="${truncated%[[:space:]]*}…"
    fi
fi

printf '%s\n' "$cleaned" > "$out_path"
echo "Wrote $(wc -c < "$out_path") bytes to $out_path (version $version, versionCode $version_code)"

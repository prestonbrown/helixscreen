#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Write the Play Store "What's new" file for the current version:
# android/fastlane/metadata/android/en-US/changelogs/<versionCode>.txt
#
# The Play Store "What's new" field is per-versionCode and capped at 500 chars
# (UTF-16 code units).
#
# Sources:
#
#   1. android/whatsnew/<version>.txt, hand-written for every release that ships
#      to Google Play (android/whatsnew/README.md). Used verbatim. CHANGELOG
#      prose is written for a reader with the whole section in front of them;
#      What's New is the same audience at a 500-char budget, a hand-picked
#      distillation that mechanical summarization cannot produce. An over-length
#      file is a hard error rather than a silent truncation: hand-authored text
#      that does not fit should be edited by its author, not chopped by a script.
#
#   2. A prerelease (-alpha/-beta/-rc) with no such file gets the version's
#      CHANGELOG section with markdown stripped, truncated on a sentence
#      boundary. Prereleases never reach Play, so this only has to give the
#      release workflow a file to carry; a note on stderr says it happened.
#
#   A stable version with no file is an error naming the file to write.
#
# Usage:
#   scripts/generate-whatsnew.sh                    # writes to default path
#   scripts/generate-whatsnew.sh /tmp/whatsnew.txt  # writes to given path
#
# Exit 0 on success; non-zero for a stable version with no whatsnew file, an
# over-length file, or a prerelease with no CHANGELOG entry to fall back on.

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

whatsnew_rel="android/whatsnew/$version.txt"
whatsnew_file="$repo_root/$whatsnew_rel"

if [ -f "$whatsnew_file" ]; then
    # Verbatim, so what lands in the Play Store is exactly what was reviewed in
    # the release diff. Command substitution drops only the trailing newlines.
    text="$(cat "$whatsnew_file")"
    if [ -z "$(printf '%s' "$text" | tr -d '[:space:]')" ]; then
        echo "error: $whatsnew_rel is empty." >&2
        exit 1
    fi
    text_len="$(utf16_len "$text")"
    if [ "$text_len" -gt "$limit" ]; then
        echo "error: $whatsnew_rel is $text_len chars," \
             "over the Play Store limit of $limit." >&2
        echo "       Edit the file - it is used verbatim and is" \
             "deliberately not truncated." >&2
        exit 1
    fi
    printf '%s\n' "$text" > "$out_path"
    echo "Wrote $(wc -c < "$out_path") bytes to $out_path" \
         "(version $version, versionCode $version_code, $whatsnew_rel)"
    exit 0
fi

case "$version" in
    *-alpha*|*-beta*|*-rc*) ;;
    *)
        echo "error: $whatsnew_rel is missing. A stable release ships to Google Play," \
             "and its \"What's new\" text is hand-written there" \
             "(at most $limit chars; android/whatsnew/README.md)." >&2
        exit 1
        ;;
esac

echo "note: no $whatsnew_rel; $version is a prerelease (never sent to Play)," \
     "so the CHANGELOG.md section is distilled instead." >&2

section="$("$repo_root/scripts/changelog-section.sh" "$version" "$repo_root/CHANGELOG.md")"

if [ -z "$(printf '%s' "$section" | tr -d '[:space:]')" ]; then
    echo "error: no CHANGELOG.md entry for version $version" >&2
    exit 1
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

# Trim leading/trailing blank lines. The `\n` matters: `tac` reverses whole
# lines, and an unterminated final line gets folded into its neighbour, which
# swaps and concatenates the last two bullets.
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

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Print one version's CHANGELOG.md section: the lines under its "## [VERSION]"
# heading, up to the next "## [" heading, with HTML comments removed.
#
# Every place that publishes a section (the Play Store fallback, the Discord
# post, the update manifest's notes) reads it through here. An HTML comment in a
# section is a note for whoever edits the file, never text for a reader, and
# rendered markdown hides it where these plain-text consumers would not.
# A line that held nothing but a comment is dropped rather than left blank.
#
# Usage:
#   scripts/changelog-section.sh VERSION [CHANGELOG]   # default: the repo's CHANGELOG.md
#
# Prints nothing and exits 0 when the version has no section; exits 1 when the
# changelog file itself is missing.

set -euo pipefail

if [ $# -lt 1 ] || [ -z "$1" ]; then
    echo "usage: $0 VERSION [CHANGELOG]" >&2
    exit 2
fi

version="$1"
changelog="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/CHANGELOG.md}"

if [ ! -f "$changelog" ]; then
    echo "error: $changelog not found" >&2
    exit 1
fi

awk -v ver="$version" '
    /^## \[/ {
        if (found) exit
        if (index($0, "[" ver "]")) found = 1
        next
    }
    !found { next }
    {
        line = $0; out = ""; had_comment = 0
        while (1) {
            if (in_comment) {
                i = index(line, "-->")
                if (!i) { line = ""; had_comment = 1; break }
                line = substr(line, i + 3); in_comment = 0; had_comment = 1
            }
            i = index(line, "<!--")
            if (!i) { out = out line; break }
            out = out substr(line, 1, i - 1)
            line = substr(line, i + 4); in_comment = 1; had_comment = 1
        }
        if (had_comment && out ~ /^[[:space:]]*$/) next
        print out
    }
' "$changelog"

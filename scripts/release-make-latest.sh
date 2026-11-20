#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release-make-latest.sh: Decide the make_latest input of the "Create GitHub
# Release" step in .github/workflows/release.yml.
#
# GitHub's "latest" release is what Moonraker's stable channel reads
# (releases/latest), and Moonraker offers an update whenever that version
# differs from the installed one, downgrades included (helixscreen#2147). Left
# to its default, every new non-prerelease becomes "latest", so a v1.0.x tag cut
# after v1.1.0 would offer every 1.1 install a downgrade. A release therefore
# becomes latest only when it is not a prerelease and outranks the current
# latest release by semver precedence (scripts/version-compare.sh), or is that
# release: a re-run for the latest tag keeps it latest rather than demoting it.
#
# Usage: release-make-latest.sh VERSION PRERELEASE [LATEST_TAG]
#   VERSION     the tag being released (v1.2.3 or 1.2.3)
#   PRERELEASE  "true" or "false", as release-channel.sh resolves it
#   LATEST_TAG  tag_name of releases/latest; empty when there is none yet
# Prints "true" or "false". A version that cannot be ranked exits 1 and an
# unrecognised PRERELEASE exits 2: neither guesses.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

make_latest_for() {
    local version="${1#v}" prerelease="$2" latest="${3:-}"
    local order
    latest="${latest#v}"

    case "$prerelease" in
        true) echo false; return 0 ;;
        false) ;;
        *)
            echo "error: PRERELEASE must be true or false, got '$prerelease'" >&2
            return 2
            ;;
    esac

    if [ -z "$latest" ]; then
        echo true
        return 0
    fi

    order=$("$SCRIPT_DIR/version-compare.sh" "$version" "$latest") || return 1
    if [ "$order" != -1 ]; then
        echo true
    else
        echo false
    fi
}

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "usage: release-make-latest.sh VERSION PRERELEASE [LATEST_TAG]" >&2
    exit 2
fi

make_latest_for "$@"

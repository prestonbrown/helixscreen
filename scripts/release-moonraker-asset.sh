#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release-moonraker-asset.sh - The release asset an old Moonraker downloads.
#
# A Moonraker without asset_name support downloads a release's assets[0]
# whatever release_info.json names, and GitHub lists assets sorted by name
# (#993). Each release therefore carries one more copy of a platform's zip
# under a name that sorts before every other asset; the installer arms the
# updater on such a Moonraker only for that platform. The platform and the
# name come from scripts/lib/installer/platform.sh
# (helix_moonraker_first_asset), the same answer the installer reads.
#
#   stage DIR   copy DIR/helixscreen-<platform>.zip to DIR/<first-asset name>
#               and fail unless that name now sorts first in DIR
#   check       read asset names on stdin, in the order the GitHub API lists
#               them, and fail unless the first is the copy
#   name        print the copy's name
#
# Used by .github/workflows/release.yml. Tests:
# tests/shell/test_release_moonraker_asset.bats

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# Sourced before set -eu: the installer modules are written for the
# installer's shell options, not for -u.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/lib/installer/platform.sh" >/dev/null 2>&1
set -eu

FIRST=$(helix_moonraker_first_asset)
SOURCE="helixscreen-${HELIX_MOONRAKER_FIRST_ASSET_PLATFORM}.zip"

# Fail unless the first line of stdin is the copy.
check_first() {
    first=$(sed -n 1p)
    if [ "$first" != "$FIRST" ]; then
        echo "::error::assets[0] is '${first}', not ${FIRST}. A Moonraker without" \
             "asset_name support downloads assets[0] and deletes the install before" \
             "it fails on anything else (#993)." >&2
        return 1
    fi
    echo "assets[0] = ${first} (ok)"
}

case "${1:-}" in
    stage)
        dir="${2:?usage: $0 stage DIR}"
        if [ ! -f "$dir/$SOURCE" ]; then
            echo "::error::${SOURCE} is missing from ${dir}; the Moonraker copy cannot be staged" >&2
            exit 1
        fi
        cp "$dir/$SOURCE" "$dir/$FIRST"
        (cd "$dir" && ls -1A | LC_ALL=C sort) | check_first
        ;;
    check)
        check_first
        ;;
    name)
        echo "$FIRST"
        ;;
    *)
        echo "usage: $0 stage DIR | check | name" >&2
        exit 2
        ;;
esac

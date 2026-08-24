#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# android-version-code.sh — The single definition of the Android versionCode.
#
# Android refuses to install an APK whose versionCode is not greater than the
# installed one, and the Play Store refuses to accept a versionCode it has
# already seen. The number is therefore a one-way ratchet derived from
# VERSION.txt, packed into fixed 1000-wide lanes:
#
#     versionCode = major * 1000000 + minor * 1000 + patch
#
# The lanes must be wide enough that a field can never overflow into the one
# above it. The earlier `major * 10000 + minor * 100 + patch` packing could not
# hold our patch numbers: 0.99.113 packed to 10013 while 1.0.0 packed to 10000,
# so the 1.0 release would have been rejected as a downgrade on every existing
# Android install and by the Play Store. Widening is itself an increase
# (0.99.113: 10013 -> 99113), so installs made under the old scheme still
# upgrade.
#
# Three places need this number and they all call here rather than repeating
# the arithmetic:
#
#   android/app/build.gradle           the APK/AAB versionCode itself
#   scripts/generate-whatsnew.sh       names the Play "What's new" file
#                                      android/fastlane/.../changelogs/<code>.txt
#   .github/workflows/release.yml      reads that same file back to upload it
#
# They diverged once already — release.yml kept the old packing when the lanes
# were widened, so the workflow looked for changelogs/10014.txt while the
# script had written changelogs/99114.txt, and the Play whatsnew artifact was
# silently never produced. tests/shell/test_android_version_code.bats gates
# against a second copy reappearing.
#
# Usage:
#   scripts/android-version-code.sh            # reads VERSION.txt at repo root
#   scripts/android-version-code.sh 1.2.3      # explicit version
#
# Prints the versionCode on stdout and nothing else. Exits non-zero, with the
# reason on stderr, if the version is malformed or overflows the lanes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

usage() {
    cat <<EOF
Usage: android-version-code.sh [VERSION]

Print the Android versionCode for VERSION, or for the repo's VERSION.txt when
no argument is given.

  versionCode = major * 1000000 + minor * 1000 + patch

A prerelease suffix on the patch field is ignored (1.0.0-beta -> 1000000):
the in-app version comparison discards it too, and the Play Store has no way
to express it.

Options:
  -h, --help   Show this help
EOF
}

VERSION=""
HAVE_VERSION=false

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "error: unknown argument '$1'" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [ "$HAVE_VERSION" = true ]; then
                echo "error: expected at most one VERSION argument" >&2
                exit 2
            fi
            VERSION="$1"
            HAVE_VERSION=true
            shift
            ;;
    esac
done

if [ "$HAVE_VERSION" = false ]; then
    VERSION_FILE="$REPO_ROOT/VERSION.txt"
    if [ ! -f "$VERSION_FILE" ]; then
        echo "error: VERSION.txt not found at '$VERSION_FILE'" >&2
        exit 1
    fi
    VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
fi

# major.minor.patch, with an optional prerelease/build suffix on the patch.
# Anchored on purpose: '1.2' and '1.2.3.4' are both rejected rather than
# silently packed from whatever the fields happen to split into.
if [[ ! "$VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-.*)?$ ]]; then
    echo "error: malformed version '$VERSION'" >&2
    echo "       expected major.minor.patch, optionally with a -suffix" >&2
    echo "       (e.g. 1.2.3 or 1.0.0-beta)" >&2
    exit 1
fi

V_MAJOR="${BASH_REMATCH[1]}"
V_MINOR="${BASH_REMATCH[2]}"
V_PATCH="${BASH_REMATCH[3]}"

# Strip leading zeros so the arithmetic below never reads a field as octal.
V_MAJOR=$((10#$V_MAJOR))
V_MINOR=$((10#$V_MINOR))
V_PATCH=$((10#$V_PATCH))

if [ "$V_MINOR" -gt 999 ] || [ "$V_PATCH" -gt 999 ]; then
    echo "error: version '$VERSION' overflows the versionCode lanes (minor and" >&2
    echo "       patch must each stay under 1000). Widening the packing means" >&2
    echo "       editing scripts/android-version-code.sh — it is the only copy," >&2
    echo "       and every consumer reads it from here." >&2
    exit 1
fi

# Android's versionCode is a signed 32-bit value capped at 2100000000, so the
# major lane runs out at 2100. Nowhere near it, but the failure mode past that
# point is a Play Store rejection after a full release build.
if [ "$V_MAJOR" -gt 2099 ]; then
    echo "error: version '$VERSION' exceeds Android's maximum versionCode of" >&2
    echo "       2100000000 (major must stay under 2100)." >&2
    exit 1
fi

echo $((V_MAJOR * 1000000 + V_MINOR * 1000 + V_PATCH))

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# android-version-code.sh — The single definition of the Android versionCode.
#
# Android refuses to install an APK whose versionCode is not greater than the
# installed one, and the Play Store refuses to accept a versionCode it has
# already seen. The number is therefore a one-way ratchet derived from
# VERSION.txt, packed into fixed-width lanes:
#
#     versionCode = (major * 1000000 + minor * 1000 + patch) * 100 + ordinal
#
# Each lane must be wide enough that a field can never carry into the one above
# it, so minor and patch each stay under 1000 and major stays at or below 20:
# Android caps versionCode at 2100000000, and 20.999.999 already packs to
# 2099999999.
#
# The low two digits are the prerelease ordinal. It orders a release's
# prereleases among themselves and below the release they lead to:
#
#     -alpha.N    N           N in 1..29
#     -beta.N     30 + N
#     -rc.N       60 + N
#     no suffix   99
#
# A bare -alpha / -beta / -rc with no .N takes N=0, so it sorts below its own
# .1 the way Semantic Versioning precedence rule 4 does. Any other suffix is a
# hard error: every ordinal this script could invent for an unknown suffix
# either collides with a real one or breaks the ratchet.
#
# Three places need this number and they all call here rather than repeating
# the arithmetic:
#
#   android/app/build.gradle           the APK/AAB versionCode itself
#   scripts/generate-whatsnew.sh       names the Play "What's new" file
#                                      android/fastlane/.../changelogs/<code>.txt
#   .github/workflows/release.yml      reads that same file back to upload it
#
# A second copy of the arithmetic drifts silently rather than loudly: the APK
# keeps a correct versionCode because Gradle owns that number, while the
# changelog filename the workflow reads back stops existing and the Play
# whatsnew artifact is never produced. tests/shell/test_android_version_code.bats
# gates against a second copy appearing.
#
# Usage:
#   scripts/android-version-code.sh            # reads VERSION.txt at repo root
#   scripts/android-version-code.sh 1.2.3      # explicit version
#
# Prints the versionCode on stdout and nothing else. Exits non-zero, with the
# reason on stderr, if the version is malformed, carries a suffix with no
# ordinal, or overflows the lanes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Highest .N a prerelease may carry. 29 keeps each stage inside its own 30-wide
# band, so an -alpha can never reach a -beta ordinal, and leaves the release
# ordinal 99 above every -rc.
ORDINAL_MAX_N=29

usage() {
    cat <<'EOF'
Usage: android-version-code.sh [VERSION]

Print the Android versionCode for VERSION, or for the repo's VERSION.txt when
no argument is given.

  versionCode = (major * 1000000 + minor * 1000 + patch) * 100 + ordinal

The ordinal encodes the prerelease suffix, so each prerelease sorts above the
last and below the release it leads to:

  -alpha.N    N           N in 1..29
  -beta.N     30 + N
  -rc.N       60 + N
  no suffix   99

A bare -alpha / -beta / -rc takes N=0. Any other suffix is an error.

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

# major.minor.patch, with an optional prerelease suffix on the patch.
# Anchored on purpose: '1.2' and '1.2.3.4' are both rejected rather than
# silently packed from whatever the fields happen to split into.
if [[ ! "$VERSION" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-.*)?$ ]]; then
    echo "error: malformed version '$VERSION'" >&2
    echo "       expected major.minor.patch, optionally with a -suffix" >&2
    echo "       (e.g. 1.2.3 or 1.0.0-beta.1)" >&2
    exit 1
fi

V_MAJOR="${BASH_REMATCH[1]}"
V_MINOR="${BASH_REMATCH[2]}"
V_PATCH="${BASH_REMATCH[3]}"
V_SUFFIX="${BASH_REMATCH[4]:-}"

# Strip leading zeros so the arithmetic below never reads a field as octal.
V_MAJOR=$((10#$V_MAJOR))
V_MINOR=$((10#$V_MINOR))
V_PATCH=$((10#$V_PATCH))

# Resolve the suffix to its ordinal. An unrecognised suffix stops here: the
# alternative is a guessed ordinal that ships an unpublishable APK.
if [ -z "$V_SUFFIX" ]; then
    V_ORDINAL=99
elif [[ "$V_SUFFIX" =~ ^-(alpha|beta|rc)(\.([0-9]+))?$ ]]; then
    case "${BASH_REMATCH[1]}" in
        alpha) STAGE_BASE=0 ;;
        beta) STAGE_BASE=30 ;;
        rc) STAGE_BASE=60 ;;
    esac
    N_FIELD="${BASH_REMATCH[3]:-}"
    if [ -z "$N_FIELD" ]; then
        # Bare stage, below its own .1.
        ORDINAL_N=0
    else
        # A leading zero is not a valid semver numeric identifier, and 10#01
        # would hand .01 the same ordinal as .1.
        if [ "${#N_FIELD}" -gt 1 ] && [ "${N_FIELD:0:1}" = "0" ]; then
            echo "error: prerelease number in '$VERSION' has a leading zero" >&2
            echo "       ('.$N_FIELD'); semver numeric identifiers must not." >&2
            exit 1
        fi
        ORDINAL_N=$((10#$N_FIELD))
        if [ "$ORDINAL_N" -lt 1 ] || [ "$ORDINAL_N" -gt "$ORDINAL_MAX_N" ]; then
            echo "error: prerelease number in '$VERSION' is out of range" >&2
            echo "       (expected 1 to $ORDINAL_MAX_N, or a bare -alpha/-beta/-rc" >&2
            echo "       for the one that precedes .1)." >&2
            exit 1
        fi
    fi
    V_ORDINAL=$((STAGE_BASE + ORDINAL_N))
else
    echo "error: version '$VERSION' carries a suffix with no versionCode" >&2
    echo "       ordinal: '$V_SUFFIX'. Recognised suffixes are -alpha[.N]," >&2
    echo "       -beta[.N] and -rc[.N] with N from 1 to $ORDINAL_MAX_N." >&2
    exit 1
fi

if [ "$V_MINOR" -gt 999 ] || [ "$V_PATCH" -gt 999 ]; then
    echo "error: version '$VERSION' overflows the versionCode lanes (minor and" >&2
    echo "       patch must each stay under 1000). Widening the packing means" >&2
    echo "       editing scripts/android-version-code.sh — it is the only copy," >&2
    echo "       and every consumer reads it from here." >&2
    exit 1
fi

# Android's versionCode is a signed 32-bit value capped at 2100000000, so the
# major lane runs out at 20. Nowhere near it, but the failure mode past that
# point is a Play Store rejection after a full release build.
if [ "$V_MAJOR" -gt 20 ]; then
    echo "error: version '$VERSION' exceeds Android's maximum versionCode of" >&2
    echo "       2100000000 (major must stay at or below 20)." >&2
    exit 1
fi

echo $(((V_MAJOR * 1000000 + V_MINOR * 1000 + V_PATCH) * 100 + V_ORDINAL))

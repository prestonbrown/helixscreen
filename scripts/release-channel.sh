#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release-channel.sh — Resolve which update channels a tagged release publishes to.
#
# The channel is declared explicitly by the RELEASE_CHANNEL file at the repo
# root, NOT derived from the tag string. Routing is a property of the branch, and
# a version cannot express it:
#
#   - `dev` has no spelling in a version at all.
#   - A plain vX.Y.Z cut from the trunk would land on the stable channel if the
#     tag decided, and the downgrade guard could not refuse it: a higher version
#     is a forward move.
#   - A stable-line candidate (v1.0.1-rc.1) has to be testable on a prerelease
#     channel while still belonging to the stable line.
#
# Declared out-of-band, each line numbers its releases however that line needs
# and no tag can route itself.
#
# Each maintenance line carries its own RELEASE_CHANNEL, so cutting a release
# is just tagging the right branch:
#
#   release/1.0  RELEASE_CHANNEL=stable  -> stable beta
#   release/1.1  RELEASE_CHANNEL=stable  -> stable beta
#   main         RELEASE_CHANNEL=dev     -> dev         (X.Y.0-alpha.N)
#
# The first channel listed is the declared one; the rest follow it.
# scripts/channel-upload-guard.sh fails a release that would move the declared
# channel backward and leaves a following channel off when it is already ahead.
#
# Shared by CI (.github/workflows/release.yml) and local dev releases.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_FILE="${SCRIPT_DIR}/../RELEASE_CHANNEL"

FILE="$DEFAULT_FILE"
FIELD=""
TAG=""

usage() {
    cat <<EOF
Usage: release-channel.sh [--file PATH] [--tag TAG] [--field FIELD]

Resolve the declared release channel and the R2 channels it publishes to.

Options:
  --file PATH    RELEASE_CHANNEL file to read (default: repo root)
  --tag TAG      Git tag to cross-check against the channel (e.g. v1.1.0).
                 A suffixed tag may not publish to stable; a plain tag must.
  --field FIELD  Print just one value: channel | channels | prerelease
  -h, --help     Show this help

With no --field, prints KEY=value lines suitable for \$GITHUB_OUTPUT:

  channel=beta
  channels=beta dev
  prerelease=true

Channel mapping (the declared channel first, then the channels following it):
  stable -> stable beta     (full GitHub release, triggers docs deploy; Beta
                             never trails Stable)
  beta   -> beta dev        (GitHub prerelease; dev tracks the devel line)
  dev    -> dev             (GitHub prerelease; bleeding edge only)

A following channel takes the release only while that moves it forward
(scripts/channel-upload-guard.sh): a stable 1.1.2 reaches a beta manifest at
1.1.1 and skips one already at 1.2.0-beta.1.

Stable does NOT publish to dev. The dev channel follows the devel line, whose
alphas of the next minor outrank every stable release of the current one.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --file)
            FILE="${2:-}"
            [ -n "$FILE" ] || { echo "error: --file requires a path" >&2; exit 2; }
            shift 2
            ;;
        --tag)
            TAG="${2:-}"
            [ -n "$TAG" ] || { echo "error: --tag requires a value" >&2; exit 2; }
            shift 2
            ;;
        --field)
            FIELD="${2:-}"
            [ -n "$FIELD" ] || { echo "error: --field requires a value" >&2; exit 2; }
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$FILE" ]; then
    echo "error: RELEASE_CHANNEL not found at '$FILE'" >&2
    echo "       Every releasable branch must declare its channel. Create the" >&2
    echo "       file containing one of: stable, beta, dev" >&2
    exit 1
fi

# First non-empty, non-comment line; trim surrounding whitespace.
CHANNEL="$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$FILE" \
    | grep -v '^$' | head -n1 || true)"

if [ -z "$CHANNEL" ]; then
    echo "error: '$FILE' declares no channel (file is empty or all comments)" >&2
    exit 1
fi

case "$CHANNEL" in
    stable)
        CHANNELS="stable beta"
        PRERELEASE="false"
        ;;
    beta)
        CHANNELS="beta dev"
        PRERELEASE="true"
        ;;
    dev)
        CHANNELS="dev"
        PRERELEASE="true"
        ;;
    *)
        echo "error: '$FILE' declares unknown channel '$CHANNEL'" >&2
        echo "       Valid channels: stable, beta, dev" >&2
        exit 1
        ;;
esac

# A tag carrying a prerelease suffix must never reach the stable channel. That
# manifest is what the stable fleet installs unattended, so it serves finished
# releases only; a candidate belongs on a prerelease channel, which is where it
# gets tested without shipping itself to everyone.
if [ -n "$TAG" ] && [ "$CHANNEL" = "stable" ]; then
    case "${TAG#v}" in
        *-*)
            echo "error: tag '$TAG' carries a prerelease suffix but '$FILE' declares" >&2
            echo "       channel 'stable'. The stable manifest is installed unattended," >&2
            echo "       so it serves finished releases only; tag stable releases as" >&2
            echo "       plain vX.Y.Z, or publish this candidate from a beta/dev branch." >&2
            exit 1
            ;;
    esac
fi

# The reverse: a plain vX.Y.Z is a finished release and belongs to the stable
# fleet. Tagged while the branch still declares a prerelease channel, it would
# publish as a GitHub prerelease to beta and dev only, and the stable fleet would
# never be offered it. The graduation commit flips RELEASE_CHANNEL to stable.
if [ -n "$TAG" ] && [ "$CHANNEL" != "stable" ]; then
    case "${TAG#v}" in
        *-*) ;;
        *)
            echo "error: tag '$TAG' is a plain release version but '$FILE' declares" >&2
            echo "error: channel '$CHANNEL', so it would publish as a prerelease and never" >&2
            echo "error: reach the stable fleet. Fix: set RELEASE_CHANNEL to stable in the" >&2
            echo "error: release commit. (A plain version is never tagged from the trunk.)" >&2
            exit 1
            ;;
    esac
fi

case "$FIELD" in
    "")
        echo "channel=$CHANNEL"
        echo "channels=$CHANNELS"
        echo "prerelease=$PRERELEASE"
        ;;
    channel)    echo "$CHANNEL" ;;
    channels)   echo "$CHANNELS" ;;
    prerelease) echo "$PRERELEASE" ;;
    *)
        echo "error: unknown field '$FIELD' (want: channel, channels, prerelease)" >&2
        exit 2
        ;;
esac

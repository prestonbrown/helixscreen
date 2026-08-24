#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release-channel.sh — Resolve which update channels a tagged release publishes to.
#
# The channel is declared explicitly by the RELEASE_CHANNEL file at the repo
# root, NOT derived from the tag string. Deriving it from the tag (the old
# `tag contains a hyphen -> prerelease` rule) forced every devel-track build to
# carry a `-devN` suffix, and helix::version::Version discards that suffix
# (include/version.h) — so v1.1.0-dev1 and v1.1.0-dev2 compared EQUAL and the
# in-app updater stopped offering devel builds after the first install.
#
# With the channel declared out-of-band, the devel track can use plain
# monotonically increasing versions (1.1.0, 1.1.1, ...) that the updater
# actually orders, without those tags landing on the stable channel.
#
# Each maintenance line carries its own RELEASE_CHANNEL, so cutting a release
# is just tagging the right branch:
#
#   release/1.0  RELEASE_CHANNEL=stable  -> stable
#   main         RELEASE_CHANNEL=beta    -> beta + dev
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
                 A tag carrying a prerelease suffix may not publish to stable.
  --field FIELD  Print just one value: channel | channels | prerelease
  -h, --help     Show this help

With no --field, prints KEY=value lines suitable for \$GITHUB_OUTPUT:

  channel=beta
  channels=beta dev
  prerelease=true

Channel mapping:
  stable -> stable          (full GitHub release, triggers docs deploy)
  beta   -> beta dev        (GitHub prerelease; dev tracks the devel line)
  dev    -> dev             (GitHub prerelease; bleeding edge only)

Note that stable does NOT publish to dev. The dev channel follows the devel
line alone so its manifest version only ever moves forward; a 1.0.x hotfix
publishing to dev would otherwise roll dev users back from 1.1.x.
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
        CHANNELS="stable"
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

# A tag carrying a prerelease suffix must never reach the stable channel. The
# suffix is invisible to the in-app version comparison, so a stable fleet that
# installed v1.0.1-rc.1 would then refuse the real v1.0.1 as "already up to
# date" — the exact trap this script exists to remove.
if [ -n "$TAG" ] && [ "$CHANNEL" = "stable" ]; then
    case "${TAG#v}" in
        *-*)
            echo "error: tag '$TAG' carries a prerelease suffix but '$FILE' declares" >&2
            echo "       channel 'stable'. Prerelease suffixes are invisible to the" >&2
            echo "       in-app version comparison; tag stable releases as plain" >&2
            echo "       vX.Y.Z, or publish this tag from a beta/dev branch." >&2
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

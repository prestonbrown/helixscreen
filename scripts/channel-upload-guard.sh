#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# channel-upload-guard.sh: decide which channel manifests a release may overwrite.
#
# A channel manifest must only ever move forward. The in-app updater offers an
# update when manifest_version > installed_version, so republishing an OLDER
# version to a channel does not roll anyone back: it strands the channel
# silently. Everyone already on the newer build sees "Already up to date"
# forever, and the workflow reports success. Republishing the SAME version is
# allowed: that is how a partially-failed upload is retried.
#
# Each channel is judged on its own manifest. The release's declared channel
# (RELEASE_CHANNEL) is the one it exists to publish, so moving it backward fails
# the run. Every other channel in the list follows the release only while that
# is a forward step: a stable 1.1.2 also goes to a beta manifest at 1.1.1, but
# is left off a beta manifest that already serves 1.2.0-beta.1, and the stable
# publish goes ahead. scripts/release-channel.sh names which channels a
# declared channel publishes to.
#
# Precedence comes from scripts/version-compare.sh, which implements Semantic
# Versioning 2.0.0 section 11. A shell version sort cannot answer it: `sort -V`
# ranks 1.1.0 BELOW 1.1.0-beta.1, so on a channel that has served a prerelease
# its verdict is inverted.
#
# Shared by .github/workflows/release.yml and .github/workflows/recover-r2.yml.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<EOF
Usage: channel-upload-guard.sh --version VERSION --declared CHANNEL \\
           --channels "CHANNEL..." --base-url URL

Reads <URL><channel>/manifest.json for each channel and prints the channels the
release may publish to as a channels=... line, also appended to \$GITHUB_OUTPUT
when that is set.

  --version   the version being published (1.1.2, 1.2.0-beta.1)
  --declared  the release's declared channel; a backward move there fails
  --channels  every channel the release would publish to
  --base-url  the manifest root, ending in / (\$R2_PUBLIC_URL/\$R2_PREFIX)

Exits 1 when the declared channel would move backward, or a served version
cannot be ordered, unless ALLOW_DOWNGRADE=true. A following channel that is
already ahead is dropped from the list, whatever ALLOW_DOWNGRADE says.
EOF
}

VERSION=""
DECLARED=""
CHANNELS=""
BASE_URL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version | --declared | --channels | --base-url)
            [ $# -ge 2 ] || { echo "error: $1 requires a value" >&2; exit 2; }
            case "$1" in
                --version) VERSION="$2" ;;
                --declared) DECLARED="$2" ;;
                --channels) CHANNELS="$2" ;;
                --base-url) BASE_URL="$2" ;;
            esac
            shift 2
            ;;
        -h | --help)
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

for required in VERSION DECLARED CHANNELS BASE_URL; do
    if [ -z "${!required}" ]; then
        echo "error: missing --$(echo "$required" | tr '[:upper:]_' '[:lower:]-')" >&2
        usage >&2
        exit 2
    fi
done
case " $CHANNELS " in
    *" $DECLARED "*) ;;
    *)
        echo "error: the declared channel '$DECLARED' is not among '$CHANNELS'" >&2
        exit 2
        ;;
esac

FAILED=0
KEEP=()
for CHANNEL in $CHANNELS; do
    MANIFEST_URL="${BASE_URL}${CHANNEL}/manifest.json"
    SERVED=$(curl -fsS --max-time 30 "$MANIFEST_URL" 2>/dev/null | jq -r '.version // empty' || true)

    if [ -z "$SERVED" ]; then
        echo "${CHANNEL}: no manifest served yet -- nothing to downgrade"
        KEEP+=("$CHANNEL")
        continue
    fi

    if [ "$SERVED" = "$VERSION" ]; then
        echo "${CHANNEL}: currently serves ${SERVED} (republishing the same version)"
        KEEP+=("$CHANNEL")
        continue
    fi

    # A pair the comparator cannot rank is a failure, not a pass: it exits
    # non-zero rather than printing an order, so an unparseable manifest
    # version can never read as "forward".
    if ! ORDER=$("$SCRIPT_DIR/version-compare.sh" "$VERSION" "$SERVED"); then
        echo "::error::${CHANNEL}: cannot order ${VERSION} against the served ${SERVED}."
        FAILED=1
        KEEP+=("$CHANNEL")
        continue
    fi

    case "$ORDER" in
        -1)
            if [ "$CHANNEL" = "$DECLARED" ]; then
                echo "::error::${CHANNEL} currently serves ${SERVED}; publishing ${VERSION} would move it BACKWARD."
                echo "::error::Clients on ${SERVED} would never be offered another ${CHANNEL} update."
                FAILED=1
                KEEP+=("$CHANNEL")
            else
                echo "::notice::${CHANNEL} already serves the newer ${SERVED}; ${VERSION} is not published there"
            fi
            ;;
        0)
            echo "${CHANNEL}: ${SERVED} and ${VERSION} rank equal (build metadata only) -- allowed"
            KEEP+=("$CHANNEL")
            ;;
        1)
            echo "${CHANNEL}: ${SERVED} -> ${VERSION} (forward)"
            KEEP+=("$CHANNEL")
            ;;
        *)
            echo "::error::${CHANNEL}: version-compare.sh printed '${ORDER}', which is not -1, 0 or 1."
            FAILED=1
            KEEP+=("$CHANNEL")
            ;;
    esac
done

if [ "$FAILED" -eq 1 ]; then
    if [ "${ALLOW_DOWNGRADE:-}" = "true" ]; then
        echo "::warning::ALLOW_CHANNEL_DOWNGRADE=true -- proceeding with a backward channel move"
    else
        echo "::error::Set the ALLOW_CHANNEL_DOWNGRADE repository variable to 'true' to override."
        exit 1
    fi
fi

echo "channels=${KEEP[*]}"
if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "channels=${KEEP[*]}" >> "$GITHUB_OUTPUT"
fi

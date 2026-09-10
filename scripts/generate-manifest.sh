#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# generate-manifest.sh — Generate manifest.json from a directory of release tarballs.
# Shared by CI (release.yml) and local dev releases (dev-release.sh).

set -euo pipefail

# Default values
VERSION=""
TAG=""
NOTES=""
DIR=""
BASE_URL=""
OUTPUT=""
INCLUDE_ZIP=true

# Platforms whose ALREADY-DEPLOYED clients cannot verify a zip, so the manifest
# must keep pointing them at the tar.gz (prestonbrown/helixscreen#993).
#
# Pre-v0.99.102 in-app updaters verify a download with `unzip -tqq`. BusyBox
# only grew `unzip -t` in 1.32 — the K1 ships 1.31.1 and the AD5M 1.29.3 — and
# the K2's OpenWrt has no unzip binary or applet at all. Those clients reject a
# byte-perfect zip as "Corrupt download" (bundle YDECJ4FZ: a K1 on v0.99.97
# downloaded all 63130591 bytes of helixscreen-k1.zip, then failed verification
# 16 ms later — the tool rejected the invocation, not the archive).
#
# v0.99.102 fixes the verifier, but that fix ships INSIDE the update the broken
# verifier refuses to install. A client-side fix cannot bootstrap itself, so the
# manifest is the only lever that reaches a deployed binary. Serving these
# platforms the tar.gz keeps `gunzip -t` — which works on every BusyBox in the
# fleet — on the verification path.
#
# Retire a platform from this list once telemetry shows its population is on
# v0.99.102+; use --zip-exclude to do that without editing this file.
ZIP_EXCLUDE_PLATFORMS="ad5m ad5x cc1 k1 k2 snapmaker-u1"

usage() {
    cat <<EOF
Usage: generate-manifest.sh --version VERSION --tag TAG --notes NOTES --dir DIR --base-url URL --output FILE [--include-zip]

Generate a manifest.json from release tarballs in DIR.

Options:
  --version VERSION   Version string (e.g., "0.9.5")
  --tag TAG           Git tag (e.g., "v0.9.5")
  --notes NOTES       Release notes text
  --dir DIR           Directory containing helixscreen-{platform}-*.tar.gz files
  --base-url URL      Base URL for download links (e.g., "https://releases.helixscreen.org/dev")
  --output FILE       Output manifest.json path
  --include-zip       Include zip_url/zip_sha256 fields when a .zip is present.
                      ON by default (adoption telemetry 2026-07 shows the
                      pre-v0.99.31 population is ~0.6% and no longer updating).
                      The legacy url/sha256 fields still point at the .tar.gz, so
                      pre-v0.99.31 in-app updaters — which never read zip_url —
                      keep working; v0.99.31+ clients prefer zip_url. Accepted as
                      a no-op for backward compatibility.
  --no-include-zip    Suppress the zip_url/zip_sha256 fields (legacy behavior;
                      only needed to protect a resurgent pre-v0.99.31 fleet).
  --zip-exclude LIST  Space-separated platforms to withhold zip_url from, even
                      when a .zip is present. REPLACES the built-in list
                      ("$ZIP_EXCLUDE_PLATFORMS"),
                      so pass "" to offer zip everywhere. These platforms still
                      get a complete tar.gz asset. Default covers the
                      BusyBox/OpenWrt devices whose deployed pre-v0.99.102
                      updaters reject an intact zip (helixscreen#993); drop a
                      platform once its fleet is on v0.99.102+.
  --help              Show this help message
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)     VERSION="$2";     shift 2 ;;
        --tag)         TAG="$2";         shift 2 ;;
        --notes)       NOTES="$2";       shift 2 ;;
        --dir)         DIR="$2";         shift 2 ;;
        --base-url)    BASE_URL="$2";    shift 2 ;;
        --output)      OUTPUT="$2";      shift 2 ;;
        --include-zip)    INCLUDE_ZIP=true;  shift ;;
        --no-include-zip) INCLUDE_ZIP=false; shift ;;
        --zip-exclude) ZIP_EXCLUDE_PLATFORMS="$2"; shift 2 ;;
        --help)        usage ;;
        *)
            echo "Error: Unknown option $1" >&2
            exit 1
            ;;
    esac
done

# Validate required arguments
missing=()
[[ -z "$VERSION" ]] && missing+=("--version")
[[ -z "$TAG" ]]     && missing+=("--tag")
[[ -z "$DIR" ]]     && missing+=("--dir")
[[ -z "$BASE_URL" ]] && missing+=("--base-url")
[[ -z "$OUTPUT" ]]  && missing+=("--output")

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Error: Missing required arguments: ${missing[*]}" >&2
    exit 1
fi

if [[ ! -d "$DIR" ]]; then
    echo "Error: Directory not found: $DIR" >&2
    exit 1
fi

# Check required tools
if ! command -v jq &>/dev/null; then
    echo "Error: jq not found. Please install it." >&2
    exit 1
fi

# Determine sha256 command
if command -v shasum &>/dev/null; then
    SHA256_CMD="shasum -a 256"
elif command -v sha256sum &>/dev/null; then
    SHA256_CMD="sha256sum"
else
    echo "Error: Neither shasum nor sha256sum found" >&2
    exit 1
fi

# Auto-discover platforms from tarballs in DIR. Filename convention is
# `helixscreen-{platform}-{version}.tar.gz` where {version} starts with 'v'
# or a digit (e.g., v0.99.31, 0.99.31). Auto-discovery keeps this script in
# sync with whatever .github/workflows/release.yml uploads — adding a new
# platform to the release matrix doesn't require editing this file.
FOUND_ANY=false
ASSETS_JSON="{}"
PLATFORMS=()
ZIP_GATED=()
for f in "$DIR"/helixscreen-*-*.tar.gz; do
    [[ -f "$f" ]] || continue
    base=$(basename "$f")
    # Strip leading 'helixscreen-' and trailing '-{version}.tar.gz' to recover
    # the platform key. Both halves carry hyphens (platforms like snapmaker-u1,
    # prerelease versions like 1.1.0-beta.1), so the split anchors on the last
    # '-' that opens a version: an optional 'v' followed by a digit.
    if [[ "$base" =~ ^helixscreen-(.+)-v?([0-9][0-9A-Za-z.+-]*)\.tar\.gz$ ]]; then
        PLATFORMS+=("${BASH_REMATCH[1]}")
    fi
done

for plat in "${PLATFORMS[@]}"; do
    tarball=""
    for f in "$DIR"/helixscreen-"${plat}"-*.tar.gz; do
        if [[ -f "$f" ]]; then
            tarball="$f"
            break
        fi
    done

    if [[ -z "$tarball" ]]; then
        continue
    fi

    FOUND_ANY=true
    filename=$(basename "$tarball")
    sha256=$($SHA256_CMD "$tarball" | awk '{print $1}')
    # `wc -c` is portable across Linux/macOS/BSD (stat(1) flags differ:
    # `-c '%s'` GNU vs `-f '%z'` BSD). The in-app updater reads `size` to
    # compute the staging-directory free-space requirement (1.2× + small
    # buffer); omitting it forces a conservative fixed-size fallback.
    size=$(wc -c < "$tarball" | tr -d ' ')
    url="${BASE_URL}/${filename}"

    ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
        --arg plat "$plat" \
        --arg url "$url" \
        --arg sha256 "$sha256" \
        --argjson size "$size" \
        '.[$plat] = {url: $url, sha256: $sha256, size: $size}')

    # Add the corresponding ZIP as the preferred asset (used by Moonraker
    # type:zip updates and v0.99.31+ in-app updaters). The tar.gz url/sha256
    # above stay as the legacy fallback for pre-v0.99.31 clients. On by default;
    # --no-include-zip restores the old suppression.
    # ...unless this platform's deployed clients can't verify a zip, in which
    # case the tar.gz above is all they get. See ZIP_EXCLUDE_PLATFORMS.
    if [[ "$INCLUDE_ZIP" == true ]]; then
        zipfile="$DIR/helixscreen-${plat}.zip"
        if [[ -f "$zipfile" && " $ZIP_EXCLUDE_PLATFORMS " == *" $plat "* ]]; then
            ZIP_GATED+=("$plat")
        elif [[ -f "$zipfile" ]]; then
            zip_sha256=$($SHA256_CMD "$zipfile" | awk '{print $1}')
            zip_size=$(wc -c < "$zipfile" | tr -d ' ')
            zip_url="${BASE_URL}/helixscreen-${plat}.zip"

            ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
                --arg plat "$plat" \
                --arg zip_url "$zip_url" \
                --arg zip_sha256 "$zip_sha256" \
                --argjson zip_size "$zip_size" \
                '.[$plat] += {zip_url: $zip_url, zip_sha256: $zip_sha256, zip_size: $zip_size}')
        fi
    fi
done

if [[ "$FOUND_ANY" == "false" ]]; then
    echo "Error: No helixscreen-*.tar.gz tarballs found in $DIR" >&2
    exit 1
fi

# Generate timestamp
PUBLISHED_AT=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Build final manifest
jq -n \
    --arg version "$VERSION" \
    --arg tag "$TAG" \
    --arg notes "${NOTES:-}" \
    --arg published_at "$PUBLISHED_AT" \
    --argjson assets "$ASSETS_JSON" \
    '{
        version: $version,
        tag: $tag,
        notes: $notes,
        published_at: $published_at,
        assets: $assets
    }' > "$OUTPUT"

echo "Generated $OUTPUT with platforms: $(echo "$ASSETS_JSON" | jq -r 'keys | join(", ")')"

# Never let a withheld asset pass silently — a gate that hides what it dropped
# reads as "everything shipped".
if [[ ${#ZIP_GATED[@]} -gt 0 ]]; then
    echo "  zip gated off (deployed clients can't verify zip, helixscreen#993): ${ZIP_GATED[*]}"
    echo "  -> these platforms self-update via the tar.gz url instead"
fi

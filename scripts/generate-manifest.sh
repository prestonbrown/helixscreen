#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# generate-manifest.sh: Generate manifest.json from a directory of release zips.
# Shared by CI (release.yml) and local dev releases (dev-release.sh).

set -euo pipefail

# Default values
VERSION=""
TAG=""
NOTES=""
DIR=""
BASE_URL=""
OUTPUT=""

usage() {
    cat <<EOF
Usage: generate-manifest.sh --version VERSION --tag TAG --notes NOTES --dir DIR --base-url URL --output FILE

Generate a manifest.json from the release zips in DIR.

Options:
  --version VERSION   Version string (e.g., "0.9.5")
  --tag TAG           Git tag (e.g., "v0.9.5")
  --notes NOTES       Release notes text
  --dir DIR           Directory containing helixscreen-{platform}.zip files
  --base-url URL      Base URL for download links (e.g., "https://releases.helixscreen.org/dev")
  --output FILE       Output manifest.json path
  --help              Show this help message

Each platform entry carries the zip twice: as zip_url/zip_sha256/zip_size,
which every in-app updater since v0.99.31 prefers, and as url/sha256/size,
which the installer and older readers of the schema look up the platform by.
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

# Auto-discover platforms from the zips in DIR, so adding a platform to the
# release matrix in .github/workflows/release.yml needs no edit here. The zip
# carries no version (cross.mk's release-* recipes write
# `helixscreen-$(platform).zip`), so everything between the prefix and `.zip`
# is the platform key, hyphens included: there is no version half for a key
# like snapmaker-u1 or x86-desktop to be split on.
FOUND_ANY=false
ASSETS_JSON="{}"

for f in "$DIR"/helixscreen-*.zip; do
    [[ -f "$f" ]] || continue
    base=$(basename "$f")
    # ESP32 firmware zips are flashed over USB, not installed by an updater, so
    # they are not platforms; they are listed separately below.
    [[ "$base" == helixscreen-esp32-* ]] && continue
    [[ "$base" =~ ^helixscreen-(.+)\.zip$ ]] || continue
    plat="${BASH_REMATCH[1]}"

    sha256=$($SHA256_CMD "$f" | awk '{print $1}')
    # `wc -c` is portable across Linux/macOS/BSD (stat(1) flags differ:
    # `-c '%s'` GNU vs `-f '%z'` BSD). The in-app updater reads the size to
    # compute the staging-directory free-space requirement (1.2x + small
    # buffer); omitting it forces a conservative fixed-size fallback.
    size=$(wc -c < "$f" | tr -d ' ')
    url="${BASE_URL}/${base}"

    ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
        --arg plat "$plat" \
        --arg url "$url" \
        --arg sha256 "$sha256" \
        --argjson size "$size" \
        '.[$plat] = {url: $url, sha256: $sha256, size: $size,
                     zip_url: $url, zip_sha256: $sha256, zip_size: $size}')
    FOUND_ANY=true
done

# ESP32 firmware zips (`helixscreen-esp32-<board>-v<version>.zip`) are listed
# under an `esp32-<board>` key with only url/sha256/size, after every platform
# so existing keys keep their order. They carry no zip_* fields: nothing
# self-installs them. An absent zip (a release line with the ESP32 build off)
# leaves the key out entirely.
for f in "$DIR"/helixscreen-esp32-*-v*.zip; do
    [[ -f "$f" ]] || continue
    base=$(basename "$f")
    [[ "$base" =~ ^helixscreen-(esp32-.+)-v([0-9][0-9A-Za-z.+-]*)\.zip$ ]] || continue
    # A zip left over from another release must not be listed under this one.
    if [[ "${BASH_REMATCH[2]}" != "${VERSION#v}" ]]; then
        echo "Skipping $base: not version $VERSION" >&2
        continue
    fi
    ASSETS_JSON=$(echo "$ASSETS_JSON" | jq \
        --arg key "${BASH_REMATCH[1]}" \
        --arg url "${BASE_URL}/${base}" \
        --arg sha256 "$($SHA256_CMD "$f" | awk '{print $1}')" \
        --argjson size "$(wc -c < "$f" | tr -d ' ')" \
        '.[$key] = {url: $url, sha256: $sha256, size: $size}')
done

if [[ "$FOUND_ANY" == "false" ]]; then
    echo "Error: No helixscreen-*.zip assets found in $DIR" >&2
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

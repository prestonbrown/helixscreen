#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Default values
VERSION=""
PLATFORM="all"
DRY_RUN=false
R2_BUCKET_NAME="${R2_BUCKET_NAME:-helixscreen-releases}"
CHANNEL="dev"

# Show usage
usage() {
    cat <<EOF
Usage: dev-release.sh [--version VERSION] [--platform PLATFORM] [--channel CHANNEL] [--dry-run] [--help]

Options:
  --version VERSION   Version string (default: VERSION.txt + -dev.TIMESTAMP)
  --platform PLATFORM Platform to upload (pi, ad5m, cc1, k1, or "all") (default: all available)
  --channel CHANNEL   Channel prefix in bucket (default: dev)
  --dry-run           Show what would be uploaded without actually uploading
  --help              Show this help message

Environment:
  R2_ACCOUNT_ID       Cloudflare account ID (required)
  R2_ACCESS_KEY_ID    R2 API token key (required)
  R2_SECRET_ACCESS_KEY R2 API token secret (required)
  R2_PUBLIC_URL       Public base URL, e.g. https://releases.helixscreen.org (required)
  R2_BUCKET_NAME      Bucket name (default: helixscreen-releases)
EOF
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            VERSION="$2"
            shift 2
            ;;
        --platform)
            PLATFORM="$2"
            shift 2
            ;;
        --channel)
            CHANNEL="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --help)
            usage
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}" >&2
            usage
            ;;
    esac
done

# A local dev build must never overwrite a user-facing channel manifest. Both
# stable and beta are served to real fleets, and the in-app updater only ever
# moves forward -- a hand-built manifest landing there strands every client on
# whatever version it declares.
case "$CHANNEL" in
    stable|beta)
        echo -e "${RED}Error: refusing to publish a dev build to the '${CHANNEL}' channel.${NC}" >&2
        echo "       stable and beta are published by .github/workflows/release.yml from a" >&2
        echo "       tagged commit, so the version and artifacts are reproducible." >&2
        echo "       Use --channel dev, or a scratch prefix." >&2
        exit 1
        ;;
esac

# Check required tools
if ! command -v aws &> /dev/null; then
    echo -e "${RED}Error: aws CLI not found. Please install it first.${NC}" >&2
    exit 1
fi

if ! command -v jq &> /dev/null; then
    echo -e "${RED}Error: jq not found. Please install it first.${NC}" >&2
    exit 1
fi

# Check required environment variables
if [[ -z "${R2_ACCOUNT_ID:-}" ]]; then
    echo -e "${RED}Error: R2_ACCOUNT_ID environment variable not set${NC}" >&2
    exit 1
fi

if [[ -z "${R2_ACCESS_KEY_ID:-}" ]]; then
    echo -e "${RED}Error: R2_ACCESS_KEY_ID environment variable not set${NC}" >&2
    exit 1
fi

if [[ -z "${R2_SECRET_ACCESS_KEY:-}" ]]; then
    echo -e "${RED}Error: R2_SECRET_ACCESS_KEY environment variable not set${NC}" >&2
    exit 1
fi

if [[ -z "${R2_PUBLIC_URL:-}" ]]; then
    echo -e "${RED}Error: R2_PUBLIC_URL environment variable not set${NC}" >&2
    exit 1
fi

# Find git repo root
REPO_ROOT=$(git rev-parse --show-toplevel)
if [[ ! -f "$REPO_ROOT/VERSION.txt" ]]; then
    echo -e "${RED}Error: VERSION.txt not found in repo root${NC}" >&2
    exit 1
fi

# Determine version
if [[ -z "$VERSION" ]]; then
    BASE_VERSION=$(cat "$REPO_ROOT/VERSION.txt" | tr -d '\n')
    TIMESTAMP=$(date +%Y%m%d%H%M%S)
    VERSION="${BASE_VERSION}-dev.${TIMESTAMP}"
fi

echo -e "${GREEN}Version: $VERSION${NC}"

# Find build artifacts
BUILD_DIR="$REPO_ROOT/build/release"
if [[ ! -d "$BUILD_DIR" ]]; then
    echo -e "${RED}Error: Build directory not found: $BUILD_DIR${NC}" >&2
    exit 1
fi

# Determine which platforms to process
declare -a PLATFORMS=()
if [[ "$PLATFORM" == "all" ]]; then
    for tarball in "$BUILD_DIR"/helixscreen-*-*.tar.gz; do
        if [[ -f "$tarball" ]]; then
            filename=$(basename "$tarball")
            # Extract platform from filename: helixscreen-{platform}-*.tar.gz
            plat=$(echo "$filename" | sed -E 's/helixscreen-([^-]+)-.*/\1/')
            PLATFORMS+=("$plat")
        fi
    done

    if [[ ${#PLATFORMS[@]} -eq 0 ]]; then
        echo -e "${RED}Error: No tarballs found in $BUILD_DIR${NC}" >&2
        exit 1
    fi

    echo -e "${GREEN}Found platforms: ${PLATFORMS[*]}${NC}"
else
    # Validate platform
    if [[ ! "$PLATFORM" =~ ^(pi|pi32|ad5m|cc1|k1)$ ]]; then
        echo -e "${RED}Error: Invalid platform '$PLATFORM'. Must be pi, pi32, ad5m, cc1, k1, or all${NC}" >&2
        exit 1
    fi
    PLATFORMS=("$PLATFORM")
fi

# Rename tarballs with version
for plat in "${PLATFORMS[@]}"; do
    for tarball in "$BUILD_DIR"/helixscreen-"${plat}"-*.tar.gz; do
        if [[ -f "$tarball" ]]; then
            new_name="$BUILD_DIR/helixscreen-${plat}-v${VERSION}.tar.gz"
            if [[ "$tarball" != "$new_name" ]]; then
                mv "$tarball" "$new_name"
                echo -e "${YELLOW}Renamed $(basename "$tarball") -> $(basename "$new_name")${NC}"
            fi
            break
        fi
    done
done

# Generate manifest using shared script
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANIFEST_FILE=$(mktemp)

"$SCRIPT_DIR/generate-manifest.sh" \
    --version "$VERSION" \
    --tag "v${VERSION}" \
    --notes "Dev build from $(git rev-parse --short HEAD)" \
    --dir "$BUILD_DIR" \
    --base-url "${R2_PUBLIC_URL}/releases/v${VERSION}" \
    --output "$MANIFEST_FILE"

MANIFEST=$(cat "$MANIFEST_FILE")
rm -f "$MANIFEST_FILE"

echo ""
echo -e "${GREEN}Generated manifest:${NC}"
echo "$MANIFEST" | jq .

# Set up AWS credentials for R2
export AWS_ACCESS_KEY_ID="$R2_ACCESS_KEY_ID"
export AWS_SECRET_ACCESS_KEY="$R2_SECRET_ACCESS_KEY"
R2_ENDPOINT="https://${R2_ACCOUNT_ID}.r2.cloudflarestorage.com"

if [[ "$DRY_RUN" == "true" ]]; then
    echo ""
    echo -e "${YELLOW}DRY RUN - Would execute the following commands:${NC}"
    echo ""

    for tarball in "$BUILD_DIR"/helixscreen-*-*.tar.gz; do
        if [[ -f "$tarball" ]]; then
            filename=$(basename "$tarball")
            echo "aws s3 cp \"$tarball\" \"s3://${R2_BUCKET_NAME}/releases/v${VERSION}/${filename}\" --endpoint-url \"$R2_ENDPOINT\""
        fi
    done

    echo ""
    echo "# Upload manifest.json"
    echo "echo '<manifest>' | aws s3 cp - \"s3://${R2_BUCKET_NAME}/${CHANNEL}/manifest.json\" --endpoint-url \"$R2_ENDPOINT\" --content-type \"application/json\""

    echo ""
    echo "# Upload install.sh to bucket root"
    echo "aws s3 cp \"$REPO_ROOT/scripts/install.sh\" \"s3://${R2_BUCKET_NAME}/install.sh\" --endpoint-url \"$R2_ENDPOINT\" --content-type \"text/x-shellscript\""

    echo ""
    echo -e "${GREEN}Manifest would be available at: ${R2_PUBLIC_URL}/${CHANNEL}/manifest.json${NC}"
else
    echo ""
    echo -e "${GREEN}Uploading tarballs to releases/v${VERSION}/...${NC}"

    for tarball in "$BUILD_DIR"/helixscreen-*-*.tar.gz; do
        if [[ -f "$tarball" ]]; then
            filename=$(basename "$tarball")
            echo -e "${YELLOW}Uploading releases/v${VERSION}/${filename}...${NC}"
            aws s3 cp "$tarball" "s3://${R2_BUCKET_NAME}/releases/v${VERSION}/${filename}" \
                --endpoint-url "$R2_ENDPOINT"
            echo -e "${GREEN}  Uploaded successfully${NC}"
        fi
    done

    echo ""
    echo -e "${GREEN}Uploading ${CHANNEL}/manifest.json...${NC}"
    echo "$MANIFEST" | aws s3 cp - "s3://${R2_BUCKET_NAME}/${CHANNEL}/manifest.json" \
        --endpoint-url "$R2_ENDPOINT" \
        --content-type "application/json"

    echo ""
    echo -e "${GREEN}Uploading install.sh to bucket root...${NC}"
    aws s3 cp "$REPO_ROOT/scripts/install.sh" "s3://${R2_BUCKET_NAME}/install.sh" \
        --endpoint-url "$R2_ENDPOINT" \
        --content-type "text/x-shellscript"

    echo ""
    echo -e "${GREEN}✓ Upload complete!${NC}"
    echo -e "${GREEN}Manifest available at: ${R2_PUBLIC_URL}/${CHANNEL}/manifest.json${NC}"
fi

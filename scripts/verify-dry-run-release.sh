#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# verify-dry-run-release.sh - Check what a release.yml dry run published.
#
# A dry run publishes twice: the would-be GitHub release assets as a workflow
# artifact, and the R2 upload under dry-run/<run_id>/. Given a local copy of
# each, this asserts they agree and that the manifests point only inside the
# dry-run prefix. Every failure is reported before exiting non-zero.

set -euo pipefail

usage() {
    cat <<EOF
Usage: verify-dry-run-release.sh --artifact DIR --r2 DIR --base-url URL
                                 --version VERSION --platforms "P..." --channels "C..."

  --artifact DIR     The downloaded dry-run-release-assets artifact
  --r2 DIR           A local copy of the R2 dry-run/<run_id>/ prefix
  --base-url URL     The public URL of that prefix (\${R2_PUBLIC_URL}/dry-run/<run_id>/)
  --version VERSION  The version the manifests must carry (no leading v)
  --platforms LIST   Platforms whose tarball and zip must be present
  --channels LIST    Channels whose manifest must be present
EOF
}

ARTIFACT="" R2="" BASE_URL="" VERSION="" PLATFORMS="" CHANNELS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --artifact)  ARTIFACT="${2:-}"; shift 2 ;;
        --r2)        R2="${2:-}"; shift 2 ;;
        --base-url)  BASE_URL="${2:-}"; shift 2 ;;
        --version)   VERSION="${2:-}"; shift 2 ;;
        --platforms) PLATFORMS="${2:-}"; shift 2 ;;
        --channels)  CHANNELS="${2:-}"; shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
for v in ARTIFACT R2 BASE_URL VERSION PLATFORMS CHANNELS; do
    [ -n "${!v}" ] || { echo "error: --$(echo "$v" | tr 'A-Z_' 'a-z-') is required" >&2; exit 2; }
done

FAILED=0
fail() { echo "FAIL: $*"; FAILED=1; }
ok() { echo "  ok: $*"; }
# expect_file PATH WHAT
expect_file() {
    if [ -f "$1" ]; then ok "$2"; else fail "$2 missing: $1"; fi
}

# install.sh
if [ ! -f "$ARTIFACT/install.sh" ]; then
    fail "install.sh missing from the release artifact"
elif [ ! -f "$R2/install.sh" ]; then
    fail "install.sh missing from R2"
elif ! cmp -s "$ARTIFACT/install.sh" "$R2/install.sh"; then
    fail "install.sh differs between the release artifact and R2"
else
    ok "install.sh identical in both"
fi

# Per-platform assets, in both places
RELEASE_DIR="$R2/releases/v$VERSION"
for p in $PLATFORMS; do
    for dir in "$ARTIFACT" "$RELEASE_DIR"; do
        # The v anchors the version so pi does not match pi32's tarball.
        tarballs=("$dir"/helixscreen-"$p"-v[0-9]*.tar.gz)
        expect_file "${tarballs[0]}" "$p tarball in $dir"
        expect_file "$dir/helixscreen-$p.zip" "$p zip in $dir"
    done
done

# Manifests: every referenced file lies inside the prefix and hashes as stated
for ch in $CHANNELS; do
    manifest="$R2/$ch/manifest.json"
    if [ ! -f "$manifest" ]; then
        fail "$ch/manifest.json missing from R2"
        continue
    fi
    # Its signing key is the run's own and gone with it; that the signature is
    # there, beside its manifest, is what a dry run can check.
    [ -s "$manifest.minisig" ] || fail "$ch/manifest.json.minisig missing from R2"
    served=$(jq -r '.version // empty' "$manifest")
    [ "$served" = "$VERSION" ] || fail "$ch manifest carries version '$served', expected '$VERSION'"
    refs=$(jq -r '.assets[] | ([.url, .sha256], [.zip_url, .zip_sha256]) | select(.[0] != null) | @tsv' "$manifest")
    [ -n "$refs" ] || fail "$ch manifest references no assets"
    while IFS=$'\t' read -r url sha; do
        [ -n "$url" ] || continue
        case "$url" in
            "$BASE_URL"*) ;;
            *) fail "$ch manifest points outside the dry-run prefix: $url"; continue ;;
        esac
        file="$R2/${url#"$BASE_URL"}"
        if [ ! -f "$file" ]; then
            fail "$ch manifest references $url, not present in R2"
        elif [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sha" ]; then
            fail "$ch manifest sha256 for $url does not match the uploaded file"
        else
            ok "$ch -> ${url#"$BASE_URL"}"
        fi
    done <<<"$refs"
done

if [ "$FAILED" -ne 0 ]; then
    echo "Dry-run verification FAILED"
    exit 1
fi
echo "Dry-run verification passed"

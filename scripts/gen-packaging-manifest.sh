#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# gen-packaging-manifest.sh — single source of truth for which source-tree
# directories ship in a HelixScreen package.
#
# Walks the canonical data roots (ui_xml/, assets/, config/) and emits one
# sorted, relative directory path per line. Consumed by:
#
#   - android/app/build.gradle:  writes output to src/main/assets/MANIFEST.txt
#     so android_asset_extractor.cpp can iterate at install time (the Android
#     AAssetDir API can only list files, not subdirectories — the extractor
#     must know the dir tree ahead of time).
#
#   - mk/cross.mk:  (optional) can source this instead of the hand-maintained
#     RELEASE_ASSETS whitelist.
#
#   - tests/unit/test_packaging_manifest.cpp:  regression test asserts this
#     output matches a filesystem walk so new subdirs can't silently drop
#     out of packages.
#
# Usage:   scripts/gen-packaging-manifest.sh [PROJECT_ROOT]
# Default PROJECT_ROOT is the parent of this script's directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${1:-$(cd "$SCRIPT_DIR/.." && pwd)}"

if [ ! -d "$PROJECT_ROOT/ui_xml" ] || [ ! -d "$PROJECT_ROOT/assets" ]; then
    echo "gen-packaging-manifest.sh: '$PROJECT_ROOT' does not look like a HelixScreen project root" >&2
    exit 1
fi

cd "$PROJECT_ROOT"

# Roots to walk. config/ is included so tools can enumerate the writable-state
# scaffolding dirs (printer_database.d/, themes/, custom_images/) the extractor
# places on first run.
ROOTS=(ui_xml assets config)

# Emit each root and every subdirectory beneath it, relative to PROJECT_ROOT.
# LC_ALL=C gives a stable, locale-independent sort.
#
# Hidden dirs (name starting with '.') are skipped — they hold dev-only state
# that should never ship (.git, .DS_Store sidecars, etc.).
# `find -name '.*' -prune` prunes the directory AND its subtree.
for root in "${ROOTS[@]}"; do
    if [ -d "$root" ]; then
        find "$root" -type d \( -name '.*' -prune \) -o -type d -print
    fi
done | LC_ALL=C sort

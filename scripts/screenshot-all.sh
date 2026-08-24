#!/bin/bash

# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later

# Capture all screenshots for the documentation gallery
# Uses the default HelixScreen theme in dark mode for consistency

set -e
cd "$(dirname "$0")/.."

# Configuration
THEME="${HELIX_THEME:-helixscreen}"
MODE="--dark"
FLAGS="--test"
OUTPUT_DIR="docs/images/user"

mkdir -p "$OUTPUT_DIR"

echo "Capturing screenshots with HELIX_THEME=$THEME $MODE $FLAGS"
echo "Output: $OUTPUT_DIR/"
echo ""

# Define all screens: "output-name:token". The token resolves to a helix-screen ctl
# navigation recipe (see scripts/screenshot-recipes.sh) — screenshot.sh boots a
# fresh instance, drives it to that screen, and captures it.
PANELS=(
    # Base panels
    "home:home"
    "controls:controls"
    "filament:filament"
    "settings:settings"
    "advanced:advanced"
    "print-select:print-select"

    # Control overlays
    "controls-motion:motion"
    "controls-temperature:nozzle-temp"
    "controls-extrusion:extrusion"
    "controls-fan:fan"
    "controls-bed-mesh:bed-mesh"
    "controls-pid:pid"

    # Settings overlays
    "settings-display:display"
    "settings-sensors:sensors"
    "settings-theme:theme"
    "settings-network:network"
    "settings-hardware:hardware-health"
    "settings-fans:fan-settings"
    "settings-barcode-scanner:barcode-scanner"
    "settings-label-printer:label-printer"
    "settings-security:security"
    "screen-lock:lock-screen"

    # Advanced overlays
    "advanced-shaper:input-shaper"
    "advanced-screws:screws"
    "advanced-zoffset:zoffset"
    "advanced-spoolman:spoolman"
    "advanced-history:history-dashboard"
    "advanced-macros:macros"
    "advanced-console:console"

    # Print overlays
    "print-status:print-status"
    "print-tune:print-tune"

    # Sample-data modals
    "preflight-check:preflight-check"
    "color-mismatch:color-mismatch"
    "runout-modal:runout-modal"
)

# Track results
SUCCESS=0
FAILED=0
FAILED_LIST=""

for item in "${PANELS[@]}"; do
    name="${item%%:*}"
    panel="${item##*:}"

    echo -n "  $name... "

    if HELIX_THEME="$THEME" ./scripts/screenshot.sh helix-screen "$name" "$panel" $MODE $FLAGS >/dev/null 2>&1; then
        mv "/tmp/ui-screenshot-$name.png" "$OUTPUT_DIR/$name.png"
        echo "ok"
        ((SUCCESS++))
    else
        echo "FAILED"
        ((FAILED++))
        FAILED_LIST="$FAILED_LIST $name"
    fi
done

echo ""
echo "Done: $SUCCESS captured, $FAILED failed"
if [ -n "$FAILED_LIST" ]; then
    echo "Failed:$FAILED_LIST"
fi

# List output
echo ""
echo "Screenshots in $OUTPUT_DIR/:"
ls -1 "$OUTPUT_DIR"/*.png 2>/dev/null | wc -l | xargs echo "  Total:"

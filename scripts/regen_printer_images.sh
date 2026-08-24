#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pre-render printer images to LVGL binary format at multiple sizes
#
# Creates optimized .bin versions of printer images for instant display.
# Two sizes are generated:
#   - 300px: For wizard/home on medium-large displays (800x480+)
#   - 150px: For small displays (480x320) or compact views
#
# Original PNGs are kept as fallbacks for unknown display sizes.
#
# Usage:
#   ./scripts/regen_printer_images.sh                    # Generate all
#   ./scripts/regen_printer_images.sh --clean            # Remove generated files
#   ./scripts/regen_printer_images.sh --list             # List what would be generated
#
# Output: build/assets/images/printers/prerendered/*.bin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source shared library
source "$SCRIPT_DIR/lib/lvgl_image_lib.sh"

# Configuration
OUTPUT_DIR="${OUTPUT_DIR:-$LVGL_PROJECT_DIR/build/assets/images/printers/prerendered}"
SOURCE_DIR="$LVGL_PROJECT_DIR/assets/images/printers"
TARGET_SIZES=(300 150)

# Use colors from library
RED="$LVGL_RED"
GREEN="$LVGL_GREEN"
YELLOW="$LVGL_YELLOW"
CYAN="$LVGL_CYAN"
NC="$LVGL_NC"

clean_prerendered() {
    echo -e "${YELLOW}Cleaning pre-rendered printer images...${NC}"
    if [ -d "$OUTPUT_DIR" ]; then
        rm -rf "$OUTPUT_DIR"
        echo -e "${GREEN}✓ Cleaned $OUTPUT_DIR${NC}"
    else
        echo -e "${YELLOW}Nothing to clean (directory doesn't exist)${NC}"
    fi
}

list_targets() {
    echo -e "${CYAN}Printer images to pre-render:${NC}"
    echo ""

    local count=0
    for png in "$SOURCE_DIR"/*.png; do
        [ -f "$png" ] || continue
        local basename
        basename=$(basename "$png" .png)

        echo -e "  ${GREEN}$basename${NC}"
        for size in "${TARGET_SIZES[@]}"; do
            echo "    → ${basename}-${size}.bin"
        done
        count=$((count + 1))
    done
    echo ""
    echo "Total: $count printers × ${#TARGET_SIZES[@]} sizes = $((count * ${#TARGET_SIZES[@]})) files"
}

render_all() {
    local success=0
    local failed=0
    mkdir -p "$OUTPUT_DIR"

    echo -e "${CYAN}Pre-rendering printer images...${NC}"
    echo "  Source: $SOURCE_DIR"
    echo "  Output: $OUTPUT_DIR"
    echo "  Sizes: ${TARGET_SIZES[*]}px"
    echo ""

    for png in "$SOURCE_DIR"/*.png; do
        [ -f "$png" ] || continue
        local basename
        basename=$(basename "$png" .png)

        echo -ne "  $basename: "

        local img_success=true
        for size in "${TARGET_SIZES[@]}"; do
            local output_name="${basename}-${size}"
            if lvgl_render_image "$png" "$OUTPUT_DIR" "$output_name" "$size"; then
                echo -ne "${GREEN}${size}✓${NC} "
            else
                echo -ne "${RED}${size}✗${NC} "
                img_success=false
            fi
        done
        echo ""

        if [ "$img_success" = true ]; then
            success=$((success + 1))
        else
            failed=$((failed + 1))
        fi
    done

    echo ""
    echo -e "${GREEN}✓ Rendered $success printers${NC}"
    [ $failed -gt 0 ] && echo -e "${RED}  Failed: $failed${NC}"

    # Show total size
    if [ -d "$OUTPUT_DIR" ]; then
        local total_size
        total_size=$(du -sh "$OUTPUT_DIR" | cut -f1)
        echo -e "  Total size: ${CYAN}$total_size${NC}"
    fi
}

# Main
case "${1:-}" in
    --clean)
        clean_prerendered
        ;;
    --list)
        list_targets
        ;;
    --help|-h)
        head -20 "$0" | tail -n +2 | sed 's/^# //' | sed 's/^#//'
        ;;
    *)
        lvgl_check_deps
        render_all
        ;;
esac

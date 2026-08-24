#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pre-render SPLASH SCREEN images to LVGL binary format
#
# This script converts the splash screen logo to EXACT-SIZE .bin files for
# each supported screen resolution. Since we know the exact display size at
# build time, we pre-render at pixel-perfect sizes - NO runtime scaling needed.
#
# NOTE: This is for SPLASH SCREEN only, not thumbnails. Thumbnails use the
# ThumbnailCache/ThumbnailProcessor system which handles dynamic scaling
# since thumbnail source sizes vary.
#
# Performance Impact (AD5M benchmark):
#   With PNG decoding:    ~2 FPS during splash
#   With pre-rendered:    ~116 FPS (instant display)
#
# Usage:
#   ./scripts/regen_images.sh                    # Generate to build/assets/...
#   OUTPUT_DIR=/custom/path ./scripts/regen_images.sh  # Custom output
#   ./scripts/regen_images.sh --clean            # Remove generated files
#   ./scripts/regen_images.sh --list             # List what would be generated
#
# Output files go to build/assets/images/prerendered/ by default.
# They are build artifacts and should NOT be committed to the repository.
# Use 'make gen-images' to generate as part of the build process.
#
# See: docs/devel/PRE_RENDERED_IMAGES.md for full documentation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source shared library
source "$SCRIPT_DIR/lib/lvgl_image_lib.sh"

# Configuration
OUTPUT_DIR="${OUTPUT_DIR:-$LVGL_PROJECT_DIR/build/assets/images/prerendered}"

# Use colors from library
RED="$LVGL_RED"
GREEN="$LVGL_GREEN"
YELLOW="$LVGL_YELLOW"
CYAN="$LVGL_CYAN"
NC="$LVGL_NC"

# Screen size definitions matching ui_theme.h
# Format: "name:width:height:logo_size"
# logo_size calculated as: width * 0.5 (if height < 500) or width * 0.6 (if height >= 500)
#
# Filter with TARGET_SIZES env var (comma-separated): "small" or "tiny,small,medium,large"
# Examples:
#   TARGET_SIZES=small ./scripts/regen_images.sh        # Only 800x480 (AD5M)
#   TARGET_SIZES=tiny,medium ./scripts/regen_images.sh  # Specific sizes
#   ./scripts/regen_images.sh                           # All sizes (Pi, generic)
# SPLASH SCREEN sizes - EXACT pixel sizes matching splash_screen.cpp logic:
#   if (screen_height < 500) target = screen_width / 2;  // 50%
#   else                     target = (screen_width * 3) / 5;  // 60%
#
# These are the EXACT sizes the splash logo renders at - NO runtime scaling needed!
ALL_SCREEN_SIZES=(
    "tiny:480:320:240"      # 480 * 0.5 = 240 (height 320 < 500)
    "small:800:480:400"     # 800 * 0.5 = 400 (height 480 < 500) - AD5M
    "medium:1024:600:614"   # 1024 * 0.6 = 614 (height 600 >= 500)
    "large:1280:720:768"    # 1280 * 0.6 = 768 (height 720 >= 500)
)

# Filter screen sizes based on TARGET_SIZES environment variable
filter_screen_sizes() {
    if [ -z "${TARGET_SIZES:-}" ]; then
        # No filter - use all sizes
        SCREEN_SIZES=("${ALL_SCREEN_SIZES[@]}")
        return
    fi

    SCREEN_SIZES=()
    IFS=',' read -ra REQUESTED <<< "$TARGET_SIZES"
    for size_spec in "${ALL_SCREEN_SIZES[@]}"; do
        IFS=':' read -r name _ _ _ <<< "$size_spec"
        for requested in "${REQUESTED[@]}"; do
            if [ "$name" = "$requested" ]; then
                SCREEN_SIZES+=("$size_spec")
            fi
        done
    done

    if [ ${#SCREEN_SIZES[@]} -eq 0 ]; then
        echo -e "${RED}Error: No valid sizes in TARGET_SIZES=$TARGET_SIZES${NC}"
        echo "Valid sizes: tiny, small, medium, large"
        exit 1
    fi
}

# Call the filter function
filter_screen_sizes

# Source images to pre-render
# Format: "source_path:output_prefix:description"
IMAGES_TO_RENDER=(
    "assets/images/helixscreen-logo.png:splash-logo:Splash screen logo"
)

# Fixed-size images (not scaled per screen size)
# Format: "source_path:output_name:size:description"
FIXED_IMAGES=(
    "assets/images/helixscreen-logo.png:about-logo:120:About screen logo"
)

print_header() {
    echo -e "${CYAN}╔════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║         HelixScreen Image Pre-Rendering System             ║${NC}"
    echo -e "${CYAN}╚════════════════════════════════════════════════════════════╝${NC}"
}

# check_dependencies - delegated to shared library's lvgl_check_deps()

ensure_output_dir() {
    mkdir -p "$OUTPUT_DIR"
}

clean_prerendered() {
    echo -e "${YELLOW}Cleaning pre-rendered images...${NC}"
    if [ -d "$OUTPUT_DIR" ]; then
        rm -rf "$OUTPUT_DIR"/*.bin 2>/dev/null || true
        echo -e "${GREEN}✓ Cleaned $OUTPUT_DIR${NC}"
    else
        echo -e "${YELLOW}Nothing to clean (directory doesn't exist)${NC}"
    fi
}

list_targets() {
    echo -e "${CYAN}Pre-render targets:${NC}"
    echo ""
    for image_spec in "${IMAGES_TO_RENDER[@]}"; do
        IFS=':' read -r source_path output_prefix description <<< "$image_spec"
        echo -e "  ${GREEN}$description${NC} ($source_path)"
        for screen_spec in "${SCREEN_SIZES[@]}"; do
            IFS=':' read -r name width height logo_size <<< "$screen_spec"
            output_file="$OUTPUT_DIR/${output_prefix}-${name}.bin"
            echo "    → ${output_prefix}-${name}.bin (${logo_size}x${logo_size})"
        done
        echo ""
    done
}

render_image() {
    local source_path="$1"
    local output_prefix="$2"
    local screen_name="$3"
    local target_size="$4"

    local full_source="$LVGL_PROJECT_DIR/$source_path"
    local output_name="${output_prefix}-${screen_name}"
    local output_file="$OUTPUT_DIR/${output_name}.bin"

    if [ ! -f "$full_source" ]; then
        echo -e "${RED}    ✗ Source not found: $source_path${NC}"
        return 1
    fi

    echo -ne "    ${screen_name} (${target_size}x${target_size})... "

    if lvgl_render_image "$full_source" "$OUTPUT_DIR" "$output_name" "$target_size"; then
        local size
        size=$(lvgl_file_size "$output_file")
        echo -e "${GREEN}✓${NC} ($size)"
        return 0
    else
        echo -e "${RED}✗ Failed${NC}"
        return 1
    fi
}

render_all() {
    local success=0
    local failed=0

    for image_spec in "${IMAGES_TO_RENDER[@]}"; do
        IFS=':' read -r source_path output_prefix description <<< "$image_spec"
        echo -e "\n${CYAN}Rendering: $description${NC}"
        echo "  Source: $source_path"
        echo ""

        for screen_spec in "${SCREEN_SIZES[@]}"; do
            IFS=':' read -r name width height logo_size <<< "$screen_spec"

            if render_image "$source_path" "$output_prefix" "$name" "$logo_size"; then
                ((success++)) || true
            else
                ((failed++)) || true
            fi
        done
    done

    # Fixed-size images — output to assets/images/ (committed to repo, like gradients)
    local fixed_output="$LVGL_PROJECT_DIR/assets/images"
    for image_spec in "${FIXED_IMAGES[@]}"; do
        IFS=':' read -r source_path output_name target_size description <<< "$image_spec"
        local full_source="$LVGL_PROJECT_DIR/$source_path"
        echo -e "\n${CYAN}Rendering: $description${NC}"
        echo "  Source: $source_path"
        echo ""
        echo -ne "    ${target_size}x${target_size}... "

        if lvgl_render_image "$full_source" "$fixed_output" "$output_name" "$target_size"; then
            local size
            size=$(lvgl_file_size "$fixed_output/${output_name}.bin")
            echo -e "${GREEN}✓${NC} ($size)"
            ((success++)) || true
        else
            echo -e "${RED}✗ Failed${NC}"
            ((failed++)) || true
        fi
    done

    echo ""
    echo -e "${CYAN}════════════════════════════════════════════════════════════${NC}"
    echo -e "  ${GREEN}✓ Generated: $success files${NC}"
    if [ $failed -gt 0 ]; then
        echo -e "  ${RED}✗ Failed: $failed files${NC}"
    fi
    echo -e "${CYAN}════════════════════════════════════════════════════════════${NC}"

    if [ $failed -gt 0 ]; then
        return 1
    fi
    return 0
}

# Main
case "${1:-}" in
    --clean|-c)
        clean_prerendered
        ;;
    --list|-l)
        print_header
        list_targets
        ;;
    --help|-h)
        print_header
        echo ""
        echo "Usage: $0 [OPTIONS]"
        echo ""
        echo "Options:"
        echo "  (none)     Generate pre-rendered images (filtered by TARGET_SIZES)"
        echo "  --clean    Remove generated .bin files"
        echo "  --list     List what would be generated"
        echo "  --help     Show this help message"
        echo ""
        echo "Environment Variables:"
        echo "  OUTPUT_DIR    Output directory (default: build/assets/images/prerendered)"
        echo "  TARGET_SIZES  Comma-separated sizes to generate (default: all)"
        echo "                Values: tiny, small, medium, large"
        echo "                Example: TARGET_SIZES=small (for AD5M fixed 800x480)"
        echo ""
        echo "All Screen Sizes:"
        for screen_spec in "${ALL_SCREEN_SIZES[@]}"; do
            IFS=':' read -r name width height logo_size <<< "$screen_spec"
            echo "  $name: ${width}x${height} → logo ${logo_size}px"
        done
        echo ""
        echo "Platform Shortcuts:"
        echo "  AD5M (fixed 800x480):  TARGET_SIZES=small"
        echo "  Pi (variable):         TARGET_SIZES=  (all sizes)"
        ;;
    *)
        print_header
        lvgl_check_deps
        ensure_output_dir
        render_all
        ;;
esac

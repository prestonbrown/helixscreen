#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared library for LVGL image pre-rendering scripts
#
# Provides common functions for:
#   - Python/Pillow environment setup
#   - LVGLImage.py invocation with error handling
#   - Consistent output formatting
#
# Usage: source scripts/lib/lvgl_image_lib.sh

# Prevent double-sourcing
if [ -n "$_LVGL_IMAGE_LIB_SOURCED" ]; then
    return 0
fi
_LVGL_IMAGE_LIB_SOURCED=1

# ==============================================================================
# Environment Setup
# ==============================================================================

# Find project directory (parent of scripts/)
_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_SCRIPTS_DIR="$(dirname "$_LIB_DIR")"
LVGL_PROJECT_DIR="$(dirname "$_SCRIPTS_DIR")"

# Python and tools
LVGL_IMAGE_PY="$_SCRIPTS_DIR/LVGLImage.py"
LVGL_PYTHON="${PYTHON:-python3}"

# Use virtual environment if available
if [ -f "$LVGL_PROJECT_DIR/.venv/bin/python" ]; then
    LVGL_PYTHON="$LVGL_PROJECT_DIR/.venv/bin/python"
fi

# Colors for output
LVGL_RED='\033[0;31m'
# shellcheck disable=SC2034  # consumed by regen_images.sh / regen_printer_images.sh / regen_placeholder_images.sh
LVGL_GREEN='\033[0;32m'
# shellcheck disable=SC2034  # consumed by regen_images.sh / regen_printer_images.sh / regen_placeholder_images.sh
LVGL_YELLOW='\033[0;33m'
# shellcheck disable=SC2034  # consumed by regen_images.sh / regen_printer_images.sh / regen_placeholder_images.sh
LVGL_CYAN='\033[0;36m'
LVGL_NC='\033[0m'

# ==============================================================================
# Dependency Checking
# ==============================================================================

# Check that LVGLImage.py and PIL are available
# Returns 0 if OK, exits with error message if not
lvgl_check_deps() {
    if [ ! -f "$LVGL_IMAGE_PY" ]; then
        echo -e "${LVGL_RED}Error: LVGLImage.py not found at $LVGL_IMAGE_PY${LVGL_NC}"
        echo "This script requires the LVGL image converter."
        exit 1
    fi

    if ! $LVGL_PYTHON -c "from PIL import Image" 2>/dev/null; then
        echo -e "${LVGL_RED}Error: Python PIL/Pillow not found${LVGL_NC}"
        echo "Install with: pip install Pillow"
        echo "Or: .venv/bin/pip install Pillow"
        exit 1
    fi
}

# ==============================================================================
# Image Rendering
# ==============================================================================

# Render a single image to LVGL .bin format
#
# Arguments:
#   $1 - source_path: Full path to source PNG
#   $2 - output_dir: Directory for output .bin file
#   $3 - output_name: Filename without extension (e.g., "splash-logo-small")
#   $4 - target_size: Target size in pixels (e.g., 300)
#   $5 - color_format: LVGL color format (default: ARGB8888)
#
# Returns: 0 on success, 1 on failure
lvgl_render_image() {
    local source_path="$1"
    local output_dir="$2"
    local output_name="$3"
    local target_size="$4"
    local color_format="${5:-ARGB8888}"

    local error_file="/tmp/lvgl_image_error_$$.txt"

    # Ensure output directory exists
    mkdir -p "$output_dir"

    # Run LVGLImage.py with resize options, capture output
    if $LVGL_PYTHON "$LVGL_IMAGE_PY" \
        --cf "$color_format" \
        --ofmt BIN \
        --compress LZ4 \
        --resize "${target_size}x${target_size}" \
        --resize-fit \
        -o "$output_dir" \
        --name "$output_name" \
        "$source_path" >/dev/null 2>"$error_file"; then
        rm -f "$error_file"
        return 0
    else
        # Show error on failure
        if [ -s "$error_file" ]; then
            echo -e "\n${LVGL_RED}Error converting $(basename "$source_path"):${LVGL_NC}" >&2
            cat "$error_file" >&2
        fi
        rm -f "$error_file"
        return 1
    fi
}

# Get size of a file in human-readable format
lvgl_file_size() {
    local file="$1"
    if [ -f "$file" ]; then
        du -h "$file" | cut -f1
    else
        echo "?"
    fi
}

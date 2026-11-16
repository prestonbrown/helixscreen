#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test that the application shuts down cleanly without crashes.
#
# Catches a double-free crash in shutdown: lv_deinit() handles all LVGL
# resource cleanup internally, so a manual lv_display_delete() or
# lv_group_delete() before it frees those twice.
#
# Usage:
#   ./scripts/test_clean_shutdown.sh          # Run 3 tests
#   ./scripts/test_clean_shutdown.sh 10       # Run 10 tests

set -e

# Number of test iterations (default: 3)
ITERATIONS=${1:-3}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "========================================"
echo "Clean Shutdown Test"
echo "Testing $ITERATIONS iteration(s)..."
echo "========================================"

# Build first
if ! make -j >/dev/null 2>&1; then
    echo -e "${RED}Build failed${NC}"
    exit 1
fi

BINARY="./build/bin/helix-screen"
if [ ! -x "$BINARY" ]; then
    echo -e "${RED}Binary not found: $BINARY${NC}"
    exit 1
fi

FAILURES=0

for i in $(seq 1 $ITERATIONS); do
    echo -n "Test $i/$ITERATIONS: "

    # Run with --test mode and 1 second timeout
    # Capture exit code
    set +e
    $BINARY --test --timeout 1 >/dev/null 2>&1
    EXIT_CODE=$?
    set -e

    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "${GREEN}✓${NC} (exit code 0)"
    else
        echo -e "${RED}✗${NC} (exit code $EXIT_CODE)"
        ((FAILURES++)) || true
    fi
done

echo ""
echo "========================================"
if [ $FAILURES -eq 0 ]; then
    echo -e "${GREEN}All $ITERATIONS shutdown tests passed!${NC}"
    exit 0
else
    echo -e "${RED}$FAILURES of $ITERATIONS tests failed!${NC}"
    echo ""
    echo "This may indicate a double-free crash in shutdown."
    echo "Check display_manager.cpp - ensure lv_display_delete(),"
    echo "lv_group_delete(), and lv_indev_delete() are NOT called manually."
    echo "lv_deinit() handles all LVGL cleanup internally."
    exit 1
fi

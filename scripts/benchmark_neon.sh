#!/bin/bash
# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Benchmark script to compare NEON vs non-NEON rendering performance
# Uses built-in HELIX_BENCHMARK mode for accurate FPS measurement

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

TARGET="${1:-root@192.168.1.67}"
DURATION_MS="${2:-20000}"  # 20 seconds default

echo "========================================"
echo "NEON vs Non-NEON Rendering Benchmark"
echo "========================================"
echo "Target: $TARGET"
echo "Duration: $((DURATION_MS / 1000)) seconds"
echo ""

run_benchmark() {
    local label="$1"
    local binary="$2"

    echo "--- $label ---"
    echo "Deploying..."
    scp -O "$binary" "$TARGET:/opt/helixscreen/helix-screen" 2>/dev/null

    echo "Running benchmark..."
    ssh "$TARGET" "
        cd /opt/helixscreen
        killall helix-screen 2>/dev/null || true
        sleep 1

        # Run with benchmark mode and auto-quit
        HELIX_BENCHMARK=1 HELIX_AUTO_QUIT_MS=$DURATION_MS ./helix-screen --test --skip-splash -v 2>&1 | grep -E '\[Benchmark\]'
    " 2>/dev/null || echo "  (benchmark completed)"

    echo ""
}

# Check for existing builds
NO_NEON_BUILD="$PROJECT_DIR/build/ad5m/bin/helix-screen"

if [[ ! -f "$NO_NEON_BUILD" ]]; then
    echo "ERROR: No AD5M build found at $NO_NEON_BUILD"
    echo "Run 'make ad5m-docker' first"
    exit 1
fi

echo "Using current build (check mk/cross.mk for NEON status)"
run_benchmark "Current Build" "$NO_NEON_BUILD"

echo "========================================"
echo "To compare NEON vs non-NEON:"
echo ""
echo "1. WITHOUT NEON (current default):"
echo "   make ad5m-docker -j"
echo "   ./scripts/benchmark_neon.sh"
echo ""
echo "2. WITH NEON (add to mk/cross.mk TARGET_CFLAGS):"
echo "   -D__ARM_NEON"
echo "   make ad5m-docker -j"
echo "   ./scripts/benchmark_neon.sh"
echo "========================================"
